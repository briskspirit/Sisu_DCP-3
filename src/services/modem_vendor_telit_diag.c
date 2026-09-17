#include "modem_vendor_telit_internal.h"

#include <ctype.h>
#include <string.h>

/* ------------------------------------------------------------------------ */
/* Net Monitor v2 typed diagnostics.                                        */
/* ------------------------------------------------------------------------ */

static modem_diag_line_result_t telit_diag_result(bool matches, bool valid) {
    if (!matches) {
        return MODEM_DIAG_LINE_IGNORE;
    }
    return valid ? MODEM_DIAG_LINE_ACCEPT : MODEM_DIAG_LINE_INVALID;
}

static bool telit_view_parse_decimal_x10(telit_csv_view_t field,
                                         int16_t min_x10, int16_t max_x10,
                                         int16_t *value_out) {
    if (field.text == NULL || field.length == 0u || value_out == NULL) {
        return false;
    }
    uint8_t dot = field.length;
    for (uint8_t i = 0u; i < field.length; i++) {
        if (field.text[i] == '.') {
            if (dot != field.length) {
                return false;
            }
            dot = i;
        }
    }
    telit_csv_view_t whole_field = field;
    if (dot != field.length) {
        if (dot == 0u || dot + 2u != field.length ||
            field.text[dot + 1u] < '0' || field.text[dot + 1u] > '9') {
            return false;
        }
        whole_field.length = dot;
    }
    int32_t whole = 0;
    if (!telit_view_parse_i32(whole_field, -3276, 3276, &whole)) {
        return false;
    }
    int32_t value = whole * 10;
    if (dot != field.length) {
        int32_t fraction = field.text[dot + 1u] - '0';
        value += field.text[0] == '-' ? -fraction : fraction;
    }
    if (value < min_x10 || value > max_x10) {
        return false;
    }
    *value_out = (int16_t)value;
    return true;
}

static bool telit_view_copy_code(char *dst, size_t dst_cap,
                                 telit_csv_view_t field) {
    if (field.length == 0u || (size_t)field.length >= dst_cap) {
        return false;
    }
    for (uint8_t i = 0u; i < field.length; i++) {
        unsigned char ch = (unsigned char)field.text[i];
        if (!isalnum(ch)) {
            return false;
        }
        dst[i] = (char)toupper(ch);
    }
    dst[field.length] = '\0';
    return true;
}

static uint16_t telit_diag_gsm_band(uint32_t active_band) {
    static const uint8_t bands[] = {0u, 5u, 8u, 3u, 2u};
    return active_band < sizeof(bands) ? bands[active_band] : 0u;
}

static uint16_t telit_diag_wcdma_band(uint32_t active_band) {
    static const uint8_t bands[] = {0u, 1u, 2u, 5u, 8u, 4u, 6u, 3u};
    return active_band < sizeof(bands) ? bands[active_band] : 0u;
}

static void telit_diag_infer_rf(modem_diag_serving_t *serving) {
    serving->inferred_rf_state = 0u;
    serving->inferred_rf_tuned = false;
    if (serving->band == 0u) {
        return;
    }

    uint8_t rf = 0u;
    bool tuned = true;
    if (serving->rat == MODEM_DIAG_RAT_LTE) {
        switch (serving->band) {
        case 1: case 2: case 3: case 9: case 25: case 65:
            rf = 1u; break;
        case 12: case 13: case 17: case 28: case 68: case 71: case 85:
            rf = 2u; break;
        case 5: case 6: case 8: case 14: case 18: case 19: case 20:
        case 26: case 27: case 30:
            rf = 3u; break;
        case 4: case 10: case 66:
            rf = 4u; break;
        case 7:
            rf = 1u; tuned = false; break;
        default:
            return;
        }
        switch (serving->band) {
        case 6: case 10: case 17: case 27: case 30: case 65: case 68: case 85:
            tuned = false;
            break;
        default:
            break;
        }
    } else if (serving->rat == MODEM_DIAG_RAT_WCDMA) {
        switch (serving->band) {
        case 1: case 2: case 3: rf = 1u; break;
        case 4: rf = 4u; break;
        case 5: case 6: case 8: case 19: rf = 3u; break;
        default: return;
        }
    } else if (serving->rat == MODEM_DIAG_RAT_GSM) {
        switch (serving->band) {
        case 2: case 3: rf = 1u; break;
        case 5: case 8: rf = 3u; break;
        default: return;
        }
    }
    serving->inferred_rf_state = rf;
    serving->inferred_rf_tuned = tuned;
}

static modem_diag_line_result_t telit_parse_rfsts_serving(
    const char *line, modem_diag_serving_t *serving_out,
    uint64_t *present_out) {
    if (!telit_starts_with(line, "#RFSTS:")) {
        return MODEM_DIAG_LINE_IGNORE;
    }
    if (serving_out == NULL || present_out == NULL) {
        return MODEM_DIAG_LINE_INVALID;
    }

    telit_csv_view_t fields[48];
    size_t count = 0u;
    if (!telit_view_split_prefixed(line, "#RFSTS:", fields,
                                   sizeof(fields) / sizeof(fields[0]),
                                   &count)) {
        return MODEM_DIAG_LINE_INVALID;
    }

    modem_diag_serving_t next;
    memset(&next, 0, sizeof(next));
    uint64_t present = 0u;
    char mcc[4] = {0};
    char mnc[4] = {0};
    uint32_t channel = 0u;
    int32_t value = 0;
    uint32_t band_raw = 0u;

    if (count == TELIT_RFSTS_FIELD_COUNT) {
        int16_t rsrq_x2 = 0;
        uint32_t drx = 0u, mm = 0u, rrc = 0u, domain = 0u, sinr = 0u;
        if (!telit_view_parse_plmn_spaced(fields[0], mcc, mnc) ||
            !telit_view_parse_u32(fields[1], 262143u, &channel) ||
            !telit_view_parse_i32(fields[2], -200, 0, &value)) {
            return MODEM_DIAG_LINE_INVALID;
        }
        next.rsrp_dbm = (int16_t)value;
        if (!telit_view_parse_i32(fields[3], -200, 0, &value) ||
            !telit_view_parse_decimal_x2(fields[4], -200, 200, &rsrq_x2) ||
            !telit_view_copy_hex(next.area_code, sizeof(next.area_code), fields[5]) ||
            !telit_view_parse_u32(fields[7], UINT16_MAX, &drx) ||
            !telit_view_parse_u32(fields[8], UINT8_MAX, &mm) ||
            !telit_view_parse_u32(fields[9], UINT8_MAX, &rrc) ||
            !telit_view_copy_hex(next.cell_id, sizeof(next.cell_id), fields[10]) ||
            !telit_view_copy_exact(next.operator_name,
                                   sizeof(next.operator_name), fields[12]) ||
            !telit_view_parse_u32(fields[13], 4u, &domain) ||
            !telit_view_parse_u32(fields[14], 71u, &band_raw) || band_raw == 0u ||
            !telit_view_parse_u32(fields[15], 250u, &sinr)) {
            return MODEM_DIAG_LINE_INVALID;
        }
        next.rssi_dbm = (int16_t)value;
        next.rsrq_db_x2 = rsrq_x2;
        next.sinr_db_x10 = (int16_t)((int32_t)sinr * 2 - 200);
        next.drx_ms = (uint16_t)drx;
        next.mm_state = (uint8_t)mm;
        next.rrc_state = (uint8_t)rrc;
        next.service_domain = (uint8_t)domain;
        next.band = (uint16_t)band_raw;
        next.rat = MODEM_DIAG_RAT_LTE;
        present = MODEM_DIAG_SERVING_RAT | MODEM_DIAG_SERVING_PLMN |
            MODEM_DIAG_SERVING_CHANNEL | MODEM_DIAG_SERVING_BAND |
            MODEM_DIAG_SERVING_RSSI | MODEM_DIAG_SERVING_RSRP |
            MODEM_DIAG_SERVING_RSRQ | MODEM_DIAG_SERVING_SINR |
            MODEM_DIAG_SERVING_AREA | MODEM_DIAG_SERVING_CELL |
            MODEM_DIAG_SERVING_OPERATOR | MODEM_DIAG_SERVING_DRX |
            MODEM_DIAG_SERVING_MM | MODEM_DIAG_SERVING_RRC |
            MODEM_DIAG_SERVING_DOMAIN;
        if (fields[6].length != 0u) {
            if (!telit_view_parse_i32(fields[6], -1120, 230, &value)) {
                return MODEM_DIAG_LINE_INVALID;
            }
            next.tx_power_dbm_x10 = (int16_t)value;
            present |= MODEM_DIAG_SERVING_TX_POWER;
        }
    } else if (count == 14u) {
        uint32_t mm = 0u, rr = 0u, domain = 0u;
        if (!telit_view_parse_plmn_spaced(fields[0], mcc, mnc) ||
            !telit_view_parse_u32(fields[1], UINT16_MAX, &channel) ||
            !telit_view_parse_i32(fields[2], -200, 0, &value)) {
            return MODEM_DIAG_LINE_INVALID;
        }
        next.rssi_dbm = (int16_t)value;
        if (!telit_view_copy_code(next.area_code, sizeof(next.area_code), fields[3]) ||
            !telit_view_parse_u32(fields[6], UINT8_MAX, &mm) ||
            !telit_view_parse_u32(fields[7], UINT8_MAX, &rr) ||
            !telit_view_copy_code(next.cell_id, sizeof(next.cell_id), fields[9]) ||
            !telit_view_copy_exact(next.operator_name,
                                   sizeof(next.operator_name), fields[11]) ||
            !telit_view_parse_u32(fields[12], 3u, &domain) ||
            !telit_view_parse_u32(fields[13], 4u, &band_raw) || band_raw == 0u) {
            return MODEM_DIAG_LINE_INVALID;
        }
        next.mm_state = (uint8_t)mm;
        next.rrc_state = (uint8_t)rr;
        next.service_domain = (uint8_t)domain;
        next.band = telit_diag_gsm_band(band_raw);
        if (next.band == 0u) {
            return MODEM_DIAG_LINE_INVALID;
        }
        next.rat = MODEM_DIAG_RAT_GSM;
        present = MODEM_DIAG_SERVING_RAT | MODEM_DIAG_SERVING_PLMN |
            MODEM_DIAG_SERVING_CHANNEL | MODEM_DIAG_SERVING_BAND |
            MODEM_DIAG_SERVING_RSSI | MODEM_DIAG_SERVING_AREA |
            MODEM_DIAG_SERVING_CELL | MODEM_DIAG_SERVING_OPERATOR |
            MODEM_DIAG_SERVING_MM | MODEM_DIAG_SERVING_RRC |
            MODEM_DIAG_SERVING_DOMAIN;
        if (fields[5].length != 0u) {
            if (!telit_view_parse_i32(fields[5], -200, 200, &value)) {
                return MODEM_DIAG_LINE_INVALID;
            }
            next.tx_power_dbm_x10 = (int16_t)(value * 10);
            present |= MODEM_DIAG_SERVING_TX_POWER;
        }
    } else if (count >= 22u) {
        uint32_t psc = 0u, drx = 0u, mm = 0u, rrc = 0u, domain = 0u;
        int16_t ecio_x10 = 0;
        if (!telit_view_parse_plmn_spaced(fields[0], mcc, mnc) ||
            !telit_view_parse_u32(fields[1], UINT16_MAX, &channel) ||
            !telit_view_parse_u32(fields[2], UINT16_MAX, &psc) ||
            !telit_view_parse_decimal_x10(fields[3], -500, 100, &ecio_x10) ||
            !telit_view_parse_i32(fields[4], -200, 0, &value)) {
            return MODEM_DIAG_LINE_INVALID;
        }
        next.rsrp_dbm = (int16_t)value;
        if (!telit_view_parse_i32(fields[5], -200, 0, &value) ||
            !telit_view_copy_code(next.area_code, sizeof(next.area_code), fields[6]) ||
            !telit_view_parse_u32(fields[9], UINT16_MAX, &drx) ||
            !telit_view_parse_u32(fields[10], UINT8_MAX, &mm) ||
            !telit_view_parse_u32(fields[11], UINT8_MAX, &rrc) ||
            !telit_view_copy_hex(next.cell_id, sizeof(next.cell_id), fields[14]) ||
            !telit_view_copy_exact(next.operator_name,
                                   sizeof(next.operator_name), fields[16]) ||
            !telit_view_parse_u32(fields[17], 4u, &domain) ||
            !telit_view_parse_u32(fields[count - 1u], 7u, &band_raw) ||
            band_raw == 0u) {
            return MODEM_DIAG_LINE_INVALID;
        }
        next.rssi_dbm = (int16_t)value;
        next.rsrq_db_x2 = (int16_t)(ecio_x10 / 5);
        next.pci = (uint16_t)psc;
        next.drx_ms = (uint16_t)drx;
        next.mm_state = (uint8_t)mm;
        next.rrc_state = (uint8_t)rrc;
        next.service_domain = (uint8_t)domain;
        next.band = telit_diag_wcdma_band(band_raw);
        if (next.band == 0u) {
            return MODEM_DIAG_LINE_INVALID;
        }
        next.rat = MODEM_DIAG_RAT_WCDMA;
        present = MODEM_DIAG_SERVING_RAT | MODEM_DIAG_SERVING_PLMN |
            MODEM_DIAG_SERVING_CHANNEL | MODEM_DIAG_SERVING_BAND |
            MODEM_DIAG_SERVING_RSSI | MODEM_DIAG_SERVING_RSRP |
            MODEM_DIAG_SERVING_RSRQ | MODEM_DIAG_SERVING_AREA |
            MODEM_DIAG_SERVING_CELL | MODEM_DIAG_SERVING_PCI |
            MODEM_DIAG_SERVING_OPERATOR | MODEM_DIAG_SERVING_DRX |
            MODEM_DIAG_SERVING_MM | MODEM_DIAG_SERVING_RRC |
            MODEM_DIAG_SERVING_DOMAIN;
        if (fields[8].length != 0u) {
            if (!telit_view_parse_i32(fields[8], -200, 200, &value)) {
                return MODEM_DIAG_LINE_INVALID;
            }
            next.tx_power_dbm_x10 = (int16_t)(value * 10);
            present |= MODEM_DIAG_SERVING_TX_POWER;
        }
    } else {
        return MODEM_DIAG_LINE_INVALID;
    }

    memcpy(next.mcc, mcc, sizeof(next.mcc));
    memcpy(next.mnc, mnc, sizeof(next.mnc));
    next.channel = channel;
    telit_diag_infer_rf(&next);
    *serving_out = next;
    *present_out = present;
    return MODEM_DIAG_LINE_ACCEPT;
}

modem_diag_line_result_t telit_diag_parse_rfsts(
    const char *line, modem_diag_snapshot_t *shadow) {
    if (shadow == NULL) {
        return MODEM_DIAG_LINE_INVALID;
    }
    modem_diag_serving_t serving;
    uint64_t present = 0u;
    modem_diag_line_result_t result =
        telit_parse_rfsts_serving(line, &serving, &present);
    if (result == MODEM_DIAG_LINE_ACCEPT) {
        shadow->serving = serving;
        shadow->group[MODEM_DIAG_GROUP_SERVING].present_fields = present;
    }
    return result;
}

bool telit_parse_signal_response(const char *line,
                                        modem_signal_sample_t *out) {
    if (out == NULL) {
        return false;
    }

    /* Keep one RFSTS grammar authority: the runtime sample is a neutral
     * projection of the same typed parser used by Net Monitor. Its CSV splitter
     * preserves empty optional TXPWR/NetName fields, so later columns never
     * slide when Telit emits consecutive commas. */
    modem_diag_serving_t serving;
    uint64_t present = 0u;
    if (telit_parse_rfsts_serving(line, &serving, &present) !=
        MODEM_DIAG_LINE_ACCEPT) {
        return false;
    }

    modem_signal_sample_t next;
    memset(&next, 0, sizeof(next));
    if ((present & MODEM_DIAG_SERVING_PLMN) != 0u) {
        memcpy(next.mcc, serving.mcc, sizeof(next.mcc));
        memcpy(next.mnc, serving.mnc, sizeof(next.mnc));
        next.valid_fields |= MODEM_SIGNAL_VALID_PLMN;
    }
    switch (serving.rat) {
    case MODEM_DIAG_RAT_GSM:   next.rat = MODEM_SIGNAL_RAT_GSM; break;
    case MODEM_DIAG_RAT_WCDMA: next.rat = MODEM_SIGNAL_RAT_WCDMA; break;
    case MODEM_DIAG_RAT_LTE:   next.rat = MODEM_SIGNAL_RAT_LTE; break;
    default: return false;
    }

    if ((present & MODEM_DIAG_SERVING_RSSI) != 0u) {
        next.rssi_dbm = serving.rssi_dbm;
        next.valid_fields |= MODEM_SIGNAL_VALID_RSSI;
    }
    if ((present & MODEM_DIAG_SERVING_RSRP) != 0u) {
        next.rsrp_dbm = serving.rsrp_dbm;
        next.valid_fields |= MODEM_SIGNAL_VALID_RSRP;
    }
    if ((present & MODEM_DIAG_SERVING_RSRQ) != 0u) {
        next.rsrq_db_x2 = serving.rsrq_db_x2;
        next.valid_fields |= MODEM_SIGNAL_VALID_RSRQ;
    }
    if ((present & MODEM_DIAG_SERVING_SINR) != 0u) {
        /* telit_parse_rfsts_serving already converted raw 0..250 with
         * raw * 0.2 - 20 dB. Never apply the Telit transform twice. */
        next.sinr_db_x10 = serving.sinr_db_x10;
        next.valid_fields |= MODEM_SIGNAL_VALID_SINR;
    }
    if ((present & MODEM_DIAG_SERVING_CHANNEL) != 0u) {
        next.channel = serving.channel;
        next.valid_fields |= MODEM_SIGNAL_VALID_CHANNEL;
    }
    if ((present & MODEM_DIAG_SERVING_CELL) != 0u &&
        serving.cell_id[0] != '\0') {
        telit_csv_view_t cell = {
            .text = serving.cell_id,
            .length = (uint8_t)strlen(serving.cell_id),
        };
        uint64_t value = 0u;
        if (telit_view_parse_hex_u64(cell, UINT32_MAX, &value)) {
            next.cell_id = (uint32_t)value;
            next.valid_fields |= MODEM_SIGNAL_VALID_CELL_ID;
        }
    }

    *out = next;
    return true;
}

modem_diag_line_result_t telit_diag_parse_moni(
    const char *line, modem_diag_snapshot_t *shadow) {
    if (!telit_starts_with(line, "#MONI:")) {
        return MODEM_DIAG_LINE_IGNORE;
    }
    if (shadow == NULL ||
        shadow->group[MODEM_DIAG_GROUP_SERVING].present_fields == 0u) {
        return MODEM_DIAG_LINE_INVALID;
    }
    if (shadow->serving.rat != MODEM_DIAG_RAT_LTE) {
        /* RFSTS already carries every field v2 displays for GSM/WCDMA. */
        return MODEM_DIAG_LINE_ACCEPT;
    }

    telit_csv_view_t channel_field, area_field, cell_field, pci_field;
    uint32_t channel = 0u, pci = 0u;
    uint64_t area = 0u, cell = 0u;
    if (!telit_view_find_labeled(line, "EARFCN:", &channel_field) ||
        !telit_view_find_labeled(line, "TAC:", &area_field) ||
        !telit_view_find_labeled(line, "Id:", &cell_field) ||
        !telit_view_find_labeled(line, "pci:", &pci_field) ||
        !telit_view_parse_u32(channel_field, 262143u, &channel) ||
        !telit_view_parse_hex_u64(area_field, UINT16_MAX, &area) ||
        !telit_view_parse_hex_u64(cell_field, UINT32_MAX, &cell) ||
        !telit_view_parse_u32(pci_field, 503u, &pci)) {
        return MODEM_DIAG_LINE_INVALID;
    }

    modem_diag_serving_t *serving = &shadow->serving;
    telit_csv_view_t serving_area = {
        serving->area_code, (uint8_t)strlen(serving->area_code)};
    telit_csv_view_t serving_cell = {
        serving->cell_id, (uint8_t)strlen(serving->cell_id)};
    uint64_t expected_area = 0u, expected_cell = 0u;
    if (!telit_view_parse_hex_u64(serving_area, UINT16_MAX, &expected_area) ||
        !telit_view_parse_hex_u64(serving_cell, UINT32_MAX, &expected_cell)) {
        return MODEM_DIAG_LINE_INVALID;
    }
    /* MONI repeats all three RFSTS cell keys. This keeps the two-command
     * snapshot atomic even if the module reselects between commands. WWX
     * SERVINFO cannot be used for this: the bench module reports TAC 0001
     * there while RFSTS and MONI agree on the real TAC (for example 3401). */
    if (channel != serving->channel || area != expected_area ||
        cell != expected_cell) {
        return MODEM_DIAG_LINE_INVALID;
    }
    serving->pci = (uint16_t)pci;
    shadow->group[MODEM_DIAG_GROUP_SERVING].present_fields |=
        MODEM_DIAG_SERVING_PCI;
    return MODEM_DIAG_LINE_ACCEPT;
}

typedef enum {
    TELIT_DIAG_REG_CS = 0,
    TELIT_DIAG_REG_PS,
    TELIT_DIAG_REG_EPS,
} telit_diag_reg_domain_t;

static modem_diag_line_result_t telit_diag_parse_reg(
    const char *line, const char *prefix, modem_diag_snapshot_t *shadow,
    telit_diag_reg_domain_t domain) {
    if (!telit_starts_with(line, prefix)) {
        return MODEM_DIAG_LINE_IGNORE;
    }
    telit_csv_view_t fields[7];
    size_t count = 0u;
    uint32_t mode = 0u, stat = 0u, act = 0u;
    if (shadow == NULL ||
        !telit_view_split_prefixed(line, prefix, fields,
                                   sizeof(fields) / sizeof(fields[0]), &count) ||
        count < 2u || !telit_view_parse_u32(fields[0], 5u, &mode) ||
        !telit_view_parse_u32(fields[1], 10u, &stat)) {
        return MODEM_DIAG_LINE_INVALID;
    }
    (void)mode;
    char area[5] = {0};
    char cell[9] = {0};
    bool has_location = count >= 4u && fields[2].length != 0u &&
        fields[3].length != 0u;
    if (has_location &&
        (!telit_view_copy_hex(area, sizeof(area), fields[2]) ||
         !telit_view_copy_hex(cell, sizeof(cell), fields[3]))) {
        return MODEM_DIAG_LINE_INVALID;
    }
    bool has_act = count >= 5u && fields[4].length != 0u;
    if (has_act && !telit_view_parse_u32(fields[4], UINT8_MAX, &act)) {
        return MODEM_DIAG_LINE_INVALID;
    }

    modem_diag_registration_t *reg = &shadow->registration;
    uint64_t bit;
    if (domain == TELIT_DIAG_REG_CS) {
        reg->cs_stat = (uint8_t)stat;
        reg->cs_act = has_act ? (uint8_t)act : UINT8_MAX;
        if (has_location) {
            memcpy(reg->cs_area, area, sizeof(area));
            memcpy(reg->cs_cell, cell, sizeof(cell));
        }
        bit = MODEM_DIAG_REG_CS;
    } else if (domain == TELIT_DIAG_REG_PS) {
        reg->ps_stat = (uint8_t)stat;
        reg->ps_act = has_act ? (uint8_t)act : UINT8_MAX;
        if (has_location) {
            memcpy(reg->ps_area, area, sizeof(area));
            memcpy(reg->ps_cell, cell, sizeof(cell));
        }
        bit = MODEM_DIAG_REG_PS;
    } else {
        reg->eps_stat = (uint8_t)stat;
        reg->eps_act = has_act ? (uint8_t)act : UINT8_MAX;
        if (has_location) {
            memcpy(reg->eps_area, area, sizeof(area));
            memcpy(reg->eps_cell, cell, sizeof(cell));
        }
        bit = MODEM_DIAG_REG_EPS;
    }
    shadow->group[MODEM_DIAG_GROUP_REGISTRATION].present_fields |= bit;
    return MODEM_DIAG_LINE_ACCEPT;
}

static modem_diag_line_result_t telit_diag_parse_creg(
    const char *line, modem_diag_snapshot_t *shadow) {
    return telit_diag_parse_reg(line, "+CREG:", shadow, TELIT_DIAG_REG_CS);
}

static modem_diag_line_result_t telit_diag_parse_cgreg(
    const char *line, modem_diag_snapshot_t *shadow) {
    return telit_diag_parse_reg(line, "+CGREG:", shadow, TELIT_DIAG_REG_PS);
}

static modem_diag_line_result_t telit_diag_parse_cereg(
    const char *line, modem_diag_snapshot_t *shadow) {
    return telit_diag_parse_reg(line, "+CEREG:", shadow, TELIT_DIAG_REG_EPS);
}

static modem_diag_line_result_t telit_diag_parse_cireg(
    const char *line, modem_diag_snapshot_t *shadow) {
    if (!telit_starts_with(line, "+CIREG:")) {
        return MODEM_DIAG_LINE_IGNORE;
    }
    telit_csv_view_t fields[3];
    size_t count = 0u;
    uint32_t mode = 0u, registration = 0u;
    if (shadow == NULL ||
        !telit_view_split_prefixed(line, "+CIREG:", fields, 3u, &count) ||
        count < 2u || !telit_view_parse_u32(fields[0], 2u, &mode) ||
        !telit_view_parse_u32(fields[1], UINT8_MAX, &registration)) {
        return MODEM_DIAG_LINE_INVALID;
    }
    (void)mode;
    shadow->registration.ims_stat = (uint8_t)registration;
    shadow->group[MODEM_DIAG_GROUP_REGISTRATION].present_fields |=
        MODEM_DIAG_REG_IMS;
    return MODEM_DIAG_LINE_ACCEPT;
}

static modem_diag_line_result_t telit_diag_parse_cfun(
    const char *line, modem_diag_snapshot_t *shadow) {
    if (!telit_starts_with(line, "+CFUN:")) {
        return MODEM_DIAG_LINE_IGNORE;
    }
    telit_csv_view_t fields[1];
    size_t count = 0u;
    uint32_t value = 0u;
    bool valid = shadow != NULL &&
        telit_view_split_prefixed(line, "+CFUN:", fields, 1u, &count) &&
        count == 1u && telit_view_parse_u32(fields[0], 7u, &value);
    if (valid) {
        shadow->radio_policy.cfun = (uint8_t)value;
        shadow->group[MODEM_DIAG_GROUP_RADIO_POLICY].present_fields |=
            MODEM_DIAG_POLICY_CFUN;
    }
    return telit_diag_result(true, valid);
}

static modem_diag_line_result_t telit_diag_parse_cops(
    const char *line, modem_diag_snapshot_t *shadow) {
    if (!telit_starts_with(line, "+COPS:")) {
        return MODEM_DIAG_LINE_IGNORE;
    }
    telit_csv_view_t fields[4];
    size_t count = 0u;
    uint32_t mode = 0u, format = 0u, act = 0u;
    if (shadow == NULL ||
        !telit_view_split_prefixed(line, "+COPS:", fields, 4u, &count) ||
        count < 1u || !telit_view_parse_u32(fields[0], 4u, &mode)) {
        return MODEM_DIAG_LINE_INVALID;
    }
    modem_diag_radio_policy_t *policy = &shadow->radio_policy;
    policy->cops_mode = (uint8_t)mode;
    policy->cops_format = UINT8_MAX;
    policy->cops_act = UINT8_MAX;
    policy->cops_operator[0] = '\0';
    if (count >= 2u && fields[1].length != 0u &&
        !telit_view_parse_u32(fields[1], 2u, &format)) {
        return MODEM_DIAG_LINE_INVALID;
    }
    if (count >= 2u && fields[1].length != 0u) {
        policy->cops_format = (uint8_t)format;
    }
    if (count >= 3u &&
        !telit_view_copy_exact(policy->cops_operator,
                               sizeof(policy->cops_operator), fields[2])) {
        return MODEM_DIAG_LINE_INVALID;
    }
    if (count >= 4u && fields[3].length != 0u) {
        if (!telit_view_parse_u32(fields[3], UINT8_MAX, &act)) {
            return MODEM_DIAG_LINE_INVALID;
        }
        policy->cops_act = (uint8_t)act;
    }
    shadow->group[MODEM_DIAG_GROUP_RADIO_POLICY].present_fields |=
        MODEM_DIAG_POLICY_COPS;
    return MODEM_DIAG_LINE_ACCEPT;
}

static modem_diag_line_result_t telit_diag_parse_single_policy_u8(
    const char *line, const char *prefix, modem_diag_snapshot_t *shadow,
    uint8_t max_value, uint8_t *target, uint64_t present_bit) {
    if (!telit_starts_with(line, prefix)) {
        return MODEM_DIAG_LINE_IGNORE;
    }
    telit_csv_view_t fields[1];
    size_t count = 0u;
    uint32_t value = 0u;
    bool valid = shadow != NULL && target != NULL &&
        telit_view_split_prefixed(line, prefix, fields, 1u, &count) &&
        count == 1u && telit_view_parse_u32(fields[0], max_value, &value);
    if (valid) {
        *target = (uint8_t)value;
        shadow->group[MODEM_DIAG_GROUP_RADIO_POLICY].present_fields |= present_bit;
    }
    return telit_diag_result(true, valid);
}

static modem_diag_line_result_t telit_diag_parse_ens(
    const char *line, modem_diag_snapshot_t *shadow) {
    return telit_diag_parse_single_policy_u8(
        line, "#ENS:", shadow, 1u,
        shadow != NULL ? &shadow->radio_policy.ens : NULL,
        MODEM_DIAG_POLICY_ENS);
}

static modem_diag_line_result_t telit_diag_parse_ws46(
    const char *line, modem_diag_snapshot_t *shadow) {
    return telit_diag_parse_single_policy_u8(
        line, "+WS46:", shadow, UINT8_MAX,
        shadow != NULL ? &shadow->radio_policy.ws46 : NULL,
        MODEM_DIAG_POLICY_WS46);
}

modem_diag_line_result_t telit_diag_parse_selbndmode(
    const char *line, modem_diag_snapshot_t *shadow) {
    uint8_t mode = 0u;
    modem_diag_line_result_t result = telit_parse_band_mode_value(
        line, shadow != NULL ? &mode : NULL);
    if (result == MODEM_DIAG_LINE_ACCEPT) {
        shadow->radio_policy.select_band_mode = mode;
        shadow->group[MODEM_DIAG_GROUP_RADIO_POLICY].present_fields |=
            MODEM_DIAG_POLICY_SELBNDMODE;
    }
    return result;
}

static modem_diag_line_result_t telit_diag_parse_fwswitch_v2(
    const char *line, modem_diag_snapshot_t *shadow) {
    if (!telit_starts_with(line, "#FWSWITCH:")) {
        return MODEM_DIAG_LINE_IGNORE;
    }
    telit_fwswitch_t state;
    bool valid = shadow != NULL && telit_parse_fwswitch(line, &state);
    if (valid) {
        shadow->radio_policy.firmware_image = state.image;
        shadow->radio_policy.firmware_storage = state.storage;
        shadow->radio_policy.firmware_restore = state.restore;
        shadow->group[MODEM_DIAG_GROUP_RADIO_POLICY].present_fields |=
            MODEM_DIAG_POLICY_FWSWITCH;
    }
    return telit_diag_result(true, valid);
}

static modem_diag_line_result_t telit_diag_parse_fwautosim_v2(
    const char *line, modem_diag_snapshot_t *shadow) {
    if (!telit_starts_with(line, "#FWAUTOSIM:")) {
        return MODEM_DIAG_LINE_IGNORE;
    }
    uint8_t mode = 0u;
    bool valid = shadow != NULL && telit_parse_fwautosim(line, &mode);
    if (valid) {
        shadow->radio_policy.firmware_auto_sim = mode;
        shadow->group[MODEM_DIAG_GROUP_RADIO_POLICY].present_fields |=
            MODEM_DIAG_POLICY_FWAUTOSIM;
    }
    return telit_diag_result(true, valid);
}

static modem_diag_line_result_t telit_diag_parse_band_common(
    const char *line, const char *prefix, modem_diag_snapshot_t *shadow,
    bool ram_only) {
    if (!telit_starts_with(line, prefix)) {
        return MODEM_DIAG_LINE_IGNORE;
    }
    telit_csv_view_t fields[4];
    size_t count = 0u;
    uint32_t gsm = 0u, wcdma = 0u;
    if (shadow == NULL ||
        !telit_view_split_prefixed(line, prefix, fields, 4u, &count) ||
        count < 3u || !telit_view_parse_u32(fields[0], UINT8_MAX, &gsm) ||
        !telit_view_parse_u32(fields[1], UINT16_MAX, &wcdma)) {
        return MODEM_DIAG_LINE_INVALID;
    }
    char lte[17];
    if (!telit_view_copy_hex(lte, sizeof(lte), fields[2])) {
        return MODEM_DIAG_LINE_INVALID;
    }
    if (ram_only) {
        shadow->radio_policy.bndram_gsm = (uint8_t)gsm;
        shadow->radio_policy.bndram_wcdma = (uint16_t)wcdma;
        memcpy(shadow->radio_policy.bndram_lte, lte, sizeof(lte));
        shadow->group[MODEM_DIAG_GROUP_RADIO_POLICY].present_fields |=
            MODEM_DIAG_POLICY_BNDRAM;
    } else {
        shadow->radio_policy.bnd_gsm = (uint8_t)gsm;
        shadow->radio_policy.bnd_wcdma = (uint16_t)wcdma;
        memcpy(shadow->radio_policy.bnd_lte, lte, sizeof(lte));
        shadow->group[MODEM_DIAG_GROUP_RADIO_POLICY].present_fields |=
            MODEM_DIAG_POLICY_BND;
    }
    return MODEM_DIAG_LINE_ACCEPT;
}

static modem_diag_line_result_t telit_diag_parse_bnd(
    const char *line, modem_diag_snapshot_t *shadow) {
    return telit_diag_parse_band_common(line, "#BND:", shadow, false);
}

static modem_diag_line_result_t telit_diag_parse_bndram(
    const char *line, modem_diag_snapshot_t *shadow) {
    return telit_diag_parse_band_common(line, "#BNDRAM:", shadow, true);
}

modem_diag_line_result_t telit_diag_parse_scan_config(
    const char *line, modem_diag_snapshot_t *shadow) {
    uint16_t seconds = 0u;
    modem_diag_line_result_t result = telit_parse_scan_timer_value(
        line, shadow != NULL ? &seconds : NULL);
    if (result == MODEM_DIAG_LINE_ACCEPT) {
        shadow->radio_policy.scan_timer_s = seconds;
        shadow->group[MODEM_DIAG_GROUP_RADIO_POLICY].present_fields |=
            MODEM_DIAG_POLICY_SCAN_CONFIG;
    }
    return result;
}

static modem_diag_line_result_t telit_diag_parse_scan_remaining(
    const char *line, modem_diag_snapshot_t *shadow) {
    if (!telit_starts_with(line, "#NWSCANTMREXP:")) {
        return MODEM_DIAG_LINE_IGNORE;
    }
    telit_csv_view_t fields[1];
    size_t count = 0u;
    uint32_t seconds = 0u;
    bool valid = shadow != NULL &&
        telit_view_split_prefixed(line, "#NWSCANTMREXP:", fields, 1u, &count) &&
        count == 1u && telit_view_parse_u32(fields[0], 3600u, &seconds);
    if (valid) {
        shadow->radio_policy.scan_remaining_s = (uint16_t)seconds;
        shadow->group[MODEM_DIAG_GROUP_RADIO_POLICY].present_fields |=
            MODEM_DIAG_POLICY_SCAN_REMAINING;
    }
    return telit_diag_result(true, valid);
}

modem_diag_line_result_t telit_diag_parse_stune_enabled(
    const char *line, modem_diag_snapshot_t *shadow) {
    bool enabled = false;
    modem_diag_line_result_t result = telit_parse_tuner_enabled_value(
        line, shadow != NULL ? &enabled : NULL);
    if (result == MODEM_DIAG_LINE_ACCEPT) {
        shadow->tuner.enabled = enabled;
        shadow->group[MODEM_DIAG_GROUP_TUNER].present_fields |=
            MODEM_DIAG_TUNER_ENABLED;
    }
    return result;
}

modem_diag_line_result_t telit_diag_parse_stune_capability(
    const char *line, modem_diag_snapshot_t *shadow) {
    static const char prefix[] = "#STUNEANT: (0,1),(";
    static const char suffix[] = "),(0,1),(0,1)";
    if (!telit_starts_with(line, "#STUNEANT:")) {
        return MODEM_DIAG_LINE_IGNORE;
    }
    if (shadow == NULL || !telit_starts_with(line, prefix)) {
        return MODEM_DIAG_LINE_INVALID;
    }
    size_t line_len = strlen(line);
    size_t prefix_len = sizeof(prefix) - 1u;
    size_t suffix_len = sizeof(suffix) - 1u;
    if (line_len <= prefix_len + suffix_len ||
        memcmp(line + line_len - suffix_len, suffix, suffix_len) != 0) {
        return MODEM_DIAG_LINE_INVALID;
    }
    size_t mask_len = line_len - prefix_len - suffix_len;
    if (mask_len == 0u || mask_len > UINT8_MAX) {
        return MODEM_DIAG_LINE_INVALID;
    }
    telit_csv_view_t field = {
        .text = line + prefix_len,
        .length = (uint8_t)mask_len,
    };
    uint64_t mask = 0u;
    if (!telit_view_parse_hex_u64(field, TELIT_TUNE_COMMAND_DOMAIN_MASK,
                                  &mask) || mask == 0u) {
        return MODEM_DIAG_LINE_INVALID;
    }
    shadow->tuner.supported_mask = mask;
    shadow->group[MODEM_DIAG_GROUP_TUNER].present_fields |=
        MODEM_DIAG_TUNER_SUPPORTED_MASK;
    return MODEM_DIAG_LINE_ACCEPT;
}

modem_diag_line_result_t telit_diag_parse_gtune(
    const char *line, modem_diag_snapshot_t *shadow) {
    if (!telit_starts_with(line, "#GTUNEANT:")) {
        return MODEM_DIAG_LINE_IGNORE;
    }
    telit_csv_view_t fields[3];
    size_t count = 0u;
    uint64_t mask = 0u;
    uint32_t ctrl1 = 0u, ctrl2 = 0u;
    if (shadow == NULL ||
        !telit_view_split_prefixed(line, "#GTUNEANT:", fields, 3u, &count) ||
        count != 3u ||
        !telit_view_parse_hex_u64(fields[0], TELIT_TUNE_COMMAND_DOMAIN_MASK,
                                  &mask) ||
        !telit_view_parse_u32(fields[1], 1u, &ctrl1) ||
        !telit_view_parse_u32(fields[2], 1u, &ctrl2)) {
        return MODEM_DIAG_LINE_INVALID;
    }
    if ((shadow->group[MODEM_DIAG_GROUP_TUNER].present_fields &
         MODEM_DIAG_TUNER_SUPPORTED_MASK) != 0u &&
        (mask & ~shadow->tuner.supported_mask) != 0u) {
        return MODEM_DIAG_LINE_INVALID;
    }

    modem_diag_tuner_row_t *row = NULL;
    for (uint8_t i = 0u; i < shadow->tuner.row_count; i++) {
        if (shadow->tuner.rows[i].ctrl1 == ctrl1 &&
            shadow->tuner.rows[i].ctrl2 == ctrl2) {
            row = &shadow->tuner.rows[i];
            break;
        }
        if ((shadow->tuner.rows[i].band_mask & mask) != 0u) {
            return MODEM_DIAG_LINE_INVALID;
        }
    }
    if (row == NULL) {
        if (shadow->tuner.row_count >= MODEM_DIAG_TUNER_ROWS_MAX) {
            return MODEM_DIAG_LINE_INVALID;
        }
        row = &shadow->tuner.rows[shadow->tuner.row_count++];
        memset(row, 0, sizeof(*row));
        row->ctrl1 = (uint8_t)ctrl1;
        row->ctrl2 = (uint8_t)ctrl2;
    }
    row->band_mask |= mask;
    shadow->group[MODEM_DIAG_GROUP_TUNER].present_fields |=
        MODEM_DIAG_TUNER_TABLE;
    return MODEM_DIAG_LINE_ACCEPT;
}

modem_diag_line_result_t telit_diag_parse_cgatt(
    const char *line, modem_diag_snapshot_t *shadow) {
    if (!telit_starts_with(line, "+CGATT:")) {
        return MODEM_DIAG_LINE_IGNORE;
    }
    telit_csv_view_t fields[1];
    size_t count = 0u;
    uint32_t attached = 0u;
    bool valid = shadow != NULL &&
        telit_view_split_prefixed(line, "+CGATT:", fields, 1u, &count) &&
        count == 1u && telit_view_parse_u32(fields[0], 1u, &attached);
    if (valid) {
        shadow->packet.attached = (uint8_t)attached;
        shadow->group[MODEM_DIAG_GROUP_PACKET].present_fields |=
            MODEM_DIAG_PACKET_ATTACH;
    }
    return telit_diag_result(true, valid);
}

modem_diag_line_result_t telit_diag_parse_cgact(
    const char *line, modem_diag_snapshot_t *shadow) {
    if (!telit_starts_with(line, "+CGACT:")) {
        return MODEM_DIAG_LINE_IGNORE;
    }
    telit_csv_view_t fields[2];
    size_t count = 0u;
    uint32_t cid = 0u, active = 0u;
    if (shadow == NULL ||
        !telit_view_split_prefixed(line, "+CGACT:", fields, 2u, &count) ||
        count != 2u || !telit_view_parse_u32(fields[0], UINT8_MAX, &cid) ||
        !telit_view_parse_u32(fields[1], 1u, &active)) {
        return MODEM_DIAG_LINE_INVALID;
    }
    if (shadow->packet.context_count != UINT8_MAX) {
        shadow->packet.context_count++;
    }
    if (active != 0u) {
        if (shadow->packet.active_context_count != UINT8_MAX) {
            shadow->packet.active_context_count++;
        }
        if (shadow->packet.first_cid == 0u) {
            shadow->packet.first_cid = (uint8_t)cid;
            shadow->packet.first_status = 1u;
        }
    } else if (shadow->packet.first_cid == 0u) {
        shadow->packet.first_cid = (uint8_t)cid;
        shadow->packet.first_status = 0u;
    }
    shadow->group[MODEM_DIAG_GROUP_PACKET].present_fields |=
        MODEM_DIAG_PACKET_CONTEXTS;
    return MODEM_DIAG_LINE_ACCEPT;
}

static bool telit_diag_take_csv_field(const char **cursor,
                                      telit_csv_view_t *field,
                                      size_t max_length,
                                      bool allow_quoted_sequence,
                                      bool *has_more) {
    if (cursor == NULL || *cursor == NULL || field == NULL ||
        max_length > UINT8_MAX || has_more == NULL) {
        return false;
    }
    const char *p = *cursor;
    while (*p == ' ') {
        p++;
    }
    const char *start = p;
    size_t length = 0u;
    if (*p == '"') {
        start = ++p;
        while (*p != '\0' && *p != '"') {
            if (*p == '\r' || *p == '\n' ||
                length >= max_length) {
                return false;
            }
            p++;
            length++;
        }
        if (*p != '"') {
            return false;
        }
        p++;
        for (;;) {
            bool separated = false;
            while (*p == ' ') {
                separated = true;
                p++;
            }
            if (*p != '"') {
                break;
            }
            if (!allow_quoted_sequence || !separated) {
                return false;
            }
            p++;
            size_t continuation_length = 0u;
            while (*p != '\0' && *p != '"') {
                if (*p == '\r' || *p == '\n' ||
                    continuation_length >= max_length) {
                    return false;
                }
                p++;
                continuation_length++;
            }
            if (*p != '"') {
                return false;
            }
            p++;
        }
        if (*p != '\0' && *p != ',') {
            return false;
        }
    } else {
        while (*p != '\0' && *p != ',') {
            if (*p == '"' || *p == '\r' || *p == '\n' ||
                length >= max_length) {
                return false;
            }
            p++;
            length++;
        }
        while (length != 0u && start[length - 1u] == ' ') {
            length--;
        }
    }
    field->text = start;
    field->length = (uint8_t)length;
    *has_more = *p == ',';
    *cursor = *has_more ? p + 1u : p;
    return true;
}

static bool telit_diag_apn_valid(telit_csv_view_t field) {
    if (field.text == NULL) {
        return false;
    }
    if (field.length == 0u) {
        return true;
    }

    size_t label_length = 0u;
    bool label_ends_alnum = false;
    for (uint8_t i = 0u; i < field.length; i++) {
        unsigned char ch = (unsigned char)field.text[i];
        bool alnum = (ch >= '0' && ch <= '9') ||
                     (ch >= 'A' && ch <= 'Z') ||
                     (ch >= 'a' && ch <= 'z');
        if (ch == '.') {
            if (label_length == 0u || !label_ends_alnum) {
                return false;
            }
            label_length = 0u;
            label_ends_alnum = false;
            continue;
        }
        if (!alnum && ch != '-') {
            return false;
        }
        if (label_length == 0u && !alnum) {
            return false;
        }
        if (label_length >= 63u) {
            return false;
        }
        label_length++;
        label_ends_alnum = alnum;
    }
    return label_length != 0u && label_ends_alnum;
}

static telit_csv_view_t telit_diag_first_address(telit_csv_view_t field) {
    while (field.length != 0u && field.text[0] == ' ') {
        field.text++;
        field.length--;
    }
    uint8_t length = 0u;
    while (length < field.length && field.text[length] != ' ') {
        length++;
    }
    field.length = length;
    return field;
}

static bool telit_diag_copy_preview(char *dst, size_t dst_cap,
                                    telit_csv_view_t field) {
    size_t count = field.length;
    if (count >= dst_cap) {
        count = dst_cap - 1u;
    }
    memcpy(dst, field.text, count);
    dst[count] = '\0';
    return count != field.length;
}

modem_diag_line_result_t telit_diag_parse_cgcontrdp(
    const char *line, modem_diag_snapshot_t *shadow) {
    if (!telit_starts_with(line, "+CGCONTRDP:")) {
        return MODEM_DIAG_LINE_IGNORE;
    }
    if (shadow == NULL) {
        return MODEM_DIAG_LINE_INVALID;
    }
    const char *cursor = line + strlen("+CGCONTRDP:");
    telit_csv_view_t cid_field, bearer_field, apn_field;
    telit_csv_view_t address_field = {0};
    bool has_more = false;
    uint32_t cid = 0u, bearer = 0u;
    if (!telit_diag_take_csv_field(&cursor, &cid_field, 3u, false,
                                   &has_more) ||
        !has_more ||
        !telit_diag_take_csv_field(&cursor, &bearer_field, 3u, false,
                                   &has_more) ||
        !has_more ||
        !telit_diag_take_csv_field(&cursor, &apn_field,
                                   MODEM_DIAG_PACKET_APN_MAX, false,
                                   &has_more) ||
        !telit_view_parse_u32(cid_field, UINT8_MAX, &cid) || cid == 0u ||
        !telit_view_parse_u32(bearer_field, UINT8_MAX, &bearer) ||
        !telit_diag_apn_valid(apn_field)) {
        return MODEM_DIAG_LINE_INVALID;
    }
    (void)bearer;

    if (has_more) {
        if (!telit_diag_take_csv_field(&cursor, &address_field, UINT8_MAX,
                                       true, &has_more)) {
            return MODEM_DIAG_LINE_INVALID;
        }
        while (has_more) {
            telit_csv_view_t ignored;
            if (!telit_diag_take_csv_field(&cursor, &ignored, UINT8_MAX,
                                           true, &has_more)) {
                return MODEM_DIAG_LINE_INVALID;
            }
        }
    }

    address_field = telit_diag_first_address(address_field);
    if ((size_t)address_field.length > MODEM_DIAG_PACKET_ADDRESS_MAX) {
        return MODEM_DIAG_LINE_INVALID;
    }

    uint64_t present =
        shadow->group[MODEM_DIAG_GROUP_PACKET].present_fields;
    bool selected = (present & MODEM_DIAG_PACKET_APN) == 0u &&
        (shadow->packet.first_cid == 0u ||
         shadow->packet.first_cid == (uint8_t)cid);
    if (selected) {
        if (telit_diag_copy_preview(shadow->packet.apn,
                                    sizeof(shadow->packet.apn), apn_field)) {
            shadow->group[MODEM_DIAG_GROUP_PACKET].present_fields |=
                MODEM_DIAG_PACKET_APN_TRUNCATED;
        }
        shadow->packet.first_cid = (uint8_t)cid;
        shadow->group[MODEM_DIAG_GROUP_PACKET].present_fields |=
            MODEM_DIAG_PACKET_APN;
        if (address_field.length != 0u &&
            !telit_view_equals(address_field, "0.0.0.0")) {
            if (telit_diag_copy_preview(shadow->packet.address,
                                        sizeof(shadow->packet.address),
                                        address_field)) {
                shadow->group[MODEM_DIAG_GROUP_PACKET].present_fields |=
                    MODEM_DIAG_PACKET_ADDRESS_TRUNCATED;
            }
            shadow->group[MODEM_DIAG_GROUP_PACKET].present_fields |=
                MODEM_DIAG_PACKET_ADDRESS;
        }
    }
    return MODEM_DIAG_LINE_ACCEPT;
}

static modem_diag_line_result_t telit_diag_parse_qss_v2(
    const char *line, modem_diag_snapshot_t *shadow) {
    if (!telit_starts_with(line, "#QSS:")) {
        return MODEM_DIAG_LINE_IGNORE;
    }
    telit_qss_state_t state;
    bool valid = shadow != NULL && telit_parse_qss_query(line, &state);
    if (valid) {
        shadow->sim.qss_mode = state.mode;
        shadow->sim.qss_status = state.status;
        shadow->group[MODEM_DIAG_GROUP_SIM].present_fields |= MODEM_DIAG_SIM_QSS;
    }
    return telit_diag_result(true, valid);
}

static modem_diag_line_result_t telit_diag_parse_cpin_v2(
    const char *line, modem_diag_snapshot_t *shadow) {
    if (!telit_starts_with(line, "+CPIN:")) {
        return MODEM_DIAG_LINE_IGNORE;
    }
    const char *value = line + strlen("+CPIN:");
    while (*value == ' ') {
        value++;
    }
    size_t length = strlen(value);
    bool valid = shadow != NULL && length > 0u &&
                 length < sizeof(shadow->sim.cpin);
    if (valid) {
        memcpy(shadow->sim.cpin, value, length + 1u);
        shadow->group[MODEM_DIAG_GROUP_SIM].present_fields |= MODEM_DIAG_SIM_CPIN;
    }
    return telit_diag_result(true, valid);
}

static modem_diag_line_result_t telit_diag_parse_cevdp(
    const char *line, modem_diag_snapshot_t *shadow) {
    if (!telit_starts_with(line, "+CEVDP:")) {
        return MODEM_DIAG_LINE_IGNORE;
    }
    telit_csv_view_t fields[1];
    size_t count = 0u;
    uint32_t domain = 0u;
    bool valid = shadow != NULL &&
        telit_view_split_prefixed(line, "+CEVDP:", fields, 1u, &count) &&
        count == 1u && telit_view_parse_u32(fields[0], 4u, &domain) &&
        domain >= 1u;
    if (valid) {
        shadow->voice.service_domain = (uint8_t)domain;
        shadow->group[MODEM_DIAG_GROUP_VOICE].present_fields |=
            MODEM_DIAG_VOICE_DOMAIN;
    }
    return telit_diag_result(true, valid);
}

static modem_diag_line_result_t telit_diag_parse_voice_cireg(
    const char *line, modem_diag_snapshot_t *shadow) {
    if (!telit_starts_with(line, "+CIREG:")) {
        return MODEM_DIAG_LINE_IGNORE;
    }
    telit_csv_view_t fields[3];
    size_t count = 0u;
    uint32_t mode = 0u, registration = 0u;
    bool valid = shadow != NULL &&
        telit_view_split_prefixed(line, "+CIREG:", fields, 3u, &count) &&
        count >= 2u && telit_view_parse_u32(fields[0], 2u, &mode) &&
        telit_view_parse_u32(fields[1], UINT8_MAX, &registration);
    (void)mode;
    if (valid) {
        shadow->voice.ims_registration = (uint8_t)registration;
        shadow->group[MODEM_DIAG_GROUP_VOICE].present_fields |=
            MODEM_DIAG_VOICE_IMS_REG;
    }
    return telit_diag_result(true, valid);
}

static modem_diag_line_result_t telit_diag_parse_temperature_v2(
    const char *line, modem_diag_snapshot_t *shadow) {
    if (!telit_starts_with(line, "#TEMPMEAS:")) {
        return MODEM_DIAG_LINE_IGNORE;
    }
    telit_temperature_t temperature;
    bool valid =
        shadow != NULL && telit_parse_temperature(line, &temperature);
    if (valid) {
        shadow->temperature.level = temperature.level;
        shadow->temperature.celsius = temperature.celsius;
        shadow->group[MODEM_DIAG_GROUP_TEMPERATURE].present_fields = 1u;
    }
    return telit_diag_result(true, valid);
}

modem_diag_line_result_t telit_diag_parse_dvi(
    const char *line, modem_diag_snapshot_t *shadow) {
    if (!telit_starts_with(line, "#DVI:")) {
        return MODEM_DIAG_LINE_IGNORE;
    }
    telit_csv_view_t fields[3];
    size_t count = 0u;
    uint32_t mode = 0u, port = 0u, clock = 0u;
    bool valid = shadow != NULL &&
        telit_view_split_prefixed(line, "#DVI:", fields, 3u, &count) &&
        count == 3u && telit_view_parse_u32(fields[0], 1u, &mode) &&
        telit_view_parse_u32(fields[1], UINT8_MAX, &port) &&
        telit_view_parse_u32(fields[2], 2u, &clock);
    if (valid) {
        shadow->dvi.enabled = (uint8_t)mode;
        shadow->dvi.mode = (uint8_t)port;
        shadow->dvi.clock = (uint8_t)clock;
        shadow->group[MODEM_DIAG_GROUP_DVI].present_fields |=
            MODEM_DIAG_DVI_BASIC;
    }
    return telit_diag_result(true, valid);
}

modem_diag_line_result_t telit_diag_parse_dviext(
    const char *line, modem_diag_snapshot_t *shadow) {
    if (!telit_starts_with(line, "#DVIEXT:")) {
        return MODEM_DIAG_LINE_IGNORE;
    }
    telit_csv_view_t fields[5];
    size_t count = 0u;
    uint32_t values[5] = {0u};
    bool valid = shadow != NULL &&
        telit_view_split_prefixed(line, "#DVIEXT:", fields, 5u, &count) &&
        count == 5u;
    for (uint8_t i = 0u; valid && i < 5u; i++) {
        valid = telit_view_parse_u32(fields[i], i == 1u ? 2u : 1u,
                                     &values[i]);
    }
    if (valid) {
        shadow->dvi.config = (uint8_t)values[0];
        shadow->dvi.sample_rate = (uint8_t)values[1];
        shadow->dvi.sample_width = (uint8_t)values[2];
        shadow->dvi.audio_mode = (uint8_t)values[3];
        shadow->dvi.edge = (uint8_t)values[4];
        shadow->group[MODEM_DIAG_GROUP_DVI].present_fields |= MODEM_DIAG_DVI_EXT;
    }
    return telit_diag_result(true, valid);
}

static bool telit_diag_copy_plain(char *dst, size_t dst_cap,
                                  const char *line) {
    if (dst == NULL || dst_cap == 0u || line == NULL) {
        return false;
    }
    size_t length = strlen(line);
    if (length == 0u || length >= dst_cap) {
        return false;
    }
    bool has_alnum = false;
    for (size_t i = 0u; i < length; i++) {
        unsigned char ch = (unsigned char)line[i];
        if (ch < 0x20u || ch > 0x7eu) {
            return false;
        }
        has_alnum = has_alnum || isalnum(ch);
    }
    if (!has_alnum) {
        return false;
    }
    memcpy(dst, line, length + 1u);
    return true;
}

modem_diag_line_result_t telit_diag_parse_model(
    const char *line, modem_diag_snapshot_t *shadow) {
    if (line == NULL || strncmp(line, "LE910", 5u) != 0) {
        return MODEM_DIAG_LINE_IGNORE;
    }
    bool valid = shadow != NULL &&
        telit_diag_copy_plain(shadow->identity.model,
                              sizeof(shadow->identity.model), line);
    if (valid) {
        shadow->group[MODEM_DIAG_GROUP_IDENTITY].present_fields |=
            MODEM_DIAG_ID_MODEL;
    }
    return telit_diag_result(true, valid);
}

modem_diag_line_result_t telit_diag_parse_firmware(
    const char *line, modem_diag_snapshot_t *shadow) {
    if (line == NULL || line[0] == '+' || line[0] == '#' || line[0] == '^' ||
        strcmp(line, "OK") == 0 || strcmp(line, "ERROR") == 0 ||
        strcmp(line, "RING") == 0 || strcmp(line, "NO CARRIER") == 0) {
        return MODEM_DIAG_LINE_IGNORE;
    }
    bool valid = shadow != NULL &&
        telit_diag_copy_plain(shadow->identity.firmware,
                              sizeof(shadow->identity.firmware), line);
    if (valid) {
        shadow->group[MODEM_DIAG_GROUP_IDENTITY].present_fields |=
            MODEM_DIAG_ID_FIRMWARE;
    }
    return telit_diag_result(true, valid);
}

modem_diag_line_result_t telit_diag_parse_imei(
    const char *line, modem_diag_snapshot_t *shadow) {
    if (line == NULL || strlen(line) != 15u) {
        return MODEM_DIAG_LINE_IGNORE;
    }
    for (size_t i = 0u; i < 15u; i++) {
        if (!isdigit((unsigned char)line[i])) {
            return MODEM_DIAG_LINE_IGNORE;
        }
    }
    if (shadow == NULL) {
        return MODEM_DIAG_LINE_INVALID;
    }
    memcpy(shadow->identity.imei, line, 16u);
    shadow->group[MODEM_DIAG_GROUP_IDENTITY].present_fields |=
        MODEM_DIAG_ID_IMEI;
    return MODEM_DIAG_LINE_ACCEPT;
}

static modem_diag_line_result_t telit_diag_parse_ceer(
    const char *line, modem_diag_snapshot_t *shadow) {
    if (!telit_starts_with(line, "+CEER:")) {
        return MODEM_DIAG_LINE_IGNORE;
    }
    const char *text = line + strlen("+CEER:");
    while (*text == ' ') {
        text++;
    }
    size_t length = strlen(text);
    if (length >= 2u && text[0] == '"' && text[length - 1u] == '"') {
        text++;
        length -= 2u;
    }
    if (shadow == NULL || length == 0u ||
        length >= sizeof(shadow->call_cause.ceer)) {
        return MODEM_DIAG_LINE_INVALID;
    }
    for (size_t i = 0u; i < length; i++) {
        unsigned char ch = (unsigned char)text[i];
        if (ch < 0x20u || ch > 0x7eu) {
            return MODEM_DIAG_LINE_INVALID;
        }
    }
    memcpy(shadow->call_cause.ceer, text, length);
    shadow->call_cause.ceer[length] = '\0';
    shadow->group[MODEM_DIAG_GROUP_CALL_CAUSE].present_fields = 1u;
    return MODEM_DIAG_LINE_ACCEPT;
}

modem_diag_line_result_t telit_diag_parse_csms(
    const char *line, modem_diag_snapshot_t *shadow) {
    if (!telit_starts_with(line, "+CSMS:")) {
        return MODEM_DIAG_LINE_IGNORE;
    }
    telit_csv_view_t fields[4];
    size_t count = 0u;
    uint32_t values[4] = {0u};
    bool valid = shadow != NULL &&
        telit_view_split_prefixed(line, "+CSMS:", fields, 4u, &count) &&
        count == 4u;
    for (uint8_t i = 0u; valid && i < 4u; i++) {
        valid = telit_view_parse_u32(fields[i], i == 0u ? 1u : 1u,
                                     &values[i]);
    }
    if (valid) {
        shadow->sms.csms_service = (uint8_t)values[0];
        shadow->sms.csms_mt = (uint8_t)values[1];
        shadow->sms.csms_mo = (uint8_t)values[2];
        shadow->sms.csms_bm = (uint8_t)values[3];
        shadow->group[MODEM_DIAG_GROUP_SMS_CONFIG].present_fields |=
            MODEM_DIAG_SMS_CSMS;
    }
    return telit_diag_result(true, valid);
}

modem_diag_line_result_t telit_diag_parse_cnmi(
    const char *line, modem_diag_snapshot_t *shadow) {
    if (!telit_starts_with(line, "+CNMI:")) {
        return MODEM_DIAG_LINE_IGNORE;
    }
    static const uint8_t maxima[5] = {3u, 3u, 3u, 2u, 1u};
    telit_csv_view_t fields[5];
    size_t count = 0u;
    uint32_t values[5] = {0u};
    bool valid = shadow != NULL &&
        telit_view_split_prefixed(line, "+CNMI:", fields, 5u, &count) &&
        count == 5u;
    for (uint8_t i = 0u; valid && i < 5u; i++) {
        valid = telit_view_parse_u32(fields[i], maxima[i], &values[i]);
    }
    if (valid) {
        shadow->sms.cnmi_mode = (uint8_t)values[0];
        shadow->sms.cnmi_mt = (uint8_t)values[1];
        shadow->sms.cnmi_bm = (uint8_t)values[2];
        shadow->sms.cnmi_ds = (uint8_t)values[3];
        shadow->sms.cnmi_bfr = (uint8_t)values[4];
        shadow->group[MODEM_DIAG_GROUP_SMS_CONFIG].present_fields |=
            MODEM_DIAG_SMS_CNMI;
    }
    return telit_diag_result(true, valid);
}

modem_diag_line_result_t telit_diag_parse_csmp(
    const char *line, modem_diag_snapshot_t *shadow) {
    if (!telit_starts_with(line, "+CSMP:")) {
        return MODEM_DIAG_LINE_IGNORE;
    }
    telit_csv_view_t fields[4];
    size_t count = 0u;
    uint32_t values[4] = {0u};
    bool valid = shadow != NULL &&
        telit_view_split_prefixed(line, "+CSMP:", fields, 4u, &count) &&
        count == 4u;
    for (uint8_t i = 0u; valid && i < 4u; i++) {
        valid = telit_view_parse_u32(fields[i], UINT8_MAX, &values[i]);
    }
    if (valid) {
        shadow->sms.csmp_fo = (uint8_t)values[0];
        shadow->sms.csmp_vp = (uint8_t)values[1];
        shadow->sms.csmp_pid = (uint8_t)values[2];
        shadow->sms.csmp_dcs = (uint8_t)values[3];
        shadow->group[MODEM_DIAG_GROUP_SMS_CONFIG].present_fields |=
            MODEM_DIAG_SMS_CSMP;
    }
    return telit_diag_result(true, valid);
}

modem_diag_line_result_t telit_diag_parse_csca(
    const char *line, modem_diag_snapshot_t *shadow) {
    if (!telit_starts_with(line, "+CSCA:")) {
        return MODEM_DIAG_LINE_IGNORE;
    }
    telit_csv_view_t fields[2];
    size_t count = 0u;
    uint32_t type = 0u;
    bool valid = shadow != NULL &&
        telit_view_split_prefixed(line, "+CSCA:", fields, 2u, &count) &&
        count >= 1u && count <= 2u &&
        telit_view_copy_exact(shadow->sms.service_center,
                              sizeof(shadow->sms.service_center), fields[0]);
    if (valid && count == 2u) {
        valid = telit_view_parse_u32(fields[1], UINT8_MAX, &type);
    }
    if (valid) {
        shadow->sms.service_center_type = count == 2u ? (uint8_t)type : 0u;
        shadow->group[MODEM_DIAG_GROUP_SMS_CONFIG].present_fields |=
            MODEM_DIAG_SMS_CSCA;
    }
    return telit_diag_result(true, valid);
}

modem_diag_line_result_t telit_diag_parse_csdh(
    const char *line, modem_diag_snapshot_t *shadow) {
    if (!telit_starts_with(line, "+CSDH:")) {
        return MODEM_DIAG_LINE_IGNORE;
    }
    telit_csv_view_t fields[1];
    size_t count = 0u;
    uint32_t value = 0u;
    bool valid = shadow != NULL &&
        telit_view_split_prefixed(line, "+CSDH:", fields, 1u, &count) &&
        count == 1u && telit_view_parse_u32(fields[0], 1u, &value);
    if (valid) {
        shadow->sms.csdh = (uint8_t)value;
        shadow->group[MODEM_DIAG_GROUP_SMS_CONFIG].present_fields |=
            MODEM_DIAG_SMS_CSDH;
    }
    return telit_diag_result(true, valid);
}

modem_diag_line_result_t telit_diag_parse_ismscfg_v2(
    const char *line, modem_diag_snapshot_t *shadow) {
    if (!telit_starts_with(line, "#ISMSCFG:")) {
        return MODEM_DIAG_LINE_IGNORE;
    }
    uint8_t mode = 0u;
    bool valid = shadow != NULL && telit_parse_ismscfg(line, &mode);
    if (valid) {
        shadow->sms.ims_mode = mode;
        shadow->group[MODEM_DIAG_GROUP_SMS_CONFIG].present_fields |=
            MODEM_DIAG_SMS_IMS;
    }
    return telit_diag_result(true, valid);
}

modem_diag_line_result_t telit_diag_parse_mwi_v2(
    const char *line, modem_diag_snapshot_t *shadow) {
    if (!telit_starts_with(line, "#MWI:")) {
        return MODEM_DIAG_LINE_IGNORE;
    }
    telit_mwi_state_t state;
    bool valid = shadow != NULL && telit_parse_mwi_query(line, &state);
    if (valid) {
        shadow->sms.mwi_enabled = state.enabled;
        if (state.category == MODEM_MESSAGE_WAITING_ALL) {
            memset(&shadow->sms.message_waiting, 0,
                   sizeof(shadow->sms.message_waiting));
        } else if (state.category < MODEM_MESSAGE_WAITING_CATEGORY_COUNT) {
            modem_message_waiting_state_t *waiting =
                &shadow->sms.message_waiting.category[state.category];
            waiting->active = state.active;
            waiting->count = state.active ? state.count : 0u;
        }
        shadow->group[MODEM_DIAG_GROUP_SMS_CONFIG].present_fields |=
            MODEM_DIAG_SMS_MWI;
    }
    return telit_diag_result(true, valid);
}

modem_diag_line_result_t telit_diag_parse_cpms_v2(
    const char *line, modem_diag_snapshot_t *shadow) {
    if (!telit_starts_with(line, "+CPMS:")) {
        return MODEM_DIAG_LINE_IGNORE;
    }
    telit_csv_view_t fields[9];
    size_t count = 0u;
    uint32_t used[3] = {0u};
    uint32_t total[3] = {0u};
    bool valid = shadow != NULL &&
        telit_view_split_prefixed(line, "+CPMS:", fields, 9u, &count) &&
        count == 9u;
    for (uint8_t i = 0u; valid && i < 3u; i++) {
        valid = telit_view_parse_u32(fields[i * 3u + 1u], UINT16_MAX,
                                     &used[i]) &&
                telit_view_parse_u32(fields[i * 3u + 2u], UINT16_MAX,
                                     &total[i]);
    }
    if (valid) {
        valid = telit_view_copy_exact(shadow->storage.sms_read_store,
                                      sizeof(shadow->storage.sms_read_store),
                                      fields[0]) &&
                telit_view_copy_exact(shadow->storage.sms_write_store,
                                      sizeof(shadow->storage.sms_write_store),
                                      fields[3]) &&
                telit_view_copy_exact(shadow->storage.sms_receive_store,
                                      sizeof(shadow->storage.sms_receive_store),
                                      fields[6]);
    }
    if (valid) {
        shadow->storage.sms_used = (uint16_t)used[0];
        shadow->storage.sms_total = (uint16_t)total[0];
        shadow->group[MODEM_DIAG_GROUP_STORAGE_CAPS].present_fields |=
            MODEM_DIAG_STORAGE_SMS;
    }
    return telit_diag_result(true, valid);
}

modem_diag_line_result_t telit_diag_parse_cpbs_v2(
    const char *line, modem_diag_snapshot_t *shadow) {
    if (!telit_starts_with(line, "+CPBS:")) {
        return MODEM_DIAG_LINE_IGNORE;
    }
    telit_csv_view_t fields[3];
    size_t count = 0u;
    uint32_t used = 0u, total = 0u;
    bool valid = shadow != NULL &&
        telit_view_split_prefixed(line, "+CPBS:", fields, 3u, &count) &&
        count == 3u &&
        telit_view_copy_exact(shadow->storage.phonebook_store,
                              sizeof(shadow->storage.phonebook_store), fields[0]) &&
        telit_view_parse_u32(fields[1], UINT16_MAX, &used) &&
        telit_view_parse_u32(fields[2], UINT16_MAX, &total);
    if (valid) {
        shadow->storage.phonebook_used = (uint16_t)used;
        shadow->storage.phonebook_total = (uint16_t)total;
        shadow->group[MODEM_DIAG_GROUP_STORAGE_CAPS].present_fields |=
            MODEM_DIAG_STORAGE_PHONEBOOK;
    }
    return telit_diag_result(true, valid);
}

bool telit_diag_group_finish(modem_diag_group_t group,
                                    modem_diag_snapshot_t *shadow) {
    if (shadow == NULL || group <= MODEM_DIAG_GROUP_NONE ||
        group >= MODEM_DIAG_GROUP_COUNT) {
        return false;
    }
    uint64_t present = shadow->group[group].present_fields;
    switch (group) {
    case MODEM_DIAG_GROUP_SERVING:
        return (present & (MODEM_DIAG_SERVING_RAT |
                           MODEM_DIAG_SERVING_CHANNEL |
                           MODEM_DIAG_SERVING_BAND)) ==
               (MODEM_DIAG_SERVING_RAT | MODEM_DIAG_SERVING_CHANNEL |
                MODEM_DIAG_SERVING_BAND);
    case MODEM_DIAG_GROUP_REGISTRATION:
        return (present & (MODEM_DIAG_REG_CS | MODEM_DIAG_REG_PS |
                           MODEM_DIAG_REG_EPS)) != 0u;
    case MODEM_DIAG_GROUP_RADIO_POLICY:
        return (present & (MODEM_DIAG_POLICY_CFUN | MODEM_DIAG_POLICY_COPS)) ==
               (MODEM_DIAG_POLICY_CFUN | MODEM_DIAG_POLICY_COPS);
    case MODEM_DIAG_GROUP_TUNER: {
        uint64_t required = MODEM_DIAG_TUNER_ENABLED |
                            MODEM_DIAG_TUNER_SUPPORTED_MASK |
                            MODEM_DIAG_TUNER_TABLE;
        if ((present & required) != required ||
            shadow->tuner.supported_mask == 0u) {
            return false;
        }
        uint64_t observed[TELIT_TUNE_RF_COUNT] = {0u};
        uint64_t union_mask = 0u;
        for (uint8_t i = 0u; i < shadow->tuner.row_count; i++) {
            const modem_diag_tuner_row_t *row = &shadow->tuner.rows[i];
            telit_tune_rf_t rf = telit_tune_rf_from_controls(
                row->ctrl1, row->ctrl2);
            if (rf >= TELIT_TUNE_RF_COUNT || row->band_mask == 0u ||
                (union_mask & row->band_mask) != 0u) {
                return false;
            }
            observed[rf] |= row->band_mask;
            union_mask |= row->band_mask;
        }
        shadow->tuner.table_complete =
            union_mask == shadow->tuner.supported_mask;
        shadow->tuner.table_exact = shadow->tuner.enabled &&
            shadow->tuner.table_complete;
        uint64_t policy_union = telit_tune_policy_union();
        for (telit_tune_rf_t rf = TELIT_TUNE_RF1;
             rf < TELIT_TUNE_RF_COUNT; rf++) {
            uint64_t expected = shadow->tuner.supported_mask &
                                telit_tune_policy_mask(rf);
            if (rf == TELIT_TUNE_RF1) {
                expected |= shadow->tuner.supported_mask & ~policy_union;
            }
            if (observed[rf] != expected) {
                shadow->tuner.table_exact = false;
            }
        }
        return true;
    }
    case MODEM_DIAG_GROUP_PACKET:
        return (present & MODEM_DIAG_PACKET_ATTACH) != 0u;
    case MODEM_DIAG_GROUP_SIM:
        return (present & MODEM_DIAG_SIM_QSS) != 0u;
    case MODEM_DIAG_GROUP_VOICE:
        return (present & MODEM_DIAG_VOICE_DOMAIN) != 0u;
    case MODEM_DIAG_GROUP_TEMPERATURE:
        return present != 0u;
    case MODEM_DIAG_GROUP_IDENTITY:
        return (present & (MODEM_DIAG_ID_MODEL | MODEM_DIAG_ID_FIRMWARE |
                           MODEM_DIAG_ID_IMEI)) ==
               (MODEM_DIAG_ID_MODEL | MODEM_DIAG_ID_FIRMWARE |
                MODEM_DIAG_ID_IMEI);
    case MODEM_DIAG_GROUP_DVI:
        return (present & (MODEM_DIAG_DVI_BASIC | MODEM_DIAG_DVI_EXT)) ==
               (MODEM_DIAG_DVI_BASIC | MODEM_DIAG_DVI_EXT);
    case MODEM_DIAG_GROUP_CALL_CAUSE:
        return present != 0u;
    case MODEM_DIAG_GROUP_SMS_CONFIG:
        return (present & (MODEM_DIAG_SMS_CSMS | MODEM_DIAG_SMS_CNMI |
                           MODEM_DIAG_SMS_CSMP | MODEM_DIAG_SMS_CSCA |
                           MODEM_DIAG_SMS_CSDH)) ==
               (MODEM_DIAG_SMS_CSMS | MODEM_DIAG_SMS_CNMI |
                MODEM_DIAG_SMS_CSMP | MODEM_DIAG_SMS_CSCA |
                MODEM_DIAG_SMS_CSDH);
    case MODEM_DIAG_GROUP_STORAGE_CAPS:
        return (present & (MODEM_DIAG_STORAGE_SMS |
                           MODEM_DIAG_STORAGE_PHONEBOOK)) ==
               (MODEM_DIAG_STORAGE_SMS | MODEM_DIAG_STORAGE_PHONEBOOK);
    case MODEM_DIAG_GROUP_NONE:
    case MODEM_DIAG_GROUP_COUNT:
        return false;
    }
    return false;
}

#define TELIT_DIAG_QUERY(group_, cmd_, timeout_, prefix_, minimum_, optional_, parse_) \
    {                                                                                 \
        .group = (group_), .cmd = (cmd_), .timeout_ms = (timeout_),                   \
        .prefix = (prefix_), .minimum_lines = (minimum_),                            \
        .optional = (optional_), .parse = (parse_),                                  \
    }

#define TELIT_DIAG_ISOLATED_QUERY(group_, cmd_, timeout_, prefix_, minimum_, parse_) \
    {                                                                                 \
        .group = (group_), .cmd = (cmd_), .timeout_ms = (timeout_),                   \
        .prefix = (prefix_), .minimum_lines = (minimum_), .optional = true,           \
        .isolate_malformed = true, .parse = (parse_),                                 \
    }

const modem_diag_query_t TELIT_DIAG_QUERIES[] = {
    TELIT_DIAG_QUERY(MODEM_DIAG_GROUP_SERVING, "AT#RFSTS", 5000u,
                     "#RFSTS:", 1u, false, telit_diag_parse_rfsts),
    TELIT_DIAG_QUERY(MODEM_DIAG_GROUP_SERVING, "AT#MONI", 5000u,
                     "#MONI:", 1u, true, telit_diag_parse_moni),

    TELIT_DIAG_QUERY(MODEM_DIAG_GROUP_REGISTRATION, "AT+CREG?", 3000u,
                     "+CREG:", 1u, false, telit_diag_parse_creg),
    TELIT_DIAG_QUERY(MODEM_DIAG_GROUP_REGISTRATION, "AT+CGREG?", 3000u,
                     "+CGREG:", 1u, false, telit_diag_parse_cgreg),
    TELIT_DIAG_QUERY(MODEM_DIAG_GROUP_REGISTRATION, "AT+CEREG?", 3000u,
                     "+CEREG:", 1u, false, telit_diag_parse_cereg),
    TELIT_DIAG_QUERY(MODEM_DIAG_GROUP_REGISTRATION, "AT+CIREG?", 3000u,
                     "+CIREG:", 1u, true, telit_diag_parse_cireg),

    TELIT_DIAG_QUERY(MODEM_DIAG_GROUP_RADIO_POLICY, "AT+CFUN?", 3000u,
                     "+CFUN:", 1u, false, telit_diag_parse_cfun),
    TELIT_DIAG_QUERY(MODEM_DIAG_GROUP_RADIO_POLICY, "AT+COPS?", 5000u,
                     "+COPS:", 1u, false, telit_diag_parse_cops),
    TELIT_DIAG_QUERY(MODEM_DIAG_GROUP_RADIO_POLICY, "AT#ENS?", 3000u,
                     "#ENS:", 1u, true, telit_diag_parse_ens),
    TELIT_DIAG_QUERY(MODEM_DIAG_GROUP_RADIO_POLICY, "AT#FWSWITCH?", 3000u,
                     "#FWSWITCH:", 1u, true, telit_diag_parse_fwswitch_v2),
    TELIT_DIAG_QUERY(MODEM_DIAG_GROUP_RADIO_POLICY, "AT#FWAUTOSIM?", 3000u,
                     "#FWAUTOSIM:", 1u, true, telit_diag_parse_fwautosim_v2),
    TELIT_DIAG_QUERY(MODEM_DIAG_GROUP_RADIO_POLICY, "AT+WS46?", 3000u,
                     "+WS46:", 1u, true, telit_diag_parse_ws46),
    TELIT_DIAG_QUERY(MODEM_DIAG_GROUP_RADIO_POLICY, "AT#SELBNDMODE?", 3000u,
                     "#SELBNDMODE:", 1u, true, telit_diag_parse_selbndmode),
    TELIT_DIAG_QUERY(MODEM_DIAG_GROUP_RADIO_POLICY, "AT#BND?", 3000u,
                     "#BND:", 1u, true, telit_diag_parse_bnd),
    TELIT_DIAG_QUERY(MODEM_DIAG_GROUP_RADIO_POLICY, "AT#BNDRAM?", 3000u,
                     "#BNDRAM:", 1u, true, telit_diag_parse_bndram),
    TELIT_DIAG_QUERY(MODEM_DIAG_GROUP_RADIO_POLICY, "AT#NWSCANTMR?", 3000u,
                     "#NWSCANTMR:", 1u, true, telit_diag_parse_scan_config),
    TELIT_DIAG_QUERY(MODEM_DIAG_GROUP_RADIO_POLICY, "AT#NWSCANTMR", 3000u,
                     "#NWSCANTMREXP:", 1u, true,
                     telit_diag_parse_scan_remaining),

    TELIT_DIAG_QUERY(MODEM_DIAG_GROUP_TUNER, "AT#STUNEANT?", 3000u,
                     "#STUNEANT:", 1u, false, telit_diag_parse_stune_enabled),
    TELIT_DIAG_QUERY(MODEM_DIAG_GROUP_TUNER, "AT#STUNEANT=?", 3000u,
                     "#STUNEANT:", 1u, false,
                     telit_diag_parse_stune_capability),
    TELIT_DIAG_QUERY(MODEM_DIAG_GROUP_TUNER, "AT#GTUNEANT?", 5000u,
                     "#GTUNEANT:", 1u, false, telit_diag_parse_gtune),

    TELIT_DIAG_QUERY(MODEM_DIAG_GROUP_PACKET, "AT+CGATT?", 3000u,
                     "+CGATT:", 1u, false, telit_diag_parse_cgatt),
    TELIT_DIAG_QUERY(MODEM_DIAG_GROUP_PACKET, "AT+CGACT?", 3000u,
                     "+CGACT:", 0u, true, telit_diag_parse_cgact),
    TELIT_DIAG_ISOLATED_QUERY(MODEM_DIAG_GROUP_PACKET, "AT+CGCONTRDP",
                              3000u, "+CGCONTRDP:", 0u,
                              telit_diag_parse_cgcontrdp),

    TELIT_DIAG_QUERY(MODEM_DIAG_GROUP_SIM, "AT#QSS?", 3000u,
                     "#QSS:", 1u, false, telit_diag_parse_qss_v2),
    TELIT_DIAG_QUERY(MODEM_DIAG_GROUP_SIM, "AT+CPIN?", 3000u,
                     "+CPIN:", 1u, true, telit_diag_parse_cpin_v2),

    TELIT_DIAG_QUERY(MODEM_DIAG_GROUP_VOICE, "AT+CEVDP?", 3000u,
                     "+CEVDP:", 1u, false, telit_diag_parse_cevdp),
    TELIT_DIAG_QUERY(MODEM_DIAG_GROUP_VOICE, "AT+CIREG?", 3000u,
                     "+CIREG:", 1u, true, telit_diag_parse_voice_cireg),

    TELIT_DIAG_QUERY(MODEM_DIAG_GROUP_TEMPERATURE, "AT#TEMPMON=1", 3000u,
                     "#TEMPMEAS:", 1u, false,
                     telit_diag_parse_temperature_v2),

    TELIT_DIAG_QUERY(MODEM_DIAG_GROUP_IDENTITY, "AT+CGMM", 3000u,
                     NULL, 1u, false, telit_diag_parse_model),
    TELIT_DIAG_QUERY(MODEM_DIAG_GROUP_IDENTITY, "AT+CGMR", 3000u,
                     NULL, 1u, false, telit_diag_parse_firmware),
    TELIT_DIAG_QUERY(MODEM_DIAG_GROUP_IDENTITY, "AT+CGSN", 3000u,
                     NULL, 1u, false, telit_diag_parse_imei),

    TELIT_DIAG_QUERY(MODEM_DIAG_GROUP_DVI, "AT#DVI?", 3000u,
                     "#DVI:", 1u, false, telit_diag_parse_dvi),
    TELIT_DIAG_QUERY(MODEM_DIAG_GROUP_DVI, "AT#DVIEXT?", 3000u,
                     "#DVIEXT:", 1u, false, telit_diag_parse_dviext),

    TELIT_DIAG_QUERY(MODEM_DIAG_GROUP_CALL_CAUSE, "AT+CEER", 3000u,
                     "+CEER:", 1u, false, telit_diag_parse_ceer),

    TELIT_DIAG_QUERY(MODEM_DIAG_GROUP_SMS_CONFIG, "AT+CSMS?", 3000u,
                     "+CSMS:", 1u, false, telit_diag_parse_csms),
    TELIT_DIAG_QUERY(MODEM_DIAG_GROUP_SMS_CONFIG, "AT+CNMI?", 3000u,
                     "+CNMI:", 1u, false, telit_diag_parse_cnmi),
    TELIT_DIAG_QUERY(MODEM_DIAG_GROUP_SMS_CONFIG, "AT+CSMP?", 3000u,
                     "+CSMP:", 1u, false, telit_diag_parse_csmp),
    TELIT_DIAG_QUERY(MODEM_DIAG_GROUP_SMS_CONFIG, "AT+CSCA?", 3000u,
                     "+CSCA:", 1u, false, telit_diag_parse_csca),
    TELIT_DIAG_QUERY(MODEM_DIAG_GROUP_SMS_CONFIG, "AT+CSDH?", 3000u,
                     "+CSDH:", 1u, false, telit_diag_parse_csdh),
    TELIT_DIAG_QUERY(MODEM_DIAG_GROUP_SMS_CONFIG, "AT#ISMSCFG?", 3000u,
                     "#ISMSCFG:", 1u, true, telit_diag_parse_ismscfg_v2),
    TELIT_DIAG_QUERY(MODEM_DIAG_GROUP_SMS_CONFIG, "AT#MWI?", 3000u,
                     "#MWI:", 1u, true, telit_diag_parse_mwi_v2),

    TELIT_DIAG_QUERY(MODEM_DIAG_GROUP_STORAGE_CAPS, "AT+CPMS?", 5000u,
                     "+CPMS:", 1u, false, telit_diag_parse_cpms_v2),
    TELIT_DIAG_QUERY(MODEM_DIAG_GROUP_STORAGE_CAPS, "AT+CPBS?", 3000u,
                     "+CPBS:", 1u, false, telit_diag_parse_cpbs_v2),
};

_Static_assert(sizeof(TELIT_DIAG_QUERIES) /
                   sizeof(TELIT_DIAG_QUERIES[0]) == TELIT_DIAG_QUERY_COUNT,
               "Telit diagnostic query count contract drifted");
_Static_assert(sizeof(TELIT_DIAG_QUERIES) /
                   sizeof(TELIT_DIAG_QUERIES[0]) <= UINT8_MAX,
               "Telit diagnostic query table exceeds scheduler index");

#undef TELIT_DIAG_QUERY

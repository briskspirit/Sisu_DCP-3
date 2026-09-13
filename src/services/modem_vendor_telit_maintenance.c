#include "modem_vendor_telit_internal.h"

#include <stdio.h>

/* ------------------------------------------------------------------------ */
/* Guarded maintenance command boundary.                                    */
/* ------------------------------------------------------------------------ */

static bool telit_maintenance_format(char *out, size_t out_cap,
                                     const char *format, unsigned value) {
    if (out == NULL || out_cap == 0u || format == NULL) {
        return false;
    }
    int length = snprintf(out, out_cap, format, value);
    return length > 0 && (size_t)length < out_cap;
}

bool telit_maintenance_build_scan_timer(uint16_t seconds, char *out,
                                               size_t out_cap) {
    return seconds >= 5u && seconds <= 3600u &&
           telit_maintenance_format(out, out_cap, "AT#NWSCANTMR=%u",
                                    (unsigned)seconds);
}

modem_diag_line_result_t telit_maintenance_parse_scan_timer(
    const char *line, uint16_t *seconds) {
    return telit_parse_scan_timer_value(line, seconds);
}

bool telit_maintenance_build_band_mode(uint8_t mode, char *out,
                                              size_t out_cap) {
    return mode <= 1u &&
           telit_maintenance_format(out, out_cap, "AT#SELBNDMODE=%u",
                                    (unsigned)mode);
}

modem_diag_line_result_t telit_maintenance_parse_band_mode(
    const char *line, uint8_t *mode) {
    return telit_parse_band_mode_value(line, mode);
}

static modem_diag_line_result_t telit_maintenance_parse_band_common(
    const char *line, const char *prefix, modem_band_config_t *config) {
    if (!telit_starts_with(line, prefix)) {
        return MODEM_DIAG_LINE_IGNORE;
    }
    telit_csv_view_t fields[4];
    size_t count = 0u;
    uint32_t gsm = 0u;
    uint32_t wcdma = 0u;
    uint64_t lte = 0u;
    if (config == NULL ||
        !telit_view_split_prefixed(line, prefix, fields, 4u, &count) ||
        count < 3u ||
        !telit_view_parse_u32(fields[0], UINT8_MAX, &gsm) ||
        !telit_view_parse_u32(fields[1], UINT16_MAX, &wcdma) ||
        !telit_view_parse_hex_u64(fields[2], UINT64_MAX, &lte)) {
        return MODEM_DIAG_LINE_INVALID;
    }
    config->gsm = (uint8_t)gsm;
    config->wcdma = (uint16_t)wcdma;
    config->lte = lte;
    return MODEM_DIAG_LINE_ACCEPT;
}

modem_diag_line_result_t telit_maintenance_parse_band_nvm(
    const char *line, modem_band_config_t *config) {
    return telit_maintenance_parse_band_common(line, "#BND:", config);
}

modem_diag_line_result_t telit_maintenance_parse_band_ram(
    const char *line, modem_band_config_t *config) {
    return telit_maintenance_parse_band_common(line, "#BNDRAM:", config);
}

bool telit_maintenance_build_band_ram(
    const modem_band_config_t *config, char *out, size_t out_cap) {
    if (config == NULL || out == NULL || out_cap == 0u || config->lte == 0u) {
        return false;
    }
    int length = snprintf(out, out_cap, "AT#BNDRAM=%u,%u,%llX",
                          (unsigned)config->gsm,
                          (unsigned)config->wcdma,
                          (unsigned long long)config->lte);
    return length > 0 && (size_t)length < out_cap;
}

bool telit_maintenance_band_preset(
    uint8_t preset, const modem_band_config_t *active_config,
    modem_band_config_t *desired) {
    static const uint64_t LTE_MASKS[] = {
        UINT64_C(0x2),    /* B2 */
        UINT64_C(0x8),    /* B4 */
        UINT64_C(0x10),   /* B5 */
        UINT64_C(0x800),  /* B12 */
        UINT64_C(0x2000), /* B14 */
    };
    if (active_config == NULL || desired == NULL || preset == 0u ||
        preset > sizeof(LTE_MASKS) / sizeof(LTE_MASKS[0])) {
        return false;
    }
    uint64_t mask = LTE_MASKS[preset - 1u];
    if ((active_config->lte & mask) == 0u) {
        return false;
    }
    *desired = *active_config;
    desired->lte = mask;
    return true;
}

bool telit_maintenance_build_function(uint8_t mode, char *out,
                                             size_t out_cap) {
    return (mode == 4u || mode == 5u) &&
           telit_maintenance_format(out, out_cap, "AT+CFUN=%u",
                                    (unsigned)mode);
}

modem_diag_line_result_t telit_maintenance_parse_function(
    const char *line, uint8_t *mode) {
    if (!telit_starts_with(line, "+CFUN:")) {
        return MODEM_DIAG_LINE_IGNORE;
    }
    telit_csv_view_t fields[1];
    size_t count = 0u;
    uint32_t value = 0u;
    if (mode == NULL ||
        !telit_view_split_prefixed(line, "+CFUN:", fields, 1u, &count) ||
        count != 1u || !telit_view_parse_u32(fields[0], UINT8_MAX, &value)) {
        return MODEM_DIAG_LINE_INVALID;
    }
    *mode = (uint8_t)value;
    return MODEM_DIAG_LINE_ACCEPT;
}

bool telit_maintenance_build_tuner_enabled(bool enabled, char *out,
                                                  size_t out_cap) {
    return telit_maintenance_format(out, out_cap, "AT#STUNEANT=%u",
                                    enabled ? 1u : 0u);
}

modem_diag_line_result_t telit_maintenance_parse_tuner_enabled(
    const char *line, bool *enabled) {
    return telit_parse_tuner_enabled_value(line, enabled);
}

modem_diag_line_result_t telit_maintenance_parse_tuner_row(
    const char *line) {
    if (!telit_starts_with(line, "#GTUNEANT:")) {
        return MODEM_DIAG_LINE_IGNORE;
    }
    modem_provision_line_t result = telit_provision_gtune_line(line);
    return result == MODEM_PROVISION_LINE_INVALID
               ? MODEM_DIAG_LINE_INVALID : MODEM_DIAG_LINE_ACCEPT;
}

bool telit_maintenance_tuner_table_exact(void) {
    return telit_tune_readback_exact();
}

bool telit_maintenance_build_tuner_row(uint8_t rf_state, char *out,
                                              size_t out_cap) {
    if (rf_state < 1u || rf_state > TELIT_TUNE_RF_COUNT) {
        return false;
    }
    return telit_tune_build_row((telit_tune_rf_t)(rf_state - 1u), out,
                                out_cap);
}

bool telit_maintenance_antenna_controls(uint8_t rf_state, bool *a,
                                               bool *b) {
    if (rf_state < 1u || rf_state > TELIT_TUNE_RF_COUNT || a == NULL ||
        b == NULL) {
        return false;
    }
    return telit_tune_controls((telit_tune_rf_t)(rf_state - 1u), a, b);
}

bool telit_maintenance_build_gpio_query(uint8_t pin, char *out,
                                               size_t out_cap) {
    if (pin == 0u) {
        return false;
    }
    int length = snprintf(out, out_cap, "AT#GPIO=%u,2", (unsigned)pin);
    return length > 0 && (size_t)length < out_cap;
}

bool telit_maintenance_build_gpio_set(uint8_t pin, bool state,
                                             uint8_t direction, bool save,
                                             char *out, size_t out_cap) {
    if (pin == 0u || direction > 20u) {
        return false;
    }
    int length = snprintf(out, out_cap, "AT#GPIO=%u,%u,%u,%u",
                          (unsigned)pin, state ? 1u : 0u,
                          (unsigned)direction, save ? 1u : 0u);
    return length > 0 && (size_t)length < out_cap;
}

modem_diag_line_result_t telit_maintenance_parse_gpio(
    const char *line, modem_gpio_config_t *config) {
    if (!telit_starts_with(line, "#GPIO:")) {
        return MODEM_DIAG_LINE_IGNORE;
    }
    telit_csv_view_t fields[2];
    size_t count = 0u;
    uint32_t direction = 0u;
    uint32_t state = 0u;
    if (config == NULL ||
        !telit_view_split_prefixed(line, "#GPIO:", fields, 2u, &count) ||
        count != 2u ||
        !telit_view_parse_u32(fields[0], 20u, &direction) ||
        !telit_view_parse_u32(fields[1], 1u, &state)) {
        return MODEM_DIAG_LINE_INVALID;
    }
    config->direction = (uint8_t)direction;
    config->state = state != 0u;
    return MODEM_DIAG_LINE_ACCEPT;
}

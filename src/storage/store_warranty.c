#include "storage/store_service.h"

#include "storage_bytes.h"
#include "store_service_internal.h"

#include <string.h>

#include "services/log.h"

#define WARRANTY_MAGIC 0x57525431u
#define STORE_WARRANTY_DEFAULT_MADE "0899"
#define STORE_WARRANTY_DEFAULT_REPAIRED "0000"

_Static_assert(STORE_WARRANTY_SERIAL_MAX == 15u,
               "board IMEI validation assumes a 15-digit identity");

static store_warranty_state_t s_warranty;

store_status_t store_warranty_get(store_warranty_state_t *out_state) {
    if (out_state == 0) {
        return STORE_STATUS_INVALID_ARGUMENT;
    }
    *out_state = s_warranty;
    return STORE_STATUS_OK;
}

bool store_board_imei_valid(const char *imei) {
    if (imei == 0 || strlen(imei) != STORE_WARRANTY_SERIAL_MAX) {
        return false;
    }

    unsigned sum = 0u;
    for (uint8_t i = 0u; i < STORE_WARRANTY_SERIAL_MAX; i++) {
        if (imei[i] < '0' || imei[i] > '9') {
            return false;
        }
        unsigned digit = (unsigned)(imei[i] - '0');
        if ((i & 1u) != 0u) {
            digit *= 2u;
            if (digit > 9u) {
                digit -= 9u;
            }
        }
        sum += digit;
    }
    return (sum % 10u) == 0u;
}

store_status_t store_board_imei_get(char *out_imei, size_t out_cap) {
    if (out_imei == 0 || out_cap < STORE_WARRANTY_SERIAL_MAX + 1u) {
        return STORE_STATUS_INVALID_ARGUMENT;
    }
    out_imei[0] = '\0';
    if (!store_board_imei_valid(s_warranty.serial)) {
        return STORE_STATUS_NOT_FOUND;
    }
    memcpy(out_imei, s_warranty.serial, STORE_WARRANTY_SERIAL_MAX + 1u);
    return STORE_STATUS_OK;
}

store_status_t store_board_imei_provision(const char *imei) {
    if (!store_board_imei_valid(imei)) {
        return STORE_STATUS_INVALID_ARGUMENT;
    }
    if (store_board_imei_valid(s_warranty.serial)) {
        return strcmp(s_warranty.serial, imei) == 0
                   ? STORE_STATUS_OK
                   : STORE_STATUS_CONFLICT;
    }
    memcpy(s_warranty.serial, imei, STORE_WARRANTY_SERIAL_MAX + 1u);
    return store_engine_mark_dirty(STORE_UNIT_SERVICE_WARRANTY);
}

store_status_t store_warranty_set(const store_warranty_state_t *state) {
    if (state == 0) {
        return STORE_STATUS_INVALID_ARGUMENT;
    }
    store_warranty_state_t normalized;
    memset(&normalized, 0, sizeof(normalized));
    /* Serial/IMEI is board identity, not an editable warranty field. Preserve
     * it even when a caller submits a full record with another value. */
    store_copy_text(normalized.serial, sizeof(normalized.serial), s_warranty.serial);
    store_copy_text(normalized.made, sizeof(normalized.made), state->made);
    store_copy_text(normalized.repaired, sizeof(normalized.repaired), state->repaired);
    store_copy_text(normalized.purchase_date, sizeof(normalized.purchase_date), state->purchase_date);
    normalized.flags = state->flags;
    if (normalized.made[0] == '\0') {
        store_copy_text(normalized.made, sizeof(normalized.made), STORE_WARRANTY_DEFAULT_MADE);
    }
    if (normalized.repaired[0] == '\0') {
        store_copy_text(normalized.repaired, sizeof(normalized.repaired), STORE_WARRANTY_DEFAULT_REPAIRED);
    }
    if (memcmp(&s_warranty, &normalized, sizeof(s_warranty)) == 0) {
        return STORE_STATUS_OK;
    }
    s_warranty = normalized;
    return store_engine_mark_dirty(STORE_UNIT_SERVICE_WARRANTY);
}

store_status_t store_warranty_set_purchase_date(const char *mmyy) {
    if (mmyy == 0) {
        return STORE_STATUS_INVALID_ARGUMENT;
    }
    store_warranty_state_t updated = s_warranty;
    store_copy_text(updated.purchase_date, sizeof(updated.purchase_date), mmyy);
    if (updated.purchase_date[0] != '\0') {
        updated.flags |= 0x80u;
    } else {
        updated.flags &= (uint8_t)~0x80u;
    }
    return store_warranty_set(&updated);
}

static void load_warranty_defaults(void) {
    memset(&s_warranty, 0, sizeof(s_warranty));
    store_copy_text(s_warranty.made, sizeof(s_warranty.made), STORE_WARRANTY_DEFAULT_MADE);
    store_copy_text(s_warranty.repaired, sizeof(s_warranty.repaired), STORE_WARRANTY_DEFAULT_REPAIRED);
    s_warranty.flags = 0u;
}

void store_warranty_post_load(void) {
    if (s_warranty.serial[0] != '\0' &&
        !store_board_imei_valid(s_warranty.serial)) {
        LOGW("store", "cleared invalid board IMEI");
        s_warranty.serial[0] = '\0';
        (void)store_engine_mark_dirty(STORE_UNIT_SERVICE_WARRANTY);
    }
}

static bool serialize_service_warranty(uint8_t *dst, size_t cap, size_t *out_len) {
    size_t pos = 0u;
    uint8_t serial_len = (uint8_t)strnlen(s_warranty.serial, STORE_WARRANTY_SERIAL_MAX);
    uint8_t made_len = (uint8_t)strnlen(s_warranty.made, STORE_WARRANTY_MMYY_MAX);
    uint8_t repaired_len = (uint8_t)strnlen(s_warranty.repaired, STORE_WARRANTY_MMYY_MAX);
    uint8_t purchase_len = (uint8_t)strnlen(s_warranty.purchase_date, STORE_WARRANTY_MMYY_MAX);
    uint8_t fixed_serial[STORE_WARRANTY_SERIAL_MAX + 1u];
    uint8_t fixed_made[STORE_WARRANTY_MMYY_MAX + 1u];
    uint8_t fixed_repaired[STORE_WARRANTY_MMYY_MAX + 1u];
    uint8_t fixed_purchase[STORE_WARRANTY_MMYY_MAX + 1u];
    memset(fixed_serial, 0, sizeof(fixed_serial));
    memset(fixed_made, 0, sizeof(fixed_made));
    memset(fixed_repaired, 0, sizeof(fixed_repaired));
    memset(fixed_purchase, 0, sizeof(fixed_purchase));
    memcpy(fixed_serial, s_warranty.serial, serial_len);
    memcpy(fixed_made, s_warranty.made, made_len);
    memcpy(fixed_repaired, s_warranty.repaired, repaired_len);
    memcpy(fixed_purchase, s_warranty.purchase_date, purchase_len);
    if (!write_u32_field(dst, cap, &pos, WARRANTY_MAGIC) ||
        !write_u16_field(dst, cap, &pos, STORE_PAYLOAD_VERSION) ||
        !write_u8_field(dst, cap, &pos, s_warranty.flags) ||
        !write_u8_field(dst, cap, &pos, 0u) ||
        !write_u32_field(dst, cap, &pos, 0u) ||
        !write_u8_field(dst, cap, &pos, serial_len) ||
        !write_bytes(dst, cap, &pos, fixed_serial, sizeof(fixed_serial)) ||
        !write_u8_field(dst, cap, &pos, made_len) ||
        !write_bytes(dst, cap, &pos, fixed_made, sizeof(fixed_made)) ||
        !write_u8_field(dst, cap, &pos, repaired_len) ||
        !write_bytes(dst, cap, &pos, fixed_repaired, sizeof(fixed_repaired)) ||
        !write_u8_field(dst, cap, &pos, purchase_len) ||
        !write_bytes(dst, cap, &pos, fixed_purchase, sizeof(fixed_purchase))) {
        return false;
    }
    *out_len = pos;
    return true;
}

static bool apply_service_warranty_payload(const uint8_t *payload, size_t len) {
    if (len < 12u || read_u32(&payload[0]) != WARRANTY_MAGIC ||
        read_u16(&payload[4]) != STORE_PAYLOAD_VERSION ||
        payload[7] != 0u || read_u32(&payload[8]) != 0u) {
        return false;
    }
    store_warranty_state_t loaded;
    memset(&loaded, 0, sizeof(loaded));
    loaded.flags = payload[6];
    size_t pos = 12u;
    if (pos + 1u + STORE_WARRANTY_SERIAL_MAX + 1u > len) {
        return false;
    }
    uint8_t serial_len = payload[pos++];
    if (serial_len > STORE_WARRANTY_SERIAL_MAX) {
        return false;
    }
    memcpy(loaded.serial, &payload[pos], serial_len);
    loaded.serial[serial_len] = '\0';
    pos += STORE_WARRANTY_SERIAL_MAX + 1u;

    if (pos + 1u + STORE_WARRANTY_MMYY_MAX + 1u > len) {
        return false;
    }
    uint8_t made_len = payload[pos++];
    if (made_len > STORE_WARRANTY_MMYY_MAX) {
        return false;
    }
    memcpy(loaded.made, &payload[pos], made_len);
    loaded.made[made_len] = '\0';
    pos += STORE_WARRANTY_MMYY_MAX + 1u;

    if (pos + 1u + STORE_WARRANTY_MMYY_MAX + 1u > len) {
        return false;
    }
    uint8_t repaired_len = payload[pos++];
    if (repaired_len > STORE_WARRANTY_MMYY_MAX) {
        return false;
    }
    memcpy(loaded.repaired, &payload[pos], repaired_len);
    loaded.repaired[repaired_len] = '\0';
    pos += STORE_WARRANTY_MMYY_MAX + 1u;

    if (pos + 1u + STORE_WARRANTY_MMYY_MAX + 1u > len) {
        return false;
    }
    uint8_t purchase_len = payload[pos++];
    if (purchase_len > STORE_WARRANTY_MMYY_MAX) {
        return false;
    }
    memcpy(loaded.purchase_date, &payload[pos], purchase_len);
    loaded.purchase_date[purchase_len] = '\0';

    if (loaded.made[0] == '\0') {
        store_copy_text(loaded.made, sizeof(loaded.made), STORE_WARRANTY_DEFAULT_MADE);
    }
    if (loaded.repaired[0] == '\0') {
        store_copy_text(loaded.repaired, sizeof(loaded.repaired), STORE_WARRANTY_DEFAULT_REPAIRED);
    }
    s_warranty = loaded;
    return true;
}

static void reset_warranty_unit(uint8_t instance) {
    (void)instance;
    load_warranty_defaults();
}

static void fallback_warranty_unit(uint8_t instance) {
    (void)instance;
    load_warranty_defaults();
}

static bool serialize_warranty_unit(uint8_t instance,
                                    uint8_t *dst,
                                    size_t cap,
                                    size_t *out_len) {
    (void)instance;
    return serialize_service_warranty(dst, cap, out_len);
}

static bool apply_warranty_unit(uint8_t instance, const uint8_t *payload, size_t len) {
    (void)instance;
    return apply_service_warranty_payload(payload, len);
}

const store_unit_ops_t g_store_warranty_unit_ops = {
    .reset_ram = reset_warranty_unit,
    .serialize = serialize_warranty_unit,
    .apply = apply_warranty_unit,
    .fallback_missing_or_corrupt = fallback_warranty_unit,
    .name = "service warranty",
};

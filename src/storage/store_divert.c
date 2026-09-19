#include "storage/store_service.h"

#include "storage_bytes.h"
#include "store_service_internal.h"

#include <string.h>

#define CALL_DIVERT_MAGIC 0x44495631u
#define STORE_CALL_DIVERT_DEFAULT_DELAY_SECONDS 20u

static store_call_divert_state_t s_call_divert;

static void reset_divert_unit(uint8_t instance) {
    (void)instance;
    memset(&s_call_divert, 0, sizeof(s_call_divert));
    s_call_divert.delay_seconds = STORE_CALL_DIVERT_DEFAULT_DELAY_SECONDS;
}

static bool serialize_divert_unit(uint8_t instance,
                                  uint8_t *dst,
                                  size_t cap,
                                  size_t *out_len) {
    (void)instance;
    size_t pos = 0u;
    if (!write_u32_field(dst, cap, &pos, CALL_DIVERT_MAGIC) ||
        !write_u16_field(dst, cap, &pos, STORE_PAYLOAD_VERSION) ||
        !write_u8_field(dst, cap, &pos,
                        (uint8_t)(s_call_divert.active_mask & 0x1fu)) ||
        !write_u8_field(dst, cap, &pos, s_call_divert.delay_seconds)) {
        return false;
    }
    for (uint8_t i = 0u; i < STORE_CALL_DIVERT_CONDITION_COUNT; i++) {
        uint8_t fixed[STORE_CALL_DIVERT_NUMBER_MAX + 1u];
        uint8_t number_len = (uint8_t)strnlen(
            s_call_divert.numbers[i], STORE_CALL_DIVERT_NUMBER_MAX);
        memset(fixed, 0, sizeof(fixed));
        memcpy(fixed, s_call_divert.numbers[i], number_len);
        if (!write_u8_field(dst, cap, &pos, number_len) ||
            !write_bytes(dst, cap, &pos, fixed, sizeof(fixed))) {
            return false;
        }
    }
    *out_len = pos;
    return true;
}

static bool apply_divert_unit(uint8_t instance,
                              const uint8_t *payload,
                              size_t len) {
    (void)instance;
    if (len < 8u || read_u32(&payload[0]) != CALL_DIVERT_MAGIC ||
        read_u16(&payload[4]) != STORE_PAYLOAD_VERSION) {
        return false;
    }
    store_call_divert_state_t loaded;
    memset(&loaded, 0, sizeof(loaded));
    loaded.active_mask = (uint8_t)(payload[6] & 0x1fu);
    loaded.delay_seconds = payload[7] == 0u
                               ? STORE_CALL_DIVERT_DEFAULT_DELAY_SECONDS
                               : payload[7];
    size_t pos = 8u;
    for (uint8_t i = 0u; i < STORE_CALL_DIVERT_CONDITION_COUNT; i++) {
        if (pos + 1u + STORE_CALL_DIVERT_NUMBER_MAX + 1u > len) {
            return false;
        }
        uint8_t number_len = payload[pos++];
        if (number_len > STORE_CALL_DIVERT_NUMBER_MAX) {
            return false;
        }
        memcpy(loaded.numbers[i], &payload[pos], number_len);
        loaded.numbers[i][number_len] = '\0';
        pos += STORE_CALL_DIVERT_NUMBER_MAX + 1u;
    }
    s_call_divert = loaded;
    return true;
}

const store_unit_ops_t g_store_divert_unit_ops = {
    .reset_ram = reset_divert_unit,
    .serialize = serialize_divert_unit,
    .apply = apply_divert_unit,
    .fallback_missing = 0,
    .name = "call divert",
};

store_status_t store_call_divert_get(store_call_divert_state_t *out_state) {
    if (out_state == 0) {
        return STORE_STATUS_INVALID_ARGUMENT;
    }
    *out_state = s_call_divert;
    if (out_state->delay_seconds == 0u) {
        out_state->delay_seconds = STORE_CALL_DIVERT_DEFAULT_DELAY_SECONDS;
    }
    return STORE_STATUS_OK;
}

store_status_t store_call_divert_set(const store_call_divert_state_t *state) {
    if (state == 0) {
        return STORE_STATUS_INVALID_ARGUMENT;
    }
    s_call_divert.active_mask = (uint8_t)(state->active_mask & 0x1fu);
    s_call_divert.delay_seconds =
        state->delay_seconds == 0u
            ? STORE_CALL_DIVERT_DEFAULT_DELAY_SECONDS
            : state->delay_seconds;
    for (uint8_t i = 0u; i < STORE_CALL_DIVERT_CONDITION_COUNT; i++) {
        store_copy_text(s_call_divert.numbers[i],
                        sizeof(s_call_divert.numbers[i]), state->numbers[i]);
    }
    return store_engine_mark_dirty(STORE_UNIT_CALL_DIVERT);
}

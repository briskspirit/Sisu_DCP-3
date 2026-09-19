#include "storage/store_service.h"

#include "storage_bytes.h"
#include "store_service_internal.h"

#include <string.h>

#define T9_USER_MAGIC 0x54395531u

typedef struct {
    char words[STORE_T9_USER_WORD_LIMIT][STORE_T9_WORD_MAX + 1u];
    uint8_t count;
} t9_user_state_t;

static t9_user_state_t s_t9_user;

static void reset_t9_unit(uint8_t instance) {
    (void)instance;
    memset(&s_t9_user, 0, sizeof(s_t9_user));
}

static bool serialize_t9_unit(uint8_t instance,
                              uint8_t *dst,
                              size_t cap,
                              size_t *out_len) {
    (void)instance;
    size_t pos = 0u;
    uint8_t count = s_t9_user.count > STORE_T9_USER_WORD_LIMIT
                        ? STORE_T9_USER_WORD_LIMIT
                        : s_t9_user.count;
    if (!write_u32_field(dst, cap, &pos, T9_USER_MAGIC) ||
        !write_u16_field(dst, cap, &pos, STORE_PAYLOAD_VERSION) ||
        !write_u8_field(dst, cap, &pos, count) ||
        !write_u8_field(dst, cap, &pos, 0u)) {
        return false;
    }
    for (uint8_t i = 0u; i < count; i++) {
        uint8_t fixed[STORE_T9_WORD_MAX + 1u];
        uint8_t word_len =
            (uint8_t)strnlen(s_t9_user.words[i], STORE_T9_WORD_MAX);
        memset(fixed, 0, sizeof(fixed));
        memcpy(fixed, s_t9_user.words[i], word_len);
        if (!write_u8_field(dst, cap, &pos, word_len) ||
            !write_bytes(dst, cap, &pos, fixed, sizeof(fixed))) {
            return false;
        }
    }
    *out_len = pos;
    return true;
}

static bool apply_t9_unit(uint8_t instance,
                          const uint8_t *payload,
                          size_t len) {
    (void)instance;
    if (len < 8u || read_u32(&payload[0]) != T9_USER_MAGIC ||
        read_u16(&payload[4]) != STORE_PAYLOAD_VERSION) {
        return false;
    }
    uint8_t count = payload[6];
    if (count > STORE_T9_USER_WORD_LIMIT) {
        return false;
    }
    t9_user_state_t loaded;
    memset(&loaded, 0, sizeof(loaded));
    loaded.count = count;
    size_t pos = 8u;
    for (uint8_t i = 0u; i < count; i++) {
        if (pos + 1u + STORE_T9_WORD_MAX + 1u > len) {
            return false;
        }
        uint8_t word_len = payload[pos++];
        if (word_len > STORE_T9_WORD_MAX) {
            return false;
        }
        memcpy(loaded.words[i], &payload[pos], word_len);
        loaded.words[i][word_len] = '\0';
        pos += STORE_T9_WORD_MAX + 1u;
    }
    s_t9_user = loaded;
    return true;
}

const store_unit_ops_t g_store_t9_unit_ops = {
    .reset_ram = reset_t9_unit,
    .serialize = serialize_t9_unit,
    .apply = apply_t9_unit,
    .fallback_missing = 0,
    .name = "T9 user dictionary",
};

store_status_t store_t9_user_words_load(
    char words[][STORE_T9_WORD_MAX + 1u],
    uint8_t word_cap,
    uint8_t *out_count) {
    if (out_count == 0 || (word_cap > 0u && words == 0)) {
        return STORE_STATUS_INVALID_ARGUMENT;
    }
    uint8_t count = s_t9_user.count;
    if (count > STORE_T9_USER_WORD_LIMIT) {
        count = STORE_T9_USER_WORD_LIMIT;
    }
    uint8_t copy_count = count < word_cap ? count : word_cap;
    for (uint8_t i = 0u; i < copy_count; i++) {
        store_copy_text(words[i], STORE_T9_WORD_MAX + 1u,
                        s_t9_user.words[i]);
    }
    *out_count = copy_count;
    return STORE_STATUS_OK;
}

store_status_t store_t9_user_words_save(
    char words[][STORE_T9_WORD_MAX + 1u],
    uint8_t count) {
    if (count > STORE_T9_USER_WORD_LIMIT || (count > 0u && words == 0)) {
        return STORE_STATUS_INVALID_ARGUMENT;
    }
    memset(&s_t9_user, 0, sizeof(s_t9_user));
    s_t9_user.count = count;
    for (uint8_t i = 0u; i < count; i++) {
        store_copy_text(s_t9_user.words[i], sizeof(s_t9_user.words[i]),
                        words[i]);
    }
    return store_engine_mark_dirty(STORE_UNIT_T9_USER_DICT);
}

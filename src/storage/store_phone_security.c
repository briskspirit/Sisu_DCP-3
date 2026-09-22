#include "storage/store_phone_security.h"
#include "store_service_internal.h"
#include "storage_bytes.h"

#define SECURITY_MAGIC 0x31534550u
#define SECURITY_BYTES (8u + PHONE_SECURITY_SIM_COUNT * PHONE_SECURITY_SIM_CAP)

static phone_security_record_t s_record;

bool phone_security_identity_valid(const char *id) {
    if (id == NULL) return false;
    size_t n = strnlen(id, PHONE_SECURITY_SIM_CAP);
    return (n == 19u || n == 20u) && strspn(id, "0123456789") == n;
}

static bool valid(const phone_security_record_t *record) {
    if (record == NULL || record->failures > 5u) return false;
    bool empty = false;
    for (unsigned i = 0u; i < PHONE_SECURITY_SIM_COUNT; i++) {
        if (record->sims[i][0] == '\0') { empty = true; continue; }
        if (empty || !phone_security_identity_valid(record->sims[i])) return false;
        for (unsigned j = 0u; j < i; j++)
            if (strcmp(record->sims[i], record->sims[j]) == 0) return false;
    }
    return true;
}

bool store_phone_security_read(phone_security_record_t *out) {
    if (out == NULL || !store_service_unit_ready(STORE_UNIT_PHONE_SECURITY)) return false;
    *out = s_record;
    return true;
}

bool store_phone_security_write(const phone_security_record_t *record) {
    if (!valid(record) || !store_service_unit_ready(STORE_UNIT_PHONE_SECURITY)) return false;
    if (memcmp(record, &s_record, sizeof(s_record)) != 0) {
        if (store_engine_mark_dirty(STORE_UNIT_PHONE_SECURITY) != STORE_STATUS_OK) return false;
        s_record = *record;
    }
    return true;
}

static void reset(uint8_t instance) {
    (void)instance;
    memset(&s_record, 0, sizeof(s_record));
}

static bool serialize(uint8_t instance, uint8_t *dst, size_t cap, size_t *len) {
    (void)instance;
    if (cap < SECURITY_BYTES || !valid(&s_record)) return false;
    memset(dst, 0, SECURITY_BYTES);
    write_u32(dst, SECURITY_MAGIC);
    dst[4] = 1u;
    dst[5] = s_record.enabled;
    dst[6] = s_record.clear_calls;
    dst[7] = s_record.failures;
    memcpy(dst + 8u, s_record.sims, sizeof(s_record.sims));
    *len = SECURITY_BYTES;
    return true;
}

static bool apply(uint8_t instance, const uint8_t *src, size_t len) {
    (void)instance;
    if (len != SECURITY_BYTES || read_u32(src) != SECURITY_MAGIC ||
        src[4] != 1u || src[5] > 1u || src[6] > 1u) return false;
    phone_security_record_t record = {0};
    record.enabled = src[5] != 0u;
    record.clear_calls = src[6] != 0u;
    record.failures = src[7];
    memcpy(record.sims, src + 8u, sizeof(record.sims));
    for (unsigned i = 0u; i < PHONE_SECURITY_SIM_COUNT; i++) {
        if (record.sims[i][PHONE_SECURITY_SIM_CAP - 1u] != '\0') return false;
        if (record.sims[i][0] == '\0') {
            for (unsigned j = 1u; j < PHONE_SECURITY_SIM_CAP; j++)
                if (record.sims[i][j] != '\0') return false;
        }
    }
    if (!valid(&record)) return false;
    s_record = record;
    return true;
}

const store_unit_ops_t g_store_phone_security_unit_ops = {
    .reset_ram = reset, .serialize = serialize, .apply = apply,
    .name = "phone security",
};

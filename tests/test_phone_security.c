#include "services/phone_security.h"
#include "storage/store_phone_security.h"
#include "storage/store_service_internal.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define SECURITY_MASK (1u << STORE_UNIT_PHONE_SECURITY)
#define CALLS_MASK ((1u << STORE_UNIT_CALLS_MISSED) | (1u << STORE_UNIT_CALLS_RECEIVED) | (1u << STORE_UNIT_CALLS_DIALLED))
#define QUIET_MS (0x969au * 8u)
static store_diag_snapshot_t diag;
static uint8_t durable[113];
static unsigned flushes, fail_at, clears;
static char stored_code[41];
static unsigned ram_calls[3], disk_calls[3];
static const char *ids[] = {
    "8900000000000000001", "8900000000000000002", "8900000000000000003",
    "8900000000000000004", "8900000000000000005", "8900000000000000006",
};

bool store_service_unit_ready(store_unit_t unit) {
    return diag.ready && !((diag.unavailable_mask | diag.degraded_mask) & (1u << unit));
}
store_status_t store_engine_mark_dirty(store_unit_t unit) { diag.dirty_mask |= 1u << unit; return STORE_STATUS_OK; }
void store_service_get_diag(store_diag_snapshot_t *out) { *out = diag; }
void store_service_require_service(uint8_t faults) { diag.boot_faults |= faults; }
store_status_t store_setting_get_text(store_setting_key_t key, char *out, uint8_t cap) {
    assert(key == STORE_SETTING_SECURITY_CODE); snprintf(out, cap, "%s", stored_code); return STORE_STATUS_OK;
}
store_status_t store_setting_set_text(store_setting_key_t key, const char *text) {
    assert(key == STORE_SETTING_SECURITY_CODE); strcpy(stored_code, text); return STORE_STATUS_OK;
}
store_status_t store_call_clear(store_call_list_t list) {
    assert(list < 3u); ram_calls[list] = 0u; clears++; diag.dirty_mask |= 1u << (STORE_UNIT_CALLS_MISSED + list);
    return STORE_STATUS_OK;
}
bool store_service_flush_all(void) {
    if (++flushes == fail_at) return false;
    if (diag.dirty_mask & SECURITY_MASK) {
        size_t size = 0u;
        assert(g_store_phone_security_unit_ops.serialize(0u, durable, sizeof(durable), &size));
        assert(size == sizeof(durable));
    }
    if (diag.dirty_mask & CALLS_MASK) memcpy(disk_calls, ram_calls, sizeof(disk_calls));
    diag.dirty_mask = 0u;
    return true;
}
static void boot(uint32_t now) {
    g_store_phone_security_unit_ops.reset_ram(0u);
    assert(g_store_phone_security_unit_ops.apply(0u, durable, sizeof(durable)));
    memcpy(ram_calls, disk_calls, sizeof(ram_calls));
    diag.dirty_mask = 0u;
    phone_security_init(now);
}
static void fresh(bool enabled) {
    diag = (store_diag_snapshot_t){.ready = true};
    fail_at = flushes = clears = 0u;
    strcpy(stored_code, "12345");
    g_store_phone_security_unit_ops.reset_ram(0u);
    phone_security_record_t record = {.enabled = enabled};
    assert(store_phone_security_write(&record));
    diag.dirty_mask |= SECURITY_MASK;
    assert(store_service_flush_all());
    for (unsigned i = 0u; i < 3u; i++) disk_calls[i] = ram_calls[i] = 7u;
    phone_security_init(0u);
}
static void authorize(uint32_t now) {
    assert(security_code_verify("12345", now) == SECURITY_CODE_OK);
    assert(phone_security_unlock(now));
    assert(phone_security_state() == PHONE_SECURITY_OPEN);
}
static void test_sim_memory(void) {
    fresh(true);
    assert(phone_security_state() == PHONE_SECURITY_WAIT_IDENTITY);
    phone_security_tick("123", 0u);
    assert(phone_security_state() == PHONE_SECURITY_WAIT_IDENTITY);
    for (unsigned i = 0u; i < 5u; i++) {
        phone_security_tick(ids[i], (i + 1u) * 1000u);
        assert(phone_security_state() == PHONE_SECURITY_LOCKED);
        assert(!phone_security_unlock(0u));
        authorize((i + 1u) * 1000u);
    }
    phone_security_record_t record;
    assert(store_phone_security_read(&record));
    assert(strcmp(record.sims[0], ids[4]) == 0 && strcmp(record.sims[4], ids[0]) == 0);
    phone_security_tick(ids[0], 6000u);
    assert(phone_security_state() == PHONE_SECURITY_OPEN);
    assert(store_phone_security_read(&record) && strcmp(record.sims[0], ids[4]) == 0);
    phone_security_tick(ids[5], 7000u);
    authorize(7000u);
    phone_security_tick(ids[0], 8000u);
    assert(phone_security_state() == PHONE_SECURITY_LOCKED);
    assert(clears == 15u);
    assert(security_code_verify("12345", 8001u) == SECURITY_CODE_OK);
    uint32_t generation = phone_security_generation();
    phone_security_tick(NULL, 8002u);
    assert(phone_security_generation() != generation && !phone_security_unlock(8002u));
    assert(phone_security_state() == PHONE_SECURITY_WAIT_IDENTITY);
}
static void test_retry_limit(void) {
    fresh(true);
    phone_security_tick(ids[0], 1000u);
    for (unsigned i = 0u; i < 5u; i++) assert(security_code_verify("00000", 1001u + i) == SECURITY_CODE_WRONG);
    assert(security_code_verify("12345", 1010u) == SECURITY_CODE_WRONG);
    boot(2000u);
    phone_security_tick(ids[0], 2001u);
    assert(security_code_verify("12345", 2002u) == SECURITY_CODE_WRONG);
    assert(security_code_verify("12345", 2002u + QUIET_MS - 1u) == SECURITY_CODE_WRONG);
    assert(security_code_verify("12345", 2001u + 2u * QUIET_MS) == SECURITY_CODE_OK);
    assert(phone_security_unlock(0u));
    fresh(true);
    phone_security_tick(ids[0], UINT32_MAX - 100u);
    for (unsigned i = 0u; i < 5u; i++) assert(security_code_verify("00000", UINT32_MAX - 100u) == SECURITY_CODE_WRONG);
    assert(security_code_verify("12345", QUIET_MS - 101u) == SECURITY_CODE_OK);
    assert(phone_security_unlock(0u));
}
static void test_powercuts(void) {
    for (unsigned stage = 1u; stage <= 3u; stage++) {
        fresh(true);
        phone_security_tick(ids[0], 1000u); authorize(1001u);
        phone_security_tick(ids[1], 2000u);
        assert(security_code_verify("12345", 2001u) == SECURITY_CODE_OK);
        fail_at = flushes + stage;
        assert(!phone_security_unlock(2002u));
        assert(phone_security_state() != PHONE_SECURITY_OPEN);
        fail_at = 0u;
        boot(3000u);
        phone_security_tick(ids[1], 3001u);
        if (stage == 1u) {
            assert(phone_security_state() == PHONE_SECURITY_LOCKED);
            authorize(3002u);
        }
        assert(phone_security_state() == PHONE_SECURITY_OPEN);
        for (unsigned i = 0u; i < 3u; i++) assert(disk_calls[i] == 0u);
        phone_security_record_t record;
        assert(store_phone_security_read(&record) && !record.clear_calls);
    }
    fresh(true);
    phone_security_tick(ids[0], 1000u);
    fail_at = flushes + 1u;
    assert(security_code_verify("00000", 1001u) == SECURITY_CODE_UNAVAILABLE);
    assert(phone_security_state() == PHONE_SECURITY_FAULT);
    fail_at = 0u;
    phone_security_tick(ids[0], 2001u);
    assert(phone_security_state() == PHONE_SECURITY_LOCKED);
    assert(!phone_security_unlock(2001u));
    phone_security_record_t record;
    assert(store_phone_security_read(&record) && record.failures == 1u);
}
static void test_settings_and_wire(void) {
    fresh(false);
    phone_security_tick(ids[0], 1000u);
    assert(clears == 0u);
    assert(!phone_security_set_enabled(true, 1001u));
    assert(!phone_security_restore_defaults());
    assert(!security_code_change("54321"));
    assert(security_code_verify("12345", 1001u) == SECURITY_CODE_OK);
    assert(phone_security_set_enabled(true, 1001u));
    assert(clears == 3u);
    assert(!phone_security_set_enabled(false, 1002u));
    assert(security_code_verify("12345", 1003u) == SECURITY_CODE_OK);
    assert(!security_code_change("1234"));
    assert(security_code_verify("12345", 1004u) == SECURITY_CODE_OK);
    assert(security_code_change("54321"));
    assert(security_code_verify("12345", 1005u) == SECURITY_CODE_WRONG);
    assert(security_code_verify("54321", 1006u) == SECURITY_CODE_OK);
    assert(phone_security_restore_defaults() && !phone_security_enabled());
    uint8_t encoded[113]; size_t size;
    assert(g_store_phone_security_unit_ops.serialize(0u, encoded, sizeof(encoded), &size));
    for (size_t n = 0u; n < size; n++) assert(!g_store_phone_security_unit_ops.apply(0u, encoded, n));
    encoded[5] = 2u; assert(!g_store_phone_security_unit_ops.apply(0u, encoded, size)); encoded[5] = 0u;
    encoded[7] = 6u; assert(!g_store_phone_security_unit_ops.apply(0u, encoded, size)); encoded[7] = 0u;
    memcpy(encoded + 29u, encoded + 8u, 21u);
    assert(!g_store_phone_security_unit_ops.apply(0u, encoded, size));
    diag.unavailable_mask = SECURITY_MASK;
    phone_security_init(0u);
    assert(phone_security_state() == PHONE_SECURITY_FAULT);
    assert(security_code_verify("54321", 0u) == SECURITY_CODE_UNAVAILABLE);
    phone_security_tick(ids[0], 1000u);
    assert(diag.boot_faults & STORE_BOOT_FAULT_RECORD);
}
int main(void) {
    test_sim_memory(); test_retry_limit(); test_powercuts(); test_settings_and_wire();
    puts("phone security policy, retry persistence, wire validation and interrupted admission passed");
    return 0;
}

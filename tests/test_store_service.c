/* Host unit test for the persistent store in src/storage/store_*.c.
 *
 * The littlefs backing is stubbed in RAM via a fake nvm_hal_t (see
 * storage_backend_open below), running the real adapter against a
 * plain byte array. rtc_alarm_hal_get_datetime and the log_* sink are also
 * stubbed for the host. NO test framework: a static failure counter, plain
 * assert helpers, and a main() that returns non-zero on any failure.
 *
 * Oracles are derived from store_service.h / the public contract, not from
 * reading what the implementation prints back.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <setjmp.h>

#include "services/backlight_service.h"
#include "services/lcd_calibration.h"
#include "storage/nvm_hal.h"
#include "storage/store_service.h"
#define TEST_LOGICAL_SLOT_SIZE 4096u
#include "storage/storage_lfs.h"
#include "storage/store_service_internal.h"

/* --------------------------------------------------------------------------
 * Host stubs for hardware / log dependencies.
 * ------------------------------------------------------------------------ */

void log_set_level(int level);
void log_write(int level, const char *tag, const char *fmt, ...);
void log_vwrite(int level, const char *tag, const char *fmt, void *args);

void log_set_level(int level) { (void)level; }
void log_write(int level, const char *tag, const char *fmt, ...) {
    (void)level;
    (void)tag;
    (void)fmt;
}
void log_vwrite(int level, const char *tag, const char *fmt, void *args) {
    (void)level;
    (void)tag;
    (void)fmt;
    (void)args;
}

/* timebase stub: store_service_defer_commits_until now reads time_ms() to
 * wrap-safely re-base the commit-defer deadline. The store tests drive tick()
 * with explicit timestamps and don't exercise defer, so a fixed 0 is fine. */
uint32_t time_ms(void) { return 0u; }
uint64_t time_ms64(void) { return 0u; }

/* rtc stub: returns a fixed datetime so call records are deterministic. */
void rtc_alarm_hal_get_datetime(rtc_datetime_t *datetime);
void rtc_alarm_hal_get_datetime(rtc_datetime_t *datetime) {
    if (datetime == 0) {
        return;
    }
    datetime->year = 2026u;
    datetime->month = 6u;
    datetime->day = 24u;
    datetime->hour = 7u;
    datetime->minute = 35u;
    datetime->second = 12u;
}

/* --------------------------------------------------------------------------
 * RAM-backed NVM HAL. The journal reserves STORE_UNIT_COUNT * 2 slots; with
 * erase_required = true the slot size is TEST_LOGICAL_SLOT_SIZE (4096).
 * We size the backing array generously.
 * ------------------------------------------------------------------------ */

#define FAKE_NVM_CAPACITY (256u * 1024u)
static uint8_t s_fake_nvm[FAKE_NVM_CAPACITY];
static storage_backend_t s_real_backend;
static jmp_buf s_power_cut;
static unsigned s_cut_at, s_media_ops, s_cut_mode;

/* Poison window: writes in [s_poison_lo, s_poison_hi) are stored CORRUPTED so the
 * journal verify fails (a degraded sector). s_poison_erases counts commit
 * attempts (one erase each) in the window; s_poison_writes the corrupted writes. */
static uint32_t s_poison_lo;
static uint32_t s_poison_hi;
static uint32_t s_poison_writes;
static uint32_t s_poison_erases;
static uint32_t s_fake_busy_ops;
/* Default false = the fake flash operation runs. True makes erase/write return
 * the typed transient-busy result used for a core1 park timeout. */
static bool s_fake_op_busy = false;

static nvm_status_t fake_read(nvm_hal_t *hal, uint32_t offset, void *dst, size_t len) {
    offset += (uint32_t)(uintptr_t)hal->ctx;
    if ((uint64_t)offset + len > FAKE_NVM_CAPACITY) {
        return NVM_STATUS_OUT_OF_RANGE;
    }
    memcpy(dst, &s_fake_nvm[offset], len);
    return NVM_STATUS_OK;
}

static nvm_status_t fake_erase(nvm_hal_t *hal, uint32_t offset, size_t len) {
    offset += (uint32_t)(uintptr_t)hal->ctx;
    if ((uint64_t)offset + len > FAKE_NVM_CAPACITY) {
        return NVM_STATUS_OUT_OF_RANGE;
    }
    if (s_fake_op_busy) {
        s_fake_busy_ops++;
        return NVM_STATUS_BUSY;
    }
    if (s_poison_hi > s_poison_lo && offset >= s_poison_lo && offset < s_poison_hi) {
        s_poison_erases++;
    }
    s_media_ops++;
    bool cut = s_cut_at != 0 && s_media_ops == s_cut_at;
    size_t changed = cut ? (s_cut_mode == 0 ? 0 : s_cut_mode == 1 ? len / 2 : len) : len;
    memset(&s_fake_nvm[offset], 0xff, changed);
    if (cut) {
        longjmp(s_power_cut, 1);
    }
    return NVM_STATUS_OK;
}

static nvm_status_t fake_write(nvm_hal_t *hal, uint32_t offset, const void *src, size_t len) {
    offset += (uint32_t)(uintptr_t)hal->ctx;
    if ((uint64_t)offset + len > FAKE_NVM_CAPACITY) {
        return NVM_STATUS_OUT_OF_RANGE;
    }
    if (s_fake_op_busy) {
        s_fake_busy_ops++;
        return NVM_STATUS_BUSY;
    }
    if (s_poison_hi > s_poison_lo && offset >= s_poison_lo && offset < s_poison_hi) {
        for (size_t k = 0; k < len; k++) {
            s_fake_nvm[offset + k] = (uint8_t)~((const uint8_t *)src)[k];
        }
        s_poison_writes++;
        return NVM_STATUS_OK; /* HAL "succeeds"; the journal's own verify catches it */
    }
    s_media_ops++;
    bool cut = s_cut_at != 0 && s_media_ops == s_cut_at;
    size_t changed = cut ? (s_cut_mode == 0 ? 0 : s_cut_mode == 1 ? len / 2 : len) : len;
    for (size_t i = 0; i < changed; i++) {
        assert((s_fake_nvm[offset + i] & ((const uint8_t *)src)[i]) == ((const uint8_t *)src)[i]);
        s_fake_nvm[offset + i] &= ((const uint8_t *)src)[i];
    }
    if (cut) {
        longjmp(s_power_cut, 1);
    }
    return NVM_STATUS_OK;
}

/* Toggle to make the backend appear absent (capacity 0). */
static bool s_nvm_present = true;

nvm_status_t nvm_flash_hal_init(nvm_hal_t *hal) {
    if (hal == 0) {
        return NVM_STATUS_INVALID_ARGUMENT;
    }
    memset(hal, 0, sizeof(*hal));
    if (!s_nvm_present) {
        return NVM_STATUS_NOT_PRESENT;
    }
    hal->name = "fake-ram";
    hal->ctx = 0;
    hal->capacity = FAKE_NVM_CAPACITY / 2u;
    hal->erase_block = 4096u;
    hal->write_block = 256u;
    hal->erase_required = true;
    hal->read = fake_read;
    hal->erase = fake_erase;
    hal->write = fake_write;
    return NVM_STATUS_OK;
}

/* Inject a failing semantic record to test scheduler isolation. Physical
 * torn-program and corrupt-media behavior is covered by test_storage_lfs. */
static storage_record_result_t fault_write(storage_backend_t *backend, uint16_t id,
                                            const uint8_t *src, size_t len) {
    uint32_t logical_offset = (uint32_t)(id - 0x3210u) * 8192u;
    if (s_poison_hi > s_poison_lo && logical_offset >= s_poison_lo &&
        logical_offset < s_poison_hi) {
        if (s_fake_op_busy) {
            s_fake_busy_ops++;
            return STORAGE_RECORD_BUSY;
        }
        s_poison_erases++;
        return STORAGE_RECORD_ERROR;
    }
    return s_real_backend.write(backend, id, src, len);
}

storage_record_result_t storage_backend_open(storage_backend_t *backend) {
    static nvm_hal_t hal;
    if (nvm_flash_hal_init(&hal) != NVM_STATUS_OK) {
        return STORAGE_RECORD_ERROR;
    }
    hal.ctx = (void *)(uintptr_t)(FAKE_NVM_CAPACITY / 2u);
    storage_record_result_t result = storage_lfs_init(&s_real_backend, &hal);
    if (result == STORAGE_RECORD_OK) {
        *backend = s_real_backend;
        backend->write = fault_write;
    }
    return result;
}

/* --------------------------------------------------------------------------
 * Test scaffolding.
 * ------------------------------------------------------------------------ */

static int s_failures;

static void assert_true(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        s_failures++;
    }
}

static void assert_eq_u32(uint32_t expected, uint32_t actual, const char *message) {
    if (expected != actual) {
        fprintf(stderr, "FAIL: %s (expected %lu, got %lu)\n", message,
                (unsigned long)expected, (unsigned long)actual);
        s_failures++;
    }
}

static void assert_eq_u64(uint64_t expected, uint64_t actual, const char *message) {
    if (expected != actual) {
        fprintf(stderr, "FAIL: %s (expected %016llx, got %016llx)\n", message,
                (unsigned long long)expected, (unsigned long long)actual);
        s_failures++;
    }
}

static bool write_unit_fixture(store_unit_t unit,
                               const uint8_t *payload,
                               size_t payload_len) {
    return s_real_backend.write(&s_real_backend, (uint16_t)(0x3210u + unit),
                                 payload, payload_len) == STORAGE_RECORD_OK;
}

static bool read_unit_payload(store_unit_t unit,
                              uint8_t *payload,
                              size_t payload_cap,
                              size_t *payload_len) {
    return s_real_backend.read(&s_real_backend, (uint16_t)(0x3210u + unit),
                                payload, payload_cap, payload_len) == STORAGE_RECORD_OK;
}

static uint64_t fnv1a64(const uint8_t *data, size_t len) {
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t i = 0u; i < len; i++) {
        hash ^= data[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

/* Wipe the simulated flash to an erased state and re-init the store. */
static void fresh_store(void) {
    memset(s_fake_nvm, 0xff, sizeof(s_fake_nvm));
    s_nvm_present = true;
    s_fake_op_busy = false;
    s_fake_busy_ops = 0u;
    store_status_t st = store_service_init();
    assert_true(st == STORE_STATUS_OK, "store_service_init OK on fresh flash");
    assert_true(store_service_ready(), "store ready after init");
}

/* Force all pending dirty units to flush to flash. The tick gate uses signed
 * deltas against now_ms; commit one unit per tick, so pump many ticks. */
static void flush_commits(void) {
    /* Defer window starts at 0; pass increasing now_ms well past it. */
    for (uint32_t t = 0; t < 4000u; t += 40u) {
        store_service_tick(t);
    }
}

static store_status_t dirty_exact_unit(store_unit_t unit) {
    store_call_record_t call;
    store_picture_message_t picture;
    store_own_tone_t tone;
    store_call_divert_state_t divert;
    char words[1][STORE_T9_WORD_MAX + 1u];
    memset(&call, 0, sizeof(call));
    memset(&picture, 0, sizeof(picture));
    memset(&tone, 0, sizeof(tone));
    memset(&divert, 0, sizeof(divert));
    memset(words, 0, sizeof(words));

    switch (unit) {
        case STORE_UNIT_SETTINGS_PHONEBOOK:
            return store_setting_set_u8(STORE_SETTING_PHONEBOOK_VIEW_MODE, 1u);
        case STORE_UNIT_SETTINGS_SMS:
            return store_setting_set_u8(STORE_SETTING_SMS_VALIDITY, 4u);
        case STORE_UNIT_SETTINGS_CALLS:
            return store_setting_set_u8(STORE_SETTING_CALL_SUMMARY_AFTER_CALL, 1u);
        case STORE_UNIT_SETTINGS_PROFILES:
            return store_setting_set_u8(STORE_SETTING_PROFILE_ACTIVE, 1u);
        case STORE_UNIT_SETTINGS_CLOCK:
            return store_setting_set_u8(STORE_SETTING_CLOCK_FORMAT_24H, 0u);
        case STORE_UNIT_SETTINGS_SYSTEM:
            return store_setting_set_u8(STORE_SETTING_SYSTEM_LANGUAGE, 1u);
        case STORE_UNIT_CALLS_MISSED:
        case STORE_UNIT_CALLS_RECEIVED:
        case STORE_UNIT_CALLS_DIALLED:
            strcpy(call.number, "+15550000");
            call.reason = (store_call_reason_t)(STORE_CALL_REASON_MISSED +
                          (unit - STORE_UNIT_CALLS_MISSED));
            return store_call_add((store_call_list_t)(unit - STORE_UNIT_CALLS_MISSED),
                                  &call, 0);
        case STORE_UNIT_T9_USER_DICT:
            strcpy(words[0], "phasefive");
            return store_t9_user_words_save(words, 1u);
        case STORE_UNIT_PICTURE_MESSAGES:
            return store_picture_message_clear(0u);
        case STORE_UNIT_OWN_TONES:
            strcpy(tone.name, "Phase5");
            return store_own_tone_set(0u, &tone);
        case STORE_UNIT_CALL_DIVERT:
            divert.active_mask = 1u;
            divert.delay_seconds = 20u;
            strcpy(divert.numbers[0], "+15550100");
            return store_call_divert_set(&divert);
        case STORE_UNIT_SERVICE_WARRANTY:
            return store_warranty_set_purchase_date("0826");
        case STORE_UNIT_BATTERY_LEARNING: {
            battery_learning_persisted_t learning;
            if (store_battery_learning_get(&learning) != STORE_STATUS_OK) {
                return STORE_STATUS_STORAGE_ERROR;
            }
            learning.pack_generation++;
            return store_battery_learning_set(&learning);
        }
        case STORE_UNIT_BATTERY_CHARGE_SUPERVISOR: {
            battery_charge_supervisor_persisted_t supervisor;
            if (store_battery_charge_supervisor_get(&supervisor) !=
                STORE_STATUS_OK) {
                return STORE_STATUS_STORAGE_ERROR;
            }
            supervisor.charge_generation++;
            return store_battery_charge_supervisor_set(&supervisor);
        }
        default:
            return STORE_STATUS_INVALID_ARGUMENT;
    }
}

static void test_exact_dirty_unit_mapping(void) {
    for (store_unit_t unit = 0; unit < STORE_UNIT_COUNT; unit++) {
        fresh_store();
        store_diag_snapshot_t diag;
        store_service_get_diag(&diag);
        assert_eq_u32(0u, diag.dirty_mask, "fresh store starts with no dirty units");
        assert_true(dirty_exact_unit(unit) == STORE_STATUS_OK,
                    "unit fixture mutation succeeds");
        store_service_get_diag(&diag);
        assert_eq_u32((uint32_t)(1u << unit), diag.dirty_mask,
                      "public mutation dirties exactly its owning unit");
    }
}

static void test_unit_handler_registry(void) {
    static const store_unit_ops_t *const expected_ops[STORE_UNIT_COUNT] = {
        &g_store_settings_unit_ops,
        &g_store_settings_unit_ops,
        &g_store_settings_unit_ops,
        &g_store_settings_unit_ops,
        &g_store_settings_unit_ops,
        &g_store_settings_unit_ops,
        &g_store_calls_unit_ops,
        &g_store_calls_unit_ops,
        &g_store_calls_unit_ops,
        &g_store_t9_unit_ops,
        &g_store_pictures_unit_ops,
        &g_store_tones_unit_ops,
        &g_store_divert_unit_ops,
        &g_store_warranty_unit_ops,
        &g_store_battery_learning_unit_ops,
        &g_store_battery_charge_supervisor_unit_ops,
    };
    static const uint8_t expected_instances[STORE_UNIT_COUNT] = {
        0u, 1u, 2u, 3u, 4u, 5u,
        0u, 1u, 2u,
        0u, 0u, 0u, 0u, 0u, 0u, 0u,
    };

    for (store_unit_t unit = 0; unit < STORE_UNIT_COUNT; unit++) {
        const store_unit_binding_t *binding = store_engine_unit_binding(unit);
        assert_true(binding != 0, "every persistent unit has a binding");
        if (binding == 0) {
            continue;
        }
        assert_true(binding->ops == expected_ops[unit],
                    "unit maps to its owning domain handler");
        assert_eq_u32(expected_instances[unit], binding->instance,
                      "unit maps to the correct domain instance");
        assert_true(binding->ops->reset_ram != 0 &&
                        binding->ops->serialize != 0 &&
                        binding->ops->apply != 0 &&
                        binding->ops->name != 0,
                    "unit binding has a complete mandatory contract");
        bool needs_fallback = unit == STORE_UNIT_PICTURE_MESSAGES ||
                              unit == STORE_UNIT_SERVICE_WARRANTY;
        assert_true((binding->ops->fallback_missing_or_corrupt != 0) ==
                        needs_fallback,
                    "only fallback-owning domains expose a fallback handler");
    }
    assert_true(store_engine_unit_binding(STORE_UNIT_COUNT) == 0,
                "out-of-range persistent unit has no binding");
}

static void test_standby_readiness_tracks_persistence(void) {
    fresh_store();
    assert_true(store_service_standby_ready(),
                "clean initialized store permits standby");
    assert_true(store_setting_set_u8(STORE_SETTING_SMS_VALIDITY, 1u) ==
                    STORE_STATUS_OK,
                "standby fixture dirties a healthy settings unit");
    assert_true(!store_service_standby_ready(),
                "healthy dirty unit blocks standby");
    flush_commits();
    assert_true(store_service_standby_ready(),
                "successful commit releases standby gate");
}

/* --------------------------------------------------------------------------
 * SETTINGS_META completeness: every key 0..COUNT-1 must round-trip through a
 * get of the correct type, which only works if a meta entry exists. A missing
 * entry => setting_meta() returns NULL => get returns INVALID_ARGUMENT.
 * We probe all four typed getters; exactly one must accept each key.
 * ------------------------------------------------------------------------ */

static void test_every_key_has_meta(void) {
    fresh_store();
    for (uint32_t k = 0; k < (uint32_t)STORE_SETTING_COUNT; k++) {
        store_setting_key_t key = (store_setting_key_t)k;
        uint8_t u8;
        uint16_t u16;
        uint32_t u32;
        char text[STORE_TEXT_MAX + 1u];
        int accepted = 0;
        store_status_t s;
        s = store_setting_get_u8(key, &u8);
        if (s == STORE_STATUS_OK) accepted++;
        else assert_true(s == STORE_STATUS_TYPE_MISMATCH, "u8 get: meta exists (not INVALID_ARGUMENT)");
        s = store_setting_get_u16(key, &u16);
        if (s == STORE_STATUS_OK) accepted++;
        else assert_true(s == STORE_STATUS_TYPE_MISMATCH, "u16 get: meta exists");
        s = store_setting_get_u32(key, &u32);
        if (s == STORE_STATUS_OK) accepted++;
        else assert_true(s == STORE_STATUS_TYPE_MISMATCH, "u32 get: meta exists");
        s = store_setting_get_text(key, text, sizeof(text));
        if (s == STORE_STATUS_OK) accepted++;
        else assert_true(s == STORE_STATUS_TYPE_MISMATCH, "text get: meta exists");
        /* Exactly one typed getter must accept the key (proves a single meta
         * entry of a definite type exists for it). */
        char msg[64];
        snprintf(msg, sizeof(msg), "key %lu accepted by exactly one getter (got %d)",
                 (unsigned long)k, accepted);
        assert_true(accepted == 1, msg);
    }

    /* Out-of-range key must be rejected by every accessor. */
    uint8_t u8;
    assert_true(store_setting_get_u8(STORE_SETTING_COUNT, &u8) == STORE_STATUS_INVALID_ARGUMENT,
                "out-of-range key rejected (u8 get)");
    assert_true(store_setting_set_u8(STORE_SETTING_COUNT, 1u) == STORE_STATUS_INVALID_ARGUMENT,
                "out-of-range key rejected (u8 set)");
    assert_true(store_setting_get_u8((store_setting_key_t)0xffffu, &u8) == STORE_STATUS_INVALID_ARGUMENT,
                "huge key rejected");
}

/* --------------------------------------------------------------------------
 * Default values for unset keys (oracle from SETTINGS_META in the source).
 * ------------------------------------------------------------------------ */

static void test_defaults(void) {
    fresh_store();
    uint8_t u8;
    uint16_t u16;
    uint32_t u32;
    char text[STORE_TEXT_MAX + 1u];

    assert_true(store_setting_get_u8(STORE_SETTING_PHONEBOOK_VIEW_MODE, &u8) == STORE_STATUS_OK && u8 == 0u,
                "default view mode 0");
    assert_true(store_setting_get_u8(STORE_SETTING_SMS_VALIDITY, &u8) == STORE_STATUS_OK && u8 == 5u,
                "default SMS validity 5");
    assert_true(store_setting_get_u8(STORE_SETTING_CLOCK_SHOW_STANDBY, &u8) == STORE_STATUS_OK && u8 == 1u,
                "default clock standby 1");
    assert_true(store_setting_get_u8(STORE_SETTING_CLOCK_HOUR, &u8) == STORE_STATUS_OK && u8 == 18u,
                "default clock hour 18");
    assert_true(store_setting_get_u8(STORE_SETTING_CLOCK_MINUTE, &u8) == STORE_STATUS_OK && u8 == 21u,
                "default clock minute 21");
    assert_true(store_setting_get_u16(STORE_SETTING_CLOCK_DATE_YEAR, &u16) == STORE_STATUS_OK && u16 == 2026u,
                "default year 2026");
    assert_true(store_setting_get_u8(STORE_SETTING_SYSTEM_LANGUAGE, &u8) == STORE_STATUS_OK && u8 == 0u,
                "default language 0 (Automatic -> English)");
    assert_true(store_setting_get_u8(STORE_SETTING_PROFILE_RINGING_TONE, &u8) == STORE_STATUS_OK && u8 == 52u,
                "default ringing tone 52");
    assert_true(store_setting_get_u8(STORE_SETTING_SETTINGS_PHONE_SECURITY, &u8) == STORE_STATUS_OK && u8 == 17u,
                "default phone security 17");
    assert_true(store_setting_get_u8(STORE_SETTING_SERVICE_CODEC_BYTE, &u8) == STORE_STATUS_OK && u8 == 0x30u,
                "default codec byte 0x30");
    assert_true(store_setting_get_u32(STORE_SETTING_CALL_DURATION_ALL, &u32) == STORE_STATUS_OK && u32 == 0u,
                "default duration all 0");
    assert_true(store_setting_get_u32(STORE_SETTING_CALL_DURATION_LIFETIME, &u32) == STORE_STATUS_OK && u32 == 0u,
                "default lifetime duration 0");
    /* Recently-added headset + SAVED keys. */
    assert_true(store_setting_get_u8(STORE_SETTING_PROFILE_HEADSET_RINGING_VOLUME, &u8) == STORE_STATUS_OK && u8 == 8u,
                "default headset volume 8");
    assert_true(store_setting_get_u8(STORE_SETTING_PROFILE_SAVED, &u8) == STORE_STATUS_OK && u8 == 0xffu,
                "default profile SAVED 0xff");
    assert_true(store_setting_get_u8(STORE_SETTING_SYSTEM_LCD_VOP, &u8) == STORE_STATUS_OK &&
                    u8 == LCD_CALIBRATION_STOCK_VOP,
                "default LCD Vop matches Nokia EEPROM");
    assert_true(store_setting_get_u16(
                    STORE_SETTING_SYSTEM_MODEM_PROVISION_VERSION, &u16) ==
                    STORE_STATUS_OK && u16 == 0u,
                "default modem provisioning version is unverified");
    assert_true(store_setting_get_u8(
                    STORE_SETTING_SYSTEM_NET_MONITOR_SCHEMA, &u8) ==
                    STORE_STATUS_OK && u8 == 0u,
                "default Net Monitor schema requires migration");
    assert_true(store_setting_get_u32(
                    STORE_SETTING_SYSTEM_LCD_TUNING, &u32) ==
                    STORE_STATUS_OK &&
                    u32 == LCD_CALIBRATION_STOCK_PACKED,
                "default LCD tuple matches Nokia controller tuning");
    assert_true(store_setting_get_u8(
                    STORE_SETTING_SYSTEM_BACKLIGHT_LEVEL, &u8) ==
                    STORE_STATUS_OK &&
                    u8 == BACKLIGHT_LEVEL_DEFAULT_PERCENT,
                "default backlight preserves the original 100% duty");
    /* Text defaults. */
    assert_true(store_setting_get_text(STORE_SETTING_SECURITY_CODE, text, sizeof(text)) == STORE_STATUS_OK &&
                    strcmp(text, "12345") == 0,
                "default security code 12345");
    assert_true(store_setting_get_text(STORE_SETTING_CALCULATOR_EXCHANGE_RATE, text, sizeof(text)) == STORE_STATUS_OK &&
                    strcmp(text, "1") == 0,
                "default exchange rate 1");
    assert_true(store_setting_get_text(STORE_SETTING_SMS_MESSAGE_CENTRE, text, sizeof(text)) == STORE_STATUS_OK &&
                    text[0] == '\0',
                "default message centre empty");
    /* Speed dials default to EMPTY. */
    uint16_t sd;
    assert_true(store_setting_get_u16(STORE_SETTING_SPEED_DIAL_1, &sd) == STORE_STATUS_OK && sd == STORE_SPEED_DIAL_EMPTY,
                "default speed dial empty");
}

/* --------------------------------------------------------------------------
 * Type-mismatch handling.
 * ------------------------------------------------------------------------ */

static void test_type_mismatch(void) {
    fresh_store();
    uint8_t u8;
    uint16_t u16;
    uint32_t u32;
    char text[8];
    /* PHONEBOOK_VIEW_MODE is U8: other typed accessors must report mismatch. */
    assert_true(store_setting_get_u16(STORE_SETTING_PHONEBOOK_VIEW_MODE, &u16) == STORE_STATUS_TYPE_MISMATCH,
                "u16 get on u8 key -> mismatch");
    assert_true(store_setting_get_u32(STORE_SETTING_PHONEBOOK_VIEW_MODE, &u32) == STORE_STATUS_TYPE_MISMATCH,
                "u32 get on u8 key -> mismatch");
    assert_true(store_setting_get_text(STORE_SETTING_PHONEBOOK_VIEW_MODE, text, sizeof(text)) ==
                    STORE_STATUS_TYPE_MISMATCH,
                "text get on u8 key -> mismatch");
    assert_true(store_setting_set_u16(STORE_SETTING_PHONEBOOK_VIEW_MODE, 1u) == STORE_STATUS_TYPE_MISMATCH,
                "u16 set on u8 key -> mismatch");
    /* DATE_YEAR is U16: u8 get must mismatch. */
    assert_true(store_setting_get_u8(STORE_SETTING_CLOCK_DATE_YEAR, &u8) == STORE_STATUS_TYPE_MISMATCH,
                "u8 get on u16 key -> mismatch");
    /* SECURITY_CODE is TEXT: numeric get must mismatch. */
    assert_true(store_setting_get_u8(STORE_SETTING_SECURITY_CODE, &u8) == STORE_STATUS_TYPE_MISMATCH,
                "u8 get on text key -> mismatch");

    /* NULL out-pointers rejected. */
    assert_true(store_setting_get_u8(STORE_SETTING_PHONEBOOK_VIEW_MODE, 0) == STORE_STATUS_INVALID_ARGUMENT,
                "null out u8");
    assert_true(store_setting_set_text(STORE_SETTING_SECURITY_CODE, 0) == STORE_STATUS_INVALID_ARGUMENT,
                "null text in");
    assert_true(store_setting_get_text(STORE_SETTING_SECURITY_CODE, text, 0) == STORE_STATUS_INVALID_ARGUMENT,
                "zero out_cap text");
}

/* --------------------------------------------------------------------------
 * set -> get round trips (in-RAM, no persistence needed).
 * ------------------------------------------------------------------------ */

static void test_setget_round_trip(void) {
    fresh_store();
    uint8_t u8;
    uint16_t u16;
    uint32_t u32;

    assert_true(store_setting_set_u8(STORE_SETTING_PHONEBOOK_VIEW_MODE, 200u) == STORE_STATUS_OK, "set u8");
    assert_true(store_setting_get_u8(STORE_SETTING_PHONEBOOK_VIEW_MODE, &u8) == STORE_STATUS_OK && u8 == 200u,
                "u8 round trip");

    assert_true(store_setting_set_u16(STORE_SETTING_GAMES_SNAKE_TOP_SCORE, 65535u) == STORE_STATUS_OK, "set u16 max");
    assert_true(store_setting_get_u16(STORE_SETTING_GAMES_SNAKE_TOP_SCORE, &u16) == STORE_STATUS_OK && u16 == 65535u,
                "u16 max round trip");

    assert_true(store_setting_set_u32(STORE_SETTING_CALL_DURATION_ALL, 0xdeadbeefu) == STORE_STATUS_OK, "set u32");
    assert_true(store_setting_get_u32(STORE_SETTING_CALL_DURATION_ALL, &u32) == STORE_STATUS_OK && u32 == 0xdeadbeefu,
                "u32 round trip");

    /* Setting identical value is a no-op success. */
    assert_true(store_setting_set_u8(STORE_SETTING_PHONEBOOK_VIEW_MODE, 200u) == STORE_STATUS_OK, "set u8 idempotent");
}

/* --------------------------------------------------------------------------
 * Text clamping / truncation / NUL termination.
 * STORE_TEXT_MAX = 40. copy_text keeps cap-1 chars + NUL.
 * ------------------------------------------------------------------------ */

static void test_text_clamp(void) {
    fresh_store();
    char out[STORE_TEXT_MAX + 1u];

    /* Exactly STORE_TEXT_MAX chars: must survive intact. */
    char exact[STORE_TEXT_MAX + 1u];
    memset(exact, 'A', STORE_TEXT_MAX);
    exact[STORE_TEXT_MAX] = '\0';
    assert_true(store_setting_set_text(STORE_SETTING_SYSTEM_WELCOME_NOTE, exact) == STORE_STATUS_OK,
                "set exact-length text");
    assert_true(store_setting_get_text(STORE_SETTING_SYSTEM_WELCOME_NOTE, out, sizeof(out)) == STORE_STATUS_OK,
                "get exact-length text");
    assert_eq_u32(STORE_TEXT_MAX, (uint32_t)strlen(out), "exact text length preserved");
    assert_true(strcmp(out, exact) == 0, "exact text content preserved");

    /* Over-long input (60 chars): truncated to STORE_TEXT_MAX, NUL-terminated. */
    char longstr[80];
    memset(longstr, 'B', 60);
    longstr[60] = '\0';
    assert_true(store_setting_set_text(STORE_SETTING_SYSTEM_WELCOME_NOTE, longstr) == STORE_STATUS_OK,
                "set over-long text");
    assert_true(store_setting_get_text(STORE_SETTING_SYSTEM_WELCOME_NOTE, out, sizeof(out)) == STORE_STATUS_OK,
                "get truncated text");
    assert_eq_u32(STORE_TEXT_MAX, (uint32_t)strlen(out), "over-long text truncated to STORE_TEXT_MAX");
    for (uint32_t i = 0; i < STORE_TEXT_MAX; i++) {
        assert_true(out[i] == 'B', "truncated text content");
    }

    /* Small out_cap on get must truncate + NUL-terminate (no overrun). */
    char small[4];
    memset(small, 0x7f, sizeof(small));
    assert_true(store_setting_get_text(STORE_SETTING_SYSTEM_WELCOME_NOTE, small, sizeof(small)) == STORE_STATUS_OK,
                "get into small buffer");
    assert_eq_u32(3u, (uint32_t)strlen(small), "small get truncated to cap-1");
    assert_true(small[3] == '\0', "small get NUL terminated");

    /* out_cap == 1 -> empty string, terminated. */
    char one[1];
    one[0] = 0x55;
    assert_true(store_setting_get_text(STORE_SETTING_SYSTEM_WELCOME_NOTE, one, 1u) == STORE_STATUS_OK,
                "get cap-1");
    assert_true(one[0] == '\0', "cap-1 get yields empty string");

    /* Empty string round trip. */
    assert_true(store_setting_set_text(STORE_SETTING_SYSTEM_WELCOME_NOTE, "") == STORE_STATUS_OK, "set empty");
    assert_true(store_setting_get_text(STORE_SETTING_SYSTEM_WELCOME_NOTE, out, sizeof(out)) == STORE_STATUS_OK &&
                    out[0] == '\0',
                "empty round trip");
}

/* --------------------------------------------------------------------------
 * Persistence round trip: set, flush, re-init, values survive.
 * ------------------------------------------------------------------------ */

static void test_persistence(void) {
    fresh_store();
    assert_true(store_setting_set_u8(STORE_SETTING_PROFILE_ACTIVE, 3u) == STORE_STATUS_OK, "set profile active");
    assert_true(store_setting_set_u16(STORE_SETTING_CLOCK_DATE_YEAR, 2099u) == STORE_STATUS_OK, "set year");
    assert_true(store_setting_set_u32(STORE_SETTING_CALL_DURATION_LAST, 123456u) == STORE_STATUS_OK, "set dur last");
    assert_true(store_setting_set_text(STORE_SETTING_SYSTEM_VOICE_MAILBOX_NUMBER, "+15551234") == STORE_STATUS_OK,
                "set mailbox");
    assert_true(store_setting_set_u8(STORE_SETTING_PROFILE_SAVED, 2u) == STORE_STATUS_OK, "set saved");
    assert_true(store_setting_set_u8(STORE_SETTING_SYSTEM_LCD_VOP, 54u) == STORE_STATUS_OK,
                "set LCD Vop");
    assert_true(store_setting_set_u16(
                    STORE_SETTING_SYSTEM_MODEM_PROVISION_VERSION, 7u) ==
                    STORE_STATUS_OK,
                "set modem provisioning version");
    assert_true(store_setting_set_u8(
                    STORE_SETTING_SYSTEM_NET_MONITOR_SCHEMA, 3u) ==
                    STORE_STATUS_OK,
                "set Net Monitor schema");
    assert_true(store_setting_set_u32(
                    STORE_SETTING_SYSTEM_LCD_TUNING,
                    LCD_CALIBRATION_PACK(54u, 2u, 5u)) == STORE_STATUS_OK,
                "set LCD tuple");
    assert_true(store_setting_set_u8(
                    STORE_SETTING_SYSTEM_BACKLIGHT_LEVEL, 73u) ==
                    STORE_STATUS_OK,
                "set backlight level");
    flush_commits();

    /* Re-init from the same simulated flash (do NOT wipe). */
    store_status_t st = store_service_init();
    assert_true(st == STORE_STATUS_OK, "re-init OK");

    uint8_t u8;
    uint16_t u16;
    uint32_t u32;
    char text[STORE_TEXT_MAX + 1u];
    assert_true(store_setting_get_u8(STORE_SETTING_PROFILE_ACTIVE, &u8) == STORE_STATUS_OK && u8 == 3u,
                "profile active persisted");
    assert_true(store_setting_get_u16(STORE_SETTING_CLOCK_DATE_YEAR, &u16) == STORE_STATUS_OK && u16 == 2099u,
                "year persisted");
    assert_true(store_setting_get_u32(STORE_SETTING_CALL_DURATION_LAST, &u32) == STORE_STATUS_OK && u32 == 123456u,
                "duration persisted");
    assert_true(store_setting_get_text(STORE_SETTING_SYSTEM_VOICE_MAILBOX_NUMBER, text, sizeof(text)) ==
                    STORE_STATUS_OK && strcmp(text, "+15551234") == 0,
                "mailbox persisted");
    assert_true(store_setting_get_u8(STORE_SETTING_PROFILE_SAVED, &u8) == STORE_STATUS_OK && u8 == 2u,
                "saved persisted");
    assert_true(store_setting_get_u8(STORE_SETTING_SYSTEM_LCD_VOP, &u8) == STORE_STATUS_OK &&
                    u8 == 54u,
                "LCD Vop persisted");
    assert_true(store_setting_get_u16(
                    STORE_SETTING_SYSTEM_MODEM_PROVISION_VERSION, &u16) ==
                    STORE_STATUS_OK && u16 == 7u,
                "modem provisioning version persisted");
    assert_true(store_setting_get_u8(
                    STORE_SETTING_SYSTEM_NET_MONITOR_SCHEMA, &u8) ==
                    STORE_STATUS_OK && u8 == 3u,
                "Net Monitor schema persisted");
    assert_true(store_setting_get_u32(
                    STORE_SETTING_SYSTEM_LCD_TUNING, &u32) ==
                    STORE_STATUS_OK &&
                    u32 == LCD_CALIBRATION_PACK(54u, 2u, 5u),
                "LCD tuple persisted");
    assert_true(store_setting_get_u8(
                    STORE_SETTING_SYSTEM_BACKLIGHT_LEVEL, &u8) ==
                    STORE_STATUS_OK && u8 == 73u,
                "backlight level persisted");
}

/* --------------------------------------------------------------------------
 * Call-list ring: add/evict/order/bounds.
 * ------------------------------------------------------------------------ */

static void test_call_list_ring(void) {
    fresh_store();
    store_call_record_t rec;

    /* Empty list. */
    assert_eq_u32(0u, store_call_count(STORE_CALL_LIST_MISSED), "empty list count 0");
    assert_true(store_call_get(STORE_CALL_LIST_MISSED, 0u, &rec) == STORE_STATUS_NOT_FOUND,
                "get on empty -> NOT_FOUND");

    /* Add LIMIT+5 records; ids increase, count caps at LIMIT, newest at index 0. */
    uint32_t first_id = 0, last_id = 0;
    for (uint32_t i = 0; i < STORE_CALL_LIST_LIMIT + 5u; i++) {
        char num[16];
        snprintf(num, sizeof(num), "%lu", (unsigned long)i);
        uint32_t id = 0;
        store_status_t s = store_call_add_now(STORE_CALL_LIST_MISSED, num, "Caller",
                                              i * 10u, STORE_CALL_REASON_MISSED, &id);
        assert_true(s == STORE_STATUS_OK, "add_now OK");
        if (i == 0) first_id = id;
        last_id = id;
    }
    assert_eq_u32(STORE_CALL_LIST_LIMIT, store_call_count(STORE_CALL_LIST_MISSED), "count caps at LIMIT");
    assert_true(last_id == first_id + (STORE_CALL_LIST_LIMIT + 5u - 1u), "ids strictly increase");

    /* Index 0 must be the most-recently-added ("24"). */
    assert_true(store_call_get(STORE_CALL_LIST_MISSED, 0u, &rec) == STORE_STATUS_OK, "get newest");
    assert_true(strcmp(rec.number, "24") == 0, "newest is last added (24)");
    assert_true(rec.id == last_id, "newest has last id");

    /* Oldest surviving at index LIMIT-1 must be "5" (0..4 evicted). */
    assert_true(store_call_get(STORE_CALL_LIST_MISSED, (uint8_t)(STORE_CALL_LIST_LIMIT - 1u), &rec) == STORE_STATUS_OK,
                "get oldest surviving");
    assert_true(strcmp(rec.number, "5") == 0, "oldest surviving is 5");

    /* Out-of-range index. */
    assert_true(store_call_get(STORE_CALL_LIST_MISSED, (uint8_t)STORE_CALL_LIST_LIMIT, &rec) == STORE_STATUS_NOT_FOUND,
                "index == count -> NOT_FOUND");

    /* Invalid list. */
    assert_true(store_call_count(STORE_CALL_LIST_COUNT) == 0u, "invalid list count 0");
    assert_true(store_call_add(STORE_CALL_LIST_COUNT, &rec, 0) == STORE_STATUS_INVALID_ARGUMENT,
                "add to invalid list rejected");
    assert_true(store_call_add(STORE_CALL_LIST_MISSED, 0, 0) == STORE_STATUS_INVALID_ARGUMENT,
                "add null record rejected");
}

static void test_call_list_delete_clear(void) {
    fresh_store();
    for (uint32_t i = 0; i < 5u; i++) {
        char num[8];
        snprintf(num, sizeof(num), "%lu", (unsigned long)i);
        store_call_add_now(STORE_CALL_LIST_DIALLED, num, "X", 0u, STORE_CALL_REASON_DIALLED, 0);
    }
    /* Order: index0="4",1="3",2="2",3="1",4="0". Delete middle (index 2 = "2"). */
    assert_true(store_call_delete(STORE_CALL_LIST_DIALLED, 2u) == STORE_STATUS_OK, "delete middle");
    assert_eq_u32(4u, store_call_count(STORE_CALL_LIST_DIALLED), "count after delete");
    store_call_record_t rec;
    store_call_get(STORE_CALL_LIST_DIALLED, 2u, &rec);
    assert_true(strcmp(rec.number, "1") == 0, "post-delete shift correct");

    assert_true(store_call_delete(STORE_CALL_LIST_DIALLED, 99u) == STORE_STATUS_NOT_FOUND, "delete oob -> NOT_FOUND");

    /* update_result by id. */
    store_call_get(STORE_CALL_LIST_DIALLED, 0u, &rec);
    uint32_t id = rec.id;
    assert_true(store_call_update_result(STORE_CALL_LIST_DIALLED, id, 999u, STORE_CALL_REASON_BUSY) == STORE_STATUS_OK,
                "update_result by id");
    store_call_get(STORE_CALL_LIST_DIALLED, 0u, &rec);
    assert_eq_u32(999u, rec.duration_seconds, "update_result applied");
    assert_true(rec.reason == STORE_CALL_REASON_BUSY, "update_result reason");
    assert_true(store_call_update_result(STORE_CALL_LIST_DIALLED, 0u, 1u, STORE_CALL_REASON_BUSY) ==
                    STORE_STATUS_INVALID_ARGUMENT, "update_result id 0 rejected");
    assert_true(store_call_update_result(STORE_CALL_LIST_DIALLED, 0xdeadu, 1u, STORE_CALL_REASON_BUSY) ==
                    STORE_STATUS_NOT_FOUND, "update_result unknown id -> NOT_FOUND");

    /* clear. */
    assert_true(store_call_clear(STORE_CALL_LIST_DIALLED) == STORE_STATUS_OK, "clear");
    assert_eq_u32(0u, store_call_count(STORE_CALL_LIST_DIALLED), "count 0 after clear");
}

/* add_now with NULL number/name must store empty strings, not crash. */
static void test_call_add_now_null(void) {
    fresh_store();
    uint32_t id = 0;
    assert_true(store_call_add_now(STORE_CALL_LIST_MISSED, 0, 0, 5u, STORE_CALL_REASON_MISSED, &id) ==
                    STORE_STATUS_OK,
                "add_now with NULL strings OK");
    store_call_record_t got;
    assert_true(store_call_get(STORE_CALL_LIST_MISSED, 0u, &got) == STORE_STATUS_OK, "get null-string record");
    assert_true(got.number[0] == '\0', "null number -> empty");
    assert_true(got.name[0] == '\0', "null name -> empty");
    assert_eq_u32(5u, got.duration_seconds, "duration stored");
}

/* Truncation of over-long number/name in a record passed to store_call_add. */
static void test_call_record_truncation(void) {
    fresh_store();
    store_call_record_t rec;
    memset(&rec, 0, sizeof(rec));
    /* Fill number/name buffers fully (they include +1 for NUL already). */
    memset(rec.number, '9', STORE_CALL_NUMBER_MAX);
    rec.number[STORE_CALL_NUMBER_MAX] = '9'; /* clobber would-be NUL; add() must fix */
    memset(rec.name, 'N', STORE_CALL_NAME_MAX);
    rec.name[STORE_CALL_NAME_MAX] = 'N';
    rec.duration_seconds = 1u;
    rec.reason = STORE_CALL_REASON_RECEIVED;
    assert_true(store_call_add(STORE_CALL_LIST_RECEIVED, &rec, 0) == STORE_STATUS_OK, "add full record");
    store_call_record_t got;
    assert_true(store_call_get(STORE_CALL_LIST_RECEIVED, 0u, &got) == STORE_STATUS_OK, "get full record");
    assert_true(got.number[STORE_CALL_NUMBER_MAX] == '\0', "number NUL-terminated by add");
    assert_true(got.name[STORE_CALL_NAME_MAX] == '\0', "name NUL-terminated by add");
    assert_eq_u32(STORE_CALL_NUMBER_MAX, (uint32_t)strlen(got.number), "number length capped");
    assert_eq_u32(STORE_CALL_NAME_MAX, (uint32_t)strlen(got.name), "name length capped");

    /* update_number with over-long input truncates + terminates. */
    char longnum[80];
    memset(longnum, '1', 70);
    longnum[70] = '\0';
    assert_true(store_call_update_number(STORE_CALL_LIST_RECEIVED, 0u, longnum) == STORE_STATUS_OK,
                "update_number long");
    store_call_get(STORE_CALL_LIST_RECEIVED, 0u, &got);
    assert_eq_u32(STORE_CALL_NUMBER_MAX, (uint32_t)strlen(got.number), "update_number truncated");
    assert_true(got.number[STORE_CALL_NUMBER_MAX] == '\0', "update_number terminated");
}

/* Call-list persistence round-trip (datetime is packed/unpacked; verify). */
static void test_call_list_persistence(void) {
    fresh_store();
    uint32_t id = 0;
    store_call_add_now(STORE_CALL_LIST_RECEIVED, "+49123", "Bob", 77u, STORE_CALL_REASON_RECEIVED, &id);
    flush_commits();
    assert_true(store_service_init() == STORE_STATUS_OK, "re-init for call persistence");
    assert_eq_u32(1u, store_call_count(STORE_CALL_LIST_RECEIVED), "1 call persisted");
    store_call_record_t got;
    assert_true(store_call_get(STORE_CALL_LIST_RECEIVED, 0u, &got) == STORE_STATUS_OK, "get persisted call");
    assert_true(strcmp(got.number, "+49123") == 0, "number persisted");
    assert_true(strcmp(got.name, "Bob") == 0, "name persisted");
    assert_eq_u32(77u, got.duration_seconds, "duration persisted");
    assert_eq_u32(id, got.id, "id persisted");
    /* The legacy timestamp still packs month/day/time; payload v2 carries the
     * full supported year separately. Stub clock = 2026-06-24 07:35:12. */
    assert_eq_u32(2026u, got.datetime.year, "year packed/unpacked");
    assert_eq_u32(6u, got.datetime.month, "month packed/unpacked");
    assert_eq_u32(24u, got.datetime.day, "day packed/unpacked");
    assert_eq_u32(7u, got.datetime.hour, "hour packed/unpacked");
    assert_eq_u32(35u, got.datetime.minute, "minute packed/unpacked");
    assert_eq_u32(12u, got.datetime.second, "second packed/unpacked");
}

static void test_call_datetime_full_rtc_range_persists(void) {
    static const uint16_t YEARS[] = {1999u, 2000u, 2063u, 2064u, 2090u};

    fresh_store();
    for (size_t i = 0u; i < sizeof(YEARS) / sizeof(YEARS[0]); i++) {
        store_call_record_t record;
        memset(&record, 0, sizeof(record));
        record.datetime = (rtc_datetime_t){
            .year = YEARS[i],
            .month = 12u,
            .day = 31u,
            .hour = 23u,
            .minute = 59u,
            .second = 58u,
        };
        record.reason = STORE_CALL_REASON_RECEIVED;
        assert_true(store_call_add(STORE_CALL_LIST_RECEIVED, &record, 0) ==
                        STORE_STATUS_OK,
                    "add full-range call timestamp");
    }
    flush_commits();
    assert_true(store_service_init() == STORE_STATUS_OK,
                "re-init full-range call timestamps");

    for (size_t i = 0u; i < sizeof(YEARS) / sizeof(YEARS[0]); i++) {
        store_call_record_t record;
        size_t source_index = sizeof(YEARS) / sizeof(YEARS[0]) - 1u - i;
        assert_true(store_call_get(STORE_CALL_LIST_RECEIVED, (uint8_t)i,
                                   &record) == STORE_STATUS_OK,
                    "get full-range call timestamp");
        assert_eq_u32(YEARS[source_index], record.datetime.year,
                      "full RTC year survives call-log persistence");
        assert_true(record.datetime.month == 12u && record.datetime.day == 31u &&
                        record.datetime.hour == 23u && record.datetime.minute == 59u &&
                        record.datetime.second == 58u,
                    "non-year call timestamp fields survive");
    }
}

static void test_call_datetime_v1_payload_migrates(void) {
    enum {
        CALL_RECORD_OFFSET = 12u,
        CALL_RECORD_YEAR_CODE_OFFSET = CALL_RECORD_OFFSET + 15u,
    };

    fresh_store();
    store_call_record_t record;
    memset(&record, 0, sizeof(record));
    record.datetime = (rtc_datetime_t){
        .year = 2063u,
        .month = 7u,
        .day = 8u,
        .hour = 9u,
        .minute = 10u,
        .second = 11u,
    };
    record.reason = STORE_CALL_REASON_RECEIVED;
    assert_true(store_call_add(STORE_CALL_LIST_RECEIVED, &record, 0) ==
                    STORE_STATUS_OK,
                "seed v1 call timestamp fixture");

    uint8_t payload[128];
    size_t payload_len = 0u;
    assert_true(g_store_calls_unit_ops.serialize(
                    STORE_CALL_LIST_RECEIVED, payload, sizeof(payload),
                    &payload_len),
                "serialize call timestamp fixture");
    assert_true(payload[4] == 2u && payload[5] == 0u,
                "new call payload advertises version 2");
    uint8_t valid_year_code = payload[CALL_RECORD_YEAR_CODE_OFFSET];
    payload[CALL_RECORD_YEAR_CODE_OFFSET] = (uint8_t)(2090u - 1999u + 1u);
    assert_true(!g_store_calls_unit_ops.apply(
                    STORE_CALL_LIST_RECEIVED, payload, payload_len),
                "out-of-range version-2 call year rejected");
    store_call_record_t unchanged;
    assert_true(store_call_get(STORE_CALL_LIST_RECEIVED, 0u, &unchanged) ==
                    STORE_STATUS_OK && unchanged.datetime.year == 2063u,
                "invalid version-2 year leaves call list unchanged");

    payload[CALL_RECORD_YEAR_CODE_OFFSET] = valid_year_code;
    payload[4] = 1u;
    payload[5] = 0u;
    payload[CALL_RECORD_YEAR_CODE_OFFSET] = 0u;
    assert_true(write_unit_fixture(STORE_UNIT_CALLS_RECEIVED,
                                   payload, payload_len),
                "write legacy version-1 call fixture");
    assert_true(store_service_init() == STORE_STATUS_OK,
                "load legacy version-1 call fixture");

    store_call_record_t loaded;
    assert_true(store_call_get(STORE_CALL_LIST_RECEIVED, 0u, &loaded) ==
                    STORE_STATUS_OK,
                "read migrated version-1 call");
    assert_true(loaded.datetime.year == 2063u &&
                    loaded.datetime.month == 7u && loaded.datetime.day == 8u &&
                    loaded.datetime.hour == 9u && loaded.datetime.minute == 10u &&
                    loaded.datetime.second == 11u,
                "version-1 packed timestamp remains readable");
}

/* --------------------------------------------------------------------------
 * Speed dial helpers.
 * ------------------------------------------------------------------------ */

static void test_speed_dial(void) {
    fresh_store();
    uint16_t idx;
    /* Unset -> NOT_FOUND. */
    assert_true(store_phonebook_get_speed_dial(1u, &idx) == STORE_STATUS_NOT_FOUND, "unset speed dial NOT_FOUND");
    /* key out of range 0 and 10. */
    assert_true(store_phonebook_get_speed_dial(0u, &idx) == STORE_STATUS_INVALID_ARGUMENT, "speed dial key 0 invalid");
    assert_true(store_phonebook_get_speed_dial(10u, &idx) == STORE_STATUS_INVALID_ARGUMENT, "speed dial key 10 invalid");
    /* set/get each of 1..9. */
    for (uint8_t k = 1u; k <= 9u; k++) {
        assert_true(store_phonebook_set_speed_dial(k, (uint16_t)(100u + k)) == STORE_STATUS_OK, "set speed dial");
    }
    for (uint8_t k = 1u; k <= 9u; k++) {
        assert_true(store_phonebook_get_speed_dial(k, &idx) == STORE_STATUS_OK && idx == (uint16_t)(100u + k),
                    "get speed dial round trip");
    }
    /* Setting EMPTY sentinel as a real index is rejected. */
    assert_true(store_phonebook_set_speed_dial(1u, STORE_SPEED_DIAL_EMPTY) == STORE_STATUS_INVALID_ARGUMENT,
                "set speed dial to EMPTY rejected");
    /* clear -> back to NOT_FOUND. */
    assert_true(store_phonebook_clear_speed_dial(1u) == STORE_STATUS_OK, "clear speed dial");
    assert_true(store_phonebook_get_speed_dial(1u, &idx) == STORE_STATUS_NOT_FOUND, "cleared -> NOT_FOUND");
    assert_true(store_phonebook_set_speed_dial(0u, 5u) == STORE_STATUS_INVALID_ARGUMENT, "set key 0 invalid");
    assert_true(store_phonebook_clear_speed_dial(10u) == STORE_STATUS_INVALID_ARGUMENT, "clear key 10 invalid");
    /* Boundary value 0xfffe (one below EMPTY sentinel) must NOT be treated as empty. */
    assert_true(store_phonebook_set_speed_dial(2u, 0xfffeu) == STORE_STATUS_OK, "set near-sentinel speed dial");
    assert_true(store_phonebook_get_speed_dial(2u, &idx) == STORE_STATUS_OK && idx == 0xfffeu,
                "0xfffe speed dial not treated as empty");
}

/* --------------------------------------------------------------------------
 * Contact ringing tone storage (table of STORE_CONTACT_TONE_LIMIT entries).
 * ------------------------------------------------------------------------ */

static void test_contact_tone(void) {
    fresh_store();
    /* Default (unset) -> PRESET. */
    assert_true(store_phonebook_get_contact_tone_value(42u) == STORE_CONTACT_TONE_PRESET, "unset tone -> PRESET");
    /* set a non-preset value. */
    assert_true(store_phonebook_set_contact_tone_value(42u, 7u) == STORE_STATUS_OK, "set tone");
    assert_eq_u32(7u, store_phonebook_get_contact_tone_value(42u), "tone round trip");
    /* update existing. */
    assert_true(store_phonebook_set_contact_tone_value(42u, 9u) == STORE_STATUS_OK, "update tone");
    assert_eq_u32(9u, store_phonebook_get_contact_tone_value(42u), "tone updated");
    /* contact_index 0 rejected. */
    assert_true(store_phonebook_set_contact_tone_value(0u, 5u) == STORE_STATUS_INVALID_ARGUMENT,
                "contact 0 rejected");
    /* set to PRESET removes entry (back to default). */
    assert_true(store_phonebook_set_contact_tone_value(42u, STORE_CONTACT_TONE_PRESET) == STORE_STATUS_OK,
                "set PRESET removes");
    assert_eq_u32(STORE_CONTACT_TONE_PRESET, store_phonebook_get_contact_tone_value(42u), "removed -> PRESET");
    /* Fill the table to its limit (32 distinct contacts), then one more must fail. */
    for (uint16_t c = 1u; c <= 32u; c++) {
        assert_true(store_phonebook_set_contact_tone_value(c, 3u) == STORE_STATUS_OK, "fill tone table");
    }
    /* 33rd distinct contact: table full -> STORAGE_ERROR. */
    assert_true(store_phonebook_set_contact_tone_value(1000u, 3u) == STORE_STATUS_STORAGE_ERROR,
                "full tone table -> STORAGE_ERROR");
    /* But updating an existing contact still works when full. */
    assert_true(store_phonebook_set_contact_tone_value(1u, 4u) == STORE_STATUS_OK, "update when full OK");
    assert_eq_u32(4u, store_phonebook_get_contact_tone_value(1u), "updated when full");
    /* Setting PRESET on a non-existent contact while full is a no-op OK. */
    assert_true(store_phonebook_set_contact_tone_value(2000u, STORE_CONTACT_TONE_PRESET) == STORE_STATUS_OK,
                "PRESET on absent contact OK");
}

/* Contact-tone persistence + slot reuse after PRESET-removal. */
static void test_contact_tone_persistence(void) {
    fresh_store();
    store_phonebook_set_contact_tone_value(11u, 5u);
    store_phonebook_set_contact_tone_value(22u, 0xffu); /* NO_TONE = silent */
    flush_commits();
    assert_true(store_service_init() == STORE_STATUS_OK, "re-init contact tones");
    assert_eq_u32(5u, store_phonebook_get_contact_tone_value(11u), "tone 11 persisted");
    assert_eq_u32(0xffu, store_phonebook_get_contact_tone_value(22u), "tone 22 (silent) persisted");
}

/* --------------------------------------------------------------------------
 * T9 user dictionary save/load + bounds.
 * ------------------------------------------------------------------------ */

static void test_t9_user_words(void) {
    fresh_store();
    char words[STORE_T9_USER_WORD_LIMIT][STORE_T9_WORD_MAX + 1u];
    uint8_t count = 0;
    assert_true(store_t9_user_words_load(words, STORE_T9_USER_WORD_LIMIT, &count) == STORE_STATUS_OK, "load empty t9");
    assert_eq_u32(0u, count, "empty t9 count 0");

    /* Save a few, including one that is exactly max length. */
    char in[STORE_T9_USER_WORD_LIMIT][STORE_T9_WORD_MAX + 1u];
    memset(in, 0, sizeof(in));
    strcpy(in[0], "hello");
    strcpy(in[1], "world");
    memset(in[2], 'Z', STORE_T9_WORD_MAX);
    in[2][STORE_T9_WORD_MAX] = '\0';
    assert_true(store_t9_user_words_save(in, 3u) == STORE_STATUS_OK, "save 3 t9 words");
    assert_true(store_t9_user_words_load(words, STORE_T9_USER_WORD_LIMIT, &count) == STORE_STATUS_OK, "load t9");
    assert_eq_u32(3u, count, "t9 count 3");
    assert_true(strcmp(words[0], "hello") == 0, "t9 word0");
    assert_true(strcmp(words[2], in[2]) == 0, "t9 maxlen word preserved");

    /* Over-limit save rejected. */
    assert_true(store_t9_user_words_save(in, STORE_T9_USER_WORD_LIMIT + 1u) == STORE_STATUS_INVALID_ARGUMENT,
                "over-limit t9 save rejected");

    /* Load into smaller cap clamps count. */
    char small[2][STORE_T9_WORD_MAX + 1u];
    uint8_t scount = 0;
    assert_true(store_t9_user_words_save(in, 3u) == STORE_STATUS_OK, "save 3 again");
    assert_true(store_t9_user_words_load(small, 2u, &scount) == STORE_STATUS_OK, "load into small cap");
    assert_eq_u32(2u, scount, "load clamped to cap");

    /* Persistence. */
    flush_commits();
    assert_true(store_service_init() == STORE_STATUS_OK, "re-init t9");
    assert_true(store_t9_user_words_load(words, STORE_T9_USER_WORD_LIMIT, &count) == STORE_STATUS_OK, "load after reinit");
    assert_eq_u32(3u, count, "t9 persisted count");
    assert_true(strcmp(words[1], "world") == 0, "t9 persisted word1");
}

/* --------------------------------------------------------------------------
 * Picture messages: default seeding, set/get/clear, bounds, persistence.
 * ------------------------------------------------------------------------ */

static void test_picture_messages(void) {
    fresh_store();
    /* Fresh flash seeds all 4 default slots as used. */
    assert_eq_u32(4u, store_picture_message_count(), "four original templates seeded; extra slots remain free");
    store_picture_message_t msg;
    assert_true(store_picture_message_get(0u, &msg) == STORE_STATUS_OK, "get seeded slot 0");
    assert_true(msg.used, "seeded slot used");
    assert_eq_u32(STORE_PICTURE_WIDTH, msg.width, "seeded width");
    assert_eq_u32(STORE_PICTURE_HEIGHT, msg.height, "seeded height");
    assert_eq_u32(STORE_PICTURE_BITMAP_BYTES, msg.bitmap_len, "seeded bitmap len");

    /* Clear slot 0 then get -> NOT_FOUND, count drops. */
    assert_true(store_picture_message_clear(0u) == STORE_STATUS_OK, "clear slot 0");
    assert_true(store_picture_message_get(0u, &msg) == STORE_STATUS_NOT_FOUND, "cleared slot NOT_FOUND");
    assert_eq_u32(3u, store_picture_message_count(), "count after clear");

    /* Set a custom message, width/height 0 -> defaults applied. */
    store_picture_message_t set;
    memset(&set, 0, sizeof(set));
    set.width = 0u;
    set.height = 0u;
    set.bitmap_len = STORE_PICTURE_BITMAP_BYTES;
    memset(set.bitmap, 0xa5, STORE_PICTURE_BITMAP_BYTES);
    strcpy(set.text, "Hi there");
    assert_true(store_picture_message_set(1u, &set) == STORE_STATUS_OK, "set slot 1");
    assert_true(store_picture_message_get(1u, &msg) == STORE_STATUS_OK, "get slot 1");
    assert_eq_u32(STORE_PICTURE_WIDTH, msg.width, "set width default applied");
    assert_eq_u32(STORE_PICTURE_HEIGHT, msg.height, "set height default applied");
    assert_true(strcmp(msg.text, "Hi there") == 0, "set text round trip");
    assert_true(msg.bitmap[0] == 0xa5u && msg.bitmap[STORE_PICTURE_BITMAP_BYTES - 1u] == 0xa5u, "bitmap round trip");

    /* Out-of-range slot + oversized bitmap_len rejected. */
    assert_true(store_picture_message_get(STORE_PICTURE_SLOT_COUNT, &msg) == STORE_STATUS_INVALID_ARGUMENT,
                "oob slot get");
    assert_true(store_picture_message_set(STORE_PICTURE_SLOT_COUNT, &set) == STORE_STATUS_INVALID_ARGUMENT,
                "oob slot set");
    set.bitmap_len = STORE_PICTURE_BITMAP_BYTES + 1u;
    assert_true(store_picture_message_set(2u, &set) == STORE_STATUS_INVALID_ARGUMENT, "oversized bitmap rejected");

    /* Over-long text gets NUL-terminated at TEXT_MAX (struct field is TEXT_MAX+1). */
    memset(&set, 0, sizeof(set));
    set.width = STORE_PICTURE_WIDTH;
    set.height = STORE_PICTURE_HEIGHT;
    set.bitmap_len = 0u;
    memset(set.text, 'T', STORE_PICTURE_TEXT_MAX);
    set.text[STORE_PICTURE_TEXT_MAX] = '\0';
    assert_true(store_picture_message_set(2u, &set) == STORE_STATUS_OK, "set maxlen text");
    assert_true(store_picture_message_get(2u, &msg) == STORE_STATUS_OK, "get maxlen text");
    assert_true(msg.text[STORE_PICTURE_TEXT_MAX] == '\0', "picture text terminated");
    assert_eq_u32(STORE_PICTURE_TEXT_MAX, (uint32_t)strlen(msg.text), "picture maxlen text preserved");

    /* Persistence. */
    flush_commits();
    assert_true(store_service_init() == STORE_STATUS_OK, "re-init pictures");
    assert_true(store_picture_message_get(1u, &msg) == STORE_STATUS_OK, "slot 1 persisted");
    assert_true(strcmp(msg.text, "Hi there") == 0, "picture text persisted");
    assert_true(store_picture_message_get(0u, &msg) == STORE_STATUS_NOT_FOUND, "cleared slot stays cleared");
}

/* --------------------------------------------------------------------------
 * Own tones: set/get/clear, name/notes/packed clamping, persistence.
 * ------------------------------------------------------------------------ */

static void test_own_tones(void) {
    fresh_store();
    store_own_tone_t tone;
    assert_true(store_own_tone_get(0u, &tone) == STORE_STATUS_NOT_FOUND, "own tone unset NOT_FOUND");
    assert_true(!store_own_tone_used(0u), "own tone slot 0 unused");

    store_own_tone_t set;
    memset(&set, 0, sizeof(set));
    strcpy(set.name, "MyTone");
    strcpy(set.notes, "c d e f g");
    set.tempo_index = 4u;
    set.packed_len = STORE_OWN_TONE_PACKED_MAX; /* exactly max */
    memset(set.packed, 0x3c, STORE_OWN_TONE_PACKED_MAX);
    assert_true(store_own_tone_set(0u, &set) == STORE_STATUS_OK, "set own tone");
    assert_true(store_own_tone_used(0u), "own tone slot 0 used");
    assert_true(store_own_tone_get(0u, &tone) == STORE_STATUS_OK, "get own tone");
    assert_true(strcmp(tone.name, "MyTone") == 0, "own tone name");
    assert_eq_u32(STORE_OWN_TONE_PACKED_MAX, tone.packed_len, "own tone packed_len at max");

    /* packed_len over max is clamped. */
    memset(&set, 0, sizeof(set));
    strcpy(set.name, "Over");
    set.packed_len = STORE_OWN_TONE_PACKED_MAX + 100u;
    assert_true(store_own_tone_set(1u, &set) == STORE_STATUS_OK, "set over-packed tone");
    assert_true(store_own_tone_get(1u, &tone) == STORE_STATUS_OK, "get over-packed tone");
    assert_eq_u32(STORE_OWN_TONE_PACKED_MAX, tone.packed_len, "packed_len clamped to max");

    /* OOB slot. */
    assert_true(store_own_tone_get(STORE_OWN_TONE_SLOT_COUNT, &tone) == STORE_STATUS_INVALID_ARGUMENT, "oob own tone");
    assert_true(store_own_tone_set(STORE_OWN_TONE_SLOT_COUNT, &set) == STORE_STATUS_INVALID_ARGUMENT,
                "oob own tone set");

    /* Persistence. */
    flush_commits();
    assert_true(store_service_init() == STORE_STATUS_OK, "re-init own tones");
    assert_true(store_own_tone_get(0u, &tone) == STORE_STATUS_OK, "own tone persisted");
    assert_true(strcmp(tone.name, "MyTone") == 0, "own tone name persisted");
    assert_eq_u32(STORE_OWN_TONE_PACKED_MAX, tone.packed_len, "own tone packed_len persisted");

    /* clear. */
    assert_true(store_own_tone_clear(0u) == STORE_STATUS_OK, "clear own tone");
    assert_true(!store_own_tone_used(0u), "own tone cleared");
}

/* --------------------------------------------------------------------------
 * Call divert + warranty.
 * ------------------------------------------------------------------------ */

static void test_call_divert(void) {
    fresh_store();
    store_call_divert_state_t st;
    assert_true(store_call_divert_get(&st) == STORE_STATUS_OK, "get divert default");
    assert_eq_u32(20u, st.delay_seconds, "default divert delay 20");

    memset(&st, 0, sizeof(st));
    st.active_mask = 0xffu; /* only low 5 bits should be kept */
    st.delay_seconds = 0u;  /* 0 -> default 20 */
    strcpy(st.numbers[0], "+49111");
    assert_true(store_call_divert_set(&st) == STORE_STATUS_OK, "set divert");
    store_call_divert_state_t got;
    assert_true(store_call_divert_get(&got) == STORE_STATUS_OK, "get divert");
    assert_eq_u32(0x1fu, got.active_mask, "divert mask clamped to 5 bits");
    assert_eq_u32(20u, got.delay_seconds, "divert delay 0 -> default 20");
    assert_true(strcmp(got.numbers[0], "+49111") == 0, "divert number round trip");

    flush_commits();
    assert_true(store_service_init() == STORE_STATUS_OK, "re-init divert");
    assert_true(store_call_divert_get(&got) == STORE_STATUS_OK, "get divert after reinit");
    assert_eq_u32(0x1fu, got.active_mask, "divert mask persisted");
    assert_true(strcmp(got.numbers[0], "+49111") == 0, "divert number persisted");
}

static void write_legacy_warranty_fixture(void) {
    uint8_t payload[47];
    memset(payload, 0, sizeof(payload));

    /* Warranty v1 payload from the pre-provisioning firmware carrying an
     * invalid serial: retain its non-identity fields while migrating the
     * serial to blank (re-arming first-run provisioning). */
    payload[0] = 0x31u; /* WARRANTY_MAGIC 0x57525431, little-endian */
    payload[1] = 0x54u;
    payload[2] = 0x52u;
    payload[3] = 0x57u;
    payload[4] = 1u;    /* STORE_PAYLOAD_VERSION */
    payload[6] = 0x80u;
    payload[8] = 0x41u; /* life timer = 321 seconds */
    payload[9] = 0x01u;
    payload[12] = 15u;
    memcpy(&payload[13], "490154203237519", 15u); /* bad Luhn check digit */
    payload[29] = 4u;
    memcpy(&payload[30], "0899", 4u);
    payload[35] = 4u;
    memcpy(&payload[36], "0000", 4u);
    payload[41] = 4u;
    memcpy(&payload[42], "0626", 4u);

    assert_true(write_unit_fixture(STORE_UNIT_SERVICE_WARRANTY, payload, sizeof(payload)),
                "legacy warranty fixture writes v1 payload");
}

static void put_u32_le(uint8_t *dst, uint32_t value) {
    dst[0] = (uint8_t)value;
    dst[1] = (uint8_t)(value >> 8);
    dst[2] = (uint8_t)(value >> 16);
    dst[3] = (uint8_t)(value >> 24);
}

static void put_u16_le(uint8_t *dst, uint16_t value) {
    dst[0] = (uint8_t)value;
    dst[1] = (uint8_t)(value >> 8);
}

static void put_u64_le(uint8_t *dst, uint64_t value) {
    for (uint8_t i = 0u; i < 8u; i++) {
        dst[i] = (uint8_t)(value >> (i * 8u));
    }
}

static void assert_warranty_snapshot(const store_warranty_state_t *expected,
                                     uint32_t expected_legacy_timer,
                                     const char *message) {
    store_warranty_state_t actual;
    bool unchanged = store_warranty_get(&actual) == STORE_STATUS_OK &&
                     memcmp(&actual, expected, sizeof(actual)) == 0 &&
                     store_warranty_legacy_life_timer() == expected_legacy_timer;
    assert_true(unchanged, message);
}

static void test_warranty_payload_apply_is_atomic(void) {
    enum {
        SERIAL_LEN_OFFSET = 12u,
        MADE_LEN_OFFSET = SERIAL_LEN_OFFSET + 1u + STORE_WARRANTY_SERIAL_MAX + 1u,
        REPAIRED_LEN_OFFSET = MADE_LEN_OFFSET + 1u + STORE_WARRANTY_MMYY_MAX + 1u,
        PURCHASE_LEN_OFFSET = REPAIRED_LEN_OFFSET + 1u + STORE_WARRANTY_MMYY_MAX + 1u,
    };
    static const size_t length_offsets[] = {
        SERIAL_LEN_OFFSET,
        MADE_LEN_OFFSET,
        REPAIRED_LEN_OFFSET,
        PURCHASE_LEN_OFFSET,
    };
    static const uint8_t invalid_lengths[] = {
        STORE_WARRANTY_SERIAL_MAX + 1u,
        STORE_WARRANTY_MMYY_MAX + 1u,
        STORE_WARRANTY_MMYY_MAX + 1u,
        STORE_WARRANTY_MMYY_MAX + 1u,
    };

    fresh_store();
    assert_true(store_board_imei_provision("490154203237518") == STORE_STATUS_OK,
                "atomic warranty fixture IMEI");
    assert_true(store_warranty_set_purchase_date("0826") == STORE_STATUS_OK,
                "atomic warranty fixture purchase date");

    uint8_t payload[64];
    size_t payload_len = 0u;
    assert_true(g_store_warranty_unit_ops.serialize(
                    0u, payload, sizeof(payload), &payload_len),
                "serialize atomic warranty fixture");
    assert_eq_u32(PURCHASE_LEN_OFFSET + 1u + STORE_WARRANTY_MMYY_MAX + 1u,
                  (uint32_t)payload_len,
                  "warranty payload layout matches field boundaries");

    put_u32_le(&payload[8], 0x11223344u);
    assert_true(g_store_warranty_unit_ops.apply(0u, payload, payload_len),
                "seed warranty migration donor");
    store_warranty_state_t baseline;
    assert_true(store_warranty_get(&baseline) == STORE_STATUS_OK,
                "capture atomic warranty baseline");
    assert_eq_u32(0x11223344u, store_warranty_legacy_life_timer(),
                  "baseline migration donor seeded");

    uint8_t candidate[sizeof(payload)];
    memcpy(candidate, payload, payload_len);
    candidate[6] ^= 0x5au;
    put_u32_le(&candidate[8], 0xa1b2c3d4u);

    for (size_t truncated_len = 0u; truncated_len < payload_len; truncated_len++) {
        assert_true(!g_store_warranty_unit_ops.apply(
                        0u, candidate, truncated_len),
                    "truncated warranty payload rejected");
        assert_warranty_snapshot(&baseline, 0x11223344u,
                                 "truncated warranty payload is atomic");
    }

    for (size_t i = 0u; i < sizeof(length_offsets) / sizeof(length_offsets[0]); i++) {
        uint8_t malformed[sizeof(payload)];
        memcpy(malformed, candidate, payload_len);
        malformed[length_offsets[i]] = invalid_lengths[i];
        assert_true(!g_store_warranty_unit_ops.apply(0u, malformed, payload_len),
                    "overlength warranty field rejected");
        assert_warranty_snapshot(&baseline, 0x11223344u,
                                 "overlength warranty field is atomic");
    }

    assert_true(g_store_warranty_unit_ops.apply(0u, candidate, payload_len),
                "complete warranty payload accepted");
    store_warranty_state_t updated;
    assert_true(store_warranty_get(&updated) == STORE_STATUS_OK &&
                    updated.flags == candidate[6],
                "complete warranty payload publishes state");
    assert_eq_u32(0xa1b2c3d4u, store_warranty_legacy_life_timer(),
                  "complete warranty payload publishes donor atomically");
}

static void test_warranty(void) {
    fresh_store();
    store_warranty_state_t w;
    char imei[STORE_WARRANTY_SERIAL_MAX + 1u];
    assert_true(store_warranty_get(&w) == STORE_STATUS_OK, "get warranty default");
    assert_true(w.serial[0] == '\0', "fresh board identity is unprovisioned");
    assert_true(store_board_imei_get(imei, sizeof(imei)) == STORE_STATUS_NOT_FOUND,
                "fresh board has no IMEI");
    assert_true(strcmp(w.made, "0899") == 0, "default warranty made");

    assert_true(store_board_imei_valid("490154203237518"),
                "valid 15-digit Luhn IMEI accepted");
    assert_true(!store_board_imei_valid("490154203237519"),
                "bad IMEI check digit rejected");
    assert_true(!store_board_imei_valid("49015420323751X"),
                "non-digit IMEI rejected");
    assert_true(!store_board_imei_valid("49015420323751"),
                "short IMEI rejected");
    assert_true(store_board_imei_get(imei, 15u) == STORE_STATUS_INVALID_ARGUMENT,
                "undersized IMEI output rejected");
    assert_true(store_board_imei_provision("490154203237519") ==
                    STORE_STATUS_INVALID_ARGUMENT,
                "invalid IMEI cannot claim the board slot");
    assert_true(store_board_imei_provision("490154203237518") == STORE_STATUS_OK,
                "first valid IMEI provisions board identity");
    assert_true(store_board_imei_get(imei, sizeof(imei)) == STORE_STATUS_OK &&
                    strcmp(imei, "490154203237518") == 0,
                "provisioned IMEI reads back exactly");
    assert_true(store_board_imei_provision("490154203237518") == STORE_STATUS_OK,
                "same IMEI provisioning is idempotent");
    assert_true(store_board_imei_provision("356938035643809") ==
                    STORE_STATUS_CONFLICT,
                "different valid IMEI cannot overwrite board identity");

    /* Empty service fields refill from defaults, while even a full-record
     * caller cannot mutate the write-once serial. */
    store_warranty_state_t set;
    memset(&set, 0, sizeof(set));
    strcpy(set.serial, "356938035643809");
    assert_true(store_warranty_set(&set) == STORE_STATUS_OK, "set warranty (empty -> defaults)");
    assert_true(store_warranty_get(&w) == STORE_STATUS_OK, "get warranty after set");
    assert_true(strcmp(w.serial, "490154203237518") == 0,
                "generic warranty update preserves board IMEI");
    assert_true(strcmp(w.repaired, "0000") == 0, "repaired refilled from default");

    flush_commits();
    assert_true(store_service_init() == STORE_STATUS_OK, "re-init provisioned warranty");
    assert_true(store_board_imei_get(imei, sizeof(imei)) == STORE_STATUS_OK &&
                    strcmp(imei, "490154203237518") == 0,
                "board IMEI persists across reboot");

    /* purchase_date setter sets flag 0x80. */
    assert_true(store_warranty_set_purchase_date("0626") == STORE_STATUS_OK, "set purchase date");
    assert_true(store_warranty_get(&w) == STORE_STATUS_OK, "get warranty pd");
    assert_true(strcmp(w.purchase_date, "0626") == 0, "purchase date stored");
    assert_true((w.flags & 0x80u) != 0u, "purchase flag set");
    /* Clearing purchase date clears the flag. */
    assert_true(store_warranty_set_purchase_date("") == STORE_STATUS_OK, "clear purchase date");
    assert_true(store_warranty_get(&w) == STORE_STATUS_OK, "get warranty pd cleared");
    assert_true((w.flags & 0x80u) == 0u, "purchase flag cleared");

    /* A short completed call must be persisted. The old 30-minute warranty
     * batching policy lost this exact update on every reboot/reflash. */
    fresh_store();
    assert_eq_u32(0u, store_life_timer_seconds(), "life timer starts 0");
    assert_true(store_life_timer_add_seconds(100u) == STORE_STATUS_OK, "add 100s");
    assert_eq_u32(100u, store_life_timer_seconds(), "life timer reads 100 immediately");
    assert_true(store_life_timer_add_seconds(0u) == STORE_STATUS_OK, "add 0 is no-op OK");
    flush_commits();
    assert_true(store_service_init() == STORE_STATUS_OK, "re-init short life timer");
    assert_eq_u32(100u, store_life_timer_seconds(), "short life timer persists");

    /* Overflow saturates at UINT32_MAX rather than wrapping. */
    fresh_store();
    assert_true(store_setting_set_u32(STORE_SETTING_CALL_DURATION_LIFETIME,
                                      0xfffffff0u) == STORE_STATUS_OK,
                "seed near-overflow life timer");
    assert_true(store_life_timer_add_seconds(0x100u) == STORE_STATUS_OK, "add near-overflow");
    assert_eq_u32(0xffffffffu, store_life_timer_seconds(), "life timer saturates at UINT32_MAX");

    /* NULL rejected. */
    assert_true(store_warranty_get(0) == STORE_STATUS_INVALID_ARGUMENT, "null warranty get");
    assert_true(store_warranty_set(0) == STORE_STATUS_INVALID_ARGUMENT, "null warranty set");
    assert_true(store_board_imei_get(0, sizeof(imei)) == STORE_STATUS_INVALID_ARGUMENT,
                "null IMEI get rejected");
    assert_true(store_board_imei_provision(0) == STORE_STATUS_INVALID_ARGUMENT,
                "null IMEI provision rejected");

    fresh_store();
    write_legacy_warranty_fixture();
    assert_true(store_service_init() == STORE_STATUS_OK,
                "load pre-provisioning warranty payload");
    assert_true(store_board_imei_get(imei, sizeof(imei)) == STORE_STATUS_NOT_FOUND,
                "legacy invalid serial migrates to unprovisioned");
    assert_true(store_warranty_get(&w) == STORE_STATUS_OK &&
                    strcmp(w.made, "0899") == 0 &&
                    strcmp(w.repaired, "0000") == 0 &&
                    strcmp(w.purchase_date, "0626") == 0 &&
                    w.flags == 0x80u,
                "IMEI migration preserves all warranty fields");
    assert_eq_u32(321u, store_life_timer_seconds(),
                  "legacy warranty Life timer migrates to call accounting");
    flush_commits();
    assert_true(store_service_init() == STORE_STATUS_OK,
                "re-init migrated warranty payload");
    assert_true(store_board_imei_get(imei, sizeof(imei)) == STORE_STATUS_NOT_FOUND,
                "cleared legacy identity persists");
    assert_eq_u32(321u, store_life_timer_seconds(),
                  "migrated legacy Life timer persists");

    /* Existing installations may have lost the batched warranty value while
     * their resettable All-calls counter survived. Recover the larger total,
     * then prove a user Clear-timers equivalent cannot erase the odometer. */
    fresh_store();
    assert_true(store_setting_set_u32(STORE_SETTING_CALL_DURATION_ALL, 600u) ==
                    STORE_STATUS_OK,
                "seed surviving All-calls total");
    flush_commits();
    assert_true(store_service_init() == STORE_STATUS_OK,
                "re-init for All-calls migration");
    assert_eq_u32(600u, store_life_timer_seconds(),
                  "All-calls total repairs missing Life timer");
    assert_true(store_setting_set_u32(STORE_SETTING_CALL_DURATION_ALL, 0u) ==
                    STORE_STATUS_OK,
                "simulate Clear timers");
    flush_commits();
    assert_true(store_service_init() == STORE_STATUS_OK,
                "re-init after Clear timers");
    assert_eq_u32(600u, store_life_timer_seconds(),
                  "Clear timers leaves Life timer intact");
}

/* --------------------------------------------------------------------------
 * Persistent wire contract. Public round-trip tests cannot catch a serializer
 * and parser drifting together, so this fixture pins every journal payload at
 * the storage boundary. FNV-1a is only a compact representation of exact bytes;
 * it is not part of the on-device format.
 * ------------------------------------------------------------------------ */

typedef struct {
    uint16_t length;
    uint64_t fnv64;
} wire_expectation_t;

static const wire_expectation_t WIRE_EXPECTED[STORE_UNIT_COUNT] = {
    [STORE_UNIT_SETTINGS_PHONEBOOK] = {79u, UINT64_C(0xf86cf5cdb26f0f03)},
    [STORE_UNIT_SETTINGS_SMS] = {61u, UINT64_C(0xb1adfff1f95740f2)},
    [STORE_UNIT_SETTINGS_CALLS] = {98u, UINT64_C(0x06494ae4b8997cfd)},
    [STORE_UNIT_SETTINGS_PROFILES] = {228u, UINT64_C(0xbdc1090e2e99d1fd)},
    [STORE_UNIT_SETTINGS_CLOCK] = {64u, UINT64_C(0x74cdc936f4b05dbe)},
    [STORE_UNIT_SETTINGS_SYSTEM] = {199u, UINT64_C(0xb256c8c33d919af1)},
    [STORE_UNIT_CALLS_MISSED] = {86u, UINT64_C(0x5d274c1062d12d57)},
    [STORE_UNIT_CALLS_RECEIVED] = {86u, UINT64_C(0xeccbd2835e818e41)},
    [STORE_UNIT_CALLS_DIALLED] = {86u, UINT64_C(0x5defde58a2af983b)},
    [STORE_UNIT_T9_USER_DICT] = {76u, UINT64_C(0x48a39e50599417d3)},
    [STORE_UNIT_PICTURE_MESSAGES] = {3824u, UINT64_C(0xde94c68eff59b554)},
    [STORE_UNIT_OWN_TONES] = {1120u, UINT64_C(0xc374477106e5bbfb)},
    [STORE_UNIT_CALL_DIVERT] = {178u, UINT64_C(0x42696acc31545aa2)},
    [STORE_UNIT_SERVICE_WARRANTY] = {47u, UINT64_C(0x5f7a97e531cb6c07)},
    [STORE_UNIT_BATTERY_LEARNING] = {96u, UINT64_C(0x3c6dd9f41c1206b2)},
    [STORE_UNIT_BATTERY_CHARGE_SUPERVISOR] = {
        128u, UINT64_C(0x08158b2f629793cb)},
};

static void test_battery_learning_payload_and_persistence(void) {
    battery_learning_persisted_t expected;
    battery_learning_persisted_defaults(&expected, NULL);
    expected.capacity_history_mah[0] = 1000u;
    expected.capacity_history_count = 1u;
    expected.capacity_history_next = 1u;
    expected.accepted_capacity_cycles = 1u;
    expected.last_capacity_mah = 1000u;
    expected.resistance_mohm[0] = 190u;
    expected.resistance_sample_count[0] = 9u;
    expected.pack_generation = 3u;
    expected.full_anchor_acr_raw = UINT32_C(0x10203040);
    expected.full_anchor_nah = -INT64_C(987654321);
    expected.cycle_min_temperature_mdegc = -5000;
    expected.cycle_max_temperature_mdegc = 41000;
    expected.full_anchor_valid = true;
    expected.soc_valid = true;
    expected.soc_charge_segment = true;
    expected.soc_capacity_mah = 1000u;
    expected.soc_charge_factor_permille = 1224u;
    expected.soc_bootstrap_reference_mv = 2500u;
    expected.soc_anchor_provenance = BATTERY_SOC_PROVENANCE_TRACKED;
    expected.soc_confidence = BATTERY_SOC_CONFIDENCE_PROVISIONAL;
    expected.soc_anchor_session_delta_nah = -INT64_C(123456789);
    expected.soc_anchor_remaining_nah = INT64_C(600000000);

    fresh_store();
    battery_learning_persisted_t actual;
    assert_true(store_battery_learning_get(&actual) == STORE_STATUS_OK &&
                    actual.nominal_capacity_mah == 1225u &&
                    actual.capacity_history_count == 0u,
                "battery learning starts from product prior");
    assert_true(store_battery_learning_set(&expected) == STORE_STATUS_OK,
                "battery learning record accepted");
    assert_true(store_battery_learning_get(&actual) == STORE_STATUS_OK &&
                    memcmp(&actual, &expected, sizeof(actual)) == 0,
                "battery learning record round-trips through RAM owner");

    flush_commits();
    battery_learning_persisted_t same_fields;
    memset(&same_fields, 0xa5, sizeof(same_fields));
    same_fields.profile_id = expected.profile_id;
    same_fields.nominal_capacity_mah = expected.nominal_capacity_mah;
    memcpy(same_fields.capacity_history_mah,
           expected.capacity_history_mah,
           sizeof(same_fields.capacity_history_mah));
    same_fields.capacity_history_count = expected.capacity_history_count;
    same_fields.capacity_history_next = expected.capacity_history_next;
    same_fields.accepted_capacity_cycles =
        expected.accepted_capacity_cycles;
    same_fields.rejected_capacity_cycles =
        expected.rejected_capacity_cycles;
    same_fields.last_capacity_mah = expected.last_capacity_mah;
    memcpy(same_fields.resistance_mohm, expected.resistance_mohm,
           sizeof(same_fields.resistance_mohm));
    memcpy(same_fields.resistance_sample_count,
           expected.resistance_sample_count,
           sizeof(same_fields.resistance_sample_count));
    same_fields.pack_generation = expected.pack_generation;
    same_fields.full_anchor_acr_raw = expected.full_anchor_acr_raw;
    same_fields.full_anchor_nah = expected.full_anchor_nah;
    same_fields.cycle_min_temperature_mdegc =
        expected.cycle_min_temperature_mdegc;
    same_fields.cycle_max_temperature_mdegc =
        expected.cycle_max_temperature_mdegc;
    same_fields.full_anchor_valid = expected.full_anchor_valid;
    same_fields.capacity_cycle_qualified =
        expected.capacity_cycle_qualified;
    same_fields.natural_empty_valid = expected.natural_empty_valid;
    same_fields.soc_valid = expected.soc_valid;
    same_fields.soc_charge_segment = expected.soc_charge_segment;
    same_fields.soc_capacity_mah = expected.soc_capacity_mah;
    same_fields.soc_charge_factor_permille =
        expected.soc_charge_factor_permille;
    same_fields.soc_bootstrap_reference_mv =
        expected.soc_bootstrap_reference_mv;
    same_fields.soc_anchor_provenance = expected.soc_anchor_provenance;
    same_fields.soc_confidence = expected.soc_confidence;
    same_fields.soc_anchor_session_delta_nah =
        expected.soc_anchor_session_delta_nah;
    same_fields.soc_anchor_remaining_nah =
        expected.soc_anchor_remaining_nah;
    assert_true(store_battery_learning_set(&same_fields) == STORE_STATUS_OK,
                "semantic battery record ignores C-struct padding");
    store_diag_snapshot_t diag;
    store_service_get_diag(&diag);
    assert_eq_u32(0u, diag.dirty_mask,
                  "equal battery record does not dirty flash");

    uint8_t payload[96];
    size_t len = 0u;
    assert_true(!g_store_battery_learning_unit_ops.serialize(
                    0u, payload, sizeof(payload) - 1u, &len),
                "battery learning serializer rejects short destination");
    assert_true(g_store_battery_learning_unit_ops.serialize(
                    0u, payload, sizeof(payload), &len) &&
                    len == sizeof(payload),
                "battery learning serializer emits fixed wire record");
    for (size_t truncated = 0u; truncated < sizeof(payload); truncated++) {
        assert_true(!g_store_battery_learning_unit_ops.apply(
                        0u, payload, truncated),
                    "battery learning parser rejects every truncation");
    }

    g_store_battery_learning_unit_ops.reset_ram(0u);
    assert_true(g_store_battery_learning_unit_ops.apply(
                    0u, payload, sizeof(payload)),
                "battery learning parser accepts exact record");
    assert_true(store_battery_learning_get(&actual) == STORE_STATUS_OK &&
                    memcmp(&actual, &expected, sizeof(actual)) == 0,
                "battery learning wire record restores every field");

    uint8_t malformed[sizeof(payload)];
    memcpy(malformed, payload, sizeof(malformed));
    malformed[7] |= 0x80u;
    assert_true(!g_store_battery_learning_unit_ops.apply(
                    0u, malformed, sizeof(malformed)),
                "battery learning parser rejects unknown flags");
    assert_true(store_battery_learning_get(&actual) == STORE_STATUS_OK &&
                    memcmp(&actual, &expected, sizeof(actual)) == 0,
                "failed battery payload apply is atomic");
    memcpy(malformed, payload, sizeof(malformed));
    malformed[20] = BATTERY_LEARNING_CAPACITY_HISTORY_COUNT + 1u;
    assert_true(!g_store_battery_learning_unit_ops.apply(
                    0u, malformed, sizeof(malformed)),
                "battery learning parser rejects impossible model state");
    memcpy(malformed, payload, sizeof(malformed));
    malformed[8] = 0u;
    assert_true(!g_store_battery_learning_unit_ops.apply(
                    0u, malformed, sizeof(malformed)),
                "battery learning parser rejects a mismatched chemistry profile");
    memcpy(malformed, payload, sizeof(malformed));
    malformed[12] ^= 1u;
    assert_true(!g_store_battery_learning_unit_ops.apply(
                    0u, malformed, sizeof(malformed)),
                "battery learning parser rejects a mismatched product prior");

    battery_learning_persisted_t empty_endpoint = expected;
    empty_endpoint.full_anchor_valid = false;
    empty_endpoint.capacity_cycle_qualified = false;
    empty_endpoint.natural_empty_valid = true;
    empty_endpoint.soc_charge_segment = false;
    empty_endpoint.soc_capacity_mah = 1000u;
    empty_endpoint.soc_charge_factor_permille = 1224u;
    empty_endpoint.soc_bootstrap_reference_mv = 0u;
    empty_endpoint.soc_anchor_provenance =
        BATTERY_SOC_PROVENANCE_ANCHORED_EMPTY;
    empty_endpoint.soc_confidence = BATTERY_SOC_CONFIDENCE_ANCHORED;
    empty_endpoint.soc_anchor_remaining_nah = 0;
    assert_true(store_battery_learning_set(&empty_endpoint) == STORE_STATUS_OK,
                "battery learning natural-empty record accepted");
    uint8_t empty_payload[sizeof(payload)];
    assert_true(g_store_battery_learning_unit_ops.serialize(
                    0u, empty_payload, sizeof(empty_payload), &len) &&
                    (empty_payload[6] & 0x04u) != 0u,
                "battery learning natural-empty flag is encoded");
    g_store_battery_learning_unit_ops.reset_ram(0u);
    assert_true(g_store_battery_learning_unit_ops.apply(
                    0u, empty_payload, sizeof(empty_payload)),
                "battery learning natural-empty record decodes");
    assert_true(store_battery_learning_get(&actual) == STORE_STATUS_OK &&
                    memcmp(&actual, &empty_endpoint, sizeof(actual)) == 0,
                "battery learning natural-empty marker round-trips");

    /* A deployed BTL1 journal must remain readable. The storage layer imports
     * only evidence that existed in v1; battery_learning_init then promotes
     * the exact FULL anchor into the general SOC ledger. */
    uint8_t legacy[64] = {0};
    put_u32_le(&legacy[0], UINT32_C(0x314c5442));
    put_u16_le(&legacy[4], STORE_PAYLOAD_VERSION);
    legacy[6] = 0x03u;
    legacy[7] = BATTERY_LEARNING_PROFILE_NIMH_2S;
    put_u16_le(&legacy[8], 1225u);
    put_u16_le(&legacy[10], 1000u);
    legacy[16] = 1u;
    legacy[17] = 1u;
    put_u16_le(&legacy[18], 1u);
    put_u16_le(&legacy[22], 1000u);
    put_u32_le(&legacy[36], 4u);
    put_u32_le(&legacy[40], UINT32_C(0x81234567));
    put_u64_le(&legacy[44], (uint64_t)-INT64_C(222000000));
    put_u32_le(&legacy[52], (uint32_t)12000);
    put_u32_le(&legacy[56], (uint32_t)33000);
    assert_true(g_store_battery_learning_unit_ops.apply(
                    0u, legacy, sizeof(legacy)),
                "battery learning parser accepts deployed BTL1 record");
    assert_true(store_battery_learning_get(&actual) == STORE_STATUS_OK &&
                    actual.full_anchor_valid &&
                    actual.capacity_cycle_qualified &&
                    actual.capacity_history_count == 1u &&
                    actual.capacity_history_mah[0] == 1000u &&
                    !actual.soc_valid,
                "BTL1 import preserves old evidence without inventing wire fields");
    battery_learning_state_t migrated;
    battery_learning_init(&migrated, NULL, &actual);
    battery_learning_snapshot_t migrated_snapshot;
    battery_learning_get_snapshot(&migrated, &migrated_snapshot);
    assert_true(migrated_snapshot.soc_confidence ==
                    BATTERY_SOC_CONFIDENCE_ANCHORED &&
                    migrated.persisted.soc_valid &&
                    migrated.persisted.soc_anchor_provenance ==
                        BATTERY_SOC_PROVENANCE_ANCHORED_FULL &&
                    migrated.persisted.soc_anchor_remaining_nah ==
                        INT64_C(1000000000),
                "BTL1 FULL anchor migrates exactly into anchored SOC");

    battery_learning_persisted_t invalid = expected;
    invalid.capacity_history_next = 2u;
    assert_true(store_battery_learning_set(&invalid) ==
                    STORE_STATUS_INVALID_ARGUMENT,
                "battery learning setter rejects impossible model state");
    assert_true(store_battery_learning_set(NULL) ==
                    STORE_STATUS_INVALID_ARGUMENT &&
                    store_battery_learning_get(NULL) ==
                    STORE_STATUS_INVALID_ARGUMENT,
                "battery learning public API rejects null arguments");

    assert_true(store_battery_learning_set(&expected) == STORE_STATUS_OK,
                "battery learning state restored before persistence test");
    flush_commits();
    assert_true(store_service_init() == STORE_STATUS_OK,
                "battery learning journal reload succeeds");
    assert_true(store_battery_learning_get(&actual) == STORE_STATUS_OK &&
                    memcmp(&actual, &expected, sizeof(actual)) == 0,
                "battery learning state survives restart");
}

static battery_charge_supervisor_persisted_t charge_supervisor_fixture(void) {
    return (battery_charge_supervisor_persisted_t){
        .hardware_profile_id = 1u,
        .chemistry = BATTERY_CHARGE_CHEMISTRY_NIMH_2S,
        .charge_generation = 9u,
        .active_session_valid = true,
        .pack_generation = 7u,
        .gauge_session = 3u,
        .start_acr_raw = UINT32_C(0x89abcdef),
        .start_session_delta_nah = -INT64_C(123456789),
        .frozen_capacity_valid = true,
        .frozen_capacity_mah = 1100u,
        .frozen_capacity_confidence = BATTERY_CAPACITY_CONFIDENCE_LEARNED,
        .frozen_remaining_valid = true,
        .frozen_remaining_mah = 250u,
        .frozen_remaining_nah = UINT64_C(250250000),
        .frozen_soc_provenance = BATTERY_SOC_PROVENANCE_TRACKED,
        .frozen_soc_confidence = BATTERY_SOC_CONFIDENCE_ANCHORED,
        .frozen_resistance_valid = true,
        .frozen_resistance_mohm = 220u,
        .deficit_valid = true,
        .deficit_mah = 850u,
        .deficit_nah = UINT64_C(849750000),
        .charge_factor_permille = 1400u,
        .charge_factor_confident = true,
        .target_valid = true,
        .target_input_nah = UINT64_C(1189650000),
        .safety_input_nah = UINT64_C(1925000000),
        .configured_policy = BATTERY_CHARGE_POLICY_ENFORCE_BOOTSTRAP,
        .configured_charge_factor_permille = 1224u,
        .configured_charge_factor_confident = true,
        .supervisor_inhibit_latched = true,
        .latched_stop_reason =
            BATTERY_CHARGE_TERMINAL_SOFTWARE_COULOMB_FULL,
        .completion_rearm_used = true,
        .last_terminal_valid = true,
        .last_terminal_full_qualified = true,
        .last_terminal_net_input_valid = true,
        .last_terminal_voltage_valid = true,
        .last_terminal_trace_complete = true,
        .last_terminal_reason =
            BATTERY_CHARGE_TERMINAL_SOFTWARE_COULOMB_FULL,
        .last_terminal_generation = 9u,
        .last_terminal_elapsed_ms = 123456u,
        .last_terminal_net_input_nah = INT64_C(1000000000),
        .last_terminal_mv = 2860u,
        .last_curve_peak_mv = 2840u,
        .last_curve_drop_mv = 12u,
        .last_curve_slope_mv_per_min = -3,
    };
}

static void test_charge_supervisor_payload_and_persistence(void) {
    fresh_store();
    battery_charge_supervisor_persisted_t actual;
    assert_true(store_battery_charge_supervisor_get(&actual) ==
                    STORE_STATUS_OK &&
                    battery_charge_supervisor_persisted_valid(&actual) &&
                    !actual.active_session_valid,
                "charge supervisor starts detached");

    battery_charge_supervisor_persisted_t expected =
        charge_supervisor_fixture();
    assert_true(store_battery_charge_supervisor_set(&expected) ==
                    STORE_STATUS_OK,
                "charge supervisor record accepted");
    assert_true(store_battery_charge_supervisor_get(&actual) ==
                    STORE_STATUS_OK &&
                    memcmp(&actual, &expected, sizeof(actual)) == 0,
                "charge supervisor RAM record round-trips");

    uint8_t payload[128];
    size_t len = 0u;
    assert_true(!g_store_battery_charge_supervisor_unit_ops.serialize(
                    0u, payload, sizeof(payload) - 1u, &len),
                "charge supervisor serializer rejects short destination");
    assert_true(g_store_battery_charge_supervisor_unit_ops.serialize(
                    0u, payload, sizeof(payload), &len) &&
                    len == sizeof(payload),
                "charge supervisor serializer emits fixed wire record");
    for (size_t truncated = 0u; truncated < sizeof(payload); truncated++) {
        assert_true(!g_store_battery_charge_supervisor_unit_ops.apply(
                        0u, payload, truncated),
                    "charge supervisor parser rejects every truncation");
    }

    g_store_battery_charge_supervisor_unit_ops.reset_ram(0u);
    assert_true(g_store_battery_charge_supervisor_unit_ops.apply(
                    0u, payload, sizeof(payload)),
                "charge supervisor parser accepts exact record");
    assert_true(store_battery_charge_supervisor_get(&actual) ==
                    STORE_STATUS_OK &&
                    memcmp(&actual, &expected, sizeof(actual)) == 0,
                "charge supervisor wire record restores every field");

    uint8_t malformed[sizeof(payload)];
    memcpy(malformed, payload, sizeof(malformed));
    malformed[7] |= 0x80u;
    assert_true(!g_store_battery_charge_supervisor_unit_ops.apply(
                    0u, malformed, sizeof(malformed)),
                "charge supervisor parser rejects unknown flags");
    memcpy(malformed, payload, sizeof(malformed));
    malformed[6] &= (uint8_t)~(1u << 7);
    assert_true(!g_store_battery_charge_supervisor_unit_ops.apply(
                    0u, malformed, sizeof(malformed)),
                "terminal evidence flags require a terminal record");
    memcpy(malformed, payload, sizeof(malformed));
    put_u32_le(&malformed[108], 2u);
    assert_true(!g_store_battery_charge_supervisor_unit_ops.apply(
                    0u, malformed, sizeof(malformed)),
                "charge supervisor parser rejects invalid rearm guard");
    memcpy(malformed, payload, sizeof(malformed));
    malformed[112] = 1u;
    assert_true(!g_store_battery_charge_supervisor_unit_ops.apply(
                    0u, malformed, sizeof(malformed)),
                "charge supervisor parser rejects nonzero reserved data");
    memcpy(malformed, payload, sizeof(malformed));
    malformed[8] = 0u;
    assert_true(!g_store_battery_charge_supervisor_unit_ops.apply(
                    0u, malformed, sizeof(malformed)),
                "active charge record requires a hardware profile");
    assert_true(store_battery_charge_supervisor_get(&actual) ==
                    STORE_STATUS_OK &&
                    memcmp(&actual, &expected, sizeof(actual)) == 0,
                "failed charge payload apply is atomic");

    /* Deployed CGS3 records remain readable. A pending maintenance transaction
     * migrates as having consumed its one reset; other records default clear. */
    uint8_t legacy_v3[sizeof(payload)];
    memcpy(legacy_v3, payload, sizeof(legacy_v3));
    put_u32_le(&legacy_v3[0], UINT32_C(0x33534743));
    memset(&legacy_v3[108], 0, sizeof(legacy_v3) - 108u);
    assert_true(g_store_battery_charge_supervisor_unit_ops.apply(
                    0u, legacy_v3, sizeof(legacy_v3)),
                "charge supervisor parser accepts deployed CGS3 record");
    battery_charge_supervisor_persisted_t expected_v3 = expected;
    expected_v3.completion_rearm_used = false;
    assert_true(store_battery_charge_supervisor_get(&actual) ==
                    STORE_STATUS_OK &&
                    memcmp(&actual, &expected_v3, sizeof(actual)) == 0,
                "CGS3 migration defaults the completion rearm guard clear");

    uint8_t legacy_v3_pending[sizeof(payload)];
    memcpy(legacy_v3_pending, legacy_v3, sizeof(legacy_v3_pending));
    uint16_t v3_pending_flags = (uint16_t)(
        ((uint16_t)legacy_v3_pending[6] |
         ((uint16_t)legacy_v3_pending[7] << 8)) &
        (uint16_t)~((1u << 0) | (1u << 14)));
    v3_pending_flags |= (1u << 13);
    put_u16_le(&legacy_v3_pending[6], v3_pending_flags);
    legacy_v3_pending[83] = BATTERY_CHARGE_TERMINAL_MAINTENANCE_REARM;
    assert_true(g_store_battery_charge_supervisor_unit_ops.apply(
                    0u, legacy_v3_pending, sizeof(legacy_v3_pending)),
                "charge supervisor parser accepts pending CGS3 maintenance");
    assert_true(store_battery_charge_supervisor_get(&actual) ==
                    STORE_STATUS_OK && actual.maintenance_rearm_pending &&
                    actual.completion_rearm_used,
                "CGS3 pending maintenance migrates with loop guard consumed");

    /* Deployed CGS2 records remain readable. Pending maintenance defaults
     * false, while intrinsic terminal provenance recovers FULL authority. */
    uint8_t legacy_v2[sizeof(payload)];
    memcpy(legacy_v2, payload, sizeof(legacy_v2));
    put_u32_le(&legacy_v2[0], UINT32_C(0x32534743));
    uint16_t v2_flags = (uint16_t)(
        ((uint16_t)legacy_v2[6] | ((uint16_t)legacy_v2[7] << 8)) &
        UINT16_C(0x1fff));
    put_u16_le(&legacy_v2[6], v2_flags);
    memset(&legacy_v2[108], 0, sizeof(legacy_v2) - 108u);
    assert_true(g_store_battery_charge_supervisor_unit_ops.apply(
                    0u, legacy_v2, sizeof(legacy_v2)),
                "charge supervisor parser accepts deployed CGS2 record");
    battery_charge_supervisor_persisted_t expected_v2 = expected;
    expected_v2.maintenance_rearm_pending = false;
    expected_v2.completion_rearm_used = false;
    assert_true(store_battery_charge_supervisor_get(&actual) ==
                    STORE_STATUS_OK &&
                    memcmp(&actual, &expected_v2, sizeof(actual)) == 0,
                "CGS2 migration recovers intrinsic qualified FULL evidence");

    legacy_v2[11] = BATTERY_CHARGE_TERMINAL_BQ_ALREADY_FULL_AT_ATTACH;
    assert_true(g_store_battery_charge_supervisor_unit_ops.apply(
                    0u, legacy_v2, sizeof(legacy_v2)),
                "CGS2 migration accepts unqualified terminal provenance");
    battery_charge_supervisor_persisted_t expected_unqualified_v2 =
        expected_v2;
    expected_unqualified_v2.last_terminal_reason =
        BATTERY_CHARGE_TERMINAL_BQ_ALREADY_FULL_AT_ATTACH;
    expected_unqualified_v2.last_terminal_full_qualified = false;
    assert_true(store_battery_charge_supervisor_get(&actual) ==
                    STORE_STATUS_OK &&
                    memcmp(&actual, &expected_unqualified_v2,
                           sizeof(actual)) == 0,
                "CGS2 migration does not invent unqualified FULL evidence");

    legacy_v2[11] = BATTERY_CHARGE_TERMINAL_BQ_COMPLETE_AFTER_ACTIVE;
    assert_true(g_store_battery_charge_supervisor_unit_ops.apply(
                    0u, legacy_v2, sizeof(legacy_v2)),
                "CGS2 migration accepts legacy BQ completion provenance");
    expected_unqualified_v2.last_terminal_reason =
        BATTERY_CHARGE_TERMINAL_BQ_COMPLETE_AFTER_ACTIVE;
    assert_true(store_battery_charge_supervisor_get(&actual) ==
                    STORE_STATUS_OK &&
                    memcmp(&actual, &expected_unqualified_v2,
                           sizeof(actual)) == 0,
                "CGS2 BQ completion cannot invent coulomb corroboration");

    /* Deployed CGS1 records remain readable. V1 had only rounded-mAh
     * capacity/deficit fields and no policy, exact SOC, safety ceiling, or
     * durable inhibit latch. Migration must preserve that evidence strength
     * without silently enabling enforcement. */
    uint8_t legacy[96] = {0};
    put_u32_le(&legacy[0], UINT32_C(0x31534743));
    put_u16_le(&legacy[4], STORE_PAYLOAD_VERSION);
    put_u16_le(&legacy[6], UINT16_C(0x07ff));
    legacy[8] = 3u;
    legacy[9] = BATTERY_CHARGE_CHEMISTRY_NIMH_2S;
    legacy[10] = BATTERY_CAPACITY_CONFIDENCE_OBSERVED;
    legacy[11] = BATTERY_CHARGE_TERMINAL_BQ_COMPLETE_AFTER_ACTIVE;
    put_u32_le(&legacy[12], 7u);
    put_u32_le(&legacy[16], 4u);
    put_u32_le(&legacy[20], 9u);
    put_u32_le(&legacy[24], UINT32_C(0x81234567));
    put_u64_le(&legacy[28], (uint64_t)-INT64_C(100000000));
    put_u16_le(&legacy[36], 1000u);
    put_u16_le(&legacy[38], 200u);
    put_u16_le(&legacy[40], 180u);
    put_u16_le(&legacy[42], 800u);
    put_u16_le(&legacy[44], 1400u);
    put_u64_le(&legacy[48], UINT64_C(1120000000));
    put_u32_le(&legacy[56], 6u);
    put_u32_le(&legacy[60], 123000u);
    put_u64_le(&legacy[64], UINT64_C(1110000000));
    put_u16_le(&legacy[72], 2840u);
    put_u16_le(&legacy[74], 2830u);
    put_u16_le(&legacy[76], 8u);
    put_u16_le(&legacy[78], (uint16_t)-2);
    assert_true(g_store_battery_charge_supervisor_unit_ops.apply(
                    0u, legacy, sizeof(legacy)),
                "charge supervisor parser accepts deployed CGS1 record");
    assert_true(store_battery_charge_supervisor_get(&actual) ==
                    STORE_STATUS_OK &&
                    actual.active_session_valid &&
                    actual.frozen_remaining_nah == UINT64_C(200000000) &&
                    actual.deficit_nah == UINT64_C(800000000) &&
                    actual.safety_input_nah == UINT64_C(1750000000) &&
                    actual.frozen_soc_provenance ==
                        BATTERY_SOC_PROVENANCE_TRACKED &&
                    actual.frozen_soc_confidence ==
                        BATTERY_SOC_CONFIDENCE_ANCHORED &&
                    actual.configured_policy ==
                        BATTERY_CHARGE_POLICY_OBSERVE &&
                    actual.configured_charge_factor_permille == 0u &&
                    !actual.supervisor_inhibit_latched &&
                    !actual.completion_rearm_used,
                "CGS1 migration preserves evidence without enabling control");

    battery_charge_supervisor_persisted_t invalid = expected;
    invalid.target_valid = true;
    invalid.deficit_valid = false;
    assert_true(store_battery_charge_supervisor_set(&invalid) ==
                    STORE_STATUS_INVALID_ARGUMENT,
                "charge supervisor setter rejects inconsistent target");
    assert_true(store_battery_charge_supervisor_set(NULL) ==
                    STORE_STATUS_INVALID_ARGUMENT &&
                    store_battery_charge_supervisor_get(NULL) ==
                    STORE_STATUS_INVALID_ARGUMENT,
                "charge supervisor API rejects null arguments");

    assert_true(store_battery_charge_supervisor_set(&expected) ==
                    STORE_STATUS_OK,
                "charge supervisor restored before persistence test");
    flush_commits();
    assert_true(store_service_init() == STORE_STATUS_OK,
                "charge supervisor journal reload succeeds");
    assert_true(store_battery_charge_supervisor_get(&actual) ==
                    STORE_STATUS_OK &&
                    memcmp(&actual, &expected, sizeof(actual)) == 0,
                "charge supervisor survives restart");
}

static void populate_wire_fixture(void) {
    assert_true(store_setting_set_u8(STORE_SETTING_PHONEBOOK_VIEW_MODE, 2u) ==
                    STORE_STATUS_OK,
                "wire fixture phonebook setting");
    assert_true(store_phonebook_set_contact_tone_value(42u, 7u) ==
                    STORE_STATUS_OK,
                "wire fixture contact tone");
    assert_true(store_setting_set_u8(STORE_SETTING_SMS_VALIDITY, 3u) ==
                    STORE_STATUS_OK,
                "wire fixture SMS setting");
    assert_true(store_setting_set_text(STORE_SETTING_SMS_MESSAGE_CENTRE,
                                       "+15551234") == STORE_STATUS_OK,
                "wire fixture SMS text");
    assert_true(store_setting_set_u8(STORE_SETTING_CALL_OWN_NUMBER_SENDING, 1u) ==
                    STORE_STATUS_OK,
                "wire fixture call setting");
    assert_true(store_setting_set_u8(STORE_SETTING_PROFILE_ACTIVE, 3u) ==
                    STORE_STATUS_OK,
                "wire fixture profile setting");
    assert_true(store_setting_set_u8(STORE_SETTING_PROFILE_RINGING_VOLUME, 7u) ==
                    STORE_STATUS_OK,
                "wire fixture profile volume");
    assert_true(store_setting_set_u8(STORE_SETTING_CLOCK_FORMAT_24H, 0u) ==
                    STORE_STATUS_OK,
                "wire fixture clock format");
    assert_true(store_setting_set_u8(STORE_SETTING_CLOCK_ALARM_HOUR, 6u) ==
                    STORE_STATUS_OK,
                "wire fixture alarm hour");
    assert_true(store_setting_set_u8(STORE_SETTING_SYSTEM_LANGUAGE, 4u) ==
                    STORE_STATUS_OK,
                "wire fixture system language");
    assert_true(store_setting_set_text(STORE_SETTING_SYSTEM_WELCOME_NOTE,
                                       "Phase 5 wire") == STORE_STATUS_OK,
                "wire fixture welcome text");

    for (store_call_list_t list = 0; list < STORE_CALL_LIST_COUNT; list++) {
        store_call_record_t call;
        memset(&call, 0, sizeof(call));
        strcpy(call.number, "+15550001");
        call.number[8] = (char)('1' + list);
        strcpy(call.name, "Wire");
        call.duration_seconds = 61u + list;
        call.datetime.year = 2026u;
        call.datetime.month = 8u;
        call.datetime.day = 20u;
        call.datetime.hour = 14u;
        call.datetime.minute = 35u;
        call.datetime.second = 40u;
        call.reason = (store_call_reason_t)(STORE_CALL_REASON_MISSED + list);
        assert_true(store_call_add(list, &call, 0) == STORE_STATUS_OK,
                    "wire fixture call list");
    }

    char words[2][STORE_T9_WORD_MAX + 1u];
    memset(words, 0, sizeof(words));
    strcpy(words[0], "sisu");
    strcpy(words[1], "phasefive");
    assert_true(store_t9_user_words_save(words, 2u) == STORE_STATUS_OK,
                "wire fixture T9");

    for (uint8_t slot = 0u; slot < STORE_PICTURE_SLOT_COUNT; slot++) {
        assert_true(store_picture_message_clear(slot) == STORE_STATUS_OK,
                    "wire fixture clear seeded picture");
    }
    store_picture_message_t picture;
    memset(&picture, 0, sizeof(picture));
    picture.width = STORE_PICTURE_WIDTH;
    picture.height = STORE_PICTURE_HEIGHT;
    picture.bitmap_len = 8u;
    for (uint8_t i = 0u; i < picture.bitmap_len; i++) {
        picture.bitmap[i] = (uint8_t)(0xa0u + i);
    }
    strcpy(picture.text, "Wire picture");
    assert_true(store_picture_message_set(2u, &picture) == STORE_STATUS_OK,
                "wire fixture picture");

    store_own_tone_t tone;
    memset(&tone, 0, sizeof(tone));
    strcpy(tone.name, "WireTone");
    strcpy(tone.notes, "c1 d1 e1");
    tone.tempo_index = 4u;
    tone.packed_len = 5u;
    tone.packed[0] = 0x02u;
    tone.packed[1] = 0x4au;
    tone.packed[2] = 0x10u;
    tone.packed[3] = 0x20u;
    tone.packed[4] = 0x30u;
    assert_true(store_own_tone_set(0u, &tone) == STORE_STATUS_OK,
                "wire fixture own tone");

    store_call_divert_state_t divert;
    memset(&divert, 0, sizeof(divert));
    divert.active_mask = 0x15u;
    divert.delay_seconds = 25u;
    strcpy(divert.numbers[0], "+15550101");
    strcpy(divert.numbers[2], "+15550103");
    strcpy(divert.numbers[4], "+15550105");
    assert_true(store_call_divert_set(&divert) == STORE_STATUS_OK,
                "wire fixture divert");

    assert_true(store_board_imei_provision("490154203237518") == STORE_STATUS_OK,
                "wire fixture IMEI");
    assert_true(store_warranty_set_purchase_date("0826") == STORE_STATUS_OK,
                "wire fixture warranty");

    battery_learning_persisted_t learning;
    battery_learning_persisted_defaults(&learning, NULL);
    learning.capacity_history_mah[0] = 900u;
    learning.capacity_history_mah[1] = 1000u;
    learning.capacity_history_mah[2] = 1100u;
    learning.capacity_history_count = 3u;
    learning.capacity_history_next = 2u;
    learning.accepted_capacity_cycles = 5u;
    learning.rejected_capacity_cycles = 2u;
    learning.last_capacity_mah = 1100u;
    learning.resistance_mohm[0] = 180u;
    learning.resistance_mohm[1] = 220u;
    learning.resistance_mohm[2] = 300u;
    learning.resistance_sample_count[0] = 11u;
    learning.resistance_sample_count[1] = 22u;
    learning.resistance_sample_count[2] = 33u;
    learning.pack_generation = 7u;
    learning.full_anchor_acr_raw = UINT32_C(0x89abcdef);
    learning.full_anchor_nah = -INT64_C(123456789);
    learning.cycle_min_temperature_mdegc = 12000;
    learning.cycle_max_temperature_mdegc = 33000;
    learning.full_anchor_valid = true;
    learning.capacity_cycle_qualified = true;
    assert_true(store_battery_learning_set(&learning) == STORE_STATUS_OK,
                "wire fixture battery learning");
    battery_charge_supervisor_persisted_t supervisor =
        charge_supervisor_fixture();
    assert_true(store_battery_charge_supervisor_set(&supervisor) ==
                    STORE_STATUS_OK,
                "wire fixture charge supervisor");
}

static void test_persistent_wire_contract(void) {
    static uint8_t payload[STORAGE_RECORD_MAX_PAYLOAD];
    fresh_store();
    populate_wire_fixture();
    flush_commits();

    store_diag_snapshot_t diag;
    store_service_get_diag(&diag);
    assert_eq_u32(0u, diag.dirty_mask, "wire fixture fully committed");
    for (store_unit_t unit = 0; unit < STORE_UNIT_COUNT; unit++) {
        size_t len = 0u;
        char length_message[64];
        char hash_message[64];
        snprintf(length_message, sizeof(length_message),
                 "wire unit %u payload length", (unsigned)unit);
        snprintf(hash_message, sizeof(hash_message),
                 "wire unit %u payload bytes", (unsigned)unit);
        assert_true(read_unit_payload(unit, payload, sizeof(payload), &len),
                    "wire fixture journal readable");
        if (unit >= STORE_UNIT_CALLS_MISSED &&
            unit <= STORE_UNIT_CALLS_DIALLED) {
            assert_true(payload[4] == 2u && payload[5] == 0u,
                        "call wire payload uses schema version 2");
            assert_eq_u32(2026u - 1999u, payload[12u + 15u],
                          "call wire payload carries full-year code");
        }
        assert_eq_u32(WIRE_EXPECTED[unit].length, (uint32_t)len,
                      length_message);
        assert_eq_u64(WIRE_EXPECTED[unit].fnv64, fnv1a64(payload, len),
                      hash_message);
    }
}

static void test_default_initialization_power_cuts(void) {
    static uint8_t snapshot[FAKE_NVM_CAPACITY];
    static uint8_t expected[STORE_UNIT_COUNT][STORAGE_RECORD_MAX_PAYLOAD];
    static size_t lengths[STORE_UNIT_COUNT];
    uint8_t payload[STORAGE_RECORD_MAX_PAYLOAD];
    storage_lfs_deinit();
    memset(s_fake_nvm, 0xff, sizeof(s_fake_nvm));
    /* Disconnected old journal bytes must not be read or modified. */
    memset(s_fake_nvm, 0xa5, FAKE_NVM_CAPACITY / 2u);
    storage_backend_t backend;
    assert(storage_backend_open(&backend) == STORAGE_RECORD_OK);
    memcpy(snapshot, s_fake_nvm, sizeof(snapshot));
    s_media_ops = 0;
    assert(store_service_init() == STORE_STATUS_OK);
    unsigned operation_count = s_media_ops;
    assert(operation_count > 0);
    for (store_unit_t unit = 0; unit < STORE_UNIT_COUNT; unit++) {
        assert(read_unit_payload(unit, expected[unit], sizeof(expected[unit]), &lengths[unit]));
    }
    for (unsigned point = 1; point <= operation_count; point++) {
        for (unsigned mode = 0; mode < 3; mode++) {
            storage_lfs_deinit();
            memcpy(s_fake_nvm, snapshot, sizeof(snapshot));
            s_media_ops = 0;
            s_cut_at = point;
            s_cut_mode = mode;
            if (setjmp(s_power_cut) == 0) {
                (void)store_service_init();
                assert(!"default initialization cut must execute");
            }
            s_cut_at = 0;
            assert(store_service_init() == STORE_STATUS_OK);
            for (store_unit_t unit = 0; unit < STORE_UNIT_COUNT; unit++) {
                size_t len;
                assert(read_unit_payload(unit, payload, sizeof(payload), &len));
                assert(len == lengths[unit] && memcmp(payload, expected[unit], len) == 0);
            }
            assert(memcmp(s_fake_nvm, snapshot, FAKE_NVM_CAPACITY / 2u) == 0);
        }
    }
    printf("default initialization: %u operation boundaries x 3 torn-write modes passed\\n",
           operation_count);
}

static void test_corrupt_picture_payload_uses_seeded_fallback(void) {
    static const uint8_t corrupt_payload[] = {0x50u, 0x49u, 0x43u};
    fresh_store();
    assert_true(write_unit_fixture(STORE_UNIT_PICTURE_MESSAGES,
                                   corrupt_payload, sizeof(corrupt_payload)),
                "write corrupt picture journal");
    assert_true(store_service_init() == STORE_STATUS_OK,
                "re-init with corrupt picture payload");
    assert_eq_u32(4u, store_picture_message_count(),
                  "corrupt picture payload seeds built-in slots");
}

static void test_corrupt_settings_payload_is_atomic(void) {
    uint8_t sms_payload[] = {
        0x31u, 0x54u, 0x45u, 0x53u, /* SETTINGS_MAGIC */
        0x01u, 0x00u,               /* version */
        0x01u, 0x02u,               /* SMS domain, two records */
        (uint8_t)STORE_SETTING_SMS_DEFAULT_PROFILE, 0x00u,
        0x01u, 0x01u, 0x09u,        /* U8 value 9; second record absent */
    };
    fresh_store();
    assert_true(write_unit_fixture(STORE_UNIT_SETTINGS_SMS,
                                   sms_payload, sizeof(sms_payload)),
                "write truncated SMS settings journal");
    assert_true(store_service_init() == STORE_STATUS_OK,
                "re-init with truncated SMS settings payload");
    uint8_t profile = 0xffu;
    assert_true(store_setting_get_u8(STORE_SETTING_SMS_DEFAULT_PROFILE,
                                     &profile) == STORE_STATUS_OK,
                "read SMS profile after corrupt payload");
    assert_eq_u32(0u, profile,
                  "corrupt SMS payload cannot publish a valid prefix");

    uint8_t phonebook_payload[] = {
        0x31u, 0x54u, 0x45u, 0x53u, /* SETTINGS_MAGIC */
        0x01u, 0x00u,               /* version */
        0x00u, 0x00u,               /* phonebook domain, zero settings */
        0x4fu, 0x54u, 0x02u,        /* tone block, two records */
        0x2au, 0x00u, 0x01u, 0x07u, /* contact 42 -> tone 7; second absent */
    };
    fresh_store();
    assert_true(write_unit_fixture(STORE_UNIT_SETTINGS_PHONEBOOK,
                                   phonebook_payload,
                                   sizeof(phonebook_payload)),
                "write truncated phonebook tone journal");
    assert_true(store_service_init() == STORE_STATUS_OK,
                "re-init with truncated phonebook tone payload");
    assert_eq_u32(STORE_CONTACT_TONE_PRESET,
                  store_phonebook_get_contact_tone_value(42u),
                  "corrupt tone block cannot publish a valid prefix");
}

/* --------------------------------------------------------------------------
 * No-backend behavior: init reports storage error, accessors still safe.
 * ------------------------------------------------------------------------ */

static void test_no_backend(void) {
    memset(s_fake_nvm, 0xff, sizeof(s_fake_nvm));
    s_nvm_present = false;
    store_status_t st = store_service_init();
    assert_true(st == STORE_STATUS_STORAGE_ERROR, "init without backend -> STORAGE_ERROR");
    assert_true(!store_service_ready(), "not ready without backend");
    assert_true(store_service_standby_ready(),
                "absent backend cannot pin powered-on standby awake");
    /* Defaults still served from RAM. */
    uint8_t u8;
    assert_true(store_setting_get_u8(STORE_SETTING_SMS_VALIDITY, &u8) == STORE_STATUS_OK && u8 == 5u,
                "defaults available without backend");
    /* set returns NOT_READY (still updates RAM). */
    assert_true(store_setting_set_u8(STORE_SETTING_SMS_VALIDITY, 1u) == STORE_STATUS_NOT_READY,
                "set without backend -> NOT_READY");
    assert_true(store_setting_get_u8(STORE_SETTING_SMS_VALIDITY, &u8) == STORE_STATUS_OK && u8 == 1u,
                "RAM updated even when not ready");
    assert_eq_u32(0u, store_picture_message_count(),
                  "no-backend init does not seed persistent picture defaults");
    s_nvm_present = true;
}

/* A degraded journal sector (persistent verify-mismatch) must neither STARVE the
 * healthy higher-index units nor erase-cycle itself unboundedly. Poison unit 6
 * (CALLS_MISSED) and confirm unit 7 (CALLS_RECEIVED) still persists, while unit
 * 6's commit attempts are capped (mirrors STORE_COMMIT_FAIL_LIMIT = 8). */
static void test_commit_failure_cap_and_no_starvation(void) {
    fresh_store();
    uint32_t region = 2u * (uint32_t)TEST_LOGICAL_SLOT_SIZE; /* per-unit journal size */
    s_poison_lo = 6u * region;   /* unit 6 = STORE_UNIT_CALLS_MISSED */
    s_poison_hi = 7u * region;
    s_poison_erases = 0u;
    s_poison_writes = 0u;

    uint32_t id = 0u;
    store_call_add_now(STORE_CALL_LIST_MISSED, "+1000", "Miss", 0u, STORE_CALL_REASON_MISSED, &id);
    store_call_add_now(STORE_CALL_LIST_RECEIVED, "+2000", "Recv", 7u, STORE_CALL_REASON_RECEIVED, &id);
    assert_true(!store_service_standby_ready(),
                "uncapped dirty call units block standby");

    flush_commits(); /* ~100 paced ticks */

    /* Cap: the poisoned unit is attempted a bounded number of times, not once
     * per tick forever (endurance protection). */
    assert_true(s_poison_erases > 0u, "poisoned unit was attempted at least once");
    assert_true(s_poison_erases <= 8u, "poisoned unit erase-cycling is capped (<= FAIL_LIMIT)");
    assert_true(store_service_standby_ready(),
                "only a capped degraded unit may remain dirty at standby");

    /* flush_all (the power-off path, retried ~1/s by power_sleep in soft-off)
     * must ALSO respect the cap: a capped unit is skipped, so flush_all returns
     * true (never blocks dormant entry forever) and does NOT re-erase the dead
     * sector. */
    uint32_t erases_after_ticks = s_poison_erases;
    assert_true(store_service_flush_all(), "flush_all does not block on a capped unit");
    assert_true(store_service_flush_all(), "repeated flush_all stays unblocked");
    assert_true(s_poison_erases == erases_after_ticks, "flush_all does not re-erase a capped unit");

    /* No starvation: unit 7 committed despite unit 6 failing first in the scan.
     * Un-poison and reload from flash to confirm. */
    s_poison_lo = s_poison_hi = 0u;
    assert_true(store_service_init() == STORE_STATUS_OK, "re-init after poison");
    store_call_record_t got;
    assert_true(store_call_get(STORE_CALL_LIST_RECEIVED, 0u, &got) == STORE_STATUS_OK,
                "higher unit persisted -- not starved by the failing lower unit");
    assert_true(store_call_get(STORE_CALL_LIST_MISSED, 0u, &got) == STORE_STATUS_NOT_FOUND,
                "poisoned unit did not persist (verify mismatch)");
}

/* The endurance cap must survive RE-MUTATION: a frequently updated call-log unit is
 * re-dirtied on every change, and if that reset the cap the degraded sector would
 * erase-cycle without bound. Prove the cap holds across many re-dirties, and that
 * the slow park re-arm grants only a single bounded retry per interval. */
static void test_commit_cap_survives_remutation(void) {
    /* Mirror of the private engine constants in store_service.c (the test links
     * the translation unit separately, so the macros are not visible here --
     * kept in sync by hand, like the "8u"
     * literal in the cap test above). */
    const uint32_t FAIL_LIMIT = 8u;
    const uint32_t PARK_REARM_MS = 300000u;

    fresh_store();
    uint32_t region = 2u * (uint32_t)TEST_LOGICAL_SLOT_SIZE;
    s_poison_lo = 6u * region;   /* unit 6 = STORE_UNIT_CALLS_MISSED */
    s_poison_hi = 7u * region;
    s_poison_erases = 0u;
    s_poison_writes = 0u;

    uint32_t id = 0u;
    uint32_t t = 0u;

    /* Cap the poisoned unit (FAIL_LIMIT consecutive commit failures). */
    store_call_add_now(STORE_CALL_LIST_MISSED, "+1000", "Miss", 0u, STORE_CALL_REASON_MISSED, &id);
    for (uint32_t k = 0; k < 400u; k++, t += 40u) {
        store_service_tick(t);
    }
    assert_true(s_poison_erases == FAIL_LIMIT,
                "poisoned unit capped at FAIL_LIMIT erases");
    uint32_t capped = s_poison_erases;

    /* Re-dirty the capped unit repeatedly, staying inside the park re-arm window.
     * The cap MUST hold -- the OLD un-park-on-mutation behaviour would add another
     * FAIL_LIMIT erases per re-dirty here. */
    for (uint32_t m = 0; m < 20u && t < PARK_REARM_MS - 40000u; m++) {
        store_call_add_now(STORE_CALL_LIST_MISSED, "+2000", "Miss", 0u, STORE_CALL_REASON_MISSED, &id);
        for (uint32_t k = 0; k < 4u; k++, t += 40u) {
            store_service_tick(t);
        }
    }
    assert_true(s_poison_erases == capped,
                "re-dirtying a capped unit does NOT reset the endurance cap");

    /* After the re-arm interval the parked unit gets exactly ONE retry -> one more
     * erase -> re-parks. Bounded (a trickle), not once-per-mutation. */
    t = PARK_REARM_MS + 40u;
    for (uint32_t k = 0; k < 10u; k++, t += 40u) {
        store_service_tick(t);
    }
    assert_true(s_poison_erases == capped + 1u,
                "park re-arm grants exactly one bounded retry per interval");

    s_poison_lo = s_poison_hi = 0u;
    assert_true(store_service_init() == STORE_STATUS_OK, "re-init after remutation test");
}

/* Stronger form of the finding: mutate the degraded unit BEFORE EVERY commit
 * attempt (during the 1..LIMIT climb, not only after the cap). The old
 * reset-on-mutation code reset the count to 0 on each mutation, so the cap never
 * engaged and the dead sector erase-cycled forever; the fixed code counts truly
 * consecutive failures, so the cap still lands at exactly FAIL_LIMIT. */
static void test_commit_cap_survives_interleaved_mutation(void) {
    fresh_store();
    uint32_t region = 2u * (uint32_t)TEST_LOGICAL_SLOT_SIZE;
    s_poison_lo = 6u * region;   /* unit 6 = STORE_UNIT_CALLS_MISSED */
    s_poison_hi = 7u * region;
    s_poison_erases = 0u;
    s_poison_writes = 0u;

    uint32_t id = 0u;
    uint32_t t = 0u;
    /* Re-dirty the poisoned unit before every single paced commit tick, staying
     * inside the park re-arm window (300000 ms). */
    for (uint32_t k = 0; k < 300u && t < 100000u; k++, t += 40u) {
        store_call_add_now(STORE_CALL_LIST_MISSED, "+9", "M", 0u, STORE_CALL_REASON_MISSED, &id);
        store_service_tick(t);
    }
    /* The cap still engages: exactly FAIL_LIMIT (8) erases, then parked -- not the
     * unbounded ~31/s the old reset-on-mutation code would have produced. */
    assert_true(s_poison_erases >= 8u, "cap engaged despite mutation before every commit");
    assert_true(s_poison_erases <= 8u, "interleaved mutation does not let erases exceed the cap");

    s_poison_lo = s_poison_hi = 0u;
    assert_true(store_service_init() == STORE_STATUS_OK, "re-init after interleaved test");
}

/* A transient core1-park timeout (HAL returns NVM_STATUS_BUSY) is retryable, NOT a
 * degraded sector: it must NOT count toward the per-unit failure cap. Otherwise
 * a sustained core1 wedge would park a single dirty unit at the cap and freeze
 * the global 20-park reboot streak below its threshold, silently losing the
 * unit's newest delta at power-off. */
static void test_park_busy_not_counted_as_degraded(void) {
    fresh_store();
    uint32_t region = 2u * (uint32_t)TEST_LOGICAL_SLOT_SIZE;
    s_poison_lo = 6u * region;   /* unit 6 */
    s_poison_hi = 7u * region;
    s_poison_erases = 0u;
    s_fake_op_busy = true;       /* HAL says: the op could not run (core1 busy) */

    uint32_t id = 0u;
    store_call_add_now(STORE_CALL_LIST_MISSED, "+1", "M", 0u, STORE_CALL_REASON_MISSED, &id);
    for (uint32_t t = 0; t < 8000u; t += 40u) {
        store_service_tick(t);
    }
    /* Uncapped: the unit keeps reaching the backend well past FAIL_LIMIT rather
     * than parking at 8. No erase executes while the backend reports busy. */
    assert_true(s_fake_busy_ops > 8u,
                "park-busy failures are retried, not capped as degraded media");
    assert_eq_u32(0u, s_poison_erases,
                  "park-busy backend never executes the erase");

    s_fake_op_busy = false;
    s_poison_lo = s_poison_hi = 0u;
    assert_true(store_service_init() == STORE_STATUS_OK, "re-init after park-busy test");
}

static bool fail_before_hal_serialize(uint8_t instance,
                                      uint8_t *dst,
                                      size_t cap,
                                      size_t *out_len) {
    (void)instance;
    (void)dst;
    (void)cap;
    if (out_len != 0) {
        *out_len = 0u;
    }
    return false;
}

static void test_serializer_failure_is_not_stale_busy(void) {
    static const store_unit_ops_t FAILING_OPS = {
        .reset_ram = 0,
        .serialize = fail_before_hal_serialize,
        .apply = 0,
        .fallback_missing_or_corrupt = 0,
        .name = "failing test unit",
    };
    static const store_unit_binding_t FAILING_BINDING = {
        .ops = &FAILING_OPS,
        .instance = 0u,
    };

    fresh_store();
    s_fake_op_busy = true;
    uint8_t marker = 0x5au;
    assert_true(s_real_backend.write(&s_real_backend, 0x321du, &marker, 1u) ==
                    STORAGE_RECORD_BUSY,
                "preceding littlefs attempt records backend busy");
    uint32_t busy_ops_before = s_fake_busy_ops;

    uint8_t payload[16];
    store_commit_result_t result = store_engine_commit_binding(
        &FAILING_BINDING, &s_real_backend, 0x321du, payload, sizeof(payload));
    assert_true(result.status == STORE_STATUS_STORAGE_ERROR,
                "serializer failure reports storage error");
    assert_true(!result.flash_busy,
                "serializer failure cannot inherit prior backend busy origin");
    assert_eq_u32(busy_ops_before, s_fake_busy_ops,
                  "serializer failure stops before the HAL");

    s_fake_op_busy = false;
    assert_true(store_service_init() == STORE_STATUS_OK,
                "re-init after serializer-failure test");
}

static void picture_part(sms_codec_message_t *part, const uint8_t *payload,
                         uint16_t len, uint8_t seq, uint16_t ref) {
    memset(part, 0, sizeof(*part));
    part->binary = part->has_ports = part->has_concat = true;
    part->dcs = 4u;
    part->dest_port = SMS_CODEC_PICTURE_PORT;
    part->source_port = 0u;
    part->concat_ref = ref;
    part->concat_total = (uint8_t)((len + 127u) / 128u);
    part->concat_seq = seq;
    strcpy(part->address, "+12025550123");
    strcpy(part->timestamp, "26/09/18,12:00:00");
    uint16_t offset = (uint16_t)(seq - 1u) * 128u;
    part->binary_len = len - offset > 128u ? 128u : len - offset;
    memcpy(part->binary_data, payload + offset, part->binary_len);
}

static void test_picture_receive_journal(void) {
    fresh_store();
    store_picture_message_t source, received;
    assert_true(store_picture_message_get(0u, &source) == STORE_STATUS_OK, "receive fixture template");
    uint8_t payload[MODEM_SMS_BINARY_MAX], chunks = 0u;
    uint16_t len = 0u;
    assert_true(sms_picture_payload_encode(&source, "Received picture", payload, sizeof(payload), &len, &chunks),
                "encode receive fixture");
    assert_eq_u32(3u, chunks, "fixture has three parts");
    sms_codec_message_t part;
    picture_part(&part, payload, len, 3u, 17u);
    assert_true(store_picture_receive(&part, 100u) == STORE_STATUS_OK, "out of order final part staged");
    picture_part(&part, payload, len, 1u, 17u);
    assert_true(store_picture_receive(&part, 101u) == STORE_STATUS_OK, "first part staged");
    flush_commits();
    assert_true(store_service_init() == STORE_STATUS_OK, "restart during assembly");
    assert_eq_u32(0u, store_picture_pending_first(), "incomplete picture is not announced");
    picture_part(&part, payload, len, 2u, 17u);
    assert_true(store_picture_receive(&part, 102u) == STORE_STATUS_OK, "assembly resumes after restart");
    assert_eq_u32(0u, store_picture_pending_first(), "RAM completion is not durable publication");
    flush_commits();
    uint32_t id = store_picture_pending_first();
    assert_true(id != 0u && store_picture_pending_get(id, &received, NULL, 0u) == STORE_STATUS_OK &&
                strcmp(received.text, "Received picture") == 0 &&
                memcmp(received.bitmap, source.bitmap, sizeof(source.bitmap)) == 0,
                "complete picture and caption survive ordered assembly");
    assert_true(store_picture_receive(&part, 103u) == STORE_STATUS_OK &&
                store_picture_commit_status() == STORE_STATUS_OK,
                "duplicate part is idempotent without flash wear");
    part.binary_data[0] ^= 1u;
    assert_true(store_picture_receive(&part, 104u) == STORE_STATUS_CONFLICT &&
                store_picture_pending_first() == id, "conflicting duplicate cannot destroy complete picture");
    for (uint8_t seq = 1u; seq <= chunks; seq++) {
        picture_part(&part, payload, len, seq, 18u);
        assert_true(store_picture_receive(&part, 110u + seq) == STORE_STATUS_OK, "second pending picture");
    }
    flush_commits();
    picture_part(&part, payload, len, 1u, 19u);
    assert_true(store_picture_receive(&part, 120u) == STORE_STATUS_STORAGE_ERROR &&
                store_picture_pending_first() == id, "full receive queue never evicts a pending picture");
    assert_true(store_picture_pending_save(id, 6u) == STORE_STATUS_OK, "atomic save requested");
    s_fake_op_busy = true;
    flush_commits();
    assert_true(store_picture_commit_status() == STORE_STATUS_NOT_READY, "busy flash is not a successful save");
    s_fake_op_busy = false;
    assert_true(store_service_init() == STORE_STATUS_OK && store_picture_pending_first() == id &&
                store_picture_message_get(6u, &received) == STORE_STATUS_NOT_FOUND,
                "power loss before save commit retains pending picture and old gallery");
    assert_true(store_picture_pending_save(id, 6u) == STORE_STATUS_OK, "retry atomic save");
    flush_commits();
    assert_true(store_service_init() == STORE_STATUS_OK &&
                store_picture_message_get(6u, &received) == STORE_STATUS_OK &&
                strcmp(received.text, "Received picture") == 0,
                "saved picture durable in expanded gallery");
    char sender[33];
    assert_true(store_picture_message_sender(6u, sender, sizeof(sender)) == STORE_STATUS_OK &&
                strcmp(sender, "+12025550123") == 0, "saved sender retained");
    assert_true(store_picture_message_set_text(6u, "Edited caption") == STORE_STATUS_OK,
                "caption edit accepted");
    flush_commits();
    assert_true(store_service_init() == STORE_STATUS_OK &&
                store_picture_message_sender(6u, sender, sizeof(sender)) == STORE_STATUS_OK &&
                strcmp(sender, "+12025550123") == 0, "caption edit preserves sender across reboot");
    assert_true(store_picture_message_set(6u, &source) == STORE_STATUS_OK &&
                store_picture_message_sender(6u, sender, sizeof(sender)) == STORE_STATUS_OK &&
                sender[0] == '\0', "replacing a bitmap does not inherit the previous sender");
    flush_commits();
    uint32_t second = store_picture_pending_first();
    assert_true(second != 0u && second != id, "next reception remains pending after atomic save");
    picture_part(&part, payload, len, 2u, 17u);
    assert_true(store_picture_receive(&part, 130u) == STORE_STATUS_OK &&
                store_picture_pending_first() == second, "consumed duplicate does not resurrect saved picture");
    assert_true(store_picture_pending_discard(second) == STORE_STATUS_OK, "discard requested");
    flush_commits();
    assert_true(store_service_init() == STORE_STATUS_OK && store_picture_pending_first() == 0u,
                "discard persists across reboot");
    picture_part(&part, payload, len, 1u, 22u);
    assert_true(store_picture_receive(&part, 140u) == STORE_STATUS_OK, "consumed queue slot reusable");
    part.binary_data[0] ^= 1u;
    assert_true(store_picture_receive(&part, 141u) == STORE_STATUS_CONFLICT, "incomplete conflict quarantined");
    picture_part(&part, payload, len, 2u, 22u);
    assert_true(store_picture_receive(&part, 142u) == STORE_STATUS_CONFLICT, "conflicting assembly cannot complete");
    picture_part(&part, payload, len, 1u, 22u);
    assert_true(store_picture_receive(&part, 1800200u) == STORE_STATUS_OK, "expired incomplete assembly can restart");
}

static void test_picture_reference_reuse_after_reboot(void) {
    fresh_store();
    store_picture_message_t source;
    assert_true(store_picture_message_get(0u, &source) == STORE_STATUS_OK, "reference fixture");
    uint8_t payload[MODEM_SMS_BINARY_MAX], chunks = 0u;
    uint16_t len = 0u;
    assert_true(sms_picture_payload_encode(&source, "Same picture", payload,
                                         sizeof(payload), &len, &chunks), "reference payload");
    sms_codec_message_t part;
    for (uint8_t seq = 1u; seq <= chunks; seq++) {
        picture_part(&part, payload, len, seq, 1u);
        part.timestamp[16] = (char)('0' + seq);
        assert_true(store_picture_receive(&part, seq) == STORE_STATUS_OK,
                    "parts can have adjacent SMSC seconds");
    }
    flush_commits();
    uint32_t old = store_picture_pending_first();
    assert_true(old != 0u && store_picture_pending_save(old, 6u) == STORE_STATUS_OK,
                "save original reference");
    flush_commits();
    assert_true(store_service_init() == STORE_STATUS_OK, "reboot with saved duplicate marker");
    picture_part(&part, payload, len, 2u, 1u);
    part.timestamp[16] = '2';
    assert_true(store_picture_receive(&part, 10u) == STORE_STATUS_OK &&
                store_picture_commit_status() == STORE_STATUS_OK && store_picture_pending_first() == 0u,
                "true old duplicate remains suppressed after reboot without flash wear");
    for (uint8_t seq = chunks; seq > 0u; seq--) {
        picture_part(&part, payload, len, seq, 1u);
        strcpy(part.timestamp, "26/09/18,12:31:00");
        part.timestamp[16] = (char)('0' + seq);
        assert_true(store_picture_receive(&part, 20u + chunks - seq) == STORE_STATUS_OK,
                    "later reference reuse accepted despite fresh uptime and identical bytes");
    }
    flush_commits();
    uint32_t current = store_picture_pending_first();
    assert_true(current != 0u && current != old,
                "later identical picture is a new durable reception, not a day-wide duplicate");
}

static void test_picture_legacy_migration(void) {
    uint8_t legacy[1524] = {0};
    put_u32_le(legacy, 0x50494331u);
    put_u16_le(legacy + 4u, 1u);
    legacy[6] = 1u;
    legacy[8] = 1u; legacy[9] = 72u; legacy[10] = 28u;
    put_u16_le(legacy + 11u, 252u);
    legacy[13] = 6u;
    legacy[14] = 0xa5u;
    memcpy(legacy + 266u, "Legacy", 6u);
    fresh_store();
    assert_true(write_unit_fixture(STORE_UNIT_PICTURE_MESSAGES, legacy, sizeof(legacy)), "write legacy four-slot fixture");
    assert_true(store_service_init() == STORE_STATUS_OK, "load old picture schema");
    store_picture_message_t picture;
    assert_true(store_picture_message_get(0u, &picture) == STORE_STATUS_OK &&
                picture.bitmap[0] == 0xa5u && strcmp(picture.text, "Legacy") == 0,
                "migration preserves old bitmap and caption");
    assert_eq_u32(1u, store_picture_message_count(), "migration does not reseed erased slots");
    assert_true(store_picture_message_get(6u, &picture) == STORE_STATUS_NOT_FOUND,
                "expanded slots start empty");
    assert_true(store_picture_message_set(6u, &picture) == STORE_STATUS_OK, "new schema mutation");
    flush_commits();
    assert_true(store_service_init() == STORE_STATUS_OK &&
                store_picture_message_get(0u, &picture) == STORE_STATUS_OK && strcmp(picture.text, "Legacy") == 0,
                "migration remains readable after new-format commit");
}

int main(void) {
    test_default_initialization_power_cuts();
    log_set_level(0);
    test_unit_handler_registry();
    test_exact_dirty_unit_mapping();
    test_persistent_wire_contract();
    test_battery_learning_payload_and_persistence();
    test_charge_supervisor_payload_and_persistence();
    test_corrupt_picture_payload_uses_seeded_fallback();
    test_corrupt_settings_payload_is_atomic();
    test_standby_readiness_tracks_persistence();
    test_commit_failure_cap_and_no_starvation();
    test_commit_cap_survives_remutation();
    test_commit_cap_survives_interleaved_mutation();
    test_park_busy_not_counted_as_degraded();
    test_serializer_failure_is_not_stale_busy();
    test_every_key_has_meta();
    test_defaults();
    test_type_mismatch();
    test_setget_round_trip();
    test_text_clamp();
    test_persistence();
    test_call_list_ring();
    test_call_list_delete_clear();
    test_call_add_now_null();
    test_call_record_truncation();
    test_call_list_persistence();
    test_call_datetime_full_rtc_range_persists();
    test_call_datetime_v1_payload_migrates();
    test_speed_dial();
    test_contact_tone();
    test_contact_tone_persistence();
    test_t9_user_words();
    test_picture_messages();
    test_picture_receive_journal();
    test_picture_reference_reuse_after_reboot();
    test_picture_legacy_migration();
    test_own_tones();
    test_call_divert();
    test_warranty_payload_apply_is_atomic();
    test_warranty();
    test_no_backend();

    if (s_failures != 0) {
        fprintf(stderr, "%d failures\n", s_failures);
        return 1;
    }
    printf("store_service tests passed\n");
    return 0;
}

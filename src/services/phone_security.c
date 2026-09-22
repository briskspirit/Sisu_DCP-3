#include "services/phone_security.h"
#include "storage/store_phone_security.h"

#include <string.h>

/* ROM 0x2ae6b0: five failures, timer 0x32 restarted with 0x969a ticks. */
#define CODE_QUIET_MS (0x969au * 8u)
#define CALLS_MASK ((UINT32_C(1) << STORE_UNIT_CALLS_MISSED) | \
                   (UINT32_C(1) << STORE_UNIT_CALLS_RECEIVED) | \
                   (UINT32_C(1) << STORE_UNIT_CALLS_DIALLED))

static phone_security_record_t s_record;
static bool s_ready, s_authorized, s_quiet, s_code_grant;
static uint32_t s_quiet_at, s_retry_at;
static uint32_t s_generation;
static char s_identity[PHONE_SECURITY_SIM_CAP];

static bool save(const phone_security_record_t *record) {
    if (!store_phone_security_write(record)) { s_ready = false; return false; }
    (void)store_service_flush_all();
    store_diag_snapshot_t diag;
    store_service_get_diag(&diag);
    if (!diag.ready || diag.boot_faults != 0u ||
        ((diag.dirty_mask | diag.degraded_mask | diag.unavailable_mask) &
         (UINT32_C(1) << STORE_UNIT_PHONE_SECURITY)) != 0u) {
        s_ready = false;
        return false;
    }
    s_record = *record;
    s_ready = true;
    return true;
}

static bool finish_clear(void) {
    if (!s_record.clear_calls) return true;
    store_diag_snapshot_t diag;
    store_service_get_diag(&diag);
    if (!diag.ready || ((diag.unavailable_mask | diag.degraded_mask) & CALLS_MASK)) return false;
    for (store_call_list_t list = 0u; list < STORE_CALL_LIST_COUNT; list++)
        if (store_call_clear(list) != STORE_STATUS_OK) return false;
    (void)store_service_flush_all();
    store_service_get_diag(&diag);
    if ((diag.dirty_mask | diag.unavailable_mask | diag.degraded_mask) & CALLS_MASK) return false;
    phone_security_record_t next = s_record;
    next.clear_calls = false;
    return save(&next);
}

void phone_security_init(uint32_t now) {
    if (++s_generation == 0u) ++s_generation;
    memset(&s_record, 0, sizeof(s_record));
    memset(s_identity, 0, sizeof(s_identity));
    s_ready = store_phone_security_read(&s_record);
    s_authorized = s_code_grant = false;
    s_quiet = s_ready && s_record.failures != 0u;
    s_quiet_at = now;
    s_retry_at = now - 1000u;
}

static bool known(void) {
    if (s_identity[0] == '\0') return false;
    for (unsigned i = 0u; i < PHONE_SECURITY_SIM_COUNT; i++)
        if (strcmp(s_identity, s_record.sims[i]) == 0) return true;
    return false;
}

static bool admit(void) {
    if (known()) { s_authorized = true; return true; }
    phone_security_record_t next = s_record;
    /* The ROM inserts an unfamiliar identity at the front, but leaves a
     * recognized identity in place (0x27c504/0x27c434). */
    next.clear_calls = next.sims[0][0] != '\0';
    memmove(next.sims[1], next.sims[0], (PHONE_SECURITY_SIM_COUNT - 1u) * PHONE_SECURITY_SIM_CAP);
    memcpy(next.sims[0], s_identity, PHONE_SECURITY_SIM_CAP);
    if (!save(&next) || !finish_clear()) return false;
    s_authorized = true;
    return true;
}

void phone_security_tick(const char *identity, uint32_t now) {
    if (!phone_security_identity_valid(identity)) identity = "";
    if (strcmp(s_identity, identity) != 0) {
        if (++s_generation == 0u) ++s_generation;
        memset(s_identity, 0, sizeof(s_identity));
        memcpy(s_identity, identity, strlen(identity));
        s_authorized = s_code_grant = false;
    }
    if ((uint32_t)(now - s_retry_at) < 1000u) return;
    s_retry_at = now;
    store_diag_snapshot_t diag;
    store_service_get_diag(&diag);
    uint32_t required = (UINT32_C(1) << STORE_UNIT_PHONE_SECURITY) |
                        (UINT32_C(1) << STORE_UNIT_SETTINGS_SYSTEM);
    if (s_record.clear_calls) required |= CALLS_MASK;
    if ((diag.degraded_mask | diag.unavailable_mask) & required) {
        s_ready = s_code_grant = false;
        store_service_require_service(STORE_BOOT_FAULT_RECORD);
        return;
    }
    /* Retry an incomplete durable transaction, never publish RAM-only state. */
    if (!s_ready) {
        phone_security_record_t pending;
        if (!store_phone_security_read(&pending) || !save(&pending)) return;
        s_authorized = s_code_grant = false;
        if (!s_quiet && s_record.failures != 0u) {
            s_quiet = true;
            s_quiet_at = now;
        }
    }
    if (s_record.clear_calls && !finish_clear()) return;
    if (s_quiet && (uint32_t)(now - s_quiet_at) >= CODE_QUIET_MS) {
        phone_security_record_t next = s_record;
        next.failures = 0u;
        if (!save(&next)) return;
        s_quiet = false;
    }
    if (!s_authorized && s_identity[0] != '\0' && (!s_record.enabled || known()))
        (void)admit();
}

phone_security_state_t phone_security_state(void) {
    if (!s_ready || s_record.clear_calls) return PHONE_SECURITY_FAULT;
    if (!s_record.enabled || s_authorized) return PHONE_SECURITY_OPEN;
    return s_identity[0] == '\0' ? PHONE_SECURITY_WAIT_IDENTITY : PHONE_SECURITY_LOCKED;
}

bool phone_security_enabled(void) { return s_record.enabled; }
uint32_t phone_security_generation(void) { return s_generation; }
void security_code_forget_grant(void) { s_code_grant = false; }

security_code_result_t security_code_verify(const char *code, uint32_t now) {
    s_code_grant = false;
    if (!s_ready || s_record.clear_calls || code == NULL ||
        !store_service_unit_ready(STORE_UNIT_SETTINGS_SYSTEM)) return SECURITY_CODE_UNAVAILABLE;
    store_diag_snapshot_t diag;
    store_service_get_diag(&diag);
    if (diag.boot_faults != 0u || ((diag.degraded_mask | diag.unavailable_mask) &
        (UINT32_C(1) << STORE_UNIT_SETTINGS_SYSTEM))) return SECURITY_CODE_UNAVAILABLE;
    char expected[STORE_TEXT_MAX + 1u] = {0};
    if (store_setting_get_text(STORE_SETTING_SECURITY_CODE, expected, sizeof(expected)) != STORE_STATUS_OK)
        return SECURITY_CODE_UNAVAILABLE;
    size_t n = strlen(expected);
    bool valid = n != 0u && n <= 10u && strspn(expected, "0123456789") == n;
    bool matches = valid && strcmp(code, expected) == 0;
    volatile char *wipe = expected;
    for (size_t i = 0u; i < sizeof(expected); i++) wipe[i] = '\0';
    if (!valid) return SECURITY_CODE_UNAVAILABLE;
    bool expired = s_quiet && (uint32_t)(now - s_quiet_at) >= CODE_QUIET_MS;
    phone_security_record_t next = s_record;
    if (expired) next.failures = 0u;
    bool accepted = matches && next.failures < 5u;
    if (accepted) next.failures = 0u;
    else {
        if (next.failures < 5u) next.failures++;
        s_quiet = true;
        s_quiet_at = now;
    }
    if (!save(&next)) return SECURITY_CODE_UNAVAILABLE;
    if (accepted) { s_quiet = false; s_code_grant = true; }
    return accepted ? SECURITY_CODE_OK : SECURITY_CODE_WRONG;
}

bool phone_security_unlock(uint32_t now) {
    (void)now;
    bool granted = s_code_grant;
    s_code_grant = false;
    return granted && s_ready && s_identity[0] != '\0' && admit();
}

bool phone_security_set_enabled(bool enabled, uint32_t now) {
    (void)now;
    bool granted = s_code_grant;
    s_code_grant = false;
    if (!granted || !s_ready || s_record.clear_calls) return false;
    if (s_record.enabled == enabled) return true;
    phone_security_record_t next = s_record;
    next.enabled = enabled;
    next.clear_calls = true;
    return save(&next) && finish_clear();
}

bool phone_security_restore_defaults(void) {
    bool granted = s_code_grant;
    s_code_grant = false;
    if (!granted || !s_ready) return false;
    phone_security_record_t next = s_record;
    next.clear_calls |= next.enabled;
    next.enabled = false;
    if (!save(&next) || !finish_clear()) return false;
    s_authorized = s_code_grant = false;
    return true;
}

bool security_code_change(const char *code) {
    bool granted = s_code_grant;
    s_code_grant = false;
    if (!granted || !s_ready || code == NULL || strlen(code) != 5u ||
        strspn(code, "0123456789") != 5u ||
        store_setting_set_text(STORE_SETTING_SECURITY_CODE, code) != STORE_STATUS_OK) return false;
    (void)store_service_flush_all();
    store_diag_snapshot_t diag;
    store_service_get_diag(&diag);
    return diag.ready && diag.boot_faults == 0u &&
        ((diag.dirty_mask | diag.degraded_mask | diag.unavailable_mask) &
         (UINT32_C(1) << STORE_UNIT_SETTINGS_SYSTEM)) == 0u;
}

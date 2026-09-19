#include "storage/store_service.h"

#include "store_service_internal.h"

#include <string.h>

#include "services/log.h"
#include "services/timebase.h"

/* Domain codecs use stable record IDs, independently of filesystem layout. */
_Static_assert(STORE_UNIT_COUNT <= 32,
               "store diagnostics use 32-bit unit masks");

static store_commit_result_t commit_unit(store_unit_t unit);
static bool load_unit(store_unit_t unit);
static uint16_t unit_id_for_index(store_unit_t unit);

static const store_unit_binding_t STORE_UNIT_BINDINGS[STORE_UNIT_COUNT] = {
    [STORE_UNIT_SETTINGS_PHONEBOOK] = {&g_store_settings_unit_ops, STORE_DOMAIN_PHONEBOOK},
    [STORE_UNIT_SETTINGS_SMS] = {&g_store_settings_unit_ops, STORE_DOMAIN_SMS},
    [STORE_UNIT_SETTINGS_CALLS] = {&g_store_settings_unit_ops, STORE_DOMAIN_CALLS},
    [STORE_UNIT_SETTINGS_PROFILES] = {&g_store_settings_unit_ops, STORE_DOMAIN_PROFILES},
    [STORE_UNIT_SETTINGS_CLOCK] = {&g_store_settings_unit_ops, STORE_DOMAIN_CLOCK},
    [STORE_UNIT_SETTINGS_SYSTEM] = {&g_store_settings_unit_ops, STORE_DOMAIN_SYSTEM},
    [STORE_UNIT_CALLS_MISSED] = {&g_store_calls_unit_ops, STORE_CALL_LIST_MISSED},
    [STORE_UNIT_CALLS_RECEIVED] = {&g_store_calls_unit_ops, STORE_CALL_LIST_RECEIVED},
    [STORE_UNIT_CALLS_DIALLED] = {&g_store_calls_unit_ops, STORE_CALL_LIST_DIALLED},
    [STORE_UNIT_T9_USER_DICT] = {&g_store_t9_unit_ops, 0u},
    [STORE_UNIT_PICTURE_MESSAGES] = {&g_store_pictures_unit_ops, 0u},
    [STORE_UNIT_OWN_TONES] = {&g_store_tones_unit_ops, 0u},
    [STORE_UNIT_CALL_DIVERT] = {&g_store_divert_unit_ops, 0u},
    [STORE_UNIT_SERVICE_WARRANTY] = {&g_store_warranty_unit_ops, 0u},
    [STORE_UNIT_BATTERY_LEARNING] = {&g_store_battery_learning_unit_ops, 0u},
    [STORE_UNIT_BATTERY_CHARGE_SUPERVISOR] = {
        &g_store_battery_charge_supervisor_unit_ops, 0u},
    [STORE_UNIT_STORAGE_HEALTH] = {&g_store_health_unit_ops, 0u},
};

_Static_assert(sizeof(STORE_UNIT_BINDINGS) / sizeof(STORE_UNIT_BINDINGS[0]) ==
                   STORE_UNIT_COUNT,
               "every persistent unit needs one immutable binding");

static storage_backend_t s_backend;
static bool s_dirty_units[STORE_UNIT_COUNT];
/* Per-unit consecutive commit-failure count: bounds repeated attempts against
 * degraded media (a verify-mismatch fails every attempt). A unit at the
 * cap is skipped -- so it neither burns its own sector nor starves the others --
 * until the slow trickle re-arm grants one retry credit or the phone reboots. */
static uint8_t s_commit_fail_count[STORE_UNIT_COUNT];
/* Round-robin scan start for store_service_tick: so a persistently-failing
 * low-index unit cannot monopolise the one-commit-per-tick slot and starve
 * every higher-index dirty unit. */
static uint8_t s_commit_scan_start;
/* Elapsed-since base for the parked-unit re-arm trickle (wrap-safe, like
 * s_last_commit_ms). A confirmed-degraded (capped) unit is granted ONE retry
 * per STORE_COMMIT_PARK_REARM_MS -- so the endurance cap is not defeated by a
 * high-mutation-rate unit (for example, a call log) re-dirtying its dead sector on
 * every change, yet a transiently-parked unit still self-heals without a reboot. */
static uint32_t s_park_rearm_base_ms;
static bool s_ready;
static uint32_t s_available_units;
static uint8_t s_boot_faults;
static uint32_t s_commit_defer_until_ms;
static uint32_t s_last_commit_ms; /* base for the min-interval pace (elapsed-since,
                                   * wrap-safe -- replaces an absolute next-commit
                                   * deadline that went stale over long idle) */
static store_diag_snapshot_t s_diag;

/* Min gap between deferred commits (was the +32 ms next-commit deadline). */
#define STORE_COMMIT_MIN_INTERVAL_MS 32u
/* Give up committing a unit after this many CONSECUTIVE failures (a degraded
 * sector); reset on a successful commit or the slow park re-arm below. */
#define STORE_COMMIT_FAIL_LIMIT 8u
/* A parked (capped) unit gets a single retry credit this often. Long enough that
 * a genuinely dead sector erase-cycles only at a trickle (endurance), short
 * enough that a transient park recovers on its own well within a usage session. */
#define STORE_COMMIT_PARK_REARM_MS 300000u /* 5 min */
/* A defer deadline is only honored while it sits within this forward window of
 * "now". A legit defer is <= STORE_COMMIT_AUDIO_GUARD_MS (180 ms) ahead; a value
 * further out (or a stale one wrapped into the past) is treated as expired, so a
 * >2^31-stale deadline can't silently block ALL commits. */
#define STORE_COMMIT_DEFER_MAX_MS (STORE_COMMIT_AUDIO_GUARD_MS + 64u)
static uint8_t s_payload[STORAGE_RECORD_MAX_PAYLOAD];

const store_unit_binding_t *store_engine_unit_binding(store_unit_t unit) {
    if (unit >= STORE_UNIT_COUNT) {
        return 0;
    }
    return &STORE_UNIT_BINDINGS[unit];
}

store_status_t store_service_init(void) {
    s_ready = false;
    s_available_units = 0u;
    s_boot_faults = 0u;
    memset(&s_backend, 0, sizeof(s_backend));
    for (store_unit_t unit = 0; unit < STORE_UNIT_COUNT; unit++) {
        const store_unit_binding_t *binding = store_engine_unit_binding(unit);
        if (binding == 0 || binding->ops == 0 ||
            binding->ops->reset_ram == 0 || binding->ops->serialize == 0 ||
            binding->ops->apply == 0) {
            s_ready = false;
            LOGE("store", "missing persistent-unit binding %u", (unsigned)unit);
            return STORE_STATUS_STORAGE_ERROR;
        }
        binding->ops->reset_ram(binding->instance);
    }
    memset(s_dirty_units, 0, sizeof(s_dirty_units));
    memset(s_commit_fail_count, 0, sizeof(s_commit_fail_count));
    s_commit_scan_start = 0u;
    s_park_rearm_base_ms = 0u;
    s_commit_defer_until_ms = 0;
    s_last_commit_ms = 0;
    memset(&s_diag, 0, sizeof(s_diag));
    s_diag.current_unit = STORE_DIAG_NO_UNIT;
    s_diag.last_unit = STORE_DIAG_NO_UNIT;
    storage_record_result_t opened = storage_backend_open(&s_backend);
    if (opened != STORAGE_RECORD_OK) {
        store_service_require_service(STORE_BOOT_FAULT_BACKEND);
        LOGW("store", "record backend unavailable; media left intact");
    }
    if (s_backend.read == NULL || s_backend.write == NULL) {
        return STORE_STATUS_STORAGE_ERROR;
    }

    for (store_unit_t unit = 0; unit < STORE_UNIT_COUNT; unit++) {
        if (load_unit(unit)) {
            s_available_units |= UINT32_C(1) << unit;
        } else {
            store_service_require_service(STORE_BOOT_FAULT_RECORD);
            LOGE("store", "record %u unavailable", (unsigned)unit);
        }
    }
    if (store_service_unit_ready(STORE_UNIT_SERVICE_WARRANTY)) store_warranty_post_load();

    s_ready = true;
    s_diag.ready = true;
    LOGI("store", "record backend ready; %u units", (unsigned)STORE_UNIT_COUNT);
    return s_boot_faults == 0u ? STORE_STATUS_OK : STORE_STATUS_STORAGE_ERROR;
}

bool store_service_ready(void) {
    return s_ready;
}

bool store_service_unit_ready(store_unit_t unit) {
    return (unsigned)unit < STORE_UNIT_COUNT &&
        (s_available_units & (UINT32_C(1) << unit)) != 0u;
}

void store_service_require_service(uint8_t faults) { s_boot_faults |= faults; }
bool store_service_contact_service_required(void) { return s_boot_faults != 0u; }

bool store_service_standby_ready(void) {
    if (!s_ready) {
        return true; /* no usable backend and therefore no commit can run */
    }
    if (s_diag.commit_active) {
        return false;
    }
    for (uint8_t i = 0u; i < (uint8_t)STORE_UNIT_COUNT; i++) {
        if (s_dirty_units[i] &&
            s_commit_fail_count[i] < STORE_COMMIT_FAIL_LIMIT) {
            return false;
        }
    }
    return true;
}

void store_service_get_diag(store_diag_snapshot_t *out) {
    if (out == NULL) {
        return;
    }
    store_diag_snapshot_t snapshot = s_diag;
    snapshot.ready = s_ready;
    snapshot.boot_faults = s_boot_faults;
    snapshot.unavailable_mask = (UINT32_MAX >> (32u - STORE_UNIT_COUNT)) & ~s_available_units;
    snapshot.next_scan_unit = s_commit_scan_start;
    snapshot.defer_active =
        (uint32_t)(s_commit_defer_until_ms - time_ms()) <=
        STORE_COMMIT_DEFER_MAX_MS;
    snapshot.dirty_mask = 0u;
    snapshot.degraded_mask = 0u;
    for (uint8_t i = 0u; i < (uint8_t)STORE_UNIT_COUNT; i++) {
        if (s_dirty_units[i]) {
            snapshot.dirty_mask |= UINT32_C(1) << i;
        }
        if (s_commit_fail_count[i] >= STORE_COMMIT_FAIL_LIMIT) {
            snapshot.degraded_mask |= UINT32_C(1) << i;
        }
        snapshot.consecutive_failures[i] = s_commit_fail_count[i];
    }
    *out = snapshot;
}

static store_commit_result_t commit_unit_tracked(store_unit_t unit) {
    s_diag.commit_active = true;
    s_diag.current_unit = (uint8_t)unit;
    s_diag.commit_attempts++;
    store_commit_result_t result = commit_unit(unit);
    store_status_t status = result.status;
    s_diag.last_unit = (uint8_t)unit;
    s_diag.last_status[unit] = (uint8_t)status;
    s_diag.last_commit_ms = time_ms();
    if (status == STORE_STATUS_OK) {
        s_diag.commit_successes++;
    } else {
        s_diag.commit_failures++;
        if (result.flash_busy) {
            s_diag.busy_deferrals++;
        }
    }
    s_diag.current_unit = STORE_DIAG_NO_UNIT;
    s_diag.commit_active = false;
    return result;
}

void store_service_defer_commits_until(uint32_t deadline_ms) {
    uint32_t now = time_ms();
    /* Adopt the new deadline if the current one is expired/stale (outside the
     * bounded forward window) or the new one is further ahead. Comparing forward
     * DISTANCES from now (unsigned) is wrap-safe, so a stale wrapped value can
     * neither win here nor block commits in the tick. */
    bool current_valid = (uint32_t)(s_commit_defer_until_ms - now) <= STORE_COMMIT_DEFER_MAX_MS;
    if (!current_valid ||
        (uint32_t)(deadline_ms - now) > (uint32_t)(s_commit_defer_until_ms - now)) {
        s_commit_defer_until_ms = deadline_ms;
    }
}

bool store_service_flush_all(void) {
    if (!s_ready) {
        return true; /* nothing journaled to lose */
    }
    bool all_clean = true;
    s_diag.flush_attempts++;
    for (uint8_t i = 0; i < STORE_UNIT_COUNT; i++) {
        if (!s_dirty_units[i]) {
            continue;
        }
        if (s_commit_fail_count[i] >= STORE_COMMIT_FAIL_LIMIT) {
            /* Confirmed-degraded unit: its data is unpersistable. Accept the
             * loss rather than report a flush failure -- this path is retried
             * ~1/s by power_sleep_enter() during soft-off, so blocking on it
             * would keep the phone off the dormant floor (pack drain) and
             * erase-cycle the dead sector forever. The cap is shared with the
             * tick path, so a unit already parked there is skipped here too. */
            continue;
        }
        store_commit_result_t result = commit_unit_tracked((store_unit_t)i);
        store_status_t status = result.status;
        if (status == STORE_STATUS_OK) {
            s_dirty_units[i] = false;
            s_commit_fail_count[i] = 0u;
        } else {
            all_clean = false;
            if (result.flash_busy) {
                /* Transient core1-park timeout, NOT degraded media: leave the unit
                 * uncapped so flush stays not-clean (power_sleep retries) and a
                 * SUSTAINED wedge escalates to the nvm_flash_hal 20-park-fail
                 * reboot -- instead of parking the unit and accepting the loss. */
                LOGW("store", "flush unit %u deferred (core1 busy)", (unsigned)i);
            } else if (++s_commit_fail_count[i] >= STORE_COMMIT_FAIL_LIMIT) {
                LOGW("store", "flush unit %u failing persistently (%u); parking it",
                     (unsigned)i, (unsigned)status);
            } else {
                LOGW("store", "power-off flush unit %u failed: %u", (unsigned)i, (unsigned)status);
            }
        }
    }
    if (!all_clean) {
        s_diag.flush_incomplete++;
    }
    return all_clean;
}

bool store_service_write_window_open(uint32_t now_ms) {
    return (uint32_t)(s_commit_defer_until_ms - now_ms) > STORE_COMMIT_DEFER_MAX_MS;
}

void store_service_tick(uint32_t now_ms) {
    /* Wrap-safe gates (the old signed absolute-deadline compares inverted after
     * ~24.8 days of idle, blocking ALL commits -> silent loss of call logs and
     * settings on any later reboot). Defer: honored only while the
     * deadline is within the bounded forward window. Pace: elapsed-since the
     * last commit. Both use unsigned distances, immune to the 32-bit wrap. */
    if (!s_ready) {
        return;
    }

    /* Trickle re-arm of parked (confirmed-degraded) units, independent of commit
     * pacing: once per STORE_COMMIT_PARK_REARM_MS grant each still-dirty capped
     * unit a SINGLE retry credit. This is what bounds the erase-cycling of a dead
     * sector -- store_engine_mark_dirty deliberately does NOT un-park (else a
     * frequently updated unit could defeat the cap and hammer the sector). A
     * transiently-parked unit recovers here without a reboot. */
    if ((uint32_t)(now_ms - s_park_rearm_base_ms) >= STORE_COMMIT_PARK_REARM_MS) {
        s_park_rearm_base_ms = now_ms;
        for (uint8_t i = 0; i < STORE_UNIT_COUNT; i++) {
            if (s_dirty_units[i] && s_commit_fail_count[i] >= STORE_COMMIT_FAIL_LIMIT) {
                s_commit_fail_count[i] = STORE_COMMIT_FAIL_LIMIT - 1u;
            }
        }
    }

    bool defer_active = (uint32_t)(s_commit_defer_until_ms - now_ms) <= STORE_COMMIT_DEFER_MAX_MS;
    bool pace_ok = (uint32_t)(now_ms - s_last_commit_ms) >= STORE_COMMIT_MIN_INTERVAL_MS;
    if (defer_active || !pace_ok) {
        return;
    }

    /* Commit ONE dirty unit per paced tick, scanning round-robin from
     * s_commit_scan_start and skipping units that have hit the failure cap, so a
     * single degraded sector can neither starve the healthy units nor erase-cycle
     * itself unboundedly (the park-fail path got a reboot backstop; this
     * verify-fail path had none). */
    for (uint8_t n = 0; n < STORE_UNIT_COUNT; n++) {
        uint8_t i = (uint8_t)((s_commit_scan_start + n) % STORE_UNIT_COUNT);
        if (!s_dirty_units[i] || s_commit_fail_count[i] >= STORE_COMMIT_FAIL_LIMIT) {
            continue;
        }
        store_commit_result_t result = commit_unit_tracked((store_unit_t)i);
        store_status_t status = result.status;
        if (status == STORE_STATUS_OK) {
            s_dirty_units[i] = false;
            s_commit_fail_count[i] = 0u;
        } else if (result.flash_busy) {
            /* Transient core1-park timeout (core1 busy/wedged), NOT a degraded
             * sector: do NOT count it toward the per-unit cap. The unit stays
             * uncapped and is retried; a SUSTAINED wedge escalates to the
             * nvm_flash_hal 20-park-fail reboot rather than being masked by
             * parking this one unit (which froze the global park streak < 20). */
            LOGW("store", "unit %u commit deferred (core1 busy); not counted toward cap",
                 (unsigned)i);
        } else if (++s_commit_fail_count[i] >= STORE_COMMIT_FAIL_LIMIT) {
            LOGW("store", "unit %u commit failing persistently (%u); parking until timed retry",
                 (unsigned)i, (unsigned)status);
        } else {
            LOGW("store", "deferred commit unit %u failed: %u", (unsigned)i, (unsigned)status);
        }
        s_last_commit_ms = now_ms; /* pace the next deferred commit */
        s_commit_scan_start = (uint8_t)((i + 1u) % STORE_UNIT_COUNT); /* next tick starts past it */
        break;
    }
}

store_status_t store_engine_mark_dirty(store_unit_t unit) {
    if (unit >= STORE_UNIT_COUNT) {
        return STORE_STATUS_INVALID_ARGUMENT;
    }
    if (!store_service_unit_ready(unit)) return STORE_STATUS_NOT_READY;
    s_dirty_units[unit] = true;
    /* Do NOT reset s_commit_fail_count here. The cap counts CONSECUTIVE commit
     * failures of a unit's (degraded) sector -- it is cleared only by a SUCCESSFUL
     * commit (store_service_tick / flush_all) or decremented by the slow trickle
     * re-arm. Resetting on any mutation let a high-mutation-rate unit such as a
     * call log re-arm the counter on every change: mutating once between each
     * failed commit kept the count below the cap forever, erase-cycling a dead
     * sector without bound -- defeating the cap for exactly the units most likely
     * to hit it. Fresh data is still committed while unparked (the count<LIMIT
     * path in the tick scan), so nothing is lost by not resetting. */
    return s_ready ? STORE_STATUS_OK : STORE_STATUS_NOT_READY;
}

store_commit_result_t store_engine_commit_binding(
    const store_unit_binding_t *binding,
    storage_backend_t *backend,
    uint16_t id,
    uint8_t *payload,
    size_t payload_cap) {
    store_commit_result_t result = {
        .status = STORE_STATUS_INVALID_ARGUMENT,
        .flash_busy = false,
    };
    if (binding == 0 || binding->ops == 0 ||
        binding->ops->serialize == 0 || backend == 0 || backend->write == 0 || payload == 0) {
        return result;
    }
    size_t len = 0u;
    if (!binding->ops->serialize(binding->instance, payload,
                                 payload_cap, &len) || len > payload_cap) {
        result.status = STORE_STATUS_STORAGE_ERROR;
        return result;
    }
    storage_record_result_t write_result = backend->write(backend, id, payload, len);
    result.status = write_result == STORAGE_RECORD_OK
                        ? STORE_STATUS_OK
                        : STORE_STATUS_STORAGE_ERROR;
    result.flash_busy = write_result == STORAGE_RECORD_BUSY;
    return result;
}

static store_commit_result_t commit_unit(store_unit_t unit) {
    store_commit_result_t result = {
        .status = STORE_STATUS_INVALID_ARGUMENT,
        .flash_busy = false,
    };
    const store_unit_binding_t *binding = store_engine_unit_binding(unit);
    if (!s_ready || !store_service_unit_ready(unit)) {
        result.status = STORE_STATUS_NOT_READY;
        return result;
    }
    if (binding == 0 || binding->ops == 0 || binding->ops->serialize == 0) {
        return result;
    }
    return store_engine_commit_binding(binding, &s_backend, unit_id_for_index(unit),
                                       s_payload, sizeof(s_payload));
}

static bool load_unit(store_unit_t unit) {
    const store_unit_binding_t *binding = store_engine_unit_binding(unit);
    size_t len = 0;
    storage_record_result_t result = s_backend.read(&s_backend, unit_id_for_index(unit),
                                                     s_payload, sizeof(s_payload), &len);
    if (result == STORAGE_RECORD_OK) {
        if (binding->ops->apply(binding->instance, s_payload, len)) {
            return true;
        }
        /* A codec can fail after partially applying fields. Keep safe RAM
         * defaults, but never publish them over the damaged record. */
        binding->ops->reset_ram(binding->instance);
        LOGW("store", "preserved corrupt %s unit %u", binding->ops->name,
             (unsigned)unit);
        return false;
    } else if (result != STORAGE_RECORD_NOT_FOUND) {
        return false;
    }
    if (binding->ops->fallback_missing != 0) {
        binding->ops->fallback_missing(binding->instance);
    }
    if (result == STORAGE_RECORD_NOT_FOUND) {
        /* Each default is independently atomic. An interrupted first boot
         * resumes only missing files; it never consults old journal bytes. */
        store_commit_result_t committed = store_engine_commit_binding(
            binding, &s_backend, unit_id_for_index(unit), s_payload, sizeof(s_payload));
        /* Space pressure is not corruption. Missing defaults may remain in RAM
         * and be retried by the ordinary bounded commit scheduler. */
        s_dirty_units[unit] = committed.status != STORE_STATUS_OK;
    }
    return true;
}

static uint16_t unit_id_for_index(store_unit_t unit) {
    return (uint16_t)(0x3210u + (uint16_t)unit);
}

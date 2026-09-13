#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "services/battery_learning_service.h"
#include "services/board_diag_service.h"
#include "storage/store_service.h"

#define TEST_CHARGE_FACTOR_PERMILLE 1224u

static battery_learning_observation_t s_observation;
static battery_learning_persisted_t s_stored;
static store_status_t s_get_status;
static store_status_t s_set_status;
static bool s_store_ready;
static uint32_t s_set_calls;

void board_diag_get_battery_learning_observation(
    uint32_t now_ms, battery_learning_observation_t *out) {
    *out = s_observation;
    out->now_ms = now_ms;
}

bool store_service_ready(void) {
    return s_store_ready;
}

store_status_t store_battery_learning_get(
    battery_learning_persisted_t *out_state) {
    if (s_get_status == STORE_STATUS_OK) {
        *out_state = s_stored;
    }
    return s_get_status;
}

store_status_t store_battery_learning_set(
    const battery_learning_persisted_t *state) {
    s_set_calls++;
    if (s_set_status == STORE_STATUS_OK) {
        s_stored = *state;
    }
    return s_set_status;
}

static void reset_fixture(void) {
    battery_learning_persisted_defaults(&s_stored, NULL);
    s_get_status = STORE_STATUS_OK;
    s_set_status = STORE_STATUS_OK;
    s_store_ready = true;
    s_set_calls = 0u;
    s_observation = (battery_learning_observation_t){
        .sample_sequence = 1u,
        .gauge_session = 0u,
        .acr_raw = UINT32_C(0x80000000),
        .session_delta_nah = 0,
        .terminal_mv = 2700u,
        .current_ua = 0,
        .temperature_mdegc = 25000,
        .sample_valid = true,
        .current_valid = true,
        .continuity_valid = true,
        .charger_connected = true,
        .charge_active = false,
        .authoritative = true,
    };
}

static void test_load_and_persist_full_anchor(void) {
    reset_fixture();
    battery_learning_service_init();
    battery_learning_service_snapshot_t snapshot;
    battery_learning_service_get_snapshot(&snapshot);
    assert(snapshot.initialized);
    assert(!snapshot.persistence_pending);
    assert(snapshot.model.nominal_capacity_mah == 1225u);

    uint32_t result = battery_learning_service_poll(
        1000u, BATTERY_LEARNING_EVENT_FULL,
        TEST_CHARGE_FACTOR_PERMILLE);
    assert((result & BATTERY_LEARNING_RESULT_FULL_ANCHORED) != 0u);
    assert(s_set_calls == 1u);
    assert(s_stored.full_anchor_valid);
    battery_learning_service_get_snapshot(&snapshot);
    assert(!snapshot.persistence_pending);
    assert(snapshot.persistence_requests == 1u);
    assert(snapshot.update_count == 1u);
}

static void test_pending_write_retries_without_new_sample(void) {
    reset_fixture();
    battery_learning_service_init();
    s_store_ready = false;
    (void)battery_learning_service_poll(
        1000u, BATTERY_LEARNING_EVENT_FULL,
        TEST_CHARGE_FACTOR_PERMILLE);
    battery_learning_service_snapshot_t snapshot;
    battery_learning_service_get_snapshot(&snapshot);
    assert(snapshot.persistence_pending);
    assert(s_set_calls == 0u);

    s_store_ready = true;
    (void)battery_learning_service_poll(
        1001u, 0u, TEST_CHARGE_FACTOR_PERMILLE);
    battery_learning_service_get_snapshot(&snapshot);
    assert(!snapshot.persistence_pending);
    assert(s_set_calls == 1u);
    assert(s_stored.full_anchor_valid);
}

static void test_failed_store_write_remains_pending(void) {
    reset_fixture();
    battery_learning_service_init();
    s_set_status = STORE_STATUS_STORAGE_ERROR;
    (void)battery_learning_service_poll(
        1000u, BATTERY_LEARNING_EVENT_FULL,
        TEST_CHARGE_FACTOR_PERMILLE);
    battery_learning_service_snapshot_t snapshot;
    battery_learning_service_get_snapshot(&snapshot);
    assert(snapshot.persistence_pending);
    assert(snapshot.persistence_failures == 1u);

    s_set_status = STORE_STATUS_OK;
    (void)battery_learning_service_poll(
        1001u, 0u, TEST_CHARGE_FACTOR_PERMILLE);
    battery_learning_service_get_snapshot(&snapshot);
    assert(!snapshot.persistence_pending);
    assert(snapshot.persistence_failures == 1u);
    assert(s_set_calls == 2u);
}

static void test_invalid_record_is_repaired_from_prior(void) {
    reset_fixture();
    s_stored.nominal_capacity_mah = 0u;
    battery_learning_service_init();
    battery_learning_service_snapshot_t snapshot;
    battery_learning_service_get_snapshot(&snapshot);
    assert(snapshot.model.nominal_capacity_mah == 1225u);
    assert(snapshot.persistence_requests == 1u);
    assert(!snapshot.persistence_pending);
    assert(s_set_calls == 1u);
    assert(s_stored.nominal_capacity_mah == 1225u);
}

static void test_non_authoritative_observation_is_inert(void) {
    reset_fixture();
    battery_learning_service_init();
    s_observation.authoritative = false;
    uint32_t result = battery_learning_service_poll(
        1000u, BATTERY_LEARNING_EVENT_FULL,
        TEST_CHARGE_FACTOR_PERMILLE);
    assert(result == 0u);
    assert(s_set_calls == 0u);
    battery_learning_service_snapshot_t snapshot;
    battery_learning_service_get_snapshot(&snapshot);
    assert(!snapshot.model.full_anchor_valid);
}

static void test_runtime_charge_factor_owns_soc_projection(void) {
    reset_fixture();
    battery_learning_service_init();
    uint32_t result = battery_learning_service_poll(
        1000u, BATTERY_LEARNING_EVENT_FULL, 1375u);
    assert((result & BATTERY_LEARNING_RESULT_FULL_ANCHORED) != 0u);
    assert(s_stored.soc_charge_factor_permille == 1375u);

    reset_fixture();
    battery_learning_service_init();
    result = battery_learning_service_poll(
        1000u, BATTERY_LEARNING_EVENT_FULL,
        BATTERY_CHARGE_FACTOR_MIN_PERMILLE - 1u);
    assert((result & BATTERY_LEARNING_RESULT_FULL_ANCHORED) != 0u);
    assert(s_stored.soc_charge_factor_permille ==
           TEST_CHARGE_FACTOR_PERMILLE);
}

static battery_learning_provision_t provision_fixture(void) {
    return (battery_learning_provision_t){
        .capacity_mah = 987u,
        .resistance_mohm = {180u, 220u, 0u},
    };
}

static void test_provision_binds_current_session_and_persists(void) {
    reset_fixture();
    battery_learning_service_init();
    s_observation.charger_connected = false;
    s_observation.gauge_session = 9u;
    battery_learning_provision_t provision = provision_fixture();
    assert(battery_learning_service_provision(&provision) ==
           BATTERY_LEARNING_PROVISION_OK);
    assert(s_set_calls == 1u);
    assert(s_stored.capacity_history_count == 1u);
    assert(s_stored.capacity_history_mah[0] == 987u);
    assert(s_stored.accepted_capacity_cycles == 1u);
    assert(!s_stored.full_anchor_valid);
    assert(s_stored.resistance_mohm[0] == 180u);
    assert(s_stored.resistance_mohm[1] == 220u);
    assert(s_stored.resistance_sample_count[0] == 0u);

    battery_learning_service_snapshot_t snapshot;
    battery_learning_service_get_snapshot(&snapshot);
    assert(snapshot.model.learned_capacity_mah == 987u);
    assert(snapshot.model.capacity_confidence ==
           BATTERY_CAPACITY_CONFIDENCE_OBSERVED);
    assert(!snapshot.model.full_anchor_valid);
    assert(!snapshot.persistence_pending);
    assert(snapshot.persistence_requests == 1u);

    s_observation.sample_sequence++;
    uint32_t result = battery_learning_service_poll(
        1000u, 0u, TEST_CHARGE_FACTOR_PERMILLE);
    assert((result & BATTERY_LEARNING_RESULT_PACK_RESET) == 0u);
    battery_learning_service_get_snapshot(&snapshot);
    assert(snapshot.model.learned_capacity_mah == 987u);

    s_observation.sample_sequence++;
    s_observation.gauge_session++;
    result = battery_learning_service_poll(
        2000u, 0u, TEST_CHARGE_FACTOR_PERMILLE);
    assert((result & BATTERY_LEARNING_RESULT_PACK_RESET) != 0u);
    battery_learning_service_get_snapshot(&snapshot);
    assert(!snapshot.model.learned_capacity_valid);
}

static void test_provision_guards_active_or_untrusted_evidence(void) {
    battery_learning_provision_t provision = provision_fixture();

    reset_fixture();
    battery_learning_service_init();
    (void)battery_learning_service_poll(
        1000u, BATTERY_LEARNING_EVENT_FULL,
        TEST_CHARGE_FACTOR_PERMILLE);
    uint32_t set_calls = s_set_calls;
    s_observation.charger_connected = false;
    assert(battery_learning_service_provision(&provision) ==
           BATTERY_LEARNING_PROVISION_ACTIVE_EVIDENCE);
    assert(s_set_calls == set_calls);

    reset_fixture();
    battery_learning_service_init();
    s_observation.charger_connected = false;
    s_observation.charge_active = true;
    assert(battery_learning_service_provision(&provision) ==
           BATTERY_LEARNING_PROVISION_ACTIVE_EVIDENCE);
    assert(s_set_calls == 0u);

    reset_fixture();
    battery_learning_service_init();
    s_observation.charger_connected = false;
    s_observation.authoritative = false;
    assert(battery_learning_service_provision(&provision) ==
           BATTERY_LEARNING_PROVISION_OBSERVATION_UNAVAILABLE);
    assert(s_set_calls == 0u);

    reset_fixture();
    battery_learning_service_init();
    s_observation.charger_connected = false;
    s_observation.sample_valid = false;
    assert(battery_learning_service_provision(&provision) ==
           BATTERY_LEARNING_PROVISION_OBSERVATION_UNAVAILABLE);
    assert(s_set_calls == 0u);

    s_observation.sample_valid = true;
    s_observation.current_valid = false;
    assert(battery_learning_service_provision(&provision) ==
           BATTERY_LEARNING_PROVISION_OBSERVATION_UNAVAILABLE);
    assert(s_set_calls == 0u);

    s_observation.current_valid = true;
    s_observation.continuity_valid = false;
    assert(battery_learning_service_provision(&provision) ==
           BATTERY_LEARNING_PROVISION_OBSERVATION_UNAVAILABLE);
    assert(s_set_calls == 0u);
}

static void test_provision_failures_leave_live_model_unchanged(void) {
    battery_learning_provision_t provision = provision_fixture();

    reset_fixture();
    battery_learning_service_init();
    s_observation.charger_connected = false;
    s_store_ready = false;
    assert(battery_learning_service_provision(&provision) ==
           BATTERY_LEARNING_PROVISION_STORE_NOT_READY);

    s_store_ready = true;
    provision.capacity_mah = 299u;
    assert(battery_learning_service_provision(&provision) ==
           BATTERY_LEARNING_PROVISION_INVALID_ARGUMENT);
    battery_learning_service_snapshot_t snapshot;
    battery_learning_service_get_snapshot(&snapshot);
    assert(!snapshot.model.learned_capacity_valid);
    assert(s_set_calls == 0u);

    provision = provision_fixture();
    s_set_status = STORE_STATUS_STORAGE_ERROR;
    assert(battery_learning_service_provision(&provision) ==
           BATTERY_LEARNING_PROVISION_STORE_ERROR);
    battery_learning_service_get_snapshot(&snapshot);
    assert(!snapshot.model.learned_capacity_valid);
    assert(s_set_calls == 1u);
}

static void seed_recovery_fixture(void) {
    reset_fixture();
    s_stored.capacity_history_mah[0] = 1242u;
    s_stored.capacity_history_count = 1u;
    s_stored.capacity_history_next = 1u;
    s_stored.accepted_capacity_cycles = 1u;
    s_stored.rejected_capacity_cycles = 1u;
    s_stored.last_capacity_mah = 1242u;
    s_stored.resistance_mohm[0] = 115u;
    s_stored.resistance_mohm[1] = 132u;
    s_stored.resistance_mohm[2] = 89u;
    s_stored.resistance_sample_count[0] = 7u;
    s_stored.resistance_sample_count[1] = 8u;
    s_stored.resistance_sample_count[2] = 9u;
    s_stored.pack_generation = 1u;
    s_stored.full_anchor_acr_raw = UINT32_C(0x80012345);
    s_stored.full_anchor_nah = INT64_C(500000000);
    s_stored.cycle_min_temperature_mdegc = 22000;
    s_stored.cycle_max_temperature_mdegc = 26000;
    s_stored.full_anchor_valid = true;
    s_stored.capacity_cycle_qualified = true;
    s_stored.soc_valid = true;
    s_stored.soc_capacity_mah = 1242u;
    s_stored.soc_charge_factor_permille = TEST_CHARGE_FACTOR_PERMILLE;
    s_stored.soc_anchor_provenance =
        BATTERY_SOC_PROVENANCE_ANCHORED_FULL;
    s_stored.soc_confidence = BATTERY_SOC_CONFIDENCE_ANCHORED;
    s_stored.soc_anchor_session_delta_nah = INT64_C(500000000);
    s_stored.soc_anchor_remaining_nah = INT64_C(1242000000);
    assert(battery_learning_persisted_valid(&s_stored, NULL));
    s_observation.charger_connected = false;
    s_observation.session_delta_nah = INT64_C(500000000);
}

static void test_recovered_cycle_is_transactional_and_idempotent(void) {
    seed_recovery_fixture();
    battery_learning_service_init();
    assert(battery_learning_service_recover_capacity_cycle(1301u, 1u) ==
           BATTERY_LEARNING_RECOVER_OK);
    assert(s_set_calls == 1u);
    assert(s_stored.capacity_history_count == 2u);
    assert(s_stored.capacity_history_mah[0] == 1242u);
    assert(s_stored.capacity_history_mah[1] == 1301u);
    assert(s_stored.accepted_capacity_cycles == 2u);
    assert(s_stored.rejected_capacity_cycles == 1u);
    assert(s_stored.full_anchor_valid);
    assert(s_stored.capacity_cycle_qualified);
    assert(s_stored.full_anchor_acr_raw == UINT32_C(0x80012345));
    assert(s_stored.soc_capacity_mah == 1272u);
    assert(s_stored.soc_anchor_remaining_nah == INT64_C(1272000000));
    assert(s_stored.resistance_mohm[0] == 115u);
    assert(s_stored.resistance_sample_count[2] == 9u);
    assert(s_stored.pack_generation == 1u);

    battery_learning_service_snapshot_t snapshot;
    battery_learning_service_get_snapshot(&snapshot);
    assert(snapshot.model.learned_capacity_mah == 1272u);
    assert(snapshot.model.capacity_history_mah[0] == 1242u);
    assert(snapshot.model.capacity_history_mah[1] == 1301u);
    assert(snapshot.model.remaining_capacity_mah == 1272u);
    assert(snapshot.persistence_requests == 1u);
    assert(!snapshot.persistence_pending);

    assert(battery_learning_service_recover_capacity_cycle(1301u, 1u) ==
           BATTERY_LEARNING_RECOVER_STALE_COUNT);
    assert(s_set_calls == 1u);
}

static void test_recovered_cycle_guards_and_store_failure_are_atomic(void) {
    seed_recovery_fixture();
    battery_learning_service_init();
    s_observation.charger_connected = true;
    assert(battery_learning_service_recover_capacity_cycle(1301u, 1u) ==
           BATTERY_LEARNING_RECOVER_ACTIVE_EVIDENCE);
    assert(s_set_calls == 0u);

    s_observation.charger_connected = false;
    s_observation.current_valid = false;
    assert(battery_learning_service_recover_capacity_cycle(1301u, 1u) ==
           BATTERY_LEARNING_RECOVER_OBSERVATION_UNAVAILABLE);
    assert(s_set_calls == 0u);

    s_observation.current_valid = true;
    s_store_ready = false;
    assert(battery_learning_service_recover_capacity_cycle(1301u, 1u) ==
           BATTERY_LEARNING_RECOVER_STORE_NOT_READY);
    assert(s_set_calls == 0u);

    s_store_ready = true;
    s_set_status = STORE_STATUS_STORAGE_ERROR;
    assert(battery_learning_service_recover_capacity_cycle(1301u, 1u) ==
           BATTERY_LEARNING_RECOVER_STORE_ERROR);
    battery_learning_service_snapshot_t snapshot;
    battery_learning_service_get_snapshot(&snapshot);
    assert(snapshot.model.capacity_history_count == 1u);
    assert(snapshot.model.learned_capacity_mah == 1242u);
    assert(snapshot.model.full_anchor_valid);
    assert(s_set_calls == 1u);

    assert(battery_learning_service_recover_capacity_cycle(299u, 1u) ==
           BATTERY_LEARNING_RECOVER_INVALID_ARGUMENT);
}

static void seed_full_recovery_fixture(void) {
    reset_fixture();
    s_stored.capacity_history_mah[0] = 1272u;
    s_stored.capacity_history_count = 1u;
    s_stored.capacity_history_next = 1u;
    s_stored.accepted_capacity_cycles = 1u;
    s_stored.last_capacity_mah = 1272u;
    assert(battery_learning_persisted_valid(&s_stored, NULL));
    s_observation.charger_connected = false;
    s_observation.charge_active = false;
    s_observation.current_ua = -27073;
    s_observation.acr_raw = UINT32_C(0x800a22c9);
    s_observation.session_delta_nah = INT64_C(2540813625);
    s_observation.terminal_mv = 2845u;
}

static void test_historical_full_recovery_is_guarded_and_transactional(void) {
    seed_full_recovery_fixture();
    battery_learning_service_init();
    assert(battery_learning_service_recover_full_endpoint(
               UINT32_C(0x800a2cc5), INT64_C(2550590325), 1u) ==
           BATTERY_LEARNING_RECOVER_OK);
    assert(s_set_calls == 1u);
    assert(s_stored.full_anchor_valid);
    assert(s_stored.capacity_cycle_qualified);
    assert(s_stored.full_anchor_acr_raw == UINT32_C(0x800a2cc5));
    assert(s_stored.full_anchor_nah == INT64_C(2550590325));
    assert(s_stored.soc_confidence == BATTERY_SOC_CONFIDENCE_ANCHORED);
    assert(s_stored.soc_anchor_remaining_nah == INT64_C(1272000000));

    battery_learning_service_snapshot_t snapshot;
    battery_learning_service_get_snapshot(&snapshot);
    assert(snapshot.model.remaining_capacity_nah == UINT64_C(1262223300));
    assert(snapshot.model.soc_bars_valid);
    assert(snapshot.model.soc_bars == 4u);
    assert(!snapshot.persistence_pending);

    seed_full_recovery_fixture();
    battery_learning_service_init();
    s_observation.charger_connected = true;
    assert(battery_learning_service_recover_full_endpoint(
               UINT32_C(0x800a2cc5), INT64_C(2550590325), 1u) ==
           BATTERY_LEARNING_RECOVER_ACTIVE_EVIDENCE);
    assert(s_set_calls == 0u);

    s_observation.charger_connected = false;
    s_set_status = STORE_STATUS_STORAGE_ERROR;
    assert(battery_learning_service_recover_full_endpoint(
               UINT32_C(0x800a2cc5), INT64_C(2550590325), 1u) ==
           BATTERY_LEARNING_RECOVER_STORE_ERROR);
    battery_learning_service_get_snapshot(&snapshot);
    assert(!snapshot.model.full_anchor_valid);
}

int main(void) {
    test_load_and_persist_full_anchor();
    test_pending_write_retries_without_new_sample();
    test_failed_store_write_remains_pending();
    test_invalid_record_is_repaired_from_prior();
    test_non_authoritative_observation_is_inert();
    test_runtime_charge_factor_owns_soc_projection();
    test_provision_binds_current_session_and_persists();
    test_provision_guards_active_or_untrusted_evidence();
    test_provision_failures_leave_live_model_unchanged();
    test_recovered_cycle_is_transactional_and_idempotent();
    test_recovered_cycle_guards_and_store_failure_are_atomic();
    test_historical_full_recovery_is_guarded_and_transactional();
    puts("battery learning service tests passed");
    return 0;
}

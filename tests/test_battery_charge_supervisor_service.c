#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "services/battery_charge_supervisor_service.h"
#include "services/battery_learning_service.h"
#include "services/board_diag_service.h"
#include "services/charger_control_service.h"
#include "storage/store_service.h"

static battery_learning_observation_t s_gauge;
static board_diag_snapshot_t s_board;
static battery_learning_service_snapshot_t s_learner;
static battery_charge_supervisor_persisted_t s_stored;
static store_status_t s_get_status;
static store_status_t s_set_status;
static bool s_store_ready;
static bool s_store_dirty;
static bool s_store_degraded;
static uint32_t s_set_calls;
static charger_control_snapshot_t s_control;
static bool s_control_set_ok;
static uint32_t s_control_set_calls;

board_diag_chemistry_t board_diag_battery_chemistry(void) {
    return s_board.chemistry;
}

void board_diag_get_battery_learning_observation(
    uint32_t now_ms, battery_learning_observation_t *out) {
    *out = s_gauge;
    out->now_ms = now_ms;
}

void board_diag_get_snapshot(board_diag_snapshot_t *out) {
    *out = s_board;
}

void battery_learning_service_get_snapshot(
    battery_learning_service_snapshot_t *out) {
    *out = s_learner;
}

bool store_service_ready(void) {
    return s_store_ready;
}

void store_service_get_diag(store_diag_snapshot_t *out) {
    memset(out, 0, sizeof(*out));
    out->ready = s_store_ready;
    if (s_store_dirty) {
        out->dirty_mask = (uint16_t)(
            UINT16_C(1) << STORE_UNIT_BATTERY_CHARGE_SUPERVISOR);
    }
    if (s_store_degraded) {
        out->degraded_mask = (uint16_t)(
            UINT16_C(1) << STORE_UNIT_BATTERY_CHARGE_SUPERVISOR);
    }
}

store_status_t store_battery_charge_supervisor_get(
    battery_charge_supervisor_persisted_t *out_state) {
    if (s_get_status == STORE_STATUS_OK) {
        *out_state = s_stored;
    }
    return s_get_status;
}

store_status_t store_battery_charge_supervisor_set(
    const battery_charge_supervisor_persisted_t *state) {
    s_set_calls++;
    if (s_set_status == STORE_STATUS_OK) {
        s_stored = *state;
        s_store_dirty = true;
    }
    return s_set_status;
}

void charger_control_service_get_snapshot(charger_control_snapshot_t *out) {
    *out = s_control;
}

bool charger_control_service_set_inhibit(charger_inhibit_owner_t owner,
                                         bool inhibit) {
    assert(owner == CHARGER_INHIBIT_SUPERVISOR);
    s_control_set_calls++;
    if (!s_control_set_ok) {
        return false;
    }
    if (inhibit) {
        s_control.inhibit_owner_mask |= CHARGER_INHIBIT_SUPERVISOR;
    } else {
        s_control.inhibit_owner_mask &=
            (uint8_t)~CHARGER_INHIBIT_SUPERVISOR;
    }
    s_control.requested_enabled = s_control.inhibit_owner_mask == 0u;
    s_control.actual_enabled = s_control.requested_enabled;
    s_control.readback_valid = true;
    s_board.charger_inhibit_owner_mask = s_control.inhibit_owner_mask;
    s_board.charger_enabled_requested = s_control.requested_enabled;
    s_board.charger_enabled = s_control.actual_enabled;
    s_board.charger_enable_valid = true;
    s_board.charge_state = s_control.actual_enabled
        ? BOARD_DIAG_CHARGE_ACTIVE : BOARD_DIAG_CHARGE_DISABLED;
    return true;
}

static void mark_store_durable(void) {
    s_store_dirty = false;
}

static void reset_fixture(void) {
    memset(&s_gauge, 0, sizeof(s_gauge));
    memset(&s_board, 0, sizeof(s_board));
    memset(&s_learner, 0, sizeof(s_learner));
    s_gauge.sample_sequence = 1u;
    s_gauge.gauge_session = 7u;
    s_gauge.acr_raw = UINT32_C(0x12345678);
    s_gauge.session_delta_nah = INT64_C(100000000);
    s_gauge.terminal_mv = 2700u;
    s_gauge.reference_mv = 2700u;
    s_gauge.current_ua = 100000;
    s_gauge.temperature_mdegc = 25000;
    s_gauge.sample_valid = true;
    s_gauge.reference_valid = true;
    s_gauge.current_valid = true;
    s_gauge.continuity_valid = true;
    s_gauge.charger_connected = true;
    s_gauge.charge_active = true;
    s_gauge.authoritative = true;
    s_board.charger_connected = true;
    s_board.charger_input_mv = 5000u;
    s_board.charge_status_valid = true;
    s_board.charger_enabled_requested = true;
    s_board.charger_enabled = true;
    s_board.charger_enable_valid = true;
    s_board.charge_state = BOARD_DIAG_CHARGE_ACTIVE;
    s_board.chemistry = BOARD_DIAG_CHEM_NI_MH;
    s_learner.initialized = true;
    s_learner.profile_id = BATTERY_LEARNING_PROFILE_NIMH_2S;
    s_learner.model.nominal_capacity_mah = 1225u;
    s_learner.model.learned_capacity_valid = true;
    s_learner.model.learned_capacity_mah = 1000u;
    s_learner.model.capacity_confidence =
        BATTERY_CAPACITY_CONFIDENCE_OBSERVED;
    s_learner.model.remaining_capacity_valid = true;
    s_learner.model.remaining_capacity_mah = 250u;
    s_learner.model.remaining_capacity_nah = UINT64_C(250000000);
    s_learner.model.soc_capacity_mah = 1000u;
    s_learner.model.soc_provenance = BATTERY_SOC_PROVENANCE_TRACKED;
    s_learner.model.soc_confidence = BATTERY_SOC_CONFIDENCE_ANCHORED;
    s_learner.model.pack_generation = 4u;
    s_learner.model.gauge_session_bound = true;
    s_learner.model.gauge_session = 7u;
    s_learner.model.current_resistance_bin_valid = true;
    s_learner.model.current_resistance_bin = BATTERY_RESISTANCE_BIN_MID;
    s_learner.model.resistance_mohm[BATTERY_RESISTANCE_BIN_MID] = 180u;
    battery_charge_supervisor_persisted_defaults(&s_stored);
    s_get_status = STORE_STATUS_OK;
    s_set_status = STORE_STATUS_OK;
    s_store_ready = true;
    s_store_dirty = false;
    s_store_degraded = false;
    s_set_calls = 0u;
    s_control = (charger_control_snapshot_t){
        .requested_enabled = true,
        .actual_enabled = true,
        .readback_valid = true,
    };
    s_control_set_ok = true;
    s_control_set_calls = 0u;
    battery_charge_supervisor_service_init();
}

static battery_charge_supervisor_service_snapshot_t snapshot(void) {
    battery_charge_supervisor_service_snapshot_t out;
    battery_charge_supervisor_service_get_snapshot(&out);
    return out;
}

static uint32_t prepare_durable_software_full(void);

static void seed_policy(battery_charge_supervisor_policy_t policy,
                        bool factor_confident) {
    battery_charge_supervisor_persisted_defaults(&s_stored);
    s_stored.hardware_profile_id = 3u;
    s_stored.chemistry = BATTERY_CHARGE_CHEMISTRY_NIMH_2S;
    s_stored.configured_policy = policy;
    s_stored.configured_charge_factor_permille = 1224u;
    s_stored.configured_charge_factor_confident = factor_confident;
    s_store_dirty = false;
    battery_charge_supervisor_service_init();
}

static void test_maps_and_freezes_precharge_evidence(void) {
    reset_fixture();
    uint32_t result = battery_charge_supervisor_service_poll(1000u);
    assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_ATTACHED) != 0u);
    assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_ADMITTED) != 0u);
    battery_charge_supervisor_service_snapshot_t service = snapshot();
    assert(service.model.phase == BATTERY_CHARGE_PHASE_CHARGING);
    assert(service.model.frozen_capacity_mah == 1000u);
    assert(service.model.frozen_remaining_mah == 250u);
    assert(service.model.frozen_remaining_nah == UINT64_C(250000000));
    assert(service.model.frozen_soc_confidence ==
           BATTERY_SOC_CONFIDENCE_ANCHORED);
    assert(service.model.frozen_resistance_mohm == 180u);
    assert(service.model.pack_generation == 4u);
    assert(service.model.gauge_session == 7u);
    assert(service.attach_events == 1u && service.admission_events == 1u);
    assert(s_set_calls == 1u);
    assert(s_stored.active_session_valid);
    assert(s_stored.charge_generation == service.model.charge_generation);
    assert(s_stored.start_acr_raw == UINT32_C(0x12345678));
    assert(s_stored.start_session_delta_nah == INT64_C(100000000));
    assert(s_stored.frozen_capacity_mah == 1000u);
    assert(s_stored.frozen_remaining_mah == 250u);
    assert(s_stored.frozen_remaining_nah == UINT64_C(250000000));
    assert(s_stored.frozen_resistance_mohm == 180u);

    s_learner.model.learned_capacity_mah = 500u;
    s_learner.model.remaining_capacity_mah = 0u;
    s_learner.model.resistance_mohm[BATTERY_RESISTANCE_BIN_MID] = 900u;
    s_gauge.sample_sequence++;
    s_gauge.session_delta_nah += INT64_C(1000000);
    (void)battery_charge_supervisor_service_poll(2000u);
    service = snapshot();
    assert(service.model.frozen_capacity_mah == 1000u);
    assert(service.model.frozen_remaining_mah == 250u);
    assert(service.model.frozen_resistance_mohm == 180u);
    assert(s_set_calls == 1u);
}

static void test_natural_empty_admission_provides_full_deficit(void) {
    reset_fixture();
    s_learner.model.remaining_capacity_valid = false;
    s_learner.model.remaining_capacity_mah = 0u;
    s_learner.model.remaining_capacity_nah = 0u;
    s_learner.model.soc_capacity_mah = 0u;
    s_learner.model.soc_provenance = BATTERY_SOC_PROVENANCE_UNKNOWN;
    s_learner.model.soc_confidence = BATTERY_SOC_CONFIDENCE_NONE;
    s_learner.model.natural_empty_valid = true;

    uint32_t result = battery_charge_supervisor_service_poll(1000u);
    assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_ADMITTED) != 0u);
    battery_charge_supervisor_service_snapshot_t service = snapshot();
    assert(service.model.frozen_capacity_valid);
    assert(!service.model.frozen_remaining_valid);
    assert(service.model.deficit_valid);
    assert(service.model.deficit_mah == 1000u);
    assert(service.model.target_valid);
    assert(service.model.target_input_nah == UINT64_C(1224000000));
    assert((service.model.blockers &
            BATTERY_CHARGE_BLOCK_UNKNOWN_DEFICIT) == 0u);
}

static void test_new_gauge_session_cannot_admit_previous_pack_soc(void) {
    reset_fixture();
    s_gauge.gauge_session = 8u;
    /* This is the first app poll after boot: BTL2 has populated the model, but
     * the learner has not yet processed the HAL's nonzero reset session. */
    s_learner.model.gauge_session_bound = false;
    s_learner.model.gauge_session = 0u;
    s_learner.model.natural_empty_valid = true;

    uint32_t result = battery_charge_supervisor_service_poll(1000u);
    assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_ATTACHED) != 0u);
    assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_ADMITTED) == 0u);
    assert((snapshot().model.blockers &
            BATTERY_CHARGE_BLOCK_STALE_ADMISSION) != 0u);
    assert(!s_stored.active_session_valid);

    /* The learner has now processed the new session and cleared old-pack SOC.
     * Admission may proceed, but only with the new pack generation/evidence. */
    s_learner.model.gauge_session_bound = true;
    s_learner.model.gauge_session = 8u;
    s_learner.model.pack_generation = 5u;
    s_learner.model.learned_capacity_valid = false;
    s_learner.model.learned_capacity_mah = 0u;
    s_learner.model.capacity_confidence = BATTERY_CAPACITY_CONFIDENCE_PRIOR;
    s_learner.model.remaining_capacity_valid = false;
    s_learner.model.remaining_capacity_mah = 0u;
    s_learner.model.remaining_capacity_nah = 0u;
    s_learner.model.soc_capacity_mah = 0u;
    s_learner.model.soc_provenance = BATTERY_SOC_PROVENANCE_UNKNOWN;
    s_learner.model.soc_confidence = BATTERY_SOC_CONFIDENCE_NONE;
    s_learner.model.natural_empty_valid = false;
    s_gauge.sample_sequence++;
    result = battery_charge_supervisor_service_poll(2000u);
    assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_ADMITTED) != 0u);
    battery_charge_supervisor_service_snapshot_t service = snapshot();
    assert(service.model.pack_generation == 5u);
    assert(service.model.frozen_capacity_mah == 1225u);
    assert(!service.model.frozen_remaining_valid);
    assert(service.model.deficit_valid);
    assert(service.model.target_full_capacity);
    assert((service.model.blockers &
            BATTERY_CHARGE_BLOCK_CAPACITY_CONFIDENCE) != 0u);
}

static void test_terminal_history_rejects_untrusted_acr(void) {
    reset_fixture();
    (void)battery_charge_supervisor_service_poll(1000u);
    mark_store_durable();

    s_gauge.current_valid = false;
    s_gauge.sample_sequence++;
    s_board.charger_connected = false;
    s_gauge.charger_connected = false;
    uint32_t result = battery_charge_supervisor_service_poll(2000u);
    assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_DETACHED) != 0u);
    assert(s_stored.last_terminal_valid);
    assert(!s_stored.last_terminal_net_input_valid);

    reset_fixture();
    (void)battery_charge_supervisor_service_poll(1000u);
    mark_store_durable();
    s_gauge.authoritative = false;
    s_gauge.sample_sequence++;
    s_board.charger_connected = false;
    s_gauge.charger_connected = false;
    result = battery_charge_supervisor_service_poll(2000u);
    assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_DETACHED) != 0u);
    assert(s_stored.last_terminal_valid);
    assert(!s_stored.last_terminal_net_input_valid);
}

static void test_shadow_comparison_and_completion(void) {
    reset_fixture();
    (void)battery_charge_supervisor_service_poll(1000u);
    battery_charge_supervisor_service_note_legacy(true, false);
    battery_charge_supervisor_service_snapshot_t service = snapshot();
    assert(service.shadow_compares == 1u);
    assert(service.shadow_matches == 1u);

    s_board.charge_state = BOARD_DIAG_CHARGE_FULL;
    s_gauge.charge_active = false;
    s_gauge.sample_sequence++;
    (void)battery_charge_supervisor_service_poll(2000u);
    battery_charge_supervisor_service_note_legacy(true, false);
    s_gauge.sample_sequence++;
    uint32_t result = battery_charge_supervisor_service_poll(
        2000u + BATTERY_CHARGE_COMPLETE_CONFIRM_MS);
    assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_BQ_COMPLETE) != 0u);
    battery_charge_supervisor_service_note_legacy(false, true);
    service = snapshot();
    assert(service.shadow_compares == 3u);
    assert(service.shadow_matches == 3u);
    assert(service.shadow_mismatches == 0u);

    battery_charge_supervisor_service_note_legacy(true, false);
    service = snapshot();
    assert(service.shadow_mismatches == 1u);
    assert(!service.shadow_last_active_match);
    assert(!service.shadow_last_completion_match);
}

static void test_trace_ring_is_bounded_and_oldest_first(void) {
    reset_fixture();
    (void)battery_charge_supervisor_service_poll(1000u);
    for (uint32_t second = 1u; second <= 18u * 60u; second++) {
        s_gauge.sample_sequence++;
        s_gauge.session_delta_nah += INT64_C(100000);
        (void)battery_charge_supervisor_service_poll(
            1000u + second * 1000u);
    }
    battery_charge_supervisor_service_snapshot_t service = snapshot();
    assert(service.minute_events == 18u);
    assert(service.trace_count == BATTERY_CHARGE_SUPERVISOR_TRACE_CAP);
    assert(s_set_calls == 1u);
    battery_charge_supervisor_minute_t oldest;
    battery_charge_supervisor_minute_t newest;
    assert(battery_charge_supervisor_service_get_trace(0u, &oldest));
    assert(battery_charge_supervisor_service_get_trace(
        BATTERY_CHARGE_SUPERVISOR_TRACE_CAP - 1u, &newest));
    assert(oldest.at_ms ==
           1000u + 3u * BATTERY_CHARGE_SUPERVISOR_TRACE_INTERVAL_MS);
    assert(newest.at_ms ==
           1000u + 18u * BATTERY_CHARGE_SUPERVISOR_TRACE_INTERVAL_MS);
    assert(!battery_charge_supervisor_service_get_trace(
        BATTERY_CHARGE_SUPERVISOR_TRACE_CAP, &newest));
    assert(!battery_charge_supervisor_service_get_trace(0u, NULL));
}

static void test_manual_inhibit_prevents_admission(void) {
    reset_fixture();
    s_board.charger_inhibit_owner_mask = 1u << 1;
    s_board.charger_enabled_requested = false;
    s_board.charger_enabled = false;
    uint32_t result = battery_charge_supervisor_service_poll(1000u);
    assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_ATTACHED) != 0u);
    assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_ADMITTED) == 0u);
    assert(!snapshot().model.session_authoritative);
}

static void test_persistence_retries_without_replaying_transition(void) {
    reset_fixture();
    s_store_ready = false;
    uint32_t result = battery_charge_supervisor_service_poll(1000u);
    assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_ADMITTED) != 0u);
    battery_charge_supervisor_service_snapshot_t service = snapshot();
    assert(service.persistence_pending);
    assert(service.persisted.active_session_valid);
    assert(s_set_calls == 0u);

    s_store_ready = true;
    result = battery_charge_supervisor_service_poll(1001u);
    assert(result == BATTERY_CHARGE_SUPERVISOR_RESULT_NONE);
    service = snapshot();
    assert(!service.persistence_pending);
    assert(s_set_calls == 1u);
    assert(s_stored.active_session_valid);

    s_set_status = STORE_STATUS_STORAGE_ERROR;
    s_board.charge_state = BOARD_DIAG_CHARGE_FULL;
    s_gauge.sample_sequence++;
    (void)battery_charge_supervisor_service_poll(2000u);
    s_gauge.sample_sequence++;
    result = battery_charge_supervisor_service_poll(
        2000u + BATTERY_CHARGE_COMPLETE_CONFIRM_MS);
    assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_BQ_COMPLETE) != 0u);
    service = snapshot();
    assert(service.persistence_pending);
    assert(service.persistence_failures == 1u);
    assert(!service.persisted.active_session_valid);

    s_set_status = STORE_STATUS_OK;
    (void)battery_charge_supervisor_service_poll(
        2001u + BATTERY_CHARGE_COMPLETE_CONFIRM_MS);
    service = snapshot();
    assert(!service.persistence_pending);
    assert(service.persistence_failures == 1u);
    assert(!s_stored.active_session_valid);
    assert(s_stored.last_terminal_reason ==
           BATTERY_CHARGE_TERMINAL_BQ_COMPLETE_AFTER_ACTIVE);
    assert(s_stored.hardware_profile_id == 3u);
    assert(s_stored.chemistry == BATTERY_CHARGE_CHEMISTRY_NIMH_2S);
    assert(s_stored.last_terminal_net_input_valid);
    assert(s_stored.last_terminal_voltage_valid);
    assert(s_stored.last_terminal_trace_complete);
}

static void test_active_session_restore_waits_for_complete_evidence(void) {
    reset_fixture();
    (void)battery_charge_supervisor_service_poll(1000u);
    assert(s_stored.active_session_valid);
    uint32_t generation = s_stored.charge_generation;
    s_set_calls = 0u;

    battery_charge_supervisor_service_init();
    battery_charge_supervisor_service_snapshot_t service = snapshot();
    assert(service.restore_pending);
    assert(service.model.phase == BATTERY_CHARGE_PHASE_DETACHED);
    assert(service.model.charge_generation == generation);

    s_gauge.sample_valid = false;
    s_gauge.current_valid = false;
    uint32_t result = battery_charge_supervisor_service_poll(2000u);
    assert(result == BATTERY_CHARGE_SUPERVISOR_RESULT_NONE);
    battery_charge_supervisor_service_note_legacy(true, false);
    service = snapshot();
    assert(service.restore_pending);
    assert(service.restore_attempts == 0u);
    assert(service.shadow_compares == 0u);
    assert(s_set_calls == 0u);

    s_gauge.sample_valid = true;
    s_gauge.current_valid = true;
    s_gauge.sample_sequence++;
    s_gauge.session_delta_nah += INT64_C(5000000);
    result = battery_charge_supervisor_service_poll(3000u);
    assert(result == BATTERY_CHARGE_SUPERVISOR_RESULT_RESTORED);
    service = snapshot();
    assert(!service.restore_pending);
    assert(service.restored_after_reset);
    assert(service.restore_attempts == 1u);
    assert(service.restore_successes == 1u);
    assert(service.restore_failures == 0u);
    assert(service.model.phase == BATTERY_CHARGE_PHASE_CHARGING);
    assert(service.model.charge_generation == generation);
    assert(service.model.net_input_nah == INT64_C(5000000));
    assert(s_set_calls == 0u);
}

static void test_restore_identity_mismatch_fails_degraded_once(void) {
    reset_fixture();
    (void)battery_charge_supervisor_service_poll(1000u);
    uint32_t generation = s_stored.charge_generation;
    s_set_calls = 0u;
    battery_charge_supervisor_service_init();
    s_gauge.gauge_session++;

    uint32_t result = battery_charge_supervisor_service_poll(2000u);
    assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_DEGRADED) != 0u);
    assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_TERMINAL) != 0u);
    battery_charge_supervisor_service_snapshot_t service = snapshot();
    assert(!service.restore_pending);
    assert(service.restore_attempts == 1u);
    assert(service.restore_failures == 1u);
    assert(service.model.phase == BATTERY_CHARGE_PHASE_DEGRADED);
    assert(service.model.charge_generation == generation);
    assert(s_set_calls == 1u);
    assert(!s_stored.active_session_valid);
    assert(s_stored.last_terminal_reason ==
           BATTERY_CHARGE_TERMINAL_GAUGE_CONTINUITY_LOST);
    assert(s_stored.hardware_profile_id == 3u);
    assert(s_stored.chemistry == BATTERY_CHARGE_CHEMISTRY_NIMH_2S);
    assert(!s_stored.last_terminal_net_input_valid);
    assert(s_stored.last_terminal_voltage_valid);
    assert(!s_stored.last_terminal_trace_complete);

    result = battery_charge_supervisor_service_poll(3000u);
    assert(result == BATTERY_CHARGE_SUPERVISOR_RESULT_NONE);
    assert(s_set_calls == 1u);
}

static void test_detached_before_restore_closes_stored_session(void) {
    reset_fixture();
    (void)battery_charge_supervisor_service_poll(1000u);
    uint32_t generation = s_stored.charge_generation;
    int64_t baseline = s_stored.start_session_delta_nah;
    s_set_calls = 0u;
    battery_charge_supervisor_service_init();
    s_board.charger_connected = false;
    s_gauge.charger_connected = false;
    s_gauge.session_delta_nah += INT64_C(9000000);

    uint32_t result = battery_charge_supervisor_service_poll(2000u);
    assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_DETACHED) != 0u);
    assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_TERMINAL) != 0u);
    battery_charge_supervisor_service_snapshot_t service = snapshot();
    assert(!service.restore_pending);
    assert(service.restore_attempts == 0u);
    assert(s_set_calls == 1u);
    assert(!s_stored.active_session_valid);
    assert(s_stored.last_terminal_reason ==
           BATTERY_CHARGE_TERMINAL_DETACHED);
    assert(s_stored.hardware_profile_id == 3u);
    assert(s_stored.chemistry == BATTERY_CHARGE_CHEMISTRY_NIMH_2S);
    assert(s_stored.last_terminal_generation == generation);
    assert(s_stored.last_terminal_net_input_nah ==
           s_gauge.session_delta_nah - baseline);
    assert(s_stored.last_terminal_net_input_valid);
    assert(s_stored.last_terminal_voltage_valid);
    assert(!s_stored.last_terminal_trace_complete);
}

static void test_detach_after_terminal_retains_useful_history(void) {
    reset_fixture();
    (void)battery_charge_supervisor_service_poll(1000u);
    s_board.charge_state = BOARD_DIAG_CHARGE_FULL;
    s_gauge.sample_sequence++;
    (void)battery_charge_supervisor_service_poll(2000u);
    s_gauge.sample_sequence++;
    uint32_t result = battery_charge_supervisor_service_poll(
        2000u + BATTERY_CHARGE_COMPLETE_CONFIRM_MS);
    assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_BQ_COMPLETE) != 0u);
    assert(s_stored.last_terminal_reason ==
           BATTERY_CHARGE_TERMINAL_BQ_COMPLETE_AFTER_ACTIVE);
    uint32_t writes = s_set_calls;

    s_board.charger_connected = false;
    s_gauge.charger_connected = false;
    s_gauge.sample_sequence++;
    result = battery_charge_supervisor_service_poll(5000u);
    assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_DETACHED) != 0u);
    assert(s_set_calls == writes);
    assert(s_stored.last_terminal_reason ==
           BATTERY_CHARGE_TERMINAL_BQ_COMPLETE_AFTER_ACTIVE);
}

static void test_unreadable_store_record_is_preserved(void) {
    reset_fixture();
    s_get_status = STORE_STATUS_STORAGE_ERROR;
    s_set_calls = 0u;
    battery_charge_supervisor_service_init();
    battery_charge_supervisor_service_snapshot_t service = snapshot();
    assert(service.initialized);
    assert(!service.persistence_pending);
    assert(service.storage_unavailable);
    assert(service.persistence_requests == 0u && s_set_calls == 0u);
    assert(battery_charge_supervisor_service_boot_inhibit_required());
    s_board.battery_mv = 1800u;
    (void)battery_charge_supervisor_service_poll(1000u);
    assert((s_control.inhibit_owner_mask & CHARGER_INHIBIT_SUPERVISOR) != 0u);
    assert(s_set_calls == 0u);
    s_board.charger_connected = false;
    (void)battery_charge_supervisor_service_poll(2000u);
    assert((s_control.inhibit_owner_mask & CHARGER_INHIBIT_SUPERVISOR) != 0u);
    assert(s_set_calls == 0u);
}

static void test_old_hardware_profile_evidence_is_repaired(void) {
    reset_fixture();
    (void)battery_charge_supervisor_service_poll(1000u);
    assert(s_stored.active_session_valid);
    uint32_t generation = s_stored.charge_generation;
    s_stored.hardware_profile_id = 2u;
    s_set_calls = 0u;

    battery_charge_supervisor_service_init();
    battery_charge_supervisor_service_snapshot_t service = snapshot();
    assert(!service.restore_pending);
    assert(service.model.charge_generation == generation);
    assert(s_set_calls == 1u);
    assert(!s_stored.active_session_valid);
    assert(!s_stored.last_terminal_valid);
    assert(s_stored.hardware_profile_id == 0u);
    assert(s_stored.chemistry == BATTERY_CHARGE_CHEMISTRY_UNKNOWN);
    assert(s_stored.charge_generation == generation);
}

static void test_configuration_is_explicit_and_idle_only(void) {
    reset_fixture();
    assert(battery_charge_supervisor_service_effective_charge_factor_permille()
           == 1224u);
    assert(battery_charge_supervisor_service_configure(
               BATTERY_CHARGE_POLICY_ENFORCE_BOOTSTRAP, 999u, true) ==
           BATTERY_CHARGE_CONFIG_INVALID_ARGUMENT);
    assert(battery_charge_supervisor_service_configure(
               BATTERY_CHARGE_POLICY_ENFORCE_BOOTSTRAP, 1224u, true) ==
           BATTERY_CHARGE_CONFIG_SESSION_ACTIVE);

    s_board.charger_connected = false;
    s_gauge.charger_connected = false;
    assert(battery_charge_supervisor_service_configure(
               BATTERY_CHARGE_POLICY_ENFORCE_BOOTSTRAP, 1300u, false) ==
           BATTERY_CHARGE_CONFIG_OK);
    battery_charge_supervisor_service_snapshot_t service = snapshot();
    assert(service.model.policy ==
           BATTERY_CHARGE_POLICY_ENFORCE_BOOTSTRAP);
    assert(service.persisted.configured_policy ==
           BATTERY_CHARGE_POLICY_ENFORCE_BOOTSTRAP);
    assert(service.persisted.configured_charge_factor_permille == 1300u);
    assert(!service.persisted.configured_charge_factor_confident);
    assert(battery_charge_supervisor_service_effective_charge_factor_permille()
           == 1300u);
    assert(s_store_dirty);

    s_store_ready = false;
    assert(battery_charge_supervisor_service_configure(
               BATTERY_CHARGE_POLICY_OBSERVE, 1224u, false) ==
           BATTERY_CHARGE_CONFIG_STORE_NOT_READY);
}

static void test_prior_capacity_keeps_only_safety_authority_without_soc(void) {
    reset_fixture();
    seed_policy(BATTERY_CHARGE_POLICY_ENFORCE_ANCHORED, false);
    s_learner.model.learned_capacity_valid = false;
    s_learner.model.learned_capacity_mah = 0u;
    s_learner.model.capacity_confidence = BATTERY_CAPACITY_CONFIDENCE_PRIOR;
    s_learner.model.remaining_capacity_valid = false;
    s_learner.model.remaining_capacity_mah = 0u;
    s_learner.model.remaining_capacity_nah = 0u;
    s_learner.model.soc_capacity_mah = 0u;
    s_learner.model.soc_provenance = BATTERY_SOC_PROVENANCE_UNKNOWN;
    s_learner.model.soc_confidence = BATTERY_SOC_CONFIDENCE_NONE;

    uint32_t result = battery_charge_supervisor_service_poll(1000u);
    assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_ADMITTED) != 0u);
    battery_charge_supervisor_snapshot_t model = snapshot().model;
    assert(model.frozen_capacity_valid);
    assert(model.frozen_capacity_mah == 1225u);
    assert(model.deficit_valid);
    assert(model.target_valid);
    assert(model.target_full_capacity);
    assert((model.blockers & BATTERY_CHARGE_BLOCK_CAPACITY_CONFIDENCE) != 0u);
    assert(model.safety_input_nah == UINT64_C(2143750000));
}

static void test_learned_capacity_stops_charge_without_starting_soc(void) {
    reset_fixture();
    seed_policy(BATTERY_CHARGE_POLICY_ENFORCE_ANCHORED, true);
    s_learner.model.remaining_capacity_valid = false;
    s_learner.model.remaining_capacity_mah = 0u;
    s_learner.model.remaining_capacity_nah = 0u;
    s_learner.model.soc_capacity_mah = 0u;
    s_learner.model.soc_provenance = BATTERY_SOC_PROVENANCE_UNKNOWN;
    s_learner.model.soc_confidence = BATTERY_SOC_CONFIDENCE_NONE;

    uint32_t result = battery_charge_supervisor_service_poll(1000u);
    assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_ADMITTED) != 0u);
    battery_charge_supervisor_snapshot_t model = snapshot().model;
    assert(model.target_full_capacity);
    assert(model.target_input_nah == UINT64_C(1224000000));
    assert(model.blockers == 0u);
    mark_store_durable();

    s_gauge.sample_sequence++;
    s_gauge.session_delta_nah = model.start_session_delta_nah +
                                (int64_t)model.target_input_nah;
    result = battery_charge_supervisor_service_poll(2000u);
    assert(result == BATTERY_CHARGE_SUPERVISOR_RESULT_STOP_REQUESTED);
    assert(s_stored.supervisor_inhibit_latched);
    assert(s_control_set_calls == 0u);

    mark_store_durable();
    result = battery_charge_supervisor_service_poll(3000u);
    assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_SOFTWARE_STOPPED) != 0u);
    assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_FULL_QUALIFIED) != 0u);
    assert(!s_control.actual_enabled);
    assert(snapshot().model.full_qualified);
}

static void test_unconfirmed_capacity_exhaustion_cannot_stop_charge(void) {
    reset_fixture();
    seed_policy(BATTERY_CHARGE_POLICY_ENFORCE_ANCHORED, true);
    s_learner.model.capacity_prediction_exhausted = true;
    s_learner.model.capacity_overrun_nah = UINT64_C(50000000);
    s_learner.model.remaining_capacity_valid = true;
    s_learner.model.remaining_capacity_mah = 0u;
    s_learner.model.remaining_capacity_nah = 0u;

    uint32_t result = battery_charge_supervisor_service_poll(1000u);
    assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_ADMITTED) != 0u);
    battery_charge_supervisor_snapshot_t model = snapshot().model;
    assert(model.frozen_capacity_valid);
    assert(model.frozen_capacity_mah == 1000u);
    assert(model.frozen_capacity_confidence ==
           BATTERY_CAPACITY_CONFIDENCE_CONFLICTED);
    assert(!model.frozen_remaining_valid);
    assert(model.target_input_nah == UINT64_C(1224000000));
    assert((model.blockers & BATTERY_CHARGE_BLOCK_CAPACITY_CONFIDENCE) != 0u);
    mark_store_durable();

    /* Reaching the old estimate-derived target may remain visible as a
     * diagnostic candidate, but it cannot request or actuate a stop. */
    s_gauge.sample_sequence++;
    s_gauge.session_delta_nah = model.start_session_delta_nah +
                                (int64_t)model.target_input_nah;
    result = battery_charge_supervisor_service_poll(2000u);
    model = snapshot().model;
    assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_STOP_REQUESTED) == 0u);
    assert(model.phase == BATTERY_CHARGE_PHASE_CHARGING);
    assert(model.candidate ==
           BATTERY_CHARGE_CANDIDATE_SOFTWARE_COULOMB_FULL);
    assert((model.blockers & BATTERY_CHARGE_BLOCK_CAPACITY_CONFIDENCE) != 0u);
    assert(s_control_set_calls == 0u);
    assert(s_control.actual_enabled);

    /* Conflicted software evidence does not block genuine BQ completion. */
    s_board.charge_state = BOARD_DIAG_CHARGE_FULL;
    s_gauge.sample_sequence++;
    result = battery_charge_supervisor_service_poll(3000u);
    assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_BQ_COMPLETE) == 0u);
    assert(snapshot().model.full_candidate);

    s_gauge.sample_sequence++;
    result = battery_charge_supervisor_service_poll(5000u);
    assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_BQ_COMPLETE) != 0u);
    assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_FULL_QUALIFIED) != 0u);
    assert(snapshot().model.phase == BATTERY_CHARGE_PHASE_COMPLETE_BQ);
    assert(snapshot().model.full_qualified);
}

static void test_stop_is_durable_before_inhibit_and_release_is_fail_open(
    void) {
    reset_fixture();
    seed_policy(BATTERY_CHARGE_POLICY_ENFORCE_ANCHORED, true);
    uint32_t result = battery_charge_supervisor_service_poll(1000u);
    assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_ADMITTED) != 0u);
    battery_charge_supervisor_snapshot_t model = snapshot().model;
    mark_store_durable();

    s_gauge.sample_sequence++;
    s_gauge.session_delta_nah = model.start_session_delta_nah +
                                (int64_t)model.target_input_nah;
    result = battery_charge_supervisor_service_poll(2000u);
    assert(result == BATTERY_CHARGE_SUPERVISOR_RESULT_STOP_REQUESTED);
    assert(s_stored.supervisor_inhibit_latched);
    assert(s_store_dirty);
    assert(s_control_set_calls == 0u);
    assert(s_control.actual_enabled);

    result = battery_charge_supervisor_service_poll(2500u);
    assert(result == BATTERY_CHARGE_SUPERVISOR_RESULT_NONE);
    assert(s_control_set_calls == 0u);
    assert(snapshot().model.phase == BATTERY_CHARGE_PHASE_STOPPING);

    mark_store_durable();
    result = battery_charge_supervisor_service_poll(3000u);
    assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_SOFTWARE_STOPPED) != 0u);
    assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_TERMINAL) != 0u);
    assert(s_control_set_calls == 1u);
    assert(!s_control.actual_enabled);
    assert(s_stored.supervisor_inhibit_latched);
    assert(s_stored.last_terminal_reason ==
           BATTERY_CHARGE_TERMINAL_SOFTWARE_COULOMB_FULL);
    assert(snapshot().model.full_qualified);
    assert(battery_charge_supervisor_service_boot_inhibit_required());

    mark_store_durable();
    battery_charge_supervisor_service_init();
    result = battery_charge_supervisor_service_poll(4000u);
    assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_RESTORED) != 0u);
    assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_SOFTWARE_STOPPED) != 0u);
    assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_FULL_QUALIFIED) != 0u);
    assert(snapshot().model.phase ==
           BATTERY_CHARGE_PHASE_COMPLETE_SOFTWARE);

    s_board.charger_connected = false;
    s_gauge.charger_connected = false;
    s_gauge.charge_active = false;
    s_gauge.sample_sequence++;
    result = battery_charge_supervisor_service_poll(5000u);
    assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_DETACHED) != 0u);
    assert(!s_stored.supervisor_inhibit_latched);
    assert(!snapshot().release_inhibit_pending);
    assert(s_control.actual_enabled);
    assert(s_store_dirty);

    (void)battery_charge_supervisor_service_poll(5500u);
    assert(!snapshot().release_inhibit_pending);
    assert(s_control.actual_enabled);
    mark_store_durable();
    (void)battery_charge_supervisor_service_poll(6000u);
    assert(!snapshot().release_inhibit_pending);
    assert(s_control.actual_enabled);
    assert(!battery_charge_supervisor_service_boot_inhibit_required());
}

static void test_recovered_empty_capacity_reaches_durable_exact_stop(void) {
    reset_fixture();
    seed_policy(BATTERY_CHARGE_POLICY_ENFORCE_ANCHORED, true);
    /* Recovery appends 1301 mAh beside the existing 1242 mAh sample; the
     * learner's rounded two-sample estimate is therefore 1272 mAh. Model the
     * following natural EMPTY admission exactly as the hardware trial will. */
    s_learner.model.learned_capacity_mah = 1272u;
    s_learner.model.remaining_capacity_valid = true;
    s_learner.model.remaining_capacity_mah = 0u;
    s_learner.model.remaining_capacity_nah = 0u;
    s_learner.model.soc_capacity_mah = 1272u;
    s_learner.model.soc_provenance =
        BATTERY_SOC_PROVENANCE_ANCHORED_EMPTY;
    s_learner.model.soc_confidence = BATTERY_SOC_CONFIDENCE_ANCHORED;
    s_learner.model.natural_empty_valid = true;

    uint32_t result = battery_charge_supervisor_service_poll(1000u);
    assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_ADMITTED) != 0u);
    battery_charge_supervisor_snapshot_t model = snapshot().model;
    assert(model.deficit_nah == UINT64_C(1272000000));
    assert(model.target_input_nah == UINT64_C(1556928000));
    assert(model.blockers == 0u);
    mark_store_durable();

    s_gauge.sample_sequence++;
    s_gauge.session_delta_nah = model.start_session_delta_nah +
                                (int64_t)model.target_input_nah - 1;
    result = battery_charge_supervisor_service_poll(2000u);
    assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_STOP_REQUESTED) == 0u);
    assert(snapshot().model.phase == BATTERY_CHARGE_PHASE_CHARGING);
    assert(s_control_set_calls == 0u);

    s_gauge.sample_sequence++;
    s_gauge.session_delta_nah++;
    result = battery_charge_supervisor_service_poll(3000u);
    assert(result == BATTERY_CHARGE_SUPERVISOR_RESULT_STOP_REQUESTED);
    assert(s_stored.supervisor_inhibit_latched);
    assert(s_store_dirty);
    assert(s_control_set_calls == 0u);
    assert(s_control.actual_enabled);

    mark_store_durable();
    result = battery_charge_supervisor_service_poll(4000u);
    assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_SOFTWARE_STOPPED) != 0u);
    assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_TERMINAL) != 0u);
    assert(s_control_set_calls == 1u);
    assert(!s_control.actual_enabled);
    assert(snapshot().model.phase ==
           BATTERY_CHARGE_PHASE_COMPLETE_SOFTWARE);
    assert(snapshot().model.full_qualified);
    assert(s_stored.last_terminal_reason ==
           BATTERY_CHARGE_TERMINAL_SOFTWARE_COULOMB_FULL);
}

static void test_control_failure_retries_without_losing_stop_latch(void) {
    reset_fixture();
    seed_policy(BATTERY_CHARGE_POLICY_ENFORCE_ANCHORED, true);
    (void)battery_charge_supervisor_service_poll(1000u);
    battery_charge_supervisor_snapshot_t model = snapshot().model;
    mark_store_durable();
    s_gauge.sample_sequence++;
    s_gauge.session_delta_nah = model.start_session_delta_nah +
                                (int64_t)model.target_input_nah;
    (void)battery_charge_supervisor_service_poll(2000u);
    mark_store_durable();

    s_control_set_ok = false;
    uint32_t result = battery_charge_supervisor_service_poll(3000u);
    assert(result == BATTERY_CHARGE_SUPERVISOR_RESULT_NONE);
    assert(snapshot().model.phase == BATTERY_CHARGE_PHASE_STOPPING);
    assert(snapshot().control_failures == 1u);
    assert(s_stored.supervisor_inhibit_latched);
    assert(s_control.actual_enabled);

    s_control_set_ok = true;
    result = battery_charge_supervisor_service_poll(4000u);
    assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_SOFTWARE_STOPPED) != 0u);
    assert(!s_control.actual_enabled);
}

static void test_battery_floor_release_does_not_wait_for_flash(void) {
    (void)prepare_durable_software_full();
    assert(s_stored.supervisor_inhibit_latched);
    assert(!s_control.actual_enabled);
    uint32_t control_calls = s_control_set_calls;

    s_set_status = STORE_STATUS_STORAGE_ERROR;
    assert(battery_charge_supervisor_service_release_for_battery_floor());
    battery_charge_supervisor_service_snapshot_t service = snapshot();
    assert(service.persistence_pending);
    assert(!service.persisted.supervisor_inhibit_latched);
    assert(!service.release_inhibit_pending);
    assert(service.maintenance_phase ==
           BATTERY_CHARGE_MAINTENANCE_WAIT_RELEASE);
    assert(s_stored.supervisor_inhibit_latched);
    assert(s_control_set_calls == control_calls + 1u);
    assert(s_control.actual_enabled);

    /* Repeated LOW polls are idempotent while the cleared record retries. */
    assert(!battery_charge_supervisor_service_release_for_battery_floor());
    assert(s_control_set_calls == control_calls + 1u);

    s_set_status = STORE_STATUS_OK;
    (void)battery_charge_supervisor_service_poll(4000u);
    assert(!snapshot().persistence_pending);
    assert(!s_stored.supervisor_inhibit_latched);
    assert(s_store_dirty);
    mark_store_durable();
    s_gauge.sample_sequence++;
    uint32_t result = battery_charge_supervisor_service_poll(5000u);
    assert((result &
            BATTERY_CHARGE_SUPERVISOR_RESULT_MAINTENANCE_RESTARTED) != 0u);
    assert(snapshot().maintenance_phase ==
           BATTERY_CHARGE_MAINTENANCE_IDLE);
}

static uint32_t prepare_durable_software_full(void) {
    reset_fixture();
    seed_policy(BATTERY_CHARGE_POLICY_ENFORCE_ANCHORED, true);
    uint32_t result = battery_charge_supervisor_service_poll(1000u);
    assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_ADMITTED) != 0u);
    battery_charge_supervisor_snapshot_t model = snapshot().model;
    mark_store_durable();

    s_gauge.sample_sequence++;
    s_gauge.session_delta_nah = model.start_session_delta_nah +
                                (int64_t)model.target_input_nah;
    result = battery_charge_supervisor_service_poll(2000u);
    assert(result == BATTERY_CHARGE_SUPERVISOR_RESULT_STOP_REQUESTED);
    mark_store_durable();
    result = battery_charge_supervisor_service_poll(3000u);
    assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_FULL_QUALIFIED) != 0u);
    assert(!s_control.actual_enabled);
    assert(s_stored.last_terminal_full_qualified);
    mark_store_durable();

    s_gauge.charge_active = false;
    s_gauge.current_ua = -50000;
    s_board.charge_state = BOARD_DIAG_CHARGE_DISABLED;
    return snapshot().model.charge_generation;
}

static void test_maintenance_rearm_requires_both_exact_boundaries(void) {
    uint32_t completed_generation = prepare_durable_software_full();
    s_learner.model.remaining_capacity_mah = 800u;
    s_learner.model.remaining_capacity_nah = UINT64_C(800000001);
    s_gauge.reference_mv = 2599u;
    s_gauge.sample_sequence++;
    uint32_t result = battery_charge_supervisor_service_poll(4000u);
    assert((result &
            BATTERY_CHARGE_SUPERVISOR_RESULT_MAINTENANCE_ARMED) == 0u);
    assert(!snapshot().maintenance_soc_ready);

    s_learner.model.remaining_capacity_nah = UINT64_C(800000000);
    s_gauge.terminal_mv = 2400u;
    s_gauge.reference_mv = 2600u;
    s_gauge.sample_sequence++;
    result = battery_charge_supervisor_service_poll(5000u);
    assert((result &
            BATTERY_CHARGE_SUPERVISOR_RESULT_MAINTENANCE_ARMED) == 0u);
    assert(snapshot().maintenance_soc_ready);
    assert(!snapshot().maintenance_voltage_ready);

    s_gauge.reference_mv = 2599u;
    s_gauge.sample_sequence++;
    result = battery_charge_supervisor_service_poll(6000u);
    assert((result &
            BATTERY_CHARGE_SUPERVISOR_RESULT_MAINTENANCE_ARMED) != 0u);
    assert(s_stored.maintenance_rearm_pending);
    assert(s_stored.supervisor_inhibit_latched);
    assert(s_stored.latched_stop_reason ==
           BATTERY_CHARGE_TERMINAL_MAINTENANCE_REARM);
    assert(!s_stored.last_terminal_full_qualified);
    assert(snapshot().maintenance_phase ==
           BATTERY_CHARGE_MAINTENANCE_WAIT_INHIBIT);

    /* The already-inhibited software-FULL path still waits for the transition
     * record to become durable, then gives the BQ a full 1 s /CE reset. */
    (void)battery_charge_supervisor_service_poll(6100u);
    assert(snapshot().maintenance_phase ==
           BATTERY_CHARGE_MAINTENANCE_WAIT_INHIBIT);
    mark_store_durable();
    (void)battery_charge_supervisor_service_poll(6200u);
    assert(snapshot().maintenance_phase ==
           BATTERY_CHARGE_MAINTENANCE_RESET_HOLD);
    (void)battery_charge_supervisor_service_poll(7199u);
    assert(snapshot().maintenance_phase ==
           BATTERY_CHARGE_MAINTENANCE_RESET_HOLD);
    (void)battery_charge_supervisor_service_poll(7200u);
    assert(snapshot().maintenance_phase ==
           BATTERY_CHARGE_MAINTENANCE_WAIT_RELEASE);
    assert(!snapshot().release_inhibit_pending);
    assert(s_control.actual_enabled);

    (void)battery_charge_supervisor_service_poll(7300u);
    assert(s_control.actual_enabled);
    mark_store_durable();
    s_gauge.charge_active = true;
    s_gauge.current_ua = 100000;
    s_gauge.sample_sequence++;
    result = battery_charge_supervisor_service_poll(8000u);
    assert((result &
            BATTERY_CHARGE_SUPERVISOR_RESULT_MAINTENANCE_RESTARTED) != 0u);
    assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_ATTACHED) != 0u);
    assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_ADMITTED) != 0u);
    battery_charge_supervisor_service_snapshot_t service = snapshot();
    assert(service.maintenance_phase == BATTERY_CHARGE_MAINTENANCE_IDLE);
    assert(service.maintenance_rearm_events == 1u);
    assert(service.model.charge_generation == completed_generation + 1u);
    assert(service.model.deficit_nah == UINT64_C(200000000));
    assert(service.model.target_input_nah == UINT64_C(244800000));
    assert(s_control.actual_enabled);
    assert(!s_stored.maintenance_rearm_pending);
    assert(s_stored.completion_rearm_used);

    /* A genuinely qualified completion closes the guarded interval and earns
     * a future maintenance cycle while the charger remains attached. */
    mark_store_durable();
    s_gauge.session_delta_nah = service.model.start_session_delta_nah +
                                (int64_t)service.model.target_input_nah;
    s_gauge.sample_sequence++;
    result = battery_charge_supervisor_service_poll(9000u);
    assert(result == BATTERY_CHARGE_SUPERVISOR_RESULT_STOP_REQUESTED);
    mark_store_durable();
    result = battery_charge_supervisor_service_poll(10000u);
    assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_FULL_QUALIFIED) != 0u);
    assert(!s_stored.completion_rearm_used);
}

static void test_maintenance_rearm_survives_reset_and_foreign_owner_blocks(
    void) {
    (void)prepare_durable_software_full();
    s_learner.model.remaining_capacity_nah = UINT64_C(800000000);
    s_gauge.reference_mv = 2599u;
    s_gauge.sample_sequence++;

    s_control.inhibit_owner_mask |= CHARGER_INHIBIT_DEBUG;
    s_board.charger_inhibit_owner_mask = s_control.inhibit_owner_mask;
    uint32_t result = battery_charge_supervisor_service_poll(4000u);
    assert((result &
            BATTERY_CHARGE_SUPERVISOR_RESULT_MAINTENANCE_ARMED) == 0u);
    assert(!s_stored.maintenance_rearm_pending);

    s_control.inhibit_owner_mask &= (uint8_t)~CHARGER_INHIBIT_DEBUG;
    s_board.charger_inhibit_owner_mask = s_control.inhibit_owner_mask;
    result = battery_charge_supervisor_service_poll(5000u);
    assert((result &
            BATTERY_CHARGE_SUPERVISOR_RESULT_MAINTENANCE_ARMED) != 0u);
    mark_store_durable();

    battery_charge_supervisor_service_init();
    battery_charge_supervisor_service_snapshot_t service = snapshot();
    assert(service.maintenance_phase ==
           BATTERY_CHARGE_MAINTENANCE_WAIT_INHIBIT);
    assert(!service.restore_pending);
    assert(battery_charge_supervisor_service_boot_inhibit_required());

    (void)battery_charge_supervisor_service_poll(6000u);
    assert(snapshot().maintenance_phase ==
           BATTERY_CHARGE_MAINTENANCE_RESET_HOLD);
    (void)battery_charge_supervisor_service_poll(7000u);
    assert(snapshot().maintenance_phase ==
           BATTERY_CHARGE_MAINTENANCE_WAIT_RELEASE);
    assert(s_store_dirty);
    assert(s_control.actual_enabled);
}

static void test_bq_full_maintenance_persists_before_ce_pulse(void) {
    reset_fixture();
    seed_policy(BATTERY_CHARGE_POLICY_ENFORCE_ANCHORED, true);
    uint32_t result = battery_charge_supervisor_service_poll(1000u);
    assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_ADMITTED) != 0u);
    mark_store_durable();

    s_board.charge_state = BOARD_DIAG_CHARGE_FULL;
    s_gauge.charge_active = false;
    s_gauge.current_ua = -10000;
    battery_charge_supervisor_snapshot_t model = snapshot().model;
    s_gauge.session_delta_nah = model.start_session_delta_nah +
                                (int64_t)model.target_input_nah;
    s_gauge.sample_sequence++;
    (void)battery_charge_supervisor_service_poll(2000u);
    s_gauge.sample_sequence++;
    result = battery_charge_supervisor_service_poll(4000u);
    assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_BQ_COMPLETE) != 0u);
    assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_FULL_QUALIFIED) != 0u);
    assert(s_control.actual_enabled);
    assert(s_stored.last_terminal_full_qualified);
    mark_store_durable();

    s_learner.model.remaining_capacity_mah = 800u;
    s_learner.model.remaining_capacity_nah = UINT64_C(800000000);
    s_gauge.reference_mv = 2599u;
    s_gauge.sample_sequence++;
    s_set_status = STORE_STATUS_STORAGE_ERROR;
    result = battery_charge_supervisor_service_poll(5000u);
    assert((result &
            BATTERY_CHARGE_SUPERVISOR_RESULT_MAINTENANCE_ARMED) != 0u);
    assert(snapshot().persistence_pending);
    assert(!s_stored.maintenance_rearm_pending);
    assert(s_control.actual_enabled);
    assert(s_control_set_calls == 0u);

    (void)battery_charge_supervisor_service_poll(6000u);
    assert(s_control.actual_enabled);
    assert(s_control_set_calls == 0u);
    s_set_status = STORE_STATUS_OK;
    (void)battery_charge_supervisor_service_poll(7000u);
    assert(s_stored.maintenance_rearm_pending);
    assert(s_store_dirty);
    assert(s_control.actual_enabled);
    mark_store_durable();
    (void)battery_charge_supervisor_service_poll(8000u);
    assert(!s_control.actual_enabled);
    assert(snapshot().maintenance_phase ==
           BATTERY_CHARGE_MAINTENANCE_RESET_HOLD);
}

static void test_unqualified_bq_full_gets_one_durable_recovery(void) {
    reset_fixture();
    seed_policy(BATTERY_CHARGE_POLICY_ENFORCE_ANCHORED, true);
    s_learner.model.remaining_capacity_mah = 600u;
    s_learner.model.remaining_capacity_nah = UINT64_C(600000000);
    s_board.charge_state = BOARD_DIAG_CHARGE_FULL;
    s_gauge.charge_active = false;
    s_gauge.current_ua = -30000;

    uint32_t result = battery_charge_supervisor_service_poll(1000u);
    assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_ATTACHED) != 0u);
    assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_ADMITTED) == 0u);
    mark_store_durable();

    s_gauge.sample_sequence++;
    result = battery_charge_supervisor_service_poll(
        1000u + BATTERY_CHARGE_COMPLETE_CONFIRM_MS);
    assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_BQ_COMPLETE) != 0u);
    assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_FULL_QUALIFIED) == 0u);
    assert((result &
            BATTERY_CHARGE_SUPERVISOR_RESULT_BQ_RECOVERY_ARMED) != 0u);
    assert(s_stored.last_terminal_reason ==
           BATTERY_CHARGE_TERMINAL_BQ_ALREADY_FULL_AT_ATTACH);
    assert(!s_stored.last_terminal_full_qualified);
    assert(s_stored.completion_rearm_used);
    assert(s_stored.maintenance_rearm_pending);
    assert(s_stored.supervisor_inhibit_latched);
    assert(s_control.actual_enabled);
    assert(snapshot().bq_recovery_events == 1u);

    /* The reset cannot touch /CE until its terminal and transaction intent are
     * durable. It then holds /CE low for the complete one-second interval. */
    (void)battery_charge_supervisor_service_poll(4000u);
    assert(s_control.actual_enabled);
    mark_store_durable();
    (void)battery_charge_supervisor_service_poll(4100u);
    assert(!s_control.actual_enabled);
    assert(snapshot().maintenance_phase ==
           BATTERY_CHARGE_MAINTENANCE_RESET_HOLD);
    (void)battery_charge_supervisor_service_poll(5099u);
    assert(!s_control.actual_enabled);
    (void)battery_charge_supervisor_service_poll(5100u);
    assert(s_control.actual_enabled);
    assert(snapshot().maintenance_phase ==
           BATTERY_CHARGE_MAINTENANCE_WAIT_RELEASE);
    mark_store_durable();

    s_board.charge_state = BOARD_DIAG_CHARGE_ACTIVE;
    s_gauge.charge_active = true;
    s_gauge.current_ua = 100000;
    s_gauge.sample_sequence++;
    result = battery_charge_supervisor_service_poll(5200u);
    assert((result &
            BATTERY_CHARGE_SUPERVISOR_RESULT_MAINTENANCE_RESTARTED) != 0u);
    assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_ATTACHED) != 0u);
    assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_ADMITTED) != 0u);
    assert(snapshot().model.target_input_nah == UINT64_C(489600000));
    assert(s_stored.completion_rearm_used);
    mark_store_durable();

    /* A second premature BQ completion in the recovered generation cannot
     * manufacture an endless reset loop. */
    s_board.charge_state = BOARD_DIAG_CHARGE_FULL;
    s_gauge.charge_active = false;
    s_gauge.current_ua = -30000;
    s_gauge.sample_sequence++;
    (void)battery_charge_supervisor_service_poll(6000u);
    s_gauge.sample_sequence++;
    result = battery_charge_supervisor_service_poll(
        6000u + BATTERY_CHARGE_COMPLETE_CONFIRM_MS);
    assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_BQ_COMPLETE) != 0u);
    assert((result &
            BATTERY_CHARGE_SUPERVISOR_RESULT_BQ_RECOVERY_ARMED) == 0u);
    assert(s_stored.completion_rearm_used);
    uint32_t control_calls = s_control_set_calls;

    s_board.charger_connected = false;
    s_gauge.charger_connected = false;
    s_gauge.sample_sequence++;
    result = battery_charge_supervisor_service_poll(9000u);
    assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_DETACHED) != 0u);
    assert(!s_stored.completion_rearm_used);
    assert(s_control_set_calls == control_calls);
}

static void test_unqualified_bq_recovery_requires_low_trusted_soc(void) {
    reset_fixture();
    seed_policy(BATTERY_CHARGE_POLICY_ENFORCE_ANCHORED, true);
    s_learner.model.remaining_capacity_mah = 800u;
    s_learner.model.remaining_capacity_nah = UINT64_C(800000001);
    s_board.charge_state = BOARD_DIAG_CHARGE_FULL;
    s_gauge.charge_active = false;
    s_gauge.current_ua = -30000;
    (void)battery_charge_supervisor_service_poll(1000u);
    mark_store_durable();
    s_gauge.sample_sequence++;
    uint32_t result = battery_charge_supervisor_service_poll(
        1000u + BATTERY_CHARGE_COMPLETE_CONFIRM_MS);
    assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_BQ_COMPLETE) != 0u);
    assert((result &
            BATTERY_CHARGE_SUPERVISOR_RESULT_BQ_RECOVERY_ARMED) == 0u);
    assert(!s_stored.completion_rearm_used);
    assert(!s_stored.maintenance_rearm_pending);

    reset_fixture();
    seed_policy(BATTERY_CHARGE_POLICY_ENFORCE_ANCHORED, true);
    s_learner.model.remaining_capacity_mah = 600u;
    s_learner.model.remaining_capacity_nah = UINT64_C(600000000);
    s_learner.model.soc_confidence = BATTERY_SOC_CONFIDENCE_PROVISIONAL;
    s_board.charge_state = BOARD_DIAG_CHARGE_FULL;
    s_gauge.charge_active = false;
    s_gauge.current_ua = -30000;
    (void)battery_charge_supervisor_service_poll(1000u);
    mark_store_durable();
    s_gauge.sample_sequence++;
    result = battery_charge_supervisor_service_poll(
        1000u + BATTERY_CHARGE_COMPLETE_CONFIRM_MS);
    assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_BQ_COMPLETE) != 0u);
    assert((result &
            BATTERY_CHARGE_SUPERVISOR_RESULT_BQ_RECOVERY_ARMED) == 0u);
    assert(!s_stored.completion_rearm_used);
}

static void test_failed_maintenance_start_does_not_pulse_forever(void) {
    (void)prepare_durable_software_full();
    s_learner.model.remaining_capacity_nah = UINT64_C(800000000);
    s_gauge.reference_mv = 2599u;
    s_gauge.sample_sequence++;
    (void)battery_charge_supervisor_service_poll(4000u);
    mark_store_durable();
    (void)battery_charge_supervisor_service_poll(5000u);
    (void)battery_charge_supervisor_service_poll(6000u);
    mark_store_durable();

    /* The reset pulse releases /CE first. The following BQ observation still
     * reports FULL, so this generation can never be admitted and cannot earn
     * another maintenance authorization. */
    s_gauge.charge_active = false;
    s_gauge.current_ua = 0;
    s_gauge.sample_sequence++;
    uint32_t result = battery_charge_supervisor_service_poll(7000u);
    assert((result &
            BATTERY_CHARGE_SUPERVISOR_RESULT_MAINTENANCE_RESTARTED) != 0u);
    assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_ATTACHED) != 0u);
    assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_ADMITTED) == 0u);
    mark_store_durable();
    s_board.charge_state = BOARD_DIAG_CHARGE_FULL;
    s_gauge.sample_sequence++;
    (void)battery_charge_supervisor_service_poll(8000u);
    s_gauge.sample_sequence++;
    (void)battery_charge_supervisor_service_poll(10000u);
    assert(snapshot().model.phase == BATTERY_CHARGE_PHASE_COMPLETE_BQ);
    assert(!snapshot().model.full_qualified);
    assert(!s_stored.last_terminal_full_qualified);
    mark_store_durable();

    uint32_t control_calls = s_control_set_calls;
    for (uint32_t now = 11000u; now <= 17000u; now += 1000u) {
        s_gauge.sample_sequence++;
        result = battery_charge_supervisor_service_poll(now);
        assert((result &
                BATTERY_CHARGE_SUPERVISOR_RESULT_MAINTENANCE_ARMED) == 0u);
        assert((result &
                BATTERY_CHARGE_SUPERVISOR_RESULT_BQ_RECOVERY_ARMED) == 0u);
    }
    assert(s_control_set_calls == control_calls);
    assert(snapshot().maintenance_rearm_events == 1u);
}

static void test_li_ion_profile_completion_restore_and_recharge(void) {
    reset_fixture();
    seed_policy(BATTERY_CHARGE_POLICY_ENFORCE_ANCHORED, true);
    s_board.chemistry = BOARD_DIAG_CHEM_LI_ION;
    s_learner.profile_id = BATTERY_LEARNING_PROFILE_LI_ION_BL5C;
    s_gauge.terminal_mv = s_gauge.reference_mv = 3900u;
    s_learner.model.nominal_capacity_mah = 1020u;
    s_learner.model.soc_capacity_mah = 1020u;
    uint32_t result = battery_charge_supervisor_service_poll(1000u);
    assert(result & BATTERY_CHARGE_SUPERVISOR_RESULT_ADMITTED);
    assert(snapshot().model.policy == BATTERY_CHARGE_POLICY_ENFORCE_LI_ION);
    assert(snapshot().model.hardware_profile_id == 4u);
    assert(snapshot().model.charge_factor_permille == 1000u);
    assert(s_stored.active_session_valid);
    assert(battery_charge_supervisor_persisted_valid(&s_stored));
    battery_charge_supervisor_service_init();
    result = battery_charge_supervisor_service_poll(2000u);
    assert(result & BATTERY_CHARGE_SUPERVISOR_RESULT_RESTORED);
    assert(battery_charge_supervisor_service_effective_charge_factor_permille() == 1000u);
    assert(battery_charge_supervisor_service_configure(
        BATTERY_CHARGE_POLICY_ENFORCE_ANCHORED, 1224u, true) ==
        BATTERY_CHARGE_CONFIG_INVALID_ARGUMENT);
    s_board.charge_state = BOARD_DIAG_CHARGE_FULL;
    s_gauge.terminal_mv = s_gauge.reference_mv = 4200u;
    s_gauge.current_ua = -10000;
    s_gauge.sample_sequence++;
    (void)battery_charge_supervisor_service_poll(3000u);
    s_gauge.sample_sequence++;
    uint32_t completed_at = 3000u + BATTERY_CHARGE_COMPLETE_CONFIRM_MS;
    result = battery_charge_supervisor_service_poll(completed_at);
    assert(result & BATTERY_CHARGE_SUPERVISOR_RESULT_FULL_QUALIFIED);
    assert(s_stored.last_terminal_full_qualified);
    assert(s_stored.supervisor_inhibit_latched);
    assert(s_stored.latched_stop_reason == BATTERY_CHARGE_TERMINAL_BQ_COMPLETE_AFTER_ACTIVE);
    assert(battery_charge_supervisor_persisted_valid(&s_stored));
    uint32_t generation = snapshot().model.charge_generation;
    uint32_t controls = s_control_set_calls;
    s_gauge.sample_sequence++;
    s_gauge.terminal_mv = s_gauge.reference_mv = 4090u;
    result = battery_charge_supervisor_service_poll(completed_at + 1000u);
    assert(!(result & BATTERY_CHARGE_SUPERVISOR_RESULT_MAINTENANCE_RESTARTED));
    assert(s_control_set_calls == controls);
    assert(s_control.actual_enabled); /* No inhibit before durable storage. */
    mark_store_durable();
    s_gauge.sample_sequence++;
    (void)battery_charge_supervisor_service_poll(completed_at + 2000u);
    assert(!s_control.actual_enabled);
    battery_charge_supervisor_service_init();
    s_gauge.sample_sequence++;
    result = battery_charge_supervisor_service_poll(completed_at + 3000u);
    assert(result & BATTERY_CHARGE_SUPERVISOR_RESULT_RESTORED);
    assert(result & BATTERY_CHARGE_SUPERVISOR_RESULT_FULL_QUALIFIED);
    assert(snapshot().model.phase == BATTERY_CHARGE_PHASE_COMPLETE_BQ);
    for (unsigned second = 4; second < 70; ++second) {
        s_gauge.sample_sequence++;
        result = battery_charge_supervisor_service_poll(completed_at + second * 1000u);
        assert(!(result & BATTERY_CHARGE_SUPERVISOR_RESULT_MAINTENANCE_ARMED));
        assert(!s_control.actual_enabled);
    }
    assert(snapshot().model.charge_generation == generation);
    s_gauge.terminal_mv = s_gauge.reference_mv = 3990u;
    for (unsigned second = 70; second < 130; ++second) {
        s_gauge.sample_sequence++;
        result = battery_charge_supervisor_service_poll(completed_at + second * 1000u);
        assert(!(result & BATTERY_CHARGE_SUPERVISOR_RESULT_MAINTENANCE_ARMED));
    }
    s_gauge.sample_sequence++;
    result = battery_charge_supervisor_service_poll(completed_at + 130000u);
    assert(result & BATTERY_CHARGE_SUPERVISOR_RESULT_MAINTENANCE_ARMED);
    assert(!s_control.actual_enabled);
}

static uint32_t li_ion_service_sample(uint32_t now_ms) {
    s_gauge.sample_sequence++;
    s_gauge.session_delta_nah += (int64_t)s_gauge.current_ua * 1000 / 3600;
    return battery_charge_supervisor_service_poll(now_ms);
}

static void setup_li_ion_taper(void) {
    reset_fixture();
    s_board.chemistry = BOARD_DIAG_CHEM_LI_ION;
    s_learner.profile_id = BATTERY_LEARNING_PROFILE_LI_ION_BL5C;
    s_gauge.gauge_session = s_learner.model.gauge_session = 0u;
    s_gauge.terminal_mv = s_gauge.reference_mv = 4180;
    s_gauge.current_ua = 18000;
    uint32_t result = battery_charge_supervisor_service_poll(1000);
    assert(result & BATTERY_CHARGE_SUPERVISOR_RESULT_ADMITTED);
    assert(snapshot().model.policy == BATTERY_CHARGE_POLICY_ENFORCE_LI_ION);
    mark_store_durable();
}

static void test_li_ion_taper_persistence_restore_and_recharge(void) {
    setup_li_ion_taper();
    int64_t baseline = s_stored.start_session_delta_nah;
    uint32_t generation = s_stored.charge_generation;
    for (unsigned second = 1; second < 180; ++second) {
        uint32_t result = li_ion_service_sample(1000 + second * 1000);
        assert(!(result & BATTERY_CHARGE_SUPERVISOR_RESULT_STOP_REQUESTED));
    }
    s_set_status = STORE_STATUS_STORAGE_ERROR;
    uint32_t result = li_ion_service_sample(181000);
    assert(result & BATTERY_CHARGE_SUPERVISOR_RESULT_STOP_REQUESTED);
    assert(snapshot().persistence_pending);
    assert(s_control.actual_enabled);
    result = li_ion_service_sample(182000);
    assert(!(result & BATTERY_CHARGE_SUPERVISOR_RESULT_FULL_QUALIFIED));
    assert(s_control.actual_enabled);
    s_set_status = STORE_STATUS_OK;
    (void)li_ion_service_sample(183000);
    assert(s_stored.supervisor_inhibit_latched && s_store_dirty);
    assert(s_control.actual_enabled);
    mark_store_durable();
    result = li_ion_service_sample(184000);
    assert(result & BATTERY_CHARGE_SUPERVISOR_RESULT_FULL_QUALIFIED);
    assert(!s_control.actual_enabled);
    assert(s_stored.last_terminal_full_qualified);
    assert(s_stored.latched_stop_reason == BATTERY_CHARGE_TERMINAL_SOFTWARE_TAPER_FULL);
    assert(battery_charge_supervisor_persisted_valid(&s_stored));
    assert(battery_charge_supervisor_service_boot_inhibit_required());
    mark_store_durable();
    battery_charge_supervisor_service_init();
    s_gauge.current_ua = -25000;
    result = li_ion_service_sample(185000);
    assert(result & BATTERY_CHARGE_SUPERVISOR_RESULT_RESTORED);
    assert(!(result & BATTERY_CHARGE_SUPERVISOR_RESULT_FULL_QUALIFIED));
    assert(snapshot().model.full_qualified); /* History is FULL, live ACR is not. */
    assert(s_stored.start_session_delta_nah == baseline);
    assert(s_stored.charge_generation == generation);
    mark_store_durable();

    /* A transient dip and the exact 4.0 V boundary cannot restart charge. */
    s_gauge.terminal_mv = s_gauge.reference_mv = 3990;
    (void)li_ion_service_sample(186000);
    s_gauge.terminal_mv = s_gauge.reference_mv = 4000;
    for (unsigned second = 187; second <= 248; ++second) {
        result = li_ion_service_sample(second * 1000);
        assert(!(result & BATTERY_CHARGE_SUPERVISOR_RESULT_MAINTENANCE_ARMED));
        assert(!s_control.actual_enabled);
    }
    s_gauge.terminal_mv = s_gauge.reference_mv = 3990;
    for (unsigned second = 249; second < 309; ++second) {
        result = li_ion_service_sample(second * 1000);
        assert(!(result & BATTERY_CHARGE_SUPERVISOR_RESULT_MAINTENANCE_ARMED));
    }
    result = li_ion_service_sample(309000);
    assert(result & BATTERY_CHARGE_SUPERVISOR_RESULT_MAINTENANCE_ARMED);
    assert(s_stored.maintenance_rearm_pending);
    assert(!s_control.actual_enabled);
    (void)li_ion_service_sample(310000);
    assert(!s_control.actual_enabled);
    mark_store_durable();
    (void)li_ion_service_sample(311000);
    (void)li_ion_service_sample(312000);
    assert(s_control.actual_enabled);
    mark_store_durable();
    s_gauge.current_ua = 100000;
    result = li_ion_service_sample(313000);
    assert(result & BATTERY_CHARGE_SUPERVISOR_RESULT_MAINTENANCE_RESTARTED);
    assert(result & BATTERY_CHARGE_SUPERVISOR_RESULT_ADMITTED);
    assert(s_stored.charge_generation == generation + 1);
    assert(!s_stored.supervisor_inhibit_latched);
    assert(snapshot().model.taper_windows == 0);
}

static void test_li_ion_recharge_rejects_interrupted_evidence(void) {
    for (unsigned failure = 0; failure < 10; ++failure) {
        setup_li_ion_taper();
        for (unsigned second = 2; second <= 181; ++second) {
            (void)li_ion_service_sample(second * 1000u);
        }
        mark_store_durable();
        uint32_t result = li_ion_service_sample(182000u);
        assert(result & BATTERY_CHARGE_SUPERVISOR_RESULT_FULL_QUALIFIED);
        mark_store_durable();
        s_gauge.current_ua = -25000;
        s_gauge.terminal_mv = s_gauge.reference_mv = 3990u;
        for (unsigned second = 183; second <= 213; ++second) {
            (void)li_ion_service_sample(second * 1000u);
        }

        battery_learning_observation_t good_gauge = s_gauge;
        board_diag_snapshot_t good_board = s_board;
        switch (failure) {
        case 0: s_gauge.terminal_mv = 4000u; break;
        case 1: s_gauge.reference_mv = 4000u; break;
        case 2: s_gauge.current_ua = 1000; break;
        case 3: s_gauge.sample_valid = false; break;
        case 4: s_gauge.continuity_valid = false; break;
        case 5: s_gauge.current_valid = false; break;
        case 6: s_board.charge_state = BOARD_DIAG_CHARGE_FAULT; break;
        case 7: s_board.charger_inhibit_owner_mask |= CHARGER_INHIBIT_DEBUG; break;
        case 8: s_board.charger_input_mv = 4200; break;
        case 9:
            s_learner.model.full_anchor_valid = true;
            battery_charge_supervisor_service_init();
            break;
        }
        result = li_ion_service_sample(214000u);
        assert(!(result & BATTERY_CHARGE_SUPERVISOR_RESULT_MAINTENANCE_ARMED));
        assert(!(result & BATTERY_CHARGE_SUPERVISOR_RESULT_FULL_QUALIFIED));
        s_gauge = good_gauge;
        s_board = good_board;
        s_gauge.sample_sequence += 2;
        unsigned ready_second = failure == 9 ? 274u : 275u;
        for (unsigned second = 215; second < ready_second; ++second) {
            result = li_ion_service_sample(second * 1000u);
            assert(!(result & BATTERY_CHARGE_SUPERVISOR_RESULT_MAINTENANCE_ARMED));
            assert(!s_control.actual_enabled);
        }
        result = li_ion_service_sample(ready_second * 1000u);
        assert(result & BATTERY_CHARGE_SUPERVISOR_RESULT_MAINTENANCE_ARMED);
    }
}

static void test_li_ion_upgrade_and_reset_discard_partial_taper(void) {
    setup_li_ion_taper();
    int64_t baseline = s_stored.start_session_delta_nah;
    for (unsigned second = 1; second <= 120; ++second) li_ion_service_sample(1000 + second * 1000);
    assert(snapshot().model.taper_windows == 2);
    s_stored.configured_policy = BATTERY_CHARGE_POLICY_OBSERVE;
    battery_charge_supervisor_service_init();
    uint32_t result = li_ion_service_sample(122000);
    assert(result & BATTERY_CHARGE_SUPERVISOR_RESULT_RESTORED);
    assert(snapshot().model.policy == BATTERY_CHARGE_POLICY_ENFORCE_LI_ION);
    assert(s_stored.start_session_delta_nah == baseline);
    assert(snapshot().model.taper_windows == 0);
    for (unsigned second = 123; second <= 245; ++second) {
        result = li_ion_service_sample(second * 1000);
        assert(!(result & BATTERY_CHARGE_SUPERVISOR_RESULT_STOP_REQUESTED));
    }
}

static void test_chemistry_change_waits_for_matching_learner(void) {
    reset_fixture();
    s_board.chemistry = BOARD_DIAG_CHEM_LI_ION;
    s_gauge.gauge_session = 0u;
    s_learner.model.gauge_session_bound = false;
    uint32_t result = battery_charge_supervisor_service_poll(1000u);
    assert(!(result & BATTERY_CHARGE_SUPERVISOR_RESULT_ADMITTED));
    assert(snapshot().model.blockers & BATTERY_CHARGE_BLOCK_STALE_ADMISSION);

    s_learner.profile_id = BATTERY_LEARNING_PROFILE_LI_ION_BL5C;
    s_learner.initialized = false;
    s_gauge.sample_sequence++;
    result = battery_charge_supervisor_service_poll(2000u);
    assert(!(result & BATTERY_CHARGE_SUPERVISOR_RESULT_ADMITTED));

    s_learner.initialized = true;
    s_gauge.sample_sequence++;
    result = battery_charge_supervisor_service_poll(3000u);
    assert(result & BATTERY_CHARGE_SUPERVISOR_RESULT_ADMITTED);
}

static void test_delayed_chemistry_restore_waits_for_learner(void) {
    for (unsigned completed = 0; completed < 2; ++completed) {
        setup_li_ion_taper();
        if (completed) {
            for (unsigned second = 2; second <= 181; ++second) {
                (void)li_ion_service_sample(second * 1000u);
            }
            mark_store_durable();
            assert(li_ion_service_sample(182000u) &
                   BATTERY_CHARGE_SUPERVISOR_RESULT_FULL_QUALIFIED);
            mark_store_durable();
        }
        battery_charge_supervisor_persisted_t saved = s_stored;
        s_board.chemistry = BOARD_DIAG_CHEM_UNKNOWN;
        s_learner.initialized = false;
        s_learner.model.gauge_session_bound = false;
        (void)battery_charge_supervisor_service_poll(183000u);
        s_board.chemistry = BOARD_DIAG_CHEM_LI_ION;
        uint32_t result = battery_charge_supervisor_service_poll(184000u);
        assert(result == BATTERY_CHARGE_SUPERVISOR_RESULT_NONE);
        assert(snapshot().restore_pending && snapshot().restore_attempts == 0u);
        assert(memcmp(&s_stored, &saved, sizeof(saved)) == 0);
        s_learner.initialized = true;
        s_learner.profile_id = BATTERY_LEARNING_PROFILE_NIMH_2S;
        result = battery_charge_supervisor_service_poll(185000u);
        assert(result == BATTERY_CHARGE_SUPERVISOR_RESULT_NONE);
        assert(snapshot().restore_pending && snapshot().restore_attempts == 0u);
        s_learner.profile_id = BATTERY_LEARNING_PROFILE_LI_ION_BL5C;
        s_learner.model.full_anchor_valid = completed != 0;
        result = battery_charge_supervisor_service_poll(186000u);
        assert(result & BATTERY_CHARGE_SUPERVISOR_RESULT_RESTORED);
        assert(!(result & BATTERY_CHARGE_SUPERVISOR_RESULT_FULL_QUALIFIED));
        assert(snapshot().restore_successes == 1u && snapshot().restore_failures == 0u);
        assert(snapshot().model.phase == (completed ? BATTERY_CHARGE_PHASE_COMPLETE_SOFTWARE
                                                   : BATTERY_CHARGE_PHASE_CHARGING));
        assert(snapshot().maintenance_full_hold == (completed != 0));
        assert(s_control.actual_enabled == (completed == 0));
        assert(memcmp(&s_stored, &saved, sizeof(saved)) == 0);
    }
}

static void test_recharge_finishes_before_returning_to_standby(void) {
    const uint32_t awake_periods_ms[] = {1000u, 5000u};
    for (unsigned cadence = 0; cadence < 2; ++cadence) {
        setup_li_ion_taper();
        for (unsigned second = 2; second <= 181; ++second) {
            (void)li_ion_service_sample(second * 1000u);
        }
        mark_store_durable();
        assert(li_ion_service_sample(182000u) & BATTERY_CHARGE_SUPERVISOR_RESULT_FULL_QUALIFIED);
        mark_store_durable();
        assert(battery_charge_supervisor_service_standby_ready());
        s_gauge.current_ua = -25000;
        s_gauge.terminal_mv = s_gauge.reference_mv = 3990u;
        uint32_t now = 182000u;
        unsigned minute_sleeps = 0u, armed = 0u, restarted = 0u;
        for (unsigned poll = 0; poll < 70; ++poll) {
            bool asleep = battery_charge_supervisor_service_standby_ready();
            minute_sleeps += asleep;
            now += asleep ? 60000u : awake_periods_ms[cadence];
            uint32_t result = li_ion_service_sample(now);
            if (result & BATTERY_CHARGE_SUPERVISOR_RESULT_MAINTENANCE_ARMED) armed++;
            if (result & BATTERY_CHARGE_SUPERVISOR_RESULT_MAINTENANCE_RESTARTED) {
                restarted++;
                break;
            }
            assert(!battery_charge_supervisor_service_standby_ready());
            mark_store_durable();
        }
        assert(minute_sleeps == 1u && armed == 1u && restarted == 1u);
        assert(s_control.actual_enabled);
        assert(battery_charge_supervisor_service_standby_ready());
    }
}

static void test_recharge_cancellation_releases_standby_hold(void) {
    for (unsigned failure = 0; failure < 5; ++failure) {
        setup_li_ion_taper();
        for (unsigned second = 2; second <= 181; ++second) {
            (void)li_ion_service_sample(second * 1000u);
        }
        mark_store_durable();
        (void)li_ion_service_sample(182000u);
        mark_store_durable();
        s_gauge.current_ua = -25000;
        s_gauge.terminal_mv = s_gauge.reference_mv = 3990u;
        (void)li_ion_service_sample(183000u);
        assert(!battery_charge_supervisor_service_standby_ready());
        if (failure == 0) s_gauge.sample_valid = s_gauge.current_valid = false;
        if (failure == 1) s_gauge.terminal_mv = 4000u;
        if (failure == 2) s_board.charger_connected = false;
        if (failure == 3) s_board.chemistry = BOARD_DIAG_CHEM_UNKNOWN;
        if (failure == 4) s_board.charge_state = BOARD_DIAG_CHARGE_FAULT;
        uint32_t result = li_ion_service_sample(184000u);
        assert(!(result & BATTERY_CHARGE_SUPERVISOR_RESULT_MAINTENANCE_ARMED));
        assert(battery_charge_supervisor_service_standby_ready());
    }
}

static void test_invalid_stop_sample_preserves_pending_full(void) {
    setup_li_ion_taper();
    for (unsigned second = 2; second <= 181; ++second) {
        (void)li_ion_service_sample(second * 1000u);
    }
    mark_store_durable();
    s_gauge.sample_valid = s_gauge.current_valid = false;
    uint32_t result = battery_charge_supervisor_service_poll(182000u);
    assert(!(result & BATTERY_CHARGE_SUPERVISOR_RESULT_TERMINAL));
    assert(snapshot().model.phase == BATTERY_CHARGE_PHASE_STOPPING);
    assert(s_stored.supervisor_inhibit_latched && !s_stored.last_terminal_full_qualified);
    assert(!s_control.actual_enabled);
    result = battery_charge_supervisor_service_poll(192000u);
    assert(!(result & BATTERY_CHARGE_SUPERVISOR_RESULT_TERMINAL));
    s_gauge.sample_valid = s_gauge.current_valid = true;
    result = li_ion_service_sample(193000u);
    assert(result & BATTERY_CHARGE_SUPERVISOR_RESULT_FULL_QUALIFIED);
    assert(s_stored.last_terminal_full_qualified);
    assert(snapshot().maintenance_full_hold);
    assert(!s_control.actual_enabled);
}

int main(void) {
    test_delayed_chemistry_restore_waits_for_learner();
    test_recharge_finishes_before_returning_to_standby();
    test_recharge_cancellation_releases_standby_hold();
    test_invalid_stop_sample_preserves_pending_full();
    test_li_ion_recharge_rejects_interrupted_evidence();
    test_li_ion_taper_persistence_restore_and_recharge();
    test_li_ion_upgrade_and_reset_discard_partial_taper();
    test_chemistry_change_waits_for_matching_learner();
    test_li_ion_profile_completion_restore_and_recharge();
    test_maps_and_freezes_precharge_evidence();
    test_natural_empty_admission_provides_full_deficit();
    test_new_gauge_session_cannot_admit_previous_pack_soc();
    test_terminal_history_rejects_untrusted_acr();
    test_shadow_comparison_and_completion();
    test_trace_ring_is_bounded_and_oldest_first();
    test_manual_inhibit_prevents_admission();
    test_persistence_retries_without_replaying_transition();
    test_active_session_restore_waits_for_complete_evidence();
    test_restore_identity_mismatch_fails_degraded_once();
    test_detached_before_restore_closes_stored_session();
    test_detach_after_terminal_retains_useful_history();
    test_unreadable_store_record_is_preserved();
    test_old_hardware_profile_evidence_is_repaired();
    test_configuration_is_explicit_and_idle_only();
    test_prior_capacity_keeps_only_safety_authority_without_soc();
    test_learned_capacity_stops_charge_without_starting_soc();
    test_unconfirmed_capacity_exhaustion_cannot_stop_charge();
    test_stop_is_durable_before_inhibit_and_release_is_fail_open();
    test_recovered_empty_capacity_reaches_durable_exact_stop();
    test_control_failure_retries_without_losing_stop_latch();
    test_battery_floor_release_does_not_wait_for_flash();
    test_maintenance_rearm_requires_both_exact_boundaries();
    test_maintenance_rearm_survives_reset_and_foreign_owner_blocks();
    test_bq_full_maintenance_persists_before_ce_pulse();
    test_unqualified_bq_full_gets_one_durable_recovery();
    test_unqualified_bq_recovery_requires_low_trusted_soc();
    test_failed_maintenance_start_does_not_pulse_forever();
    puts("battery charge supervisor service tests passed");
    return 0;
}

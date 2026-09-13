#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "services/battery_charge_supervisor_logic.h"

#define NAH_PER_MAH INT64_C(1000000)

static battery_charge_supervisor_observation_t observation(
    uint32_t now_ms, uint32_t sequence) {
    return (battery_charge_supervisor_observation_t){
        .now_ms = now_ms,
        .sample_sequence = sequence,
        .gauge_session = 7u,
        .acr_raw = UINT32_C(0x80000000),
        .session_delta_nah = 100 * NAH_PER_MAH,
        .terminal_mv = 2700u,
        .current_ua = 100000,
        .temperature_mdegc = 25000,
        .sample_valid = true,
        .current_valid = true,
        .continuity_valid = true,
        .authoritative = true,
        .charger_present = true,
        .charger_input_mv = 5000u,
        .status_valid = true,
        .charger_state = BATTERY_CHARGER_ACTIVE,
        .charger_enable_requested = true,
        .charger_enable_readback = true,
        .charger_enable_valid = true,
        .chemistry = BATTERY_CHARGE_CHEMISTRY_NIMH_2S,
        .hardware_profile_id = 3u,
        .admission = {
            .source_current = true,
            .capacity_valid = true,
            .capacity_mah = 1000u,
            .capacity_confidence = BATTERY_CAPACITY_CONFIDENCE_LEARNED,
            .remaining_valid = true,
            .remaining_mah = 200u,
            .remaining_nah = UINT64_C(200000000),
            .soc_provenance = BATTERY_SOC_PROVENANCE_TRACKED,
            .soc_confidence = BATTERY_SOC_CONFIDENCE_ANCHORED,
            .resistance_valid = true,
            .resistance_mohm = 200u,
            .pack_generation = 3u,
            .charge_factor_permille = 1224u,
        },
    };
}

static battery_charge_supervisor_snapshot_t snapshot(
    const battery_charge_supervisor_state_t *state) {
    battery_charge_supervisor_snapshot_t out;
    battery_charge_supervisor_get_snapshot(state, &out);
    return out;
}

static void admit(battery_charge_supervisor_state_t *state,
                  battery_charge_supervisor_observation_t *obs) {
    uint32_t result = battery_charge_supervisor_update(
        state, NULL, obs);
    assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_ATTACHED) != 0u);
    assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_ADMITTED) != 0u);
}

static void test_admission_freezes_basis_and_accounts_charge(void) {
    battery_charge_supervisor_state_t state;
    battery_charge_supervisor_init(&state, BATTERY_CHARGE_POLICY_OBSERVE);
    battery_charge_supervisor_observation_t obs = observation(1000u, 1u);
    admit(&state, &obs);
    battery_charge_supervisor_snapshot_t model = snapshot(&state);
    assert(model.phase == BATTERY_CHARGE_PHASE_CHARGING);
    assert(model.charge_generation == 1u);
    assert(model.pack_generation == 3u);
    assert(model.gauge_session == 7u);
    assert(model.frozen_capacity_mah == 1000u);
    assert(model.frozen_remaining_mah == 200u);
    assert(model.frozen_resistance_valid);
    assert(model.frozen_resistance_mohm == 200u);
    assert(model.deficit_valid && model.deficit_mah == 800u);
    assert(model.target_valid);
    assert(model.target_input_nah == UINT64_C(979200000));
    assert(!model.charge_factor_confident);
    assert((model.blockers & BATTERY_CHARGE_BLOCK_POLICY_OBSERVE) != 0u);
    assert((model.blockers & BATTERY_CHARGE_BLOCK_FACTOR_UNLEARNED) != 0u);

    obs.now_ms = 2000u;
    obs.sample_sequence = 2u;
    obs.session_delta_nah += 50 * NAH_PER_MAH;
    obs.admission.resistance_mohm = 900u;
    (void)battery_charge_supervisor_update(&state, NULL, &obs);
    model = snapshot(&state);
    assert(model.net_input_nah == 50 * NAH_PER_MAH);
    assert(model.compensated_mv == 2680u);
    assert(model.positive_crosscheck_nah == 27777u);

    obs.now_ms = 3000u;
    obs.sample_sequence = 3u;
    obs.current_ua = -20000;
    obs.session_delta_nah -= 5 * NAH_PER_MAH;
    (void)battery_charge_supervisor_update(&state, NULL, &obs);
    uint64_t positive = snapshot(&state).positive_crosscheck_nah;
    obs.now_ms = 3500u;
    (void)battery_charge_supervisor_update(&state, NULL, &obs);
    model = snapshot(&state);
    assert(model.positive_crosscheck_nah == positive);
    assert(model.negative_crosscheck_nah == 0u);
    obs.now_ms = 4000u;
    obs.sample_sequence = 4u;
    (void)battery_charge_supervisor_update(&state, NULL, &obs);
    model = snapshot(&state);
    assert(model.negative_crosscheck_nah == 5555u);
}

static void test_revb2_hardware_profile_identity(void) {
    const battery_charge_supervisor_profile_t *profile =
        battery_charge_supervisor_revb2_profile();
    assert(profile != NULL);
    assert(profile->chemistry == BATTERY_CHARGE_CHEMISTRY_NIMH_2S);
    assert(profile->hardware_profile_id == 3u);
    assert(profile->nominal_charge_current_ua == 273000u);
    assert(profile->nominal_backup_timer_ms == 8u * 60u * 60u * 1000u);
    assert(profile->charge_factor_prior_permille == 1224u);
    assert(profile->safety_input_factor_permille == 1750u);
    assert(profile->control_confirm_ms == 5000u);
}

static void test_stale_admission_is_blocked_and_pack_change_degrades(void) {
    battery_charge_supervisor_state_t state;
    battery_charge_supervisor_init(&state, BATTERY_CHARGE_POLICY_OBSERVE);
    battery_charge_supervisor_observation_t obs = observation(1000u, 1u);
    obs.admission.source_current = false;
    uint32_t result = battery_charge_supervisor_update(&state, NULL, &obs);
    assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_ATTACHED) != 0u);
    assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_ADMITTED) == 0u);
    assert((snapshot(&state).blockers &
            BATTERY_CHARGE_BLOCK_STALE_ADMISSION) != 0u);

    obs.now_ms++;
    obs.sample_sequence++;
    obs.admission.source_current = true;
    result = battery_charge_supervisor_update(&state, NULL, &obs);
    assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_ADMITTED) != 0u);

    obs.now_ms++;
    obs.sample_sequence++;
    obs.admission.pack_generation++;
    result = battery_charge_supervisor_update(&state, NULL, &obs);
    assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_DEGRADED) != 0u);
    assert(snapshot(&state).phase == BATTERY_CHARGE_PHASE_DEGRADED);
}

static void test_invalid_policy_fails_observe_only(void) {
    battery_charge_supervisor_state_t state;
    battery_charge_supervisor_init(
        &state, (battery_charge_supervisor_policy_t)UINT8_MAX);
    assert(snapshot(&state).policy == BATTERY_CHARGE_POLICY_OBSERVE);
    assert((snapshot(&state).blockers &
            BATTERY_CHARGE_BLOCK_POLICY_OBSERVE) != 0u);
}

static void test_bq_completion_requires_dwell_and_coulomb_corroboration(void) {
    battery_charge_supervisor_state_t state;
    battery_charge_supervisor_init(&state, BATTERY_CHARGE_POLICY_OBSERVE);
    battery_charge_supervisor_observation_t obs = observation(100u, 1u);
    admit(&state, &obs);

    obs.now_ms = 200u;
    obs.sample_sequence++;
    obs.charger_state = BATTERY_CHARGER_FULL;
    uint32_t result = battery_charge_supervisor_update(&state, NULL, &obs);
    assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_BQ_COMPLETE) == 0u);
    assert(snapshot(&state).full_candidate);

    obs.now_ms = 1200u;
    obs.sample_sequence++;
    obs.status_valid = false;
    (void)battery_charge_supervisor_update(&state, NULL, &obs);
    assert(!snapshot(&state).full_candidate);

    obs.now_ms = 1300u;
    obs.sample_sequence++;
    obs.status_valid = true;
    (void)battery_charge_supervisor_update(&state, NULL, &obs);
    obs.now_ms = 3299u;
    obs.sample_sequence++;
    result = battery_charge_supervisor_update(&state, NULL, &obs);
    assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_BQ_COMPLETE) == 0u);
    obs.now_ms = 3300u;
    obs.sample_sequence++;
    result = battery_charge_supervisor_update(&state, NULL, &obs);
    battery_charge_supervisor_snapshot_t model = snapshot(&state);
    assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_BQ_COMPLETE) != 0u);
    assert(model.phase == BATTERY_CHARGE_PHASE_COMPLETE_BQ);
    assert(model.terminal_reason ==
           BATTERY_CHARGE_TERMINAL_BQ_COMPLETE_AFTER_ACTIVE);
    assert(!model.full_qualified);
    assert((result &
            BATTERY_CHARGE_SUPERVISOR_RESULT_FULL_QUALIFIED) == 0u);

    battery_charge_supervisor_snapshot_t terminal = model;
    obs.now_ms = 70000u;
    obs.sample_sequence++;
    obs.session_delta_nah += 100 * NAH_PER_MAH;
    obs.terminal_mv = 2999u;
    obs.current_ua = 0;
    result = battery_charge_supervisor_update(&state, NULL, &obs);
    model = snapshot(&state);
    assert(result == BATTERY_CHARGE_SUPERVISOR_RESULT_NONE);
    assert(model.elapsed_ms == terminal.elapsed_ms);
    assert(model.net_input_nah == terminal.net_input_nah);
    assert(model.terminal_mv == terminal.terminal_mv);
    assert(model.current_ua == terminal.current_ua);
    assert(model.minute_count == terminal.minute_count);

    /* The same stable BQ state becomes learning authority only after the
     * independently counted replacement target has also been reached. */
    battery_charge_supervisor_init(
        &state, BATTERY_CHARGE_POLICY_OBSERVE);
    obs = observation(100u, 1u);
    obs.admission.charge_factor_confident = true;
    admit(&state, &obs);
    model = snapshot(&state);
    obs.session_delta_nah = model.start_session_delta_nah +
                            (int64_t)model.target_input_nah;
    obs.charger_state = BATTERY_CHARGER_FULL;
    obs.now_ms = 200u;
    obs.sample_sequence++;
    (void)battery_charge_supervisor_update(&state, NULL, &obs);
    obs.now_ms += BATTERY_CHARGE_COMPLETE_CONFIRM_MS;
    obs.sample_sequence++;
    result = battery_charge_supervisor_update(&state, NULL, &obs);
    assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_BQ_COMPLETE) != 0u);
    assert((result &
            BATTERY_CHARGE_SUPERVISOR_RESULT_FULL_QUALIFIED) != 0u);
    assert(snapshot(&state).full_qualified);
}

static void test_detach_cancels_full_and_bounce_reuses_generation(void) {
    battery_charge_supervisor_state_t state;
    battery_charge_supervisor_init(&state, BATTERY_CHARGE_POLICY_OBSERVE);
    battery_charge_supervisor_observation_t obs = observation(1000u, 1u);
    admit(&state, &obs);
    uint32_t generation = snapshot(&state).charge_generation;
    obs.now_ms = 1100u;
    obs.sample_sequence++;
    obs.charger_state = BATTERY_CHARGER_FULL;
    (void)battery_charge_supervisor_update(&state, NULL, &obs);

    obs.now_ms = 1500u;
    obs.sample_sequence++;
    obs.charger_present = false;
    uint32_t result = battery_charge_supervisor_update(&state, NULL, &obs);
    assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_DETACHED) != 0u);
    assert(snapshot(&state).phase == BATTERY_CHARGE_PHASE_DETACHED);
    assert(!snapshot(&state).full_qualified);

    obs = observation(1600u, 4u);
    result = battery_charge_supervisor_update(&state, NULL, &obs);
    assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_ATTACHED) != 0u);
    assert(snapshot(&state).charge_generation == generation);

    obs.charger_present = false;
    obs.now_ms = 1700u;
    obs.sample_sequence++;
    (void)battery_charge_supervisor_update(&state, NULL, &obs);
    obs = observation(2301u, 6u);
    (void)battery_charge_supervisor_update(&state, NULL, &obs);
    assert(snapshot(&state).charge_generation == generation + 1u);
}

static void test_detach_preserves_enforcement_policy(void) {
    battery_charge_supervisor_state_t state;
    battery_charge_supervisor_init(
        &state, BATTERY_CHARGE_POLICY_ENFORCE_ANCHORED);
    battery_charge_supervisor_observation_t obs = observation(1000u, 1u);
    admit(&state, &obs);
    obs.now_ms = 2000u;
    obs.sample_sequence++;
    obs.charger_present = false;
    (void)battery_charge_supervisor_update(&state, NULL, &obs);
    battery_charge_supervisor_snapshot_t model = snapshot(&state);
    assert(model.policy == BATTERY_CHARGE_POLICY_ENFORCE_ANCHORED);
    assert((model.blockers & BATTERY_CHARGE_BLOCK_POLICY_OBSERVE) == 0u);
    assert((model.blockers & BATTERY_CHARGE_BLOCK_NOT_ADMITTED) != 0u);
}

static void test_already_full_attach_is_not_qualified(void) {
    battery_charge_supervisor_state_t state;
    battery_charge_supervisor_init(&state, BATTERY_CHARGE_POLICY_OBSERVE);
    battery_charge_supervisor_observation_t obs = observation(1000u, 1u);
    obs.charger_state = BATTERY_CHARGER_FULL;
    (void)battery_charge_supervisor_update(&state, NULL, &obs);
    obs.now_ms += BATTERY_CHARGE_COMPLETE_CONFIRM_MS;
    obs.sample_sequence++;
    uint32_t result = battery_charge_supervisor_update(&state, NULL, &obs);
    battery_charge_supervisor_snapshot_t model = snapshot(&state);
    assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_BQ_COMPLETE) != 0u);
    assert(model.terminal_reason ==
           BATTERY_CHARGE_TERMINAL_BQ_ALREADY_FULL_AT_ATTACH);
    assert(!model.full_qualified);
    assert((model.blockers & BATTERY_CHARGE_BLOCK_NO_ACTIVE_HISTORY) != 0u);
}

static void test_debug_and_control_contaminate_generation(void) {
    battery_charge_supervisor_state_t state;
    battery_charge_supervisor_init(&state, BATTERY_CHARGE_POLICY_OBSERVE);
    battery_charge_supervisor_observation_t obs = observation(1000u, 1u);
    obs.authoritative = false;
    uint32_t result = battery_charge_supervisor_update(&state, NULL, &obs);
    assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_ADMITTED) == 0u);
    obs.authoritative = true;
    obs.now_ms = 2000u;
    obs.sample_sequence++;
    result = battery_charge_supervisor_update(&state, NULL, &obs);
    assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_ADMITTED) != 0u);
    assert(!snapshot(&state).session_authoritative);

    obs.now_ms = 3000u;
    obs.sample_sequence++;
    obs.inhibit_owner_mask = 1u;
    obs.charger_enable_requested = false;
    obs.charger_enable_readback = false;
    result = battery_charge_supervisor_update(&state, NULL, &obs);
    battery_charge_supervisor_snapshot_t model = snapshot(&state);
    assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_DEGRADED) != 0u);
    assert(model.phase == BATTERY_CHARGE_PHASE_DEGRADED);
    assert(model.terminal_reason ==
           BATTERY_CHARGE_TERMINAL_MANUAL_DEBUG_STOP);
}

static void test_contaminated_attach_cannot_later_request_stop(void) {
    battery_charge_supervisor_state_t state;
    battery_charge_supervisor_init(
        &state, BATTERY_CHARGE_POLICY_ENFORCE_ANCHORED);
    battery_charge_supervisor_observation_t obs = observation(1000u, 1u);
    obs.authoritative = false;
    uint32_t result = battery_charge_supervisor_update(&state, NULL, &obs);
    assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_ATTACHED) != 0u);
    assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_ADMITTED) == 0u);

    obs.authoritative = true;
    obs.admission.charge_factor_confident = true;
    obs.now_ms++;
    obs.sample_sequence++;
    result = battery_charge_supervisor_update(&state, NULL, &obs);
    assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_ADMITTED) != 0u);
    battery_charge_supervisor_snapshot_t model = snapshot(&state);
    assert(!model.session_authoritative);
    assert((model.blockers &
            BATTERY_CHARGE_BLOCK_NON_AUTHORITATIVE) != 0u);

    obs.now_ms++;
    obs.sample_sequence++;
    obs.session_delta_nah = model.start_session_delta_nah +
                            (int64_t)model.target_input_nah;
    result = battery_charge_supervisor_update(&state, NULL, &obs);
    model = snapshot(&state);
    assert((result &
            BATTERY_CHARGE_SUPERVISOR_RESULT_STOP_REQUESTED) == 0u);
    assert(model.phase == BATTERY_CHARGE_PHASE_CHARGING);
    assert((model.blockers &
            BATTERY_CHARGE_BLOCK_NON_AUTHORITATIVE) != 0u);
}

static void test_gauge_change_and_polarity_loss_degrade(void) {
    battery_charge_supervisor_state_t state;
    battery_charge_supervisor_init(&state, BATTERY_CHARGE_POLICY_OBSERVE);
    battery_charge_supervisor_observation_t obs = observation(1000u, 1u);
    admit(&state, &obs);
    obs.now_ms = 2000u;
    obs.sample_sequence++;
    obs.gauge_session++;
    uint32_t result = battery_charge_supervisor_update(&state, NULL, &obs);
    assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_DEGRADED) != 0u);
    assert(snapshot(&state).terminal_reason ==
           BATTERY_CHARGE_TERMINAL_GAUGE_CONTINUITY_LOST);
    obs.now_ms++;
    obs.sample_sequence++;
    result = battery_charge_supervisor_update(&state, NULL, &obs);
    assert((result & (BATTERY_CHARGE_SUPERVISOR_RESULT_DEGRADED |
                      BATTERY_CHARGE_SUPERVISOR_RESULT_TERMINAL)) == 0u);

    battery_charge_supervisor_init(&state, BATTERY_CHARGE_POLICY_OBSERVE);
    obs = observation(1000u, 1u);
    admit(&state, &obs);
    obs.now_ms = 2000u;
    obs.sample_sequence++;
    obs.current_valid = false;
    result = battery_charge_supervisor_update(&state, NULL, &obs);
    assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_DEGRADED) != 0u);
}

static void test_fault_is_latched_and_emitted_once(void) {
    battery_charge_supervisor_state_t state;
    battery_charge_supervisor_init(&state, BATTERY_CHARGE_POLICY_OBSERVE);
    battery_charge_supervisor_observation_t obs = observation(1000u, 1u);
    obs.charger_state = BATTERY_CHARGER_FAULT;
    uint32_t result = battery_charge_supervisor_update(&state, NULL, &obs);
    assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_TERMINAL) != 0u);
    assert(snapshot(&state).phase == BATTERY_CHARGE_PHASE_FAULT);

    obs.now_ms++;
    obs.sample_sequence++;
    result = battery_charge_supervisor_update(&state, NULL, &obs);
    assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_TERMINAL) == 0u);
    assert(snapshot(&state).phase == BATTERY_CHARGE_PHASE_FAULT);

    obs.now_ms++;
    obs.sample_sequence++;
    obs.charger_state = BATTERY_CHARGER_ACTIVE;
    result = battery_charge_supervisor_update(&state, NULL, &obs);
    assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_ADMITTED) == 0u);
    assert(snapshot(&state).phase == BATTERY_CHARGE_PHASE_FAULT);
}

static void test_minute_curve_and_coulomb_candidate(void) {
    battery_charge_supervisor_state_t state;
    battery_charge_supervisor_init(&state, BATTERY_CHARGE_POLICY_OBSERVE);
    battery_charge_supervisor_observation_t obs = observation(0u, 1u);
    admit(&state, &obs);
    uint32_t result = 0u;
    for (uint32_t second = 1u; second <= 120u; second++) {
        obs.now_ms = second * 1000u;
        obs.sample_sequence++;
        obs.terminal_mv = second <= 60u ? 2700u : 2690u;
        obs.session_delta_nah += INT64_C(33333333);
        result = battery_charge_supervisor_update(&state, NULL, &obs);
        if (second == 60u || second == 120u) {
            assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_MINUTE) != 0u);
        }
    }
    battery_charge_supervisor_snapshot_t model = snapshot(&state);
    assert(model.minute_count == 2u);
    assert(model.curve_valid);
    assert(model.curve_peak_mv == 2680u);
    assert(model.curve_drop_mv == 10u);
    assert(model.curve_slope_mv_per_min == -10);

    obs.now_ms += 1000u;
    obs.sample_sequence++;
    obs.session_delta_nah =
        model.start_session_delta_nah + (int64_t)model.target_input_nah;
    (void)battery_charge_supervisor_update(&state, NULL, &obs);
    model = snapshot(&state);
    assert(model.candidate ==
           BATTERY_CHARGE_CANDIDATE_SOFTWARE_COULOMB_FULL);
    assert((model.blockers & BATTERY_CHARGE_BLOCK_POLICY_OBSERVE) != 0u);
    assert((model.blockers & BATTERY_CHARGE_BLOCK_FACTOR_UNLEARNED) != 0u);
    assert((model.blockers & BATTERY_CHARGE_BLOCK_CURVE_UNCALIBRATED) == 0u);
}

static void test_completion_deadline_wraps(void) {
    battery_charge_supervisor_state_t state;
    battery_charge_supervisor_init(&state, BATTERY_CHARGE_POLICY_OBSERVE);
    battery_charge_supervisor_observation_t obs = observation(
        UINT32_MAX - 3000u, 1u);
    admit(&state, &obs);
    obs.now_ms = UINT32_MAX - 1000u;
    obs.sample_sequence++;
    obs.charger_state = BATTERY_CHARGER_FULL;
    (void)battery_charge_supervisor_update(&state, NULL, &obs);
    obs.now_ms = 998u;
    obs.sample_sequence++;
    uint32_t result = battery_charge_supervisor_update(&state, NULL, &obs);
    assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_BQ_COMPLETE) == 0u);
    obs.now_ms = 999u;
    obs.sample_sequence++;
    result = battery_charge_supervisor_update(&state, NULL, &obs);
    assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_BQ_COMPLETE) != 0u);
}

static battery_charge_supervisor_persisted_t persisted_session(void) {
    /* A persisted admission freezes its original factor and target; restoring
     * it must not silently replace them with the current profile prior. */
    return (battery_charge_supervisor_persisted_t){
        .hardware_profile_id = 3u,
        .chemistry = BATTERY_CHARGE_CHEMISTRY_NIMH_2S,
        .charge_generation = 9u,
        .active_session_valid = true,
        .pack_generation = 3u,
        .gauge_session = 7u,
        .start_acr_raw = UINT32_C(0x80000000),
        .start_session_delta_nah = 100 * NAH_PER_MAH,
        .frozen_capacity_valid = true,
        .frozen_capacity_mah = 1000u,
        .frozen_capacity_confidence = BATTERY_CAPACITY_CONFIDENCE_OBSERVED,
        .frozen_remaining_valid = true,
        .frozen_remaining_mah = 200u,
        .frozen_remaining_nah = UINT64_C(200000000),
        .frozen_soc_provenance = BATTERY_SOC_PROVENANCE_TRACKED,
        .frozen_soc_confidence = BATTERY_SOC_CONFIDENCE_ANCHORED,
        .frozen_resistance_valid = true,
        .frozen_resistance_mohm = 200u,
        .deficit_valid = true,
        .deficit_mah = 800u,
        .deficit_nah = UINT64_C(800000000),
        .charge_factor_permille = 1400u,
        .target_valid = true,
        .target_input_nah = UINT64_C(1120000000),
        .safety_input_nah = UINT64_C(1750000000),
    };
}

static void test_persisted_restore_requires_every_identity(void) {
    battery_charge_supervisor_persisted_t persisted = persisted_session();
    assert(battery_charge_supervisor_persisted_valid(&persisted));
    battery_charge_supervisor_observation_t obs = observation(1000u, 1u);
    obs.session_delta_nah += 50 * NAH_PER_MAH;
    battery_charge_supervisor_state_t state;
    battery_charge_supervisor_init(&state, BATTERY_CHARGE_POLICY_OBSERVE);
    assert(battery_charge_supervisor_restore_active(
        &state, BATTERY_CHARGE_POLICY_OBSERVE, NULL, &persisted, &obs));
    battery_charge_supervisor_snapshot_t model = snapshot(&state);
    assert(model.phase == BATTERY_CHARGE_PHASE_CHARGING);
    assert(model.charge_generation == 9u);
    assert(model.net_input_nah == 50 * NAH_PER_MAH);
    assert(model.frozen_resistance_mohm == 200u);

    battery_charge_supervisor_state_t unchanged = state;
    obs.gauge_session++;
    assert(!battery_charge_supervisor_restore_active(
        &state, BATTERY_CHARGE_POLICY_OBSERVE, NULL, &persisted, &obs));
    assert(memcmp(&state, &unchanged, sizeof(state)) == 0);
    obs.gauge_session--;
    obs.admission.pack_generation++;
    assert(!battery_charge_supervisor_restore_active(
        &state, BATTERY_CHARGE_POLICY_OBSERVE, NULL, &persisted, &obs));
    obs.admission.pack_generation--;
    obs.inhibit_owner_mask = 1u;
    assert(!battery_charge_supervisor_restore_active(
        &state, BATTERY_CHARGE_POLICY_OBSERVE, NULL, &persisted, &obs));
    obs.inhibit_owner_mask = 0u;
    persisted.hardware_profile_id = 2u;
    obs.hardware_profile_id = 2u;
    assert(!battery_charge_supervisor_restore_active(
        &state, BATTERY_CHARGE_POLICY_OBSERVE, NULL, &persisted, &obs));
    persisted.hardware_profile_id = 3u;
    obs.hardware_profile_id = 3u;

    persisted.active_session_valid = false;
    assert(battery_charge_supervisor_persisted_valid(&persisted));
    assert(!battery_charge_supervisor_restore_active(
        &state, BATTERY_CHARGE_POLICY_OBSERVE, NULL, &persisted, &obs));
    battery_charge_supervisor_persisted_defaults(&persisted);
    assert(battery_charge_supervisor_persisted_valid(&persisted));
    persisted.last_terminal_net_input_valid = true;
    assert(!battery_charge_supervisor_persisted_valid(&persisted));
    persisted = persisted_session();
    persisted.active_session_valid = false;
    persisted.last_terminal_valid = true;
    persisted.last_terminal_reason =
        BATTERY_CHARGE_TERMINAL_BQ_COMPLETE_AFTER_ACTIVE;
    persisted.last_terminal_generation = 8u;
    persisted.hardware_profile_id = 0u;
    assert(!battery_charge_supervisor_persisted_valid(&persisted));
    persisted = persisted_session();
    persisted.active_session_valid = false;
    persisted.last_terminal_valid = true;
    persisted.last_terminal_reason =
        BATTERY_CHARGE_TERMINAL_BQ_COMPLETE_AFTER_ACTIVE;
    persisted.last_terminal_generation = 8u;
    persisted.chemistry = BATTERY_CHARGE_CHEMISTRY_UNKNOWN;
    assert(!battery_charge_supervisor_persisted_valid(&persisted));
    battery_charge_supervisor_persisted_defaults(&persisted);
    battery_charge_supervisor_persisted_defaults(NULL);
    assert(!battery_charge_supervisor_persisted_valid(NULL));

    obs = observation(2000u, 2u);
    battery_charge_supervisor_reject_restore(
        &state, BATTERY_CHARGE_POLICY_OBSERVE, 9u, NULL, &obs);
    model = snapshot(&state);
    assert(model.phase == BATTERY_CHARGE_PHASE_DEGRADED);
    assert(model.charge_generation == 9u);
    assert(model.attached && !model.admitted);
    assert(model.terminal_reason ==
           BATTERY_CHARGE_TERMINAL_GAUGE_CONTINUITY_LOST);
    battery_charge_supervisor_state_t rejected = state;
    obs.charger_present = false;
    battery_charge_supervisor_reject_restore(
        &state, BATTERY_CHARGE_POLICY_OBSERVE, 1u, NULL, &obs);
    assert(memcmp(&state, &rejected, sizeof(state)) == 0);
}

static void test_generation_seed_only_while_detached(void) {
    battery_charge_supervisor_state_t state;
    battery_charge_supervisor_init(&state, BATTERY_CHARGE_POLICY_OBSERVE);
    battery_charge_supervisor_seed_generation(&state, 41u);
    assert(snapshot(&state).charge_generation == 41u);
    battery_charge_supervisor_observation_t obs = observation(1000u, 1u);
    admit(&state, &obs);
    assert(snapshot(&state).charge_generation == 42u);
    battery_charge_supervisor_seed_generation(&state, 2u);
    assert(snapshot(&state).charge_generation == 42u);
}

static void test_exact_sub_mah_deficit_is_preserved(void) {
    battery_charge_supervisor_state_t state;
    battery_charge_supervisor_init(&state, BATTERY_CHARGE_POLICY_OBSERVE);
    battery_charge_supervisor_observation_t obs = observation(1000u, 1u);
    obs.admission.remaining_mah = 1000u;
    obs.admission.remaining_nah = UINT64_C(999500000);
    admit(&state, &obs);

    battery_charge_supervisor_snapshot_t model = snapshot(&state);
    assert(model.frozen_remaining_mah == 1000u);
    assert(model.frozen_remaining_nah == UINT64_C(999500000));
    assert(model.deficit_mah == 1u);
    assert(model.deficit_nah == UINT64_C(500000));
    assert(model.target_input_nah == UINT64_C(612000));
}

static void test_recovered_history_capacity_trips_exact_charge_target(void) {
    battery_charge_supervisor_state_t state;
    battery_charge_supervisor_init(
        &state, BATTERY_CHARGE_POLICY_ENFORCE_ANCHORED);
    battery_charge_supervisor_observation_t obs = observation(1000u, 1u);
    /* The learner rounds the recovered 1242/1301 mAh pair to 1272 mAh. */
    obs.admission.capacity_mah = 1272u;
    obs.admission.capacity_confidence = BATTERY_CAPACITY_CONFIDENCE_OBSERVED;
    obs.admission.remaining_mah = 0u;
    obs.admission.remaining_nah = 0u;
    obs.admission.soc_provenance =
        BATTERY_SOC_PROVENANCE_ANCHORED_EMPTY;
    obs.admission.soc_confidence = BATTERY_SOC_CONFIDENCE_ANCHORED;
    obs.admission.charge_factor_permille = 1224u;
    obs.admission.charge_factor_confident = true;
    admit(&state, &obs);

    battery_charge_supervisor_snapshot_t model = snapshot(&state);
    assert(model.deficit_nah == UINT64_C(1272000000));
    assert(model.target_input_nah == UINT64_C(1556928000));
    obs.now_ms++;
    obs.sample_sequence++;
    obs.session_delta_nah = model.start_session_delta_nah +
                            (int64_t)model.target_input_nah - 1;
    uint32_t result = battery_charge_supervisor_update(&state, NULL, &obs);
    assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_STOP_REQUESTED) == 0u);
    assert(snapshot(&state).phase == BATTERY_CHARGE_PHASE_CHARGING);

    obs.now_ms++;
    obs.sample_sequence++;
    obs.session_delta_nah++;
    result = battery_charge_supervisor_update(&state, NULL, &obs);
    model = snapshot(&state);
    assert(result == BATTERY_CHARGE_SUPERVISOR_RESULT_STOP_REQUESTED);
    assert(model.phase == BATTERY_CHARGE_PHASE_STOPPING);
    assert(model.stop_reason ==
           BATTERY_CHARGE_TERMINAL_SOFTWARE_COULOMB_FULL);
}

static void test_soc_and_factor_policies_are_independent(void) {
    battery_charge_supervisor_state_t state;
    battery_charge_supervisor_observation_t obs = observation(1000u, 1u);
    obs.admission.soc_provenance =
        BATTERY_SOC_PROVENANCE_BOOTSTRAP_VOLTAGE;
    obs.admission.soc_confidence = BATTERY_SOC_CONFIDENCE_PROVISIONAL;
    obs.admission.charge_factor_confident = true;

    battery_charge_supervisor_init(
        &state, BATTERY_CHARGE_POLICY_ENFORCE_ANCHORED);
    admit(&state, &obs);
    battery_charge_supervisor_snapshot_t model = snapshot(&state);
    obs.now_ms++;
    obs.sample_sequence++;
    obs.session_delta_nah = model.start_session_delta_nah +
                            (int64_t)model.target_input_nah;
    uint32_t result = battery_charge_supervisor_update(&state, NULL, &obs);
    model = snapshot(&state);
    assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_STOP_REQUESTED) == 0u);
    assert(model.phase == BATTERY_CHARGE_PHASE_CHARGING);
    assert((model.blockers & BATTERY_CHARGE_BLOCK_SOC_CONFIDENCE) != 0u);

    obs = observation(2000u, 1u);
    obs.admission.soc_provenance =
        BATTERY_SOC_PROVENANCE_BOOTSTRAP_VOLTAGE;
    obs.admission.soc_confidence = BATTERY_SOC_CONFIDENCE_PROVISIONAL;
    obs.admission.charge_factor_confident = true;
    battery_charge_supervisor_init(
        &state, BATTERY_CHARGE_POLICY_ENFORCE_BOOTSTRAP);
    admit(&state, &obs);
    model = snapshot(&state);
    obs.now_ms++;
    obs.sample_sequence++;
    obs.session_delta_nah = model.start_session_delta_nah +
                            (int64_t)model.target_input_nah;
    result = battery_charge_supervisor_update(&state, NULL, &obs);
    assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_STOP_REQUESTED) != 0u);
    assert(snapshot(&state).phase == BATTERY_CHARGE_PHASE_STOPPING);

    obs = observation(3000u, 1u);
    battery_charge_supervisor_init(
        &state, BATTERY_CHARGE_POLICY_ENFORCE_ANCHORED);
    admit(&state, &obs);
    model = snapshot(&state);
    obs.now_ms++;
    obs.sample_sequence++;
    obs.session_delta_nah = model.start_session_delta_nah +
                            (int64_t)model.target_input_nah;
    result = battery_charge_supervisor_update(&state, NULL, &obs);
    assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_STOP_REQUESTED) == 0u);
    assert((snapshot(&state).blockers &
            BATTERY_CHARGE_BLOCK_FACTOR_UNLEARNED) != 0u);
}

static void test_durable_stop_control_preserves_authority(void) {
    battery_charge_supervisor_state_t state;
    battery_charge_supervisor_init(
        &state, BATTERY_CHARGE_POLICY_ENFORCE_ANCHORED);
    battery_charge_supervisor_observation_t obs = observation(1000u, 1u);
    obs.admission.charge_factor_confident = true;
    admit(&state, &obs);
    battery_charge_supervisor_snapshot_t model = snapshot(&state);

    obs.now_ms = 2000u;
    obs.sample_sequence++;
    obs.session_delta_nah = model.start_session_delta_nah +
                            (int64_t)model.target_input_nah;
    uint32_t result = battery_charge_supervisor_update(&state, NULL, &obs);
    assert(result == BATTERY_CHARGE_SUPERVISOR_RESULT_STOP_REQUESTED);
    assert(snapshot(&state).session_authoritative);

    obs.now_ms = 2500u;
    obs.sample_sequence++;
    result = battery_charge_supervisor_update(&state, NULL, &obs);
    assert(result == BATTERY_CHARGE_SUPERVISOR_RESULT_NONE);
    assert(snapshot(&state).phase == BATTERY_CHARGE_PHASE_STOPPING);

    obs.now_ms = 3000u;
    obs.sample_sequence++;
    obs.inhibit_owner_mask = 1u;
    obs.supervisor_inhibit_active = true;
    obs.charger_enable_requested = false;
    obs.charger_enable_readback = false;
    obs.charger_state = BATTERY_CHARGER_DETACHED;
    result = battery_charge_supervisor_update(&state, NULL, &obs);
    model = snapshot(&state);
    assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_SOFTWARE_STOPPED) != 0u);
    assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_TERMINAL) != 0u);
    assert(model.phase == BATTERY_CHARGE_PHASE_COMPLETE_SOFTWARE);
    assert(model.terminal_reason ==
           BATTERY_CHARGE_TERMINAL_SOFTWARE_COULOMB_FULL);
    assert(model.session_authoritative);
    assert(model.full_qualified);
    assert((model.blockers & BATTERY_CHARGE_BLOCK_CONTROL_UNCONFIRMED) == 0u);
}

static void test_observed_capacity_owns_full_target_without_soc(void) {
    battery_charge_supervisor_state_t state;
    battery_charge_supervisor_init(
        &state, BATTERY_CHARGE_POLICY_ENFORCE_ANCHORED);
    battery_charge_supervisor_observation_t obs = observation(1000u, 1u);
    obs.admission.remaining_valid = false;
    obs.admission.remaining_mah = 0u;
    obs.admission.remaining_nah = 0u;
    obs.admission.soc_provenance = BATTERY_SOC_PROVENANCE_UNKNOWN;
    obs.admission.soc_confidence = BATTERY_SOC_CONFIDENCE_NONE;
    obs.admission.charge_factor_confident = true;
    admit(&state, &obs);
    battery_charge_supervisor_snapshot_t model = snapshot(&state);
    assert(model.target_valid);
    assert(model.target_full_capacity);
    assert(model.deficit_nah == UINT64_C(1000000000));
    assert(model.target_input_nah == UINT64_C(1224000000));
    assert((model.blockers & BATTERY_CHARGE_BLOCK_SOC_CONFIDENCE) == 0u);
    assert(model.blockers == 0u);
    assert(model.safety_input_nah == UINT64_C(1750000000));

    obs.now_ms = 2000u;
    obs.sample_sequence++;
    obs.session_delta_nah = model.start_session_delta_nah +
                            (int64_t)model.target_input_nah;
    uint32_t result = battery_charge_supervisor_update(&state, NULL, &obs);
    model = snapshot(&state);
    assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_STOP_REQUESTED) != 0u);
    assert(model.stop_reason ==
           BATTERY_CHARGE_TERMINAL_SOFTWARE_COULOMB_FULL);

    obs.now_ms = 3000u;
    obs.sample_sequence++;
    obs.inhibit_owner_mask = 1u;
    obs.supervisor_inhibit_active = true;
    obs.charger_enable_requested = false;
    obs.charger_enable_readback = false;
    obs.charger_state = BATTERY_CHARGER_DETACHED;
    result = battery_charge_supervisor_update(&state, NULL, &obs);
    model = snapshot(&state);
    assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_SOFTWARE_STOPPED) != 0u);
    assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_FULL_QUALIFIED) != 0u);
    assert(model.phase == BATTERY_CHARGE_PHASE_COMPLETE_SOFTWARE);
    assert(model.full_qualified);
}

static void test_prior_capacity_only_retains_non_full_safety_ceiling(void) {
    battery_charge_supervisor_state_t state;
    battery_charge_supervisor_init(
        &state, BATTERY_CHARGE_POLICY_ENFORCE_ANCHORED);
    battery_charge_supervisor_observation_t obs = observation(1000u, 1u);
    obs.admission.capacity_confidence = BATTERY_CAPACITY_CONFIDENCE_PRIOR;
    obs.admission.remaining_valid = false;
    obs.admission.remaining_mah = 0u;
    obs.admission.remaining_nah = 0u;
    obs.admission.soc_provenance = BATTERY_SOC_PROVENANCE_UNKNOWN;
    obs.admission.soc_confidence = BATTERY_SOC_CONFIDENCE_NONE;
    obs.admission.charge_factor_confident = true;
    admit(&state, &obs);
    battery_charge_supervisor_snapshot_t model = snapshot(&state);
    assert(model.target_valid);
    assert(model.target_full_capacity);
    assert((model.blockers & BATTERY_CHARGE_BLOCK_CAPACITY_CONFIDENCE) != 0u);
    assert(model.safety_input_nah == UINT64_C(1750000000));

    obs.now_ms = 2000u;
    obs.sample_sequence++;
    obs.session_delta_nah = model.start_session_delta_nah +
                            (int64_t)model.target_input_nah;
    uint32_t result = battery_charge_supervisor_update(&state, NULL, &obs);
    assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_STOP_REQUESTED) == 0u);

    obs.now_ms = 3000u;
    obs.sample_sequence++;
    obs.session_delta_nah = model.start_session_delta_nah +
                            (int64_t)model.safety_input_nah;
    result = battery_charge_supervisor_update(&state, NULL, &obs);
    model = snapshot(&state);
    assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_STOP_REQUESTED) != 0u);
    assert(model.stop_reason ==
           BATTERY_CHARGE_TERMINAL_SAFETY_CHARGE_LIMIT);

    obs.now_ms = 4000u;
    obs.sample_sequence++;
    obs.inhibit_owner_mask = 1u;
    obs.supervisor_inhibit_active = true;
    obs.charger_enable_requested = false;
    obs.charger_enable_readback = false;
    obs.charger_state = BATTERY_CHARGER_DETACHED;
    result = battery_charge_supervisor_update(&state, NULL, &obs);
    model = snapshot(&state);
    assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_SOFTWARE_STOPPED) != 0u);
    assert(model.phase == BATTERY_CHARGE_PHASE_STOPPED_SAFETY);
    assert(!model.full_qualified);
}

static void test_prior_capacity_cannot_authorize_exact_deficit_stop(void) {
    battery_charge_supervisor_state_t state;
    battery_charge_supervisor_init(
        &state, BATTERY_CHARGE_POLICY_ENFORCE_ANCHORED);
    battery_charge_supervisor_observation_t obs = observation(1000u, 1u);
    obs.admission.capacity_confidence = BATTERY_CAPACITY_CONFIDENCE_PRIOR;
    obs.admission.charge_factor_confident = true;
    admit(&state, &obs);
    battery_charge_supervisor_snapshot_t model = snapshot(&state);
    assert(!model.target_full_capacity);
    assert((model.blockers &
            BATTERY_CHARGE_BLOCK_CAPACITY_CONFIDENCE) != 0u);

    obs.now_ms++;
    obs.sample_sequence++;
    obs.session_delta_nah = model.start_session_delta_nah +
                            (int64_t)model.target_input_nah;
    uint32_t result = battery_charge_supervisor_update(&state, NULL, &obs);
    assert((result &
            BATTERY_CHARGE_SUPERVISOR_RESULT_STOP_REQUESTED) == 0u);
    assert(snapshot(&state).phase == BATTERY_CHARGE_PHASE_CHARGING);

    obs.charger_state = BATTERY_CHARGER_FULL;
    obs.now_ms++;
    obs.sample_sequence++;
    (void)battery_charge_supervisor_update(&state, NULL, &obs);
    obs.now_ms += BATTERY_CHARGE_COMPLETE_CONFIRM_MS;
    obs.sample_sequence++;
    result = battery_charge_supervisor_update(&state, NULL, &obs);
    assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_BQ_COMPLETE) != 0u);
    assert((result &
            BATTERY_CHARGE_SUPERVISOR_RESULT_FULL_QUALIFIED) == 0u);
    assert(!snapshot(&state).full_qualified);
}

static void test_prior_full_target_can_corroborate_bq_terminal(void) {
    battery_charge_supervisor_state_t state;
    battery_charge_supervisor_init(&state, BATTERY_CHARGE_POLICY_OBSERVE);
    battery_charge_supervisor_observation_t obs = observation(1000u, 1u);
    obs.admission.capacity_confidence = BATTERY_CAPACITY_CONFIDENCE_PRIOR;
    obs.admission.remaining_valid = false;
    obs.admission.remaining_mah = 0u;
    obs.admission.remaining_nah = 0u;
    obs.admission.soc_provenance = BATTERY_SOC_PROVENANCE_UNKNOWN;
    obs.admission.soc_confidence = BATTERY_SOC_CONFIDENCE_NONE;
    obs.admission.charge_factor_confident = false;
    admit(&state, &obs);
    battery_charge_supervisor_snapshot_t model = snapshot(&state);
    assert(model.target_full_capacity);

    obs.now_ms++;
    obs.sample_sequence++;
    obs.session_delta_nah = model.start_session_delta_nah +
                            (int64_t)model.target_input_nah;
    obs.charger_state = BATTERY_CHARGER_FULL;
    (void)battery_charge_supervisor_update(&state, NULL, &obs);
    obs.now_ms += BATTERY_CHARGE_COMPLETE_CONFIRM_MS;
    obs.sample_sequence++;
    uint32_t result = battery_charge_supervisor_update(&state, NULL, &obs);
    assert((result &
            BATTERY_CHARGE_SUPERVISOR_RESULT_FULL_QUALIFIED) != 0u);
    assert(snapshot(&state).full_qualified);

    /* BQ25171 can expose timer completion through FAULT instead of FULL. The
     * same complete-capacity input independently corroborates that terminal. */
    battery_charge_supervisor_init(&state, BATTERY_CHARGE_POLICY_OBSERVE);
    obs = observation(5000u, 1u);
    obs.admission.capacity_confidence = BATTERY_CAPACITY_CONFIDENCE_PRIOR;
    obs.admission.remaining_valid = false;
    obs.admission.remaining_mah = 0u;
    obs.admission.remaining_nah = 0u;
    obs.admission.soc_provenance = BATTERY_SOC_PROVENANCE_UNKNOWN;
    obs.admission.soc_confidence = BATTERY_SOC_CONFIDENCE_NONE;
    admit(&state, &obs);
    model = snapshot(&state);
    obs.now_ms++;
    obs.sample_sequence++;
    obs.session_delta_nah = model.start_session_delta_nah +
                            (int64_t)model.target_input_nah;
    obs.charger_state = BATTERY_CHARGER_FAULT;
    result = battery_charge_supervisor_update(&state, NULL, &obs);
    assert((result &
            BATTERY_CHARGE_SUPERVISOR_RESULT_FULL_QUALIFIED) != 0u);
    assert(snapshot(&state).terminal_reason ==
           BATTERY_CHARGE_TERMINAL_BQ_FAULT_AFTER_COULOMB_FULL);
}

static void test_bq_fault_after_full_capacity_input_is_qualified(void) {
    battery_charge_supervisor_state_t state;
    battery_charge_supervisor_init(&state, BATTERY_CHARGE_POLICY_OBSERVE);
    battery_charge_supervisor_observation_t obs = observation(1000u, 1u);
    obs.admission.remaining_valid = false;
    obs.admission.remaining_mah = 0u;
    obs.admission.remaining_nah = 0u;
    obs.admission.soc_provenance = BATTERY_SOC_PROVENANCE_UNKNOWN;
    obs.admission.soc_confidence = BATTERY_SOC_CONFIDENCE_NONE;
    obs.admission.charge_factor_confident = false;
    admit(&state, &obs);
    battery_charge_supervisor_snapshot_t model = snapshot(&state);
    assert(model.target_full_capacity);

    obs.now_ms = 2000u;
    obs.sample_sequence++;
    obs.session_delta_nah = model.start_session_delta_nah +
                            (int64_t)model.target_input_nah;
    uint32_t result = battery_charge_supervisor_update(&state, NULL, &obs);
    assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_STOP_REQUESTED) == 0u);
    assert(snapshot(&state).candidate ==
           BATTERY_CHARGE_CANDIDATE_SOFTWARE_COULOMB_FULL);

    obs.now_ms = 3000u;
    obs.sample_sequence++;
    obs.charger_state = BATTERY_CHARGER_FAULT;
    result = battery_charge_supervisor_update(&state, NULL, &obs);
    model = snapshot(&state);
    assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_TERMINAL) != 0u);
    assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_FULL_QUALIFIED) != 0u);
    assert(model.phase == BATTERY_CHARGE_PHASE_COMPLETE_BQ);
    assert(model.terminal_reason ==
           BATTERY_CHARGE_TERMINAL_BQ_FAULT_AFTER_COULOMB_FULL);
    assert(model.full_qualified);

    battery_charge_supervisor_init(&state, BATTERY_CHARGE_POLICY_OBSERVE);
    obs = observation(4000u, 1u);
    obs.admission.remaining_valid = false;
    obs.admission.soc_confidence = BATTERY_SOC_CONFIDENCE_NONE;
    admit(&state, &obs);
    model = snapshot(&state);
    obs.now_ms++;
    obs.sample_sequence++;
    obs.session_delta_nah = model.start_session_delta_nah +
                            (int64_t)model.target_input_nah - 1;
    obs.charger_state = BATTERY_CHARGER_FAULT;
    result = battery_charge_supervisor_update(&state, NULL, &obs);
    model = snapshot(&state);
    assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_FULL_QUALIFIED) == 0u);
    assert(model.phase == BATTERY_CHARGE_PHASE_FAULT);
    assert(model.terminal_reason == BATTERY_CHARGE_TERMINAL_BQ_FAULT);
    assert(!model.full_qualified);
}

static void test_stop_control_timeout_is_terminal_not_full(void) {
    battery_charge_supervisor_state_t state;
    battery_charge_supervisor_init(
        &state, BATTERY_CHARGE_POLICY_ENFORCE_ANCHORED);
    battery_charge_supervisor_observation_t obs = observation(1000u, 1u);
    obs.admission.charge_factor_confident = true;
    admit(&state, &obs);
    battery_charge_supervisor_snapshot_t model = snapshot(&state);
    obs.now_ms = 2000u;
    obs.sample_sequence++;
    obs.session_delta_nah = model.start_session_delta_nah +
                            (int64_t)model.target_input_nah;
    (void)battery_charge_supervisor_update(&state, NULL, &obs);

    obs.now_ms = 3000u;
    obs.sample_sequence++;
    obs.inhibit_owner_mask = 1u;
    obs.supervisor_inhibit_active = true;
    obs.charger_enable_requested = false;
    obs.charger_enable_readback = true;
    (void)battery_charge_supervisor_update(&state, NULL, &obs);
    obs.now_ms += battery_charge_supervisor_revb2_profile()->control_confirm_ms;
    obs.sample_sequence++;
    uint32_t result = battery_charge_supervisor_update(&state, NULL, &obs);
    model = snapshot(&state);
    assert((result & BATTERY_CHARGE_SUPERVISOR_RESULT_TERMINAL) != 0u);
    assert(model.phase == BATTERY_CHARGE_PHASE_FAULT);
    assert(model.terminal_reason ==
           BATTERY_CHARGE_TERMINAL_CONTROL_READBACK_FAILED);
    assert(!model.full_qualified);
}

static void test_latched_stop_restores_before_or_after_confirmation(void) {
    battery_charge_supervisor_persisted_t persisted = persisted_session();
    persisted.configured_policy = BATTERY_CHARGE_POLICY_ENFORCE_ANCHORED;
    persisted.configured_charge_factor_permille = 1400u;
    persisted.configured_charge_factor_confident = true;
    persisted.charge_factor_confident = true;
    persisted.supervisor_inhibit_latched = true;
    persisted.latched_stop_reason =
        BATTERY_CHARGE_TERMINAL_SOFTWARE_COULOMB_FULL;
    assert(battery_charge_supervisor_persisted_valid(&persisted));

    battery_charge_supervisor_observation_t obs = observation(5000u, 5u);
    obs.inhibit_owner_mask = 1u;
    obs.supervisor_inhibit_active = true;
    obs.charger_enable_requested = false;
    obs.charger_enable_readback = false;
    obs.charger_state = BATTERY_CHARGER_DETACHED;
    battery_charge_supervisor_state_t state;
    battery_charge_supervisor_init(
        &state, BATTERY_CHARGE_POLICY_ENFORCE_ANCHORED);
    assert(battery_charge_supervisor_restore_active(
        &state, BATTERY_CHARGE_POLICY_ENFORCE_ANCHORED, NULL,
        &persisted, &obs));
    assert(snapshot(&state).phase == BATTERY_CHARGE_PHASE_STOPPING);

    persisted.last_terminal_valid = true;
    persisted.last_terminal_reason =
        BATTERY_CHARGE_TERMINAL_SOFTWARE_COULOMB_FULL;
    persisted.last_terminal_generation = persisted.charge_generation;
    battery_charge_supervisor_init(
        &state, BATTERY_CHARGE_POLICY_ENFORCE_ANCHORED);
    assert(battery_charge_supervisor_restore_active(
        &state, BATTERY_CHARGE_POLICY_ENFORCE_ANCHORED, NULL,
        &persisted, &obs));
    battery_charge_supervisor_snapshot_t model = snapshot(&state);
    assert(model.phase == BATTERY_CHARGE_PHASE_COMPLETE_SOFTWARE);
    assert(model.full_qualified);
}

static uint32_t random_next(uint32_t *state) {
    *state = *state * UINT32_C(1664525) + UINT32_C(1013904223);
    return *state;
}

static void test_randomized_public_api_invariants(void) {
    battery_charge_supervisor_state_t state;
    battery_charge_supervisor_init(&state, BATTERY_CHARGE_POLICY_OBSERVE);
    battery_charge_supervisor_observation_t obs = observation(0u, 0u);
    uint32_t random = UINT32_C(0x25171200);
    const uint32_t valid_results =
        BATTERY_CHARGE_SUPERVISOR_RESULT_ATTACHED |
        BATTERY_CHARGE_SUPERVISOR_RESULT_ADMITTED |
        BATTERY_CHARGE_SUPERVISOR_RESULT_DETACHED |
        BATTERY_CHARGE_SUPERVISOR_RESULT_BQ_COMPLETE |
        BATTERY_CHARGE_SUPERVISOR_RESULT_TERMINAL |
        BATTERY_CHARGE_SUPERVISOR_RESULT_DEGRADED |
        BATTERY_CHARGE_SUPERVISOR_RESULT_MINUTE |
        BATTERY_CHARGE_SUPERVISOR_RESULT_STOP_REQUESTED |
        BATTERY_CHARGE_SUPERVISOR_RESULT_SOFTWARE_STOPPED |
        BATTERY_CHARGE_SUPERVISOR_RESULT_FULL_QUALIFIED;
    for (uint32_t i = 0u; i < 100000u; i++) {
        uint32_t bits = random_next(&random);
        obs.now_ms += random_next(&random) % 2001u;
        if ((bits & 3u) != 0u) {
            obs.sample_sequence++;
        }
        if (i != 0u && i % 20000u == 0u) {
            obs.gauge_session++;
        }
        obs.session_delta_nah +=
            (int32_t)(random_next(&random) % 2000001u) - 500000;
        obs.terminal_mv = (uint16_t)(1700u + random_next(&random) % 1301u);
        obs.current_ua =
            (int32_t)(random_next(&random) % 500001u) - 150000;
        bits = random_next(&random);
        obs.sample_valid = (bits & 1u) != 0u;
        obs.current_valid = (bits & 2u) != 0u;
        obs.continuity_valid = (bits & 4u) != 0u;
        obs.authoritative = (bits & 8u) != 0u;
        obs.charger_present = (bits & 16u) != 0u;
        obs.status_valid = (bits & 32u) != 0u;
        obs.charger_state = (battery_charger_state_t)((bits >> 6u) & 3u);
        obs.charger_enable_requested = (bits & 0x100u) != 0u;
        obs.charger_enable_readback = (bits & 0x200u) != 0u;
        obs.charger_enable_valid = (bits & 0x400u) != 0u;
        obs.inhibit_owner_mask = (uint8_t)((bits >> 12u) & 3u);
        uint32_t result = battery_charge_supervisor_update(
            &state, NULL, &obs);
        assert((result & ~valid_results) == 0u);
        battery_charge_supervisor_snapshot_t model = snapshot(&state);
        assert(model.phase <= BATTERY_CHARGE_PHASE_STOPPING);
        assert(model.terminal_reason <=
               BATTERY_CHARGE_TERMINAL_BQ_FAULT_AFTER_COULOMB_FULL);
        assert(model.candidate <=
               BATTERY_CHARGE_CANDIDATE_SOFTWARE_COULOMB_FULL);
        assert(state.minute_sample_count <=
               BATTERY_CHARGE_SUPERVISOR_MINUTE_SAMPLE_CAP);
        if (model.full_qualified) {
            assert(model.terminal_reason ==
                       BATTERY_CHARGE_TERMINAL_BQ_COMPLETE_AFTER_ACTIVE ||
                   model.terminal_reason ==
                       BATTERY_CHARGE_TERMINAL_SOFTWARE_COULOMB_FULL ||
                   model.terminal_reason ==
                       BATTERY_CHARGE_TERMINAL_BQ_FAULT_AFTER_COULOMB_FULL);
        }
    }
}

int main(void) {
    test_revb2_hardware_profile_identity();
    test_stale_admission_is_blocked_and_pack_change_degrades();
    test_invalid_policy_fails_observe_only();
    test_admission_freezes_basis_and_accounts_charge();
    test_bq_completion_requires_dwell_and_coulomb_corroboration();
    test_detach_cancels_full_and_bounce_reuses_generation();
    test_detach_preserves_enforcement_policy();
    test_already_full_attach_is_not_qualified();
    test_debug_and_control_contaminate_generation();
    test_contaminated_attach_cannot_later_request_stop();
    test_gauge_change_and_polarity_loss_degrade();
    test_fault_is_latched_and_emitted_once();
    test_minute_curve_and_coulomb_candidate();
    test_completion_deadline_wraps();
    test_persisted_restore_requires_every_identity();
    test_generation_seed_only_while_detached();
    test_exact_sub_mah_deficit_is_preserved();
    test_recovered_history_capacity_trips_exact_charge_target();
    test_soc_and_factor_policies_are_independent();
    test_durable_stop_control_preserves_authority();
    test_observed_capacity_owns_full_target_without_soc();
    test_prior_capacity_only_retains_non_full_safety_ceiling();
    test_prior_capacity_cannot_authorize_exact_deficit_stop();
    test_prior_full_target_can_corroborate_bq_terminal();
    test_bq_fault_after_full_capacity_input_is_qualified();
    test_stop_control_timeout_is_terminal_not_full();
    test_latched_stop_restores_before_or_after_confirmation();
    test_randomized_public_api_invariants();
    puts("battery charge supervisor logic tests passed");
    return 0;
}

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "apps/battery_status_logic.h"

static void test_raw_collapse_requires_distinct_samples(void) {
    battery_raw_collapse_evidence_t evidence;
    battery_raw_collapse_evidence_init(&evidence);

    assert(!battery_raw_collapse_evidence_update(
        &evidence, true, 10u, 1799u, 1800u));
    for (unsigned i = 0u; i < 20u; i++) {
        assert(!battery_raw_collapse_evidence_update(
            &evidence, true, 10u, 1700u, 1800u));
    }
    assert(battery_raw_collapse_evidence_update(
        &evidence, true, 11u, 1799u, 1800u));
    assert(battery_raw_collapse_evidence_update(
        &evidence, true, 11u, 2500u, 1800u));

    /* A new recovered sample clears the latch; equality is not below. */
    assert(!battery_raw_collapse_evidence_update(
        &evidence, true, 12u, 1800u, 1800u));
    assert(!battery_raw_collapse_evidence_update(
        &evidence, true, 13u, 1700u, 1800u));
    assert(battery_raw_collapse_evidence_update(
        &evidence, true, 14u, 1700u, 1800u));

    /* Invalid evidence breaks consecutiveness. */
    assert(!battery_raw_collapse_evidence_update(
        &evidence, false, 14u, 0u, 1800u));
    assert(!battery_raw_collapse_evidence_update(
        &evidence, true, 15u, 1700u, 1800u));
}

static void test_sequence_wrap_is_still_distinct(void) {
    battery_raw_collapse_evidence_t evidence;
    battery_raw_collapse_evidence_init(&evidence);
    assert(!battery_raw_collapse_evidence_update(
        &evidence, true, UINT32_MAX, 1700u, 1800u));
    assert(battery_raw_collapse_evidence_update(
        &evidence, true, 0u, 1700u, 1800u));
}

static void test_shutdown_floor_tracks_call_policy_without_mixing_samples(void) {
    assert(battery_empty_shutdown_threshold_mv(false) ==
           BATTERY_OPERATIONAL_EMPTY_MV);
    assert(battery_empty_shutdown_threshold_mv(true) ==
           BATTERY_RAW_COLLAPSE_MV);

    battery_raw_collapse_evidence_t evidence;
    battery_raw_collapse_evidence_init(&evidence);

    /* One idle-floor sample cannot be reused as evidence for the lower call
     * floor after the call state changes. */
    assert(!battery_raw_collapse_evidence_update(
        &evidence, true, 20u, 2050u,
        battery_empty_shutdown_threshold_mv(false)));
    assert(!battery_raw_collapse_evidence_update(
        &evidence, true, 20u, 1799u,
        battery_empty_shutdown_threshold_mv(true)));
    assert(!battery_raw_collapse_evidence_update(
        &evidence, true, 20u, 1700u,
        battery_empty_shutdown_threshold_mv(true)));
    assert(battery_raw_collapse_evidence_update(
        &evidence, true, 21u, 1799u,
        battery_empty_shutdown_threshold_mv(true)));

    /* Returning to idle likewise starts a fresh two-sample confirmation, even
     * though the first newly-applicable sample is already below 2.10 V. */
    assert(!battery_raw_collapse_evidence_update(
        &evidence, true, 21u, 2050u,
        battery_empty_shutdown_threshold_mv(false)));
    assert(!battery_raw_collapse_evidence_update(
        &evidence, true, 21u, 2000u,
        battery_empty_shutdown_threshold_mv(false)));
    assert(battery_raw_collapse_evidence_update(
        &evidence, true, 22u, 2050u,
        battery_empty_shutdown_threshold_mv(false)));

    /* The floor itself is admitted; only a sample below it is low evidence. */
    assert(!battery_raw_collapse_evidence_update(
        &evidence, true, 23u, BATTERY_OPERATIONAL_EMPTY_MV,
        BATTERY_OPERATIONAL_EMPTY_MV));
}

static battery_endpoint_evidence_t endpoint_evidence(void) {
    return (battery_endpoint_evidence_t){
        .battery_sample_valid = true,
    };
}

static void test_capacity_zero_requires_independent_voltage_corroboration(void) {
    battery_endpoint_state_t state;
    battery_endpoint_state_init(&state);
    battery_endpoint_evidence_t evidence = endpoint_evidence();
    evidence.capacity_prediction_exhausted = true;

    battery_endpoint_decision_t decision =
        battery_endpoint_step(&state, &evidence);
    assert(decision.kind == BATTERY_ENDPOINT_CAPACITY_PENDING);
    assert(!decision.shutdown);
    assert(!decision.learn_empty);
    assert(decision.force_low);
    assert(decision.capacity_voltage_disagreement);

    evidence.gauge_low = true;
    decision = battery_endpoint_step(&state, &evidence);
    assert(decision.kind == BATTERY_ENDPOINT_NATURAL_EMPTY);
    assert(decision.shutdown);
    assert(decision.learn_empty);
    assert(!decision.capacity_voltage_disagreement);

    /* Once armed, a later sample cannot demote the durable shutdown path. */
    evidence.gauge_low = false;
    evidence.shutdown_armed = true;
    decision = battery_endpoint_step(&state, &evidence);
    assert(decision.kind == BATTERY_ENDPOINT_NATURAL_EMPTY);
    assert(decision.shutdown);
    assert(!decision.learn_empty);

    evidence.charge_recovery_active = true;
    decision = battery_endpoint_step(&state, &evidence);
    assert(decision.kind == BATTERY_ENDPOINT_NONE);
    assert(!decision.shutdown);
    assert(state.armed_kind == BATTERY_ENDPOINT_NONE);
}

static void test_capacity_low_waits_out_active_call_sag(void) {
    battery_endpoint_state_t state;
    battery_endpoint_state_init(&state);
    battery_endpoint_evidence_t evidence = endpoint_evidence();
    evidence.capacity_prediction_exhausted = true;
    evidence.gauge_low = true;
    evidence.call_active = true;

    battery_endpoint_decision_t decision =
        battery_endpoint_step(&state, &evidence);
    assert(decision.kind == BATTERY_ENDPOINT_CAPACITY_PENDING);
    assert(!decision.shutdown);
    assert(!decision.learn_empty);
    assert(decision.force_low);

    /* If LOW persists after load release, the independent domains now agree. */
    evidence.call_active = false;
    decision = battery_endpoint_step(&state, &evidence);
    assert(decision.kind == BATTERY_ENDPOINT_NATURAL_EMPTY);
    assert(decision.shutdown);
    assert(decision.learn_empty);
}

static void test_call_sag_with_anchored_charge_never_becomes_empty(void) {
    battery_endpoint_state_t state;
    battery_endpoint_state_init(&state);
    battery_endpoint_evidence_t evidence = endpoint_evidence();
    evidence.call_active = true;
    evidence.gauge_empty = true;
    evidence.anchored_capacity_has_charge = true;

    battery_endpoint_decision_t decision =
        battery_endpoint_step(&state, &evidence);
    assert(decision.kind == BATTERY_ENDPOINT_LOAD_SAG);
    assert(!decision.shutdown);
    assert(!decision.learn_empty);
    assert(decision.force_low);
    assert(state.load_sag_latched);

    /* A stale EMPTY projection immediately after the call remains suppressed
     * until fresh voltage evidence actually recovers. */
    evidence.call_active = false;
    decision = battery_endpoint_step(&state, &evidence);
    assert(decision.kind == BATTERY_ENDPOINT_LOAD_SAG);
    assert(!decision.shutdown);

    evidence.gauge_empty = false;
    decision = battery_endpoint_step(&state, &evidence);
    assert(decision.kind == BATTERY_ENDPOINT_NONE);
    assert(!state.load_sag_latched);

    /* A later idle EMPTY is independent physical evidence and may be learned. */
    evidence.gauge_empty = true;
    decision = battery_endpoint_step(&state, &evidence);
    assert(decision.kind == BATTERY_ENDPOINT_NATURAL_EMPTY);
    assert(decision.shutdown && decision.learn_empty);
}

static void test_call_below_idle_floor_does_not_arm_shutdown(void) {
    battery_raw_collapse_evidence_t raw;
    battery_raw_collapse_evidence_init(&raw);
    uint16_t call_floor = battery_empty_shutdown_threshold_mv(true);
    assert(call_floor == BATTERY_RAW_COLLAPSE_MV);

    /* 2050 mV is below the 2100 mV idle floor but safely above the separate
     * in-call emergency floor. Repeated fresh samples still cannot arm it. */
    assert(!battery_raw_collapse_evidence_update(
        &raw, true, 30u, 2050u, call_floor));
    assert(!battery_raw_collapse_evidence_update(
        &raw, true, 31u, 2050u, call_floor));

    battery_endpoint_state_t state;
    battery_endpoint_state_init(&state);
    battery_endpoint_evidence_t evidence = endpoint_evidence();
    evidence.call_active = true;
    evidence.gauge_low = true;
    evidence.anchored_capacity_has_charge = true;
    evidence.operating_floor_confirmed = false;
    battery_endpoint_decision_t decision =
        battery_endpoint_step(&state, &evidence);
    assert(!decision.shutdown);
    assert(!decision.learn_empty);
    assert(decision.force_low);
}

static void test_call_emergency_floor_shuts_down_without_learning_sag(void) {
    battery_endpoint_state_t state;
    battery_endpoint_state_init(&state);
    battery_endpoint_evidence_t evidence = endpoint_evidence();
    evidence.call_active = true;
    evidence.gauge_empty = true;
    evidence.anchored_capacity_has_charge = true;
    evidence.operating_floor_confirmed = true;

    battery_endpoint_decision_t decision =
        battery_endpoint_step(&state, &evidence);
    assert(decision.kind == BATTERY_ENDPOINT_EMERGENCY_SHUTDOWN);
    assert(decision.shutdown);
    assert(!decision.learn_empty);

    battery_endpoint_state_init(&state);
    evidence.capacity_prediction_exhausted = true;
    decision = battery_endpoint_step(&state, &evidence);
    assert(decision.kind == BATTERY_ENDPOINT_NATURAL_EMPTY);
    assert(decision.shutdown && decision.learn_empty);
}

static void test_modem_collapse_is_sag_while_capacity_has_charge(void) {
    battery_endpoint_state_t state;
    battery_endpoint_state_init(&state);
    battery_endpoint_evidence_t evidence = endpoint_evidence();
    evidence.modem_supply_collapse = true;
    evidence.anchored_capacity_has_charge = true;

    battery_endpoint_decision_t decision =
        battery_endpoint_step(&state, &evidence);
    assert(decision.kind == BATTERY_ENDPOINT_LOAD_SAG);
    assert(!decision.shutdown);
    assert(!decision.learn_empty);
    assert(decision.force_low);

    battery_endpoint_state_init(&state);
    evidence.anchored_capacity_has_charge = false;
    decision = battery_endpoint_step(&state, &evidence);
    assert(decision.kind == BATTERY_ENDPOINT_NATURAL_EMPTY);
    assert(decision.shutdown && decision.learn_empty);
}

static void test_idle_early_empty_can_learn_capacity_degradation(void) {
    battery_endpoint_state_t state;
    battery_endpoint_state_init(&state);
    battery_endpoint_evidence_t evidence = endpoint_evidence();
    evidence.gauge_empty = true;
    evidence.anchored_capacity_has_charge = true;

    battery_endpoint_decision_t decision =
        battery_endpoint_step(&state, &evidence);
    assert(decision.kind == BATTERY_ENDPOINT_NATURAL_EMPTY);
    assert(decision.shutdown && decision.learn_empty);

    battery_endpoint_state_init(&state);
    evidence.gauge_empty = false;
    evidence.operating_floor_confirmed = true;
    decision = battery_endpoint_step(&state, &evidence);
    assert(decision.kind == BATTERY_ENDPOINT_NATURAL_EMPTY);
    assert(decision.shutdown && decision.learn_empty);
}

static void test_warning_action_preserves_state_across_missing_data(void) {
    assert(battery_warning_action(false, false, false, false, false) ==
           BATTERY_WARNING_ACTION_HOLD);
    assert(battery_warning_action(false, false, false, true, false) ==
           BATTERY_WARNING_ACTION_EMPTY);
    assert(battery_warning_action(true, false, false, false, true) ==
           BATTERY_WARNING_ACTION_LOW);
    assert(battery_warning_action(true, false, false, false, false) ==
           BATTERY_WARNING_ACTION_HEALTHY);
    assert(battery_warning_action(false, true, false, true, false) ==
           BATTERY_WARNING_ACTION_SUPPRESS);
    assert(battery_warning_action(false, false, true, true, false) ==
           BATTERY_WARNING_ACTION_SUPPRESS);
}

static void test_only_verified_positive_charge_suppresses_empty(void) {
    assert(!battery_charge_recovery_active(
        false, true, true, true, 100000));
    assert(!battery_charge_recovery_active(
        true, false, true, true, 100000));
    assert(!battery_charge_recovery_active(
        true, true, false, true, 100000));
    assert(!battery_charge_recovery_active(
        true, true, true, false, 100000));
    assert(!battery_charge_recovery_active(
        true, true, true, true, 0));
    assert(!battery_charge_recovery_active(
        true, true, true, true, -1));
    assert(battery_charge_recovery_active(
        true, true, true, true, 1));

    battery_endpoint_state_t state;
    battery_endpoint_state_init(&state);
    battery_endpoint_evidence_t evidence = endpoint_evidence();
    evidence.gauge_empty = true;
    battery_endpoint_decision_t decision =
        battery_endpoint_step(&state, &evidence);
    assert(decision.shutdown && decision.learn_empty);

    /* VIN with an inhibited, idle, faulted, or net-discharging charger leaves
     * this recovery bit false, so an armed EMPTY remains authoritative. */
    evidence.shutdown_armed = true;
    evidence.gauge_empty = false;
    decision = battery_endpoint_step(&state, &evidence);
    assert(decision.shutdown);

    evidence.charge_recovery_active = true;
    decision = battery_endpoint_step(&state, &evidence);
    assert(!decision.shutdown);
    assert(state.armed_kind == BATTERY_ENDPOINT_NONE);
}

static void test_sag_guard_does_not_depend_on_display_bars(void) {
    assert(battery_anchored_remaining_has_charge(
        true, true, UINT64_C(1)));
    assert(!battery_anchored_remaining_has_charge(
        false, true, UINT64_C(1)));
    assert(!battery_anchored_remaining_has_charge(
        true, false, UINT64_C(1)));
    assert(!battery_anchored_remaining_has_charge(
        true, true, UINT64_C(0)));
}

static void test_low_attached_non_recovery_releases_charge_latch(void) {
    assert(battery_charge_floor_release_needed(
        true, false, true, false));
    assert(battery_charge_floor_release_needed(
        true, false, false, true));
    assert(!battery_charge_floor_release_needed(
        false, false, true, true));
    assert(!battery_charge_floor_release_needed(
        true, true, true, true));
    assert(!battery_charge_floor_release_needed(
        true, false, false, false));
}

static void test_display_bars_hold_last_qualified_value(void) {
    assert(battery_display_bars(false, 0u, true, 3u, 1u) == 3u);
    assert(battery_display_bars(false, 0u, false, 0u, 3u) == 3u);
    assert(battery_display_bars(true, 2u, true, 4u, 1u) == 2u);
    assert(battery_display_bars(true, 9u, false, 0u, 1u) == 4u);
}

static void test_maintenance_restart_rearms_full_notice(void) {
    battery_full_notice_decision_t notice =
        battery_full_notice_step(false, false, false);
    assert(!notice.notified && !notice.show);

    notice = battery_full_notice_step(false, true, false);
    assert(notice.notified && notice.show);
    notice = battery_full_notice_step(notice.notified, true, false);
    assert(notice.notified && !notice.show);

    notice = battery_full_notice_step(notice.notified, false, true);
    assert(!notice.notified && !notice.show);
    notice = battery_full_notice_step(notice.notified, true, false);
    assert(notice.notified && notice.show);

    /* A restart wins over stale same-poll completion evidence. */
    notice = battery_full_notice_step(true, true, true);
    assert(!notice.notified && !notice.show);
}

static void test_only_supervisor_completion_teaches_full(void) {
    battery_charge_completion_decision_t completion =
        battery_charge_completion_decision(false, false, true);
    assert(!completion.ui_completed && !completion.learn_full);

    completion = battery_charge_completion_decision(true, false, true);
    assert(completion.ui_completed);
    assert(!completion.learn_full);

    completion = battery_charge_completion_decision(true, false, false);
    assert(!completion.ui_completed && !completion.learn_full);

    completion = battery_charge_completion_decision(false, true, false);
    assert(completion.ui_completed && completion.learn_full);

    completion = battery_charge_completion_decision(true, true, false);
    assert(completion.ui_completed && completion.learn_full);
}

static void test_charger_insert_tone_follows_call_context(void) {
    assert(battery_charger_insert_tone_index(false) == 10u);
    assert(battery_charger_insert_tone_index(true) == 11u);
}

int main(void) {
    test_raw_collapse_requires_distinct_samples();
    test_sequence_wrap_is_still_distinct();
    test_shutdown_floor_tracks_call_policy_without_mixing_samples();
    test_capacity_zero_requires_independent_voltage_corroboration();
    test_capacity_low_waits_out_active_call_sag();
    test_call_sag_with_anchored_charge_never_becomes_empty();
    test_call_below_idle_floor_does_not_arm_shutdown();
    test_call_emergency_floor_shuts_down_without_learning_sag();
    test_modem_collapse_is_sag_while_capacity_has_charge();
    test_idle_early_empty_can_learn_capacity_degradation();
    test_warning_action_preserves_state_across_missing_data();
    test_only_verified_positive_charge_suppresses_empty();
    test_sag_guard_does_not_depend_on_display_bars();
    test_low_attached_non_recovery_releases_charge_latch();
    test_display_bars_hold_last_qualified_value();
    test_maintenance_restart_rearms_full_notice();
    test_only_supervisor_completion_teaches_full();
    test_charger_insert_tone_follows_call_context();
    puts("battery status policy tests passed");
    return 0;
}

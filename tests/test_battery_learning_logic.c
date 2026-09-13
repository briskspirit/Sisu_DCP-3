#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "services/battery_learning_logic.h"

#define NAH_PER_MAH INT64_C(1000000)

static battery_learning_observation_t observation(
    uint32_t sequence, uint32_t now_ms, int64_t delta_nah,
    uint16_t voltage_mv, int32_t current_ua) {
    battery_learning_observation_t obs = {
        .now_ms = now_ms,
        .sample_sequence = sequence,
        .gauge_session = 0u,
        .acr_raw = 0x80000000u,
        .session_delta_nah = delta_nah,
        .terminal_mv = voltage_mv,
        .current_ua = current_ua,
        .temperature_mdegc = 25000,
        .reference_mv = voltage_mv,
        .sample_valid = true,
        .current_valid = true,
        .continuity_valid = true,
        .reference_valid = false,
        .charger_connected = false,
        .charge_active = false,
        .authoritative = true,
    };
    return obs;
}

static void full_anchor(battery_learning_state_t *state,
                        battery_learning_observation_t *obs) {
    obs->charger_connected = true;
    uint32_t result = battery_learning_update(
        state, NULL, obs, BATTERY_LEARNING_EVENT_FULL);
    assert((result & BATTERY_LEARNING_RESULT_FULL_ANCHORED) != 0u);
    assert((result & BATTERY_LEARNING_RESULT_PERSIST) != 0u);
    obs->charger_connected = false;
}

static void test_defaults_and_full_anchor_remaining(void) {
    battery_learning_state_t state;
    battery_learning_init(&state, NULL, NULL);
    battery_learning_snapshot_t snapshot;
    battery_learning_get_snapshot(&state, &snapshot);
    assert(snapshot.nominal_capacity_mah == 1225u);
    assert(!snapshot.gauge_session_bound);
    assert(snapshot.capacity_confidence ==
           BATTERY_CAPACITY_CONFIDENCE_PRIOR);
    assert(!snapshot.remaining_capacity_valid);

    battery_learning_observation_t obs = observation(
        1u, 1000u, INT64_C(100000000), 2700u, 50000);
    full_anchor(&state, &obs);
    battery_learning_get_snapshot(&state, &snapshot);
    assert(snapshot.gauge_session_bound);
    assert(snapshot.gauge_session == 0u);
    assert(snapshot.full_anchor_valid);
    assert(snapshot.capacity_cycle_qualified);
    assert(snapshot.remaining_capacity_valid);
    assert(snapshot.remaining_capacity_mah == 1225u);
    assert(snapshot.state_of_charge_percent == 100u);

    obs = observation(2u, 2000u, -INT64_C(400000000), 2500u, -200000);
    (void)battery_learning_update(&state, NULL, &obs, 0u);
    battery_learning_get_snapshot(&state, &snapshot);
    assert(snapshot.cycle_discharged_mah == 500u);
    assert(snapshot.remaining_capacity_mah == 725u);
    assert(snapshot.state_of_charge_percent == 59u);
}

static uint32_t run_capacity_cycle(battery_learning_state_t *state,
                                   uint32_t base_sequence,
                                   uint16_t capacity_mah) {
    int64_t anchor_nah = INT64_C(200000000);
    battery_learning_observation_t obs = observation(
        base_sequence, base_sequence * 1000u,
        anchor_nah, 2700u, 20000);
    full_anchor(state, &obs);
    obs = observation(
        base_sequence + 1u, (base_sequence + 1u) * 1000u,
        anchor_nah - (int64_t)capacity_mah * NAH_PER_MAH,
        1850u, -100000);
    return battery_learning_update(
        state, NULL, &obs, BATTERY_LEARNING_EVENT_EMPTY);
}

static void test_capacity_history_confidence_and_soh(void) {
    battery_learning_state_t state;
    battery_learning_init(&state, NULL, NULL);
    assert((run_capacity_cycle(&state, 10u, 1000u) &
            BATTERY_LEARNING_RESULT_CAPACITY_ACCEPTED) != 0u);
    battery_learning_snapshot_t snapshot;
    battery_learning_get_snapshot(&state, &snapshot);
    assert(snapshot.learned_capacity_mah == 1000u);
    assert(snapshot.last_capacity_mah == 1000u);
    assert(snapshot.state_of_health_percent == 82u);
    assert(snapshot.capacity_confidence ==
           BATTERY_CAPACITY_CONFIDENCE_OBSERVED);
    assert(snapshot.natural_empty_valid);

    (void)run_capacity_cycle(&state, 20u, 1100u);
    (void)run_capacity_cycle(&state, 30u, 1050u);
    battery_learning_get_snapshot(&state, &snapshot);
    assert(snapshot.learned_capacity_mah == 1050u);
    assert(snapshot.capacity_spread_mah == 100u);
    assert(snapshot.capacity_confidence ==
           BATTERY_CAPACITY_CONFIDENCE_LEARNED);
    assert(snapshot.accepted_capacity_cycles == 3u);

    (void)run_capacity_cycle(&state, 40u, 500u);
    battery_learning_get_snapshot(&state, &snapshot);
    assert(snapshot.learned_capacity_mah == 1050u);
    assert(snapshot.capacity_spread_mah == 600u);
    assert(snapshot.capacity_confidence ==
           BATTERY_CAPACITY_CONFIDENCE_CONFLICTED);
}

static void test_repeated_early_empty_adapts_aging_downward(void) {
    battery_learning_state_t state;
    battery_learning_init(&state, NULL, NULL);
    (void)run_capacity_cycle(&state, 10u, 1200u);
    (void)run_capacity_cycle(&state, 20u, 900u);

    battery_learning_snapshot_t snapshot;
    battery_learning_get_snapshot(&state, &snapshot);
    assert(snapshot.learned_capacity_mah == 1050u);
    assert(snapshot.capacity_confidence ==
           BATTERY_CAPACITY_CONFIDENCE_OBSERVED);

    /* The first three-cycle disagreement is exposed rather than immediately
     * trusting a single sharp drop as permanent aging. */
    (void)run_capacity_cycle(&state, 30u, 850u);
    battery_learning_get_snapshot(&state, &snapshot);
    assert(snapshot.learned_capacity_mah == 900u);
    assert(snapshot.capacity_confidence ==
           BATTERY_CAPACITY_CONFIDENCE_CONFLICTED);

    /* A repeated early physical endpoint rotates out the old 1200 mAh sample
     * and establishes a coherent lower-capacity history. */
    (void)run_capacity_cycle(&state, 40u, 850u);
    battery_learning_get_snapshot(&state, &snapshot);
    assert(snapshot.learned_capacity_mah == 850u);
    assert(snapshot.last_capacity_mah == 850u);
    assert(snapshot.capacity_spread_mah == 50u);
    assert(snapshot.capacity_confidence ==
           BATTERY_CAPACITY_CONFIDENCE_LEARNED);
    assert(snapshot.accepted_capacity_cycles == 4u);
}

static void test_natural_empty_marker_survives_and_charge_consumes_it(void) {
    battery_learning_state_t state;
    battery_learning_init(&state, NULL, NULL);
    battery_learning_provision_t provision = {
        .capacity_mah = 1000u,
    };
    assert(battery_learning_provision(&state, NULL, &provision));

    battery_learning_observation_t obs = observation(
        1u, 1000u, -INT64_C(1000000), 1900u, -50000);
    uint32_t result = battery_learning_update(
        &state, NULL, &obs, BATTERY_LEARNING_EVENT_EMPTY);
    assert((result & BATTERY_LEARNING_RESULT_PERSIST) != 0u);
    battery_learning_snapshot_t snapshot;
    battery_learning_get_snapshot(&state, &snapshot);
    assert(snapshot.natural_empty_valid);
    assert(!snapshot.full_anchor_valid);

    battery_learning_persisted_t persisted;
    battery_learning_export_persisted(&state, &persisted);
    battery_learning_state_t restored;
    battery_learning_init(&restored, NULL, &persisted);
    battery_learning_get_snapshot(&restored, &snapshot);
    assert(snapshot.natural_empty_valid);

    obs.sample_sequence++;
    obs.now_ms++;
    obs.charger_connected = true;
    obs.charge_active = true;
    obs.current_ua = 200000;
    result = battery_learning_update(&restored, NULL, &obs, 0u);
    assert((result & BATTERY_LEARNING_RESULT_PERSIST) != 0u);
    battery_learning_get_snapshot(&restored, &snapshot);
    assert(!snapshot.natural_empty_valid);
}

static void test_connected_noncharging_empty_remains_natural(void) {
    battery_learning_state_t state;
    battery_learning_init(&state, NULL, NULL);
    battery_learning_provision_t provision = {
        .capacity_mah = 1000u,
    };
    assert(battery_learning_provision(&state, NULL, &provision));

    battery_learning_observation_t obs = observation(
        1u, 1000u, 0, 1900u, -50000);
    obs.charger_connected = true;
    obs.charge_active = false;
    uint32_t result = battery_learning_update(
        &state, NULL, &obs, BATTERY_LEARNING_EVENT_EMPTY);
    battery_learning_snapshot_t snapshot;
    battery_learning_get_snapshot(&state, &snapshot);
    assert((result & BATTERY_LEARNING_RESULT_PERSIST) != 0u);
    assert(snapshot.natural_empty_valid);

    /* A misleading ACTIVE status with verified net discharge is not evidence
     * that charge entered the pack and must not invalidate a full anchor. */
    obs = observation(2u, 2000u, INT64_C(1000000000), 2700u, 0);
    obs.charger_connected = true;
    full_anchor(&state, &obs);
    obs = observation(3u, 3000u, INT64_C(999000000), 2500u, -50000);
    obs.charger_connected = true;
    obs.charge_active = true;
    (void)battery_learning_update(&state, NULL, &obs, 0u);
    battery_learning_get_snapshot(&state, &snapshot);
    assert(snapshot.full_anchor_valid);
}

static void test_recharge_temperature_and_bounds_reject_cycles(void) {
    battery_learning_state_t state;
    battery_learning_init(&state, NULL, NULL);
    battery_learning_observation_t obs = observation(
        1u, 1000u, 0, 2700u, 0);
    full_anchor(&state, &obs);
    obs = observation(2u, 2000u, INT64_C(6000000), 2700u, 50000);
    obs.charge_active = true;
    (void)battery_learning_update(&state, NULL, &obs, 0u);
    battery_learning_snapshot_t snapshot;
    battery_learning_get_snapshot(&state, &snapshot);
    assert(!snapshot.full_anchor_valid);

    obs = observation(3u, 3000u, 0, 2700u, 0);
    full_anchor(&state, &obs);
    obs = observation(4u, 4000u, -INT64_C(100000000), 2500u, -100000);
    obs.temperature_mdegc = 5000;
    (void)battery_learning_update(&state, NULL, &obs, 0u);
    obs = observation(5u, 5000u, -INT64_C(1000000000), 1850u, -100000);
    uint32_t result = battery_learning_update(
        &state, NULL, &obs, BATTERY_LEARNING_EVENT_EMPTY);
    assert((result & BATTERY_LEARNING_RESULT_CAPACITY_REJECTED) != 0u);

    obs = observation(6u, 6000u, 0, 2700u, 0);
    full_anchor(&state, &obs);
    obs = observation(7u, 7000u, -INT64_C(100000000), 1850u, -100000);
    result = battery_learning_update(
        &state, NULL, &obs, BATTERY_LEARNING_EVENT_EMPTY);
    assert((result & BATTERY_LEARNING_RESULT_CAPACITY_REJECTED) != 0u);
    battery_learning_get_snapshot(&state, &snapshot);
    assert(snapshot.accepted_capacity_cycles == 0u);
    assert(snapshot.rejected_capacity_cycles == 2u);
}

static void test_persisted_anchor_survives_restart(void) {
    battery_learning_state_t first;
    battery_learning_init(&first, NULL, NULL);
    battery_learning_observation_t obs = observation(
        1u, 1000u, INT64_C(50000000), 2700u, 0);
    full_anchor(&first, &obs);
    battery_learning_persisted_t persisted;
    battery_learning_export_persisted(&first, &persisted);

    battery_learning_state_t restored;
    battery_learning_init(&restored, NULL, &persisted);
    obs = observation(1u, 1000u, -INT64_C(950000000), 1850u, -100000);
    uint32_t result = battery_learning_update(
        &restored, NULL, &obs, BATTERY_LEARNING_EVENT_EMPTY);
    assert((result & BATTERY_LEARNING_RESULT_CAPACITY_ACCEPTED) != 0u);
    battery_learning_snapshot_t snapshot;
    battery_learning_get_snapshot(&restored, &snapshot);
    assert(snapshot.learned_capacity_mah == 1000u);
}

static void test_new_hardware_session_clears_exact_pack_learning(void) {
    battery_learning_state_t state;
    battery_learning_init(&state, NULL, NULL);
    (void)run_capacity_cycle(&state, 1u, 1000u);
    battery_learning_observation_t obs = observation(
        10u, 10000u, 0, 2600u, -10000);
    obs.gauge_session = 1u;
    uint32_t result = battery_learning_update(&state, NULL, &obs, 0u);
    assert((result & BATTERY_LEARNING_RESULT_PACK_RESET) != 0u);
    battery_learning_snapshot_t snapshot;
    battery_learning_get_snapshot(&state, &snapshot);
    assert(snapshot.pack_generation == 1u);
    assert(!snapshot.learned_capacity_valid);
    assert(snapshot.nominal_capacity_mah == 1225u);

    result = battery_learning_update(&state, NULL, &obs, 0u);
    assert((result & BATTERY_LEARNING_RESULT_PACK_RESET) == 0u);
}

static void feed_resistance_step(battery_learning_state_t *state,
                                 uint32_t sequence,
                                 uint16_t base_mv,
                                 uint16_t drop_mv,
                                 int32_t load_step_ua) {
    battery_learning_observation_t obs = observation(
        sequence, sequence * 1000u, 0, base_mv, -30000);
    (void)battery_learning_update(state, NULL, &obs, 0u);
    obs = observation(sequence + 1u, sequence * 1000u + 250u, 0,
                      (uint16_t)(base_mv - drop_mv),
                      -30000 - load_step_ua);
    (void)battery_learning_update(state, NULL, &obs, 0u);
}

static void test_resistance_median_and_bins(void) {
    battery_learning_state_t state;
    battery_learning_init(&state, NULL, NULL);
    uint32_t result = 0u;
    for (uint32_t i = 0u; i < 9u; i++) {
        battery_learning_observation_t base = observation(
            i * 3u + 1u, i * 3000u + 1000u, 0, 2750u, -30000);
        result |= battery_learning_update(&state, NULL, &base, 0u);
        uint16_t drop = i == 4u ? 250u : 100u;
        battery_learning_observation_t load = observation(
            i * 3u + 2u, i * 3000u + 1250u, 0,
            (uint16_t)(2750u - drop), -530000);
        result |= battery_learning_update(&state, NULL, &load, 0u);
    }
    assert((result & BATTERY_LEARNING_RESULT_RESISTANCE_UPDATED) != 0u);
    assert((result & BATTERY_LEARNING_RESULT_PERSIST) != 0u);
    battery_learning_snapshot_t snapshot;
    battery_learning_get_snapshot(&state, &snapshot);
    assert(snapshot.resistance_mohm[BATTERY_RESISTANCE_BIN_HIGH] == 200u);
    assert(snapshot.resistance_sample_count[BATTERY_RESISTANCE_BIN_HIGH] == 9u);

    for (uint32_t i = 0u; i < 9u; i++) {
        feed_resistance_step(&state, 100u + i * 3u, 2450u, 80u, 400000);
    }
    battery_learning_get_snapshot(&state, &snapshot);
    assert(snapshot.resistance_mohm[BATTERY_RESISTANCE_BIN_MID] == 200u);
}

static void test_resistance_rejects_bad_evidence(void) {
    battery_learning_state_t state;
    battery_learning_init(&state, NULL, NULL);
    battery_learning_observation_t obs = observation(
        1u, 1000u, 0, 2600u, -30000);
    (void)battery_learning_update(&state, NULL, &obs, 0u);
    obs = observation(2u, 1250u, 0, 2500u, -530000);
    obs.charger_connected = true;
    (void)battery_learning_update(&state, NULL, &obs, 0u);
    obs = observation(3u, 2000u, 0, 2600u, -30000);
    obs.temperature_mdegc = 5000;
    (void)battery_learning_update(&state, NULL, &obs, 0u);
    obs = observation(4u, 2250u, 0, 2500u, -530000);
    obs.temperature_mdegc = 5000;
    (void)battery_learning_update(&state, NULL, &obs, 0u);
    battery_learning_snapshot_t snapshot;
    battery_learning_get_snapshot(&state, &snapshot);
    for (uint8_t i = 0u; i < BATTERY_LEARNING_RESISTANCE_BIN_COUNT; i++) {
        assert(snapshot.resistance_sample_count[i] == 0u);
        assert(snapshot.resistance_mohm[i] == 0u);
    }
}

static void test_duplicate_wrap_and_debug_observations_are_inert(void) {
    battery_learning_state_t state;
    battery_learning_init(&state, NULL, NULL);
    battery_learning_observation_t obs = observation(
        UINT32_MAX, 1000u, 0, 2700u, 0);
    obs.charger_connected = true;
    obs.authoritative = false;
    assert(battery_learning_update(
               &state, NULL, &obs, BATTERY_LEARNING_EVENT_FULL) == 0u);

    obs.authoritative = true;
    full_anchor(&state, &obs);
    battery_learning_persisted_t before;
    battery_learning_export_persisted(&state, &before);
    obs.session_delta_nah = -INT64_C(100000000);
    (void)battery_learning_update(&state, NULL, &obs, 0u);
    battery_learning_persisted_t after;
    battery_learning_export_persisted(&state, &after);
    assert(after.full_anchor_nah == before.full_anchor_nah);

    obs = observation(0u, 2000u, -INT64_C(100000000), 2500u, -100000);
    (void)battery_learning_update(&state, NULL, &obs, 0u);
    battery_learning_snapshot_t snapshot;
    battery_learning_get_snapshot(&state, &snapshot);
    assert(snapshot.cycle_discharged_mah == 100u);
}

static void test_exact_capacity_and_temperature_boundaries(void) {
    static const struct {
        uint16_t capacity_mah;
        bool accepted;
    } cases[] = {
        {299u, false},
        {300u, true},
        {2000u, true},
        {2001u, false},
    };
    for (uint8_t i = 0u; i < sizeof(cases) / sizeof(cases[0]); i++) {
        battery_learning_state_t state;
        battery_learning_init(&state, NULL, NULL);
        uint32_t result = run_capacity_cycle(&state, 10u,
                                             cases[i].capacity_mah);
        assert(((result & BATTERY_LEARNING_RESULT_CAPACITY_ACCEPTED) != 0u) ==
               cases[i].accepted);
        assert(((result & BATTERY_LEARNING_RESULT_CAPACITY_REJECTED) != 0u) !=
               cases[i].accepted);
    }

    static const struct {
        int32_t temperature_mdegc;
        bool accepted;
    } temperatures[] = {
        {9999, false},
        {10000, true},
        {40000, true},
        {40001, false},
    };
    for (uint8_t i = 0u;
         i < sizeof(temperatures) / sizeof(temperatures[0]); i++) {
        battery_learning_state_t state;
        battery_learning_init(&state, NULL, NULL);
        battery_learning_observation_t obs = observation(
            1u, 1000u, 0, 2700u, 0);
        obs.temperature_mdegc = temperatures[i].temperature_mdegc;
        full_anchor(&state, &obs);
        obs = observation(2u, 2000u, -INT64_C(1000000000),
                          1850u, -100000);
        obs.temperature_mdegc = temperatures[i].temperature_mdegc;
        uint32_t result = battery_learning_update(
            &state, NULL, &obs, BATTERY_LEARNING_EVENT_EMPTY);
        assert(((result & BATTERY_LEARNING_RESULT_CAPACITY_ACCEPTED) != 0u) ==
               temperatures[i].accepted);
    }
}

static void test_capacity_history_ring_replaces_oldest(void) {
    battery_learning_state_t state;
    battery_learning_init(&state, NULL, NULL);
    (void)run_capacity_cycle(&state, 10u, 800u);
    (void)run_capacity_cycle(&state, 20u, 1000u);
    (void)run_capacity_cycle(&state, 30u, 1200u);
    (void)run_capacity_cycle(&state, 40u, 1400u);
    battery_learning_snapshot_t snapshot;
    battery_learning_get_snapshot(&state, &snapshot);
    assert(snapshot.learned_capacity_mah == 1200u);
    assert(snapshot.capacity_spread_mah == 400u);
    assert(snapshot.last_capacity_mah == 1400u);
    assert(snapshot.accepted_capacity_cycles == 4u);
}

static void test_event_conflict_and_invalid_full_are_inert(void) {
    battery_learning_state_t state;
    battery_learning_init(&state, NULL, NULL);
    battery_learning_observation_t obs = observation(
        1u, 1000u, 0, 2700u, 0);
    obs.charger_connected = true;
    uint32_t result = battery_learning_update(
        &state, NULL, &obs,
        BATTERY_LEARNING_EVENT_FULL | BATTERY_LEARNING_EVENT_EMPTY);
    assert((result & (BATTERY_LEARNING_RESULT_FULL_ANCHORED |
                      BATTERY_LEARNING_RESULT_CAPACITY_ACCEPTED |
                      BATTERY_LEARNING_RESULT_CAPACITY_REJECTED)) == 0u);
    battery_learning_snapshot_t snapshot;
    battery_learning_get_snapshot(&state, &snapshot);
    assert(!snapshot.full_anchor_valid);

    obs.sample_sequence++;
    obs.sample_valid = false;
    result = battery_learning_update(
        &state, NULL, &obs, BATTERY_LEARNING_EVENT_FULL);
    assert((result & BATTERY_LEARNING_RESULT_FULL_ANCHORED) == 0u);
    battery_learning_get_snapshot(&state, &snapshot);
    assert(!snapshot.full_anchor_valid);
}

static void test_unverified_current_blocks_coulomb_estimates(void) {
    battery_learning_state_t state;
    battery_learning_init(&state, NULL, NULL);
    battery_learning_observation_t obs = observation(
        1u, 1000u, 0, 2700u, 0);
    obs.charger_connected = true;
    obs.current_valid = false;
    uint32_t result = battery_learning_update(
        &state, NULL, &obs, BATTERY_LEARNING_EVENT_FULL);
    assert((result & BATTERY_LEARNING_RESULT_FULL_ANCHORED) == 0u);

    obs.current_valid = true;
    full_anchor(&state, &obs);
    obs = observation(2u, 2000u, -INT64_C(100000000),
                      2500u, -100000);
    obs.current_valid = false;
    result = battery_learning_update(&state, NULL, &obs, 0u);
    assert((result & BATTERY_LEARNING_RESULT_PERSIST) != 0u);
    battery_learning_snapshot_t snapshot;
    battery_learning_get_snapshot(&state, &snapshot);
    assert(snapshot.full_anchor_valid);
    assert(!snapshot.capacity_cycle_qualified);
    assert(!snapshot.remaining_capacity_valid);

    result = battery_learning_update(
        &state, NULL, &obs, BATTERY_LEARNING_EVENT_EMPTY);
    assert((result & (BATTERY_LEARNING_RESULT_CAPACITY_ACCEPTED |
                      BATTERY_LEARNING_RESULT_CAPACITY_REJECTED)) == 0u);
}

static void test_cached_sample_invalidation_hides_live_projection(void) {
    battery_learning_state_t state;
    battery_learning_init(&state, NULL, NULL);
    battery_learning_observation_t obs = observation(
        7u, 1000u, 0, 2700u, 0);
    full_anchor(&state, &obs);
    battery_learning_snapshot_t snapshot;
    battery_learning_get_snapshot(&state, &snapshot);
    assert(snapshot.remaining_capacity_valid);
    assert(snapshot.current_resistance_bin_valid);

    /* The LTC failure path deliberately does not advance sample_sequence. */
    obs.charger_connected = false;
    obs.sample_valid = false;
    (void)battery_learning_update(&state, NULL, &obs, 0u);
    battery_learning_get_snapshot(&state, &snapshot);
    assert(!snapshot.remaining_capacity_valid);
    assert(!snapshot.current_resistance_bin_valid);
    assert(snapshot.full_anchor_valid);

    obs.sample_valid = true;
    obs.session_delta_nah = -INT64_C(100000000);
    obs.terminal_mv = 2500u;
    (void)battery_learning_update(&state, NULL, &obs, 0u);
    battery_learning_get_snapshot(&state, &snapshot);
    assert(!snapshot.remaining_capacity_valid);

    obs.sample_sequence++;
    (void)battery_learning_update(&state, NULL, &obs, 0u);
    battery_learning_get_snapshot(&state, &snapshot);
    assert(snapshot.remaining_capacity_valid);
    assert(snapshot.cycle_discharged_mah == 100u);
}

static void test_soc_bootstrap_piecewise_accounting_and_restart(void) {
    battery_learning_state_t state;
    battery_learning_init(&state, NULL, NULL);
    battery_learning_snapshot_t snapshot;

    for (uint32_t sequence = 1u;
         sequence <= BATTERY_LEARNING_SOC_BOOTSTRAP_SAMPLE_COUNT;
         sequence++) {
        battery_learning_observation_t obs = observation(
            sequence, sequence * 1000u, 0, 2450u, -20000);
        obs.reference_valid = true;
        uint32_t result = battery_learning_update(&state, NULL, &obs, 0u);
        battery_learning_get_snapshot(&state, &snapshot);
        if (sequence < BATTERY_LEARNING_SOC_BOOTSTRAP_SAMPLE_COUNT) {
            assert((result & BATTERY_LEARNING_RESULT_SOC_BOOTSTRAPPED) == 0u);
            assert(!snapshot.remaining_capacity_valid);
        } else {
            assert((result & BATTERY_LEARNING_RESULT_SOC_BOOTSTRAPPED) != 0u);
            assert((result & BATTERY_LEARNING_RESULT_PERSIST) != 0u);
        }
    }
    assert(snapshot.remaining_capacity_valid);
    assert(snapshot.remaining_capacity_nah == UINT64_C(612500000));
    assert(snapshot.remaining_capacity_mah == 613u);
    assert(snapshot.state_of_charge_percent == 50u);
    assert(snapshot.soc_provenance ==
           BATTERY_SOC_PROVENANCE_BOOTSTRAP_VOLTAGE);
    assert(snapshot.soc_confidence == BATTERY_SOC_CONFIDENCE_PROVISIONAL);
    assert(snapshot.soc_bootstrap_reference_mv == 2450u);

    battery_learning_observation_t obs = observation(
        6u, 6000u, -INT64_C(100000000), 2430u, -100000);
    uint32_t result = battery_learning_update(&state, NULL, &obs, 0u);
    battery_learning_get_snapshot(&state, &snapshot);
    assert((result & BATTERY_LEARNING_RESULT_PERSIST) == 0u);
    assert(snapshot.remaining_capacity_nah == UINT64_C(512500000));
    assert(snapshot.soc_provenance == BATTERY_SOC_PROVENANCE_TRACKED);

    /* The transition sample closes the discharge segment. Later positive ACR
     * movement is divided by the profile's 1.224 input/deliverable factor. */
    obs = observation(7u, 7000u, -INT64_C(90000000), 2460u, 200000);
    obs.charger_connected = true;
    obs.charge_active = true;
    result = battery_learning_update(&state, NULL, &obs, 0u);
    assert((result & BATTERY_LEARNING_RESULT_SOC_REANCHORED) != 0u);
    assert((result & BATTERY_LEARNING_RESULT_PERSIST) != 0u);

    obs = observation(8u, 8000u, INT64_C(32400000), 2550u, 200000);
    obs.charger_connected = true;
    obs.charge_active = true;
    (void)battery_learning_update(&state, NULL, &obs, 0u);
    battery_learning_get_snapshot(&state, &snapshot);
    assert(snapshot.remaining_capacity_nah == UINT64_C(622500000));
    assert(snapshot.remaining_capacity_mah == 623u);
    assert(snapshot.soc_charge_segment);

    battery_learning_persisted_t persisted;
    battery_learning_export_persisted(&state, &persisted);
    battery_learning_state_t restored;
    battery_learning_init(&restored, NULL, &persisted);
    (void)battery_learning_update(&restored, NULL, &obs, 0u);
    battery_learning_get_snapshot(&restored, &snapshot);
    assert(snapshot.remaining_capacity_nah == UINT64_C(622500000));
    assert(snapshot.soc_charge_segment);

    obs = observation(9u, 9000u, INT64_C(32400000), 2540u, -20000);
    result = battery_learning_update(&restored, NULL, &obs, 0u);
    assert((result & BATTERY_LEARNING_RESULT_SOC_REANCHORED) != 0u);
    obs = observation(10u, 10000u, INT64_C(22400000), 2520u, -20000);
    (void)battery_learning_update(&restored, NULL, &obs, 0u);
    battery_learning_get_snapshot(&restored, &snapshot);
    assert(snapshot.remaining_capacity_nah == UINT64_C(612500000));
    assert(!snapshot.soc_charge_segment);
}

static void test_soc_endpoint_authority_and_session_replacement(void) {
    battery_learning_state_t state;
    battery_learning_init(&state, NULL, NULL);
    battery_learning_observation_t obs = observation(
        1u, 1000u, INT64_C(300000000), 2700u, 10000);
    full_anchor(&state, &obs);
    battery_learning_snapshot_t snapshot;
    battery_learning_get_snapshot(&state, &snapshot);
    assert(snapshot.soc_provenance == BATTERY_SOC_PROVENANCE_ANCHORED_FULL);
    assert(snapshot.soc_confidence == BATTERY_SOC_CONFIDENCE_ANCHORED);
    assert(snapshot.remaining_capacity_nah == UINT64_C(1225000000));

    obs = observation(2u, 2000u, -INT64_C(700000000), 1850u, -100000);
    uint32_t result = battery_learning_update(
        &state, NULL, &obs, BATTERY_LEARNING_EVENT_EMPTY);
    assert((result & BATTERY_LEARNING_RESULT_CAPACITY_ACCEPTED) != 0u);
    assert((result & BATTERY_LEARNING_RESULT_SOC_REANCHORED) != 0u);
    battery_learning_get_snapshot(&state, &snapshot);
    assert(snapshot.soc_provenance == BATTERY_SOC_PROVENANCE_ANCHORED_EMPTY);
    assert(snapshot.soc_confidence == BATTERY_SOC_CONFIDENCE_ANCHORED);
    assert(snapshot.remaining_capacity_nah == 0u);
    assert(snapshot.learned_capacity_mah == 1000u);

    obs = observation(3u, 3000u, 0, 2500u, -10000);
    obs.gauge_session = 1u;
    obs.reference_valid = true;
    result = battery_learning_update(&state, NULL, &obs, 0u);
    assert((result & BATTERY_LEARNING_RESULT_PACK_RESET) != 0u);
    battery_learning_get_snapshot(&state, &snapshot);
    assert(!snapshot.remaining_capacity_valid);
    assert(!snapshot.learned_capacity_valid);

    for (uint32_t sequence = 4u; sequence <= 7u; sequence++) {
        obs.sample_sequence = sequence;
        obs.now_ms += 1000u;
        result = battery_learning_update(&state, NULL, &obs, 0u);
    }
    assert((result & BATTERY_LEARNING_RESULT_SOC_BOOTSTRAPPED) != 0u);
    battery_learning_get_snapshot(&state, &snapshot);
    assert(snapshot.remaining_capacity_valid);
    assert(snapshot.state_of_charge_percent == 50u);
    assert(snapshot.soc_confidence == BATTERY_SOC_CONFIDENCE_PROVISIONAL);
}

static void test_extreme_persisted_anchor_arithmetic_is_total(void) {
    battery_learning_persisted_t persisted;
    battery_learning_persisted_defaults(&persisted, NULL);
    persisted.full_anchor_valid = true;
    persisted.capacity_cycle_qualified = true;
    persisted.full_anchor_nah = INT64_MAX;
    persisted.cycle_min_temperature_mdegc = 25000;
    persisted.cycle_max_temperature_mdegc = 25000;
    assert(battery_learning_persisted_valid(&persisted, NULL));

    battery_learning_state_t state;
    battery_learning_init(&state, NULL, &persisted);
    battery_learning_observation_t obs = observation(
        1u, 1000u, INT64_MIN, 1800u, -100000);
    uint32_t result = battery_learning_update(&state, NULL, &obs, 0u);
    battery_learning_snapshot_t snapshot;
    battery_learning_get_snapshot(&state, &snapshot);
    assert(snapshot.cycle_discharged_mah == UINT16_MAX);
    assert(snapshot.remaining_capacity_valid);
    assert(snapshot.remaining_capacity_mah == 0u);
    result = battery_learning_update(
        &state, NULL, &obs, BATTERY_LEARNING_EVENT_EMPTY);
    assert((result & BATTERY_LEARNING_RESULT_CAPACITY_REJECTED) != 0u);
    battery_learning_get_snapshot(&state, &snapshot);
    assert(!snapshot.full_anchor_valid);

    battery_learning_persisted_defaults(&persisted, NULL);
    persisted.full_anchor_valid = true;
    persisted.capacity_cycle_qualified = true;
    persisted.full_anchor_nah = INT64_MIN;
    persisted.cycle_min_temperature_mdegc = 25000;
    persisted.cycle_max_temperature_mdegc = 25000;
    battery_learning_init(&state, NULL, &persisted);
    obs = observation(1u, 1000u, INT64_MAX, 2700u, 0);
    result = battery_learning_update(&state, NULL, &obs, 0u);
    assert((result & BATTERY_LEARNING_RESULT_PERSIST) != 0u);
    battery_learning_get_snapshot(&state, &snapshot);
    assert(!snapshot.full_anchor_valid);

    battery_learning_persisted_defaults(&persisted, NULL);
    persisted.soc_valid = true;
    persisted.soc_charge_segment = true;
    persisted.soc_capacity_mah = 1225u;
    persisted.soc_charge_factor_permille = 1224u;
    persisted.soc_anchor_provenance = BATTERY_SOC_PROVENANCE_TRACKED;
    persisted.soc_confidence = BATTERY_SOC_CONFIDENCE_ANCHORED;
    assert(battery_learning_persisted_valid(&persisted, NULL));
    battery_learning_init(&state, NULL, &persisted);
    obs = observation(1u, 1000u, INT64_C(18446744073709552), 2700u, 1);
    (void)battery_learning_update(&state, NULL, &obs, 0u);
    battery_learning_get_snapshot(&state, &snapshot);
    assert(snapshot.remaining_capacity_nah == UINT64_C(1225000000));
}

static void test_recharge_delta_and_resistance_boundaries(void) {
    battery_learning_state_t state;
    battery_learning_init(&state, NULL, NULL);
    battery_learning_observation_t obs = observation(
        1u, 1000u, 0, 2700u, 0);
    full_anchor(&state, &obs);
    obs = observation(2u, 2000u, INT64_C(5000000), 2700u, 0);
    (void)battery_learning_update(&state, NULL, &obs, 0u);
    battery_learning_snapshot_t snapshot;
    battery_learning_get_snapshot(&state, &snapshot);
    assert(snapshot.full_anchor_valid);
    obs = observation(3u, 3000u, INT64_C(5000001), 2700u, 0);
    (void)battery_learning_update(&state, NULL, &obs, 0u);
    battery_learning_get_snapshot(&state, &snapshot);
    assert(!snapshot.full_anchor_valid);

    battery_learning_init(&state, NULL, NULL);
    for (uint32_t i = 0u; i < 9u; i++) {
        feed_resistance_step(&state, 10u + i * 3u,
                             2700u, 10u, 200000);
    }
    battery_learning_get_snapshot(&state, &snapshot);
    assert(snapshot.resistance_mohm[BATTERY_RESISTANCE_BIN_HIGH] == 50u);

    battery_learning_init(&state, NULL, NULL);
    for (uint32_t i = 0u; i < 9u; i++) {
        feed_resistance_step(&state, 100u + i * 3u,
                             2700u, 300u, 300000);
    }
    battery_learning_get_snapshot(&state, &snapshot);
    assert(snapshot.resistance_mohm[BATTERY_RESISTANCE_BIN_HIGH] == 1000u);
}

static uint32_t random_next(uint32_t *state) {
    *state = *state * UINT32_C(1664525) + UINT32_C(1013904223);
    return *state;
}

static void assert_state_invariants(const battery_learning_state_t *state) {
    battery_learning_snapshot_t snapshot;
    battery_learning_persisted_t persisted;
    battery_learning_get_snapshot(state, &snapshot);
    battery_learning_export_persisted(state, &persisted);
    assert(persisted.profile_id == BATTERY_LEARNING_PROFILE_NIMH_2S);
    assert(persisted.nominal_capacity_mah == 1225u);
    assert(persisted.capacity_history_count <=
           BATTERY_LEARNING_CAPACITY_HISTORY_COUNT);
    assert(persisted.capacity_history_next <
           BATTERY_LEARNING_CAPACITY_HISTORY_COUNT);
    for (uint8_t i = 0u; i < persisted.capacity_history_count; i++) {
        assert(persisted.capacity_history_mah[i] >= 300u);
        assert(persisted.capacity_history_mah[i] <= 2000u);
    }
    assert(snapshot.capacity_confidence <=
           BATTERY_CAPACITY_CONFIDENCE_CONFLICTED);
    assert(snapshot.state_of_charge_percent <= 100u);
    assert(snapshot.soc_bars <= 4u);
    if (snapshot.capacity_prediction_exhausted) {
        assert(snapshot.soc_bars_valid);
        assert(snapshot.soc_bars == 0u);
        assert(snapshot.remaining_capacity_valid);
        assert(snapshot.remaining_capacity_nah == 0u);
        assert(snapshot.full_anchor_valid);
        assert(snapshot.capacity_cycle_qualified);
    }
    assert(snapshot.capacity_overrun_nah <=
           snapshot.cycle_discharged_nah);
    assert(snapshot.current_resistance_bin <
           BATTERY_LEARNING_RESISTANCE_BIN_COUNT);
    assert(snapshot.current_resistance_bin_valid ==
           state->latest_observation_valid);
    assert(snapshot.natural_empty_valid ==
           persisted.natural_empty_valid);
    assert(!(persisted.natural_empty_valid &&
             persisted.full_anchor_valid));
    for (uint8_t i = 0u; i < BATTERY_LEARNING_RESISTANCE_BIN_COUNT; i++) {
        uint16_t resistance = persisted.resistance_mohm[i];
        assert(resistance == 0u || (resistance >= 50u && resistance <= 1000u));
        assert(state->resistance_window_count[i] <
               BATTERY_LEARNING_RESISTANCE_WINDOW_COUNT);
    }
}

static void test_provision_replaces_health_without_importing_anchor(void) {
    battery_learning_state_t state;
    battery_learning_init(&state, NULL, NULL);
    battery_learning_observation_t obs = observation(
        1u, 1000u, INT64_C(250000000), 2700u, 10000);
    obs.gauge_session = 7u;
    assert((battery_learning_update(&state, NULL, &obs, 0u) &
            BATTERY_LEARNING_RESULT_PACK_RESET) != 0u);
    obs.charger_connected = true;
    assert((battery_learning_update(
                &state, NULL, &obs, BATTERY_LEARNING_EVENT_FULL) &
            BATTERY_LEARNING_RESULT_FULL_ANCHORED) != 0u);
    uint32_t generation_before = state.persisted.pack_generation;

    battery_learning_provision_t provision = {
        .capacity_mah = 1000u,
        .resistance_mohm = {50u, 0u, 1000u},
        .natural_empty_valid = true,
    };
    assert(battery_learning_provision(&state, NULL, &provision));
    battery_learning_snapshot_t snapshot;
    battery_learning_get_snapshot(&state, &snapshot);
    assert(snapshot.learned_capacity_valid);
    assert(snapshot.learned_capacity_mah == 1000u);
    assert(snapshot.last_capacity_mah == 1000u);
    assert(snapshot.capacity_confidence ==
           BATTERY_CAPACITY_CONFIDENCE_OBSERVED);
    assert(snapshot.accepted_capacity_cycles == 1u);
    assert(!snapshot.full_anchor_valid);
    assert(!snapshot.capacity_cycle_qualified);
    assert(!snapshot.remaining_capacity_valid);
    assert(snapshot.natural_empty_valid);
    assert(snapshot.resistance_mohm[0] == 50u);
    assert(snapshot.resistance_mohm[1] == 0u);
    assert(snapshot.resistance_mohm[2] == 1000u);
    assert(snapshot.resistance_sample_count[0] == 0u);
    assert(snapshot.pack_generation == generation_before + 1u);
    assert(state.session_seen);
    assert(state.last_gauge_session == 7u);

    obs = observation(2u, 2000u, 0, 2600u, -50000);
    obs.gauge_session = 7u;
    uint32_t result = battery_learning_update(&state, NULL, &obs, 0u);
    assert((result & BATTERY_LEARNING_RESULT_PACK_RESET) == 0u);
    battery_learning_get_snapshot(&state, &snapshot);
    assert(snapshot.learned_capacity_mah == 1000u);
    assert(snapshot.natural_empty_valid);

    obs.sample_sequence++;
    obs.gauge_session = 8u;
    result = battery_learning_update(&state, NULL, &obs, 0u);
    assert((result & BATTERY_LEARNING_RESULT_PACK_RESET) != 0u);
    battery_learning_get_snapshot(&state, &snapshot);
    assert(!snapshot.learned_capacity_valid);
    assert(!snapshot.natural_empty_valid);
}

static void test_provision_validation_is_atomic(void) {
    battery_learning_state_t state;
    battery_learning_init(&state, NULL, NULL);
    battery_learning_state_t original = state;
    battery_learning_provision_t provision = {
        .capacity_mah = 299u,
    };
    assert(!battery_learning_provision(&state, NULL, &provision));
    assert(memcmp(&state, &original, sizeof(state)) == 0);

    provision.capacity_mah = 2001u;
    assert(!battery_learning_provision(&state, NULL, &provision));
    provision.capacity_mah = 1000u;
    provision.resistance_mohm[0] = 49u;
    assert(!battery_learning_provision(&state, NULL, &provision));
    provision.resistance_mohm[0] = 1001u;
    assert(!battery_learning_provision(&state, NULL, &provision));
    assert(!battery_learning_provision(NULL, NULL, &provision));
    assert(!battery_learning_provision(&state, NULL, NULL));
    assert(memcmp(&state, &original, sizeof(state)) == 0);
}

static void test_recovered_cycle_appends_and_preserves_full_evidence(void) {
    battery_learning_state_t state;
    battery_learning_init(&state, NULL, NULL);
    assert((run_capacity_cycle(&state, 10u, 1242u) &
            BATTERY_LEARNING_RESULT_CAPACITY_ACCEPTED) != 0u);
    state.persisted.rejected_capacity_cycles = 1u;
    const uint16_t resistance[] = {115u, 132u, 89u};
    const uint16_t counts[] = {7u, 8u, 9u};
    memcpy(state.persisted.resistance_mohm, resistance,
           sizeof(resistance));
    memcpy(state.persisted.resistance_sample_count, counts,
           sizeof(counts));
    memcpy(state.resistance_persisted_mohm, resistance,
           sizeof(resistance));
    memcpy(state.resistance_persisted_count, counts, sizeof(counts));

    battery_learning_observation_t obs = observation(
        20u, 20000u, INT64_C(500000000), 2700u, 0);
    full_anchor(&state, &obs);
    battery_learning_state_t before = state;

    assert(battery_learning_recover_capacity_cycle(
        &state, NULL, 1301u, 1u));
    battery_learning_snapshot_t snapshot;
    battery_learning_get_snapshot(&state, &snapshot);
    assert(snapshot.capacity_history_count == 2u);
    assert(snapshot.capacity_history_next == 2u);
    assert(snapshot.capacity_history_mah[0] == 1242u);
    assert(snapshot.capacity_history_mah[1] == 1301u);
    assert(snapshot.learned_capacity_mah == 1272u);
    assert(snapshot.last_capacity_mah == 1301u);
    assert(snapshot.accepted_capacity_cycles == 2u);
    assert(snapshot.rejected_capacity_cycles == 1u);
    assert(snapshot.full_anchor_valid);
    assert(snapshot.capacity_cycle_qualified);
    assert(snapshot.soc_capacity_mah == 1272u);
    assert(snapshot.remaining_capacity_nah == UINT64_C(1272000000));
    assert(snapshot.state_of_charge_percent == 100u);
    assert(snapshot.soc_provenance ==
           BATTERY_SOC_PROVENANCE_ANCHORED_FULL);
    assert(snapshot.soc_confidence == BATTERY_SOC_CONFIDENCE_ANCHORED);
    assert(snapshot.pack_generation == before.persisted.pack_generation);
    assert(state.persisted.full_anchor_acr_raw ==
           before.persisted.full_anchor_acr_raw);
    assert(state.persisted.full_anchor_nah ==
           before.persisted.full_anchor_nah);
    assert(memcmp(snapshot.resistance_mohm, resistance,
                  sizeof(resistance)) == 0);
    assert(memcmp(snapshot.resistance_sample_count, counts,
                  sizeof(counts)) == 0);
    assert(state.session_seen == before.session_seen);
    assert(state.last_gauge_session == before.last_gauge_session);

    battery_learning_state_t after = state;
    assert(!battery_learning_recover_capacity_cycle(
        &state, NULL, 1301u, 1u));
    assert(memcmp(&state, &after, sizeof(state)) == 0);
    assert(!battery_learning_recover_capacity_cycle(
        &state, NULL, 299u, 2u));
    assert(memcmp(&state, &after, sizeof(state)) == 0);
    assert(!battery_learning_recover_capacity_cycle(
        NULL, NULL, 1301u, 2u));
}

static void test_capacity_soc_predicts_zero_without_manufacturing_empty(void) {
    battery_learning_state_t state;
    battery_learning_init(&state, NULL, NULL);
    battery_learning_provision_t provision = {
        .capacity_mah = 1000u,
    };
    assert(battery_learning_provision(&state, NULL, &provision));

    battery_learning_observation_t obs = observation(
        1u, 1000u, INT64_C(2000000000), 2800u, 20000);
    full_anchor(&state, &obs);
    battery_learning_snapshot_t snapshot;
    battery_learning_get_snapshot(&state, &snapshot);
    assert(snapshot.soc_bars_valid);
    assert(snapshot.soc_bars == 4u);

    obs = observation(2u, 2000u, INT64_C(1750000000), 2550u, -50000);
    uint32_t result = battery_learning_update(&state, NULL, &obs, 0u);
    battery_learning_get_snapshot(&state, &snapshot);
    assert(snapshot.remaining_capacity_nah == UINT64_C(750000000));
    assert(snapshot.soc_bars == 3u);
    assert(snapshot.cycle_discharged_nah == UINT64_C(250000000));
    assert(!snapshot.capacity_prediction_exhausted);
    assert(snapshot.capacity_overrun_nah == 0u);

    obs = observation(3u, 3000u, INT64_C(1000000001), 2284u, -45000);
    result = battery_learning_update(&state, NULL, &obs, 0u);
    battery_learning_get_snapshot(&state, &snapshot);
    assert(snapshot.soc_bars == 1u);
    assert(!snapshot.capacity_prediction_exhausted);

    obs = observation(4u, 4000u, INT64_C(1000000000), 2280u, -45000);
    result = battery_learning_update(&state, NULL, &obs, 0u);
    battery_learning_get_snapshot(&state, &snapshot);
    assert((result & (BATTERY_LEARNING_RESULT_CAPACITY_ACCEPTED |
                      BATTERY_LEARNING_RESULT_CAPACITY_REJECTED)) == 0u);
    assert(snapshot.capacity_prediction_exhausted);
    assert(!snapshot.natural_empty_valid);
    assert(snapshot.full_anchor_valid);
    assert(snapshot.remaining_capacity_nah == 0u);
    assert(snapshot.soc_bars_valid && snapshot.soc_bars == 0u);
    assert(snapshot.capacity_overrun_nah == 0u);
    assert(snapshot.accepted_capacity_cycles == 1u);

    /* Healthy-voltage operation beyond the estimate remains measurable. It
     * must not reinforce the estimate with a synthetic endpoint. */
    obs = observation(5u, 5000u, INT64_C(950000000), 2500u, -45000);
    result = battery_learning_update(&state, NULL, &obs, 0u);
    battery_learning_get_snapshot(&state, &snapshot);
    assert((result & (BATTERY_LEARNING_RESULT_CAPACITY_ACCEPTED |
                      BATTERY_LEARNING_RESULT_CAPACITY_REJECTED)) == 0u);
    assert(snapshot.capacity_prediction_exhausted);
    assert(snapshot.capacity_overrun_nah == UINT64_C(50000000));
    assert(snapshot.cycle_discharged_nah == UINT64_C(1050000000));
    assert(snapshot.accepted_capacity_cycles == 1u);

    /* Only the independent physical endpoint turns that 1050 mAh delivery
     * into a capacity observation. */
    result = battery_learning_update(
        &state, NULL, &obs, BATTERY_LEARNING_EVENT_EMPTY);
    battery_learning_get_snapshot(&state, &snapshot);
    assert((result & BATTERY_LEARNING_RESULT_CAPACITY_ACCEPTED) != 0u);
    assert((result & BATTERY_LEARNING_RESULT_PERSIST) != 0u);
    assert(!snapshot.capacity_prediction_exhausted);
    assert(snapshot.capacity_overrun_nah == 0u);
    assert(snapshot.natural_empty_valid);
    assert(!snapshot.full_anchor_valid);
    assert(snapshot.learned_capacity_mah == 1025u);
    assert(snapshot.last_capacity_mah == 1050u);
    assert(snapshot.accepted_capacity_cycles == 2u);
}

static void test_capacity_prediction_requires_observed_qualified_continuity(void) {
    battery_learning_state_t state;
    battery_learning_init(&state, NULL, NULL);
    battery_learning_observation_t obs = observation(
        1u, 1000u, INT64_C(1000000000), 2700u, 10000);
    full_anchor(&state, &obs);
    obs = observation(2u, 2000u, -INT64_C(225000000), 2200u, -50000);
    (void)battery_learning_update(&state, NULL, &obs, 0u);
    battery_learning_snapshot_t snapshot;
    battery_learning_get_snapshot(&state, &snapshot);
    assert(!snapshot.capacity_prediction_exhausted);

    battery_learning_init(&state, NULL, NULL);
    battery_learning_provision_t provision = {.capacity_mah = 1000u};
    assert(battery_learning_provision(&state, NULL, &provision));
    obs = observation(1u, 1000u, INT64_C(1000000000), 2700u, 10000);
    full_anchor(&state, &obs);
    obs = observation(2u, 2000u, 0, 2200u, -50000);
    obs.continuity_valid = false;
    (void)battery_learning_update(&state, NULL, &obs, 0u);
    battery_learning_get_snapshot(&state, &snapshot);
    assert(!snapshot.capacity_prediction_exhausted);
    obs.continuity_valid = true;
    obs.charger_connected = true;
    obs.charge_active = true;
    (void)battery_learning_update(&state, NULL, &obs, 0u);
    battery_learning_get_snapshot(&state, &snapshot);
    assert(!snapshot.capacity_prediction_exhausted);
}

static void test_recover_historical_full_endpoint_reanchors_current_soc(void) {
    battery_learning_state_t state;
    battery_learning_init(&state, NULL, NULL);
    battery_learning_provision_t provision = {.capacity_mah = 1272u};
    assert(battery_learning_provision(&state, NULL, &provision));
    battery_learning_observation_t current = observation(
        10u, 10000u, INT64_C(2540813625), 2845u, -27073);
    current.acr_raw = UINT32_C(0x800a22c9);
    (void)battery_learning_update(&state, NULL, &current, 0u);

    assert(battery_learning_recover_full_endpoint(
        &state, NULL, &current, UINT32_C(0x800a2cc5),
        INT64_C(2550590325), 1u));
    battery_learning_snapshot_t snapshot;
    battery_learning_get_snapshot(&state, &snapshot);
    assert(snapshot.full_anchor_valid);
    assert(snapshot.capacity_cycle_qualified);
    assert(snapshot.soc_confidence == BATTERY_SOC_CONFIDENCE_ANCHORED);
    assert(snapshot.remaining_capacity_nah == UINT64_C(1262223300));
    assert(snapshot.soc_bars_valid && snapshot.soc_bars == 4u);
    assert(state.persisted.full_anchor_acr_raw == UINT32_C(0x800a2cc5));
    assert(state.persisted.full_anchor_nah == INT64_C(2550590325));

    battery_learning_state_t mismatched = state;
    mismatched.persisted.full_anchor_valid = false;
    mismatched.persisted.capacity_cycle_qualified = false;
    assert(!battery_learning_recover_full_endpoint(
        &mismatched, NULL, &current, UINT32_C(0x800a2cc4),
        INT64_C(2550590325), 1u));

    battery_learning_state_t recovered = state;
    assert(battery_learning_recover_full_endpoint(
        &state, NULL, &current, UINT32_C(0x800a2cc5),
        INT64_C(2550590325), 1u));
    assert(memcmp(&state, &recovered, sizeof(state)) == 0);
    current.charger_connected = true;
    assert(!battery_learning_recover_full_endpoint(
        &state, NULL, &current, UINT32_C(0x800a2cc5),
        INT64_C(2550590325), 1u));
}

static void test_randomized_public_api_invariants(void) {
    battery_learning_state_t state;
    battery_learning_init(&state, NULL, NULL);
    uint32_t random = UINT32_C(0x32102350);
    uint32_t sequence = UINT32_MAX - 100u;
    uint32_t now_ms = UINT32_MAX - 5000u;
    int64_t delta_nah = 0;
    uint32_t gauge_session = 0u;
    const uint32_t valid_results =
        BATTERY_LEARNING_RESULT_PERSIST |
        BATTERY_LEARNING_RESULT_FULL_ANCHORED |
        BATTERY_LEARNING_RESULT_CAPACITY_ACCEPTED |
        BATTERY_LEARNING_RESULT_CAPACITY_REJECTED |
        BATTERY_LEARNING_RESULT_RESISTANCE_UPDATED |
        BATTERY_LEARNING_RESULT_PACK_RESET |
        BATTERY_LEARNING_RESULT_SOC_BOOTSTRAPPED |
        BATTERY_LEARNING_RESULT_SOC_REANCHORED |
        BATTERY_LEARNING_RESULT_SOC_INVALIDATED;

    for (uint32_t i = 0u; i < 250000u; i++) {
        uint32_t bits = random_next(&random);
        if ((bits & 7u) != 0u) {
            sequence++;
        }
        now_ms += (random_next(&random) % 2001u);
        delta_nah += (int32_t)(random_next(&random) % 8000001u) - 6000000;
        if (i != 0u && i % 50000u == 0u) {
            gauge_session++;
        }
        battery_learning_observation_t obs = observation(
            sequence, now_ms, delta_nah,
            (uint16_t)(1700u + random_next(&random) % 1201u),
            (int32_t)(random_next(&random) % 1300001u) - 1000000);
        obs.gauge_session = gauge_session;
        obs.acr_raw = random_next(&random);
        obs.temperature_mdegc =
            (int32_t)(random_next(&random) % 60001u) - 10000;
        bits = random_next(&random);
        obs.sample_valid = (bits & 1u) != 0u;
        obs.current_valid = (bits & 2u) != 0u;
        obs.continuity_valid = (bits & 4u) != 0u;
        obs.reference_valid = (bits & 64u) != 0u;
        obs.charger_connected = (bits & 8u) != 0u;
        obs.charge_active = (bits & 16u) != 0u;
        obs.authoritative = (bits & 32u) != 0u;
        uint32_t events = (random_next(&random) >> 29u) &
                          (BATTERY_LEARNING_EVENT_FULL |
                           BATTERY_LEARNING_EVENT_EMPTY);
        uint32_t result = battery_learning_update(
            &state, NULL, &obs, events);
        assert((result & ~valid_results) == 0u);
        assert_state_invariants(&state);
    }
}

static void test_persisted_semantic_validation_fails_closed(void) {
    battery_learning_persisted_t persisted;
    battery_learning_persisted_defaults(&persisted, NULL);
    assert(battery_learning_persisted_valid(&persisted, NULL));

    persisted.capacity_history_count = 1u;
    persisted.capacity_history_next = 1u;
    persisted.capacity_history_mah[0] = 1000u;
    persisted.last_capacity_mah = 1000u;
    persisted.accepted_capacity_cycles = 1u;
    assert(battery_learning_persisted_valid(&persisted, NULL));

    battery_learning_persisted_t broken = persisted;
    broken.capacity_history_next = 2u;
    assert(!battery_learning_persisted_valid(&broken, NULL));
    broken = persisted;
    broken.profile_id = 1u;
    assert(!battery_learning_persisted_valid(&broken, NULL));
    broken = persisted;
    broken.profile_id = 0u;
    assert(!battery_learning_persisted_valid(&broken, NULL));
    broken = persisted;
    broken.nominal_capacity_mah++;
    assert(!battery_learning_persisted_valid(&broken, NULL));
    broken = persisted;
    broken.resistance_mohm[0] = 49u;
    assert(!battery_learning_persisted_valid(&broken, NULL));
    broken = persisted;
    broken.full_anchor_valid = false;
    broken.capacity_cycle_qualified = true;
    assert(!battery_learning_persisted_valid(&broken, NULL));
    broken = persisted;
    broken.full_anchor_valid = true;
    broken.natural_empty_valid = true;
    assert(!battery_learning_persisted_valid(&broken, NULL));
    broken = persisted;
    broken.full_anchor_valid = true;
    broken.cycle_min_temperature_mdegc = 30000;
    broken.cycle_max_temperature_mdegc = 20000;
    assert(!battery_learning_persisted_valid(&broken, NULL));

    broken = persisted;
    broken.soc_valid = true;
    broken.soc_capacity_mah = 1225u;
    broken.soc_charge_factor_permille = 1224u;
    broken.soc_anchor_provenance = BATTERY_SOC_PROVENANCE_TRACKED;
    broken.soc_confidence = BATTERY_SOC_CONFIDENCE_PROVISIONAL;
    broken.soc_anchor_remaining_nah = INT64_C(500000000);
    assert(!battery_learning_persisted_valid(&broken, NULL));
    broken.soc_bootstrap_reference_mv = 2500u;
    assert(battery_learning_persisted_valid(&broken, NULL));
    broken.soc_anchor_remaining_nah = INT64_C(1225000001);
    assert(!battery_learning_persisted_valid(&broken, NULL));
    broken.soc_anchor_remaining_nah = INT64_C(500000000);
    broken.soc_charge_factor_permille = 999u;
    assert(!battery_learning_persisted_valid(&broken, NULL));
    broken.soc_charge_factor_permille = 1224u;
    broken.soc_confidence = BATTERY_SOC_CONFIDENCE_NONE;
    assert(!battery_learning_persisted_valid(&broken, NULL));

    battery_learning_state_t state;
    battery_learning_init(&state, NULL, &broken);
    battery_learning_snapshot_t snapshot;
    battery_learning_get_snapshot(&state, &snapshot);
    assert(snapshot.nominal_capacity_mah == 1225u);
    assert(!snapshot.learned_capacity_valid);
    assert(!snapshot.full_anchor_valid);
}

int main(void) {
    test_defaults_and_full_anchor_remaining();
    test_capacity_history_confidence_and_soh();
    test_repeated_early_empty_adapts_aging_downward();
    test_natural_empty_marker_survives_and_charge_consumes_it();
    test_connected_noncharging_empty_remains_natural();
    test_recharge_temperature_and_bounds_reject_cycles();
    test_persisted_anchor_survives_restart();
    test_new_hardware_session_clears_exact_pack_learning();
    test_resistance_median_and_bins();
    test_resistance_rejects_bad_evidence();
    test_duplicate_wrap_and_debug_observations_are_inert();
    test_exact_capacity_and_temperature_boundaries();
    test_capacity_history_ring_replaces_oldest();
    test_event_conflict_and_invalid_full_are_inert();
    test_unverified_current_blocks_coulomb_estimates();
    test_cached_sample_invalidation_hides_live_projection();
    test_soc_bootstrap_piecewise_accounting_and_restart();
    test_soc_endpoint_authority_and_session_replacement();
    test_extreme_persisted_anchor_arithmetic_is_total();
    test_recharge_delta_and_resistance_boundaries();
    test_provision_replaces_health_without_importing_anchor();
    test_provision_validation_is_atomic();
    test_recovered_cycle_appends_and_preserves_full_evidence();
    test_capacity_soc_predicts_zero_without_manufacturing_empty();
    test_capacity_prediction_requires_observed_qualified_continuity();
    test_recover_historical_full_endpoint_reanchors_current_soc();
    test_randomized_public_api_invariants();
    test_persisted_semantic_validation_fails_closed();
    puts("battery learning logic tests passed");
    return 0;
}

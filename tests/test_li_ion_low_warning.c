#include <assert.h>
#include <stdio.h>

#include "apps/battery_status_logic.h"
#include "hal/battery_gauge_logic.h"

static battery_gauge_observation_t obs(uint32_t now, uint32_t sequence,
                                       uint16_t mv) {
    return (battery_gauge_observation_t){
        .now_ms = now, .sample_sequence = sequence, .gauge_session = 1u,
        .terminal_mv = mv, .current_ua = -60000,
        .session_delta_nah = -(int64_t)now * 60000 / 3600,
        .sample_valid = true, .current_valid = true, .continuity_valid = true,
    };
}

static void sample(battery_gauge_state_t *gauge, uint32_t now, uint32_t seq,
                    uint16_t mv) {
    battery_gauge_observation_t sample = obs(now, seq, mv);
    (void)battery_gauge_update(gauge, battery_gauge_li_ion_profile(), &sample);
}

static void test_notice_dwell_and_recovery(void) {
    battery_gauge_state_t gauge;
    battery_gauge_init(&gauge);
    sample(&gauge, 0u, 1u, 3700u);
    sample(&gauge, 1000u, 2u, 3599u);
    sample(&gauge, 6000u, 3u, 3599u);
    sample(&gauge, 10999u, 4u, 3599u);
    assert(!gauge.snapshot.low_notice);
    sample(&gauge, 11000u, 5u, 3599u);
    assert(gauge.snapshot.low_notice);
    assert(gauge.snapshot.warning == BATTERY_GAUGE_WARNING_HEALTHY);

    /* Hysteresis and recovery dwell keep brief load releases from restarting
     * the app's immediate/one-minute warning cadence. */
    sample(&gauge, 16000u, 6u, 3649u);
    sample(&gauge, 21000u, 7u, 3650u);
    sample(&gauge, 26000u, 8u, 3650u);
    assert(gauge.snapshot.low_notice);
    sample(&gauge, 31000u, 9u, 3650u);
    assert(!gauge.snapshot.low_notice);

    sample(&gauge, 36000u, 10u, 3599u);
    sample(&gauge, 41000u, 11u, 3600u);
    sample(&gauge, 46000u, 12u, 3599u);
    sample(&gauge, 51000u, 13u, 3599u);
    assert(!gauge.snapshot.low_notice);
    sample(&gauge, 56000u, 14u, 3599u);
    assert(gauge.snapshot.low_notice);
}

static void test_brief_sag_and_wrap(void) {
    battery_gauge_state_t gauge;
    battery_gauge_init(&gauge);
    const uint32_t start = UINT32_MAX - 5000u;
    sample(&gauge, start, 1u, 3800u);
    for (uint32_t ms = 250u; ms < 10000u; ms += 250u) {
        sample(&gauge, start + ms, ms / 250u + 1u, 3400u);
        assert(!gauge.snapshot.low_notice);
    }
    sample(&gauge, start + 10000u, 41u, 3800u);
    assert(!gauge.snapshot.low_notice);

    for (uint32_t ms = 11000u; ms <= 21000u; ms += 250u) {
        sample(&gauge, start + ms, ms / 250u + 1u, 3590u);
    }
    assert(gauge.snapshot.low_notice);
}

static void test_stale_invalid_and_session(void) {
    battery_gauge_state_t gauge;
    battery_gauge_init(&gauge);
    sample(&gauge, 0u, 1u, 3550u);
    for (uint32_t now = 1000u; now <= 10000u; now += 1000u) {
        sample(&gauge, now, 1u, 3550u);
    }
    assert(!gauge.snapshot.low_notice);
    sample(&gauge, 10000u, 2u, 3550u);
    assert(!gauge.snapshot.low_notice); /* Two readings are insufficient. */
    sample(&gauge, 20001u, 3u, 3550u);
    assert(!gauge.snapshot.low_notice); /* Gap resets the dwell. */
    sample(&gauge, 25001u, 4u, 3550u);
    battery_gauge_observation_t invalid = obs(26000u, 5u, 3550u);
    invalid.sample_valid = false;
    (void)battery_gauge_update(&gauge, battery_gauge_li_ion_profile(), &invalid);
    sample(&gauge, 30001u, 6u, 3550u);
    sample(&gauge, 35001u, 7u, 3550u);
    assert(!gauge.snapshot.low_notice);
    sample(&gauge, 40001u, 8u, 3550u);
    assert(gauge.snapshot.low_notice);

    invalid.now_ms = 41000u;
    invalid.sample_sequence = 9u;
    (void)battery_gauge_update(&gauge, battery_gauge_li_ion_profile(), &invalid);
    assert(gauge.snapshot.low_notice && !gauge.snapshot.valid);
    sample(&gauge, 42000u, 10u, 3550u);
    assert(gauge.snapshot.low_notice);

    battery_gauge_observation_t replacement = obs(43000u, 11u, 3550u);
    replacement.gauge_session++;
    (void)battery_gauge_update(&gauge, battery_gauge_li_ion_profile(), &replacement);
    assert(!gauge.snapshot.low_notice);
}

static void test_charging_and_endpoint_separation(void) {
    battery_gauge_state_t gauge;
    battery_gauge_init(&gauge);
    sample(&gauge, 0u, 1u, 3590u);
    sample(&gauge, 5000u, 2u, 3590u);
    sample(&gauge, 10000u, 3u, 3590u);
    assert(gauge.snapshot.low_notice);
    assert(gauge.snapshot.warning == BATTERY_GAUGE_WARNING_HEALTHY);
    battery_endpoint_state_t endpoint;
    battery_endpoint_state_init(&endpoint);
    battery_endpoint_evidence_t evidence = {
        .battery_sample_valid = true,
        .capacity_prediction_exhausted = true,
        .gauge_low = gauge.snapshot.warning == BATTERY_GAUGE_WARNING_LOW,
        .gauge_empty = gauge.snapshot.warning == BATTERY_GAUGE_WARNING_EMPTY,
    };
    battery_endpoint_decision_t decision = battery_endpoint_step(&endpoint, &evidence);
    assert(!decision.shutdown && !decision.learn_empty);
    assert(battery_warning_action(true, false, false, decision.shutdown,
        decision.force_low || gauge.snapshot.low_notice) == BATTERY_WARNING_ACTION_LOW);

    battery_gauge_observation_t charging = obs(11000u, 4u, 3550u);
    charging.charge_active = true;
    /* ACTIVE status by itself does not mean the battery is recovering. */
    (void)battery_gauge_update(&gauge, battery_gauge_li_ion_profile(), &charging);
    assert(gauge.snapshot.low_notice);
    charging.now_ms = 12000u;
    charging.sample_sequence++;
    charging.current_ua = 200000;
    charging.current_valid = false;
    (void)battery_gauge_update(&gauge, battery_gauge_li_ion_profile(), &charging);
    assert(gauge.snapshot.low_notice);
    charging.now_ms = 13000u;
    charging.sample_sequence++;
    charging.current_valid = true;
    (void)battery_gauge_update(&gauge, battery_gauge_li_ion_profile(), &charging);
    assert(!gauge.snapshot.low_notice);

    battery_gauge_init(&gauge);
    for (uint32_t now = 0u; now <= 30000u; now += 1000u) {
        battery_gauge_observation_t nimh = obs(now, now / 1000u + 1u, 2500u);
        (void)battery_gauge_update(&gauge, battery_gauge_nimh_profile(), &nimh);
        assert(!gauge.snapshot.low_notice);
    }
}

static void test_maintenance_wake_qualification(void) {
    battery_gauge_state_t gauge;
    battery_gauge_init(&gauge);
    sample(&gauge, 0u, 1u, 3700u);
    sample(&gauge, 60000u, 2u, 3590u);
    assert(gauge.low_notice.pending && !gauge.snapshot.low_notice);
    /* HAL vetoes standby while pending; the ordinary awake cadence supplies
     * fresh samples without adding a recurring battery wakeup. */
    for (uint32_t second = 1u; second <= 10u; second++) {
        sample(&gauge, 60000u + second * 1000u, 2u + second, 3580u);
    }
    assert(gauge.snapshot.low_notice && !gauge.low_notice.pending);
}

static void test_discharge_ramp(unsigned mv_per_100s, unsigned sample_ms,
                               bool step_drop) {
    battery_gauge_state_t gauge;
    battery_endpoint_state_t endpoint;
    battery_raw_collapse_evidence_t floor;
    battery_gauge_init(&gauge);
    battery_endpoint_state_init(&endpoint);
    battery_raw_collapse_evidence_init(&floor);
    const battery_gauge_profile_t *profile = battery_gauge_li_ion_profile();
    bool saw_low = false;
    uint16_t first_low_mv = 0u;
    uint32_t sequence = 0u;
    for (uint32_t now = 0u; now < 4000000u; now += sample_ms) {
        uint32_t elapsed = now > 120000u ? now - 120000u : 0u;
        uint32_t drop = step_drop ? (elapsed ? 360u : 0u)
                                 : elapsed * mv_per_100s / 100000u;
        uint16_t mv = (uint16_t)(drop < 360u ? 3650u - drop : 3290u);
        sample(&gauge, now, ++sequence, mv);
        battery_endpoint_evidence_t evidence = {
            .battery_sample_valid = true, .anchored_capacity_has_charge = true,
            .gauge_low = gauge.snapshot.warning == BATTERY_GAUGE_WARNING_LOW,
            .gauge_empty = gauge.snapshot.warning == BATTERY_GAUGE_WARNING_EMPTY,
            .operating_floor_confirmed = battery_raw_collapse_evidence_update(
                &floor, true, sequence, mv, profile->operating_empty_mv),
        };
        battery_endpoint_decision_t decision = battery_endpoint_step(&endpoint, &evidence);
        battery_warning_action_t action = battery_warning_action(
            true, false, false, decision.shutdown,
            evidence.gauge_low || decision.force_low || gauge.snapshot.low_notice);
        if (action == BATTERY_WARNING_ACTION_EMPTY) {
            assert(mv < profile->operating_empty_mv);
            assert(decision.learn_empty);
            /* A sudden electrical collapse must not wait for a warning. */
            assert(step_drop ? !saw_low : saw_low);
            printf("Li-ion ramp %u/100 mV/s, %u ms: first LOW=%u, EMPTY=%u mV\n",
                   mv_per_100s, sample_ms, first_low_mv, mv);
            return;
        }
        if (action == BATTERY_WARNING_ACTION_LOW && !saw_low) {
            saw_low = true;
            first_low_mv = mv;
            assert(mv > profile->operating_empty_mv);
        }
    }
    assert(!"ramp did not reach EMPTY");
}

int main(void) {
    test_notice_dwell_and_recovery();
    test_brief_sag_and_wrap();
    test_stale_invalid_and_session();
    test_charging_and_endpoint_separation();
    test_maintenance_wake_qualification();
    const unsigned rates[] = {20u, 100u, 400u, 500u, 1000u};
    const unsigned periods[] = {250u, 1000u, 5000u};
    for (unsigned r = 0; r < sizeof(rates) / sizeof(rates[0]); r++) {
        for (unsigned p = 0; p < sizeof(periods) / sizeof(periods[0]); p++) {
            test_discharge_ramp(rates[r], periods[p], false);
        }
    }
    test_discharge_ramp(0u, 1000u, true);
    test_discharge_ramp(0u, 5000u, true);
    puts("Li-ion early LOW and unchanged EMPTY tests passed");
    return 0;
}

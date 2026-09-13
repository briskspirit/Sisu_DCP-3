#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "diag/telit_diag_logic.h"

static unsigned s_failures;

static void check(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        s_failures++;
    }
}

static telit_diag_observation_t observed(bool pg, uint16_t raw) {
    telit_diag_observation_t value = {
        .power_good = pg,
        .status_valid = true,
        .status_fresh = true,
        .status_raw = raw,
    };
    return value;
}

static void feed_status_samples(telit_diag_logic_t *logic,
                                uint32_t start_ms, uint32_t end_ms,
                                uint16_t raw) {
    for (uint32_t at = start_ms; at <= end_ms; at += 10u) {
        telit_diag_logic_tick(logic, at, observed(true, raw));
    }
}

static void test_initial_state_and_rail_gate(void) {
    telit_diag_logic_t logic;
    telit_diag_logic_init(&logic);
    check(!logic.rail_enabled && !logic.force_pwm &&
              !logic.dtr_sleep_permitted &&
              logic.pulse == TELIT_DIAG_PULSE_NONE,
          "initial state is electrically inert");
    check(!telit_diag_uart_tx_allowed(&logic, observed(true, 0u)),
          "UART TX is blocked with the rail off");

    check(telit_diag_request_rail_on(&logic, false) == TELIT_DIAG_OK,
          "rail can be enabled in power-save mode");
    check(logic.rail_enabled && !logic.force_pwm,
          "rail-on request publishes requested PMIC mode");
    check(telit_diag_request_rail_on(&logic, true) ==
              TELIT_DIAG_ERR_STATE,
          "repeated rail-on cannot silently change PMIC mode");
    check(telit_diag_request_rail_off(&logic, observed(false, 0u)) ==
              TELIT_DIAG_ERR_NEEDS_THRESHOLD,
          "rail cut requires an explicit measured off threshold");
}

static void test_boot_pulse_bounds_and_deadline(void) {
    telit_diag_logic_t logic;
    telit_diag_logic_init(&logic);
    (void)telit_diag_request_rail_on(&logic, false);

    check(telit_diag_request_boot_pulse(
              &logic, 100u, observed(false, 0u), 1000u) ==
              TELIT_DIAG_ERR_NEEDS_PG,
          "boot pulse requires PMIC PG");
    check(telit_diag_request_boot_pulse(
              &logic, 100u, observed(true, 0u), 999u) ==
              TELIT_DIAG_ERR_RANGE,
          "boot pulse rejects below guide minimum");
    check(telit_diag_request_boot_pulse(
              &logic, 100u, observed(true, 0u), 2001u) ==
              TELIT_DIAG_ERR_RANGE,
          "boot pulse rejects above guide maximum");
    check(telit_diag_request_boot_pulse(
              &logic, 100u, observed(true, 0u), 1000u) ==
              TELIT_DIAG_OK,
          "boot pulse accepts lower safe boundary");
    check(telit_diag_on_off_asserted(&logic) && logic.boot_attempted,
          "boot pulse asserts ON_OFF and latches boot attempt");
    check(logic.pulse_width_ms == 1000u &&
              logic.pulse_sequence != 0u,
          "pulse publishes bounded width and nonzero identity");
    check(!telit_diag_uart_tx_allowed(&logic, observed(true, 0u)),
          "UART TX is blocked during a control pulse");

    telit_diag_logic_tick(&logic, 1099u, observed(true, 0u));
    check(telit_diag_on_off_asserted(&logic),
          "boot pulse remains asserted below deadline");
    telit_diag_logic_tick(&logic, 1100u, observed(true, 0u));
    check(!telit_diag_on_off_asserted(&logic),
          "boot pulse deasserts exactly at deadline");
    check(telit_diag_uart_tx_allowed(&logic, observed(true, 0u)),
          "UART TX is allowed after pulse completion and PG");
}

static void test_pulse_wrap_and_pg_loss(void) {
    telit_diag_logic_t logic;
    telit_diag_logic_init(&logic);
    (void)telit_diag_request_rail_on(&logic, false);
    check(telit_diag_request_boot_pulse(
              &logic, UINT32_MAX - 499u, observed(true, 0u), 1000u) ==
              TELIT_DIAG_OK,
          "boot pulse starts across uint32 wrap");
    telit_diag_logic_tick(&logic, 499u, observed(true, 0u));
    check(telit_diag_on_off_asserted(&logic),
          "wrapped pulse remains active below its deadline");
    telit_diag_logic_tick(&logic, 500u, observed(true, 0u));
    check(!telit_diag_on_off_asserted(&logic),
          "wrapped deadline expires at the correct instant");

    telit_diag_logic_init(&logic);
    (void)telit_diag_request_rail_on(&logic, false);
    (void)telit_diag_request_boot_pulse(
        &logic, 10u, observed(true, 0u), 1000u);
    telit_diag_logic_tick(&logic, 20u, observed(false, 0u));
    check(!telit_diag_on_off_asserted(&logic),
          "PG loss immediately deasserts an active control pulse");
    check(!logic.boot_attempted,
          "PG loss during boot revokes destructive-control authorization");
}

static void test_off_threshold_interlock(void) {
    telit_diag_logic_t logic;
    telit_diag_logic_init(&logic);
    check(telit_diag_set_off_threshold(
              &logic, TELIT_DIAG_OFF_THRESHOLD_MAX_RAW + 1u) ==
              TELIT_DIAG_ERR_RANGE,
          "off threshold cannot enter the ADC on-level range");
    check(telit_diag_set_off_threshold(&logic, 100u) == TELIT_DIAG_OK,
          "measured low threshold is accepted");
    (void)telit_diag_request_rail_on(&logic, true);

    telit_diag_observation_t invalid = observed(true, 0u);
    invalid.status_valid = false;
    check(telit_diag_request_rail_off(&logic, invalid) ==
              TELIT_DIAG_ERR_NEEDS_STATUS,
          "ADC failure can never masquerade as PWRMON low");
    check(telit_diag_request_rail_off(&logic, observed(true, 101u)) ==
              TELIT_DIAG_ERR_STATUS_HIGH,
          "rail cut is refused above the measured threshold");
    check(logic.rail_enabled && logic.force_pwm,
          "refused rail cut preserves state");
    check(telit_diag_request_rail_off(&logic, observed(true, 100u)) ==
              TELIT_DIAG_ERR_STATUS_UNSTABLE,
          "one low sample cannot authorize a rail cut");
    feed_status_samples(
        &logic, 1000u,
        1000u + TELIT_DIAG_OFF_STABLE_MS - 10u, 100u);
    check(telit_diag_request_rail_off(&logic, observed(true, 100u)) ==
              TELIT_DIAG_ERR_STATUS_UNSTABLE,
          "rail cut remains blocked below the stable-low deadline");
    telit_diag_logic_tick(
        &logic, 1000u + TELIT_DIAG_OFF_STABLE_MS,
        observed(true, 100u));
    check(telit_diag_request_rail_off(&logic, observed(true, 100u)) ==
              TELIT_DIAG_OK,
          "rail cut is allowed after continuous low status");
    check(!logic.rail_enabled && !logic.force_pwm,
          "accepted rail cut clears rail and forced PWM");
}

static void test_off_stability_resets_on_noise(void) {
    telit_diag_logic_t logic;
    telit_diag_logic_init(&logic);
    (void)telit_diag_set_off_threshold(&logic, 100u);
    (void)telit_diag_request_rail_on(&logic, false);
    feed_status_samples(&logic, 100u, 490u, 90u);
    telit_diag_logic_tick(&logic, 500u, observed(true, 101u));
    feed_status_samples(&logic, 510u, 1000u, 90u);
    check(!logic.status_low_stable,
          "an above-threshold sample restarts the low-status window");
    telit_diag_logic_tick(&logic, 1010u, observed(true, 90u));
    check(logic.status_low_stable,
          "a fresh full low-status window authorizes rail cut");

    telit_diag_observation_t invalid = observed(true, 90u);
    invalid.status_valid = false;
    telit_diag_logic_tick(&logic, 1020u, invalid);
    check(!logic.status_low_tracking && !logic.status_low_stable,
          "an invalid ADC sample revokes low-status evidence");

    feed_status_samples(
        &logic, 1100u, 1100u + TELIT_DIAG_OFF_STABLE_MS, 90u);
    check(logic.status_low_stable,
          "pre-pulse low-status evidence is established for the probe");
    (void)telit_diag_request_boot_pulse(
        &logic, 1700u, observed(true, 90u), 1000u);
    telit_diag_logic_tick(&logic, 1800u, observed(true, 90u));
    check(!logic.status_low_tracking && !logic.status_low_stable,
          "a control pulse revokes and suspends rail-cut evidence");

    telit_diag_abort_pulse(&logic);
    telit_diag_logic_tick(&logic, 1900u, observed(true, 90u));
    telit_diag_observation_t stale = observed(true, 90u);
    stale.status_fresh = false;
    telit_diag_logic_tick(
        &logic, 1900u + TELIT_DIAG_STATUS_SAMPLE_MAX_GAP_MS + 1u,
        stale);
    check(!logic.status_low_tracking && !logic.status_low_stable,
          "a status-sampling gap revokes rail-cut evidence");
}

static void test_dtr_policy(void) {
    telit_diag_logic_t logic;
    telit_diag_logic_init(&logic);
    check(telit_diag_request_dtr(
              &logic, true, observed(true, 0u)) ==
              TELIT_DIAG_ERR_STATE,
          "sleep-permitting DTR high is blocked with rail off");
    (void)telit_diag_request_rail_on(&logic, false);
    check(telit_diag_request_dtr(
              &logic, true, observed(false, 0u)) ==
              TELIT_DIAG_ERR_NEEDS_PG,
          "sleep-permitting DTR high requires PG");
    check(telit_diag_request_dtr(
              &logic, true, observed(true, 0u)) == TELIT_DIAG_OK &&
              logic.dtr_sleep_permitted,
          "DTR high is allowed only on a powered module");
    telit_diag_logic_tick(&logic, 50u, observed(false, 0u));
    check(!logic.dtr_sleep_permitted,
          "PG loss returns DTR to wake/low");
    check(telit_diag_request_dtr(
              &logic, false, observed(false, 0u)) == TELIT_DIAG_OK,
          "DTR low is always accepted");
}

static void test_hw_off_and_emergency_interlocks(void) {
    telit_diag_logic_t logic;
    telit_diag_logic_init(&logic);
    (void)telit_diag_request_rail_on(&logic, false);
    check(telit_diag_request_hw_off_pulse(
              &logic, 0u, observed(true, 0u), 2500u) ==
              TELIT_DIAG_ERR_STATE,
          "hardware-off pulse requires a prior boot attempt");
    (void)telit_diag_request_boot_pulse(
        &logic, 0u, observed(true, 0u), 1000u);
    telit_diag_logic_tick(&logic, 1000u, observed(true, 0u));

    check(telit_diag_request_hw_off_pulse(
              &logic, 1100u, observed(true, 0u), 2499u) ==
              TELIT_DIAG_ERR_RANGE,
          "hardware-off rejects below documented minimum");
    check(telit_diag_request_hw_off_pulse(
              &logic, 1100u, observed(true, 0u), 2500u) ==
              TELIT_DIAG_OK,
          "hardware-off accepts documented minimum");
    check(telit_diag_on_off_asserted(&logic) &&
              logic.pulse == TELIT_DIAG_PULSE_HW_OFF,
          "hardware-off uses ON_OFF, not emergency shutdown");
    telit_diag_abort_pulse(&logic);
    check(!telit_diag_on_off_asserted(&logic),
          "abort deasserts ON_OFF immediately");

    check(telit_diag_request_emergency_fire(
              &logic, 1200u, observed(true, 0u)) ==
              TELIT_DIAG_ERR_NOT_ARMED,
          "emergency shutdown cannot fire without arming");
    check(telit_diag_request_emergency_arm(
              &logic, 1200u, observed(true, 0u)) == TELIT_DIAG_OK,
          "emergency shutdown can be armed after boot");
    telit_diag_logic_tick(&logic, 1201u, observed(false, 0u));
    check(telit_diag_request_emergency_fire(
              &logic, 1201u, observed(true, 0u)) ==
              TELIT_DIAG_ERR_NOT_ARMED,
          "PG loss invalidates an emergency arm");
    check(telit_diag_request_emergency_arm(
              &logic, 1202u, observed(true, 0u)) ==
              TELIT_DIAG_ERR_STATE,
          "PG loss requires a fresh boot attempt before rearming");
    (void)telit_diag_request_boot_pulse(
        &logic, 1300u, observed(true, 0u), 1000u);
    telit_diag_logic_tick(&logic, 2300u, observed(true, 0u));
    (void)telit_diag_request_emergency_arm(
        &logic, 2400u, observed(true, 0u));
    telit_diag_logic_tick(
        &logic, 2400u + TELIT_DIAG_EMERGENCY_ARM_MS,
        observed(true, 0u));
    check(telit_diag_request_emergency_fire(
              &logic, 2400u + TELIT_DIAG_EMERGENCY_ARM_MS,
              observed(true, 0u)) == TELIT_DIAG_ERR_NOT_ARMED,
          "emergency arm expires at its deadline");

    (void)telit_diag_request_emergency_arm(
        &logic, 8000u, observed(true, 0u));
    check(telit_diag_request_emergency_fire(
              &logic, 8001u, observed(true, 0u)) == TELIT_DIAG_OK,
          "armed emergency action starts its fixed pulse");
    check(telit_diag_emergency_asserted(&logic) &&
              !telit_diag_on_off_asserted(&logic),
          "emergency pulse uses only HW_SHUTDOWN");
    telit_diag_logic_tick(
        &logic, 8001u + TELIT_DIAG_EMERGENCY_PULSE_MS - 1u,
        observed(true, 0u));
    check(telit_diag_emergency_asserted(&logic),
          "emergency pulse remains active below deadline");
    telit_diag_logic_tick(
        &logic, 8001u + TELIT_DIAG_EMERGENCY_PULSE_MS,
        observed(true, 0u));
    check(!telit_diag_emergency_asserted(&logic),
          "emergency pulse ends at its fixed deadline");
}

static void test_abort_and_same_kind_restart(void) {
    telit_diag_logic_t logic;
    telit_diag_logic_init(&logic);
    (void)telit_diag_request_rail_on(&logic, false);
    (void)telit_diag_request_boot_pulse(
        &logic, 100u, observed(true, 0u), 1000u);
    uint32_t first_sequence = logic.pulse_sequence;
    telit_diag_abort_pulse(&logic);
    check(!logic.boot_attempted,
          "aborted boot cannot authorize hardware-off or emergency");
    check(telit_diag_request_hw_off_pulse(
              &logic, 101u, observed(true, 0u), 2500u) ==
              TELIT_DIAG_ERR_STATE,
          "hardware-off remains blocked after an aborted boot");

    check(telit_diag_request_boot_pulse(
              &logic, 102u, observed(true, 0u), 1000u) ==
              TELIT_DIAG_OK,
          "boot can be retried after an abort");
    check(logic.pulse_sequence != first_sequence,
          "same-kind retry receives a fresh output identity");
}

static uint32_t next_random(uint32_t *state) {
    uint32_t value = *state;
    value ^= value << 13u;
    value ^= value >> 17u;
    value ^= value << 5u;
    *state = value;
    return value;
}

static void test_randomized_public_api_invariants(void) {
    telit_diag_logic_t logic;
    telit_diag_logic_init(&logic);
    uint32_t random_state = 0x51f0cafeu;
    uint32_t now = 0u;

    for (uint32_t i = 0u; i < 50000u; i++) {
        uint32_t random = next_random(&random_state);
        now += random & 0x1fu;
        telit_diag_observation_t observation = {
            .power_good = (random & (1u << 5u)) != 0u,
            .status_valid = (random & (1u << 6u)) != 0u,
            .status_fresh = (random & (1u << 7u)) != 0u,
            .status_raw = (uint16_t)((random >> 8u) & 0xfffu),
        };
        telit_diag_logic_tick(&logic, now, observation);

        switch ((random >> 20u) % 10u) {
        case 0u:
            (void)telit_diag_request_rail_on(
                &logic, (random & (1u << 30u)) != 0u);
            break;
        case 1u:
            (void)telit_diag_set_off_threshold(
                &logic, (uint16_t)(random & 0x7ffu));
            break;
        case 2u:
            (void)telit_diag_request_rail_off(&logic, observation);
            break;
        case 3u:
            (void)telit_diag_request_boot_pulse(
                &logic, now, observation, 900u + (random & 0x4ffu));
            break;
        case 4u:
            (void)telit_diag_request_hw_off_pulse(
                &logic, now, observation, 2400u + (random & 0xaffu));
            break;
        case 5u:
            (void)telit_diag_request_emergency_arm(
                &logic, now, observation);
            break;
        case 6u:
            (void)telit_diag_request_emergency_fire(
                &logic, now, observation);
            break;
        case 7u:
            (void)telit_diag_request_dtr(
                &logic, (random & (1u << 31u)) != 0u, observation);
            break;
        case 8u:
            telit_diag_abort_pulse(&logic);
            break;
        default:
            break;
        }

        check(!(telit_diag_on_off_asserted(&logic) &&
                telit_diag_emergency_asserted(&logic)),
              "random walk never asserts both Telit control inputs");
        check(logic.pulse == TELIT_DIAG_PULSE_NONE ||
                  (logic.rail_enabled && logic.pulse_sequence != 0u),
              "random walk permits a pulse only on an enabled rail");
        check(!logic.status_low_stable ||
                  (logic.status_low_tracking &&
                   logic.off_threshold_set && logic.rail_enabled &&
                   logic.pulse == TELIT_DIAG_PULSE_NONE),
              "random walk retains complete rail-cut evidence");
        check(!logic.emergency_armed ||
                  (logic.rail_enabled && logic.boot_attempted &&
                   logic.pulse == TELIT_DIAG_PULSE_NONE),
              "random walk arms emergency only after a live boot attempt");
        check(!telit_diag_uart_tx_allowed(&logic, observation) ||
                  (logic.rail_enabled && observation.power_good &&
                   logic.pulse == TELIT_DIAG_PULSE_NONE),
              "random walk never permits UART TX outside its power gate");
    }
}

int main(void) {
    test_initial_state_and_rail_gate();
    test_boot_pulse_bounds_and_deadline();
    test_pulse_wrap_and_pg_loss();
    test_off_threshold_interlock();
    test_off_stability_resets_on_noise();
    test_dtr_policy();
    test_hw_off_and_emergency_interlocks();
    test_abort_and_same_kind_restart();
    test_randomized_public_api_invariants();
    if (s_failures != 0u) {
        fprintf(stderr, "%u failure(s)\n", s_failures);
        return 1;
    }
    puts("telit diagnostic safety logic tests passed");
    return 0;
}

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "hal/battery_gauge_logic.h"

static int s_failures;

static void check(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        s_failures++;
    }
}

static void check_u32(uint32_t got, uint32_t want, const char *message) {
    if (got != want) {
        fprintf(stderr, "FAIL: %s (got %lu want %lu)\n", message,
                (unsigned long)got, (unsigned long)want);
        s_failures++;
    }
}

static void check_i32(int32_t got, int32_t want, const char *message) {
    if (got != want) {
        fprintf(stderr, "FAIL: %s (got %ld want %ld)\n", message,
                (long)got, (long)want);
        s_failures++;
    }
}

static void test_bar_ladder(void) {
    check_u32(battery_gauge_bars_from_mv(2600u), 4u, "2600 -> 4 bars");
    check_u32(battery_gauge_bars_from_mv(VBAT_BAR4_MV), 4u,
              "4-bar floor is inclusive");
    check_u32(battery_gauge_bars_from_mv(VBAT_BAR4_MV - 1u), 3u,
              "just below 4-bar floor -> 3");
    check_u32(battery_gauge_bars_from_mv(VBAT_BAR3_MV), 3u,
              "3-bar floor is inclusive");
    check_u32(battery_gauge_bars_from_mv(VBAT_BAR3_MV - 1u), 2u,
              "just below 3-bar floor -> 2");
    check_u32(battery_gauge_bars_from_mv(VBAT_BAR2_MV), 2u,
              "2-bar floor is inclusive");
    check_u32(battery_gauge_bars_from_mv(VBAT_BAR2_MV - 1u), 1u,
              "the authentic wide 1-bar band starts below 2425 mV");
    check_u32(battery_gauge_bars_from_mv(VBAT_BAR1_MV), 1u,
              "1-bar floor is inclusive");
    check_u32(battery_gauge_bars_from_mv(VBAT_BAR1_MV - 1u), 0u,
              "below the empty floor -> 0");
    check_u32(battery_gauge_bar_floor_mv(0u), 0u, "bar zero has no floor");
    check_u32(battery_gauge_bar_floor_mv(5u), VBAT_BAR4_MV,
              "bar-floor lookup clamps above four");
}

static void test_reference_load_correction(void) {
    const battery_gauge_profile_t *profile = battery_gauge_nimh_profile();

    check_i32(battery_gauge_load_correction_mv(
                  profile, -12000, 0, false, false),
              -10, "12 mA idle is normalized down to the 65 mA domain");
    check_i32(battery_gauge_load_correction_mv(
                  profile, -300000, 0, false, false),
              45, "300 mA load receives the 190 mohm droop correction");
    check_i32(battery_gauge_load_correction_mv(
                  profile, -300000, -100000, true, false),
              7, "lower five-second load prevents transient over-correction");
    check_i32(battery_gauge_load_correction_mv(
                  profile, 100000, 100000, true, true),
              -29, "active charging applies Nokia's 85 percent term");
    check_i32(battery_gauge_load_correction_mv(
                  profile, 200000, 200000, true, true),
              -45, "measured 200 mA charging needs no board-current constant");

    battery_gauge_profile_t bounded = *profile;
    bounded.effective_resistance_mohm = 2000u;
    bounded.max_compensation_mv = 50u;
    check_i32(battery_gauge_load_correction_mv(
                  &bounded, -500000, 0, false, false),
              50, "profile correction is bounded");
    bounded.max_compensation_mv = UINT16_MAX;
    check_i32(battery_gauge_load_correction_mv(
                  &bounded, INT32_MIN, 0, false, false),
              INT16_MAX, "public correction never overflows int16_t");
}

static void test_bar_projection(void) {
    const battery_gauge_profile_t *profile = battery_gauge_nimh_profile();
    battery_bar_state_t state;
    battery_gauge_bar_state_init(&state);

    check_u32(battery_gauge_bar_step(
                  &state, profile, 2600u, false, true, 0),
              4u, "first reference estimate seeds immediately");
    check_u32(battery_gauge_bar_step(
                  &state, profile, 2500u, false, true, -1000),
              2u, "falling bars follow the robust estimate immediately");

    battery_gauge_bar_state_init(&state);
    (void)battery_gauge_bar_step(&state, profile, 2400u, false, true, 0);
    check_u32(battery_gauge_bar_step(
                  &state, profile, 2476u, false, true,
                  -BATTERY_GAUGE_RISE_REARM_NAH),
              1u, "1-to-2 rise waits for its midpoint boundary");
    check_u32(battery_gauge_bar_step(
                  &state, profile, 2477u, false, true,
                  -BATTERY_GAUGE_RISE_REARM_NAH),
              2u, "1-to-2 midpoint plus rearm permits the rise");
    check_u32(battery_gauge_bar_step(
                  &state, profile, 2477u, false, true,
                  -BATTERY_GAUGE_RISE_REARM_NAH),
              2u, "1-to-2 midpoint remains stable on a repeated sample");

    battery_gauge_bar_state_init(&state);
    (void)battery_gauge_bar_step(&state, profile, 2500u, false, true, 0);
    check_u32(battery_gauge_bar_step(
                  &state, profile, 2551u, false, true,
                  -BATTERY_GAUGE_RISE_REARM_NAH),
              2u, "2-to-3 rise holds below the upper midpoint");
    check_u32(battery_gauge_bar_step(
                  &state, profile, 2552u, false, true,
                  -BATTERY_GAUGE_RISE_REARM_NAH),
              3u, "2-to-3 rise accepts the recovered midpoint");
    check_u32(battery_gauge_bar_step(
                  &state, profile, 2552u, false, true,
                  -BATTERY_GAUGE_RISE_REARM_NAH),
              3u, "2-to-3 midpoint remains stable on a repeated sample");

    battery_gauge_bar_state_init(&state);
    (void)battery_gauge_bar_step(&state, profile, 2560u, false, true, 0);
    check_u32(battery_gauge_bar_step(
                  &state, profile, 2596u, false, true,
                  -BATTERY_GAUGE_RISE_REARM_NAH),
              3u, "3-to-4 rise holds below the extrapolated top boundary");
    check_u32(battery_gauge_bar_step(
                  &state, profile, 2597u, false, true,
                  -BATTERY_GAUGE_RISE_REARM_NAH),
              4u, "3-to-4 rise accepts Nokia's top extrapolation");
    check_u32(battery_gauge_bar_step(
                  &state, profile, 2597u, false, true,
                  -BATTERY_GAUGE_RISE_REARM_NAH),
              4u, "top extrapolation remains stable on a repeated sample");
    check_u32(battery_gauge_bar_step(
                  &state, profile, 2574u, false, true,
                  -BATTERY_GAUGE_RISE_REARM_NAH),
              3u, "four bars fall directly below the 2575 mV floor");

    battery_gauge_bar_state_init(&state);
    (void)battery_gauge_bar_step(&state, profile, 2500u, false, true, 0);
    check_u32(battery_gauge_bar_step(
                  &state, profile, 2600u, false, true,
                  -(BATTERY_GAUGE_RISE_REARM_NAH - 1)),
              2u, "load release below 150 mAh cannot raise the icon");
    check_u32(battery_gauge_bar_step(
                  &state, profile, 2600u, false, true,
                  -BATTERY_GAUGE_RISE_REARM_NAH),
              4u, "150 mAh discharge rearms an upward projection");

    battery_gauge_bar_state_init(&state);
    (void)battery_gauge_bar_step(&state, profile, 2500u, false, false, 0);
    check_u32(battery_gauge_bar_step(
                  &state, profile, 2600u, true, false, 0),
              4u, "active charging may bypass the discharge rearm gate");
}

static void test_warning_classifier(void) {
    battery_warning_state_t state;
    battery_gauge_warning_state_init(&state);

    check_u32(battery_gauge_warning_step(
                  &state, 0u, 2500u, 2500u, 2500u, 2500u),
              BATTERY_GAUGE_WARNING_HEALTHY,
              "healthy evidence seeds healthy");
    check_u32(battery_gauge_warning_step(
                  &state, 1000u, 2200u, 2200u, 2200u, 2400u),
              BATTERY_GAUGE_WARNING_HEALTHY,
              "healthy-to-low transition starts the 20-second hold");
    check_u32(battery_gauge_warning_step(
                  &state, 20999u, 2200u, 2200u, 2200u, 2400u),
              BATTERY_GAUGE_WARNING_HEALTHY,
              "warning remains held just before 20 seconds");
    check_u32(battery_gauge_warning_step(
                  &state, 21000u, 2200u, 2200u, 2200u, 2400u),
              BATTERY_GAUGE_WARNING_LOW,
              "persistent failure enters low after 20 seconds");
    check_u32(battery_gauge_warning_step(
                  &state, 21001u, 1800u, 1800u, 1800u, 1800u),
              BATTERY_GAUGE_WARNING_EMPTY,
              "low-to-empty transition is immediate");
    check_u32(battery_gauge_warning_step(
                  &state, 21002u, 2500u, 2500u, 2500u, 2500u),
              BATTERY_GAUGE_WARNING_HEALTHY,
              "healthy evidence clears warning state immediately");

    battery_gauge_warning_state_init(&state);
    check_u32(battery_gauge_warning_step(
                  &state, UINT32_MAX - 10u,
                  2200u, 2200u, 2200u, 2400u),
              BATTERY_GAUGE_WARNING_LOW,
              "an unhealthy first estimate is published without a fake delay");
}

static battery_gauge_observation_t observation(
    uint32_t now_ms, uint32_t sequence, uint32_t session,
    uint16_t terminal_mv, int32_t current_ua, int64_t delta_nah) {
    return (battery_gauge_observation_t){
        .now_ms = now_ms,
        .sample_sequence = sequence,
        .gauge_session = session,
        .terminal_mv = terminal_mv,
        .current_ua = current_ua,
        .session_delta_nah = delta_nah,
        .sample_valid = true,
        .current_valid = true,
        .continuity_valid = true,
        .charge_active = false,
    };
}

static void test_estimator_window_and_burst_rejection(void) {
    battery_gauge_state_t state;
    battery_gauge_snapshot_t snapshot;
    battery_gauge_init(&state);

    for (uint32_t second = 0u; second <= 5u; second++) {
        battery_gauge_observation_t obs = observation(
            second * 1000u, second + 1u, 1u, 2600u, -12000,
            -((int64_t)second * 10000 / 3));
        check(battery_gauge_update(
                  &state, battery_gauge_nimh_profile(), &obs),
              "one-Hz observations advance the model");
    }
    battery_gauge_get_snapshot(&state, &snapshot);
    check(snapshot.valid, "estimator publishes a valid snapshot");
    check(snapshot.window_current_valid,
          "five-second ACR history publishes average current");
    check(snapshot.window_current_ua >= -12005 &&
              snapshot.window_current_ua <= -11995,
          "ACR-derived five-second current retains the public sign and scale");
    check_i32(snapshot.correction_mv, -10,
              "idle estimator output is in the 65 mA reference domain");
    check_u32(snapshot.bars, 4u, "healthy full pack projects four bars");

    battery_gauge_observation_t sag = observation(
        6000u, 7u, 1u, 2200u, -300000, -100000);
    check(battery_gauge_update(
              &state, battery_gauge_nimh_profile(), &sag),
          "a burst observation advances the model");
    battery_gauge_get_snapshot(&state, &snapshot);
    check(snapshot.reference_mv > 2550u,
          "one modem-current sag is rejected by the robust slow estimate");
    check_u32(snapshot.bars, 4u,
              "one modem-current sag cannot collapse the icon");
}

static void test_unverified_current_is_neutral(void) {
    battery_gauge_state_t state;
    battery_gauge_snapshot_t snapshot;
    battery_gauge_init(&state);
    battery_gauge_observation_t obs =
        observation(0u, 1u, 1u, 2500u, INT32_MIN, INT64_MIN);
    obs.current_valid = false;
    check(battery_gauge_update(
              &state, battery_gauge_nimh_profile(), &obs),
          "voltage remains usable before current polarity is qualified");
    battery_gauge_get_snapshot(&state, &snapshot);
    check_i32(snapshot.correction_mv, 0,
              "unverified current cannot create a load correction");
    check(!snapshot.window_current_valid,
          "unverified ACR sign cannot create a window current");
}

static void test_estimator_time_and_session_semantics(void) {
    battery_gauge_state_t state;
    battery_gauge_snapshot_t before;
    battery_gauge_snapshot_t after;
    battery_gauge_init(&state);

    battery_gauge_observation_t first =
        observation(100u, 1u, 4u, 2600u, -12000, 0);
    check(battery_gauge_update(
              &state, battery_gauge_nimh_profile(), &first),
          "first sample seeds the estimator");
    battery_gauge_get_snapshot(&state, &before);

    check(!battery_gauge_update(
              &state, battery_gauge_nimh_profile(), &first),
          "duplicate LTC sequence cannot double-step the model");
    battery_gauge_observation_t early =
        observation(600u, 2u, 4u, 2400u, -12000, -1667);
    check(!battery_gauge_update(
              &state, battery_gauge_nimh_profile(), &early),
          "sub-second safety samples do not accelerate the display model");
    battery_gauge_get_snapshot(&state, &after);
    check_u32(after.model_sequence, before.model_sequence,
              "duplicate and sub-second observations preserve model sequence");
    check_u32(after.terminal_mv, 2400u,
              "fresh terminal evidence still updates between model steps");

    battery_gauge_observation_t sparse =
        observation(60100u, 3u, 4u, 2400u, -12000, -200000);
    check(battery_gauge_update(
              &state, battery_gauge_nimh_profile(), &sparse),
          "a sparse dormant observation advances in elapsed-time domain");
    battery_gauge_get_snapshot(&state, &after);
    check(after.reference_mv < 2450u,
          "one-minute dormant gaps do not turn the 15-second filter into 15 minutes");

    battery_gauge_observation_t replacement =
        observation(61100u, 4u, 5u, 2500u, -12000, 0);
    check(battery_gauge_update(
              &state, battery_gauge_nimh_profile(), &replacement),
          "new gauge session reseeds the estimator");
    battery_gauge_get_snapshot(&state, &after);
    check_u32(after.model_sequence, 1u,
              "new gauge session resets model-generation state");
    check(after.reference_mv >= 2500u && after.reference_mv <= 2520u,
          "new gauge session cannot retain the old pack estimate");

    battery_gauge_observation_t invalid = replacement;
    invalid.sample_sequence++;
    invalid.sample_valid = false;
    check(!battery_gauge_update(
              &state, battery_gauge_nimh_profile(), &invalid),
          "invalid observation cannot advance the model");
    battery_gauge_get_snapshot(&state, &after);
    check(!after.valid, "invalid current sample marks live estimate unavailable");
}

static void test_power_on_qualifier(void) {
    battery_power_on_qualifier_t state;
    battery_power_on_qualifier_init(&state);
    check(!battery_power_on_qualifier_ready(&state),
          "empty qualifier is not ready");

    for (uint32_t i = 0u; i < 5u; i++) {
        battery_power_on_qualifier_observe(
            &state, 1u, i + 1u, true, (uint16_t)(2100u + i * 10u));
    }
    check(battery_power_on_qualifier_ready(&state),
          "five valid observations complete qualification");
    check_u32(battery_power_on_qualifier_sample_count(&state), 5u,
              "qualifier retains five valid observations");
    check_u32(battery_power_on_qualifier_average_mv(&state), 2120u,
              "power-on gate uses the five-sample average");

    battery_power_on_qualifier_observe(&state, 1u, 5u, true, 3000u);
    check_u32(battery_power_on_qualifier_average_mv(&state), 2120u,
              "duplicate sequence is ignored");
    battery_power_on_qualifier_observe(&state, 1u, 6u, true, 2200u);
    check_u32(battery_power_on_qualifier_average_mv(&state), 2140u,
              "completed qualifier keeps a rolling fresh five-sample window");

    battery_power_on_qualifier_init(&state);
    for (uint32_t i = 0u; i < BATTERY_POWER_ON_MAX_ATTEMPTS; i++) {
        bool valid = i == 2u || i == 7u;
        battery_power_on_qualifier_observe(
            &state, 2u, i + 1u, valid, valid ? 2200u : 0u);
    }
    check(battery_power_on_qualifier_ready(&state),
          "ten attempts complete a partial-valid bounded acquisition");
    check_u32(battery_power_on_qualifier_sample_count(&state), 2u,
              "invalid attempts do not enter the voltage average");
    check_u32(battery_power_on_qualifier_attempt_count(&state), 10u,
              "bounded qualifier counts invalid conversion attempts");
    check_u32(battery_power_on_qualifier_average_mv(&state), 2200u,
              "partial-valid qualification averages only valid samples");

    battery_power_on_qualifier_observe(&state, 3u, 1u, false, 0u);
    check(!battery_power_on_qualifier_ready(&state),
          "new gauge session resets power-on qualification");
    check_u32(battery_power_on_qualifier_sample_count(&state), 0u,
              "new session cannot reuse old-pack samples");
}

int main(void) {
    test_bar_ladder();
    test_reference_load_correction();
    test_bar_projection();
    test_warning_classifier();
    test_estimator_window_and_burst_rejection();
    test_unverified_current_is_neutral();
    test_estimator_time_and_session_semantics();
    test_power_on_qualifier();

    if (s_failures != 0) {
        fprintf(stderr, "%d battery-gauge assertion(s) failed\n", s_failures);
        return 1;
    }
    puts("battery gauge estimator tests passed");
    return 0;
}

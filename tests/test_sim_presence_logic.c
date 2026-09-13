#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "apps/sim_presence_logic.h"

static int s_failures;

static void check(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        s_failures++;
    }
}

static void test_startup_absence_requires_ready_dwell(void) {
    sim_presence_ui_state_t state;
    sim_presence_ui_init(&state);

    check(!sim_presence_ui_update(&state, false, true, false, 1000u),
          "pre-ready QSS absence is not user-visible");
    check(!sim_presence_ui_update(&state, false, true, false, 10000u),
          "time before modem readiness cannot satisfy the dwell");
    check(!sim_presence_ui_update(&state, true, true, false, 11000u),
          "ready absence starts a fresh dwell");
    check(!sim_presence_ui_update(
              &state, true, true, false,
              11000u + SIM_PRESENCE_UI_ABSENT_DWELL_MS - 1u),
          "absence remains hidden until the full dwell");
    check(sim_presence_ui_update(
              &state, true, true, false,
              11000u + SIM_PRESENCE_UI_ABSENT_DWELL_MS),
          "stable ready-state absence is promoted");
}

static void test_presence_cancels_transient_absence(void) {
    sim_presence_ui_state_t state;
    sim_presence_ui_init(&state);

    check(!sim_presence_ui_update(&state, true, true, false, 100u),
          "candidate starts hidden");
    check(!sim_presence_ui_update(&state, true, true, true, 200u),
          "presence cancels the candidate immediately");
    check(!state.absent_candidate && !state.missing,
          "cancelled candidate leaves no missing latch");
}

static void test_confirmed_absence_survives_unknown_then_clears(void) {
    sim_presence_ui_state_t state;
    sim_presence_ui_init(&state);
    (void)sim_presence_ui_update(&state, true, true, false, 0u);
    check(sim_presence_ui_update(
              &state, true, true, false,
              SIM_PRESENCE_UI_ABSENT_DWELL_MS),
          "fixture confirms absence");
    check(sim_presence_ui_update(&state, false, false, false, 5000u),
          "a modem restart does not expose ordinary UI for a known-missing SIM");
    check(!sim_presence_ui_update(&state, true, true, true, 6000u),
          "a positive observation clears confirmed absence immediately");
}

static void test_dwell_handles_millisecond_wrap(void) {
    sim_presence_ui_state_t state;
    sim_presence_ui_init(&state);
    uint32_t start = UINT32_MAX - 1000u;
    check(!sim_presence_ui_update(&state, true, true, false, start),
          "wrap fixture starts candidate");
    check(sim_presence_ui_update(
              &state, true, true, false,
              start + SIM_PRESENCE_UI_ABSENT_DWELL_MS),
          "unsigned elapsed time survives wrap");
}

int main(void) {
    test_startup_absence_requires_ready_dwell();
    test_presence_cancels_transient_absence();
    test_confirmed_absence_survives_unknown_then_clears();
    test_dwell_handles_millisecond_wrap();
    if (s_failures != 0) {
        fprintf(stderr, "%d SIM-presence logic test(s) failed\n", s_failures);
        return 1;
    }
    puts("SIM-presence UI logic tests passed");
    return 0;
}

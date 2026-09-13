#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "apps/clock_alarm_logic.h"

/* Oracle: the v6.00 alarm-vs-foreground arbitration (ROM dig 2026-07-10, memory
 * v600-alarm-vs-call). The alarm is a priority overlay: it only fires at the
 * idle surface; a call foreground SUPPRESSES it (never hijack, never per-tick
 * fight); it stays pending under the call and re-fires at idle; a call that
 * outlasts the 61.68 s window snoozes it underneath. Expected actions below are
 * derived from that rule, not from the implementation. */

static int s_failures;

static void expect(clock_alarm_action_t got, clock_alarm_action_t want, const char *msg) {
    if (got != want) {
        fprintf(stderr, "FAIL: %s (got %d want %d)\n", msg, (int)got, (int)want);
        s_failures++;
    }
}

/* Base input: idle standby, alarm not due, nothing ringing. */
static clock_alarm_inputs_t base(void) {
    clock_alarm_inputs_t in;
    memset(&in, 0, sizeof(in));
    in.alarm_mode = CLOCK_ALARM_MODE_IDLE;
    return in;
}

static void test_not_due_is_noop(void) {
    clock_alarm_inputs_t in = base();
    in.alarm_due = false;
    expect(clock_alarm_decide(&in), CLOCK_ALARM_ACTION_NONE, "not due -> none (idle)");
    in.route_call_surface = true;
    expect(clock_alarm_decide(&in), CLOCK_ALARM_ACTION_NONE, "not due -> none (in call)");
}

static void test_idle_fires(void) {
    clock_alarm_inputs_t in = base();
    in.alarm_due = true;
    expect(clock_alarm_decide(&in), CLOCK_ALARM_ACTION_FIRE, "due at idle -> fire");
}

static void test_owns_screen_noop(void) {
    /* Alarm already ringing on its own screen: tick_clock_alarm drives it. */
    clock_alarm_inputs_t in = base();
    in.alarm_due = true;
    in.route_owns_alarm = true;
    in.alarm_mode = CLOCK_ALARM_MODE_RINGING;
    expect(clock_alarm_decide(&in), CLOCK_ALARM_ACTION_NONE, "alarm owns screen -> none");
}

static void test_power_off_wakes(void) {
    clock_alarm_inputs_t in = base();
    in.alarm_due = true;
    in.route_power_off = true;
    expect(clock_alarm_decide(&in), CLOCK_ALARM_ACTION_POWERON, "alarm while off -> poweron");
}

static void test_due_during_call_stays_pending(void) {
    /* Critical (a): alarm becomes due while a call is connected. It must NOT
     * hijack the route -- stay pending (mode idle, nothing to yield). */
    clock_alarm_inputs_t in = base();
    in.alarm_due = true;
    in.route_call_surface = true;
    in.alarm_mode = CLOCK_ALARM_MODE_IDLE;
    expect(clock_alarm_decide(&in), CLOCK_ALARM_ACTION_NONE, "due during call, not ringing -> pending");
}

static void test_ringing_preempted_by_call_yields(void) {
    /* Critical (b): the alarm was ringing when an incoming call took the
     * display. Yield the alarm's display state (one-time), do not fight. */
    clock_alarm_inputs_t in = base();
    in.alarm_due = true; /* still latched until stop/snooze */
    in.route_call_surface = true;
    in.alarm_mode = CLOCK_ALARM_MODE_RINGING;
    expect(clock_alarm_decide(&in), CLOCK_ALARM_ACTION_YIELD_TO_CALL, "ringing + call -> yield");
}

static void test_suppressed_under_short_call_stays(void) {
    /* Suppressed (already yielded), call still up, timer NOT elapsed: keep
     * waiting; it re-fires when the call ends (handled at idle). */
    clock_alarm_inputs_t in = base();
    in.alarm_due = true;
    in.route_call_surface = true;
    in.alarm_mode = CLOCK_ALARM_MODE_SUPPRESSED_BY_CALL;
    in.suppress_timer_elapsed = false;
    expect(clock_alarm_decide(&in), CLOCK_ALARM_ACTION_NONE, "suppressed, short call -> none");
}

static void test_suppressed_under_long_call_autosnoozes(void) {
    /* Call outlasted the 61.68 s window while the alarm was suppressed: snooze
     * underneath so "Snooze active" greets the user at hang-up. */
    clock_alarm_inputs_t in = base();
    in.alarm_due = true;
    in.route_call_surface = true;
    in.alarm_mode = CLOCK_ALARM_MODE_SUPPRESSED_BY_CALL;
    in.suppress_timer_elapsed = true;
    expect(clock_alarm_decide(&in), CLOCK_ALARM_ACTION_AUTOSNOOZE_UNDER_CALL, "suppressed, long call -> autosnooze");
}

static void test_call_ends_refires(void) {
    /* Call ended -> route back to idle, alarm still latched (was suppressed).
     * Idle path fires it fresh. */
    clock_alarm_inputs_t in = base();
    in.alarm_due = true;
    in.route_call_surface = false;
    in.alarm_mode = CLOCK_ALARM_MODE_SUPPRESSED_BY_CALL; /* stale from the call */
    expect(clock_alarm_decide(&in), CLOCK_ALARM_ACTION_FIRE, "call ended, alarm latched -> fire");
}

static void test_owns_screen_beats_call_flag(void) {
    /* Defensive: if both owns-alarm and call-surface were somehow set, the
     * owns-alarm early return wins (the alarm screen is up). */
    clock_alarm_inputs_t in = base();
    in.alarm_due = true;
    in.route_owns_alarm = true;
    in.route_call_surface = true;
    expect(clock_alarm_decide(&in), CLOCK_ALARM_ACTION_NONE, "owns-screen precedence");
}

int main(void) {
    test_not_due_is_noop();
    test_idle_fires();
    test_owns_screen_noop();
    test_power_off_wakes();
    test_due_during_call_stays_pending();
    test_ringing_preempted_by_call_yields();
    test_suppressed_under_short_call_stays();
    test_suppressed_under_long_call_autosnoozes();
    test_call_ends_refires();
    test_owns_screen_beats_call_flag();

    if (s_failures == 0) {
        printf("test_clock_alarm_logic: all passed\n");
        return 0;
    }
    fprintf(stderr, "test_clock_alarm_logic: %d failure(s)\n", s_failures);
    return 1;
}

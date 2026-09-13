#include "apps/clock_alarm_logic.h"

clock_alarm_action_t clock_alarm_decide(const clock_alarm_inputs_t *in) {
    /* The alarm already owns the screen (or the phone is powering up): its own
     * tick handler drives it; nothing to arbitrate here. Mirrors the original
     * poll_clock_alarm early-return. */
    if (in->route_owns_alarm) {
        return CLOCK_ALARM_ACTION_NONE;
    }
    if (!in->alarm_due) {
        return CLOCK_ALARM_ACTION_NONE;
    }
    /* Alarm-while-off: wake the phone (the ring follows once it is up). */
    if (in->route_power_off) {
        return CLOCK_ALARM_ACTION_POWERON;
    }
    /* A call foreground SUPPRESSES the alarm (v6.00: nav-state != idle). The
     * alarm must never hijack the call route -- that stranded the live call and
     * caused the per-tick ping-pong. */
    if (in->route_call_surface) {
        if (in->alarm_mode == CLOCK_ALARM_MODE_RINGING) {
            /* The alarm was mid-ring when the call preempted the display: clean
             * up its display force and mark it suppressed-but-timing. */
            return CLOCK_ALARM_ACTION_YIELD_TO_CALL;
        }
        if (in->alarm_mode == CLOCK_ALARM_MODE_SUPPRESSED_BY_CALL &&
            in->suppress_timer_elapsed) {
            /* The call outlasted the 61.68 s window: auto-snooze underneath so
             * "Snooze active" is what greets the user when the call ends. */
            return CLOCK_ALARM_ACTION_AUTOSNOOZE_UNDER_CALL;
        }
        /* Otherwise stay pending; the alarm re-fires when the phone is idle. */
        return CLOCK_ALARM_ACTION_NONE;
    }
    /* Idle (or a non-call foreground): fire normally. */
    return CLOCK_ALARM_ACTION_FIRE;
}

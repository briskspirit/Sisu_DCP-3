#ifndef CLOCK_ALARM_LOGIC_H
#define CLOCK_ALARM_LOGIC_H

#include <stdbool.h>
#include <stdint.h>

/* Pure alarm-vs-foreground arbitration, split out so the truth table is
 * host-testable without stubbing the whole app. Reproduces the v6.00 behavior
 * (ROM dig 2026-07-10, memory v600-alarm-vs-call): the alarm is a priority
 * overlay that only rings/shows at the idle surface; a call foreground
 * SUPPRESSES it (never a hijack, never a per-tick fight). The alarm stays
 * pending under the call and re-fires when the phone returns to idle; a call
 * that outlasts the 61.68 s auto-snooze window snoozes the alarm underneath. */

typedef enum {
    CLOCK_ALARM_ACTION_NONE = 0,          /* do nothing this tick */
    CLOCK_ALARM_ACTION_FIRE,              /* open the alarm runtime (ring + screen) */
    CLOCK_ALARM_ACTION_POWERON,           /* alarm-while-off: wake the phone */
    CLOCK_ALARM_ACTION_YIELD_TO_CALL,     /* was ringing; a call preempted -> drop the
                                           * alarm's display force, mark suppressed */
    CLOCK_ALARM_ACTION_AUTOSNOOZE_UNDER_CALL, /* suppressed timer elapsed -> snooze
                                               * underneath (route-preserving) */
} clock_alarm_action_t;

/* clock_alarm_mode values shared with clock/alarm.c (kept in sync there). */
#define CLOCK_ALARM_MODE_IDLE 0u
#define CLOCK_ALARM_MODE_RINGING 1u
#define CLOCK_ALARM_MODE_ACTIVATE_PROMPT 2u
#define CLOCK_ALARM_MODE_SNOOZE_ACTIVE 3u
#define CLOCK_ALARM_MODE_SUPPRESSED_BY_CALL 4u

typedef struct {
    bool alarm_due;              /* rtc_alarm_hal_alarm_due() (incl. snooze re-fire) */
    bool route_owns_alarm;       /* route == CLOCK_ALARM or POWERUP */
    bool route_power_off;        /* route == POWER_OFF */
    bool route_call_surface;     /* an incoming/active/held-call surface owns the display */
    uint8_t alarm_mode;          /* app->clock_alarm_mode */
    bool suppress_timer_elapsed; /* 61.68 s since the alarm first rang (mode 4 only) */
} clock_alarm_inputs_t;

clock_alarm_action_t clock_alarm_decide(const clock_alarm_inputs_t *in);

#endif /* CLOCK_ALARM_LOGIC_H */

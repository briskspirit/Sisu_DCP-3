#include "services/backlight_service.h"

#include "hal/board.h"
#include "services/timebase.h"

/* The original 3210's LIGHTS_TIMER counted 0x787 (1927) ticks; at the 8 ms tick
 * that is 1927 * 8 = 15416 ms ~= 15.4 s (~16 s, matching the handset). We do NOT
 * count loop iterations: this is converted once to a millisecond DURATION and
 * used as a wall-clock deadline (now_ms + BACKLIGHT_TIMEOUT_MS, compared via
 * time_diff_ms), so the timeout holds regardless of the actual main-loop rate. */
#define BACKLIGHT_TIMEOUT_TICKS 0x0787u
#define BACKLIGHT_TIMEOUT_MS (BACKLIGHT_TIMEOUT_TICKS * SYSTEM_TICK_MS)

static uint32_t s_deadline_ms;
/* Explicit "a timeout is pending" flag. We do NOT overload s_deadline_ms==0 as
 * the sentinel: a legitimate deadline (now_ms + BACKLIGHT_TIMEOUT_MS) can wrap to
 * exactly 0, which would make the tick skip the timeout and leave the backlight
 * stuck on. (Bug found by test_small_utils.) */
static bool s_deadline_active;
static bool s_on;
static bool s_forced;
static bool s_forced_on;
static bool s_always_on;
static uint8_t s_level_percent;

static void apply_level(bool on);
static void arm_timeout(uint32_t now_ms);

void backlight_service_init(uint32_t now_ms) {
    (void)now_ms;
    s_forced = false;
    s_forced_on = false;
    s_always_on = false;
    s_deadline_active = false;
    s_on = false;
    s_level_percent = BACKLIGHT_LEVEL_DEFAULT_PERCENT;
    board_set_backlight_duty_percent(s_level_percent);
    board_set_backlight(false);
}

void backlight_service_set_always_on(bool always_on, uint32_t now_ms) {
    if (s_always_on == always_on) {
        return;
    }
    s_always_on = always_on;
    if (s_forced) {
        return;
    }
    if (s_always_on) {
        s_deadline_active = false;
        apply_level(true);
    } else if (s_on) {
        arm_timeout(now_ms);
    }
}

void backlight_service_notify_activity(uint32_t now_ms) {
    s_forced = false;
    if (s_always_on) {
        s_deadline_active = false;
    } else {
        arm_timeout(now_ms);
    }
    apply_level(true);
}

void backlight_service_force_level(bool on) {
    s_forced = true;
    s_forced_on = on;
    s_deadline_active = false;
    apply_level(on);
}

void backlight_service_release_force(uint32_t now_ms) {
    if (!s_forced) {
        return;
    }
    s_forced = false;
    s_forced_on = false;
    if (s_always_on) {
        s_deadline_active = false;
    } else {
        arm_timeout(now_ms);
    }
    apply_level(true);
}

void backlight_service_tick(uint32_t now_ms) {
    if (s_forced) {
        apply_level(s_forced_on);
        return;
    }
    if (s_always_on) {
        s_deadline_active = false;
        apply_level(true);
        return;
    }
    if (s_deadline_active && time_diff_ms(now_ms, s_deadline_ms) >= 0) {
        s_deadline_active = false;
        apply_level(false);
    }
}

bool backlight_service_is_on(void) {
    return s_on;
}

bool backlight_service_set_level_percent(uint8_t level_percent) {
    if (level_percent < BACKLIGHT_LEVEL_MIN_PERCENT ||
        level_percent > BACKLIGHT_LEVEL_MAX_PERCENT) {
        return false;
    }
    if (s_level_percent != level_percent) {
        s_level_percent = level_percent;
        board_set_backlight_duty_percent(level_percent);
    }
    return true;
}

uint8_t backlight_service_level_percent(void) {
    return s_level_percent;
}

static void arm_timeout(uint32_t now_ms) {
    s_deadline_ms = now_ms + BACKLIGHT_TIMEOUT_MS;
    s_deadline_active = true;
}

static void apply_level(bool on) {
    if (s_on == on) {
        return;
    }
    s_on = on;
    board_set_backlight(on);
}

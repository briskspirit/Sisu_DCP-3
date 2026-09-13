/* Host unit tests for the small pure modules:
 *   src/services/feature_gates.c
 *   src/services/key_utils.c
 *   src/services/event_queue.c
 *   src/services/backlight_service.c
 *
 * backlight_service.c depends on board_set_backlight() (a HAL pin) and
 * time_diff_ms() (timebase.c, which pulls in pico/time.h). Rather than link the
 * pico-dependent timebase.c/board.c, we provide minimal host stubs here and then
 * directly #include the .c files under test so their behavior (and any statics)
 * are exercised as-is. We do NOT copy the functions -- the real source is
 * compiled.
 *
 * Oracles are derived from first principles:
 *   - event_queue: a textbook ring buffer of capacity 32; push fails when full,
 *     pop fails when empty, FIFO order, head/tail wrap mod capacity.
 *   - backlight: BACKLIGHT_TIMEOUT_MS = 0x787 * SYSTEM_TICK_MS = 1927*8 = 15416ms.
 *     time_diff_ms(now,deadline) = (int32_t)(now-deadline); timeout fires when
 *     now-deadline >= 0, i.e. now >= deadline (fires exactly AT the deadline).
 *   - feature_gates: lazy-init to all-visible; out-of-range gate -> visible/true.
 *   - key_utils: maps the 12 dialpad key bitmasks to ASCII, else NUL.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int s_failures;

static void assert_true(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        s_failures++;
    }
}

/* ---- Stubs the backlight service links against -------------------------- */
/* board_set_backlight records the last requested pin level and a call count so
 * tests can verify the HAL is driven only on transitions. */
static bool s_pin_level;
static int s_pin_calls;
static uint8_t s_duty_percent;
static int s_duty_calls;
void board_set_backlight(bool enabled) {
    s_pin_level = enabled;
    s_pin_calls++;
}
void board_set_backlight_duty_percent(uint8_t duty_percent) {
    s_duty_percent = duty_percent;
    s_duty_calls++;
}
/* Mirror the real timebase.c implementation exactly (modular wrap-around diff). */
int32_t time_diff_ms(uint32_t a, uint32_t b) {
    return (int32_t)(a - b);
}

/* ---- The code under test ----------------------------------------------- */
#include "../src/services/feature_gates.c"
#include "../src/services/key_utils.c"
#include "../src/services/event_queue.c"
#include "../src/services/backlight_service.c"

/* Re-derive the timeout constant from the same source the .c uses so the test
 * oracle and the implementation cannot silently diverge. */
#define EXPECT_TIMEOUT_MS (0x0787u * SYSTEM_TICK_MS) /* 15416 */

/* ========================================================================
 * feature_gates
 * ======================================================================== */
static void test_feature_gates_defaults(void) {
    feature_gates_reset_defaults();
    for (uint8_t i = 0; i < FEATURE_GATE_COUNT; i++) {
        assert_true(feature_gate_visible((feature_gate_id_t)i), "gate visible by default");
    }
}

static void test_feature_gates_set_get(void) {
    feature_gates_reset_defaults();
    feature_gate_set_visible(FEATURE_GATE_PROFILES, false);
    assert_true(!feature_gate_visible(FEATURE_GATE_PROFILES), "set false -> not visible");
    /* neighbors unaffected */
    assert_true(feature_gate_visible(FEATURE_GATE_CALL_DIVERT), "neighbor unaffected lo");
    assert_true(feature_gate_visible(FEATURE_GATE_PHONEBOOK_SERVICE_NOS), "neighbor unaffected hi");
    feature_gate_set_visible(FEATURE_GATE_PROFILES, true);
    assert_true(feature_gate_visible(FEATURE_GATE_PROFILES), "set true -> visible again");
}

static void test_feature_gates_last_gate(void) {
    feature_gates_reset_defaults();
    feature_gate_set_visible(FEATURE_GATE_GAMES_EXTRA_ROWS, false);
    assert_true(!feature_gate_visible(FEATURE_GATE_GAMES_EXTRA_ROWS), "last valid gate toggles");
}

static void test_feature_gates_out_of_range(void) {
    feature_gates_reset_defaults();
    /* Out-of-range query returns true (visible) and must not read OOB. */
    assert_true(feature_gate_visible(FEATURE_GATE_COUNT), "==COUNT -> visible/true");
    assert_true(feature_gate_visible((feature_gate_id_t)0xFFFF), "huge gate -> visible/true");
    /* Out-of-range set is a silent no-op (no OOB write); verify in-range gates
     * remained at default. */
    feature_gate_set_visible(FEATURE_GATE_COUNT, false);
    feature_gate_set_visible((feature_gate_id_t)1000, false);
    for (uint8_t i = 0; i < FEATURE_GATE_COUNT; i++) {
        assert_true(feature_gate_visible((feature_gate_id_t)i), "in-range gate survived OOB set");
    }
}

static void test_feature_gates_names(void) {
    /* Every valid gate yields a non-empty, distinct-ish name; OOB -> "unknown". */
    for (uint8_t i = 0; i < FEATURE_GATE_COUNT; i++) {
        const char *n = feature_gate_name((feature_gate_id_t)i);
        assert_true(n != NULL && n[0] != '\0', "gate name non-empty");
        assert_true(strcmp(n, "unknown") != 0, "valid gate name is not 'unknown'");
    }
    assert_true(strcmp(feature_gate_name(FEATURE_GATE_COUNT), "unknown") == 0,
                "==COUNT name is 'unknown'");
    assert_true(strcmp(feature_gate_name((feature_gate_id_t)0xFFFF), "unknown") == 0,
                "huge gate name is 'unknown'");
    /* The NAMES[] table must have exactly FEATURE_GATE_COUNT entries: if it were
     * short the loop above would read OOB (ASan would trip). Spot-check ends. */
    assert_true(strcmp(feature_gate_name(FEATURE_GATE_NET_MONITOR), "net_monitor") == 0,
                "first name");
    assert_true(strcmp(feature_gate_name(FEATURE_GATE_GAMES_EXTRA_ROWS), "games_extra_rows") == 0,
                "last name");
}

/* ========================================================================
 * key_utils
 * ======================================================================== */
static void test_key_digit_mapping(void) {
    assert_true(key_digit(KEY_0) == '0', "KEY_0 -> '0'");
    assert_true(key_digit(KEY_1) == '1', "KEY_1 -> '1'");
    assert_true(key_digit(KEY_2) == '2', "KEY_2 -> '2'");
    assert_true(key_digit(KEY_3) == '3', "KEY_3 -> '3'");
    assert_true(key_digit(KEY_4) == '4', "KEY_4 -> '4'");
    assert_true(key_digit(KEY_5) == '5', "KEY_5 -> '5'");
    assert_true(key_digit(KEY_6) == '6', "KEY_6 -> '6'");
    assert_true(key_digit(KEY_7) == '7', "KEY_7 -> '7'");
    assert_true(key_digit(KEY_8) == '8', "KEY_8 -> '8'");
    assert_true(key_digit(KEY_9) == '9', "KEY_9 -> '9'");
    assert_true(key_digit(KEY_STAR) == '*', "KEY_STAR -> '*'");
    assert_true(key_digit(KEY_HASH) == '#', "KEY_HASH -> '#'");
}

static void test_key_digit_non_dialpad(void) {
    /* Navigation / soft / power keys are not dial digits -> NUL. */
    assert_true(key_digit(KEY_NAVI) == 0, "KEY_NAVI -> NUL");
    assert_true(key_digit(KEY_UP) == 0, "KEY_UP -> NUL");
    assert_true(key_digit(KEY_DOWN) == 0, "KEY_DOWN -> NUL");
    assert_true(key_digit(KEY_C) == 0, "KEY_C -> NUL");
    assert_true(key_digit(KEY_POWER) == 0, "KEY_POWER -> NUL");
    assert_true(key_digit(0u) == 0, "no key -> NUL");
    assert_true(key_digit(0xFFFFu) == 0, "all-bits (multi-key) -> NUL");
    /* A combined bitmask (two keys at once) is not a single case label -> NUL. */
    assert_true(key_digit((uint16_t)(KEY_1 | KEY_2)) == 0, "two keys -> NUL");
}

/* ========================================================================
 * event_queue (ring buffer)
 * ======================================================================== */
static void test_event_queue_empty_pop(void) {
    event_queue_t q;
    event_queue_init(&q);
    assert_true(event_queue_empty(&q), "fresh queue empty");
    input_event_t ev;
    memset(&ev, 0xAA, sizeof(ev));
    assert_true(!event_queue_pop(&q, &ev), "pop on empty returns false");
    assert_true(event_queue_empty(&q), "still empty after failed pop");
}

static void test_event_queue_single(void) {
    event_queue_t q;
    event_queue_init(&q);
    assert_true(event_queue_push(&q, EVENT_KEY_DOWN, 0x1234u, 0x5678u, 0xDEADBEEFu),
                "push one ok");
    assert_true(!event_queue_empty(&q), "non-empty after push");
    input_event_t ev;
    memset(&ev, 0, sizeof(ev));
    assert_true(event_queue_pop(&q, &ev), "pop one ok");
    assert_true(ev.type == EVENT_KEY_DOWN && ev.code == 0x1234u &&
                ev.arg == 0x5678u && ev.when_ms == 0xDEADBEEFu,
                "popped event matches pushed");
    assert_true(event_queue_empty(&q), "empty again after pop");
}

static void test_event_queue_fifo_order(void) {
    event_queue_t q;
    event_queue_init(&q);
    for (uint16_t i = 0; i < 10u; i++) {
        assert_true(event_queue_push(&q, EVENT_SERVICE, i, (uint16_t)(i * 3u), i + 100u),
                    "push fifo");
    }
    for (uint16_t i = 0; i < 10u; i++) {
        input_event_t ev;
        assert_true(event_queue_pop(&q, &ev), "pop fifo");
        assert_true(ev.code == i && ev.arg == (uint16_t)(i * 3u) && ev.when_ms == i + 100u,
                    "fifo order preserved");
    }
    assert_true(event_queue_empty(&q), "empty after draining fifo");
}

static void test_event_queue_overflow(void) {
    event_queue_t q;
    event_queue_init(&q);
    /* Fill to exactly capacity. */
    for (uint16_t i = 0; i < EVENT_QUEUE_CAPACITY; i++) {
        assert_true(event_queue_push(&q, EVENT_KEY_DOWN, i, 0u, 0u), "push up to capacity");
    }
    assert_true(q.count == EVENT_QUEUE_CAPACITY, "count == capacity when full");
    /* One more must be rejected (overflow guard), not overwrite. */
    assert_true(!event_queue_push(&q, EVENT_KEY_DOWN, 0xFFFFu, 0u, 0u),
                "push when full rejected");
    assert_true(q.count == EVENT_QUEUE_CAPACITY, "count unchanged after rejected push");
    /* The oldest item must still be code 0 (not clobbered by the rejected push). */
    input_event_t ev;
    assert_true(event_queue_pop(&q, &ev), "pop after full");
    assert_true(ev.code == 0u, "oldest preserved despite overflow attempt");
    /* Now there is room for exactly one more. */
    assert_true(event_queue_push(&q, EVENT_KEY_DOWN, 0xBEEFu, 0u, 0u), "push after one pop");
    assert_true(!event_queue_push(&q, EVENT_KEY_DOWN, 0u, 0u, 0u), "full again, rejected");
}

static void test_event_queue_wrap(void) {
    /* Drive head/tail far past the capacity to exercise the modulo wrap. We do
     * many push/pop cycles keeping the queue near-full so tail wraps repeatedly.
     * Oracle: FIFO content is independent of the absolute head/tail position. */
    event_queue_t q;
    event_queue_init(&q);
    uint32_t next_push = 0u;
    uint32_t next_expect = 0u;
    /* Prime with half capacity. */
    for (uint16_t i = 0; i < EVENT_QUEUE_CAPACITY / 2u; i++) {
        assert_true(event_queue_push(&q, EVENT_SERVICE, (uint16_t)next_push, 0u, next_push),
                    "wrap prime push");
        next_push++;
    }
    /* Now 1000 push+pop cycles; tail and head each wrap ~31 times. */
    for (int cycle = 0; cycle < 1000; cycle++) {
        assert_true(event_queue_push(&q, EVENT_SERVICE, (uint16_t)next_push, 0u, next_push),
                    "wrap cycle push");
        next_push++;
        input_event_t ev;
        assert_true(event_queue_pop(&q, &ev), "wrap cycle pop");
        assert_true(ev.when_ms == next_expect, "wrap FIFO value correct");
        next_expect++;
    }
    /* Drain the remaining half. */
    while (!event_queue_empty(&q)) {
        input_event_t ev;
        assert_true(event_queue_pop(&q, &ev), "wrap drain pop");
        assert_true(ev.when_ms == next_expect, "wrap drain value correct");
        next_expect++;
    }
    assert_true(next_expect == next_push, "wrap accounted for every item");
    /* head/tail must be valid indices < capacity. */
    assert_true(q.head < EVENT_QUEUE_CAPACITY && q.tail < EVENT_QUEUE_CAPACITY,
                "indices stay in range after heavy wrap");
}

static void test_event_queue_refill_after_drain(void) {
    /* Fill, fully drain, refill -- the second fill must again reach capacity and
     * reject the (capacity+1)th, proving count underflow did not occur on the
     * empty pops in between. */
    event_queue_t q;
    event_queue_init(&q);
    for (int round = 0; round < 3; round++) {
        for (uint16_t i = 0; i < EVENT_QUEUE_CAPACITY; i++) {
            assert_true(event_queue_push(&q, EVENT_KEY_UP, i, 0u, 0u), "refill push");
        }
        assert_true(!event_queue_push(&q, EVENT_KEY_UP, 0u, 0u, 0u), "refill full rejected");
        /* Extra failed pops in between drains must not underflow count. */
        for (uint16_t i = 0; i < EVENT_QUEUE_CAPACITY; i++) {
            input_event_t ev;
            assert_true(event_queue_pop(&q, &ev), "refill pop");
            assert_true(ev.code == i, "refill order");
        }
        input_event_t ev;
        assert_true(!event_queue_pop(&q, &ev), "extra pop on empty rejected");
        assert_true(!event_queue_pop(&q, &ev), "second extra pop on empty rejected");
        assert_true(q.count == 0u, "count is 0, not underflowed");
    }
}

/* ========================================================================
 * backlight_service
 * ======================================================================== */
static void test_backlight_init_off(void) {
    s_pin_calls = 0;
    backlight_service_init(1000u);
    assert_true(!backlight_service_is_on(), "init -> off");
    assert_true(!s_pin_level, "init drives pin off");
}

static void test_backlight_activity_turns_on(void) {
    backlight_service_init(0u);
    backlight_service_notify_activity(1000u);
    assert_true(backlight_service_is_on(), "activity -> on");
    assert_true(s_pin_level, "activity drives pin on");
}

static void test_backlight_level_is_independent_of_on_off_policy(void) {
    s_duty_percent = 0u;
    s_duty_calls = 0;
    backlight_service_init(0u);
    assert_true(s_duty_percent == BACKLIGHT_LEVEL_DEFAULT_PERCENT,
                "init applies the 100% compatibility default");
    assert_true(backlight_service_level_percent() ==
                    BACKLIGHT_LEVEL_DEFAULT_PERCENT,
                "service reports the initialized level");

    assert_true(backlight_service_set_level_percent(50u),
                "valid level accepted while off");
    assert_true(s_duty_percent == 50u && !backlight_service_is_on(),
                "preview updates PWM without turning the light on");
    backlight_service_notify_activity(10u);
    assert_true(backlight_service_set_level_percent(25u),
                "valid level accepted while on");
    assert_true(s_duty_percent == 25u && backlight_service_is_on(),
                "live PWM update preserves logical on state");

    int before = s_duty_calls;
    assert_true(!backlight_service_set_level_percent(0u) &&
                    !backlight_service_set_level_percent(101u),
                "zero and over-range levels are rejected");
    assert_true(s_duty_calls == before && s_duty_percent == 25u,
                "invalid levels never reach the HAL");
    assert_true(backlight_service_set_level_percent(25u) &&
                    s_duty_calls == before,
                "an unchanged level does not rewrite PWM");
}

static void test_backlight_timeout_at_deadline(void) {
    /* Activity at t=1000 sets deadline = 1000 + 15416 = 16416.
     * Just BEFORE the deadline (16415) it must remain on; exactly AT the
     * deadline (16416) it must switch off, because time_diff_ms(now,deadline)>=0
     * is true when now==deadline. */
    backlight_service_init(0u);
    backlight_service_notify_activity(1000u);
    uint32_t deadline = 1000u + EXPECT_TIMEOUT_MS;

    backlight_service_tick(deadline - 1u);
    assert_true(backlight_service_is_on(), "still on one ms before deadline");

    backlight_service_tick(deadline);
    assert_true(!backlight_service_is_on(), "off exactly at deadline");
}

static void test_backlight_timeout_after_deadline(void) {
    backlight_service_init(0u);
    backlight_service_notify_activity(2000u);
    uint32_t deadline = 2000u + EXPECT_TIMEOUT_MS;
    /* A tick well past the deadline also turns it off. */
    backlight_service_tick(deadline + 5000u);
    assert_true(!backlight_service_is_on(), "off after deadline");
    /* Subsequent ticks keep it off and clear the deadline (idempotent). */
    backlight_service_tick(deadline + 6000u);
    assert_true(!backlight_service_is_on(), "stays off");
}

static void test_backlight_tick_before_any_activity(void) {
    /* deadline==0 after init means "no pending timeout": tick must not turn off
     * anything spuriously and must not fire the deadline branch (0 is sentinel). */
    backlight_service_init(0u);
    backlight_service_tick(100000u);
    assert_true(!backlight_service_is_on(), "no spurious state when deadline==0");
}

static void test_backlight_deadline_zero_now(void) {
    /* Edge: activity at a now_ms where now + TIMEOUT wraps to exactly 0. The old
     * code overloaded deadline==0 as the "no timeout" sentinel, so this case stayed
     * stuck on forever. FIXED: an explicit s_deadline_active flag means the timeout
     * still fires even when the computed deadline is 0. */
    backlight_service_init(0u);
    uint32_t now = (uint32_t)(0u - EXPECT_TIMEOUT_MS); /* now + TIMEOUT == 0 */
    backlight_service_notify_activity(now);
    assert_true(backlight_service_is_on(), "on after activity (wrap-to-0 deadline case)");
    /* A tick 1 ms before the (wrapped) deadline keeps it on... */
    backlight_service_tick((uint32_t)(now + EXPECT_TIMEOUT_MS - 1u)); /* deadline - 1 == 0xffffffff */
    assert_true(backlight_service_is_on(), "still on 1ms before the wrapped deadline");
    /* ...and a tick at the deadline (== 0) turns it off. */
    backlight_service_tick(0u); /* == deadline */
    assert_true(!backlight_service_is_on(), "wrap-to-0 deadline still times out (fixed)");
}

static void test_backlight_force_overrides(void) {
    backlight_service_init(0u);
    backlight_service_notify_activity(1000u);
    assert_true(backlight_service_is_on(), "on before force");
    /* Force OFF overrides the on state. */
    backlight_service_force_level(false);
    assert_true(!backlight_service_is_on(), "force off overrides on");
    /* Ticks far past any deadline cannot change a forced level. */
    backlight_service_tick(500000u);
    assert_true(!backlight_service_is_on(), "tick cannot override force-off");
    /* Force ON. */
    backlight_service_force_level(true);
    assert_true(backlight_service_is_on(), "force on");
    backlight_service_tick(1000000u);
    assert_true(backlight_service_is_on(), "tick cannot time out a force-on");
}

static void test_backlight_force_blocks_always_on(void) {
    /* While forced, set_always_on must be recorded but not change the pin level
     * (force wins). After release, the always-on takes effect. */
    backlight_service_init(0u);
    backlight_service_force_level(false);
    backlight_service_set_always_on(true, 1000u);
    assert_true(!backlight_service_is_on(), "force-off beats always-on while forced");
    /* Release force: now always_on should drive it on. */
    backlight_service_release_force(2000u);
    assert_true(backlight_service_is_on(), "release force -> always-on turns it on");
    backlight_service_tick(9999999u);
    assert_true(backlight_service_is_on(), "always-on never times out");
}

static void test_backlight_release_when_not_forced(void) {
    /* release_force is a no-op if not forced: must not change state. */
    backlight_service_init(0u);
    backlight_service_notify_activity(1000u);
    bool before = backlight_service_is_on();
    backlight_service_release_force(2000u);
    assert_true(backlight_service_is_on() == before, "release-when-not-forced is no-op");
    /* And the original deadline survives: it should still time out normally. */
    backlight_service_tick(1000u + EXPECT_TIMEOUT_MS);
    assert_true(!backlight_service_is_on(), "deadline preserved across no-op release");
}

static void test_backlight_release_restores_timeout(void) {
    /* Force on, then release at t=5000: a fresh deadline = 5000 + TIMEOUT should
     * be installed, and the light should time out from there. */
    backlight_service_init(0u);
    backlight_service_force_level(true);
    backlight_service_release_force(5000u);
    assert_true(backlight_service_is_on(), "on right after release");
    uint32_t deadline = 5000u + EXPECT_TIMEOUT_MS;
    backlight_service_tick(deadline - 1u);
    assert_true(backlight_service_is_on(), "on before post-release deadline");
    backlight_service_tick(deadline);
    assert_true(!backlight_service_is_on(), "off at post-release deadline");
}

static void test_backlight_always_on_disables_timeout(void) {
    backlight_service_init(0u);
    backlight_service_set_always_on(true, 1000u);
    assert_true(backlight_service_is_on(), "always-on turns on immediately");
    backlight_service_tick(10000000u);
    assert_true(backlight_service_is_on(), "always-on ignores timeout");
}

static void test_backlight_always_on_toggle_off_restarts_timeout(void) {
    /* Turn always-on ON (light on, no deadline), then OFF while on at t=3000:
     * a new deadline = 3000 + TIMEOUT is set and it times out from there. */
    backlight_service_init(0u);
    backlight_service_set_always_on(true, 0u);
    assert_true(backlight_service_is_on(), "always-on on");
    backlight_service_set_always_on(false, 3000u);
    assert_true(backlight_service_is_on(), "still on right after always-on cleared");
    uint32_t deadline = 3000u + EXPECT_TIMEOUT_MS;
    backlight_service_tick(deadline - 1u);
    assert_true(backlight_service_is_on(), "on before restarted deadline");
    backlight_service_tick(deadline);
    assert_true(!backlight_service_is_on(), "off at restarted deadline");
}

static void test_backlight_always_on_idempotent(void) {
    /* Setting always_on to its current value is an early-return no-op. Setting it
     * true twice must not reset anything, and setting false twice when already
     * false must not install a deadline. */
    backlight_service_init(0u);
    backlight_service_set_always_on(false, 0u); /* already false -> no-op */
    assert_true(!backlight_service_is_on(), "redundant always-on=false no-op");
    backlight_service_set_always_on(true, 0u);
    backlight_service_set_always_on(true, 5000u); /* redundant true -> no-op */
    assert_true(backlight_service_is_on(), "redundant always-on=true keeps it on");
    backlight_service_tick(20000000u);
    assert_true(backlight_service_is_on(), "still always-on after redundant set");
}

static void test_backlight_activity_clears_force(void) {
    /* notify_activity unconditionally clears a prior force (s_forced=false) and
     * arms a fresh timeout. Force OFF, then activity must turn it back on and let
     * a subsequent tick time it out normally (force no longer pinning it). */
    backlight_service_init(0u);
    backlight_service_force_level(false);
    assert_true(!backlight_service_is_on(), "forced off");
    backlight_service_notify_activity(4000u);
    assert_true(backlight_service_is_on(), "activity clears force-off and turns on");
    uint32_t deadline = 4000u + EXPECT_TIMEOUT_MS;
    backlight_service_tick(deadline - 1u);
    assert_true(backlight_service_is_on(), "on before deadline after force cleared");
    backlight_service_tick(deadline);
    assert_true(!backlight_service_is_on(), "times out normally after force cleared by activity");
}

static void test_backlight_force_on_over_always_on(void) {
    /* always-on active, then force ON: still on. Then force OFF: force wins over
     * always-on (off). Release force: always-on reasserts (on) and never times out. */
    backlight_service_init(0u);
    backlight_service_set_always_on(true, 0u);
    assert_true(backlight_service_is_on(), "always-on on");
    backlight_service_force_level(true);
    assert_true(backlight_service_is_on(), "force-on over always-on stays on");
    backlight_service_force_level(false);
    assert_true(!backlight_service_is_on(), "force-off beats always-on");
    backlight_service_tick(50000000u);
    assert_true(!backlight_service_is_on(), "force-off survives ticks even with always-on set");
    backlight_service_release_force(7000u);
    assert_true(backlight_service_is_on(), "release returns to always-on (on)");
    backlight_service_tick(60000000u);
    assert_true(backlight_service_is_on(), "always-on after release never times out");
}

static void test_backlight_repeated_force_toggle(void) {
    /* Sequential force changes: each force_level updates the held level immediately
     * and is idempotent under tick. */
    backlight_service_init(0u);
    backlight_service_force_level(true);
    assert_true(backlight_service_is_on(), "force on");
    backlight_service_force_level(false);
    assert_true(!backlight_service_is_on(), "force off");
    backlight_service_force_level(false);
    assert_true(!backlight_service_is_on(), "redundant force off");
    backlight_service_force_level(true);
    assert_true(backlight_service_is_on(), "force on again");
    backlight_service_tick(99999999u);
    assert_true(backlight_service_is_on(), "tick keeps last forced level");
}

static void test_event_queue_pop_output_untouched_on_empty(void) {
    /* Invariant: a failed pop (empty queue) must not write through the out pointer. */
    event_queue_t q;
    event_queue_init(&q);
    input_event_t ev;
    memset(&ev, 0x5C, sizeof(ev));
    input_event_t sentinel;
    memset(&sentinel, 0x5C, sizeof(sentinel));
    assert_true(!event_queue_pop(&q, &ev), "empty pop returns false");
    assert_true(memcmp(&ev, &sentinel, sizeof(ev)) == 0, "out param untouched on empty pop");
}

static void test_event_queue_wrap_indices_exact(void) {
    /* After exactly CAPACITY pushes then CAPACITY pops, head==tail==0 (wrapped a
     * full revolution), and after CAPACITY+1 push/pop pairs head==tail==1. */
    event_queue_t q;
    event_queue_init(&q);
    for (uint16_t i = 0; i < EVENT_QUEUE_CAPACITY; i++) {
        (void)event_queue_push(&q, EVENT_KEY_DOWN, i, 0u, 0u);
    }
    assert_true(q.tail == 0u, "tail wrapped to 0 after exactly CAPACITY pushes");
    assert_true(q.head == 0u, "head still 0 before any pop");
    for (uint16_t i = 0; i < EVENT_QUEUE_CAPACITY; i++) {
        input_event_t ev;
        (void)event_queue_pop(&q, &ev);
    }
    assert_true(q.head == 0u, "head wrapped to 0 after CAPACITY pops");
    assert_true(q.count == 0u, "empty after full revolution");
    /* One more push/pop advances both to index 1. */
    (void)event_queue_push(&q, EVENT_KEY_DOWN, 1u, 0u, 0u);
    assert_true(q.tail == 1u, "tail at 1 after one more push");
    input_event_t ev;
    (void)event_queue_pop(&q, &ev);
    assert_true(q.head == 1u, "head at 1 after one more pop");
}

static void test_backlight_pin_only_on_transition(void) {
    /* apply_level only calls board_set_backlight on a state change. Verify the
     * pin is not redundantly driven. */
    backlight_service_init(0u);   /* state off; init unconditionally calls pin off */
    backlight_service_notify_activity(1000u); /* off->on : 1 call */
    int after_on = s_pin_calls;
    backlight_service_notify_activity(1100u); /* already on : no transition */
    assert_true(s_pin_calls == after_on, "redundant activity does not re-drive pin");
    backlight_service_tick(1050u); /* before deadline, still on : no transition */
    assert_true(s_pin_calls == after_on, "tick before deadline does not re-drive pin");
}

int main(void) {
    test_feature_gates_defaults();
    test_feature_gates_set_get();
    test_feature_gates_last_gate();
    test_feature_gates_out_of_range();
    test_feature_gates_names();

    test_key_digit_mapping();
    test_key_digit_non_dialpad();

    test_event_queue_empty_pop();
    test_event_queue_single();
    test_event_queue_fifo_order();
    test_event_queue_overflow();
    test_event_queue_wrap();
    test_event_queue_refill_after_drain();
    test_event_queue_pop_output_untouched_on_empty();
    test_event_queue_wrap_indices_exact();

    test_backlight_init_off();
    test_backlight_activity_turns_on();
    test_backlight_level_is_independent_of_on_off_policy();
    test_backlight_timeout_at_deadline();
    test_backlight_timeout_after_deadline();
    test_backlight_tick_before_any_activity();
    test_backlight_deadline_zero_now();
    test_backlight_force_overrides();
    test_backlight_force_blocks_always_on();
    test_backlight_release_when_not_forced();
    test_backlight_release_restores_timeout();
    test_backlight_always_on_disables_timeout();
    test_backlight_always_on_toggle_off_restarts_timeout();
    test_backlight_always_on_idempotent();
    test_backlight_activity_clears_force();
    test_backlight_force_on_over_always_on();
    test_backlight_repeated_force_toggle();
    test_backlight_pin_only_on_transition();

    if (s_failures != 0) {
        fprintf(stderr, "%d failures\n", s_failures);
        return 1;
    }
    printf("small_utils tests passed\n");
    return 0;
}

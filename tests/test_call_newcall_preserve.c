/* Host tests for the in-call New-call preserve pair in src/apps/calls_app.c:
 * call_newcall_snapshot() captures the live call's identity (about to become
 * the HELD leg) plus any WAITING third call C BEFORE start_outgoing_call resets
 * the session; call_newcall_restore_held() re-applies it once the route settled
 * on APP_ROUTE_CALL. If C is dropped by the reset it never logs missed, and if
 * the held identity is dropped the retrieved leg shows as an unknown caller --
 * so every field must round-trip.
 *
 * calls_app.c is compiled whole with -ffunction-sections -fdata-sections
 * -Wl,-dead_strip (same recipe as test_call_divert_app), so only the leaf
 * helpers the two functions under test actually reach need host stubs:
 * copy_text (ui.c) and time_diff_ms (timebase). Everything else in the app is
 * stripped as unreachable from main. */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "apps/calls_app.h"
#include "services/core1_services.h"
#include "services/timebase.h"
#include "ui/ui.h"

static int s_failures;

static void check(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        s_failures++;
    }
}

static void check_str(const char *got, const char *want, const char *message) {
    if (strcmp(got, want) != 0) {
        fprintf(stderr, "FAIL: %s (got \"%s\", want \"%s\")\n", message, got, want);
        s_failures++;
    }
}

/* ------------------------------------------------------------------ stubs --
 * The only symbols reachable from the two functions under test. */

void copy_text(char *dst, size_t cap, const char *src) {
    if (dst == NULL || cap == 0u) {
        return;
    }
    if (src == NULL) {
        src = "";
    }
    snprintf(dst, cap, "%s", src);
}

int32_t time_diff_ms(uint32_t a, uint32_t b) {
    return (int32_t)(a - b);
}

/* ---------------------------------------------------------------- fixture */

#define NOW_MS 123456u

/* A live call A (connected 5 s ago, DIALLED list, record 77) with a waiting
 * third call C that has already been recorded (call log entry exists). */
static void populate_live_call(app_t *app, bool with_waiting, uint32_t connected_ms) {
    memset(app, 0, sizeof(*app));
    copy_text(app->call_number, sizeof(app->call_number), "+15551230001");
    copy_text(app->call_name, sizeof(app->call_name), "Alice");
    app->call_record_list = STORE_CALL_LIST_DIALLED;
    app->call_record_id = 77u;
    app->call_connected_ms = connected_ms;
    if (with_waiting) {
        copy_text(app->call_waiting_number, sizeof(app->call_waiting_number), "+15559990003");
        copy_text(app->call_waiting_name, sizeof(app->call_waiting_name), "Carol");
        app->call_waiting_pending = true;
        app->call_waiting_id = 3u;
        app->call_waiting_generation = 7u;
        app->call_waiting_episode = 42u;
        app->call_waiting_recorded = true;
        /* C is a private (CLIR) AND diverted caller: both presentation flags
         * are cleared by call_session_reset and must survive a New-call. */
        app->call_waiting_withheld = true;
        app->call_waiting_diverted = true;
    }
}

/* The PRODUCTION session reset, tone-stop linkage included: any field the
 * reset clears that the snapshot does not carry becomes a visible regression
 * here, and so does a reset that stops clearing or stops silencing. */
typedef struct {
    core1_cmd_t cmd;
    uint16_t arg;
} core1_post_t;
static core1_post_t s_core1_posts[8];
static unsigned s_core1_post_count;
void core1_post_command(core1_cmd_t cmd, uint16_t arg) {
    if (s_core1_post_count < 8u) {
        s_core1_posts[s_core1_post_count].cmd = cmd;
        s_core1_posts[s_core1_post_count].arg = arg;
    }
    s_core1_post_count++;
}

static void simulate_session_reset(app_t *app) {
    s_core1_post_count = 0u;
    call_session_reset(app);
    /* AUDIO_STOP owns marker-vibra cleanup inside the audio service, so reset
     * must be one atomic command rather than a racy stop/disarm pair. */
    check(s_core1_post_count == 1u,
          "the production reset posts exactly one core1 command");
    check(s_core1_posts[0].cmd == CORE1_CMD_AUDIO_STOP && s_core1_posts[0].arg == 0u,
          "reset posts AUDIO_STOP with arg 0");
}

/* ------------------------------------------------------------------ cases */

/* Full round-trip: live call + waiting C. Snapshot captures every field, the
 * reset wipes them, restore rebuilds the HELD leg AND re-arms the waiting C. */
static void test_snapshot_and_restore_with_waiting(void) {
    app_t app;
    populate_live_call(&app, true, NOW_MS - 5000u);

    call_newcall_preserve_t snap;
    memset(&snap, 0xa5, sizeof(snap));
    call_newcall_snapshot(&app, NOW_MS, &snap);

    check_str(snap.held_number, "+15551230001", "snapshot captures the live number");
    check_str(snap.held_name, "Alice", "snapshot captures the live name");
    check(snap.held_list == STORE_CALL_LIST_DIALLED, "snapshot captures the record list");
    check(snap.held_id == 77u, "snapshot captures the record id");
    check(snap.held_elapsed_seconds == 5u, "snapshot elapsed = (now - connected)/1000 = 5");
    check_str(snap.wait_number, "+15559990003", "snapshot captures the waiting number");
    check_str(snap.wait_name, "Carol", "snapshot captures the waiting name");
    check(snap.had_waiting, "snapshot records that a call was waiting");
    check(snap.waiting_id == 3u && snap.waiting_generation == 7u &&
              snap.waiting_episode == 42u,
          "snapshot captures the generation-qualified waiting identity");
    check(snap.waiting_recorded, "snapshot captures the waiting-recorded flag");
    check(snap.waiting_withheld, "snapshot captures the waiting-withheld flag");
    check(snap.waiting_diverted, "snapshot captures the waiting-diverted flag");

    /* The snapshot is read-only on the app. */
    check_str(app.call_number, "+15551230001", "snapshot leaves the live number alone");
    check(app.call_waiting_pending, "snapshot leaves the waiting flag alone");
    check(!app.call_held && !app.call_secondary_active,
          "snapshot does not mark the call held");

    simulate_session_reset(&app);
    check(app.call_number[0] == '\0' && !app.call_waiting_pending,
          "fixture: session reset cleared the live/waiting fields");

    call_newcall_restore_held(&app, &snap);
    check_str(app.call_held_number, "+15551230001", "restore sets the held number");
    check_str(app.call_held_name, "Alice", "restore sets the held name");
    check(app.call_held_record_list == STORE_CALL_LIST_DIALLED, "restore sets the held record list");
    check(app.call_held_record_id == 77u, "restore sets the held record id");
    check(app.call_held_elapsed_seconds == 5u, "restore sets the held elapsed seconds");
    check(app.call_secondary_active, "restore marks a secondary (held) leg active");
    check(app.call_held, "restore marks the call held");
    check_str(app.call_waiting_number, "+15559990003", "restore re-applies the waiting number");
    check_str(app.call_waiting_name, "Carol", "restore re-applies the waiting name");
    check(app.call_waiting_pending, "restore re-arms the waiting-pending flag");
    check(app.call_waiting_id == 3u && app.call_waiting_generation == 7u &&
              app.call_waiting_episode == 42u,
          "restore re-applies the generation-qualified waiting identity");
    check(app.call_waiting_recorded, "restore re-applies the waiting-recorded flag");
    check(app.call_waiting_withheld, "restore re-applies the waiting-withheld flag");
    check(app.call_waiting_diverted, "restore re-applies the waiting-diverted flag");

    /* Restore rebuilds the HELD leg only; the (new) live-call fields that the
     * reset cleared stay cleared for the dialled B to fill in. */
    check(app.call_number[0] == '\0', "restore does not touch the live number");
    check(app.call_record_id == 0u, "restore does not touch the live record id");
    check(app.call_connected_ms == 0u, "restore does not touch the live connect stamp");
}

/* No waiting call at snapshot time: restore must not fabricate one, and must
 * not clobber the (empty) waiting fields either. */
static void test_restore_without_waiting_leaves_waiting_untouched(void) {
    app_t app;
    populate_live_call(&app, false, NOW_MS - 5000u);

    call_newcall_preserve_t snap;
    memset(&snap, 0xa5, sizeof(snap));
    call_newcall_snapshot(&app, NOW_MS, &snap);
    check(!snap.had_waiting, "no waiting call -> had_waiting false");
    check(snap.waiting_id == 0u && snap.waiting_generation == 0u &&
              snap.waiting_episode == 0u,
          "no waiting call -> empty waiting identity");
    check(!snap.waiting_recorded, "no waiting call -> waiting_recorded false");
    check(snap.wait_number[0] == '\0', "no waiting call -> empty waiting number");
    check(snap.wait_name[0] == '\0', "no waiting call -> empty waiting name");

    simulate_session_reset(&app);
    /* Plant a sentinel so a spurious write to the waiting fields is visible
     * even though the snapshot's copies are empty. */
    copy_text(app.call_waiting_number, sizeof(app.call_waiting_number), "sentinel");
    copy_text(app.call_waiting_name, sizeof(app.call_waiting_name), "sentinel");

    call_newcall_restore_held(&app, &snap);
    check(!app.call_waiting_pending, "restore without waiting leaves pending false");
    check(app.call_waiting_id == 0u && app.call_waiting_generation == 0u &&
              app.call_waiting_episode == 0u,
          "restore without waiting leaves identity empty");
    check(!app.call_waiting_recorded, "restore without waiting leaves recorded false");
    check(!app.call_waiting_withheld && !app.call_waiting_diverted,
          "restore without waiting leaves presentation flags false");
    check_str(app.call_waiting_number, "sentinel", "restore without waiting leaves the waiting number untouched");
    check_str(app.call_waiting_name, "sentinel", "restore without waiting leaves the waiting name untouched");
    /* The held leg is still rebuilt. */
    check_str(app.call_held_number, "+15551230001", "held number restored without waiting");
    check_str(app.call_held_name, "Alice", "held name restored without waiting");
    check(app.call_held_record_list == STORE_CALL_LIST_DIALLED, "held list restored without waiting");
    check(app.call_held_record_id == 77u, "held id restored without waiting");
    check(app.call_held_elapsed_seconds == 5u, "held elapsed restored without waiting");
    check(app.call_secondary_active && app.call_held, "held flags set without waiting");
}

/* A live call that has not connected yet (call_connected_ms == 0) has no
 * elapsed time to carry over -- must not read the stamp as "connected at 0". */
static void test_unconnected_call_has_zero_elapsed(void) {
    app_t app;
    populate_live_call(&app, false, 0u);

    call_newcall_preserve_t snap;
    memset(&snap, 0xa5, sizeof(snap));
    call_newcall_snapshot(&app, NOW_MS, &snap);
    check(snap.held_elapsed_seconds == 0u, "connected_ms 0 -> elapsed 0");

    simulate_session_reset(&app);
    call_newcall_restore_held(&app, &snap);
    check(app.call_held_elapsed_seconds == 0u, "restore carries elapsed 0 for an unconnected leg");
    check_str(app.call_held_number, "+15551230001", "unconnected leg identity still restored");
    check(app.call_held && app.call_secondary_active, "unconnected leg still marked held");
}

/* Sub-second remainders truncate (integer seconds), and a stamp in the future
 * (clock skew / wrapped diff <= 0) reads as 0 rather than a huge value. */
static void test_elapsed_rounding_and_future_stamp(void) {
    app_t app;
    call_newcall_preserve_t snap;

    populate_live_call(&app, false, NOW_MS - 5999u);
    call_newcall_snapshot(&app, NOW_MS, &snap);
    check(snap.held_elapsed_seconds == 5u, "5.999 s truncates to 5");

    populate_live_call(&app, false, NOW_MS + 1000u);
    call_newcall_snapshot(&app, NOW_MS, &snap);
    check(snap.held_elapsed_seconds == 0u, "future connect stamp -> elapsed 0");

    populate_live_call(&app, false, NOW_MS);
    call_newcall_snapshot(&app, NOW_MS, &snap);
    check(snap.held_elapsed_seconds == 0u, "connected exactly now -> elapsed 0");
}

int main(void) {
    test_snapshot_and_restore_with_waiting();
    test_restore_without_waiting_leaves_waiting_untouched();
    test_unconnected_call_has_zero_elapsed();
    test_elapsed_rounding_and_future_stamp();

    if (s_failures != 0) {
        fprintf(stderr, "%d failures\n", s_failures);
        return 1;
    }
    printf("call_newcall_preserve tests passed\n");
    return 0;
}

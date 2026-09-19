#include "services/phonebook_service.h"
#include "apps/calls_app.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "audio/audio_levels.h"
#include "apps/call_options_logic.h"
#include "apps/dialogs_app.h"
#include "apps/phonebook_app.h"
#include "apps/profiles_app.h"
#include "services/core1_services.h"
#include "generated/tones.h"
#include "services/input_keys.h"
#include "services/log.h"
#include "services/modem_service.h"
#include "services/strings.h"
#include "ui/ui.h"
#include "ui/status_chrome.h"
#include "storage/store_service.h"
#include "services/timebase.h"

#define CALLING_FRAME_MS 480u
#define CALLING_CONTACT_REVEAL_MS 480u
#define CALL_ACTIVE_CONFIRM_MS 250u
#define CALL_CONNECT_DELAY_MS 480u
#define CALL_NO_NETWORK_DELAY_MS 1200u
/* The service does not expose supplementary-command terminal results in the
 * Phase-1 flat status. Bound a failed targeted-release admission so the menu
 * can be retried without allowing rapid duplicate commands. */
#define CALL_WAITING_RELEASE_RETRY_MS 60000u
/* Bench B6: the network can leave the survivor HELD when the remote active leg
 * releases. Give split release/retrieve events time to settle, then retrieve
 * only if the same id is still the sole held leg. */
#define CALL_SURVIVOR_RETRIEVE_SETTLE_MS 500u
#define CALL_SURVIVOR_RETRIEVE_RETRY_MS 250u
/* Backstop for the New-call (2nd MO) connect gate: if the modem sits ACTIVE with the
 * ORIGINAL active id (the dialled party never took over -- e.g. it connected then
 * dropped inside the confirm window, or its result was lost) revert to the surviving
 * single call rather than wait forever. Comfortably past any normal ring cycle, and
 * only ever evaluated while the modem is ACTIVE (a legitimately-slow ring leaves the
 * modem DIALING, so this can't preempt it). */
#define NEW_CALL_SETUP_TIMEOUT_MS 60000u
#define CALLING_STATUS_X 6
#define CALLING_STATUS_Y 7
#define CALLING_STATUS_W 72
#define CALLING_DETAIL_Y 16
#define CALLING_NUMBER_Y 28
#define CALLING_TIMER_X 0
#define CALLING_TIMER_Y 28
#define CALLING_TIMER_W 78
#define CALLING_HANDSET_BITMAP_ID 19u
#define CALLING_HANDSET_X 6
#define CALLING_HANDSET_Y 0
#define CALL_RESULT_RECORD_ID 0x33u
#define CALL_RINGBACK_TONE_INDEX 28u
#define CALL_VOLUME_TIMEOUT_MS 1400u
#define CALL_VOLUME_MIN AUDIO_CALL_VOLUME_MIN
#define CALL_VOLUME_MAX AUDIO_CALL_VOLUME_MAX
/* Standalone route is firmware-pinned; motor envelope is a clone-side tuning value. */
#define CALL_VIBRA_PULSE_TICKS 48u
#define INCOMING_DETAIL_X 6
#define INCOMING_DETAIL_Y 8
#define INCOMING_DETAIL_W 72
#define INCOMING_DETAIL_PITCH 9
#define INCOMING_STATUS_X 6
#define INCOMING_STATUS_Y 28
#define INCOMING_STATUS_W 72

typedef enum {
    CALL_OPTION_ANSWER_WAITING = 0,
    CALL_OPTION_REJECT_WAITING,
    CALL_OPTION_SWAP,
    CALL_OPTION_HOLD,
    CALL_OPTION_NEW_CALL,
    CALL_OPTION_END_THIS_CALL,
    CALL_OPTION_END_ALL,
    CALL_OPTION_SEND_DTMF,
    CALL_OPTION_PHONE_BOOK,
} call_option_t;

typedef enum {
    WAITING_SURVIVOR_UNRESOLVED = 0,
    WAITING_SURVIVOR_GONE,
    WAITING_SURVIVOR_RINGING,
    WAITING_SURVIVOR_CONNECTED,
} waiting_survivor_t;

#define CALL_OPTION_VISIBLE_MAX 11u

static void connect_call(app_t *app, uint32_t now);
static void end_call(app_t *app, uint32_t now, bool local_hangup);
static void finalize_call_record(app_t *app, uint32_t now, store_call_reason_t reason);
static void record_received_call(app_t *app);
static void open_incoming_call(app_t *app, const char *number, const char *name);
static void open_missed_call_notice(app_t *app, uint32_t now);
static void note_new_missed_call(app_t *app);
static void open_call_options(app_t *app);
static bool answer_incoming_call(app_t *app, uint32_t now);
static bool reject_incoming_call(app_t *app, uint32_t now);
static void open_waiting_call(app_t *app, const char *number, const char *name,
                              bool withheld, bool diverted, uint32_t now);
static void capture_waiting_identity(app_t *app,
                                     const modem_call_snapshot_t *snapshot);
static waiting_survivor_t waiting_survivor_state(
    const app_t *app, const modem_call_snapshot_t *snapshot);
static void clear_waiting_call(app_t *app);
static void note_waiting_missed_call(app_t *app);
static bool answer_waiting_call(app_t *app, uint32_t now);
static bool reject_waiting_call(app_t *app, uint32_t now);
static bool swap_calls(app_t *app, uint32_t now);
static bool release_active_call(app_t *app, uint32_t now);
static bool release_active_with_waiting(app_t *app, uint32_t now);
static bool cancel_new_call_setup(app_t *app, uint32_t now);
static void handoff_waiting_to_incoming(app_t *app, const modem_status_t *status,
                                        uint32_t now);
static void handoff_waiting_to_connected(app_t *app, const modem_status_t *status,
                                         uint32_t now);
static void promote_held_leg_to_active(app_t *app, uint32_t now, store_call_reason_t reason);
static void rollback_failed_new_call(app_t *app, modem_call_result_t result,
                                     const modem_status_t *status, uint32_t now);
static void swap_call_display_slots(app_t *app, uint32_t now);
static void establish_two_call_from_waiting(app_t *app, uint32_t now);
static void finalize_held_call(app_t *app);
static void open_call_result_display(app_t *app, modem_call_result_t result, bool network_registered, uint32_t now);
static store_call_reason_t call_reason_for_result(modem_call_result_t result, bool network_registered);
static void start_call_progress_tone(void);
static void start_incoming_call_tone(void);
static void stop_call_tone(void);
static void call_session_clear_fields(app_t *app);
static uint16_t vibra_pulse_arg(void);
static void render_call_volume(const app_t *app, framebuffer_t *fb);
static void show_call_volume(app_t *app, int8_t delta, uint32_t now);
static void apply_call_volume_gain(uint8_t level);
static uint8_t call_options_build(const app_t *app, call_option_t *options, const char **labels);
static bool route_is_call_surface(const app_t *app);
static bool speed_dial_entry_for_digit(char digit, phonebook_entry_t *entry);
static char dtmf_char_for_key(uint16_t key);
static void format_call_duration(uint32_t elapsed_seconds, char *dst, size_t cap);
static uint32_t call_arm_stamp(uint32_t stamp_ms);
static void draw_incoming_stacked(framebuffer_t *fb, const font_t *font,
                                  uint16_t sid, int y0);

void calls_app_init(app_t *app) {
    app->last_modem_call_state = MODEM_CALL_IDLE;
    app->last_modem_call_result = MODEM_CALL_RESULT_NONE;
    app->call_volume_level = 5u;
    uint8_t stored = 0u;
    if (store_setting_get_u8(STORE_SETTING_CALL_VOLUME, &stored) == STORE_STATUS_OK &&
        stored >= CALL_VOLUME_MIN && stored <= CALL_VOLUME_MAX) {
        app->call_volume_level = stored;
    }
    app->call_last_second = -1;
}

void render_call(const app_t *app, framebuffer_t *fb) {
    fb_clear(fb, false);
    if (app->call_phase == CALL_PHASE_CONNECTED && app->call_volume_visible) {
        render_call_volume(app, fb);
        return;
    }
    draw_status(fb, app->signal_bars);
    fb_bitmap(fb, CALLING_HANDSET_BITMAP_ID, CALLING_HANDSET_X, CALLING_HANDSET_Y, true, true);

    const font_t *setup_font = asset_font(FONT_FS2);
    const font_t *detail_font = asset_font(FONT_FS1);
    if (app->call_phase == CALL_PHASE_CONNECTED) {
        /* A SINGLE held call (not the two-call state) gets the v6.00 persistent
         * "%U on hold" screen (SID 0x1f0): the held party's name substituted into
         * the localized template, wrapped across the call area, and NO running
         * timer (RE'd 2026-07-11; render record 0x1b, duration=0). This is a
         * distinct screen from the active/two-call one, so it early-returns.
         * In the TWO-CALL state the original shows the ACTIVE call + its timer
         * only -- the held party is never co-rendered (the both-parties composite
         * 0x25da66 is dead/unreferenced) -- so call_held alone must NOT paint an
         * "on hold" line there; that was the calls_app.c:153 divergence.
         * A waiting call takes render priority (as in the active+waiting case
         * below) so the user can see who to Answer/Reject -- so gate this
         * standalone hold screen on !call_waiting_pending; held+waiting falls
         * through to the name + "waiting" block. */
        if (app->call_held && !app->call_secondary_active && !app->call_waiting_pending) {
            const char *name = app->call_name[0] ? app->call_name : app->call_number;
            char composed[48];
            if (name[0] == '\0') {
                /* No CLI/name: the ROM's no-detail held label "Call\non hold"
                 * (SID 0x189), not "%U on hold" with an empty %U (which renders a
                 * bare " on hold"). [BP] confirm vs a "Call %N on hold" variant if
                 * the held-screen %U resolver differs -- but 0x189 is the correct
                 * no-detail paired string. */
                copy_text(composed, sizeof(composed), ts_or(0x189u, "Call\non hold"));
            } else {
                ui_format_named(composed, sizeof(composed), ts_or(0x1f0u, "%U on hold"), name);
            }
            draw_text_block(fb, detail_font, composed,
                            CALLING_STATUS_X, CALLING_STATUS_Y,
                            CALLING_STATUS_W, 9, 3u);
            draw_softkey(fb, "Options");
            return;
        }
        if (app->call_waiting_pending) {
            /* v6.00's four-row incoming-state tables also own the waiting-call
             * overlay. With caller/private detail it uses the short setup row
             * (`waiting` / `waiting >`) at the bottom; without detail it shows
             * only the tall idle row (`Waiting call` / `Diverted call waiting`). */
            bool has_waiting_detail = app->call_waiting_withheld ||
                                      app->call_waiting_name[0] != '\0' ||
                                      app->call_waiting_number[0] != '\0';
            if (!has_waiting_detail) {
                draw_incoming_stacked(fb,
                                      setup_font,
                                      app->call_waiting_diverted ? 0x131u : 0x3fdu,
                                      INCOMING_DETAIL_Y);
            } else {
                if (app->call_waiting_withheld) {
                    draw_incoming_stacked(fb, detail_font, 0x0d3u,
                                          INCOMING_DETAIL_Y);
                } else if (app->call_waiting_name[0] != '\0') {
                    fb_text(fb, detail_font, app->call_waiting_name,
                            INCOMING_DETAIL_X, INCOMING_DETAIL_Y, true,
                            INCOMING_DETAIL_W);
                    if (app->call_waiting_number[0] != '\0') {
                        draw_right_text_box(fb, detail_font,
                                            app->call_waiting_number,
                                            INCOMING_DETAIL_X,
                                            INCOMING_DETAIL_Y + INCOMING_DETAIL_PITCH,
                                            INCOMING_DETAIL_W);
                    }
                } else {
                    draw_right_text_box(fb, detail_font,
                                        app->call_waiting_number,
                                        INCOMING_DETAIL_X, INCOMING_DETAIL_Y,
                                        INCOMING_DETAIL_W);
                }
                fb_text(fb,
                        setup_font,
                        app->call_waiting_diverted
                            ? ts_or(0x3feu, "waiting >")
                            : ts_or(0x18fu, "waiting"),
                        INCOMING_STATUS_X,
                        INCOMING_STATUS_Y,
                        true,
                        INCOMING_STATUS_W);
            }
            draw_softkey(fb, "Options");
            return;
        }
        /* Active call (single or the visible leg of a two-call): name + timer. */
        fb_text(fb,
                detail_font,
                app->call_name[0] ? app->call_name : app->call_number,
                CALLING_STATUS_X,
                CALLING_STATUS_Y,
                true,
                CALLING_STATUS_W);
        char duration[12];
        uint32_t elapsed = 0u;
        uint32_t now = time_ms();
        if (app->call_connected_ms != 0u &&
            time_diff_ms(now, app->call_connected_ms) >= 0) {
            elapsed = (uint32_t)time_diff_ms(now, app->call_connected_ms) / 1000u;
        }
        format_call_duration(elapsed, duration, sizeof(duration));
        draw_right_text_box(fb, detail_font, duration, CALLING_TIMER_X,
                            CALLING_TIMER_Y, CALLING_TIMER_W);
        draw_softkey(fb, "Options");
        return;
    }

    /* Sized for the longest localized "Calling" (RUSS 0x187 "Соединяет с" is 21
     * bytes) plus the animated dots -- a 12-byte buffer split the Cyrillic label
     * mid-codepoint. fb_text still clips to CALLING_STATUS_W at draw time. */
    char calling[28];
    uint8_t dots = (uint8_t)(app->call_frame_index + 1u);
    if (dots >= 4u) {
        dots = 0u;
    }
    copy_text(calling, sizeof(calling), ts_or(0x187u, "Calling"));
    size_t len = strlen(calling);
    for (uint8_t i = 0; i < dots && len + 1u < sizeof(calling); i++) {
        calling[len++] = '.';
    }
    calling[len] = '\0';
    fb_text(fb, setup_font, calling, CALLING_STATUS_X, CALLING_STATUS_Y, true, CALLING_STATUS_W);
    bool show_name = app->call_name[0] != '\0' && app->call_number[0] != '\0' &&
                     (app->call_frame_index > 0u ||
                      time_diff_ms(time_ms(), app->call_started_ms + CALLING_CONTACT_REVEAL_MS) >= 0);
    if (show_name) {
        fb_text(fb, detail_font, app->call_name, CALLING_STATUS_X, CALLING_DETAIL_Y, true, CALLING_STATUS_W);
    } else if (app->call_number[0] != '\0') {
        draw_right_text_box(fb, detail_font, app->call_number, CALLING_STATUS_X, CALLING_NUMBER_Y, CALLING_STATUS_W);
    }
    draw_softkey(fb, ts_or(0x2deu, "End"));
}

void render_call_options(const app_t *app, framebuffer_t *fb) {
    const char *labels[CALL_OPTION_VISIBLE_MAX];
    call_option_t options[CALL_OPTION_VISIBLE_MAX];
    uint8_t count = call_options_build(app, options, labels);
    uint8_t selected = app->call_options_selected;
    if (selected >= count) {
        selected = 0u;
    }
    draw_flat_list(fb,
                   labels,
                   count,
                   selected,
                   "",
                   ts_or(0x2eeu, "Select"));
}

/* Draw a stacked multi-line SID label (e.g. "Diverted\ncall", "Private\nnumber",
 * "\n       Call") on the incoming-call detail rows: fetch the localized ROM
 * string and draw each '\n'-separated segment at y0 + row*pitch. Faithful to
 * the ROM's per-language line breaks (the clone previously hard-split these). */
static void draw_incoming_stacked(framebuffer_t *fb, const font_t *font, uint16_t sid, int y0) {
    const char *text = ts(sid);
    if (text == 0) {
        return;
    }
    char line[28];
    int row = 0;
    const char *p = text;
    while (*p != '\0' && row < 3) {
        size_t n = 0u;
        while (p[n] != '\0' && p[n] != '\n' && n < sizeof(line) - 1u) {
            n++;
        }
        memcpy(line, p, n);
        line[n] = '\0';
        fb_text(fb, font, line, INCOMING_DETAIL_X, y0 + row * INCOMING_DETAIL_PITCH, true, INCOMING_DETAIL_W);
        row++;
        p += n;
        if (*p == '\n') {
            p++;
        }
    }
}

void render_incoming_call(const app_t *app, framebuffer_t *fb) {
    fb_clear(fb, false);
    draw_status(fb, app->signal_bars);
    const font_t *title_font = asset_font(FONT_FS2);
    const font_t *detail_font = asset_font(FONT_FS1);
    /* v6.00 chooses between paired tables: tall idle labels when no caller
     * detail exists, and short setup labels at the bottom when caller text or
     * the record-0x1d Private-number detail exists. Redirected calls therefore
     * keep their identity and select `calling >`; they do not hide it. */
    bool has_detail = app->call_incoming_withheld ||
                      app->call_name[0] != '\0' ||
                      app->call_number[0] != '\0';
    if (!has_detail) {
        if (app->call_incoming_diverted) {
            /* SID 0x0130 "Diverted\ncall": stacked two-line label. */
            draw_incoming_stacked(fb, title_font, 0x130u, INCOMING_DETAIL_Y);
        } else {
            /* the tall idle label SID 0x008e "\n       Call" (CALL.1): the empty
             * first line self-positions the caption on the second row. */
            draw_incoming_stacked(fb, title_font, 0x08eu, INCOMING_DETAIL_Y);
        }
        if (app->call_incoming_silenced) {
            fb_text(fb, detail_font, ts_or(0x2abu, "Silent"), INCOMING_DETAIL_X, INCOMING_STATUS_Y, true, INCOMING_DETAIL_W);
        }
        draw_softkey(fb, "Answer");
        return;
    }
    if (app->call_incoming_withheld) {
        /* Private number is detail-window text in the original, even though it
         * spans two lines. If silenced, the bottom status row remains available
         * for the Silent indication used by the existing call interaction. */
        draw_incoming_stacked(fb, detail_font, 0x0d3u, INCOMING_DETAIL_Y);
    } else if (app->call_name[0] != '\0') {
        fb_text(fb, detail_font, app->call_name, INCOMING_DETAIL_X, INCOMING_DETAIL_Y, true, INCOMING_DETAIL_W);
        if (app->call_incoming_silenced) {
            fb_text(fb, detail_font, ts_or(0x2abu, "Silent"), INCOMING_DETAIL_X, INCOMING_DETAIL_Y + INCOMING_DETAIL_PITCH, true, INCOMING_DETAIL_W);
        } else if (app->call_number[0] != '\0') {
            draw_right_text_box(fb,
                                detail_font,
                                app->call_number,
                                INCOMING_DETAIL_X,
                                INCOMING_DETAIL_Y + INCOMING_DETAIL_PITCH,
                                INCOMING_DETAIL_W);
        }
    } else {
        draw_right_text_box(fb, detail_font, app->call_number, INCOMING_DETAIL_X, INCOMING_DETAIL_Y, INCOMING_DETAIL_W);
        if (app->call_incoming_silenced) {
            fb_text(fb, detail_font, ts_or(0x2abu, "Silent"), INCOMING_DETAIL_X, INCOMING_DETAIL_Y + INCOMING_DETAIL_PITCH, true, INCOMING_DETAIL_W);
        }
    }
    /* Setup table 0x002e2e34: `calling` or redirected `calling >`. */
    fb_text(fb,
            title_font,
            app->call_incoming_withheld && app->call_incoming_silenced
                ? ts_or(0x2abu, "Silent")
                : (app->call_incoming_diverted
                    ? ts_or(0x18eu, "calling >")
                    : ts_or(0x18du, "calling")),
            INCOMING_STATUS_X,
            INCOMING_STATUS_Y,
            true,
            INCOMING_STATUS_W);
    draw_softkey(fb, "Answer");
}

bool handle_call_key(app_t *app, uint16_t key, uint32_t now) {
    if (key == KEY_C) {
        /* v6.00 waiting-call handler 0x00299c0c maps hard C to the same
         * id-scoped reject action (cmd 0x2b) as menu Reject (mask 0x0040).
         * A session hangup here drops both the foreground and waiting legs.
         * Setup mode has no waiting overlay and keeps its dedicated
         * cancellation path below. */
        if (app->call_phase == CALL_PHASE_CONNECTED &&
            app->call_waiting_pending) {
            return reject_waiting_call(app, now);
        }
        if (!cancel_new_call_setup(app, now)) {
            end_call(app, now, true);
        }
        return true;
    }
    if (app->call_phase == CALL_PHASE_CONNECTED && (key == KEY_UP || key == KEY_DOWN)) {
        show_call_volume(app, key == KEY_UP ? 1 : -1, now);
        return true;
    }
    char dtmf = dtmf_char_for_key(key);
    if (dtmf != '\0' && app->call_phase == CALL_PHASE_CONNECTED) {
        (void)modem_service_request_dtmf(dtmf);
        return true;
    }
    if (key == KEY_NAVI) {
        if (app->call_phase == CALL_PHASE_CONNECTED) {
            open_call_options(app);
        } else {
            if (!cancel_new_call_setup(app, now)) {
                end_call(app, now, true);
            }
        }
        return true;
    }
    return true;
}

bool handle_call_options_key(app_t *app, uint16_t key, uint32_t now) {
    const char *labels[CALL_OPTION_VISIBLE_MAX];
    call_option_t options[CALL_OPTION_VISIBLE_MAX];
    uint8_t count = call_options_build(app, options, labels);
    if (app->call_options_selected >= count) {
        app->call_options_selected = 0u;
    }
    if (key == KEY_C) {
        app->route = APP_ROUTE_CALL;
        app->dirty = true;
        return true;
    }
    if (key == KEY_UP) {
        if (app->call_options_selected > 0u) {
            app->call_options_selected--;
            app->dirty = true;
        }
        return true;
    }
    if (key == KEY_DOWN) {
        if (app->call_options_selected + 1u < count) {
            app->call_options_selected++;
            app->dirty = true;
        }
        return true;
    }
    if (key != KEY_NAVI) {
        return true;
    }

    switch (options[app->call_options_selected]) {
    case CALL_OPTION_ANSWER_WAITING:
        return answer_waiting_call(app, now);
    case CALL_OPTION_REJECT_WAITING:
        return reject_waiting_call(app, now);
    case CALL_OPTION_SWAP:
        return swap_calls(app, now);
    case CALL_OPTION_HOLD:
        /* The menu and service independently require a sole confirmed leg, so
         * this is the vendor's single-call network hold/retrieve operation.
         * Do NOT flip
         * call_held optimistically -- it is driven from the modem's confirmed
         * hold state in poll_call_runtime, so "On hold" reflects the real network
         * state (a failed hold never shows a false "On hold"; the uplink is muted
         * by the network, matching the original 3210's single-call hold). */
        /* A manual Hold/Unhold supersedes the delayed B6 auto-retrieve. Without
         * this cancellation, a fast user Unhold followed by the timer could
         * enqueue the same toggle twice and put the call back on hold. */
        app->call_survivor_retrieve_pending = false;
        app->call_survivor_retrieve_id = 0u;
        (void)modem_service_request_call_hold();
        app->route = APP_ROUTE_CALL;
        app->dirty = true;
        return true;
    case CALL_OPTION_NEW_CALL:
        open_editor(app,
                    "Enter number:",
                    "",
                    30u,
                    EDITOR_KIND_NUMBER,
                    EDITOR_CONTEXT_IN_CALL_NEW_CALL,
                    true,
                    now);
        return true;
    case CALL_OPTION_END_THIS_CALL:
        /* Release ONLY the active leg (v6.00 internal cmd 0x27: flags the ACTIVE/
         * outgoing calls for release, leaves the HELD and WAITING slots intact --
         * RE'd 2026-07-11). Two-call: CHLD=1 releases active + promotes the held
         * leg (release_active_call). Active+waiting: use an id-scoped release for
         * the foreground leg, leaving the waiting leg intact so it can re-present
         * as a normal incoming call. This is what makes it distinct from "End all
         * calls" below. */
        if (app->call_waiting_pending && !app->call_secondary_active) {
            return release_active_with_waiting(app, now);
        }
        if (app->call_secondary_active) {
            return release_active_call(app, now);
        }
        end_call(app, now, true);
        return true;
    case CALL_OPTION_END_ALL: {
        /* End ALL calls (v6.00 internal cmd 0x28: flags every slot incl. the
         * WAITING one). Unlike "End this call", a ringing waiting call is REJECTED
         * (CHLD=0 UDUB) so it does not re-present, then the active/held leg(s) are
         * hung up. The waiting REJECT must reach the modem FIRST (so the waiting
         * can't momentarily surface between the two commands). Both requests
         * front-push onto the modem queue (LIFO), so to send REJECT first we must
         * enqueue HANGUP first (end_call) and the REJECT second -- the later
         * front-push pops ahead of it. Capture the flag before end_call, whose
         * call_session_reset clears call_waiting_pending. */
        bool had_waiting = app->call_waiting_pending;
        end_call(app, now, true);
        if (had_waiting) {
            (void)modem_service_request_call_waiting_reject();
        }
        return true;
    }
    case CALL_OPTION_SEND_DTMF:
        open_editor(app,
                    ts_or(0x379u, "DTMF"),
                    "",
                    MODEM_DTMF_SEQUENCE_MAX,
                    EDITOR_KIND_NUMBER,
                    EDITOR_CONTEXT_IN_CALL_DTMF,
                    true,
                    now);
        return true;
    case CALL_OPTION_PHONE_BOOK:
        start_phonebook_list(app,
                             PHONEBOOK_LABEL_CALL,
                             "",
                             PHONEBOOK_CONTEXT_IN_CALL,
                             0u,
                             now);
        return true;
    default:
        return true;
    }
}

bool handle_incoming_call_key(app_t *app, uint16_t key, uint32_t now) {
    if (key == KEY_NAVI) {
        return answer_incoming_call(app, now);
    }
    if (key == KEY_C) {
        return reject_incoming_call(app, now);
    }
    if (key == KEY_UP || key == KEY_DOWN) {
        /* "Silent" (SID 0x02ab) keeps the call alive but kills ring + vibra. */
        stop_call_tone();
        app->call_incoming_silenced = true;
        app->dirty = true;
        return true;
    }
    return true;
}

bool calls_app_handle_hook(app_t *app, uint32_t now) {
    /* HDC-5 inline button = SEND on a ringing call (answer), END while in a call
     * (hang up) -- mirrors how the original funnels the headset hook into the same
     * SEND/END call actions. No-op (returns false) off the call surfaces. */
    switch (app->route) {
    case APP_ROUTE_INCOMING_CALL:
        return answer_incoming_call(app, now);
    case APP_ROUTE_CALL:
    case APP_ROUTE_CALL_OPTIONS:
        if (!cancel_new_call_setup(app, now)) {
            end_call(app, now, true);
        }
        return true;
    default:
        return false;
    }
}

static bool leg_is_presenting(const modem_call_leg_snapshot_t *leg) {
    return leg->state == CALL_LEG_INCOMING || leg->state == CALL_LEG_WAITING;
}

static const modem_call_leg_snapshot_t *waiting_leg_by_ref(
    const app_t *app, const modem_call_snapshot_t *snapshot,
    bool *ambiguous) {
    if (ambiguous != NULL) *ambiguous = false;
    if (app->call_waiting_id != 0u && app->call_waiting_generation != 0u) {
        for (uint8_t i = 0u; i < snapshot->leg_count; i++) {
            const modem_call_leg_snapshot_t *leg = &snapshot->legs[i];
            if (leg->id == app->call_waiting_id &&
                leg->generation == app->call_waiting_generation) {
                return leg;
            }
        }
    }
    if (app->call_waiting_episode == 0u) return NULL;

    const modem_call_leg_snapshot_t *match = NULL;
    for (uint8_t i = 0u; i < snapshot->leg_count; i++) {
        const modem_call_leg_snapshot_t *leg = &snapshot->legs[i];
        if (leg->incoming_episode != app->call_waiting_episode) continue;
        if (match != NULL) {
            if (ambiguous != NULL) *ambiguous = true;
            return NULL;
        }
        match = leg;
    }
    return match;
}

static void capture_waiting_identity(app_t *app,
                                     const modem_call_snapshot_t *snapshot) {
    if (snapshot == NULL || snapshot->overflow) return;
    if (app->call_waiting_episode == 0u &&
        snapshot->pending_incoming_active &&
        snapshot->pending_incoming_alert_observed) {
        app->call_waiting_episode = snapshot->pending_incoming_episode;
    }

    bool ambiguous = false;
    const modem_call_leg_snapshot_t *candidate =
        waiting_leg_by_ref(app, snapshot, &ambiguous);
    if (ambiguous) return;
    if (candidate == NULL) {
        for (uint8_t i = 0u; i < snapshot->leg_count; i++) {
            const modem_call_leg_snapshot_t *leg = &snapshot->legs[i];
            if (leg->pending_removal || !leg_is_presenting(leg)) continue;
            if (app->call_waiting_episode != 0u &&
                leg->incoming_episode != 0u &&
                leg->incoming_episode != app->call_waiting_episode) {
                continue;
            }
            if (candidate != NULL) return; /* impossible multiset; don't guess */
            candidate = leg;
        }
    }
    if (candidate == NULL || candidate->pending_removal ||
        !leg_is_presenting(candidate)) {
        return;
    }
    app->call_waiting_id = candidate->id;
    app->call_waiting_generation = candidate->generation;
    if (candidate->incoming_episode != 0u) {
        app->call_waiting_episode = candidate->incoming_episode;
    }
}

static waiting_survivor_t waiting_survivor_state(
    const app_t *app, const modem_call_snapshot_t *snapshot) {
    if (snapshot == NULL || snapshot->overflow) {
        return WAITING_SURVIVOR_UNRESOLVED;
    }
    bool ambiguous = false;
    const modem_call_leg_snapshot_t *leg =
        waiting_leg_by_ref(app, snapshot, &ambiguous);
    if (ambiguous) return WAITING_SURVIVOR_UNRESOLVED;
    if (leg != NULL) {
        if (leg->pending_removal || leg->state == CALL_LEG_RELEASING) {
            return WAITING_SURVIVOR_UNRESOLVED;
        }
        if (leg_is_presenting(leg)) return WAITING_SURVIVOR_RINGING;
        if (leg->state == CALL_LEG_ACTIVE || leg->state == CALL_LEG_HELD) {
            return WAITING_SURVIVOR_CONNECTED;
        }
        return WAITING_SURVIVOR_UNRESOLVED;
    }
    if (app->call_waiting_episode != 0u &&
        snapshot->pending_incoming_active &&
        snapshot->pending_incoming_alert_observed &&
        snapshot->pending_incoming_episode == app->call_waiting_episode) {
        return WAITING_SURVIVOR_RINGING;
    }
    return WAITING_SURVIVOR_GONE;
}

bool poll_call_runtime(app_t *app, uint32_t now) {
    modem_status_t status;
    modem_call_snapshot_t call_snapshot;
    modem_service_get_status(&status);
    modem_service_get_call_snapshot(&call_snapshot);
    modem_call_state_t previous = app->last_modem_call_state;
    modem_call_result_t previous_result = app->last_modem_call_result;
    app->last_modem_call_state = status.call_state;
    app->last_modem_call_result = status.last_call_result;
    bool call_surface = route_is_call_surface(app);

    /* Single-call network hold: drive the "On hold" indicator from
     * the modem's call-table hold state, not an optimistic UI flag, so it
     * reflects the real network state (a failed hold shows no false "On hold").
     * The two-call held-leg state (call_secondary_active) is owned by the swap /
     * answer-waiting model -- don't overwrite it. */
    if (app->call_phase == CALL_PHASE_CONNECTED && !app->call_secondary_active &&
        app->call_held != status.call_on_hold) {
        app->call_held = status.call_on_hold;
        app->dirty = true;
    }

    /* Reconcile the optimistic two-call model when the network's HELD leg goes
     * away. Fire only on the true->false EDGE of second_call_held: the app sets
     * call_secondary_active optimistically at answer-waiting time, before the
     * CHLD=2 swap round-trip makes the network report the hold, so a plain
     * !second_call_held check would prematurely clear a legit two-call session.
     * Once the held leg is confirmed (true) and then released (false), drop the
     * phantom -- keeping the surviving active call -- so a later Swap/End can't act
     * on the dead leg. */
    if (app->call_secondary_active && app->last_second_call_held &&
        app->call_phase == CALL_PHASE_CONNECTED) {
        if (!status.second_call_held) {
            /* The held leg went away. Reconcile even if a waiting call is present
             * (a 3-leg active+held+waiting session where the HELD leg drops): the
             * outer gate already requires an ESTABLISHED two-call (secondary +
             * last_second_call_held), so second_call_held going false unambiguously
             * means the held leg released -- drop the phantom / promote, keeping the
             * waiting leg. Reducing 3-leg -> 2-leg (active+waiting) then re-enables
             * Answer for the waiting call, matching v6.00. */
            if (status.active_call_id != app->last_active_call_id) {
                /* Active leg released, network promoted the held leg (active id
                 * changed): finalize the ended active call and swap the surviving
                 * held party into view -- NOT a phantom-drop. Remote counterpart of
                 * "End this call" (release_active_call). */
                promote_held_leg_to_active(app, now, STORE_CALL_REASON_NONE);
                if (status.call_on_hold && status.active_call_id != 0u) {
                    /* B6 bench truth: the network can remove active B
                     * while leaving survivor A HELD. Delay one guarded HOLD
                     * toggle (HELD->ACTIVE); a late network promotion cancels it
                     * below before dispatch. */
                    app->call_survivor_retrieve_pending = true;
                    app->call_survivor_retrieve_id = status.active_call_id;
                    app->call_survivor_retrieve_ms =
                        now + CALL_SURVIVOR_RETRIEVE_SETTLE_MS;
                }
            } else {
                /* Held leg dropped, active unchanged: drop the phantom. */
                finalize_held_call(app);
                app->call_held_number[0] = '\0';
                app->call_held_name[0] = '\0';
                app->call_secondary_active = false;
                app->call_held = false;
            }
            app->dirty = true;
        } else if (status.second_call_held &&
                   status.active_call_id != app->last_active_call_id) {
            /* Swap confirmed: both legs survive but the active/held roles flipped
             * (active_call_id changed). Swap the display slots now -- driven by the
             * confirmed active-id change, not optimistically at Swap-press, so a failed CHLD=2
             * leaves the display untouched. */
            swap_call_display_slots(app, now);
            app->dirty = true;
        }
    }
    app->last_second_call_held = status.second_call_held;
    app->last_active_call_id = status.active_call_id;

    if (call_surface && app->call_phase == CALL_PHASE_SETUP &&
        app->call_secondary_active && app->call_new_active_ref != 0u &&
        status.call_state == MODEM_CALL_DIALING &&
        status.active_call_id == 0u) {
        /* Corrected golden #3 / C4: original A ended while New-call B was
         * dialing. The flat contract identifies this by retaining DIALING for B
         * while dropping A's former active id to zero. Finalize only A's saved
         * held slot and let B continue as an ordinary sole setup; treating the
         * transition as IDLE would show "Error in connection" and lose B. */
        int32_t setup_elapsed_ms = time_diff_ms(now, app->call_started_ms);
        if (setup_elapsed_ms > 0) {
            app->call_held_elapsed_seconds +=
                (uint32_t)setup_elapsed_ms / 1000u;
        }
        finalize_held_call(app);
        app->call_held_number[0] = '\0';
        app->call_held_name[0] = '\0';
        app->call_secondary_active = false;
        app->call_held = false;
        app->call_new_active_ref = 0u;
        app->dirty = true;
        LOGI("calls", "New-call original released; continuing setup as sole call");
    }

    if (app->call_survivor_retrieve_pending) {
        bool survivor_context =
            app->call_phase == CALL_PHASE_CONNECTED &&
            !app->call_secondary_active &&
            app->call_survivor_retrieve_id != 0u;
        bool survivor_is_active =
            status.call_state == MODEM_CALL_ACTIVE &&
            status.active_call_id == app->call_survivor_retrieve_id;
        if (!survivor_context ||
            status.call_state == MODEM_CALL_IDLE ||
            status.call_state == MODEM_CALL_RINGING) {
            /* The call ended, changed episode, or the user acted manually. */
            app->call_survivor_retrieve_pending = false;
            app->call_survivor_retrieve_id = 0u;
        } else if (survivor_is_active && !status.call_on_hold) {
            /* The network promoted/retrieved it on its own. Keep the short
             * settle window so a split ACTIVE->HELD setup sequence cannot make
             * us cancel the intent before the held evidence arrives. */
            if (time_diff_ms(now, app->call_survivor_retrieve_ms) >= 0) {
                app->call_survivor_retrieve_pending = false;
                app->call_survivor_retrieve_id = 0u;
            }
        } else if (survivor_is_active && status.waiting_call) {
            /* 3GPP maps both retrieve and answer-waiting to CHLD=2. Never send
             * the survivor toggle while a third leg is waiting or it would
             * answer that caller instead. Keep the intent pending; once the
             * waiting leg clears, the guarded retrieve can proceed. */
            app->call_survivor_retrieve_ms =
                now + CALL_SURVIVOR_RETRIEVE_RETRY_MS;
        } else if (survivor_is_active && status.call_on_hold &&
                   modem_service_call_release_active_pending()) {
            /* Local "End this call" uses RELEASE_ACTIVE: its CHLD semantic
             * already releases B and accepts held A. Telit can report B gone
             * before A's delayed promotion. Let that model-owned transaction
             * finish; racing it with the remote-release B6 fallback toggles A
             * ACTIVE and then immediately back to HELD. If the transaction
             * fails, this query clears and the guarded fallback below remains. */
            app->call_survivor_retrieve_ms =
                now + CALL_SURVIVOR_RETRIEVE_RETRY_MS;
        } else if (survivor_is_active && status.call_on_hold &&
                   modem_service_new_call_cleanup_pending()) {
            /* C3 bench truth: command acceptance is not proof that the
             * cancelled outgoing setup leg has left the network. Do not issue a
             * second supplementary toggle while B still exists; wait for B's
             * id-scoped RELEASED evidence. A positive ACTIVE event for A follows
             * the separate media path immediately and cancels this intent. */
            app->call_survivor_retrieve_ms =
                now + CALL_SURVIVOR_RETRIEVE_RETRY_MS;
        } else if (survivor_is_active && status.call_on_hold &&
                   time_diff_ms(now, app->call_survivor_retrieve_ms) >= 0) {
            if (modem_service_request_call_hold()) {
                LOGI("calls", "auto-retrieving held survivor id=%u",
                     (unsigned)app->call_survivor_retrieve_id);
                app->call_survivor_retrieve_pending = false;
                app->call_survivor_retrieve_id = 0u;
            } else {
                /* Queue pressure means no command crossed the admission
                 * boundary, so a bounded later retry cannot double-toggle. */
                app->call_survivor_retrieve_ms =
                    now + CALL_SURVIVOR_RETRIEVE_RETRY_MS;
            }
        } else if (status.call_state == MODEM_CALL_ACTIVE &&
                   status.active_call_id != app->call_survivor_retrieve_id &&
                   !status.second_call_held &&
                   time_diff_ms(now, app->call_survivor_retrieve_ms) >= 0) {
            /* A different sole active leg is conclusive: the intended survivor
             * is gone, so a later hold toggle would target the wrong call. */
            app->call_survivor_retrieve_pending = false;
            app->call_survivor_retrieve_id = 0u;
        }
    }

    if (call_surface && app->call_phase == CALL_PHASE_SETUP &&
        (status.call_state == MODEM_CALL_DIALING ||
         status.call_state == MODEM_CALL_ANSWERING ||
         status.call_state == MODEM_CALL_ACTIVE)) {
        app->call_result_armed = true;
    }

    if (app->call_waiting_release_pending && status.waiting_call &&
        status.active_call_id == app->call_waiting_release_active_id &&
        time_diff_ms(now, app->call_waiting_release_retry_ms) >= 0) {
        /* No structural change arrived for a full command window. Let the user
         * retry; the still-present active+waiting state remains authoritative. */
        app->call_waiting_release_pending = false;
        app->call_waiting_release_active_id = 0u;
        LOGW("calls", "targeted active release produced no call-state change");
    }

    if (!status.waiting_call && app->call_waiting_action_pending) {
        app->call_waiting_action_pending = false;
    }

    if (status.waiting_call && app->call_phase == CALL_PHASE_CONNECTED && !app->call_waiting_action_pending) {
        capture_waiting_identity(app, &call_snapshot);
        const char *number = status.incoming_number;
        char name[PHONEBOOK_NAME_MAX + 1u];
        resolve_contact_name(number, name, sizeof(name));
        if (!app->call_waiting_pending ||
            strcmp(app->call_waiting_number, number) != 0 ||
            strcmp(app->call_waiting_name, name) != 0) {
            open_waiting_call(app, number, name,
                              status.caller_id_withheld,
                              status.incoming_diverted,
                              now);
            capture_waiting_identity(app, &call_snapshot);
            return true;
        }
        /* Caller-ID and redirect indications may trail the id-bearing waiting
         * leg. They refine the existing overlay; reopening it would reset the
         * in-call Options selection and throw the user back to the call view. */
        bool metadata_changed = false;
        if (app->call_waiting_withheld != status.caller_id_withheld) {
            app->call_waiting_withheld = status.caller_id_withheld;
            metadata_changed = true;
        }
        if (app->call_waiting_diverted != status.incoming_diverted) {
            app->call_waiting_diverted = status.incoming_diverted;
            metadata_changed = true;
        }
        if (metadata_changed) {
            app->dirty = true;
            return true;
        }
    } else if (!status.waiting_call && app->call_waiting_pending) {
        if (status.second_call_held && !app->call_secondary_active) {
            /* Answered from a SINGLE active call: the waiting call became active and
             * the old active went on hold (second_call_held true, and we were not
             * already two-call). Establish the two-call from the captured waiting
             * party -- URC-driven, so a failed CHLD=2 never falsely marks the
             * answered call missed nor loses the active call.
             * The !call_secondary_active guard is the 3-leg fix: if we were ALREADY
             * two-call (active+held) when this 3rd call waited, it could only be
             * Rejected (v6.00 never offers Answer with two calls up), so a
             * waiting-gone here is a reject/give-up, NOT an answer -- fall through to
             * "missed" and keep the active+held pair, instead of establish_two_call
             * clobbering the held leg with the active one. */
            establish_two_call_from_waiting(app, now);
        } else {
            waiting_survivor_t survivor =
                waiting_survivor_state(app, &call_snapshot);
            if (survivor == WAITING_SURVIVOR_UNRESOLVED) {
                /* Overflow or a transient leg keeps identity uncertain. The
                 * call model is already reconciling; retain the captured
                 * overlay instead of inventing a missed call. */
                return false;
            }
            app->call_waiting_release_pending = false;
            app->call_waiting_release_active_id = 0u;
            if (survivor == WAITING_SURVIVOR_RINGING) {
                handoff_waiting_to_incoming(app, &status, now);
                return true;
            }
            if (survivor == WAITING_SURVIVOR_CONNECTED) {
                handoff_waiting_to_connected(app, &status, now);
                return true;
            }
            /* The waiting call went away un-answered (caller gave up, a reject, or a
             * 3rd-leg reject while already two-call) -> log it missed, keep any
             * existing active+held pair intact. */
            note_waiting_missed_call(app);
            clear_waiting_call(app);
            if (status.call_state == MODEM_CALL_RINGING) {
                /* A different generation may already be presenting. B is
                 * correctly missed, but the replacement call must not be lost
                 * merely because both changes landed in one app poll. */
                char name[PHONEBOOK_NAME_MAX + 1u];
                resolve_contact_name(status.incoming_number, name, sizeof(name));
                finalize_call_record(app, now, STORE_CALL_REASON_NONE);
                open_incoming_call(app, status.incoming_number, name);
                app->call_incoming_withheld = status.caller_id_withheld;
                app->call_incoming_diverted = status.incoming_diverted;
                return true;
            }
        }
        if (app->route == APP_ROUTE_CALL_OPTIONS) {
            app->route = APP_ROUTE_CALL;
        }
        app->dirty = true;
        return true;
    }

    if (call_surface && app->call_phase == CALL_PHASE_SETUP &&
        app->call_pending_failure_ms != 0u &&
        time_diff_ms(now, app->call_pending_failure_ms) >= 0) {
        modem_call_result_t result = app->call_pending_failure_result;
        app->call_pending_failure_ms = 0u;
        if (app->call_secondary_active) {
            /* A New-call whose deferred admission (SIM/network) failed -> revert to the
             * surviving original call A instead of resetting to standby. Rare: SIM +
             * registration are up during a live call, so this is defensive. */
            rollback_failed_new_call(app,
                result == MODEM_CALL_RESULT_NONE ? MODEM_CALL_RESULT_NO_ANSWER : result,
                &status, now);
            return true;
        }
        if (result == MODEM_CALL_RESULT_NO_CARRIER || result == MODEM_CALL_RESULT_NO_DIALTONE) {
            finalize_call_record(app, now, STORE_CALL_REASON_NO_NETWORK);
            open_call_result_display(app, result, false, now);
        } else if (result == MODEM_CALL_RESULT_NONE) {
            finalize_call_record(app, now, STORE_CALL_REASON_NONE);
            open_display_sid(app, 2u, 0x297u, "SIM card\nnot ready", APP_ROUTE_STANDBY, now);
        } else {
            finalize_call_record(app, now, call_reason_for_result(result, status.network_registered));
            open_call_result_display(app, result, status.network_registered, now);
        }
        call_session_reset(app);
        return true;
    }

    if (status.call_state == MODEM_CALL_RINGING) {
        if (app->call_pending_local_hangup) {
            return false;
        }
        const char *number = status.incoming_number;
        if (app->route == APP_ROUTE_INCOMING_CALL) {
            bool incoming_changed = false;
            if (number[0] != '\0' && strcmp(app->call_number, number) != 0) {
                char name[PHONEBOOK_NAME_MAX + 1u];
                resolve_contact_name(number, name, sizeof(name));
                copy_text(app->call_number, sizeof(app->call_number), number);
                copy_text(app->call_name, sizeof(app->call_name), name);
                incoming_changed = true;
            }
            /* Supplementary and caller-ID notifications may trail the first
             * ringing observation. Keep the already-open screen synchronized
             * so its Nokia status-table variant changes without losing detail. */
            if (app->call_incoming_withheld != status.caller_id_withheld) {
                app->call_incoming_withheld = status.caller_id_withheld;
                incoming_changed = true;
            }
            if (app->call_incoming_diverted != status.incoming_diverted) {
                app->call_incoming_diverted = status.incoming_diverted;
                incoming_changed = true;
            }
            if (incoming_changed) {
                app->dirty = true;
            }
            return incoming_changed;
        }
        if (app->route != APP_ROUTE_CALL) {
            char name[PHONEBOOK_NAME_MAX + 1u];
            resolve_contact_name(number, name, sizeof(name));
            open_incoming_call(app, number, name);
            app->call_incoming_withheld = status.caller_id_withheld;
            app->call_incoming_diverted = status.incoming_diverted;
            return true;
        }
    }

    if (status.call_state == MODEM_CALL_ACTIVE) {
        if (app->route == APP_ROUTE_CALL && app->call_phase == CALL_PHASE_SETUP) {
            /* A NEW call dialled while already on a call (call_secondary_active):
             * call_state == ACTIVE is true from the START (the original leg is
             * still active / just went held), so gate the connect on a DIFFERENT
             * active id actually appearing -- the dialled 2nd party becoming active
             * in the reconciled call table. Without this, a slow or
             * FAILED 2nd dial connects + logs the wrong party after ~250 ms and
             * strands the real call. A plain single call has ref==0 and its own id
             * != 0 on connect, so this is a no-op for it.
             * The model requires id-bearing CLCC/event evidence for a 2nd MO
             * connect. A bare CONNECT or NO CARRIER remains non-authoritative so
             * it cannot steal or tear down the held original leg. */
            if (app->call_secondary_active &&
                (status.active_call_id == 0u ||
                 status.active_call_id == app->call_new_active_ref)) {
                /* Failure escape: the New-call (2nd) dial failed but the original call
                 * survives -- the modem latched status.second_call_result (a DEDICATED
                 * field, not last_call_result, which the network's retrieve of the held
                 * original overwrites with CONNECTED). Revert to the ongoing single call
                 * with the held party rather than waiting forever for a new active id
                 * that will never come. A genuine full teardown (second_call_result stays
                 * NONE, call_state -> IDLE) falls through to the IDLE handling below; a
                 * slow-but-pending 2nd dial (still NONE) keeps waiting. [BP]: v6.00 may
                 * briefly flash the failure notice before returning to the call -- we
                 * revert silently (priority: never drop the live leg). */
                if (status.second_call_result != MODEM_CALL_RESULT_NONE &&
                    status.second_call_result != MODEM_CALL_RESULT_CONNECTED) {
                    rollback_failed_new_call(app, status.second_call_result,
                                             &status, now);
                    return true;
                }
                /* Fast path: we WITNESSED the 2nd party B active (call_active_seen_ms is
                 * set only when the active id was != the original ref) and now the
                 * original A has returned -> B connected then dropped. Conclusive; revert
                 * IMMEDIATELY instead of waiting the #4 backstop, and log B as a brief
                 * CONNECTED call (REASON_NONE via a NONE result), not NO_ANSWER. */
                if (app->call_active_seen_ms != 0u) {
                    rollback_failed_new_call(app, MODEM_CALL_RESULT_NONE,
                                             &status, now);
                    return true;
                }
                /* [BP] limit #4 recovery: B connecting then dropping ENTIRELY between
                 * polls (never witnessed active), or an ATD-OK-then-lost result, leaves
                 * the modem ACTIVE on the promoted original A with no evidence to act on
                 * -- so neither the modem setup watchdog (DIALING-gated) nor
                 * second_call_result ever frees this gate. Bound the wait: once the modem
                 * is ACTIVE (a call IS up) and we have waited past any normal ring, revert
                 * to the surviving single call. Gated on ACTIVE so a legitimately-slow
                 * ring (modem still DIALING) is never preempted. */
                if (status.call_state == MODEM_CALL_ACTIVE &&
                    time_diff_ms(now, app->call_started_ms + NEW_CALL_SETUP_TIMEOUT_MS) >= 0) {
                    rollback_failed_new_call(app, MODEM_CALL_RESULT_NO_ANSWER,
                                             &status, now);
                    return true;
                }
                return false;
            }
            if (app->call_active_seen_ms == 0u) {
                app->call_active_seen_ms = now;
                /* M4: the codec route is (re)set + unmuted the instant the call goes
                 * ACTIVE (modem_bridge_follow_call_state runs earlier this same tick),
                 * which restores the stored gain. Seed the user's volume now -- not
                 * ~250 ms later at connect_call -- so the earpiece isn't briefly at
                 * 0 dB through the active-confirm window. */
                apply_call_volume_gain(app->call_volume_level);
                return false;
            }
            if (time_diff_ms(now, app->call_active_seen_ms + CALL_ACTIVE_CONFIRM_MS) >= 0) {
                connect_call(app, now);
                if (app->call_incoming && !app->call_incoming_recorded) {
                    record_received_call(app);
                    app->call_incoming_recorded = true;
                }
                return true;
            }
            return false;
        }
        if (app->route == APP_ROUTE_INCOMING_CALL && app->call_answer_pending) {
            /* CALL.5: an answered call goes straight to the live-call view;
             * routing through CALL_PHASE_SETUP flashed "Calling." first. */
            connect_call(app, now);
            if (app->call_incoming && !app->call_incoming_recorded) {
                record_received_call(app);
                app->call_incoming_recorded = true;
            }
            return true;
        }
    }

    if (status.call_state == MODEM_CALL_IDLE) {
        if (app->call_pending_local_hangup) {
            app->call_pending_local_hangup = false;
        }
        if (app->route == APP_ROUTE_INCOMING_CALL) {
            if (!app->call_incoming_recorded && !app->call_answer_pending) {
                store_call_add_now(STORE_CALL_LIST_MISSED,
                                   app->call_number,
                                   app->call_name,
                                   0u,
                                   STORE_CALL_REASON_MISSED,
                                   &app->call_record_id);
                app->call_record_list = STORE_CALL_LIST_MISSED;
                app->call_incoming_recorded = true;
                note_new_missed_call(app);
                open_missed_call_notice(app, now);
                call_session_reset(app);
                return true;
            }
            app->route = APP_ROUTE_STANDBY;
            call_session_reset(app);
            app->dirty = true;
            return true;
        }
        if (call_surface && app->call_result_armed &&
            !app->call_pending_local_hangup &&
            (previous == MODEM_CALL_ACTIVE || previous == MODEM_CALL_ANSWERING ||
             previous == MODEM_CALL_DIALING)) {
            if (app->call_phase == CALL_PHASE_CONNECTED) {
                finalize_call_record(app, now, STORE_CALL_REASON_NONE);
                finalize_held_call(app);
                app->route = APP_ROUTE_STANDBY;
                call_session_reset(app);
                app->dirty = true;
                return true;
            }
            finalize_call_record(app, now, call_reason_for_result(status.last_call_result, status.network_registered));
            open_call_result_display(app, status.last_call_result, status.network_registered, now);
            call_session_reset(app);
            return true;
        }
    }

    if (status.last_call_result != MODEM_CALL_RESULT_NONE &&
        status.last_call_result != MODEM_CALL_RESULT_CONNECTED &&
        app->call_result_armed &&
        call_surface && app->call_phase == CALL_PHASE_SETUP) {
        finalize_call_record(app, now, call_reason_for_result(status.last_call_result, status.network_registered));
        open_call_result_display(app, status.last_call_result, status.network_registered, now);
        call_session_reset(app);
        return true;
    }

    if (status.call_state == previous && status.last_call_result == previous_result) {
        return false;
    }
    return false;
}

bool tick_call(app_t *app, uint32_t now) {
    if (app->call_volume_visible && time_diff_ms(now, app->call_volume_until_ms) >= 0) {
        app->call_volume_visible = false;
        return true;
    }
    if (app->call_phase == CALL_PHASE_SETUP) {
        uint8_t frame = (uint8_t)((uint32_t)time_diff_ms(now, app->call_started_ms) / CALLING_FRAME_MS) % 4u;
        if (frame != app->call_frame_index) {
            app->call_frame_index = frame;
            return true;
        }
        return false;
    }
    if (app->call_connected_ms == 0u) {
        return false;
    }
    int32_t elapsed = time_diff_ms(now, app->call_connected_ms) / 1000;
    if (elapsed < 0) {
        elapsed = 0;
    }
    if (elapsed != app->call_last_second) {
        app->call_last_second = elapsed;
        return true;
    }
    return false;
}

void call_newcall_snapshot(const app_t *app, uint32_t now, call_newcall_preserve_t *out) {
    copy_text(out->held_number, sizeof(out->held_number), app->call_number);
    copy_text(out->held_name, sizeof(out->held_name), app->call_name);
    out->held_list = app->call_record_list;
    out->held_id = app->call_record_id;
    out->held_elapsed_seconds = 0u;
    if (app->call_connected_ms != 0u && time_diff_ms(now, app->call_connected_ms) > 0) {
        out->held_elapsed_seconds = (uint32_t)time_diff_ms(now, app->call_connected_ms) / 1000u;
    }
    copy_text(out->wait_number, sizeof(out->wait_number), app->call_waiting_number);
    copy_text(out->wait_name, sizeof(out->wait_name), app->call_waiting_name);
    out->had_waiting = app->call_waiting_pending;
    out->waiting_id = app->call_waiting_id;
    out->waiting_generation = app->call_waiting_generation;
    out->waiting_episode = app->call_waiting_episode;
    out->waiting_recorded = app->call_waiting_recorded;
    out->waiting_withheld = app->call_waiting_withheld;
    out->waiting_diverted = app->call_waiting_diverted;
}

void call_newcall_restore_held(app_t *app, const call_newcall_preserve_t *snap) {
    copy_text(app->call_held_number, sizeof(app->call_held_number), snap->held_number);
    copy_text(app->call_held_name, sizeof(app->call_held_name), snap->held_name);
    app->call_held_record_list = snap->held_list;
    app->call_held_record_id = snap->held_id;
    app->call_held_elapsed_seconds = snap->held_elapsed_seconds;
    app->call_secondary_active = true;
    app->call_held = true;
    if (snap->had_waiting) {
        copy_text(app->call_waiting_number, sizeof(app->call_waiting_number), snap->wait_number);
        copy_text(app->call_waiting_name, sizeof(app->call_waiting_name), snap->wait_name);
        app->call_waiting_pending = true;
        app->call_waiting_id = snap->waiting_id;
        app->call_waiting_generation = snap->waiting_generation;
        app->call_waiting_episode = snap->waiting_episode;
        app->call_waiting_recorded = snap->waiting_recorded;
        app->call_waiting_withheld = snap->waiting_withheld;
        app->call_waiting_diverted = snap->waiting_diverted;
    }
}

bool start_outgoing_call(app_t *app, const char *number, const char *name, uint32_t now, app_route_t error_route) {
    if (number == 0 || number[0] == '\0') {
        open_display_sid(app, 0u, 0x208u, "No phone\nnumber", error_route, now);
        return true;
    }

    modem_status_t status;
    modem_service_get_status(&status);

    /* Admission BEFORE mutating the session: when B can be dialled immediately
     * (SIM + network ready), queue it FIRST -- so a modem REJECT ("Call not allowed")
     * leaves the CURRENT call untouched. This matters for a New-call: call_session_reset
     * would otherwise destroy the live original A (still up on the network) + its held
     * state + a waiting C, with no way back. The SIM/network-not-ready cases fall through
     * to the deferred-failure display below (they need the "Calling..." view first, and
     * cannot occur mid-call anyway -- you can't be on a call without SIM/registration). */
    if (status.sim_ready && status.network_registered &&
        !modem_service_request_dial(number)) {
        open_display_sid(app, 0u, 0x99u, "Call not\nallowed", error_route, now);
        return true;
    }

    call_session_reset(app);
    /* Snapshot the active call id BEFORE this dial. For a plain standby call there
     * is none (0); for a New-call while already on a call it is the original leg,
     * so the connect gate can require a DIFFERENT active id (the dialled party
     * actually became active) before marking the 2nd call connected. */
    app->call_new_active_ref = status.active_call_id;
    copy_text(app->call_number, sizeof(app->call_number), number);
    copy_text(app->call_name, sizeof(app->call_name), name);
    if (app->call_name[0] == '\0') {
        /* v6.00 resolves a hand-typed number to its phonebook name at CALL
         * time (the call-register loggers resolve before writing, ROM
         * 0x29984c); the dialled record below then snapshots it -- the log
         * is never re-resolved at view time. */
        resolve_contact_name(app->call_number, app->call_name, sizeof(app->call_name));
    }
    app->route = APP_ROUTE_CALL;
    app->call_phase = CALL_PHASE_SETUP;
    app->call_result_armed = false;
    app->call_started_ms = now;
    app->call_frame_index = 0u;
    app->call_record_list = STORE_CALL_LIST_DIALLED;
    app->dirty = true;

    if (status.sim_ready) {
        (void)store_call_add_now(STORE_CALL_LIST_DIALLED,
                                 app->call_number,
                                 app->call_name,
                                 0u,
                                 STORE_CALL_REASON_DIALLED,
                                 &app->call_record_id);
    }

    if (!status.sim_ready) {
        app->call_pending_failure_ms = call_arm_stamp(now + CALL_CONNECT_DELAY_MS);
        app->call_pending_failure_result = MODEM_CALL_RESULT_NONE;
        return true;
    }
    if (!status.network_registered) {
        app->call_pending_failure_ms = call_arm_stamp(now + CALL_NO_NETWORK_DELAY_MS);
        app->call_pending_failure_result = MODEM_CALL_RESULT_NO_CARRIER;
        return true;
    }
    /* SIM + network were ready, so B's dial was already queued at the top (a reject
     * returned early there without touching the session). Just start the ringback. */
    start_call_progress_tone();
    return true;
}

bool start_standby_call(app_t *app, uint32_t now) {
    if (app->input_len == 0u || app->input_text[0] == '\0') {
        return false;
    }

    char number[MODEM_PHONE_MAX + 1u];
    char name[PHONEBOOK_NAME_MAX + 1u];
    copy_text(number, sizeof(number), app->input_text);
    name[0] = '\0';
    if (strcmp(number, "1") == 0) {
        store_setting_get_text(STORE_SETTING_SYSTEM_VOICE_MAILBOX_NUMBER,
                               number,
                               sizeof(number));
        if (number[0] == '\0' &&
            !modem_service_get_voice_mailbox_number(number, sizeof(number))) {
            app->input_len = 0u;
            app->input_text[0] = '\0';
            app->input_action = APP_STANDBY_ACTION_CALL;
            app->star_cycle_until_ms = 0u;
            open_display_sid(app, 0u, 0x20au,
                             "Save voice\nmailbox\nnumber first",
                             APP_ROUTE_STANDBY, now);
            return true;
        }
    } else if (number[0] >= '2' && number[0] <= '9' && number[1] == '\0') {
        phonebook_entry_t entry;
        if (speed_dial_entry_for_digit(number[0], &entry)) {
            copy_text(number, sizeof(number), entry.number);
            copy_text(name, sizeof(name), entry.name);
        } else {
            app->input_len = 0u;
            app->input_text[0] = '\0';
            app->input_action = APP_STANDBY_ACTION_CALL;
            app->star_cycle_until_ms = 0u;
            open_display_sid(app, 0u, 0x27fu, "Speed dial number\nnot saved", APP_ROUTE_STANDBY, now);
            return true;
        }
    }

    app->input_len = 0u;
    app->input_text[0] = '\0';
    app->input_action = APP_STANDBY_ACTION_CALL;
    app->star_cycle_until_ms = 0u;
    return start_outgoing_call(app, number, name, now, APP_ROUTE_STANDBY);
}

static void connect_call(app_t *app, uint32_t now) {
    stop_call_tone();
    app->route = APP_ROUTE_CALL;
    app->call_phase = CALL_PHASE_CONNECTED;
    apply_call_volume_gain(app->call_volume_level); /* M4: seed earpiece gain for the call */
    app->call_result_armed = true;
    app->call_connected_ms = call_arm_stamp(now);
    app->call_last_second = -1;
    app->call_pending_failure_ms = 0u;
    app->call_pending_failure_result = MODEM_CALL_RESULT_NONE;
    app->call_pending_local_hangup = false;
    app->call_volume_visible = false;
    app->dirty = true;
}

static void end_call(app_t *app, uint32_t now, bool local_hangup) {
    store_call_reason_t reason = local_hangup ? STORE_CALL_REASON_NONE : STORE_CALL_REASON_UNREACHABLE;
    if (app->call_phase == CALL_PHASE_CONNECTED) {
        reason = STORE_CALL_REASON_NONE;
    } else if (app->call_incoming && !app->call_incoming_recorded) {
        reason = local_hangup ? STORE_CALL_REASON_REJECTED : STORE_CALL_REASON_MISSED;
    }

    if (app->call_incoming && !app->call_incoming_recorded && reason == STORE_CALL_REASON_MISSED) {
        (void)store_call_add_now(STORE_CALL_LIST_MISSED,
                                 app->call_number,
                                 app->call_name,
                                 0u,
                                 STORE_CALL_REASON_MISSED,
                                 &app->call_record_id);
        app->call_record_list = STORE_CALL_LIST_MISSED;
        app->call_incoming_recorded = true;
        note_new_missed_call(app);
        open_missed_call_notice(app, now);
    } else {
        finalize_call_record(app, now, reason);
        finalize_held_call(app);
        app->route = APP_ROUTE_STANDBY;
        app->dirty = true;
    }

    if (local_hangup) {
        (void)modem_service_request_hangup();
    }
    /* call_session_reset() clears call_pending_local_hangup, so set it after the
     * reset (the earlier redundant assignment was dead). */
    call_session_reset(app);
    if (local_hangup) {
        app->call_pending_local_hangup = true;
    }
}

/* CALL.3: persistent duration accounting (original EEPROM fields
 * 0x071b last / 0x071c all / 0x071d received / 0x071e dialled). Totals are
 * independent of the erasable recent-call lists. */
static void call_accounting_add(store_call_list_t list, uint32_t elapsed_seconds) {
    if (elapsed_seconds == 0u) {
        return;
    }
    uint32_t value = 0u;
    (void)store_life_timer_add_seconds(elapsed_seconds);
    (void)store_setting_set_u32(STORE_SETTING_CALL_DURATION_LAST, elapsed_seconds);
    (void)store_setting_get_u32(STORE_SETTING_CALL_DURATION_ALL, &value);
    value = UINT32_MAX - value < elapsed_seconds ? UINT32_MAX : value + elapsed_seconds;
    (void)store_setting_set_u32(STORE_SETTING_CALL_DURATION_ALL, value);
    if (list == STORE_CALL_LIST_RECEIVED) {
        value = 0u;
        (void)store_setting_get_u32(STORE_SETTING_CALL_DURATION_RECEIVED, &value);
        value = UINT32_MAX - value < elapsed_seconds ? UINT32_MAX : value + elapsed_seconds;
        (void)store_setting_set_u32(STORE_SETTING_CALL_DURATION_RECEIVED, value);
    } else if (list == STORE_CALL_LIST_DIALLED) {
        value = 0u;
        (void)store_setting_get_u32(STORE_SETTING_CALL_DURATION_DIALLED, &value);
        value = UINT32_MAX - value < elapsed_seconds ? UINT32_MAX : value + elapsed_seconds;
        (void)store_setting_set_u32(STORE_SETTING_CALL_DURATION_DIALLED, value);
    }
}

/* v6.00: the Received-calls register add (ROM 0x256d18) SKIPS the write when
 * the CLI number is empty -- an answered call with no caller ID is NOT logged
 * to Received calls. (Only the Missed add 0x257870 is ungated: it substitutes
 * "(????)" and stores anyway; Dialled always has a number.) The register-add
 * functions are gated per-list on number content, not on a withheld flag
 * (byte-verified 2026-07-08; three adds are the sole callers of the per-list
 * commit 0x256c42). Tag the list unconditionally so duration accounting still
 * runs on hangup even when no record was written (see finalize_call_record). */
static void record_received_call(app_t *app) {
    app->call_record_list = STORE_CALL_LIST_RECEIVED;
    app->call_record_id = 0u;
    if (app->call_number[0] != '\0') {
        (void)store_call_add_now(STORE_CALL_LIST_RECEIVED,
                                 app->call_number,
                                 app->call_name,
                                 0u,
                                 STORE_CALL_REASON_RECEIVED,
                                 &app->call_record_id);
    }
}

static void finalize_call_record(app_t *app, uint32_t now, store_call_reason_t reason) {
    uint32_t elapsed = 0u;
    if (app->call_connected_ms != 0u && time_diff_ms(now, app->call_connected_ms) > 0) {
        elapsed = (uint32_t)time_diff_ms(now, app->call_connected_ms) / 1000u;
    }
    /* CALL.3 / v6.00: the cumulative duration totals + service Life timer
     * accrue for EVERY connected call, independent of whether a recent-call
     * record was written -- a no-CLI answered call is unlogged (empty number)
     * yet still advances the timers (verified 2026-07-08). So gate accounting
     * on elapsed, not on call_record_id. */
    if (elapsed != 0u) {
        call_accounting_add(app->call_record_list, elapsed);
    }
    if (app->call_record_id != 0u) {
        (void)store_call_update_result(app->call_record_list, app->call_record_id, elapsed, reason);
        app->call_record_id = 0u;
    }
    /* Idempotency: accounting now keys off elapsed (not call_record_id), so zero
     * the connect stamp here -- a second finalize before the session reset would
     * otherwise double-count. Every caller resets/reassigns call_connected_ms
     * immediately after, so this has no visible effect on the live timer. */
    app->call_connected_ms = 0u;
}

static void finalize_held_call(app_t *app) {
    /* Same decoupling as finalize_call_record: the held leg's duration accrues
     * into the cumulative counters whether or not it had a register record (a
     * no-CLI held call is unlogged but still connected). */
    if (app->call_held_elapsed_seconds != 0u) {
        call_accounting_add(app->call_held_record_list, app->call_held_elapsed_seconds);
    }
    if (app->call_held_record_id != 0u) {
        (void)store_call_update_result(app->call_held_record_list,
                                       app->call_held_record_id,
                                       app->call_held_elapsed_seconds,
                                       STORE_CALL_REASON_NONE);
        app->call_held_record_id = 0u;
    }
    app->call_held_elapsed_seconds = 0u;
}

static void open_incoming_call(app_t *app, const char *number, const char *name) {
    call_session_reset(app);
    copy_text(app->call_number, sizeof(app->call_number), number);
    copy_text(app->call_name, sizeof(app->call_name), name);
    app->route = APP_ROUTE_INCOMING_CALL;
    app->call_incoming = true;
    app->call_started_ms = time_ms();
    app->call_frame_index = 0u;
    app->backlight_activity_pending = true;
    app->backlight_activity_ms = app->call_started_ms;
    start_incoming_call_tone();
    app->dirty = true;
    LOGI("calls", "incoming open number='%s' name='%s'", app->call_number, app->call_name);
}

static void open_missed_call_notice(app_t *app, uint32_t now) {
    uint8_t count = app->missed_call_pending_count;
    app->missed_call_pending = count > 0u;
    if (count == 0u) {
        app->route = APP_ROUTE_STANDBY;
        app->dirty = true;
        return;
    }
    app->backlight_activity_pending = true;
    app->backlight_activity_ms = now;
    if (count == 1u) {
        open_display_sid(app, 5u, 0x216u, "1\nmissed\ncall", APP_ROUTE_STANDBY, now);
    } else {
        open_display_sid_num(app, 5u, 0x96u, "%N\nmissed\ncalls", (unsigned)count, APP_ROUTE_STANDBY, now);
    }
}

static void note_new_missed_call(app_t *app) {
    if (app->missed_call_pending_count < 255u) {
        app->missed_call_pending_count++;
    }
    app->missed_call_pending = true;
}

static void open_waiting_call(app_t *app, const char *number, const char *name,
                              bool withheld, bool diverted, uint32_t now) {
    copy_text(app->call_waiting_number, sizeof(app->call_waiting_number), number);
    copy_text(app->call_waiting_name, sizeof(app->call_waiting_name), name);
    app->call_waiting_pending = true;
    app->call_waiting_id = 0u;
    app->call_waiting_generation = 0u;
    app->call_waiting_episode = 0u;
    app->call_waiting_withheld = withheld;
    app->call_waiting_diverted = diverted;
    app->call_waiting_recorded = false;
    app->call_options_selected = 0u;
    app->call_volume_visible = false;
    /* CALL.4: a waiting call presents through the original four-variant
     * incoming status table; Answer/Reject live in the Navi options list. */
    app->route = APP_ROUTE_CALL;
    app->backlight_activity_pending = true;
    app->backlight_activity_ms = now;
    app->dirty = true;
    LOGI("calls", "waiting call number='%s' name='%s'", app->call_waiting_number, app->call_waiting_name);
}

static void clear_waiting_call(app_t *app) {
    app->call_waiting_number[0] = '\0';
    app->call_waiting_name[0] = '\0';
    app->call_waiting_pending = false;
    app->call_waiting_id = 0u;
    app->call_waiting_generation = 0u;
    app->call_waiting_episode = 0u;
    app->call_waiting_withheld = false;
    app->call_waiting_diverted = false;
    app->call_waiting_recorded = false;
}

static void note_waiting_missed_call(app_t *app) {
    if (!app->call_waiting_pending || app->call_waiting_recorded) {
        return;
    }
    (void)store_call_add_now(STORE_CALL_LIST_MISSED,
                             app->call_waiting_number,
                             app->call_waiting_name,
                             0u,
                             STORE_CALL_REASON_MISSED,
                             0);
    app->call_waiting_recorded = true;
    note_new_missed_call(app);
}

static bool answer_waiting_call(app_t *app, uint32_t now) {
    if (!app->call_waiting_pending) {
        return true;
    }
    if (!modem_service_request_call_waiting_answer()) {
        LOGW("calls", "waiting answer request rejected by modem queue");
        return true;
    }
    (void)now;
    /* Do NOT establish the two-call optimistically. When the network confirms the
     * answer (the waiting call becomes active, the old active goes on
     * hold -> second_call_held), poll_call_runtime's waiting handler runs
     * establish_two_call_from_waiting. A failed CHLD=2 then leaves the waiting call
     * intact -- no false-missed, no lost active. call_waiting_action_pending
     * suppresses re-opening the waiting UI during the round-trip. */
    app->call_waiting_action_pending = true;
    app->route = APP_ROUTE_CALL;
    app->dirty = true;
    return true;
}

/* Answer-waiting confirmed by the network: the previously-waiting party is now
 * the active call and the old active leg went on hold. Move the old active into
 * the held slot, the (still-captured) waiting party into the active slot, and
 * record the answered call. */
static void establish_two_call_from_waiting(app_t *app, uint32_t now) {
    uint32_t elapsed = 0u;
    if (app->call_connected_ms != 0u && time_diff_ms(now, app->call_connected_ms) > 0) {
        elapsed = (uint32_t)time_diff_ms(now, app->call_connected_ms) / 1000u;
    }
    copy_text(app->call_held_number, sizeof(app->call_held_number), app->call_number);
    copy_text(app->call_held_name, sizeof(app->call_held_name), app->call_name);
    app->call_held_record_list = app->call_record_list;
    app->call_held_record_id = app->call_record_id;
    app->call_held_elapsed_seconds = elapsed;

    copy_text(app->call_number, sizeof(app->call_number), app->call_waiting_number);
    copy_text(app->call_name, sizeof(app->call_name), app->call_waiting_name);
    record_received_call(app);
    clear_waiting_call(app); /* also resets call_waiting_recorded */
    app->call_secondary_active = true;
    app->call_held = true;
    app->call_connected_ms = call_arm_stamp(now);
    app->call_last_second = -1;
}

static bool reject_waiting_call(app_t *app, uint32_t now) {
    if (!app->call_waiting_pending) {
        return true;
    }
    (void)now;
    (void)modem_service_request_call_waiting_reject();
    /* Event-driven: don't note-missed / clear the waiting UI optimistically. When
     * the network releases the waiting call (waiting gone, no held
     * leg), poll_call_runtime's waiting handler logs it missed -- the same path a
     * caller-gave-up takes. A failed CHLD=0 leaves the waiting call intact. */
    app->call_waiting_action_pending = true;
    app->route = APP_ROUTE_CALL;
    app->dirty = true;
    return true;
}

static bool swap_calls(app_t *app, uint32_t now) {
    /* call_secondary_active is the authoritative "a call is held" flag (set at
     * every hold site, cleared on release/reset). Do NOT also gate on a
     * non-empty call_held_number -- a held call with no CLI legitimately has an
     * empty number, and gating on it made Swap a silent no-op for a held
     * private-number call. */
    if (!app->call_secondary_active) {
        return true;
    }
    (void)now;
    (void)modem_service_request_call_swap();
    /* The display swap is driven by the resulting active_call_id flip
     * in poll_call_runtime, NOT optimistically here -- so a failed CHLD=2 leaves
     * the display on the correct (unchanged) party, matching the original 3210's
     * network-driven display. */
    app->route = APP_ROUTE_CALL;
    app->dirty = true;
    return true;
}

/* Swap the active and held display slots (numbers, names, call-log records,
 * per-leg elapsed). Both legs survive; the active/held roles just flip. Driven by
 * the network swap confirmation. (An active leg ENDING -- local "End this call"
 * or a remote hangup -- uses promote_held_leg_to_active instead, driven by the
 * same reconcile.) */
static void swap_call_display_slots(app_t *app, uint32_t now) {
    uint32_t current_elapsed = 0u;
    if (app->call_connected_ms != 0u && time_diff_ms(now, app->call_connected_ms) > 0) {
        current_elapsed = (uint32_t)time_diff_ms(now, app->call_connected_ms) / 1000u;
    }
    char old_number[MODEM_PHONE_MAX + 1u];
    char old_name[PHONEBOOK_NAME_MAX + 1u];
    copy_text(old_number, sizeof(old_number), app->call_number);
    copy_text(old_name, sizeof(old_name), app->call_name);
    store_call_list_t old_list = app->call_record_list;
    uint32_t old_id = app->call_record_id;

    copy_text(app->call_number, sizeof(app->call_number), app->call_held_number);
    copy_text(app->call_name, sizeof(app->call_name), app->call_held_name);
    app->call_record_list = app->call_held_record_list;
    app->call_record_id = app->call_held_record_id;
    app->call_connected_ms = call_arm_stamp(now - app->call_held_elapsed_seconds * 1000u);

    copy_text(app->call_held_number, sizeof(app->call_held_number), old_number);
    copy_text(app->call_held_name, sizeof(app->call_held_name), old_name);
    app->call_held_record_list = old_list;
    app->call_held_record_id = old_id;
    app->call_held_elapsed_seconds = current_elapsed;
    app->call_last_second = -1; /* connected_ms changed -> force a duration redraw */
}

static bool release_active_call(app_t *app, uint32_t now) {
    /* Gate on call_secondary_active alone (see swap_calls): a held no-CLI call
     * has an empty call_held_number, and the old clause made "End this call"
     * hang up the whole session instead of releasing just the active leg. */
    if (!app->call_secondary_active) {
        end_call(app, now, true);
        return true;
    }
    (void)now;
    (void)modem_service_request_call_release_active();
    /* URC-driven: the display promote (finalize the ended active call, swap the
     * held party into view) is done by poll_call_runtime's reconcile when the
     * network confirms (active_call_id changes, second_call_held -> false) -- the
     * same path a REMOTE active-party hangup takes. A failed CHLD=1 leaves both
     * calls as-is instead of finalizing a still-live call's record. */
    app->route = APP_ROUTE_CALL;
    app->dirty = true;
    return true;
}

static bool release_active_with_waiting(app_t *app, uint32_t now) {
    if (app->call_waiting_release_pending) {
        return true;
    }
    modem_status_t status;
    modem_service_get_status(&status);
    if (!status.waiting_call || status.active_call_id == 0u) {
        LOGW("calls", "cannot release foreground with waiting: waiting=%u id=%u",
             status.waiting_call ? 1u : 0u, (unsigned)status.active_call_id);
        return true;
    }
    if (!modem_service_request_call_release_leg(status.active_call_id)) {
        LOGW("calls", "targeted active release rejected for id=%u",
             (unsigned)status.active_call_id);
        return true;
    }
    app->call_waiting_release_pending = true;
    app->call_waiting_release_active_id = status.active_call_id;
    app->call_waiting_release_retry_ms = now + CALL_WAITING_RELEASE_RETRY_MS;
    app->route = APP_ROUTE_CALL;
    app->dirty = true;
    return true;
}

static bool cancel_new_call_setup(app_t *app, uint32_t now) {
    if (app->call_phase != CALL_PHASE_SETUP || !app->call_secondary_active) {
        return false;
    }

    uint8_t survivor_id = app->call_new_active_ref;
    modem_status_t status;
    modem_service_get_status(&status);

    /* B can become ACTIVE between the last UI poll and the cancel key. Once
     * that happens the pending-DIAL tracker has legitimately retired, so give
     * the now-confirmed B id its own release transaction before acknowledging
     * the New-call attempt. This is still an id-scoped cancel; A is never sent
     * through the session-wide hangup path. */
    if (survivor_id != 0u &&
        status.call_state == MODEM_CALL_ACTIVE &&
        status.active_call_id != 0u &&
        status.active_call_id != survivor_id) {
        if (!modem_service_request_call_release_leg(status.active_call_id)) {
            LOGW("calls", "New-call cancel release rejected for id=%u",
                 (unsigned)status.active_call_id);
            return true;
        }
    }

    stop_call_tone();
    modem_service_new_call_abandoned();
    promote_held_leg_to_active(app, now, STORE_CALL_REASON_NONE);
    app->call_phase = CALL_PHASE_CONNECTED;
    app->call_new_active_ref = 0u;
    app->call_result_armed = true;
    app->call_pending_failure_ms = 0u;
    app->call_pending_failure_result = MODEM_CALL_RESULT_NONE;

    /* A may already be HELD, may be reported HELD after B's release, or may be
     * promoted by the network without our help. Keep one id-scoped
     * intent through the settle window; poll_call_runtime sends a retrieve only
     * after this exact id is positively observed as the sole held survivor. */
    if (survivor_id != 0u) {
        app->call_survivor_retrieve_pending = true;
        app->call_survivor_retrieve_id = survivor_id;
        app->call_survivor_retrieve_ms =
            now + CALL_SURVIVOR_RETRIEVE_SETTLE_MS;
    }
    apply_call_volume_gain(app->call_volume_level);
    app->route = APP_ROUTE_CALL;
    app->dirty = true;
    LOGI("calls", "New-call setup cancelled; preserving survivor id=%u",
         (unsigned)survivor_id);
    return true;
}

static void handoff_waiting_to_incoming(app_t *app, const modem_status_t *status,
                                        uint32_t now) {
    char number[MODEM_PHONE_MAX + 1u];
    char name[PHONEBOOK_NAME_MAX + 1u];
    bool withheld = app->call_waiting_withheld;
    bool diverted = app->call_waiting_diverted;
    copy_text(number, sizeof(number), app->call_waiting_number);
    copy_text(name, sizeof(name), app->call_waiting_name);
    finalize_call_record(app, now, STORE_CALL_REASON_NONE);
    open_incoming_call(app, number, name);
    app->call_incoming_withheld = withheld || status->caller_id_withheld;
    app->call_incoming_diverted = diverted || status->incoming_diverted;
}

static void handoff_waiting_to_connected(app_t *app, const modem_status_t *status,
                                         uint32_t now) {
    char number[MODEM_PHONE_MAX + 1u];
    char name[PHONEBOOK_NAME_MAX + 1u];
    bool withheld = app->call_waiting_withheld;
    bool diverted = app->call_waiting_diverted;
    copy_text(number, sizeof(number), app->call_waiting_number);
    copy_text(name, sizeof(name), app->call_waiting_name);
    finalize_call_record(app, now, STORE_CALL_REASON_NONE);
    call_session_reset(app);
    copy_text(app->call_number, sizeof(app->call_number), number);
    copy_text(app->call_name, sizeof(app->call_name), name);
    app->call_incoming = true;
    app->call_incoming_withheld = withheld || status->caller_id_withheld;
    app->call_incoming_diverted = diverted || status->incoming_diverted;
    app->call_started_ms = now;
    connect_call(app, now);
    record_received_call(app);
    app->call_incoming_recorded = true;
    LOGI("calls", "waiting caller promoted directly to active id=%u",
         (unsigned)status->active_call_id);
}

/* The active leg ended (locally via AT+CHLD=1 "End this call", or remotely -- the
 * active party hung up during a two-call and the network promoted the held leg).
 * Finalize the ended active call's record and move the held party into view as
 * the now-active call. Caller sets route/dirty. */
static void promote_held_leg_to_active(app_t *app, uint32_t now, store_call_reason_t reason) {
    /* `reason` finalizes the ended primary leg's log record: STORE_CALL_REASON_NONE
     * for a normally-connected leg being released, or the actual failure reason when a
     * failed New-call dial (B) is rolled back (see rollback_failed_new_call). */
    finalize_call_record(app, now, reason);
    copy_text(app->call_number, sizeof(app->call_number), app->call_held_number);
    copy_text(app->call_name, sizeof(app->call_name), app->call_held_name);
    app->call_record_list = app->call_held_record_list;
    app->call_record_id = app->call_held_record_id;
    app->call_connected_ms = call_arm_stamp(now - app->call_held_elapsed_seconds * 1000u);
    app->call_held_number[0] = '\0';
    app->call_held_name[0] = '\0';
    app->call_held_record_id = 0u;
    app->call_held_elapsed_seconds = 0u;
    app->call_secondary_active = false;
    app->call_survivor_retrieve_pending = false;
    app->call_survivor_retrieve_id = 0u;
    app->call_survivor_retrieve_ms = 0u;
    app->call_held = false;
}

/* A New-call (2nd MO) dial failed but the original call survives (the modem restored
 * it + latched status.second_call_result). Everything connect_call would do, since we
 * bypass it: stop the looping ringback tone, log B's actual failure reason, promote
 * the held original back to the single active call (elapsed preserved), reapply the
 * user's call gain, and mark the result armed so a later hangup shows a result. */
static void rollback_failed_new_call(app_t *app, modem_call_result_t result,
                                     const modem_status_t *status, uint32_t now) {
    stop_call_tone();
    /* Tell the service the New-call was abandoned: an app-only revert would leave the
     * modem tracking a pending 2nd leg whose (possibly-lost) outcome could later
     * misclassify a coarse teardown of the surviving call. */
    modem_service_new_call_abandoned();
    /* network_registered must reflect the actual state: a registration-loss NO_CARRIER
     * maps to NO_NETWORK only when !registered (else UNREACHABLE). */
    promote_held_leg_to_active(app, now,
        call_reason_for_result(result, status->network_registered));
    app->call_phase = CALL_PHASE_CONNECTED;
    app->call_new_active_ref = 0u;
    app->call_result_armed = true;
    if (status->call_state == MODEM_CALL_ACTIVE &&
        status->call_on_hold && status->active_call_id != 0u) {
        /* The failed second MO left the original network-held. Use the same
         * delayed, waiting-aware retrieve path as an active-leg release. */
        app->call_survivor_retrieve_pending = true;
        app->call_survivor_retrieve_id = status->active_call_id;
        app->call_survivor_retrieve_ms =
            now + CALL_SURVIVOR_RETRIEVE_SETTLE_MS;
    }
    apply_call_volume_gain(app->call_volume_level);
    app->dirty = true;
}

static void open_call_options(app_t *app) {
    const char *labels[CALL_OPTION_VISIBLE_MAX];
    call_option_t options[CALL_OPTION_VISIBLE_MAX];
    uint8_t count = call_options_build(app, options, labels);
    if (app->call_options_selected >= count) {
        app->call_options_selected = 0u;
    }
    app->call_volume_visible = false;
    app->route = APP_ROUTE_CALL_OPTIONS;
    app->dirty = true;
}

static bool answer_incoming_call(app_t *app, uint32_t now) {
    if (app->call_answer_pending) {
        /* Repeat answer inside the ~1 s ATA round-trip (NAVI double-press, or
         * NAVI + the headset hook -- both funnel here while the route stays
         * INCOMING_CALL). A second queued ATA fires after the call goes
         * ACTIVE, gets ERROR/NO CARRIER, and forces call_state back to IDLE:
         * the app then finalizes and exits while the network call is still
         * connected, with no UI left to hang it up. Swallow the repeat. */
        return true;
    }
    if (!modem_service_request_answer()) {
        LOGW("calls", "incoming answer request rejected by modem queue");
        return true;
    }
    /* Stop the ring but do NOT set call_incoming_silenced: "Silent" (SID
     * 0x02ab) belongs solely to the Up/Down silence action, and the screen
     * stays on the incoming route through the ~1 s ATA round-trip -- setting
     * it here flashed "Silent" under the caller detail while answering. The
     * tone cannot restart meanwhile (start site is only open_incoming_call). */
    stop_call_tone();
    app->call_answer_pending = true;
    app->call_result_armed = true;
    app->call_started_ms = now;
    app->dirty = true;
    LOGI("calls", "incoming answer");
    return true;
}

static bool reject_incoming_call(app_t *app, uint32_t now) {
    if (!app->call_incoming_recorded) {
        (void)store_call_add_now(STORE_CALL_LIST_MISSED,
                                 app->call_number,
                                 app->call_name,
                                 0u,
                                 STORE_CALL_REASON_MISSED,
                                 &app->call_record_id);
        app->call_record_list = STORE_CALL_LIST_MISSED;
        app->call_incoming_recorded = true;
        note_new_missed_call(app);
    }
    stop_call_tone();
    (void)modem_service_request_hangup();
    open_missed_call_notice(app, now);
    call_session_reset(app);
    app->call_pending_local_hangup = true;
    LOGI("calls", "incoming reject");
    return true;
}

static void open_call_result_display(app_t *app, modem_call_result_t result, bool network_registered, uint32_t now) {
    if (result == MODEM_CALL_RESULT_BUSY) {
        open_display_sid(app, CALL_RESULT_RECORD_ID, 0x204u, "Number\nbusy", APP_ROUTE_STANDBY, now);
    } else if (result == MODEM_CALL_RESULT_NO_ANSWER) {
        open_display_sid(app, CALL_RESULT_RECORD_ID, 0x1f5u, "No answer", APP_ROUTE_STANDBY, now);
    } else if (result == MODEM_CALL_RESULT_NO_DIALTONE ||
               (result == MODEM_CALL_RESULT_NO_CARRIER && !network_registered)) {
        open_display_sid(app, CALL_RESULT_RECORD_ID, 0x1f6u, "No network\ncoverage", APP_ROUTE_STANDBY, now);
    } else {
        open_display_sid(app, CALL_RESULT_RECORD_ID, 0x1f4u, "Error in\nconnection", APP_ROUTE_STANDBY, now);
    }
}

static store_call_reason_t call_reason_for_result(modem_call_result_t result, bool network_registered) {
    if (result == MODEM_CALL_RESULT_BUSY) {
        return STORE_CALL_REASON_BUSY;
    }
    if (result == MODEM_CALL_RESULT_NO_ANSWER) {
        return STORE_CALL_REASON_NO_ANSWER;
    }
    if (result == MODEM_CALL_RESULT_NO_DIALTONE ||
        (result == MODEM_CALL_RESULT_NO_CARRIER && !network_registered)) {
        return STORE_CALL_REASON_NO_NETWORK;
    }
    if (result == MODEM_CALL_RESULT_NO_CARRIER) {
        return STORE_CALL_REASON_UNREACHABLE;
    }
    return STORE_CALL_REASON_NONE;
}

static void start_call_progress_tone(void) {
    uint8_t active = profile_active_index();
    uint8_t volume = profile_get_tone_setting(active, PROFILE_SETTING_RINGING_VOLUME);
    uint8_t level = audio_level_from_ringing_volume(volume);
    if (level == AUDIO_LEVEL_SILENT) {
        return;
    }
    core1_post_command(CORE1_CMD_AUDIO_SYSTEM_TONE_QUIET_LOOP,
                       audio_arg(CALL_RINGBACK_TONE_INDEX, level));
}

static void start_incoming_call_tone(void) {
    uint8_t active = profile_active_index();
    uint8_t alert = profile_get_tone_setting(active, PROFILE_SETTING_INCOMING_ALERT);
    uint8_t ringtone_value = profile_get_tone_setting(active, PROFILE_SETTING_RINGING_TONE);
    uint8_t volume = profile_get_tone_setting(active, PROFILE_SETTING_RINGING_VOLUME);
    uint8_t vibra = profile_get_tone_setting(active, PROFILE_SETTING_VIBRATING_ALERT);
    uint8_t level = audio_level_from_ringing_volume(volume);

    if (alert == 4u || level == AUDIO_LEVEL_SILENT) {
        if (vibra != 0u) {
            core1_post_command(CORE1_CMD_AUDIO_VIBRA_PULSE_LOOP, vibra_pulse_arg());
        }
        return;
    }
    if (alert == 2u) {
        if (vibra != 0u) {
            core1_post_command(CORE1_CMD_AUDIO_VIBRA_PULSE, vibra_pulse_arg());
        }
        core1_post_command(CORE1_CMD_AUDIO_SYSTEM_TONE, audio_arg(21u, level));
        return;
    }

    if (ringtone_value == 19u) {
        /* "Own tone" (Composer melody): value 19 is a sentinel not present in the
         * generated ringtone table, so play the stored packed melody through the
         * composer engine -- looped for the call like a built-in ring. 1:1 with
         * the original (value 19 rings via the same tone engine); the port
         * previously fell through ringtone_by_value()==NULL and rang silent.
         * Packed sequences carry no embedded vibra markers, so drive vibra with
         * the generic pulse rather than the stream marker gate. */
        static store_own_tone_t own_tone;
        if (store_own_tone_get(0u, &own_tone) != STORE_STATUS_OK ||
            !own_tone.used || own_tone.packed_len == 0u) {
            return;
        }
        bool ring_once = (alert == 5u);
        if (vibra != 0u) {
            core1_post_command(ring_once ? CORE1_CMD_AUDIO_VIBRA_PULSE
                                         : CORE1_CMD_AUDIO_VIBRA_PULSE_LOOP,
                               vibra_pulse_arg());
        }
        if (ring_once) {
            core1_post_audio_composer_packed(own_tone.packed, own_tone.packed_len, level);
        } else {
            core1_post_audio_composer_packed_loop(own_tone.packed, own_tone.packed_len, level);
        }
        return;
    }

    const ringtone_t *ringtone = ringtone_by_value(ringtone_value);
    if (ringtone == 0) {
        return;
    }
    /* Alert mode: 5 = Ring once (one-shot), else Ringing/Ascending (looped).
     * The configured Level 1..5 is passed unchanged to the active-ring path;
     * v6.00's mode-0xf2 Level-5 preview staging is a separate concern. The
     * bytecode-marker vibra gate travels in this same command so stream
     * replacement is atomic on core1. */
    uint16_t ring_cmd = CORE1_CMD_AUDIO_RINGTONE_LOOP;
    if (alert == 5u) {
        ring_cmd = CORE1_CMD_AUDIO_RINGTONE_PREVIEW;
    }
    core1_post_command(ring_cmd,
                       audio_arg_with_marker_vibra(ringtone->index, level, vibra != 0u));
}

static void stop_call_tone(void) {
    core1_post_command(CORE1_CMD_AUDIO_STOP, 0u);
}

static uint16_t vibra_pulse_arg(void) {
    return (uint16_t)(CALL_VIBRA_PULSE_TICKS | ((uint16_t)AUDIO_VIBRA_STRENGTH_STOCK << 8u));
}

static void render_call_volume(const app_t *app, framebuffer_t *fb) {
    const font_t *font = asset_font(FONT_FS2);
    uint8_t level = app->call_volume_level;
    if (level < CALL_VOLUME_MIN) {
        level = CALL_VOLUME_MIN;
    } else if (level > CALL_VOLUME_MAX) {
        level = CALL_VOLUME_MAX;
    }
    fb_text(fb, font, ts_or(0x3fbu, "Volume"), 2, 4, true, 44);
    int x = 7;
    const int bottom = 37;
    for (uint8_t i = 0; i < CALL_VOLUME_MAX; i++) {
        uint16_t bitmap_id = (uint16_t)((i < level ? 0x00f1u : 0x00fdu) + i);
        const bitmap_t *bitmap = asset_bitmap(bitmap_id);
        if (bitmap != 0) {
            fb_bitmap(fb, bitmap_id, x, bottom - bitmap->height, true, false);
            x += bitmap->width + 1;
        } else {
            int height = 8 + i * 2;
            fb_rect(fb, x, bottom - height, 5, height, true);
            if (i < level) {
                fb_fill_rect(fb, x + 1, bottom - height + 1, 3, height - 2, true);
            }
            x += 6;
        }
    }
}

static void apply_call_volume_gain(uint8_t level) {
    if (level < CALL_VOLUME_MIN) {
        level = CALL_VOLUME_MIN;
    } else if (level > CALL_VOLUME_MAX) {
        level = CALL_VOLUME_MAX;
    }
    /* calls_app owns the persisted Nokia 1..10 value; the audio layer owns
     * route-specific attenuation and codec register encoding. */
    core1_services_codec_set_call_volume(level);
}

static void show_call_volume(app_t *app, int8_t delta, uint32_t now) {
    uint8_t level = app->call_volume_level;
    if (level < CALL_VOLUME_MIN || level > CALL_VOLUME_MAX) {
        level = 5u;
    }
    if (delta > 0 && level < CALL_VOLUME_MAX) {
        level++;
    } else if (delta < 0 && level > CALL_VOLUME_MIN) {
        level--;
    }
    app->call_volume_level = level;
    apply_call_volume_gain(level); /* M4: actually change the earpiece gain */
    /* Persist across power cycles (user-confirmed original behavior). The store
     * commit is deferred while call audio is active, so the flash write lands
     * after the call ends -- no flash pause mid-call. */
    (void)store_setting_set_u8(STORE_SETTING_CALL_VOLUME, level);
    app->call_volume_visible = true;
    app->call_volume_until_ms = now + CALL_VOLUME_TIMEOUT_MS;
    app->dirty = true;
}

static uint8_t call_options_build(const app_t *app, call_option_t *options, const char **labels) {
    /* 1:1 with v6.00 (RE'd 2026-07-11, HIGH conf -- candidate assembly 0x2988dc,
     * transform 0x2984c8 ported instruction-by-instruction, builder 0x298750).
     * The transform yields a per-situation item mask G; the builder emits one
     * item per set bit in ascending bit order. The masks below are the exact G
     * values the ROM produces per call state (each decodes to the item list in
     * the trailing comment). Notable 1:1 points the old ad-hoc list got wrong:
     * single-active offers "End all calls" NOT "End this call" and no "Send";
     * "New call" is absent whenever both an active AND a held call exist;
     * Hold/Unhold/Swap follow the foreground ACTIVE/HELD roles. The ROM's
     * waiting overlay can coexist with Hold or Unhold; the pure policy applies
     * the backend safety filter after selecting this exact ROM mask. */
    static const struct {
        uint16_t bit;
        uint16_t sid;
        call_option_t option;
        const char *fallback;
    } ITEMS[] = {
        {CALL_OPT_HOLD,       0x188u, CALL_OPTION_HOLD,           "Hold"},
        {CALL_OPT_UNHOLD,     0x3e3u, CALL_OPTION_HOLD,           "Unhold"}, /* same vendor hold toggle */
        {CALL_OPT_NEW_CALL,   0x1dau, CALL_OPTION_NEW_CALL,       "New call"},
        {CALL_OPT_END_THIS,   0x133u, CALL_OPTION_END_THIS_CALL,  "End this call"},
        {CALL_OPT_SWAP,       0x3b5u, CALL_OPTION_SWAP,           "Swap"},
        {CALL_OPT_ANSWER,     0x054u, CALL_OPTION_ANSWER_WAITING, "Answer"},
        {CALL_OPT_REJECT,     0x25cu, CALL_OPTION_REJECT_WAITING, "Reject"},
        {CALL_OPT_SEND_DTMF,  0x2a3u, CALL_OPTION_SEND_DTMF,      "Send DTMF"},
        {CALL_OPT_END_ALL,    0x13eu, CALL_OPTION_END_ALL,        "End all calls"},
        {CALL_OPT_PHONE_BOOK, 0x2ceu, CALL_OPTION_PHONE_BOOK,     "Phone book"},
    };

    /* Situation from the clone's call flags, mapped to the ROM's state5=ACTIVE /
     * state6=HELD presence: single active -> (active,!held); single held ->
     * (!active,held); two-call -> (active,held). call_waiting_pending is the
     * ROM's waiting overlay (byte 0x10e899, a 2nd/3rd incoming call ringing). */
    bool connected = app->call_phase == CALL_PHASE_CONNECTED;
    bool active = connected && (app->call_secondary_active || !app->call_held);
    bool held = connected && app->call_held;
    bool waiting = app->call_waiting_pending;

    uint16_t g = call_options_mask_for_state(
        active, held, waiting, modem_service_call_hold_available());

    uint8_t count = 0u;
    for (size_t i = 0; i < sizeof(ITEMS) / sizeof(ITEMS[0]) &&
                       count < CALL_OPTION_VISIBLE_MAX;
         i++) {
        if (g & ITEMS[i].bit) {
            options[count] = ITEMS[i].option;
            labels[count++] = ts_or(ITEMS[i].sid, ITEMS[i].fallback);
        }
    }
    return count;
}

static bool route_is_call_surface(const app_t *app) {
    if (app->route == APP_ROUTE_CALL || app->route == APP_ROUTE_CALL_OPTIONS) {
        return true;
    }
    if (app->route == APP_ROUTE_EDITOR &&
        (app->editor_context == EDITOR_CONTEXT_IN_CALL_NEW_CALL ||
         app->editor_context == EDITOR_CONTEXT_IN_CALL_DTMF)) {
        return true;
    }
    if (app->route == APP_ROUTE_PHONEBOOK_LIST &&
        (app->phonebook_context == PHONEBOOK_CONTEXT_IN_CALL ||
         app->phonebook_context == PHONEBOOK_CONTEXT_IN_CALL_NEW_CALL)) {
        return true;
    }
    return false;
}

static bool speed_dial_entry_for_digit(char digit, phonebook_entry_t *entry) {
    if (digit < '2' || digit > '9' || entry == 0) {
        return false;
    }
    uint32_t contact_index = STORE_SPEED_DIAL_EMPTY;
    if (store_phonebook_get_speed_dial((uint8_t)(digit - '0'), &contact_index) != STORE_STATUS_OK ||
        contact_index == STORE_SPEED_DIAL_EMPTY) {
        return false;
    }
    uint16_t count = phonebook_service_count();
    for (uint16_t i = 0u; i < count; i++) {
        if (phonebook_service_entry(i, entry) && entry->index == contact_index && entry->number[0] != '\0') {
            return true;
        }
    }
    return false;
}

static char dtmf_char_for_key(uint16_t key) {
    switch (key) {
    case KEY_1: return '1';
    case KEY_2: return '2';
    case KEY_3: return '3';
    case KEY_4: return '4';
    case KEY_5: return '5';
    case KEY_6: return '6';
    case KEY_7: return '7';
    case KEY_8: return '8';
    case KEY_9: return '9';
    case KEY_STAR: return '*';
    case KEY_0: return '0';
    case KEY_HASH: return '#';
    default: return '\0';
    }
}

/* Reserve 0 as the "disarmed" sentinel for the call-control ms stamps
 * (call_pending_failure_ms, call_connected_ms): if now + delay (or a held-time
 * back-date) lands exactly on 0, the armed timer would read as disarmed. Clamp
 * to 1 (a 1 ms error, invisible) so the sentinel is unambiguous. */
static uint32_t call_arm_stamp(uint32_t stamp_ms) {
    return stamp_ms == 0u ? 1u : stamp_ms;
}

static void format_call_duration(uint32_t elapsed_seconds, char *dst, size_t cap) {
    /* Binary-pinned: the in-call timer formatter 0x0027a92c always formats
     * all three fields (hours halfword, minutes/seconds bytes) and the
     * per-field number formatter 0x002a3d74 zero-pads to a minimum of 2
     * digits (threshold table 0x002e1b84 [10,100,1000,...], leading '0'
     * pre-written). So the duration is always "HH:MM:SS" zero-padded, with
     * the hours field always shown — e.g. a 35 s call reads "00:00:35". */
    uint32_t hours = elapsed_seconds / 3600u;
    uint32_t minutes = (elapsed_seconds / 60u) % 60u;
    uint32_t seconds = elapsed_seconds % 60u;
    snprintf(dst, cap, "%02lu:%02lu:%02lu",
             (unsigned long)hours, (unsigned long)minutes, (unsigned long)seconds);
}

void call_session_reset(app_t *app) {
    stop_call_tone();
    call_session_clear_fields(app);
}

static void call_session_clear_fields(app_t *app) {
    app->call_number[0] = '\0';
    app->call_name[0] = '\0';
    app->call_waiting_number[0] = '\0';
    app->call_waiting_name[0] = '\0';
    app->call_held_number[0] = '\0';
    app->call_held_name[0] = '\0';
    app->call_phase = CALL_PHASE_SETUP;
    app->call_incoming = false;
    app->call_answer_pending = false;
    app->call_incoming_recorded = false;
    app->call_incoming_silenced = false;
    app->call_incoming_withheld = false;
    app->call_incoming_diverted = false;
    app->call_waiting_pending = false;
    app->call_waiting_id = 0u;
    app->call_waiting_generation = 0u;
    app->call_waiting_episode = 0u;
    app->call_waiting_withheld = false;
    app->call_waiting_diverted = false;
    app->call_waiting_recorded = false;
    app->call_waiting_action_pending = false;
    app->call_waiting_release_pending = false;
    app->call_waiting_release_active_id = 0u;
    app->call_waiting_release_retry_ms = 0u;
    app->call_pending_local_hangup = false;
    app->call_result_armed = false;
    app->call_held = false;
    app->call_secondary_active = false;
    app->call_survivor_retrieve_pending = false;
    app->call_survivor_retrieve_id = 0u;
    app->call_survivor_retrieve_ms = 0u;
    app->call_new_active_ref = 0u;
    app->call_record_list = STORE_CALL_LIST_DIALLED;
    app->call_held_record_list = STORE_CALL_LIST_DIALLED;
    app->call_record_id = 0u;
    app->call_held_record_id = 0u;
    app->call_held_elapsed_seconds = 0u;
    app->call_started_ms = 0u;
    app->call_connected_ms = 0u;
    app->call_active_seen_ms = 0u;
    app->call_pending_failure_ms = 0u;
    app->call_pending_failure_result = MODEM_CALL_RESULT_NONE;
    app->call_frame_index = 0u;
    app->call_options_selected = 0u;
    app->call_volume_visible = false;
    app->call_volume_until_ms = 0u;
    app->call_last_second = -1;
}

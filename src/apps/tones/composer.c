#include "apps/tones_app.h"
#include "tones_internal.h"
#include "audio/composer_codec.h"
#include "audio/ringtone_codec.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "audio/audio_levels.h"
#include "apps/dialogs_app.h"
#include "services/core1_services.h"
#include "services/input_keys.h"
#include "services/modem_service.h"
#include "services/strings.h"
#include "services/timebase.h"
#include "storage/store_service.h"

typedef struct {
    const char *label;
    uint16_t sid;
} tones_label_t;

typedef struct {
    const char *label;
    uint8_t value;
    uint16_t sid; /* localized display copy only; .label stays the BPM parse key */
} composer_tempo_option_t;

typedef struct {
    char duration[3];
    bool dotted;
    bool sharp;
    char note;
    uint8_t octave;
} composer_token_t;

typedef struct {
    char text[32];
    uint16_t start;
    uint16_t end;
} composer_line_t;

typedef struct {
    uint16_t cursor_char;
    uint8_t cursor_line;
    bool cursor_found;
    uint8_t capture_start;
    composer_line_t *captured;
    uint8_t capture_cap;
    uint8_t captured_count;
} composer_layout_t;

typedef enum {
    COMPOSER_EDIT_UNCHANGED = 0,
    COMPOSER_EDIT_APPLIED,
    COMPOSER_EDIT_FULL,
} composer_edit_result_t;
/* The Composer options list (screen 9-3-*): each entry resolves to its exact
 * SID from the v6.00 UI traces. The English .label stays the option-index key. */
static const tones_label_t COMPOSER_OPTION_LABELS[] = {
    {"Play", 0x0f8u},         /* ROM 248 */
    {"Save", 0x0f9u},         /* ROM 249 */
    {"Tempo", 0x0fcu},        /* ROM 252 */
    {"Send", 0x0fbu},
    {"Clear screen", 0x0e4u}, /* ROM 228 */
    {"Exit", 0x0e5u},         /* ROM 229 */
};

/* .value = tempo bytecode; .sid = the localized "NN BPM" display copy. */
static const composer_tempo_option_t COMPOSER_TEMPO_OPTIONS[] = {
    {"40 BPM", 0x04u, 0x0f0u},
    {"45 BPM", 0x05u, 0x0f1u},
    {"50 BPM", 0x06u, 0x0f2u},
    {"56 BPM", 0x07u, 0x0f3u},
    {"63 BPM", 0x08u, 0x0f4u},
    {"70 BPM", 0x09u, 0x0f5u},
    {"80 BPM", 0x0au, 0x0f6u},
    {"90 BPM", 0x0bu, 0x0f7u},
    {"100 BPM", 0x0cu, 0x0e8u},
    {"112 BPM", 0x0du, 0x0e9u},
    {"125 BPM", 0x0eu, 0x0eau},
    {"140 BPM", 0x0fu, 0x0ebu},
    {"160 BPM", 0x10u, 0x0ecu},
    {"180 BPM", 0x11u, 0x0edu},
    {"200 BPM", 0x12u, 0x0eeu},
    {"225 BPM", 0x13u, 0x0efu},
};

static const char *const COMPOSER_DURATIONS[] = {"1", "2", "4", "8", "16", "32"};

#define COMPOSER_DEFAULT_DURATION_INDEX 2u
#define COMPOSER_DEFAULT_TEMPO_INDEX 8u
#define COMPOSER_DEFAULT_OCTAVE 1u
#define COMPOSER_TOKEN_MAX 8u
#define COMPOSER_MAX_TOKENS 96u
#define COMPOSER_CURSOR_BLINK_MS 512u
#define COMPOSER_PREVIEW_MS 180u
#define COMPOSER_SEND_TIMEOUT_MS 180000u

/* Composer actions are serialized by the core-0 app router. Reuse one token /
 * wire-event workspace instead of nesting 1-2 KiB automatic arrays while
 * editing, saving, or starting playback on a 4 KiB stack. */
typedef struct {
    char tokens[COMPOSER_MAX_TOKENS][COMPOSER_TOKEN_MAX];
    composer_note_event_t events[COMPOSER_MAX_TOKENS];
    store_own_tone_t tone;
} composer_scratch_t;

static composer_scratch_t s_composer_scratch;

static void composer_return_to_tones_menu(app_t *app);
static void composer_stop_audio(app_t *app);
static void composer_load_own_tone(app_t *app, uint32_t now);
static bool composer_split_tokens(const char *notes, char tokens[][COMPOSER_TOKEN_MAX], uint8_t *out_count);
static bool composer_join_tokens(char tokens[][COMPOSER_TOKEN_MAX], uint8_t count, char *out, size_t out_cap);
static uint8_t composer_token_count(const app_t *app);
static uint8_t composer_clamp_cursor(const app_t *app, uint8_t count);
static bool composer_parse_token(const char *token, composer_token_t *out);
static void composer_format_token(const composer_token_t *parts, char *out, size_t cap);
static bool composer_note_from_key(uint16_t key, char *out_note);
static bool composer_note_sharpable(char note);
static composer_edit_result_t composer_insert_note(app_t *app, char note, uint32_t now);
static bool composer_delete_previous(app_t *app);
static void composer_step_duration(app_t *app, int8_t delta);
static void composer_step_octave(app_t *app);
static composer_edit_result_t composer_toggle_sharp(app_t *app);
static composer_edit_result_t composer_toggle_dot(app_t *app);
static void composer_show_full(app_t *app, uint32_t now);
static void composer_move_cursor(app_t *app, int8_t delta);
static uint8_t composer_pitch_byte(const composer_token_t *parts);
static uint8_t composer_pitch_code(const composer_token_t *parts);
static uint8_t composer_duration_code(const composer_token_t *parts);
static uint16_t composer_note_duration_ms(const app_t *app, const composer_token_t *parts);
static uint16_t composer_cursor_char_index(const app_t *app);
static uint8_t composer_layout_lines(const app_t *app, composer_layout_t *layout);
static void composer_post_note(const app_t *app, const composer_token_t *parts);
static void composer_start_preview(app_t *app, const composer_token_t *parts, uint32_t now);
static void composer_start_playback(app_t *app, uint32_t now);
static void composer_play_next(app_t *app, uint32_t now);
static bool composer_build_packed_tone(const char *name,
                                       const char *notes,
                                       uint8_t tempo_index,
                                       uint8_t *dst,
                                       uint16_t cap,
                                       uint16_t *out_len);
static bool composer_store_current_own_tone(app_t *app);
void open_tone_composer(app_t *app, uint32_t now) {
    composer_stop_audio(app);
    composer_load_own_tone(app, now);
    app->route = APP_ROUTE_TONE_COMPOSER;
    app->dirty = true;
}

bool handle_tone_composer_key(app_t *app, uint16_t key, event_type_t event_type, uint32_t now) {
    if (event_type == EVENT_KEY_HOLD) {
        char note;
        if (composer_note_from_key(key, &note) && note != '-' &&
            app->tone_composer_hold_key == key && !app->tone_composer_hold_consumed) {
            if (composer_toggle_dot(app) == COMPOSER_EDIT_FULL) {
                composer_show_full(app, now);
            }
            app->tone_composer_hold_consumed = true;
            app->dirty = true;
            return true;
        }
        return true;
    }

    app->tone_composer_hold_key = 0u;
    app->tone_composer_hold_consumed = false;

    if (key == KEY_NAVI) {
        composer_stop_audio(app);
        app->tone_composer_option_index = 0u;
        app->route = APP_ROUTE_TONE_COMPOSER_OPTIONS;
        app->dirty = true;
        return true;
    }
    if (key == KEY_C) {
        composer_stop_audio(app);
        if (!composer_delete_previous(app)) {
            composer_return_to_tones_menu(app);
        }
        app->dirty = true;
        return true;
    }
    if (key == KEY_UP) {
        composer_move_cursor(app, -1);
        return true;
    }
    if (key == KEY_DOWN) {
        composer_move_cursor(app, 1);
        return true;
    }
    if (key == KEY_8) {
        composer_step_duration(app, 1);
        return true;
    }
    if (key == KEY_9) {
        composer_step_duration(app, -1);
        return true;
    }
    if (key == KEY_STAR) {
        composer_step_octave(app);
        return true;
    }
    if (key == KEY_HASH) {
        if (composer_toggle_sharp(app) == COMPOSER_EDIT_FULL) {
            composer_show_full(app, now);
        }
        return true;
    }
    char note;
    if (composer_note_from_key(key, &note)) {
        composer_edit_result_t result = composer_insert_note(app, note, now);
        if (result == COMPOSER_EDIT_FULL) {
            composer_show_full(app, now);
        } else if (result == COMPOSER_EDIT_APPLIED) {
            app->tone_composer_hold_key = key;
            app->tone_composer_hold_consumed = false;
        }
        return true;
    }
    return true;
}

bool handle_tone_composer_options_key(app_t *app, uint16_t key, uint32_t now) {
    if (key == KEY_C) {
        app->route = APP_ROUTE_TONE_COMPOSER;
        app->dirty = true;
        return true;
    }
    if (key == KEY_UP) {
        uint8_t count = (uint8_t)ARRAY_COUNT(COMPOSER_OPTION_LABELS);
        app->tone_composer_option_index = app->tone_composer_option_index == 0u
            ? (uint8_t)(count - 1u)
            : (uint8_t)(app->tone_composer_option_index - 1u);
        app->dirty = true;
        return true;
    }
    if (key == KEY_DOWN) {
        app->tone_composer_option_index = (uint8_t)((app->tone_composer_option_index + 1u) %
                                                    ARRAY_COUNT(COMPOSER_OPTION_LABELS));
        app->dirty = true;
        return true;
    }
    if (key != KEY_NAVI) {
        return true;
    }

    switch (app->tone_composer_option_index) {
    case 0:
        if (composer_token_count(app) >= 1u) {
            composer_start_playback(app, now);
            open_display(app,
                         3u,
                         "Playing tone",
                         app->tone_composer_name[0] ? app->tone_composer_name : "Own tone",
                         0,
                         APP_ROUTE_TONE_COMPOSER,
                         now);
        } else {
            app->route = APP_ROUTE_TONE_COMPOSER;
            app->dirty = true;
        }
        break;
    case 1:
    case 3:
        open_editor(app,
                    ts_or(0x0e7u, "Tone name:"),
                    app->tone_composer_name[0] ? app->tone_composer_name : "Own tone",
                    STORE_OWN_TONE_NAME_MAX,
                    EDITOR_KIND_TEXT,
                    EDITOR_CONTEXT_TONE_COMPOSER_NAME,
                    true,
                    now);
        app->tone_composer_name_action = app->tone_composer_option_index == 3u ? 1u : 0u;
        break;
    case 2:
        app->route = APP_ROUTE_TONE_COMPOSER_TEMPO;
        app->dirty = true;
        break;
    case 4:
        composer_stop_audio(app);
        app->tone_composer_notes[0] = '\0';
        app->tone_composer_cursor_index = 0u;
        app->tone_composer_sharp_armed = false;
        app->tone_composer_dotted_armed = false;
        app->route = APP_ROUTE_TONE_COMPOSER;
        app->dirty = true;
        break;
    case 5:
    default:
        composer_return_to_tones_menu(app);
        break;
    }
    return true;
}

bool handle_tone_composer_tempo_key(app_t *app, uint16_t key, uint32_t now) {
    (void)now;
    uint8_t count = (uint8_t)ARRAY_COUNT(COMPOSER_TEMPO_OPTIONS);
    if (key == KEY_C) {
        app->route = APP_ROUTE_TONE_COMPOSER_OPTIONS;
        app->dirty = true;
        return true;
    }
    if (key == KEY_UP) {
        app->tone_composer_tempo_index = app->tone_composer_tempo_index == 0u
            ? (uint8_t)(count - 1u)
            : (uint8_t)(app->tone_composer_tempo_index - 1u);
        app->dirty = true;
        return true;
    }
    if (key == KEY_DOWN) {
        app->tone_composer_tempo_index = (uint8_t)((app->tone_composer_tempo_index + 1u) % count);
        app->dirty = true;
        return true;
    }
    if (key == KEY_NAVI) {
        app->route = APP_ROUTE_TONE_COMPOSER_OPTIONS;
        app->dirty = true;
        return true;
    }
    return true;
}

void tone_composer_submit_name(app_t *app, uint32_t now) {
    copy_text(app->tone_composer_name,
              sizeof(app->tone_composer_name),
              app->editor_value[0] ? app->editor_value : "Own tone");
    close_editor(app);
    if (composer_token_count(app) < 1u) {
        app->route = APP_ROUTE_TONE_COMPOSER;
        app->dirty = true;
        return;
    }
    if (app->tone_composer_name_action != 0u) {
        app->sms_recipient_prefill[0] = '\0';
        open_tone_composer_recipient_editor(app, "", now);
        return;
    }

    if (composer_store_current_own_tone(app))
        open_display_sid(app, 3u, 0x0e6u, "Tone saved", APP_ROUTE_TONE_COMPOSER, now);
    else
        open_display_sid(app, 0u, 0x3b3u, "Not\nsaved", APP_ROUTE_TONE_COMPOSER, now);
}

void tone_composer_cancel_name(app_t *app, uint32_t now) {
    (void)now;
    close_editor(app);
    app->route = APP_ROUTE_TONE_COMPOSER_OPTIONS;
    app->dirty = true;
}

void open_tone_composer_recipient_editor(app_t *app, const char *value, uint32_t now) {
    open_editor(app, ts_or(0x212u, "Enter number:"), value, 21u, EDITOR_KIND_NUMBER, EDITOR_CONTEXT_TONE_COMPOSER_RECIPIENT, true, now);
    update_tone_composer_recipient_softkey(app);
}

void update_tone_composer_recipient_softkey(app_t *app) {
    (void)app;
}

void tone_composer_submit_recipient(app_t *app, uint32_t now) {
    modem_status_t status;
    modem_service_get_status(&status);
    if (!status.sim_ready) {
        open_display_sid(app, 2u, 0x297u, "SIM card\nnot ready", APP_ROUTE_TONE_COMPOSER_OPTIONS, now);
        return;
    }
    if (composer_token_count(app) < 1u) {
        app->route = APP_ROUTE_TONE_COMPOSER;
        app->dirty = true;
        return;
    }
    if (app->tone_composer_send_request_id != 0u) {
        open_display_sid(app, 0u, 0x35au, "Still\nsending\nprevious", APP_ROUTE_TONE_COMPOSER_OPTIONS, now);
        return;
    }
    if (!composer_build_packed_tone(app->tone_composer_name, app->tone_composer_notes,
            app->tone_composer_tempo_index, app->tone_composer_packed,
            sizeof(app->tone_composer_packed), &app->tone_composer_packed_len) ||
        !modem_service_request_send_binary_sms(app->editor_value, app->tone_composer_packed,
            app->tone_composer_packed_len, RINGTONE_SMS_PORT, 0u,
            &app->tone_composer_send_request_id)) {
        open_display_sid(app, 0u, 0x210u, "Not\ndone", APP_ROUTE_TONE_COMPOSER_OPTIONS, now);
        return;
    }
    (void)composer_store_current_own_tone(app);
    close_editor(app);
    app->tone_composer_send_waiting = true;
    app->tone_composer_send_started_ms = now;
    open_display_sid(app, 46u, 0x0feu, "Sending\ntone", APP_ROUTE_TONE_COMPOSER, now);
}

void tone_composer_cancel_recipient(app_t *app, uint32_t now) {
    (void)now;
    close_editor(app);
    app->route = APP_ROUTE_TONE_COMPOSER_OPTIONS;
    app->dirty = true;
}

bool tick_tone_composer(app_t *app, uint32_t now) {
    bool changed = false;
    if (app->tone_composer_send_request_id != 0u) {
        bool owned = app->tone_composer_send_waiting && app->route == APP_ROUTE_DISPLAY_MESSAGE &&
            app->display_record_id == 46u && app->display_return_route == APP_ROUTE_TONE_COMPOSER;
        app->tone_composer_send_waiting = owned;
        modem_sms_send_result_t result;
        if (modem_service_pop_sms_send_result(app->tone_composer_send_request_id, &result)) {
            app->tone_composer_send_request_id = 0u;
            app->tone_composer_send_waiting = false;
            if (owned) {
                if (result.kind == MODEM_SMS_REQUEST_SEND_BINARY && result.outcome == MODEM_SMS_OUTCOME_OK)
                    open_display_sid(app, 6u, 0x0fdu, "Tone sent", APP_ROUTE_TONE_COMPOSER, now);
                else if (result.outcome == MODEM_SMS_OUTCOME_UNCERTAIN)
                    open_display_sid(app, 0u, 0x229u, "Result\nunknown", APP_ROUTE_TONE_COMPOSER_OPTIONS, now);
                else open_display_sid(app, 0u, 0x359u, "Message\nsending\nfailed", APP_ROUTE_TONE_COMPOSER_OPTIONS, now);
            }
            changed = true;
        } else if (owned && time_diff_ms(now, app->tone_composer_send_started_ms + COMPOSER_SEND_TIMEOUT_MS) >= 0) {
            /* Keep the request until its result is drained, even after the UI
             * times out or a call/alarm takes over. */
            app->tone_composer_send_waiting = false;
            open_display_sid(app, 0u, 0x229u, "Result\nunknown", APP_ROUTE_TONE_COMPOSER_OPTIONS, now);
            changed = true;
        }
    }
    if (app->tone_composer_playing &&
        time_diff_ms(now, app->tone_composer_next_note_ms) >= 0) {
        composer_play_next(app, now);
        changed = true;
    }
    if (app->tone_composer_preview_stop_ms != 0u &&
        (app->route != APP_ROUTE_TONE_COMPOSER ||
         time_diff_ms(now, app->tone_composer_preview_stop_ms) >= 0)) {
        /* Route preemption can start a ring/alarm before this global tick runs.
         * Retire only the Composer note still owned by this preview; the core1
         * command is a no-op once another audio kind has superseded it. */
        core1_post_command(CORE1_CMD_AUDIO_COMPOSER_PREVIEW_STOP, 0u);
        app->tone_composer_preview_stop_ms = 0u;
    }
    if (app->route == APP_ROUTE_TONE_COMPOSER &&
        time_diff_ms(now, app->tone_composer_last_cursor_ms + COMPOSER_CURSOR_BLINK_MS) >= 0) {
        app->tone_composer_cursor_visible = !app->tone_composer_cursor_visible;
        app->tone_composer_last_cursor_ms = now;
        changed = true;
    }
    return changed;
}

void render_tone_composer(const app_t *app, framebuffer_t *fb) {
    fb_clear(fb, false);
    /* No title bar: the v6.00 note editor has no title window ("Composer" SID
     * 0x0e3 is only the Tones-menu launch label). Top-left status icon = the
     * THREE-NOTES melody glyph (bitmap 30, 18x7) -- CONFIRMED on the real handset.
     * The composer is a CUSTOM editor (open 0x28a52a -> msg 0x4bd6 -> custom
     * runtime 0x23ad1c), so it does NOT take the generic text-editor's pencil
     * (bitmap 14) that the SMS editor gets; it shows its own melody indicator. */
    fb_bitmap(fb, 30u, 0, 0, true, true);
    const font_t *font = asset_font(FONT_FS2);

    uint16_t cursor_char = composer_cursor_char_index(app);
    composer_layout_t measured = {.cursor_char = cursor_char};
    uint8_t line_count = composer_layout_lines(app, &measured);
    uint8_t cursor_line = measured.cursor_line;
    uint8_t start = cursor_line > 2u ? (uint8_t)(cursor_line - 2u) : 0u;
    if (line_count > 3u && start > line_count - 3u) {
        start = (uint8_t)(line_count - 3u);
    }

    /* A legal score can wrap to far more than 12 rows. Count the complete
     * layout above, then materialize only the visible window so long melodies
     * scroll to their real tail without a large core-0 stack allocation. */
    composer_line_t lines[3];
    composer_layout_t visible = {
        .cursor_char = cursor_char,
        .capture_start = start,
        .captured = lines,
        .capture_cap = (uint8_t)ARRAY_COUNT(lines),
    };
    (void)composer_layout_lines(app, &visible);
    for (uint8_t row = 0u; row < visible.captured_count; row++) {
        fb_text(fb, font, lines[row].text, 1, 10 + row * 9, true, 82);
    }
    if (app->tone_composer_cursor_visible) {
        uint8_t cursor_row = cursor_line >= start
            ? (uint8_t)(cursor_line - start)
            : 0u;
        if (cursor_row >= visible.captured_count) {
            cursor_row = visible.captured_count - 1u;
        }
        composer_line_t *line = &lines[cursor_row];
        uint16_t prefix_len = cursor_char > line->start ? (uint16_t)(cursor_char - line->start) : 0u;
        if (prefix_len >= sizeof(line->text)) {
            prefix_len = (uint16_t)(sizeof(line->text) - 1u);
        }
        char prefix[32];
        memcpy(prefix, line->text, prefix_len);
        prefix[prefix_len] = '\0';
        int cursor_x = 2 + asset_text_width(font, prefix);
        if (cursor_x > 82) {
            cursor_x = 82;
        }
        int cursor_y = 9 + (int)cursor_row * 9;
        fb_vline(fb, cursor_x, cursor_y, 9, true);
    }
    draw_softkey(fb, "Options");
}

void render_tone_composer_options(const app_t *app, framebuffer_t *fb) {
    const char *labels[ARRAY_COUNT(COMPOSER_OPTION_LABELS)];
    for (uint8_t i = 0; i < ARRAY_COUNT(COMPOSER_OPTION_LABELS); i++) {
        labels[i] = ts_or(COMPOSER_OPTION_LABELS[i].sid, COMPOSER_OPTION_LABELS[i].label);
    }
    draw_flat_list(fb,
                   labels,
                   (uint8_t)ARRAY_COUNT(COMPOSER_OPTION_LABELS),
                   app->tone_composer_option_index,
                   0,
                   "Select");
}

void render_tone_composer_tempo(const app_t *app, framebuffer_t *fb) {
    const char *labels[ARRAY_COUNT(COMPOSER_TEMPO_OPTIONS)];
    for (uint8_t i = 0; i < ARRAY_COUNT(COMPOSER_TEMPO_OPTIONS); i++) {
        labels[i] = ts_or(COMPOSER_TEMPO_OPTIONS[i].sid, COMPOSER_TEMPO_OPTIONS[i].label);
    }
    draw_flat_list(fb,
                   labels,
                   (uint8_t)ARRAY_COUNT(COMPOSER_TEMPO_OPTIONS),
                   app->tone_composer_tempo_index,
                   0,
                   "OK");
}
static void composer_return_to_tones_menu(app_t *app) {
    composer_stop_audio(app);
    app->route = APP_ROUTE_TONES_MENU;
    app->tones_menu_selected = 2u;
    app->dirty = true;
}

static void composer_stop_audio(app_t *app) {
    /* Main queues the keypad click before dispatching this app event. Keep the
     * cleanup owner-scoped so C/Navi remain audible while still stopping a
     * Composer stream when keypad tones are disabled. */
    core1_post_command(CORE1_CMD_AUDIO_COMPOSER_STOP, 0u);
    app->tone_composer_playing = false;
    app->tone_composer_preview_stop_ms = 0u;
}

static void composer_load_own_tone(app_t *app, uint32_t now) {
    store_own_tone_t *tone = &s_composer_scratch.tone;
    if (store_own_tone_get(0u, tone) == STORE_STATUS_OK && tone->used) {
        copy_text(app->tone_composer_name, sizeof(app->tone_composer_name), tone->name[0] ? tone->name : "Own tone");
        copy_text(app->tone_composer_notes, sizeof(app->tone_composer_notes), tone->notes);
        app->tone_composer_tempo_index = tone->tempo_index < ARRAY_COUNT(COMPOSER_TEMPO_OPTIONS)
            ? tone->tempo_index
            : COMPOSER_DEFAULT_TEMPO_INDEX;
    } else {
        copy_text(app->tone_composer_name, sizeof(app->tone_composer_name), "Own tone");
        app->tone_composer_notes[0] = '\0';
        app->tone_composer_tempo_index = COMPOSER_DEFAULT_TEMPO_INDEX;
    }
    app->tone_composer_cursor_index = composer_token_count(app);
    app->tone_composer_duration_index = COMPOSER_DEFAULT_DURATION_INDEX;
    app->tone_composer_octave = COMPOSER_DEFAULT_OCTAVE;
    app->tone_composer_option_index = 0u;
    app->tone_composer_sharp_armed = false;
    app->tone_composer_dotted_armed = false;
    app->tone_composer_cursor_visible = true;
    app->tone_composer_last_cursor_ms = now;
    app->tone_composer_hold_key = 0u;
    app->tone_composer_hold_consumed = false;
    app->tone_composer_name_action = 0u;
    app->tone_composer_send_waiting = false;
    app->tone_composer_packed_len = 0u;
}

static bool composer_split_tokens(const char *notes, char tokens[][COMPOSER_TOKEN_MAX], uint8_t *out_count) {
    if (out_count == 0) {
        return false;
    }
    *out_count = 0u;
    if (notes == 0) {
        return true;
    }
    while (*notes != '\0') {
        while (*notes == ' ') {
            notes++;
        }
        if (*notes == '\0') {
            break;
        }
        if (*out_count >= COMPOSER_MAX_TOKENS) {
            return false;
        }
        uint8_t len = 0u;
        while (*notes != '\0' && *notes != ' ') {
            if (len + 1u >= COMPOSER_TOKEN_MAX) {
                return false;
            }
            tokens[*out_count][len++] = *notes;
            notes++;
        }
        tokens[*out_count][len] = '\0';
        (*out_count)++;
    }
    return true;
}

static bool composer_join_tokens(char tokens[][COMPOSER_TOKEN_MAX], uint8_t count, char *out, size_t out_cap) {
    if (out == 0 || out_cap == 0u) {
        return false;
    }
    /* Preflight the whole serialization before touching `out`. The token array
     * is separate from the live score, so a successful second pass is an atomic
     * commit and a rejected growth leaves every original byte intact. */
    size_t used = 0u;
    for (uint8_t i = 0u; i < count; i++) {
        size_t token_len = strlen(tokens[i]);
        size_t separator = i == 0u ? 0u : 1u;
        if (used >= out_cap || separator > out_cap - 1u - used ||
            token_len > out_cap - 1u - used - separator) {
            return false;
        }
        used += separator + token_len;
    }

    out[0] = '\0';
    used = 0u;
    for (uint8_t i = 0u; i < count; i++) {
        size_t token_len = strlen(tokens[i]);
        if (i != 0u) {
            out[used++] = ' ';
        }
        memcpy(&out[used], tokens[i], token_len);
        used += token_len;
        out[used] = '\0';
    }
    return true;
}

static uint8_t composer_token_count(const app_t *app) {
    char (*tokens)[COMPOSER_TOKEN_MAX] = s_composer_scratch.tokens;
    uint8_t count = 0u;
    (void)composer_split_tokens(app->tone_composer_notes, tokens, &count);
    return count;
}

static uint8_t composer_clamp_cursor(const app_t *app, uint8_t count) {
    return app->tone_composer_cursor_index > count ? count : app->tone_composer_cursor_index;
}

static bool composer_parse_token(const char *token, composer_token_t *out) {
    if (token == 0 || out == 0) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    const char *p = token;
    if (strncmp(p, "16", 2) == 0 || strncmp(p, "32", 2) == 0) {
        out->duration[0] = p[0];
        out->duration[1] = p[1];
        out->duration[2] = '\0';
        p += 2;
    } else if (*p == '1' || *p == '2' || *p == '4' || *p == '8') {
        out->duration[0] = *p++;
        out->duration[1] = '\0';
    } else {
        return false;
    }
    if (*p == '.') {
        out->dotted = true;
        p++;
    }
    if (*p == '#') {
        out->sharp = true;
        p++;
    }
    if (*p == '-' || (*p >= 'a' && *p <= 'g')) {
        out->note = *p++;
    } else {
        return false;
    }
    if (out->note == '-') {
        out->octave = 0u;
        return *p == '\0';
    }
    if (*p >= '1' && *p <= '3') {
        out->octave = (uint8_t)(*p++ - '0');
    } else {
        return false;
    }
    return *p == '\0';
}

static void composer_format_token(const composer_token_t *parts, char *out, size_t cap) {
    if (out == 0 || cap == 0u) {
        return;
    }
    if (parts == 0) {
        out[0] = '\0';
        return;
    }
    size_t pos = 0u;
    const char *duration = parts->duration[0] ? parts->duration : "4";
    while (*duration != '\0' && pos + 1u < cap) {
        out[pos++] = *duration++;
    }
    if (parts->dotted && pos + 1u < cap) {
        out[pos++] = '.';
    }
    if (parts->sharp && parts->note != '-' && pos + 1u < cap) {
        out[pos++] = '#';
    }
    if (pos + 1u < cap) {
        out[pos++] = parts->note ? parts->note : 'c';
    }
    if (parts->note != '-' && pos + 1u < cap) {
        uint8_t octave = parts->octave == 0u ? 1u : parts->octave;
        out[pos++] = (char)('0' + octave);
    }
    out[pos] = '\0';
}

static bool composer_note_from_key(uint16_t key, char *out_note) {
    char note = 0;
    switch (key) {
    case KEY_1: note = 'c'; break;
    case KEY_2: note = 'd'; break;
    case KEY_3: note = 'e'; break;
    case KEY_4: note = 'f'; break;
    case KEY_5: note = 'g'; break;
    case KEY_6: note = 'a'; break;
    case KEY_7: note = 'b'; break;
    case KEY_0: note = '-'; break;
    default: break;
    }
    if (note == 0) {
        return false;
    }
    if (out_note != 0) {
        *out_note = note;
    }
    return true;
}

static bool composer_note_sharpable(char note) {
    return note == 'c' || note == 'd' || note == 'f' || note == 'g' || note == 'a';
}

static composer_edit_result_t composer_insert_note(app_t *app, char note, uint32_t now) {
    char (*tokens)[COMPOSER_TOKEN_MAX] = s_composer_scratch.tokens;
    uint8_t count = 0u;
    if (!composer_split_tokens(app->tone_composer_notes, tokens, &count) || count >= COMPOSER_MAX_TOKENS) {
        return COMPOSER_EDIT_FULL;
    }
    uint8_t cursor = composer_clamp_cursor(app, count);
    for (uint8_t i = count; i > cursor; i--) {
        memcpy(tokens[i], tokens[i - 1u], COMPOSER_TOKEN_MAX);
    }
    composer_token_t parts;
    memset(&parts, 0, sizeof(parts));
    copy_text(parts.duration, sizeof(parts.duration), COMPOSER_DURATIONS[app->tone_composer_duration_index]);
    parts.dotted = app->tone_composer_dotted_armed;
    parts.sharp = app->tone_composer_sharp_armed && composer_note_sharpable(note);
    parts.note = note;
    parts.octave = app->tone_composer_octave;
    composer_format_token(&parts, tokens[cursor], COMPOSER_TOKEN_MAX);
    count++;
    if (!composer_join_tokens(tokens, count, app->tone_composer_notes,
                              sizeof(app->tone_composer_notes))) {
        return COMPOSER_EDIT_FULL;
    }
    app->tone_composer_cursor_index = (uint8_t)(cursor + 1u);
    app->tone_composer_sharp_armed = false;
    app->tone_composer_dotted_armed = false;
    app->tone_composer_cursor_visible = true;
    app->tone_composer_last_cursor_ms = now;
    composer_start_preview(app, &parts, now);
    app->dirty = true;
    return COMPOSER_EDIT_APPLIED;
}

static bool composer_delete_previous(app_t *app) {
    char (*tokens)[COMPOSER_TOKEN_MAX] = s_composer_scratch.tokens;
    uint8_t count = 0u;
    if (!composer_split_tokens(app->tone_composer_notes, tokens, &count) || count == 0u) {
        return false;
    }
    uint8_t cursor = composer_clamp_cursor(app, count);
    if (cursor == 0u) {
        return false;
    }
    for (uint8_t i = (uint8_t)(cursor - 1u); i + 1u < count; i++) {
        memcpy(tokens[i], tokens[i + 1u], COMPOSER_TOKEN_MAX);
    }
    count--;
    if (!composer_join_tokens(tokens, count, app->tone_composer_notes,
                              sizeof(app->tone_composer_notes))) {
        return false;
    }
    app->tone_composer_cursor_index = (uint8_t)(cursor - 1u);
    app->tone_composer_sharp_armed = false;
    app->tone_composer_dotted_armed = false;
    return true;
}

static void composer_step_duration(app_t *app, int8_t delta) {
    uint8_t max = (uint8_t)(ARRAY_COUNT(COMPOSER_DURATIONS) - 1u);
    if (delta > 0 && app->tone_composer_duration_index < max) {
        app->tone_composer_duration_index++;
    } else if (delta < 0 && app->tone_composer_duration_index > 0u) {
        app->tone_composer_duration_index--;
    }
    app->dirty = true;
}

static void composer_step_octave(app_t *app) {
    app->tone_composer_octave = app->tone_composer_octave >= 3u ? 1u : (uint8_t)(app->tone_composer_octave + 1u);
    app->dirty = true;
}

static composer_edit_result_t composer_toggle_sharp(app_t *app) {
    char (*tokens)[COMPOSER_TOKEN_MAX] = s_composer_scratch.tokens;
    uint8_t count = 0u;
    if (!composer_split_tokens(app->tone_composer_notes, tokens, &count) || count == 0u ||
        composer_clamp_cursor(app, count) == 0u) {
        app->tone_composer_sharp_armed = !app->tone_composer_sharp_armed;
        app->dirty = true;
        return COMPOSER_EDIT_APPLIED;
    }
    uint8_t target = (uint8_t)(composer_clamp_cursor(app, count) - 1u);
    composer_token_t parts;
    if (composer_parse_token(tokens[target], &parts) && composer_note_sharpable(parts.note)) {
        parts.sharp = !parts.sharp;
        composer_format_token(&parts, tokens[target], COMPOSER_TOKEN_MAX);
        if (!composer_join_tokens(tokens, count, app->tone_composer_notes,
                                  sizeof(app->tone_composer_notes))) {
            return COMPOSER_EDIT_FULL;
        }
        app->tone_composer_sharp_armed = false;
    } else {
        app->tone_composer_sharp_armed = !app->tone_composer_sharp_armed;
    }
    app->dirty = true;
    return COMPOSER_EDIT_APPLIED;
}

static composer_edit_result_t composer_toggle_dot(app_t *app) {
    char (*tokens)[COMPOSER_TOKEN_MAX] = s_composer_scratch.tokens;
    uint8_t count = 0u;
    if (!composer_split_tokens(app->tone_composer_notes, tokens, &count) || count == 0u ||
        composer_clamp_cursor(app, count) == 0u) {
        app->tone_composer_dotted_armed = !app->tone_composer_dotted_armed;
        app->dirty = true;
        return COMPOSER_EDIT_APPLIED;
    }
    uint8_t target = (uint8_t)(composer_clamp_cursor(app, count) - 1u);
    composer_token_t parts;
    if (composer_parse_token(tokens[target], &parts)) {
        parts.dotted = !parts.dotted;
        composer_format_token(&parts, tokens[target], COMPOSER_TOKEN_MAX);
        if (!composer_join_tokens(tokens, count, app->tone_composer_notes,
                                  sizeof(app->tone_composer_notes))) {
            return COMPOSER_EDIT_FULL;
        }
        app->tone_composer_dotted_armed = false;
        app->dirty = true;
        return COMPOSER_EDIT_APPLIED;
    }
    return COMPOSER_EDIT_UNCHANGED;
}

static void composer_show_full(app_t *app, uint32_t now) {
    open_display_sid(app, 0u, 0x280u, "Memory\nfull",
                     APP_ROUTE_TONE_COMPOSER, now);
}

static void composer_move_cursor(app_t *app, int8_t delta) {
    uint8_t count = composer_token_count(app);
    uint8_t cursor = composer_clamp_cursor(app, count);
    if (delta < 0 && cursor > 0u) {
        cursor--;
    } else if (delta > 0 && cursor < count) {
        cursor++;
    }
    app->tone_composer_cursor_index = cursor;
    app->tone_composer_cursor_visible = true;
    app->tone_composer_last_cursor_ms = time_ms();
    app->dirty = true;
}

static uint8_t composer_pitch_byte(const composer_token_t *parts) {
    if (parts == 0 || parts->note == '-') {
        return 0x40u;
    }
    uint8_t semitone = 0u;
    switch (parts->note) {
    case 'c': semitone = 0u; break;
    case 'd': semitone = 2u; break;
    case 'e': semitone = 4u; break;
    case 'f': semitone = 5u; break;
    case 'g': semitone = 7u; break;
    case 'a': semitone = 9u; break;
    case 'b': semitone = 11u; break;
    default: return 0x40u;
    }
    if (parts->sharp && composer_note_sharpable(parts->note)) {
        semitone++;
    }
    uint8_t octave = parts->octave == 0u ? 1u : parts->octave;
    uint8_t freq_index = (uint8_t)(62u + semitone + (octave - 1u) * 12u);
    if (freq_index > 100u) {
        freq_index = 100u;
    }
    return (uint8_t)(0x40u + freq_index);
}

static uint8_t composer_pitch_code(const composer_token_t *parts) {
    if (parts == 0 || parts->note == '-') {
        return 0u;
    }
    uint8_t code = 0u;
    switch (parts->note) {
    case 'c': code = 1u; break;
    case 'd': code = 3u; break;
    case 'e': code = 5u; break;
    case 'f': code = 6u; break;
    case 'g': code = 8u; break;
    case 'a': code = 10u; break;
    case 'b': code = 12u; break;
    default: return 0u;
    }
    if (parts->sharp && composer_note_sharpable(parts->note)) {
        code++;
    }
    return code;
}

static uint8_t composer_duration_code(const composer_token_t *parts) {
    uint16_t denom = 4u;
    if (parts != 0 && parts->duration[0] != '\0') {
        denom = (uint16_t)atoi(parts->duration);
    }
    return composer_duration_code_for_denominator(denom);
}

static uint16_t composer_note_duration_ms(const app_t *app, const composer_token_t *parts) {
    /* BPM from the tempo BYTECODE via the shared codec table -- the label is
     * display copy only, never parsed. */
    uint8_t tempo = COMPOSER_TEMPO_OPTIONS[
        app->tone_composer_tempo_index < ARRAY_COUNT(COMPOSER_TEMPO_OPTIONS)
            ? app->tone_composer_tempo_index
            : COMPOSER_DEFAULT_TEMPO_INDEX].value;
    uint16_t denom = 4u;
    if (parts != 0 && parts->duration[0] != '\0') {
        denom = (uint16_t)atoi(parts->duration);
        if (denom == 0u) {
            denom = 4u;
        }
    }
    return composer_note_ms(composer_tempo_bpm(tempo), denom,
                            parts != 0 && parts->dotted);
}

static uint16_t composer_cursor_char_index(const app_t *app) {
    char (*tokens)[COMPOSER_TOKEN_MAX] = s_composer_scratch.tokens;
    uint8_t count = 0u;
    (void)composer_split_tokens(app->tone_composer_notes, tokens, &count);
    uint8_t cursor = composer_clamp_cursor(app, count);
    uint16_t index = 0u;
    for (uint8_t i = 0u; i < cursor; i++) {
        if (i != 0u) {
            index++;
        }
        index = (uint16_t)(index + strlen(tokens[i]));
    }
    return index;
}

static void composer_accept_layout_line(composer_layout_t *layout,
                                        const composer_line_t *line,
                                        uint8_t line_index) {
    if (!layout->cursor_found) {
        /* Retain the latest row as a total fallback for malformed/out-of-range
         * cursor state, then stop moving it once the insertion point is found. */
        layout->cursor_line = line_index;
        if (layout->cursor_char >= line->start &&
            layout->cursor_char <= line->end) {
            layout->cursor_found = true;
        }
    }
    if (layout->captured != NULL &&
        line_index >= layout->capture_start &&
        layout->captured_count < layout->capture_cap) {
        layout->captured[layout->captured_count++] = *line;
    }
}

static uint8_t composer_layout_lines(const app_t *app, composer_layout_t *layout) {
    if (app == NULL || layout == NULL) {
        return 0u;
    }
    layout->cursor_line = 0u;
    layout->cursor_found = false;
    layout->captured_count = 0u;

    char (*tokens)[COMPOSER_TOKEN_MAX] = s_composer_scratch.tokens;
    uint8_t count = 0u;
    (void)composer_split_tokens(app->tone_composer_notes, tokens, &count);
    composer_line_t line_record;
    memset(&line_record, 0, sizeof(line_record));
    if (count == 0u) {
        composer_accept_layout_line(layout, &line_record, 0u);
        return 1u;
    }
    const font_t *font = asset_font(FONT_FS2);
    uint8_t line_index = 0u;
    uint16_t char_index = 0u;
    for (uint8_t i = 0u; i < count; i++) {
        uint16_t token_start = char_index;
        uint8_t token_len = (uint8_t)strlen(tokens[i]);
        char candidate[32];
        if (line_record.text[0] == '\0') {
            copy_text(candidate, sizeof(candidate), tokens[i]);
        } else {
            size_t pos = 0u;
            while (line_record.text[pos] != '\0' && pos + 1u < sizeof(candidate)) {
                candidate[pos] = line_record.text[pos];
                pos++;
            }
            if (pos + 1u < sizeof(candidate)) {
                candidate[pos++] = ' ';
            }
            uint8_t token_pos = 0u;
            while (tokens[i][token_pos] != '\0' && pos + 1u < sizeof(candidate)) {
                candidate[pos++] = tokens[i][token_pos++];
            }
            candidate[pos] = '\0';
        }
        if (line_record.text[0] != '\0' && asset_text_width(font, candidate) > 82) {
            composer_accept_layout_line(layout, &line_record, line_index++);
            memset(&line_record, 0, sizeof(line_record));
            copy_text(line_record.text, sizeof(line_record.text), tokens[i]);
            line_record.start = token_start;
            line_record.end = (uint16_t)(token_start + token_len);
        } else {
            if (line_record.text[0] == '\0') {
                line_record.start = token_start;
            }
            copy_text(line_record.text, sizeof(line_record.text), candidate);
            line_record.end = (uint16_t)(token_start + token_len);
        }
        char_index = (uint16_t)(token_start + token_len + 1u);
    }
    composer_accept_layout_line(layout, &line_record, line_index);
    return (uint8_t)(line_index + 1u);
}

static void composer_post_note(const app_t *app, const composer_token_t *parts) {
    uint8_t level = tones_current_ringing_audio_level(app);
    if (level == AUDIO_LEVEL_SILENT || parts == 0 || parts->note == '-') {
        core1_post_command(CORE1_CMD_AUDIO_STOP, 0u);
        return;
    }
    core1_post_command(CORE1_CMD_AUDIO_COMPOSER_NOTE, audio_arg(composer_pitch_byte(parts), level));
}

static void composer_start_preview(app_t *app, const composer_token_t *parts, uint32_t now) {
    app->tone_composer_playing = false;
    composer_post_note(app, parts);
    app->tone_composer_preview_stop_ms = now + COMPOSER_PREVIEW_MS;
}

static void composer_start_playback(app_t *app, uint32_t now) {
    (void)now;
    app->tone_composer_preview_stop_ms = 0u;
    app->tone_composer_playing = false;
    app->tone_composer_play_index = 0u;
    app->tone_composer_next_note_ms = 0u;
    if (!composer_build_packed_tone(app->tone_composer_name,
                                    app->tone_composer_notes,
                                    app->tone_composer_tempo_index,
                                    app->tone_composer_packed,
                                    STORE_OWN_TONE_PACKED_MAX,
                                    &app->tone_composer_packed_len)) {
        return;
    }
    uint8_t level = tones_current_ringing_audio_level(app);
    if (level == AUDIO_LEVEL_SILENT) {
        return;
    }
    core1_post_audio_composer_packed(app->tone_composer_packed, app->tone_composer_packed_len, level);
}

static void composer_play_next(app_t *app, uint32_t now) {
    char (*tokens)[COMPOSER_TOKEN_MAX] = s_composer_scratch.tokens;
    uint8_t count = 0u;
    if (!composer_split_tokens(app->tone_composer_notes, tokens, &count) ||
        app->tone_composer_play_index >= count) {
        composer_stop_audio(app);
        return;
    }
    composer_token_t parts;
    if (!composer_parse_token(tokens[app->tone_composer_play_index], &parts)) {
        app->tone_composer_play_index++;
        app->tone_composer_next_note_ms = now + 1u;
        return;
    }
    composer_post_note(app, &parts);
    uint16_t duration = composer_note_duration_ms(app, &parts);
    app->tone_composer_play_index++;
    app->tone_composer_next_note_ms = now + duration;
}

static bool composer_build_packed_tone(const char *name,
                                       const char *notes,
                                       uint8_t tempo_index,
                                       uint8_t *dst,
                                       uint16_t cap,
                                       uint16_t *out_len) {
    if (dst == 0 || out_len == 0) {
        return false;
    }
    *out_len = 0u;
    char (*tokens)[COMPOSER_TOKEN_MAX] = s_composer_scratch.tokens;
    uint8_t count = 0u;
    if (!composer_split_tokens(notes, tokens, &count)) {
        return false;
    }
    uint8_t tempo = COMPOSER_TEMPO_OPTIONS[tempo_index < ARRAY_COUNT(COMPOSER_TEMPO_OPTIONS)
                                               ? tempo_index
                                               : COMPOSER_DEFAULT_TEMPO_INDEX]
                        .value;
    composer_note_event_t *events = s_composer_scratch.events;
    /* Rests carry the RUNNING octave (the traced v6.00 encoder rule, owned
     * and tested in composer_codec), never a fixed 1. */
    composer_octave_tracker_t octave;
    composer_octave_tracker_init(&octave);
    for (uint8_t i = 0u; i < count; i++) {
        composer_token_t parts;
        if (!composer_parse_token(tokens[i], &parts)) {
            return false;
        }
        events[i].octave = composer_octave_tracker_next(&octave, parts.octave);
        events[i].pitch_code = composer_pitch_code(&parts);
        events[i].duration_code = composer_duration_code(&parts);
        events[i].dotted = parts.dotted;
    }
    /* Editors accept legacy CP1252 as well as UTF-8. Normalize at the app
     * boundary; the wire codec remains independent of UI character rules. */
    char title[COMPOSER_CODEC_NAME_MAX * 3u + 1u];
    const char *cursor = name != NULL ? name : "";
    size_t used = 0u;
    for (unsigned i = 0u; *cursor != '\0' && i < COMPOSER_CODEC_NAME_MAX; i++) {
        uint16_t cp = asset_next_codepoint(&cursor);
        if (cp < 0x80u) title[used++] = (char)cp;
        else if (cp < 0x800u) {
            title[used++] = (char)(0xc0u | (cp >> 6u));
            title[used++] = (char)(0x80u | (cp & 63u));
        } else {
            title[used++] = (char)(0xe0u | (cp >> 12u));
            title[used++] = (char)(0x80u | ((cp >> 6u) & 63u));
            title[used++] = (char)(0x80u | (cp & 63u));
        }
    }
    title[used] = '\0';
    return composer_codec_encode(title, events, count, tempo, dst, cap, out_len);
}

static bool composer_store_current_own_tone(app_t *app) {
    store_own_tone_t *tone = &s_composer_scratch.tone;
    memset(tone, 0, sizeof(*tone));
    tone->used = true;
    copy_text(tone->name, sizeof(tone->name), app->tone_composer_name);
    copy_text(tone->notes, sizeof(tone->notes), app->tone_composer_notes);
    tone->tempo_index = app->tone_composer_tempo_index;
    if (!composer_build_packed_tone(tone->name,
                                     tone->notes,
                                     tone->tempo_index,
                                     tone->packed,
                                     STORE_OWN_TONE_PACKED_MAX,
                                     &tone->packed_len)) return false;
    return store_own_tone_set(0u, tone) == STORE_STATUS_OK;
}

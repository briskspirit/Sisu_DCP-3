/* Characterization coverage for the Composer application boundary. The full
 * production app source is linked with dead stripping so this test exercises
 * the public editor/storage/audio flow while unrelated Tones-menu code stays
 * outside the host fixture. Keep these assertions behavior-focused: Phase 5A
 * may move the implementation between translation units, but must not change
 * the route transitions, wire bytes, or persistent record. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "apps/dialogs_app.h"
#include "apps/profiles_app.h"
#include "apps/tones_app.h"
#include "audio/audio_levels.h"
#include "audio/composer_codec.h"
#include "services/core1_services.h"
#include "services/feature_gates.h"
#include "services/input_keys.h"
#include "services/modem_service.h"
#include "services/strings.h"
#include "services/timebase.h"
#include "storage/store_service.h"
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

typedef struct {
    core1_cmd_t cmd;
    uint16_t arg;
} command_post_t;

static command_post_t s_posts[32];
static size_t s_post_count;
static uint8_t s_packed[CORE1_AUDIO_COMPOSER_PACKED_MAX];
static uint16_t s_packed_len;
static uint8_t s_packed_level;
static unsigned s_packed_posts;
static store_own_tone_t s_stored_tone;
static bool s_stored_tone_present;
static unsigned s_store_sets;
static unsigned s_profile_sets;
static uint8_t s_profile_set_index;
static profile_setting_kind_t s_profile_set_kind;
static uint8_t s_profile_set_value;
static modem_status_t s_modem_status;
static uint32_t s_now;
static bool s_send_result_ready;
static modem_sms_outcome_t s_send_outcome;
static store_own_tone_t s_received_tone;
static uint32_t s_pending_id;
static store_status_t s_save_status, s_commit_status;
static unsigned s_received_saves, s_received_discards;

typedef struct {
    unsigned plain_count;
    unsigned sid_count;
    uint8_t record_id;
    uint16_t sid;
    char a[40];
    char b[40];
    char fallback[64];
    app_route_t return_route;
    uint32_t now;
} display_trace_t;

static display_trace_t s_display;
typedef struct {
    unsigned count;
    confirm_context_t context;
    uint16_t sid;
    char fallback[64];
    int8_t first_y;
} confirm_trace_t;

static confirm_trace_t s_confirm;
static unsigned s_editor_opens;
static unsigned s_editor_closes;
static uint8_t s_list_count;
static uint8_t s_list_selected;
static bool s_list_circular;
static uint8_t s_list_view_start;
static char s_list_breadcrumb[24];
static char s_list_labels[6][24];

static void reset_fixture(void) {
    memset(s_posts, 0, sizeof(s_posts));
    s_post_count = 0u;
    memset(s_packed, 0, sizeof(s_packed));
    s_packed_len = 0u;
    s_packed_level = 0u;
    s_packed_posts = 0u;
    memset(&s_stored_tone, 0, sizeof(s_stored_tone));
    s_stored_tone_present = false;
    s_store_sets = 0u;
    s_profile_sets = 0u;
    s_profile_set_index = 0u;
    s_profile_set_kind = PROFILE_SETTING_INCOMING_ALERT;
    s_profile_set_value = 0u;
    memset(&s_modem_status, 0, sizeof(s_modem_status));
    memset(&s_display, 0, sizeof(s_display));
    memset(&s_confirm, 0, sizeof(s_confirm));
    s_editor_opens = 0u;
    s_editor_closes = 0u;
    s_list_count = 0u;
    s_list_selected = 0u;
    s_list_circular = false;
    s_list_view_start = 0u;
    s_list_breadcrumb[0] = '\0';
    memset(s_list_labels, 0, sizeof(s_list_labels));
    s_now = 0u;
    s_send_result_ready = false;
    s_send_outcome = MODEM_SMS_OUTCOME_OK;
    memset(&s_received_tone, 0, sizeof(s_received_tone));
    s_pending_id = 0u;
    s_save_status = s_commit_status = STORE_STATUS_OK;
    s_received_saves = s_received_discards = 0u;
}

static void make_capacity_score(char *dst, size_t cap) {
    size_t used = 0u;
    if (dst == NULL || cap == 0u) {
        return;
    }
    dst[0] = '\0';
    /* 68 * "4c1", one "4.c1", and 68 separators = 276 bytes: exactly the
     * persistent Composer text limit. The first note remains eligible for both
     * dot and sharp edits, so every growth path can be exercised in place. */
    for (uint8_t i = 0u; i < 69u; i++) {
        const char *token = i == 68u ? "4.c1" : "4c1";
        int wrote = snprintf(&dst[used], cap - used, "%s%s", i == 0u ? "" : " ", token);
        check(wrote > 0 && (size_t)wrote < cap - used,
              "capacity-score fixture fits its destination");
        if (wrote <= 0 || (size_t)wrote >= cap - used) {
            return;
        }
        used += (size_t)wrote;
    }
    check(used == STORE_OWN_TONE_NOTES_MAX,
          "capacity-score fixture reaches the exact persistent text limit");
}

static void make_repeated_score(char *dst, size_t cap, uint8_t count) {
    size_t used = 0u;
    if (dst == NULL || cap == 0u) {
        return;
    }
    dst[0] = '\0';
    for (uint8_t i = 0u; i < count; i++) {
        int wrote = snprintf(&dst[used], cap - used, "%s4c1", i == 0u ? "" : " ");
        check(wrote > 0 && (size_t)wrote < cap - used,
              "repeated-score fixture fits its destination");
        if (wrote <= 0 || (size_t)wrote >= cap - used) {
            return;
        }
        used += (size_t)wrote;
    }
}

static uint8_t plain_tokens_per_composer_line(void) {
    const font_t *font = asset_font(FONT_FS2);
    char line[32] = "";
    uint8_t count = 0u;
    for (;;) {
        char candidate[32];
        int wrote = snprintf(candidate, sizeof(candidate), "%s%s4c1",
                             line, count == 0u ? "" : " ");
        if (wrote <= 0 || (size_t)wrote >= sizeof(candidate) ||
            (count != 0u && asset_text_width(font, candidate) > 82)) {
            return count;
        }
        copy_text(line, sizeof(line), candidate);
        count++;
    }
}

/* --------------------------------------------------------------- host seams */

void copy_text(char *dst, size_t cap, const char *src) {
    if (dst == NULL || cap == 0u) {
        return;
    }
    snprintf(dst, cap, "%s", src != NULL ? src : "");
}

uint32_t time_ms(void) {
    return s_now;
}

int32_t time_diff_ms(uint32_t a, uint32_t b) {
    return (int32_t)(a - b);
}

uint8_t profile_get_tone_setting(uint8_t profile_index, profile_setting_kind_t kind) {
    (void)profile_index;
    return kind == PROFILE_SETTING_RINGING_VOLUME ? 8u : 0u;
}

uint8_t profile_default_setting_value(uint8_t profile_index,
                                      profile_setting_kind_t kind) {
    return profile_get_tone_setting(profile_index, kind);
}

uint8_t profile_count(void) {
    return 5u;
}

uint8_t profile_active_index(void) {
    return 0u;
}

void open_profiles_options(app_t *app, uint8_t selected_profile) {
    app->route = APP_ROUTE_PROFILES_MENU;
    app->profiles_selected_index = selected_profile;
}

void open_main_menu_at(app_t *app, uint8_t selected, uint32_t now_ms) {
    (void)now_ms;
    app->route = APP_ROUTE_MAIN_MENU;
    app->menu_index = selected;
}

bool feature_gate_visible(feature_gate_id_t gate) {
    (void)gate;
    return true;
}

void profile_set_tone_setting(uint8_t profile_index,
                              profile_setting_kind_t kind,
                              uint8_t value) {
    s_profile_sets++;
    s_profile_set_index = profile_index;
    s_profile_set_kind = kind;
    s_profile_set_value = value;
}

void core1_post_command(core1_cmd_t cmd, uint16_t arg) {
    if (s_post_count < sizeof(s_posts) / sizeof(s_posts[0])) {
        s_posts[s_post_count].cmd = cmd;
        s_posts[s_post_count].arg = arg;
    }
    s_post_count++;
}

void core1_post_audio_composer_packed(const uint8_t *data, uint16_t len, uint8_t level) {
    s_packed_posts++;
    s_packed_len = len > sizeof(s_packed) ? (uint16_t)sizeof(s_packed) : len;
    memcpy(s_packed, data, s_packed_len);
    s_packed_level = level;
}

void core1_post_audio_packed_tone_preview(const uint8_t *data, uint16_t len, uint8_t level) {
    core1_post_audio_composer_packed(data, len, level);
}

store_status_t store_own_tone_get(uint8_t slot, store_own_tone_t *out_tone) {
    check(slot == 0u, "Composer reads own-tone slot zero");
    if (!s_stored_tone_present) {
        return STORE_STATUS_NOT_FOUND;
    }
    *out_tone = s_stored_tone;
    return STORE_STATUS_OK;
}

bool store_own_tone_used(uint8_t slot) {
    if (slot == 1u) return false;
    check(slot == 0u, "Tones menu checks own-tone slot zero");
    return s_stored_tone_present && s_stored_tone.used;
}

store_status_t store_own_tone_set(uint8_t slot, const store_own_tone_t *tone) {
    check(slot == 0u, "Composer writes own-tone slot zero");
    if (s_save_status != STORE_STATUS_OK) return s_save_status;
    s_stored_tone = *tone;
    s_stored_tone_present = true;
    s_store_sets++;
    return STORE_STATUS_OK;
}

void modem_service_get_status(modem_status_t *out) {
    *out = s_modem_status;
}

uint32_t store_ringtone_pending_first(void) { return s_pending_id; }
store_status_t store_ringtone_pending_get(uint32_t id, store_own_tone_t *tone) {
    if (id == 0u || id != s_pending_id) return STORE_STATUS_NOT_FOUND;
    *tone = s_received_tone;
    return STORE_STATUS_OK;
}
store_status_t store_ringtone_pending_save(uint32_t id) {
    check(id == s_pending_id, "save addresses the selected incoming ringtone");
    s_received_saves++;
    return s_save_status;
}
store_status_t store_ringtone_pending_discard(uint32_t id) {
    check(id == s_pending_id, "discard addresses the selected incoming ringtone");
    s_received_discards++;
    return s_save_status;
}
store_status_t store_ringtone_commit_status(void) { return s_commit_status; }

bool modem_service_request_send_binary_sms(const char *number, const uint8_t *payload,
                                          uint16_t length, uint16_t port, uint16_t source,
                                          uint32_t *id) {
    check(strcmp(number, "+15551234567") == 0, "ringtone send uses recipient editor number");
    check(payload != NULL && length != 0u && port == 0x1581u && source == 0u,
          "ringtone send uses native binary port and source");
    *id = 42u;
    return true;
}
bool modem_service_pop_sms_send_result(uint32_t id, modem_sms_send_result_t *result) {
    check(id == 42u, "ringtone result is requested by its own ID");
    if (!s_send_result_ready) return false;
    s_send_result_ready = false;
    memset(result, 0, sizeof(*result));
    result->kind = MODEM_SMS_REQUEST_SEND_BINARY;
    result->outcome = s_send_outcome;
    return true;
}

const char *ts_or(uint16_t sid, const char *fallback) {
    (void)sid;
    return fallback;
}

const char *ui_breadcrumb_path(char *buf,
                               size_t cap,
                               const char *parent,
                               unsigned selected_ordinal) {
    if (parent != NULL && parent[0] != '\0') {
        snprintf(buf, cap, "%s-%u", parent, selected_ordinal);
    } else {
        snprintf(buf, cap, "%u", selected_ordinal);
    }
    return buf;
}

void draw_flat_list(framebuffer_t *fb,
                    const char *const *labels,
                    uint8_t count,
                    uint8_t selected,
                    const char *breadcrumb,
                    const char *softkey) {
    (void)fb;
    (void)softkey;
    s_list_count = count;
    s_list_selected = selected;
    s_list_circular = false;
    copy_text(s_list_breadcrumb, sizeof(s_list_breadcrumb), breadcrumb);
    for (uint8_t i = 0u; i < count && i < 6u; i++) {
        copy_text(s_list_labels[i], sizeof(s_list_labels[i]), labels[i]);
    }
}

void draw_flat_list_circular_view(framebuffer_t *fb,
                                  const char *const *labels,
                                  uint8_t count,
                                  uint8_t selected,
                                  uint8_t view_start,
                                  const char *breadcrumb,
                                  const char *softkey) {
    (void)fb;
    (void)softkey;
    s_list_count = count;
    s_list_selected = selected;
    s_list_circular = true;
    s_list_view_start = view_start;
    copy_text(s_list_breadcrumb, sizeof(s_list_breadcrumb), breadcrumb);
    for (uint8_t row = 0u; row < count && row < 6u; row++) {
        uint8_t index = count == 0u ? 0u : (uint8_t)((view_start + row) % count);
        copy_text(s_list_labels[row], sizeof(s_list_labels[row]), labels[index]);
    }
}

void draw_softkey(framebuffer_t *fb, const char *label) {
    (void)fb;
    (void)label;
}

void open_display(app_t *app,
                  uint8_t record_id,
                  const char *a,
                  const char *b,
                  const char *c,
                  app_route_t return_route,
                  uint32_t now) {
    app->route = APP_ROUTE_DISPLAY_MESSAGE;
    app->display_record_id = record_id;
    app->display_return_route = return_route;
    (void)c;
    s_display.plain_count++;
    s_display.record_id = record_id;
    copy_text(s_display.a, sizeof(s_display.a), a);
    copy_text(s_display.b, sizeof(s_display.b), b);
    s_display.return_route = return_route;
    s_display.now = now;
}

void return_from_display(app_t *app) {
    app->route = app->display_return_route;
    app->dirty = true;
}

void open_display_sid(app_t *app,
                      uint8_t record_id,
                      uint16_t sid,
                      const char *fallback,
                      app_route_t return_route,
                      uint32_t now) {
    app->route = APP_ROUTE_DISPLAY_MESSAGE;
    app->display_record_id = record_id;
    app->display_return_route = return_route;
    s_display.sid_count++;
    s_display.record_id = record_id;
    s_display.sid = sid;
    copy_text(s_display.fallback, sizeof(s_display.fallback), fallback);
    s_display.return_route = return_route;
    s_display.now = now;
}

void open_confirm_sid(app_t *app,
                      confirm_context_t context,
                      uint16_t sid,
                      const char *fallback,
                      int8_t first_y) {
    s_confirm.count++;
    s_confirm.context = context;
    s_confirm.sid = sid;
    copy_text(s_confirm.fallback, sizeof(s_confirm.fallback), fallback);
    s_confirm.first_y = first_y;
    app->confirm_context = (uint8_t)context;
    app->route = APP_ROUTE_CONFIRM;
    app->dirty = true;
}

void open_editor(app_t *app,
                 const char *title,
                 const char *value,
                 uint8_t max_len,
                 editor_kind_t kind,
                 editor_context_t context,
                 bool show_cursor,
                 uint32_t now) {
    (void)app;
    (void)title;
    (void)value;
    (void)max_len;
    (void)kind;
    (void)context;
    (void)show_cursor;
    (void)now;
    s_editor_opens++;
}

void close_editor(app_t *app) {
    (void)app;
    s_editor_closes++;
}

/* --------------------------------------------------------------- scenarios */

static void test_default_open_and_note_editing(void) {
    reset_fixture();
    app_t app;
    memset(&app, 0, sizeof(app));
    app.tones_profile_index = 2u;

    open_tone_composer(&app, 1000u);
    check(app.route == APP_ROUTE_TONE_COMPOSER, "open routes to Composer");
    check_str(app.tone_composer_name, "Own tone", "empty slot loads the default name");
    check(app.tone_composer_notes[0] == '\0', "empty slot loads an empty score");
    check(app.tone_composer_tempo_index == 8u, "default tempo is 100 BPM");
    check(app.tone_composer_duration_index == 2u, "default duration is a quarter note");
    check(app.tone_composer_octave == 1u, "default octave is one");
    check(app.tone_composer_cursor_visible, "open shows the cursor");
    check(app.tone_composer_last_cursor_ms == 1000u, "open seeds the cursor timer");
    check(s_post_count == 1u && s_posts[0].cmd == CORE1_CMD_AUDIO_COMPOSER_STOP &&
              s_posts[0].arg == 0u,
          "open retires only an existing Composer audio owner");

    check(handle_tone_composer_key(&app, KEY_1, EVENT_KEY_DOWN, 1100u), "key 1 is consumed");
    check_str(app.tone_composer_notes, "4c1", "key 1 inserts middle C with current duration/octave");
    check(s_posts[s_post_count - 1u].cmd == CORE1_CMD_AUDIO_COMPOSER_NOTE,
          "insert previews through the Composer note command");
    check(s_posts[s_post_count - 1u].arg == audio_arg(0x7eu, 3u),
          "preview carries the exact pitch byte and ringing level");
    check(app.tone_composer_preview_stop_ms == 1280u, "preview is bounded to 180 ms");

    check(handle_tone_composer_key(&app, KEY_1, EVENT_KEY_HOLD, 1120u), "held note is consumed");
    check_str(app.tone_composer_notes, "4.c1", "holding the inserted note toggles dotted");
    check(handle_tone_composer_key(&app, KEY_HASH, EVENT_KEY_DOWN, 1130u), "hash is consumed");
    check_str(app.tone_composer_notes, "4.#c1", "hash toggles sharp on the preceding note");

    (void)handle_tone_composer_key(&app, KEY_STAR, EVENT_KEY_DOWN, 1140u);
    (void)handle_tone_composer_key(&app, KEY_8, EVENT_KEY_DOWN, 1150u);
    (void)handle_tone_composer_key(&app, KEY_2, EVENT_KEY_DOWN, 1160u);
    check_str(app.tone_composer_notes, "4.#c1 8d2", "duration and octave apply to the next note");
    check(s_posts[s_post_count - 1u].arg == audio_arg(0x8cu, 3u),
          "second preview uses D in octave two");

    s_now = 1200u;
    (void)handle_tone_composer_key(&app, KEY_UP, EVENT_KEY_DOWN, 1200u);
    check(app.tone_composer_cursor_index == 1u && app.tone_composer_last_cursor_ms == 1200u,
          "cursor movement uses the shared timebase");
    (void)handle_tone_composer_key(&app, KEY_C, EVENT_KEY_DOWN, 1210u);
    check_str(app.tone_composer_notes, "8d2", "C deletes the token before the insertion cursor");
    check(app.route == APP_ROUTE_TONE_COMPOSER, "deleting a token stays in Composer");
    check(s_posts[s_post_count - 1u].cmd == CORE1_CMD_AUDIO_COMPOSER_STOP,
          "C uses owner-scoped Composer cleanup so its keypad click survives");

    (void)handle_tone_composer_key(&app, KEY_NAVI, EVENT_KEY_DOWN, 1220u);
    check(app.route == APP_ROUTE_TONE_COMPOSER_OPTIONS,
          "Navi opens Composer options");
    check(s_posts[s_post_count - 1u].cmd == CORE1_CMD_AUDIO_COMPOSER_STOP,
          "Navi uses owner-scoped Composer cleanup so its keypad click survives");
}

static void test_saved_tone_playback_and_persistence(void) {
    reset_fixture();
    memset(&s_stored_tone, 0, sizeof(s_stored_tone));
    s_stored_tone.used = true;
    copy_text(s_stored_tone.name, sizeof(s_stored_tone.name), "Saved");
    copy_text(s_stored_tone.notes, sizeof(s_stored_tone.notes), "4c1 4- 8e2");
    s_stored_tone.tempo_index = 9u;
    s_stored_tone_present = true;

    app_t app;
    memset(&app, 0, sizeof(app));
    app.tones_profile_index = 1u;
    open_tone_composer(&app, 2000u);
    check_str(app.tone_composer_name, "Saved", "open loads the persisted name");
    check_str(app.tone_composer_notes, "4c1 4- 8e2", "open loads the persisted score");
    check(app.tone_composer_cursor_index == 3u, "open places the cursor after all loaded tokens");
    check(app.tone_composer_tempo_index == 9u, "open loads a valid persisted tempo");

    app.route = APP_ROUTE_TONE_COMPOSER_OPTIONS;
    app.tone_composer_option_index = 0u;
    (void)handle_tone_composer_options_key(&app, KEY_NAVI, 2100u);
    check(s_packed_posts == 1u && s_packed_len != 0u, "Play posts one packed Composer stream");
    check(s_packed_level == 3u, "packed playback uses the active ringing level");
    check(s_display.plain_count == 1u && s_display.record_id == 3u,
          "Play opens the v6.00 playing-tone display");
    check_str(s_display.a, "Playing tone", "Play display heading is preserved");
    check_str(s_display.b, "Saved", "Play display carries the tone name");
    check(s_display.return_route == APP_ROUTE_TONE_COMPOSER,
          "Play display returns to the editor");

    composer_note_event_t events[4];
    uint8_t event_count = 0u;
    uint8_t tempo = 0u;
    check(composer_codec_decode(s_packed,
                                s_packed_len,
                                &tempo,
                                events,
                                (uint8_t)(sizeof(events) / sizeof(events[0])),
                                &event_count),
          "posted packed stream decodes");
    check(event_count == 3u && tempo == 0x0du, "packed stream preserves note count and tempo byte");
    check(events[0].octave == 1u && events[0].pitch_code == 1u,
          "packed stream preserves the first C");
    check(events[1].octave == 1u && events[1].pitch_code == 0u,
          "rest carries the running octave");
    check(events[2].octave == 2u && events[2].pitch_code == 5u,
          "packed stream preserves E in octave two");

    copy_text(app.editor_value, sizeof(app.editor_value), "Mine");
    app.tone_composer_name_action = 0u;
    memset(&s_display, 0, sizeof(s_display));
    tone_composer_submit_name(&app, 2200u);
    check(s_editor_closes == 1u, "saving closes the name editor");
    check(s_store_sets == 1u && s_stored_tone.used, "saving writes the own-tone record");
    check_str(s_stored_tone.name, "Mine", "saved record uses the submitted name");
    check_str(s_stored_tone.notes, "4c1 4- 8e2", "saved record preserves the score text");
    check(s_stored_tone.tempo_index == 9u && s_stored_tone.packed_len != 0u,
          "saved record carries tempo and packed wire form");
    check(s_display.sid_count == 1u && s_display.sid == 0x0e6u,
          "saving opens the localized Tone saved note");
}

static void test_level5_ringing_volume_requires_confirmation(void) {
    reset_fixture();
    app_t app;
    memset(&app, 0, sizeof(app));
    app.route = APP_ROUTE_TONES_SETTING;
    app.tones_profile_index = 2u;
    app.tones_setting_kind = TONES_SETTING_RINGING_VOLUME;

    app.tones_setting_selected = 3u; /* raw value 9 / Level 4 */
    check(handle_tones_setting_key(&app, KEY_NAVI, 2500u),
          "Level 4 save key is consumed");
    check(s_profile_sets == 1u && s_profile_set_index == 2u &&
              s_profile_set_kind == PROFILE_SETTING_RINGING_VOLUME &&
              s_profile_set_value == 9u,
          "Level 4 persists immediately without confirmation");
    check(s_confirm.count == 0u && s_display.plain_count == 1u,
          "ordinary ringing volume keeps the existing save notice");
    check(s_post_count == 1u &&
              s_posts[0].cmd == CORE1_CMD_AUDIO_TONES_PREVIEW_STOP,
          "ordinary save stops only its active Tones preview");

    reset_fixture();
    memset(&app, 0, sizeof(app));
    app.route = APP_ROUTE_TONES_SETTING;
    app.tones_profile_index = 2u;
    app.tones_setting_kind = TONES_SETTING_RINGING_VOLUME;
    app.tones_setting_selected = 4u; /* raw value 10 / Level 5 */
    check(handle_tones_setting_key(&app, KEY_NAVI, 2600u),
          "Level 5 save key is consumed");
    check(s_profile_sets == 0u,
          "Level 5 is not persisted before explicit confirmation");
    check(s_confirm.count == 1u &&
              s_confirm.context == CONFIRM_CONTEXT_TONES_RINGING_VOLUME &&
              s_confirm.sid == 0x264u && s_confirm.first_y == 2 &&
              strcmp(s_confirm.fallback, "Note:\nVERY LOUD RINGING") == 0,
          "Level 5 opens the exact v6.00 very-loud bool dialog");
    check(app.route == APP_ROUTE_CONFIRM &&
              app.confirm_context == CONFIRM_CONTEXT_TONES_RINGING_VOLUME,
          "warning owns the confirmation route");
    check(s_post_count == 1u &&
              s_posts[0].cmd == CORE1_CMD_AUDIO_TONES_PREVIEW_STOP,
          "warning stops only the Level 5 preview before opening");

    tones_confirm_ringing_volume(&app, false, 2610u);
    check(s_profile_sets == 0u && app.route == APP_ROUTE_TONES_SETTING &&
              app.confirm_context == CONFIRM_CONTEXT_NONE,
          "Back returns to the volume selector without saving Level 5");
    check(app.tones_setting_selected == 4u,
          "cancelled warning preserves the selected Level 5 row");

    app.confirm_context = CONFIRM_CONTEXT_TONES_RINGING_VOLUME;
    app.route = APP_ROUTE_CONFIRM;
    tones_confirm_ringing_volume(&app, true, 2620u);
    check(s_profile_sets == 1u && s_profile_set_index == 2u &&
              s_profile_set_kind == PROFILE_SETTING_RINGING_VOLUME &&
              s_profile_set_value == 10u,
          "OK persists raw ringing-volume value 10 exactly once");
    check(s_display.sid_count == 1u && s_display.record_id == 3u &&
              s_display.sid == 0x132u &&
              strcmp(s_display.fallback, "Done") == 0 &&
              s_display.return_route == APP_ROUTE_TONES_MENU,
          "confirmed Level 5 shows the v6.00 Done record");
    check(app.confirm_context == CONFIRM_CONTEXT_NONE,
          "accepted warning retires its confirmation ownership");
}

static void test_ringtone_preview_debounce(void) {
    reset_fixture();
    app_t app;
    memset(&app, 0, sizeof(app));
    app.route = APP_ROUTE_TONES_SETTING;
    app.tones_profile_index = 0u;
    app.tones_setting_kind = TONES_SETTING_RINGING_TONE;
    app.tones_setting_selected = 5u; /* Bee; Down selects Nokia tune (index 6). */

    check(handle_tones_setting_key(&app, KEY_DOWN, 1000u),
          "ringtone Down is consumed");
    check(s_post_count == 1u &&
              s_posts[0].cmd == CORE1_CMD_AUDIO_TONES_PREVIEW_STOP,
          "ringtone motion stops only the preceding Tones preview");
    check(app.tones_preview_pending && app.tones_preview_due_ms == 2024u,
          "ringtone motion arms the exact 1024 ms ROM debounce");
    check(!tick_tones_setting(&app, 2023u) && s_post_count == 1u,
          "ringtone preview remains silent through the last early millisecond");
    check(tick_tones_setting(&app, 2024u) && s_post_count == 2u &&
              s_posts[1].cmd == CORE1_CMD_AUDIO_RINGTONE_MENU_PREVIEW &&
              audio_arg_code(s_posts[1].arg) == 6u && !app.tones_preview_pending,
          "ringtone preview starts on the delayed event for the selected row");

    check(handle_tones_setting_key(&app, KEY_DOWN, 3000u),
          "first rapid ringtone step is consumed");
    check(handle_tones_setting_key(&app, KEY_DOWN, 3400u),
          "second rapid ringtone step is consumed");
    check(app.tones_preview_due_ms == 4424u,
          "a later row restarts rather than inherits the first deadline");
    size_t posts_before = s_post_count;
    check(!tick_tones_setting(&app, 4024u) && s_post_count == posts_before,
          "the superseded row never chirps at its old deadline");
    check(tick_tones_setting(&app, 4424u) &&
              s_posts[s_post_count - 1u].cmd == CORE1_CMD_AUDIO_RINGTONE_MENU_PREVIEW &&
              audio_arg_code(s_posts[s_post_count - 1u].arg) == 8u,
          "the restarted deadline previews only the final row");

    check(handle_tones_setting_key(&app, KEY_UP, 0xffffff00u),
          "wrap-boundary ringtone step is consumed");
    check(app.tones_preview_due_ms == 0x00000300u,
          "preview deadline wraps without using zero as a sentinel");
    posts_before = s_post_count;
    check(!tick_tones_setting(&app, 0x000002ffu) && s_post_count == posts_before,
          "wrap-safe deadline remains early at wrapped deadline minus one");
    check(tick_tones_setting(&app, 0x00000300u) &&
              audio_arg_code(s_posts[s_post_count - 1u].arg) == 7u,
          "wrap-safe deadline fires at the exact wrapped instant");

    check(handle_tones_setting_key(&app, KEY_DOWN, 5000u),
          "cancellation fixture arms another preview");
    check(handle_tones_setting_key(&app, KEY_C, 5001u) &&
              !app.tones_preview_pending && app.route == APP_ROUTE_TONES_MENU,
          "C cancels the pending preview while leaving the selector");
    posts_before = s_post_count;
    check(!tick_tones_setting(&app, 7000u) && s_post_count == posts_before,
          "a cancelled preview cannot fire after its deadline");

    reset_fixture();
    memset(&app, 0, sizeof(app));
    app.route = APP_ROUTE_TONES_SETTING;
    app.tones_setting_kind = TONES_SETTING_RINGING_VOLUME;
    app.tones_setting_selected = 1u;
    (void)handle_tones_setting_key(&app, KEY_DOWN, 8000u);
    check(s_post_count == 1u &&
              s_posts[0].cmd == CORE1_CMD_AUDIO_TONES_PREVIEW_STOP &&
              app.tones_preview_pending && app.tones_preview_due_ms == 9024u,
          "ringing-volume rows use the same deferred preview contract");

    reset_fixture();
    memset(&app, 0, sizeof(app));
    app.route = APP_ROUTE_TONES_SETTING;
    app.tones_setting_kind = TONES_SETTING_MESSAGE_ALERT;
    app.tones_setting_selected = 0u;
    (void)handle_tones_setting_key(&app, KEY_DOWN, 9000u);
    check(!app.tones_preview_pending && s_post_count == 2u &&
              s_posts[0].cmd == CORE1_CMD_AUDIO_TONES_PREVIEW_STOP &&
              s_posts[1].cmd == CORE1_CMD_AUDIO_TONES_SYSTEM_PREVIEW,
          "message-alert editor retains its immediate preview behavior");
}

static void test_tones_setting_list_contract(void) {
    reset_fixture();
    app_t app;
    memset(&app, 0, sizeof(app));
    app.route = APP_ROUTE_TONES_MENU;
    app.tones_profile_index = 2u;
    app.tones_menu_selected = 3u; /* Tones -> Ringing volume. */

    check(handle_tones_menu_key(&app, KEY_NAVI, 9100u),
          "Ringing-volume picker open is consumed");
    check(app.route == APP_ROUTE_TONES_SETTING &&
              app.tones_setting_kind == TONES_SETTING_RINGING_VOLUME &&
              app.tones_setting_selected == 2u &&
              app.tones_setting_view_start == 2u,
          "picker opens on the profile's current Level 3 value");

    framebuffer_t fb;
    memset(&fb, 0, sizeof(fb));
    render_tones_setting(&app, &fb);
    check(s_list_circular && s_list_selected == 2u &&
              s_list_view_start == 2u,
          "Tones picker opens its circular viewport on the current value");
    check_str(s_list_labels[0], "Level 3",
              "current setting is the first visible picker row");
    check_str(s_list_labels[1], "Level 4",
              "picker continues forward after the current value");
    check_str(s_list_breadcrumb, "9-4-3",
              "direct Tones picker carries the full parent and option path");

    check(handle_tones_setting_key(&app, KEY_DOWN, 9110u),
          "picker Down is consumed");
    check(s_post_count == 1u &&
              s_posts[0].cmd == CORE1_CMD_AUDIO_TONES_PREVIEW_STOP,
          "picker movement uses owner-scoped preview cleanup");
    render_tones_setting(&app, &fb);
    check(s_list_selected == 3u && s_list_view_start == 2u,
          "first Down moves selection to the second row without scrolling");
    check_str(s_list_labels[0], "Level 3",
              "first Down leaves the viewport anchored at its entry row");
    check_str(s_list_labels[1], "Level 4",
              "first Down highlights the second visible option");
    check_str(s_list_breadcrumb, "9-4-4",
              "picker breadcrumb follows the selected option");

    check(handle_tones_setting_key(&app, KEY_DOWN, 9111u),
          "second picker Down is consumed");
    render_tones_setting(&app, &fb);
    check(s_list_selected == 4u && s_list_view_start == 2u,
          "second Down reaches the third row without scrolling");

    check(handle_tones_setting_key(&app, KEY_DOWN, 9112u),
          "third picker Down is consumed");
    render_tones_setting(&app, &fb);
    check(s_list_selected == 0u && s_list_view_start == 3u,
          "Down past the third row advances the circular viewport once");
    check_str(s_list_labels[0], "Level 4",
              "scrolled circular viewport retains the previous second row");
    check_str(s_list_labels[2], "Level 1",
              "scrolled circular viewport highlights the wrapped option");

    check(handle_tones_setting_key(&app, KEY_UP, 9113u),
          "first picker Up is consumed");
    check(app.tones_setting_selected == 4u &&
              app.tones_setting_view_start == 3u,
          "Up from the third row moves to the second without scrolling");
    check(handle_tones_setting_key(&app, KEY_UP, 9114u),
          "second picker Up is consumed");
    check(app.tones_setting_selected == 3u &&
              app.tones_setting_view_start == 3u,
          "second Up reaches the first row without scrolling");
    check(handle_tones_setting_key(&app, KEY_UP, 9115u),
          "third picker Up is consumed");
    check(app.tones_setting_selected == 2u &&
              app.tones_setting_view_start == 2u,
          "Up past the first row scrolls the circular viewport once");

    check(handle_tones_setting_key(&app, KEY_C, 9120u) &&
              app.route == APP_ROUTE_TONES_MENU,
          "picker C returns to the Tones menu");
    check(s_post_count == 7u &&
              s_posts[6].cmd == CORE1_CMD_AUDIO_TONES_PREVIEW_STOP,
          "picker C cannot erase its already-queued keypad click");

    app.tones_from_profiles = true;
    app.tones_profile_index = 3u;
    app.tones_menu_selected = 4u;
    app.tones_setting_kind = TONES_SETTING_MESSAGE_ALERT;
    app.tones_setting_selected = 1u;
    app.tones_setting_view_start = 1u;
    render_tones_setting(&app, &fb);
    check_str(s_list_breadcrumb, "10-4-2-5-2",
              "Profiles personalisation picker carries its full nested path");
}

static void test_option_routes_and_tempo_selection(void) {
    reset_fixture();
    app_t app;
    memset(&app, 0, sizeof(app));
    copy_text(app.tone_composer_name, sizeof(app.tone_composer_name), "Trace");

    framebuffer_t fb;
    memset(&fb, 0, sizeof(fb));
    render_tone_composer_options(&app, &fb);
    check(s_list_count == 6u, "Composer exposes all six original options");
    check_str(s_list_labels[0], "Play", "Composer option 1 is Play");
    check_str(s_list_labels[1], "Save", "Composer option 2 is Save");
    check_str(s_list_labels[2], "Tempo", "Composer option 3 is Tempo");
    check_str(s_list_labels[3], "Send", "Composer option 4 is Send");
    check_str(s_list_labels[4], "Clear screen", "Composer option 5 is Clear screen");
    check_str(s_list_labels[5], "Exit", "Composer option 6 is Exit");

    app.route = APP_ROUTE_TONE_COMPOSER_OPTIONS;
    app.tone_composer_option_index = 1u;
    (void)handle_tone_composer_options_key(&app, KEY_NAVI, 2300u);
    check(s_editor_opens == 1u && app.tone_composer_name_action == 0u,
          "Save opens the name editor with the save action");

    copy_text(app.tone_composer_notes, sizeof(app.tone_composer_notes), "4.#c2");
    app.tone_composer_cursor_index = 1u;
    app.tone_composer_sharp_armed = true;
    app.tone_composer_dotted_armed = true;
    app.tone_composer_playing = true;
    app.tone_composer_preview_stop_ms = 9999u;
    app.route = APP_ROUTE_TONE_COMPOSER_OPTIONS;
    app.tone_composer_option_index = 4u;
    size_t posts_before = s_post_count;
    (void)handle_tone_composer_options_key(&app, KEY_NAVI, 2310u);
    check(app.route == APP_ROUTE_TONE_COMPOSER &&
              app.tone_composer_notes[0] == '\0' &&
              app.tone_composer_cursor_index == 0u &&
              !app.tone_composer_sharp_armed &&
              !app.tone_composer_dotted_armed,
          "Clear screen resets the score and returns to Composer");
    check(s_editor_opens == 1u,
          "Clear screen does not open an editor");
    check(!app.tone_composer_playing &&
              app.tone_composer_preview_stop_ms == 0u &&
              s_post_count == posts_before + 1u &&
              s_posts[posts_before].cmd == CORE1_CMD_AUDIO_COMPOSER_STOP,
          "Clear screen stops its Composer audio owner exactly once");

    app.tone_composer_playing = true;
    app.tone_composer_preview_stop_ms = 9999u;
    app.route = APP_ROUTE_TONE_COMPOSER_OPTIONS;
    app.tone_composer_option_index = 5u;
    posts_before = s_post_count;
    (void)handle_tone_composer_options_key(&app, KEY_NAVI, 2330u);
    check(app.route == APP_ROUTE_TONES_MENU && app.tones_menu_selected == 2u,
          "Exit returns to the Own tones menu entry");
    check(!app.tone_composer_playing &&
              app.tone_composer_preview_stop_ms == 0u &&
              s_post_count == posts_before + 1u &&
              s_posts[posts_before].cmd == CORE1_CMD_AUDIO_COMPOSER_STOP,
          "Exit stops its Composer audio owner exactly once");

    app.route = APP_ROUTE_TONE_COMPOSER_OPTIONS;
    app.tone_composer_option_index = 2u;
    (void)handle_tone_composer_options_key(&app, KEY_NAVI, 2340u);
    check(app.route == APP_ROUTE_TONE_COMPOSER_TEMPO,
          "Tempo option opens the tempo selector");

    app.tone_composer_tempo_index = 8u;
    (void)handle_tone_composer_tempo_key(&app, KEY_DOWN, 2350u);
    check(app.tone_composer_tempo_index == 9u,
          "tempo Down advances one selection");
    (void)handle_tone_composer_tempo_key(&app, KEY_UP, 2360u);
    check(app.tone_composer_tempo_index == 8u,
          "tempo Up reverses one selection");

    app.tone_composer_tempo_index = 0u;
    (void)handle_tone_composer_tempo_key(&app, KEY_UP, 2370u);
    check(app.tone_composer_tempo_index == 15u,
          "tempo Up wraps from first to last");
    (void)handle_tone_composer_tempo_key(&app, KEY_DOWN, 2380u);
    check(app.tone_composer_tempo_index == 0u,
          "tempo Down wraps from last to first");

    (void)handle_tone_composer_tempo_key(&app, KEY_NAVI, 2390u);
    check(app.route == APP_ROUTE_TONE_COMPOSER_OPTIONS,
          "tempo Navi accepts and returns to options");
    app.route = APP_ROUTE_TONE_COMPOSER_TEMPO;
    (void)handle_tone_composer_tempo_key(&app, KEY_C, 2400u);
    check(app.route == APP_ROUTE_TONE_COMPOSER_OPTIONS,
          "tempo C returns to options without changing the selection");
}

static void test_send_timeout_and_timers(void) {
    reset_fixture();
    app_t app;
    memset(&app, 0, sizeof(app));
    copy_text(app.tone_composer_name, sizeof(app.tone_composer_name), "Send me");
    copy_text(app.tone_composer_notes, sizeof(app.tone_composer_notes), "4c1");
    app.tone_composer_tempo_index = 8u;

    tone_composer_submit_recipient(&app, 3000u);
    check(!app.tone_composer_send_waiting, "SIM-not-ready does not arm send progress");
    check(s_display.sid_count == 1u && s_display.sid == 0x297u,
          "SIM-not-ready opens the matching localized note");

    memset(&s_display, 0, sizeof(s_display));
    s_modem_status.sim_ready = true;
    strcpy(app.editor_value, "+15551234567");
    tone_composer_submit_recipient(&app, 3100u);
    check(app.tone_composer_send_waiting && app.tone_composer_send_started_ms == 3100u,
          "SIM-ready send arms its progress deadline");
    check(s_store_sets == 1u, "send persists the current tone first");
    check(s_display.sid_count == 1u && s_display.sid == 0x0feu,
          "send opens the localized Sending tone note");
    check(!tick_tone_composer(&app, 4100u), "send does not invent a failure after one second");
    s_send_result_ready = true; s_send_outcome = MODEM_SMS_OUTCOME_OK;
    check(tick_tone_composer(&app, 4200u), "matching send result reports a state change");
    check(!app.tone_composer_send_waiting && app.tone_composer_send_request_id == 0u,
          "send completion releases its result slot");
    check(s_display.sid == 0x0fdu, "accepted send opens Tone sent");
    strcpy(app.editor_value, "+15551234567");
    tone_composer_submit_recipient(&app, 4300u);
    check(tick_tone_composer(&app, 184300u), "send timeout releases UI");
    check(s_display.sid == 0x229u && app.tone_composer_send_request_id == 42u,
          "timeout is uncertain and keeps result ownership");
    app.route = APP_ROUTE_INCOMING_CALL;
    s_send_result_ready = true; s_send_outcome = MODEM_SMS_OUTCOME_OK;
    (void)tick_tone_composer(&app, 184400u);
    check(app.route == APP_ROUTE_INCOMING_CALL && app.tone_composer_send_request_id == 0u,
          "late success is drained without replacing a call");

    memset(&app, 0, sizeof(app));
    app.tones_profile_index = 0u;
    app.route = APP_ROUTE_TONE_COMPOSER;
    app.tone_composer_duration_index = 2u;
    app.tone_composer_octave = 1u;
    (void)handle_tone_composer_key(&app, KEY_3, EVENT_KEY_DOWN, 5000u);
    size_t posts_after_note = s_post_count;
    check(!tick_tone_composer(&app, 5179u), "preview remains active before 180 ms");
    check(s_post_count == posts_after_note, "preview deadline posts nothing early");
    (void)tick_tone_composer(&app, 5180u);
    check(s_posts[s_post_count - 1u].cmd == CORE1_CMD_AUDIO_COMPOSER_PREVIEW_STOP,
          "preview deadline posts its owner-scoped stop at 180 ms");
    check(app.tone_composer_preview_stop_ms == 0u, "preview deadline clears itself");

    app.route = APP_ROUTE_TONE_COMPOSER;
    (void)handle_tone_composer_key(&app, KEY_4, EVENT_KEY_DOWN, 5200u);
    posts_after_note = s_post_count;
    app.route = APP_ROUTE_CALL;
    (void)tick_tone_composer(&app, 5201u);
    check(s_post_count == posts_after_note + 1u &&
              s_posts[s_post_count - 1u].cmd == CORE1_CMD_AUDIO_COMPOSER_PREVIEW_STOP &&
              app.tone_composer_preview_stop_ms == 0u,
          "incoming-call route preemption retires the preview immediately and conditionally");

    app.route = APP_ROUTE_TONE_COMPOSER;
    (void)handle_tone_composer_key(&app, KEY_5, EVENT_KEY_DOWN, 5300u);
    posts_after_note = s_post_count;
    app.route = APP_ROUTE_CLOCK_ALARM;
    (void)tick_tone_composer(&app, 5301u);
    check(s_post_count == posts_after_note + 1u &&
              s_posts[s_post_count - 1u].cmd == CORE1_CMD_AUDIO_COMPOSER_PREVIEW_STOP &&
              app.tone_composer_preview_stop_ms == 0u,
          "alarm route preemption retires the preview immediately and conditionally");

    app.route = APP_ROUTE_TONE_COMPOSER;
    app.tone_composer_cursor_visible = true;
    app.tone_composer_last_cursor_ms = 6000u;
    check(!tick_tone_composer(&app, 6511u), "cursor remains stable before 512 ms");
    check(tick_tone_composer(&app, 6512u), "cursor toggles at 512 ms");
    check(!app.tone_composer_cursor_visible && app.tone_composer_last_cursor_ms == 6512u,
          "cursor timer publishes its new phase and epoch");
}

static void test_capacity_edits_are_atomic(void) {
    app_t app;
    char original[STORE_OWN_TONE_NOTES_MAX + 1u];

    reset_fixture();
    memset(&app, 0, sizeof(app));
    make_capacity_score(app.tone_composer_notes, sizeof(app.tone_composer_notes));
    copy_text(original, sizeof(original), app.tone_composer_notes);
    app.route = APP_ROUTE_TONE_COMPOSER;
    app.tone_composer_cursor_index = 1u;
    size_t posts_before = s_post_count;
    (void)handle_tone_composer_key(&app, KEY_HASH, EVENT_KEY_DOWN, 7000u);
    check_str(app.tone_composer_notes, original,
              "sharp overflow preserves every byte of the original score");
    check(app.tone_composer_cursor_index == 1u,
          "sharp overflow preserves the insertion cursor");
    check(s_post_count == posts_before,
          "sharp overflow never starts a preview or stops unrelated audio");
    check(s_display.sid_count == 1u && s_display.sid == 0x280u &&
              s_display.return_route == APP_ROUTE_TONE_COMPOSER,
          "sharp overflow reports the localized Memory full note");

    reset_fixture();
    memset(&app, 0, sizeof(app));
    make_capacity_score(app.tone_composer_notes, sizeof(app.tone_composer_notes));
    copy_text(original, sizeof(original), app.tone_composer_notes);
    app.route = APP_ROUTE_TONE_COMPOSER;
    app.tone_composer_cursor_index = 1u;
    app.tone_composer_hold_key = KEY_1;
    posts_before = s_post_count;
    (void)handle_tone_composer_key(&app, KEY_1, EVENT_KEY_HOLD, 7010u);
    check_str(app.tone_composer_notes, original,
              "dot overflow preserves every byte of the original score");
    check(app.tone_composer_cursor_index == 1u && app.tone_composer_hold_consumed,
          "dot overflow preserves the cursor and consumes the one hold gesture");
    check(s_post_count == posts_before,
          "dot overflow never starts a preview or stops unrelated audio");
    check(s_display.sid_count == 1u && s_display.sid == 0x280u &&
              s_display.return_route == APP_ROUTE_TONE_COMPOSER,
          "dot overflow reports the localized Memory full note");

    reset_fixture();
    memset(&app, 0, sizeof(app));
    make_capacity_score(app.tone_composer_notes, sizeof(app.tone_composer_notes));
    copy_text(original, sizeof(original), app.tone_composer_notes);
    app.route = APP_ROUTE_TONE_COMPOSER;
    app.tone_composer_cursor_index = 1u;
    app.tone_composer_duration_index = 2u;
    app.tone_composer_octave = 1u;
    app.tone_composer_sharp_armed = true;
    app.tone_composer_dotted_armed = true;
    posts_before = s_post_count;
    (void)handle_tone_composer_key(&app, KEY_2, EVENT_KEY_DOWN, 7020u);
    check_str(app.tone_composer_notes, original,
              "insert overflow preserves every byte of the original score");
    check(app.tone_composer_cursor_index == 1u &&
              app.tone_composer_sharp_armed && app.tone_composer_dotted_armed,
          "insert overflow preserves cursor and armed modifiers");
    check(app.tone_composer_preview_stop_ms == 0u && s_post_count == posts_before,
          "insert overflow does not preview a note that was not inserted");
    check(s_display.sid_count == 1u && s_display.sid == 0x280u &&
              s_display.return_route == APP_ROUTE_TONE_COMPOSER,
          "insert overflow reports the localized Memory full note");
}

static void test_long_score_render_reaches_tail(void) {
    enum { NOTE_COUNT = 68 };
    reset_fixture();

    app_t app;
    memset(&app, 0, sizeof(app));
    make_repeated_score(app.tone_composer_notes,
                        sizeof(app.tone_composer_notes), NOTE_COUNT);
    app.tone_composer_cursor_index = NOTE_COUNT;
    app.tone_composer_cursor_visible = true;

    framebuffer_t actual;
    memset(&actual, 0, sizeof(actual));
    render_tone_composer(&app, &actual);

    uint8_t per_line = plain_tokens_per_composer_line();
    check(per_line != 0u, "Composer font fits at least one note per line");
    if (per_line == 0u) {
        return;
    }
    uint8_t line_count = (uint8_t)((NOTE_COUNT + per_line - 1u) / per_line);
    check(line_count > 12u,
          "long-score fixture exceeds the retired twelve-line render limit");
    uint8_t first_visible_token = (uint8_t)((line_count - 3u) * per_line);
    uint8_t visible_token_count = (uint8_t)(NOTE_COUNT - first_visible_token);

    app_t expected_app;
    memset(&expected_app, 0, sizeof(expected_app));
    make_repeated_score(expected_app.tone_composer_notes,
                        sizeof(expected_app.tone_composer_notes),
                        visible_token_count);
    expected_app.tone_composer_cursor_index = visible_token_count;
    expected_app.tone_composer_cursor_visible = true;

    framebuffer_t expected;
    memset(&expected, 0, sizeof(expected));
    render_tone_composer(&expected_app, &expected);
    check(memcmp(&actual, &expected, sizeof(actual)) == 0,
          "long Composer score renders the real final three rows and cursor");
}

static void test_received_tone_flow(void) {
    reset_fixture();
    app_t app = {0};
    app.route = APP_ROUTE_STANDBY;
    open_received_tone(&app);
    check(app.route == APP_ROUTE_STANDBY, "empty pending queue does not open a phantom tone");
    composer_note_event_t note = {.octave=1u, .pitch_code=1u, .duration_code=2u};
    check(composer_codec_encode("Test", &note, 1u, 16u, s_received_tone.packed,
              sizeof(s_received_tone.packed), &s_received_tone.packed_len), "incoming UI melody encodes");
    s_pending_id = 7u;
    open_received_tone(&app);
    check(app.route == APP_ROUTE_RECEIVED_TONE && app.ringtone_receive_id == 7u,
          "incoming notice opens its durable pending tone");
    render_received_tone(&app, NULL);
    check(s_list_count == 3u && strcmp(s_list_labels[0], "Playback") == 0 &&
          strcmp(s_list_labels[1], "Save") == 0 && strcmp(s_list_labels[2], "Discard") == 0,
          "incoming options preserve the original Playback / Save / Discard menu");
    handle_received_tone_key(&app, KEY_NAVI, 100u);
    check(s_packed_posts == 1u && s_display.record_id == 37u && app.ringtone_playing,
          "Playback starts binary melody with the original striped Quit dialog");
    check_str(s_display.a, "Playing tone\nTest", "Playback formats the localized title template");
    check(handle_received_tone_display_key(&app, KEY_NAVI), "Quit owns the playback dialog");
    check(app.route == APP_ROUTE_RECEIVED_TONE && !app.ringtone_playing &&
          s_posts[s_post_count - 1u].cmd == CORE1_CMD_AUDIO_COMPOSER_STOP,
          "Quit stops playback and returns to incoming options");
    handle_received_tone_key(&app, KEY_NAVI, 200u);
    size_t before = s_post_count;
    app.route = APP_ROUTE_CALL;
    tick_received_tone(&app, 300u);
    check(!app.ringtone_playing && s_post_count == before && app.route == APP_ROUTE_CALL,
          "call preemption never stops the new call's audio");
    open_received_tone(&app);
    handle_received_tone_key(&app, KEY_NAVI, 400u);
    tick_received_tone(&app, app.ringtone_play_until_ms);
    check(app.route == APP_ROUTE_RECEIVED_TONE && !app.ringtone_playing,
          "finite playback returns to options at its decoded duration");
    handle_received_tone_key(&app, KEY_DOWN, 500u);
    s_commit_status = STORE_STATUS_NOT_READY;
    handle_received_tone_key(&app, KEY_NAVI, 500u);
    unsigned dialogs = s_display.sid_count;
    tick_received_tone(&app, 600u);
    check(s_received_saves == 1u && app.ringtone_save_action == 1u && s_display.sid_count == dialogs,
          "Save waits for durable commit without claiming success early");
    s_commit_status = STORE_STATUS_OK;
    tick_received_tone(&app, 700u);
    check(s_display.sid == 0x224u && s_display.record_id == 3u &&
          s_display.return_route == APP_ROUTE_STANDBY, "durable Save uses the original saved note");
    open_received_tone(&app);
    app.ringtone_option = 1u;
    s_save_status = STORE_STATUS_STORAGE_ERROR;
    handle_received_tone_key(&app, KEY_NAVI, 800u);
    check(s_display.sid == 0x3b3u && app.ringtone_save_action == 0u,
          "failed Save reports Not saved and leaves the incoming tone retryable");
    s_save_status = STORE_STATUS_OK;
    open_received_tone(&app);
    app.ringtone_option = 2u;
    handle_received_tone_key(&app, KEY_NAVI, 900u);
    tick_received_tone(&app, 1000u);
    check(s_received_discards == 1u && app.route == APP_ROUTE_STANDBY,
          "durable Discard returns to standby without a saved note");
    open_received_tone(&app);
    app.ringtone_option = 1u;
    s_commit_status = STORE_STATUS_NOT_READY;
    handle_received_tone_key(&app, KEY_NAVI, 1100u);
    handle_received_tone_key(&app, KEY_C, 1200u);
    check(app.route == APP_ROUTE_STANDBY, "C can leave a pending flash write without undoing it");
    app.route = APP_ROUTE_CALL;
    s_commit_status = STORE_STATUS_OK;
    tick_received_tone(&app, 1300u);
    check(app.route == APP_ROUTE_CALL && app.ringtone_save_action == 0u,
          "late save completion cannot hijack a call");
}

int main(void) {
    test_default_open_and_note_editing();
    test_saved_tone_playback_and_persistence();
    test_ringtone_preview_debounce();
    test_tones_setting_list_contract();
    test_level5_ringing_volume_requires_confirmation();
    test_option_routes_and_tempo_selection();
    test_send_timeout_and_timers();
    test_capacity_edits_are_atomic();
    test_long_score_render_reaches_tail();
    test_received_tone_flow();

    if (s_failures != 0) {
        fprintf(stderr, "%d failures\n", s_failures);
        return 1;
    }
    printf("tone composer app tests passed\n");
    return 0;
}

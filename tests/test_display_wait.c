/* A message-body read uses the sticky v6.00 "Opening" progress note. It must
 * remain on screen until the async read result (or its timeout) resolves it;
 * otherwise C/Navi can expose the list while sms_read_waiting remains true. */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "apps/call_divert_app.h"
#include "apps/dialogs_app.h"
#include "apps/phonebook_app.h"
#include "apps/profiles_app.h"
#include "apps/tones_app.h"
#include "audio/audio_levels.h"
#include "services/core1_services.h"
#include "services/input_keys.h"
#include "services/modem_service.h"
#include "services/strings.h"
#include "storage/store_service.h"
#include "ui/assets.h"
#include "ui/framebuffer.h"

static int s_failures;
static unsigned s_tones_confirm_calls;
static bool s_tones_confirm_accepted;
static uint32_t s_tones_confirm_now;
static unsigned s_picture_confirm_calls;
static bool s_picture_confirm_save;

void messages_picture_confirm_save(app_t *app, bool save, uint32_t now) {
    (void)app;
    (void)now;
    s_picture_confirm_calls++;
    s_picture_confirm_save = save;
}

static void check(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        s_failures++;
    }
}

bool handle_call_divert_display_key(app_t *app, uint16_t key, uint32_t now) {
    (void)app;
    (void)key;
    (void)now;
    return false;
}

uint16_t audio_arg(uint8_t code, uint8_t level) {
    return (uint16_t)(code | ((uint16_t)level << 8u));
}

void core1_post_command(core1_cmd_t cmd, uint16_t arg) {
    (void)cmd;
    (void)arg;
}

uint8_t profile_active_index(void) {
    return 0u;
}

uint8_t profile_get_tone_setting(uint8_t profile_index,
                                 profile_setting_kind_t kind) {
    (void)profile_index;
    (void)kind;
    return 0u;
}

const char *ts(uint16_t sid) {
    (void)sid;
    return NULL;
}

bool modem_service_phonebook_entry(uint16_t position,
                                   modem_phonebook_entry_t *out) {
    (void)position;
    (void)out;
    return false;
}

bool start_phonebook_delete(app_t *app, uint16_t index, uint32_t now) {
    (void)app;
    (void)index;
    (void)now;
    return false;
}

uint8_t store_call_count(store_call_list_t list) {
    (void)list;
    return 0u;
}

store_status_t store_call_delete(store_call_list_t list, uint8_t index) {
    (void)list;
    (void)index;
    return STORE_STATUS_OK;
}

uint8_t store_picture_message_count(void) {
    return 0u;
}

store_status_t store_picture_message_clear(uint8_t slot) {
    (void)slot;
    return STORE_STATUS_OK;
}

void tones_confirm_ringing_volume(app_t *app, bool accepted, uint32_t now) {
    s_tones_confirm_calls++;
    s_tones_confirm_accepted = accepted;
    s_tones_confirm_now = now;
    app->confirm_context = CONFIRM_CONTEXT_NONE;
}

void draw_softkey(framebuffer_t *fb, const char *label) {
    (void)fb;
    (void)label;
}

static int added_cursor_column(const framebuffer_t *without_cursor,
                               const framebuffer_t *with_cursor) {
    int column = -1;
    for (int y = 0; y < (int)FB_HEIGHT; y++) {
        for (int x = 0; x < (int)FB_WIDTH; x++) {
            if (!fb_get_pixel(without_cursor, x, y) &&
                fb_get_pixel(with_cursor, x, y)) {
                if (column >= 0 && column != x) {
                    return -2;
                }
                column = x;
            }
        }
    }
    return column;
}

static int render_number_cursor_column(const char *number,
                                       uint8_t cursor_index) {
    app_t app;
    framebuffer_t without_cursor;
    framebuffer_t with_cursor;
    memset(&app, 0, sizeof(app));
    strcpy(app.editor_title, "Number:");
    strcpy(app.editor_value, number);
    app.editor_kind = EDITOR_KIND_NUMBER;
    app.editor_show_cursor = true;
    app.editor_cursor_index = cursor_index;

    app.editor_cursor_visible = false;
    render_editor(&app, &without_cursor);
    app.editor_cursor_visible = true;
    render_editor(&app, &with_cursor);
    return added_cursor_column(&without_cursor, &with_cursor);
}

static void test_number_editor_cursor_uses_glyph_gap(void) {
    const font_t *large = asset_font(FONT_FS0);
    const char *large_number = "12";
    int large_x = 3 + 80 - asset_text_width(large, large_number);
    int large_expected = large_x + asset_text_width(large, "1") - 1;
    check(render_number_cursor_column(large_number, 1u) == large_expected,
          "large-number cursor occupies the preceding glyph's blank column");

    const font_t *small = asset_font(FONT_FS2);
    const char *small_number = "123456789012";
    int small_x = 3 + 80 - asset_text_width(small, small_number);
    int small_expected = small_x + asset_text_width(small, "12345") - 1;
    check(render_number_cursor_column(small_number, 5u) == small_expected,
          "small-number cursor occupies the preceding glyph's blank column");
}

static void test_sms_read_progress_is_sticky(void) {
    app_t app;
    memset(&app, 0, sizeof(app));
    app.route = APP_ROUTE_DISPLAY_MESSAGE;
    app.display_return_route = APP_ROUTE_MESSAGES_LIST;
    app.display_record_id = 4u;
    app.sms_read_waiting = true;

    check(handle_display_message_key(&app, KEY_C, 100u),
          "C is consumed while an SMS body read is pending");
    check(app.route == APP_ROUTE_DISPLAY_MESSAGE && app.display_record_id == 4u,
          "C cannot dismiss the Opening note during an SMS body read");

    check(handle_display_message_key(&app, KEY_NAVI, 110u),
          "Navi is consumed while an SMS body read is pending");
    check(app.route == APP_ROUTE_DISPLAY_MESSAGE && app.display_record_id == 4u,
          "Navi cannot dismiss the Opening note during an SMS body read");

    app.sms_read_waiting = false;
    check(handle_display_message_key(&app, KEY_C, 120u),
          "C still dismisses an ordinary display note");
    check(app.route == APP_ROUTE_MESSAGES_LIST && app.display_record_id == 0u,
          "ordinary display dismissal keeps its existing return behavior");
}

static void test_phonebook_abort_detaches_request_identity(void) {
    app_t app;
    memset(&app, 0, sizeof(app));
    app.route = APP_ROUTE_DISPLAY_MESSAGE;
    app.display_return_route = APP_ROUTE_PHONEBOOK_MENU;
    app.display_record_id = PHONEBOOK_REQUEST_DISPLAY_RECORD_ID;
    app.phonebook_pending_kind = PHONEBOOK_PENDING_LIST;
    app.phonebook_request_id = 77u;
    app.phonebook_wait_display_request_id = 77u;
    app.phonebook_request_started_ms = 10u;

    check(handle_display_message_key(&app, KEY_C, 100u) &&
              app.route == APP_ROUTE_PHONEBOOK_MENU &&
              app.phonebook_pending_kind == PHONEBOOK_PENDING_NONE &&
              app.phonebook_request_id == 0u &&
              app.phonebook_wait_display_request_id == 0u &&
              app.phonebook_request_started_ms == 0u,
          "C detaches the phonebook token before leaving its progress note");
}

static void test_editor_delete_keeps_codepoints_whole(void) {
    app_t app;
    memset(&app, 0, sizeof(app));
    strcpy(app.editor_value, "A\xce\x94\xe2\x82\xac" "B");
    app.editor_cursor_index = 6u; /* immediately after the three-byte euro */
    check(editor_delete_one(&app) &&
              strcmp(app.editor_value, "A\xce\x94" "B") == 0 &&
              app.editor_cursor_index == 3u,
          "generic editor C removes one complete multibyte glyph");

    strcpy(app.editor_value, "A\xce\x94\xe2\x82\xac" "B");
    app.editor_cursor_index = 5u; /* corrupted/interior euro-byte position */
    check(editor_delete_one(&app) &&
              strcmp(app.editor_value, "A\xe2\x82\xac" "B") == 0 &&
              app.editor_cursor_index == 1u,
          "generic editor clamps an interior cursor before deleting");
}

static void test_ringing_volume_confirmation_dispatch(void) {
    app_t app;
    memset(&app, 0, sizeof(app));
    app.route = APP_ROUTE_CONFIRM;
    app.confirm_context = CONFIRM_CONTEXT_TONES_RINGING_VOLUME;
    s_tones_confirm_calls = 0u;

    check(handle_confirm_key(&app, KEY_C, 700u),
          "C is consumed by the ringing-volume warning");
    check(s_tones_confirm_calls == 1u && !s_tones_confirm_accepted &&
              s_tones_confirm_now == 700u,
          "C dispatches the Level-5 Back continuation exactly once");

    app.route = APP_ROUTE_CONFIRM;
    app.confirm_context = CONFIRM_CONTEXT_TONES_RINGING_VOLUME;
    check(handle_confirm_key(&app, KEY_NAVI, 710u),
          "Navi is consumed by the ringing-volume warning");
    check(s_tones_confirm_calls == 2u && s_tones_confirm_accepted &&
              s_tones_confirm_now == 710u,
          "Navi dispatches the Level-5 OK continuation exactly once");
}

int main(void) {
    app_t picture = {0};
    picture.confirm_context = CONFIRM_CONTEXT_PICTURE_MESSAGE_SAVE_FIRST;
    (void)handle_confirm_key(&picture, KEY_DOWN, 10u);
    check(s_picture_confirm_calls == 0u, "scroll does not accept save-first confirmation");
    (void)handle_confirm_key(&picture, KEY_NAVI, 20u);
    check(s_picture_confirm_calls == 1u && s_picture_confirm_save,
          "save-first OK dispatches Save");
    (void)handle_confirm_key(&picture, KEY_C, 30u);
    check(s_picture_confirm_calls == 2u && !s_picture_confirm_save,
          "save-first Exit dispatches discard");
    test_sms_read_progress_is_sticky();
    test_phonebook_abort_detaches_request_identity();
    test_editor_delete_keeps_codepoints_whole();
    test_number_editor_cursor_uses_glyph_gap();
    test_ringing_volume_confirmation_dispatch();
    if (s_failures != 0) {
        fprintf(stderr, "%d display-wait test(s) failed\n", s_failures);
        return 1;
    }
    puts("display wait tests passed");
    return 0;
}

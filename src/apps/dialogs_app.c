#include "services/phonebook_service.h"
#include "apps/dialogs_app.h"

#include <stdio.h>
#include <string.h>

#include "apps/call_divert_app.h"
#include "apps/calls_app.h"
#include "apps/messages_app.h"
#include "apps/phonebook_app.h"
#include "apps/profiles_app.h"
#include "apps/settings_app.h"
#include "apps/tones_app.h"
#include "services/strings.h"
#include "audio/audio_levels.h"
#include "services/core1_services.h"
#include "services/input_keys.h"
#include "services/key_utils.h"
#include "services/modem_service.h"
#include "storage/store_service.h"
#include "services/timebase.h"
#include "ui/text_layout.h"

/* ---- Display-message record metadata (v6.00 table 0x002d83b0) ----
 * Decoded columns per record: text window, graphic kind/id, auto-dismiss
 * delay (table byte x 8 ms) and the system tone dispatched by display_msg
 * core 0x002a7556 via send_message(0x8190, tone, 0xf1). Tone 0x21 is the
 * documented "silent" sentinel. */

#define DISPLAY_TONE_SILENT 0x21u

typedef enum {
    DISPLAY_GFX_NONE = 0,
    DISPLAY_GFX_BITMAP,            /* static bitmap in window 3 at x=62 y=0 */
    DISPLAY_GFX_ANIM_STRIPE,       /* anim 1: progress stripe, window 5 */
    DISPLAY_GFX_ANIM_SENDING,      /* anim 5: SMS sending envelope fill */
    DISPLAY_GFX_ANIM_SENT,         /* anim 6: SMS sent envelope */
    DISPLAY_GFX_ANIM_CHECK,        /* anim 7: success checkmark */
    DISPLAY_GFX_ANIM_KEYGUARD_HELP,   /* anim 9 */
    DISPLAY_GFX_ANIM_KEYGUARD_LOCKED, /* anim 10 */
    DISPLAY_GFX_ANIM_TRASH,        /* anim 13: trashcan */
    DISPLAY_GFX_ANIM_BATTERY_LOW,  /* anim 3 */
    DISPLAY_GFX_ANIM_CHARGING,     /* anim 4 (240 ms/frame) */
} display_gfx_kind_t;

typedef enum {
    DISPLAY_TEXT_WIN12 = 0,  /* FS0 x0 y3 pitch 13, w84 (graphic is a separate overlay) */
    DISPLAY_TEXT_WIN13,      /* FS2 x0 y7 w84 h30, pitch 9 */
    DISPLAY_TEXT_WIN64,      /* FS2 x6 w72, pitch 9, EV-centered in y7 h30 */
    DISPLAY_TEXT_PROGRESS,   /* FS2 under the stripe box (records 0x04/0x23) */
} display_text_window_t;

typedef struct {
    uint8_t record_id;
    uint8_t text_window;  /* display_text_window_t */
    uint8_t gfx_kind;     /* display_gfx_kind_t */
    uint8_t tone;         /* system tone index; DISPLAY_TONE_SILENT = none */
    uint16_t gfx_bitmap;  /* DISPLAY_GFX_BITMAP only */
    uint16_t delay_ms;    /* 0 = stays until a key / the flow dismisses it */
} display_record_meta_t;

static const display_record_meta_t DISPLAY_RECORDS[] = {
    /* id   text window            graphic                          tone                 bitmap delay */
    {0u,  DISPLAY_TEXT_WIN12, DISPLAY_GFX_BITMAP,               5u,                   54u, 3080u},
    {1u,  DISPLAY_TEXT_WIN12, DISPLAY_GFX_BITMAP,               6u,                   52u, 3080u},
    {2u,  DISPLAY_TEXT_WIN12, DISPLAY_GFX_BITMAP,               10u,                  53u, 3080u},
    {3u,  DISPLAY_TEXT_WIN12, DISPLAY_GFX_ANIM_CHECK,           DISPLAY_TONE_SILENT,  0u,  1536u},
    {4u,  DISPLAY_TEXT_PROGRESS, DISPLAY_GFX_ANIM_STRIPE,       DISPLAY_TONE_SILENT,  0u,  0u},
    {5u,  DISPLAY_TEXT_WIN12, DISPLAY_GFX_BITMAP,               DISPLAY_TONE_SILENT,  53u, 3080u},
    {6u,  DISPLAY_TEXT_WIN12, DISPLAY_GFX_BITMAP,               10u,                  53u, 3080u},
    {12u, DISPLAY_TEXT_WIN12, DISPLAY_GFX_BITMAP,               DISPLAY_TONE_SILENT,  53u, 3080u},
    {13u, DISPLAY_TEXT_WIN12, DISPLAY_GFX_BITMAP,               6u,                   52u, 0u},
    /* Battery/charger dialog family. */
    {14u, DISPLAY_TEXT_WIN12, DISPLAY_GFX_BITMAP,               DISPLAY_TONE_SILENT,  166u, 3080u},
    {15u, DISPLAY_TEXT_WIN12, DISPLAY_GFX_BITMAP,               8u,                   170u, 0u},
    {16u, DISPLAY_TEXT_WIN12, DISPLAY_GFX_ANIM_BATTERY_LOW,     7u,                   0u,  6168u},
    {17u, DISPLAY_TEXT_WIN12, DISPLAY_GFX_BITMAP,               DISPLAY_TONE_SILENT,  171u, 0u},
    {18u, DISPLAY_TEXT_WIN12, DISPLAY_GFX_ANIM_CHARGING,        DISPLAY_TONE_SILENT,  0u,  6168u},
    {21u, DISPLAY_TEXT_WIN12, DISPLAY_GFX_BITMAP,               5u,                   54u, 0u},
    {22u, DISPLAY_TEXT_WIN12, DISPLAY_GFX_ANIM_KEYGUARD_LOCKED, DISPLAY_TONE_SILENT,  0u,  3080u},
    {23u, DISPLAY_TEXT_WIN12, DISPLAY_GFX_ANIM_KEYGUARD_HELP,   DISPLAY_TONE_SILENT,  0u,  3080u},
    {24u, DISPLAY_TEXT_WIN12, DISPLAY_GFX_BITMAP,               10u,                  53u, 3080u},
    {25u, DISPLAY_TEXT_WIN12, DISPLAY_GFX_BITMAP,               10u,                  51u, 1536u},
    {26u, DISPLAY_TEXT_WIN12, DISPLAY_GFX_ANIM_CHECK,           DISPLAY_TONE_SILENT,  0u,  3080u},
    {31u, DISPLAY_TEXT_WIN12, DISPLAY_GFX_NONE,                 DISPLAY_TONE_SILENT,  0u,  0u},
    {32u, DISPLAY_TEXT_WIN13, DISPLAY_GFX_NONE,                 DISPLAY_TONE_SILENT,  0u,  0u},
    {35u, DISPLAY_TEXT_PROGRESS, DISPLAY_GFX_ANIM_STRIPE,       DISPLAY_TONE_SILENT,  0u,  0u},
    {36u, DISPLAY_TEXT_PROGRESS, DISPLAY_GFX_ANIM_STRIPE,       DISPLAY_TONE_SILENT,  0u,  0u},
    {CALL_DIVERT_REQUEST_RECORD_ID,
          DISPLAY_TEXT_PROGRESS, DISPLAY_GFX_ANIM_STRIPE,       DISPLAY_TONE_SILENT,  0u,  0u},
    {42u, DISPLAY_TEXT_WIN12, DISPLAY_GFX_BITMAP,               DISPLAY_TONE_SILENT,  49u, 1536u},
    {44u, DISPLAY_TEXT_WIN12, DISPLAY_GFX_ANIM_TRASH,           DISPLAY_TONE_SILENT,  0u,  3080u},
    /* 0x2e "Sending message": table delay is 1536 ms, but pico holds the
     * notice until the async modem send completes (J1 decision, CHANGELOG). */
    {46u, DISPLAY_TEXT_WIN12, DISPLAY_GFX_ANIM_SENDING,         DISPLAY_TONE_SILENT,  0u,  0u},
    {47u, DISPLAY_TEXT_WIN12, DISPLAY_GFX_ANIM_SENT,            DISPLAY_TONE_SILENT,  0u,  1536u},
    {49u, DISPLAY_TEXT_WIN64, DISPLAY_GFX_NONE,                 DISPLAY_TONE_SILENT,  0u,  1536u},
    {50u, DISPLAY_TEXT_WIN64, DISPLAY_GFX_NONE,                 5u,                   0u,  3080u},
    {51u, DISPLAY_TEXT_WIN64, DISPLAY_GFX_NONE,                 DISPLAY_TONE_SILENT,  0u,  3080u},
    {52u, DISPLAY_TEXT_WIN12, DISPLAY_GFX_BITMAP,               10u,                  54u, 3080u},
};

/* Frame tables straight from assets/animations/anim_NN.json (all 200 ms/frame
 * "once" mode except keyguard help at 160 ms). */
static const uint16_t CHECK_FRAMES[] = {193u, 193u, 193u, 194u, 195u, 195u, 195u, 195u, 195u, 195u};
static const uint16_t SENDING_FRAMES[] = {179u, 180u, 181u, 182u, 183u, 184u, 185u, 185u, 185u, 185u, 185u};
static const uint16_t SENT_FRAMES[] = {185u, 186u, 187u, 188u, 189u, 190u, 191u, 192u, 192u, 192u, 192u};
static const uint16_t TRASH_FRAMES[] = {231u, 232u, 233u, 234u, 235u, 236u, 233u, 232u, 231u, 231u, 231u, 231u};
static const uint16_t KEYGUARD_HELP_FRAMES[] = {200u, 200u, 200u, 201u, 201u, 201u, 202u, 203u, 204u, 205u, 206u, 206u, 206u, 200u, 200u, 200u, 200u, 200u, 200u};
static const uint16_t KEYGUARD_LOCK_FRAMES[] = {207u, 207u, 208u, 209u, 210u, 210u, 210u, 210u, 210u};
static const uint16_t BATTERY_LOW_FRAMES[] = {167u, 168u, 167u, 168u, 169u, 170u, 169u, 170u, 169u};
static const uint16_t CHARGING_FRAMES[] = {171u, 172u, 173u, 174u, 173u, 172u, 175u, 176u, 177u, 178u, 175u, 176u, 177u, 178u, 178u, 178u};

static bool editor_input_key(app_t *app, uint16_t key, event_type_t event_type, uint32_t now);
static bool editor_insert_char(app_t *app, char ch);
static bool editor_move_cursor(app_t *app, int8_t delta);
static int editor_number_cursor_x(const font_t *font,
                                  const char *text,
                                  size_t row_start,
                                  size_t cursor,
                                  int text_x);
static bool editor_number_search_key(app_t *app, uint16_t key);
static void editor_reset_multitap(app_t *app);
static const display_record_meta_t *display_record_meta(uint8_t record_id);
static const uint16_t *display_anim_frames(uint8_t gfx_kind, uint8_t *out_count);
static void play_display_record_tone(uint8_t index);

bool handle_display_message_key(app_t *app, uint16_t key, uint32_t now) {
    bool phonebook_wait_owned = phonebook_request_display_owned(app);
    bool phonebook_erase_owned = phonebook_erase_all_display_owned(app);
    if (phonebook_wait_owned ||
        phonebook_erase_owned ||
        app->phonebook_send_waiting ||
        app->tone_composer_send_waiting ||
        app->messages_open_pending ||
        app->messages_picture_send_waiting ||
        app->sms_read_waiting ||
        app->sms_send_waiting ||
        app->sms_delete_waiting ||
        app->sms_save_waiting) {
        /* C aborts a phonebook read stuck behind the sticky "Opening" progress so
         * a dead/slow modem can't trap the UI (the original never hangs here);
         * poll_phonebook also times it out. Other modem waits keep their existing
         * behaviour. Erase-all self-completes via tick_phonebook_erase_all. */
        if (key == KEY_C &&
            phonebook_wait_owned &&
            app->phonebook_erase_all_started_ms == 0u) {
            app->phonebook_pending_kind = PHONEBOOK_PENDING_NONE;
            app->phonebook_request_id = 0u;
            app->phonebook_wait_display_request_id = 0u;
            app->phonebook_request_started_ms = 0u;
            return_from_display(app);
            app->dirty = true;
        }
        return true;
    }
    if (handle_call_divert_display_key(app, key, now)) {
        return true;
    }
    if (key == KEY_C || key == KEY_NAVI) {
        return_from_display(app);
        app->dirty = true;
        return true;
    }
    return true;
}

void render_display_message(const app_t *app, framebuffer_t *fb) {
    fb_clear(fb, false);
    const display_record_meta_t *meta = display_record_meta(app->display_record_id);

    if (meta->gfx_kind == DISPLAY_GFX_ANIM_STRIPE) {
        const font_t *font = asset_font(FONT_FS2);
        fb_rect(fb, 0, 7, 84, 8, true);
        const bitmap_t *item = asset_bitmap(164u);
        if (item != 0) {
            /* Original renderer 0x002304c8 rotates bitmap 0164 RIGHT by phase:
             * dest column i shows source column (i - phase) mod width. */
            const uint8_t *data = asset_bitmap_data(item);
            int width = item->width;
            for (int i = 0; i < width; i++) {
                int src = i - (int)app->display_progress_phase;
                if (src < 0) {
                    src += width;
                }
                uint8_t bits = data[src];
                for (int row = 0; row < item->height; row++) {
                    if (bits & (1u << row)) {
                        fb_pixel(fb, 2 + i, 9 + row, true);
                    }
                }
            }
        }
        char lines[2][32];
        uint8_t count = wrap_text_lines_ex(font, app->display_text, FB_WIDTH, (char *)lines, 32u, 2u);
        for (uint8_t i = 0; i < count; i++) {
            fb_text(fb, font, lines[i], 0, 18 + i * 9, true, FB_WIDTH);
        }
        if (app->display_record_id == CALL_DIVERT_REQUEST_RECORD_ID &&
            app->call_divert_pending_action != 0u) {
            draw_softkey(fb, "Quit");
        }
        return;
    }

    if (meta->text_window == DISPLAY_TEXT_WIN13 || meta->text_window == DISPLAY_TEXT_WIN64) {
        /* Window 13: x0 y7 w84 h30; window 64: x6 y7 w72 h30 with the line
         * block vertically centered (<EV>). Both FS2, 9 px pitch, <=3 lines.
         * Wrap the localized text to the window width so long lines flow onto
         * the next row instead of being clipped/split. */
        const font_t *font = asset_font(FONT_FS2);
        int x = meta->text_window == DISPLAY_TEXT_WIN64 ? 6 : 0;
        int width = meta->text_window == DISPLAY_TEXT_WIN64 ? 72 : 84;
        char lines[3][32];
        uint8_t count = wrap_text_lines_ex(font, app->display_text, width, (char *)lines, 32u, 3u);
        int first_y = 7;
        if (meta->text_window == DISPLAY_TEXT_WIN64 && count > 0u) {
            int block_h = (int)(count - 1u) * 9 + (int)font->height;
            first_y = 7 + (30 - block_h) / 2;
        }
        for (uint8_t i = 0; i < count; i++) {
            fb_text(fb, font, lines[i], x, first_y + i * 9, true, width);
        }
        if (app->display_record_id == CALL_DIVERT_DETAIL_RECORD_ID) {
            /* v6.00 record 0x20 selects action 0x14: Back/<blank>. */
            draw_softkey(fb, ts_or(0x2d8u, "Back"));
        }
        return;
    }

    /* Window 12: FS0 text with an optional graphic in window 3 at x=62 y=0. */
    uint16_t bitmap = 0u;
    if (meta->gfx_kind == DISPLAY_GFX_BITMAP) {
        bitmap = meta->gfx_bitmap;
    } else if (meta->gfx_kind != DISPLAY_GFX_NONE) {
        uint8_t frame_count = 0u;
        const uint16_t *frames = display_anim_frames(meta->gfx_kind, &frame_count);
        if (frames != 0 && frame_count > 0u) {
            uint8_t idx = app->display_frame_index;
            if (idx >= frame_count) {
                idx = (uint8_t)(frame_count - 1u);
            }
            bitmap = frames[idx];
        }
    }
    if (bitmap != 0u) {
        fb_bitmap(fb, bitmap, 62, 0, true, true);
    }
    const font_t *font = asset_font(FONT_FS0);
    /* ROM 0x22e6cc narrows each text row against neighboring graphic bounds
     * (0x22ea62..0x22eaee). Rows below the graphic regain the full width. */
    int widths[3] = {FB_WIDTH, FB_WIDTH, FB_WIDTH};
    const bitmap_t *graphic = bitmap != 0u ? asset_bitmap(bitmap) : NULL;
    for (uint8_t i = 0u; i < 3u; i++) {
        if (graphic != NULL && 3 + i * 13 < graphic->height) {
            widths[i] = 62;
        }
    }
    char lines[3][32];
    uint8_t count = wrap_text_lines_widths(font, app->display_text, widths,
                                         (char *)lines, 32u, 3u);
    for (uint8_t i = 0; i < count; i++) {
        fb_text(fb, font, lines[i], 0, 3 + i * 13, true, widths[i]);
    }
}

void render_confirm(const app_t *app, framebuffer_t *fb) {
    fb_clear(fb, false);
    /* Bool dialog window 44 (x0 y0 w84 h37, attr 58 <FS2><EV><MT2><TLS2>):
     * FS2 small/bold at 9 px pitch. Pinned layouts: "Erase?" question y=4
     * with the name block at y=15; "Are you / sure?" first line y=2. */
    const font_t *font = asset_font(FONT_FS2);
    int y = app->confirm_first_y;
    for (uint8_t i = 0; i < app->confirm_line_count; i++) {
        fb_text(fb, font, app->confirm_lines[i], 0, y, true, FB_WIDTH);
        y = (i == 0u && app->confirm_first_y == 4) ? 15 : y + 9;
    }
    draw_softkey(fb, "OK");
}

void render_editor(const app_t *app, framebuffer_t *fb) {
    fb_clear(fb, false);
    const font_t *small = asset_font(FONT_FS2);
    const font_t *large = asset_font(FONT_FS0);
    fb_bitmap(fb, 14u, 0, 0, true, true);
    if (app->editor_show_mode_badge && app->editor_kind == EDITOR_KIND_TEXT) {
        uint16_t badge = app->editor_mode == EDITOR_MODE_LOWER ? 10u : (app->editor_mode == EDITOR_MODE_NUMERIC ? 12u : 11u);
        fb_bitmap(fb, badge, 14, 0, true, true);
    }
    if (app->editor_kind == EDITOR_KIND_NUMBER) {
        const char *text = app->editor_value;
        uint8_t len = (uint8_t)strlen(text);
        uint8_t cursor_index = app->editor_cursor_index;
        if (cursor_index > len) {
            cursor_index = len;
        }
        const char *title_break = strchr(app->editor_title, '\n');
        if (title_break != 0) {
            /* Two-line traced prompt ("Enter new\nsecurity code:"): prompt
             * lines at y7/y16 and the entry at y28, like the security-code
             * surface — no field box. */
            char line1[20];
            size_t n = (size_t)(title_break - app->editor_title);
            if (n >= sizeof(line1)) {
                n = sizeof(line1) - 1u;
            }
            memcpy(line1, app->editor_title, n);
            line1[n] = '\0';
            fb_text(fb, small, line1, 0, 7, true, FB_WIDTH);
            fb_text(fb, small, title_break + 1, 0, 16, true, FB_WIDTH);
            const font_t *value_font = len <= 10u ? large : small;
            fb_text(fb, value_font, text, 0, 28, true, FB_WIDTH);
            draw_softkey(fb, "OK");
            return;
        }
        bool expanded = len > 26u;
        int field_y = expanded ? 7 : 16;
        int field_h = expanded ? 30 : 21;
        if (!expanded) {
            fb_text(fb, small, app->editor_title, 0, 7, true, FB_WIDTH);
        }
        fb_rect(fb, 0, field_y, 84, field_h, true);
        const font_t *font = len <= 10u ? large : small;
        int inner_x = 3;
        int inner_w = 80;
        int cursor_x = inner_x + 1;
        int cursor_y = expanded ? 9 : 22;
        int cursor_h = font->height + 1;
        if (len <= 10u) {
            int w = asset_text_width(font, text);
            int x = inner_x + inner_w - w;
            if (x < inner_x) {
                x = inner_x;
            }
            fb_text(fb, font, text, x, 23, true, inner_w);
            cursor_x = editor_number_cursor_x(font, text, 0u,
                                               cursor_index, x);
            cursor_y = 22;
        } else {
            int rows_y[3] = {10, 19, 28};
            uint8_t starts[3] = {0u, 0u, 0u};
            uint8_t lens[3] = {0u, 0u, 0u};
            uint8_t bottom_len = len > 13u ? 13u : len;
            uint8_t middle_len = len > 13u ? (uint8_t)(len - 13u > 13u ? 13u : len - 13u) : 0u;
            uint8_t top_len = len > 26u ? (uint8_t)(len - 26u) : 0u;
            starts[2] = (uint8_t)(len - bottom_len);
            lens[2] = bottom_len;
            starts[1] = (uint8_t)(starts[2] - middle_len);
            lens[1] = middle_len;
            starts[0] = 0u;
            lens[0] = top_len;
            uint8_t first = expanded ? 0u : 1u;
            for (uint8_t row = first; row < 3u; row++) {
                if (lens[row] == 0u) {
                    continue;
                }
                char buf[14];
                memcpy(buf, &text[starts[row]], lens[row]);
                buf[lens[row]] = '\0';
                int w = asset_text_width(font, buf);
                int x = inner_x + inner_w - w;
                if (x < inner_x) {
                    x = inner_x;
                }
                fb_text(fb, font, buf, x, rows_y[row], true, inner_w);
                if (cursor_index >= starts[row] && cursor_index <= starts[row] + lens[row]) {
                    cursor_x = editor_number_cursor_x(font, text,
                                                       starts[row],
                                                       cursor_index, x);
                    cursor_y = rows_y[row] - 1;
                }
            }
        }
        if (app->editor_show_cursor && app->editor_cursor_visible) {
            if (cursor_x > 82) {
                cursor_x = 82;
            }
            fb_vline(fb, cursor_x, cursor_y, cursor_h, true);
        }
        const char *softkey = "OK";
        if (app->editor_context == EDITOR_CONTEXT_IN_CALL_DTMF) {
            softkey = "Send";
        } else if ((app->editor_context == EDITOR_CONTEXT_PHONEBOOK_SEND_RECIPIENT ||
                    app->editor_context == EDITOR_CONTEXT_SMS_RECIPIENT ||
                    app->editor_context == EDITOR_CONTEXT_PICTURE_RECIPIENT ||
                    app->editor_context == EDITOR_CONTEXT_TONE_COMPOSER_RECIPIENT ||
                    app->editor_context == EDITOR_CONTEXT_IN_CALL_NEW_CALL) &&
                   app->editor_value[0] == '\0') {
            softkey = "Search";
        }
        draw_softkey(fb, softkey);
        return;
    }

    fb_text(fb, small, app->editor_title, 0, 7, true, FB_WIDTH);
    fb_rect(fb, 0, 16, 84, 21, true);
    const font_t *font = asset_text_width(large, app->editor_value[0] ? app->editor_value : " ") > 82 ? small : large;
    int line_pitch = font == large ? 13 : 9;
    uint16_t line_count = ui_glyph_line_count(font, app->editor_value, 82);
    size_t cursor = ui_text_clamp_boundary(app->editor_value,
                                           app->editor_cursor_index);
    uint16_t cursor_row = 0u;
    ui_text_span_t cursor_line;
    if (!ui_glyph_line_for_offset(font, app->editor_value, 82, cursor,
                                  &cursor_row, &cursor_line)) {
        return;
    }
    uint16_t first_visible = cursor_row > 0u
        ? (uint16_t)(cursor_row - 1u) : 0u;
    char line[sizeof(app->editor_value)];
    for (uint8_t row = 0u;
         row < 2u && first_visible + row < line_count;
         row++) {
        ui_text_span_t span;
        if (ui_glyph_line_at(font, app->editor_value, 82,
                             (uint16_t)(first_visible + row), &span) &&
            ui_text_span_copy(app->editor_value, span, line,
                              sizeof(line))) {
            fb_text(fb, font, line, 2, 18 + row * line_pitch, true, 82);
        }
    }
    if (app->editor_show_cursor && app->editor_cursor_visible) {
        uint8_t row = (uint8_t)(cursor_row - first_visible);
        int cx = 2 + ui_text_range_width(font, app->editor_value,
                                         cursor_line.start, cursor) +
                 app->editor_cursor_x_offset;
        if (cx > 82) {
            cx = 82;
        }
        int cy = 18 + row * line_pitch + app->editor_cursor_y_offset;
        fb_vline(fb, cx, cy, font->height + 1, true);
    }
    draw_softkey(fb, "OK");
}

static int editor_number_cursor_x(const font_t *font,
                                  const char *text,
                                  size_t row_start,
                                  size_t cursor,
                                  int text_x) {
    /* The traced FS0/FS2 glyph advance includes a blank final column. The
     * insertion caret occupies that column; advancing beyond it draws the
     * caret inside the next digit when the cursor is moved through a number. */
    return text_x + ui_text_range_width(font, text, row_start, cursor) - 1;
}

bool handle_editor_key(app_t *app, uint16_t key, event_type_t event_type, uint32_t now) {
    if (key == KEY_NAVI) {
        if (app->editor_context == EDITOR_CONTEXT_PHONEBOOK_SEARCH) {
            start_phonebook_search(app, app->editor_value, now);
        } else if (app->editor_context == EDITOR_CONTEXT_PHONEBOOK_ADD_NAME) {
            if (app->editor_value[0] != '\0') {
                copy_text(app->editor_draft_name, sizeof(app->editor_draft_name), app->editor_value);
                if (app->editor_draft_number[0] != '\0') {
                    /* PB.7: standby typed-number Save already staged the
                     * number; Name OK saves directly, no second prompt. */
                    start_phonebook_add(app, app->editor_draft_name, app->editor_draft_number, now);
                } else {
                    open_editor(app, ts_or(0x28du, "Number:"), app->editor_draft_number, 30u, EDITOR_KIND_NUMBER, EDITOR_CONTEXT_PHONEBOOK_ADD_NUMBER, true, now);
                }
            }
        } else if (app->editor_context == EDITOR_CONTEXT_PHONEBOOK_ADD_NUMBER) {
            if (app->editor_value[0] != '\0') {
                copy_text(app->editor_draft_number, sizeof(app->editor_draft_number), app->editor_value);
                start_phonebook_add(app, app->editor_draft_name, app->editor_value, now);
            }
        } else if (app->editor_context == EDITOR_CONTEXT_PHONEBOOK_EDIT_NAME) {
            if (app->editor_value[0] != '\0') {
                phonebook_entry_t entry;
                copy_text(app->editor_draft_name, sizeof(app->editor_draft_name), app->editor_value);
                if (phonebook_service_entry(app->phonebook_pending_index, &entry)) {
                    open_editor(app, ts_or(0x28du, "Number:"), entry.number, 30u, EDITOR_KIND_NUMBER, EDITOR_CONTEXT_PHONEBOOK_EDIT_NUMBER, true, now);
                }
            }
        } else if (app->editor_context == EDITOR_CONTEXT_PHONEBOOK_EDIT_NUMBER) {
            phonebook_entry_t entry;
            if (app->editor_value[0] != '\0' && phonebook_service_entry(app->phonebook_pending_index, &entry)) {
                copy_text(app->editor_draft_number, sizeof(app->editor_draft_number), app->editor_value);
                if (strcmp(app->editor_draft_name, app->editor_original_name) != 0) {
                    app->phonebook_edit_choice_selected = 0u;
                    app->route = APP_ROUTE_PHONEBOOK_EDIT_CHOICE;
                    app->dirty = true;
                } else {
                    start_phonebook_update(app, entry.index, app->editor_draft_name, app->editor_value, now);
                }
            }
        } else if (app->editor_context == EDITOR_CONTEXT_PHONEBOOK_SEND_RECIPIENT) {
            if (app->editor_value[0] == '\0') {
                copy_text(app->sms_recipient_prefill, sizeof(app->sms_recipient_prefill), app->editor_value);
                start_phonebook_list(app, PHONEBOOK_LABEL_OK, "1-7", PHONEBOOK_CONTEXT_SEND_RECIPIENT, 0u, now);
            } else {
                phonebook_entry_t entry;
                if (phonebook_service_entry(app->phonebook_pending_index, &entry)) {
                    char text[64];
                    snprintf(text, sizeof(text), "%s\n%s", entry.name, entry.number);
                    uint32_t request_id = 0u;
                    if (!modem_service_request_send_sms(
                            app->editor_value, text, &request_id)) {
                        open_display_sid(app, 0u, 0x35au, "Still\nsending\nprevious", APP_ROUTE_EDITOR, now);
                        return true;
                    }
                    app->phonebook_send_waiting = true;
                    app->phonebook_send_request_id = request_id;
                    app->phonebook_send_started_ms = now;
                    close_editor(app);
                    show_phonebook_list(app, PHONEBOOK_LABEL_SEND, "1-7", PHONEBOOK_CONTEXT_SEND, app->phonebook_pending_selected);
                    open_display_sid(app, 46u, 0x35fu, "Sending\nmessage", APP_ROUTE_PHONEBOOK_LIST, now);
                }
            }
        } else if (app->editor_context == EDITOR_CONTEXT_SMS_RECIPIENT) {
            if (app->editor_value[0] == '\0') {
                copy_text(app->sms_recipient_prefill, sizeof(app->sms_recipient_prefill), app->editor_value);
                start_phonebook_list(app, PHONEBOOK_LABEL_OK, "2-3", PHONEBOOK_CONTEXT_SMS_RECIPIENT, 0u, now);
            } else {
                copy_text(app->sms_recipient_prefill, sizeof(app->sms_recipient_prefill), app->editor_value);
                close_editor(app);
                start_sms_send(app, app->sms_recipient_prefill, app->sms_composer_text, APP_ROUTE_SMS_COMPOSER, now);
            }
        } else if (app->editor_context == EDITOR_CONTEXT_PICTURE_RECIPIENT) {
            if (app->editor_value[0] == '\0') {
                copy_text(app->sms_recipient_prefill, sizeof(app->sms_recipient_prefill), app->editor_value);
                start_phonebook_list(app, PHONEBOOK_LABEL_OK, "2-4", PHONEBOOK_CONTEXT_PICTURE_RECIPIENT, 0u, now);
            } else {
                messages_submit_picture_recipient(app, now);
            }
        } else if (app->editor_context == EDITOR_CONTEXT_TONE_COMPOSER_RECIPIENT) {
            if (app->editor_value[0] == '\0') {
                copy_text(app->sms_recipient_prefill, sizeof(app->sms_recipient_prefill), app->editor_value);
                start_phonebook_list(app, PHONEBOOK_LABEL_OK, "9-3", PHONEBOOK_CONTEXT_TONE_COMPOSER_RECIPIENT, 0u, now);
            } else {
                copy_text(app->sms_recipient_prefill, sizeof(app->sms_recipient_prefill), app->editor_value);
                close_editor(app);
                tone_composer_submit_recipient(app, now);
            }
        } else if (app->editor_context == EDITOR_CONTEXT_IN_CALL_NEW_CALL) {
            if (app->editor_value[0] == '\0') {
                start_phonebook_list(app, PHONEBOOK_LABEL_CALL, "", PHONEBOOK_CONTEXT_IN_CALL_NEW_CALL, 0u, now);
            } else {
                char number[MODEM_PHONE_MAX + 1u];
                copy_text(number, sizeof(number), app->editor_value);
                /* The live call becomes the HELD leg; C's waiting fields must
                 * survive the session reset (see call_newcall_snapshot). */
                call_newcall_preserve_t preserve;
                call_newcall_snapshot(app, now, &preserve);
                close_editor(app);
                bool handled = start_outgoing_call(app, number, "", now, APP_ROUTE_CALL);
                if (app->route == APP_ROUTE_CALL) {
                    call_newcall_restore_held(app, &preserve);
                }
                return handled;
            }
        } else if (app->editor_context == EDITOR_CONTEXT_IN_CALL_DTMF) {
            if (app->editor_value[0] != '\0' &&
                !modem_service_request_dtmf_sequence(app->editor_value)) {
                open_display_sid(app, 32u, 0x210u, "Not done",
                                 APP_ROUTE_EDITOR, now);
                return true;
            }
            close_editor(app);
            app->route = APP_ROUTE_CALL;
            app->dirty = true;
        } else if (app->editor_context == EDITOR_CONTEXT_SMS_MESSAGE_CENTRE) {
            if (app->editor_value[0] == '\0') {
                open_display_sid(app, 0u, 0x208u, "No phone\nnumber", APP_ROUTE_EDITOR, now);
            } else {
                store_setting_set_text(STORE_SETTING_SMS_MESSAGE_CENTRE, app->editor_value);
                close_editor(app);
                open_display_sid(app, 3u, 0x358u, "Centre\nnumber\nsaved", APP_ROUTE_MESSAGES_LIST, now);
            }
        } else if (app->editor_context == EDITOR_CONTEXT_VOICE_MAILBOX_NUMBER) {
            store_setting_set_text(STORE_SETTING_SYSTEM_VOICE_MAILBOX_NUMBER, app->editor_value);
            close_editor(app);
            open_display_sid(app, 3u, 0x3b4u, "Saved", APP_ROUTE_MAIN_MENU, now);
        } else if (app->editor_context == EDITOR_CONTEXT_SMS_T9_INSERT_WORD) {
            if (app->editor_value[0] != '\0') {
                sms_t9_insert_text(app, app->editor_value, app->sms_t9_insert_replace_active, true);
            }
            app->sms_t9_insert_replace_active = false;
            close_editor(app);
            app->route = APP_ROUTE_SMS_COMPOSER;
            app->dirty = true;
        } else if (app->editor_context == EDITOR_CONTEXT_SMS_T9_INSERT_NUMBER) {
            if (app->editor_value[0] != '\0') {
                sms_t9_insert_text(app, app->editor_value, false, false);
            }
            app->sms_t9_insert_replace_active = false;
            close_editor(app);
            app->route = APP_ROUTE_SMS_COMPOSER;
            app->dirty = true;
        } else if (app->editor_context == EDITOR_CONTEXT_CALL_REGISTER_EDIT_NUMBER) {
            if (app->editor_value[0] == '\0') {
                open_display_sid(app, 0u, 0x208u, "No phone\nnumber", APP_ROUTE_EDITOR, now);
            } else {
                store_call_update_number((store_call_list_t)app->call_register_list_kind,
                                         app->call_register_list_selected,
                                         app->editor_value);
                close_editor(app);
                open_display_sid(app, 3u, 0x296u, "Saved", APP_ROUTE_CALL_REGISTER_LIST, now);
            }
        } else if (app->editor_context == EDITOR_CONTEXT_SETTINGS_WELCOME_NOTE) {
            settings_submit_welcome_editor(app, now);
        } else if (app->editor_context == EDITOR_CONTEXT_SETTINGS_PIN_REQUEST) {
            settings_submit_pin_request_editor(app, now);
        } else if (app->editor_context == EDITOR_CONTEXT_SETTINGS_ACCESS_CODE) {
            settings_submit_access_code_editor(app, now);
        } else if (app->editor_context == EDITOR_CONTEXT_CALL_DIVERT_OTHER_NUMBER) {
            call_divert_submit_other_number(app, now);
        } else if (app->editor_context == EDITOR_CONTEXT_TONE_COMPOSER_NAME) {
            tone_composer_submit_name(app, now);
        }
        return true;
    }
    if (key == KEY_C) {
        if (editor_delete_one(app)) {
            if (app->editor_context == EDITOR_CONTEXT_PHONEBOOK_SEND_RECIPIENT) {
                update_phonebook_send_recipient_softkey(app);
            } else if (app->editor_context == EDITOR_CONTEXT_SMS_RECIPIENT) {
                update_sms_recipient_softkey(app);
            } else if (app->editor_context == EDITOR_CONTEXT_TONE_COMPOSER_RECIPIENT) {
                update_tone_composer_recipient_softkey(app);
            }
            app->dirty = true;
            return true;
        }
        if (app->editor_context == EDITOR_CONTEXT_PHONEBOOK_ADD_NUMBER) {
            open_editor(app, ts_or(0x283u, "Name:"), app->editor_draft_name, 16u, EDITOR_KIND_TEXT, EDITOR_CONTEXT_PHONEBOOK_ADD_NAME, true, now);
        } else if (app->editor_context == EDITOR_CONTEXT_PHONEBOOK_EDIT_NUMBER) {
            open_editor(app, ts_or(0x283u, "Name:"), app->editor_draft_name, 16u, EDITOR_KIND_TEXT, EDITOR_CONTEXT_PHONEBOOK_EDIT_NAME, true, now);
        } else if (app->editor_context == EDITOR_CONTEXT_PHONEBOOK_SEND_RECIPIENT) {
            close_editor(app);
            show_phonebook_list(app, PHONEBOOK_LABEL_SEND, "1-7", PHONEBOOK_CONTEXT_SEND, app->phonebook_pending_selected);
        } else if (app->editor_context == EDITOR_CONTEXT_SMS_RECIPIENT) {
            close_editor(app);
            app->route = APP_ROUTE_SMS_COMPOSER;
            app->dirty = true;
        } else if (app->editor_context == EDITOR_CONTEXT_PICTURE_RECIPIENT) {
            messages_cancel_picture_recipient(app, now);
        } else if (app->editor_context == EDITOR_CONTEXT_TONE_COMPOSER_RECIPIENT) {
            tone_composer_cancel_recipient(app, now);
        } else if (app->editor_context == EDITOR_CONTEXT_SMS_MESSAGE_CENTRE) {
            close_editor(app);
            app->route = APP_ROUTE_MESSAGES_LIST;
            app->dirty = true;
        } else if (app->editor_context == EDITOR_CONTEXT_VOICE_MAILBOX_NUMBER) {
            close_editor(app);
            app->route = APP_ROUTE_MAIN_MENU;
            app->dirty = true;
        } else if (app->editor_context == EDITOR_CONTEXT_SMS_T9_INSERT_WORD ||
                   app->editor_context == EDITOR_CONTEXT_SMS_T9_INSERT_NUMBER) {
            app->sms_t9_insert_replace_active = false;
            close_editor(app);
            app->route = APP_ROUTE_SMS_COMPOSER;
            app->dirty = true;
        } else if (app->editor_context == EDITOR_CONTEXT_CALL_REGISTER_EDIT_NUMBER) {
            close_editor(app);
            app->route = APP_ROUTE_CALL_REGISTER_OPTIONS;
            app->dirty = true;
        } else if (app->editor_context == EDITOR_CONTEXT_SETTINGS_WELCOME_NOTE ||
                   app->editor_context == EDITOR_CONTEXT_SETTINGS_PIN_REQUEST ||
                   app->editor_context == EDITOR_CONTEXT_SETTINGS_ACCESS_CODE) {
            settings_cancel_editor(app, now);
        } else if (app->editor_context == EDITOR_CONTEXT_CALL_DIVERT_OTHER_NUMBER) {
            call_divert_cancel_other_number(app, now);
        } else if (app->editor_context == EDITOR_CONTEXT_TONE_COMPOSER_NAME) {
            tone_composer_cancel_name(app, now);
        } else if (app->editor_context == EDITOR_CONTEXT_IN_CALL_NEW_CALL ||
                   app->editor_context == EDITOR_CONTEXT_IN_CALL_DTMF) {
            close_editor(app);
            app->route = APP_ROUTE_CALL;
            app->dirty = true;
        } else {
            close_editor(app);
            app->route = APP_ROUTE_MAIN_MENU;
            app->dirty = true;
        }
        return true;
    }
    if ((key == KEY_UP || key == KEY_DOWN) && !editor_number_search_key(app, key)) {
        /* S6.2: editable inputs carry DF3_CURSOR_MOVABLE — Up/Down move the
         * insertion point in text editors too, not only number fields. */
        (void)editor_move_cursor(app, key == KEY_UP ? 1 : -1);
        app->dirty = true;
        return true;
    }
    if (app->editor_context == EDITOR_CONTEXT_PHONEBOOK_SEND_RECIPIENT && (key == KEY_UP || key == KEY_DOWN)) {
        copy_text(app->sms_recipient_prefill, sizeof(app->sms_recipient_prefill), app->editor_value);
        start_phonebook_list(app, PHONEBOOK_LABEL_OK, "1-7", PHONEBOOK_CONTEXT_SEND_RECIPIENT, key == KEY_DOWN ? 1u : 0u, now);
        return true;
    }
    if (app->editor_context == EDITOR_CONTEXT_SMS_RECIPIENT && (key == KEY_UP || key == KEY_DOWN)) {
        copy_text(app->sms_recipient_prefill, sizeof(app->sms_recipient_prefill), app->editor_value);
        start_phonebook_list(app, PHONEBOOK_LABEL_OK, "2-3", PHONEBOOK_CONTEXT_SMS_RECIPIENT, key == KEY_DOWN ? 1u : 0u, now);
        return true;
    }
    if (app->editor_context == EDITOR_CONTEXT_PICTURE_RECIPIENT && (key == KEY_UP || key == KEY_DOWN)) {
        copy_text(app->sms_recipient_prefill, sizeof(app->sms_recipient_prefill), app->editor_value);
        start_phonebook_list(app, PHONEBOOK_LABEL_OK, "2-4", PHONEBOOK_CONTEXT_PICTURE_RECIPIENT, key == KEY_DOWN ? 1u : 0u, now);
        return true;
    }
    if (app->editor_context == EDITOR_CONTEXT_TONE_COMPOSER_RECIPIENT && (key == KEY_UP || key == KEY_DOWN)) {
        copy_text(app->sms_recipient_prefill, sizeof(app->sms_recipient_prefill), app->editor_value);
        start_phonebook_list(app, PHONEBOOK_LABEL_OK, "9-3", PHONEBOOK_CONTEXT_TONE_COMPOSER_RECIPIENT, key == KEY_DOWN ? 1u : 0u, now);
        return true;
    }
    if (editor_input_key(app, key, event_type, now)) {
        if (app->editor_context == EDITOR_CONTEXT_PHONEBOOK_SEND_RECIPIENT) {
            update_phonebook_send_recipient_softkey(app);
        } else if (app->editor_context == EDITOR_CONTEXT_SMS_RECIPIENT ||
                   app->editor_context == EDITOR_CONTEXT_PICTURE_RECIPIENT) {
            update_sms_recipient_softkey(app);
        } else if (app->editor_context == EDITOR_CONTEXT_TONE_COMPOSER_RECIPIENT) {
            update_tone_composer_recipient_softkey(app);
        }
        app->dirty = true;
    }
    return true;
}

bool handle_confirm_key(app_t *app, uint16_t key, uint32_t now) {
    if (app->confirm_context == CONFIRM_CONTEXT_PICTURE_MESSAGE_SAVE_FIRST) {
        if (key == KEY_C || key == KEY_NAVI) {
            messages_picture_confirm_save(app, key == KEY_NAVI, now);
        }
        return true;
    }
    if (key == KEY_C) {
        if (app->confirm_context == CONFIRM_CONTEXT_PHONEBOOK_ERASE) {
            app->route = APP_ROUTE_PHONEBOOK_LIST;
        } else if (app->confirm_context == CONFIRM_CONTEXT_CALL_REGISTER_ERASE) {
            app->route = APP_ROUTE_CALL_REGISTER_LIST;
        } else if (app->confirm_context == CONFIRM_CONTEXT_PICTURE_MESSAGE_ERASE) {
            app->messages_kind = MESSAGES_KIND_PICTURES;
            app->messages_mode = MESSAGES_MODE_OPTIONS;
            app->route = APP_ROUTE_MESSAGES_LIST;
        } else if (app->confirm_context == CONFIRM_CONTEXT_TONES_RINGING_VOLUME) {
            tones_confirm_ringing_volume(app, false, now);
            return true;
        } else {
            app->route = APP_ROUTE_MAIN_MENU;
        }
        app->confirm_context = CONFIRM_CONTEXT_NONE;
        app->dirty = true;
        return true;
    }
    if (key != KEY_NAVI) {
        return true;
    }
    if (app->confirm_context == CONFIRM_CONTEXT_PHONEBOOK_ERASE) {
        phonebook_entry_t entry;
        if (phonebook_service_entry(app->phonebook_pending_index, &entry)) {
            start_phonebook_delete(app, entry.index, now);
        }
    } else if (app->confirm_context == CONFIRM_CONTEXT_PHONEBOOK_ERASE_ALL) {
        (void)now;
        app->editor_value[0] = '\0';
        app->route = APP_ROUTE_PHONEBOOK_SECURITY;
        app->dirty = true;
    } else if (app->confirm_context == CONFIRM_CONTEXT_CALL_REGISTER_ERASE) {
        store_call_delete((store_call_list_t)app->call_register_list_kind, app->call_register_list_selected);
        uint8_t count = store_call_count((store_call_list_t)app->call_register_list_kind);
        if (app->call_register_list_selected >= count && count > 0u) {
            app->call_register_list_selected = (uint8_t)(count - 1u);
        }
        app->confirm_context = CONFIRM_CONTEXT_NONE;
        open_display_sid(app, 44u, 0x278u, "Erased", APP_ROUTE_CALL_REGISTER_LIST, now);
    } else if (app->confirm_context == CONFIRM_CONTEXT_PICTURE_MESSAGE_ERASE) {
        if (app->messages_picture_pending_valid && app->messages_picture_pending_slot < STORE_PICTURE_SLOT_COUNT) {
            (void)store_picture_message_clear(app->messages_picture_pending_slot);
        }
        uint8_t count = store_picture_message_count();
        if (app->messages_selected >= count && count > 0u) {
            app->messages_selected = (uint8_t)(count - 1u);
        }
        app->messages_picture_pending_valid = false;
        app->messages_kind = MESSAGES_KIND_PICTURES;
        app->messages_mode = MESSAGES_MODE_LIST;
        app->confirm_context = CONFIRM_CONTEXT_NONE;
        open_display_sid(app, 6u, 0x172u, "Picture message erased", APP_ROUTE_MESSAGES_LIST, now);
    } else if (app->confirm_context == CONFIRM_CONTEXT_TONES_RINGING_VOLUME) {
        tones_confirm_ringing_volume(app, true, now);
    }
    return true;
}

bool tick_display_message(app_t *app, uint32_t now) {
    bool changed = false;
    const display_record_meta_t *meta = display_record_meta(app->display_record_id);
    if (meta->gfx_kind == DISPLAY_GFX_ANIM_STRIPE) {
        /* anim-1 table 0x002dcbe8 speed byte 0x06 ticks x 80 ms = 480 ms/step;
         * progress records stay until the owning flow dismisses them. */
        if (time_diff_ms(now, app->display_last_frame_ms + 480u) >= 0) {
            app->display_progress_phase = (uint8_t)((app->display_progress_phase + 1u) % 80u);
            app->display_last_frame_ms = now;
            changed = true;
        }
        return changed;
    }
    uint8_t frame_count = 0u;
    if (display_anim_frames(meta->gfx_kind, &frame_count) != 0 && frame_count > 0u) {
        uint32_t frame_delay = 200u;
        if (meta->gfx_kind == DISPLAY_GFX_ANIM_KEYGUARD_HELP) {
            frame_delay = 160u;
        } else if (meta->gfx_kind == DISPLAY_GFX_ANIM_CHARGING) {
            frame_delay = 240u; /* anim 4 runs at 240 ms/frame */
        }
        if (time_diff_ms(now, app->display_last_frame_ms + frame_delay) >= 0) {
            app->display_last_frame_ms = now;
            if (app->display_frame_index + 1u < frame_count) {
                app->display_frame_index++;
                changed = true;
            }
        }
    }
    if (meta->delay_ms == 0u) {
        return changed; /* stays until a key (record 0x1f, 0x20, ...) */
    }
    if (time_diff_ms(now, app->display_opened_ms + meta->delay_ms) >= 0) {
        return_from_display(app);
        changed = true;
    }
    return changed;
}

static const display_record_meta_t *display_record_meta(uint8_t record_id) {
    static const display_record_meta_t fallback = {
        0xffu, DISPLAY_TEXT_WIN12, DISPLAY_GFX_NONE, DISPLAY_TONE_SILENT, 0u, 3080u};
    for (uint8_t i = 0; i < (uint8_t)ARRAY_COUNT(DISPLAY_RECORDS); i++) {
        if (DISPLAY_RECORDS[i].record_id == record_id) {
            return &DISPLAY_RECORDS[i];
        }
    }
    return &fallback;
}

static const uint16_t *display_anim_frames(uint8_t gfx_kind, uint8_t *out_count) {
    switch (gfx_kind) {
    case DISPLAY_GFX_ANIM_SENDING:
        *out_count = (uint8_t)ARRAY_COUNT(SENDING_FRAMES);
        return SENDING_FRAMES;
    case DISPLAY_GFX_ANIM_SENT:
        *out_count = (uint8_t)ARRAY_COUNT(SENT_FRAMES);
        return SENT_FRAMES;
    case DISPLAY_GFX_ANIM_CHECK:
        *out_count = (uint8_t)ARRAY_COUNT(CHECK_FRAMES);
        return CHECK_FRAMES;
    case DISPLAY_GFX_ANIM_TRASH:
        *out_count = (uint8_t)ARRAY_COUNT(TRASH_FRAMES);
        return TRASH_FRAMES;
    case DISPLAY_GFX_ANIM_KEYGUARD_LOCKED:
        *out_count = (uint8_t)ARRAY_COUNT(KEYGUARD_LOCK_FRAMES);
        return KEYGUARD_LOCK_FRAMES;
    case DISPLAY_GFX_ANIM_KEYGUARD_HELP:
        *out_count = (uint8_t)ARRAY_COUNT(KEYGUARD_HELP_FRAMES);
        return KEYGUARD_HELP_FRAMES;
    case DISPLAY_GFX_ANIM_BATTERY_LOW:
        *out_count = (uint8_t)ARRAY_COUNT(BATTERY_LOW_FRAMES);
        return BATTERY_LOW_FRAMES;
    case DISPLAY_GFX_ANIM_CHARGING:
        *out_count = (uint8_t)ARRAY_COUNT(CHARGING_FRAMES);
        return CHARGING_FRAMES;
    default:
        *out_count = 0u;
        return 0;
    }
}

/* C1: display_msg core 0x002a7556 dispatches the record tone through
 * send_message(0x8190, tone, 0xf1) unless it is the 0x21 silent sentinel.
 * Volume follows the profile "Warning and game tones" setting. */
static void play_display_record_tone(uint8_t index) {
    uint8_t level = profile_get_tone_setting(profile_active_index(), PROFILE_SETTING_WARNING_GAME_TONES);
    if (level == 255u) {
        return;
    }
    if (level > AUDIO_LEVEL_MAX) {
        level = AUDIO_LEVEL_MAX;
    }
    core1_post_command(CORE1_CMD_AUDIO_SYSTEM_TONE, audio_arg(index, level));
}

bool tick_editor(app_t *app, uint32_t now) {
    if (!app->editor_show_cursor) {
        return false;
    }
    if (time_diff_ms(now, app->editor_last_cursor_ms + 512u) < 0) {
        return false;
    }
    app->editor_cursor_visible = !app->editor_cursor_visible;
    app->editor_last_cursor_ms = now;
    return true;
}

void open_display(app_t *app, uint8_t record_id, const char *a, const char *b, const char *c, app_route_t return_route, uint32_t now) {
    /* Display record 0x20 is shared. Any generic dialog open relinquishes a
     * call-divert detail sequence; show_status_detail() explicitly claims the
     * newly opened record again after this function returns. */
    app->call_divert_status_detail_active = false;
    /* Any new dialog replaces an outstanding phonebook progress note. A
     * phonebook start path explicitly reclaims this after opening its note. */
    app->phonebook_wait_display_request_id = 0u;
    app->route = APP_ROUTE_DISPLAY_MESSAGE;
    app->display_return_route = return_route;
    app->display_record_id = record_id;
    app->display_opened_ms = now;
    app->display_last_frame_ms = now;
    app->display_frame_index = 0u;
    app->display_progress_phase = 0u;
    /* Join the (already short) caller lines with '\n' into the full-text buffer;
     * render_display_message wraps it to the record's window. Callers pass ASCII
     * English literals here, well under the buffer, so the byte copy never lands
     * mid-codepoint; open_display_sid feeds localized text as a single line. */
    const char *src[3] = {a, b, c};
    size_t di = 0u;
    for (uint8_t i = 0; i < 3u; i++) {
        if (src[i] == 0 || src[i][0] == '\0') {
            continue;
        }
        if (di != 0u && di + 1u < sizeof(app->display_text)) {
            app->display_text[di++] = '\n';
        }
        /* Copy whole codepoints only: open_display_sid feeds localized (multibyte)
         * text here, so a byte-wise cut at the cap could split a glyph. */
        const char *p = src[i];
        while (*p != '\0') {
            const char *nx = p;
            asset_next_codepoint(&nx);
            size_t cb = (size_t)(nx - p);
            if (di + cb + 1u > sizeof(app->display_text)) {
                break;
            }
            while (p < nx) {
                app->display_text[di++] = *p++;
            }
        }
    }
    app->display_text[di] = '\0';
    const display_record_meta_t *meta = display_record_meta(record_id);
    if (meta->record_id != 0xffu && meta->tone != DISPLAY_TONE_SILENT) {
        play_display_record_tone(meta->tone);
    }
    app->dirty = true;
}

void open_display_sid(app_t *app, uint8_t record_id, uint16_t sid, const char *fallback,
                      app_route_t return_route, uint32_t now) {
    /* v6.00 stores most dialog messages as a single (often multiline) SID.
     * Resolve it (localized, English fallback, or the caller's literal for a
     * clone-only sid) and hand the whole string to open_display -- its embedded
     * '\n' breaks are kept and render_display_message wraps the rest to the
     * record's window. Storing the full text (not pre-split fixed slots) is what
     * lets long localized lines survive intact. */
    const char *text = ts(sid);
    if (text == 0) {
        text = fallback;
    }
    open_display(app, record_id, text, 0, 0, return_route, now);
}

void return_from_display(app_t *app) {
    app->phonebook_wait_display_request_id = 0u;
    app->route = app->display_return_route;
    app->display_record_id = 0u;
    app->dirty = true;
}

void open_editor(app_t *app, const char *title, const char *value, uint8_t max_len, editor_kind_t kind, editor_context_t context, bool show_cursor, uint32_t now) {
    memset(app->editor_title, 0, sizeof(app->editor_title));
    memset(app->editor_value, 0, sizeof(app->editor_value));
    copy_text(app->editor_title, sizeof(app->editor_title), title);
    copy_text(app->editor_value, sizeof(app->editor_value), value);
    app->editor_max_len = max_len;
    app->editor_kind = (uint8_t)kind;
    app->editor_context = (uint8_t)context;
    app->editor_mode = EDITOR_MODE_UPPER;
    app->editor_show_cursor = show_cursor;
    app->editor_show_mode_badge = kind == EDITOR_KIND_TEXT;
    app->editor_cursor_visible = show_cursor;
    app->editor_last_cursor_ms = now;
    app->editor_cursor_x_offset = 0;
    app->editor_cursor_y_offset = 0;
    app->editor_cursor_index = (uint8_t)strlen(app->editor_value);
    if (context == EDITOR_CONTEXT_PHONEBOOK_ADD_NAME || context == EDITOR_CONTEXT_PHONEBOOK_EDIT_NAME) {
        app->editor_cursor_x_offset = -1;
        app->editor_cursor_y_offset = -1;
    }
    editor_reset_multitap(app);
    app->route = APP_ROUTE_EDITOR;
    app->dirty = true;
}

void close_editor(app_t *app) {
    app->editor_context = EDITOR_CONTEXT_NONE;
    app->editor_value[0] = '\0';
    app->editor_cursor_index = 0u;
}

void open_confirm(app_t *app, confirm_context_t context, const char *a, const char *b, const char *c, int8_t first_y) {
    const char *src[3] = {a, b, c};
    app->confirm_context = (uint8_t)context;
    app->confirm_line_count = 0u;
    for (uint8_t i = 0; i < 3u; i++) {
        app->confirm_lines[i][0] = '\0';
        if (src[i] != 0 && src[i][0] != '\0') {
            copy_text(app->confirm_lines[app->confirm_line_count], sizeof(app->confirm_lines[0]), src[i]);
            app->confirm_lines[app->confirm_line_count][sizeof(app->confirm_lines[0]) - 1u] = '\0';
            app->confirm_line_count++;
        }
    }
    app->confirm_first_y = first_y;
    app->route = APP_ROUTE_CONFIRM;
    app->dirty = true;
}

void open_confirm_sid(app_t *app, confirm_context_t context, uint16_t sid, const char *fallback, int8_t first_y) {
    /* Localized confirm question: resolve the SID and wrap it to the bool-dialog
     * width (FS2, w84) into the pre-split confirm_lines the renderer draws. */
    const char *text = ts(sid);
    if (text == 0) {
        text = fallback;
    }
    char lines[3][32];
    uint8_t n = wrap_text_lines_ex(asset_font(FONT_FS2), text, FB_WIDTH, (char *)lines, 32u, 3u);
    app->confirm_context = (uint8_t)context;
    app->confirm_line_count = 0u;
    for (uint8_t i = 0; i < 3u; i++) {
        app->confirm_lines[i][0] = '\0';
        if (i < n) {
            copy_text(app->confirm_lines[app->confirm_line_count], sizeof(app->confirm_lines[0]), lines[i]);
            app->confirm_line_count++;
        }
    }
    app->confirm_first_y = first_y;
    app->route = APP_ROUTE_CONFIRM;
    app->dirty = true;
}

void open_display_sid_num(app_t *app, uint8_t record_id, uint16_t sid, const char *fallback,
                          unsigned num, app_route_t return_route, uint32_t now) {
    /* Like open_display_sid, but substitutes the v6.00 "%N" token with `num`
     * (e.g. 0x057 "Speed dial\nkey %N\nsaved", 0x096 "%N\nmissed\ncalls",
     * 0x3a5 "Delay time\n%N seconds"). render_display_message wraps the result. */
    const char *tmpl = ts(sid);
    if (tmpl == 0) {
        tmpl = fallback;
    }
    char num_s[12];
    snprintf(num_s, sizeof(num_s), "%u", num);
    char buf[sizeof(app->display_text)];
    size_t di = 0u;
    for (size_t i = 0u; tmpl[i] != '\0' && di + 1u < sizeof(buf);) {
        if (tmpl[i] == '%' && tmpl[i + 1u] == 'N') {
            for (size_t k = 0u; num_s[k] != '\0' && di + 1u < sizeof(buf); k++) {
                buf[di++] = num_s[k];
            }
            i += 2u;
        } else {
            buf[di++] = tmpl[i++];
        }
    }
    buf[di] = '\0';
    open_display(app, record_id, buf, 0, 0, return_route, now);
}

static bool editor_input_key(app_t *app, uint16_t key, event_type_t event_type, uint32_t now) {
    if (app->editor_kind == EDITOR_KIND_TEXT && key == KEY_HASH) {
        /* C8: name editors start ABC; short-# toggles ABC<->abc only,
         * hold-# selects 123 (badges 0007/0010/0011/0012 binary-pinned). */
        if (event_type == EVENT_KEY_HOLD) {
            app->editor_mode = app->editor_mode == EDITOR_MODE_NUMERIC ? EDITOR_MODE_UPPER : EDITOR_MODE_NUMERIC;
        } else if (app->editor_mode == EDITOR_MODE_UPPER) {
            app->editor_mode = EDITOR_MODE_LOWER;
        } else {
            app->editor_mode = EDITOR_MODE_UPPER;
        }
        editor_reset_multitap(app);
        return true;
    }
    size_t len = strlen(app->editor_value);
    if (app->editor_kind == EDITOR_KIND_NUMBER) {
        static const char STAR_SYMBOLS[] = {'*', '+', 'p', 'w'};
        if (app->editor_cursor_index > len) {
            app->editor_cursor_index = (uint8_t)len;
        }
        if (key == KEY_STAR) {
            bool cycle_active = app->editor_cursor_index > 0u &&
                                app->editor_last_key == KEY_STAR &&
                                time_diff_ms(now, app->editor_last_key_ms + 1200u) < 0;
            if (cycle_active) {
                char previous = app->editor_value[app->editor_cursor_index - 1u];
                for (uint8_t i = 0u; i < ARRAY_COUNT(STAR_SYMBOLS); i++) {
                    if (previous == STAR_SYMBOLS[i]) {
                        app->editor_value[app->editor_cursor_index - 1u] = STAR_SYMBOLS[(i + 1u) % ARRAY_COUNT(STAR_SYMBOLS)];
                        app->editor_last_key_ms = now;
                        return true;
                    }
                }
            }
            if (!editor_insert_char(app, STAR_SYMBOLS[0])) {
                return false;
            }
            app->editor_last_key = KEY_STAR;
            app->editor_last_key_ms = now;
            return true;
        }
        char ch = key_digit(key);
        if ((ch < '0' || ch > '9') && ch != '#') {
            return false;
        }
        if (!editor_insert_char(app, ch)) {
            return false;
        }
        editor_reset_multitap(app);
        return true;
    }
    if (app->editor_mode == EDITOR_MODE_NUMERIC) {
        char ch = key_digit(key);
        if (ch < '0' || ch > '9') {
            return false;
        }
        if (!editor_insert_char(app, ch)) {
            return false;
        }
        editor_reset_multitap(app);
        return true;
    }
    const char *chars = editor_chars_for_key(key, (editor_mode_t)app->editor_mode);
    if (chars[0] == '\0') {
        return false;
    }
    app->editor_cursor_index = (uint8_t)ui_text_clamp_boundary(
        app->editor_value, app->editor_cursor_index);
    size_t previous = ui_text_previous_boundary(app->editor_value,
                                                app->editor_cursor_index);
    if (app->editor_last_key == key && app->editor_cursor_index > 0u &&
        app->editor_cursor_index - previous == 1u &&
        time_diff_ms(now, app->editor_last_key_ms + 900u) < 0) {
        app->editor_tap_index = (uint8_t)((app->editor_tap_index + 1u) % strlen(chars));
        app->editor_value[app->editor_cursor_index - 1u] = chars[app->editor_tap_index];
    } else {
        app->editor_tap_index = 0u;
        if (!editor_insert_char(app, chars[0])) {
            return false;
        }
    }
    app->editor_last_key = key;
    app->editor_last_key_ms = now;
    return true;
}

bool editor_delete_one(app_t *app) {
    editor_reset_multitap(app);
    size_t len = strlen(app->editor_value);
    if (len == 0u) {
        return false;
    }
    size_t cursor = ui_text_clamp_boundary(app->editor_value,
                                           app->editor_cursor_index);
    app->editor_cursor_index = (uint8_t)cursor;
    if (cursor == 0u) {
        return false;
    }
    size_t index = ui_text_previous_boundary(app->editor_value, cursor);
    memmove(&app->editor_value[index],
            &app->editor_value[cursor],
            len - cursor + 1u);
    app->editor_cursor_index = (uint8_t)index;
    return true;
}

static bool editor_insert_char(app_t *app, char ch) {
    size_t len = strlen(app->editor_value);
    if (len >= app->editor_max_len || len + 1u >= sizeof(app->editor_value)) {
        return false;
    }
    size_t index = ui_text_clamp_boundary(app->editor_value,
                                          app->editor_cursor_index);
    memmove(&app->editor_value[index + 1u],
            &app->editor_value[index],
            len - index + 1u);
    app->editor_value[index] = ch;
    app->editor_cursor_index = (uint8_t)(index + 1u);
    return true;
}

static bool editor_move_cursor(app_t *app, int8_t delta) {
    size_t len = strlen(app->editor_value);
    size_t cursor = ui_text_clamp_boundary(app->editor_value,
                                           app->editor_cursor_index);
    if (delta > 0) {
        if (cursor >= len) {
            return false;
        }
        cursor = ui_text_next_boundary(app->editor_value, cursor);
    } else {
        if (cursor == 0u) {
            return false;
        }
        cursor = ui_text_previous_boundary(app->editor_value, cursor);
    }
    app->editor_cursor_index = (uint8_t)cursor;
    app->editor_cursor_visible = true;
    editor_reset_multitap(app);
    return true;
}

static bool editor_number_search_key(app_t *app, uint16_t key) {
    if (key != KEY_UP && key != KEY_DOWN) {
        return false;
    }
    return app->editor_context == EDITOR_CONTEXT_PHONEBOOK_SEND_RECIPIENT ||
           app->editor_context == EDITOR_CONTEXT_SMS_RECIPIENT ||
           app->editor_context == EDITOR_CONTEXT_PICTURE_RECIPIENT ||
           app->editor_context == EDITOR_CONTEXT_TONE_COMPOSER_RECIPIENT;
}

static void editor_reset_multitap(app_t *app) {
    app->editor_last_key = 0u;
    app->editor_last_key_ms = 0u;
    app->editor_tap_index = 0u;
}

const char *editor_chars_for_key(uint16_t key, editor_mode_t mode) {
    /* C7a/C7c: full ENGL multi-tap sets traced byte-for-byte at language
     * index 703 in flash, including the accent tails (CP1252 bytes; the
     * full glyph set ships in the generated fonts). The traced lower-case
     * tails differ from a naive tolower of the upper sets. */
    static const char *const UPPER[10] = {
        " 0",                          /* 0 */
        ".,?!-&'1",                    /* 1 */
        "ABC2\xc4\xc5\xc6\xc7",        /* 2: ABC2ÄÅÆÇ */
        "DEF3\xc9",                    /* 3: DEF3É */
        "GHI4",                        /* 4 */
        "JKL5\xa3",                    /* 5: JKL5£ */
        "MNO6\xd1\xd6\xd8",            /* 6: MNO6ÑÖØ */
        "PQRS7$",                      /* 7 */
        "TUV8\xdc",                    /* 8: TUV8Ü */
        "WXYZ9",                       /* 9 */
    };
    static const char *const LOWER[10] = {
        " 0",                          /* 0 */
        ".,?!-&'1",                    /* 1 */
        "abc2\xe4\xe5\xe0\xe6",        /* 2: abc2äåàæ */
        "def3\xe9\xe8",                /* 3: def3éè */
        "ghi4\xec",                    /* 4: ghi4ì */
        "jkl5\xa3",                    /* 5: jkl5£ */
        "mno6\xf1\xf6\xf8\xf2",        /* 6: mno6ñöøò */
        "pqrs7$\xdf",                  /* 7: pqrs7$ß */
        "tuv8\xfc\xf9",                /* 8: tuv8üù */
        "wxyz9",                       /* 9 */
    };
    int index;
    switch (key) {
    case KEY_0: index = 0; break;
    case KEY_1: index = 1; break;
    case KEY_2: index = 2; break;
    case KEY_3: index = 3; break;
    case KEY_4: index = 4; break;
    case KEY_5: index = 5; break;
    case KEY_6: index = 6; break;
    case KEY_7: index = 7; break;
    case KEY_8: index = 8; break;
    case KEY_9: index = 9; break;
    default: return "";
    }
    return mode == EDITOR_MODE_LOWER ? LOWER[index] : UPPER[index];
}

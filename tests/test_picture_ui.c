/* ROM resource/window geometry with the real fonts and language tables. */
#include <stdio.h>
#include <string.h>

#include "app_internal.h"
#include "apps/dialogs_app.h"
#include "apps/messages_app.h"
#include "services/strings.h"
#include "storage/store_service.h"
#include "ui/ui.h"
#include "../src/apps/messages/messages_internal.h"

static unsigned failures;
static store_picture_message_t saved;

uint32_t time_ms(void) { return 0u; }
int32_t time_diff_ms(uint32_t a, uint32_t b) { return (int32_t)(a - b); }
uint8_t store_picture_message_count(void) { return 1u; }
store_status_t store_picture_message_get(uint8_t slot, store_picture_message_t *out) {
    if (slot != 0u) return STORE_STATUS_NOT_FOUND;
    *out = saved;
    return STORE_STATUS_OK;
}
store_status_t store_picture_message_sender(uint8_t slot, char *out, size_t cap) {
    (void)slot;
    copy_text(out, cap, "+12345678901");
    return STORE_STATUS_OK;
}
void resolve_contact_name(const char *number, char *out, size_t cap) {
    (void)number;
    copy_text(out, cap, "Test sender");
}

static void check(bool ok, const char *message) {
    if (!ok) {
        fprintf(stderr, "FAIL: %s (language %u)\n", message, strings_get_language());
        failures++;
    }
}

static void same(const framebuffer_t *got, const framebuffer_t *want, const char *message) {
    check(memcmp(got->data, want->data, FB_SIZE) == 0, message);
}

int main(void) {
    strings_set_language(1u);
    const uint16_t notice_sids[] = {0x174u, 0x176u, 0x17du, 0x180u, 0x183u};
    for (size_t i = 0u; i < sizeof(notice_sids) / sizeof(notice_sids[0]); i++) {
        unsigned count = ui_wrap_line_count(asset_font(FONT_FS0), ts(notice_sids[i]), 84);
        if (count > 3u) fprintf(stderr, "SID %03x needs %u rows\n", notice_sids[i], count);
        check(count <= 3u, "picture information note fits the existing dialog");
    }
    const font_t *font = asset_font(FONT_FS2);
    framebuffer_t got = {0}, want = {0};
    messages_picture_draw_notice(&got);
    fb_text(&want, font, "Picture", 6, 8, true, 72);
    fb_text(&want, font, "message", 6, 17, true, 72);
    fb_text(&want, font, "received", 6, 26, true, 72);
    same(&got, &want, "arrival uses all three original notice rows, without clipping");

    app_t app = {0};
    app.picture_receive_id = 1u;
    app.messages_kind = MESSAGES_KIND_PICTURES;
    app.messages_mode = MESSAGES_MODE_READ;
    store_picture_message_t *picture = &app.messages_picture_save_candidate;
    picture->width = 72u;
    picture->height = 28u;
    picture->bitmap_len = 252u;
    memset(picture->bitmap, 0xff, picture->bitmap_len);
    copy_text(picture->text, sizeof(picture->text), "Caption");
    saved = *picture;
    messages_picture_render(&app, &got);
    fb_clear(&want, false);
    fb_fill_rect(&want, 6, 0, 72, 28, true);
    fb_text(&want, font, "Caption", 0, 29, true, 84);
    draw_softkey(&want, "Save");
    same(&got, &want, "preview uses top-centred bitmap, attached MT1 text and direct Save");

    app.messages_read_page = 1u;
    messages_picture_render(&app, &got);
    fb_clear(&want, false);
    fb_text(&want, font, "Caption", 0, 0, true, 84);
    draw_softkey(&want, "Save");
    same(&got, &want, "caption page uses window 0x24 with no top margin");

    app_t details = {0};
    details.messages_kind = MESSAGES_KIND_PICTURES;
    details.messages_mode = MESSAGES_MODE_DETAIL;
    fb_clear(&got, false);
    messages_picture_render(&details, &got);
    fb_clear(&want, false);
    fb_text(&want, font, "Sender:", 0, 0, true, 84);
    fb_text(&want, font, "Test sender", 0, 9, true, 84);
    draw_softkey(&want, "OK");
    same(&got, &want, "sender details use window 0x24, resolved name and OK");

    app_t note = {0};
    note.display_record_id = 6u;
    copy_text(note.display_text, sizeof(note.display_text), ts(0x180u));
    render_display_message(&note, &got);
    fb_clear(&want, false);
    fb_bitmap(&want, 53u, 62, 0, true, true);
    fb_text(&want, asset_font(FONT_FS0), "Picture", 0, 3, true, 84);
    fb_text(&want, asset_font(FONT_FS0), "message", 0, 16, true, 84);
    fb_text(&want, asset_font(FONT_FS0), "saved", 0, 29, true, 84);
    same(&got, &want, "saved note uses stock FS0 rows and information graphic");

    copy_text(note.display_text, sizeof(note.display_text), ts(0x183u));
    render_display_message(&note, &got);
    fb_clear(&want, false);
    fb_bitmap(&want, 53u, 62, 0, true, true);
    fb_text(&want, asset_font(FONT_FS0), "Picture", 0, 3, true, 62);
    fb_text(&want, asset_font(FONT_FS0), "message", 0, 16, true, 62);
    fb_text(&want, asset_font(FONT_FS0), "sent", 0, 29, true, 84);
    same(&got, &want, "sent note wraps below the information icon, not through it");

    for (uint8_t index = 0u; index < strings_language_count(); index++) {
        strings_set_language(strings_language_id_at(index));
        const int widths[] = {62, 62, 84, 84, 84};
        for (size_t sid = 0u; sid < sizeof(notice_sids) / sizeof(notice_sids[0]); sid++) {
            char rows[5][UI_WRAP_LINE_BYTES];
            uint8_t rows_used = wrap_text_lines_widths(asset_font(FONT_FS0),
                ts(notice_sids[sid]), widths, (char *)rows, sizeof(rows[0]), 5u);
            if (rows_used > 3u) fprintf(stderr, "SID %03x needs %u rows\n", notice_sids[sid], rows_used);
            check(rows_used > 0u && rows_used <= 3u,
                  "translated picture note fits around the graphic");
            note.display_record_id = 6u;
            copy_text(note.display_text, sizeof(note.display_text), ts(notice_sids[sid]));
            render_display_message(&note, &got);
            fb_clear(&want, false);
            fb_bitmap(&want, 53u, 62, 0, true, true);
            for (int y = 0; y < 26; y++) {
                for (int x = 62; x < 84; x++) {
                    check(fb_get_pixel(&got, x, y) == fb_get_pixel(&want, x, y),
                          "translated note leaves information graphic intact");
                }
            }
        }
        const char *notice = ts(0x17cu);
        uint16_t count = ui_wrap_line_count(font, notice, 72);
        check(count > 0u && count <= 3u, "translated arrival fits window 0x40");
        fb_clear(&got, false);
        fb_clear(&want, false);
        messages_picture_draw_notice(&got);
        char line[UI_WRAP_LINE_BYTES];
        for (uint16_t row = 0u; row < count; row++) {
            check(ui_wrap_line_at(font, notice, 72, row, line, sizeof(line)),
                  "translated notice row is complete");
            fb_text(&want, font, line, 6, 7 + (30 - count * 9) / 2 + row * 9, true, 72);
        }
        same(&got, &want, "notice uses active language and original centering");

        messages_picture_ask_save(&app);
        check(app.confirm_context == CONFIRM_CONTEXT_PICTURE_MESSAGE_SAVE_FIRST,
              "save-first uses the boolean-dialog framework");
        count = ui_wrap_line_count(font, ts(0x17fu), 84);
        check(count > 0u && count <= 3u && app.confirm_line_count == count,
              "translated save-first question is not truncated");
        render_confirm(&app, &got);
        fb_clear(&want, false);
        for (uint16_t row = 0u; row < count; row++) {
            (void)ui_wrap_line_at(font, ts(0x17fu), 84, row, line, sizeof(line));
            fb_text(&want, font, line, 0, 2 + (35 - count * 9) / 2 + row * 9, true, 84);
        }
        draw_softkey(&want, "OK");
        same(&got, &want, "save-first uses original window 0x2c and localized OK");

        copy_text(note.display_text, sizeof(note.display_text), ts(0x182u));
        for (uint8_t phase = 0u; phase < 4u; phase++) {
            note.display_progress_phase = phase;
            note.display_record_id = 36u;
            render_display_message(&note, &got);
            note.display_record_id = 4u;
            render_display_message(&note, &want);
            same(&got, &want, "picture sending uses the stock progress-stripe layout");
        }
    }
    return failures != 0u;
}

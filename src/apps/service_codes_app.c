#include "apps/service_codes_app.h"

#include <stdio.h>
#include <string.h>

#include "audio/audio_levels.h"
#include "apps/call_divert_app.h"
#include "apps/dialogs_app.h"
#include "services/core1_services.h"
#include "services/input_keys.h"
#include "services/key_utils.h"
#include "services/call_forward_mmi.h"
#include "services/strings.h"
#include "storage/store_service.h"
#include "services/timebase.h"

#include "build_info.h" /* generated: SISU_BUILD_HASH / SISU_BUILD_DATE */

#define SERVICE_DISPLAY_RECORD_TEXT 0x1fu
#define SERVICE_DISPLAY_RECORD_INFO 0x02u
#define SERVICE_DISPLAY_RECORD_ERROR 0x00u
#define SERVICE_DISPLAY_RECORD_NOTE 0x03u
#define SERVICE_SERIAL_TEMPLATE_SID 0x18bu
#define SERVICE_CODEC_TONE_INDEX 10u
#define SERVICE_CODEC_HIGH_MASK 0xc0u
#define SERVICE_CODEC_DEFAULT_BYTE 0x30u
#define SERVICE_PURCHASE_MASK "mmyy"
#define SERVICE_TRANSFER_RESULT_MS 1200u
#define SERVICE_CURSOR_BLINK_MS 512u

typedef enum {
    SERVICE_STAGE_MENU = 0,
    SERVICE_STAGE_PURCHASE_INLINE,
    SERVICE_STAGE_PURCHASE_EDITOR,
    SERVICE_STAGE_TRANSFER_CONFIRM,
    SERVICE_STAGE_TRANSFER_ACTIVE,
} service_stage_t;

typedef struct {
    const char *label;
    const char *preview;
    const char *softkey;
} service_menu_item_t;

static bool service_code_from_input(const app_t *app, char *out, size_t out_cap);
static void clear_standby_input(app_t *app);
static void open_service_text(app_t *app, uint8_t record_id, const char *a, const char *b, const char *c, uint32_t now);
static bool apply_service_codec_toggle(const char *code);
static uint8_t service_codec_byte_for_state(uint8_t before, bool efr, bool half_rate);
static bool service_codec_efr(uint8_t byte);
static bool service_codec_half_rate(uint8_t byte);
static void service_serial_text(char *dst, size_t cap);
static void service_serial_dialog_text(char *dst, size_t cap,
                                       const char *serial);
static void service_purchase_preview(char *dst, size_t cap);
static void service_life_timer_preview(char *dst, size_t cap);
static bool service_parse_mmyy(const char *text, uint8_t *month, uint8_t *year);
static service_menu_item_t service_menu_item(const app_t *app, uint8_t index, char *preview, size_t preview_cap);
static void submit_service_menu_item(app_t *app, uint32_t now);
static void open_purchase_inline(app_t *app, uint32_t now);
static void open_purchase_editor(app_t *app, uint32_t now);
static void submit_purchase_editor(app_t *app, uint32_t now);
static bool purchase_date_valid(const char *text);
static void purchase_insert_digit(app_t *app, char digit, uint32_t now);
static void purchase_delete_digit(app_t *app, uint32_t now);
static void purchase_move_cursor(app_t *app, int8_t delta, uint32_t now);
static void render_purchase_editor(const app_t *app, framebuffer_t *fb);
static void render_transfer_confirm(framebuffer_t *fb);
static void render_transfer_active(framebuffer_t *fb);

bool handle_standby_service_code(app_t *app, uint32_t now) {
    call_forward_request_t forward_request;
    call_forward_mmi_result_t forward_result =
        call_forward_mmi_parse(app->input_text, &forward_request);
    if (forward_result == CALL_FORWARD_MMI_VALID) {
        clear_standby_input(app);
        return call_divert_submit_mmi(app, &forward_request, now);
    }
    if (forward_result == CALL_FORWARD_MMI_INVALID) {
        /* A syntactically recognized supplementary code belongs to this
         * service even when malformed. Consume it instead of letting Navi dial
         * the literal stars and hashes as a telephone number. */
        clear_standby_input(app);
        open_display_sid(app, 32u, 0x210u, "Not done",
                         APP_ROUTE_STANDBY, now);
        return true;
    }

    char code[16];
    if (!service_code_from_input(app, code, sizeof(code))) {
        return false;
    }

    clear_standby_input(app);
    if (strcmp(code, "*#06") == 0) {
        char serial[STORE_WARRANTY_SERIAL_MAX + 1u];
        char text[96];
        service_serial_text(serial, sizeof(serial));
        service_serial_dialog_text(text, sizeof(text), serial);
        open_service_text(app, SERVICE_DISPLAY_RECORD_TEXT, text, 0, 0, now);
        return true;
    }
    if (strcmp(code, "*#0000") == 0) {
        /* Sisu build stamp, NOT the 3210 v6.00 version (a sanctioned deviation --
         * this is our firmware). Line 1 = short git hash, line 2 = build date
         * (both from generated/build_info.h, stamped at build time), line 3 = the
         * build-target board. The ORIGINAL 3210 *#0000# text is preserved in
         * docs/service_0000.md. */
        open_service_text(app, SERVICE_DISPLAY_RECORD_TEXT,
                          SISU_BUILD_HASH, SISU_BUILD_DATE, SISU_HW_REV_NAME, now);
        return true;
    }
    if (strcmp(code, "*#746025625") == 0) {
        open_service_text(app, SERVICE_DISPLAY_RECORD_INFO, "SIM clock", "stop not", "allowed", now);
        return true;
    }
    if (strcmp(code, "*#92702689") == 0) {
        open_service_codes_warranty(app, now);
        return true;
    }
    if (apply_service_codec_toggle(code)) {
        app->route = APP_ROUTE_STANDBY;
        app->dirty = true;
        return true;
    }
    return false;
}

void open_service_codes_warranty(app_t *app, uint32_t now) {
    app->route = APP_ROUTE_SERVICE_CODES;
    app->service_code_selected = 0u;
    app->service_code_stage = SERVICE_STAGE_MENU;
    app->service_code_purchase_draft[0] = '\0';
    app->service_code_purchase_cursor = 0u;
    app->service_code_cursor_visible = true;
    app->service_code_last_cursor_ms = now;
    app->service_code_transfer_started_ms = 0u;
    app->dirty = true;
}

bool handle_service_codes_key(app_t *app, uint16_t key, uint32_t now) {
    service_stage_t stage = (service_stage_t)app->service_code_stage;
    if (stage == SERVICE_STAGE_PURCHASE_EDITOR) {
        if (key == KEY_NAVI) {
            submit_purchase_editor(app, now);
            return true;
        }
        if (key == KEY_C) {
            purchase_delete_digit(app, now);
            return true;
        }
        if (key == KEY_UP) {
            purchase_move_cursor(app, 1, now);
            return true;
        }
        if (key == KEY_DOWN) {
            purchase_move_cursor(app, -1, now);
            return true;
        }
        char digit = key_digit(key);
        if (digit >= '0' && digit <= '9') {
            purchase_insert_digit(app, digit, now);
            return true;
        }
        return true;
    }

    if (stage == SERVICE_STAGE_TRANSFER_CONFIRM) {
        if (key == KEY_NAVI) {
            app->service_code_stage = SERVICE_STAGE_TRANSFER_ACTIVE;
            app->service_code_transfer_started_ms = now;
            app->dirty = true;
        } else if (key == KEY_C) {
            app->service_code_stage = SERVICE_STAGE_MENU;
            app->dirty = true;
        }
        return true;
    }

    if (stage == SERVICE_STAGE_TRANSFER_ACTIVE) {
        return true;
    }

    if (key == KEY_UP) {
        app->service_code_selected = app->service_code_selected == 0u ? 5u : (uint8_t)(app->service_code_selected - 1u);
        if (app->service_code_stage == SERVICE_STAGE_PURCHASE_INLINE && app->service_code_selected != 2u) {
            app->service_code_stage = SERVICE_STAGE_MENU;
        }
        app->dirty = true;
        return true;
    }
    if (key == KEY_DOWN) {
        app->service_code_selected = (uint8_t)((app->service_code_selected + 1u) % 6u);
        if (app->service_code_stage == SERVICE_STAGE_PURCHASE_INLINE && app->service_code_selected != 2u) {
            app->service_code_stage = SERVICE_STAGE_MENU;
        }
        app->dirty = true;
        return true;
    }
    if (key == KEY_NAVI) {
        submit_service_menu_item(app, now);
        return true;
    }
    if (key == KEY_C) {
        if (app->service_code_stage == SERVICE_STAGE_PURCHASE_INLINE) {
            app->service_code_stage = SERVICE_STAGE_MENU;
            app->dirty = true;
        }
        return true;
    }
    return true;
}

bool tick_service_codes(app_t *app, uint32_t now) {
    if (app->route != APP_ROUTE_SERVICE_CODES) {
        return false;
    }
    if (app->service_code_stage == SERVICE_STAGE_TRANSFER_ACTIVE &&
        time_diff_ms(now, app->service_code_transfer_started_ms + SERVICE_TRANSFER_RESULT_MS) >= 0) {
        app->service_code_stage = SERVICE_STAGE_MENU;
        app->service_code_transfer_started_ms = 0u;
        open_display(app, SERVICE_DISPLAY_RECORD_NOTE, "Connec-", "tion", "failed", APP_ROUTE_SERVICE_CODES, now);
        return true;
    }
    if (app->service_code_stage == SERVICE_STAGE_PURCHASE_EDITOR &&
        time_diff_ms(now, app->service_code_last_cursor_ms + SERVICE_CURSOR_BLINK_MS) >= 0) {
        app->service_code_cursor_visible = !app->service_code_cursor_visible;
        app->service_code_last_cursor_ms = now;
        return true;
    }
    return false;
}

void render_service_codes(const app_t *app, framebuffer_t *fb) {
    service_stage_t stage = (service_stage_t)app->service_code_stage;
    if (stage == SERVICE_STAGE_PURCHASE_EDITOR) {
        render_purchase_editor(app, fb);
        return;
    }
    if (stage == SERVICE_STAGE_TRANSFER_CONFIRM) {
        render_transfer_confirm(fb);
        return;
    }
    if (stage == SERVICE_STAGE_TRANSFER_ACTIVE) {
        render_transfer_active(fb);
        return;
    }

    char preview[24];
    service_menu_item_t item = service_menu_item(app, app->service_code_selected, preview, sizeof(preview));
    draw_static_page_list(fb,
                          item.label,
                          item.preview,
                          app->service_code_selected,
                          6u,
                          "",
                          item.softkey);
    if (stage == SERVICE_STAGE_PURCHASE_INLINE && app->service_code_selected == 2u) {
        fb_bitmap(fb, 14u, 0, 0, true, true);
    }
}

static bool service_code_from_input(const app_t *app, char *out, size_t out_cap) {
    if (app->input_len == 0u || app->input_text[app->input_len - 1u] != '#' || out == 0 || out_cap == 0u) {
        return false;
    }
    size_t len = app->input_len - 1u;
    if (len + 1u > out_cap) {
        return false;
    }
    memcpy(out, app->input_text, len);
    out[len] = '\0';
    return strcmp(out, "*#06") == 0 ||
           strcmp(out, "*#0000") == 0 ||
           strcmp(out, "*#746025625") == 0 ||
           strcmp(out, "*#92702689") == 0 ||
           strcmp(out, "*3370") == 0 ||
           strcmp(out, "#3370") == 0 ||
           strcmp(out, "*4720") == 0 ||
           strcmp(out, "#4720") == 0;
}

static void clear_standby_input(app_t *app) {
    app->input_len = 0u;
    app->input_text[0] = '\0';
    app->input_action = APP_STANDBY_ACTION_CALL;
    app->star_cycle_index = 0u;
    app->star_cycle_until_ms = 0u;
}

static void open_service_text(app_t *app, uint8_t record_id, const char *a, const char *b, const char *c, uint32_t now) {
    open_display(app, record_id, a, b, c, APP_ROUTE_STANDBY, now);
}

static bool apply_service_codec_toggle(const char *code) {
    uint8_t before = SERVICE_CODEC_DEFAULT_BYTE;
    store_setting_get_u8(STORE_SETTING_SERVICE_CODEC_BYTE, &before);
    bool efr = service_codec_efr(before);
    bool half_rate = service_codec_half_rate(before);
    if (strcmp(code, "*3370") == 0) {
        efr = true;
    } else if (strcmp(code, "#3370") == 0) {
        efr = false;
    } else if (strcmp(code, "*4720") == 0) {
        half_rate = true;
    } else if (strcmp(code, "#4720") == 0) {
        half_rate = false;
    } else {
        return false;
    }
    store_setting_set_u8(STORE_SETTING_SERVICE_CODEC_BYTE, service_codec_byte_for_state(before, efr, half_rate));
    core1_post_command(CORE1_CMD_AUDIO_SYSTEM_TONE, audio_arg(SERVICE_CODEC_TONE_INDEX, AUDIO_LEVEL_MAX));
    return true;
}

static uint8_t service_codec_byte_for_state(uint8_t before, bool efr, bool half_rate) {
    uint8_t low = 0x30u;
    if (efr && half_rate) {
        low = 0x27u;
    } else if (efr) {
        low = 0x23u;
    } else if (half_rate) {
        low = 0x38u;
    }
    return (uint8_t)((before & SERVICE_CODEC_HIGH_MASK) | low);
}

static bool service_codec_efr(uint8_t byte) {
    return (byte & 0x03u) != 0u;
}

static bool service_codec_half_rate(uint8_t byte) {
    return (byte & 0x0cu) != 0u;
}

static void service_serial_text(char *dst, size_t cap) {
    if (dst == 0 || cap == 0u) {
        return;
    }
    dst[0] = '\0';
    (void)store_board_imei_get(dst, cap);
}

static void service_serial_dialog_text(char *dst, size_t cap,
                                       const char *serial) {
    if (dst == 0 || cap == 0u) {
        return;
    }

    /* v6.00 handler 0x00255c00 formats PPM record index 337
     * (runtime SID = 337 + STRINGS_SID_BASE = 0x18b, "Serial No.\n%S") before
     * opening display record 0x1f. Preserve that localized template; %S is a
     * Nokia token, so expand it explicitly rather than handing it to the C
     * printf family where %S means a wide string. */
    const char *template =
        ts_or(SERVICE_SERIAL_TEMPLATE_SID, "Serial No.\n%S");
    const char *value = serial != 0 ? serial : "";
    size_t used = 0u;
    while (*template != '\0' && used + 1u < cap) {
        if (template[0] == '%' && template[1] == 'S') {
            size_t value_len = strlen(value);
            size_t room = cap - used - 1u;
            if (value_len > room) {
                value_len = room;
            }
            memcpy(&dst[used], value, value_len);
            used += value_len;
            template += 2;
            continue;
        }

        const char *next = template;
        asset_next_codepoint(&next);
        size_t bytes = (size_t)(next - template);
        if (used + bytes + 1u > cap) {
            break;
        }
        memcpy(&dst[used], template, bytes);
        used += bytes;
        template = next;
    }
    dst[used] = '\0';
}

static void service_purchase_preview(char *dst, size_t cap) {
    store_warranty_state_t warranty;
    if (store_warranty_get(&warranty) == STORE_STATUS_OK && strlen(warranty.purchase_date) == 4u) {
        copy_text(dst, cap, warranty.purchase_date);
    } else {
        copy_text(dst, cap, SERVICE_PURCHASE_MASK);
    }
}

static void service_life_timer_preview(char *dst, size_t cap) {
    uint32_t total_minutes = store_life_timer_seconds() / 60u;
    uint32_t hours = total_minutes / 60u;
    uint32_t minutes = total_minutes % 60u;
    if (hours > 9999u) {
        hours = 9999u;
        minutes = 59u;
    }
    snprintf(dst, cap, "%04lu:%02lu", (unsigned long)hours, (unsigned long)minutes);
}

static service_menu_item_t service_menu_item(const app_t *app, uint8_t index, char *preview, size_t preview_cap) {
    static const char *const LABELS[] = {
        "Serial No.:",
        "Made:",
        "Purchasing date:",
        "Repaired:",
        "Transfer user data?",
        "Life timer",
    };
    preview[0] = '\0';
    const char *softkey = "";
    store_warranty_state_t warranty;
    bool have_warranty = store_warranty_get(&warranty) == STORE_STATUS_OK;
    if (index == 0u) {
        service_serial_text(preview, preview_cap);
    } else if (index == 1u) {
        copy_text(preview, preview_cap, have_warranty && warranty.made[0] != '\0' ? warranty.made : "0899");
    } else if (index == 2u) {
        service_purchase_preview(preview, preview_cap);
        softkey = app->service_code_stage == SERVICE_STAGE_PURCHASE_INLINE ? "OK" : "Edit";
    } else if (index == 3u) {
        copy_text(preview, preview_cap, have_warranty && warranty.repaired[0] != '\0' ? warranty.repaired : "0000");
    } else if (index == 4u) {
        softkey = "OK";
    } else if (index == 5u) {
        service_life_timer_preview(preview, preview_cap);
    }
    service_menu_item_t item = {LABELS[index < 6u ? index : 0u], preview, softkey};
    return item;
}

static void submit_service_menu_item(app_t *app, uint32_t now) {
    if (app->service_code_selected == 2u) {
        if (app->service_code_stage == SERVICE_STAGE_PURCHASE_INLINE) {
            open_purchase_editor(app, now);
        } else {
            open_purchase_inline(app, now);
        }
    } else if (app->service_code_selected == 4u) {
        app->service_code_stage = SERVICE_STAGE_TRANSFER_CONFIRM;
        app->dirty = true;
    }
}

static void open_purchase_inline(app_t *app, uint32_t now) {
    app->service_code_stage = SERVICE_STAGE_PURCHASE_INLINE;
    app->service_code_purchase_draft[0] = '\0';
    app->service_code_purchase_cursor = 0u;
    app->service_code_cursor_visible = true;
    app->service_code_last_cursor_ms = now;
    app->dirty = true;
}

static void open_purchase_editor(app_t *app, uint32_t now) {
    store_warranty_state_t warranty;
    memset(&warranty, 0, sizeof(warranty));
    (void)store_warranty_get(&warranty);
    copy_text(app->service_code_purchase_draft,
              sizeof(app->service_code_purchase_draft),
              strlen(warranty.purchase_date) == 4u ? warranty.purchase_date : SERVICE_PURCHASE_MASK);
    app->service_code_purchase_cursor = (uint8_t)strlen(app->service_code_purchase_draft);
    app->service_code_stage = SERVICE_STAGE_PURCHASE_EDITOR;
    app->service_code_cursor_visible = true;
    app->service_code_last_cursor_ms = now;
    app->dirty = true;
}

static void submit_purchase_editor(app_t *app, uint32_t now) {
    if (!purchase_date_valid(app->service_code_purchase_draft)) {
        app->service_code_stage = SERVICE_STAGE_PURCHASE_EDITOR;
        open_display(app, SERVICE_DISPLAY_RECORD_ERROR, "Invalid date,", "use format", "MMYY", APP_ROUTE_SERVICE_CODES, now);
        return;
    }
    store_warranty_set_purchase_date(app->service_code_purchase_draft);
    app->service_code_stage = SERVICE_STAGE_MENU;
    open_display(app, SERVICE_DISPLAY_RECORD_NOTE, "Saved", 0, 0, APP_ROUTE_SERVICE_CODES, now);
}

static bool service_parse_mmyy(const char *text, uint8_t *month, uint8_t *year) {
    if (text == 0 || month == 0 || year == 0 || strlen(text) != 4u) {
        return false;
    }
    for (uint8_t i = 0; i < 4u; i++) {
        if (text[i] < '0' || text[i] > '9') {
            return false;
        }
    }
    *month = (uint8_t)((text[0] - '0') * 10 + (text[1] - '0'));
    *year = (uint8_t)((text[2] - '0') * 10 + (text[3] - '0'));
    return true;
}

static bool purchase_date_valid(const char *text) {
    uint8_t purchase_month = 0u;
    uint8_t purchase_year = 0u;
    if (!service_parse_mmyy(text, &purchase_month, &purchase_year)) {
        return false;
    }
    uint8_t made_month = 8u;
    uint8_t made_year = 99u;
    store_warranty_state_t warranty;
    if (store_warranty_get(&warranty) == STORE_STATUS_OK) {
        uint8_t parsed_month = 0u;
        uint8_t parsed_year = 0u;
        if (service_parse_mmyy(warranty.made, &parsed_month, &parsed_year)) {
            made_month = parsed_month;
            made_year = parsed_year;
        }
    }
    if (purchase_month > 12u) {
        return false;
    }
    if (made_year > purchase_year) {
        return (uint8_t)(made_year - purchase_year) > 70u;
    }
    if (made_year < purchase_year) {
        return (uint8_t)(purchase_year - made_year) <= 70u;
    }
    return made_month <= purchase_month;
}

static void purchase_insert_digit(app_t *app, char digit, uint32_t now) {
    uint8_t len = (uint8_t)strlen(app->service_code_purchase_draft);
    if (len >= 4u) {
        return;
    }
    uint8_t cursor = app->service_code_purchase_cursor;
    if (cursor > len) {
        cursor = len;
    }
    for (uint8_t i = len; i > cursor; i--) {
        app->service_code_purchase_draft[i] = app->service_code_purchase_draft[i - 1u];
    }
    app->service_code_purchase_draft[cursor] = digit;
    app->service_code_purchase_draft[len + 1u] = '\0';
    app->service_code_purchase_cursor = (uint8_t)(cursor + 1u);
    app->service_code_cursor_visible = true;
    app->service_code_last_cursor_ms = now;
    app->dirty = true;
}

static void purchase_delete_digit(app_t *app, uint32_t now) {
    uint8_t len = (uint8_t)strlen(app->service_code_purchase_draft);
    if (len == 0u) {
        app->service_code_stage = SERVICE_STAGE_PURCHASE_INLINE;
        app->dirty = true;
        return;
    }
    uint8_t cursor = app->service_code_purchase_cursor;
    if (cursor == 0u) {
        return;
    }
    if (cursor > len) {
        cursor = len;
    }
    for (uint8_t i = (uint8_t)(cursor - 1u); i < len; i++) {
        app->service_code_purchase_draft[i] = app->service_code_purchase_draft[i + 1u];
    }
    app->service_code_purchase_cursor = (uint8_t)(cursor - 1u);
    app->service_code_cursor_visible = true;
    app->service_code_last_cursor_ms = now;
    app->dirty = true;
}

static void purchase_move_cursor(app_t *app, int8_t delta, uint32_t now) {
    int cursor = app->service_code_purchase_cursor + delta;
    int len = (int)strlen(app->service_code_purchase_draft);
    if (cursor < 0) {
        cursor = 0;
    } else if (cursor > len) {
        cursor = len;
    }
    app->service_code_purchase_cursor = (uint8_t)cursor;
    app->service_code_cursor_visible = true;
    app->service_code_last_cursor_ms = now;
    app->dirty = true;
}

static void render_purchase_editor(const app_t *app, framebuffer_t *fb) {
    fb_clear(fb, false);
    const font_t *title_font = asset_font(FONT_FS2);
    const font_t *value_font = asset_font(FONT_FS0);
    fb_bitmap(fb, 14u, 0, 0, true, true);
    fb_text(fb, title_font, "Enter date:", 0, 7, true, FB_WIDTH);
    fb_rect(fb, 0, 17, 84, 20, true);
    int value_w = asset_text_width(value_font, app->service_code_purchase_draft);
    int tx = 2 + 80 - value_w;
    if (tx < 2) {
        tx = 2;
    }
    fb_text(fb, value_font, app->service_code_purchase_draft, tx, 23, true, 80);
    if (app->service_code_cursor_visible) {
        char prefix[5];
        uint8_t cursor = app->service_code_purchase_cursor;
        uint8_t len = (uint8_t)strlen(app->service_code_purchase_draft);
        if (cursor > len) {
            cursor = len;
        }
        memcpy(prefix, app->service_code_purchase_draft, cursor);
        prefix[cursor] = '\0';
        int cx = tx + asset_text_width(value_font, prefix) + 1;
        if (cx < 1) {
            cx = 1;
        } else if (cx > 82) {
            cx = 82;
        }
        fb_vline(fb, cx, 22, value_font->height + 1, true);
    }
    draw_softkey(fb, "OK");
}

static void render_transfer_confirm(framebuffer_t *fb) {
    fb_clear(fb, false);
    const font_t *font = asset_font(FONT_FS2);
    fb_text(fb, font, "Confirm", 6, 7, true, 72);
    fb_text(fb, font, "transfer?", 6, 16, true, 72);
    draw_softkey(fb, "OK");
}

static void render_transfer_active(framebuffer_t *fb) {
    fb_clear(fb, false);
    const font_t *font = asset_font(FONT_FS2);
    fb_text(fb, font, "Transfer", 0, 7, true, FB_WIDTH);
    fb_text(fb, font, "active", 0, 16, true, FB_WIDTH);
}

#include "apps/messages_app.h"
#include "messages_internal.h"
#include "ui/menu_visible.h"

#include <stdio.h>
#include <string.h>

#include "audio/audio_levels.h"
#include "app_internal.h"
#include "apps/dialogs_app.h"
#include "apps/profiles_app.h"
#include "ui/assets.h"
#include "services/core1_services.h"
#include "services/feature_gates.h"
#include "services/input_keys.h"
#include "services/log.h"
#include "services/modem_service.h"
#include "services/message_service.h"
#include "services/sms_picture_codec.h"
#include "storage/store_service.h"
#include "services/strings.h"
#include "services/timebase.h"

#define SMS_SEND_TIMEOUT_MS 60000u
#define SMS_MAILBOX_TIMEOUT_MS 60000u
#define SMS_READ_TIMEOUT_MS 30000u
#define SMS_DELETE_TIMEOUT_MS 10000u
#define SMS_SAVE_TIMEOUT_MS 20000u
#define SMS_MAILBOX_TEXT_X 10
#define SMS_MAILBOX_TEXT_W (FB_WIDTH - SMS_MAILBOX_TEXT_X)
#define SMS_MAILBOX_ROW_Y 8
#define SMS_MAILBOX_ROW_PITCH 10
#define SMS_MAILBOX_ROW_H 10
#define SMS_MAILBOX_HIGHLIGHT_X 8
#define SMS_IN_CALL_TONE_INDEX 14u
/* Presence gate for the dedicated core1 command. Its bench-trimmed amplitude
 * deliberately lives below the global level-1 floor. */
#define SMS_IN_CALL_TONE_LEVEL 1u
/* v6.00 status envelopes (row builder 0x0022782c): read/unread/sent/unsent. */
#define SMS_INBOX_READ_ICON_ID 42u
#define SMS_INBOX_UNREAD_ICON_ID 43u
#define SMS_OUTBOX_SENT_ICON_ID 44u
#define SMS_OUTBOX_UNSENT_ICON_ID 45u
/* Compared prefix of the fixed-width "YY/MM/DD,HH:MM:SS" SMS timestamp -- enough
 * to order chronologically by a plain strncmp (newest-first when descending). */
#define SMS_TIMESTAMP_SORT_LEN 17u

static const char *const MESSAGES_ROOT_LABELS[] = {
    "Inbox",
    "Outbox",
    "Write\nmessages",
    "Picture messages",
    "Message settings",
    "Info\nservice",
    "Voice\nmailbox\nnumber",
};
static const char *const MESSAGE_INBOX_OPTIONS[] = {"Reply", "Forward", "Edit", "Use number", "Erase", "Details"};
static const char *const MESSAGE_OUTBOX_OPTIONS[] = {"Send", "Edit", "Use number", "Erase", "Details"};
static const char *const MESSAGE_SETTINGS_TOP_LABELS[] = {"Set 1", "Common"};
static const char *const MESSAGE_SETTINGS_SET_LABELS[] = {"Message centre number", "Messages sent as", "Message validity"};
static const char *const MESSAGE_SETTINGS_COMMON_LABELS[] = {"Delivery reports", "Reply via\nsame centre"};
static const char *const MESSAGE_SENT_AS_LABELS[] = {"Text", "Fax", "Paging", "E-mail"};
static const char *const MESSAGE_VALIDITY_LABELS[] = {"1 hour", "6 hours", "24 hours", "72 hours", "1 week", "Maximum time"};
static const char *const MESSAGE_YES_NO_LABELS[] = {"Yes", "No"};
static const char *const MESSAGE_INFO_SERVICE_LABELS[] = {"On", "Off", "Topic index", "Topics", "Read", "Language"};

/* v6.00 string ids (SIDs) paralleling each localizable label array, same order.
 * The English arrays above stay BOTH the fallback AND (where an array is used as
 * a strcmp dispatch key) the dispatch key -- only the rendered copy is localized
 * (ts_or). sid 0 = no unambiguous 1:1 v6.00 match (the label resolves to several
 * context-specific SIDs, so localizing it would risk a wrong translation); the
 * label is left English. Per-app trace map (2nd pass) resolved several formerly-
 * ambiguous slots (Edit 0x344, Save 0x17e, Exit 0x173, the settings labels).
 * Yes/No, On/Off, Send, Forward, Use number, Erase, Details, Fax, Read, Language,
 * Set 1, Dictionary stay English -- absent from the map (no unambiguous record). */
static const uint16_t MESSAGES_ROOT_SID[] = {
    0x330u, 0x34bu, 0x341u, 0x17au, 0x34au, 0xc7u, 0x3f8u,
};
static const uint16_t MESSAGE_INBOX_OPTIONS_SID[] = {0x347u, 0u, 0x344u, 0u, 0u, 0u}; /* Reply, Edit */
static const uint16_t MESSAGE_OUTBOX_OPTIONS_SID[] = {0u, 0x344u, 0u, 0u, 0u};    /* Edit */
static const uint16_t MESSAGE_SETTINGS_TOP_SID[] = {0u, 0x31au};                  /* Common */
static const uint16_t MESSAGE_SETTINGS_SET_SID[] = {0x335u, 0x336u, 0x371u};      /* Message centre number, Messages sent as, Message validity */
static const uint16_t MESSAGE_SETTINGS_COMMON_SID[] = {0x31fu, 0x353u};           /* Delivery reports, Reply via same centre */
static const uint16_t MESSAGE_SENT_AS_SID[] = {0x33bu, 0u, 0x34du, 0x323u};       /* Text, -, Paging, E-mail */
static const uint16_t MESSAGE_VALIDITY_SID[] = {0x30du, 0x310u, 0x30fu, 0x311u, 0x30eu, 0x334u};
static const uint16_t MESSAGE_INFO_SERVICE_SID[] = {0u, 0u, 0xaeu, 0xc6u, 0u, 0u}; /* Topic index, Topics */

/* Localize an English label array by its parallel SID array into caller storage.
 * Each slot resolves to the active language's string, or the English literal for
 * a sid-0 (ambiguous / no-match) slot. The English array is untouched, so it
 * remains valid as a strcmp dispatch key. */
static void msg_localize_labels(const char *const *labels,
                                const uint16_t *sids,
                                uint8_t count,
                                const char **out,
                                uint8_t out_cap) {
    for (uint8_t i = 0u; i < count && i < out_cap; i++) {
        out[i] = ts_or(sids[i], labels[i]);
    }
}


static bool sms_async_ui_active(const app_t *app);
static void sms_finish_submitted_text(app_t *app, bool sent);
static const app_sms_record_t *current_sms_record_const(const app_t *app);
static uint16_t current_sms_count(const app_t *app);
static bool sms_outbox_is_recipientless(const app_sms_record_t *record);
static uint8_t sms_outbox_option_count(const app_sms_record_t *record);
static const char *sms_record_label(const app_sms_record_t *record, bool outbox, char *scratch, size_t cap);
static void sms_sender_page_text(const app_sms_record_t *record, char *dst, size_t cap);
static void sms_recipient_page_text(const app_sms_record_t *record, char *dst, size_t cap);
static void sms_timestamp_text(const char *timestamp, char *dst, size_t cap);
static void sms_detail_page_text(const app_t *app, char *dst, size_t cap);
static void sms_use_number_candidate(const app_sms_record_t *record,
                                     const char *body, char *dst, size_t cap);
static void sms_delete_current(app_t *app, uint32_t now);
static void sms_remove_current_local(app_t *app, uint32_t now);
static void sms_open_empty_notice(app_t *app, uint32_t now);
static void sms_begin_read_current(app_t *app, uint32_t now);
static void sms_open_current_options(app_t *app);
static void sms_select_read_option(app_t *app, uint32_t now);
static void sms_open_settings_menu(app_t *app);
static void sms_select_settings_item(app_t *app, uint32_t now);
static void sms_save_settings_value(app_t *app, uint32_t now);
static void play_message_alert(app_t *app, const modem_status_t *status,
                               uint32_t now);
static const char *messages_settings_preview(const app_t *app, uint8_t index, char *scratch, size_t cap);
static const char *messages_settings_value_label(uint8_t key, uint8_t value);
static uint8_t messages_settings_value_count(uint8_t key);
static store_setting_key_t messages_settings_store_key(uint8_t key);
static void messages_mark_current_read(app_t *app);
static bool messages_root_item_visible(uint8_t raw);
static uint8_t messages_root_visible_count(void);
static uint8_t messages_root_visible_index(uint8_t raw);
static uint8_t messages_root_raw_at_visible(uint8_t visible);
static uint8_t messages_root_normalized_raw(uint8_t raw);
static void messages_menu_step(app_t *app, int8_t delta);

void messages_app_init(app_t *app) {
    messages_composer_init(app);
    message_status_t status;
    message_service_get_status(&status);
    app->sms_unread_count = status.unread;
    app->last_local_sms_received_count = status.received;
}

void render_messages_menu(const app_t *app, framebuffer_t *fb) {
    uint8_t count = messages_root_visible_count();
    uint8_t selected = messages_root_normalized_raw(app->messages_menu_selected);
    uint8_t visible_selected = messages_root_visible_index(selected);
    char breadcrumb[8];
    char preview[8] = "";
    snprintf(breadcrumb, sizeof(breadcrumb), "2-%u", (unsigned)(selected + 1u));
    if (selected == 5u) {
        uint8_t enabled = 0u;
        store_setting_get_u8(STORE_SETTING_SMS_INFO_SERVICE, &enabled);
        copy_text(preview, sizeof(preview), enabled ? "On" : "Off");
    }
    draw_static_page_list(fb,
                          ts_or(MESSAGES_ROOT_SID[selected], MESSAGES_ROOT_LABELS[selected]),
                          preview,
                          visible_selected,
                          count,
                          breadcrumb,
                          "Select");
}

void render_messages_list(const app_t *app, framebuffer_t *fb) {
    fb_clear(fb, false);
    const font_t *font = asset_font(FONT_FS2);
    const font_t *counter = asset_font(FONT_FS3);
    if (app->messages_mode == MESSAGES_MODE_SETTINGS_TOP) {
        char crumb[8];
        snprintf(crumb, sizeof(crumb), "2-5-%u", (unsigned)(app->messages_settings_top_selected + 1u));
        const char *top[ARRAY_COUNT(MESSAGE_SETTINGS_TOP_LABELS)];
        msg_localize_labels(MESSAGE_SETTINGS_TOP_LABELS,
                            MESSAGE_SETTINGS_TOP_SID,
                            (uint8_t)ARRAY_COUNT(MESSAGE_SETTINGS_TOP_LABELS),
                            top,
                            (uint8_t)ARRAY_COUNT(top));
        draw_flat_list_at_y(fb,
                            top,
                            (uint8_t)ARRAY_COUNT(MESSAGE_SETTINGS_TOP_LABELS),
                            app->messages_settings_top_selected,
                            crumb,
                            "Select",
                            7);
        return;
    }
    if (app->messages_mode == MESSAGES_MODE_SETTINGS_MENU) {
        const char *const *labels = app->messages_settings_kind == 0u
            ? MESSAGE_SETTINGS_SET_LABELS
            : MESSAGE_SETTINGS_COMMON_LABELS;
        const uint16_t *sids = app->messages_settings_kind == 0u
            ? MESSAGE_SETTINGS_SET_SID
            : MESSAGE_SETTINGS_COMMON_SID;
        uint8_t count = app->messages_settings_kind == 0u
            ? (uint8_t)ARRAY_COUNT(MESSAGE_SETTINGS_SET_LABELS)
            : (uint8_t)ARRAY_COUNT(MESSAGE_SETTINGS_COMMON_LABELS);
        uint8_t selected = app->messages_settings_selected >= count ? 0u : app->messages_settings_selected;
        char crumb[10];
        char preview[24];
        snprintf(crumb,
                 sizeof(crumb),
                 "2-5-%u-%u",
                 (unsigned)(app->messages_settings_kind == 0u ? 1u : 2u),
                 (unsigned)(selected + 1u));
        draw_static_page_list(fb,
                              ts_or(sids[selected], labels[selected]),
                              messages_settings_preview(app, selected, preview, sizeof(preview)),
                              selected,
                              count,
                              crumb,
                              "Select");
        return;
    }
    if (app->messages_mode == MESSAGES_MODE_SETTINGS_VALUE) {
        const char *const *labels = MESSAGE_YES_NO_LABELS;
        const uint16_t *sids = 0; /* Yes/No ambiguous -> rendered English */
        uint8_t count = messages_settings_value_count(app->messages_settings_value_key);
        if (app->messages_settings_value_key == 1u) {
            labels = MESSAGE_SENT_AS_LABELS;
            sids = MESSAGE_SENT_AS_SID;
        } else if (app->messages_settings_value_key == 2u) {
            labels = MESSAGE_VALIDITY_LABELS;
            sids = MESSAGE_VALIDITY_SID;
        }
        const char *loc[6];
        if (count > (uint8_t)ARRAY_COUNT(loc)) {
            count = (uint8_t)ARRAY_COUNT(loc);
        }
        for (uint8_t i = 0u; i < count; i++) {
            loc[i] = sids != 0 ? ts_or(sids[i], labels[i]) : labels[i];
        }
        draw_flat_list_at_y(fb, loc, count, app->messages_settings_value_selected, "", "OK", 7);
        return;
    }
    if (app->messages_mode == MESSAGES_MODE_INFO) {
        char crumb[8];
        snprintf(crumb, sizeof(crumb), "2-6-%u", (unsigned)(app->messages_info_selected + 1u));
        const char *info[ARRAY_COUNT(MESSAGE_INFO_SERVICE_LABELS)];
        msg_localize_labels(MESSAGE_INFO_SERVICE_LABELS,
                            MESSAGE_INFO_SERVICE_SID,
                            (uint8_t)ARRAY_COUNT(MESSAGE_INFO_SERVICE_LABELS),
                            info,
                            (uint8_t)ARRAY_COUNT(info));
        draw_flat_list_at_y(fb,
                            info,
                            (uint8_t)ARRAY_COUNT(MESSAGE_INFO_SERVICE_LABELS),
                            app->messages_info_selected,
                            crumb,
                            "Select",
                            7);
        return;
    }

    if (app->messages_kind == MESSAGES_KIND_PICTURES) {
        messages_picture_render(app, fb);
        return;
    }

    const app_sms_record_t *record = current_sms_record_const(app);
    if (app->messages_mode == MESSAGES_MODE_OPTIONS) {
        const char *const *options = app->messages_kind == MESSAGES_KIND_OUTBOX
            ? MESSAGE_OUTBOX_OPTIONS
            : MESSAGE_INBOX_OPTIONS;
        uint8_t count = app->messages_kind == MESSAGES_KIND_OUTBOX
            ? sms_outbox_option_count(record)
            : (uint8_t)ARRAY_COUNT(MESSAGE_INBOX_OPTIONS);
        /* Outbox localizes "Edit" (0x344); inbox localizes "Reply" (0x347) +
         * "Edit" (0x344). The other entries are absent from the map -> English. */
        const uint16_t *sids = app->messages_kind == MESSAGES_KIND_OUTBOX
            ? MESSAGE_OUTBOX_OPTIONS_SID
            : MESSAGE_INBOX_OPTIONS_SID;
        const char *loc[6];
        if (count > (uint8_t)ARRAY_COUNT(loc)) {
            count = (uint8_t)ARRAY_COUNT(loc);
        }
        for (uint8_t i = 0u; i < count; i++) {
            loc[i] = sids != 0 ? ts_or(sids[i], options[i]) : options[i];
        }
        draw_flat_list(fb, loc, count, app->messages_option_selected, "", "Select");
        return;
    }

    if (app->messages_mode == MESSAGES_MODE_READ) {
        char detail[PHONEBOOK_NAME_MAX + MODEM_SMS_SENDER_MAX + 32u];
        const char *text = detail;
        if (record == 0 || !app->sms_selected_content.valid) {
            return;
        }
        if (app->messages_kind == MESSAGES_KIND_INBOX && app->messages_read_page == 1u) {
            sms_sender_page_text(record, detail, sizeof(detail));
        } else if (app->messages_kind == MESSAGES_KIND_INBOX && app->messages_read_page == 2u) {
            char ts[40];
            sms_timestamp_text(record->timestamp, ts, sizeof(ts));
            copy_text(detail, sizeof(detail), "Sent:\n");
            strncat(detail, ts, sizeof(detail) - strlen(detail) - 1u);
        } else {
            const char *body = app->sms_selected_content.text;
            text = body[0] ? body : "Message";
        }
        /* Read pages word-wrap like the original (no mid-word breaks). Count
         * the complete body through the iterator so scrolling reaches its
         * tail without retaining a max-sized matrix in BSS. */
        uint16_t line_count = ui_wrap_line_count(font, text, FB_WIDTH);
        uint16_t start = app->messages_read_page == 0u
            ? app->messages_read_scroll : 0u;
        char line[UI_WRAP_LINE_BYTES];
        for (uint8_t i = 0u; i < 4u && start + i < line_count; i++) {
            if (ui_wrap_line_at(font, text, FB_WIDTH,
                                (uint16_t)(start + i), line,
                                sizeof(line))) {
                fb_text(fb, font, line, 0, i * 9, true, FB_WIDTH);
            }
        }
        draw_softkey(fb, "Options");
        return;
    }

    if (app->messages_mode == MESSAGES_MODE_DETAIL) {
        char text[96];
        sms_detail_page_text(app, text, sizeof(text));
        draw_text_block(fb, font, text, 0, 0, FB_WIDTH, 9, 4u);
        draw_softkey(fb, "Back");
        return;
    }

    uint16_t count = current_sms_count(app);
    if (count == 0u) {
        fb_text(fb, asset_font(FONT_FS0), "No messages", 0, 9, true, FB_WIDTH);
        return;
    }
    uint16_t selected = app->messages_selected >= count ? 0u : app->messages_selected;
    char crumb[12];
    snprintf(crumb,
             sizeof(crumb),
             "2-%u-%u",
             app->messages_kind == MESSAGES_KIND_OUTBOX ? 2u : 1u,
             (unsigned)(selected + 1u));
    draw_right_text_box(fb, counter, crumb, 0, 0, FB_WIDTH);
    uint16_t start = selected > 2u ? (uint16_t)(selected - 2u) : 0u;
    if (count > 3u && start > count - 3u) {
        start = (uint16_t)(count - 3u);
    }
    for (uint8_t row = 0; row < 3u && start + row < count; row++) {
        uint16_t index = (uint16_t)(start + row);
        const app_sms_record_t *item = app->messages_kind == MESSAGES_KIND_OUTBOX
            ? &app->sms_outbox[index]
            : &app->sms_inbox[index];
        int y = SMS_MAILBOX_ROW_Y + row * SMS_MAILBOX_ROW_PITCH;
        bool selected_row = index == selected;
        if (selected_row) {
            fb_fill_rect(fb, SMS_MAILBOX_HIGHLIGHT_X, y - 1, FB_WIDTH - SMS_MAILBOX_HIGHLIGHT_X, SMS_MAILBOX_ROW_H, true);
        }
        /* 1:1 four-status envelope (v6.00 row builder 0x0022782c): inbox READ=42
         * / UNREAD=43, outbox SENT=44 / UNSENT=45. Local file state
         * is the source of truth for both groups. */
        uint16_t icon_id;
        if (app->messages_kind == MESSAGES_KIND_OUTBOX) {
            icon_id = !item->sent ? SMS_OUTBOX_UNSENT_ICON_ID
                                                          : SMS_OUTBOX_SENT_ICON_ID;
        } else {
            icon_id = !item->read ? SMS_INBOX_UNREAD_ICON_ID
                                                         : SMS_INBOX_READ_ICON_ID;
        }
        fb_bitmap(fb, icon_id, 0, y, true, true);
        char label[24];
        sms_record_label(item,
                         app->messages_kind == MESSAGES_KIND_OUTBOX,
                         label,
                         sizeof(label));
        if (selected_row) {
            char scroll[44];
            fb_text(fb,
                    font,
                    ui_marquee_text(font, label, SMS_MAILBOX_TEXT_W, scroll, sizeof(scroll)),
                    SMS_MAILBOX_TEXT_X,
                    y,
                    false,
                    SMS_MAILBOX_TEXT_W);
        } else {
            fb_text(fb, font, label, SMS_MAILBOX_TEXT_X, y, true, SMS_MAILBOX_TEXT_W);
        }
    }
    draw_softkey(fb, "Read");
}


bool handle_messages_menu_key(app_t *app, uint16_t key, uint32_t now) {
    uint8_t count = messages_root_visible_count();
    if (count == 0u) {
        return true;
    }
    app->messages_menu_selected = messages_root_normalized_raw(app->messages_menu_selected);
    if (key == KEY_UP) {
        messages_menu_step(app, -1);
        return true;
    }
    if (key == KEY_DOWN) {
        messages_menu_step(app, 1);
        return true;
    }
    if (key == KEY_C) {
        app->menu_index = 1u;
        app->route = APP_ROUTE_MAIN_MENU;
        app->dirty = true;
        return true;
    }
    if (key != KEY_NAVI) {
        return true;
    }
    switch (app->messages_menu_selected) {
    case 0:
        open_messages_mailbox(app, MESSAGES_KIND_INBOX, now);
        break;
    case 1:
        open_messages_mailbox(app, MESSAGES_KIND_OUTBOX, now);
        break;
    case 2:
        open_sms_composer(app, "", "", now);
        break;
    case 3:
        messages_picture_open_menu(app, now);
        break;
    case 4:
        app->messages_kind = MESSAGES_KIND_SETTINGS;
        app->messages_mode = MESSAGES_MODE_SETTINGS_TOP;
        app->messages_settings_top_selected = 0u;
        app->route = APP_ROUTE_MESSAGES_LIST;
        app->dirty = true;
        break;
    case 5:
        app->messages_kind = MESSAGES_KIND_INFO;
        app->messages_mode = MESSAGES_MODE_INFO;
        app->messages_info_selected = 0u;
        app->route = APP_ROUTE_MESSAGES_LIST;
        app->dirty = true;
        break;
    case 6: {
        char value[STORE_TEXT_MAX + 1u];
        store_setting_get_text(STORE_SETTING_SYSTEM_VOICE_MAILBOX_NUMBER, value, sizeof(value));
        if (value[0] == '\0') {
            (void)modem_service_get_voice_mailbox_number(value,
                                                         sizeof(value));
        }
        open_editor(app, "Enter number:", value, 30u, EDITOR_KIND_NUMBER, EDITOR_CONTEXT_VOICE_MAILBOX_NUMBER, true, now);
        break;
    }
    default:
        break;
    }
    return true;
}

bool handle_messages_list_key(app_t *app, uint16_t key, uint32_t now) {
    if (app->messages_mode == MESSAGES_MODE_SETTINGS_TOP) {
        if (key == KEY_UP || key == KEY_DOWN) {
            app->messages_settings_top_selected ^= 1u;
            app->dirty = true;
            return true;
        }
        if (key == KEY_C) {
            open_messages_menu(app, 4u);
            return true;
        }
        if (key == KEY_NAVI) {
            sms_open_settings_menu(app);
            return true;
        }
        return true;
    }
    if (app->messages_mode == MESSAGES_MODE_SETTINGS_MENU) {
        uint8_t count = app->messages_settings_kind == 0u
            ? (uint8_t)ARRAY_COUNT(MESSAGE_SETTINGS_SET_LABELS)
            : (uint8_t)ARRAY_COUNT(MESSAGE_SETTINGS_COMMON_LABELS);
        if (key == KEY_UP) {
            app->messages_settings_selected = app->messages_settings_selected == 0u ? (uint8_t)(count - 1u) : (uint8_t)(app->messages_settings_selected - 1u);
            app->dirty = true;
            return true;
        }
        if (key == KEY_DOWN) {
            app->messages_settings_selected = (uint8_t)((app->messages_settings_selected + 1u) % count);
            app->dirty = true;
            return true;
        }
        if (key == KEY_C) {
            app->messages_mode = MESSAGES_MODE_SETTINGS_TOP;
            app->dirty = true;
            return true;
        }
        if (key == KEY_NAVI) {
            sms_select_settings_item(app, now);
            return true;
        }
        return true;
    }
    if (app->messages_mode == MESSAGES_MODE_SETTINGS_VALUE) {
        uint8_t count = messages_settings_value_count(app->messages_settings_value_key);
        if (key == KEY_UP) {
            app->messages_settings_value_selected = app->messages_settings_value_selected == 0u ? (uint8_t)(count - 1u) : (uint8_t)(app->messages_settings_value_selected - 1u);
            app->dirty = true;
            return true;
        }
        if (key == KEY_DOWN) {
            app->messages_settings_value_selected = (uint8_t)((app->messages_settings_value_selected + 1u) % count);
            app->dirty = true;
            return true;
        }
        if (key == KEY_C) {
            app->messages_mode = MESSAGES_MODE_SETTINGS_MENU;
            app->dirty = true;
            return true;
        }
        if (key == KEY_NAVI) {
            sms_save_settings_value(app, now);
            return true;
        }
        return true;
    }
    if (app->messages_mode == MESSAGES_MODE_INFO) {
        uint8_t count = (uint8_t)ARRAY_COUNT(MESSAGE_INFO_SERVICE_LABELS);
        if (key == KEY_UP) {
            app->messages_info_selected = app->messages_info_selected == 0u ? (uint8_t)(count - 1u) : (uint8_t)(app->messages_info_selected - 1u);
            app->dirty = true;
            return true;
        }
        if (key == KEY_DOWN) {
            app->messages_info_selected = (uint8_t)((app->messages_info_selected + 1u) % count);
            app->dirty = true;
            return true;
        }
        if (key == KEY_C) {
            open_messages_menu(app, 5u);
            return true;
        }
        if (key == KEY_NAVI) {
            if (app->messages_info_selected <= 1u) {
                store_setting_set_u8(STORE_SETTING_SMS_INFO_SERVICE, app->messages_info_selected == 0u ? 1u : 0u);
                open_display_sid(app, 3u, 0x3b4u, "Saved", APP_ROUTE_MESSAGES_LIST, now);
            } else {
                open_display_sid(app, 2u, 0xb5u, "No info\nmessages\non this topic", APP_ROUTE_MESSAGES_LIST, now);
            }
            return true;
        }
        return true;
    }

    if (app->messages_kind == MESSAGES_KIND_PICTURES) {
        return messages_picture_handle_key(app, key, now);
    }

    if (app->messages_mode == MESSAGES_MODE_OPTIONS) {
        const app_sms_record_t *record = current_sms_record_const(app);
        uint8_t count = app->messages_kind == MESSAGES_KIND_OUTBOX
            ? sms_outbox_option_count(record)
            : (uint8_t)ARRAY_COUNT(MESSAGE_INBOX_OPTIONS);
        if (key == KEY_UP) {
            app->messages_option_selected = app->messages_option_selected == 0u ? (uint8_t)(count - 1u) : (uint8_t)(app->messages_option_selected - 1u);
            app->dirty = true;
            return true;
        }
        if (key == KEY_DOWN) {
            app->messages_option_selected = (uint8_t)((app->messages_option_selected + 1u) % count);
            app->dirty = true;
            return true;
        }
        if (key == KEY_C) {
            app->messages_mode = MESSAGES_MODE_READ;
            app->dirty = true;
            return true;
        }
        if (key == KEY_NAVI) {
            sms_select_read_option(app, now);
            return true;
        }
        return true;
    }
    if (app->messages_mode == MESSAGES_MODE_DETAIL) {
        if (key == KEY_UP && app->messages_detail_page > 0u) {
            app->messages_detail_page--;
            app->dirty = true;
        } else if (key == KEY_DOWN && app->messages_detail_page < 2u) {
            app->messages_detail_page++;
            app->dirty = true;
        } else if (key == KEY_C || key == KEY_NAVI) {
            app->messages_mode = MESSAGES_MODE_OPTIONS;
            app->dirty = true;
        }
        return true;
    }
    if (app->messages_mode == MESSAGES_MODE_READ) {
        if (key == KEY_NAVI) {
            sms_open_current_options(app);
            return true;
        }
        if (key == KEY_C) {
            app->messages_mode = MESSAGES_MODE_LIST;
            app->dirty = true;
            return true;
        }
        if (key == KEY_UP || key == KEY_DOWN) {
            int delta = key == KEY_DOWN ? 1 : -1;
            const app_sms_record_t *record = current_sms_record_const(app);
            if (record == 0) {
                return true;
            }
            if (app->messages_read_page == 0u) {
                uint16_t line_count = ui_wrap_line_count(
                    asset_font(FONT_FS2), app->sms_selected_content.text,
                    FB_WIDTH);
                if (delta > 0 && app->messages_read_scroll + 4u < line_count) {
                    app->messages_read_scroll++;
                    app->dirty = true;
                    return true;
                }
                if (delta < 0 && app->messages_read_scroll > 0u) {
                    app->messages_read_scroll--;
                    app->dirty = true;
                    return true;
                }
            }
            if (app->messages_kind == MESSAGES_KIND_INBOX) {
                app->messages_read_page = delta > 0
                    ? (uint8_t)((app->messages_read_page + 1u) % 3u)
                    : (app->messages_read_page == 0u ? 2u : (uint8_t)(app->messages_read_page - 1u));
                app->messages_read_scroll = 0u;
                app->dirty = true;
            }
            return true;
        }
        return true;
    }

    uint16_t count = current_sms_count(app);
    if (count == 0u) {
        if (key == KEY_C || key == KEY_NAVI) {
            open_messages_menu(app,
                               app->messages_kind == MESSAGES_KIND_OUTBOX ? 1u : 0u);
        }
        return true;
    }
    if (key == KEY_UP) {
        app->messages_selected = app->messages_selected == 0u ? (uint16_t)(count - 1u) : (uint16_t)(app->messages_selected - 1u);
        memset(&app->sms_selected_content, 0,
               sizeof(app->sms_selected_content));
        app->dirty = true;
        return true;
    }
    if (key == KEY_DOWN) {
        app->messages_selected = (uint16_t)((app->messages_selected + 1u) % count);
        memset(&app->sms_selected_content, 0,
               sizeof(app->sms_selected_content));
        app->dirty = true;
        return true;
    }
    if (key == KEY_C) {
        open_messages_menu(app,
                           app->messages_kind == MESSAGES_KIND_OUTBOX ? 1u : 0u);
        return true;
    }
    if (key == KEY_NAVI) {
        sms_begin_read_current(app, now);
        return true;
    }
    return true;
}


static bool sms_async_ui_active(const app_t *app) {
    return app->route == APP_ROUTE_MESSAGES_LIST ||
           (app->route == APP_ROUTE_DISPLAY_MESSAGE &&
            app->display_return_route == APP_ROUTE_MESSAGES_LIST &&
            app->display_record_id == 35u);
}

static bool sms_progress_ui_active(const app_t *app, uint8_t record_id,
                                   app_route_t return_route) {
    return app->route == APP_ROUTE_DISPLAY_MESSAGE &&
           app->display_record_id == record_id &&
           app->display_return_route == return_route;
}

static void sms_finish_submitted_text(app_t *app, bool sent) {
    if (sent &&
        strcmp(app->sms_composer_text, app->sms_send_submitted_text) == 0 &&
        strcmp(app->sms_recipient_prefill,
               app->sms_send_submitted_recipient) == 0) {
        messages_composer_clear_text(app);
        app->sms_recipient_prefill[0] = '\0';
    }
    app->sms_send_submitted_text[0] = '\0';
    app->sms_send_submitted_recipient[0] = '\0';
}

bool poll_sms(app_t *app, uint32_t now) {
    bool changed = false;
    static modem_status_t status;
    modem_service_get_status(&status);
    store_status_t picture_store = store_picture_commit_status();
    bool picture_failed = picture_store == STORE_STATUS_STORAGE_ERROR;
    if (picture_failed && !app->picture_storage_failed) app->picture_storage_warning = true;
    app->picture_storage_failed = picture_failed;
    if (picture_store == STORE_STATUS_OK && app->route != APP_ROUTE_POWER_OFF &&
        app->route != APP_ROUTE_POWERUP) {
        uint32_t picture_id = store_picture_pending_first();
        if (picture_id != 0u && picture_id != app->picture_last_notice_id) {
            app->picture_last_notice_id = picture_id;
            app->picture_notice_id = picture_id;
            play_message_alert(app, &status, now);
            changed = true;
        } else if (picture_id == 0u && app->picture_notice_id != 0u) {
            app->picture_notice_id = 0u;
            changed = true;
        }
    }
    changed |= messages_picture_poll_storage(app, now);
    if (status.picture_receive_errors != app->picture_receive_errors_seen) {
        app->picture_receive_warning |= status.picture_receive_errors > app->picture_receive_errors_seen;
        app->picture_receive_errors_seen = status.picture_receive_errors;
    }
    if ((app->picture_receive_warning || app->picture_storage_warning) && app->route == APP_ROUTE_STANDBY &&
        app->input_len == 0u && !app->keyguard_locked) {
        app->picture_receive_warning = false;
        open_display_sid(app, 0u, app->picture_storage_warning ? 0x3b3u : 0x210u,
                         app->picture_storage_warning ? "Not\nsaved" : "Not\ndone",
                         APP_ROUTE_STANDBY, now);
        app->picture_storage_warning = false;
        changed = true;
    }
    message_status_t local;
    message_service_get_status(&local);
    if (local.ready && app->sms_unread_count != local.unread) {
        app->sms_unread_count = local.unread;
        changed = true;
    }
    if (local.received != app->last_local_sms_received_count) {
        uint32_t delta = local.received > app->last_local_sms_received_count
            ? local.received - app->last_local_sms_received_count : 0u;
        app->last_local_sms_received_count = local.received;
        bool inbox_open = sms_async_ui_active(app) && app->messages_kind == MESSAGES_KIND_INBOX;
        if (delta != 0u && !inbox_open) {
            app->sms_received_pending = true;
            uint32_t count = app->sms_received_pending_count + delta;
            app->sms_received_pending_count = (uint8_t)(count > 99u ? 99u : count);
            play_message_alert(app, &status, now);
        }
        changed = true;
    }
    if ((local.storage_error && !app->sms_storage_error_seen) ||
        local.receive_errors != app->sms_receive_errors_seen) {
        app->sms_storage_warning = true;
    }
    app->sms_storage_error_seen = local.storage_error;
    app->sms_receive_errors_seen = local.receive_errors;

    /* Calls and alarms may replace progress UI. Keep the token until its exact
     * completion is drained, but never let a late result steal that UI back. */
    if (!sms_async_ui_active(app)) {
        app->messages_open_pending = false;
        app->sms_read_waiting = false;
    }
    if (!sms_progress_ui_active(app, 4u, APP_ROUTE_MESSAGES_LIST)) app->sms_delete_waiting = false;
    if (!sms_progress_ui_active(app, 4u, APP_ROUTE_SMS_COMPOSER)) app->sms_save_waiting = false;
    if (app->sms_send_waiting &&
        !sms_progress_ui_active(app, 46u, app->sms_send_return_route)) {
        app->sms_send_waiting = false;
        app->sms_send_return_route = APP_ROUTE_SMS_COMPOSER;
        changed = true;
    }
    if (local.full_events != app->last_sms_storage_full_events &&
        app->route == APP_ROUTE_STANDBY) {
        app->last_sms_storage_full_events = local.full_events;
        play_message_alert(app, &status, now);
        open_display_sid(app, 2u, 0x1c5u, "No space\nfor new\nmessages", APP_ROUTE_STANDBY, now);
        changed = true;
    } else if (app->sms_storage_warning && app->route == APP_ROUTE_STANDBY &&
               app->input_len == 0u && !app->keyguard_locked) {
        app->sms_storage_warning = false;
        open_display_sid(app, 0u, 0x3b3u, "Not\nsaved", APP_ROUTE_STANDBY, now);
        changed = true;
    }

    message_result_t result;
    if (message_service_pop_result(app->messages_open_request_id, &result, NULL)) {
        app->messages_open_request_id = 0u;
        if (app->messages_open_pending) {
            app->messages_open_pending = false;
            if (result.kind == MESSAGE_OP_LIST && result.outcome == MESSAGE_RESULT_OK) {
                if (app->messages_kind == MESSAGES_KIND_INBOX) app->sms_inbox_count = result.count;
                else app->sms_outbox_count = result.count;
                app->sms_cache_revision = result.revision;
                app->messages_mode = MESSAGES_MODE_LIST;
                app->messages_selected = 0u;
                for (uint16_t i = 0u; i < result.count; i++)
                    if (app->sms_inbox[i].id == app->sms_list_selected_id) app->messages_selected = i;
                if (current_sms_count(app) == 0u) sms_open_empty_notice(app, now);
                else { app->route = APP_ROUTE_MESSAGES_LIST; app->display_record_id = 0u; }
            } else {
                open_display(app, 0u, "Message", "function", "failed", APP_ROUTE_MESSAGES_MENU, now);
            }
        }
        changed = true;
    } else if (app->messages_open_pending &&
               time_diff_ms(now, app->messages_open_started_ms + SMS_MAILBOX_TIMEOUT_MS) >= 0) {
        app->messages_open_pending = false;
        open_display(app, 0u, "Message", "function", "failed", APP_ROUTE_MESSAGES_MENU, now);
        changed = true;
    }

    static message_content_t read_content;
    if (message_service_pop_result(app->sms_read_request_id, &result, &read_content)) {
        app->sms_read_request_id = 0u;
        if (app->sms_read_waiting) {
            app->sms_read_waiting = false;
            const app_sms_record_t *record = current_sms_record_const(app);
            if (result.kind == MESSAGE_OP_READ && result.outcome == MESSAGE_RESULT_OK &&
                record != NULL && result.object_id == app->sms_read_object_id &&
                result.object_id == record->id &&
                read_content.metadata.id == result.object_id) {
                memset(&app->sms_selected_content, 0, sizeof(app->sms_selected_content));
                app->sms_selected_content.valid = true;
                copy_text(app->sms_selected_content.text, sizeof(app->sms_selected_content.text),
                          read_content.metadata.binary ? ts_or(SID_DATA_MESSAGE, "Data message") : read_content.text);
                app->messages_mode = MESSAGES_MODE_READ;
                app->messages_read_page = 0u;
                app->messages_read_scroll = 0u;
                app->route = APP_ROUTE_MESSAGES_LIST;
                app->display_record_id = 0u;
                messages_mark_current_read(app);
            } else {
                memset(&app->sms_selected_content, 0, sizeof(app->sms_selected_content));
                open_display(app, 0u, "Message", "function", "failed", APP_ROUTE_MESSAGES_LIST, now);
            }
        }
        changed = true;
    } else if (app->sms_read_waiting &&
               time_diff_ms(now, app->sms_read_started_ms + SMS_READ_TIMEOUT_MS) >= 0) {
        app->sms_read_waiting = false;
        open_display(app, 0u, "Message", "function", "failed", APP_ROUTE_MESSAGES_LIST, now);
        changed = true;
    }

    if (message_service_pop_result(app->sms_delete_request_id, &result, NULL)) {
        app->sms_delete_request_id = 0u;
        if (app->sms_delete_waiting) {
            app->sms_delete_waiting = false;
            const app_sms_record_t *record = current_sms_record_const(app);
            if (result.kind == MESSAGE_OP_DELETE && result.outcome == MESSAGE_RESULT_OK &&
                record != NULL && record->id == result.object_id &&
                result.object_id == app->sms_delete_object_id) {
                sms_remove_current_local(app, now);
            } else {
                open_display(app, 0u, "Message", "function", "failed", APP_ROUTE_MESSAGES_LIST, now);
            }
        }
        changed = true;
    } else if (app->sms_delete_waiting &&
               time_diff_ms(now, app->sms_delete_started_ms + SMS_DELETE_TIMEOUT_MS) >= 0) {
        app->sms_delete_waiting = false;
        open_display(app, 0u, "Message", "function", "failed", APP_ROUTE_MESSAGES_LIST, now);
        changed = true;
    }

    if (message_service_pop_result(app->sms_save_request_id, &result, NULL)) {
        app->sms_save_request_id = 0u;
        if (app->sms_save_waiting) {
            app->sms_save_waiting = false;
            if (result.kind == MESSAGE_OP_SAVE && result.outcome == MESSAGE_RESULT_OK)
                open_display_sid(app, 3u, 0x365u, "Message\nsaved", APP_ROUTE_SMS_COMPOSER, now);
            else if (result.outcome == MESSAGE_RESULT_FULL)
                open_display_sid(app, 0u, 0x280u, "Memory full", APP_ROUTE_SMS_COMPOSER, now);
            else
                open_display_sid(app, 0u, 0x3b3u, "Not\nsaved", APP_ROUTE_SMS_COMPOSER, now);
        }
        changed = true;
    } else if (app->sms_save_waiting &&
               time_diff_ms(now, app->sms_save_started_ms + SMS_SAVE_TIMEOUT_MS) >= 0) {
        app->sms_save_waiting = false;
        open_display_sid(app, 0u, 0x3b3u, "Not\nsaved", APP_ROUTE_SMS_COMPOSER, now);
        changed = true;
    }
    if (local.ready && app->sms_cache_revision != local.revision &&
        app->route == APP_ROUTE_MESSAGES_LIST && app->messages_mode == MESSAGES_MODE_LIST &&
        (app->messages_kind == MESSAGES_KIND_INBOX || app->messages_kind == MESSAGES_KIND_OUTBOX) &&
        app->messages_open_request_id == 0u && app->sms_read_request_id == 0u &&
        app->sms_delete_request_id == 0u) {
        const app_sms_record_t *record = current_sms_record_const(app);
        uint32_t selected_id = record != NULL ? record->id : 0u;
        open_messages_mailbox(app, (messages_kind_t)app->messages_kind, now);
        app->sms_list_selected_id = selected_id;
        changed = true;
    }

    modem_sms_send_result_t send_result;
    bool send_terminal = app->sms_send_request_id != 0u &&
        modem_service_pop_sms_send_result(app->sms_send_request_id,
                                          &send_result);
    if (send_terminal && !app->sms_send_waiting) {
        bool sent = send_result.kind == MODEM_SMS_REQUEST_SEND_TEXT &&
                    send_result.outcome == MODEM_SMS_OUTCOME_OK;
        sms_finish_submitted_text(app, sent);
        app->sms_send_request_id = 0u;
        changed = true;
    } else if (send_terminal &&
               send_result.kind != MODEM_SMS_REQUEST_SEND_TEXT) {
        send_result.outcome = MODEM_SMS_OUTCOME_ERROR;
    }
    if (app->sms_send_waiting) {
        if (send_terminal) {
            app->sms_send_waiting = false;
            app->sms_send_request_id = 0u;
            app_route_t return_route = app->sms_send_return_route;
            if (send_result.outcome == MODEM_SMS_OUTCOME_OK) {
                sms_finish_submitted_text(app, true);
                open_display_sid(app, 47u, 0x2a8u, "Message\nsent", return_route, now);
            } else if (send_result.outcome == MODEM_SMS_OUTCOME_UNCERTAIN) {
                sms_finish_submitted_text(app, false);
                open_display_sid(app, 0u, 0x229u, "Result\nunknown",
                                 return_route, now);
            } else {
                sms_finish_submitted_text(app, false);
                open_display_sid(app, 0u, 0x359u, "Message\nsending\nfailed", return_route, now);
            }
            app->sms_send_return_route = APP_ROUTE_SMS_COMPOSER;
            changed = true;
        } else if (time_diff_ms(now, app->sms_send_started_ms + SMS_SEND_TIMEOUT_MS) >= 0) {
            app->sms_send_waiting = false;
            open_display_sid(app, 0u, 0x359u, "Message\nsending\nfailed", app->sms_send_return_route, now);
            app->sms_send_return_route = APP_ROUTE_SMS_COMPOSER;
            changed = true;
        }
    }
    if (messages_picture_poll_send(app, now)) {
        changed = true;
    }
    return changed;
}

void open_messages_menu(app_t *app, uint8_t selected) {
    app->route = APP_ROUTE_MESSAGES_MENU;
    app->messages_menu_selected = messages_root_normalized_raw(selected);
    app->dirty = true;
}

void open_messages_mailbox(app_t *app, messages_kind_t kind, uint32_t now) {
    if (kind != MESSAGES_KIND_PICTURES && app->messages_open_request_id != 0u) {
        open_display_sid(app, 0u, 0x20fu, "Not allowed", APP_ROUTE_MESSAGES_MENU, now);
        return;
    }
    app->messages_kind = (uint8_t)kind;
    app->messages_mode = MESSAGES_MODE_LIST;
    app->messages_selected = 0u;
    app->messages_read_page = 0u;
    app->messages_read_scroll = 0u;
    memset(&app->sms_selected_content, 0, sizeof(app->sms_selected_content));
    app->sms_read_waiting = false;
    app->messages_open_pending = false;
    app->messages_open_started_ms = now;
    if (kind == MESSAGES_KIND_PICTURES) {
        messages_picture_open_list(app);
        return;
    }
    if (kind == MESSAGES_KIND_INBOX) {
        app->sms_received_pending = false;
        app->sms_received_pending_count = 0u;
    }
    message_mailbox_t mailbox = kind == MESSAGES_KIND_OUTBOX ? MESSAGE_OUTBOX : MESSAGE_INBOX;
    uint32_t request_id = 0u;
    if (!message_service_request_list(mailbox, app->sms_inbox, APP_SMS_RECORD_LIMIT, &request_id)) {
        open_display_sid(app, 0u, 0x20fu, "Not allowed", APP_ROUTE_MESSAGES_MENU, now);
        return;
    }
    app->messages_open_pending = true;
    app->messages_open_request_id = request_id;
    app->sms_list_selected_id = 0u;
    open_display_sid(app, 35u, 0x33du, "Opening", APP_ROUTE_MESSAGES_LIST, now);
}


void open_sms_recipient_editor(app_t *app, const char *value, uint32_t now) {
    open_editor(app, "Enter number:", value != 0 ? value : app->sms_recipient_prefill, 21u, EDITOR_KIND_NUMBER, EDITOR_CONTEXT_SMS_RECIPIENT, true, now);
    update_sms_recipient_softkey(app);
}

void update_sms_recipient_softkey(app_t *app) {
    app->dirty = true;
}

void start_sms_send(app_t *app, const char *recipient, const char *text, app_route_t return_route, uint32_t now) {
    app->sms_send_return_route = return_route;
    modem_status_t status;
    modem_service_get_status(&status);
    if (!status.sim_ready) {
        open_display_sid(app, 2u, 0x297u, "SIM card\nnot ready", return_route, now);
        return;
    }
    uint32_t request_id = 0u;
    if (!modem_service_request_send_sms(
            recipient, text != 0 ? text : "", &request_id)) {
        open_display_sid(app, 0u, 0x35au, "Still\nsending\nprevious", return_route, now);
        return;
    }
    app->sms_send_waiting = true;
    app->sms_send_request_id = request_id;
    copy_text(app->sms_send_submitted_text,
              sizeof(app->sms_send_submitted_text),
              text != NULL ? text : "");
    copy_text(app->sms_send_submitted_recipient,
              sizeof(app->sms_send_submitted_recipient), recipient);
    app->sms_send_started_ms = now;
    open_display_sid(app, 46u, 0x35fu, "Sending\nmessage", return_route, now);
}

/* Mirror the already committed local read-state transition in this view. */
static void messages_mark_current_read(app_t *app) {
    if (app->messages_kind != MESSAGES_KIND_INBOX) {
        return;
    }
    uint16_t count = app->sms_inbox_count;
    if (count == 0u) {
        return;
    }
    uint16_t selected = app->messages_selected >= count ? (uint16_t)(count - 1u) : app->messages_selected;
    app_sms_record_t *record = &app->sms_inbox[selected];
    record->read = true;
}

static const app_sms_record_t *current_sms_record_const(const app_t *app) {
    uint16_t count = current_sms_count(app);
    if (count == 0u || app->messages_kind == MESSAGES_KIND_PICTURES) {
        return 0;
    }
    uint16_t selected = app->messages_selected >= count ? (uint16_t)(count - 1u) : app->messages_selected;
    return app->messages_kind == MESSAGES_KIND_OUTBOX ? &app->sms_outbox[selected] : &app->sms_inbox[selected];
}

static uint16_t current_sms_count(const app_t *app) {
    return app->messages_kind == MESSAGES_KIND_OUTBOX ? app->sms_outbox_count : app->sms_inbox_count;
}

static void sms_begin_read_current(app_t *app, uint32_t now) {
    const app_sms_record_t *record = current_sms_record_const(app);
    if (record == NULL || record->id == 0u) return;
    uint32_t request_id = 0u;
    if (app->sms_read_request_id != 0u || !message_service_request_read(
            app->messages_kind == MESSAGES_KIND_OUTBOX ? MESSAGE_OUTBOX : MESSAGE_INBOX,
            record->id, &request_id)) {
        open_display_sid(app, 0u, 0x20fu, "Not allowed", APP_ROUTE_MESSAGES_LIST, now);
        return;
    }
    memset(&app->sms_selected_content, 0, sizeof(app->sms_selected_content));
    app->sms_read_waiting = true;
    app->sms_read_request_id = request_id;
    app->sms_read_started_ms = now;
    app->sms_read_object_id = record->id;
    open_display_sid(app, 35u, 0x33du, "Opening", APP_ROUTE_MESSAGES_LIST, now);
}

static bool sms_outbox_is_recipientless(const app_sms_record_t *record) {
    return record != 0 && record->address[0] == '\0';
}

static uint8_t sms_outbox_option_count(const app_sms_record_t *record) {
    uint8_t count = (uint8_t)ARRAY_COUNT(MESSAGE_OUTBOX_OPTIONS);
    /* Verified on a v6.00 handset: a saved recipient-less draft omits the
     * trailing Details item. Addressed Outbox records retain the full menu. */
    return sms_outbox_is_recipientless(record) ? (uint8_t)(count - 1u) : count;
}

static const char *sms_record_label(const app_sms_record_t *record,
                                    bool outbox,
                                    char *scratch,
                                    size_t cap) {
    if (record == 0) {
        copy_text(scratch, cap, "");
        return scratch;
    }
    if (outbox && sms_outbox_is_recipientless(record)) {
        copy_text(scratch, cap, ts_or(0x337u, "Message"));
        return scratch;
    }
    if (record->address[0] != '\0') {
        char name[PHONEBOOK_NAME_MAX + 1u];
        resolve_contact_name(record->address, name, sizeof(name));
        if (name[0] != '\0') {
            copy_text(scratch, cap, name);
            return scratch;
        }
        copy_text(scratch, cap, record->address);
        return scratch;
    }
    copy_text(scratch, cap, ts_or(0x337u, "Message"));
    return scratch;
}

static void sms_sender_page_text(const app_sms_record_t *record, char *dst, size_t cap) {
    /* v6.00 "Sender:" page (SID 0x035c "Sender:\n%S"): %S is the resolved
     * phonebook name when one matches, else the raw number -- and when a
     * name IS shown the original appends the raw number on the next line
     * (ROM 0x2295ce-0x2295f8). Same resolver as the inbox list rows. */
    if (record->address[0] == '\0') {
        snprintf(dst, cap, "Sender:\n%s", "Message");
        return;
    }
    char name[PHONEBOOK_NAME_MAX + 1u];
    resolve_contact_name(record->address, name, sizeof(name));
    if (name[0] != '\0') {
        snprintf(dst, cap, "Sender:\n%s\n%s", name, record->address);
    } else {
        snprintf(dst, cap, "Sender:\n%s", record->address);
    }
}

static void sms_recipient_page_text(const app_sms_record_t *record, char *dst, size_t cap) {
    /* v6.00 "Recipient:" page (SID 0x034f "Recipient:\n%S"): the outbox detail
     * shares ONE composer with the inbox Sender: page (ROM 0x229388); the title
     * SID is picked from the record. Unlike Sender:, the Recipient: branch emits
     * a SINGLE value -- the resolved phonebook name when one matches, else the
     * raw number -- and never appends the number underneath (ROM 0x229584-0x229594
     * has no line-break/second-emit; verified 2026-07-07). An empty recipient
     * number suppresses the value (ROM gates on the number field at 0x229574). */
    if (record->address[0] == '\0') {
        copy_text(dst, cap, "Recipient:");
        return;
    }
    char name[PHONEBOOK_NAME_MAX + 1u];
    resolve_contact_name(record->address, name, sizeof(name));
    snprintf(dst, cap, "Recipient:\n%s", name[0] != '\0' ? name : record->address);
}

static void sms_timestamp_text(const char *timestamp, char *dst, size_t cap) {
    static const char *const MONTHS[] = {"Jan", "Feb", "Mar", "Apr", "May", "June", "July", "Aug", "Sept", "Oct", "Nov", "Dec"};
    unsigned year = 0u;
    unsigned month = 0u;
    unsigned day = 0u;
    unsigned hour = 0u;
    unsigned minute = 0u;
    unsigned second = 0u;
    if (timestamp != 0 &&
        sscanf(timestamp, "%2u/%2u/%2u,%2u:%2u:%2u", &year, &month, &day, &hour, &minute, &second) == 6 &&
        month >= 1u && month <= 12u) {
        snprintf(dst,
                 cap,
                 "%02u-%s-20%02u\n%02u:%02u:%02u",
                 day,
                 MONTHS[month - 1u],
                 year,
                 hour,
                 minute,
                 second);
        return;
    }
    if (timestamp != 0 && timestamp[0] != '\0') {
        copy_text(dst, cap, timestamp);
        return;
    }
    copy_text(dst, cap, "12-May-2026\n00:00:00");
}

static void sms_detail_page_text(const app_t *app, char *dst, size_t cap) {
    const app_sms_record_t *record = current_sms_record_const(app);
    if (record == 0) {
        copy_text(dst, cap, "");
        return;
    }
    if (app->messages_kind == MESSAGES_KIND_INBOX) {
        if (app->messages_detail_page == 0u) {
            sms_sender_page_text(record, dst, cap);
        } else if (app->messages_detail_page == 1u) {
            char centre[STORE_TEXT_MAX + 1u];
            store_setting_get_text(STORE_SETTING_SMS_MESSAGE_CENTRE, centre, sizeof(centre));
            snprintf(dst, cap, "Message centre: %s", centre[0] ? centre : "+1234567890");
        } else {
            sms_timestamp_text(record->timestamp, dst, cap);
        }
        return;
    }
    if (app->messages_detail_page == 0u) {
        sms_recipient_page_text(record, dst, cap);
    } else if (app->messages_detail_page == 1u) {
        copy_text(dst, cap, record->address);
    } else {
        sms_timestamp_text(record->timestamp, dst, cap);
    }
}

static void sms_use_number_candidate(const app_sms_record_t *record,
                                     const char *body, char *dst,
                                     size_t cap) {
    dst[0] = '\0';
    if (record == 0) {
        return;
    }
    if (record->address[0] != '\0') {
        copy_text(dst, cap, record->address);
        return;
    }
    const char *text = body != NULL ? body : "";
    for (size_t i = 0; text[i] != '\0'; i++) {
        if ((text[i] >= '0' && text[i] <= '9') || text[i] == '+') {
            size_t start = i;
            size_t len = 0u;
            while ((text[i] >= '0' && text[i] <= '9') || text[i] == '+') {
                if (len + 1u < cap) {
                    dst[len++] = text[i];
                }
                i++;
            }
            dst[len] = '\0';
            if (i - start >= 3u) {
                return;
            }
            dst[0] = '\0';
        }
    }
}

static void sms_delete_current(app_t *app, uint32_t now) {
    const app_sms_record_t *record = current_sms_record_const(app);
    if (record == NULL) return;
    uint32_t request_id = 0u;
    if (app->sms_delete_request_id != 0u || !message_service_request_delete(
            app->messages_kind == MESSAGES_KIND_OUTBOX ? MESSAGE_OUTBOX : MESSAGE_INBOX,
            record->id, &request_id)) {
        open_display_sid(app, 0u, 0x20fu, "Not allowed", APP_ROUTE_MESSAGES_LIST, now);
        return;
    }
    app->sms_delete_waiting = true;
    app->sms_delete_request_id = request_id;
    app->sms_delete_object_id = record->id;
    app->sms_delete_started_ms = now;
    open_display_sid(app, 4u, 0x328u, "Erasing\nmessage", APP_ROUTE_MESSAGES_LIST, now);
}

static void sms_remove_current_local(app_t *app, uint32_t now) {
    uint16_t count = current_sms_count(app);
    if (count == 0u) {
        sms_open_empty_notice(app, now);
        return;
    }
    app_sms_record_t *records = app->messages_kind == MESSAGES_KIND_OUTBOX ? app->sms_outbox : app->sms_inbox;
    uint16_t *count_ptr = app->messages_kind == MESSAGES_KIND_OUTBOX ? &app->sms_outbox_count : &app->sms_inbox_count;
    uint16_t selected = app->messages_selected >= count ? (uint16_t)(count - 1u) : app->messages_selected;
    /* The complete logical message has left the local store. */
    for (uint16_t i = selected; i + 1u < count; i++) {
        records[i] = records[i + 1u];
    }
    (*count_ptr)--;
    memset(&app->sms_selected_content, 0,
           sizeof(app->sms_selected_content));
    if (app->messages_selected >= *count_ptr && *count_ptr > 0u) {
        app->messages_selected = (uint16_t)(*count_ptr - 1u);
    }
    app->messages_mode = MESSAGES_MODE_LIST;
    if (*count_ptr == 0u) {
        sms_open_empty_notice(app, now);
    } else {
        open_display_sid(app, 44u, 0x327u, "Erased", APP_ROUTE_MESSAGES_LIST, now);
    }
}

static void sms_open_empty_notice(app_t *app, uint32_t now) {
    /* S6.4: original uses display record 0x03 (SID 0x0331/0x0332) and
     * returns to the Messages menu, not the main menu. */
    open_messages_menu(app, app->messages_kind == MESSAGES_KIND_OUTBOX ? 1u : 0u);
    bool outbox = app->messages_kind == MESSAGES_KIND_OUTBOX;
    open_display_sid(app,
                     3u,
                     outbox ? 0x332u : 0x331u,
                     outbox ? "No more messages\nin outbox" : "No more messages\nin inbox",
                     APP_ROUTE_MESSAGES_MENU,
                     now);
}

void messages_save_composed_message(app_t *app, uint32_t now) {
    if (app->sms_composer_text[0] == '\0') {
        app->route = APP_ROUTE_SMS_COMPOSER;
        app->dirty = true;
        return;
    }

    uint32_t request_id = 0u;
    if (!message_service_request_save(
            app->sms_recipient_prefill, app->sms_composer_text,
            &request_id)) {
        open_display_sid(app, 0u, 0x20fu, "Not allowed", APP_ROUTE_SMS_COMPOSER, now);
        return;
    }
    app->sms_save_waiting = true;
    app->sms_save_request_id = request_id;
    app->sms_save_started_ms = now;
    open_display(app, 4u, "Saving", 0, 0, APP_ROUTE_SMS_COMPOSER, now);
}

static void sms_open_current_options(app_t *app) {
    app->messages_mode = MESSAGES_MODE_OPTIONS;
    app->messages_option_selected = 0u;
    app->dirty = true;
}

static void sms_select_read_option(app_t *app, uint32_t now) {
    const app_sms_record_t *record = current_sms_record_const(app);
    const app_sms_content_t *content = &app->sms_selected_content;
    if (record == 0 || !content->valid) {
        app->messages_mode = MESSAGES_MODE_LIST;
        app->dirty = true;
        return;
    }
    uint8_t option = app->messages_option_selected;
    uint8_t option_count = app->messages_kind == MESSAGES_KIND_OUTBOX
        ? sms_outbox_option_count(record)
        : (uint8_t)ARRAY_COUNT(MESSAGE_INBOX_OPTIONS);
    if (option >= option_count) {
        option = 0u;
    }
    const char *label = app->messages_kind == MESSAGES_KIND_OUTBOX
        ? MESSAGE_OUTBOX_OPTIONS[option]
        : MESSAGE_INBOX_OPTIONS[option];
    const char *body = content->text;
    if ((strcmp(label, "Forward") == 0 || strcmp(label, "Edit") == 0 ||
         strcmp(label, "Send") == 0) &&
        (record->binary || strlen(body) > MODEM_SMS_TEXT_MAX)) {
        open_display_sid(app, 0u, 0x20fu, "Not allowed", APP_ROUTE_MESSAGES_LIST, now);
        return;
    }
    if (strcmp(label, "Reply") == 0) {
        open_sms_composer(app, "", record->address, now);
    } else if (strcmp(label, "Forward") == 0) {
        open_sms_composer(app, body, "", now);
    } else if (strcmp(label, "Edit") == 0) {
        open_sms_composer(app, body, record->address, now);
    } else if (strcmp(label, "Send") == 0) {
        open_sms_composer(app, body, record->address, now);
        if (record->address[0] != '\0') {
            /* A decoded inbound/outbox body may occupy more UTF-8 bytes than
             * the single-message composer can send. Submit the glyph-safe,
             * visible composer copy rather than a separately truncated source. */
            start_sms_send(app, record->address, app->sms_composer_text,
                           APP_ROUTE_SMS_COMPOSER, now);
        } else {
            open_sms_recipient_editor(app, "", now);
        }
    } else if (strcmp(label, "Use number") == 0) {
        char candidate[MODEM_PHONE_MAX + 1u];
        sms_use_number_candidate(record, body, candidate, sizeof(candidate));
        if (candidate[0] == '\0') {
            open_display_sid(app, 2u, 0x33au, "No number\nfound\non this screen", APP_ROUTE_MESSAGES_LIST, now);
            return;
        }
        copy_text(app->input_text, sizeof(app->input_text), candidate);
        app->input_len = (uint8_t)strlen(app->input_text);
        app->input_action = APP_STANDBY_ACTION_CALL;
        app->star_cycle_until_ms = 0u;
        app->route = APP_ROUTE_STANDBY;
        app->dirty = true;
    } else if (strcmp(label, "Erase") == 0) {
        sms_delete_current(app, now);
    } else if (strcmp(label, "Details") == 0) {
        app->messages_mode = MESSAGES_MODE_DETAIL;
        app->messages_detail_page = 0u;
        app->dirty = true;
    }
}

static void sms_open_settings_menu(app_t *app) {
    app->messages_settings_kind = app->messages_settings_top_selected == 0u ? 0u : 1u;
    app->messages_settings_selected = 0u;
    app->messages_mode = MESSAGES_MODE_SETTINGS_MENU;
    app->dirty = true;
}

static void sms_select_settings_item(app_t *app, uint32_t now) {
    if (app->messages_settings_kind == 0u && app->messages_settings_selected == 0u) {
        char centre[STORE_TEXT_MAX + 1u];
        store_setting_get_text(STORE_SETTING_SMS_MESSAGE_CENTRE, centre, sizeof(centre));
        open_editor(app, "Enter number:", centre, 30u, EDITOR_KIND_NUMBER, EDITOR_CONTEXT_SMS_MESSAGE_CENTRE, true, now);
        return;
    }

    uint8_t key = 0u;
    if (app->messages_settings_kind == 0u) {
        key = app->messages_settings_selected == 1u ? 1u : 2u;
    } else {
        key = app->messages_settings_selected == 0u ? 3u : 4u;
    }
    app->messages_settings_value_key = key;
    uint8_t value = 0u;
    store_setting_get_u8(messages_settings_store_key(key), &value);
    if (key == 3u || key == 4u) {
        app->messages_settings_value_selected = value ? 0u : 1u;
    } else {
        uint8_t count = messages_settings_value_count(key);
        app->messages_settings_value_selected = value < count ? value : 0u;
    }
    app->messages_mode = MESSAGES_MODE_SETTINGS_VALUE;
    app->dirty = true;
}

static void sms_save_settings_value(app_t *app, uint32_t now) {
    uint8_t key = app->messages_settings_value_key;
    uint8_t value = app->messages_settings_value_selected;
    if (key == 3u || key == 4u) {
        value = app->messages_settings_value_selected == 0u ? 1u : 0u;
    }
    store_setting_set_u8(messages_settings_store_key(key), value);
    app->messages_mode = MESSAGES_MODE_SETTINGS_MENU;
    open_display_sid(app, 3u, 0x3b4u, "Saved", APP_ROUTE_MESSAGES_LIST, now);
}

static void play_message_alert(app_t *app, const modem_status_t *status,
                               uint32_t now) {
    /* v6.00 FUN_0022769c branches on runtime call state before consulting the
     * profile alert: state 1 (incoming ringing) emits nothing; every other
     * non-idle state plays fixed system tone 14 and posts light event 0x01f5.
     * Tone 14 is a 425 Hz / ~303 ms earpiece beep with no vibra marker. */
    if (status != NULL && status->call_state == MODEM_CALL_RINGING) {
        return;
    }

    app->backlight_activity_pending = true;
    app->backlight_activity_ms = now;
    if (status != NULL && status->call_state != MODEM_CALL_IDLE) {
        core1_post_command(CORE1_CMD_AUDIO_SYSTEM_TONE_QUIET,
                           audio_arg(SMS_IN_CALL_TONE_INDEX,
                                     SMS_IN_CALL_TONE_LEVEL));
        return;
    }

    uint8_t active = profile_active_index();
    uint8_t value = profile_get_tone_setting(active, PROFILE_SETTING_MESSAGE_ALERT);
    if (value == 0u || value > 4u) {
        return;
    }
    uint8_t volume = profile_get_tone_setting(active, PROFILE_SETTING_RINGING_VOLUME);
    uint8_t level = audio_level_from_ringing_volume(volume);
    if (level == AUDIO_LEVEL_SILENT) {
        return;
    }
    core1_post_command(CORE1_CMD_AUDIO_SYSTEM_TONE, audio_arg((uint8_t)(28u + value), level));
}

static const char *messages_settings_preview(const app_t *app, uint8_t index, char *scratch, size_t cap) {
    scratch[0] = '\0';
    if (app->messages_settings_kind == 0u) {
        if (index == 0u) {
            store_setting_get_text(STORE_SETTING_SMS_MESSAGE_CENTRE, scratch, (uint8_t)cap);
            return scratch;
        }
        uint8_t key = index == 1u ? 1u : 2u;
        uint8_t value = 0u;
        store_setting_get_u8(messages_settings_store_key(key), &value);
        copy_text(scratch, cap, messages_settings_value_label(key, value));
        return scratch;
    }
    uint8_t key = index == 0u ? 3u : 4u;
    uint8_t value = 0u;
    store_setting_get_u8(messages_settings_store_key(key), &value);
    copy_text(scratch, cap, value ? "Yes" : "No");
    return scratch;
}

static const char *messages_settings_value_label(uint8_t key, uint8_t value) {
    if (key == 1u) {
        if (value >= ARRAY_COUNT(MESSAGE_SENT_AS_LABELS)) {
            value = 0u;
        }
        return ts_or(MESSAGE_SENT_AS_SID[value], MESSAGE_SENT_AS_LABELS[value]);
    }
    if (key == 2u) {
        if (value >= ARRAY_COUNT(MESSAGE_VALIDITY_LABELS)) {
            value = (uint8_t)(ARRAY_COUNT(MESSAGE_VALIDITY_LABELS) - 1u);
        }
        return ts_or(MESSAGE_VALIDITY_SID[value], MESSAGE_VALIDITY_LABELS[value]);
    }
    return value ? "Yes" : "No"; /* Yes/No ambiguous -> English */
}

static uint8_t messages_settings_value_count(uint8_t key) {
    if (key == 1u) {
        return (uint8_t)ARRAY_COUNT(MESSAGE_SENT_AS_LABELS);
    }
    if (key == 2u) {
        return (uint8_t)ARRAY_COUNT(MESSAGE_VALIDITY_LABELS);
    }
    return (uint8_t)ARRAY_COUNT(MESSAGE_YES_NO_LABELS);
}

static store_setting_key_t messages_settings_store_key(uint8_t key) {
    if (key == 1u) {
        return STORE_SETTING_SMS_SENT_AS;
    }
    if (key == 2u) {
        return STORE_SETTING_SMS_VALIDITY;
    }
    if (key == 3u) {
        return STORE_SETTING_SMS_DELIVERY_REPORTS;
    }
    return STORE_SETTING_SMS_REPLY_SAME_CENTRE;
}


static bool messages_root_item_visible(uint8_t raw) {
    if (raw >= ARRAY_COUNT(MESSAGES_ROOT_LABELS)) {
        return false;
    }
    if (raw == 5u) {
        return feature_gate_visible(FEATURE_GATE_MESSAGES_INFO_SERVICE);
    }
    if (raw == 6u) {
        return feature_gate_visible(FEATURE_GATE_MESSAGES_VOICE_MAILBOX_NUMBER);
    }
    return true;
}

static uint8_t messages_root_visible_count(void) {
    uint8_t count = ui_menu_visible_count(
        messages_root_item_visible, (uint8_t)ARRAY_COUNT(MESSAGES_ROOT_LABELS));
    return count == 0u ? 1u : count;
}

static uint8_t messages_root_visible_index(uint8_t raw) {
    return ui_menu_visible_index(
        messages_root_item_visible, (uint8_t)ARRAY_COUNT(MESSAGES_ROOT_LABELS),
        messages_root_normalized_raw(raw));
}

static uint8_t messages_root_raw_at_visible(uint8_t visible) {
    return ui_menu_raw_at_visible(
        messages_root_item_visible, (uint8_t)ARRAY_COUNT(MESSAGES_ROOT_LABELS),
        visible);
}

static uint8_t messages_root_normalized_raw(uint8_t raw) {
    return ui_menu_normalized_raw(
        messages_root_item_visible, (uint8_t)ARRAY_COUNT(MESSAGES_ROOT_LABELS),
        raw);
}

static void messages_menu_step(app_t *app, int8_t delta) {
    uint8_t count = messages_root_visible_count();
    uint8_t visible = messages_root_visible_index(app->messages_menu_selected);
    visible = (uint8_t)((visible + count + delta) % count);
    app->messages_menu_selected = messages_root_raw_at_visible(visible);
    app->dirty = true;
}

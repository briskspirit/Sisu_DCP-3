#include "messages_internal.h"

#include <stdio.h>
#include <string.h>

#include "app_internal.h"
#include "apps/dialogs_app.h"
#include "services/input_keys.h"
#include "services/modem_service.h"
#include "services/sms_picture_codec.h"
#include "services/strings.h"
#include "services/timebase.h"
#include "storage/store_service.h"
#include "ui/assets.h"
#include "ui/ui.h"

#define PICTURE_SMS_SEND_TIMEOUT_MS 180000u
#define PICTURE_SMS_PAYLOAD_MAX \
    (1u + 3u + STORE_PICTURE_TEXT_MAX + 3u + 4u + STORE_PICTURE_BITMAP_BYTES)
#define PICTURE_SMS_SOURCE_PORT 0x0000u

static const char *const PICTURE_OPTIONS[] = {
    "Edit text", "Erase", "Use number", "Forward", "Details",
};
static const uint16_t PICTURE_OPTIONS_SID[] = {
    0x16fu, 0u, 0u, 0u, 0u,
};
static const char *const PICTURE_EDITOR_OPTIONS[] = {
    "Send", "Save", "Clear text", "Preview", "Exit",
};
static const uint16_t PICTURE_EDITOR_OPTIONS_SID[] = {
    0u, 0x17eu, 0x16du, 0x17bu, 0x173u,
};

static bool picture_by_ordinal(uint8_t ordinal,
                               uint8_t *out_slot,
                               store_picture_message_t *out_message);
static uint8_t picture_selected_slot(const app_t *app,
                                     store_picture_message_t *out_message);
static uint8_t picture_active_slot(const app_t *app,
                                   store_picture_message_t *out_message);
static uint8_t picture_text_max_len(const store_picture_message_t *message);
static void picture_apply_draft(const app_t *app,
                                uint8_t slot,
                                store_picture_message_t *message);
static void picture_draw_preview(framebuffer_t *fb,
                                 const store_picture_message_t *message);
static void picture_begin_edit(app_t *app, uint32_t now);
static void picture_save_draft(app_t *app, uint32_t now);
static void picture_clear_draft(app_t *app, uint32_t now);
static void picture_start_send(app_t *app,
                               const char *recipient,
                               uint32_t now);
static void picture_select_saved_option(app_t *app, uint32_t now);
static void picture_store_in_slot(app_t *app,
                                  uint8_t slot,
                                  const store_picture_message_t *picture,
                                  bool replacement,
                                  uint32_t now);
static uint8_t picture_count(void) {
    return store_picture_message_count();
}

void messages_picture_open_menu(app_t *app, uint32_t now) {
    if (picture_count() == 0u) {
        open_display_sid(app,
                         2u,
                         0x178u,
                         "No picture\nmessages\navailable",
                         APP_ROUTE_MAIN_MENU,
                         now);
        return;
    }
    app->messages_kind = MESSAGES_KIND_PICTURES;
    app->messages_mode = MESSAGES_MODE_LIST;
    app->messages_selected = 0u;
    app->route = APP_ROUTE_MESSAGES_LIST;
    app->dirty = true;
}

void messages_picture_open_list(app_t *app) {
    app->messages_kind = MESSAGES_KIND_PICTURES;
    app->messages_mode = MESSAGES_MODE_LIST;
    app->messages_selected = 0u;
    app->messages_read_page = 0u;
    app->messages_read_scroll = 0u;
    memset(&app->sms_selected_content, 0, sizeof(app->sms_selected_content));
    app->sms_read_waiting = false;
    if (!app->sms_status_sync_silent) {
        app->messages_open_pending = false;
    }
    app->route = APP_ROUTE_MESSAGES_LIST;
    app->dirty = true;
}

void messages_picture_render(const app_t *app, framebuffer_t *fb) {
    if (app->messages_mode == MESSAGES_MODE_OPTIONS) {
        const char *labels[ARRAY_COUNT(PICTURE_OPTIONS)];
        for (uint8_t i = 0u; i < ARRAY_COUNT(PICTURE_OPTIONS); i++) {
            labels[i] = ts_or(PICTURE_OPTIONS_SID[i], PICTURE_OPTIONS[i]);
        }
        draw_flat_list(fb,
                       labels,
                       (uint8_t)ARRAY_COUNT(labels),
                       app->messages_option_selected,
                       "",
                       "Select");
        return;
    }

    if (app->messages_mode == MESSAGES_MODE_READ) {
        store_picture_message_t picture;
        uint8_t slot = picture_selected_slot(app, &picture);
        if (slot >= STORE_PICTURE_SLOT_COUNT) {
            return;
        }
        picture_apply_draft(app, slot, &picture);
        picture_draw_preview(fb, &picture);
        draw_softkey(fb, "Options");
        return;
    }

    if (app->messages_mode == MESSAGES_MODE_DETAIL) {
        store_picture_message_t picture;
        uint8_t slot = picture_selected_slot(app, &picture);
        if (slot >= STORE_PICTURE_SLOT_COUNT) {
            return;
        }
        char text[64];
        snprintf(text,
                 sizeof(text),
                 "Picture:\n%ux%u\nSlot %u",
                 (unsigned)(picture.width == 0u ? STORE_PICTURE_WIDTH : picture.width),
                 (unsigned)(picture.height == 0u ? STORE_PICTURE_HEIGHT : picture.height),
                 (unsigned)(slot + 1u));
        draw_text_block(fb, asset_font(FONT_FS2), text,
                        0, 0, FB_WIDTH, 9, 4u);
        draw_softkey(fb, "Back");
        return;
    }

    uint8_t count = picture_count();
    if (count == 0u) {
        draw_text_block(fb,
                        asset_font(FONT_FS0),
                        ts_or(0x178u, "No picture\nmessages\navailable"),
                        0,
                        5,
                        FB_WIDTH,
                        11,
                        3u);
        return;
    }
    uint8_t selected = app->messages_selected >= count ? 0u : app->messages_selected;
    char labels[STORE_PICTURE_SLOT_COUNT][24];
    const char *label_ptrs[STORE_PICTURE_SLOT_COUNT];
    for (uint8_t i = 0u; i < count; i++) {
        store_picture_message_t picture;
        uint8_t slot = 0u;
        if (picture_by_ordinal(i, &slot, &picture)) {
            if (picture.text[0] != '\0') {
                copy_text(labels[i], sizeof(labels[i]), picture.text);
            } else {
                snprintf(labels[i], sizeof(labels[i]), "Picture %u", (unsigned)(slot + 1u));
            }
        } else {
            copy_text(labels[i], sizeof(labels[i]), "Picture");
        }
        label_ptrs[i] = labels[i];
    }
    char crumb[8];
    snprintf(crumb, sizeof(crumb), "2-4-%u", (unsigned)(selected + 1u));
    draw_flat_list_at_y(fb,
                        label_ptrs,
                        count,
                        selected,
                        crumb,
                        app->messages_picture_save_pending ? "Replace" : "View",
                        7);
}

void messages_picture_draw_received_preview(framebuffer_t *fb,
                                            const store_picture_message_t *picture) {
    picture_draw_preview(fb, picture);
}

bool messages_picture_handle_key(app_t *app, uint16_t key, uint32_t now) {
    if (app->messages_mode == MESSAGES_MODE_OPTIONS) {
        uint8_t count = (uint8_t)ARRAY_COUNT(PICTURE_OPTIONS);
        if (key == KEY_UP) {
            app->messages_option_selected = app->messages_option_selected == 0u
                ? (uint8_t)(count - 1u)
                : (uint8_t)(app->messages_option_selected - 1u);
            app->dirty = true;
        } else if (key == KEY_DOWN) {
            app->messages_option_selected = (uint8_t)((app->messages_option_selected + 1u) % count);
            app->dirty = true;
        } else if (key == KEY_C) {
            app->messages_mode = MESSAGES_MODE_READ;
            app->dirty = true;
        } else if (key == KEY_NAVI) {
            picture_select_saved_option(app, now);
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
            app->messages_mode = MESSAGES_MODE_OPTIONS;
            app->messages_option_selected = 0u;
            app->dirty = true;
        } else if (key == KEY_C) {
            app->messages_mode = MESSAGES_MODE_LIST;
            app->dirty = true;
        }
        return true;
    }

    uint8_t count = picture_count();
    if (count == 0u) {
        if (key == KEY_C || key == KEY_NAVI) {
            open_messages_menu(app, 3u);
        }
        return true;
    }
    if (key == KEY_UP) {
        app->messages_selected = app->messages_selected == 0u
            ? (uint8_t)(count - 1u)
            : (uint8_t)(app->messages_selected - 1u);
        memset(&app->sms_selected_content, 0, sizeof(app->sms_selected_content));
        app->dirty = true;
        return true;
    }
    if (key == KEY_DOWN) {
        app->messages_selected = (uint8_t)((app->messages_selected + 1u) % count);
        memset(&app->sms_selected_content, 0, sizeof(app->sms_selected_content));
        app->dirty = true;
        return true;
    }
    if (key == KEY_C) {
        if (app->messages_picture_save_pending) {
            app->messages_picture_save_pending = false;
            app->messages_kind = app->messages_picture_save_return_kind;
            app->messages_selected = app->messages_picture_save_return_selected;
            app->messages_mode = MESSAGES_MODE_READ;
            app->route = APP_ROUTE_MESSAGES_LIST;
            app->dirty = true;
            return true;
        }
        open_messages_menu(app, 3u);
        return true;
    }
    if (key == KEY_NAVI) {
        if (app->messages_picture_save_pending) {
            store_picture_message_t ignored;
            uint8_t slot = picture_selected_slot(app, &ignored);
            if (slot < STORE_PICTURE_SLOT_COUNT) {
                picture_store_in_slot(app,
                                      slot,
                                      &app->messages_picture_save_candidate,
                                      true,
                                      now);
            }
            return true;
        }
        app->messages_mode = MESSAGES_MODE_READ;
        app->messages_read_page = 0u;
        app->messages_read_scroll = 0u;
        app->dirty = true;
    }
    return true;
}

bool messages_picture_poll_send(app_t *app, uint32_t now) {
    if (app->messages_picture_send_request_id == 0u) {
        return false;
    }
    bool ui_owned = app->messages_picture_send_waiting;
    bool detached = false;
    if (ui_owned &&
        (app->route != APP_ROUTE_DISPLAY_MESSAGE ||
         app->display_record_id != 46u ||
         app->display_return_route != APP_ROUTE_MESSAGES_LIST)) {
        app->messages_picture_send_waiting = false;
        ui_owned = false;
        detached = true;
    }
    modem_sms_send_result_t result;
    bool matching = modem_service_pop_sms_send_result(
        app->messages_picture_send_request_id, &result);
    if (matching) {
        app->messages_picture_send_waiting = false;
        app->messages_picture_send_request_id = 0u;
        if (result.kind != MODEM_SMS_REQUEST_SEND_BINARY) {
            result.outcome = MODEM_SMS_OUTCOME_ERROR;
        }
        if (result.outcome == MODEM_SMS_OUTCOME_OK &&
            strcmp(app->sms_recipient_prefill,
                   app->sms_send_submitted_recipient) == 0) {
            app->sms_recipient_prefill[0] = '\0';
        }
        app->sms_send_submitted_text[0] = '\0';
        app->sms_send_submitted_recipient[0] = '\0';
        if (!ui_owned) {
            return true;
        }
        if (result.outcome == MODEM_SMS_OUTCOME_OK) {
            open_display_sid(app,
                             3u,
                             0x183u,
                             "Picture\nmessage\nsent",
                             APP_ROUTE_MESSAGES_LIST,
                             now);
        } else if (result.outcome == MODEM_SMS_OUTCOME_UNCERTAIN) {
            open_display_sid(app,
                             0u,
                             0x229u,
                             "Result\nunknown",
                             APP_ROUTE_MESSAGES_LIST,
                             now);
        } else {
            open_display_sid(app,
                             0u,
                             0x359u,
                             "Message\nsending\nfailed",
                             APP_ROUTE_MESSAGES_LIST,
                             now);
        }
        return true;
    }
    if (ui_owned &&
        time_diff_ms(now, app->messages_picture_send_started_ms +
                              PICTURE_SMS_SEND_TIMEOUT_MS) >= 0) {
        app->messages_picture_send_waiting = false;
        open_display_sid(app,
                         0u,
                         0x359u,
                         "Message\nsending\nfailed",
                         APP_ROUTE_MESSAGES_LIST,
                         now);
        return true;
    }
    return detached;
}

void messages_picture_save_received(app_t *app,
                                    const store_picture_message_t *picture,
                                    uint32_t now) {
    if (picture == NULL) {
        open_display(app,
                     0u,
                     "Message",
                     "function",
                     "failed",
                     APP_ROUTE_MESSAGES_LIST,
                     now);
        return;
    }
    for (uint8_t slot = 0u; slot < STORE_PICTURE_SLOT_COUNT; slot++) {
        store_picture_message_t existing;
        if (store_picture_message_get(slot, &existing) != STORE_STATUS_OK) {
            picture_store_in_slot(app, slot, picture, false, now);
            return;
        }
    }
    app->messages_picture_save_pending = true;
    app->messages_picture_save_candidate = *picture;
    app->messages_picture_save_return_kind = app->messages_kind;
    app->messages_picture_save_return_selected = app->messages_selected;
    app->messages_kind = MESSAGES_KIND_PICTURES;
    app->messages_mode = MESSAGES_MODE_LIST;
    app->messages_selected = 0u;
    app->route = APP_ROUTE_MESSAGES_LIST;
    app->dirty = true;
}

uint16_t messages_picture_composer_max_len(const app_t *app) {
    store_picture_message_t picture;
    if (picture_active_slot(app, &picture) < STORE_PICTURE_SLOT_COUNT) {
        return picture_text_max_len(&picture);
    }
    return STORE_PICTURE_TEXT_MAX;
}

uint8_t messages_picture_editor_option_count(void) {
    return (uint8_t)ARRAY_COUNT(PICTURE_EDITOR_OPTIONS);
}

const char *messages_picture_editor_option_label(uint8_t index) {
    if (index >= ARRAY_COUNT(PICTURE_EDITOR_OPTIONS)) {
        return "";
    }
    return ts_or(PICTURE_EDITOR_OPTIONS_SID[index], PICTURE_EDITOR_OPTIONS[index]);
}

void messages_picture_select_editor_option(app_t *app,
                                            uint8_t index,
                                            uint32_t now) {
    if (index >= ARRAY_COUNT(PICTURE_EDITOR_OPTIONS)) {
        return;
    }
    const char *label = PICTURE_EDITOR_OPTIONS[index];
    if (strcmp(label, "Send") == 0) {
        copy_text(app->messages_picture_draft,
                  sizeof(app->messages_picture_draft),
                  app->sms_composer_text);
        open_picture_recipient_editor(app, app->sms_recipient_prefill, now);
    } else if (strcmp(label, "Save") == 0) {
        picture_save_draft(app, now);
    } else if (strcmp(label, "Clear text") == 0) {
        picture_clear_draft(app, now);
    } else if (strcmp(label, "Preview") == 0 || strcmp(label, "Exit") == 0) {
        copy_text(app->messages_picture_draft,
                  sizeof(app->messages_picture_draft),
                  app->sms_composer_text);
        messages_picture_return_to_preview(app);
    }
}

void messages_picture_return_to_preview(app_t *app) {
    store_picture_message_t picture;
    uint8_t slot = picture_active_slot(app, &picture);
    if (slot >= STORE_PICTURE_SLOT_COUNT) {
        open_messages_menu(app, 3u);
        return;
    }
    copy_text(app->messages_picture_draft,
              sizeof(app->messages_picture_draft),
              app->sms_composer_text);
    app->messages_picture_pending_valid = true;
    app->messages_picture_pending_slot = slot;
    app->messages_picture_text_editing = false;
    app->messages_kind = MESSAGES_KIND_PICTURES;
    app->messages_mode = MESSAGES_MODE_READ;
    app->route = APP_ROUTE_MESSAGES_LIST;
    app->dirty = true;
}

void open_picture_recipient_editor(app_t *app,
                                   const char *value,
                                   uint32_t now) {
    open_editor(app,
                "Enter number:",
                value != NULL ? value : app->sms_recipient_prefill,
                21u,
                EDITOR_KIND_NUMBER,
                EDITOR_CONTEXT_PICTURE_RECIPIENT,
                true,
                now);
    update_sms_recipient_softkey(app);
}

void messages_submit_picture_recipient(app_t *app, uint32_t now) {
    if (app->editor_value[0] == '\0') {
        open_display_sid(app,
                         0u,
                         0x208u,
                         "No phone\nnumber",
                         APP_ROUTE_EDITOR,
                         now);
        return;
    }
    copy_text(app->sms_recipient_prefill,
              sizeof(app->sms_recipient_prefill),
              app->editor_value);
    close_editor(app);
    picture_start_send(app, app->sms_recipient_prefill, now);
}

void messages_cancel_picture_recipient(app_t *app, uint32_t now) {
    (void)now;
    close_editor(app);
    app->messages_kind = MESSAGES_KIND_PICTURES;
    app->messages_mode = MESSAGES_MODE_READ;
    app->route = APP_ROUTE_MESSAGES_LIST;
    app->dirty = true;
}

static bool picture_by_ordinal(uint8_t ordinal,
                               uint8_t *out_slot,
                               store_picture_message_t *out_message) {
    uint8_t seen = 0u;
    for (uint8_t slot = 0u; slot < STORE_PICTURE_SLOT_COUNT; slot++) {
        store_picture_message_t picture;
        if (store_picture_message_get(slot, &picture) != STORE_STATUS_OK) {
            continue;
        }
        if (seen == ordinal) {
            if (out_slot != NULL) {
                *out_slot = slot;
            }
            if (out_message != NULL) {
                *out_message = picture;
            }
            return true;
        }
        seen++;
    }
    return false;
}

static uint8_t picture_selected_slot(const app_t *app,
                                     store_picture_message_t *out_message) {
    uint8_t count = picture_count();
    if (count == 0u) {
        return STORE_PICTURE_SLOT_COUNT;
    }
    uint8_t selected = app->messages_selected >= count
        ? (uint8_t)(count - 1u)
        : app->messages_selected;
    uint8_t slot = STORE_PICTURE_SLOT_COUNT;
    if (!picture_by_ordinal(selected, &slot, out_message)) {
        return STORE_PICTURE_SLOT_COUNT;
    }
    return slot;
}

static uint8_t picture_active_slot(const app_t *app,
                                   store_picture_message_t *out_message) {
    if (app->messages_picture_pending_valid &&
        app->messages_picture_pending_slot < STORE_PICTURE_SLOT_COUNT &&
        store_picture_message_get(app->messages_picture_pending_slot,
                                  out_message) == STORE_STATUS_OK) {
        return app->messages_picture_pending_slot;
    }
    return picture_selected_slot(app, out_message);
}

static uint8_t picture_text_max_len(const store_picture_message_t *message) {
    uint8_t width = STORE_PICTURE_WIDTH;
    uint8_t height = STORE_PICTURE_HEIGHT;
    if (message != NULL) {
        width = message->width == 0u ? STORE_PICTURE_WIDTH : message->width;
        height = message->height == 0u ? STORE_PICTURE_HEIGHT : message->height;
    }
    uint16_t bitmap_bytes =
        (uint16_t)(((uint16_t)width * (uint16_t)height + 7u) / 8u);
    uint16_t room = bitmap_bytes >= 0x175u
        ? 20u
        : (uint16_t)(0x175u - bitmap_bytes);
    if (room < 20u) {
        room = 20u;
    }
    if (room > STORE_PICTURE_TEXT_MAX) {
        room = STORE_PICTURE_TEXT_MAX;
    }
    return (uint8_t)room;
}

static void picture_apply_draft(const app_t *app,
                                uint8_t slot,
                                store_picture_message_t *message) {
    if (message == NULL || !app->messages_picture_pending_valid ||
        app->messages_picture_pending_slot != slot) {
        return;
    }
    copy_text(message->text,
              sizeof(message->text),
              app->messages_picture_draft);
}

static void picture_draw_preview(framebuffer_t *fb,
                                 const store_picture_message_t *message) {
    fb_clear(fb, false);
    if (message == NULL) {
        return;
    }
    uint8_t width = message->width == 0u ? STORE_PICTURE_WIDTH : message->width;
    uint8_t height = message->height == 0u ? STORE_PICTURE_HEIGHT : message->height;
    if (width > STORE_PICTURE_WIDTH) {
        width = STORE_PICTURE_WIDTH;
    }
    if (height > STORE_PICTURE_HEIGHT) {
        height = STORE_PICTURE_HEIGHT;
    }
    for (uint8_t y = 0u; y < height; y++) {
        for (uint8_t x = 0u; x < width; x++) {
            uint16_t bit = (uint16_t)y * width + x;
            uint16_t byte_index = bit / 8u;
            if (byte_index >= message->bitmap_len ||
                byte_index >= STORE_PICTURE_BITMAP_BYTES) {
                continue;
            }
            if ((message->bitmap[byte_index] &
                 (uint8_t)(1u << (7u - (bit & 7u)))) != 0u) {
                fb_pixel(fb, 6 + x, 2 + y, true);
            }
        }
    }
    if (message->text[0] != '\0') {
        fb_text(fb,
                asset_font(FONT_FS2),
                message->text,
                2,
                32,
                true,
                80);
    }
}

static void picture_begin_edit(app_t *app, uint32_t now) {
    store_picture_message_t picture;
    uint8_t slot = picture_active_slot(app, &picture);
    if (slot >= STORE_PICTURE_SLOT_COUNT) {
        open_messages_menu(app, 3u);
        return;
    }
    bool same_pending = app->messages_picture_pending_valid &&
                        app->messages_picture_pending_slot == slot;
    app->messages_picture_pending_valid = true;
    app->messages_picture_pending_slot = slot;
    if (!same_pending) {
        copy_text(app->messages_picture_draft,
                  sizeof(app->messages_picture_draft),
                  picture.text);
    }
    messages_composer_open_picture(app, app->messages_picture_draft, now);
}

static void picture_save_draft(app_t *app, uint32_t now) {
    store_picture_message_t picture;
    uint8_t slot = picture_active_slot(app, &picture);
    if (slot >= STORE_PICTURE_SLOT_COUNT) {
        open_messages_menu(app, 3u);
        return;
    }
    copy_text(app->messages_picture_draft,
              sizeof(app->messages_picture_draft),
              app->sms_composer_text);
    copy_text(picture.text,
              sizeof(picture.text),
              app->messages_picture_draft);
    (void)store_picture_message_set(slot, &picture);
    app->messages_picture_text_editing = false;
    app->messages_picture_pending_valid = true;
    app->messages_picture_pending_slot = slot;
    app->messages_kind = MESSAGES_KIND_PICTURES;
    app->messages_mode = MESSAGES_MODE_READ;
    app->route = APP_ROUTE_MESSAGES_LIST;
    open_display_sid(app,
                     3u,
                     0x180u,
                     "Picture\nmessage\nsaved",
                     APP_ROUTE_MESSAGES_LIST,
                     now);
}

static void picture_clear_draft(app_t *app, uint32_t now) {
    (void)now;
    app->messages_picture_draft[0] = '\0';
    messages_composer_clear_text(app);
    app->route = APP_ROUTE_SMS_COMPOSER;
    app->dirty = true;
}

static void picture_start_send(app_t *app,
                               const char *recipient,
                               uint32_t now) {
    store_picture_message_t picture;
    uint8_t slot = picture_active_slot(app, &picture);
    if (slot >= STORE_PICTURE_SLOT_COUNT) {
        open_messages_menu(app, 3u);
        return;
    }
    picture_apply_draft(app, slot, &picture);
    uint8_t payload[PICTURE_SMS_PAYLOAD_MAX];
    uint16_t payload_len = 0u;
    uint8_t chunks = 0u;
    if (!sms_picture_payload_encode(&picture,
                                    app->messages_picture_draft,
                                    payload,
                                    sizeof(payload),
                                    &payload_len,
                                    &chunks)) {
        open_display(app,
                     0u,
                     "Message",
                     "function",
                     "failed",
                     APP_ROUTE_MESSAGES_LIST,
                     now);
        return;
    }
    app->messages_picture_payload_len = payload_len;
    app->messages_picture_payload_chunks = chunks;
    modem_status_t status;
    modem_service_get_status(&status);
    if (!status.sim_ready) {
        open_display_sid(app,
                         2u,
                         0x297u,
                         "SIM card\nnot ready",
                         APP_ROUTE_MESSAGES_LIST,
                         now);
        return;
    }
    uint32_t request_id = 0u;
    if (!modem_service_request_send_binary_sms(recipient,
                                               payload,
                                               payload_len,
                                               SMS_CODEC_PICTURE_PORT,
                                               PICTURE_SMS_SOURCE_PORT,
                                               &request_id)) {
        open_display_sid(app,
                         0u,
                         0x35au,
                         "Still\nsending\nprevious",
                         APP_ROUTE_MESSAGES_LIST,
                         now);
        return;
    }
    app->messages_picture_send_waiting = true;
    app->messages_picture_send_request_id = request_id;
    app->sms_send_submitted_text[0] = '\0';
    copy_text(app->sms_send_submitted_recipient,
              sizeof(app->sms_send_submitted_recipient), recipient);
    app->messages_picture_send_started_ms = now;
    app->messages_picture_text_editing = false;
    app->messages_kind = MESSAGES_KIND_PICTURES;
    app->messages_mode = MESSAGES_MODE_READ;
    open_display_sid(app,
                     46u,
                     0x182u,
                     "Sending\npicture message",
                     APP_ROUTE_MESSAGES_LIST,
                     now);
}

static void picture_select_saved_option(app_t *app, uint32_t now) {
    uint8_t option = app->messages_option_selected;
    if (option >= ARRAY_COUNT(PICTURE_OPTIONS)) {
        option = 0u;
    }
    store_picture_message_t picture;
    uint8_t slot = picture_selected_slot(app, &picture);
    if (slot >= STORE_PICTURE_SLOT_COUNT) {
        app->messages_mode = MESSAGES_MODE_LIST;
        app->dirty = true;
        return;
    }
    const char *label = PICTURE_OPTIONS[option];
    bool same_pending = app->messages_picture_pending_valid &&
                        app->messages_picture_pending_slot == slot;
    app->messages_picture_pending_valid = true;
    app->messages_picture_pending_slot = slot;
    if (!same_pending) {
        copy_text(app->messages_picture_draft,
                  sizeof(app->messages_picture_draft),
                  picture.text);
    }
    picture_apply_draft(app, slot, &picture);
    if (strcmp(label, "Edit text") == 0) {
        picture_begin_edit(app, now);
    } else if (strcmp(label, "Erase") == 0) {
        open_confirm_sid(app,
                         CONFIRM_CONTEXT_PICTURE_MESSAGE_ERASE,
                         0x171u,
                         "Erase picture\nmessage?",
                         2);
    } else if (strcmp(label, "Details") == 0) {
        open_display_sid(app,
                         2u,
                         0x177u,
                         "No more\ndetails\navailable",
                         APP_ROUTE_MESSAGES_LIST,
                         now);
    } else if (strcmp(label, "Use number") == 0) {
        open_display_sid(app,
                         2u,
                         0x33au,
                         "No number\nfound\non this screen",
                         APP_ROUTE_MESSAGES_LIST,
                         now);
    } else if (strcmp(label, "Forward") == 0) {
        open_picture_recipient_editor(app, "", now);
    }
}

static void picture_store_in_slot(app_t *app,
                                  uint8_t slot,
                                  const store_picture_message_t *picture,
                                  bool replacement,
                                  uint32_t now) {
    if (slot >= STORE_PICTURE_SLOT_COUNT || picture == NULL ||
        store_picture_message_set(slot, picture) != STORE_STATUS_OK) {
        open_display_sid(app,
                         2u,
                         0x176u,
                         "No space\nfor new\npictures",
                         APP_ROUTE_MESSAGES_LIST,
                         now);
        return;
    }
    app->messages_picture_save_pending = false;
    app->messages_picture_pending_valid = true;
    app->messages_picture_pending_slot = slot;
    app->messages_kind = MESSAGES_KIND_PICTURES;
    app->messages_mode = MESSAGES_MODE_READ;
    app->messages_selected = 0u;
    uint8_t seen = 0u;
    for (uint8_t i = 0u; i < STORE_PICTURE_SLOT_COUNT; i++) {
        store_picture_message_t existing;
        if (store_picture_message_get(i, &existing) != STORE_STATUS_OK) {
            continue;
        }
        if (i == slot) {
            app->messages_selected = seen;
            break;
        }
        seen++;
    }
    if (replacement) {
        open_display_sid(app,
                         3u,
                         0x17du,
                         "Old picture\nreplaced",
                         APP_ROUTE_MESSAGES_LIST,
                         now);
    } else {
        open_display_sid(app,
                         3u,
                         0x180u,
                         "Picture message\nsaved",
                         APP_ROUTE_MESSAGES_LIST,
                         now);
    }
}

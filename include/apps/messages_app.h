#ifndef APPS_MESSAGES_APP_H
#define APPS_MESSAGES_APP_H

#include <stdbool.h>
#include <stdint.h>

#include "app.h"

typedef enum {
    MESSAGES_KIND_INBOX = 0,
    MESSAGES_KIND_OUTBOX,
    MESSAGES_KIND_PICTURES,
    MESSAGES_KIND_SETTINGS,
    MESSAGES_KIND_INFO,
} messages_kind_t;

typedef enum {
    MESSAGES_MODE_LIST = 0,
    MESSAGES_MODE_READ,
    MESSAGES_MODE_OPTIONS,
    MESSAGES_MODE_DETAIL,
    MESSAGES_MODE_SETTINGS_TOP,
    MESSAGES_MODE_SETTINGS_MENU,
    MESSAGES_MODE_SETTINGS_VALUE,
    MESSAGES_MODE_INFO,
} messages_mode_t;

typedef enum {
    SMS_MODE_SENTENCE = 0,
    SMS_MODE_LOWER,
    SMS_MODE_UPPER,
    SMS_MODE_NUMERIC,
} sms_mode_t;

void messages_app_init(app_t *app);

void render_messages_menu(const app_t *app, framebuffer_t *fb);
void render_messages_list(const app_t *app, framebuffer_t *fb);
void render_sms_composer(const app_t *app, framebuffer_t *fb);
void render_sms_options(const app_t *app, framebuffer_t *fb);
void render_sms_symbols(const app_t *app, framebuffer_t *fb);

bool handle_messages_menu_key(app_t *app, uint16_t key, uint32_t now);
bool handle_messages_list_key(app_t *app, uint16_t key, uint32_t now);
bool handle_sms_composer_key(app_t *app, uint16_t key, event_type_t event_type, uint32_t now);
bool handle_sms_options_key(app_t *app, uint16_t key, uint32_t now);
bool handle_sms_symbols_key(app_t *app, uint16_t key, uint32_t now);
bool poll_sms(app_t *app, uint32_t now);
bool tick_sms_composer(app_t *app, uint32_t now);

void open_messages_menu(app_t *app, uint8_t selected);
void open_messages_mailbox(app_t *app, messages_kind_t kind, uint32_t now);
void open_sms_composer(app_t *app, const char *text, const char *recipient, uint32_t now);
void open_sms_recipient_editor(app_t *app, const char *value, uint32_t now);
void open_picture_recipient_editor(app_t *app, const char *value, uint32_t now);
void update_sms_recipient_softkey(app_t *app);
void start_sms_send(app_t *app,
                    const char *recipient,
                    const char *text,
                    app_route_t return_route,
                    uint32_t now);
void messages_submit_picture_recipient(app_t *app, uint32_t now);
void messages_cancel_picture_recipient(app_t *app, uint32_t now);
bool messages_picture_open_received(app_t *app, uint32_t now);
void messages_picture_ask_save(app_t *app);
void messages_picture_confirm_save(app_t *app, bool save, uint32_t now);
void messages_picture_draw_notice(framebuffer_t *fb);
bool sms_t9_insert_text(app_t *app, const char *text, bool replace_active, bool add_user_word);

#endif

#ifndef APPS_SETTINGS_APP_H
#define APPS_SETTINGS_APP_H

#include <stdbool.h>
#include <stdint.h>

#include "app_internal.h"

typedef enum {
    SETTINGS_MENU_ROOT = 0,
    SETTINGS_MENU_CALL,
    SETTINGS_MENU_PHONE,
    SETTINGS_MENU_SECURITY,
    SETTINGS_MENU_ACCESS_CODES,
} settings_menu_kind_t;

void open_settings_menu(app_t *app, settings_menu_kind_t kind, uint8_t selected);
void settings_open_phone_security_selector(app_t *app);

bool handle_settings_menu_key(app_t *app, uint16_t key, uint32_t now);
bool handle_settings_value_key(app_t *app, uint16_t key, uint32_t now);
bool handle_settings_welcome_options_key(app_t *app, uint16_t key, uint32_t now);
bool handle_settings_restore_key(app_t *app, uint16_t key, event_type_t type, uint32_t now);
bool tick_settings_restore(app_t *app, uint32_t now);
void reset_settings_restore(app_t *app);

void render_settings_menu(const app_t *app, framebuffer_t *fb);
void render_settings_value(const app_t *app, framebuffer_t *fb);
void render_settings_welcome_options(const app_t *app, framebuffer_t *fb);
void render_settings_restore(const app_t *app, framebuffer_t *fb);

void settings_submit_welcome_editor(app_t *app, uint32_t now);
void settings_cancel_editor(app_t *app, uint32_t now);
void settings_submit_pin_request_editor(app_t *app, uint32_t now);
void settings_submit_access_code_editor(app_t *app, uint32_t now);

/* Restore the Settings tree, profile customizations and saved operator logo. Does not
 * erase user data, service provisioning, calibration, or lifetime counters.
 * Requires a successful security-code challenge. Completes the phone-security
 * transaction first; the UI waits for the remaining default writes to commit. */
bool settings_restore_factory_defaults(void);

#endif

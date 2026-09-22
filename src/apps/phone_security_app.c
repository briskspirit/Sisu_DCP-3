#include "apps/phone_security_app.h"
#include "apps/clock_app.h"
#include "apps/dialogs_app.h"
#include "apps/menu_shortcuts.h"
#include "apps/settings_app.h"
#include "services/phone_security.h"
#include "services/input_keys.h"
#include "services/key_utils.h"
#include "services/strings.h"
#include "ui/status_chrome.h"

#include <string.h>

static void erase_code(app_t *app) {
    volatile char *p = app->phone_security_code;
    for (size_t i = 0u; i < sizeof(app->phone_security_code); i++) p[i] = '\0';
}

void open_phone_security(app_t *app, bool startup, uint32_t now) {
    menu_shortcuts_cancel(app);
    erase_code(app);
    security_code_forget_grant();
    app->phone_security_startup = startup;
    if (startup) app->keyguard_locked = false;
    app->phone_security_setting_authorized = false;
    app->phone_security_generation = phone_security_generation();
    app->route = APP_ROUTE_PHONE_SECURITY;
    app->backlight_activity_pending = true;
    app->backlight_activity_ms = now;
    app->dirty = true;
}

bool phone_security_blocks_ui(const app_t *app) {
    if (app->route == APP_ROUTE_POWER_OFF || app->route == APP_ROUTE_CONTACT_SERVICE) return false;
    return app->phone_security_startup || phone_security_state() != PHONE_SECURITY_OPEN;
}

bool tick_phone_security(app_t *app, uint32_t now) {
    bool changed = false;
    if (app->phone_security_view_state != (uint8_t)phone_security_state()) {
        app->phone_security_view_state = (uint8_t)phone_security_state();
        app->dirty = true;
        changed = true;
    }
    if (app->phone_security_generation != phone_security_generation()) {
        erase_code(app);
        app->phone_security_setting_authorized = false;
        app->phone_security_generation = phone_security_generation();
        changed = true;
    }
    if (app->route == APP_ROUTE_POWER_OFF) {
        erase_code(app);
        app->phone_security_startup = false;
        app->phone_security_setting_authorized = false;
        security_code_forget_grant();
        return changed;
    }
    if (app->route == APP_ROUTE_POWERUP) return changed;
    if (!phone_security_blocks_ui(app)) return changed;
    bool covered = app->route == APP_ROUTE_DISPLAY_MESSAGE &&
                   app->display_return_route == APP_ROUTE_PHONE_SECURITY;
    menu_shortcuts_cancel(app);
    if (phone_security_state() == PHONE_SECURITY_OPEN && app->phone_security_startup) {
        if (covered) return changed;
        erase_code(app);
        app->phone_security_startup = false;
        app->route = APP_ROUTE_STANDBY;
        (void)start_clock_boot_setup_if_needed(app, now);
        app->dirty = true;
        return true;
    }
    if (app->route != APP_ROUTE_PHONE_SECURITY && !covered) {
        open_phone_security(app, true, now);
        return true;
    }
    app->phone_security_startup = true;
    if (changed) app->dirty = true;
    return changed;
}

bool handle_phone_security_key(app_t *app, uint16_t key, event_type_t type, uint32_t now) {
    if (app->phone_security_startup && phone_security_state() != PHONE_SECURITY_LOCKED) return true;
    char *code = app->phone_security_code;
    size_t len = strlen(code);
    if (key == KEY_C && (type == EVENT_KEY_DOWN || type == EVENT_KEY_HOLD)) {
        if (len == 0u && !app->phone_security_startup) {
            security_code_forget_grant();
            open_settings_menu(app, SETTINGS_MENU_SECURITY, 3u);
        } else if (type == EVENT_KEY_HOLD) erase_code(app);
        else if (len != 0u) code[len - 1u] = '\0';
        app->dirty = true;
        return true;
    }
    if (type != EVENT_KEY_DOWN) return true;
    char digit = key_digit(key);
    if (digit >= '0' && digit <= '9') {
        if (len + 1u < sizeof(app->phone_security_code)) {
            code[len] = digit;
            code[len + 1u] = '\0';
            app->dirty = true;
        }
        return true;
    }
    if (key != KEY_NAVI || len == 0u) return true;
    security_code_result_t result = security_code_verify(code, now);
    erase_code(app);
    if (result == SECURITY_CODE_OK) {
        if (app->phone_security_startup) {
            if (phone_security_unlock(now)) return tick_phone_security(app, now);
            result = SECURITY_CODE_UNAVAILABLE;
        } else {
            app->phone_security_setting_authorized = true;
            settings_open_phone_security_selector(app);
            return true;
        }
    }
    open_display_sid(app, result == SECURITY_CODE_WRONG ? 0u : 32u,
                     result == SECURITY_CODE_WRONG ? 0x145u : 0x210u,
                     result == SECURITY_CODE_WRONG ? "Code\nerror" : "Not done",
                     APP_ROUTE_PHONE_SECURITY, now);
    return true;
}

void render_phone_security(const app_t *app, framebuffer_t *fb) {
    fb_clear(fb, false);
    if (app->phone_security_startup && phone_security_state() != PHONE_SECURITY_LOCKED) {
        draw_battery(fb, app->battery_bars);
        return;
    }
    const font_t *font = asset_font(FONT_FS2);
    fb_bitmap(fb, 14u, 0, 0, true, true);
    draw_text_block(fb, font, ts_or(0x29au, "Security\ncode:"), 0, 7, FB_WIDTH, 9, 2u);
    char stars[sizeof(app->phone_security_code)];
    size_t len = strlen(app->phone_security_code);
    memset(stars, '*', len);
    stars[len] = '\0';
    fb_text(fb, font, stars, 0, 28, true, FB_WIDTH);
    draw_softkey(fb, ts_or(0x2e9u, "OK"));
}

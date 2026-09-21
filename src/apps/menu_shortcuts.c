#include "apps/menu_shortcuts.h"

#include <stdio.h>
#include <string.h>

#include "app_internal.h"
#include "app_router.h"
#include "apps/dialogs_app.h"
#include "services/input_keys.h"
#include "services/key_utils.h"
#include "services/timebase.h"

/* v6.00: numeric editor 0x002ded94 (timer 0x31, 0x181 ticks),
 * menu dispatcher 0x002638e4 (timer 0x3e, 0xc0 ticks). Each tick is 8 ms. */
#define SHORTCUT_INPUT_MS 3080u
#define SHORTCUT_SELECT_MS 1536u

static bool current_view(app_t *app, menu_shortcut_view_t *view) {
    memset(view, 0, sizeof(*view));
    switch (app->route) {
    case APP_ROUTE_MAIN_MENU: return main_menu_shortcut_view(app, view);
    case APP_ROUTE_PHONEBOOK_MENU: return phonebook_shortcut_view(app, view);
    case APP_ROUTE_MESSAGES_MENU:
    case APP_ROUTE_MESSAGES_LIST: return messages_shortcut_view(app, view);
    case APP_ROUTE_SETTINGS_MENU:
    case APP_ROUTE_SETTINGS_VALUE: return settings_shortcut_view(app, view);
    case APP_ROUTE_CALL_REGISTER_MENU:
    case APP_ROUTE_CALL_REGISTER_METRIC: return call_register_shortcut_view(app, view);
    case APP_ROUTE_CALL_DIVERT_MENU: return call_divert_shortcut_view(app, view);
    case APP_ROUTE_CLOCK_MENU: return clock_shortcut_view(app, view);
    case APP_ROUTE_TONES_MENU:
    case APP_ROUTE_TONES_SETTING: return tones_shortcut_view(app, view);
    case APP_ROUTE_PROFILES_MENU: return profiles_shortcut_view(app, view);
    case APP_ROUTE_GAMES_MENU: return games_shortcut_view(app, view);
    case APP_ROUTE_SNAKE_MENU: return snake_shortcut_view(app, view);
    case APP_ROUTE_ROTATION_MENU: return rotation_shortcut_view(app, view);
    case APP_ROUTE_MEMORY_MENU: return memory_shortcut_view(app, view);
    case APP_ROUTE_REACT_MENU: return react_shortcut_view(app, view);
    case APP_ROUTE_LOGIC_MENU: return logic_shortcut_view(app, view);
    case APP_ROUTE_PACMAN_MENU: return pacman_shortcut_view(app, view);
    default: return false;
    }
}

void menu_shortcuts_cancel(app_t *app) {
    if (app->menu_shortcut_active) app->dirty = true;
    app->menu_shortcut_active = false;
    app->menu_shortcut_pending = false;
    app->menu_shortcut_digits = 0u;
    app->menu_shortcut_path[0] = '\0';
    /* Keep the physical-key latch until release, even after entering an editor. */
}

static bool bind_view(app_t *app, uint32_t now) {
    menu_shortcut_view_t view;
    if (!current_view(app, &view) || view.count == 0u || app->keyguard_locked) {
        menu_shortcuts_cancel(app);
        return false;
    }
    app->menu_shortcut_route = app->route;
    app->menu_shortcut_context = view.context;
    app->menu_shortcut_deadline_ms = now + SHORTCUT_INPUT_MS;
    app->menu_shortcut_active = true;
    return true;
}

void menu_shortcuts_begin(app_t *app, uint32_t now) {
    menu_shortcuts_cancel(app);
    app->menu_shortcut_number = 0u;
    (void)bind_view(app, now);
}

static bool valid_view(app_t *app, menu_shortcut_view_t *view) {
    if (!app->menu_shortcut_active) return false;
    if (app->keyguard_locked || app->route != app->menu_shortcut_route ||
        !current_view(app, view) || view->context != app->menu_shortcut_context ||
        view->count == 0u) {
        menu_shortcuts_cancel(app);
        return false;
    }
    return true;
}

static bool row_visible(const menu_shortcut_view_t *view, uint8_t raw) {
    return raw < view->count && raw < 64u &&
           (view->hidden_mask & (UINT64_C(1) << raw)) == 0u;
}

static void invalid_shortcut(app_t *app, uint32_t now) {
    menu_shortcuts_cancel(app);
    /* 0x00263b1a uses display record 0x34 / SID 0x2aa, then closes the menu. */
    open_display_sid(app, 52u, 0x2aau, "Invalid\nshortcut", APP_ROUTE_STANDBY, now);
}

/* These descriptor frames must finish before entering stack-heavy app actions. */
static __attribute__((noinline)) bool prepare_activation(app_t *app) {
    menu_shortcut_view_t view;
    if (!valid_view(app, &view)) return false;
    if (*view.selected != app->menu_shortcut_selected ||
        !row_visible(&view, app->menu_shortcut_selected)) {
        menu_shortcuts_cancel(app);
        return false;
    }
    size_t len = strlen(app->menu_shortcut_path);
    unsigned ordinal = (unsigned)app->menu_shortcut_selected + 1u;
    int written = snprintf(app->menu_shortcut_path + len,
                           sizeof(app->menu_shortcut_path) - len,
                           "%s%u", len == 0u ? "" : "-", ordinal);
    app->menu_shortcut_pending = false;
    if (written < 0 || (size_t)written >= sizeof(app->menu_shortcut_path) - len) {
        menu_shortcuts_cancel(app);
        return false;
    }
    return true;
}

static __attribute__((noinline)) void finish_activation(app_t *app, app_route_t previous_route,
                               uint32_t previous_context, uint32_t now) {
    menu_shortcut_view_t view;
    if (!current_view(app, &view) ||
        (app->route == previous_route && view.context == previous_context)) {
        menu_shortcuts_cancel(app);
        return;
    }
    (void)bind_view(app, now);
}

static void activate(app_t *app, uint32_t now) {
    app_route_t previous_route = app->route;
    uint32_t previous_context = app->menu_shortcut_context;
    if (!prepare_activation(app)) return;
    /* Keep the descriptor and formatting frames out of the app action's stack. */
    app_router_select_menu(app, now);
    finish_activation(app, previous_route, previous_context, now);
}

static bool select_number(app_t *app, uint32_t now) {
    menu_shortcut_view_t view;
    if (!valid_view(app, &view)) return false;
    uint8_t ordinal = app->menu_shortcut_number;
    if (ordinal == 0u || !row_visible(&view, (uint8_t)(ordinal - 1u))) {
        invalid_shortcut(app, now);
        return false;
    }
    uint8_t raw = (uint8_t)(ordinal - 1u);
    *view.selected = raw;
    if (view.view_start != NULL) {
        uint8_t visible = 0u;
        for (uint8_t i = 0u; i < raw; i++) {
            if (row_visible(&view, i)) visible++;
        }
        *view.view_start = visible;
    }
    if (app->route == APP_ROUTE_MAIN_MENU) {
        app->menu_frame_index = 0u;
        app->menu_animation_playing = false;
        app->menu_selected_ms = now;
        app->menu_next_frame_ms = 0u;
    }
    app->menu_shortcut_selected = raw;
    app->menu_shortcut_digits = 0u;
    app->menu_shortcut_number = 0u;
    app->menu_shortcut_pending = true;
    app->menu_shortcut_deadline_ms = now + SHORTCUT_SELECT_MS;
    app->dirty = true;
    if (app->route == APP_ROUTE_TONES_SETTING) tones_shortcut_selected(app, now);
    return raw < 32u && (view.immediate_mask & (1u << raw)) != 0u;
}

bool menu_shortcuts_tick(app_t *app, uint32_t now) {
    bool was_active = app->menu_shortcut_active;
    menu_shortcut_view_t view;
    if (!valid_view(app, &view)) return was_active;
    if (time_diff_ms(now, app->menu_shortcut_deadline_ms) < 0) return false;
    if (app->menu_shortcut_pending) {
        activate(app, now);
    } else if (app->menu_shortcut_digits != 0u) {
        if (select_number(app, now)) activate(app, now);
    } else {
        menu_shortcuts_cancel(app);
    }
    return true;
}

bool menu_shortcuts_handle(app_t *app, const input_event_t *event) {
    if (event->type == EVENT_KEY_UP) {
        if (event->code == app->menu_shortcut_held_key) app->menu_shortcut_held_key = 0u;
        return false;
    }
    if (event->type == EVENT_KEY_HOLD && event->code == app->menu_shortcut_held_key) {
        return true;
    }
    if (event->type != EVENT_KEY_DOWN && event->type != EVENT_KEY_HOLD) return false;
    if (event->type == EVENT_KEY_DOWN) app->menu_shortcut_held_key = 0u;
    char digit = key_digit(event->code);
    if (digit == '\0') {
        menu_shortcuts_cancel(app);
        return false;
    }
    uint32_t now = event->when_ms;
    bool was_active = app->menu_shortcut_active;
    (void)menu_shortcuts_tick(app, now);
    if (was_active && app->route == APP_ROUTE_DISPLAY_MESSAGE) return true;
    menu_shortcut_view_t view;
    if (!valid_view(app, &view)) return false;
    if (event->type != EVENT_KEY_DOWN) return true;
    if (app->menu_shortcut_pending) {
        activate(app, now);
        /* A fresh digit after the navigation sequence belongs to the editor. */
        if (!valid_view(app, &view)) return app->route == APP_ROUTE_DISPLAY_MESSAGE;
    }
    app->menu_shortcut_held_key = event->code;
    app->menu_keyguard_armed = false;
    /* The ROM's numeric buffer holds two characters, dropping the oldest on
     * overflow. Zero remains a prefix; invalid zero is reported on timeout. */
    app->menu_shortcut_number = (uint8_t)((app->menu_shortcut_number * 10u +
                                         (unsigned)(digit - '0')) % 100u);
    if (app->menu_shortcut_digits < 2u) app->menu_shortcut_digits++;
    app->menu_shortcut_deadline_ms = now + SHORTCUT_INPUT_MS;
    app->dirty = true;
    uint8_t last = view.count;
    while (last != 0u && !row_visible(&view, (uint8_t)(last - 1u))) last--;
    if ((unsigned)app->menu_shortcut_number * 10u > last) {
        if (select_number(app, now)) activate(app, now);
    }
    return true;
}

void menu_shortcuts_render(const app_t *app, framebuffer_t *fb) {
    if (!app->menu_shortcut_active || app->menu_shortcut_digits == 0u ||
        app->route != app->menu_shortcut_route) return;
    char crumb[28];
    snprintf(crumb, sizeof(crumb), "%s%s%u", app->menu_shortcut_path,
             app->menu_shortcut_path[0] == '\0' ? "" : "-",
             (unsigned)app->menu_shortcut_number);
    fb_fill_rect(fb, 0, 0, FB_WIDTH, 6, false);
    draw_right_text_box(fb, asset_font(FONT_FS3), crumb, 0, 0, FB_WIDTH);
}

#include "apps/network_selection_app.h"

#include "apps/dialogs_app.h"
#include "services/input_keys.h"
#include "services/modem_service.h"
#include "services/strings.h"

static bool owns_progress(const app_t *app) {
    return app->network_request_id != 0u && app->route == APP_ROUTE_DISPLAY_MESSAGE &&
        app->display_record_id == 0x25u &&
        app->display_generation == app->network_display_generation;
}

static void leave_progress(app_t *app) {
    modem_service_network_cancel(app->network_request_id);
    app->network_request_id = 0u;
    app->network_display_generation = 0u;
}

static void not_done(app_t *app, uint32_t now) {
    app->network_request_id = 0u;
    open_display_sid(app, 32u, 0x210u, "Not done", APP_ROUTE_SETTINGS_VALUE, now);
}

static void request(app_t *app, modem_network_action_t action, uint32_t now) {
    app->network_request_id = modem_service_network_request(action,
        app->network_list_generation, app->network_selected);
    if (app->network_request_id == 0u) { not_done(app, now); return; }
    /* ROM 0x28a094 and 0x28a054 both use progress record 0x25. */
    open_display_sid(app, 0x25u, action == MODEM_NETWORK_SCAN ? 0x299u : 0x23eu,
        action == MODEM_NETWORK_SCAN ? "Searching" : "Requesting", APP_ROUTE_SETTINGS_VALUE, now);
    app->network_display_generation = app->display_generation;
}

void open_network_selection(app_t *app, bool manual, uint32_t now) {
    leave_progress(app);
    app->network_selected = app->network_view_start = 0u;
    app->network_manual_requested = manual;
    modem_network_snapshot_t view;
    modem_service_get_network_snapshot(&view);
    request(app, !view.ready ? MODEM_NETWORK_REFRESH :
        manual ? MODEM_NETWORK_SCAN : MODEM_NETWORK_SELECT_AUTO, now);
}

bool handle_network_selection_display_key(app_t *app, uint16_t key, uint32_t now) {
    (void)now;
    if (!owns_progress(app)) return false;
    if (key == KEY_C || key == KEY_NAVI) {
        leave_progress(app);
        app->route = APP_ROUTE_SETTINGS_VALUE;
        app->dirty = true;
    }
    return true;
}

bool handle_network_selection_key(app_t *app, uint16_t key, uint32_t now) {
    modem_network_snapshot_t view;
    modem_service_get_network_snapshot(&view);
    if (key == KEY_C) {
        app->route = APP_ROUTE_SETTINGS_VALUE;
    } else if (!view.ready || view.busy || view.list_generation != app->network_list_generation ||
               view.count == 0u) {
        not_done(app, now);
    } else if (key == KEY_UP || key == KEY_DOWN) {
        ui_circular_list_step_3rows(view.count, key == KEY_UP ? -1 : 1,
            &app->network_selected, &app->network_view_start);
    } else if (key == KEY_NAVI) {
        request(app, MODEM_NETWORK_SELECT_MANUAL, now);
    }
    app->dirty = true;
    return true;
}

bool tick_network_selection(app_t *app, uint32_t now) {
    if (app->network_request_id == 0u) {
        if (app->route == APP_ROUTE_NETWORK_LIST) {
            modem_network_snapshot_t view;
            modem_service_get_network_snapshot(&view);
            if (!view.ready || view.list_generation != app->network_list_generation) {
                not_done(app, now);
                return true;
            }
        }
        return false;
    }
    if (!owns_progress(app) || app->keyguard_locked) {
        /* A call, shutdown, or another notice owns the screen now. */
        leave_progress(app);
        return false;
    }
    modem_network_snapshot_t view;
    modem_service_get_network_snapshot(&view);
    if (view.request_id != app->network_request_id) { not_done(app, now); return true; }
    if (view.busy) return false;
    app->network_request_id = 0u;
    if (view.result == MODEM_NETWORK_CANCELLED) {
        app->route = APP_ROUTE_SETTINGS_VALUE;
    } else if (view.result != MODEM_NETWORK_DONE) {
        open_display_sid(app, 32u, view.result == MODEM_NETWORK_NO_ACCESS ? 0x1ddu : 0x210u,
            view.result == MODEM_NETWORK_NO_ACCESS ? "No\naccess" : "Not done",
            APP_ROUTE_SETTINGS_VALUE, now);
    } else if (view.action == MODEM_NETWORK_REFRESH) {
        request(app, app->network_manual_requested ? MODEM_NETWORK_SCAN : MODEM_NETWORK_SELECT_AUTO, now);
    } else if (view.action == MODEM_NETWORK_SCAN) {
        if (view.count == 0u) {
            open_display_sid(app, 0u, 0x207u, "No network\nfound", APP_ROUTE_SETTINGS_VALUE, now);
        } else {
            app->network_list_generation = view.list_generation;
            app->network_selected = app->network_view_start = 0u;
            app->route = APP_ROUTE_NETWORK_LIST;
        }
    } else if (view.action == MODEM_NETWORK_SELECT_AUTO) {
        /* Automatic's original 0xfa3 handler exits to the parent menu. */
        app->settings_menu_kind = app->settings_value_parent_kind;
        app->settings_menu_selected = app->settings_value_parent_selected;
        app->route = APP_ROUTE_SETTINGS_MENU;
    } else {
        modem_network_entry_t entry;
        char text[128];
        const char *name = modem_service_get_network_entry(app->network_list_generation,
            app->network_selected, &entry) ? entry.name : view.plmn;
        ui_format_named(text, sizeof(text), ts_or(0x3b9u, "Selected\nnetwork:\n%U"), name);
        open_display(app, 3u, text, "", "", APP_ROUTE_SETTINGS_VALUE, now);
    }
    app->dirty = true;
    return true;
}

void render_network_selection(const app_t *app, framebuffer_t *fb) {
    modem_network_snapshot_t view;
    modem_service_get_network_snapshot(&view);
    if (view.busy || view.list_generation != app->network_list_generation || view.count == 0u) {
        fb_clear(fb, false);
        return;
    }
    /* Only the three visible names need copying out of the service. */
    char names[3][MODEM_NETWORK_NAME_CAP];
    const char *labels[MODEM_NETWORK_MAX] = {0};
    uint8_t selected = app->network_selected < view.count ? app->network_selected : 0u;
    uint8_t start = app->network_view_start < view.count ? app->network_view_start : 0u;
    if ((uint8_t)((selected + view.count - start) % view.count) >= 3u) start = selected;
    for (uint8_t row = 0u; row < 3u && row < view.count; row++) {
        uint8_t index = (uint8_t)((start + row) % view.count);
        modem_network_entry_t entry;
        if (!modem_service_get_network_entry(view.list_generation, index, &entry)) {
            fb_clear(fb, false); return;
        }
        copy_text(names[row], sizeof(names[row]), entry.name);
        labels[index] = names[row];
    }
    draw_flat_list_circular_view(fb, labels, view.count, selected,
        start, UI_BREADCRUMB_NONE, ts_or(0x2eeu, "Select"));
}

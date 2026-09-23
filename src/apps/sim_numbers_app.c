#include "apps/sim_numbers_app.h"

#include "apps/calls_app.h"
#include "apps/dialogs_app.h"
#include "services/input_keys.h"
#include "services/modem_service.h"
#include "services/strings.h"

_Static_assert(SIM_NUMBER_CAP <= MODEM_PHONE_MAX + 1u, "SIM numbers must fit the dial request");

static bool owns_progress(const app_t *app) {
    return app->sim_numbers_request != 0u && app->route == APP_ROUTE_DISPLAY_MESSAGE &&
        app->display_record_id == 4u && app->display_generation == app->sim_numbers_display;
}
static void cancel(app_t *app) {
    modem_service_sim_numbers_cancel(app->sim_numbers_request);
    app->sim_numbers_request = 0u;
}
static void failure(app_t *app, bool unavailable, uint32_t now) {
    cancel(app);
    open_display_sid(app, 0u, unavailable ? 0x20du : 0x2aeu,
        unavailable ? "Not\nallowed" : "SIM card\nerror", APP_ROUTE_PHONEBOOK_MENU, now);
}
static bool visible(const app_t *app, uint8_t index, sim_number_t *out) {
    return modem_service_sim_number(app->sim_numbers_list, index, out) &&
        out->parent == app->sim_numbers_parent &&
        (!out->home_only || modem_service_sim_numbers_home());
}
/* Keep the scan buffer out of the call-dispatch stack path. */
static __attribute__((noinline)) bool select_first(app_t *app) {
    sim_number_t entry;
    for (uint8_t i = 0u; i < SIM_NUMBERS_MAX; i++) {
        if (visible(app, i, &entry)) { app->sim_numbers_selected = i; return true; }
    }
    return false;
}
void open_sim_numbers(app_t *app, sim_numbers_kind_t kind, uint32_t now) {
    cancel(app);
    app->sim_numbers_parent = SIM_NUMBERS_ROOT;
    app->sim_numbers_list = 0u;
    app->sim_numbers_request = modem_service_sim_numbers_request(kind);
    if (app->sim_numbers_request == 0u) { failure(app, true, now); return; }
    /* Use the existing Phone book wait surface while SIM records arrive. */
    open_display_sid(app, 4u, 0x33du, "Opening", APP_ROUTE_PHONEBOOK_MENU, now);
    app->sim_numbers_display = app->display_generation;
}
bool handle_sim_numbers_display_key(app_t *app, uint16_t key, uint32_t now) {
    (void)now;
    if (!owns_progress(app)) return false;
    if (key == KEY_C || key == KEY_NAVI) {
        cancel(app);
        app->route = APP_ROUTE_PHONEBOOK_MENU;
        app->dirty = true;
    }
    return true;
}
bool tick_sim_numbers(app_t *app, uint32_t now) {
    uint8_t visibility = (uint8_t)(modem_service_sim_numbers_available(SIM_NUMBERS_SERVICE) |
        (modem_service_sim_numbers_available(SIM_NUMBERS_INFO) << 1u));
    bool changed = visibility != app->sim_numbers_visibility;
    app->sim_numbers_visibility = visibility;
    if (changed) app->dirty = true;
    sim_numbers_snapshot_t view;
    modem_service_sim_numbers_snapshot(&view);
    if (app->sim_numbers_request != 0u) {
        if (!owns_progress(app) || app->keyguard_locked) { cancel(app); return changed; }
        if (view.request_id != app->sim_numbers_request) { failure(app, false, now); return true; }
        if (view.busy) return changed;
        app->sim_numbers_list = app->sim_numbers_request;
        app->sim_numbers_request = 0u;
        if (view.result == SIM_NUMBERS_UNAVAILABLE) failure(app, true, now);
        else if (view.result == SIM_NUMBERS_ERROR) failure(app, false, now);
        else if (view.result == SIM_NUMBERS_CANCELLED || !select_first(app))
            app->route = APP_ROUTE_PHONEBOOK_MENU;
        else app->route = APP_ROUTE_SIM_NUMBERS;
        app->dirty = true;
        return true;
    }
    if (app->route != APP_ROUTE_SIM_NUMBERS) return changed;
    if (view.request_id != app->sim_numbers_list || view.result != SIM_NUMBERS_DONE || view.busy) {
        failure(app, false, now); return true;
    }
    sim_number_t entry;
    if (!visible(app, app->sim_numbers_selected, &entry)) {
        app->sim_numbers_parent = SIM_NUMBERS_ROOT;
        if (!select_first(app)) app->route = APP_ROUTE_PHONEBOOK_MENU;
        app->dirty = true;
        return true;
    }
    return changed;
}
bool handle_sim_numbers_key(app_t *app, uint16_t key, uint32_t now) {
    sim_number_t entry;
    if (key == KEY_C) {
        uint8_t parent = app->sim_numbers_parent;
        if (parent == SIM_NUMBERS_ROOT ||
            !modem_service_sim_number(app->sim_numbers_list, parent, &entry))
            app->route = APP_ROUTE_PHONEBOOK_MENU;
        else { app->sim_numbers_parent = entry.parent; app->sim_numbers_selected = parent; }
    } else if (!visible(app, app->sim_numbers_selected, &entry)) {
        failure(app, false, now);
    } else if (key == KEY_UP || key == KEY_DOWN) {
        for (uint8_t step = 1u; step <= SIM_NUMBERS_MAX; step++) {
            uint8_t i = (uint8_t)((app->sim_numbers_selected + SIM_NUMBERS_MAX +
                (key == KEY_UP ? -(int)step : (int)step)) % SIM_NUMBERS_MAX);
            if (visible(app, i, &entry)) { app->sim_numbers_selected = i; break; }
        }
    } else if (key == KEY_NAVI) {
        if (entry.folder) {
            uint8_t old = app->sim_numbers_parent;
            app->sim_numbers_parent = app->sim_numbers_selected;
            if (!select_first(app)) app->sim_numbers_parent = old;
        } else if (entry.number[0] == '\0') {
            open_display_sid(app, 0u, 0x208u, "No phone\nnumber", APP_ROUTE_SIM_NUMBERS, now);
        } else {
            return start_outgoing_call(app, entry.number, entry.name, now, APP_ROUTE_SIM_NUMBERS);
        }
    }
    app->dirty = true;
    return true;
}
void render_sim_numbers(const app_t *app, framebuffer_t *fb) {
    sim_number_t entry;
    fb_clear(fb, false);
    if (!visible(app, app->sim_numbers_selected, &entry)) return;
    char lower[96];
    if (entry.premium) ui_format_named(lower, sizeof(lower), ts_or(0x191u, "Premium %S"), entry.number);
    else copy_text(lower, sizeof(lower), entry.number);
    uint8_t position = 0u, count = 0u;
    sim_number_t other;
    for (uint8_t i = 0u; i < SIM_NUMBERS_MAX; i++) {
        if (visible(app, i, &other)) {
            if (i == app->sim_numbers_selected) position = count;
            count++;
        }
    }
    /* ROM's two runtime strings: alpha tag above, number/premium text below. */
    draw_static_page_list(fb, entry.name[0] ? entry.name : entry.number,
        lower[0] ? lower : NULL, position, count, UI_BREADCRUMB_NONE,
        entry.folder ? "Select" : "Call");
}

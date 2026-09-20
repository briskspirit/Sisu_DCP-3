#include "apps/operator_logo_app.h"

#include "apps/dialogs_app.h"
#include "services/input_keys.h"
#include "services/strings.h"
#include "storage/store_operator_logo.h"

#include <string.h>

void open_received_operator_logo(app_t *app) {
    uint32_t id = store_operator_logo_pending_first();
    if (id == 0u) return;
    app->operator_logo_receive_id = id;
    app->operator_logo_option = 0u;
    app->operator_logo_preview = false;
    app->operator_logo_save_action = 0u;
    app->route = APP_ROUTE_OPERATOR_LOGO;
    app->dirty = true;
}

bool handle_operator_logo_key(app_t *app, uint16_t key, uint32_t now) {
    if (app->operator_logo_save_action != 0u) return true;
    if (app->operator_logo_preview) {
        if (key == KEY_C || key == KEY_NAVI) {
            app->operator_logo_preview = false;
            app->dirty = true;
        }
    } else if (key == KEY_UP || key == KEY_DOWN) {
        app->operator_logo_option = (uint8_t)((app->operator_logo_option +
            (key == KEY_UP ? 2u : 1u)) % 3u);
        app->dirty = true;
    } else if (key == KEY_NAVI || key == KEY_C) {
        if (key == KEY_NAVI && app->operator_logo_option == 0u) {
            app->operator_logo_preview = true;
            app->dirty = true;
        } else {
            bool save = key == KEY_NAVI && app->operator_logo_option == 1u;
            store_status_t status = save
                ? store_operator_logo_pending_save(app->operator_logo_receive_id)
                : store_operator_logo_pending_discard(app->operator_logo_receive_id);
            if (status == STORE_STATUS_OK) app->operator_logo_save_action = save ? 1u : 2u;
            else open_display_sid(app, 0u, 0x3b3u, "Not\nsaved", APP_ROUTE_OPERATOR_LOGO, now);
        }
    }
    return true;
}

bool tick_operator_logo(app_t *app, uint32_t now) {
    if (app->operator_logo_save_action == 0u) return false;
    store_status_t status = store_operator_logo_commit_status();
    if (status == STORE_STATUS_NOT_READY) return false;
    uint8_t action = app->operator_logo_save_action;
    app->operator_logo_save_action = 0u;
    if (app->route == APP_ROUTE_OPERATOR_LOGO) {
        if (status != STORE_STATUS_OK)
            open_display_sid(app, 0u, 0x3b3u, "Not\nsaved", APP_ROUTE_OPERATOR_LOGO, now);
        else if (action == 1u)
            open_display_sid(app, 3u, 0x21du, "Operator\nlogo\nsaved", APP_ROUTE_STANDBY, now);
        else {
            app->route = APP_ROUTE_STANDBY;
            open_received_operator_logo(app);
        }
    }
    app->dirty = true;
    return true;
}

void draw_operator_logo(framebuffer_t *fb, const operator_logo_t *logo, int y) {
    for (unsigned row = 0u; row < OPERATOR_LOGO_HEIGHT; row++) {
        for (unsigned x = 0u; x < OPERATOR_LOGO_WIDTH; x++) {
            unsigned bit = row * OPERATOR_LOGO_WIDTH + x;
            fb_pixel(fb, 6 + (int)x, y + (int)row,
                     (logo->pixels[bit / 8u] & (0x80u >> (bit % 8u))) != 0u);
        }
    }
}

void render_operator_logo(const app_t *app, framebuffer_t *fb) {
    fb_clear(fb, false);
    if (app->operator_logo_preview) {
        operator_logo_t logo;
        if (store_operator_logo_pending_get(app->operator_logo_receive_id, &logo) == STORE_STATUS_OK)
            /* Original window 0x4b: (6,7), 72x30, horizontal centering. */
            draw_operator_logo(fb, &logo, 7);
        draw_softkey(fb, ts_or(0x2d8u, "Back"));
    } else {
        /* v6.00 menu 0x2dcff8; no gallery or forwarding action. */
        const char *labels[] = {ts_or(0x21eu, "Show"), ts_or(0x21cu, "Save"), ts_or(0x21au, "Discard")};
        draw_flat_list(fb, labels, 3u, app->operator_logo_option, 0, "Select");
    }
}

bool operator_logo_update_network(app_t *app, const modem_status_t *status) {
    operator_logo_t logo = {0};
    bool visible = status->at_ready && status->network_registered && !app->sim_missing &&
        (status->signal.valid_fields & MODEM_SIGNAL_VALID_PLMN) != 0u &&
        store_operator_logo_get(&logo) == STORE_STATUS_OK &&
        operator_logo_matches(&logo, status->signal.mcc, status->signal.mnc);
    bool changed = visible != app->operator_logo_visible ||
        (visible && memcmp(&logo, &app->operator_logo, sizeof(logo)) != 0);
    app->operator_logo_visible = visible;
    if (visible) app->operator_logo = logo;
    return changed;
}

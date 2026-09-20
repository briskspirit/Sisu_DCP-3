#include "apps/business_card_app.h"

#include "apps/dialogs_app.h"
#include "services/business_card_service.h"
#include "services/input_keys.h"
#include "services/strings.h"

void open_received_business_card(app_t *app) {
    uint32_t id = business_card_service_pending_first();
    if (id == 0u || app->business_card_action_pending) return;
    app->business_card_receive_id = id;
    app->business_card_option = 0u;
    app->business_card_preview = false;
    app->route = APP_ROUTE_BUSINESS_CARD;
    app->dirty = true;
}

bool handle_business_card_key(app_t *app, uint16_t key, uint32_t now) {
    if (app->business_card_action_pending) return true;
    if (app->business_card_preview) {
        if (key == KEY_C || key == KEY_NAVI) {
            app->business_card_preview = false;
            app->dirty = true;
        }
    } else if (key == KEY_UP || key == KEY_DOWN) {
        app->business_card_option = (uint8_t)((app->business_card_option +
            (key == KEY_UP ? 2u : 1u)) % 3u);
        app->dirty = true;
    } else if (key == KEY_NAVI || key == KEY_C) {
        if (key == KEY_NAVI && app->business_card_option == 0u) {
            app->business_card_preview = true;
            app->dirty = true;
        } else {
            bool save = key == KEY_NAVI && app->business_card_option == 1u;
            if (business_card_service_request(app->business_card_receive_id, save))
                app->business_card_action_pending = true;
            else open_display_sid(app, 0u, 0x3b3u, "Not\nsaved", APP_ROUTE_BUSINESS_CARD, now);
        }
    }
    return true;
}

bool tick_business_card(app_t *app, uint32_t now) {
    business_card_action_result_t result;
    if (!business_card_service_pop_result(&result)) return false;
    bool own_action = app->business_card_action_pending &&
        result.id == app->business_card_receive_id;
    app->business_card_action_pending = false;
    /* Completion must not replace a call or alarm that interrupted this menu. */
    if (own_action && app->route == APP_ROUTE_BUSINESS_CARD) {
        if (result.outcome == PHONEBOOK_OUTCOME_FULL)
            open_display_sid(app, 0u, 0x280u, "Memory\nfull", APP_ROUTE_BUSINESS_CARD, now);
        else if (result.outcome != PHONEBOOK_OUTCOME_OK)
            open_display_sid(app, 0u, 0x3b3u, "Not\nsaved", APP_ROUTE_BUSINESS_CARD, now);
        else if (result.save)
            open_display_sid(app, 26u, 0x296u, "Saved", APP_ROUTE_STANDBY, now);
        else {
            app->route = APP_ROUTE_STANDBY;
            open_received_business_card(app);
        }
    }
    app->dirty = true;
    return true;
}

void render_business_card(const app_t *app, framebuffer_t *fb) {
    fb_clear(fb, false);
    if (app->business_card_preview) {
        phonebook_entry_t entry;
        if (business_card_service_get(app->business_card_receive_id, &entry)) {
            char visible[MODEM_PHONE_MAX + PHONEBOOK_NAME_MAX + 1u];
            const font_t *name = asset_font(FONT_FS2), *number = asset_font(FONT_FS0);
            /* v6.00 windows 0x37/0x38: shared 84x28 area at y=7;
             * name top-left, large number bottom-right. */
            const char *text = ui_marquee_text(name, entry.name, FB_WIDTH, visible, sizeof(visible));
            fb_text(fb, name, text, 0, 7, true, FB_WIDTH);
            text = ui_marquee_text_line(1u, number, entry.number, FB_WIDTH, visible, sizeof(visible));
            draw_text_right(fb, number, text, 35 - number->height, FB_WIDTH);
        }
        draw_softkey(fb, ts_or(0x2d8u, "Back"));
    } else {
        const char *labels[] = {ts_or(0x67u, "Show"), ts_or(0x65u, "Save"), ts_or(0x63u, "Discard")};
        draw_flat_list(fb, labels, 3u, app->business_card_option, 0, "Select");
    }
}

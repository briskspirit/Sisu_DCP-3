#include "app_runtime.h"

#include "app_status_runtime.h"
#include "apps/calculator_app.h"
#include "apps/call_divert_app.h"
#include "apps/calls_app.h"
#include "apps/clock_app.h"
#include "apps/dialogs_app.h"
#include "apps/games_app.h"
#include "apps/logic_app.h"
#include "apps/main_menu_app.h"
#include "apps/memory_app.h"
#include "apps/messages_app.h"
#include "apps/net_monitor_app.h"
#include "apps/pacman_app.h"
#include "apps/phonebook_app.h"
#include "apps/power_app.h"
#include "apps/powerup_app.h"
#include "apps/profiles_app.h"
#include "apps/react_app.h"
#include "apps/rotation_app.h"
#include "apps/settings_app.h"
#include "apps/service_codes_app.h"
#include "apps/snake_app.h"
#include "apps/standby_app.h"
#include "apps/tones_app.h"
#include "services/core1_services.h"
#include "services/timebase.h"
#include "ui/ui.h"
#include "storage/store_service.h"

void app_runtime_init(app_t *app) {
    app->route = APP_ROUTE_POWER_OFF;
    app->power_on_pending = false;
    app->powerup_stage = APP_POWERUP_DONE;
    app->backlight_force_active = true;
    app->backlight_force_on = false;
    /* Boot lands in the "off" route (battery insert != switch-on, 1:1), but
     * main() has already fully powered the codec -- park its analog like
     * power_off() does, or a cold-booted "off" burns ~10 mA more than a
     * powered-off one (bench 2026-07-05: 50 vs 40 mA). power_on() /
     * the alarm wake re-init it. */
    core1_services_codec_standby();
    app->dirty = true;
    app->input_action = APP_STANDBY_ACTION_CALL;
    messages_app_init(app);
    calls_app_init(app);
    profiles_app_init(app);
    games_app_init(app);
    net_monitor_app_init(app);
    app_status_runtime_init(app);
}

bool app_runtime_tick(app_t *app, uint32_t now_ms) {
    bool changed = false;
    if (store_service_contact_service_required()) {
        if (app->route != APP_ROUTE_POWER_OFF && app->route != APP_ROUTE_CONTACT_SERVICE) {
            enter_contact_service(app, now_ms);
            changed = true;
        }
        /* No alarm, incoming-call, SMS or clock editor can escape this state.
         * Battery protection and the ordinary power-off path remain live. */
        changed |= tick_power_off(app, now_ms);
        changed |= tick_contact_service(app, now_ms);
        changed |= poll_battery(app, now_ms);
        return changed;
    }

    if (app->route == APP_ROUTE_POWERUP && tick_powerup(app, now_ms)) {
        changed = true;
    }
    if (tick_power_off(app, now_ms)) {
        changed = true;
    }

    if (tick_standby(app, now_ms)) {
        changed = true;
    }
    if (app->route == APP_ROUTE_MAIN_MENU && tick_main_menu(app, now_ms)) {
        changed = true;
    }
    if (app->route == APP_ROUTE_DISPLAY_MESSAGE && tick_display_message(app, now_ms)) {
        changed = true;
    }
    if (app->route == APP_ROUTE_EDITOR && tick_editor(app, now_ms)) {
        changed = true;
    }
    if (app->route == APP_ROUTE_CLOCK_EDITOR && tick_clock_editor(app, now_ms)) {
        changed = true;
    }
    if (poll_clock_alarm(app, now_ms)) {
        changed = true;
    }
    if (poll_clock_datetime_commit(app, now_ms)) {
        changed = true;
    }
    if (poll_call_runtime(app, now_ms)) {
        changed = true;
    }
    if (poll_sms(app, now_ms)) {
        changed = true;
    }
    if (tick_sms_composer(app, now_ms)) {
        changed = true;
    }
    if (app->route == APP_ROUTE_CLOCK_ALARM && tick_clock_alarm(app, now_ms)) {
        changed = true;
    }
    if (app->route == APP_ROUTE_CALL && tick_call(app, now_ms)) {
        changed = true;
    }
    if (poll_phonebook(app, now_ms)) {
        changed = true;
    }
    if (tick_phonebook_erase_all(app, now_ms)) {
        changed = true;
    }
    if (tick_phonebook_send(app, now_ms)) {
        changed = true;
    }
    if (tick_settings(app, now_ms)) {
        changed = true;
    }
    if (tick_call_divert(app, now_ms)) {
        changed = true;
    }
    if (tick_service_codes(app, now_ms)) {
        changed = true;
    }
    if (tick_net_monitor(app, now_ms)) {
        changed = true;
    }
    if (tick_profiles(app, now_ms)) {
        changed = true;
    }
    if (tick_tones_setting(app, now_ms)) {
        changed = true;
    }
    if (tick_tone_composer(app, now_ms)) {
        changed = true;
    }
    if (tick_received_tone(app, now_ms)) {
        changed = true;
    }
    if (tick_calculator(app, now_ms)) {
        changed = true;
    }
    if (tick_rotation(app, now_ms)) {
        changed = true;
    }
    if (tick_memory(app, now_ms)) {
        changed = true;
    }
    if (tick_react(app, now_ms)) {
        changed = true;
    }
    if (tick_logic(app, now_ms)) {
        changed = true;
    }
    if (tick_pacman(app, now_ms)) {
        changed = true;
    }
    if (tick_snake(app, now_ms)) {
        changed = true;
    }

    if (poll_app_status(app, now_ms)) {
        changed = true;
    }
    if (poll_battery(app, now_ms)) {
        changed = true;
    }

    if (ui_marquee_tick(now_ms)) {
        changed = true;
    }

    return changed;
}

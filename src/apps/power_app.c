#include "apps/power_app.h"

#include "apps/dialogs_app.h"
#include "apps/powerup_app.h"
#include "apps/profiles_app.h"
#include "apps/standby_app.h"
#include "services/board_diag_service.h"
#include "services/core1_services.h"
#include "services/input_keys.h"
#include "hal/rtc_alarm_hal.h"
#include "services/modem_service.h"
#include "services/strings.h"
#include "ui/status_chrome.h"
#include "ui/ui.h"
#include "storage/store_service.h"

#define POWER_MENU_COUNT 5u

/* Quick power/profile menu. Each entry carries its v6.00 string id (SID)
 * alongside the English literal, which stays BOTH the fallback and the record
 * of the ROM string. Dispatch is by index (apply_power_menu_item: 0 = switch
 * off, 1.. = profile), never by string, so the entry is localized only at
 * render time via L(). */
typedef struct {
    const char *label;
    uint16_t sid;
} power_menu_item_t;

/* Localize by SID with English fallback: sid 0 (no 1:1 / ambiguous match) or a
 * sid with no v6.00 record both return the supplied literal. */
static const char *L(uint16_t sid, const char *en) {
    if (sid != 0u) {
        const char *t = ts(sid);
        if (t != 0) {
            return t;
        }
    }
    return en;
}

static const power_menu_item_t POWER_MENU_ITEMS[POWER_MENU_COUNT] = {
    {"Switch off!", 0x1d4u}, /* SID 468 */
    {"Personal", 0x2cau},    /* SID 714 (profile-name block 0x2c9..0x2cc) */
    {"Silent", 0x2ccu},      /* SID 716 (profile-name block 0x2c9..0x2cc). The
                              * v6.00 UI trace for this menu resolves the
                              * otherwise-ambiguous "Silent" to this record. */
    {"Discreet", 0x2c9u},    /* SID 713 (profile-name block) */
    {"Loud", 0x2cbu},        /* SID 715 (profile-name block) */
};

static bool power_can_use_quick_menu(const app_t *app);
static void open_power_menu(app_t *app);
static void step_power_menu(app_t *app);
static void apply_power_menu_item(app_t *app, uint32_t now);
static uint8_t power_menu_start(uint8_t selected);
static void draw_power_menu_scrollbar(framebuffer_t *fb, uint8_t selected);

bool handle_power_key(app_t *app, const input_event_t *event) {
    if (event->code != KEY_POWER) {
        return false;
    }

    uint32_t now = event->when_ms;
    if (event->type == EVENT_KEY_DOWN) {
        if (app->route == APP_ROUTE_POWER_OFF) {
            return true;
        }
        if (app->route == APP_ROUTE_POWERUP) {
            return true;
        }
        if (app->keyguard_locked) {
            handle_keyguard_key(app, KEY_POWER, now);
            return true;
        }
        if (power_can_use_quick_menu(app)) {
            if (app->route == APP_ROUTE_POWER_MENU) {
                step_power_menu(app);
            } else {
                open_power_menu(app);
            }
            return true;
        }
        return true;
    }

    if (event->type == EVENT_KEY_HOLD) {
        if (app->route == APP_ROUTE_POWER_OFF) {
            (void)power_on(app, now);
            return true;
        }
        if (app->route == APP_ROUTE_POWERUP) {
            return true;
        }
        if (app->keyguard_locked) {
            handle_keyguard_key(app, KEY_POWER, now);
            return true;
        }
        power_off(app, now);
        return true;
    }

    return true;
}

bool handle_power_menu_key(app_t *app, uint16_t key, uint32_t now) {
    if (key == KEY_UP) {
        app->power_menu_selected = app->power_menu_selected == 0u
            ? (uint8_t)(POWER_MENU_COUNT - 1u)
            : (uint8_t)(app->power_menu_selected - 1u);
        app->dirty = true;
        return true;
    }
    if (key == KEY_DOWN) {
        step_power_menu(app);
        return true;
    }
    if (key == KEY_C) {
        app->route = APP_ROUTE_STANDBY;
        app->dirty = true;
        return true;
    }
    if (key == KEY_NAVI) {
        apply_power_menu_item(app, now);
        return true;
    }
    return true;
}

bool tick_power_off(app_t *app) {
    if (app->route != APP_ROUTE_POWER_OFF) {
        return false;
    }
    if (app->power_off_failed && modem_service_is_powered_off()) {
        /* A terminal fault keeps sampling the already-authorized PWRMON-low
         * condition. If that late evidence finally arrives, retire the warning
         * and return to the ordinary dark soft-off screen instead of leaving
         * the backlight latched on after the rail was safely released. */
        app->power_off_failed = false;
        app->backlight_force_active = true;
        app->backlight_force_on = false;
        app->dirty = true;
        return true;
    }
    if (!modem_service_take_power_off_failure()) {
        return false;
    }
    /* Every safe module-off mechanism was exhausted while PWRMON stayed live.
     * Keep the rail owned, wake only the LCD/backlight, and make that expensive
     * fail-closed state impossible to confuse with a successful switch-off.
     * A normal long power-key press remains the recovery action. */
    app->power_off_failed = true;
    app->backlight_force_active = true;
    app->backlight_force_on = true;
    app->dirty = true;
    return true;
}

void power_off(app_t *app, uint32_t now) {
    (void)now;
    /* Net Monitor can drive BQ25171 /CE high for a bench test. Restore the
     * production fail-enabled state before entering soft-off: with Rev B2 R77
     * DNP, retaining that override can prevent a flat pack from accepting a
     * charger. A failed write remains visible in diagnostics; TCA re-init
     * retains the charger arbiter's most recent target. */
    (void)board_diag_restore_charger_default();
    core1_post_command(CORE1_CMD_AUDIO_STOP, 0u);
    /* Stop the voice bridge NOW if a call was up (power-off-during-call): the
     * call-state follower only reacts when modem_enter_off() resets the status,
     * which the CPWROFF discharge window can defer by seconds; the BCLK-loss
     * fallback is the backstop but this is immediate. Idempotent when idle. */
    core1_post_command(CORE1_CMD_AUDIO_BRIDGE_STOP, 0u);
    modem_service_power_off();
    /* Analog codec off for the "off" idle (bias/drivers are a steady multi-mA
     * drain). power_on() and the alarm-while-off wake re-init the codec. */
    core1_services_codec_standby();

    app->route = APP_ROUTE_POWER_OFF;
    app->power_off_failed = false;
    app->power_menu_selected = 0u;
    app->input_len = 0u;
    app->input_text[0] = '\0';
    app->standby_message[0] = '\0';
    app->standby_message_until_ms = 0u;
    app->unlock_armed = false;
    app->unlock_armed_until_ms = 0u;
    app->menu_keyguard_armed = false;
    /* If the alarm is RINGING at power-off (the user dismissing it, or a
     * battery-empty countdown firing mid-ring), clear the RTC fire latch so the
     * POWER_OFF poll (poll_clock_alarm) doesn't immediately misread it as an
     * alarm-while-off wake and reboot straight back into the ring -- a
     * power-off/power-up loop that runs an empty pack to brownout. A PENDING
     * SNOOZE reads alarm_event false (it armed the snooze timer instead), so the
     * auto-snooze-then-power-off-wake flow is left intact to wake at its time. */
    if (rtc_alarm_hal_alarm_event_pending()) {
        rtc_alarm_hal_clear_alarm_event();
    }
    app->clock_alarm_mode = 0u;
    app->clock_alarm_power_off_wake = false;
    app->backlight_force_active = true;
    app->backlight_force_on = false;
    app->dirty = true;
}

bool power_off_display_should_sleep(const app_t *app) {
    return app != NULL && app->route == APP_ROUTE_POWER_OFF &&
           !app->battery_charge_active && !app->power_off_failed;
}

void render_power_off(const app_t *app, framebuffer_t *fb) {
    fb_clear(fb, false);
    if (app->power_off_failed) {
        draw_text_block(fb, asset_font(FONT_FS2),
                        L(0x1f4u, "Error in\nconnection"),
                        6, 8, 72, 9, 3u);
    }
    /* Powered-off charging shows only the right-side animated battery bars.
     * Physical charger presence outlives the charging session: once FULL is
     * confirmed, charge_active clears and the LCD returns to power-down rather
     * than retaining a static full icon until unplug. */
    if (app->battery_charge_active) {
        draw_battery(fb, app->battery_anim_level);
    }
}

void render_power_menu(const app_t *app, framebuffer_t *fb) {
    fb_clear(fb, false);
    uint8_t selected = app->power_menu_selected;
    if (selected >= POWER_MENU_COUNT) {
        selected = 0u;
    }
    uint8_t start = power_menu_start(selected);
    const font_t *font = asset_font(FONT_FS2);
    for (uint8_t row = 0; row < 3u && start + row < POWER_MENU_COUNT; row++) {
        uint8_t index = (uint8_t)(start + row);
        int y = 8 + row * 10;
        bool selected_row = index == selected;
        if (selected_row) {
            fb_fill_rect(fb, 0, y, 80, 10, true);
        }
        fb_text(fb, font, L(POWER_MENU_ITEMS[index].sid, POWER_MENU_ITEMS[index].label),
                2, y + 1, !selected_row, 78);
    }
    draw_power_menu_scrollbar(fb, selected);
    draw_softkey(fb, "OK");
}

static bool power_can_use_quick_menu(const app_t *app) {
    return app->route == APP_ROUTE_STANDBY || app->route == APP_ROUTE_POWER_MENU;
}

bool power_on(app_t *app, uint32_t now) {
    if (app->route != APP_ROUTE_POWER_OFF) {
        return false;
    }

    /* Power-on voltage gate derived from v6.00 EM (ROM 0x27d5fc): the single
     * 2100 mV floor and silent refusal are original behavior. Rev B2 feeds that
     * decision from its rolling bounded LTC qualification rather than one
     * filtered value; a charger remains the recovery exception. */
    if (!board_diag_battery_power_on_allowed()) {
        /* Silent refuse: stay in soft-off (route is already POWER_OFF). */
        return false;
    }
    app->backlight_force_active = false;
    app->backlight_force_on = false;
    app->power_off_failed = false;
    if (store_service_contact_service_required()) {
        enter_contact_service(app, now);
        return true;
    }
    modem_service_power_on();
    /* Codec was put in power-off standby; full re-init (blocking ~500 ms depop
     * ramp -- fine here, the powerup animation follows). The service wrapper
     * establishes MCLK before that MCLK-counted ramp and reconciles the idle
     * gate left behind by soft-off. */
    core1_services_codec_init();
    start_powerup(app, now);
    return true;
}

static void open_power_menu(app_t *app) {
    app->route = APP_ROUTE_POWER_MENU;
    app->power_menu_selected = 0u;
    app->input_len = 0u;
    app->input_text[0] = '\0';
    app->standby_message[0] = '\0';
    app->dirty = true;
}

static void step_power_menu(app_t *app) {
    app->power_menu_selected = (uint8_t)((app->power_menu_selected + 1u) % POWER_MENU_COUNT);
    app->dirty = true;
}

static void apply_power_menu_item(app_t *app, uint32_t now) {
    uint8_t selected = app->power_menu_selected;
    if (selected >= POWER_MENU_COUNT) {
        selected = 0u;
    }
    if (selected == 0u) {
        power_off(app, now);
        return;
    }

    uint8_t profile_index = (uint8_t)(selected - 1u);
    profile_activate(profile_index);
    app->route = APP_ROUTE_STANDBY;
    open_display(app,
                 2u,
                 "Selected",
                 "profile:",
                 profile_label(profile_index),
                 APP_ROUTE_STANDBY,
                 now);
}

static uint8_t power_menu_start(uint8_t selected) {
    if (POWER_MENU_COUNT <= 3u || selected < 3u) {
        return 0u;
    }
    uint8_t max_start = POWER_MENU_COUNT - 3u;
    uint8_t start = (uint8_t)(selected - 2u);
    return start > max_start ? max_start : start;
}

static void draw_power_menu_scrollbar(framebuffer_t *fb, uint8_t selected) {
    fb_vline(fb, 81, 7, 30, true);
    int max_offset = 23;
    int offset = selected >= POWER_MENU_COUNT - 1u
        ? max_offset
        : (int)(selected * max_offset) / (int)(POWER_MENU_COUNT - 1u);
    fb_bitmap(fb, 263u, 81, 7 + offset, true, false);
}

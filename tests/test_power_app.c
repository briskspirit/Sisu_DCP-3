#include <stdbool.h>
#include <stdio.h>

#include "apps/power_app.h"

static int s_failures;
static bool s_failure_pending;
static bool s_modem_powered_off;
static uint32_t s_failure_reads;
static unsigned s_battery_draws;
static uint8_t s_last_battery_level;

static void check(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        s_failures++;
    }
}

bool modem_service_take_power_off_failure(void) {
    s_failure_reads++;
    bool pending = s_failure_pending;
    s_failure_pending = false;
    return pending;
}

bool modem_service_is_powered_off(void) {
    return s_modem_powered_off;
}

void fb_clear(framebuffer_t *fb, bool color) {
    (void)fb;
    (void)color;
}

void draw_battery(framebuffer_t *fb, uint8_t level) {
    (void)fb;
    s_battery_draws++;
    s_last_battery_level = level;
}

const font_t *asset_font(font_id_t font_id) {
    (void)font_id;
    return NULL;
}

const char *ts(uint16_t sid) {
    (void)sid;
    return NULL;
}

void draw_text_block(framebuffer_t *fb,
                     const font_t *font,
                     const char *text,
                     int x,
                     int y,
                     int width,
                     int pitch,
                     uint8_t max_lines) {
    (void)fb;
    (void)font;
    (void)text;
    (void)x;
    (void)y;
    (void)width;
    (void)pitch;
    (void)max_lines;
}

int main(void) {
    app_t app = {0};
    app.route = APP_ROUTE_POWER_OFF;

    check(power_off_display_should_sleep(&app),
          "quiet soft-off powers down the LCD");
    app.battery_charger_connected = true;
    check(power_off_display_should_sleep(&app),
          "physical charger presence alone cannot retain a full battery icon");
    app.battery_charge_active = true;
    check(!power_off_display_should_sleep(&app),
          "an active powered-off charging session owns the LCD");
    app.battery_charge_active = false;
    app.power_off_failed = true;
    check(!power_off_display_should_sleep(&app),
          "a terminal shutdown error remains visible in soft-off");
    app.power_off_failed = false;
    app.route = APP_ROUTE_STANDBY;
    check(!power_off_display_should_sleep(&app),
          "powered-on routes never enter the power-off LCD policy");
    app.route = APP_ROUTE_POWER_OFF;
    app.battery_charger_connected = false;

    framebuffer_t fb = {0};
    app.battery_charger_connected = true;
    app.battery_charge_active = false;
    render_power_off(&app, &fb);
    check(s_battery_draws == 0u,
          "completed powered-off charge renders no static full battery icon");
    app.battery_charge_active = true;
    app.battery_anim_level = 3u;
    render_power_off(&app, &fb);
    check(s_battery_draws == 1u && s_last_battery_level == 3u,
          "active powered-off charge renders the current animation frame");
    app.battery_charge_active = false;
    app.battery_charger_connected = false;

    check(!tick_power_off(&app) && !app.power_off_failed,
          "quiet soft-off ignores an empty failure latch");

    s_failure_pending = true;
    check(tick_power_off(&app) && app.power_off_failed && app.dirty &&
              app.backlight_force_active && app.backlight_force_on,
          "terminal modem shutdown failure becomes a visible powered-off fault");
    check(!tick_power_off(&app),
          "the app-visible shutdown fault is consumed exactly once");

    s_modem_powered_off = true;
    app.dirty = false;
    check(tick_power_off(&app) && !app.power_off_failed && app.dirty &&
              app.backlight_force_active && !app.backlight_force_on,
          "late safe shutdown retires the warning and restores dark soft-off");
    check(!tick_power_off(&app),
          "late shutdown completion is also handled exactly once");
    s_modem_powered_off = false;

    app = (app_t){0};
    app.route = APP_ROUTE_STANDBY;
    s_failure_pending = true;
    uint32_t reads_before = s_failure_reads;
    check(!tick_power_off(&app) && s_failure_pending &&
              s_failure_reads == reads_before,
          "a non-power-off route neither steals nor renders the failure event");

    if (s_failures != 0) {
        fprintf(stderr, "%d power app test(s) failed\n", s_failures);
        return 1;
    }
    puts("power app tests passed");
    return 0;
}

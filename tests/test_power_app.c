#include <stdbool.h>
#include <stdio.h>

#include "apps/power_app.h"
#include "apps/powerup_app.h"
#include "hal/battery_gauge_logic.h"
#include "hal/power_button_hal.h"
#include "services/board_diag_service.h"
#include "services/input_keys.h"
#include "storage/store_service.h"

static int s_failures;
static bool s_failure_pending;
static bool s_modem_powered_off;
static uint32_t s_failure_reads;
static unsigned s_battery_draws;
static uint8_t s_last_battery_level;
static bool s_service_fault, s_power_allowed = true;
static bool s_use_qualifier;
static bool s_power_pending;
static battery_power_on_qualifier_t s_qualifier;
static unsigned s_modem_starts, s_codec_starts, s_success_starts, s_service_starts;
bool store_service_contact_service_required(void) { return s_service_fault; }
board_diag_power_on_status_t board_diag_battery_power_on_status(void) {
    if (s_use_qualifier) {
        if (!battery_power_on_qualifier_ready(&s_qualifier)) {
            return BOARD_DIAG_POWER_ON_PENDING;
        }
        return battery_power_on_qualifier_average_mv(&s_qualifier) >= 2100u
            ? BOARD_DIAG_POWER_ON_ALLOWED : BOARD_DIAG_POWER_ON_REFUSED;
    }
    if (s_power_pending) {
        return BOARD_DIAG_POWER_ON_PENDING;
    }
    return s_power_allowed
        ? BOARD_DIAG_POWER_ON_ALLOWED : BOARD_DIAG_POWER_ON_REFUSED;
}
void gpio_init(unsigned int gpio) { (void)gpio; }
void gpio_set_dir(unsigned int gpio, bool output) { (void)gpio; (void)output; }
void gpio_pull_up(unsigned int gpio) { (void)gpio; }
bool gpio_get(unsigned int gpio) { (void)gpio; return true; }
int32_t time_diff_ms(uint32_t a, uint32_t b) { return (int32_t)(a - b); }
void modem_service_power_on(void) { s_modem_starts++; }
void core1_services_codec_init(void) { s_codec_starts++; }
void start_powerup(app_t *app, uint32_t now) {
    (void)now; s_success_starts++; app->route = APP_ROUTE_POWERUP;
}
void enter_contact_service(app_t *app, uint32_t now) {
    (void)now; s_service_starts++; app->route = APP_ROUTE_CONTACT_SERVICE;
}

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

static void test_held_wake_waits_for_battery_samples(uint32_t release_ms) {
    app_t app = {.route = APP_ROUTE_POWER_OFF, .backlight_force_active = true};
    power_button_t button;
    event_queue_t queue;
    power_button_init(&button);
    power_button_seed_held(&button, 0u);
    event_queue_init(&queue);
    battery_power_on_qualifier_init(&s_qualifier);
    s_use_qualifier = true;
    s_service_fault = false;
    unsigned starts = s_modem_starts;
    unsigned holds = 0u;

    /* The first LTC conversion settles; five later 250 ms conversions must
     * qualify the pack after the one-shot 1.2 s held-key event. */
    uint32_t sequence = 0u;
    for (uint32_t now = 200u; now <= 2500u; now += 10u) {
        if ((now - 200u) % 250u == 0u) {
            sequence++;
            battery_power_on_qualifier_observe(
                &s_qualifier, 0u, sequence, sequence > 1u, 2550u);
        }
        power_button_feed(&button, now < release_ms, &queue, now);
        input_event_t event;
        while (event_queue_pop(&queue, &event)) {
            if (event.type == EVENT_KEY_HOLD && event.code == KEY_POWER) {
                holds++;
                (void)power_on(&app, event.when_ms);
            }
        }
        (void)tick_power_off(&app, now);
        if (now < 1450u) {
            check(s_modem_starts == starts,
                  "wake does not start modem before battery qualification");
            check(app.backlight_force_active && !app.backlight_force_on &&
                      power_off_display_should_sleep(&app),
                  "wake keeps display and backlight asleep before battery qualification");
        }
    }
    if (release_ms > POWER_BUTTON_POWER_ON_HOLD_MS) {
        check(holds == 1u, "qualifying Power hold produces just one hold event");
        check(app.route == APP_ROUTE_POWERUP && s_modem_starts == starts + 1u,
              "held wake starts once when battery qualification completes without a second press");
        check(!app.backlight_force_active && !power_off_display_should_sleep(&app),
              "accepted wake releases the display and backlight");
    } else {
        check(holds == 0u && app.route == APP_ROUTE_POWER_OFF &&
                  !app.power_on_pending && s_modem_starts == starts,
              "short wake press never starts after battery samples become ready");
        check(app.backlight_force_active && !app.backlight_force_on &&
                  power_off_display_should_sleep(&app),
              "short wake remains dark even after samples become ready");
    }
    s_use_qualifier = false;
}

static void test_pending_power_on_is_bounded(void) {
    app_t app = {.route = APP_ROUTE_POWER_OFF};
    unsigned starts = s_modem_starts;
    s_power_pending = true;
    check(power_on(&app, 100u) && app.power_on_pending &&
              app.route == APP_ROUTE_POWER_OFF && s_modem_starts == starts,
          "pending admission accepts intent without starting hardware");
    uint32_t deadline = app.power_on_deadline_ms;
    check(power_on(&app, 200u) && app.power_on_deadline_ms == deadline,
          "repeated pending requests cannot extend the qualification deadline");
    check(!tick_power_off(&app, deadline - 1u) && app.power_on_pending,
          "request remains pending just before its deadline");
    check(!tick_power_off(&app, deadline) && !app.power_on_pending &&
              s_modem_starts == starts,
          "missing battery evidence expires without starting hardware");
    s_power_pending = false;
    s_power_allowed = true;
    (void)tick_power_off(&app, deadline + 10u);
    check(s_modem_starts == starts, "expired request cannot cause a late power-on");

    s_power_pending = true;
    check(power_on(&app, 5000u), "new explicit request can qualify again");
    s_power_pending = false;
    s_power_allowed = false;
    check(!tick_power_off(&app, 5100u) && !app.power_on_pending &&
              s_modem_starts == starts,
          "confirmed low or unusable battery cancels pending request immediately");
    s_power_allowed = true;
    (void)tick_power_off(&app, 5200u);
    check(s_modem_starts == starts, "later battery recovery cannot revive a refused request");

    s_power_pending = true;
    check(power_on(&app, UINT32_MAX - 2999u) && app.power_on_deadline_ms == 0u,
          "pending deadline may wrap to zero without becoming a sentinel");
    check(!tick_power_off(&app, UINT32_MAX) && app.power_on_pending,
          "wrapped deadline is still pending before expiry");
    check(!tick_power_off(&app, 0u) && !app.power_on_pending,
          "wrapped deadline expires at zero");

    check(power_on(&app, 100u), "route-change case begins pending");
    app.route = APP_ROUTE_CLOCK_ALARM;
    (void)tick_power_off(&app, 200u);
    check(!app.power_on_pending, "another route cancels deferred power-on intent");
    s_power_pending = false;
}

int main(void) {
    app_t app = {0};
    app.route = APP_ROUTE_POWER_OFF;
    s_service_fault = true;
    check(power_on(&app, 100u) && app.route == APP_ROUTE_CONTACT_SERVICE,
          "failed self-test enters service route at the power-on gate");
    check(s_service_starts == 1u && s_success_starts == 0u &&
          s_modem_starts == 0u && s_codec_starts == 0u,
          "known fatal fault never starts modem, codec or success animation");
    app.route = APP_ROUTE_POWER_OFF; s_power_allowed = false;
    check(!power_on(&app, 200u) && app.route == APP_ROUTE_POWER_OFF,
          "battery power-on floor still applies to service-fault startup");
    app.route = APP_ROUTE_POWER_OFF; s_power_allowed = true; s_service_fault = false;
    check(power_on(&app, 300u) && app.route == APP_ROUTE_POWERUP &&
          s_success_starts == 1u && s_modem_starts == 1u && s_codec_starts == 1u,
          "healthy storage retains normal power-on sequence");
    app = (app_t){0};
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

    check(!tick_power_off(&app, 0u) && !app.power_off_failed,
          "quiet soft-off ignores an empty failure latch");

    s_failure_pending = true;
    check(tick_power_off(&app, 0u) && app.power_off_failed && app.dirty &&
              app.backlight_force_active && app.backlight_force_on,
          "terminal modem shutdown failure becomes a visible powered-off fault");
    check(!tick_power_off(&app, 0u),
          "the app-visible shutdown fault is consumed exactly once");

    s_modem_powered_off = true;
    app.dirty = false;
    check(tick_power_off(&app, 0u) && !app.power_off_failed && app.dirty &&
              app.backlight_force_active && !app.backlight_force_on,
          "late safe shutdown retires the warning and restores dark soft-off");
    check(!tick_power_off(&app, 0u),
          "late shutdown completion is also handled exactly once");
    s_modem_powered_off = false;

    app = (app_t){0};
    app.route = APP_ROUTE_STANDBY;
    s_failure_pending = true;
    uint32_t reads_before = s_failure_reads;
    check(!tick_power_off(&app, 0u) && s_failure_pending &&
              s_failure_reads == reads_before,
          "a non-power-off route neither steals nor renders the failure event");

    s_failure_pending = false;
    test_held_wake_waits_for_battery_samples(UINT32_MAX);
    test_held_wake_waits_for_battery_samples(1210u);
    test_held_wake_waits_for_battery_samples(1000u);
    test_pending_power_on_is_bounded();

    if (s_failures != 0) {
        fprintf(stderr, "%d power app test(s) failed\n", s_failures);
        return 1;
    }
    puts("power app tests passed");
    return 0;
}

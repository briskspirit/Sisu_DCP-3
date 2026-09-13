/* App-level alarm characterization. This compiles the production Clock app
 * with dead stripping and drives only its public alarm surface. Phase 5A can
 * move that surface into a dedicated translation unit, but these route,
 * storage, RTC, audio, and power traces must remain unchanged. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "apps/clock_alarm_logic.h"
#include "apps/clock_app.h"
#include "apps/dialogs_app.h"
#include "apps/power_app.h"
#include "apps/powerup_app.h"
#include "apps/profiles_app.h"
#include "audio/audio_levels.h"
#include "hal/rtc_alarm_hal.h"
#include "services/core1_services.h"
#include "services/board_diag_service.h"
#include "services/input_keys.h"
#include "services/modem_service.h"
#include "services/strings.h"
#include "services/timebase.h"
#include "storage/store_service.h"
#include "ui/ui.h"

static int s_failures;

static void check(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        s_failures++;
    }
}

static void check_str(const char *got, const char *want, const char *message) {
    if (strcmp(got, want) != 0) {
        fprintf(stderr, "FAIL: %s (got \"%s\", want \"%s\")\n", message, got, want);
        s_failures++;
    }
}

typedef struct {
    core1_cmd_t cmd;
    uint16_t arg;
} command_post_t;

static command_post_t s_posts[32];
static size_t s_post_count;
static uint16_t s_settings[STORE_SETTING_COUNT];
static rtc_datetime_t s_datetime;
static bool s_alarm_due;
static bool s_event_time_valid;
static uint8_t s_event_hour;
static uint8_t s_event_minute;
static unsigned s_event_clears;
static unsigned s_snooze_calls;
static uint8_t s_snooze_minutes;
static unsigned s_daily_alarm_calls;
static uint8_t s_daily_alarm_hour;
static uint8_t s_daily_alarm_minute;
static bool s_daily_alarm_enabled;
static unsigned s_codec_init_calls;
static unsigned s_modem_power_on_calls;
static unsigned s_powerup_calls;
static unsigned s_power_off_calls;
static uint32_t s_power_transition_now;
static unsigned s_display_calls;
static uint16_t s_display_sid;
static app_route_t s_display_return_route;
static bool s_battery_power_on_allowed;

static void reset_fixture(void) {
    memset(s_posts, 0, sizeof(s_posts));
    s_post_count = 0u;
    memset(s_settings, 0, sizeof(s_settings));
    s_settings[STORE_SETTING_CLOCK_FORMAT_24H] = 1u;
    s_settings[STORE_SETTING_CLOCK_TIME_SET] = 1u;
    s_settings[STORE_SETTING_CLOCK_SHOW_STANDBY] = 1u;
    s_settings[STORE_SETTING_CLOCK_ALARM_ENABLED] = 1u;
    s_settings[STORE_SETTING_CLOCK_ALARM_HOUR] = 6u;
    s_settings[STORE_SETTING_CLOCK_ALARM_MINUTE] = 31u;
    s_datetime = (rtc_datetime_t){2026u, 8u, 19u, 6u, 31u, 0u};
    s_alarm_due = false;
    s_event_time_valid = false;
    s_event_hour = 0u;
    s_event_minute = 0u;
    s_event_clears = 0u;
    s_snooze_calls = 0u;
    s_snooze_minutes = 0u;
    s_daily_alarm_calls = 0u;
    s_daily_alarm_hour = 0u;
    s_daily_alarm_minute = 0u;
    s_daily_alarm_enabled = false;
    s_codec_init_calls = 0u;
    s_modem_power_on_calls = 0u;
    s_powerup_calls = 0u;
    s_power_off_calls = 0u;
    s_power_transition_now = 0u;
    s_display_calls = 0u;
    s_display_sid = 0u;
    s_display_return_route = APP_ROUTE_STANDBY;
    s_battery_power_on_allowed = true;
}

static void arm_due(uint8_t hour, uint8_t minute) {
    s_alarm_due = true;
    s_event_time_valid = true;
    s_event_hour = hour;
    s_event_minute = minute;
}

/* --------------------------------------------------------------- host seams */

void copy_text(char *dst, size_t cap, const char *src) {
    if (dst == NULL || cap == 0u) {
        return;
    }
    snprintf(dst, cap, "%s", src != NULL ? src : "");
}

void ui_format_named(char *dst, size_t cap, const char *tmpl, const char *value) {
    if (dst == NULL || cap == 0u) {
        return;
    }
    dst[0] = '\0';
    const char *token = strstr(tmpl != NULL ? tmpl : "", "%U");
    if (token == NULL) {
        copy_text(dst, cap, tmpl);
        return;
    }
    size_t prefix = (size_t)(token - tmpl);
    if (prefix >= cap) {
        prefix = cap - 1u;
    }
    memcpy(dst, tmpl, prefix);
    dst[prefix] = '\0';
    snprintf(dst + prefix, cap - prefix, "%s%s", value != NULL ? value : "", token + 2);
}

int32_t time_diff_ms(uint32_t a, uint32_t b) {
    return (int32_t)(a - b);
}

const char *ts(uint16_t sid) {
    (void)sid;
    return NULL;
}

const char *ts_or(uint16_t sid, const char *fallback) {
    (void)sid;
    return fallback;
}

store_status_t store_setting_get_u8(store_setting_key_t key, uint8_t *out_value) {
    *out_value = (uint8_t)s_settings[key];
    return STORE_STATUS_OK;
}

store_status_t store_setting_set_u8(store_setting_key_t key, uint8_t value) {
    s_settings[key] = value;
    return STORE_STATUS_OK;
}

store_status_t store_setting_get_u16(store_setting_key_t key, uint16_t *out_value) {
    *out_value = s_settings[key];
    return STORE_STATUS_OK;
}

store_status_t store_setting_set_u16(store_setting_key_t key, uint16_t value) {
    s_settings[key] = value;
    return STORE_STATUS_OK;
}

uint8_t profile_active_index(void) {
    return 2u;
}

uint8_t profile_get_tone_setting(uint8_t profile_index, profile_setting_kind_t kind) {
    check(profile_index == 2u, "alarm tone reads the active profile");
    return kind == PROFILE_SETTING_VIBRATING_ALERT ? 1u : 5u;
}

void core1_post_command(core1_cmd_t cmd, uint16_t arg) {
    if (s_post_count < sizeof(s_posts) / sizeof(s_posts[0])) {
        s_posts[s_post_count] = (command_post_t){cmd, arg};
    }
    s_post_count++;
}

bool core1_services_codec_init(void) {
    s_codec_init_calls++;
    return true;
}

bool rtc_alarm_hal_alarm_due(void) {
    return s_alarm_due;
}

bool rtc_alarm_hal_alarm_event_time(uint8_t *out_hour, uint8_t *out_minute) {
    if (!s_event_time_valid) {
        return false;
    }
    *out_hour = s_event_hour;
    *out_minute = s_event_minute;
    return true;
}

void rtc_alarm_hal_snooze_minutes(uint8_t minutes) {
    s_snooze_calls++;
    s_snooze_minutes = minutes;
    s_alarm_due = false;
    s_event_time_valid = false;
}

void rtc_alarm_hal_clear_alarm_event(void) {
    s_event_clears++;
    s_alarm_due = false;
    s_event_time_valid = false;
}

void rtc_alarm_hal_set_daily_alarm(uint8_t hour, uint8_t minute, bool enabled) {
    s_daily_alarm_calls++;
    s_daily_alarm_hour = hour;
    s_daily_alarm_minute = minute;
    s_daily_alarm_enabled = enabled;
    if (!enabled) {
        s_alarm_due = false;
        s_event_time_valid = false;
    }
}

void rtc_alarm_hal_get_datetime(rtc_datetime_t *datetime) {
    *datetime = s_datetime;
}

void modem_service_power_on(void) {
    s_modem_power_on_calls++;
}

bool board_diag_battery_power_on_allowed(void) {
    return s_battery_power_on_allowed;
}

void start_powerup_no_logo(app_t *app, uint32_t now_ms) {
    s_powerup_calls++;
    s_power_transition_now = now_ms;
    app->route = APP_ROUTE_POWERUP;
}

void power_off(app_t *app, uint32_t now) {
    s_power_off_calls++;
    s_power_transition_now = now;
    app->route = APP_ROUTE_POWER_OFF;
}

void open_display_sid(app_t *app,
                      uint8_t record_id,
                      uint16_t sid,
                      const char *fallback,
                      app_route_t return_route,
                      uint32_t now) {
    (void)app;
    (void)record_id;
    (void)fallback;
    (void)now;
    s_display_calls++;
    s_display_sid = sid;
    s_display_return_route = return_route;
}

/* --------------------------------------------------------------- scenarios */

static void expect_alarm_started(const app_t *app, uint32_t now, const char *text) {
    check(app->route == APP_ROUTE_CLOCK_ALARM, "fire owns the alarm route");
    check(app->clock_alarm_mode == CLOCK_ALARM_MODE_RINGING, "fire enters ringing mode");
    check_str(app->clock_alarm_text, text, "fire displays the RTC event time");
    check(app->clock_alarm_started_ms == now && app->clock_alarm_last_frame_ms == now &&
              app->clock_alarm_last_backlight_ms == now,
          "fire seeds all alarm epochs from the same tick");
    check(app->clock_alarm_frame_index == 0u, "fire starts at animation frame zero");
    check(app->backlight_force_active && app->backlight_force_on,
          "fire starts the forced backlight in its on phase");
    check(s_post_count >= 1u,
          "fire posts the alarm stream");
    check(s_posts[s_post_count - 1u].cmd == CORE1_CMD_AUDIO_SYSTEM_TONE_LOOP &&
              s_posts[s_post_count - 1u].arg ==
                  audio_arg_with_marker_vibra(12u, AUDIO_LEVEL_MAX, true),
          "fire atomically starts the alarm loop with profile-controlled vibra");
}

static void test_fire_snooze_refire_and_dismiss(void) {
    reset_fixture();
    app_t app;
    memset(&app, 0, sizeof(app));
    app.route = APP_ROUTE_STANDBY;
    arm_due(6u, 31u);

    check(poll_clock_alarm(&app, 1000u), "due alarm changes the app");
    expect_alarm_started(&app, 1000u, "Alarm!\n06:31");

    check(handle_clock_alarm_key(&app, KEY_NAVI, 1200u), "Snooze key is consumed");
    check(s_snooze_calls == 1u && s_snooze_minutes == 5u,
          "Snooze programs the traced five-minute RTC offset");
    check(app.route == APP_ROUTE_STANDBY && app.clock_alarm_mode == CLOCK_ALARM_MODE_SNOOZE_ACTIVE,
          "Snooze returns to standby with the persistent overlay active");
    check(!app.backlight_force_active && !app.backlight_force_on,
          "Snooze releases the alarm backlight force");
    check_str(app.clock_text, "06:31", "Snooze refreshes the standby clock");

    arm_due(6u, 36u);
    s_post_count = 0u;
    check(poll_clock_alarm(&app, 2000u), "snooze compare re-fires");
    expect_alarm_started(&app, 2000u, "Alarm!\n06:36");

    s_post_count = 0u;
    check(handle_clock_alarm_key(&app, KEY_C, 2200u), "C dismisses an on-state alarm");
    check(app.route == APP_ROUTE_STANDBY && app.clock_alarm_mode == CLOCK_ALARM_MODE_IDLE,
          "dismiss returns to ordinary standby");
    check(s_daily_alarm_calls == 1u && !s_daily_alarm_enabled &&
              s_daily_alarm_hour == 6u && s_daily_alarm_minute == 31u,
          "dismiss disables the one-shot while preserving its configured time");
    check(s_settings[STORE_SETTING_CLOCK_ALARM_ENABLED] == 0u && !app.clock_alarm_enabled,
          "dismiss clears persistent and app-local alarm-enabled state");
    check(s_post_count == 1u && s_posts[0].cmd == CORE1_CMD_AUDIO_STOP &&
              s_posts[0].arg == 0u,
          "dismiss atomically stops audio and its marker-vibra ownership");
}

static void test_auto_snooze_and_call_suppression(void) {
    reset_fixture();
    app_t app;
    memset(&app, 0, sizeof(app));
    app.route = APP_ROUTE_STANDBY;
    arm_due(7u, 0u);
    (void)poll_clock_alarm(&app, 10000u);

    (void)tick_clock_alarm(&app, 10000u + 61679u);
    check(s_snooze_calls == 0u && app.clock_alarm_mode == CLOCK_ALARM_MODE_RINGING,
          "runtime remains active before 61.68 s");
    check(tick_clock_alarm(&app, 10000u + 61680u), "runtime auto-snoozes at 61.68 s");
    check(s_snooze_calls == 1u && app.clock_alarm_mode == CLOCK_ALARM_MODE_SNOOZE_ACTIVE,
          "ignored alarm enters the same snooze state as the softkey");

    reset_fixture();
    memset(&app, 0, sizeof(app));
    app.route = APP_ROUTE_STANDBY;
    arm_due(8u, 15u);
    (void)poll_clock_alarm(&app, 20000u);
    size_t posts_after_fire = s_post_count;

    app.route = APP_ROUTE_INCOMING_CALL;
    check(poll_clock_alarm(&app, 20100u), "incoming call yields a ringing alarm once");
    check(app.route == APP_ROUTE_INCOMING_CALL &&
              app.clock_alarm_mode == CLOCK_ALARM_MODE_SUPPRESSED_BY_CALL,
          "call keeps its route while alarm becomes suppressed");
    check(!app.backlight_force_active && !app.backlight_force_on,
          "yield releases only the alarm display force");
    check(s_post_count == posts_after_fire,
          "yield posts no AUDIO_STOP that could kill the incoming ringtone");
    check(!poll_clock_alarm(&app, 20000u + 61679u),
          "suppressed alarm waits under a shorter call");
    check(poll_clock_alarm(&app, 20000u + 61680u),
          "suppressed alarm auto-snoozes under a long call");
    check(app.route == APP_ROUTE_INCOMING_CALL &&
              app.clock_alarm_mode == CLOCK_ALARM_MODE_SNOOZE_ACTIVE,
          "under-call auto-snooze preserves the call route");
}

static void test_power_off_wake_and_activate_prompt(void) {
    reset_fixture();
    app_t app;
    memset(&app, 0, sizeof(app));
    app.route = APP_ROUTE_POWER_OFF;
    arm_due(9u, 45u);

    check(poll_clock_alarm(&app, 30000u), "off-state alarm starts powerup");
    check(s_codec_init_calls == 1u && s_powerup_calls == 1u && s_power_transition_now == 30000u,
          "off-state alarm restores the codec then starts no-logo powerup");
    check(app.clock_alarm_power_off_wake && app.route == APP_ROUTE_POWERUP,
          "off-state evidence survives into powerup");

    /* Powerup completion returns to an idle surface while the RTC event remains
     * latched; the ordinary poll then opens the same alarm runtime. */
    app.route = APP_ROUTE_STANDBY;
    check(poll_clock_alarm(&app, 30500u), "latched off-state event rings after powerup");
    expect_alarm_started(&app, 30500u, "Alarm!\n09:45");
    check(app.clock_alarm_power_off_wake, "ringing screen retains off-wake chrome mode");

    s_post_count = 0u;
    check(handle_clock_alarm_key(&app, KEY_C, 31000u), "C on off-wake alarm is consumed");
    check(app.route == APP_ROUTE_CLOCK_ALARM &&
              app.clock_alarm_mode == CLOCK_ALARM_MODE_ACTIVATE_PROMPT,
          "C opens Activate phone for calls instead of standby");
    check(!app.clock_alarm_enabled && s_settings[STORE_SETTING_CLOCK_ALARM_ENABLED] == 0u,
          "off-wake C disables the one-shot before prompting");
    check(s_posts[s_post_count - 1u].cmd == CORE1_CMD_AUDIO_SYSTEM_TONE &&
              s_posts[s_post_count - 1u].arg == audio_arg(10u, AUDIO_LEVEL_MAX),
          "activate prompt plays warning tone 10 once");

    check(handle_clock_alarm_key(&app, KEY_NAVI, 31500u), "Yes accepts activate prompt");
    check(s_event_clears == 1u && s_modem_power_on_calls == 1u && s_powerup_calls == 2u,
          "accept clears the event, powers the modem, and resumes no-logo powerup");
    check(!app.clock_alarm_power_off_wake && app.clock_alarm_mode == CLOCK_ALARM_MODE_IDLE,
          "accept retires all off-wake alarm state");

    reset_fixture();
    memset(&app, 0, sizeof(app));
    app.route = APP_ROUTE_CLOCK_ALARM;
    app.clock_alarm_mode = CLOCK_ALARM_MODE_ACTIVATE_PROMPT;
    s_battery_power_on_allowed = false;
    check(handle_clock_alarm_key(&app, KEY_NAVI, 32000u),
          "low-battery activate choice is consumed");
    check(s_modem_power_on_calls == 0u && s_powerup_calls == 0u,
          "low-battery activate choice cannot launch the modem");
    check(s_power_off_calls == 1u && app.route == APP_ROUTE_POWER_OFF,
          "refused alarm activation returns to soft-off");
}

static void test_snooze_stop_note(void) {
    reset_fixture();
    app_t app;
    memset(&app, 0, sizeof(app));
    app.route = APP_ROUTE_STANDBY;
    app.clock_alarm_mode = CLOCK_ALARM_MODE_SNOOZE_ACTIVE;

    dismiss_clock_snooze(&app, 40000u);
    check(s_daily_alarm_calls == 1u && !s_daily_alarm_enabled,
          "Stop disables the snoozed one-shot");
    check(app.clock_alarm_mode == CLOCK_ALARM_MODE_IDLE && !app.clock_alarm_power_off_wake,
          "Stop retires the snooze state");
    check(s_display_calls == 1u && s_display_sid == 0x045u &&
              s_display_return_route == APP_ROUTE_STANDBY,
          "Stop opens the localized Snooze off note returning to standby");
}

int main(void) {
    test_fire_snooze_refire_and_dismiss();
    test_auto_snooze_and_call_suppression();
    test_power_off_wake_and_activate_prompt();
    test_snooze_stop_note();

    if (s_failures != 0) {
        fprintf(stderr, "%d failures\n", s_failures);
        return 1;
    }
    printf("clock alarm app tests passed\n");
    return 0;
}

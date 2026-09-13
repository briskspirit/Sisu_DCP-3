/* App-level characterization for Clock's time/date editor. The test drives
 * public routes and keys against the production implementation, pinning the
 * boot Time->Date chain and 12/24-hour commits across its Phase 5A boundary. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "apps/clock_app.h"
#include "apps/dialogs_app.h"
#include "apps/profiles_app.h"
#include "audio/audio_levels.h"
#include "hal/rtc_alarm_hal.h"
#include "services/core1_services.h"
#include "services/input_keys.h"
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

static uint16_t s_settings[STORE_SETTING_COUNT];
static rtc_datetime_t s_datetime;
static unsigned s_datetime_sets;
static rtc_datetime_t s_last_datetime_set;
static rtc_datetime_write_status_t s_datetime_request_status;
static rtc_datetime_write_status_t s_datetime_status;
static unsigned s_daily_alarm_sets;
static uint8_t s_daily_alarm_hour;
static uint8_t s_daily_alarm_minute;
static bool s_daily_alarm_enabled;
static unsigned s_display_calls;
static uint8_t s_display_record;
static uint16_t s_display_sid;
static app_route_t s_display_return_route;
static core1_cmd_t s_last_command;
static uint16_t s_last_command_arg;
static unsigned s_command_posts;

static void reset_fixture(void) {
    memset(s_settings, 0, sizeof(s_settings));
    s_settings[STORE_SETTING_CLOCK_TIME_SET] = 1u;
    s_settings[STORE_SETTING_CLOCK_SHOW_STANDBY] = 1u;
    s_settings[STORE_SETTING_CLOCK_FORMAT_24H] = 1u;
    s_settings[STORE_SETTING_CLOCK_ALARM_ENABLED] = 0u;
    s_settings[STORE_SETTING_CLOCK_ALARM_HOUR] = 7u;
    s_settings[STORE_SETTING_CLOCK_ALARM_MINUTE] = 30u;
    s_datetime = (rtc_datetime_t){2026u, 8u, 19u, 18u, 21u, 37u};
    s_datetime_sets = 0u;
    memset(&s_last_datetime_set, 0, sizeof(s_last_datetime_set));
    s_datetime_request_status = RTC_DATETIME_WRITE_COMMITTED;
    s_datetime_status = RTC_DATETIME_WRITE_IDLE;
    s_daily_alarm_sets = 0u;
    s_daily_alarm_hour = 0u;
    s_daily_alarm_minute = 0u;
    s_daily_alarm_enabled = false;
    s_display_calls = 0u;
    s_display_record = 0u;
    s_display_sid = 0u;
    s_display_return_route = APP_ROUTE_STANDBY;
    s_last_command = CORE1_CMD_NONE;
    s_last_command_arg = 0u;
    s_command_posts = 0u;
}

static void clear_display_trace(void) {
    s_display_calls = 0u;
    s_display_record = 0u;
    s_display_sid = 0u;
    s_display_return_route = APP_ROUTE_STANDBY;
}

/* --------------------------------------------------------------- host seams */

void copy_text(char *dst, size_t cap, const char *src) {
    if (dst == NULL || cap == 0u) {
        return;
    }
    snprintf(dst, cap, "%s", src != NULL ? src : "");
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

void rtc_alarm_hal_get_datetime(rtc_datetime_t *datetime) {
    *datetime = s_datetime;
}

rtc_datetime_write_status_t rtc_alarm_hal_set_datetime(const rtc_datetime_t *datetime) {
    s_last_datetime_set = *datetime;
    s_datetime_sets++;
    s_datetime_status = s_datetime_request_status;
    if (s_datetime_status == RTC_DATETIME_WRITE_COMMITTED) {
        s_datetime = *datetime;
    }
    return s_datetime_status;
}

rtc_datetime_write_status_t rtc_alarm_hal_datetime_write_status(void) {
    return s_datetime_status;
}

void rtc_alarm_hal_set_daily_alarm(uint8_t hour, uint8_t minute, bool enabled) {
    s_daily_alarm_hour = hour;
    s_daily_alarm_minute = minute;
    s_daily_alarm_enabled = enabled;
    s_daily_alarm_sets++;
}

uint8_t profile_active_index(void) {
    return 0u;
}

uint8_t profile_get_tone_setting(uint8_t profile_index, profile_setting_kind_t kind) {
    (void)profile_index;
    return kind == PROFILE_SETTING_WARNING_GAME_TONES ? 5u : 0u;
}

void core1_post_command(core1_cmd_t cmd, uint16_t arg) {
    s_last_command = cmd;
    s_last_command_arg = arg;
    s_command_posts++;
}

void open_display_sid(app_t *app,
                      uint8_t record_id,
                      uint16_t sid,
                      const char *fallback,
                      app_route_t return_route,
                      uint32_t now) {
    (void)app;
    (void)fallback;
    (void)now;
    s_display_calls++;
    s_display_record = record_id;
    s_display_sid = sid;
    s_display_return_route = return_route;
}

/* --------------------------------------------------------------- scenarios */

static void open_time_editor_from_menu(app_t *app, uint32_t now) {
    open_clock_root(app);
    app->clock_menu_selected = 1u; /* Clock settings. */
    (void)handle_clock_menu_key(app, KEY_NAVI, now);
    check(app->route == APP_ROUTE_CLOCK_MENU && app->clock_menu_kind == CLOCK_MENU_VISIBLE,
          "Clock settings opens the visible-clock submenu");
    app->clock_menu_selected = 1u; /* Set the time. */
    (void)handle_clock_menu_key(app, KEY_NAVI, now + 1u);
    check(app->route == APP_ROUTE_CLOCK_EDITOR && app->clock_editor_kind == CLOCK_EDITOR_TIME,
          "Set the time opens the time editor");
}

static void test_boot_setup_invalid_time_then_default_date(void) {
    reset_fixture();
    s_settings[STORE_SETTING_CLOCK_TIME_SET] = 0u;
    app_t app;
    memset(&app, 0, sizeof(app));

    check(start_clock_boot_setup_if_needed(&app, 1000u), "unset visible clock starts boot setup");
    check(app.clock_setup_pending && app.route == APP_ROUTE_CLOCK_EDITOR &&
              app.clock_editor_kind == CLOCK_EDITOR_TIME,
          "boot setup starts with the time editor");
    check_str(app.clock_editor_value, "hh:mm", "unset boot time uses the empty positional mask");
    check(app.clock_editor_cursor_digit == 0u && app.clock_editor_cursor_visible,
          "boot time editor starts at its first visible digit");

    (void)handle_clock_editor_key(&app, KEY_NAVI, 1010u);
    check(s_display_calls == 1u && s_display_sid == 0x08du &&
              s_display_return_route == APP_ROUTE_CLOCK_EDITOR,
          "accepting untouched time shows Invalid time and returns to the editor");
    check(s_datetime_sets == 0u, "invalid time never touches the RTC");

    clear_display_trace();
    (void)handle_clock_editor_key(&app, KEY_2, 1020u);
    (void)handle_clock_editor_key(&app, KEY_3, 1030u);
    (void)handle_clock_editor_key(&app, KEY_5, 1040u);
    (void)handle_clock_editor_key(&app, KEY_9, 1050u);
    check_str(app.clock_editor_value, "23:59", "four valid keys fill the 24-hour template");
    (void)handle_clock_editor_key(&app, KEY_NAVI, 1060u);
    check(s_datetime_sets == 1u && s_last_datetime_set.hour == 23u &&
              s_last_datetime_set.minute == 59u && s_last_datetime_set.second == 0u,
          "boot time commit updates the RTC but resets seconds");
    check(s_settings[STORE_SETTING_CLOCK_TIME_SET] == 0u,
          "time alone does not complete the mandatory boot chain");
    check(app.clock_setup_pending && app.clock_editor_kind == CLOCK_EDITOR_DATE &&
              app.route == APP_ROUTE_CLOCK_EDITOR,
          "time commit chains into the date editor");
    check_str(app.clock_editor_value, "dd.mm.yyyy", "boot date starts untouched");
    check(s_display_sid == 0x254u && s_display_record == 42u &&
              s_display_return_route == APP_ROUTE_CLOCK_EDITOR,
          "time commit shows Time is set over the date editor");

    clear_display_trace();
    (void)handle_clock_editor_key(&app, KEY_NAVI, 1070u);
    check(s_datetime_sets == 2u && s_last_datetime_set.day == 1u &&
              s_last_datetime_set.month == 1u && s_last_datetime_set.year == 1999u,
          "untouched boot date commits the v6.00 01.01.1999 default");
    check(s_settings[STORE_SETTING_CLOCK_TIME_SET] == 1u &&
              s_settings[STORE_SETTING_CLOCK_SHOW_STANDBY] == 1u,
          "date completion marks the whole clock set and visible");
    check(!app.clock_setup_pending && s_display_sid == 0x089u &&
              s_display_return_route == APP_ROUTE_STANDBY,
          "completed boot chain shows Date is set and returns to standby");
}

static void test_partial_setup_date_is_rejected(void) {
    reset_fixture();
    app_t app;
    memset(&app, 0, sizeof(app));
    app.route = APP_ROUTE_CLOCK_EDITOR;
    app.clock_editor_kind = CLOCK_EDITOR_DATE;
    app.clock_setup_pending = true;
    copy_text(app.clock_editor_value, sizeof(app.clock_editor_value), "01.mm.yyyy");

    (void)handle_clock_editor_key(&app, KEY_NAVI, 2000u);
    check(s_datetime_sets == 0u, "partial date never touches the RTC");
    check(app.clock_setup_pending && s_settings[STORE_SETTING_CLOCK_TIME_SET] == 1u,
          "partial date leaves setup pending and persistent state untouched");
    check(s_display_calls == 1u && s_display_sid == 0x08cu &&
              s_display_return_route == APP_ROUTE_CLOCK_EDITOR,
          "partial date shows Invalid date and remains in the editor");
}

static void test_rejected_digit_warning_and_cursor_timer(void) {
    reset_fixture();
    app_t app;
    memset(&app, 0, sizeof(app));
    open_time_editor_from_menu(&app, 3000u);
    copy_text(app.clock_editor_value, sizeof(app.clock_editor_value), "hh:mm");
    app.clock_editor_cursor_digit = 0u;

    (void)handle_clock_editor_key(&app, KEY_9, 3010u);
    check_str(app.clock_editor_value, "hh:mm", "impossible leading 9 is rejected");
    check(app.clock_editor_cursor_digit == 0u, "rejected digit does not advance the cursor");
    check(s_command_posts == 1u && s_last_command == CORE1_CMD_AUDIO_SYSTEM_TONE &&
              s_last_command_arg == audio_arg(5u, 5u),
          "rejected digit plays warning tone 5 at the profile level");

    app.clock_editor_cursor_visible = true;
    app.clock_editor_last_cursor_ms = 4000u;
    check(!tick_clock_editor(&app, 4511u), "cursor stays visible before 512 ms");
    check(tick_clock_editor(&app, 4512u) && !app.clock_editor_cursor_visible &&
              app.clock_editor_last_cursor_ms == 4512u,
          "cursor toggles and re-epochs at 512 ms");
}

static void test_24_hour_commit(void) {
    reset_fixture();
    app_t app;
    memset(&app, 0, sizeof(app));
    open_time_editor_from_menu(&app, 5000u);
    copy_text(app.clock_editor_value, sizeof(app.clock_editor_value), "04:07");
    clear_display_trace();

    (void)handle_clock_editor_key(&app, KEY_NAVI, 5010u);
    check(s_datetime_sets == 1u && s_last_datetime_set.hour == 4u &&
              s_last_datetime_set.minute == 7u,
          "24-hour commit writes the entered wall time directly");
    check(app.clock_menu_kind == CLOCK_MENU_ROOT && app.clock_menu_selected == 1u,
          "ordinary time commit restores the Clock settings root selection");
    check(s_display_sid == 0x254u && s_display_return_route == APP_ROUTE_CLOCK_MENU,
          "ordinary time commit shows Time is set and returns to the clock menu");
}

static void test_12_hour_pm_commit(void) {
    reset_fixture();
    s_settings[STORE_SETTING_CLOCK_FORMAT_24H] = 0u;
    s_datetime.hour = 14u; /* PM preselection source. */
    app_t app;
    memset(&app, 0, sizeof(app));
    open_time_editor_from_menu(&app, 6000u);
    check_str(app.clock_editor_value, "02:21", "12-hour editor converts the running 14:21 to 02:21");
    copy_text(app.clock_editor_value, sizeof(app.clock_editor_value), "02:45");

    (void)handle_clock_editor_key(&app, KEY_NAVI, 6010u);
    check(app.route == APP_ROUTE_CLOCK_MENU && app.clock_menu_kind == CLOCK_MENU_AM_PM &&
              app.clock_menu_selected == 1u,
          "12-hour time commit opens AM/PM with PM preselected");
    check_str(app.clock_pending_time, "02:45", "AM/PM handoff preserves the entered time");
    check(s_datetime_sets == 0u, "RTC write waits for AM/PM selection");

    clear_display_trace();
    (void)handle_clock_menu_key(&app, KEY_NAVI, 6020u); /* preselected pm */
    check(s_datetime_sets == 1u && s_last_datetime_set.hour == 14u &&
              s_last_datetime_set.minute == 45u,
          "PM selection converts 02:45 to 14:45");
    check(app.clock_pending_time[0] == '\0' && app.clock_pending_kind == 0u,
          "AM/PM selection retires its handoff state");
    check(s_display_sid == 0x254u && s_display_return_route == APP_ROUTE_CLOCK_MENU,
          "12-hour commit returns through the same Time is set note");
}

static void test_alarm_editor_commit(void) {
    reset_fixture();
    app_t app;
    memset(&app, 0, sizeof(app));
    open_clock_root(&app);
    app.clock_menu_selected = 0u;
    (void)handle_clock_menu_key(&app, KEY_NAVI, 7000u);
    check(app.clock_menu_kind == CLOCK_MENU_ALARM, "Alarm clock opens its On/Off menu");
    app.clock_menu_selected = 0u;
    (void)handle_clock_menu_key(&app, KEY_NAVI, 7010u);
    check(app.route == APP_ROUTE_CLOCK_EDITOR && app.clock_editor_kind == CLOCK_EDITOR_ALARM,
          "Alarm On opens the alarm-time editor");
    copy_text(app.clock_editor_value, sizeof(app.clock_editor_value), "06:24");
    (void)handle_clock_editor_key(&app, KEY_NAVI, 7020u);
    check(s_daily_alarm_sets == 1u && s_daily_alarm_hour == 6u &&
              s_daily_alarm_minute == 24u && s_daily_alarm_enabled,
          "alarm editor programs the one-shot RTC compare");
    check(s_settings[STORE_SETTING_CLOCK_ALARM_ENABLED] == 1u &&
              s_settings[STORE_SETTING_CLOCK_ALARM_HOUR] == 6u &&
              s_settings[STORE_SETTING_CLOCK_ALARM_MINUTE] == 24u,
          "alarm editor persists its enabled time");
    check(app.clock_alarm_enabled && s_display_sid == 0x042u,
          "alarm editor updates the standby icon mirror and shows Alarm on");
}

static void test_datetime_write_outcome_owns_success_ui(void) {
    reset_fixture();
    app_t app;
    memset(&app, 0, sizeof(app));
    open_time_editor_from_menu(&app, 8000u);
    copy_text(app.clock_editor_value, sizeof(app.clock_editor_value), "09:42");
    clear_display_trace();
    s_datetime_request_status = RTC_DATETIME_WRITE_PENDING;

    (void)handle_clock_editor_key(&app, KEY_NAVI, 8010u);
    check(app.clock_datetime_commit_pending && app.route == APP_ROUTE_CLOCK_EDITOR,
          "a deferred RTC write keeps ownership in the time editor");
    check(s_display_calls == 0u,
          "a deferred RTC write is not acknowledged as Time is set");
    check(s_settings[STORE_SETTING_CLOCK_HOUR] == 0u &&
              s_settings[STORE_SETTING_CLOCK_MINUTE] == 0u,
          "time persistence waits for hardware confirmation");
    check(s_datetime.hour == 18u && s_datetime.minute == 21u,
          "the committed RTC mirror stays unchanged while a write is pending");

    char before_key[sizeof(app.clock_editor_value)];
    copy_text(before_key, sizeof(before_key), app.clock_editor_value);
    (void)handle_clock_editor_key(&app, KEY_1, 8020u);
    check_str(app.clock_editor_value, before_key,
              "the editor cannot mutate underneath an in-flight RTC commit");

    s_datetime_status = RTC_DATETIME_WRITE_FAILED;
    check(poll_clock_datetime_commit(&app, 8030u),
          "terminal RTC failure publishes an editor state change");
    check(!app.clock_datetime_commit_pending,
          "terminal RTC failure retires the editor wait state");
    check(s_display_calls == 1u && s_display_sid == 0x3a9u &&
              s_display_return_route == APP_ROUTE_CLOCK_EDITOR,
          "terminal RTC failure shows the localized operation-not-done note");
    check(s_settings[STORE_SETTING_CLOCK_HOUR] == 0u &&
              s_settings[STORE_SETTING_CLOCK_MINUTE] == 0u,
          "failed RTC time remains uncommitted in storage");

    reset_fixture();
    memset(&app, 0, sizeof(app));
    open_time_editor_from_menu(&app, 8100u);
    copy_text(app.clock_editor_value, sizeof(app.clock_editor_value), "07:08");
    clear_display_trace();
    s_datetime_request_status = RTC_DATETIME_WRITE_PENDING;
    (void)handle_clock_editor_key(&app, KEY_NAVI, 8110u);
    s_datetime = s_last_datetime_set; /* model the HAL's confirmed mirror */
    s_datetime_status = RTC_DATETIME_WRITE_COMMITTED;
    check(poll_clock_datetime_commit(&app, 8120u),
          "a retried RTC write completion wakes the editor transaction");
    check(!app.clock_datetime_commit_pending &&
              s_settings[STORE_SETTING_CLOCK_HOUR] == 7u &&
              s_settings[STORE_SETTING_CLOCK_MINUTE] == 8u,
          "confirmed retry persists the exact committed wall time");
    check(s_display_sid == 0x254u && s_display_return_route == APP_ROUTE_CLOCK_MENU,
          "confirmed retry follows the normal Time is set route");

    reset_fixture();
    s_settings[STORE_SETTING_CLOCK_TIME_SET] = 0u;
    memset(&app, 0, sizeof(app));
    app.route = APP_ROUTE_CLOCK_EDITOR;
    app.clock_editor_kind = CLOCK_EDITOR_DATE;
    app.clock_setup_pending = true;
    copy_text(app.clock_editor_value, sizeof(app.clock_editor_value), "24.08.2026");
    s_datetime_request_status = RTC_DATETIME_WRITE_PENDING;
    (void)handle_clock_editor_key(&app, KEY_NAVI, 8200u);
    s_datetime_status = RTC_DATETIME_WRITE_FAILED;
    (void)poll_clock_datetime_commit(&app, 8210u);
    check(app.clock_setup_pending && s_settings[STORE_SETTING_CLOCK_TIME_SET] == 0u &&
              s_settings[STORE_SETTING_CLOCK_SHOW_STANDBY] == 1u,
          "failed setup-date write cannot promote an unset clock");
    check(s_settings[STORE_SETTING_CLOCK_DATE_DAY] == 0u &&
              s_settings[STORE_SETTING_CLOCK_DATE_MONTH] == 0u &&
              s_settings[STORE_SETTING_CLOCK_DATE_YEAR] == 0u,
          "failed setup-date write leaves all persisted date fields untouched");
    check(s_display_sid == 0x3a9u && s_display_return_route == APP_ROUTE_CLOCK_EDITOR,
          "failed setup-date write returns to the date editor");

    reset_fixture();
    memset(&app, 0, sizeof(app));
    open_time_editor_from_menu(&app, 8300u);
    copy_text(app.clock_editor_value, sizeof(app.clock_editor_value), "12:34");
    clear_display_trace();
    s_datetime_request_status = RTC_DATETIME_WRITE_PENDING;
    (void)handle_clock_editor_key(&app, KEY_NAVI, 8310u);
    app.route = APP_ROUTE_CALL; /* incoming-call/alarm ownership preemption */
    s_datetime = s_last_datetime_set;
    s_datetime_status = RTC_DATETIME_WRITE_COMMITTED;
    check(poll_clock_datetime_commit(&app, 8320u),
          "a preempted RTC transaction still retires its terminal result");
    check(app.route == APP_ROUTE_CALL && s_display_calls == 0u,
          "RTC completion never steals a preempting call/alarm route");
    check(s_settings[STORE_SETTING_CLOCK_HOUR] == 12u &&
              s_settings[STORE_SETTING_CLOCK_MINUTE] == 34u,
          "preempted confirmed write still synchronizes its persisted fallback");
}

int main(void) {
    test_boot_setup_invalid_time_then_default_date();
    test_partial_setup_date_is_rejected();
    test_rejected_digit_warning_and_cursor_timer();
    test_24_hour_commit();
    test_12_hour_pm_commit();
    test_alarm_editor_commit();
    test_datetime_write_outcome_owns_success_ui();

    if (s_failures != 0) {
        fprintf(stderr, "%d failures\n", s_failures);
        return 1;
    }
    printf("clock editor app tests passed\n");
    return 0;
}

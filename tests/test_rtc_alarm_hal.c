#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "hal/rtc_alarm_hal.h"
#include "hal/rv8803_hal.h"
#include "services/log.h"

static uint64_t s_now_ms;
static rtc_datetime_t s_chip_time;
static bool s_chip_available;
static bool s_chip_time_valid;
static bool s_alarm_flag;
static bool s_alarm_enabled;
static bool s_programmed_snooze;
static unsigned s_read_time_failures;
static unsigned s_read_time_calls;
static unsigned s_write_time_failures;
static unsigned s_write_time_calls;
static unsigned s_set_alarm_failures;
static unsigned s_set_alarm_calls;
static uint8_t s_programmed_hour;
static uint8_t s_programmed_minute;

static void assert_event_time(uint8_t hour, uint8_t minute) {
    uint8_t got_hour = 0xffu;
    uint8_t got_minute = 0xffu;
    assert(rtc_alarm_hal_alarm_event_time(&got_hour, &got_minute));
    assert(got_hour == hour);
    assert(got_minute == minute);
}

uint32_t time_ms(void) {
    return (uint32_t)s_now_ms;
}

uint64_t time_ms64(void) {
    return s_now_ms;
}

void log_write(log_level_t level, const char *tag, const char *fmt, ...) {
    (void)level;
    (void)tag;
    (void)fmt;
}

bool rv8803_hal_init(bool *out_time_valid) {
    if (out_time_valid != NULL) {
        *out_time_valid = s_chip_available && s_chip_time_valid;
    }
    return s_chip_available;
}

bool rv8803_hal_read_time_checked(rtc_datetime_t *datetime, bool *out_time_valid) {
    s_read_time_calls++;
    if (!s_chip_available || datetime == NULL || out_time_valid == NULL) {
        return false;
    }
    if (s_read_time_failures != 0u) {
        s_read_time_failures--;
        return false;
    }
    *out_time_valid = s_chip_time_valid;
    if (!s_chip_time_valid) {
        return true;
    }
    *datetime = s_chip_time;
    return true;
}

bool rv8803_hal_read_time(rtc_datetime_t *datetime) {
    bool valid = false;
    return rv8803_hal_read_time_checked(datetime, &valid) && valid;
}

bool rv8803_hal_write_time(const rtc_datetime_t *datetime) {
    s_write_time_calls++;
    if (!s_chip_available || datetime == NULL) {
        return false;
    }
    if (s_write_time_failures != 0u) {
        s_write_time_failures--;
        return false;
    }
    s_chip_time = *datetime;
    s_chip_time_valid = true;
    return true;
}

bool rv8803_hal_set_alarm(uint8_t hour, uint8_t minute) {
    s_set_alarm_calls++;
    if (!s_chip_available || s_set_alarm_failures != 0u) {
        if (s_set_alarm_failures != 0u) {
            s_set_alarm_failures--;
        }
        return false;
    }
    s_programmed_hour = hour;
    s_programmed_minute = minute;
    s_programmed_snooze = false;
    s_alarm_enabled = true;
    s_alarm_flag = false;
    return true;
}

bool rv8803_hal_set_snooze_alarm(uint8_t hour, uint8_t minute) {
    s_set_alarm_calls++;
    if (!s_chip_available || s_set_alarm_failures != 0u) {
        if (s_set_alarm_failures != 0u) {
            s_set_alarm_failures--;
        }
        return false;
    }
    s_programmed_hour = hour;
    s_programmed_minute = minute;
    s_programmed_snooze = true;
    s_alarm_enabled = true;
    s_alarm_flag = false;
    return true;
}

bool rv8803_hal_read_alarm(rv8803_alarm_state_t *out) {
    if (!s_chip_available || out == NULL) {
        return false;
    }
    *out = (rv8803_alarm_state_t){
        .hour = s_programmed_hour,
        .minute = s_programmed_minute,
        .enabled = s_alarm_enabled,
        .snooze_marker = s_programmed_snooze,
    };
    return true;
}

bool rv8803_hal_disable_alarm(void) {
    if (!s_chip_available) {
        return false;
    }
    s_alarm_enabled = false;
    s_alarm_flag = false;
    return true;
}

bool rv8803_hal_poll_alarm_flag(bool *out_fired) {
    if (!s_chip_available || out_fired == NULL) {
        return false;
    }
    *out_fired = s_alarm_flag;
    return true;
}

bool rv8803_hal_clear_alarm_flag(void) {
    if (!s_chip_available) {
        return false;
    }
    s_alarm_flag = false;
    return true;
}

static void reset_at(uint8_t hour, uint8_t minute, uint8_t second) {
    s_now_ms = 0u;
    s_chip_time = (rtc_datetime_t){
        .year = 2026u,
        .month = 8u,
        .day = 13u,
        .hour = hour,
        .minute = minute,
        .second = second,
    };
    s_chip_available = true;
    s_chip_time_valid = true;
    s_alarm_flag = false;
    s_alarm_enabled = false;
    s_programmed_snooze = false;
    s_read_time_failures = 0u;
    s_read_time_calls = 0u;
    s_write_time_failures = 0u;
    s_write_time_calls = 0u;
    s_set_alarm_failures = 0u;
    s_set_alarm_calls = 0u;
    s_programmed_hour = 0u;
    s_programmed_minute = 0u;
    rtc_alarm_hal_init();
    rtc_alarm_hal_set_daily_alarm(6u, 31u, true);
    assert(rtc_alarm_hal_alarm_config_committed());
    assert(s_programmed_hour == 6u);
    assert(s_programmed_minute == 31u);
}

static void test_snooze_is_armed_in_hardware(void) {
    reset_at(6u, 31u, 20u);

    rtc_alarm_hal_force_alarm_event();
    rtc_alarm_hal_snooze_minutes(5u);

    assert(!rtc_alarm_hal_alarm_event_pending());
    assert(rtc_alarm_hal_alarm_config_committed());
    assert(s_programmed_hour == 6u);
    assert(s_programmed_minute == 37u);
    assert(s_programmed_snooze);
    assert(rtc_alarm_hal_snooze_active());
    uint8_t hour = 0u;
    uint8_t minute = 0u;
    assert(!rtc_alarm_hal_alarm_event_time(&hour, &minute));
}

static void test_daily_fire_carries_configured_time(void) {
    reset_at(6u, 31u, 20u);

    rtc_alarm_hal_force_alarm_event();

    assert_event_time(6u, 31u);
    rtc_alarm_hal_clear_alarm_event();
    uint8_t hour = 0u;
    uint8_t minute = 0u;
    assert(!rtc_alarm_hal_alarm_event_time(&hour, &minute));
    assert(!rtc_alarm_hal_alarm_event_time(NULL, &minute));
    assert(!rtc_alarm_hal_alarm_event_time(&hour, NULL));
}

static void test_nokia_epoch_is_live_and_rolls_into_2000(void) {
    reset_at(6u, 31u, 20u);
    rtc_datetime_t epoch_end = {
        .year = 1999u,
        .month = 12u,
        .day = 31u,
        .hour = 23u,
        .minute = 59u,
        .second = 59u,
    };

    assert(rtc_alarm_hal_set_datetime(&epoch_end) == RTC_DATETIME_WRITE_COMMITTED);
    assert(s_chip_time.year == 1999u);
    assert(s_chip_time.month == 12u && s_chip_time.day == 31u);

    rtc_datetime_t got;
    rtc_alarm_hal_get_datetime(&got);
    assert(got.year == 1999u && got.month == 12u && got.day == 31u);

    s_now_ms = 1000u;
    rtc_alarm_hal_get_datetime(&got);
    assert(got.year == 2000u && got.month == 1u && got.day == 1u);
    assert(got.hour == 0u && got.minute == 0u && got.second == 0u);
}

static void test_datetime_write_retries_without_publishing_old_hardware(void) {
    reset_at(6u, 31u, 20u);
    rtc_datetime_t desired = {
        .year = 2027u,
        .month = 4u,
        .day = 5u,
        .hour = 9u,
        .minute = 42u,
        .second = 0u,
    };
    s_write_time_failures = 2u;

    assert(rtc_alarm_hal_set_datetime(&desired) == RTC_DATETIME_WRITE_PENDING);
    assert(rtc_alarm_hal_datetime_write_status() == RTC_DATETIME_WRITE_PENDING);
    assert(rtc_alarm_hal_datetime_write_pending());
    assert(s_write_time_calls == 1u);

    rtc_datetime_t got;
    rtc_alarm_hal_get_datetime(&got);
    assert(got.year == 2026u && got.month == 8u && got.day == 13u);
    assert(got.hour == 6u && got.minute == 31u && got.second == 20u);

    s_now_ms = 250u;
    assert(!rtc_alarm_hal_alarm_due());
    assert(s_write_time_calls == 2u);
    assert(rtc_alarm_hal_datetime_write_status() == RTC_DATETIME_WRITE_PENDING);
    rtc_alarm_hal_get_datetime(&got);
    assert(got.year == 2026u && got.hour == 6u && got.minute == 31u);

    s_now_ms = 500u;
    assert(!rtc_alarm_hal_alarm_due());
    assert(s_write_time_calls == 3u);
    assert(rtc_alarm_hal_datetime_write_status() == RTC_DATETIME_WRITE_COMMITTED);
    assert(!rtc_alarm_hal_datetime_write_pending());
    rtc_alarm_hal_get_datetime(&got);
    assert(got.year == desired.year && got.month == desired.month && got.day == desired.day);
    assert(got.hour == desired.hour && got.minute == desired.minute && got.second == desired.second);
}

static void test_datetime_write_failure_is_bounded_and_keeps_old_time(void) {
    reset_at(6u, 31u, 20u);
    rtc_datetime_t desired = {
        .year = 2028u,
        .month = 9u,
        .day = 10u,
        .hour = 11u,
        .minute = 12u,
        .second = 0u,
    };
    s_write_time_failures = 10u;

    assert(rtc_alarm_hal_set_datetime(&desired) == RTC_DATETIME_WRITE_PENDING);
    for (unsigned i = 1u; i <= 4u; i++) {
        s_now_ms = (uint64_t)i * 250u;
        assert(!rtc_alarm_hal_alarm_due());
    }
    assert(s_write_time_calls == 5u); /* immediate attempt plus four retries */
    assert(rtc_alarm_hal_datetime_write_status() == RTC_DATETIME_WRITE_FAILED);
    assert(!rtc_alarm_hal_datetime_write_pending());

    rtc_datetime_t got;
    rtc_alarm_hal_get_datetime(&got);
    assert(got.year == 2026u && got.month == 8u && got.day == 13u);
    assert(got.hour == 6u && got.minute == 31u && got.second == 21u);

    s_now_ms = 1250u;
    assert(!rtc_alarm_hal_alarm_due());
    assert(s_write_time_calls == 5u); /* no unbounded repair loop after failure */
}

static void test_snooze_target_wraps_across_midnight(void) {
    reset_at(23u, 58u, 30u);

    rtc_alarm_hal_force_alarm_event();
    rtc_alarm_hal_snooze_minutes(5u);

    assert(rtc_alarm_hal_alarm_config_committed());
    assert(s_programmed_hour == 0u);
    assert(s_programmed_minute == 4u);
    assert(s_programmed_snooze);
}

static void test_snooze_interrupt_restores_configured_alarm(void) {
    reset_at(6u, 31u, 20u);
    rtc_alarm_hal_force_alarm_event();
    rtc_alarm_hal_snooze_minutes(5u);
    assert(s_programmed_minute == 37u);

    /* The shared-IRQ service clears AF before publishing this evidence. */
    rtc_alarm_hal_capture_alarm_interrupt();

    assert(rtc_alarm_hal_alarm_event_pending());
    assert_event_time(6u, 37u);
    assert(rtc_alarm_hal_alarm_config_committed());
    assert(s_programmed_hour == 6u);
    assert(s_programmed_minute == 31u);
    assert(!s_programmed_snooze);
}

static void test_unrelated_reboot_preserves_snooze(void) {
    reset_at(6u, 31u, 20u);
    rtc_alarm_hal_force_alarm_event();
    rtc_alarm_hal_snooze_minutes(5u);
    assert(s_programmed_snooze);
    unsigned calls_before_reboot = s_set_alarm_calls;

    /* P1.7 wake from USB/button/another shared IRQ before the snooze target. */
    s_chip_time.minute = 32u;
    s_chip_time.second = 0u;
    rtc_alarm_hal_init();
    rtc_alarm_hal_set_daily_alarm(6u, 31u, true);

    assert(rtc_alarm_hal_snooze_active());
    assert(rtc_alarm_hal_alarm_config_committed());
    assert(s_set_alarm_calls == calls_before_reboot);
    assert(s_programmed_hour == 6u);
    assert(s_programmed_minute == 37u);
    assert(s_programmed_snooze);

    rtc_alarm_hal_capture_alarm_interrupt();
    assert(rtc_alarm_hal_alarm_event_pending());
    assert_event_time(6u, 37u);
    assert(s_programmed_hour == 6u);
    assert(s_programmed_minute == 31u);
    assert(!s_programmed_snooze);
}

static void test_boot_time_read_failure_still_preserves_tagged_snooze(void) {
    reset_at(6u, 31u, 20u);
    rtc_alarm_hal_force_alarm_event();
    rtc_alarm_hal_snooze_minutes(5u);
    unsigned calls_before_reboot = s_set_alarm_calls;

    s_chip_time.minute = 32u;
    s_chip_time.second = 0u;
    s_read_time_failures = 1u;
    rtc_alarm_hal_init();
    assert(!rtc_alarm_hal_time_valid());

    /* Mirrors load_clock_into_rtc(): seed a timebase, then apply the stored
     * stored one-shot config. Neither operation may overwrite the tagged live compare. */
    rtc_datetime_t fallback = s_chip_time;
    rtc_alarm_hal_set_datetime(&fallback);
    rtc_alarm_hal_set_daily_alarm(6u, 31u, true);

    assert(rtc_alarm_hal_snooze_active());
    assert(s_set_alarm_calls == calls_before_reboot);
    assert(s_programmed_hour == 6u);
    assert(s_programmed_minute == 37u);
    assert(s_programmed_snooze);
}

static void test_reboot_after_snooze_minute_fires_overdue_now(void) {
    reset_at(6u, 31u, 20u);
    rtc_alarm_hal_force_alarm_event();
    rtc_alarm_hal_snooze_minutes(5u);

    /* Model the narrow reset race after AF was cleared but before the tagged
     * compare was replaced. Recovery must not reinterpret 06:37 as tomorrow. */
    s_chip_time.hour = 6u;
    s_chip_time.minute = 38u;
    s_chip_time.second = 0u;
    s_alarm_flag = false;
    rtc_alarm_hal_init();
    rtc_alarm_hal_set_daily_alarm(6u, 31u, true);

    assert(rtc_alarm_hal_snooze_active());
    assert(rtc_alarm_hal_alarm_due());
    /* Recovery fires immediately at 06:38, but the user-visible event still
     * belongs to the temporary 06:37 compare. */
    assert_event_time(6u, 37u);
    assert(s_programmed_hour == 6u);
    assert(s_programmed_minute == 31u);
    assert(!s_programmed_snooze);
}

static void test_p1_7_reboot_preserves_snooze_wake(void) {
    reset_at(6u, 31u, 20u);
    rtc_alarm_hal_force_alarm_event();
    rtc_alarm_hal_snooze_minutes(5u);
    assert(s_programmed_hour == 6u);
    assert(s_programmed_minute == 37u);

    /* The external compare fires after P1.7 discarded every RP-side field. */
    s_chip_time.hour = 6u;
    s_chip_time.minute = 37u;
    s_chip_time.second = 0u;
    s_alarm_flag = true;
    rtc_alarm_hal_init();

    /* power_sleep_boot_probe() snapshots AF before app initialization restores
     * the persisted one-shot setting and clears the physical flag. */
    bool boot_af = false;
    assert(rv8803_hal_poll_alarm_flag(&boot_af));
    assert(boot_af);
    rtc_alarm_hal_set_daily_alarm(6u, 31u, true);
    assert(s_alarm_flag);
    assert(s_programmed_hour == 6u);
    assert(s_programmed_minute == 37u);
    assert(s_programmed_snooze);

    /* power_sleep_boot_finish() reinjects that preserved evidence. */
    rtc_alarm_hal_force_alarm_event();
    assert(rtc_alarm_hal_alarm_due());
    assert_event_time(6u, 37u);
    assert(!s_alarm_flag);
    assert(s_programmed_hour == 6u);
    assert(s_programmed_minute == 31u);
    assert(!s_programmed_snooze);
}

static void test_software_deadline_restores_configured_alarm(void) {
    reset_at(6u, 31u, 20u);
    rtc_alarm_hal_force_alarm_event();
    rtc_alarm_hal_snooze_minutes(5u);

    s_chip_time.hour = 6u;
    s_chip_time.minute = 37u;
    s_chip_time.second = 0u;
    s_now_ms = 340000u;
    assert(rtc_alarm_hal_alarm_due());
    assert_event_time(6u, 37u);
    assert(s_programmed_hour == 6u);
    assert(s_programmed_minute == 31u);
    assert(!s_programmed_snooze);
}

static void test_polled_snooze_flag_restores_configured_alarm(void) {
    reset_at(6u, 31u, 20u);
    rtc_alarm_hal_force_alarm_event();
    rtc_alarm_hal_snooze_minutes(5u);

    s_alarm_flag = true;
    s_chip_time.hour = 6u;
    s_chip_time.minute = 37u;
    s_chip_time.second = 0u;
    s_now_ms = 60000u;
    assert(rtc_alarm_hal_alarm_due());
    assert_event_time(6u, 37u);
    assert(!s_alarm_flag);
    assert(s_programmed_hour == 6u);
    assert(s_programmed_minute == 31u);
    assert(!s_programmed_snooze);
}

static void test_cancelling_snooze_restores_configured_alarm(void) {
    reset_at(6u, 31u, 20u);
    rtc_alarm_hal_force_alarm_event();
    rtc_alarm_hal_snooze_minutes(5u);
    assert(s_programmed_minute == 37u);

    rtc_alarm_hal_clear_alarm_event();

    assert(!rtc_alarm_hal_alarm_event_pending());
    assert(rtc_alarm_hal_alarm_config_committed());
    assert(s_programmed_hour == 6u);
    assert(s_programmed_minute == 31u);
    assert(!s_programmed_snooze);
}

static void test_failed_snooze_arm_blocks_sleep_until_retry(void) {
    reset_at(6u, 31u, 20u);
    rtc_alarm_hal_force_alarm_event();
    s_set_alarm_failures = 1u;

    rtc_alarm_hal_snooze_minutes(5u);
    assert(!rtc_alarm_hal_alarm_config_committed());

    unsigned calls_after_failed_arm = s_set_alarm_calls;
    s_chip_time.second = 21u;
    s_now_ms = 300u;
    assert(!rtc_alarm_hal_alarm_due());
    assert(s_set_alarm_calls == calls_after_failed_arm + 1u);
    assert(rtc_alarm_hal_alarm_config_committed());
    assert(s_programmed_hour == 6u);
    assert(s_programmed_minute == 37u);
}

static void test_final_dismissal_disarms_one_shot_alarm(void) {
    reset_at(6u, 31u, 20u);
    rtc_alarm_hal_force_alarm_event();
    rtc_alarm_hal_snooze_minutes(5u);

    s_chip_time.hour = 6u;
    s_chip_time.minute = 37u;
    s_chip_time.second = 0u;
    s_now_ms = 340000u;
    assert(rtc_alarm_hal_alarm_due());

    /* Mirrors clock_app's final Stop/C path. Restoring the configured HH:MM
     * after snooze re-fire must not turn the user-facing alarm into a recurring
     * alarm: the final dismissal disables the hardware compare. */
    rtc_alarm_hal_set_daily_alarm(6u, 31u, false);

    assert(!rtc_alarm_hal_alarm_enabled());
    assert(!rtc_alarm_hal_alarm_event_pending());
    assert(!rtc_alarm_hal_snooze_active());
    assert(rtc_alarm_hal_alarm_config_committed());
    assert(!s_alarm_enabled);

    /* Even stale AF evidence cannot re-publish a disabled one-shot alarm. */
    s_alarm_flag = true;
    s_chip_time.day++;
    s_chip_time.hour = 6u;
    s_chip_time.minute = 31u;
    s_now_ms += 300u;
    assert(!rtc_alarm_hal_alarm_due());
}

static void test_healthy_chip_uses_minute_maintenance_cadence(void) {
    reset_at(6u, 31u, 20u);
    unsigned reads_after_init = s_read_time_calls;

    s_now_ms = 59999u;
    assert(!rtc_alarm_hal_alarm_due());
    assert(s_read_time_calls == reads_after_init);

    s_now_ms = 60000u;
    assert(!rtc_alarm_hal_alarm_due());
    assert(s_read_time_calls == reads_after_init + 1u);
}

static void test_midrun_v2f_restores_hardware_from_software_mirror(void) {
    reset_at(6u, 31u, 20u);
    unsigned writes_before = s_write_time_calls;
    unsigned alarm_sets_before = s_set_alarm_calls;

    /* The RTC alone loses power while the RP and its monotonic mirror continue.
     * Its reset-looking calendar must never replace the good software time. */
    s_chip_time = (rtc_datetime_t){
        .year = 2000u,
        .month = 1u,
        .day = 1u,
        .hour = 0u,
        .minute = 0u,
        .second = 0u,
    };
    s_chip_time_valid = false;
    s_write_time_failures = 1u;
    s_now_ms = 60000u;
    assert(!rtc_alarm_hal_alarm_due());

    rtc_datetime_t got;
    rtc_alarm_hal_get_datetime(&got);
    assert(got.year == 2026u && got.month == 8u && got.day == 13u);
    assert(got.hour == 6u && got.minute == 32u && got.second == 20u);
    assert(!s_chip_time_valid);
    assert(s_write_time_calls == writes_before + 1u);
    assert(s_set_alarm_calls == alarm_sets_before);

    /* The exceptional recovery uses the existing 250 ms repair cadence. */
    s_now_ms = 60250u;
    assert(!rtc_alarm_hal_alarm_due());
    assert(s_chip_time_valid);
    assert(s_write_time_calls == writes_before + 2u);
    assert(s_chip_time.year == 2026u && s_chip_time.month == 8u &&
           s_chip_time.day == 13u);
    assert(s_chip_time.hour == 6u && s_chip_time.minute == 32u &&
           s_chip_time.second == 20u);
    assert(s_set_alarm_calls == alarm_sets_before + 1u);
    assert(s_programmed_hour == 6u && s_programmed_minute == 31u);

    rtc_alarm_hal_get_datetime(&got);
    assert(got.year == 2026u && got.hour == 6u && got.minute == 32u &&
           got.second == 20u);
}

static void test_next_minute_deadline_uses_cached_wall_clock(void) {
    reset_at(6u, 31u, 20u);
    unsigned reads_after_init = s_read_time_calls;

    assert(rtc_alarm_hal_ms_until_next_minute(s_now_ms) == 40000u);
    s_now_ms = 39999u;
    assert(rtc_alarm_hal_ms_until_next_minute(s_now_ms) == 1u);
    s_now_ms = 40000u;
    assert(rtc_alarm_hal_ms_until_next_minute(s_now_ms) == 60000u);
    s_now_ms = 100000u;
    assert(rtc_alarm_hal_ms_until_next_minute(s_now_ms) == 60000u);
    assert(s_read_time_calls == reads_after_init);
}

int main(void) {
    test_snooze_is_armed_in_hardware();
    test_daily_fire_carries_configured_time();
    test_nokia_epoch_is_live_and_rolls_into_2000();
    test_datetime_write_retries_without_publishing_old_hardware();
    test_datetime_write_failure_is_bounded_and_keeps_old_time();
    test_snooze_target_wraps_across_midnight();
    test_snooze_interrupt_restores_configured_alarm();
    test_unrelated_reboot_preserves_snooze();
    test_boot_time_read_failure_still_preserves_tagged_snooze();
    test_reboot_after_snooze_minute_fires_overdue_now();
    test_p1_7_reboot_preserves_snooze_wake();
    test_software_deadline_restores_configured_alarm();
    test_polled_snooze_flag_restores_configured_alarm();
    test_cancelling_snooze_restores_configured_alarm();
    test_failed_snooze_arm_blocks_sleep_until_retry();
    test_final_dismissal_disarms_one_shot_alarm();
    test_healthy_chip_uses_minute_maintenance_cadence();
    test_midrun_v2f_restores_hardware_from_software_mirror();
    test_next_minute_deadline_uses_cached_wall_clock();
    puts("RTC alarm snooze lifecycle tests passed");
    return 0;
}

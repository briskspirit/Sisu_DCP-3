#ifndef RTC_ALARM_HAL_H
#define RTC_ALARM_HAL_H

#include <stdbool.h>
#include <stdint.h>

#include "services/datetime_types.h"

typedef enum {
    RTC_DATETIME_WRITE_IDLE = 0,
    RTC_DATETIME_WRITE_PENDING,
    RTC_DATETIME_WRITE_COMMITTED,
    RTC_DATETIME_WRITE_FAILED,
} rtc_datetime_write_status_t;

void rtc_alarm_hal_init(void);
/* True if the RTC reported a trustworthy time at init (chip kept time across the
 * reboot). Callers use this to avoid overwriting the live chip time with a stale
 * stored value at boot. */
bool rtc_alarm_hal_time_valid(void);
bool rtc_alarm_hal_chip_available(void);
/* Submit a wall-clock write. COMMITTED means the RV-8803 acknowledged it;
 * PENDING means the HAL retained the exact intent for its bounded 250 ms retry
 * loop; FAILED includes invalid input. The committed software mirror is not
 * changed until the chip confirms the write. */
rtc_datetime_write_status_t rtc_alarm_hal_set_datetime(const rtc_datetime_t *datetime);
rtc_datetime_write_status_t rtc_alarm_hal_datetime_write_status(void);
bool rtc_alarm_hal_datetime_write_pending(void);
void rtc_alarm_hal_get_datetime(rtc_datetime_t *datetime);
/* Remaining time in the HAL's software wall-clock phase until the next whole
 * minute, in 1..60000 ms. No I2C transaction is performed. */
uint32_t rtc_alarm_hal_ms_until_next_minute(uint64_t now_ms);
/* Program the RV-8803's daily HH:MM compare primitive. The clock app gives it
 * one-shot semantics by persisting enabled=false and disabling AIE on the final
 * Stop/C dismissal. */
void rtc_alarm_hal_set_daily_alarm(uint8_t hour, uint8_t minute, bool enabled);
/* Temporarily replace the live RTC compare with the minute-rounded snooze
 * target. The configured one-shot alarm time remains unchanged and its HH:MM
 * compare is restored when snooze fires; final dismissal still disables AIE. */
void rtc_alarm_hal_snooze_minutes(uint8_t minutes);
/* True once the requested alarm/disable/snooze configuration has reached the
 * external RTC. Full-RAM-loss power-off must not begin while this is false. */
bool rtc_alarm_hal_alarm_config_committed(void);
/* True while a temporary snooze compare is active, including one recovered
 * from the RV-8803 GP0 marker after an RP reset. */
bool rtc_alarm_hal_snooze_active(void);
void rtc_alarm_hal_clear_alarm_event(void);
bool rtc_alarm_hal_alarm_due(void);
/* Raw "is the alarm currently ringing" latch (no chip service, no snooze
 * maturation). A pending snooze reads false. Used by power-off to dismiss a
 * ring without disturbing a pending snooze. */
bool rtc_alarm_hal_alarm_event_pending(void);
/* Effective HH:MM carried by the currently latched fire. For a normal fire this
 * is the configured alarm time; for a snooze fire it is the temporary snooze
 * compare, even after that compare has been restored to the configured alarm.
 * Returns false when no timed alarm event is latched. */
bool rtc_alarm_hal_alarm_event_time(uint8_t *out_hour, uint8_t *out_minute);
bool rtc_alarm_hal_alarm_enabled(void);
/* Publish AF evidence captured by the shared-IRQ service before it clears the
 * RV-8803 flag. A snooze AF restores the configured alarm-time compare before
 * the event is exposed to the app. */
void rtc_alarm_hal_capture_alarm_interrupt(void);
/* Dormant-wake capture: the RV-8803 AF was latched at power-up, but the boot
 * path's alarm re-arm (set_daily_alarm) clears AF before the normal poll can
 * see it. power_sleep captures AF pre-app_init and re-injects the fire here
 * post-app_init, feeding the exact same alarm_due() path as a live fire. */
void rtc_alarm_hal_force_alarm_event(void);

#endif

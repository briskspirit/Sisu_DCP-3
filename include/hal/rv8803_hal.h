#ifndef RV8803_HAL_H
#define RV8803_HAL_H

#include <stdbool.h>
#include <stdint.h>

#include "hal/rtc_alarm_hal.h" /* rtc_datetime_t */

/* Micro Crystal RV-8803-C7 RTC on the shared I2C bus (0x32). Provides the time/
 * date source and a hardware daily HH:MM alarm whose flag (AF) latches at the
 * minute and holds /INT (SYS_INT, GP42) until cleared -- the backend behind
 * rtc_alarm_hal. See docs and the RV-8803-C7 Application Manual. */

#define RV8803_I2C_ADDR 0x32u

typedef struct {
    bool transport_ok;
    bool did_work;
    bool more_work;
    bool alarm;
    bool timer;
    uint8_t flags;
} rv8803_irq_result_t;

typedef struct {
    uint8_t hour;
    uint8_t minute;
    bool enabled;
    bool snooze_marker;
} rv8803_alarm_state_t;

/* --- Pure register helpers (no I2C; unit-tested on host in test_rv8803_regs) --- */

uint8_t rv8803_bin_to_bcd(uint8_t bin);
uint8_t rv8803_bcd_to_bin(uint8_t bcd);

/* Pack a datetime into the 7 clock/calendar bytes (sec, min, hour, weekday,
 * date, month, year) in the RV-8803 layout: BCD, 24h; weekday is the one-hot
 * bit computed from the date (the chip uses it only for week-alarms, unused). */
void rv8803_encode_time(const rtc_datetime_t *dt, uint8_t regs[7]);

/* Decode the 7 clock/calendar bytes; false if any field is invalid BCD or out
 * of range. Weekday byte is ignored (we carry the date). */
bool rv8803_decode_time(const uint8_t regs[7], rtc_datetime_t *dt);

/* Pack a daily HH:MM alarm into the 3 alarm bytes: minute and hour enabled
 * (AE=0), week/date masked (AE_WD=1) -> "fires once per day at H:M". */
void rv8803_encode_alarm(uint8_t hour, uint8_t minute, uint8_t regs[3]);
/* Same daily HH:MM compare, tagged with the Hours Alarm register's documented
 * GP0 bit. GP0 does not participate in the compare, so the marker and target
 * are committed in the same I2C burst and survive an RP P1.7 reset together. */
void rv8803_encode_snooze_alarm(uint8_t hour, uint8_t minute, uint8_t regs[3]);
bool rv8803_decode_alarm(const uint8_t regs[3],
                         uint8_t *hour,
                         uint8_t *minute,
                         bool *snooze_marker);

/* Round epoch-seconds up to the next whole-minute boundary (unchanged if already
 * on one). Landing snooze re-fires on minute boundaries is what gives the
 * original 3210 its ~6-minute snooze spacing. */
uint32_t rv8803_round_up_to_minute(uint32_t epoch_seconds);

/* Pack a periodic-countdown period into the two Timer Counter bytes (0x0B low 8,
 * 0x0C high 4). The driver runs the counter from the 1 Hz source, so the period
 * is 1..4095 s (~68 min ceiling); false if out of range. */
bool rv8803_encode_timer_seconds(uint16_t seconds, uint8_t regs[2]);

/* --- Driver (core0, shared I2C). Every transfer is time-bounded; after a run
 * of failures the driver latches off until re-init (mirrors tca8418_hal). --- */

/* Bring up the chip. out_time_valid is set false if the chip reports lost data
 * (V2F), i.e. the stored time can't be trusted. Returns false on I2C failure. */
bool rv8803_hal_init(bool *out_time_valid);
/* Read the clock/calendar and V2F from one register burst. A true return means
 * the transfer succeeded; out_time_valid distinguishes a decoded trustworthy
 * calendar from a responding chip that reports lost data. `dt` is left
 * untouched when V2F is set. */
bool rv8803_hal_read_time_checked(rtc_datetime_t *dt, bool *out_time_valid);
/* Convenience wrapper: false for either transport failure or V2F-invalid time. */
bool rv8803_hal_read_time(rtc_datetime_t *dt);
bool rv8803_hal_write_time(const rtc_datetime_t *dt); /* also clears the V1F/V2F low-voltage flags */
bool rv8803_hal_set_alarm(uint8_t hour, uint8_t minute); /* daily HH:MM, AIE on, AF cleared */
bool rv8803_hal_set_snooze_alarm(uint8_t hour, uint8_t minute); /* tagged temporary HH:MM */
bool rv8803_hal_read_alarm(rv8803_alarm_state_t *out); /* compare + AIE + GP0 marker */
bool rv8803_hal_disable_alarm(void);                     /* AIE off, AF cleared */
bool rv8803_hal_poll_alarm_flag(bool *out_fired);        /* read AF (alarm latched?) */
bool rv8803_hal_clear_alarm_flag(void);                  /* clear AF -> releases /INT */
/* Capture and clear AF/TF in one read plus one constant write. The returned
 * flags preserve event evidence before the write-0-to-clear operation. */
void rv8803_hal_service_interrupt(rv8803_irq_result_t *out);

/* Periodic countdown timer support. Auto-reloads every
 * `seconds` (1..4095, 1 Hz source). At each expiry the TF flag latches until
 * software clears it, but -- UNLIKE the alarm's AF -- the physical /INT
 * (SYS_INT, GP42) output is transient: ~7.8 ms low, then it self-releases
 * (App Manual 4.5, tRTN1); clearing TF does not touch the pin. As a POWMAN
 * wake this still works: the falling-edge PWRUP latches its STATUS from the
 * pulse (sampled on LPOSC, ~30 us), and a pulse lost to the power-down
 * arming blind window is simply retried by the next period. Wake-cause
 * decode must therefore read TF over I2C, never sample the GP42 level.
 * Power-off no longer arms this timer; boot still stops it to retire a timer
 * inherited from an older firmware image. start_periodic_timer is therefore
 * intentionally unreferenced in firmware (tests/test_revb2_static_audit.sh
 * forbids power_sleep.c from arming it) -- kept for bench use. */
bool rv8803_hal_start_periodic_timer(uint16_t seconds); /* program + TIE/TE on, TF cleared */
bool rv8803_hal_stop_periodic_timer(void);              /* TE/TIE off, TF cleared */
bool rv8803_hal_clear_timer_flag(void);                 /* clear TF (the /INT pulse self-releases) */

#endif

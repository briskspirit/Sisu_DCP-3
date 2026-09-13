#include "hal/rtc_alarm_hal.h"

#include <string.h>

#include "hal/rv8803_hal.h"
#include "services/log.h"
#include "services/timebase.h"

/* The RV-8803-C7 is the time source and the alarm engine. This layer keeps a
 * cheap software mirror of the time (resynced from the chip on a cadence, so the
 * frequent get_datetime() redraw path needs no I2C) and drives the chip's
 * hardware HH:MM alarm: the alarm flag (AF) latches at HH:MM:00 and holds /INT
 * until cleared, so a busy/late poll cannot miss it -- there is no software
 * minute-equality compare and no catch-up heuristic to get wrong.
 * Snooze temporarily replaces the chip's live HH:MM compare with a
 * minute-rounded target, reproducing the original 3210's ~6-minute spacing.
 * The RP may lose all RAM in P1.7 while "off", so the external RTC must own
 * that wake. Snooze restores the configured alarm-time compare when it fires;
 * clock_app supplies the user-visible one-shot behavior by disabling AIE and
 * the persisted enabled flag on the final Stop/C dismissal. */

#define RTC_MAINTENANCE_MS 60000u /* healthy time-resync + latched-AF backstop */
#define RTC_REPAIR_SERVICE_MS 250u /* owed writes and chip recovery stay prompt */
#define RTC_REINIT_MS 1000u /* bounded re-init retry while the chip is unresponsive */
#define RTC_DATETIME_WRITE_RETRIES 4u /* after the immediate attempt: <=1 s total */
#define RTC_EPOCH_YEAR 1999u
#define RTC_LAST_YEAR 2090u
/* A recovered GP0-tagged compare can only be the short clock-app snooze. If its
 * next HH:MM occurrence is farther away than this, the intended occurrence has
 * already passed and recovery must fire now instead of postponing it a day. */
#define RTC_SNOOZE_RECOVERY_MAX_AHEAD_S 600u

typedef struct {
    rtc_datetime_t base_datetime; /* time mirror anchor, resynced from the chip */
    uint64_t base_ms;
    bool time_valid;             /* software mirror is trustworthy */
    bool chip_ok;                /* driver is up / responding */
    bool chip_time_valid;        /* latest checked read/write cleared V2F */
    rtc_datetime_t pending_datetime;
    rtc_datetime_write_status_t datetime_write_status;
    uint8_t datetime_write_retries;
    bool datetime_write_pending;
    bool alarm_enabled;          /* config mirror (the chip holds the live alarm) */
    bool alarm_event;            /* latched fire, consumed by alarm_due() */
    bool af_dismiss_pending;     /* a HW alarm-flag clear NAK'd; retry it in service_chip */
    bool hw_arm_pending;         /* a HW alarm set/disable NAK'd; retry it in service_chip */
    uint8_t alarm_hour;
    uint8_t alarm_minute;
    uint32_t snooze_until_epoch; /* live HW snooze target + awake fallback; 0 = none */
    uint8_t snooze_hour;         /* exact temporary compare, retained if recovery is overdue */
    uint8_t snooze_minute;
    bool alarm_event_time_valid; /* HH:MM belonging to the currently latched fire */
    uint8_t alarm_event_hour;
    uint8_t alarm_event_minute;
    bool boot_snooze_detected;   /* tagged live compare awaiting stored config load */
    uint8_t boot_snooze_hour;
    uint8_t boot_snooze_minute;
    uint64_t next_service_ms;
    uint64_t next_reinit_ms;
} rtc_state_t;

static bool is_leap_year(uint16_t year);
static uint8_t days_in_month(uint16_t year, uint8_t month);
static uint32_t datetime_to_epoch_days(const rtc_datetime_t *datetime);
static uint32_t datetime_to_epoch_seconds(const rtc_datetime_t *datetime);
static void epoch_seconds_to_datetime(uint32_t seconds, rtc_datetime_t *datetime);
static uint32_t current_epoch_seconds(void);
static bool valid_datetime(const rtc_datetime_t *datetime);
static uint32_t snooze_target_epoch(uint8_t hour, uint8_t minute);
static bool commit_pending_datetime(void);
static bool program_alarm_configuration(void);
static void publish_daily_alarm_event(void);
static void fire_snooze(bool alarm_flag_may_be_latched);
static void service_chip(void);
static void maybe_reinit(void);
static bool restore_chip_time_from_mirror(void);
static bool repair_needed(void);
static void schedule_next_service(uint64_t now_ms);
static void request_repair_service(void);

static rtc_state_t s_rtc;

void rtc_alarm_hal_init(void) {
    memset(&s_rtc, 0, sizeof(s_rtc));
    s_rtc.base_datetime.year = RTC_EPOCH_YEAR;
    s_rtc.base_datetime.month = 1u;
    s_rtc.base_datetime.day = 1u;
    s_rtc.base_ms = time_ms64();

    bool valid = false;
    if (rv8803_hal_init(&valid)) {
        s_rtc.chip_ok = true;
        s_rtc.chip_time_valid = valid;
        /* GP0 is committed in the same register burst as the temporary alarm
         * compare. Capture it even if the time read below fails; load_clock's
         * fallback set_datetime establishes a software timebase before the
         * stored alarm config is applied. */
        rv8803_alarm_state_t live_alarm;
        if (rv8803_hal_read_alarm(&live_alarm) &&
            live_alarm.enabled && live_alarm.snooze_marker) {
            s_rtc.boot_snooze_detected = true;
            s_rtc.boot_snooze_hour = live_alarm.hour;
            s_rtc.boot_snooze_minute = live_alarm.minute;
            LOGI("rtc", "recovered snooze compare %02u:%02u",
                 (unsigned)live_alarm.hour,
                 (unsigned)live_alarm.minute);
        }
        if (valid) {
            rtc_datetime_t dt;
            bool read_valid = false;
            bool read_ok = rv8803_hal_read_time_checked(&dt, &read_valid);
            if (read_ok && read_valid) {
                s_rtc.base_datetime = dt;
                s_rtc.base_ms = time_ms64();
                s_rtc.time_valid = true;
                s_rtc.chip_time_valid = true;
            } else if (read_ok) {
                s_rtc.chip_time_valid = false;
                LOGW("rtc", "RV-8803 lost time during initialization");
            } else {
                /* FLAG and alarm reads already proved the chip is responding.
                 * A transient calendar-read NAK prevents this boot from seeding
                 * the software mirror, but must not discard a tagged snooze or
                 * block the fallback set_datetime write. */
                LOGW("rtc", "RV-8803 initial time read failed");
            }
        } else {
            LOGW("rtc", "RV-8803 reports lost time; awaiting clock set");
        }
    } else {
        LOGW("rtc", "RV-8803 init failed; running on software time");
    }
    schedule_next_service(time_ms64());
    s_rtc.next_reinit_ms = time_ms64();
}

bool rtc_alarm_hal_time_valid(void) {
    return s_rtc.time_valid;
}

bool rtc_alarm_hal_chip_available(void) {
    return s_rtc.chip_ok;
}

rtc_datetime_write_status_t rtc_alarm_hal_set_datetime(const rtc_datetime_t *datetime) {
    if (datetime == 0 || !valid_datetime(datetime)) {
        return RTC_DATETIME_WRITE_FAILED;
    }
    s_rtc.pending_datetime = *datetime;
    s_rtc.datetime_write_pending = true;
    s_rtc.datetime_write_retries = 0u;
    s_rtc.datetime_write_status = RTC_DATETIME_WRITE_PENDING;
    if (commit_pending_datetime()) {
        return RTC_DATETIME_WRITE_COMMITTED;
    }
    request_repair_service();
    return RTC_DATETIME_WRITE_PENDING;
}

rtc_datetime_write_status_t rtc_alarm_hal_datetime_write_status(void) {
    return s_rtc.datetime_write_status;
}

bool rtc_alarm_hal_datetime_write_pending(void) {
    return s_rtc.datetime_write_pending;
}

void rtc_alarm_hal_get_datetime(rtc_datetime_t *datetime) {
    if (datetime == 0) {
        return;
    }
    epoch_seconds_to_datetime(current_epoch_seconds(), datetime);
}

uint32_t rtc_alarm_hal_ms_until_next_minute(uint64_t now_ms) {
    uint64_t elapsed_ms = now_ms >= s_rtc.base_ms
                              ? now_ms - s_rtc.base_ms
                              : 0u;
    uint32_t phase_ms = (uint32_t)(
        ((uint64_t)s_rtc.base_datetime.second * 1000u +
         (elapsed_ms % 60000u)) % 60000u);
    return phase_ms == 0u ? 60000u : 60000u - phase_ms;
}

void rtc_alarm_hal_set_daily_alarm(uint8_t hour, uint8_t minute, bool enabled) {
    if (hour > 23u || minute > 59u) {
        return;
    }
    bool preserve_boot_snooze = enabled && s_rtc.boot_snooze_detected;
    s_rtc.alarm_hour = hour;
    s_rtc.alarm_minute = minute;
    s_rtc.alarm_enabled = enabled;
    s_rtc.alarm_event = false;
    s_rtc.alarm_event_time_valid = false;
    s_rtc.af_dismiss_pending = false; /* re-arm/disable below clears AF in HW */
    if (preserve_boot_snooze) {
        s_rtc.snooze_hour = s_rtc.boot_snooze_hour;
        s_rtc.snooze_minute = s_rtc.boot_snooze_minute;
        s_rtc.snooze_until_epoch = snooze_target_epoch(
            s_rtc.boot_snooze_hour, s_rtc.boot_snooze_minute);
    } else {
        s_rtc.snooze_until_epoch = 0u;
        s_rtc.snooze_hour = 0u;
        s_rtc.snooze_minute = 0u;
    }
    s_rtc.boot_snooze_detected = false;
    /* Track a FAILED set/disable so service_chip retries it. Without this a NAK'd
     * disable (the one-shot Stop path) leaves AIE/AF asserted while software
     * believes the alarm is off -> a false re-fire next day, or a latched AF/AIE
     * holding /INT low on SYS_INT (a dormant wake source) -> spurious wake / the
     * powered-off floor never reached. Retried regardless of alarm_enabled. */
    if (preserve_boot_snooze) {
        /* The tagged temporary compare is already live in the external RTC.
         * Keep it; alarm_hour/minute above now hold the configured value to
         * restore on fire or cancellation. */
        s_rtc.hw_arm_pending = false;
    } else {
        (void)program_alarm_configuration();
    }
}

void rtc_alarm_hal_snooze_minutes(uint8_t minutes) {
    if (minutes == 0u || !s_rtc.alarm_enabled) {
        return;
    }
    /* Land the re-fire on a whole-minute boundary so the spacing matches the
     * original 3210 (~6 min for a 5-min snooze taken mid-minute). P1.7 loses
     * RP RAM, so program this temporary target into the external RTC; the epoch
     * mirror remains an awake fallback if IRQ servicing or an I2C transfer is
     * delayed. */
    uint32_t target = rv8803_round_up_to_minute(current_epoch_seconds() + ((uint32_t)minutes * 60u));
    s_rtc.snooze_until_epoch = target;
    rtc_datetime_t snooze;
    epoch_seconds_to_datetime(target, &snooze);
    s_rtc.snooze_hour = snooze.hour;
    s_rtc.snooze_minute = snooze.minute;
    s_rtc.boot_snooze_detected = false;
    s_rtc.alarm_event = false;
    s_rtc.alarm_event_time_valid = false;
    (void)program_alarm_configuration();
}

bool rtc_alarm_hal_alarm_config_committed(void) {
    return !s_rtc.hw_arm_pending;
}

bool rtc_alarm_hal_snooze_active(void) {
    return s_rtc.snooze_until_epoch != 0u;
}

void rtc_alarm_hal_clear_alarm_event(void) {
    bool had_snooze = s_rtc.snooze_until_epoch != 0u;
    s_rtc.alarm_event = false;
    s_rtc.alarm_event_time_valid = false;
    s_rtc.snooze_until_epoch = 0u;
    s_rtc.snooze_hour = 0u;
    s_rtc.snooze_minute = 0u;
    s_rtc.boot_snooze_detected = false;
    if (s_rtc.alarm_enabled) {
        if (had_snooze) {
            /* Cancelling a pending snooze must put the configured alarm compare
             * back; merely clearing AF would leave the live comparator at the
             * temporary snooze time. */
            (void)program_alarm_configuration();
            return;
        }
        /* Remember a FAILED hardware AF clear -- a NAK, OR the chip being down at
         * dismissal time -- so service_chip retries it once the bus is back.
         * Without this, a NAK'd clear leaves AF latched; the next poll re-reads it
         * as a fresh fire and re-sets alarm_event -- on the power-off-mid-ring
         * dismissal (the only caller with alarm_enabled still true) that recreates
         * the power-off/power-up loop this dismissal exists to break. */
        s_rtc.af_dismiss_pending = !(s_rtc.chip_ok && rv8803_hal_clear_alarm_flag());
        if (s_rtc.af_dismiss_pending) {
            request_repair_service();
        }
    }
}

bool rtc_alarm_hal_alarm_event_pending(void) {
    /* Raw latched-fire state, WITHOUT servicing the chip or maturing the snooze
     * (unlike rtc_alarm_hal_alarm_due). True == the alarm is currently ringing;
     * a pending snooze reads false (it set alarm_event false when armed). */
    return s_rtc.alarm_event;
}

bool rtc_alarm_hal_alarm_event_time(uint8_t *out_hour, uint8_t *out_minute) {
    if (!s_rtc.alarm_event || !s_rtc.alarm_event_time_valid ||
        out_hour == 0 || out_minute == 0) {
        return false;
    }
    *out_hour = s_rtc.alarm_event_hour;
    *out_minute = s_rtc.alarm_event_minute;
    return true;
}

bool rtc_alarm_hal_alarm_due(void) {
    uint64_t now_ms = time_ms64();
    if (now_ms >= s_rtc.next_service_ms) {
        service_chip();
        schedule_next_service(now_ms);
    }
    /* Software snooze re-fire (cheap; checked every tick). */
    uint32_t now = current_epoch_seconds();
    if (s_rtc.snooze_until_epoch != 0u && now >= s_rtc.snooze_until_epoch) {
        fire_snooze(false);
    }
    return s_rtc.alarm_event;
}

bool rtc_alarm_hal_alarm_enabled(void) {
    return s_rtc.alarm_enabled;
}

void rtc_alarm_hal_capture_alarm_interrupt(void) {
    if (s_rtc.chip_time_valid && s_rtc.alarm_enabled &&
        !s_rtc.alarm_event &&
        !s_rtc.af_dismiss_pending &&
        !s_rtc.hw_arm_pending) {
        if (s_rtc.snooze_until_epoch != 0u) {
            /* shared_irq_service preserved the AF evidence before clearing it. */
            fire_snooze(false);
        } else {
            publish_daily_alarm_event();
        }
    }
}

void rtc_alarm_hal_force_alarm_event(void) {
    if (s_rtc.snooze_until_epoch != 0u) {
        /* A tagged snooze AF captured before app initialization survived the
         * config load above. Restore the configured compare before publishing it. */
        fire_snooze(true);
    } else {
        publish_daily_alarm_event();
    }
}

static uint32_t snooze_target_epoch(uint8_t hour, uint8_t minute) {
    uint32_t now = current_epoch_seconds();
    uint32_t now_in_day = now % 86400u;
    uint32_t target_in_day = ((uint32_t)hour * 3600u) +
                             ((uint32_t)minute * 60u);
    uint32_t seconds_ahead = (target_in_day + 86400u - now_in_day) % 86400u;
    if (seconds_ahead > RTC_SNOOZE_RECOVERY_MAX_AHEAD_S) {
        /* The tagged target is behind us (possibly just across midnight). AF
         * should also be latched, but make the software fallback independently
         * correct if a reset raced the shared-IRQ clear. */
        return now;
    }
    return now + seconds_ahead;
}

static bool commit_pending_datetime(void) {
    if (!s_rtc.datetime_write_pending || !s_rtc.chip_ok ||
        !rv8803_hal_write_time(&s_rtc.pending_datetime)) {
        return false;
    }

    s_rtc.base_datetime = s_rtc.pending_datetime;
    s_rtc.base_ms = time_ms64();
    s_rtc.time_valid = true;
    s_rtc.chip_time_valid = true;
    s_rtc.datetime_write_pending = false;
    s_rtc.datetime_write_retries = 0u;
    s_rtc.datetime_write_status = RTC_DATETIME_WRITE_COMMITTED;
    s_rtc.alarm_event = false; /* don't carry a stale fire across a time change */
    s_rtc.alarm_event_time_valid = false;
    s_rtc.af_dismiss_pending = false;
    if (s_rtc.alarm_enabled) {
        /* Re-arm the current compare (configured alarm or snooze) for the new
         * time context; this also clears any spurious AF. */
        (void)program_alarm_configuration();
    }
    return true;
}

static bool program_alarm_configuration(void) {
    bool ok = false;
    if (s_rtc.chip_ok && s_rtc.chip_time_valid) {
        if (!s_rtc.alarm_enabled) {
            ok = rv8803_hal_disable_alarm();
        } else {
            uint8_t hour = s_rtc.alarm_hour;
            uint8_t minute = s_rtc.alarm_minute;
            if (s_rtc.snooze_until_epoch != 0u) {
                rtc_datetime_t snooze;
                epoch_seconds_to_datetime(s_rtc.snooze_until_epoch, &snooze);
                hour = snooze.hour;
                minute = snooze.minute;
            }
            ok = s_rtc.snooze_until_epoch != 0u
                ? rv8803_hal_set_snooze_alarm(hour, minute)
                : rv8803_hal_set_alarm(hour, minute);
        }
    }
    s_rtc.hw_arm_pending = !ok;
    if (!ok) {
        request_repair_service();
    }
    if (ok) {
        /* set_alarm/disable_alarm both clear AF as part of committing the live
         * configuration, so no older dismissal remains owed. */
        s_rtc.af_dismiss_pending = false;
    }
    return ok;
}

static void publish_daily_alarm_event(void) {
    s_rtc.alarm_event_hour = s_rtc.alarm_hour;
    s_rtc.alarm_event_minute = s_rtc.alarm_minute;
    s_rtc.alarm_event_time_valid = true;
    s_rtc.alarm_event = true;
}

static void fire_snooze(bool alarm_flag_may_be_latched) {
    if (alarm_flag_may_be_latched) {
        /* If restoring the configured compare fails before its AF-clear step,
         * keep a separate clear owed so SYS_INT cannot remain pinned low. */
        s_rtc.af_dismiss_pending = true;
    }
    /* Capture the temporary compare before restoring the configured daily alarm.
     * In the overdue-recovery case snooze_until_epoch may have been clamped to
     * "now", while snooze_hour/minute still preserve the actual fired HH:MM. */
    s_rtc.alarm_event_hour = s_rtc.snooze_hour;
    s_rtc.alarm_event_minute = s_rtc.snooze_minute;
    s_rtc.alarm_event_time_valid = true;
    s_rtc.snooze_until_epoch = 0u;
    s_rtc.snooze_hour = 0u;
    s_rtc.snooze_minute = 0u;
    s_rtc.alarm_event = true;
    (void)program_alarm_configuration();
}

static void service_chip(void) {
    rtc_datetime_t dt;
    bool hardware_time_valid = false;
    if (rv8803_hal_read_time_checked(&dt, &hardware_time_valid)) {
        s_rtc.chip_ok = true;
        s_rtc.chip_time_valid = hardware_time_valid;
        if (!hardware_time_valid) {
            /* A chip-only brownout must never replace the still-running RP
             * mirror with reset calendar bytes. Restore the external source and
             * make the live alarm compare an owed operation until that succeeds. */
            if (s_rtc.alarm_enabled) {
                s_rtc.hw_arm_pending = true;
            }
            if (!s_rtc.datetime_write_pending) {
                (void)restore_chip_time_from_mirror();
            }
        } else if (!s_rtc.datetime_write_pending) {
            s_rtc.base_datetime = dt;
            s_rtc.base_ms = time_ms64();
        }
    } else {
        /* Track the chip as down so the AF poll below is gated on the actual
         * current health; maybe_reinit() flips it back true if it recovers. */
        s_rtc.chip_ok = false;
        maybe_reinit();
    }
    if (s_rtc.datetime_write_pending) {
        if (!commit_pending_datetime()) {
            s_rtc.datetime_write_retries++;
            if (s_rtc.datetime_write_retries >= RTC_DATETIME_WRITE_RETRIES) {
                s_rtc.datetime_write_pending = false;
                s_rtc.datetime_write_status = RTC_DATETIME_WRITE_FAILED;
                LOGW("rtc", "RV-8803 time write failed after bounded retries");
            }
        }
    }
    /* Retry a deferred HW arm/disable (a set_daily_alarm / set_datetime whose
     * set_alarm or disable_alarm NAK'd) so AIE/AF end up matching the software
     * alarm_enabled state -- otherwise a NAK'd disable leaves AIE/AF asserted and
     * re-fires a "disabled" alarm or holds /INT low. Retried regardless of
     * alarm_enabled (the disable case must clear even when disabled). */
    if (s_rtc.chip_ok && s_rtc.chip_time_valid && s_rtc.hw_arm_pending) {
        (void)program_alarm_configuration();
    }
    /* Retry a deferred AF dismissal (a clear_alarm_event whose HW write NAK'd)
     * BEFORE polling for a fresh fire, so a still-latched AF from a DISMISSED alarm
     * is cleared rather than re-read as a new fire. */
    if (s_rtc.chip_ok && s_rtc.af_dismiss_pending) {
        if (rv8803_hal_clear_alarm_flag()) {
            s_rtc.af_dismiss_pending = false;
        }
    }
    /* Hardware alarm/snooze compare: AF latches at HH:MM:00 and holds until
     * cleared. Poll only while armed, not already latched, not mid-dismissal,
     * and not mid-(re)arm. A snooze fire restores the configured compare first. */
    if (s_rtc.chip_ok && s_rtc.chip_time_valid && s_rtc.alarm_enabled &&
        !s_rtc.alarm_event && !s_rtc.af_dismiss_pending && !s_rtc.hw_arm_pending) {
        bool fired = false;
        if (rv8803_hal_poll_alarm_flag(&fired) && fired) {
            if (s_rtc.snooze_until_epoch != 0u) {
                fire_snooze(true);
            } else {
                /* Latch the fire; if clearing AF NAK'd, arm the retry so /INT
                 * is eventually released. */
                s_rtc.af_dismiss_pending = !rv8803_hal_clear_alarm_flag();
                publish_daily_alarm_event();
            }
        }
    }
}

static void maybe_reinit(void) {
    uint64_t now_ms = time_ms64();
    if (now_ms < s_rtc.next_reinit_ms) {
        return;
    }
    s_rtc.next_reinit_ms = now_ms + RTC_REINIT_MS;
    bool valid = false;
    if (rv8803_hal_init(&valid)) {
        s_rtc.chip_ok = true;
        s_rtc.chip_time_valid = valid;
        if (!valid && s_rtc.alarm_enabled) {
            s_rtc.hw_arm_pending = true;
        }
        if (!valid && !s_rtc.datetime_write_pending) {
            (void)restore_chip_time_from_mirror();
        }
        if (s_rtc.chip_ok && s_rtc.chip_time_valid &&
            (s_rtc.alarm_enabled || s_rtc.hw_arm_pending)) {
            /* Reprogram the configured/snooze alarm (or finish an owed disable)
             * that the chip may have lost across its own reset. */
            (void)program_alarm_configuration();
        }
    } else {
        s_rtc.chip_ok = false;
    }
}

static bool restore_chip_time_from_mirror(void) {
    if (!s_rtc.chip_ok || s_rtc.chip_time_valid || !s_rtc.time_valid ||
        s_rtc.datetime_write_pending) {
        return false;
    }
    rtc_datetime_t current;
    epoch_seconds_to_datetime(current_epoch_seconds(), &current);
    if (!rv8803_hal_write_time(&current)) {
        s_rtc.chip_ok = false;
        request_repair_service();
        return false;
    }
    s_rtc.chip_time_valid = true;
    LOGW("rtc", "restored RV-8803 time after V2F data loss");
    return true;
}

static bool repair_needed(void) {
    return !s_rtc.chip_ok ||
           s_rtc.datetime_write_pending ||
           s_rtc.af_dismiss_pending ||
           s_rtc.hw_arm_pending;
}

static void schedule_next_service(uint64_t now_ms) {
    s_rtc.next_service_ms = now_ms +
        (repair_needed() ? RTC_REPAIR_SERVICE_MS : RTC_MAINTENANCE_MS);
}

static void request_repair_service(void) {
    uint64_t deadline = time_ms64() + RTC_REPAIR_SERVICE_MS;
    if (s_rtc.next_service_ms == 0u || deadline < s_rtc.next_service_ms) {
        s_rtc.next_service_ms = deadline;
    }
}

static bool is_leap_year(uint16_t year) {
    return ((year % 4u) == 0u && (year % 100u) != 0u) || ((year % 400u) == 0u);
}

static uint8_t days_in_month(uint16_t year, uint8_t month) {
    static const uint8_t DAYS[] = {31u, 28u, 31u, 30u, 31u, 30u, 31u, 31u, 30u, 31u, 30u, 31u};
    if (month == 2u && is_leap_year(year)) {
        return 29u;
    }
    return DAYS[month - 1u];
}

static uint32_t datetime_to_epoch_days(const rtc_datetime_t *datetime) {
    uint32_t days = 0;
    for (uint16_t year = RTC_EPOCH_YEAR; year < datetime->year; year++) {
        days += is_leap_year(year) ? 366u : 365u;
    }
    for (uint8_t month = 1u; month < datetime->month; month++) {
        days += days_in_month(datetime->year, month);
    }
    days += (uint32_t)datetime->day - 1u;
    return days;
}

static uint32_t datetime_to_epoch_seconds(const rtc_datetime_t *datetime) {
    return (datetime_to_epoch_days(datetime) * 86400u) +
           ((uint32_t)datetime->hour * 3600u) +
           ((uint32_t)datetime->minute * 60u) +
           datetime->second;
}

static void epoch_seconds_to_datetime(uint32_t seconds, rtc_datetime_t *datetime) {
    uint32_t days = seconds / 86400u;
    uint32_t rem = seconds % 86400u;
    uint16_t year = RTC_EPOCH_YEAR;
    while (true) {
        uint16_t year_days = is_leap_year(year) ? 366u : 365u;
        if (days < year_days) {
            break;
        }
        days -= year_days;
        year++;
    }
    uint8_t month = 1u;
    while (month < 12u) { /* guard: never index days_in_month() past December */
        uint8_t month_days = days_in_month(year, month);
        if (days < month_days) {
            break;
        }
        days -= month_days;
        month++;
    }
    datetime->year = year;
    datetime->month = month;
    datetime->day = (uint8_t)(days + 1u);
    datetime->hour = (uint8_t)(rem / 3600u);
    rem %= 3600u;
    datetime->minute = (uint8_t)(rem / 60u);
    datetime->second = (uint8_t)(rem % 60u);
}

static uint32_t current_epoch_seconds(void) {
    /* 64-bit elapsed milliseconds: immune to the ~49.7-day wrap of the 32-bit
     * millisecond clock, which would otherwise corrupt the time/alarm. */
    uint32_t elapsed = (uint32_t)((time_ms64() - s_rtc.base_ms) / 1000u);
    return datetime_to_epoch_seconds(&s_rtc.base_datetime) + elapsed;
}

static bool valid_datetime(const rtc_datetime_t *datetime) {
    /* The RV-8803 has a two-digit year and no century bit. The product date
     * domain is 1999..2090, so register 99 is reserved for 1999 while 00..90
     * represent 2000..2090. This makes the v6.00 default 01.01.1999 a live RTC
     * value instead of a storage-only value rejected by this layer. */
    if (datetime->year < RTC_EPOCH_YEAR || datetime->year > RTC_LAST_YEAR ||
        datetime->month < 1u || datetime->month > 12u ||
        datetime->hour > 23u || datetime->minute > 59u || datetime->second > 59u) {
        return false;
    }
    return datetime->day >= 1u && datetime->day <= days_in_month(datetime->year, datetime->month);
}

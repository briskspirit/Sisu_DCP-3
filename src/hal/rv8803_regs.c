#include "hal/rv8803_hal.h"

/* Pure RV-8803 register packing/unpacking + RTC math. No I2C, no SDK, so this
 * unit is host-compilable and unit-tested directly (test_rv8803_regs). */

static bool valid_bcd(uint8_t b) {
    return ((b & 0x0fu) <= 9u) && (((b >> 4) & 0x0fu) <= 9u);
}

uint8_t rv8803_bin_to_bcd(uint8_t bin) {
    return (uint8_t)(((bin / 10u) << 4) | (bin % 10u));
}

uint8_t rv8803_bcd_to_bin(uint8_t bcd) {
    return (uint8_t)((((bcd >> 4) & 0x0fu) * 10u) + (bcd & 0x0fu));
}

/* Sakamoto's algorithm: 0 = Sunday .. 6 = Saturday. The exact mapping is
 * don't-care for us (week-alarm is masked); we only need a stable one-hot. */
static uint8_t day_of_week(uint16_t year, uint8_t month, uint8_t day) {
    static const uint8_t t[12] = {0u, 3u, 2u, 5u, 0u, 3u, 5u, 1u, 4u, 6u, 2u, 4u};
    uint16_t y = year;
    if (month < 3u) {
        y = (uint16_t)(y - 1u);
    }
    return (uint8_t)((y + y / 4u - y / 100u + y / 400u + t[month - 1u] + day) % 7u);
}

void rv8803_encode_time(const rtc_datetime_t *dt, uint8_t regs[7]) {
    regs[0] = rv8803_bin_to_bcd(dt->second);
    regs[1] = rv8803_bin_to_bcd(dt->minute);
    regs[2] = rv8803_bin_to_bcd(dt->hour);
    regs[3] = (uint8_t)(1u << day_of_week(dt->year, dt->month, dt->day));
    regs[4] = rv8803_bin_to_bcd(dt->day);
    regs[5] = rv8803_bin_to_bcd(dt->month);
    regs[6] = rv8803_bin_to_bcd((uint8_t)(dt->year % 100u));
}

bool rv8803_decode_time(const uint8_t regs[7], rtc_datetime_t *dt) {
    uint8_t sec_b = regs[0] & 0x7fu;
    uint8_t min_b = regs[1] & 0x7fu;
    uint8_t hour_b = regs[2] & 0x3fu;
    uint8_t date_b = regs[4] & 0x3fu;
    uint8_t mon_b = regs[5] & 0x1fu;
    uint8_t year_b = regs[6];
    if (!valid_bcd(sec_b) || !valid_bcd(min_b) || !valid_bcd(hour_b) || !valid_bcd(date_b) ||
        !valid_bcd(mon_b) || !valid_bcd(year_b)) {
        return false;
    }
    uint8_t second = rv8803_bcd_to_bin(sec_b);
    uint8_t minute = rv8803_bcd_to_bin(min_b);
    uint8_t hour = rv8803_bcd_to_bin(hour_b);
    uint8_t day = rv8803_bcd_to_bin(date_b);
    uint8_t month = rv8803_bcd_to_bin(mon_b);
    /* The part stores only two year digits. Our product domain ends at 2090,
     * leaving 99 unambiguous: reserve it for the Nokia epoch year 1999 and map
     * every other two-digit value into the 2000s. Encoding writes year % 100. */
    uint8_t year_digits = rv8803_bcd_to_bin(year_b);
    uint16_t year = year_digits == 99u
        ? 1999u
        : (uint16_t)(2000u + year_digits);
    if (second > 59u || minute > 59u || hour > 23u || month < 1u || month > 12u || day < 1u ||
        day > 31u) {
        return false;
    }
    dt->year = year;
    dt->month = month;
    dt->day = day;
    dt->hour = hour;
    dt->minute = minute;
    dt->second = second;
    return true;
}

void rv8803_encode_alarm(uint8_t hour, uint8_t minute, uint8_t regs[3]) {
    regs[0] = (uint8_t)(rv8803_bin_to_bcd(minute) & 0x7fu); /* AE_M = 0 (minute enabled) */
    regs[1] = (uint8_t)(rv8803_bin_to_bcd(hour) & 0x3fu);   /* AE_H = 0 (hour enabled) */
    regs[2] = 0x80u;                                        /* AE_WD = 1 (week/date masked -> daily) */
}

void rv8803_encode_snooze_alarm(uint8_t hour, uint8_t minute, uint8_t regs[3]) {
    rv8803_encode_alarm(hour, minute, regs);
    /* Application Manual 3.4: Hours Alarm bit 6 is GP0, a general-purpose bit
     * separate from the BCD hour and AE_H fields. */
    regs[1] |= 0x40u;
}

bool rv8803_decode_alarm(const uint8_t regs[3],
                         uint8_t *hour,
                         uint8_t *minute,
                         bool *snooze_marker) {
    if (regs == 0 || hour == 0 || minute == 0 || snooze_marker == 0) {
        return false;
    }
    /* This firmware's alarm shape requires minute + hour compares enabled and
     * weekday/date masked. Bit 6 of Hours Alarm is GP0, not part of the BCD. */
    if ((regs[0] & 0x80u) != 0u || (regs[1] & 0x80u) != 0u ||
        (regs[2] & 0x80u) == 0u) {
        return false;
    }
    uint8_t minute_bcd = (uint8_t)(regs[0] & 0x7fu);
    uint8_t hour_bcd = (uint8_t)(regs[1] & 0x3fu);
    if (!valid_bcd(minute_bcd) || !valid_bcd(hour_bcd)) {
        return false;
    }
    uint8_t decoded_minute = rv8803_bcd_to_bin(minute_bcd);
    uint8_t decoded_hour = rv8803_bcd_to_bin(hour_bcd);
    if (decoded_minute > 59u || decoded_hour > 23u) {
        return false;
    }
    *hour = decoded_hour;
    *minute = decoded_minute;
    *snooze_marker = (regs[1] & 0x40u) != 0u;
    return true;
}

uint32_t rv8803_round_up_to_minute(uint32_t epoch_seconds) {
    return ((epoch_seconds + 59u) / 60u) * 60u;
}

bool rv8803_encode_timer_seconds(uint16_t seconds, uint8_t regs[2]) {
    /* 12-bit down-counter clocked at 1 Hz (TD=10): period = counter seconds.
     * 0 is meaningless and >4095 does not fit the counter. */
    if (seconds == 0u || seconds > 0x0fffu) {
        return false;
    }
    regs[0] = (uint8_t)(seconds & 0xffu);        /* Timer Counter 0: bits 7:0 */
    regs[1] = (uint8_t)((seconds >> 8) & 0x0fu); /* Timer Counter 1: bits 11:8 */
    return true;
}

#include "hal/rv8803_hal.h"

#include <string.h>

#include "hardware/i2c.h"
#include "hal/board.h"
#include "services/log.h"
#include "pico/stdlib.h"

#define RV8803_REG_SEC 0x00u       /* seconds .. year occupy 0x00..0x06 */
#define RV8803_REG_ALARM_MIN 0x08u /* alarm minute/hour/week-date occupy 0x08..0x0a */
#define RV8803_REG_TIMER_CNT 0x0bu /* countdown counter: low 8 @0x0b, high 4 @0x0c */
#define RV8803_REG_EXT 0x0du
#define RV8803_REG_FLAG 0x0eu
#define RV8803_REG_CTRL 0x0fu

#define RV8803_FLAG_V1F 0x01u
#define RV8803_FLAG_V2F 0x02u
#define RV8803_FLAG_AF 0x08u
#define RV8803_FLAG_TF 0x10u
/* All six FLAG-register event bits (0..5: V1F,V2F,EVF,AF,TF,UF); bits 6-7 reserved
 * (write 0). Every bit is write-0-to-clear / write-1-ignored, so a SINGLE constant
 * write of (RV8803_FLAG_ALL & ~target) clears only `target` and preserves every
 * other flag WITHOUT a read -- closing the read-modify-write window where a flag
 * latching between the read and the write was silently wiped (a lost dormant
 * alarm/timer wake). */
#define RV8803_FLAG_ALL 0x3Fu
#define RV8803_CTRL_AIE 0x08u
#define RV8803_CTRL_TIE 0x10u
#define RV8803_EXT_TD_MASK 0x03u /* countdown source: 00=4096Hz 01=64Hz 10=1Hz 11=1/60Hz */
#define RV8803_EXT_TD_1HZ 0x02u
#define RV8803_EXT_TE 0x10u

/* Consecutive timed-out/failed transfers after which the chip is treated as
 * wedged and transfers short-circuit until rv8803_hal_init() re-arms it (so a
 * stuck bus cannot stall the 8 ms loop). Mirrors the TCA8418 discipline. */
#define RV8803_I2C_FAIL_LIMIT 8u

static bool s_bus_failed;
static uint8_t s_i2c_fail_count;

static void note_i2c_result(bool ok) {
    if (ok) {
        s_i2c_fail_count = 0u;
        return;
    }
    if (s_i2c_fail_count < 0xffu) {
        s_i2c_fail_count++;
    }
    if (s_i2c_fail_count >= RV8803_I2C_FAIL_LIMIT && !s_bus_failed) {
        s_bus_failed = true;
        LOGW("rtc", "RV-8803 I2C unresponsive; disabling until re-init");
    }
}

static bool write_regs(uint8_t reg, const uint8_t *src, uint8_t len) {
    if (s_bus_failed || len > 8u) {
        return false;
    }
    uint8_t buf[1u + 8u];
    buf[0] = reg;
    for (uint8_t i = 0u; i < len; i++) {
        buf[1u + i] = src[i];
    }
    bool ok = i2c_write_timeout_us(BOARD_I2C_PORT, RV8803_I2C_ADDR, buf, (size_t)(len + 1u), false,
                                   BOARD_I2C_TIMEOUT_US) == (int)(len + 1u);
    note_i2c_result(ok);
    return ok;
}

static bool read_regs(uint8_t reg, uint8_t *dst, uint8_t len) {
    if (dst == 0 || len == 0u || s_bus_failed) {
        return false;
    }
    int written = i2c_write_timeout_us(BOARD_I2C_PORT, RV8803_I2C_ADDR, &reg, 1u, true,
                                       BOARD_I2C_TIMEOUT_US);
    if (written != 1) {
        note_i2c_result(false);
        return false;
    }
    bool ok = i2c_read_timeout_us(BOARD_I2C_PORT, RV8803_I2C_ADDR, dst, len, false,
                                  BOARD_I2C_TIMEOUT_US) == (int)len;
    note_i2c_result(ok);
    return ok;
}

static bool write_reg(uint8_t reg, uint8_t value) {
    return write_regs(reg, &value, 1u);
}

static bool read_reg(uint8_t reg, uint8_t *value) {
    return read_regs(reg, value, 1u);
}

bool rv8803_hal_init(bool *out_time_valid) {
    s_bus_failed = false;
    s_i2c_fail_count = 0u;
    if (out_time_valid != 0) {
        *out_time_valid = false;
    }
    uint8_t flag = 0u;
    if (!read_reg(RV8803_REG_FLAG, &flag)) {
        return false;
    }
    if (out_time_valid != 0) {
        /* V2F set => the chip lost power / never been set: time is not trustworthy.
         * Leave V1F/V2F set; rv8803_hal_write_time() clears them once a valid
         * time is written, so the flag stays meaningful until then. */
        *out_time_valid = (flag & RV8803_FLAG_V2F) == 0u;
    }
    return true;
}

bool rv8803_hal_read_time(rtc_datetime_t *dt) {
    bool valid = false;
    return rv8803_hal_read_time_checked(dt, &valid) && valid;
}

bool rv8803_hal_read_time_checked(rtc_datetime_t *dt, bool *out_time_valid) {
    if (dt == 0 || out_time_valid == 0) {
        return false;
    }
    *out_time_valid = false;
    /* SEC..FLAG is one auto-incrementing 15-byte burst. Reading the intervening
     * alarm/timer registers has no side effects and keeps the healthy 60 s
     * maintenance wake to one I2C transaction while pairing V2F with the time
     * bytes it qualifies. */
    uint8_t regs[RV8803_REG_FLAG - RV8803_REG_SEC + 1u];
    if (!read_regs(RV8803_REG_SEC, regs, (uint8_t)sizeof(regs))) {
        return false;
    }
    /* RV-8803 seconds==59 re-read guard (datasheet): a 1 Hz carry landing mid-burst
     * tears the read (minute/hour/day advance while seconds still reads the old
     * value) -- and 59 is the only seconds value that value can surface as. Re-read
     * once when seconds==59; the carry is already past, so the second burst is
     * consistent. Costs one extra ~150 us I2C burst per minute, core0-only. */
    if ((regs[0] & 0x7fu) == 0x59u) {
        if (!read_regs(RV8803_REG_SEC, regs, (uint8_t)sizeof(regs))) {
            return false;
        }
    }
    if ((regs[RV8803_REG_FLAG - RV8803_REG_SEC] & RV8803_FLAG_V2F) != 0u) {
        return true;
    }
    if (!rv8803_decode_time(regs, dt)) {
        return false;
    }
    *out_time_valid = true;
    return true;
}

bool rv8803_hal_write_time(const rtc_datetime_t *dt) {
    if (dt == 0) {
        return false;
    }
    uint8_t regs[7];
    rv8803_encode_time(dt, regs);
    if (!write_regs(RV8803_REG_SEC, regs, 7u)) {
        return false;
    }
    /* Time is now valid: clear the low-voltage/data-loss flags (constant write,
     * no read -- preserves AF/TF/EVF/UF, which write-1 ignores). */
    return write_reg(RV8803_REG_FLAG, (uint8_t)(RV8803_FLAG_ALL & ~(RV8803_FLAG_V1F | RV8803_FLAG_V2F)));
}

static bool set_alarm(uint8_t hour, uint8_t minute, bool snooze_marker) {
    if (hour > 23u || minute > 59u) {
        return false;
    }
    uint8_t ctrl = 0u;
    if (!read_reg(RV8803_REG_CTRL, &ctrl)) {
        return false;
    }
    /* Datasheet: clear AIE while reprogramming the alarm to avoid a spurious
     * interrupt, then clear AF, then enable AIE. */
    if (!write_reg(RV8803_REG_CTRL, (uint8_t)(ctrl & ~RV8803_CTRL_AIE))) {
        return false;
    }
    uint8_t alarm[3];
    if (snooze_marker) {
        rv8803_encode_snooze_alarm(hour, minute, alarm);
    } else {
        rv8803_encode_alarm(hour, minute, alarm);
    }
    if (!write_regs(RV8803_REG_ALARM_MIN, alarm, 3u)) {
        return false;
    }
    if (!rv8803_hal_clear_alarm_flag()) {
        return false;
    }
    return write_reg(RV8803_REG_CTRL, (uint8_t)(ctrl | RV8803_CTRL_AIE));
}

bool rv8803_hal_set_alarm(uint8_t hour, uint8_t minute) {
    return set_alarm(hour, minute, false);
}

bool rv8803_hal_set_snooze_alarm(uint8_t hour, uint8_t minute) {
    return set_alarm(hour, minute, true);
}

bool rv8803_hal_read_alarm(rv8803_alarm_state_t *out) {
    if (out == 0) {
        return false;
    }
    uint8_t alarm[3];
    uint8_t ctrl = 0u;
    if (!read_regs(RV8803_REG_ALARM_MIN, alarm, 3u) ||
        !read_reg(RV8803_REG_CTRL, &ctrl)) {
        return false;
    }
    rv8803_alarm_state_t state = {0};
    if (!rv8803_decode_alarm(
            alarm, &state.hour, &state.minute, &state.snooze_marker)) {
        return false;
    }
    state.enabled = (ctrl & RV8803_CTRL_AIE) != 0u;
    *out = state;
    return true;
}

bool rv8803_hal_disable_alarm(void) {
    uint8_t ctrl = 0u;
    if (!read_reg(RV8803_REG_CTRL, &ctrl)) {
        return false;
    }
    if (!write_reg(RV8803_REG_CTRL, (uint8_t)(ctrl & ~RV8803_CTRL_AIE))) {
        return false;
    }
    return rv8803_hal_clear_alarm_flag();
}

bool rv8803_hal_poll_alarm_flag(bool *out_fired) {
    if (out_fired == 0) {
        return false;
    }
    uint8_t flag = 0u;
    if (!read_reg(RV8803_REG_FLAG, &flag)) {
        return false;
    }
    *out_fired = (flag & RV8803_FLAG_AF) != 0u;
    return true;
}

bool rv8803_hal_clear_alarm_flag(void) {
    /* Single constant write (no read): flag bits clear on write-0 and ignore
     * write-1, so writing all-flags-except-AF clears AF and preserves the others.
     * Was a read-modify-write whose read->write gap could silently wipe a TF/UF
     * (or a re-latched AF) that fired in between -- losing a dormant wake. */
    return write_reg(RV8803_REG_FLAG, (uint8_t)(RV8803_FLAG_ALL & ~RV8803_FLAG_AF));
}

void rv8803_hal_service_interrupt(rv8803_irq_result_t *out) {
    rv8803_irq_result_t result;
    memset(&result, 0, sizeof(result));

    uint8_t flags = 0u;
    if (!read_reg(RV8803_REG_FLAG, &flags)) {
        if (out != NULL) {
            *out = result;
        }
        return;
    }
    result.flags = flags;
    result.alarm = (flags & RV8803_FLAG_AF) != 0u;
    result.timer = (flags & RV8803_FLAG_TF) != 0u;
    result.did_work = result.alarm || result.timer;
    if (result.did_work) {
        uint8_t clear_mask = 0u;
        if (result.alarm) {
            clear_mask |= RV8803_FLAG_AF;
        }
        if (result.timer) {
            clear_mask |= RV8803_FLAG_TF;
        }
        if (!write_reg(
                RV8803_REG_FLAG,
                (uint8_t)(RV8803_FLAG_ALL & ~clear_mask))) {
            result.more_work = true;
            if (out != NULL) {
                *out = result;
            }
            return;
        }
    }
    result.transport_ok = true;
    if (out != NULL) {
        *out = result;
    }
}

bool rv8803_hal_start_periodic_timer(uint16_t seconds) {
    uint8_t counter[2];
    if (!rv8803_encode_timer_seconds(seconds, counter)) {
        return false;
    }
    /* App-manual order: TE=0 before touching TD/counter (the counter must not
     * be written while running), clear a stale TF, arm TIE, then TE=1 last.
     * EXT is read-modify-written so WADA/USEL/FD (alarm mode, CLKOUT) survive. */
    uint8_t ext = 0u;
    if (!read_reg(RV8803_REG_EXT, &ext)) {
        return false;
    }
    ext = (uint8_t)((ext & ~(RV8803_EXT_TE | RV8803_EXT_TD_MASK)) | RV8803_EXT_TD_1HZ);
    if (!write_reg(RV8803_REG_EXT, ext)) {
        return false;
    }
    if (!write_regs(RV8803_REG_TIMER_CNT, counter, 2u)) {
        return false;
    }
    if (!rv8803_hal_clear_timer_flag()) {
        return false;
    }
    uint8_t ctrl = 0u;
    if (!read_reg(RV8803_REG_CTRL, &ctrl)) {
        return false;
    }
    if (!write_reg(RV8803_REG_CTRL, (uint8_t)(ctrl | RV8803_CTRL_TIE))) {
        return false;
    }
    return write_reg(RV8803_REG_EXT, (uint8_t)(ext | RV8803_EXT_TE));
}

bool rv8803_hal_stop_periodic_timer(void) {
    uint8_t ext = 0u;
    if (!read_reg(RV8803_REG_EXT, &ext)) {
        return false;
    }
    if (!write_reg(RV8803_REG_EXT, (uint8_t)(ext & ~RV8803_EXT_TE))) {
        return false;
    }
    uint8_t ctrl = 0u;
    if (!read_reg(RV8803_REG_CTRL, &ctrl)) {
        return false;
    }
    if (!write_reg(RV8803_REG_CTRL, (uint8_t)(ctrl & ~RV8803_CTRL_TIE))) {
        return false;
    }
    return rv8803_hal_clear_timer_flag();
}

bool rv8803_hal_clear_timer_flag(void) {
    /* Single constant write (no read): clears TF, preserves the other flags via
     * write-1-ignored -- no read->write window (see rv8803_hal_clear_alarm_flag). */
    return write_reg(RV8803_REG_FLAG, (uint8_t)(RV8803_FLAG_ALL & ~RV8803_FLAG_TF));
}

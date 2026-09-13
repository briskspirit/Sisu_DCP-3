#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "hal/rv8803_hal.h"
#include "hardware/i2c.h"
#include "services/log.h"

#define REG_SEC 0x00u
#define REG_FLAG 0x0eu
#define FLAG_V2F 0x02u

static i2c_inst_t s_i2c;
i2c_inst_t *const i2c0 = &s_i2c;

static uint8_t s_regs[256];
static uint8_t s_selected_reg;
static unsigned s_pointer_writes;
static unsigned s_reads;
static size_t s_last_read_len;
static bool s_advance_after_first_time_read;

static void load_time(const rtc_datetime_t *dt) {
    uint8_t encoded[7];
    rv8803_encode_time(dt, encoded);
    memcpy(&s_regs[REG_SEC], encoded, sizeof(encoded));
}

int i2c_write_timeout_us(i2c_inst_t *i2c,
                         uint8_t addr,
                         const uint8_t *src,
                         size_t len,
                         bool nostop,
                         unsigned int timeout_us) {
    (void)i2c;
    (void)timeout_us;
    if (addr != RV8803_I2C_ADDR || src == NULL || len == 0u) {
        return -1;
    }
    if (len == 1u) {
        assert(nostop);
        s_pointer_writes++;
        s_selected_reg = src[0];
        return 1;
    }
    assert(!nostop);
    s_selected_reg = src[0];
    memcpy(&s_regs[s_selected_reg], &src[1], len - 1u);
    return (int)len;
}

int i2c_read_timeout_us(i2c_inst_t *i2c,
                        uint8_t addr,
                        uint8_t *dst,
                        size_t len,
                        bool nostop,
                        unsigned int timeout_us) {
    (void)i2c;
    (void)timeout_us;
    if (addr != RV8803_I2C_ADDR || dst == NULL || len == 0u) {
        return -1;
    }
    assert(!nostop);
    s_reads++;
    s_last_read_len = len;
    memcpy(dst, &s_regs[s_selected_reg], len);
    if (s_advance_after_first_time_read && s_selected_reg == REG_SEC &&
        len == REG_FLAG - REG_SEC + 1u && s_reads == 1u) {
        const rtc_datetime_t next = {
            .year = 2026u,
            .month = 8u,
            .day = 14u,
            .hour = 0u,
            .minute = 0u,
            .second = 0u,
        };
        load_time(&next);
    }
    return (int)len;
}

void log_write(log_level_t level, const char *tag, const char *fmt, ...) {
    (void)level;
    (void)tag;
    (void)fmt;
}

static void reset_bus(void) {
    memset(s_regs, 0, sizeof(s_regs));
    s_selected_reg = 0u;
    s_pointer_writes = 0u;
    s_reads = 0u;
    s_last_read_len = 0u;
    s_advance_after_first_time_read = false;
    assert(rv8803_hal_init(NULL));
    s_pointer_writes = 0u;
    s_reads = 0u;
}

static void test_checked_read_pairs_time_with_v2f(void) {
    reset_bus();
    const rtc_datetime_t expected = {
        .year = 2026u,
        .month = 8u,
        .day = 13u,
        .hour = 21u,
        .minute = 47u,
        .second = 12u,
    };
    load_time(&expected);

    rtc_datetime_t got = {0};
    bool valid = false;
    assert(rv8803_hal_read_time_checked(&got, &valid));
    assert(valid);
    assert(memcmp(&got, &expected, sizeof(got)) == 0);
    assert(s_pointer_writes == 1u);
    assert(s_reads == 1u);
    assert(s_last_read_len == REG_FLAG - REG_SEC + 1u);

    s_regs[REG_FLAG] = FLAG_V2F;
    const rtc_datetime_t sentinel = {
        .year = 1999u,
        .month = 12u,
        .day = 31u,
        .hour = 23u,
        .minute = 59u,
        .second = 58u,
    };
    got = sentinel;
    valid = true;
    assert(rv8803_hal_read_time_checked(&got, &valid));
    assert(!valid);
    assert(memcmp(&got, &sentinel, sizeof(got)) == 0);
    assert(!rv8803_hal_read_time(&got));
}

static void test_seconds_59_rereads_full_qualified_snapshot(void) {
    reset_bus();
    const rtc_datetime_t before_rollover = {
        .year = 2026u,
        .month = 8u,
        .day = 13u,
        .hour = 23u,
        .minute = 59u,
        .second = 59u,
    };
    load_time(&before_rollover);
    s_advance_after_first_time_read = true;

    rtc_datetime_t got = {0};
    bool valid = false;
    assert(rv8803_hal_read_time_checked(&got, &valid));
    assert(valid);
    assert(s_pointer_writes == 2u);
    assert(s_reads == 2u);
    assert(s_last_read_len == REG_FLAG - REG_SEC + 1u);
    assert(got.year == 2026u && got.month == 8u && got.day == 14u);
    assert(got.hour == 0u && got.minute == 0u && got.second == 0u);
}

int main(void) {
    test_checked_read_pairs_time_with_v2f();
    test_seconds_59_rereads_full_qualified_snapshot();
    puts("RV-8803 checked-read tests passed");
    return 0;
}

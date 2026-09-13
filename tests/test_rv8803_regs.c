#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "hal/rv8803_hal.h"

static int s_failures;

static void assert_true(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        s_failures++;
    }
}

static void test_bcd_round_trip(void) {
    for (uint8_t v = 0u; v <= 99u; v++) {
        uint8_t bcd = rv8803_bin_to_bcd(v);
        assert_true((bcd & 0x0fu) <= 9u && ((bcd >> 4) & 0x0fu) <= 9u, "bcd digits in range");
        assert_true(rv8803_bcd_to_bin(bcd) == v, "bcd round-trips");
    }
    assert_true(rv8803_bin_to_bcd(0u) == 0x00u, "bcd 0");
    assert_true(rv8803_bin_to_bcd(59u) == 0x59u, "bcd 59");
    assert_true(rv8803_bin_to_bcd(23u) == 0x23u, "bcd 23");
}

static void check_time_round_trip(rtc_datetime_t in, const char *what) {
    uint8_t regs[7];
    rv8803_encode_time(&in, regs);
    rtc_datetime_t out;
    memset(&out, 0, sizeof(out));
    assert_true(rv8803_decode_time(regs, &out), what);
    assert_true(out.year == in.year && out.month == in.month && out.day == in.day &&
                    out.hour == in.hour && out.minute == in.minute && out.second == in.second,
                what);
}

static void test_time_round_trip(void) {
    check_time_round_trip((rtc_datetime_t){1999u, 1u, 1u, 0u, 0u, 0u}, "Nokia epoch start");
    check_time_round_trip((rtc_datetime_t){2026u, 6u, 24u, 7u, 35u, 12u}, "ordinary datetime");
    check_time_round_trip((rtc_datetime_t){2000u, 1u, 1u, 0u, 0u, 0u}, "hardware century start");
    check_time_round_trip((rtc_datetime_t){2090u, 12u, 31u, 23u, 59u, 59u}, "end of product range");
    check_time_round_trip((rtc_datetime_t){2024u, 2u, 29u, 12u, 0u, 30u}, "leap day");

    uint8_t regs[7];
    rv8803_encode_time(&(rtc_datetime_t){1999u, 1u, 1u, 0u, 0u, 0u}, regs);
    assert_true(regs[6] == 0x99u, "1999 uses the reserved BCD-99 year");
}

static void test_decode_masks_and_rejects(void) {
    /* Upper status bits on the time bytes must be masked off, not decoded. */
    uint8_t regs[7] = {0x80u | 0x12u, 0x80u | 0x34u, 0x40u | 0x07u, 0xffu, 0x40u | 0x18u, 0x06u, 0x26u};
    rtc_datetime_t out;
    memset(&out, 0, sizeof(out));
    assert_true(rv8803_decode_time(regs, &out), "decode with high status bits set");
    assert_true(out.second == 12u && out.minute == 34u && out.hour == 7u && out.day == 18u &&
                    out.month == 6u && out.year == 2026u,
                "decode masks status bits correctly");

    /* Invalid BCD nibble (0x0a) must be rejected. */
    uint8_t bad_bcd[7] = {0x0au, 0x00u, 0x00u, 0x01u, 0x01u, 0x01u, 0x00u};
    assert_true(!rv8803_decode_time(bad_bcd, &out), "reject invalid BCD seconds");

    /* Out-of-range month (BCD 13) must be rejected. */
    uint8_t bad_month[7] = {0x00u, 0x00u, 0x00u, 0x01u, 0x01u, 0x13u, 0x00u};
    assert_true(!rv8803_decode_time(bad_month, &out), "reject month 13");

    /* Out-of-range hour (BCD 24) must be rejected. */
    uint8_t bad_hour[7] = {0x00u, 0x00u, 0x24u, 0x01u, 0x01u, 0x01u, 0x00u};
    assert_true(!rv8803_decode_time(bad_hour, &out), "reject hour 24");
}

static void test_encode_alarm(void) {
    uint8_t regs[3];
    rv8803_encode_alarm(7u, 35u, regs);
    assert_true(regs[0] == 0x35u, "alarm minute = BCD 35, AE_M=0");
    assert_true(regs[1] == 0x07u, "alarm hour = BCD 07, AE_H=0");
    assert_true(regs[2] == 0x80u, "alarm week/date masked (AE_WD=1) -> daily");

    rv8803_encode_alarm(23u, 59u, regs);
    assert_true(regs[0] == 0x59u && regs[1] == 0x23u && regs[2] == 0x80u, "alarm 23:59 encodes");
    assert_true((regs[0] & 0x80u) == 0u && (regs[1] & 0x80u) == 0u, "minute/hour AE bits cleared");

    rv8803_encode_snooze_alarm(7u, 35u, regs);
    assert_true(regs[0] == 0x35u, "snooze minute remains BCD 35");
    assert_true(regs[1] == 0x47u, "snooze sets documented GP0 beside BCD hour 07");
    assert_true(regs[2] == 0x80u, "snooze remains a daily HH:MM compare");

    uint8_t hour = 0u;
    uint8_t minute = 0u;
    bool snooze = false;
    assert_true(rv8803_decode_alarm(regs, &hour, &minute, &snooze),
                "tagged snooze alarm decodes");
    assert_true(hour == 7u && minute == 35u && snooze,
                "decode separates GP0 from the BCD hour");

    rv8803_encode_alarm(6u, 31u, regs);
    assert_true(rv8803_decode_alarm(regs, &hour, &minute, &snooze),
                "ordinary alarm decodes");
    assert_true(hour == 6u && minute == 31u && !snooze,
                "ordinary alarm has no snooze marker");

    regs[0] = 0x80u;
    assert_true(!rv8803_decode_alarm(regs, &hour, &minute, &snooze),
                "disabled minute compare is not our alarm shape");
}

static void test_round_up_to_minute(void) {
    assert_true(rv8803_round_up_to_minute(0u) == 0u, "0 stays on boundary");
    assert_true(rv8803_round_up_to_minute(60u) == 60u, "60 stays on boundary");
    assert_true(rv8803_round_up_to_minute(1u) == 60u, "1 -> 60");
    assert_true(rv8803_round_up_to_minute(59u) == 60u, "59 -> 60");
    assert_true(rv8803_round_up_to_minute(300u) == 300u, "300 stays on boundary");
    /* Snooze fired at a minute boundary (21:30:00); the user reacts :20 s later;
     * +300 s lands at :20 of the 5th minute, which rounds up to the next boundary
     * -> 6 minutes after the original fire (the 3210 snooze behaviour). */
    uint32_t armed = 77400u;                /* 21:30:00 = 21*3600 + 30*60, a minute boundary */
    uint32_t now = armed + 20u;             /* user reacts 20 s in */
    uint32_t target = rv8803_round_up_to_minute(now + 300u);
    assert_true(target == armed + 360u, "snooze rounds up to the +6 minute boundary");
}

static void test_encode_timer_seconds(void) {
    uint8_t regs[2];
    /* Counter split per the register map: bits 7:0 at 0x0B, bits 11:8 at 0x0C. */
    assert_true(rv8803_encode_timer_seconds(1u, regs), "1 s encodes");
    assert_true(regs[0] == 0x01u && regs[1] == 0x00u, "1 s -> counter 0x001");
    assert_true(rv8803_encode_timer_seconds(60u, regs), "60 s encodes");
    assert_true(regs[0] == 0x3cu && regs[1] == 0x00u, "60 s -> counter 0x03C");
    assert_true(rv8803_encode_timer_seconds(300u, regs), "300 s encodes");
    assert_true(regs[0] == 0x2cu && regs[1] == 0x01u, "300 s -> counter 0x12C");
    assert_true(rv8803_encode_timer_seconds(4095u, regs), "4095 s encodes");
    assert_true(regs[0] == 0xffu && regs[1] == 0x0fu, "4095 s -> counter 0xFFF");
    assert_true((regs[1] & 0xf0u) == 0u, "counter high nibble stays clear");

    assert_true(!rv8803_encode_timer_seconds(0u, regs), "0 s rejected");
    assert_true(!rv8803_encode_timer_seconds(4096u, regs), "4096 s rejected (12-bit counter)");
    assert_true(!rv8803_encode_timer_seconds(0xffffu, regs), "65535 s rejected");
}

int main(void) {
    test_bcd_round_trip();
    test_time_round_trip();
    test_decode_masks_and_rejects();
    test_encode_alarm();
    test_round_up_to_minute();
    test_encode_timer_seconds();
    if (s_failures != 0) {
        fprintf(stderr, "%d failures\n", s_failures);
        return 1;
    }
    printf("rv8803_regs tests passed\n");
    return 0;
}

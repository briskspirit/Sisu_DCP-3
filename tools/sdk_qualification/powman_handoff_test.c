#include <stdio.h>

#include "hardware/powman.h"
#include "pico/stdlib.h"

static void checkpoint(void) {
    uint64_t now = powman_timer_get_ms();
    powman_timer_set_ms(now);
    if (!powman_timer_is_running()) powman_timer_start();
}

static bool check_progress(uint64_t before, uint64_t after) {
    return after >= before + 8u && after <= before + 30u;
}

int main(void) {
    powman_disable_alarm_wakeup();
    const uint32_t calibration = powman_timer_get_lposc_calib_freq();
    for (unsigned use_checkpoint = 0; use_checkpoint < 2; use_checkpoint++) {
        powman_timer_set_1khz_tick_source_xosc();
        powman_timer_set_ms(UINT64_C(0xffffffc0));
        powman_timer_start();
        for (unsigned i = 0; i < 200; i++) {
            uint64_t before = powman_timer_get_ms();
            if (use_checkpoint) checkpoint();
            /* Alternate calibration by one hertz to cover both the SDK's
             * frequency-change pause and unchanged-frequency source switch. */
            uint32_t hz = calibration + ((i / 2u) & 1u);
            powman_timer_set_1khz_tick_source_lposc_with_hz(hz);
            uint64_t switched = powman_timer_get_ms();
            sleep_ms(10);
            uint64_t after = powman_timer_get_ms();
            if (switched < before || !check_progress(switched, after)) {
                printf("POWMAN LPOSC failed: checkpoint=%u cycle=%u before=%llu switched=%llu after=%llu\n",
                       use_checkpoint, i, (unsigned long long)before,
                       (unsigned long long)switched, (unsigned long long)after);
                return 1;
            }

            if (use_checkpoint) checkpoint();
            powman_timer_set_1khz_tick_source_xosc();
            switched = powman_timer_get_ms();
            sleep_ms(10);
            uint64_t end = powman_timer_get_ms();
            if (switched < after || !check_progress(switched, end) ||
                !powman_timer_is_running()) {
                printf("POWMAN XOSC failed: checkpoint=%u cycle=%u after=%llu switched=%llu end=%llu\n",
                       use_checkpoint, i, (unsigned long long)after,
                       (unsigned long long)switched, (unsigned long long)end);
                return 1;
            }
            if ((i % 25u) == 0u) {
                printf("POWMAN progress: checkpoint=%u cycle=%u count=%llu\n",
                       use_checkpoint, i, (unsigned long long)end);
            }
        }
        if (powman_timer_get_ms() <= UINT64_C(0x100000000)) return 2;
        printf("POWMAN passed: checkpoint=%u cycles=200 rollover=1 calibration=%lu\n",
               use_checkpoint, (unsigned long)calibration);
    }
    return 0;
}

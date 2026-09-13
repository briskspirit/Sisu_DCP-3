#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "hal/battery_runtime_logic.h"

static void test_sample_modes(void) {
    assert(battery_sample_mode_select(false, false) ==
           BATTERY_SAMPLE_MODE_QUIET);
    assert(battery_sample_mode_select(false, true) ==
           BATTERY_SAMPLE_MODE_AWAKE);
    assert(battery_sample_mode_select(true, false) ==
           BATTERY_SAMPLE_MODE_HIGH_LOAD);
    assert(battery_sample_mode_select(true, true) ==
           BATTERY_SAMPLE_MODE_HIGH_LOAD);

    assert(battery_sample_period_ms(BATTERY_SAMPLE_MODE_QUIET, false) ==
           BATTERY_SAMPLE_QUIET_MS);
    assert(battery_sample_period_ms(BATTERY_SAMPLE_MODE_AWAKE, false) ==
           BATTERY_SAMPLE_AWAKE_MS);
    assert(battery_sample_period_ms(BATTERY_SAMPLE_MODE_HIGH_LOAD, false) ==
           BATTERY_SAMPLE_HIGH_LOAD_MS);
    assert(battery_sample_period_ms(BATTERY_SAMPLE_MODE_QUIET, true) ==
           BATTERY_SAMPLE_AWAKE_MS);
    assert(battery_sample_period_ms((battery_sample_mode_t)99, false) ==
           BATTERY_SAMPLE_QUIET_MS);
}

int main(void) {
    test_sample_modes();
    puts("battery runtime policy tests passed");
    return 0;
}

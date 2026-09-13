#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "hal/battery_poll_logic.h"

static void test_normal_cadence_and_fast_confirmation(void) {
    battery_poll_schedule_t schedule;
    battery_poll_schedule_init(&schedule);

    assert(battery_poll_status_due(&schedule, 0u));
    assert(battery_poll_adc_due(&schedule, 0u));

    battery_poll_note_status(&schedule, 0u, false);
    battery_poll_note_adc(&schedule, 0u, false);
    assert(!battery_poll_status_due(&schedule, 999u));
    assert(battery_poll_status_due(&schedule, 1000u));
    assert(!battery_poll_adc_due(&schedule, 59999u));
    assert(battery_poll_adc_due(&schedule, 60000u));

    battery_poll_note_status(&schedule, 1000u, true);
    assert(battery_poll_adc_due(&schedule, 1000u));
    battery_poll_note_adc(&schedule, 1000u, true);
    assert(!battery_poll_adc_due(&schedule, 1079u));
    assert(battery_poll_adc_due(&schedule, 1080u));

    battery_poll_note_adc(&schedule, 1080u, false);
    battery_poll_request_adc(&schedule, 2000u);
    assert(battery_poll_adc_due(&schedule, 2000u));
}

static void test_deadlines_cross_uint32_wrap(void) {
    battery_poll_schedule_t schedule;
    battery_poll_schedule_init(&schedule);
    uint32_t start = UINT32_MAX - 39u;

    battery_poll_note_status(&schedule, start, false);
    assert(!battery_poll_status_due(&schedule, 959u));
    assert(battery_poll_status_due(&schedule, 960u));

    battery_poll_note_adc(&schedule, start, true);
    assert(!battery_poll_adc_due(&schedule, 39u));
    assert(battery_poll_adc_due(&schedule, 40u));
}

int main(void) {
    test_normal_cadence_and_fast_confirmation();
    test_deadlines_cross_uint32_wrap();
    puts("battery polling schedule tests passed");
    return 0;
}

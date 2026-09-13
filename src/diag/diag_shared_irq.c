/* Rev B2 GP42 wired-OR arbiter diagnostic. */

#include <stdio.h>

#include "hal/board.h"
#include "hal/ltc2959_hal.h"
#include "hal/rtc_alarm_hal.h"
#include "hal/tca8418_hal.h"
#include "pico/stdlib.h"
#include "services/shared_irq_service.h"

int main(void) {
    board_set_system_clock();
    board_init();
    board_3v8_rail_set_enabled(false);
    stdio_init_all();

    bool tca_ok = tca8418_hal_init();
    rtc_alarm_hal_init();
    uint32_t now_ms = to_ms_since_boot(get_absolute_time());
    bool ltc_ok = ltc2959_hal_init(now_ms);
    shared_irq_service_init(now_ms);
    printf("[irq] Rev B2 shared IRQ diagnostic TCA=%u RTC=%u LTC=%u; "
           "a=force LTC alert, d=drain\n",
           tca_ok ? 1u : 0u,
           rtc_alarm_hal_chip_available() ? 1u : 0u,
           ltc_ok ? 1u : 0u);

    uint32_t next_print_ms = now_ms;
    while (true) {
        now_ms = to_ms_since_boot(get_absolute_time());
        /* Mirror production ordering so the GP42 arbiter sees an LTC alert
         * before the periodic gauge poll can consume its clear-on-read STATUS. */
        shared_irq_service_poll(now_ms);
        ltc2959_hal_poll(now_ms, LTC2959_SAMPLE_DEFAULT_PERIOD_MS);
        (void)tca8418_hal_key_state();

        int ch = getchar_timeout_us(0);
        if (ch == 'a' || ch == 'A') {
            printf("[irq] force LTC=%u\n",
                   ltc2959_hal_debug_force_voltage_alert()
                       ? 1u
                       : 0u);
        } else if (ch == 'd' || ch == 'D') {
            shared_irq_drain_result_t drain;
            bool ok = shared_irq_service_drain_now(&drain);
            printf("[irq] drain ok=%u r=%u svc=%02x err=%02x "
                   "released=%u stuck=%u\n",
                   ok ? 1u : 0u,
                   (unsigned)drain.rounds,
                   (unsigned)drain.serviced_mask,
                   (unsigned)drain.error_mask,
                   drain.line_released ? 1u : 0u,
                   drain.stuck ? 1u : 0u);
        }

        if ((int32_t)(now_ms - next_print_ms) >= 0) {
            next_print_ms = now_ms + 500u;
            shared_irq_service_snapshot_t s;
            shared_irq_service_get_snapshot(&s);
            printf("[irq] drains=%lu rounds=%lu stuck=%lu "
                   "T=%lu/%lu/%lu R=%lu/%lu L=%lu/%lu "
                   "err=%lu/%lu/%lu last=%02x/%02x/%02x/%02x/%02x\n",
                   (unsigned long)s.drain_count,
                   (unsigned long)s.total_rounds,
                   (unsigned long)s.stuck_count,
                   (unsigned long)s.tca_service_count,
                   (unsigned long)s.tca_key_events,
                   (unsigned long)s.tca_gpi_events,
                   (unsigned long)s.rtc_alarm_events,
                   (unsigned long)s.rtc_timer_events,
                   (unsigned long)s.ltc_alerts,
                   (unsigned long)s.ltc_ara_failures,
                   (unsigned long)s.source_errors[0],
                   (unsigned long)s.source_errors[1],
                   (unsigned long)s.source_errors[2],
                   (unsigned)s.last_serviced_mask,
                   (unsigned)s.last_error_mask,
                   (unsigned)s.last_tca_int_status,
                   (unsigned)s.last_rtc_flags,
                   (unsigned)s.last_ltc_status);
        }
        sleep_ms(10u);
    }
}

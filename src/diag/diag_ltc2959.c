/* Rev B2 LTC2959 coherent-snapshot and SMBus-alert diagnostic. */

#include <stdio.h>

#include "hal/board.h"
#include "hal/board_irq_hal.h"
#include "hal/ltc2959_hal.h"
#include "pico/stdlib.h"

static void print_snapshot(void) {
    ltc2959_snapshot_t s;
    ltc2959_hal_get_snapshot(&s);
    printf("[ltc] p=%u cfg=%u valid=%u cont=%u v=%u i=%ld "
           "acr=%08lx dq=%lld temp=%ld st=%02x seq=%lu "
           "session=%lu i2c=%lu ara=%lu/%u/%u/%ld/%02x line=%u\n",
           s.present ? 1u : 0u,
           s.configured ? 1u : 0u,
           s.sample_valid ? 1u : 0u,
           s.continuity_valid ? 1u : 0u,
           (unsigned)s.voltage_mv,
           (long)s.current_ua,
           (unsigned long)s.acr_raw,
           (long long)s.session_delta_nah,
           (long)s.temperature_mdegc,
           (unsigned)s.status_latched,
           (unsigned long)s.sample_sequence,
           (unsigned long)s.gauge_session,
           (unsigned long)s.i2c_error_count,
           (unsigned long)s.ara_error_count,
           s.ara_last_transport_ok ? 1u : 0u,
           s.ara_last_valid ? 1u : 0u,
           (long)s.ara_last_read_result,
           (unsigned)s.ara_last_response,
           board_irq_hal_shared_asserted() ? 1u : 0u);
}

int main(void) {
    board_set_system_clock();
    board_init();
    board_3v8_rail_set_enabled(false);
    board_irq_hal_init();
    stdio_init_all();

    uint32_t now_ms = to_ms_since_boot(get_absolute_time());
    bool init_ok = ltc2959_hal_init(now_ms);
    printf("[ltc] Rev B2 LTC2959 diagnostic init=%u; "
           "a=force alert, s=service alert\n",
           init_ok ? 1u : 0u);

    uint32_t next_print_ms = now_ms;
    while (true) {
        now_ms = to_ms_since_boot(get_absolute_time());
        ltc2959_hal_poll(now_ms, LTC2959_SAMPLE_DEFAULT_PERIOD_MS);
        int ch = getchar_timeout_us(0);
        if (ch == 'a' || ch == 'A') {
            printf("[ltc] force=%u\n",
                   ltc2959_hal_debug_force_voltage_alert()
                       ? 1u
                       : 0u);
        } else if (ch == 's' || ch == 'S' ||
                   board_irq_hal_shared_asserted()) {
            ltc2959_irq_result_t result;
            ltc2959_hal_service_alert(&result);
            printf("[ltc] alert ok=%u work=%u more=%u st=%02x ara=%u\n",
                   result.transport_ok ? 1u : 0u,
                   result.did_work ? 1u : 0u,
                   result.more_work ? 1u : 0u,
                   (unsigned)result.status,
                   result.ara_released ? 1u : 0u);
        }
        if ((int32_t)(now_ms - next_print_ms) >= 0) {
            next_print_ms = now_ms + 500u;
            print_snapshot();
        }
        sleep_ms(10u);
    }
}

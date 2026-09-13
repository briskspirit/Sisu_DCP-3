/* Rev B2 PMIC rail-only diagnostic. The cellular control FETs remain
 * deasserted for the entire program; only GP6, GP4, GP33, and GP40 are used. */

#include <stdio.h>

#include "hal/board.h"
#include "hal/modem_power_monitor_hal.h"
#include "pico/stdlib.h"

/* [BP] Bench timeout for TPS63020 PG assertion/deassertion. It is deliberately
 * bounded and diagnostic-only until Rev B2 measurements replace the value. */
#define DIAG_PMIC_PG_TIMEOUT_MS 250u

static bool wait_for_pg(bool expected) {
    absolute_time_t deadline =
        make_timeout_time_ms(DIAG_PMIC_PG_TIMEOUT_MS);
    while (!time_reached(deadline)) {
        if (board_3v8_rail_power_good() == expected) {
            return true;
        }
        sleep_ms(1u);
    }
    return board_3v8_rail_power_good() == expected;
}

static void run_rail_test(bool force_pwm) {
    board_3v8_rail_set_enabled(false);
    board_3v8_rail_set_force_pwm(force_pwm);
    board_3v8_rail_set_enabled(true);
    bool pg_up = wait_for_pg(true);

    uint16_t status_raw = 0u;
    uint16_t status_mv = 0u;
    (void)modem_power_monitor_hal_read(&status_raw, &status_mv);
    printf("[power] up pwm=%u pg=%u status_adc=%u status_pin=%umV "
           "on=%u shdn=%u\n",
           force_pwm ? 1u : 0u,
           pg_up ? 1u : 0u,
           (unsigned)status_raw,
           (unsigned)status_mv,
           board_modem_on_off_asserted() ? 1u : 0u,
           board_modem_hw_shutdown_asserted() ? 1u : 0u);

    board_3v8_rail_set_enabled(false);
    board_3v8_rail_set_force_pwm(false);
    bool pg_released = wait_for_pg(false);
    printf("[power] down pg=%u released=%u en=%u pwm=%u on=%u shdn=%u\n",
           board_3v8_rail_power_good() ? 1u : 0u,
           pg_released ? 1u : 0u,
           board_3v8_rail_enabled() ? 1u : 0u,
           board_3v8_rail_force_pwm_enabled() ? 1u : 0u,
           board_modem_on_off_asserted() ? 1u : 0u,
           board_modem_hw_shutdown_asserted() ? 1u : 0u);
}

int main(void) {
    board_set_system_clock();
    board_init();
    board_3v8_rail_set_enabled(false);
    board_3v8_rail_set_force_pwm(false);
    modem_power_monitor_hal_init();
    stdio_init_all();

    printf("[power] Rev B2 rail-only diagnostic\n");
    printf("[power] p=power-save rail test, f=forced-PWM rail test, "
           "o=force rail off\n");
    while (true) {
        int ch = getchar_timeout_us(0);
        if (ch == 'p' || ch == 'P') {
            run_rail_test(false);
        } else if (ch == 'f' || ch == 'F') {
            run_rail_test(true);
        } else if (ch == 'o' || ch == 'O') {
            board_3v8_rail_set_enabled(false);
            board_3v8_rail_set_force_pwm(false);
            printf("[power] forced off\n");
        }
        sleep_ms(10u);
    }
}

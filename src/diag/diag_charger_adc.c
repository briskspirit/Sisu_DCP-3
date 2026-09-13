/* Rev B2 Nokia-charger and battery-gauge observation diagnostic. */

#include <stdio.h>

#include "hal/battery_hal.h"
#include "hal/board.h"
#include "hal/ltc2959_hal.h"
#include "hal/tca8418_hal.h"
#include "pico/stdlib.h"

#define CHARGER_ENABLE_ARM_MS 10000u
#define CHARGER_ENABLE_WINDOW_MS 60000u

static bool s_charger_enabled;
static bool s_enable_armed;
static uint32_t s_enable_arm_deadline_ms;
static uint32_t s_enable_deadline_ms;

static void disable_charger(const char *reason) {
    bool ok = tca8418_hal_set_charger_enabled(false);
    s_charger_enabled = false;
    s_enable_armed = false;
    printf("[charger] disabled reason=%s write=%u\n", reason, ok ? 1u : 0u);
}

int main(void) {
    board_set_system_clock();
    board_init();
    board_3v8_rail_set_enabled(false);
    stdio_init_all();

    bool tca_ok = tca8418_hal_init();
    bool disable_ok = tca_ok && tca8418_hal_set_charger_enabled(false);
    bool nimh_ok = tca_ok && tca8418_hal_set_charger_li_ion_mode(false);
    s_charger_enabled = false;
    s_enable_armed = false;
    battery_hal_init();
    printf("[charger] Rev B2 charger/ADC diagnostic, TCA=%u disable=%u "
           "NiMH=%u\n",
           tca_ok ? 1u : 0u, disable_ok ? 1u : 0u, nimh_ok ? 1u : 0u);
    printf("[charger] SAFE DEFAULT: /CE high (disabled). "
           "a=arm, e=enable 60s, d=disable\n");

    uint32_t next_print_ms = 0u;
    while (true) {
        uint32_t now_ms =
            to_ms_since_boot(get_absolute_time());
        int ch = getchar_timeout_us(0);
        if (ch == 'a' || ch == 'A') {
            s_enable_armed = true;
            s_enable_arm_deadline_ms = now_ms + CHARGER_ENABLE_ARM_MS;
            printf("[charger] enable ARMED for %ums; press e to enable\n",
                   (unsigned)CHARGER_ENABLE_ARM_MS);
        } else if (ch == 'e' || ch == 'E') {
            if (!s_enable_armed ||
                (int32_t)(now_ms - s_enable_arm_deadline_ms) >= 0) {
                s_enable_armed = false;
                printf("[charger] enable REFUSED; press a then e\n");
            } else if (tca8418_hal_set_charger_enabled(true)) {
                s_enable_armed = false;
                s_charger_enabled = true;
                s_enable_deadline_ms = now_ms + CHARGER_ENABLE_WINDOW_MS;
                printf("[charger] ENABLED for at most %ums\n",
                       (unsigned)CHARGER_ENABLE_WINDOW_MS);
            } else {
                s_enable_armed = false;
                printf("[charger] enable FAILED\n");
            }
        } else if (ch == 'd' || ch == 'D') {
            disable_charger("command");
        }
        if (s_enable_armed &&
            (int32_t)(now_ms - s_enable_arm_deadline_ms) >= 0) {
            s_enable_armed = false;
            printf("[charger] enable arm expired\n");
        }
        if (s_charger_enabled &&
            (int32_t)(now_ms - s_enable_deadline_ms) >= 0) {
            disable_charger("timeout");
        }
        battery_hal_poll(now_ms);
        if ((int32_t)(now_ms - next_print_ms) >= 0) {
            next_print_ms = now_ms + 500u;
            ltc2959_snapshot_t ltc;
            ltc2959_hal_get_snapshot(&ltc);
            bool stat1_high = true;
            bool stat2_high = true;
            bool stat1_ok = tca8418_hal_input_level(
                TCA8418_PIN_COL3_CHR_STAT1, &stat1_high);
            bool stat2_ok = tca8418_hal_input_level(
                TCA8418_PIN_COL4_CHR_STAT2, &stat2_high);
            printf("[charger] ce=%s armed=%u present=%u vin=%lumV adc=%u pin=%umV "
                   "pins=%u%u/%u stat=%u valid=%u | ltc=%u cfg=%u sample=%u "
                   "v=%umV i=%lduA st=%02x\n",
                   s_charger_enabled ? "EN" : "DIS",
                   s_enable_armed ? 1u : 0u,
                   battery_hal_charger_connected() ? 1u : 0u,
                   (unsigned long)battery_hal_charger_input_mv(),
                   (unsigned)battery_hal_charger_adc_raw(),
                   (unsigned)battery_hal_charger_pin_mv(),
                   stat1_high ? 1u : 0u,
                   stat2_high ? 1u : 0u,
                   stat1_ok && stat2_ok ? 1u : 0u,
                   (unsigned)battery_hal_charge_status(),
                   battery_hal_charge_status_valid() ? 1u : 0u,
                   ltc.present ? 1u : 0u,
                   ltc.configured ? 1u : 0u,
                   ltc.sample_valid ? 1u : 0u,
                   (unsigned)ltc.voltage_mv,
                   (long)ltc.current_ua,
                   (unsigned)ltc.status_latched);
        }
        sleep_ms(10u);
    }
}

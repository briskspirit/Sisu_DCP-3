#include "hal/modem_power_monitor_hal.h"

#include <stddef.h>

#include "hal/board.h"
#include "hal/modem_power_monitor_logic.h"
#include "hardware/adc.h"
#include "hardware/gpio.h"

#define MODEM_POWER_MONITOR_SETTLE_SAMPLES 4u
#define MODEM_POWER_MONITOR_BURST_SAMPLES 8u
void modem_power_monitor_hal_init(void) {
#if MODEM_STATUS_MONITOR_AVAILABLE
    adc_init();
    adc_gpio_init(MODEM_PIN_STATUS_ADC);
    gpio_set_input_enabled(MODEM_PIN_STATUS_ADC, false);
#endif
}

bool modem_power_monitor_hal_read(uint16_t *raw, uint16_t *pin_mv) {
    if (raw == NULL || pin_mv == NULL) {
        return false;
    }
#if !MODEM_STATUS_MONITOR_AVAILABLE
    *raw = 0u;
    *pin_mv = 0u;
    return false;
#else
    adc_select_input(MODEM_STATUS_ADC_INPUT);
    for (uint8_t i = 0u;
         i < MODEM_POWER_MONITOR_SETTLE_SAMPLES;
         i++) {
        (void)adc_read();
    }
    uint32_t sum = 0u;
    for (uint8_t i = 0u;
         i < MODEM_POWER_MONITOR_BURST_SAMPLES;
         i++) {
        sum += adc_read();
    }
    *raw = (uint16_t)(
        sum / MODEM_POWER_MONITOR_BURST_SAMPLES
    );
    *pin_mv = modem_power_monitor_mv_from_raw(*raw);
    return true;
#endif
}

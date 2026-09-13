#include "hal/modem_power_monitor_logic.h"

#define MODEM_POWER_MONITOR_ADC_MAX 4095u
#define MODEM_POWER_MONITOR_ADC_REF_MV 3300u

uint16_t modem_power_monitor_mv_from_raw(uint16_t raw) {
    if (raw > MODEM_POWER_MONITOR_ADC_MAX) {
        raw = MODEM_POWER_MONITOR_ADC_MAX;
    }
    return (uint16_t)(
        ((uint32_t)raw * MODEM_POWER_MONITOR_ADC_REF_MV +
         (MODEM_POWER_MONITOR_ADC_MAX / 2u)) /
        MODEM_POWER_MONITOR_ADC_MAX
    );
}

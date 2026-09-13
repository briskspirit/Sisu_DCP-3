#ifndef MODEM_POWER_MONITOR_HAL_H
#define MODEM_POWER_MONITOR_HAL_H

#include <stdbool.h>
#include <stdint.h>

void modem_power_monitor_hal_init(void);
bool modem_power_monitor_hal_read(uint16_t *raw, uint16_t *pin_mv);

#endif

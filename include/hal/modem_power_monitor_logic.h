#ifndef MODEM_POWER_MONITOR_LOGIC_H
#define MODEM_POWER_MONITOR_LOGIC_H

#include <stdint.h>

/* Pure 12-bit ADC conversion. GP40 remains an analog observation only;
 * no Boolean modem-on threshold is inferred from this voltage. */
uint16_t modem_power_monitor_mv_from_raw(uint16_t raw);

#endif

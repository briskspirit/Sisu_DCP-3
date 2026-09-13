#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "hal/modem_power_monitor_logic.h"

int main(void) {
    assert(modem_power_monitor_mv_from_raw(0u) == 0u);
    assert(modem_power_monitor_mv_from_raw(2048u) == 1650u);
    assert(modem_power_monitor_mv_from_raw(4095u) == 3300u);
    assert(modem_power_monitor_mv_from_raw(UINT16_MAX) == 3300u);
    puts("modem power monitor tests passed");
    return 0;
}

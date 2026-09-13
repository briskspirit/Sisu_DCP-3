#include <assert.h>
#include <stdio.h>

#include "hal/bq25171_stat.h"

/* BQ25171-Q1 Table 7-4 STAT decode and the device->generic state bridge. */
int main(void) {
    assert(bq25171_stat_decode(true, true) == BATTERY_CHARGE_IDLE);
    assert(bq25171_stat_decode(true, false) == BATTERY_CHARGE_ACTIVE);
    assert(bq25171_stat_decode(false, true) == BATTERY_CHARGE_FAULT);
    assert(bq25171_stat_decode(false, false) == BATTERY_CHARGE_FAULT);

    assert(bq25171_charger_state_from_inputs(false, BATTERY_CHARGE_ACTIVE) ==
           BATTERY_CHARGER_DETACHED);
    assert(bq25171_charger_state_from_inputs(true, BATTERY_CHARGE_ACTIVE) ==
           BATTERY_CHARGER_ACTIVE);
    assert(bq25171_charger_state_from_inputs(true, BATTERY_CHARGE_IDLE) ==
           BATTERY_CHARGER_FULL);
    assert(bq25171_charger_state_from_inputs(true, BATTERY_CHARGE_FAULT) ==
           BATTERY_CHARGER_FAULT);

    puts("bq25171 stat decode tests passed");
    return 0;
}

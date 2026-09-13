#include "hal/bq25171_stat.h"

battery_charge_status_t bq25171_stat_decode(bool stat1_high, bool stat2_high) {
    if (stat1_high && !stat2_high) {
        return BATTERY_CHARGE_ACTIVE;
    }
    if (!stat1_high) {
        return BATTERY_CHARGE_FAULT;
    }
    return BATTERY_CHARGE_IDLE;
}

battery_charger_state_t bq25171_charger_state_from_inputs(
    bool charger_present,
    battery_charge_status_t status) {
    if (!charger_present) {
        return BATTERY_CHARGER_DETACHED;
    }
    switch (status) {
    case BATTERY_CHARGE_ACTIVE:
        return BATTERY_CHARGER_ACTIVE;
    case BATTERY_CHARGE_FAULT:
        return BATTERY_CHARGER_FAULT;
    case BATTERY_CHARGE_IDLE:
    default:
        return BATTERY_CHARGER_FULL;
    }
}

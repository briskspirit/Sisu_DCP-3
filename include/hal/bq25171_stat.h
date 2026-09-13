#ifndef HAL_BQ25171_STAT_H
#define HAL_BQ25171_STAT_H

#include <stdbool.h>

#include "services/battery_charge_logic.h"

/* BQ25171-Q1 charger STAT1/STAT2 pin decode (Table 7-4). GPIO levels are true
 * when the open-drain output is released/high. This is the device contract;
 * the generic charger-state model and retention policy live in
 * services/battery_charge_logic.h and never see pin levels. */
typedef enum {
    BATTERY_CHARGE_IDLE = 0,
    BATTERY_CHARGE_ACTIVE,
    BATTERY_CHARGE_FAULT,
} battery_charge_status_t;

/* (1,0) active; (0,1)/(0,0) fault; (1,1) idle or complete. STAT alone cannot
 * distinguish full-while-plugged from unplugged (both read (1,1)) -- that
 * distinction needs a separate charger-presence sense. */
battery_charge_status_t bq25171_stat_decode(bool stat1_high, bool stat2_high);

/* Device status + charger-presence sense -> the generic charger state the
 * policy layer (services/battery_charge_logic.h) consumes. */
battery_charger_state_t bq25171_charger_state_from_inputs(
    bool charger_present,
    battery_charge_status_t status);

#endif

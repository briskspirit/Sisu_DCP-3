#ifndef BATTERY_CHARGE_LOGIC_H
#define BATTERY_CHARGE_LOGIC_H

#include <stdbool.h>
#include <stdint.h>

/* Generic charger state and the app's charge-retention policy. This module
 * never sees a device: battery_hal decodes the BQ25171 STAT pins
 * (hal/bq25171_stat.h) and maps them to battery_charger_state_t; only that
 * neutral state enters here. */
typedef enum {
    BATTERY_CHARGER_DETACHED = 0,
    BATTERY_CHARGER_ACTIVE,
    BATTERY_CHARGER_FULL,
    BATTERY_CHARGER_FAULT,
} battery_charger_state_t;

typedef struct {
    bool active;
    bool completed;
} battery_charge_transition_t;

/* Board-1 unplug reaches stable DETACHED within about 650 ms, but STAT changes
 * to IDLE first. Require continuous full evidence for comfortably longer than
 * that hardware tail before publishing a completed charge. */
#define BATTERY_CHARGE_COMPLETE_CONFIRM_MS 2000u

typedef struct {
    bool completion_pending;
    uint32_t completion_since_ms;
} battery_charge_transition_state_t;

void battery_charge_transition_init(
    battery_charge_transition_state_t *state);
/* Convert possibly transient charger observations into the app's retained
 * charging state. Completion requires continuous, valid FULL evidence for the
 * confirmation interval; detach, fault, invalid evidence, or resumed charging
 * cancels the candidate. */
battery_charge_transition_t battery_charge_transition(
    battery_charge_transition_state_t *state,
    bool status_valid,
    battery_charger_state_t observed_state,
    bool was_active,
    uint32_t now_ms);

#endif

#include "services/battery_charge_logic.h"

#include <stddef.h>

void battery_charge_transition_init(
    battery_charge_transition_state_t *state) {
    if (state == NULL) {
        return;
    }
    state->completion_pending = false;
    state->completion_since_ms = 0u;
}

battery_charge_transition_t battery_charge_transition(
    battery_charge_transition_state_t *state,
    bool status_valid,
    battery_charger_state_t observed_state,
    bool was_active,
    uint32_t now_ms) {
    battery_charge_transition_t transition = {
        .active = false,
        .completed = false,
    };

    if (observed_state == BATTERY_CHARGER_DETACHED) {
        battery_charge_transition_init(state);
        return transition;
    }
    if (!status_valid) {
        battery_charge_transition_init(state);
        transition.active = was_active;
        return transition;
    }
    if (observed_state == BATTERY_CHARGER_ACTIVE) {
        battery_charge_transition_init(state);
        transition.active = true;
        return transition;
    }
    if (observed_state != BATTERY_CHARGER_FULL || !was_active) {
        battery_charge_transition_init(state);
        return transition;
    }

    /* Keep the charging UI alive while FULL is only a candidate. A real
     * unplug reaches DETACHED and cancels this path before the dwell expires. */
    transition.active = true;
    if (state == NULL) {
        return transition;
    }
    if (!state->completion_pending) {
        state->completion_pending = true;
        state->completion_since_ms = now_ms;
        return transition;
    }
    if ((uint32_t)(now_ms - state->completion_since_ms) >=
        BATTERY_CHARGE_COMPLETE_CONFIRM_MS) {
        battery_charge_transition_init(state);
        transition.active = false;
        transition.completed = true;
    }
    return transition;
}

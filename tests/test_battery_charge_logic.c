#include <assert.h>
#include <stdio.h>

#include "services/battery_charge_logic.h"

int main(void) {
    battery_charge_transition_state_t state;
    battery_charge_transition_init(&state);

    battery_charge_transition_t transition =
        battery_charge_transition(
            &state, true, BATTERY_CHARGER_ACTIVE, false, 0u);
    assert(transition.active);
    assert(!transition.completed);
    assert(!state.completion_pending);

    transition = battery_charge_transition(
        &state, false, BATTERY_CHARGER_FULL, true, 10u);
    assert(transition.active);
    assert(!transition.completed);
    assert(!state.completion_pending);

    transition = battery_charge_transition(
        &state, true, BATTERY_CHARGER_FULL, true, 20u);
    assert(transition.active);
    assert(!transition.completed);
    assert(state.completion_pending);

    transition = battery_charge_transition(
        &state, true, BATTERY_CHARGER_FULL, true,
        20u + BATTERY_CHARGE_COMPLETE_CONFIRM_MS - 1u);
    assert(transition.active);
    assert(!transition.completed);
    assert(state.completion_pending);

    transition = battery_charge_transition(
        &state, true, BATTERY_CHARGER_FULL, true,
        20u + BATTERY_CHARGE_COMPLETE_CONFIRM_MS);
    assert(!transition.active);
    assert(transition.completed);
    assert(!state.completion_pending);

    transition = battery_charge_transition(
        &state, true, BATTERY_CHARGER_FAULT, true, 3000u);
    assert(!transition.active);
    assert(!transition.completed);

    transition = battery_charge_transition(
        &state, false, BATTERY_CHARGER_DETACHED, true, 3010u);
    assert(!transition.active);
    assert(!transition.completed);

    /* An unplug during the confirmation dwell never reports completion. */
    transition = battery_charge_transition(
        &state, true, BATTERY_CHARGER_ACTIVE, false, 4000u);
    assert(transition.active);
    transition = battery_charge_transition(
        &state, true, BATTERY_CHARGER_FULL, true, 4010u);
    assert(transition.active);
    assert(state.completion_pending);
    transition = battery_charge_transition(
        &state, true, BATTERY_CHARGER_DETACHED, true, 4500u);
    assert(!transition.active);
    assert(!transition.completed);
    assert(!state.completion_pending);

    /* Fault or resumed charge cancels a completion candidate. */
    transition = battery_charge_transition(
        &state, true, BATTERY_CHARGER_ACTIVE, false, 5000u);
    assert(transition.active);
    transition = battery_charge_transition(
        &state, true, BATTERY_CHARGER_FULL, true, 5010u);
    assert(state.completion_pending);
    transition = battery_charge_transition(
        &state, true, BATTERY_CHARGER_FAULT, true, 5100u);
    assert(!transition.active);
    assert(!transition.completed);
    assert(!state.completion_pending);

    transition = battery_charge_transition(
        &state, true, BATTERY_CHARGER_ACTIVE, false, 5200u);
    assert(transition.active);
    transition = battery_charge_transition(
        &state, true, BATTERY_CHARGER_FULL, true, 5210u);
    assert(state.completion_pending);
    transition = battery_charge_transition(
        &state, true, BATTERY_CHARGER_ACTIVE, true, 5300u);
    assert(transition.active);
    assert(!transition.completed);
    assert(!state.completion_pending);

    /* A pack already full at insertion never fabricates a completion edge. */
    transition = battery_charge_transition(
        &state, true, BATTERY_CHARGER_FULL, false, 5400u);
    assert(!transition.active);
    assert(!transition.completed);
    assert(!state.completion_pending);

    /* The confirmation deadline remains correct across uint32_t wrap. */
    uint32_t wrap_start = UINT32_MAX - 999u;
    transition = battery_charge_transition(
        &state, true, BATTERY_CHARGER_ACTIVE, false, wrap_start - 1u);
    assert(transition.active);
    transition = battery_charge_transition(
        &state, true, BATTERY_CHARGER_FULL, true, wrap_start);
    assert(transition.active);
    assert(state.completion_pending);
    transition = battery_charge_transition(
        &state, true, BATTERY_CHARGER_FULL, true,
        999u);
    assert(transition.active);
    assert(!transition.completed);
    transition = battery_charge_transition(
        &state, true, BATTERY_CHARGER_FULL, true,
        1000u);
    assert(!transition.active);
    assert(transition.completed);

    puts("battery charge logic tests passed");
    return 0;
}

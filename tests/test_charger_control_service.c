#include <assert.h>
#include <stdio.h>

#include "services/charger_control_service.h"

static bool s_actual_enabled;
static bool s_set_ok;
static bool s_get_ok;
static bool s_readback_override;
static bool s_readback_value;
static uint32_t s_set_calls;
static uint32_t s_get_calls;

bool tca8418_hal_set_charger_enabled(bool enabled) {
    s_set_calls++;
    if (s_set_ok) {
        s_actual_enabled = enabled;
    }
    return s_set_ok;
}

bool tca8418_hal_get_charger_enabled(bool *enabled) {
    s_get_calls++;
    if (enabled != NULL && s_get_ok) {
        *enabled = s_readback_override ? s_readback_value
                                       : s_actual_enabled;
    }
    return s_get_ok;
}

static charger_control_snapshot_t snapshot(void) {
    charger_control_snapshot_t out;
    charger_control_service_get_snapshot(&out);
    return out;
}

static void reset_fixture_with_mask(uint8_t initial_mask) {
    s_actual_enabled = false;
    s_set_ok = true;
    s_get_ok = true;
    s_readback_override = false;
    s_readback_value = false;
    s_set_calls = 0u;
    s_get_calls = 0u;
    charger_control_service_init(1000u, initial_mask);
}

static void reset_fixture(void) {
    reset_fixture_with_mask(0u);
}

static void test_boot_inhibit_is_applied_on_first_output(void) {
    reset_fixture_with_mask(CHARGER_INHIBIT_SUPERVISOR);
    charger_control_snapshot_t state = snapshot();
    assert(state.inhibit_owner_mask == CHARGER_INHIBIT_SUPERVISOR);
    assert(!state.requested_enabled);
    assert(!state.actual_enabled);
    assert(state.readback_valid);
    assert(s_set_calls == 1u);
    assert(s_get_calls == 1u);
}

static void test_additive_ownership(void) {
    reset_fixture();
    charger_control_snapshot_t state = snapshot();
    assert(state.requested_enabled && state.actual_enabled);
    assert(state.readback_valid);
    assert(state.inhibit_owner_mask == 0u);

    assert(charger_control_service_set_inhibit(
        CHARGER_INHIBIT_DEBUG, true));
    state = snapshot();
    assert(!state.requested_enabled && !state.actual_enabled);
    assert(state.inhibit_owner_mask == CHARGER_INHIBIT_DEBUG);
    uint32_t writes = s_set_calls;

    assert(charger_control_service_set_inhibit(
        CHARGER_INHIBIT_SUPERVISOR, true));
    assert(snapshot().inhibit_owner_mask ==
           (CHARGER_INHIBIT_DEBUG | CHARGER_INHIBIT_SUPERVISOR));
    assert(s_set_calls == writes);

    assert(charger_control_service_set_inhibit(
        CHARGER_INHIBIT_DEBUG, false));
    state = snapshot();
    assert(state.inhibit_owner_mask == CHARGER_INHIBIT_SUPERVISOR);
    assert(!state.requested_enabled && !state.actual_enabled);
    assert(s_set_calls == writes);

    assert(charger_control_service_set_inhibit(
        CHARGER_INHIBIT_SUPERVISOR, false));
    state = snapshot();
    assert(state.inhibit_owner_mask == 0u);
    assert(state.requested_enabled && state.actual_enabled);
    assert(state.owner_transitions == 4u);
}

static void test_failures_retry_without_losing_target(void) {
    reset_fixture();
    s_set_ok = false;
    assert(!charger_control_service_set_inhibit(
        CHARGER_INHIBIT_DEBUG, true));
    charger_control_snapshot_t state = snapshot();
    assert(!state.requested_enabled);
    assert(!state.readback_valid);
    assert(state.apply_failures == 1u);

    s_set_ok = true;
    charger_control_service_poll(1001u);
    assert(!snapshot().readback_valid);
    charger_control_service_poll(2000u);
    state = snapshot();
    assert(state.readback_valid && !state.actual_enabled);
    assert(state.inhibit_owner_mask == CHARGER_INHIBIT_DEBUG);

    s_get_ok = false;
    assert(!charger_control_service_set_inhibit(
        CHARGER_INHIBIT_DEBUG, false));
    assert(snapshot().requested_enabled);
    assert(!snapshot().readback_valid);
    s_get_ok = true;
    charger_control_service_poll(3000u);
    assert(snapshot().readback_valid && snapshot().actual_enabled);
}

static void test_readback_mismatch_and_periodic_poll(void) {
    reset_fixture();
    uint32_t writes = s_set_calls;
    uint32_t reads = s_get_calls;
    charger_control_service_poll(1999u);
    assert(s_set_calls == writes);
    charger_control_service_poll(2000u);
    assert(s_set_calls == writes);
    assert(s_get_calls == reads + 1u);

    s_readback_override = true;
    s_readback_value = false;
    charger_control_service_poll(3000u);
    charger_control_snapshot_t state = snapshot();
    assert(state.readback_valid);
    assert(state.requested_enabled);
    assert(!state.actual_enabled);
    assert(state.readback_mismatches == 1u);
    assert(s_set_calls == writes + 1u);
}

static void test_invalid_owner_is_inert(void) {
    reset_fixture();
    charger_control_snapshot_t before = snapshot();
    assert(!charger_control_service_set_inhibit(
        (charger_inhibit_owner_t)0u, true));
    assert(!charger_control_service_set_inhibit(
        (charger_inhibit_owner_t)(CHARGER_INHIBIT_DEBUG |
                                  CHARGER_INHIBIT_SUPERVISOR), true));
    charger_control_snapshot_t after = snapshot();
    assert(after.inhibit_owner_mask == before.inhibit_owner_mask);
    assert(after.owner_transitions == before.owner_transitions);
    charger_control_service_get_snapshot(NULL);
}

int main(void) {
    test_boot_inhibit_is_applied_on_first_output();
    test_additive_ownership();
    test_failures_retry_without_losing_target();
    test_readback_mismatch_and_periodic_poll();
    test_invalid_owner_is_inert();
    puts("charger control service tests passed");
    return 0;
}

#include "services/charger_control_service.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>

#include "hal/tca8418_hal.h"

#define CHARGER_CONTROL_POLL_MS 1000u

static charger_control_snapshot_t s_snapshot;
static uint32_t s_next_poll_ms;

static uint32_t saturating_increment_u32(uint32_t value) {
    return value == UINT32_MAX ? value : value + 1u;
}

static bool owner_valid(charger_inhibit_owner_t owner) {
    uint8_t value = (uint8_t)owner;
    return value != 0u && (value & (uint8_t)(value - 1u)) == 0u &&
        (value & (uint8_t)~CHARGER_INHIBIT_VALID_MASK) == 0u;
}

static bool apply_target(void) {
    s_snapshot.apply_attempts = saturating_increment_u32(
        s_snapshot.apply_attempts);
    s_snapshot.readback_valid = false;
    if (!tca8418_hal_set_charger_enabled(s_snapshot.requested_enabled)) {
        s_snapshot.apply_failures = saturating_increment_u32(
            s_snapshot.apply_failures);
        return false;
    }
    bool actual = false;
    if (!tca8418_hal_get_charger_enabled(&actual)) {
        s_snapshot.apply_failures = saturating_increment_u32(
            s_snapshot.apply_failures);
        return false;
    }
    s_snapshot.actual_enabled = actual;
    s_snapshot.readback_valid = true;
    if (actual != s_snapshot.requested_enabled) {
        s_snapshot.readback_mismatches = saturating_increment_u32(
            s_snapshot.readback_mismatches);
        return false;
    }
    return true;
}

void charger_control_service_init(uint32_t now_ms,
                                  uint8_t initial_inhibit_owner_mask) {
    memset(&s_snapshot, 0, sizeof(s_snapshot));
    s_snapshot.inhibit_owner_mask = (uint8_t)(
        initial_inhibit_owner_mask & CHARGER_INHIBIT_VALID_MASK);
    s_snapshot.requested_enabled =
        s_snapshot.inhibit_owner_mask == 0u;
    s_snapshot.actual_enabled = s_snapshot.requested_enabled;
    s_next_poll_ms = now_ms + CHARGER_CONTROL_POLL_MS;
    (void)apply_target();
}

void charger_control_service_poll(uint32_t now_ms) {
    if ((int32_t)(now_ms - s_next_poll_ms) < 0) {
        return;
    }
    s_next_poll_ms = now_ms + CHARGER_CONTROL_POLL_MS;
    bool actual = false;
    if (tca8418_hal_get_charger_enabled(&actual)) {
        s_snapshot.actual_enabled = actual;
        s_snapshot.readback_valid = true;
        if (actual == s_snapshot.requested_enabled) {
            return;
        }
    } else {
        s_snapshot.readback_valid = false;
    }
    (void)apply_target();
}

bool charger_control_service_set_inhibit(charger_inhibit_owner_t owner,
                                         bool inhibit) {
    if (!owner_valid(owner)) {
        return false;
    }
    uint8_t old_mask = s_snapshot.inhibit_owner_mask;
    uint8_t owner_bit = (uint8_t)owner;
    uint8_t new_mask = inhibit
        ? (uint8_t)(old_mask | owner_bit)
        : (uint8_t)(old_mask & (uint8_t)~owner_bit);
    if (new_mask != old_mask) {
        s_snapshot.inhibit_owner_mask = new_mask;
        s_snapshot.owner_transitions = saturating_increment_u32(
            s_snapshot.owner_transitions);
    }
    bool new_target = new_mask == 0u;
    bool target_changed = new_target != s_snapshot.requested_enabled;
    s_snapshot.requested_enabled = new_target;
    if (!target_changed && s_snapshot.readback_valid &&
        s_snapshot.actual_enabled == new_target) {
        return true;
    }
    return apply_target();
}

void charger_control_service_get_snapshot(charger_control_snapshot_t *out) {
    if (out != NULL) {
        *out = s_snapshot;
    }
}

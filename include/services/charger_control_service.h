#ifndef CHARGER_CONTROL_SERVICE_H
#define CHARGER_CONTROL_SERVICE_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    CHARGER_INHIBIT_SUPERVISOR = 1u << 0,
    CHARGER_INHIBIT_DEBUG = 1u << 1,
    CHARGER_INHIBIT_FAULT = 1u << 2,
} charger_inhibit_owner_t;

#define CHARGER_INHIBIT_VALID_MASK \
    (CHARGER_INHIBIT_SUPERVISOR | CHARGER_INHIBIT_DEBUG | \
     CHARGER_INHIBIT_FAULT)

typedef struct {
    uint8_t inhibit_owner_mask;
    bool requested_enabled;
    bool actual_enabled;
    bool readback_valid;
    uint32_t owner_transitions;
    uint32_t apply_attempts;
    uint32_t apply_failures;
    uint32_t readback_mismatches;
} charger_control_snapshot_t;

void charger_control_service_init(uint32_t now_ms,
                                  uint8_t initial_inhibit_owner_mask);
void charger_control_service_poll(uint32_t now_ms);

/* Inhibit ownership is additive. Clearing one owner cannot restart charging
 * while any other owner remains. Returns true only when hardware readback
 * confirms the resulting effective target. */
bool charger_control_service_set_inhibit(charger_inhibit_owner_t owner,
                                         bool inhibit);

void charger_control_service_get_snapshot(charger_control_snapshot_t *out);

#endif

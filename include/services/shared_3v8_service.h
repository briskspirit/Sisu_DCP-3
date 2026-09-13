#ifndef SHARED_3V8_SERVICE_H
#define SHARED_3V8_SERVICE_H

#include <stdbool.h>
#include <stdint.h>

/* +3V8 feeds both the cellular module supply and the magnetic buzzer. Merely
 * raising it does not start the modem; modem ON/OFF sequencing remains wholly
 * owned by modem_service. All calls are core0-only. */
typedef enum {
    SHARED_3V8_OWNER_MODEM = 1u << 0,
    SHARED_3V8_OWNER_AUDIO = 1u << 1,
    /* RAM-only bench hold. Production code must never acquire this owner. */
    SHARED_3V8_OWNER_DIAGNOSTIC = 1u << 2,
} shared_3v8_owner_t;

/* Call once after board_init(). Starts with no owners and the rail disabled. */
void shared_3v8_service_init(void);

/* Acquire/release one owner's level-held requirement. The physical rail is on
 * iff at least one owner is present, so one client can never cut another off. */
void shared_3v8_service_set_required(shared_3v8_owner_t owner, bool required);

/* Bounded readiness wait for a client that must not proceed before PMIC PG.
 * Returns immediately when PG is already high. */
bool shared_3v8_service_wait_power_good(uint32_t timeout_us);

uint8_t shared_3v8_service_owner_mask(void);
bool shared_3v8_service_enabled(void);
/* Monotonic (wrapping) count of manager-owned physical on/off transitions.
 * Consumers use equality snapshots, so wrap is harmless. */
uint32_t shared_3v8_service_transition_count(void);

/* Point-of-no-return shutdown helper. Clears all ownership and forces +3V8
 * down; callers must ensure no client can resume afterwards. */
void shared_3v8_service_force_off(void);

#endif

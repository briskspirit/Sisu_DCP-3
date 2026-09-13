#ifndef SERVICES_RUNTIME_WATCHDOG_H
#define SERVICES_RUNTIME_WATCHDOG_H

#include <stdbool.h>
#include <stdint.h>

#define RUNTIME_WATCHDOG_MAIN_TIMEOUT_MS 12000u
#define RUNTIME_WATCHDOG_FLASH_TIMEOUT_MS 3000u

typedef enum {
    RUNTIME_WATCHDOG_PHASE_NONE = 0,
    RUNTIME_WATCHDOG_PHASE_BOOT = 1,
    RUNTIME_WATCHDOG_PHASE_MAIN_LOOP = 2,
    RUNTIME_WATCHDOG_PHASE_FLASH = 3,
    RUNTIME_WATCHDOG_PHASE_POWER_OFF = 4,
    RUNTIME_WATCHDOG_PHASE_STANDBY = 5,
    RUNTIME_WATCHDOG_PHASE_IDLE = 6,
    RUNTIME_WATCHDOG_PHASE_DEBUG_HANG = 7,
    RUNTIME_WATCHDOG_PHASE_COUNT,
} runtime_watchdog_phase_t;

typedef struct {
    bool timeout_reset;
    bool phase_valid;
    runtime_watchdog_phase_t phase;
    uint32_t raw_phase_stamp;
} runtime_watchdog_boot_evidence_t;

/* Call before normal initialization touches the inherited watchdog. It captures
 * the previous runtime timeout marker, then disarms any ROM/reflash timer. */
void runtime_watchdog_boot_capture(void);
void runtime_watchdog_get_boot_evidence(
    runtime_watchdog_boot_evidence_t *out);

/* Arm only after fallible boot initialization is complete. The main loop feeds
 * once after completing a healthy scheduling turn; phase notes do not feed. */
void runtime_watchdog_start(void);
bool runtime_watchdog_started(void);
void runtime_watchdog_note_phase(runtime_watchdog_phase_t phase);
void runtime_watchdog_feed(runtime_watchdog_phase_t phase);

/* Flash owns a shorter temporary lease while both cores are excluded from XIP.
 * End restores the runtime lease if it was active, or disarms a standalone
 * early-boot flash lease. */
void runtime_watchdog_flash_begin(void);
void runtime_watchdog_flash_checkpoint(void);
void runtime_watchdog_flash_end(void);

const char *runtime_watchdog_phase_name(runtime_watchdog_phase_t phase);

#endif

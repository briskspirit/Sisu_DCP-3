#include "services/runtime_watchdog.h"

#include <stddef.h>

#include "hardware/structs/watchdog.h"
#include "hardware/watchdog.h"

#define RUNTIME_WATCHDOG_PHASE_MAGIC 0x53495300u
#define RUNTIME_WATCHDOG_PHASE_MASK 0xffffff00u
#define RUNTIME_WATCHDOG_PHASE_SCRATCH 5u

_Static_assert(RUNTIME_WATCHDOG_MAIN_TIMEOUT_MS <= 16000u,
               "runtime watchdog must fit RP2350's hardware limit");
_Static_assert(RUNTIME_WATCHDOG_FLASH_TIMEOUT_MS <
                   RUNTIME_WATCHDOG_MAIN_TIMEOUT_MS,
               "flash watchdog lease must remain the stricter bound");

static bool s_boot_captured;
static bool s_started;
static uint8_t s_flash_depth;
static runtime_watchdog_boot_evidence_t s_boot_evidence;

static bool valid_phase(runtime_watchdog_phase_t phase) {
    return phase > RUNTIME_WATCHDOG_PHASE_NONE &&
           phase < RUNTIME_WATCHDOG_PHASE_COUNT;
}

static uint32_t phase_stamp(runtime_watchdog_phase_t phase) {
    uint8_t encoded = valid_phase(phase) ? (uint8_t)phase : 0u;
    return RUNTIME_WATCHDOG_PHASE_MAGIC | encoded;
}

static bool phase_from_stamp(uint32_t stamp,
                             runtime_watchdog_phase_t *phase) {
    runtime_watchdog_phase_t decoded =
        (runtime_watchdog_phase_t)(stamp & 0xffu);
    bool valid = (stamp & RUNTIME_WATCHDOG_PHASE_MASK) ==
                     RUNTIME_WATCHDOG_PHASE_MAGIC &&
                 valid_phase(decoded);
    if (phase != NULL) {
        *phase = valid ? decoded : RUNTIME_WATCHDOG_PHASE_NONE;
    }
    return valid;
}

static void publish_phase(runtime_watchdog_phase_t phase) {
    watchdog_hw->scratch[RUNTIME_WATCHDOG_PHASE_SCRATCH] =
        phase_stamp(phase);
}

void runtime_watchdog_boot_capture(void) {
    if (s_boot_captured) {
        return;
    }

    s_boot_evidence.raw_phase_stamp =
        watchdog_hw->scratch[RUNTIME_WATCHDOG_PHASE_SCRATCH];
    s_boot_evidence.timeout_reset = watchdog_enable_caused_reboot();
    s_boot_evidence.phase_valid =
        s_boot_evidence.timeout_reset &&
        phase_from_stamp(s_boot_evidence.raw_phase_stamp,
                         &s_boot_evidence.phase);
    if (!s_boot_evidence.phase_valid) {
        s_boot_evidence.phase = RUNTIME_WATCHDOG_PHASE_NONE;
    }

    watchdog_disable();
    s_started = false;
    s_flash_depth = 0u;
    s_boot_captured = true;
}

void runtime_watchdog_get_boot_evidence(
    runtime_watchdog_boot_evidence_t *out) {
    if (out != NULL) {
        *out = s_boot_evidence;
    }
}

void runtime_watchdog_start(void) {
    if (!s_boot_captured) {
        runtime_watchdog_boot_capture();
    }
    if (s_started) {
        return;
    }
    publish_phase(RUNTIME_WATCHDOG_PHASE_BOOT);
    watchdog_enable(RUNTIME_WATCHDOG_MAIN_TIMEOUT_MS, true);
    s_started = true;
}

bool runtime_watchdog_started(void) {
    return s_started;
}

void runtime_watchdog_note_phase(runtime_watchdog_phase_t phase) {
    if (s_started && s_flash_depth == 0u) {
        publish_phase(phase);
    }
}

void runtime_watchdog_feed(runtime_watchdog_phase_t phase) {
    if (!s_started || s_flash_depth != 0u) {
        return;
    }
    publish_phase(phase);
    watchdog_update();
}

void runtime_watchdog_flash_begin(void) {
    if (s_flash_depth != 0u) {
        if (s_flash_depth != UINT8_MAX) {
            s_flash_depth++;
        }
        publish_phase(RUNTIME_WATCHDOG_PHASE_FLASH);
        watchdog_update();
        return;
    }
    s_flash_depth = 1u;
    publish_phase(RUNTIME_WATCHDOG_PHASE_FLASH);
    watchdog_enable(RUNTIME_WATCHDOG_FLASH_TIMEOUT_MS, true);
}

void runtime_watchdog_flash_checkpoint(void) {
    if (s_flash_depth == 0u) {
        return;
    }
    publish_phase(RUNTIME_WATCHDOG_PHASE_FLASH);
    watchdog_update();
}

void runtime_watchdog_flash_end(void) {
    if (s_flash_depth == 0u) {
        return;
    }
    s_flash_depth--;
    if (s_flash_depth != 0u) {
        publish_phase(RUNTIME_WATCHDOG_PHASE_FLASH);
        watchdog_update();
        return;
    }
    if (s_started) {
        publish_phase(RUNTIME_WATCHDOG_PHASE_MAIN_LOOP);
        watchdog_enable(RUNTIME_WATCHDOG_MAIN_TIMEOUT_MS, true);
    } else {
        watchdog_disable();
    }
}

const char *runtime_watchdog_phase_name(runtime_watchdog_phase_t phase) {
    switch (phase) {
    case RUNTIME_WATCHDOG_PHASE_BOOT:
        return "boot";
    case RUNTIME_WATCHDOG_PHASE_MAIN_LOOP:
        return "main";
    case RUNTIME_WATCHDOG_PHASE_FLASH:
        return "flash";
    case RUNTIME_WATCHDOG_PHASE_POWER_OFF:
        return "power-off";
    case RUNTIME_WATCHDOG_PHASE_STANDBY:
        return "standby";
    case RUNTIME_WATCHDOG_PHASE_IDLE:
        return "idle";
    case RUNTIME_WATCHDOG_PHASE_DEBUG_HANG:
        return "debug-hang";
    default:
        return "unknown";
    }
}

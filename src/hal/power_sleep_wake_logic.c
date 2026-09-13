#include "hal/power_sleep_hal.h"

#include <stddef.h>

/* RP2350 A4 board evidence establishes that LAST_SWCORE_PWRUP is a source-bit
 * field despite the datasheet register table describing enum values 0..6:
 * PWRUP0 reports 0x02, PWRUP1 reports 0x04, and multiple sources may coexist
 * (0x06 was observed). Keep this pure and host-tested; transient STATUS is
 * still accepted as a second source of evidence. */
#define POWER_SLEEP_PWRUP_STATUS_BITS (1u << 9)
#define POWER_SLEEP_LAST_PWRUP_BUTTON_BITS (1u << 1)
#define POWER_SLEEP_LAST_PWRUP_SHARED_IRQ_BITS (1u << 2)
#define POWER_SLEEP_LAST_PWRUP_SERVICE_VBUS_BITS (1u << 3)

void power_sleep_hal_decode_wake_sources(
    const power_sleep_hal_wake_evidence_t *evidence,
    uint32_t last_swcore_pwrup,
    power_sleep_hal_wake_sources_t *out) {
    if (evidence == NULL || out == NULL) {
        return;
    }
    out->button =
        (evidence->button & POWER_SLEEP_PWRUP_STATUS_BITS) != 0u ||
        (last_swcore_pwrup & POWER_SLEEP_LAST_PWRUP_BUTTON_BITS) != 0u;
    out->shared_irq =
        (evidence->shared_irq & POWER_SLEEP_PWRUP_STATUS_BITS) != 0u ||
        (last_swcore_pwrup & POWER_SLEEP_LAST_PWRUP_SHARED_IRQ_BITS) != 0u;
    out->service_vbus =
        (evidence->service_vbus & POWER_SLEEP_PWRUP_STATUS_BITS) != 0u ||
        (last_swcore_pwrup & POWER_SLEEP_LAST_PWRUP_SERVICE_VBUS_BITS) != 0u;
}

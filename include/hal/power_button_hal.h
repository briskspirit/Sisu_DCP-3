#ifndef POWER_BUTTON_HAL_H
#define POWER_BUTTON_HAL_H

#include <stdbool.h>
#include <stdint.h>

#include "services/event_queue.h"

#define POWER_BUTTON_POWER_ON_HOLD_MS 1200u

typedef struct {
    bool stable_pressed;
    bool hold_sent;
    bool release_pending;
    uint32_t pressed_since_ms;
    uint32_t release_since_ms;
    uint32_t hold_ms;
    uint32_t release_debounce_ms;
} power_button_t;

void power_button_init(power_button_t *button);
bool power_button_scan_raw(void);
void power_button_feed(power_button_t *button, bool raw_pressed, event_queue_t *queue, uint32_t now_ms);
/* Dormant-wake hold credit: the press that woke the chip predates this boot,
 * so backdate it (pressed_since_ms ~ 0 = power-up) and mark the press stable.
 * The 1.2 s power-on hold then measures from the physical press, keeping the
 * wake-to-on gesture identical to the soft-off one. No EVENT_KEY_DOWN is
 * synthesized (power_app ignores it in the off route anyway). */
void power_button_seed_held(power_button_t *button, uint32_t pressed_since_ms);

#endif

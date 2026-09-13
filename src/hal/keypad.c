#include "hal/keypad.h"

#include <string.h>

#include "hal/tca8418_hal.h"
#include "services/log.h"
#include "services/timebase.h"
#include "pico/stdlib.h"

#define KEYPAD_INIT_BOOT_ATTEMPTS 3u
#define KEYPAD_INIT_BOOT_RETRY_MS 2u
/* Four stock C-hold recordings place the clear-all click at a 497 ms median.
 * A 500 ms one-shot threshold matches that behavior and applies uniformly to
 * the original's other hold gestures (speed dial, numeric entry, etc.). */
#define KEYPAD_HOLD_MS 500u

void keypad_init(keypad_t *keypad) {
    memset(keypad, 0, sizeof(*keypad));
    keypad->hold_ms = KEYPAD_HOLD_MS;
    keypad->release_debounce_ms = 48;

    /* A transient I2C glitch before rails settle can fail the first init; retry a
     * few times here, and tca8418_hal_key_state() keeps retrying during scan. */
    bool ok = false;
    for (uint8_t attempt = 0u; attempt < KEYPAD_INIT_BOOT_ATTEMPTS && !ok; attempt++) {
        ok = tca8418_hal_init();
        if (!ok) {
            sleep_ms(KEYPAD_INIT_BOOT_RETRY_MS);
        }
    }
    if (!ok) {
        LOGW("keypad", "TCA8418 init failed at boot; retrying during scan");
    }
}

uint16_t keypad_scan_raw(void) {
    return tca8418_hal_key_state();
}

void keypad_feed(keypad_t *keypad, uint16_t raw_state, event_queue_t *queue, uint32_t now_ms) {
    for (uint8_t bit = 0; bit < 16; bit++) {
        uint16_t mask = (uint16_t)(1u << bit);
        bool raw_pressed = (raw_state & mask) != 0;
        bool stable_pressed = (keypad->stable_state & mask) != 0;
        if (stable_pressed) {
            if (raw_pressed) {
                keypad->release_pending &= (uint16_t)~mask;
                if ((keypad->hold_sent & mask) == 0 &&
                    time_diff_ms(now_ms, keypad->pressed_since[bit]) >= (int32_t)keypad->hold_ms) {
                    keypad->hold_sent |= mask;
                    event_queue_push(queue, EVENT_KEY_HOLD, mask, 0, now_ms);
                }
            } else if ((keypad->release_pending & mask) == 0) {
                keypad->release_pending |= mask;
                keypad->release_since[bit] = now_ms;
            } else if (time_diff_ms(now_ms, keypad->release_since[bit]) >= (int32_t)keypad->release_debounce_ms) {
                keypad->stable_state &= (uint16_t)~mask;
                keypad->release_pending &= (uint16_t)~mask;
                keypad->hold_sent &= (uint16_t)~mask;
                keypad->pressed_since[bit] = 0;
                event_queue_push(queue, EVENT_KEY_UP, mask, 0, now_ms);
            }
        } else if (raw_pressed) {
            keypad->stable_state |= mask;
            keypad->pressed_since[bit] = now_ms;
            keypad->release_pending &= (uint16_t)~mask;
            keypad->hold_sent &= (uint16_t)~mask;
            event_queue_push(queue, EVENT_KEY_DOWN, mask, 0, now_ms);
        }
    }
}

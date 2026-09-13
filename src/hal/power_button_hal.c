#include "hal/power_button_hal.h"

#include <string.h>

#include "hardware/gpio.h"
#include "hal/board.h"
#include "hal/keypad.h"
#include "services/timebase.h"

#define POWER_BUTTON_RELEASE_DEBOUNCE_MS 48u

void power_button_init(power_button_t *button) {
    memset(button, 0, sizeof(*button));
    button->hold_ms = POWER_BUTTON_POWER_ON_HOLD_MS;
    button->release_debounce_ms = POWER_BUTTON_RELEASE_DEBOUNCE_MS;

    gpio_init(POWER_BUTTON_PIN);
    gpio_set_dir(POWER_BUTTON_PIN, GPIO_IN);
    gpio_pull_up(POWER_BUTTON_PIN);
}

bool power_button_scan_raw(void) {
    return !gpio_get(POWER_BUTTON_PIN);
}

void power_button_seed_held(power_button_t *button, uint32_t pressed_since_ms) {
    button->stable_pressed = true;
    button->release_pending = false;
    button->hold_sent = false;
    button->pressed_since_ms = pressed_since_ms;
}

void power_button_feed(power_button_t *button, bool raw_pressed, event_queue_t *queue, uint32_t now_ms) {
    if (button->stable_pressed) {
        if (raw_pressed) {
            button->release_pending = false;
            if (!button->hold_sent &&
                time_diff_ms(now_ms, button->pressed_since_ms) >= (int32_t)button->hold_ms) {
                button->hold_sent = true;
                event_queue_push(queue, EVENT_KEY_HOLD, KEY_POWER, 0, now_ms);
            }
            return;
        }
        if (!button->release_pending) {
            button->release_pending = true;
            button->release_since_ms = now_ms;
            return;
        }
        if (time_diff_ms(now_ms, button->release_since_ms) >= (int32_t)button->release_debounce_ms) {
            button->stable_pressed = false;
            button->release_pending = false;
            button->hold_sent = false;
            button->pressed_since_ms = 0;
            event_queue_push(queue, EVENT_KEY_UP, KEY_POWER, 0, now_ms);
        }
        return;
    }

    if (raw_pressed) {
        button->stable_pressed = true;
        button->pressed_since_ms = now_ms;
        button->release_pending = false;
        button->hold_sent = false;
        event_queue_push(queue, EVENT_KEY_DOWN, KEY_POWER, 0, now_ms);
    }
}

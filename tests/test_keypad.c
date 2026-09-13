#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "hal/keypad.h"
#include "hal/tca8418_hal.h"
#include "services/log.h"
#include "services/timebase.h"

uint64_t host_test_time_us;

bool tca8418_hal_init(void) {
    return true;
}

uint16_t tca8418_hal_key_state(void) {
    return 0u;
}

void log_write(log_level_t level, const char *tag, const char *fmt, ...) {
    (void)level;
    (void)tag;
    (void)fmt;
}

int32_t time_diff_ms(uint32_t a, uint32_t b) {
    return (int32_t)(a - b);
}

static int failures;

static void check(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        failures++;
    }
}

int main(void) {
    keypad_t keypad;
    event_queue_t queue;
    input_event_t event;

    keypad_init(&keypad);
    event_queue_init(&queue);
    check(keypad.hold_ms == 500u, "stock hold threshold is 500 ms");

    keypad_feed(&keypad, KEY_C, &queue, 1000u);
    check(event_queue_pop(&queue, &event) &&
              event.type == EVENT_KEY_DOWN && event.code == KEY_C &&
              event.when_ms == 1000u,
          "press emits one immediate key-down");

    keypad_feed(&keypad, KEY_C, &queue, 1499u);
    check(event_queue_empty(&queue), "hold does not fire one millisecond early");
    keypad_feed(&keypad, KEY_C, &queue, 1500u);
    check(event_queue_pop(&queue, &event) &&
              event.type == EVENT_KEY_HOLD && event.code == KEY_C &&
              event.when_ms == 1500u,
          "hold fires exactly 500 ms after key-down");
    keypad_feed(&keypad, KEY_C, &queue, 1800u);
    check(event_queue_empty(&queue), "hold is one-shot while the key remains down");

    keypad_feed(&keypad, 0u, &queue, 1800u);
    keypad_feed(&keypad, 0u, &queue, 1847u);
    check(event_queue_empty(&queue), "release remains debounced before 48 ms");
    keypad_feed(&keypad, 0u, &queue, 1848u);
    check(event_queue_pop(&queue, &event) &&
              event.type == EVENT_KEY_UP && event.code == KEY_C &&
              event.when_ms == 1848u,
          "release emits after the existing 48 ms debounce");

    if (failures != 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("PASS: test_keypad\n");
    return 0;
}

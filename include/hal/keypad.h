#ifndef KEYPAD_H
#define KEYPAD_H

#include <stdint.h>

#include "services/event_queue.h"
#include "services/input_keys.h"

typedef struct {
    uint16_t stable_state;
    uint16_t hold_sent;
    uint16_t release_pending;
    uint32_t pressed_since[16];
    uint32_t release_since[16];
    uint32_t hold_ms;
    uint32_t release_debounce_ms;
} keypad_t;

void keypad_init(keypad_t *keypad);
uint16_t keypad_scan_raw(void);
void keypad_feed(keypad_t *keypad, uint16_t raw_state, event_queue_t *queue, uint32_t now_ms);

#endif

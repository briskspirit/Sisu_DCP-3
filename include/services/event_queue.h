#ifndef EVENT_QUEUE_H
#define EVENT_QUEUE_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    EVENT_NONE = 0,
    EVENT_KEY_DOWN,
    EVENT_KEY_UP,
    EVENT_KEY_HOLD,
    EVENT_SERVICE,
} event_type_t;

typedef struct {
    event_type_t type;
    uint16_t code;
    uint16_t arg;
    uint32_t when_ms;
} input_event_t;

#define EVENT_QUEUE_CAPACITY 32u

typedef struct {
    input_event_t items[EVENT_QUEUE_CAPACITY];
    uint8_t head;
    uint8_t tail;
    uint8_t count;
} event_queue_t;

typedef struct {
    uint8_t depth;
    uint8_t high_water;
    uint32_t dropped;
} event_queue_stats_t;

void event_queue_init(event_queue_t *queue);
bool event_queue_push(event_queue_t *queue, event_type_t type, uint16_t code, uint16_t arg, uint32_t when_ms);
bool event_queue_pop(event_queue_t *queue, input_event_t *event);
bool event_queue_empty(const event_queue_t *queue);
void event_queue_get_stats(event_queue_stats_t *out);

#endif

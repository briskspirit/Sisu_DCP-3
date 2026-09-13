#include "services/event_queue.h"

#include <stddef.h>

static event_queue_stats_t s_stats;

void event_queue_init(event_queue_t *queue) {
    queue->head = 0;
    queue->tail = 0;
    queue->count = 0;
    s_stats.depth = 0u;
    s_stats.high_water = 0u;
    s_stats.dropped = 0u;
}

bool event_queue_push(event_queue_t *queue, event_type_t type, uint16_t code, uint16_t arg, uint32_t when_ms) {
    if (queue->count >= EVENT_QUEUE_CAPACITY) {
        if (s_stats.dropped != UINT32_MAX) {
            s_stats.dropped++;
        }
        return false;
    }
    input_event_t *event = &queue->items[queue->tail];
    event->type = type;
    event->code = code;
    event->arg = arg;
    event->when_ms = when_ms;
    queue->tail = (uint8_t)((queue->tail + 1u) % EVENT_QUEUE_CAPACITY);
    queue->count++;
    s_stats.depth = queue->count;
    if (s_stats.high_water < queue->count) {
        s_stats.high_water = queue->count;
    }
    return true;
}

bool event_queue_pop(event_queue_t *queue, input_event_t *event) {
    if (queue->count == 0) {
        return false;
    }
    *event = queue->items[queue->head];
    queue->head = (uint8_t)((queue->head + 1u) % EVENT_QUEUE_CAPACITY);
    queue->count--;
    s_stats.depth = queue->count;
    return true;
}

bool event_queue_empty(const event_queue_t *queue) {
    return queue->count == 0;
}

void event_queue_get_stats(event_queue_stats_t *out) {
    if (out != NULL) {
        *out = s_stats;
    }
}

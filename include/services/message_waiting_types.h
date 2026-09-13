#ifndef MESSAGE_WAITING_TYPES_H
#define MESSAGE_WAITING_TYPES_H

#include <stdbool.h>
#include <stdint.h>

/* Protocol-neutral message-waiting categories. Backends translate their own
 * numeric indicator values at the parser boundary; generic service and UI
 * code must never depend on a modem vendor's numbering. */
typedef enum {
    MODEM_MESSAGE_WAITING_VOICE_LINE_1 = 0,
    MODEM_MESSAGE_WAITING_VOICE_LINE_2,
    MODEM_MESSAGE_WAITING_FAX,
    MODEM_MESSAGE_WAITING_EMAIL,
    MODEM_MESSAGE_WAITING_OTHER,
    MODEM_MESSAGE_WAITING_CATEGORY_COUNT,
    /* A clear with no category is authoritative for the whole snapshot. This
     * is an event-only sentinel and must never index category[]. */
    MODEM_MESSAGE_WAITING_ALL = 0xffu,
} modem_message_waiting_category_t;

typedef uint8_t modem_message_waiting_category_mask_t;

static inline modem_message_waiting_category_mask_t
modem_message_waiting_category_bit(modem_message_waiting_category_t category) {
    uint8_t index = (uint8_t)category;
    return index < MODEM_MESSAGE_WAITING_CATEGORY_COUNT
        ? (modem_message_waiting_category_mask_t)(1u << index)
        : 0u;
}

typedef struct {
    bool active;
    uint16_t count;
} modem_message_waiting_state_t;

typedef struct {
    modem_message_waiting_state_t
        category[MODEM_MESSAGE_WAITING_CATEGORY_COUNT];
} modem_message_waiting_status_t;

#endif /* MESSAGE_WAITING_TYPES_H */

#include "services/modem_line_framer.h"

#include <stddef.h>

#define MODEM_LINE_LENGTH_MASK 0x01ffu
#define MODEM_LINE_OVERFLOW    0x8000u

void modem_line_framer_reset(modem_line_framer_t *framer) {
    if (framer != NULL) {
        framer->state = 0u;
    }
}

modem_line_framer_event_t modem_line_framer_feed(modem_line_framer_t *framer,
                                                  uint8_t byte) {
    if (framer == NULL || byte == '\r') {
        return MODEM_LINE_FRAMER_NONE;
    }
    if (byte == '\n') {
        if ((framer->state & MODEM_LINE_OVERFLOW) != 0u) {
            framer->state = 0u;
            return MODEM_LINE_FRAMER_DROPPED;
        }
        uint16_t length = framer->state & MODEM_LINE_LENGTH_MASK;
        if (length != 0u) {
            framer->line[length] = '\0';
            framer->state = 0u;
            return MODEM_LINE_FRAMER_LINE;
        }
        return MODEM_LINE_FRAMER_NONE;
    }
    if ((framer->state & MODEM_LINE_OVERFLOW) != 0u) {
        return MODEM_LINE_FRAMER_NONE;
    }
    uint16_t length = framer->state & MODEM_LINE_LENGTH_MASK;
    if (length + 1u < MODEM_LINE_FRAMER_CAP) {
        framer->line[length++] =
            (char)(byte < 128u ? byte : (uint8_t)'?');
        framer->state = length;
    } else {
        framer->state = MODEM_LINE_OVERFLOW;
    }
    return MODEM_LINE_FRAMER_NONE;
}

char *modem_line_framer_line(modem_line_framer_t *framer) {
    return framer != NULL ? framer->line : NULL;
}

bool modem_line_framer_pending(const modem_line_framer_t *framer) {
    return framer != NULL && framer->state != 0u;
}

bool modem_line_framer_overflowed(const modem_line_framer_t *framer) {
    return framer != NULL &&
           (framer->state & MODEM_LINE_OVERFLOW) != 0u;
}

#ifndef MODEM_LINE_FRAMER_H
#define MODEM_LINE_FRAMER_H

#include <stdbool.h>
#include <stdint.h>

#define MODEM_LINE_FRAMER_CAP 512u

typedef enum {
    MODEM_LINE_FRAMER_NONE = 0,
    MODEM_LINE_FRAMER_LINE,
    MODEM_LINE_FRAMER_DROPPED,
} modem_line_framer_event_t;

typedef struct {
    char line[MODEM_LINE_FRAMER_CAP];
    /* Low nine bits are the buffered length; bit 15 marks discard-until-LF.
     * One state word avoids padding this always-resident 512-byte object. */
    uint16_t state;
} modem_line_framer_t;

void modem_line_framer_reset(modem_line_framer_t *framer);
modem_line_framer_event_t modem_line_framer_feed(modem_line_framer_t *framer,
                                                  uint8_t byte);
char *modem_line_framer_line(modem_line_framer_t *framer);
bool modem_line_framer_pending(const modem_line_framer_t *framer);
bool modem_line_framer_overflowed(const modem_line_framer_t *framer);

#endif

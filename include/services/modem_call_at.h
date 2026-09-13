#ifndef MODEM_CALL_AT_H
#define MODEM_CALL_AT_H

/* Vendor-neutral 3GPP call-list parsing. The selected modem implements +CLCC;
 * keeping one strict parser prevents bounds, optional-field, or mode-mapping
 * behavior from leaking into the call model. */

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "services/call_types.h"

static inline bool modem_call_at_parse_uint(const char **cursor,
                                            unsigned max_value,
                                            bool comma_required,
                                            unsigned *value_out) {
    if (cursor == NULL || *cursor == NULL || value_out == NULL) {
        return false;
    }
    const char *p = *cursor;
    while (*p == ' ') p++;
    if (*p < '0' || *p > '9') return false;

    unsigned value = 0u;
    do {
        unsigned digit = (unsigned)(*p++ - '0');
        if (digit > max_value || value > (max_value - digit) / 10u) {
            return false;
        }
        value = value * 10u + digit;
    } while (*p >= '0' && *p <= '9');

    while (*p == ' ') p++;
    if (comma_required) {
        if (*p != ',') return false;
        p++;
    } else if (*p != '\0' && *p != ',') {
        return false;
    }
    *cursor = p;
    *value_out = value;
    return true;
}

static inline bool modem_call_at_parse_number(const char **cursor,
                                              char *out,
                                              size_t out_cap) {
    if (cursor == NULL || *cursor == NULL || out == NULL || out_cap == 0u) {
        return false;
    }
    const char *p = *cursor;
    if (*p != ',') return false;
    p++;
    while (*p == ' ') p++;

    size_t len = 0u;
    if (*p == '"') {
        p++;
        while (*p != '\0' && *p != '"') {
            if (len + 1u < out_cap) out[len++] = *p;
            p++;
        }
        if (*p != '"') return false;
        p++;
        while (*p == ' ') p++;
        if (*p != ',') return false;
    } else {
        const char *start = p;
        while (*p != '\0' && *p != ',') p++;
        if (*p != ',') return false;
        const char *end = p;
        while (end > start && end[-1] == ' ') end--;
        while (start < end) {
            if (len + 1u < out_cap) out[len++] = *start;
            start++;
        }
    }
    out[len] = '\0';

    unsigned type = 0u;
    p++;
    if (!modem_call_at_parse_uint(&p, 255u, false, &type)) return false;
    (void)type;
    *cursor = p;
    return true;
}

static inline bool modem_call_at_parse_clcc_row(const char *line,
                                                modem_clcc_row_t *out) {
    static const char prefix[] = "+CLCC:";
    if (line == NULL || out == NULL ||
        strncmp(line, prefix, sizeof(prefix) - 1u) != 0) {
        return false;
    }

    const char *p = line + sizeof(prefix) - 1u;
    unsigned id = 0u;
    unsigned dir = 0u;
    unsigned stat = 0u;
    unsigned mode = 0u;
    unsigned mpty = 0u;
    if (!modem_call_at_parse_uint(&p, MODEM_CALL_ID_MAX, true, &id) ||
        id == 0u ||
        !modem_call_at_parse_uint(&p, 1u, true, &dir) ||
        !modem_call_at_parse_uint(&p, 5u, true, &stat) ||
        !modem_call_at_parse_uint(&p, 9u, true, &mode) ||
        !modem_call_at_parse_uint(&p, 1u, false, &mpty)) {
        return false;
    }
    if (mode != 0u && mode != 1u && mode != 2u && mode != 9u) {
        return false;
    }

    modem_clcc_row_t row;
    memset(&row, 0, sizeof(row));
    row.id = (uint8_t)id;
    row.dir = dir == 0u ? CALL_DIR_MO : CALL_DIR_MT;
    row.mode = mode == 0u ? CALL_MODE_VOICE
                          : (mode == 1u || mode == 2u) ? CALL_MODE_DATA
                                                      : CALL_MODE_UNKNOWN;
    row.mpty = mpty != 0u;
    static const call_leg_state_t stat_map[6] = {
        CALL_LEG_ACTIVE,
        CALL_LEG_HELD,
        CALL_LEG_DIALING,
        CALL_LEG_ALERTING,
        CALL_LEG_INCOMING,
        CALL_LEG_WAITING,
    };
    row.state = stat_map[stat];
    if (*p == ',' &&
        !modem_call_at_parse_number(&p, row.number, sizeof(row.number))) {
        return false;
    }
    if (*p != '\0' && *p != ',') return false;
    *out = row;
    return true;
}

#endif

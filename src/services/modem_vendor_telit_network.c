#include "modem_vendor_telit_internal.h"

#include <stdio.h>
#include <string.h>

static bool build(modem_network_step_t step, const char *plmn,
                  char *out, size_t cap, uint32_t *timeout_ms) {
    if (out == NULL || cap == 0u || timeout_ms == NULL) return false;
    const char *command;
    *timeout_ms = 5000u;
    switch (step) {
    case MODEM_NETWORK_STEP_IDENTITY: command = "AT#CCID"; break;
    case MODEM_NETWORK_STEP_READ: command = "AT+COPS=3,2;+COPS?"; break;
    /* Software User Guide r17, Table 9: COPS test may take 180 seconds.
     * Give the UART final a small margin; selection uses the same bound. */
    case MODEM_NETWORK_STEP_SCAN: command = "AT+COPS=?"; *timeout_ms = 185000u; break;
    case MODEM_NETWORK_STEP_AUTO: command = "AT+COPS=0"; *timeout_ms = 185000u; break;
    case MODEM_NETWORK_STEP_MANUAL: {
        if (!modem_network_plmn_valid(plmn)) return false;
        *timeout_ms = 185000u;
        int n = snprintf(out, cap, "AT+COPS=1,2,\"%s\"", plmn);
        return n >= 0 && (size_t)n < cap;
    }
    default: return false;
    }
    size_t n = strlen(command);
    if (n >= cap) return false;
    memcpy(out, command, n + 1u);
    return true;
}

static modem_network_line_t identity(const char *line, char *out, size_t cap) {
    if (!telit_starts_with(line, "#CCID:")) return MODEM_NETWORK_LINE_IGNORE;
    telit_csv_view_t field; size_t count;
    if (!telit_view_split_prefixed(line, "#CCID:", &field, 1u, &count) || count != 1u ||
        !telit_view_digits_only(field, 19u, 20u) || !telit_view_copy_exact(out, cap, field))
        return MODEM_NETWORK_LINE_INVALID;
    return MODEM_NETWORK_LINE_VALID;
}

static modem_network_line_t read_mode(const char *line, modem_network_mode_t *mode, char plmn[7]) {
    if (!telit_starts_with(line, "+COPS:")) return MODEM_NETWORK_LINE_IGNORE;
    telit_csv_view_t fields[4]; size_t count; uint32_t value, format, act;
    if (!telit_view_split_prefixed(line, "+COPS:", fields, 4u, &count) ||
        count == 0u || count == 2u || !telit_view_parse_u32(fields[0], 5u, &value) || value == 3u)
        return MODEM_NETWORK_LINE_INVALID;
    plmn[0] = '\0';
    if (count >= 3u && (!telit_view_parse_u32(fields[1], 2u, &format) || format != 2u ||
        !telit_view_digits_only(fields[2], 5u, 6u) ||
        !telit_view_copy_exact(plmn, 7u, fields[2]) ||
        (count == 4u && !telit_view_parse_u32(fields[3], 255u, &act))))
        return MODEM_NETWORK_LINE_INVALID;
    *mode = value == 0u ? MODEM_NETWORK_AUTOMATIC : value == 1u ? MODEM_NETWORK_MANUAL :
        value == 5u ? MODEM_NETWORK_UNKNOWN : MODEM_NETWORK_OTHER;
    return MODEM_NETWORK_LINE_VALID;
}

static const char *skip_space(const char *p) {
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

static bool range_tuple(const char *text) {
    bool digit = false;
    for (; *text != '\0'; text++) {
        if (*text >= '0' && *text <= '9') digit = true;
        else if (*text != ',' && *text != '-' && *text != ' ' && *text != '\t') return false;
    }
    return digit;
}

static modem_network_line_t scan(const char *line, modem_network_collect_fn collect) {
    if (!telit_starts_with(line, "+COPS:")) return MODEM_NETWORK_LINE_IGNORE;
    const char *p = skip_space(line + 6);
    bool ranges = false;
    unsigned range_count = 0u;
    if (*p == ',') {
        p = skip_space(p + 1);
        if (*p++ != ',') return MODEM_NETWORK_LINE_INVALID;
        p = skip_space(p);
        ranges = true;
    }
    while (*p != '\0') {
        if (*p != '(') return MODEM_NETWORK_LINE_INVALID;
        const char *start = ++p;
        bool quoted = false;
        while (*p != '\0' && (*p != ')' || quoted)) {
            if (*p == '"') quoted = !quoted;
            if ((unsigned char)*p < 32u && *p != '\t') return MODEM_NETWORK_LINE_INVALID;
            p++;
        }
        size_t length = (size_t)(p - start);
        char tuple[160];
        if (*p != ')' || quoted || length == 0u || length >= sizeof(tuple))
            return MODEM_NETWORK_LINE_INVALID;
        memcpy(tuple, start, length); tuple[length] = '\0';
        if (ranges) {
            if (++range_count > 2u || !range_tuple(tuple)) return MODEM_NETWORK_LINE_INVALID;
        } else {
            telit_csv_view_t fields[5]; size_t count; uint32_t stat, act;
            modem_network_entry_t entry = {0};
            if (!telit_csv_view_split(tuple, fields, 5u, &count) || (count != 4u && count != 5u) ||
                !telit_view_parse_u32(fields[0], 3u, &stat) ||
                !telit_view_digits_only(fields[3], 5u, 6u) ||
                !telit_view_copy_exact(entry.plmn, sizeof(entry.plmn), fields[3]) ||
                !telit_view_copy_exact(entry.name, sizeof(entry.name), fields[1]) ||
                (count == 5u && !telit_view_parse_u32(fields[4], 255u, &act)))
                return MODEM_NETWORK_LINE_INVALID;
            if (entry.name[0] == '\0' &&
                !telit_view_copy_exact(entry.name, sizeof(entry.name), fields[2]))
                return MODEM_NETWORK_LINE_INVALID;
            entry.availability = (uint8_t)stat;
            if (collect == NULL || !collect(&entry)) return MODEM_NETWORK_LINE_INVALID;
        }
        p = skip_space(p + 1);
        if (*p == '\0') break;
        if (*p++ != ',') return MODEM_NETWORK_LINE_INVALID;
        p = skip_space(p);
        if (*p == ',') { ranges = true; p = skip_space(p + 1); }
        if (*p == '\0') return MODEM_NETWORK_LINE_INVALID;
    }
    if (ranges && range_count != 2u) return MODEM_NETWORK_LINE_INVALID;
    return MODEM_NETWORK_LINE_VALID;
}

static bool denied(const char *line) {
    return line != NULL && (strcmp(line, "+CME ERROR: 32") == 0 ||
        strcmp(line, "+CME ERROR: network not allowed - emergency calls only") == 0);
}

const modem_network_backend_t TELIT_NETWORK_BACKEND = {
    .build = build, .identity = identity, .read = read_mode, .scan = scan,
    /* ESC and CR did not abort the WWX scan. Drain the original transaction. */
    .denied = denied, .drain_timeout_ms = 5000u,
};

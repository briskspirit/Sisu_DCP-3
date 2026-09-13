#include "services/modem_line_parser.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "services/modem_at_util.h"

static bool parse_uint_decimal(const char *text, const char **end_out,
                               unsigned *value_out) {
    if (text == NULL || end_out == NULL || value_out == NULL ||
        text[0] < '0' || text[0] > '9') {
        return false;
    }
    unsigned value = 0u;
    const char *p = text;
    while (*p >= '0' && *p <= '9') {
        unsigned digit = (unsigned)(*p - '0');
        if (value > (UINT_MAX - digit) / 10u) {
            return false;
        }
        value = value * 10u + digit;
        p++;
    }
    *end_out = p;
    *value_out = value;
    return true;
}

bool modem_line_parse_csq(const char *line, modem_csq_line_t *out) {
    if (line == NULL || out == NULL) {
        return false;
    }

    static const char prefix[] = "+CSQ:";
    if (strncmp(line, prefix, sizeof(prefix) - 1u) != 0) {
        return false;
    }

    const char *p = line + sizeof(prefix) - 1u;
    while (*p == ' ') {
        p++;
    }
    const char *end = NULL;
    unsigned rssi = 0u;
    if (!parse_uint_decimal(p, &end, &rssi)) {
        return false;
    }
    while (*end == ' ') {
        end++;
    }
    if (*end != ',') {
        return false;
    }

    p = end + 1u;
    while (*p == ' ') {
        p++;
    }
    unsigned ber = 0u;
    if (!parse_uint_decimal(p, &end, &ber)) {
        return false;
    }
    while (*end == ' ') {
        end++;
    }
    if (*end != '\0' || (rssi > 31u && rssi != 99u) ||
        (ber > 7u && ber != 99u)) {
        return false;
    }

    out->rssi = (uint8_t)rssi;
    out->ber = (uint8_t)ber;
    return true;
}

bool modem_line_parse_cereg(const char *line, unsigned *status_out) {
    if (line == NULL || status_out == NULL) {
        return false;
    }
    const char *colon = strchr(line, ':');
    if (colon == NULL) {
        return false;
    }
    const char *p = colon + 1;
    while (*p == ' ') {
        p++;
    }
    const char *end = NULL;
    unsigned status = 0u;
    if (!parse_uint_decimal(p, &end, &status)) {
        return false;
    }
    while (*end == ' ') {
        end++;
    }
    if (*end != '\0' && *end != ',') {
        return false;
    }
    /* Solicited AT+CEREG? is <n>,<stat>; a location-bearing URC has a quoted
     * TAC after its first value and therefore keeps that first value as stat. */
    if (*end == ',') {
        const char *second = end + 1;
        while (*second == ' ') {
            second++;
        }
        if (*second >= '0' && *second <= '9') {
            const char *second_end = NULL;
            unsigned second_status = 0u;
            if (!parse_uint_decimal(second, &second_end, &second_status)) {
                return false;
            }
            while (*second_end == ' ') {
                second_end++;
            }
            if (*second_end != '\0' && *second_end != ',') {
                return false;
            }
            status = second_status;
        } else if (*second != '"') {
            return false;
        }
    }
    *status_out = status;
    return true;
}

void modem_line_parse_cops(const char *line, char *operator_out,
                           size_t operator_cap) {
    if (operator_out == NULL || operator_cap == 0u) {
        return;
    }
    operator_out[0] = '\0';
    if (line == NULL) {
        return;
    }
    const char *first = strchr(line, '"');
    const char *last = first != NULL ? strchr(first + 1, '"') : NULL;
    if (first == NULL || last == NULL || last <= first + 1) {
        return;
    }
    size_t len = (size_t)(last - first - 1);
    if (len >= operator_cap) {
        len = operator_cap - 1u;
    }
    memcpy(operator_out, first + 1, len);
    operator_out[len] = '\0';
}

bool modem_line_parse_cpms(const char *line, uint16_t *used_out,
                           uint16_t *total_out) {
    if (line == NULL || used_out == NULL || total_out == NULL) {
        return false;
    }
    char fields[9][16];
    uint8_t count = 0u;
    const char *colon = strchr(line, ':');
    const char *cursor = colon != NULL ? colon + 1 : line;
    while (count < 9u &&
           modem_at_csv_next_field(&cursor, fields[count],
                                   sizeof(fields[count]))) {
        count++;
    }
    if (count < 9u) {
        return false;
    }
    *used_out = (uint16_t)modem_at_parse_uint_token(fields[7], 0u);
    *total_out = (uint16_t)modem_at_parse_uint_token(fields[8], 0u);
    return true;
}

bool modem_line_parse_cmti(const char *line, const char *expected_storage,
                           uint16_t *index_out) {
    if (line == NULL || expected_storage == NULL || index_out == NULL) {
        return false;
    }
    char storage[16];
    char trailing = '\0';
    unsigned index = 0u;
    int fields = sscanf(line, "+CMTI: \"%15[^\"]\",%u %c",
                        storage, &index, &trailing);
    if (fields != 2 || strcmp(storage, expected_storage) != 0 ||
        index > UINT16_MAX) {
        return false;
    }
    *index_out = (uint16_t)index;
    return true;
}

const char *modem_line_sms_status_from_numeric(unsigned status) {
    static const char *const STATUS[] = {
        "REC UNREAD",
        "REC READ",
        "STO UNSENT",
        "STO SENT",
    };
    return status < (sizeof(STATUS) / sizeof(STATUS[0]))
               ? STATUS[status]
               : NULL;
}

static void copy_quoted_field(const char *first, const char *last,
                              char *out, size_t out_cap) {
    if (first == NULL || last == NULL || out == NULL || out_cap == 0u ||
        last <= first + 1) {
        return;
    }
    size_t len = (size_t)(last - first - 1);
    if (len >= out_cap) {
        len = out_cap - 1u;
    }
    memcpy(out, first + 1, len);
    out[len] = '\0';
}

void modem_line_parse_cmgr_header(const char *line,
                                  char *status_out, size_t status_cap,
                                  char *sender_out, size_t sender_cap,
                                  char *timestamp_out, size_t timestamp_cap) {
    if (line == NULL) {
        return;
    }
    unsigned numeric_status = 0u;
    if (sscanf(line, "+CMGR: %u", &numeric_status) == 1) {
        const char *status =
            modem_line_sms_status_from_numeric(numeric_status);
        if (status != NULL) {
            modem_at_copy_bounded(status_out, status_cap, status);
        }
        return;
    }

    const char *first = strchr(line, '"');
    if (first == NULL) {
        return;
    }
    const char *second = strchr(first + 1, '"');
    copy_quoted_field(first, second, status_out, status_cap);

    const char *third = second != NULL ? strchr(second + 1, '"') : NULL;
    const char *fourth = third != NULL ? strchr(third + 1, '"') : NULL;
    copy_quoted_field(third, fourth, sender_out, sender_cap);

    const char *stamp_first =
        fourth != NULL ? strchr(fourth + 1, '"') : NULL;
    const char *stamp_last =
        stamp_first != NULL ? strchr(stamp_first + 1, '"') : NULL;
    copy_quoted_field(stamp_first, stamp_last, timestamp_out, timestamp_cap);
}

static bool parse_cli_fields(const char *line, unsigned validity_comma,
                             modem_cli_line_t *out) {
    if (line == NULL || out == NULL) {
        return false;
    }
    const char *first = strchr(line, '"');
    const char *second = first != NULL ? strchr(first + 1, '"') : NULL;
    if (first == NULL || second == NULL) {
        return false;
    }
    uint8_t validity = 0u;
    const char *p = second + 1;
    unsigned commas = 0u;
    bool quoted = false;
    for (; *p != '\0'; p++) {
        if (*p == '"') {
            quoted = !quoted;
        } else if (*p == ',' && !quoted) {
            commas++;
            if (commas == validity_comma) {
                const char *v = p + 1;
                while (*v == ' ') {
                    v++;
                }
                if (*v >= '0' && *v <= '2') {
                    validity = (uint8_t)(*v++ - '0');
                    while (*v == ' ') {
                        v++;
                    }
                    if (*v != '\0' && *v != ',') {
                        return false;
                    }
                } else if (*v != '\0' && *v != ',') {
                    return false;
                }
                break;
            }
        }
    }
    if (quoted) {
        return false;
    }
    size_t len = (size_t)(second - first - 1);
    if (len == 0u && validity == 0u) {
        validity = 2u;
    }
    if (len >= sizeof(out->number)) {
        len = sizeof(out->number) - 1u;
    }
    memcpy(out->number, first + 1, len);
    out->number[len] = '\0';
    out->validity = validity;
    return true;
}

bool modem_line_parse_clip(const char *line, modem_cli_line_t *out) {
    /* +CLIP: "<number>",<type>,<subaddr>,<satype>,<alpha>,<validity> */
    return parse_cli_fields(line, 5u, out);
}

bool modem_line_parse_ccwa(const char *line, modem_cli_line_t *out) {
    /* +CCWA: "<number>",<type>,<class>,[<alpha>][,<validity>,...] */
    return parse_cli_fields(line, 4u, out);
}

bool modem_line_is_ccwa_urc(const char *line) {
    if (line == NULL || !modem_at_starts_with(line, "+CCWA:")) {
        return false;
    }
    const char *p = line + strlen("+CCWA:");
    while (*p == ' ') {
        p++;
    }
    /* The query response is "+CCWA: <n>". Only the unsolicited caller-info
     * shape begins with a quoted number. */
    return *p == '"';
}

bool modem_line_is_sim_absent_error(const char *line) {
    if (line == NULL || !modem_at_starts_with(line, "+CME ERROR:")) {
        return false;
    }
    const char *value = line + strlen("+CME ERROR:");
    while (*value == ' ') {
        value++;
    }
    return strcmp(value, "10") == 0 ||
           strcmp(value, "SIM not inserted") == 0 ||
           strcmp(value, "SIM NOT INSERTED") == 0;
}

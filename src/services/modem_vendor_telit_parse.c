#include "modem_vendor_telit_internal.h"

#include "services/modem_at_util.h"

#include <ctype.h>
#include <string.h>

_Static_assert(sizeof(telit_csv_view_t) * TELIT_RFSTS_FIELD_COUNT <= 256u,
               "RFSTS field views exceed the parser stack budget");

bool telit_starts_with(const char *text, const char *prefix) {
    return text != NULL && prefix != NULL &&
           modem_at_starts_with(text, prefix);
}

bool telit_csv_view_split(const char *body, telit_csv_view_t *fields,
                          size_t max_fields, size_t *count_out) {
    if (body == NULL || fields == NULL || max_fields == 0u ||
        count_out == NULL) {
        return false;
    }

    const char *p = body;
    size_t count = 0u;
    bool another = true;
    while (another) {
        if (count >= max_fields) {
            return false;
        }

        while (*p == ' ') {
            p++;
        }

        const char *start = p;
        size_t length = 0u;
        if (*p == '"') {
            start = ++p;
            while (*p != '\0' && *p != '"') {
                if (*p == '\r' || *p == '\n' ||
                    length >= TELIT_FIELD_CAP - 1u) {
                    return false;
                }
                length++;
                p++;
            }
            if (*p != '"') {
                return false;
            }
            p++;
            while (*p == ' ') {
                p++;
            }
            if (*p != '\0' && *p != ',') {
                return false;
            }
        } else {
            while (*p != '\0' && *p != ',') {
                if (*p == '"' || *p == '\r' || *p == '\n' ||
                    length >= TELIT_FIELD_CAP - 1u) {
                    return false;
                }
                length++;
                p++;
            }
            while (length > 0u && start[length - 1u] == ' ') {
                length--;
            }
        }

        fields[count].text = start;
        fields[count].length = (uint8_t)length;
        count++;

        if (*p == ',') {
            p++;
        } else {
            another = false;
        }
    }

    *count_out = count;
    return true;
}

bool telit_view_split_prefixed(const char *line, const char *prefix,
                               telit_csv_view_t *fields, size_t max_fields,
                               size_t *count_out) {
    if (!telit_starts_with(line, prefix)) {
        return false;
    }
    return telit_csv_view_split(line + strlen(prefix), fields, max_fields,
                                count_out);
}

bool telit_view_find_labeled(const char *line, const char *label,
                             telit_csv_view_t *field_out) {
    if (line == NULL || label == NULL || label[0] == '\0' ||
        field_out == NULL) {
        return false;
    }
    size_t label_length = strlen(label);
    const char *search = line;
    while ((search = strstr(search, label)) != NULL) {
        bool token_start = search == line || search[-1] == ' ';
        const char *value = search + label_length;
        if (token_start && *value != '\0' && *value != ' ') {
            size_t length = 0u;
            while (value[length] != '\0' && value[length] != ' ') {
                if (value[length] == '\r' || value[length] == '\n' ||
                    length >= TELIT_FIELD_CAP - 1u) {
                    return false;
                }
                length++;
            }
            field_out->text = value;
            field_out->length = (uint8_t)length;
            return true;
        }
        search++;
    }
    return false;
}

bool telit_view_parse_u32(telit_csv_view_t field, uint32_t max_value,
                          uint32_t *value_out) {
    if (field.text == NULL || field.length == 0u || value_out == NULL) {
        return false;
    }

    uint32_t value = 0u;
    for (uint8_t i = 0u; i < field.length; i++) {
        char ch = field.text[i];
        if (ch < '0' || ch > '9') {
            return false;
        }
        uint32_t digit = (uint32_t)(ch - '0');
        if (digit > max_value || value > (max_value - digit) / 10u) {
            return false;
        }
        value = value * 10u + digit;
    }
    *value_out = value;
    return true;
}

bool telit_view_parse_hex_u64(telit_csv_view_t field, uint64_t max_value,
                              uint64_t *value_out) {
    if (field.text == NULL || field.length == 0u || value_out == NULL) {
        return false;
    }

    uint64_t value = 0u;
    for (uint8_t i = 0u; i < field.length; i++) {
        unsigned char ch = (unsigned char)field.text[i];
        uint8_t digit;
        if (ch >= '0' && ch <= '9') {
            digit = (uint8_t)(ch - '0');
        } else if (ch >= 'A' && ch <= 'F') {
            digit = (uint8_t)(ch - 'A' + 10u);
        } else if (ch >= 'a' && ch <= 'f') {
            digit = (uint8_t)(ch - 'a' + 10u);
        } else {
            return false;
        }
        if (digit > max_value || value > (max_value - digit) / 16u) {
            return false;
        }
        value = value * 16u + digit;
    }
    *value_out = value;
    return true;
}

bool telit_view_parse_i32(telit_csv_view_t field, int32_t min_value,
                          int32_t max_value, int32_t *value_out) {
    if (field.text == NULL || field.length == 0u || value_out == NULL ||
        min_value > max_value) {
        return false;
    }

    uint8_t index = 0u;
    bool negative = false;
    if (field.text[index] == '-' || field.text[index] == '+') {
        negative = field.text[index] == '-';
        index++;
    }
    if (index == field.length) {
        return false;
    }

    uint32_t magnitude = 0u;
    uint32_t limit = negative ? (uint32_t)(-(int64_t)min_value)
                              : (uint32_t)max_value;
    for (; index < field.length; index++) {
        char ch = field.text[index];
        if (ch < '0' || ch > '9') {
            return false;
        }
        uint32_t digit = (uint32_t)(ch - '0');
        if (digit > limit || magnitude > (limit - digit) / 10u) {
            return false;
        }
        magnitude = magnitude * 10u + digit;
    }

    int64_t value = negative ? -(int64_t)magnitude : (int64_t)magnitude;
    if (value < min_value || value > max_value) {
        return false;
    }
    *value_out = (int32_t)value;
    return true;
}

bool telit_view_parse_decimal_x2(telit_csv_view_t field, int16_t min_x2,
                                 int16_t max_x2, int16_t *value_out) {
    if (field.text == NULL || field.length == 0u || value_out == NULL) {
        return false;
    }

    uint8_t dot = field.length;
    for (uint8_t i = 0u; i < field.length; i++) {
        if (field.text[i] == '.') {
            if (dot != field.length) {
                return false;
            }
            dot = i;
        }
    }

    int32_t whole = 0;
    if (dot == field.length) {
        if (!telit_view_parse_i32(field, -16384, 16383, &whole)) {
            return false;
        }
    } else {
        if (dot == 0u || dot + 2u != field.length ||
            (field.text[dot + 1u] != '0' && field.text[dot + 1u] != '5')) {
            return false;
        }
        telit_csv_view_t whole_field = {
            .text = field.text,
            .length = dot,
        };
        if (!telit_view_parse_i32(whole_field, -16384, 16383, &whole)) {
            return false;
        }
    }

    int32_t value_x2 = whole * 2;
    if (dot != field.length && field.text[dot + 1u] == '5') {
        value_x2 += field.text[0] == '-' ? -1 : 1;
    }
    if (value_x2 < min_x2 || value_x2 > max_x2) {
        return false;
    }
    *value_out = (int16_t)value_x2;
    return true;
}

bool telit_view_copy_exact(char *dst, size_t dst_cap,
                           telit_csv_view_t field) {
    if (dst == NULL || dst_cap == 0u || field.text == NULL ||
        (size_t)field.length >= dst_cap) {
        return false;
    }
    memcpy(dst, field.text, field.length);
    dst[field.length] = '\0';
    return true;
}

bool telit_view_equals(telit_csv_view_t field, const char *expected) {
    size_t expected_len = expected != NULL ? strlen(expected) : 0u;
    return field.text != NULL && expected != NULL &&
           (size_t)field.length == expected_len &&
           memcmp(field.text, expected, expected_len) == 0;
}

bool telit_view_copy_hex(char *dst, size_t dst_cap,
                         telit_csv_view_t field) {
    if (dst == NULL || dst_cap == 0u || field.text == NULL ||
        field.length == 0u || (size_t)field.length >= dst_cap) {
        return false;
    }
    for (uint8_t i = 0u; i < field.length; i++) {
        unsigned char ch = (unsigned char)field.text[i];
        if (!isxdigit(ch)) {
            return false;
        }
        dst[i] = (char)toupper(ch);
    }
    dst[field.length] = '\0';
    return true;
}

bool telit_view_digits_only(telit_csv_view_t field, size_t min_len,
                            size_t max_len) {
    if (field.text == NULL || field.length < min_len ||
        field.length > max_len) {
        return false;
    }
    for (uint8_t i = 0u; i < field.length; i++) {
        if (field.text[i] < '0' || field.text[i] > '9') {
            return false;
        }
    }
    return true;
}

bool telit_view_parse_plmn_spaced(telit_csv_view_t field, char mcc[4],
                                  char mnc[4]) {
    if (field.text == NULL ||
        (field.length != 6u && field.length != 7u) ||
        field.text[3] != ' ') {
        return false;
    }
    telit_csv_view_t mcc_field = {
        .text = field.text,
        .length = 3u,
    };
    telit_csv_view_t mnc_field = {
        .text = field.text + 4u,
        .length = (uint8_t)(field.length - 4u),
    };
    return telit_view_digits_only(mcc_field, 3u, 3u) &&
           telit_view_digits_only(mnc_field, 2u, 3u) &&
           telit_view_copy_exact(mcc, 4u, mcc_field) &&
           telit_view_copy_exact(mnc, 4u, mnc_field);
}

static modem_diag_line_result_t telit_parse_bounded_u32_value(
    const char *line, const char *prefix, uint32_t minimum,
    uint32_t maximum, uint32_t *value_out) {
    if (!telit_starts_with(line, prefix)) {
        return MODEM_DIAG_LINE_IGNORE;
    }
    telit_csv_view_t fields[1];
    size_t count = 0u;
    uint32_t value = 0u;
    if (value_out == NULL ||
        !telit_view_split_prefixed(line, prefix, fields, 1u, &count) ||
        count != 1u ||
        !telit_view_parse_u32(fields[0], maximum, &value) ||
        value < minimum) {
        return MODEM_DIAG_LINE_INVALID;
    }
    *value_out = value;
    return MODEM_DIAG_LINE_ACCEPT;
}

modem_diag_line_result_t telit_parse_scan_timer_value(
    const char *line, uint16_t *seconds) {
    uint32_t value = 0u;
    modem_diag_line_result_t result = telit_parse_bounded_u32_value(
        line, "#NWSCANTMR:", 5u, 3600u,
        seconds != NULL ? &value : NULL);
    if (result == MODEM_DIAG_LINE_ACCEPT) {
        *seconds = (uint16_t)value;
    }
    return result;
}

modem_diag_line_result_t telit_parse_band_mode_value(
    const char *line, uint8_t *mode) {
    uint32_t value = 0u;
    modem_diag_line_result_t result = telit_parse_bounded_u32_value(
        line, "#SELBNDMODE:", 0u, 1u, mode != NULL ? &value : NULL);
    if (result == MODEM_DIAG_LINE_ACCEPT) {
        *mode = (uint8_t)value;
    }
    return result;
}

modem_diag_line_result_t telit_parse_tuner_enabled_value(
    const char *line, bool *enabled) {
    uint32_t value = 0u;
    modem_diag_line_result_t result = telit_parse_bounded_u32_value(
        line, "#STUNEANT:", 0u, 1u, enabled != NULL ? &value : NULL);
    if (result == MODEM_DIAG_LINE_ACCEPT) {
        *enabled = value != 0u;
    }
    return result;
}

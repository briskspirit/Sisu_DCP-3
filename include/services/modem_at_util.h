#ifndef MODEM_AT_UTIL_H
#define MODEM_AT_UTIL_H

/* Shared AT-response parsing primitives, extracted verbatim from modem_service.c
 * so both the vendor-agnostic core (modem_service.c) and the vendor driver
 * vendor backends can use them. Header-only static inline: no separate
 * TU, no link coupling. The core keeps its short call-site names via `#define`
 * aliases in modem_service.c (never in a header); the vendor .c does the same. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static inline bool modem_at_starts_with(const char *text, const char *prefix) {
    return strncmp(text, prefix, strlen(prefix)) == 0;
}

static inline void modem_at_copy_bounded(char *dst, size_t dst_len, const char *src) {
    if (dst == 0 || dst_len == 0) {
        return;
    }
    if (src == 0) {
        dst[0] = '\0';
        return;
    }
    size_t i = 0;
    while (i + 1u < dst_len && src[i] != '\0') {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static inline bool modem_at_csv_next_field(const char **cursor, char *out, size_t out_cap) {
    if (cursor == 0 || *cursor == 0 || out == 0 || out_cap == 0u) {
        return false;
    }
    const char *p = *cursor;
    while (*p == ' ') {
        p++;
    }
    if (*p == '\0') {
        *cursor = p;
        return false;
    }
    bool quoted = false;
    if (*p == '"') {
        quoted = true;
        p++;
    }
    size_t len = 0u;
    while (*p != '\0') {
        if (quoted) {
            if (*p == '"') {
                p++;
                break;
            }
        } else if (*p == ',') {
            break;
        }
        if (len + 1u < out_cap) {
            out[len++] = *p;
        }
        p++;
    }
    out[len] = '\0';
    while (*p == ' ') {
        p++;
    }
    if (*p == ',') {
        p++;
    }
    *cursor = p;
    return true;
}

static inline unsigned modem_at_parse_uint_token(const char *text, unsigned fallback) {
    if (text == 0 || text[0] == '\0') {
        return fallback;
    }
    char *end = 0;
    unsigned value = (unsigned)strtoul(text, &end, 10);
    return end != text ? value : fallback;
}

static inline int16_t modem_at_parse_decimal_x100(const char *text) {
    if (text == 0 || text[0] == '\0') {
        return 0;
    }
    bool neg = false;
    const char *p = text;
    if (*p == '-') {
        neg = true;
        p++;
    }
    int whole = 0;
    while (*p >= '0' && *p <= '9') {
        if (whole <= 32767) { /* clamp: the result saturates to int16 anyway, and this
                               * stops whole*10/whole*100 from overflowing on a long run */
            whole = whole * 10 + (*p - '0');
        }
        p++;
    }
    int frac = 0;
    uint8_t digits = 0u;
    if (*p == '.') {
        p++;
        while (*p >= '0' && *p <= '9' && digits < 2u) {
            frac = frac * 10 + (*p - '0');
            digits++;
            p++;
        }
    }
    while (digits < 2u) {
        frac *= 10;
        digits++;
    }
    int value = whole * 100 + frac; /* whole <= 327679 -> fits int, no overflow */
    if (value > 32767) {
        value = 32767; /* saturate to int16 range */
    }
    return (int16_t)(neg ? -value : value);
}

static inline void modem_at_copy_hex_token(char *dst, size_t cap, const char *src) {
    if (dst == 0 || cap == 0u) {
        return;
    }
    if (src == 0) {
        dst[0] = '\0';
        return;
    }
    while (*src == '"') {
        src++;
    }
    size_t len = 0u;
    while (len + 1u < cap && src[len] != '\0' && src[len] != '"') {
        char ch = src[len];
        dst[len] = (char)((ch >= 'a' && ch <= 'f') ? (ch - 'a' + 'A') : ch);
        len++;
    }
    dst[len] = '\0';
}

static inline void modem_at_append_quoted(char *dst, size_t dst_len,
                                          const char *src) {
    if (dst == 0 || dst_len == 0u) {
        return;
    }
    size_t pos = 0u;
    dst[pos++] = '"';
    for (size_t i = 0u; src != 0 && src[i] != '\0' && pos + 2u < dst_len;
         i++) {
        char ch = src[i] == '"' ? '\'' : src[i];
        dst[pos++] = ch;
    }
    if (pos + 1u < dst_len) {
        dst[pos++] = '"';
    }
    if (pos >= dst_len) {
        pos = dst_len - 1u;
    }
    dst[pos] = '\0';
}

#endif

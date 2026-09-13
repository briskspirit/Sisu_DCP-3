#include "render_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

void netmon_render_linef(netmon_frame_t *frame, uint8_t row, const char *format, ...) {
    if (frame == NULL || row >= NETMON_FRAME_LINE_COUNT || format == NULL) {
        return;
    }
    va_list args;
    va_start(args, format);
    (void)vsnprintf(frame->lines[row], NETMON_FRAME_LINE_CAP, format, args);
    va_end(args);
}

void netmon_render_copy_bounded(char *dst, size_t cap, const char *src) {
    if (dst == NULL || cap == 0u) {
        return;
    }
    size_t count = src != NULL ? strlen(src) : 0u;
    if (count >= cap) {
        count = cap - 1u;
    }
    if (count != 0u) {
        memcpy(dst, src, count);
    }
    dst[count] = '\0';
}

void netmon_render_text_chunks(netmon_frame_t *frame, const char *text,
                        uint8_t first_row) {
    if (frame == NULL || first_row >= NETMON_FRAME_LINE_COUNT) {
        return;
    }
    const char *src = text != NULL && text[0] != '\0' ? text : "--";
    size_t length = strlen(src);
    for (uint8_t row = first_row; row < NETMON_FRAME_LINE_COUNT; row++) {
        size_t offset = (size_t)(row - first_row) *
                        NETMON_FRAME_VISIBLE_CHARS;
        if (offset >= length) {
            break;
        }
        size_t count = length - offset;
        if (count > NETMON_FRAME_VISIBLE_CHARS) {
            count = NETMON_FRAME_VISIBLE_CHARS;
        }
        memcpy(frame->lines[row], &src[offset], count);
        frame->lines[row][count] = '\0';
    }
}

void netmon_render_text_chunks_mark_truncated(netmon_frame_t *frame,
                                              const char *text,
                                              uint8_t first_row,
                                              bool source_truncated) {
    netmon_render_text_chunks(frame, text, first_row);
    if (frame == NULL || first_row >= NETMON_FRAME_LINE_COUNT ||
        text == NULL || text[0] == '\0') {
        return;
    }
    size_t capacity = (size_t)(NETMON_FRAME_LINE_COUNT - first_row) *
                      NETMON_FRAME_VISIBLE_CHARS;
    size_t shown = strlen(text);
    if (!source_truncated && shown <= capacity) {
        return;
    }
    if (shown >= capacity) {
        char *last = frame->lines[NETMON_FRAME_LINE_COUNT - 1u];
        last[NETMON_FRAME_VISIBLE_CHARS - 1u] = '>';
        last[NETMON_FRAME_VISIBLE_CHARS] = '\0';
        return;
    }

    uint8_t row = (uint8_t)(first_row +
        shown / NETMON_FRAME_VISIBLE_CHARS);
    size_t column = shown % NETMON_FRAME_VISIBLE_CHARS;
    frame->lines[row][column] = '>';
    frame->lines[row][column + 1u] = '\0';
}

const char *netmon_render_yes_no(bool value) {
    return value ? "yes" : "no";
}

const char *netmon_render_on_off(bool value) {
    return value ? "on" : "off";
}

const char *netmon_render_rat_text(modem_diag_rat_t rat) {
    switch (rat) {
    case MODEM_DIAG_RAT_GSM: return "GSM";
    case MODEM_DIAG_RAT_WCDMA: return "WCDMA";
    case MODEM_DIAG_RAT_LTE: return "LTE";
    case MODEM_DIAG_RAT_UNKNOWN:
    default: return "--";
    }
}
void netmon_render_half_db(char *dst, size_t cap, const char *label,
                           int16_t value_x2) {
    int value = value_x2;
    bool negative = value < 0;
    unsigned magnitude = (unsigned)(negative ? -value : value);
    char text[24];
    (void)snprintf(text, sizeof(text), "%s %s%u.%u", label,
                   negative ? "-" : "", magnitude / 2u,
                   (magnitude & 1u) ? 5u : 0u);
    netmon_render_copy_bounded(dst, cap, text);
}

void netmon_render_charge_delta(char *dst, size_t cap, int64_t delta_nah) {
    if (dst == NULL || cap == 0u) {
        return;
    }
    int64_t micro_ah = delta_nah / 1000;
    bool negative = micro_ah < 0;
    uint64_t magnitude64 = (uint64_t)(negative ? -micro_ah : micro_ah);
    if (magnitude64 >= UINT64_C(10000000)) {
        (void)snprintf(dst, cap, "dQ RANGE");
        return;
    }
    uint32_t magnitude = (uint32_t)magnitude64;
    uint32_t whole = magnitude / 1000u;
    uint32_t fraction = magnitude % 1000u;
    const char *sign = negative ? "-" : "";
    char text[32];
    if (whole < 10u) {
        (void)snprintf(text, sizeof(text), "dQ %s%lu.%03lumAh", sign,
                       (unsigned long)whole, (unsigned long)fraction);
    } else if (whole < 100u) {
        (void)snprintf(text, sizeof(text), "dQ %s%lu.%02lumAh", sign,
                       (unsigned long)whole,
                       (unsigned long)(fraction / 10u));
    } else if (whole < 1000u) {
        (void)snprintf(text, sizeof(text), "dQ %s%lu.%01lumAh", sign,
                       (unsigned long)whole,
                       (unsigned long)(fraction / 100u));
    } else {
        (void)snprintf(text, sizeof(text), "dQ %s%lumAh", sign,
                       (unsigned long)whole);
    }
    netmon_render_copy_bounded(dst, cap, text);
}

void netmon_render_tenth(char *dst, size_t cap, const char *label,
                         int32_t value_x10) {
    bool negative = value_x10 < 0;
    uint32_t magnitude = (uint32_t)(negative ? -(int64_t)value_x10
                                             : value_x10);
    (void)snprintf(dst, cap, "%s %s%lu.%lu", label,
                   negative ? "-" : "",
                   (unsigned long)(magnitude / 10u),
                   (unsigned long)(magnitude % 10u));
}

void netmon_render_current(char *dst, size_t cap, int32_t current_ua) {
    bool negative = current_ua < 0;
    uint32_t magnitude = (uint32_t)(negative ? -(int64_t)current_ua
                                             : current_ua);
    char text[24];
    (void)snprintf(text, sizeof(text), "I %s%lu.%lumA",
                   negative ? "-" : "",
                   (unsigned long)(magnitude / 1000u),
                   (unsigned long)((magnitude / 100u) % 10u));
    netmon_render_copy_bounded(dst, cap, text);
}

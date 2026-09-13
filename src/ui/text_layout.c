#include "ui/text_layout.h"

#include <limits.h>
#include <string.h>

typedef struct {
    const font_t *font;
    const char *text;
    size_t text_len;
    size_t pos;
    int width;
    bool done;
} ui_glyph_line_iter_t;

static size_t advance_codepoint(const char *text, size_t pos) {
    const char *cursor = &text[pos];
    asset_next_codepoint(&cursor);
    return (size_t)(cursor - text);
}

size_t ui_text_clamp_boundary(const char *text, size_t offset) {
    if (text == NULL) {
        return 0u;
    }
    size_t pos = 0u;
    while (text[pos] != '\0' && pos < offset) {
        size_t next = advance_codepoint(text, pos);
        if (next > offset) {
            break;
        }
        pos = next;
    }
    return pos;
}

size_t ui_text_previous_boundary(const char *text, size_t offset) {
    size_t boundary = ui_text_clamp_boundary(text, offset);
    size_t previous = 0u;
    size_t pos = 0u;
    while (pos < boundary) {
        previous = pos;
        pos = advance_codepoint(text, pos);
    }
    return previous;
}

size_t ui_text_next_boundary(const char *text, size_t offset) {
    size_t boundary = ui_text_clamp_boundary(text, offset);
    if (text == NULL || text[boundary] == '\0') {
        return boundary;
    }
    return advance_codepoint(text, boundary);
}

int ui_text_range_width(const font_t *font,
                        const char *text,
                        size_t start,
                        size_t end) {
    if (font == NULL || text == NULL) {
        return 0;
    }
    start = ui_text_clamp_boundary(text, start);
    end = ui_text_clamp_boundary(text, end);
    if (end < start) {
        end = start;
    }
    const char *cursor = &text[start];
    const char *limit = &text[end];
    int width = 0;
    bool first = true;
    while (cursor < limit && *cursor != '\0') {
        const glyph_t *glyph = asset_glyph_codepoint(
            font, asset_next_codepoint(&cursor));
        if (glyph == NULL) {
            continue;
        }
        if (!first) {
            width += font->spacing;
        }
        width += glyph->width;
        first = false;
    }
    return width;
}

size_t ui_text_offset_for_x(const font_t *font,
                            const char *text,
                            ui_text_span_t span,
                            int x) {
    if (font == NULL || text == NULL || span.end < span.start || x <= 0) {
        return span.start;
    }
    size_t pos = span.start;
    int width = 0;
    bool first = true;
    while (pos < span.end && text[pos] != '\0') {
        const char *cursor = &text[pos];
        const glyph_t *glyph = asset_glyph_codepoint(
            font, asset_next_codepoint(&cursor));
        size_t next = (size_t)(cursor - text);
        if (next > span.end) {
            break;
        }
        int advance = glyph != NULL
            ? (first ? 0 : font->spacing) + glyph->width
            : 0;
        if (width + advance > x) {
            break;
        }
        width += advance;
        if (glyph != NULL) {
            first = false;
        }
        pos = next;
    }
    return pos;
}

bool ui_text_span_copy(const char *text,
                       ui_text_span_t span,
                       char *dst,
                       size_t cap) {
    if (text == NULL || dst == NULL || cap == 0u || span.end < span.start) {
        return false;
    }
    size_t text_len = strlen(text);
    size_t len = (size_t)span.end - span.start;
    if (span.end > text_len || len + 1u > cap) {
        dst[0] = '\0';
        return false;
    }
    memcpy(dst, &text[span.start], len);
    dst[len] = '\0';
    return true;
}

static bool glyph_line_next(ui_glyph_line_iter_t *iter,
                            ui_text_span_t *span_out) {
    if (iter == NULL || span_out == NULL || iter->done) {
        return false;
    }

    size_t start = iter->pos;
    int line_width = 0;
    bool first = true;
    while (iter->pos < iter->text_len) {
        if (iter->text[iter->pos] == '\n') {
            span_out->start = (uint16_t)start;
            span_out->end = (uint16_t)iter->pos;
            iter->pos++;
            return true;
        }

        const char *cursor = &iter->text[iter->pos];
        const glyph_t *glyph = asset_glyph_codepoint(
            iter->font, asset_next_codepoint(&cursor));
        size_t next = (size_t)(cursor - iter->text);
        int advance = glyph != NULL
            ? (first ? 0 : iter->font->spacing) + glyph->width
            : 0;
        if (!first && line_width + advance > iter->width) {
            span_out->start = (uint16_t)start;
            span_out->end = (uint16_t)iter->pos;
            return true;
        }
        line_width += advance;
        if (glyph != NULL) {
            first = false;
        }
        iter->pos = next;
    }

    span_out->start = (uint16_t)start;
    span_out->end = (uint16_t)iter->pos;
    iter->done = true;
    return true;
}

static bool glyph_line_iter_init(ui_glyph_line_iter_t *iter,
                                 const font_t *font,
                                 const char *text,
                                 int width) {
    if (iter == NULL || font == NULL || text == NULL) {
        return false;
    }
    size_t text_len = strlen(text);
    if (text_len > UINT16_MAX) {
        return false;
    }
    *iter = (ui_glyph_line_iter_t){
        .font = font,
        .text = text,
        .text_len = text_len,
        .width = width,
    };
    return true;
}

bool ui_glyph_line_spans(const font_t *font,
                         const char *text,
                         int width,
                         ui_text_span_t *spans,
                         uint16_t span_cap,
                         uint16_t *span_count_out) {
    if (span_count_out != NULL) {
        *span_count_out = 0u;
    }
    if (font == NULL || text == NULL || spans == NULL || span_cap == 0u ||
        span_count_out == NULL) {
        return false;
    }
    ui_glyph_line_iter_t iter;
    if (!glyph_line_iter_init(&iter, font, text, width)) {
        return false;
    }

    uint16_t count = 0u;
    ui_text_span_t span;
    while (glyph_line_next(&iter, &span)) {
        if (count >= span_cap) {
            return false;
        }
        spans[count++] = span;
    }
    *span_count_out = count;
    return true;
}

uint16_t ui_glyph_line_count(const font_t *font,
                             const char *text,
                             int width) {
    ui_glyph_line_iter_t iter;
    if (!glyph_line_iter_init(&iter, font, text, width)) {
        return 0u;
    }
    uint16_t count = 0u;
    ui_text_span_t span;
    while (glyph_line_next(&iter, &span)) {
        if (count == UINT16_MAX) {
            return 0u;
        }
        count++;
    }
    return count;
}

bool ui_glyph_line_at(const font_t *font,
                      const char *text,
                      int width,
                      uint16_t line_index,
                      ui_text_span_t *span_out) {
    if (span_out == NULL) {
        return false;
    }
    ui_glyph_line_iter_t iter;
    if (!glyph_line_iter_init(&iter, font, text, width)) {
        return false;
    }
    for (uint16_t index = 0u; glyph_line_next(&iter, span_out); index++) {
        if (index == line_index) {
            return true;
        }
    }
    return false;
}

bool ui_glyph_line_for_offset(const font_t *font,
                              const char *text,
                              int width,
                              size_t offset,
                              uint16_t *line_index_out,
                              ui_text_span_t *span_out) {
    if (line_index_out == NULL || span_out == NULL) {
        return false;
    }
    ui_glyph_line_iter_t iter;
    if (!glyph_line_iter_init(&iter, font, text, width)) {
        return false;
    }
    offset = ui_text_clamp_boundary(text, offset);
    uint16_t index = 0u;
    ui_text_span_t span;
    bool found = false;
    while (glyph_line_next(&iter, &span)) {
        if (span.start > offset) {
            break;
        }
        *line_index_out = index;
        *span_out = span;
        found = true;
        if (index == UINT16_MAX) {
            break;
        }
        index++;
    }
    return found;
}

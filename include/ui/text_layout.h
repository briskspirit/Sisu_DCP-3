#ifndef UI_TEXT_LAYOUT_H
#define UI_TEXT_LAYOUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ui/assets.h"

typedef struct {
    uint16_t start;
    uint16_t end;
} ui_text_span_t;

/* Character-wrap exact source text into pixel-bounded byte spans. Newlines are
 * excluded from the preceding span and create a new row; empty/trailing rows
 * are retained so an editor cursor always has a line to own. */
bool ui_glyph_line_spans(const font_t *font,
                         const char *text,
                         int width,
                         ui_text_span_t *spans,
                         uint16_t span_cap,
                         uint16_t *span_count_out);

/* Streaming queries over the same character-wrap layout. These avoid placing
 * a max-text-sized span array on the UI stack when a caller needs only a line
 * count, one row, or the row containing its cursor. */
uint16_t ui_glyph_line_count(const font_t *font,
                             const char *text,
                             int width);
bool ui_glyph_line_at(const font_t *font,
                      const char *text,
                      int width,
                      uint16_t line_index,
                      ui_text_span_t *span_out);
bool ui_glyph_line_for_offset(const font_t *font,
                              const char *text,
                              int width,
                              size_t offset,
                              uint16_t *line_index_out,
                              ui_text_span_t *span_out);

/* Clamp arbitrary byte offsets to valid boundaries in the hybrid UTF-8/CP1252
 * text representation used by the UI. */
size_t ui_text_clamp_boundary(const char *text, size_t offset);
size_t ui_text_previous_boundary(const char *text, size_t offset);
size_t ui_text_next_boundary(const char *text, size_t offset);

int ui_text_range_width(const font_t *font,
                        const char *text,
                        size_t start,
                        size_t end);
size_t ui_text_offset_for_x(const font_t *font,
                            const char *text,
                            ui_text_span_t span,
                            int x);
bool ui_text_span_copy(const char *text,
                       ui_text_span_t span,
                       char *dst,
                       size_t cap);

#endif

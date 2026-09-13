#ifndef UI_H
#define UI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ui/assets.h"
#include "ui/framebuffer.h"

void copy_text(char *dst, size_t cap, const char *src);
/* Copy `tmpl` into dst substituting every v6.00 "%U" token with `value`
 * (bounded, always NUL-terminated). Localized templates put %U in different
 * places -- e.g. DE "Gespräch mit\n%U gehalten", and HU has none at all --
 * so substitution must be token-position-agnostic, never a slice after "%U". */
void ui_format_named(char *dst, size_t cap, const char *tmpl, const char *value);

void draw_softkey(framebuffer_t *fb, const char *label);
void draw_notice(framebuffer_t *fb, const font_t *font, const char *text);
void draw_right_text_box(framebuffer_t *fb,
                         const font_t *font,
                         const char *text,
                         int x,
                         int y,
                         int width);
/* Wrap `text` to `width` (UTF-8-safe, honors '\n') and draw its lines
 * left-aligned from (x,y) at `pitch` px spacing, up to `max_lines` (<=4). */
void draw_text_block(framebuffer_t *fb, const font_t *font, const char *text,
                     int x, int y, int width, int pitch, uint8_t max_lines);
void draw_center_text_box(framebuffer_t *fb,
                          const font_t *font,
                          const char *text,
                          int x,
                          int y,
                          int width);
void draw_menu_position(framebuffer_t *fb, uint8_t selected, uint8_t count, uint8_t ordinal);
void draw_string_slice_right(framebuffer_t *fb,
                             const font_t *font,
                             const char *text,
                             uint8_t start,
                             uint8_t len,
                             int x,
                             int y,
                             int width);
void draw_flat_list(framebuffer_t *fb,
                    const char *const *labels,
                    uint8_t count,
                    uint8_t selected,
                    const char *breadcrumb,
                    const char *softkey);
/* Circular setting-choice viewport. `view_start` names the logical option on
 * the first row; following rows wrap while `selected` and the position
 * indicator retain their original logical ordinals. */
void draw_flat_list_circular_view(framebuffer_t *fb,
                                  const char *const *labels,
                                  uint8_t count,
                                  uint8_t selected,
                                  uint8_t view_start,
                                  const char *breadcrumb,
                                  const char *softkey);
void draw_flat_list_at_y(framebuffer_t *fb,
                         const char *const *labels,
                         uint8_t count,
                         uint8_t selected,
                         const char *breadcrumb,
                         const char *softkey,
                         int y0);
void draw_flat_list_view_at_y(framebuffer_t *fb,
                              const char *const *labels,
                              uint8_t count,
                              uint8_t selected,
                              const char *breadcrumb,
                              const char *softkey,
                              int y0,
                              uint8_t view_start);
void draw_static_page_list(framebuffer_t *fb,
                           const char *label,
                           const char *preview,
                           uint8_t selected,
                           uint8_t count,
                           const char *breadcrumb,
                           const char *softkey);
void draw_position_indicator(framebuffer_t *fb, uint16_t selected, uint16_t count, const char *breadcrumb);

/* F3: the original counter builder 0x0026281e appends the selected row's
 * STORED ordinal to the menu-depth path ("Messages/Inbox" = "2-1"). Build
 * with ui_breadcrumb_path; pass UI_BREADCRUMB_NONE for surfaces the original
 * leaves uncounted (choice lists), which suppresses the local-index fallback. */
extern const char *const UI_BREADCRUMB_NONE;
const char *ui_breadcrumb_path(char *buf, size_t cap, const char *parent, unsigned selected_ordinal);
/* Advance a circular three-row choice list by one item. Selection moves within
 * the current window first; the window advances only beyond its top/bottom. */
static inline void ui_circular_list_step_3rows(uint8_t count,
                                               int direction,
                                               uint8_t *selected,
                                               uint8_t *view_start) {
    if (count == 0u || direction == 0 || selected == NULL || view_start == NULL) {
        return;
    }

    if (*selected >= count) {
        *selected = 0u;
    }
    if (*view_start >= count) {
        *view_start = *selected;
    }

    uint8_t row = (uint8_t)((*selected + count - *view_start) % count);
    if (row >= 3u) {
        *view_start = *selected;
        row = 0u;
    }

    if (direction < 0) {
        *selected = *selected == 0u ? (uint8_t)(count - 1u)
                                    : (uint8_t)(*selected - 1u);
        if (count > 3u && row == 0u) {
            *view_start = *view_start == 0u ? (uint8_t)(count - 1u)
                                            : (uint8_t)(*view_start - 1u);
        }
        return;
    }

    *selected = (uint8_t)((*selected + 1u) % count);
    if (count > 3u && row == 2u) {
        *view_start = (uint8_t)((*view_start + 1u) % count);
    }
}

/* F2: selected-text marquee (timer 0x59, 1024 ms chunked). Renderers call
 * ui_marquee_text for the selected/overflowing label; ui_marquee_tick runs
 * from the app tick and reports when a redraw is needed. */
const char *ui_marquee_text(const font_t *font, const char *text, int max_width, char *buf, size_t cap);
bool ui_marquee_tick(uint32_t now);
void draw_text_right(framebuffer_t *fb, const font_t *font, const char *text, int y, int width);

/* The generated UI fonts' worst byte density at the 84 px LCD width is 126
 * bytes (42 three-byte glyphs at width 2). The host test derives and pins this
 * bound from every generated glyph, so a future font change cannot silently
 * reintroduce byte-driven wrapping. */
#define UI_WRAP_LINE_BYTES 128u

uint16_t ui_wrap_line_count(const font_t *font,
                            const char *text,
                            int width);
bool ui_wrap_line_at(const font_t *font,
                     const char *text,
                     int width,
                     uint16_t line_index,
                     char *line,
                     size_t line_cap);
/* Multi-line adapter for the remaining callers that need all rows at once.
 * A line that fits in pixels but not in `stride` is an explicit failure (0),
 * never a hidden byte-based wrap or partial codepoint. */
uint8_t wrap_text_lines_ex(const font_t *font, const char *text, int width,
                           char *lines, size_t stride, uint8_t max_lines);

#endif

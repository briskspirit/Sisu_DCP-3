#include "ui/ui.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "services/strings.h"
#include "services/timebase.h"
#include "ui/text_layout.h"

#define UI_ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))
#define UI_MENU_SCROLL_TOP 7
#define UI_MENU_SCROLL_H 30

/* F2: selected-text marquee — timer 0x59, 0x80 ticks x 8 ms = 1024 ms. Step 0
 * shows the static (clipped) fit, then the start index advances by
 * whole-glyph chunks that fit the visible width; the final chunk is
 * end-aligned (0x80 mode); loops while overflow remains. */
#define UI_MARQUEE_DELAY_MS 1024u
#define UI_MARQUEE_TEXT_MAX 44u
#define UI_MARQUEE_MAX_CHUNKS 24u

typedef struct {
    bool active;
    bool seen;          /* drawn since the last step advance */
    bool check_pending; /* verify the marquee row is still on screen */
    uint8_t step;
    uint8_t chunk_count;
    uint8_t starts[UI_MARQUEE_MAX_CHUNKS];
    uint32_t last_step_ms;
    int width;
    const font_t *font;
    char text[UI_MARQUEE_TEXT_MAX];
} ui_marquee_state_t;

static ui_marquee_state_t s_marquee;

static size_t marquee_fit_end(const font_t *font, const char *text, size_t start, int max_width);
static uint8_t flat_list_view_start(const char *const *labels,
                                    uint8_t count,
                                    uint8_t selected,
                                    const char *softkey,
                                    int y0);
static void draw_flat_list_row(framebuffer_t *fb,
                               const font_t *font,
                               const char *label,
                               int y,
                               bool selected);

void copy_text(char *dst, size_t cap, const char *src) {
    if (dst == 0 || cap == 0u) {
        return;
    }
    if (src == 0) {
        src = "";
    }
    /* Copy WHOLE codepoints only. A byte-wise truncation at cap-1 can split a
     * multibyte UTF-8 sequence, leaving a partial lead byte that the renderer
     * decodes as a wrong CP1252 glyph -- localized (Cyrillic/Greek/...) strings
     * are two-plus bytes per glyph, so English-sized buffers hit this. Stopping
     * on a codepoint boundary clips cleanly instead. asset_next_codepoint always
     * advances >=1 byte (ASCII/CP1252 included), so this also matches the old
     * behavior for single-byte text. */
    size_t di = 0u;
    const char *p = src;
    while (*p != '\0') {
        const char *next = p;
        asset_next_codepoint(&next);
        size_t n = (size_t)(next - p);
        if (di + n + 1u > cap) {
            break;
        }
        while (p < next) {
            dst[di++] = *p++;
        }
    }
    dst[di] = '\0';
}

void ui_format_named(char *dst, size_t cap, const char *tmpl, const char *value) {
    size_t di = 0u;
    if (dst == 0 || cap == 0u) {
        return;
    }
    if (tmpl == 0) {
        tmpl = "";
    }
    if (value == 0) {
        value = "";
    }
    for (const char *p = tmpl; *p != '\0' && di + 1u < cap;) {
        if (p[0] == '%' && p[1] == 'U') {
            for (const char *q = value; *q != '\0' && di + 1u < cap; q++) {
                dst[di++] = *q;
            }
            p += 2;
        } else {
            dst[di++] = *p++;
        }
    }
    dst[di] = '\0';
}

void draw_softkey(framebuffer_t *fb, const char *label) {
    /* Softkeys localize centrally by resolving the fixed English caption to its
     * exact v6.00 framework menu-bar SID (SET.1). A caption an app already
     * resolved to a context-specific SID (an already-localized string) passes
     * through ts_softkey unchanged. */
    draw_center_text_box(fb, asset_font(FONT_FS2), ts_softkey(label), 0, 40, FB_WIDTH);
}

void draw_notice(framebuffer_t *fb, const font_t *font, const char *text) {
    const char *lines[3];
    char scratch[64];
    uint8_t count = 0u;
    copy_text(scratch, sizeof(scratch), text);
    scratch[sizeof(scratch) - 1u] = '\0';

    char *cursor = scratch;
    while (*cursor != '\0' && count < UI_ARRAY_COUNT(lines)) {
        lines[count++] = cursor;
        while (*cursor != '\0' && *cursor != '\n') {
            cursor++;
        }
        if (*cursor == '\n') {
            *cursor++ = '\0';
        }
    }

    if (count >= 2u && strcmp(lines[0], "Now") == 0 && strcmp(lines[1], "press *") == 0) {
        fb_text(fb, font, lines[0], 7, 9, true, 71);
        fb_text(fb, font, lines[1], 7, 18, true, 71);
        return;
    }

    uint8_t visible_count = count > 3u ? 3u : count;
    int y = 7 + (30 - (int)visible_count * 9) / 2;
    if (y < 7) {
        y = 7;
    }
    for (uint8_t i = 0; i < visible_count; i++) {
        fb_text(fb, font, lines[i], 6, y + i * 9, true, 72);
    }
}

typedef struct {
    const font_t *font;
    const char *cursor;
    int width;
    bool failed;
} ui_word_wrap_iter_t;

static bool word_wrap_next(ui_word_wrap_iter_t *iter,
                           char *line,
                           size_t cap) {
    if (iter == NULL || line == NULL || cap == 0u || iter->failed) {
        return false;
    }
    line[0] = '\0';
    size_t line_len = 0u;
    int line_width = 0;
    const int space_width = asset_text_width(iter->font, " ");

    for (;;) {
        while (*iter->cursor == ' ') {
            iter->cursor++;
        }
        if (*iter->cursor == '\n') {
            iter->cursor++;
            if (line_len != 0u) {
                return true;
            }
            continue;
        }
        if (*iter->cursor == '\0') {
            return line_len != 0u;
        }

        const char *word_start = iter->cursor;
        const char *word_end = word_start;
        while (*word_end != '\0' && *word_end != ' ' &&
               *word_end != '\n') {
            asset_next_codepoint(&word_end);
        }
        size_t word_len = (size_t)(word_end - word_start);
        int word_width = ui_text_range_width(iter->font, word_start,
                                             0u, word_len);

        if (line_len != 0u) {
            int joined_width = line_width + iter->font->spacing +
                               space_width + iter->font->spacing +
                               word_width;
            if (joined_width > iter->width) {
                return true;
            }
            if (line_len + 1u + word_len + 1u > cap) {
                iter->failed = true;
                line[0] = '\0';
                return false;
            }
            line[line_len++] = ' ';
            memcpy(&line[line_len], word_start, word_len);
            line_len += word_len;
            line[line_len] = '\0';
            line_width = joined_width;
            iter->cursor = word_end;
            continue;
        }

        if (word_width <= iter->width) {
            if (word_len + 1u > cap) {
                iter->failed = true;
                return false;
            }
            memcpy(line, word_start, word_len);
            line[word_len] = '\0';
            line_len = word_len;
            line_width = word_width;
            iter->cursor = word_end;
            continue;
        }

        const char *scan = word_start;
        const char *fit_end = word_start;
        int fit_width = 0;
        bool first = true;
        while (scan < word_end) {
            const char *next = scan;
            const glyph_t *glyph = asset_glyph_codepoint(
                iter->font, asset_next_codepoint(&next));
            int advance = glyph != NULL
                ? (first ? 0 : iter->font->spacing) + glyph->width
                : 0;
            if (!first && fit_width + advance > iter->width) {
                break;
            }
            fit_width += advance;
            if (glyph != NULL) {
                first = false;
            }
            fit_end = next;
            scan = next;
        }
        if (fit_end == word_start) {
            asset_next_codepoint(&fit_end);
        }
        size_t fit_len = (size_t)(fit_end - word_start);
        if (fit_len + 1u > cap) {
            iter->failed = true;
            return false;
        }
        memcpy(line, word_start, fit_len);
        line[fit_len] = '\0';
        iter->cursor = fit_end;
        return true;
    }
}

uint16_t ui_wrap_line_count(const font_t *font,
                            const char *text,
                            int width) {
    if (font == NULL || text == NULL || text[0] == '\0') {
        return 0u;
    }
    ui_word_wrap_iter_t iter = {font, text, width, false};
    char line[UI_WRAP_LINE_BYTES];
    uint16_t count = 0u;
    while (word_wrap_next(&iter, line, sizeof(line))) {
        if (count == UINT16_MAX) {
            return 0u;
        }
        count++;
    }
    return iter.failed ? 0u : count;
}

bool ui_wrap_line_at(const font_t *font,
                     const char *text,
                     int width,
                     uint16_t line_index,
                     char *line,
                     size_t line_cap) {
    if (font == NULL || text == NULL || line == NULL || line_cap == 0u) {
        return false;
    }
    ui_word_wrap_iter_t iter = {font, text, width, false};
    char discard[UI_WRAP_LINE_BYTES];
    for (uint16_t index = 0u;; index++) {
        char *dst = index == line_index ? line : discard;
        size_t cap = index == line_index ? line_cap : sizeof(discard);
        if (!word_wrap_next(&iter, dst, cap)) {
            line[0] = '\0';
            return false;
        }
        if (index == line_index) {
            return true;
        }
    }
}

uint8_t wrap_text_lines_ex(const font_t *font, const char *text, int width,
                           char *lines, size_t stride, uint8_t max_lines) {
    if (font == NULL || text == NULL || lines == NULL || stride == 0u ||
        max_lines == 0u) {
        return 0u;
    }
    ui_word_wrap_iter_t iter = {font, text, width, false};
    for (uint8_t i = 0u; i < max_lines; i++) {
        lines[(size_t)i * stride] = '\0';
    }
    uint8_t count = 0u;
    while (count < max_lines &&
           word_wrap_next(&iter, lines + (size_t)count * stride, stride)) {
        count++;
    }
    if (iter.failed) {
        for (uint8_t i = 0u; i < max_lines; i++) {
            lines[(size_t)i * stride] = '\0';
        }
        return 0u;
    }
    return count;
}

void draw_right_text_box(framebuffer_t *fb, const font_t *font, const char *text, int x, int y, int width) {
    const char *visible = text;
    int text_width = asset_text_width(font, visible);
    while (*visible != '\0' && text_width > width) {
        asset_next_codepoint(&visible); /* drop a whole glyph, not a byte */
        text_width = asset_text_width(font, visible);
    }
    int tx = x + width - text_width;
    if (tx < x) {
        tx = x;
    }
    fb_text(fb, font, visible, tx, y, true, width - (tx - x));
}

void draw_center_text_box(framebuffer_t *fb, const font_t *font, const char *text, int x, int y, int width) {
    int text_width = asset_text_width(font, text);
    int tx = x + ((width - text_width) / 2);
    if (tx < x) {
        tx = x;
    }
    fb_text(fb, font, text, tx, y, true, width - (tx - x));
}

void draw_text_block(framebuffer_t *fb, const font_t *font, const char *text,
                     int x, int y, int width, int pitch, uint8_t max_lines) {
    /* Wrap `text` (honoring '\n', UTF-8-safe) to `width` and draw each line
     * left-aligned at `pitch` spacing. Localized strings the clone used to
     * hand-split across several fb_text calls (e.g. "No phone numbers") are one
     * SID here; wrapping reproduces the per-language line breaks. */
    if (max_lines > 4u) {
        max_lines = 4u;
    }
    uint16_t count = ui_wrap_line_count(font, text, width);
    if (count > max_lines) {
        count = max_lines;
    }
    char line[UI_WRAP_LINE_BYTES];
    for (uint16_t i = 0u; i < count; i++) {
        if (ui_wrap_line_at(font, text, width, i, line, sizeof(line))) {
            fb_text(fb, font, line, x, y + (int)i * pitch, true, width);
        }
    }
}

void draw_menu_position(framebuffer_t *fb, uint8_t selected, uint8_t count, uint8_t ordinal) {
    char text[3];
    if (ordinal >= 10u) {
        text[0] = (char)('0' + (ordinal / 10u));
        text[1] = (char)('0' + (ordinal % 10u));
        text[2] = '\0';
    } else {
        text[0] = (char)('0' + ordinal);
        text[1] = '\0';
    }
    draw_right_text_box(fb, asset_font(FONT_FS3), text, 0, 0, FB_WIDTH);

    fb_vline(fb, 81, UI_MENU_SCROLL_TOP, UI_MENU_SCROLL_H, true);
    const bitmap_t *thumb = asset_bitmap(263u);
    int thumb_h = thumb != 0 ? thumb->height : 7;
    int max_offset = UI_MENU_SCROLL_H - thumb_h;
    int offset = 0;
    if (count > 1u) {
        offset = selected >= count - 1u ? max_offset : (int)(selected * max_offset) / (int)(count - 1u);
    }
    if (thumb != 0) {
        fb_bitmap(fb, 263u, 81, UI_MENU_SCROLL_TOP + offset, true, false);
    } else {
        fb_fill_rect(fb, 81, UI_MENU_SCROLL_TOP + offset, 3, thumb_h, true);
    }
}

void draw_string_slice_right(framebuffer_t *fb,
                             const font_t *font,
                             const char *text,
                             uint8_t start,
                             uint8_t len,
                             int x,
                             int y,
                             int width) {
    char buf[12];
    if (len >= sizeof(buf)) {
        len = (uint8_t)(sizeof(buf) - 1u);
    }
    memcpy(buf, &text[start], len);
    buf[len] = '\0';
    draw_right_text_box(fb, font, buf, x, y, width);
}

void draw_flat_list(framebuffer_t *fb,
                    const char *const *labels,
                    uint8_t count,
                    uint8_t selected,
                    const char *breadcrumb,
                    const char *softkey) {
    draw_flat_list_at_y(fb, labels, count, selected, breadcrumb, softkey, 8);
}

void draw_flat_list_circular_view(framebuffer_t *fb,
                                  const char *const *labels,
                                  uint8_t count,
                                  uint8_t selected,
                                  uint8_t view_start,
                                  const char *breadcrumb,
                                  const char *softkey) {
    fb_clear(fb, false);
    const font_t *font = asset_font(FONT_FS2);
    if (count == 0u) {
        return;
    }
    if (selected >= count) {
        selected = 0u;
    }
    if (view_start >= count ||
        (uint8_t)((selected + count - view_start) % count) >= 3u) {
        view_start = selected;
    }
    for (uint8_t row = 0u; row < 3u && row < count; row++) {
        uint8_t index = (uint8_t)((view_start + row) % count);
        draw_flat_list_row(fb, font, labels[index], 8 + row * 10,
                           index == selected);
    }
    draw_position_indicator(fb, selected, count, breadcrumb);
    draw_softkey(fb, softkey);
}

void draw_flat_list_at_y(framebuffer_t *fb,
                         const char *const *labels,
                         uint8_t count,
                         uint8_t selected,
                         const char *breadcrumb,
                         const char *softkey,
                         int y0) {
    draw_flat_list_view_at_y(fb, labels, count, selected, breadcrumb, softkey, y0, 0xffu);
}

void draw_flat_list_view_at_y(framebuffer_t *fb,
                              const char *const *labels,
                              uint8_t count,
                              uint8_t selected,
                              const char *breadcrumb,
                              const char *softkey,
                              int y0,
                              uint8_t view_start) {
    fb_clear(fb, false);
    const font_t *font = asset_font(FONT_FS2);
    if (count == 0u) {
        return;
    }
    if (selected >= count) {
        selected = 0u;
    }
    uint8_t start = flat_list_view_start(labels, count, selected, softkey, y0);
    if (view_start != 0xffu) {
        uint8_t max_start = count > 3u ? (uint8_t)(count - 3u) : 0u;
        start = view_start > max_start ? max_start : view_start;
    }
    for (uint8_t row = 0; row < 3u && start + row < count; row++) {
        uint8_t index = (uint8_t)(start + row);
        draw_flat_list_row(fb, font, labels[index], y0 + row * 10,
                           index == selected);
    }
    draw_position_indicator(fb, selected, count, breadcrumb);
    draw_softkey(fb, softkey);
}

static void draw_flat_list_row(framebuffer_t *fb,
                               const font_t *font,
                               const char *label,
                               int y,
                               bool selected) {
    if (selected) {
        /* Inverted selection bar stops 2px short of the text column's right
         * edge (x0..78), matching v6.00; the label/marquee clips to fit. */
        fb_fill_rect(fb, 0, y, 78, 10, true);
        char scroll[UI_MARQUEE_TEXT_MAX];
        fb_text(fb, font,
                ui_marquee_text(font, label, 76, scroll, sizeof(scroll)),
                2, y + 1, false, 76);
    } else {
        /* Non-selected rows clip to the window with no marker -- the v6.00 ROM
         * has no ellipsis; only the selected row scrolls. */
        fb_text(fb, font, label, 2, y + 1, true, 80);
    }
}

static uint8_t flat_list_view_start(const char *const *labels,
                                    uint8_t count,
                                    uint8_t selected,
                                    const char *softkey,
                                    int y0) {
    static const char *last_first_label;
    static const char *last_last_label;
    static const char *last_softkey;
    static uint8_t last_count;
    static uint8_t last_selected;
    static uint8_t start;
    static int last_y0;

    if (count <= 3u) {
        start = 0u;
        last_first_label = count > 0u ? labels[0] : 0;
        last_last_label = count > 0u ? labels[count - 1u] : 0;
        last_softkey = softkey;
        last_count = count;
        last_selected = selected;
        last_y0 = y0;
        return 0u;
    }

    uint8_t max_start = (uint8_t)(count - 3u);
    const char *first_label = labels[0];
    const char *last_label = labels[count - 1u];
    bool same_list = last_first_label == first_label &&
                     last_last_label == last_label &&
                     last_softkey == softkey &&
                     last_count == count &&
                     last_y0 == y0;
    if (!same_list) {
        start = selected > 2u ? (uint8_t)(selected - 2u) : 0u;
    } else if (selected == (uint8_t)(last_selected + 1u)) {
        if (selected > (uint8_t)(start + 2u)) {
            start = (uint8_t)(selected - 2u);
        }
    } else if ((uint8_t)(selected + 1u) == last_selected) {
        if (selected < start) {
            start = selected;
        }
    } else if (selected != last_selected) {
        start = selected > 2u ? (uint8_t)(selected - 2u) : 0u;
    }

    if (start > max_start) {
        start = max_start;
    }
    if (selected < start) {
        start = selected;
    } else if (selected > (uint8_t)(start + 2u)) {
        start = (uint8_t)(selected - 2u);
    }

    last_first_label = first_label;
    last_last_label = last_label;
    last_softkey = softkey;
    last_count = count;
    last_selected = selected;
    last_y0 = y0;
    return start;
}

void draw_static_page_list(framebuffer_t *fb,
                           const char *label,
                           const char *preview,
                           uint8_t selected,
                           uint8_t count,
                           const char *breadcrumb,
                           const char *softkey) {
    fb_clear(fb, false);
    const font_t *font = asset_font(FONT_FS2);
    const font_t *preview_font = asset_font(FONT_FS1);
    /* Word-wrap the current item's label to the ~80px text column (left of the
     * scrollbar at x81), honoring the PPM's authored '\n'. 32-byte slots hold a
     * full-width line of two-byte glyphs -- the old 18-byte slots truncated
     * Russian/Greek lines (last letter cut) and forced mid-word breaks. Some
     * languages store a label with NO '\n' (e.g. RUSS "Графические сообщения"),
     * so wrapping -- not '\n'-splitting -- is what puts it on two lines. */
    char lines[3][32];
    uint8_t max_lines = preview != 0 && preview[0] != '\0' ? 2u : 3u;
    uint8_t line_count = wrap_text_lines_ex(font, label, 80, (char *)lines, 32u, max_lines);
    for (uint8_t i = 0; i < line_count; i++) {
        fb_text(fb, font, lines[i], 0, 7 + i * 9, true, 80);
    }
    if (preview != 0 && preview[0] != '\0') {
        draw_right_text_box(fb, preview_font, preview, 0, 31, 78);
    }
    draw_position_indicator(fb, selected, count, breadcrumb);
    draw_softkey(fb, softkey);
}

static size_t marquee_fit_end(const font_t *font, const char *text, size_t start, int max_width) {
    /* Advance by whole codepoints (glyphs), never bytes, so a chunk boundary
     * never splits a multibyte UTF-8 sequence -- the C equivalent of the ROM
     * marquee's "skip 0xFF advance-cache entries" rule. */
    int fit = asset_text_fit_bytes(font, &text[start], max_width);
    if (fit > 0) {
        return start + (size_t)fit;
    }
    /* Nothing fits (a single glyph wider than the window): still advance one
     * whole codepoint so the scan always progresses on a glyph boundary. */
    const char *p = &text[start];
    if (*p != '\0') {
        asset_next_codepoint(&p);
    }
    return (size_t)(p - text);
}

const char *ui_marquee_text(const font_t *font, const char *text, int max_width, char *buf, size_t cap) {
    if (text == 0 || buf == 0 || cap == 0u) {
        return "";
    }
    char clean[UI_MARQUEE_TEXT_MAX];
    size_t clean_len = 0u;
    for (size_t i = 0; text[i] != '\0' && clean_len + 1u < sizeof(clean); i++) {
        clean[clean_len++] = text[i] == '\n' ? ' ' : text[i];
    }
    clean[clean_len] = '\0';

    if (asset_text_width(font, clean) <= max_width) {
        if (s_marquee.active && strncmp(s_marquee.text, clean, sizeof(s_marquee.text)) == 0) {
            s_marquee.active = false;
        }
        copy_text(buf, cap, clean);
        return buf;
    }

    bool same = s_marquee.active &&
                s_marquee.font == font &&
                s_marquee.width == max_width &&
                strncmp(s_marquee.text, clean, sizeof(s_marquee.text)) == 0;
    if (!same) {
        memset(&s_marquee, 0, sizeof(s_marquee));
        copy_text(s_marquee.text, sizeof(s_marquee.text), clean);
        s_marquee.font = font;
        s_marquee.width = max_width;
        s_marquee.active = true;
        s_marquee.last_step_ms = time_ms();
    }

    uint8_t count = 0u;
    uint8_t starts[UI_MARQUEE_MAX_CHUNKS];
    starts[count++] = 0u;
    size_t start = 0u;
    while (start < clean_len && count < UI_MARQUEE_MAX_CHUNKS) {
        size_t end = marquee_fit_end(font, clean, start, max_width);
        if (end >= clean_len) {
            break;
        }
        start = end;
        starts[count++] = (uint8_t)start;
    }
    if (count > 1u) {
        /* 0x80 mode: end-align the final chunk -- advance the start by whole
         * codepoints to the earliest glyph boundary whose tail still fits the
         * window, so the end-aligned chunk never begins mid-sequence. */
        const char *p = clean;
        size_t s = 0u;
        while (s < starts[count - 1u] && asset_text_width(font, &clean[s]) > max_width) {
            asset_next_codepoint(&p);
            s = (size_t)(p - clean);
        }
        starts[count - 1u] = (uint8_t)s;
    }
    s_marquee.chunk_count = count;
    memcpy(s_marquee.starts, starts, sizeof(starts));
    s_marquee.seen = true;
    if (s_marquee.step >= count) {
        s_marquee.step = 0u;
    }

    if (s_marquee.step == 0u) {
        /* Static fit from index 0: hand the whole cleaned label to fb_text, which
         * clips to the window on a glyph boundary (the ROM shows no ellipsis). */
        copy_text(buf, cap, clean);
        return buf;
    }
    size_t chunk_start = s_marquee.starts[s_marquee.step];
    size_t chunk_end = marquee_fit_end(font, clean, chunk_start, max_width);
    size_t n = chunk_end - chunk_start;
    if (n >= cap) {
        n = cap - 1u;
    }
    memcpy(buf, &clean[chunk_start], n);
    buf[n] = '\0';
    return buf;
}

bool ui_marquee_tick(uint32_t now) {
    if (!s_marquee.active) {
        return false;
    }
    if (s_marquee.check_pending) {
        s_marquee.check_pending = false;
        if (!s_marquee.seen) {
            /* The marquee row left the screen without a key change. */
            s_marquee.active = false;
            return false;
        }
    }
    if (s_marquee.chunk_count <= 1u) {
        return false;
    }
    if (time_diff_ms(now, s_marquee.last_step_ms + UI_MARQUEE_DELAY_MS) < 0) {
        return false;
    }
    s_marquee.step = (uint8_t)((s_marquee.step + 1u) % s_marquee.chunk_count);
    s_marquee.last_step_ms = now;
    s_marquee.seen = false;
    s_marquee.check_pending = true;
    return true;
}

static const char UI_BREADCRUMB_NONE_VALUE[] = "";
const char *const UI_BREADCRUMB_NONE = UI_BREADCRUMB_NONE_VALUE;

const char *ui_breadcrumb_path(char *buf, size_t cap, const char *parent, unsigned selected_ordinal) {
    if (parent != 0 && parent[0] != '\0') {
        snprintf(buf, cap, "%s-%u", parent, selected_ordinal);
    } else {
        snprintf(buf, cap, "%u", selected_ordinal);
    }
    return buf;
}

void draw_position_indicator(framebuffer_t *fb, uint16_t selected, uint16_t count, const char *breadcrumb) {
    char fallback[6];
    const char *text = breadcrumb;
    if (text == UI_BREADCRUMB_NONE) {
        text = 0;
    } else if (text == 0 || text[0] == '\0') {
        snprintf(fallback, sizeof(fallback), "%u", (unsigned)(selected + 1u));
        text = fallback;
    }
    if (text != 0 && text[0] != '\0') {
        draw_right_text_box(fb, asset_font(FONT_FS3), text, 0, 0, FB_WIDTH);
    }
    if (count <= 1u) {
        return;
    }
    fb_vline(fb, 81, 7, 30, true);
    int max_offset = 23;
    int offset = selected >= count - 1u ? max_offset : (int)(selected * max_offset) / (int)(count - 1u);
    fb_bitmap(fb, 263u, 81, 7 + offset, true, false);
}

void draw_text_right(framebuffer_t *fb, const font_t *font, const char *text, int y, int width) {
    draw_right_text_box(fb, font, text, 0, y, width);
}

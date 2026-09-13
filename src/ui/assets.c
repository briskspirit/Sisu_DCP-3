#include "ui/assets.h"

static const glyph_t *find_glyph_codepoint(const font_t *font, uint16_t codepoint);

/* C7c text model: keypad entry emits single-byte CP1252, while static labels,
 * decoded messages, and prefilled editors may carry UTF-8 (native language
 * names: Greek, Cyrillic, Hebrew, Arabic presentation forms). The decoder
 * consumes valid UTF-8 and falls back to one CP1252 byte for everything else;
 * cursor/layout code uses its boundaries rather than assuming one byte per
 * glyph. RTL labels are stored in visual order. */

/* CP1252 0x80..0x9F to Unicode (0xA0..0xFF maps 1:1 to Latin-1). */
static const uint16_t CP1252_HIGH[32] = {
    0x20ac, 0x0081, 0x201a, 0x0192, 0x201e, 0x2026, 0x2020, 0x2021,
    0x02c6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008d, 0x017d, 0x008f,
    0x0090, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014,
    0x02dc, 0x2122, 0x0161, 0x203a, 0x0153, 0x009d, 0x017e, 0x0178,
};

static uint16_t cp1252_to_unicode(uint8_t byte) {
    if (byte >= 0x80u && byte <= 0x9fu) {
        return CP1252_HIGH[byte - 0x80u];
    }
    return byte;
}

uint16_t asset_next_codepoint(const char **cursor) {
    const uint8_t *s = (const uint8_t *)*cursor;
    uint8_t b0 = s[0];
    if (b0 < 0x80u) {
        (*cursor)++;
        return b0;
    }
    if ((b0 & 0xe0u) == 0xc0u && (s[1] & 0xc0u) == 0x80u) {
        uint16_t cp = (uint16_t)(((uint16_t)(b0 & 0x1fu) << 6) | (s[1] & 0x3fu));
        if (cp >= 0x80u) {
            *cursor += 2;
            return cp;
        }
    } else if ((b0 & 0xf0u) == 0xe0u && (s[1] & 0xc0u) == 0x80u && (s[2] & 0xc0u) == 0x80u) {
        uint16_t cp = (uint16_t)(((uint16_t)(b0 & 0x0fu) << 12) |
                                 ((uint16_t)(s[1] & 0x3fu) << 6) |
                                 (s[2] & 0x3fu));
        /* Reject overlong forms (< 0x800) and UTF-16 surrogates (0xD800..0xDFFF),
         * per RFC 3629 -- mirrors the 2-byte path's `cp >= 0x80` guard. An invalid
         * sequence falls through to the CP1252 single-byte path below (consuming
         * only the lead byte), instead of being silently accepted as a glyph. */
        if (cp >= 0x800u && (cp < 0xd800u || cp > 0xdfffu)) {
            *cursor += 3;
            return cp;
        }
    }
    (*cursor)++;
    return cp1252_to_unicode(b0);
}

const font_t *asset_font(font_id_t font_id) {
    if ((unsigned)font_id >= FONT_COUNT) {
        font_id = FONT_FS2;
    }
    return &g_fonts[font_id];
}

const glyph_t *asset_glyph_codepoint(const font_t *font, uint16_t cp) {
    if (font == 0) {
        return 0;
    }
    const glyph_t *glyph = find_glyph_codepoint(font, cp);
    if (glyph != 0) {
        return glyph;
    }
    if (cp >= 'a' && cp <= 'z') {
        glyph = find_glyph_codepoint(font, (uint16_t)(cp - ('a' - 'A')));
    } else if (cp >= 0xe0u && cp <= 0xfeu && cp != 0xf7u) {
        /* Latin-1 lowercase accents fall back to their uppercase forms. */
        glyph = find_glyph_codepoint(font, (uint16_t)(cp - 0x20u));
    }
    if (glyph != 0) {
        return glyph;
    }
    glyph = find_glyph_codepoint(font, '?');
    if (glyph != 0) {
        return glyph;
    }
    return find_glyph_codepoint(font, ' ');
}

const glyph_t *asset_glyph(const font_t *font, char ch) {
    return asset_glyph_codepoint(font, cp1252_to_unicode((uint8_t)ch));
}

const uint8_t *asset_glyph_data(const font_t *font, const glyph_t *glyph) {
    if (font == 0 || glyph == 0 || glyph->data_offset >= font->data_size) {
        return 0;
    }
    return &font->data[glyph->data_offset];
}

int asset_text_width(const font_t *font, const char *text) {
    if (font == 0 || text == 0) {
        return 0;
    }
    int width = 0;
    bool first = true;
    while (*text) {
        const glyph_t *glyph = asset_glyph_codepoint(font, asset_next_codepoint(&text));
        if (glyph == 0) {
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

int asset_text_fit_bytes(const font_t *font, const char *text, int max_width) {
    if (font == 0 || text == 0) {
        return 0;
    }
    /* Longest prefix that fits in max_width px, ending on a CODEPOINT boundary,
     * using the same per-glyph + inter-glyph-spacing model and clip rule as
     * fb_text. Lets the marquee slicer cut on glyph boundaries instead
     * of bytes, so a multibyte UTF-8 sequence is never split. A negative
     * max_width means "no limit" (mirrors fb_text). */
    const char *cursor = text;
    int width = 0;
    bool first = true;
    size_t fit = 0u;
    while (*cursor) {
        const glyph_t *glyph = asset_glyph_codepoint(font, asset_next_codepoint(&cursor));
        if (glyph != 0) {
            int spacing = first ? 0 : font->spacing;
            if (max_width >= 0 && width + spacing + glyph->width > max_width) {
                break;
            }
            width += spacing + glyph->width;
            first = false;
        }
        fit = (size_t)(cursor - text);
    }
    return (int)fit;
}

const bitmap_t *asset_bitmap(uint16_t bitmap_id) {
    uint16_t lo = 0;
    uint16_t hi = g_bitmaps.count;
    while (lo < hi) {
        uint16_t mid = (uint16_t)(lo + ((hi - lo) >> 1));
        const bitmap_t *item = &g_bitmaps.items[mid];
        if (item->id == bitmap_id) {
            return item;
        }
        if (item->id < bitmap_id) {
            lo = (uint16_t)(mid + 1u);
        } else {
            hi = mid;
        }
    }
    return 0;
}

const uint8_t *asset_bitmap_data(const bitmap_t *bitmap) {
    if (bitmap == 0 || bitmap->data_offset >= g_bitmaps.data_size) {
        return 0;
    }
    return &g_bitmaps.data[bitmap->data_offset];
}

static const glyph_t *find_glyph_codepoint(const font_t *font, uint16_t codepoint) {
    uint16_t lo = 0;
    uint16_t hi = font->glyph_count;
    while (lo < hi) {
        uint16_t mid = (uint16_t)(lo + ((hi - lo) >> 1));
        const glyph_t *glyph = &font->glyphs[mid];
        if (glyph->codepoint == codepoint) {
            return glyph;
        }
        if (glyph->codepoint < codepoint) {
            lo = (uint16_t)(mid + 1u);
        } else {
            hi = mid;
        }
    }
    return 0;
}

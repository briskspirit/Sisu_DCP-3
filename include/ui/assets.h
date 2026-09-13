#ifndef ASSETS_H
#define ASSETS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    FONT_FS0 = 0,
    FONT_FS1,
    FONT_FS2,
    FONT_FS3,
    FONT_FS4,
    FONT_COUNT,
} font_id_t;

typedef struct {
    uint16_t codepoint;
    uint8_t width;
    uint8_t height;
    uint16_t data_offset;
} glyph_t;

typedef struct {
    const char *name;
    const glyph_t *glyphs;
    uint16_t glyph_count;
    const uint8_t *data;
    uint16_t data_size;
    uint8_t height;
    uint8_t spacing;
} font_t;

typedef struct {
    uint16_t id;
    uint8_t width;
    uint8_t height;
    uint16_t data_offset;
} bitmap_t;

typedef struct {
    const bitmap_t *items;
    uint16_t count;
    const uint8_t *data;
    uint16_t data_size;
} bitmap_store_t;

const font_t *asset_font(font_id_t font_id);
const glyph_t *asset_glyph(const font_t *font, char ch);
const glyph_t *asset_glyph_codepoint(const font_t *font, uint16_t cp);
/* Hybrid decoder: consumes a valid UTF-8 sequence (static labels) or one
 * CP1252 byte (editable text). Advances *cursor past the consumed bytes. */
uint16_t asset_next_codepoint(const char **cursor);
const uint8_t *asset_glyph_data(const font_t *font, const glyph_t *glyph);
int asset_text_width(const font_t *font, const char *text);
/* Byte length of the longest prefix of text that fits in max_width px, ending on
 * a codepoint boundary (same metrics/clip as fb_text). max_width < 0 = no limit. */
int asset_text_fit_bytes(const font_t *font, const char *text, int max_width);

const bitmap_t *asset_bitmap(uint16_t bitmap_id);
const uint8_t *asset_bitmap_data(const bitmap_t *bitmap);

extern const font_t g_fonts[FONT_COUNT];
extern const bitmap_store_t g_bitmaps;

#endif


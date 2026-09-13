#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ui/framebuffer.h"

/* ------------------------------------------------------------------------- */
/* Stubs for the asset layer.                                                  */
/* framebuffer.c references asset_glyph_codepoint / asset_next_codepoint /      */
/* asset_glyph_data (via fb_text) and asset_bitmap / asset_bitmap_data (via     */
/* fb_bitmap). We provide controllable host stubs so we can feed fb_blit_vlsb   */
/* exact geometry/data without linking the generated asset tables. fb_text and  */
/* fb_bitmap are only exercised lightly; the focus is the drawing primitives.   */
/* g_fonts / g_bitmaps are declared extern in assets.h and must be defined.     */
/* ------------------------------------------------------------------------- */

const font_t g_fonts[FONT_COUNT];
const bitmap_store_t g_bitmaps;

/* A 5x7 test font: one glyph for codepoint 'A' that is a solid 5x7 block.
 * VLSB layout: width columns, each column 1 byte (height 7 -> 1 bank). */
static const uint8_t k_glyph_A_data[5] = {0x7f, 0x7f, 0x7f, 0x7f, 0x7f};
static const glyph_t k_glyph_A = {.codepoint = 'A', .width = 5, .height = 7, .data_offset = 0};
static font_t k_test_font;

const font_t *asset_font(font_id_t font_id) {
    (void)font_id;
    return &k_test_font;
}
const glyph_t *asset_glyph(const font_t *font, char ch) {
    (void)font;
    (void)ch;
    return 0;
}
const glyph_t *asset_glyph_codepoint(const font_t *font, uint16_t cp) {
    (void)font;
    if (cp == 'A') {
        return &k_glyph_A;
    }
    return 0;
}
uint16_t asset_next_codepoint(const char **cursor) {
    uint16_t cp = (uint16_t)(uint8_t)**cursor;
    (*cursor)++;
    return cp;
}
const uint8_t *asset_glyph_data(const font_t *font, const glyph_t *glyph) {
    (void)font;
    if (glyph == &k_glyph_A) {
        return k_glyph_A_data;
    }
    return 0;
}
int asset_text_width(const font_t *font, const char *text) {
    (void)font;
    (void)text;
    return 0;
}
int asset_text_fit_bytes(const font_t *font, const char *text, int max_width) {
    (void)font;
    (void)text;
    (void)max_width;
    return 0;
}

static const bitmap_t *s_stub_bitmap;
static const uint8_t *s_stub_bitmap_data;
const bitmap_t *asset_bitmap(uint16_t bitmap_id) {
    (void)bitmap_id;
    return s_stub_bitmap;
}
const uint8_t *asset_bitmap_data(const bitmap_t *bitmap) {
    (void)bitmap;
    return s_stub_bitmap_data;
}

/* ------------------------------------------------------------------------- */
/* Test harness.                                                              */
/* ------------------------------------------------------------------------- */

static int s_failures;

static void assert_true(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        s_failures++;
    }
}

static void assert_eq_int(long got, long want, const char *message) {
    if (got != want) {
        fprintf(stderr, "FAIL: %s (got %ld, want %ld)\n", message, got, want);
        s_failures++;
    }
}

/* Independent reference: read bit (x,y) directly out of the raw buffer using
 * the documented 1bpp vertical-LSB packing (index=(y/8)*WIDTH+x, bit=y&7).
 * Used to cross-check fb_get_pixel and the geometry of every primitive without
 * trusting the module's own accessor for the bit math. */
static bool raw_bit(const framebuffer_t *fb, int x, int y) {
    int index = (y >> 3) * (int)FB_WIDTH + x;
    return (fb->data[index] & (1u << (y & 7))) != 0;
}

/* Count how many of the 84*48 on-screen pixels are set. */
static int count_set(const framebuffer_t *fb) {
    int n = 0;
    for (int y = 0; y < (int)FB_HEIGHT; y++) {
        for (int x = 0; x < (int)FB_WIDTH; x++) {
            if (raw_bit(fb, x, y)) {
                n++;
            }
        }
    }
    return n;
}

/* ------------------------------------------------------------------------- */
/* fb_clear                                                                   */
/* ------------------------------------------------------------------------- */

static void test_clear(void) {
    framebuffer_t fb;
    memset(&fb, 0xaa, sizeof(fb));
    fb_clear(&fb, false);
    int nonzero = 0;
    for (size_t i = 0; i < FB_SIZE; i++) {
        if (fb.data[i] != 0x00) {
            nonzero++;
        }
    }
    assert_eq_int(nonzero, 0, "clear(false) zeroes all bytes");
    assert_eq_int(count_set(&fb), 0, "clear(false) -> no pixels set");

    fb_clear(&fb, true);
    int notff = 0;
    for (size_t i = 0; i < FB_SIZE; i++) {
        if (fb.data[i] != 0xff) {
            notff++;
        }
    }
    assert_eq_int(notff, 0, "clear(true) sets all bytes to 0xff");
    assert_eq_int(count_set(&fb), (int)(FB_WIDTH * FB_HEIGHT), "clear(true) -> every pixel set");
}

/* ------------------------------------------------------------------------- */
/* fb_pixel / fb_get_pixel : packing, clipping, idempotence                   */
/* ------------------------------------------------------------------------- */

static void test_pixel_packing(void) {
    framebuffer_t fb;
    fb_clear(&fb, false);

    /* Corners and bank boundaries. */
    int xs[] = {0, 1, 41, 82, 83};
    int ys[] = {0, 1, 7, 8, 15, 16, 23, 24, 39, 47};
    for (size_t xi = 0; xi < sizeof(xs) / sizeof(xs[0]); xi++) {
        for (size_t yi = 0; yi < sizeof(ys) / sizeof(ys[0]); yi++) {
            int x = xs[xi];
            int y = ys[yi];
            fb_clear(&fb, false);
            fb_pixel(&fb, x, y, true);
            assert_true(fb_get_pixel(&fb, x, y), "set pixel reads back set");
            assert_true(raw_bit(&fb, x, y), "set pixel matches raw VLSB packing");
            assert_eq_int(count_set(&fb), 1, "single pixel sets exactly one bit");
            fb_pixel(&fb, x, y, false);
            assert_true(!fb_get_pixel(&fb, x, y), "cleared pixel reads back clear");
            assert_eq_int(count_set(&fb), 0, "clearing the pixel clears exactly that bit");
        }
    }

    /* Idempotence: setting twice == setting once. */
    fb_clear(&fb, false);
    fb_pixel(&fb, 10, 10, true);
    fb_pixel(&fb, 10, 10, true);
    assert_eq_int(count_set(&fb), 1, "set pixel is idempotent");
}

static void test_pixel_clipping(void) {
    framebuffer_t fb;
    fb_clear(&fb, false);

    /* Every off-screen coord must be a no-op (and not corrupt the buffer). */
    int coords[][2] = {
        {-1, 0},   {0, -1},     {-1, -1},  {84, 0},    {0, 48},
        {84, 48},  {-1000, 5},  {5, -1000}, {1000, 5}, {5, 1000},
        {83, 48},  {84, 47},    {-1, 47},   {0, -1},
    };
    for (size_t i = 0; i < sizeof(coords) / sizeof(coords[0]); i++) {
        fb_pixel(&fb, coords[i][0], coords[i][1], true);
    }
    assert_eq_int(count_set(&fb), 0, "off-screen fb_pixel writes nothing (ASan guards OOB)");

    /* fb_get_pixel of off-screen coords returns false, never OOB-reads. */
    for (size_t i = 0; i < sizeof(coords) / sizeof(coords[0]); i++) {
        assert_true(!fb_get_pixel(&fb, coords[i][0], coords[i][1]), "off-screen get_pixel is false");
    }

    /* Clearing off-screen on an all-set buffer must leave it fully set. */
    fb_clear(&fb, true);
    for (size_t i = 0; i < sizeof(coords) / sizeof(coords[0]); i++) {
        fb_pixel(&fb, coords[i][0], coords[i][1], false);
    }
    assert_eq_int(count_set(&fb), (int)(FB_WIDTH * FB_HEIGHT), "off-screen clear is a no-op");
}

/* ------------------------------------------------------------------------- */
/* fb_hline / fb_vline                                                        */
/* ------------------------------------------------------------------------- */

static void test_hline(void) {
    framebuffer_t fb;

    /* Fully on-screen hline sets exactly w pixels on row y. */
    fb_clear(&fb, false);
    fb_hline(&fb, 10, 20, 30, true);
    assert_eq_int(count_set(&fb), 30, "hline sets exactly w pixels");
    for (int x = 10; x < 40; x++) {
        assert_true(raw_bit(&fb, x, 20), "hline pixel set");
    }
    assert_true(!raw_bit(&fb, 9, 20), "hline left boundary exclusive");
    assert_true(!raw_bit(&fb, 40, 20), "hline right boundary exclusive");

    /* Partially off-screen left: x=-5,w=10 -> on-screen x in [0,5). */
    fb_clear(&fb, false);
    fb_hline(&fb, -5, 5, 10, true);
    assert_eq_int(count_set(&fb), 5, "hline clipped at left edge sets 5");
    for (int x = 0; x < 5; x++) {
        assert_true(raw_bit(&fb, x, 5), "left-clipped hline pixel");
    }

    /* Partially off-screen right: x=80,w=10 -> x in [80,84). */
    fb_clear(&fb, false);
    fb_hline(&fb, 80, 5, 10, true);
    assert_eq_int(count_set(&fb), 4, "hline clipped at right edge sets 4");

    /* Fully off-screen row. */
    fb_clear(&fb, false);
    fb_hline(&fb, 0, 48, 84, true);
    assert_eq_int(count_set(&fb), 0, "hline on off-screen row sets nothing");
    fb_hline(&fb, 0, -1, 84, true);
    assert_eq_int(count_set(&fb), 0, "hline on negative row sets nothing");

    /* Zero / negative width -> no-op. */
    fb_clear(&fb, false);
    fb_hline(&fb, 10, 10, 0, true);
    assert_eq_int(count_set(&fb), 0, "hline w=0 is a no-op");
    fb_hline(&fb, 10, 10, -5, true);
    assert_eq_int(count_set(&fb), 0, "hline negative w is a no-op");

    /* Full width line. */
    fb_clear(&fb, false);
    fb_hline(&fb, 0, 0, (int)FB_WIDTH, true);
    assert_eq_int(count_set(&fb), (int)FB_WIDTH, "full-width hline sets WIDTH pixels");
}

static void test_vline(void) {
    framebuffer_t fb;

    fb_clear(&fb, false);
    fb_vline(&fb, 20, 10, 30, true);
    assert_eq_int(count_set(&fb), 30, "vline sets exactly h pixels");
    for (int y = 10; y < 40; y++) {
        assert_true(raw_bit(&fb, 20, y), "vline pixel set");
    }
    assert_true(!raw_bit(&fb, 20, 9), "vline top boundary exclusive");
    assert_true(!raw_bit(&fb, 20, 40), "vline bottom boundary exclusive");

    /* Crosses several banks (y 0..47). */
    fb_clear(&fb, false);
    fb_vline(&fb, 0, 0, (int)FB_HEIGHT, true);
    assert_eq_int(count_set(&fb), (int)FB_HEIGHT, "full-height vline sets HEIGHT pixels");

    /* Clipping top/bottom. */
    fb_clear(&fb, false);
    fb_vline(&fb, 5, -5, 10, true);
    assert_eq_int(count_set(&fb), 5, "vline clipped at top sets 5");
    fb_clear(&fb, false);
    fb_vline(&fb, 5, 44, 10, true);
    assert_eq_int(count_set(&fb), 4, "vline clipped at bottom sets 4");

    /* Off-screen column. */
    fb_clear(&fb, false);
    fb_vline(&fb, 84, 0, 48, true);
    assert_eq_int(count_set(&fb), 0, "vline on off-screen column sets nothing");
    fb_vline(&fb, -1, 0, 48, true);
    assert_eq_int(count_set(&fb), 0, "vline on negative column sets nothing");

    /* Zero / negative h. */
    fb_clear(&fb, false);
    fb_vline(&fb, 10, 10, 0, true);
    assert_eq_int(count_set(&fb), 0, "vline h=0 is a no-op");
    fb_vline(&fb, 10, 10, -3, true);
    assert_eq_int(count_set(&fb), 0, "vline negative h is a no-op");
}

/* ------------------------------------------------------------------------- */
/* fb_rect (outline)                                                          */
/* ------------------------------------------------------------------------- */

static void test_rect(void) {
    framebuffer_t fb;

    /* A wxh outline of an on-screen rect: perimeter = 2w + 2h - 4 pixels
     * (the four corners are shared). */
    fb_clear(&fb, false);
    int w = 10, h = 8;
    fb_rect(&fb, 5, 5, w, h, true);
    int expected = 2 * w + 2 * h - 4;
    assert_eq_int(count_set(&fb), expected, "rect outline perimeter pixel count");
    /* Corners. */
    assert_true(raw_bit(&fb, 5, 5), "rect TL corner");
    assert_true(raw_bit(&fb, 5 + w - 1, 5), "rect TR corner");
    assert_true(raw_bit(&fb, 5, 5 + h - 1), "rect BL corner");
    assert_true(raw_bit(&fb, 5 + w - 1, 5 + h - 1), "rect BR corner");
    /* Interior is hollow. */
    assert_true(!raw_bit(&fb, 6, 6), "rect interior is hollow");
    assert_true(!raw_bit(&fb, 8, 8), "rect interior is hollow 2");

    /* 1x1 rect == single pixel (perimeter formula: 2+2-4=0; but the four lines
     * all collapse onto one pixel). Verify it sets exactly that one pixel. */
    fb_clear(&fb, false);
    fb_rect(&fb, 3, 3, 1, 1, true);
    assert_eq_int(count_set(&fb), 1, "1x1 rect sets one pixel");
    assert_true(raw_bit(&fb, 3, 3), "1x1 rect pixel at origin");

    /* 1xN and Nx1 rects (degenerate to a line). */
    fb_clear(&fb, false);
    fb_rect(&fb, 2, 2, 1, 5, true);
    assert_eq_int(count_set(&fb), 5, "1x5 rect == vertical line of 5");
    fb_clear(&fb, false);
    fb_rect(&fb, 2, 2, 5, 1, true);
    assert_eq_int(count_set(&fb), 5, "5x1 rect == horizontal line of 5");

    /* Zero / negative dims -> no-op. */
    fb_clear(&fb, false);
    fb_rect(&fb, 5, 5, 0, 8, true);
    assert_eq_int(count_set(&fb), 0, "rect w=0 no-op");
    fb_rect(&fb, 5, 5, 8, 0, true);
    assert_eq_int(count_set(&fb), 0, "rect h=0 no-op");
    fb_rect(&fb, 5, 5, -4, 8, true);
    assert_eq_int(count_set(&fb), 0, "rect negative w no-op");

    /* Rect straddling the right/bottom edge: must clip, never OOB. */
    fb_clear(&fb, false);
    fb_rect(&fb, 80, 44, 10, 10, true);
    /* On-screen portion: x in [80,84) cols, y in [44,48) rows -> a 4x4 region.
     * Top edge y=44 x[80..83]=4; the bottom edge y=53 is off-screen (0);
     * left edge x=80 y[44..47]=4; right edge x=89 off-screen (0). Top and left
     * share corner (80,44). So set = 4 + 4 - 1 = 7. */
    assert_eq_int(count_set(&fb), 7, "rect clipped at BR corner");

    /* Fully off-screen rect. */
    fb_clear(&fb, false);
    fb_rect(&fb, 100, 100, 10, 10, true);
    assert_eq_int(count_set(&fb), 0, "fully off-screen rect no-op");
    fb_rect(&fb, -50, -50, 10, 10, true);
    assert_eq_int(count_set(&fb), 0, "fully off-screen (neg) rect no-op");

    /* Full-screen rect outline. */
    fb_clear(&fb, false);
    fb_rect(&fb, 0, 0, (int)FB_WIDTH, (int)FB_HEIGHT, true);
    int full_expected = 2 * (int)FB_WIDTH + 2 * (int)FB_HEIGHT - 4;
    assert_eq_int(count_set(&fb), full_expected, "full-screen rect outline");
}

/* ------------------------------------------------------------------------- */
/* fb_fill_rect                                                               */
/* ------------------------------------------------------------------------- */

static void test_fill_rect(void) {
    framebuffer_t fb;

    fb_clear(&fb, false);
    fb_fill_rect(&fb, 10, 10, 20, 15, true);
    assert_eq_int(count_set(&fb), 20 * 15, "fill_rect sets w*h pixels");
    for (int y = 10; y < 25; y++) {
        for (int x = 10; x < 30; x++) {
            assert_true(raw_bit(&fb, x, y), "fill_rect pixel set");
        }
    }
    assert_true(!raw_bit(&fb, 9, 10), "fill_rect left boundary exclusive");
    assert_true(!raw_bit(&fb, 30, 10), "fill_rect right boundary exclusive");
    assert_true(!raw_bit(&fb, 10, 9), "fill_rect top boundary exclusive");
    assert_true(!raw_bit(&fb, 10, 25), "fill_rect bottom boundary exclusive");

    /* Whole-screen fill via fill_rect == clear(true). */
    fb_clear(&fb, false);
    fb_fill_rect(&fb, 0, 0, (int)FB_WIDTH, (int)FB_HEIGHT, true);
    assert_eq_int(count_set(&fb), (int)(FB_WIDTH * FB_HEIGHT), "full fill_rect == every pixel");

    /* Fill then clear-fill (color=false) a sub-region. */
    fb_clear(&fb, true);
    fb_fill_rect(&fb, 10, 10, 20, 15, false);
    assert_eq_int(count_set(&fb), (int)(FB_WIDTH * FB_HEIGHT) - 20 * 15, "fill_rect(false) clears region");

    /* Degenerate dims. */
    fb_clear(&fb, false);
    fb_fill_rect(&fb, 10, 10, 0, 10, true);
    assert_eq_int(count_set(&fb), 0, "fill_rect w=0 no-op");
    fb_fill_rect(&fb, 10, 10, 10, 0, true);
    assert_eq_int(count_set(&fb), 0, "fill_rect h=0 no-op");
    fb_fill_rect(&fb, 10, 10, -5, 10, true);
    assert_eq_int(count_set(&fb), 0, "fill_rect negative w no-op");

    /* Off-screen and straddling fills must clip (ASan guards OOB writes). */
    fb_clear(&fb, false);
    fb_fill_rect(&fb, -5, -5, 200, 200, true);
    assert_eq_int(count_set(&fb), (int)(FB_WIDTH * FB_HEIGHT), "huge fill clips to screen, fills all");

    fb_clear(&fb, false);
    fb_fill_rect(&fb, 70, 40, 50, 50, true);
    /* On-screen: x [70,84)=14 wide, y [40,48)=8 tall -> 14*8=112. */
    assert_eq_int(count_set(&fb), 14 * 8, "fill straddling BR clips correctly");

    fb_clear(&fb, false);
    fb_fill_rect(&fb, 100, 100, 5, 5, true);
    assert_eq_int(count_set(&fb), 0, "fully off-screen fill no-op");
}

/* ------------------------------------------------------------------------- */
/* fb_text5 (built-in 5x7 font)                                               */
/* ------------------------------------------------------------------------- */

static void test_text5(void) {
    framebuffer_t fb;

    /* Returned advance width: N glyphs at 6px stride -> cx-x == 6*N. */
    fb_clear(&fb, false);
    int adv = fb_text5(&fb, "ABC", 0, 0, true, -1);
    assert_eq_int(adv, 18, "text5 advance = 6 per glyph");

    /* fb_text5_width: N glyphs -> 6N-1 (no trailing inter-char gap). */
    assert_eq_int(fb_text5_width("ABC"), 17, "text5_width = 6N-1");
    assert_eq_int(fb_text5_width(""), 0, "text5_width empty = 0");
    assert_eq_int(fb_text5_width("A"), 5, "text5_width single = 5");

    /* Empty string draws nothing, advance 0. */
    fb_clear(&fb, false);
    adv = fb_text5(&fb, "", 10, 10, true, -1);
    assert_eq_int(adv, 0, "empty text5 advance 0");
    assert_eq_int(count_set(&fb), 0, "empty text5 draws nothing");

    /* A space glyph is all-zero -> draws nothing but advances 6. */
    fb_clear(&fb, false);
    adv = fb_text5(&fb, " ", 0, 0, true, -1);
    assert_eq_int(adv, 6, "space advance 6");
    assert_eq_int(count_set(&fb), 0, "space draws no pixels");

    /* max_width clipping: with max_width=5, only the first glyph fits
     * ((cx-x)+5 > 5 is false only for the first). After first glyph cx-x=6,
     * 6+5>5 -> break. So exactly one glyph drawn, advance 6. */
    fb_clear(&fb, false);
    adv = fb_text5(&fb, "WWWW", 0, 0, true, 5);
    assert_eq_int(adv, 6, "text5 max_width=5 draws one glyph");

    /* max_width=0 -> 0+5>0 true immediately -> nothing drawn. */
    fb_clear(&fb, false);
    adv = fb_text5(&fb, "W", 0, 0, true, 0);
    assert_eq_int(adv, 0, "text5 max_width=0 draws nothing");
    assert_eq_int(count_set(&fb), 0, "text5 max_width=0 no pixels");

    /* Drawing partly/fully off-screen must clip via fb_pixel (no OOB). */
    fb_clear(&fb, false);
    fb_text5(&fb, "ABCDEFGHIJKLMNOP", 70, 42, true, -1); /* runs off right & bottom */
    /* Just assert it didn't crash and stayed within bounds (ASan). */
    assert_true(count_set(&fb) >= 0, "text5 off-screen does not crash");

    fb_clear(&fb, false);
    fb_text5(&fb, "ABC", -20, -5, true, -1); /* negative origin */
    assert_true(count_set(&fb) >= 0, "text5 negative origin does not crash");

    /* '1' digit glyph {0x00,0x42,0x7f,0x40,0x00}: column 2 (0x7f) has rows 0..6
     * all set -> a 7px vertical bar; columns 1 and 3 have one pixel each. Verify
     * the geometry against the bitmap definition. */
    fb_clear(&fb, false);
    fb_text5(&fb, "1", 0, 0, true, -1);
    for (int row = 0; row < 7; row++) {
        assert_true(raw_bit(&fb, 2, row), "digit 1 center column bar");
    }
    assert_true(raw_bit(&fb, 1, 1), "digit 1 col1 row1 (0x42 bit1)");
    assert_true(raw_bit(&fb, 1, 6), "digit 1 col1 row6 (0x42 bit6)");
    assert_true(raw_bit(&fb, 3, 6), "digit 1 col3 row6 (0x40 bit6)");
    assert_true(!raw_bit(&fb, 0, 0), "digit 1 col0 empty");
}

/* ------------------------------------------------------------------------- */
/* fb_blit_vlsb : slow path (clipping) and fast path (transparent on-screen)   */
/* ------------------------------------------------------------------------- */

/* Build a VLSB sprite: w columns, ceil(h/8) banks, all bits within [0,h) set. */
static void make_solid_sprite(uint8_t *buf, int w, int h) {
    int banks = (h + 7) >> 3;
    memset(buf, 0, (size_t)(w * banks));
    for (int yy = 0; yy < h; yy++) {
        for (int xx = 0; xx < w; xx++) {
            buf[(yy >> 3) * w + xx] |= (uint8_t)(1u << (yy & 7));
        }
    }
}

static void test_blit_slow_path(void) {
    framebuffer_t fb;
    uint8_t sprite[8 * 4]; /* up to 8 wide, 4 banks (32 tall) */

    /* 8x8 solid, transparent, placed off-grid at (3,5): NOT fully on-screen?
     * It IS on-screen (3+8<=84, 5+8<=48) so this hits the FAST path. Use a
     * non-byte-aligned y to exercise the shift logic. */
    make_solid_sprite(sprite, 8, 8);
    fb_clear(&fb, false);
    fb_blit_vlsb(&fb, sprite, 3, 5, 8, 8, true, true);
    assert_eq_int(count_set(&fb), 8 * 8, "transparent fast-path 8x8 sets 64");
    for (int yy = 0; yy < 8; yy++) {
        for (int xx = 0; xx < 8; xx++) {
            assert_true(raw_bit(&fb, 3 + xx, 5 + yy), "fast-path pixel set");
        }
    }
    assert_true(!raw_bit(&fb, 2, 5), "fast-path left boundary clean");
    assert_true(!raw_bit(&fb, 11, 5), "fast-path right boundary clean");
    assert_true(!raw_bit(&fb, 3, 4), "fast-path top boundary clean");
    assert_true(!raw_bit(&fb, 3, 13), "fast-path bottom boundary clean");

    /* Transparent fast-path with y byte-aligned (shift==0). */
    fb_clear(&fb, false);
    fb_blit_vlsb(&fb, sprite, 10, 8, 8, 8, true, true);
    assert_eq_int(count_set(&fb), 64, "fast-path aligned 8x8 sets 64");

    /* Slow path triggered by transparent but partly OFF-screen (x<0). */
    make_solid_sprite(sprite, 8, 8);
    fb_clear(&fb, false);
    fb_blit_vlsb(&fb, sprite, -3, 5, 8, 8, true, true);
    /* On-screen columns xx in [3,8) -> 5 columns, 8 rows -> 40. */
    assert_eq_int(count_set(&fb), 5 * 8, "slow-path left-clipped blit sets 40");

    /* Slow path: partly off bottom (y near edge, not fully on-screen). */
    fb_clear(&fb, false);
    fb_blit_vlsb(&fb, sprite, 10, 44, 8, 8, true, true);
    /* y+h=52 > 48 so slow path. On-screen rows [44,48) -> 4 rows * 8 cols = 32. */
    assert_eq_int(count_set(&fb), 8 * 4, "slow-path bottom-clipped blit sets 32");

    /* Slow path: NON-transparent on-screen always uses slow path. Solid sprite
     * with transparent=false on a cleared buffer: 'on' pixels set color(true),
     * 'off' pixels set !color(=false). Solid sprite has all-on within [0,h). */
    fb_clear(&fb, false);
    fb_blit_vlsb(&fb, sprite, 0, 0, 8, 8, true, false);
    assert_eq_int(count_set(&fb), 64, "non-transparent solid blit sets 64");

    /* Non-transparent opaque blit of an all-ZERO sprite onto an all-set buffer
     * must CLEAR the covered region (paints background = !color). */
    uint8_t zero[8 * 1];
    memset(zero, 0, sizeof(zero));
    fb_clear(&fb, true);
    fb_blit_vlsb(&fb, zero, 0, 0, 8, 8, true, false);
    assert_eq_int(count_set(&fb), (int)(FB_WIDTH * FB_HEIGHT) - 64, "opaque zero sprite clears 8x8");

    /* Guards: null data, zero/neg dims -> no-op. */
    fb_clear(&fb, false);
    fb_blit_vlsb(&fb, 0, 0, 0, 8, 8, true, true);
    assert_eq_int(count_set(&fb), 0, "blit null data no-op");
    fb_blit_vlsb(&fb, sprite, 0, 0, 0, 8, true, true);
    assert_eq_int(count_set(&fb), 0, "blit w=0 no-op");
    fb_blit_vlsb(&fb, sprite, 0, 0, 8, 0, true, true);
    assert_eq_int(count_set(&fb), 0, "blit h=0 no-op");
    fb_blit_vlsb(&fb, sprite, 0, 0, -4, 8, true, true);
    assert_eq_int(count_set(&fb), 0, "blit negative w no-op");

    /* Fully off-screen transparent (slow path because not fully on-screen). */
    fb_clear(&fb, false);
    fb_blit_vlsb(&fb, sprite, -100, -100, 8, 8, true, true);
    assert_eq_int(count_set(&fb), 0, "fully off-screen blit no-op");
    fb_blit_vlsb(&fb, sprite, 200, 200, 8, 8, true, true);
    assert_eq_int(count_set(&fb), 0, "fully off-screen (pos) blit no-op");
}

/* Cross-check: the transparent FAST path (fully on-screen) must produce the
 * EXACT same pixels as a from-first-principles VLSB reference, for a sprite
 * whose height is not a multiple of 8 and placed at a non-aligned y (so the
 * shift / carry-into-next-bank logic is exercised). */
static void test_blit_fast_vs_reference(void) {
    framebuffer_t fb;
    int w = 6, h = 13; /* 2 banks, last bank partial */
    uint8_t sprite[6 * 2];
    /* A non-trivial pattern. */
    for (size_t i = 0; i < sizeof(sprite); i++) {
        sprite[i] = (uint8_t)(0x35u ^ (i * 7u));
    }
    /* But only bits within [0,h) are meaningful; mask the rest like a real sprite. */
    for (int xx = 0; xx < w; xx++) {
        /* clear bits >= h in the top bank of each column */
        int top_bank_bits = h - 8; /* bits valid in bank 1: rows 8..h-1 */
        if (top_bank_bits < 8) {
            uint8_t keep = (uint8_t)((1u << top_bank_bits) - 1u);
            sprite[1 * w + xx] &= keep;
        }
    }

    int ox = 7, oy = 11; /* oy & 7 = 3, non-aligned */
    fb_clear(&fb, false);
    fb_blit_vlsb(&fb, sprite, ox, oy, w, h, true, true);

    /* Independent reference using fb-coordinate reasoning: pixel (ox+xx, oy+yy)
     * is set iff sprite bit (xx,yy) is set. */
    int mism = 0;
    int onpix = 0;
    for (int yy = 0; yy < h; yy++) {
        for (int xx = 0; xx < w; xx++) {
            bool want = (sprite[(yy >> 3) * w + xx] & (1u << (yy & 7))) != 0;
            if (want) {
                onpix++;
            }
            bool got = raw_bit(&fb, ox + xx, oy + yy);
            if (want != got) {
                mism++;
            }
        }
    }
    assert_eq_int(mism, 0, "fast-path blit matches VLSB reference exactly");
    assert_eq_int(count_set(&fb), onpix, "fast-path sets exactly the sprite's on-bits");

    /* Same sprite/geometry through the SLOW path (force it by shifting one px so
     * it is no longer fully on screen is wrong -- instead compare to opaque=false
     * reference). Verify slow path with transparent gives same result by placing
     * partly off-screen and checking only the on-screen window. */
    fb_clear(&fb, false);
    int ox2 = -2; /* forces slow path (x<0) */
    fb_blit_vlsb(&fb, sprite, ox2, oy, w, h, true, true);
    mism = 0;
    for (int yy = 0; yy < h; yy++) {
        for (int xx = 0; xx < w; xx++) {
            int px = ox2 + xx, py = oy + yy;
            if (px < 0 || px >= (int)FB_WIDTH || py < 0 || py >= (int)FB_HEIGHT) {
                continue;
            }
            bool want = (sprite[(yy >> 3) * w + xx] & (1u << (yy & 7))) != 0;
            if (want != raw_bit(&fb, px, py)) {
                mism++;
            }
        }
    }
    assert_eq_int(mism, 0, "slow-path clipped blit matches reference in-window");
}

/* fast-path with color=false (erase) and shift!=0: clears matching bits on an
 * all-set buffer. */
static void test_blit_fast_erase(void) {
    framebuffer_t fb;
    int w = 5, h = 10;
    uint8_t sprite[5 * 2];
    make_solid_sprite(sprite, w, h);
    fb_clear(&fb, true);
    fb_blit_vlsb(&fb, sprite, 4, 3, w, h, false, true); /* erase, shift=3 */
    assert_eq_int(count_set(&fb), (int)(FB_WIDTH * FB_HEIGHT) - w * h,
                  "fast-path erase clears exactly w*h pixels");
    for (int yy = 0; yy < h; yy++) {
        for (int xx = 0; xx < w; xx++) {
            assert_true(!raw_bit(&fb, 4 + xx, 3 + yy), "erased pixel is clear");
        }
    }
    assert_true(raw_bit(&fb, 3, 3), "pixel left of erase region still set");
}

/* fast-path where the sprite's high bits carry into a bank beyond the last
 * on-screen bank: place an 8-tall sprite at y=44 -> y+h=52 > 48 so it's slow
 * path; instead test the carry-guard: sprite at y=43 with h=5 -> y+h=48 fully
 * on-screen, shift=3, last sprite bank carries into bank index... verify no OOB
 * and correct pixels. */
static void test_blit_fast_carry_guard(void) {
    framebuffer_t fb;
    int w = 4, h = 5;
    uint8_t sprite[4 * 1];
    make_solid_sprite(sprite, w, h); /* rows 0..4 set */
    fb_clear(&fb, false);
    /* y=43, h=5 -> spans rows 43..47 (all on-screen, y+h=48<=48). shift=43&7=3.
     * dst_bank=43>>3=5 (last bank). high bits would go to bank 6 which is OOB;
     * the guard dst_bank+bank+1 < FB_BANKS(6) must suppress that write. */
    fb_blit_vlsb(&fb, sprite, 10, 43, w, h, true, true);
    assert_eq_int(count_set(&fb), w * h, "carry-guard fast-path sets exactly w*h");
    for (int yy = 0; yy < h; yy++) {
        for (int xx = 0; xx < w; xx++) {
            assert_true(raw_bit(&fb, 10 + xx, 43 + yy), "carry-guard pixel set");
        }
    }
}

/* ------------------------------------------------------------------------- */
/* fb_text (proportional font via stubs) and fb_bitmap                         */
/* ------------------------------------------------------------------------- */

static void test_fb_text(void) {
    framebuffer_t fb;
    k_test_font.spacing = 1;
    k_test_font.height = 7;

    /* Null guards. */
    assert_eq_int(fb_text(&fb, 0, "A", 0, 0, true, -1), 0, "fb_text null font -> 0");
    assert_eq_int(fb_text(&fb, &k_test_font, 0, 0, 0, true, -1), 0, "fb_text null text -> 0");

    /* Single 'A' (5 wide): advance = width 5, first glyph no leading spacing. */
    fb_clear(&fb, false);
    int adv = fb_text(&fb, &k_test_font, "A", 0, 0, true, -1);
    assert_eq_int(adv, 5, "fb_text single glyph advance = width");
    assert_eq_int(count_set(&fb), 5 * 7, "fb_text 'A' solid 5x7 = 35 px");

    /* Two glyphs: 5 + spacing(1) + 5 = 11. */
    fb_clear(&fb, false);
    adv = fb_text(&fb, &k_test_font, "AA", 0, 0, true, -1);
    assert_eq_int(adv, 11, "fb_text two glyphs advance = w + spacing + w");

    /* Unknown codepoint (asset_glyph_codepoint returns 0) is skipped. */
    fb_clear(&fb, false);
    adv = fb_text(&fb, &k_test_font, "AZA", 0, 0, true, -1);
    /* 'Z' -> glyph 0 -> continue; so two 'A's drawn: 5 + 1 + 5 = 11. */
    assert_eq_int(adv, 11, "fb_text skips unknown glyph");

    /* max_width clip: width 5, spacing 1. max_width=5 fits one 'A' (0+0+5<=5);
     * second needs 5+1+5=11>5 -> break. advance = 5. */
    fb_clear(&fb, false);
    adv = fb_text(&fb, &k_test_font, "AA", 0, 0, true, 5);
    assert_eq_int(adv, 5, "fb_text max_width clips at one glyph");
}

static void test_fb_bitmap(void) {
    framebuffer_t fb;

    /* Unknown bitmap (asset_bitmap returns null) -> no-op. */
    s_stub_bitmap = 0;
    s_stub_bitmap_data = 0;
    fb_clear(&fb, false);
    fb_bitmap(&fb, 1, 0, 0, true, true);
    assert_eq_int(count_set(&fb), 0, "fb_bitmap null bitmap no-op");

    /* A real 6x6 solid bitmap drawn on-screen. */
    static const bitmap_t bmp = {.id = 1, .width = 6, .height = 6, .data_offset = 0};
    static uint8_t bmp_data[6 * 1];
    make_solid_sprite(bmp_data, 6, 6);
    s_stub_bitmap = &bmp;
    s_stub_bitmap_data = bmp_data;
    fb_clear(&fb, false);
    fb_bitmap(&fb, 1, 5, 5, true, true);
    assert_eq_int(count_set(&fb), 36, "fb_bitmap 6x6 solid sets 36");
}

int main(void) {
    test_clear();
    test_pixel_packing();
    test_pixel_clipping();
    test_hline();
    test_vline();
    test_rect();
    test_fill_rect();
    test_text5();
    test_blit_slow_path();
    test_blit_fast_vs_reference();
    test_blit_fast_erase();
    test_blit_fast_carry_guard();
    test_fb_text();
    test_fb_bitmap();
    if (s_failures != 0) {
        fprintf(stderr, "%d failures\n", s_failures);
        return 1;
    }
    printf("framebuffer tests passed\n");
    return 0;
}

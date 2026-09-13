#include "ui/framebuffer.h"

#include <string.h>

static const uint8_t *glyph5(char ch);

void fb_clear(framebuffer_t *fb, bool color) {
    memset(fb->data, color ? 0xff : 0x00, FB_SIZE);
}

void fb_pixel(framebuffer_t *fb, int x, int y, bool color) {
    if (x < 0 || y < 0 || x >= (int)FB_WIDTH || y >= (int)FB_HEIGHT) {
        return;
    }
    uint16_t index = (uint16_t)((y >> 3) * (int)FB_WIDTH + x);
    uint8_t mask = (uint8_t)(1u << (y & 7));
    if (color) {
        fb->data[index] |= mask;
    } else {
        fb->data[index] &= (uint8_t)~mask;
    }
}

bool fb_get_pixel(const framebuffer_t *fb, int x, int y) {
    if (x < 0 || y < 0 || x >= (int)FB_WIDTH || y >= (int)FB_HEIGHT) {
        return false;
    }
    uint16_t index = (uint16_t)((y >> 3) * (int)FB_WIDTH + x);
    uint8_t mask = (uint8_t)(1u << (y & 7));
    return (fb->data[index] & mask) != 0;
}

void fb_hline(framebuffer_t *fb, int x, int y, int w, bool color) {
    for (int i = 0; i < w; i++) {
        fb_pixel(fb, x + i, y, color);
    }
}

void fb_vline(framebuffer_t *fb, int x, int y, int h, bool color) {
    for (int i = 0; i < h; i++) {
        fb_pixel(fb, x, y + i, color);
    }
}

void fb_rect(framebuffer_t *fb, int x, int y, int w, int h, bool color) {
    if (w <= 0 || h <= 0) {
        return;
    }
    fb_hline(fb, x, y, w, color);
    fb_hline(fb, x, y + h - 1, w, color);
    fb_vline(fb, x, y, h, color);
    fb_vline(fb, x + w - 1, y, h, color);
}

void fb_fill_rect(framebuffer_t *fb, int x, int y, int w, int h, bool color) {
    if (w <= 0 || h <= 0) { /* match fb_rect; degenerate rects are a no-op */
        return;
    }
    for (int yy = 0; yy < h; yy++) {
        fb_hline(fb, x, y + yy, w, color);
    }
}

int fb_text5(framebuffer_t *fb, const char *text, int x, int y, bool color, int max_width) {
    int cx = x;
    while (*text) {
        if (max_width >= 0 && (cx - x) + 5 > max_width) {
            break;
        }
        const uint8_t *g = glyph5(*text++);
        for (int col = 0; col < 5; col++) {
            uint8_t bits = g[col];
            for (int row = 0; row < 7; row++) {
                if (bits & (1u << row)) {
                    fb_pixel(fb, cx + col, y + row, color);
                }
            }
        }
        cx += 6;
    }
    return cx - x;
}

int fb_text5_width(const char *text) {
    int width = 0;
    while (*text++) {
        width += 6;
    }
    return width > 0 ? width - 1 : 0;
}

void fb_blit_vlsb(framebuffer_t *fb, const uint8_t *data, int x, int y, int w, int h, bool color, bool transparent) {
    if (data == 0 || w <= 0 || h <= 0) {
        return;
    }
    if (transparent && x >= 0 && y >= 0 && x + w <= (int)FB_WIDTH && y + h <= (int)FB_HEIGHT) {
        int shift = y & 7;
        int dst_bank = y >> 3;
        int src_banks = (h + 7) >> 3;
        for (int bank = 0; bank < src_banks; bank++) {
            const uint8_t *src = &data[bank * w];
            uint8_t *dst = &fb->data[(dst_bank + bank) * (int)FB_WIDTH + x];
            for (int col = 0; col < w; col++) {
                uint8_t value = src[col];
                if (!value) {
                    continue;
                }
                if (shift) {
                    uint8_t low = (uint8_t)(value << shift);
                    uint8_t high = (uint8_t)(value >> (8 - shift));
                    if (color) {
                        dst[col] |= low;
                        if (high && dst_bank + bank + 1 < (int)FB_BANKS) {
                            dst[col + FB_WIDTH] |= high;
                        }
                    } else {
                        dst[col] &= (uint8_t)~low;
                        if (high && dst_bank + bank + 1 < (int)FB_BANKS) {
                            dst[col + FB_WIDTH] &= (uint8_t)~high;
                        }
                    }
                } else if (color) {
                    dst[col] |= value;
                } else {
                    dst[col] &= (uint8_t)~value;
                }
            }
        }
        return;
    }

    for (int yy = 0; yy < h; yy++) {
        for (int xx = 0; xx < w; xx++) {
            int src_index = (yy >> 3) * w + xx;
            bool on = (data[src_index] & (1u << (yy & 7))) != 0;
            if (on) {
                fb_pixel(fb, x + xx, y + yy, color);
            } else if (!transparent) {
                fb_pixel(fb, x + xx, y + yy, !color);
            }
        }
    }
}

int fb_text(framebuffer_t *fb, const font_t *font, const char *text, int x, int y, bool color, int max_width) {
    if (font == 0 || text == 0) {
        return 0;
    }
    int cx = x;
    bool first = true;
    while (*text) {
        const glyph_t *glyph = asset_glyph_codepoint(font, asset_next_codepoint(&text));
        if (glyph == 0) {
            continue;
        }
        int spacing = first ? 0 : font->spacing;
        if (max_width >= 0 && (cx - x) + spacing + glyph->width > max_width) {
            break;
        }
        cx += spacing;
        fb_blit_vlsb(fb, asset_glyph_data(font, glyph), cx, y, glyph->width, glyph->height, color, true);
        cx += glyph->width;
        first = false;
    }
    return cx - x;
}

void fb_bitmap(framebuffer_t *fb, uint16_t bitmap_id, int x, int y, bool color, bool transparent) {
    const bitmap_t *bitmap = asset_bitmap(bitmap_id);
    if (bitmap == 0) {
        return;
    }
    fb_blit_vlsb(fb, asset_bitmap_data(bitmap), x, y, bitmap->width, bitmap->height, color, transparent);
}

static const uint8_t *glyph5(char ch) {
    static const uint8_t space[5] = {0x00, 0x00, 0x00, 0x00, 0x00};
    static const uint8_t unknown[5] = {0x02, 0x01, 0x51, 0x09, 0x06};
    static const uint8_t digits[10][5] = {
        {0x3e, 0x51, 0x49, 0x45, 0x3e},
        {0x00, 0x42, 0x7f, 0x40, 0x00},
        {0x42, 0x61, 0x51, 0x49, 0x46},
        {0x21, 0x41, 0x45, 0x4b, 0x31},
        {0x18, 0x14, 0x12, 0x7f, 0x10},
        {0x27, 0x45, 0x45, 0x45, 0x39},
        {0x3c, 0x4a, 0x49, 0x49, 0x30},
        {0x01, 0x71, 0x09, 0x05, 0x03},
        {0x36, 0x49, 0x49, 0x49, 0x36},
        {0x06, 0x49, 0x49, 0x29, 0x1e},
    };
    static const uint8_t letters[26][5] = {
        {0x7e, 0x11, 0x11, 0x11, 0x7e},
        {0x7f, 0x49, 0x49, 0x49, 0x36},
        {0x3e, 0x41, 0x41, 0x41, 0x22},
        {0x7f, 0x41, 0x41, 0x22, 0x1c},
        {0x7f, 0x49, 0x49, 0x49, 0x41},
        {0x7f, 0x09, 0x09, 0x09, 0x01},
        {0x3e, 0x41, 0x49, 0x49, 0x7a},
        {0x7f, 0x08, 0x08, 0x08, 0x7f},
        {0x00, 0x41, 0x7f, 0x41, 0x00},
        {0x20, 0x40, 0x41, 0x3f, 0x01},
        {0x7f, 0x08, 0x14, 0x22, 0x41},
        {0x7f, 0x40, 0x40, 0x40, 0x40},
        {0x7f, 0x02, 0x0c, 0x02, 0x7f},
        {0x7f, 0x04, 0x08, 0x10, 0x7f},
        {0x3e, 0x41, 0x41, 0x41, 0x3e},
        {0x7f, 0x09, 0x09, 0x09, 0x06},
        {0x3e, 0x41, 0x51, 0x21, 0x5e},
        {0x7f, 0x09, 0x19, 0x29, 0x46},
        {0x46, 0x49, 0x49, 0x49, 0x31},
        {0x01, 0x01, 0x7f, 0x01, 0x01},
        {0x3f, 0x40, 0x40, 0x40, 0x3f},
        {0x1f, 0x20, 0x40, 0x20, 0x1f},
        {0x3f, 0x40, 0x38, 0x40, 0x3f},
        {0x63, 0x14, 0x08, 0x14, 0x63},
        {0x07, 0x08, 0x70, 0x08, 0x07},
        {0x61, 0x51, 0x49, 0x45, 0x43},
    };
    static const uint8_t colon[5] = {0x00, 0x36, 0x36, 0x00, 0x00};
    static const uint8_t dash[5] = {0x08, 0x08, 0x08, 0x08, 0x08};
    static const uint8_t star[5] = {0x14, 0x08, 0x3e, 0x08, 0x14};
    static const uint8_t plus[5] = {0x08, 0x08, 0x3e, 0x08, 0x08};

    if (ch == ' ') {
        return space;
    }
    if (ch >= '0' && ch <= '9') {
        return digits[ch - '0'];
    }
    if (ch >= 'a' && ch <= 'z') {
        ch = (char)(ch - ('a' - 'A'));
    }
    if (ch >= 'A' && ch <= 'Z') {
        return letters[ch - 'A'];
    }
    if (ch == ':') {
        return colon;
    }
    if (ch == '-') {
        return dash;
    }
    if (ch == '*') {
        return star;
    }
    if (ch == '+') {
        return plus;
    }
    return unknown;
}

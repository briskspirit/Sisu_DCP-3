#ifndef FRAMEBUFFER_H
#define FRAMEBUFFER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ui/assets.h"

#define FB_WIDTH 84u
#define FB_HEIGHT 48u
#define FB_BANKS (FB_HEIGHT / 8u)
#define FB_SIZE (FB_WIDTH * FB_BANKS)

typedef struct {
    uint8_t data[FB_SIZE];
} framebuffer_t;

void fb_clear(framebuffer_t *fb, bool color);
void fb_pixel(framebuffer_t *fb, int x, int y, bool color);
bool fb_get_pixel(const framebuffer_t *fb, int x, int y);
void fb_hline(framebuffer_t *fb, int x, int y, int w, bool color);
void fb_vline(framebuffer_t *fb, int x, int y, int h, bool color);
void fb_rect(framebuffer_t *fb, int x, int y, int w, int h, bool color);
void fb_fill_rect(framebuffer_t *fb, int x, int y, int w, int h, bool color);
int fb_text5(framebuffer_t *fb, const char *text, int x, int y, bool color, int max_width);
int fb_text5_width(const char *text);
void fb_blit_vlsb(framebuffer_t *fb, const uint8_t *data, int x, int y, int w, int h, bool color, bool transparent);
int fb_text(framebuffer_t *fb, const font_t *font, const char *text, int x, int y, bool color, int max_width);
void fb_bitmap(framebuffer_t *fb, uint16_t bitmap_id, int x, int y, bool color, bool transparent);

#endif

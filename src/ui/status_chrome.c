#include "ui/status_chrome.h"

#include <stdbool.h>

#define STATUS_ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))

typedef struct {
    int8_t x;
    int8_t y;
    uint8_t width;
    uint8_t height;
    const uint8_t *data;
} raw_bitmap_t;

static const uint8_t STATUS_LEFT_ICON_DATA[] = {0x03u, 0x05u, 0x3fu, 0x05u, 0x03u};
static const uint8_t STATUS_RIGHT_ICON_DATA[] = {0x3eu, 0x23u, 0x23u, 0x3eu};
static const uint8_t STATUS_BAR_4X7_DATA[] = {0xffu, 0xffu, 0xffu, 0xffu};
static const uint8_t STATUS_BAR_3X7_DATA[] = {0xffu, 0xffu, 0xffu};
static const uint8_t STATUS_BAR_2X7_DATA[] = {0xffu, 0xffu};
static const uint8_t STATUS_BAR_2X6_DATA[] = {0x3fu, 0x3fu};

static const raw_bitmap_t STATUS_SIGNAL_FILLED[] = {
    {0, 24, 2, 6, STATUS_BAR_2X6_DATA},
    {0, 16, 2, 7, STATUS_BAR_2X7_DATA},
    {0, 8, 3, 7, STATUS_BAR_3X7_DATA},
    {0, 0, 4, 7, STATUS_BAR_4X7_DATA},
};

static const raw_bitmap_t STATUS_BATTERY_FILLED[] = {
    {82, 24, 2, 6, STATUS_BAR_2X6_DATA},
    {82, 16, 2, 7, STATUS_BAR_2X7_DATA},
    {81, 8, 3, 7, STATUS_BAR_3X7_DATA},
    {80, 0, 4, 7, STATUS_BAR_4X7_DATA},
};

static const raw_bitmap_t STATUS_LEFT_ICON = {0, 31, 5, 6, STATUS_LEFT_ICON_DATA};
static const raw_bitmap_t STATUS_RIGHT_ICON = {80, 31, 4, 6, STATUS_RIGHT_ICON_DATA};

static void draw_raw_bitmap(framebuffer_t *fb, const raw_bitmap_t *bitmap);

/* Battery level shown in the right-side bars; the battery poll updates it
 * (real level while discharging, the 512 ms wrap animation while charging). */
static uint8_t s_battery_level = 4u;

void status_chrome_set_battery(uint8_t level) {
    s_battery_level = level;
}

void draw_status(framebuffer_t *fb, uint8_t bars) {
    draw_raw_bitmap(fb, &STATUS_LEFT_ICON);
    if (bars > STATUS_ARRAY_COUNT(STATUS_SIGNAL_FILLED)) {
        bars = (uint8_t)STATUS_ARRAY_COUNT(STATUS_SIGNAL_FILLED);
    }
    for (uint8_t i = 0; i < bars; i++) {
        draw_raw_bitmap(fb, &STATUS_SIGNAL_FILLED[i]);
    }

    draw_battery(fb, s_battery_level);
}

void draw_battery(framebuffer_t *fb, uint8_t level) {
    draw_raw_bitmap(fb, &STATUS_RIGHT_ICON);
    if (level > STATUS_ARRAY_COUNT(STATUS_BATTERY_FILLED)) {
        level = (uint8_t)STATUS_ARRAY_COUNT(STATUS_BATTERY_FILLED);
    }
    for (uint8_t i = 0; i < level; i++) {
        draw_raw_bitmap(fb, &STATUS_BATTERY_FILLED[i]);
    }
}

void draw_alarm_indicator(framebuffer_t *fb, int right_edge) {
    const bitmap_t *alarm = asset_bitmap(31u);
    int alarm_w = alarm != 0 ? alarm->width : 7;
    int x = right_edge - alarm_w;
    if (x < 6) {
        x = 6;
    }
    fb_bitmap(fb, 31u, x, 0, true, true);
}

uint8_t status_bars_from_rssi(uint8_t rssi) {
    if (rssi == 99u) {
        return 0;
    }
    if (rssi >= 22u) {
        return 4;
    }
    if (rssi >= 16u) {
        return 3;
    }
    if (rssi >= 10u) {
        return 2;
    }
    return rssi >= 2u ? 1u : 0u;
}

static void draw_raw_bitmap(framebuffer_t *fb, const raw_bitmap_t *bitmap) {
    fb_blit_vlsb(fb, bitmap->data, bitmap->x, bitmap->y, bitmap->width, bitmap->height, true, false);
}

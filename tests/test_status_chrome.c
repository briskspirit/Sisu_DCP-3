#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ui/assets.h"
#include "ui/framebuffer.h"
#include "ui/status_chrome.h"

static int s_failures;

static void assert_true(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        s_failures++;
    }
}

static unsigned count_region(const framebuffer_t *fb, int x, int y, int w, int h) {
    unsigned count = 0u;
    for (int py = y; py < y + h; py++) {
        for (int px = x; px < x + w; px++) {
            if (fb_get_pixel(fb, px, py)) {
                count++;
            }
        }
    }
    return count;
}

static void test_zero_bar_status_keeps_shells_only(void) {
    framebuffer_t fb;
    fb_clear(&fb, false);
    status_chrome_set_battery(4u);

    draw_status(&fb, 0u);

    assert_true(count_region(&fb, 0, 0, 5, 30) == 0u,
                "zero signal bars leave the left bar stack empty");
    assert_true(count_region(&fb, 0, 31, 5, 6) > 0u,
                "zero signal bars retain the aerial shell");
    assert_true(count_region(&fb, 80, 0, 4, 37) > 0u,
                "status still draws the battery bars and shell");
}

static void test_alarm_indicator_matches_hidden_clock_position(void) {
    framebuffer_t got;
    framebuffer_t expected;
    fb_clear(&got, false);
    fb_clear(&expected, false);

    const bitmap_t *alarm = asset_bitmap(31u);
    assert_true(alarm != NULL && alarm->width == 7u && alarm->height == 7u,
                "v6.00 alarm status asset is the expected 7x7 glyph");
    draw_alarm_indicator(&got, 78);
    fb_bitmap(&expected, 31u, 71, 0, true, true);
    assert_true(memcmp(got.data, expected.data, sizeof(got.data)) == 0,
                "hidden-clock alarm indicator is placed at x=71, y=0");

    fb_clear(&got, false);
    fb_clear(&expected, false);
    draw_alarm_indicator(&got, 8);
    fb_bitmap(&expected, 31u, 6, 0, true, true);
    assert_true(memcmp(got.data, expected.data, sizeof(got.data)) == 0,
                "alarm indicator clamps to the status content edge");
}

static void test_message_waiting_assets_match_v600_slots(void) {
    const bitmap_t *fax = asset_bitmap(1u);
    const bitmap_t *email = asset_bitmap(2u);
    const bitmap_t *voice = asset_bitmap(26u);
    assert_true(fax != NULL && fax->width == 16u && fax->height == 7u,
                "v6.00 fax MWI asset is bitmap 0001 (16x7)");
    assert_true(email != NULL && email->width == 10u && email->height == 7u,
                "v6.00 e-mail MWI asset is bitmap 0002 (10x7)");
    assert_true(voice != NULL && voice->width == 18u && voice->height == 7u,
                "v6.00 generic voice MWI asset remains bitmap 0026 (18x7)");
}

int main(void) {
    test_zero_bar_status_keeps_shells_only();
    test_alarm_indicator_matches_hidden_clock_position();
    test_message_waiting_assets_match_v600_slots();

    if (s_failures == 0) {
        puts("test_status_chrome: all passed");
        return 0;
    }
    fprintf(stderr, "test_status_chrome: %d failure(s)\n", s_failures);
    return 1;
}

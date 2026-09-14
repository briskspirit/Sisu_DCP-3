#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "apps/game_common.h"
#include "apps/profiles_app.h"
#include "audio/audio_levels.h"
#include "services/core1_services.h"
#include "ui/assets.h"
#include "ui/ui.h"

typedef struct {
    uint16_t id;
    int x;
    int y;
    bool transparent;
} bitmap_call_t;

static int s_failures;
static uint8_t s_warning_level = 4u;
static unsigned s_post_count;
static core1_cmd_t s_last_command;
static uint16_t s_last_arg;
static unsigned s_clear_count;
static unsigned s_text_count;
static int s_text_x;
static int s_text_y;
static int s_text_width;
static char s_text[16];
static bitmap_call_t s_bitmap_calls[24];
static unsigned s_bitmap_count;
static unsigned s_rect_count;
static unsigned s_fill_count;
static unsigned s_softkey_count;
static char s_softkey[8];
static bool s_assets_available = true;
static font_t s_font;
static bitmap_t s_bitmap;

static void check(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        s_failures++;
    }
}

uint8_t profile_active_index(void) {
    return 2u;
}

uint8_t profile_get_tone_setting(uint8_t profile_index, profile_setting_kind_t kind) {
    check(profile_index == 2u, "game feedback reads the active profile");
    check(kind == PROFILE_SETTING_WARNING_GAME_TONES,
          "game feedback reads Warning and game tones");
    return s_warning_level;
}

void core1_post_command(core1_cmd_t cmd, uint16_t arg) {
    s_post_count++;
    s_last_command = cmd;
    s_last_arg = arg;
}

const char *ts_or(uint16_t sid, const char *fallback) {
    check(sid == 0x159u, "level selector requests the stock Level string");
    return fallback;
}

const font_t *asset_font(font_id_t font_id) {
    check(font_id == FONT_FS2, "level selector uses stock FS2 title text");
    return &s_font;
}

const bitmap_t *asset_bitmap(uint16_t bitmap_id) {
    if (!s_assets_available ||
        !((bitmap_id >= 241u && bitmap_id <= 250u) ||
          (bitmap_id >= 253u && bitmap_id <= 262u))) {
        return 0;
    }
    uint16_t first = bitmap_id >= 253u ? 253u : 241u;
    s_bitmap.id = bitmap_id;
    s_bitmap.width = 6u;
    s_bitmap.height = (uint8_t)(8u + 2u * (bitmap_id - first));
    return &s_bitmap;
}

void fb_clear(framebuffer_t *fb, bool color) {
    (void)fb;
    check(!color, "level selector clears to white");
    s_clear_count++;
}

int fb_text(framebuffer_t *fb, const font_t *font, const char *text,
            int x, int y, bool color, int max_width) {
    (void)fb;
    check(font == &s_font && color, "level title uses the selected black font");
    s_text_count++;
    s_text_x = x;
    s_text_y = y;
    s_text_width = max_width;
    snprintf(s_text, sizeof(s_text), "%s", text);
    return 0;
}

void fb_bitmap(framebuffer_t *fb, uint16_t bitmap_id, int x, int y,
               bool color, bool transparent) {
    (void)fb;
    check(color, "level bars draw in black");
    if (s_bitmap_count < sizeof(s_bitmap_calls) / sizeof(s_bitmap_calls[0])) {
        s_bitmap_calls[s_bitmap_count] = (bitmap_call_t){bitmap_id, x, y, transparent};
    }
    s_bitmap_count++;
}

void fb_rect(framebuffer_t *fb, int x, int y, int w, int h, bool color) {
    (void)fb;
    (void)x;
    (void)y;
    check(w == 6 && h >= 8 && color, "fallback empty bar keeps stock dimensions");
    s_rect_count++;
}

void fb_fill_rect(framebuffer_t *fb, int x, int y, int w, int h, bool color) {
    (void)fb;
    (void)x;
    (void)y;
    check(w == 6 && h >= 8 && color, "fallback filled bar keeps stock dimensions");
    s_fill_count++;
}

void draw_softkey(framebuffer_t *fb, const char *label) {
    (void)fb;
    s_softkey_count++;
    snprintf(s_softkey, sizeof(s_softkey), "%s", label);
}

static void reset_render_spy(void) {
    s_clear_count = 0u;
    s_text_count = 0u;
    s_bitmap_count = 0u;
    s_rect_count = 0u;
    s_fill_count = 0u;
    s_softkey_count = 0u;
    memset(s_bitmap_calls, 0, sizeof(s_bitmap_calls));
    s_text[0] = '\0';
    s_softkey[0] = '\0';
}

static void test_stock_level_ladder(void) {
    framebuffer_t fb = {0};
    reset_render_spy();
    s_assets_available = true;

    draw_game_level_selector(&fb, 3u, 9u);

    check(s_clear_count == 1u, "level selector clears once");
    check(s_text_count == 1u && strcmp(s_text, "Level:") == 0,
          "level selector draws only the Level title as text");
    check(s_text_x == 5 && s_text_y == 0 && s_text_width == 79,
          "Level title uses the v6.00 window-22 geometry");
    check(s_bitmap_count == 13u, "nine empty bars plus four filled bars are drawn");
    for (unsigned i = 0u; i < 9u; i++) {
        bitmap_call_t call = s_bitmap_calls[i];
        check(call.id == 253u + i, "maximum ladder uses the empty bitmap family");
        check(call.x == 5 + (int)(8u * i), "ladder bars use the stock two-pixel gap");
        check(call.y == 35 - (int)(8u + 2u * i), "ladder bars bottom-align at y=35");
        check(!call.transparent, "empty ladder bars use opaque stock drawing");
    }
    for (unsigned i = 0u; i < 4u; i++) {
        bitmap_call_t call = s_bitmap_calls[9u + i];
        check(call.id == 241u + i, "current level overlays the filled bitmap family");
        check(call.x == 5 + (int)(8u * i), "filled bars retain stock x geometry");
        check(call.y == 35 - (int)(8u + 2u * i), "filled bars retain stock alignment");
        check(!call.transparent, "filled ladder bars use opaque stock drawing");
    }
    check(s_softkey_count == 1u && strcmp(s_softkey, "OK") == 0,
          "level selector keeps the OK softkey");
}

static void test_public_asset_fallback(void) {
    framebuffer_t fb = {0};
    reset_render_spy();
    s_assets_available = false;

    draw_game_level_selector(&fb, 1u, 5u);

    check(s_bitmap_count == 0u, "missing proprietary ladder assets are not blitted");
    check(s_rect_count == 5u, "fallback draws every empty level bar");
    check(s_fill_count == 2u, "fallback fills the selected level bars");
}

static void expect_feedback(uint8_t initial, uint8_t count, int8_t direction,
                            uint8_t expected, core1_audio_game_level_tone_t feedback,
                            const char *message) {
    uint8_t level = initial;
    s_post_count = 0u;
    adjust_game_level(&level, count, direction);
    check(level == expected, message);
    check(s_post_count == 1u && s_last_command == CORE1_CMD_AUDIO_GAME_LEVEL_TONE,
          "level adjustment posts the semantic level-feedback command");
    check(audio_arg_code(s_last_arg) == (uint8_t)feedback,
          "level adjustment preserves the traced feedback mode");
    check(audio_arg_level(s_last_arg) == s_warning_level,
          "level feedback uses the active warning-tone level");
}

static void test_level_adjustment_feedback(void) {
    s_warning_level = 4u;
    expect_feedback(2u, 5u, 1, 3u, CORE1_AUDIO_GAME_LEVEL_STEP,
                    "up advances one level");
    expect_feedback(3u, 5u, -1, 2u, CORE1_AUDIO_GAME_LEVEL_STEP,
                    "down retreats one level");
    expect_feedback(4u, 5u, 1, 4u, CORE1_AUDIO_GAME_LEVEL_UPPER_LIMIT,
                    "up at maximum remains clamped");
    expect_feedback(0u, 5u, -1, 0u, CORE1_AUDIO_GAME_LEVEL_LOWER_LIMIT,
                    "down at minimum remains clamped");

    uint8_t level = 2u;
    s_warning_level = 255u;
    s_post_count = 0u;
    adjust_game_level(&level, 5u, 1);
    check(level == 3u, "disabled game tones do not disable level movement");
    check(s_post_count == 0u, "disabled game tones suppress level feedback");
}

int main(void) {
    test_stock_level_ladder();
    test_public_asset_fallback();
    test_level_adjustment_feedback();

    if (s_failures != 0) {
        fprintf(stderr, "%d game-common test(s) failed\n", s_failures);
        return 1;
    }
    puts("game-common tests passed");
    return 0;
}

#include "apps/game_common.h"

#include "apps/profiles_app.h"
#include "audio/audio_levels.h"
#include "services/core1_services.h"
#include "services/strings.h"
#include "ui/assets.h"
#include "ui/ui.h"

#define GAME_LEVEL_FILLED_BITMAP_FIRST 0x00f1u
#define GAME_LEVEL_EMPTY_BITMAP_FIRST 0x00fdu
#define GAME_LEVEL_BITMAP_COUNT 10u
#define GAME_LEVEL_LEFT 5
#define GAME_LEVEL_BOTTOM 35
#define GAME_LEVEL_STEP_X 8

uint8_t game_audio_level(void) {
    uint8_t warning = profile_get_tone_setting(profile_active_index(), PROFILE_SETTING_WARNING_GAME_TONES);
    if (warning == 255u) {
        return AUDIO_LEVEL_SILENT;
    }
    return warning > AUDIO_LEVEL_MAX ? AUDIO_LEVEL_MAX : warning;
}

void play_game_system_tone(uint8_t index) {
    uint8_t level = game_audio_level();
    if (level != AUDIO_LEVEL_SILENT) {
        core1_post_command(CORE1_CMD_AUDIO_SYSTEM_TONE, audio_arg(index, level));
    }
}

static void play_game_level_feedback(core1_audio_game_level_tone_t feedback) {
    uint8_t level = game_audio_level();
    if (level != AUDIO_LEVEL_SILENT) {
        core1_post_command(CORE1_CMD_AUDIO_GAME_LEVEL_TONE,
                           audio_arg((uint8_t)feedback, level));
    }
}

static void draw_game_level_bar(framebuffer_t *fb, uint8_t index, bool filled) {
    uint16_t bitmap_id = (uint16_t)((filled ? GAME_LEVEL_FILLED_BITMAP_FIRST
                                            : GAME_LEVEL_EMPTY_BITMAP_FIRST) +
                                    index);
    const bitmap_t *bitmap = asset_bitmap(bitmap_id);
    int x = GAME_LEVEL_LEFT + GAME_LEVEL_STEP_X * index;
    int height = 8 + 2 * index;
    if (bitmap != 0) {
        fb_bitmap(fb, bitmap_id, x, GAME_LEVEL_BOTTOM - bitmap->height, true, false);
    } else if (filled) {
        fb_fill_rect(fb, x, GAME_LEVEL_BOTTOM - height, 6, height, true);
    } else {
        fb_rect(fb, x, GAME_LEVEL_BOTTOM - height, 6, height, true);
    }
}

void draw_game_level_selector(framebuffer_t *fb, uint8_t draft_level, uint8_t level_count) {
    if (level_count > GAME_LEVEL_BITMAP_COUNT) {
        level_count = GAME_LEVEL_BITMAP_COUNT;
    }
    uint8_t current = draft_level >= level_count ? level_count
                                                  : (uint8_t)(draft_level + 1u);

    fb_clear(fb, false);
    fb_text(fb, asset_font(FONT_FS2), ts_or(0x159u, "Level:"),
            GAME_LEVEL_LEFT, 0, true, FB_WIDTH - GAME_LEVEL_LEFT);
    for (uint8_t i = 0u; i < level_count; i++) {
        draw_game_level_bar(fb, i, false);
    }
    for (uint8_t i = 0u; i < current; i++) {
        draw_game_level_bar(fb, i, true);
    }
    draw_softkey(fb, "OK");
}

void adjust_game_level(uint8_t *draft_level, uint8_t level_count, int8_t direction) {
    if (draft_level == 0 || level_count == 0u || direction == 0) {
        return;
    }
    uint8_t maximum = (uint8_t)(level_count - 1u);
    if (*draft_level > maximum) {
        *draft_level = maximum;
    }

    core1_audio_game_level_tone_t feedback = CORE1_AUDIO_GAME_LEVEL_STEP;
    if (direction > 0) {
        if (*draft_level < maximum) {
            (*draft_level)++;
        } else {
            feedback = CORE1_AUDIO_GAME_LEVEL_UPPER_LIMIT;
        }
    } else if (*draft_level > 0u) {
        (*draft_level)--;
    } else {
        feedback = CORE1_AUDIO_GAME_LEVEL_LOWER_LIMIT;
    }
    play_game_level_feedback(feedback);
}

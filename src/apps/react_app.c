#include "apps/react_app.h"
#include "apps/game_common.h"

#include <stdio.h>
#include <string.h>

#include "apps/games_app.h"
#include "services/input_keys.h"
#include "storage/store_service.h"
#include "services/timebase.h"
#include "services/strings.h"

#define REACT_MAX_LEVELS 5u
#define REACT_TIMER_MS 120u
#define REACT_PHASE_TICKS 4u
#define REACT_MAX_ATTEMPTS 50u
#define REACT_TIMEOUT_TICKS 0x012cu
#define REACT_RESULT_DISMISS_MS 3850u
#define REACT_TOP_SCORE_FRAME_MS 160u
#define REACT_RNG_FALLBACK_SEED 1u
/* Traced playfield bitmaps (flash 0x002d9764 / 0x002d9a10 / 0x002d9a20),
 * registered as synthetic asset ids by generate_pico_c_assets.py; id 301 is
 * the 10x10 hit marker 0x002d99fc (REACT_HIT_SPRITE matches it byte-for-byte). */
#define REACT_BOARD_BITMAP_ID 300u
#define REACT_LIFE_BITMAP_ID 302u
#define REACT_TRY_BITMAP_ID 303u

typedef enum {
    REACT_MENU_NEW = 0,
    REACT_MENU_ACTIVE,
    REACT_MENU_CONTINUE,
    REACT_MENU_LAST_VIEW,
} react_menu_variant_t;

typedef enum {
    REACT_ITEM_LEVEL = 0,
    REACT_ITEM_CONTINUE,
    REACT_ITEM_LAST_VIEW,
    REACT_ITEM_NEW_GAME,
    REACT_ITEM_TOP_SCORE,
    REACT_ITEM_INSTRUCTIONS,
} react_item_kind_t;

typedef struct {
    react_item_kind_t kind;
    const char *label;
} react_menu_item_t;

typedef struct {
    int8_t score;
    uint8_t data[20];
} react_sprite_t;

static const int8_t REACT_SLOT_X[6] = {28, 46, 64, 28, 46, 64};
static const int8_t REACT_SLOT_Y[6] = {8, 8, 8, 27, 27, 27};
static const react_sprite_t REACT_SPRITES[8] = {
    [1u] = {.score = -25, .data = {0x48u, 0xfau, 0x04u, 0x52u, 0x0bu, 0x42u, 0x2au, 0x05u, 0xf8u, 0x24u, 0x00u, 0x02u, 0x03u, 0x03u, 0x03u, 0x03u, 0x03u, 0x03u, 0x02u, 0x00u}},
    [2u] = {.score = -25, .data = {0x48u, 0xfau, 0xfcu, 0xaeu, 0xf7u, 0xbeu, 0xd6u, 0xfdu, 0xf8u, 0x24u, 0x00u, 0x02u, 0x03u, 0x03u, 0x03u, 0x03u, 0x03u, 0x03u, 0x02u, 0x00u}},
    [3u] = {.score = 10, .data = {0x00u, 0x80u, 0x40u, 0x20u, 0x90u, 0x48u, 0x44u, 0x24u, 0x1fu, 0x07u, 0x02u, 0x03u, 0x01u, 0x01u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u}},
    [4u] = {.score = 5, .data = {0x00u, 0x80u, 0xc0u, 0xe0u, 0xf0u, 0x78u, 0x74u, 0x3cu, 0x1fu, 0x07u, 0x02u, 0x03u, 0x01u, 0x01u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u}},
    [5u] = {.score = 15, .data = {0xf0u, 0x08u, 0x04u, 0x04u, 0x08u, 0x0eu, 0x65u, 0x04u, 0x08u, 0xf0u, 0x00u, 0x01u, 0x02u, 0x02u, 0x02u, 0x02u, 0x02u, 0x02u, 0x01u, 0x00u}},
    [6u] = {.score = 20, .data = {0xf0u, 0xf8u, 0xfcu, 0xfcu, 0xf8u, 0xfeu, 0x9du, 0xfcu, 0xf8u, 0xf0u, 0x00u, 0x01u, 0x03u, 0x03u, 0x03u, 0x03u, 0x03u, 0x03u, 0x01u, 0x00u}},
    [7u] = {.score = 10, .data = {0x78u, 0xfcu, 0xfeu, 0xfeu, 0xfau, 0xfau, 0xfeu, 0xfcu, 0x78u, 0x00u, 0x00u, 0x01u, 0x01u, 0x01u, 0x01u, 0x01u, 0x01u, 0x00u, 0x00u}},
};

static const uint8_t REACT_HIT_SPRITE[20] = {
    0x00u, 0x20u, 0x84u, 0x30u, 0x4au, 0x48u, 0x30u, 0x84u, 0x10u, 0x00u,
    0x00u, 0x00u, 0x00u, 0x00u, 0x01u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
};

static const uint16_t REACT_TOP_SCORE_FRAMES[] = {
    220u, 221u, 222u, 223u, 224u, 225u, 226u, 227u, 228u, 229u,
    229u, 230u, 230u, 229u, 229u, 230u, 230u, 229u, 230u, 230u,
};

static const char REACT_INSTRUCTIONS[] =
    "Try to hit the pictures by using keys 1-6. You have six tries which you can repeat by pressing key 9. Your score depends on the picture you hit. You lose points by hitting the cactuses.";

static void open_react_menu_variant(app_t *app, react_menu_variant_t variant, react_item_kind_t selected_kind);
static void open_react_level(app_t *app);
static void open_react_top_score(app_t *app, uint32_t now);
static void open_react_instructions(app_t *app);
static void start_react_game(app_t *app, uint32_t now);
static void pause_react_to_menu(app_t *app);
static void finish_react_game(app_t *app, uint32_t now);
static void react_step(app_t *app, uint32_t now);
static void react_start_try_set(app_t *app, bool repeat);
static void react_spawn_empty_slots(app_t *app);
static void react_process_slots(app_t *app);
static void react_clear_slot(app_t *app, uint8_t index);
static void react_attempt_key(app_t *app, uint8_t key, uint32_t now);
static void react_repeat_tries(app_t *app);
static uint16_t react_random(app_t *app);
static uint32_t react_seed(uint32_t now);
static uint8_t react_slot_duration(const app_t *app);
static void react_save_storage(const app_t *app);
static uint8_t react_menu_items(const app_t *app, react_menu_item_t *items, uint8_t cap);
static int8_t react_menu_index_for_kind(const app_t *app, react_item_kind_t kind);
static uint8_t react_instruction_lines(char lines[][32], uint8_t max_lines);
static uint8_t react_instruction_max_scroll(void);
static void draw_react_board(const app_t *app, framebuffer_t *fb);
static void draw_react_sprite(framebuffer_t *fb, const uint8_t *data, int x, int y);
static void draw_react_markers(const app_t *app, framebuffer_t *fb);
static void draw_react_message_text(framebuffer_t *fb, const char *a, const char *b, const char *c);

void react_app_init(app_t *app) {
    uint8_t level = 0u;
    uint16_t top_score = 0u;
    store_setting_get_u8(STORE_SETTING_GAMES_REACT_LEVEL, &level);
    store_setting_get_u16(STORE_SETTING_GAMES_REACT_TOP_SCORE, &top_score);
    if (level >= REACT_MAX_LEVELS) {
        level = 0u;
    }
    if (top_score > 9999u) {
        top_score = 9999u;
    }
    app->react_menu_variant = REACT_MENU_NEW;
    app->react_menu_selected = 0u;
    app->react_level_byte = level;
    app->react_level_draft_byte = level;
    app->react_top_score = top_score;
}

void open_react_menu(app_t *app) {
    open_react_menu_variant(app, (react_menu_variant_t)app->react_menu_variant, REACT_ITEM_NEW_GAME);
}

bool handle_react_menu_key(app_t *app, uint16_t key, uint32_t now) {
    react_menu_item_t items[6];
    uint8_t count = react_menu_items(app, items, (uint8_t)ARRAY_COUNT(items));
    if (count == 0u) {
        return true;
    }
    if (app->react_menu_selected >= count) {
        app->react_menu_selected = 0u;
        app->game_option_view_start = 0u;
    }
    if (key == KEY_C) {
        open_games_menu(app, 3u);
        return true;
    }
    if (key == KEY_UP || key == KEY_2) {
        ui_circular_list_step_3rows(count, -1,
                                    &app->react_menu_selected,
                                    &app->game_option_view_start);
        app->dirty = true;
        return true;
    }
    if (key == KEY_DOWN || key == KEY_8) {
        ui_circular_list_step_3rows(count, 1,
                                    &app->react_menu_selected,
                                    &app->game_option_view_start);
        app->dirty = true;
        return true;
    }
    if (key != KEY_NAVI && key != KEY_5) {
        return true;
    }
    react_item_kind_t kind = items[app->react_menu_selected].kind;
    if (kind == REACT_ITEM_NEW_GAME) {
        start_react_game(app, now);
    } else if (kind == REACT_ITEM_CONTINUE) {
        app->route = APP_ROUTE_REACT_PLAY;
        app->react_game_over = false;
        app->react_next_tick_ms = now + REACT_TIMER_MS;
        app->dirty = true;
    } else if (kind == REACT_ITEM_LAST_VIEW) {
        app->route = APP_ROUTE_REACT_LAST_VIEW;
        app->dirty = true;
    } else if (kind == REACT_ITEM_LEVEL) {
        open_react_level(app);
    } else if (kind == REACT_ITEM_TOP_SCORE) {
        open_react_top_score(app, now);
    } else if (kind == REACT_ITEM_INSTRUCTIONS) {
        open_react_instructions(app);
    }
    return true;
}

bool handle_react_play_key(app_t *app, uint16_t key, uint32_t now) {
    if (app->route == APP_ROUTE_REACT_LAST_VIEW) {
        if (key == KEY_C || key == KEY_NAVI || key == KEY_5) {
            open_react_menu_variant(app, REACT_MENU_LAST_VIEW, REACT_ITEM_LAST_VIEW);
        }
        return true;
    }
    if (key == KEY_C || key == KEY_NAVI) {
        pause_react_to_menu(app);
        return true;
    }
    if (app->react_game_over) {
        return true;
    }
    if (key >= KEY_1 && key <= KEY_6) {
        uint8_t number = 0u;
        if (key == KEY_1) number = 1u;
        else if (key == KEY_2) number = 2u;
        else if (key == KEY_3) number = 3u;
        else if (key == KEY_4) number = 4u;
        else if (key == KEY_5) number = 5u;
        else if (key == KEY_6) number = 6u;
        react_attempt_key(app, number, now);
    } else if (key == KEY_9) {
        react_repeat_tries(app);
    }
    return true;
}

bool handle_react_level_key(app_t *app, uint16_t key, uint32_t now) {
    uint8_t max_level = REACT_MAX_LEVELS - 1u;
    if (key == KEY_UP || key == KEY_2) {
        /* Traced level-screen tones: move 0x03, boundary high 0x08 / low 0x09. */
        if (app->react_level_draft_byte < max_level) {
            app->react_level_draft_byte++;
            play_game_system_tone(3u);
        } else {
            play_game_system_tone(8u);
        }
        app->dirty = true;
        return true;
    }
    if (key == KEY_DOWN || key == KEY_8) {
        if (app->react_level_draft_byte > 0u) {
            app->react_level_draft_byte--;
            play_game_system_tone(3u);
        } else {
            play_game_system_tone(9u);
        }
        app->dirty = true;
        return true;
    }
    if (key == KEY_NAVI || key == KEY_5) {
        app->react_level_byte = app->react_level_draft_byte;
        app->react_menu_variant = REACT_MENU_ACTIVE;
        react_save_storage(app);
        open_react_menu_variant(app, REACT_MENU_ACTIVE, REACT_ITEM_LEVEL);
        return true;
    }
    if (key == KEY_C) {
        app->react_level_draft_byte = app->react_level_byte;
        open_react_menu_variant(app, (react_menu_variant_t)app->react_menu_variant, REACT_ITEM_LEVEL);
        return true;
    }
    (void)now;
    return true;
}

bool handle_react_instructions_key(app_t *app, uint16_t key, uint32_t now) {
    uint8_t max_scroll = react_instruction_max_scroll();
    if ((key == KEY_UP || key == KEY_2) && app->react_instructions_scroll > 0u) {
        app->react_instructions_scroll--;
        app->dirty = true;
    } else if ((key == KEY_DOWN || key == KEY_8) && app->react_instructions_scroll < max_scroll) {
        app->react_instructions_scroll++;
        app->dirty = true;
    } else if (key == KEY_C || key == KEY_NAVI || key == KEY_5) {
        open_react_menu_variant(app, (react_menu_variant_t)app->react_menu_variant, REACT_ITEM_INSTRUCTIONS);
    }
    (void)now;
    return true;
}

bool handle_react_message_key(app_t *app, uint16_t key, uint32_t now) {
    if (key == KEY_C || key == KEY_NAVI || key == KEY_5) {
        react_item_kind_t item = app->route == APP_ROUTE_REACT_TOP_SCORE ? REACT_ITEM_TOP_SCORE : REACT_ITEM_LAST_VIEW;
        react_menu_variant_t variant = app->route == APP_ROUTE_REACT_RESULT ? REACT_MENU_LAST_VIEW :
            (react_menu_variant_t)app->react_menu_variant;
        open_react_menu_variant(app, variant, item);
    }
    (void)now;
    return true;
}

bool tick_react(app_t *app, uint32_t now) {
    bool changed = false;
    if (app->route == APP_ROUTE_REACT_PLAY && !app->react_game_over &&
        app->react_next_tick_ms != 0u && time_diff_ms(now, app->react_next_tick_ms) >= 0) {
        react_step(app, now);
        changed = true;
    }
    if (app->route == APP_ROUTE_REACT_RESULT &&
        app->react_result_deadline_ms != 0u &&
        time_diff_ms(now, app->react_result_deadline_ms) >= 0) {
        open_react_menu_variant(app, REACT_MENU_LAST_VIEW, REACT_ITEM_LAST_VIEW);
        changed = true;
    }
    if (app->route == APP_ROUTE_REACT_TOP_SCORE &&
        time_diff_ms(now, app->react_top_score_last_frame_ms + REACT_TOP_SCORE_FRAME_MS) >= 0) {
        app->react_top_score_last_frame_ms = now;
        if (app->react_top_score_frame + 1u < ARRAY_COUNT(REACT_TOP_SCORE_FRAMES)) {
            app->react_top_score_frame++;
            changed = true;
        }
    }
    return changed;
}

void render_react_menu(const app_t *app, framebuffer_t *fb) {
    react_menu_item_t items[6];
    const char *labels[6];
    uint8_t count = react_menu_items(app, items, (uint8_t)ARRAY_COUNT(items));
    for (uint8_t i = 0; i < count; i++) {
        labels[i] = items[i].label;
    }
    char crumb[10];
    draw_flat_list_circular_view(fb, labels, count,
                                 app->react_menu_selected,
                                 app->game_option_view_start,
                                 ui_breadcrumb_path(crumb, sizeof(crumb), "6-4", (unsigned)(app->react_menu_selected + 1u)),
                                 "Select");
}

void render_react_play(const app_t *app, framebuffer_t *fb) {
    draw_react_board(app, fb);
}

void render_react_level(const app_t *app, framebuffer_t *fb) {
    fb_clear(fb, false);
    const font_t *large = asset_font(FONT_FS0);
    const font_t *small = asset_font(FONT_FS1);
    const font_t *bold = asset_font(FONT_FS2);
    char current[4];
    char max[4];
    snprintf(current, sizeof(current), "%u", (unsigned)(app->react_level_draft_byte + 1u));
    snprintf(max, sizeof(max), "/%u", (unsigned)REACT_MAX_LEVELS);
    int current_w = asset_text_width(large, current);
    int max_w = asset_text_width(small, max);
    int pair_w = current_w + 1 + max_w;
    int x = 6 + (72 - pair_w) / 2;
    if (x < 6) {
        x = 6;
    }
    fb_text(fb, large, current, x, 15, true, current_w);
    fb_text(fb, small, max, x + current_w + 1, 18, true, max_w);
    fb_text(fb, bold, ts_or(0x159u, "Level:"), 3, 28, true, 81);
    draw_softkey(fb, "OK");
}

void render_react_instructions(const app_t *app, framebuffer_t *fb) {
    fb_clear(fb, false);
    char lines[18][32];
    uint8_t count = react_instruction_lines(lines, (uint8_t)ARRAY_COUNT(lines));
    uint8_t start = app->react_instructions_scroll;
    if (start > count) {
        start = 0u;
    }
    const font_t *font = asset_font(FONT_FS1);
    for (uint8_t row = 0; row < 3u && start + row < count; row++) {
        fb_text(fb, font, lines[start + row], 0, 7 + row * 9, true, FB_WIDTH);
    }
    draw_softkey(fb, "Back");
}

void render_react_top_score(const app_t *app, framebuffer_t *fb) {
    fb_clear(fb, false);
    char score[8];
    snprintf(score, sizeof(score), "%u", (unsigned)app->react_top_score);
    draw_react_message_text(fb, "Top score:", score, 0);
    uint8_t frame = app->react_top_score_frame;
    if (frame >= ARRAY_COUNT(REACT_TOP_SCORE_FRAMES)) {
        frame = (uint8_t)(ARRAY_COUNT(REACT_TOP_SCORE_FRAMES) - 1u);
    }
    fb_bitmap(fb, REACT_TOP_SCORE_FRAMES[frame], 63, 0, true, true);
}

void render_react_result(const app_t *app, framebuffer_t *fb) {
    fb_clear(fb, false);
    char score[8];
    snprintf(score, sizeof(score), "%u", (unsigned)app->react_score);
    draw_react_message_text(fb,
                            ts_or(0x157u, "Game over!"),
                            app->react_result_top_score ? "TOP SCORE:" : "Your score:",
                            score);
}

static void open_react_menu_variant(app_t *app, react_menu_variant_t variant, react_item_kind_t selected_kind) {
    app->route = APP_ROUTE_REACT_MENU;
    app->react_menu_variant = (uint8_t)variant;
    int8_t selected = react_menu_index_for_kind(app, selected_kind);
    app->react_menu_selected = selected >= 0 ? (uint8_t)selected : 0u;
    app->game_option_view_start = app->react_menu_selected;
    app->react_next_tick_ms = 0u;
    app->dirty = true;
}

static void open_react_level(app_t *app) {
    app->route = APP_ROUTE_REACT_LEVEL;
    app->react_level_draft_byte = app->react_level_byte;
    app->dirty = true;
}

static void open_react_top_score(app_t *app, uint32_t now) {
    app->route = APP_ROUTE_REACT_TOP_SCORE;
    app->react_top_score_frame = 0u;
    app->react_top_score_last_frame_ms = now;
    app->dirty = true;
}

static void open_react_instructions(app_t *app) {
    app->route = APP_ROUTE_REACT_INSTRUCTIONS;
    app->react_instructions_scroll = 0u;
    app->dirty = true;
}

static void start_react_game(app_t *app, uint32_t now) {
    app->route = APP_ROUTE_REACT_PLAY;
    app->react_menu_variant = REACT_MENU_ACTIVE;
    app->react_rng_seed = react_seed(now);
    app->react_score = 0u;
    app->react_tries_remaining = 6u;
    app->react_attempts_used = 0u;
    app->react_lives_remaining = 3u;
    app->react_tick_counter = 0u;
    app->react_phase_tick = 0u;
    app->react_game_over = false;
    app->react_result_top_score = false;
    app->react_result_deadline_ms = 0u;
    react_start_try_set(app, false);
    app->react_next_tick_ms = now + REACT_TIMER_MS;
    app->dirty = true;
}

static void pause_react_to_menu(app_t *app) {
    open_react_menu_variant(app, REACT_MENU_CONTINUE, REACT_ITEM_CONTINUE);
}

static void finish_react_game(app_t *app, uint32_t now) {
    app->route = APP_ROUTE_REACT_RESULT;
    app->react_game_over = true;
    app->react_next_tick_ms = 0u;
    app->react_result_deadline_ms = now + REACT_RESULT_DISMISS_MS;
    app->react_menu_variant = REACT_MENU_LAST_VIEW;
    app->react_result_top_score = app->react_score > app->react_top_score;
    if (app->react_result_top_score) {
        app->react_top_score = app->react_score;
        react_save_storage(app);
        play_game_system_tone(20u);
    }
    app->dirty = true;
}

static void react_step(app_t *app, uint32_t now) {
    app->react_tick_counter++;
    if (app->react_tick_counter > REACT_TIMEOUT_TICKS) {
        finish_react_game(app, now);
        return;
    }
    app->react_phase_tick++;
    if (app->react_phase_tick >= REACT_PHASE_TICKS) {
        app->react_phase_tick = 0u;
        react_spawn_empty_slots(app);
        react_process_slots(app);
    } else {
        for (uint8_t i = 0u; i < 6u; i++) {
            if (app->react_slot_hit_ticks[i] > 0u) {
                app->react_slot_hit_ticks[i]--;
            }
        }
    }
    app->react_next_tick_ms = now + REACT_TIMER_MS;
    app->dirty = true;
}

static void react_start_try_set(app_t *app, bool repeat) {
    if (!repeat) {
        memset(app->react_slot_flags, 0, sizeof(app->react_slot_flags));
        memset(app->react_slot_delays, 0, sizeof(app->react_slot_delays));
        memset(app->react_slot_timers, 0, sizeof(app->react_slot_timers));
        memset(app->react_slot_hit_ticks, 0, sizeof(app->react_slot_hit_ticks));
        app->react_active_mask = 0u;
    }
    if (repeat) {
        uint8_t remaining = app->react_attempts_used >= REACT_MAX_ATTEMPTS
            ? 0u
            : (uint8_t)(REACT_MAX_ATTEMPTS - app->react_attempts_used);
        app->react_tries_remaining = remaining > 6u ? 6u : remaining;
    }
}

static void react_spawn_empty_slots(app_t *app) {
    for (uint8_t i = 0u; i < 6u; i++) {
        if (app->react_slot_flags[i] != 0u || (app->react_active_mask & (uint8_t)(1u << i)) != 0u) {
            continue;
        }
        uint8_t flag = (uint8_t)(react_random(app) % 8u);
        if (flag == 0u) {
            continue;
        }
        app->react_slot_flags[i] = flag;
        app->react_slot_delays[i] = (uint8_t)((react_random(app) % 3u) + 1u);
        app->react_slot_timers[i] = react_slot_duration(app);
    }
}

static void react_process_slots(app_t *app) {
    for (uint8_t i = 0u; i < 6u; i++) {
        if (app->react_slot_hit_ticks[i] > 0u) {
            app->react_slot_hit_ticks[i]--;
        }
        if (app->react_slot_flags[i] == 0u) {
            continue;
        }
        if (app->react_slot_delays[i] > 0u) {
            app->react_slot_delays[i]--;
            if (app->react_slot_delays[i] == 0u) {
                app->react_active_mask |= (uint8_t)(1u << i);
            }
        }
        if ((app->react_active_mask & (uint8_t)(1u << i)) != 0u) {
            if (app->react_slot_timers[i] > 0u) {
                app->react_slot_timers[i]--;
            }
            if (app->react_slot_timers[i] == 0u) {
                react_clear_slot(app, i);
            }
        }
    }
}

static void react_clear_slot(app_t *app, uint8_t index) {
    app->react_slot_flags[index] = 0u;
    app->react_slot_delays[index] = 0u;
    app->react_slot_timers[index] = 0u;
    app->react_active_mask &= (uint8_t)~(uint8_t)(1u << index);
}

static void react_attempt_key(app_t *app, uint8_t key, uint32_t now) {
    if (key == 0u || key > 6u || app->react_attempts_used >= REACT_MAX_ATTEMPTS ||
        app->react_tries_remaining == 0u) {
        app->dirty = true;
        return;
    }
    uint8_t index = (uint8_t)(key - 1u);
    play_game_system_tone(18u);
    app->react_tries_remaining--;
    app->react_attempts_used++;
    app->react_slot_hit_ticks[index] = 1u;
    if ((app->react_active_mask & (uint8_t)(1u << index)) == 0u || app->react_slot_flags[index] == 0u) {
        app->dirty = true;
        return;
    }

    uint8_t flag = app->react_slot_flags[index];
    play_game_system_tone(16u);
    int16_t score = (int16_t)app->react_score + REACT_SPRITES[flag].score;
    if (score < 0) {
        score = 0;
    } else if (score > 9999) {
        score = 9999;
    }
    app->react_score = (uint16_t)score;
    if (flag == 1u || flag == 2u) {
        if (app->react_lives_remaining > 0u) {
            app->react_lives_remaining--;
        }
    } else if (flag == 5u) {
        app->react_attempts_used = app->react_attempts_used > 6u ? (uint8_t)(app->react_attempts_used - 6u) : 0u;
    } else if (flag == 6u) {
        app->react_tries_remaining = 6u;
    } else if (flag == 7u) {
        app->react_tries_remaining = 0u;
    }
    react_clear_slot(app, index);
    app->react_slot_hit_ticks[index] = 1u;
    if (app->react_lives_remaining == 0u) {
        finish_react_game(app, now);
        return;
    }
    app->dirty = true;
}

static void react_repeat_tries(app_t *app) {
    react_start_try_set(app, true);
    app->dirty = true;
}

static uint16_t react_random(app_t *app) {
    app->react_rng_seed = app->react_rng_seed * 1103515245u + 12345u;
    return (uint16_t)((app->react_rng_seed >> 16) & 0x7fffu);
}

static uint32_t react_seed(uint32_t now) {
    uint32_t seed = now ^ (time_ticks8() * 1103515245u) ^ 0x29afe8u ^ 0x242890u;
    return seed == 0u ? REACT_RNG_FALLBACK_SEED : seed;
}

static uint8_t react_slot_duration(const app_t *app) {
    uint8_t level = app->react_level_byte >= REACT_MAX_LEVELS ? 0u : app->react_level_byte;
    return (uint8_t)(8u - level);
}

static void react_save_storage(const app_t *app) {
    store_setting_set_u8(STORE_SETTING_GAMES_REACT_LEVEL, app->react_level_byte);
    store_setting_set_u16(STORE_SETTING_GAMES_REACT_TOP_SCORE, app->react_top_score);
}

static uint8_t react_menu_items(const app_t *app, react_menu_item_t *items, uint8_t cap) {
    uint8_t count = 0u;
#define ADD_ITEM(k, text) do { \
    if (count < cap) { \
        items[count].kind = (k); \
        items[count].label = (text); \
        count++; \
    } \
} while (0)
    /* Labels are display-only: dispatch keys on .kind (see handle_react_menu_key),
     * so localizing the rendered label is safe. */
    ADD_ITEM(REACT_ITEM_LEVEL, ts_or(0x164u, "Level"));
    if (app->react_menu_variant == REACT_MENU_CONTINUE) {
        ADD_ITEM(REACT_ITEM_CONTINUE, ts_or(0x160u, "Continue"));
    }
    if (app->react_menu_variant == REACT_MENU_LAST_VIEW) {
        ADD_ITEM(REACT_ITEM_LAST_VIEW, ts_or(0x163u, "Last view"));
    }
    ADD_ITEM(REACT_ITEM_NEW_GAME, ts_or(0x165u, "New game"));
    ADD_ITEM(REACT_ITEM_TOP_SCORE, ts_or(0x161u, "Top score"));
    ADD_ITEM(REACT_ITEM_INSTRUCTIONS, ts_or(0x162u, "Instructions"));
#undef ADD_ITEM
    return count;
}

static int8_t react_menu_index_for_kind(const app_t *app, react_item_kind_t kind) {
    react_menu_item_t items[6];
    uint8_t count = react_menu_items(app, items, (uint8_t)ARRAY_COUNT(items));
    for (uint8_t i = 0u; i < count; i++) {
        if (items[i].kind == kind) {
            return (int8_t)i;
        }
    }
    return -1;
}

static uint8_t react_instruction_lines(char lines[][32], uint8_t max_lines) {
    return wrap_text_lines_ex(asset_font(FONT_FS1), ts_or(0x40bu, REACT_INSTRUCTIONS), FB_WIDTH, (char *)lines, 32u, max_lines);
}

static uint8_t react_instruction_max_scroll(void) {
    char lines[18][32];
    uint8_t count = react_instruction_lines(lines, (uint8_t)ARRAY_COUNT(lines));
    return count > 3u ? (uint8_t)(count - 3u) : 0u;
}

static void draw_react_board(const app_t *app, framebuffer_t *fb) {
    fb_clear(fb, false);
    /* Full-screen traced board (rounded border, 14x14 slot frames, desert
     * doodle) replaces the primitive rect border + 12x12 frames. */
    fb_bitmap(fb, REACT_BOARD_BITMAP_ID, 0, 0, true, true);
    draw_react_markers(app, fb);
    for (uint8_t i = 0u; i < 6u; i++) {
        bool active = (app->react_active_mask & (uint8_t)(1u << i)) != 0u;
        if (!active && app->react_slot_hit_ticks[i] == 0u) {
            continue;
        }
        const uint8_t *data = app->react_slot_hit_ticks[i] > 0u
            ? REACT_HIT_SPRITE
            : REACT_SPRITES[app->react_slot_flags[i]].data;
        draw_react_sprite(fb, data, REACT_SLOT_X[i], REACT_SLOT_Y[i]);
    }
}

static void draw_react_sprite(framebuffer_t *fb, const uint8_t *data, int x, int y) {
    for (uint8_t col = 0u; col < 10u; col++) {
        for (uint8_t row = 0u; row < 10u; row++) {
            uint8_t bits = data[(row >> 3) * 10u + col];
            if ((bits & (uint8_t)(1u << (row & 7u))) != 0u) {
                fb_pixel(fb, x + col, y + row, true);
            }
        }
    }
}

static void draw_react_markers(const app_t *app, framebuffer_t *fb) {
    /* Traced notch try markers and circle-dot life icons. */
    for (uint8_t i = 0u; i < app->react_tries_remaining && i < 6u; i++) {
        fb_bitmap(fb, REACT_TRY_BITMAP_ID, 14, 3 + i * 3, true, true);
    }
    for (uint8_t i = 0u; i < app->react_lives_remaining && i < 3u; i++) {
        fb_bitmap(fb, REACT_LIFE_BITMAP_ID, 5, 3 + i * 6, true, true);
    }
}

static void draw_react_message_text(framebuffer_t *fb, const char *a, const char *b, const char *c) {
    const font_t *font = asset_font(FONT_FS0);
    if (a != 0 && a[0] != '\0') {
        fb_text(fb, font, a, 0, 3, true, FB_WIDTH);
    }
    if (b != 0 && b[0] != '\0') {
        fb_text(fb, font, b, 0, 16, true, FB_WIDTH);
    }
    if (c != 0 && c[0] != '\0') {
        fb_text(fb, font, c, 0, 29, true, FB_WIDTH);
    }
}

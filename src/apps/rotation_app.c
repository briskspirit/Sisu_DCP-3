#include "apps/rotation_app.h"
#include "apps/game_common.h"

#include <stdio.h>
#include <string.h>

#include "apps/games_app.h"
#include "services/input_keys.h"
#include "storage/store_service.h"
#include "services/timebase.h"
#include "services/strings.h"

#define ROTATION_MAX_LEVELS 7u
#define ROTATION_MAX_SPAN 6u
#define ROTATION_MAX_CELLS (ROTATION_MAX_SPAN * ROTATION_MAX_SPAN)
#define ROTATION_ANIMATION_STEP_MS 150u
#define ROTATION_IDLE_TICK_MS 640u
#define ROTATION_SOLVE_SETTLE_MS 2570u
#define ROTATION_RESULT_DISMISS_MS 3850u
#define ROTATION_TOP_SCORE_FRAME_MS 160u

typedef enum {
    ROTATION_MENU_NEW = 0,
    ROTATION_MENU_ACTIVE,
    ROTATION_MENU_CONTINUE,
    ROTATION_MENU_LAST_VIEW,
} rotation_menu_variant_t;

typedef enum {
    ROTATION_ITEM_LEVEL = 0,
    ROTATION_ITEM_CONTINUE,
    ROTATION_ITEM_LAST_VIEW,
    ROTATION_ITEM_NEW_GAME,
    ROTATION_ITEM_TOP_SCORE,
    ROTATION_ITEM_INSTRUCTIONS,
} rotation_item_kind_t;

typedef struct {
    rotation_item_kind_t kind;
    const char *label;
} rotation_menu_item_t;

typedef struct {
    uint8_t board_span;
    uint8_t frame_size;
    uint8_t ready_ticks;
    uint8_t cells[ROTATION_MAX_CELLS];
} rotation_level_t;

typedef struct {
    uint8_t row;
    uint8_t col;
} rotation_coord_t;

static const rotation_level_t ROTATION_LEVELS[] = {
    {3u, 2u, 24u, {3u, 6u, 2u, 8u, 9u, 7u, 1u, 4u, 5u}},
    {4u, 2u, 32u, {8u, 3u, 9u, 15u, 16u, 4u, 6u, 10u, 5u, 14u, 13u, 7u, 1u, 12u, 2u, 11u}},
    {5u, 2u, 40u, {23u, 17u, 8u, 9u, 5u, 20u, 16u, 24u, 3u, 10u, 14u, 13u, 1u, 4u, 7u, 21u, 15u, 11u, 6u, 12u, 2u, 25u, 22u, 18u, 19u}},
    {6u, 2u, 48u, {17u, 29u, 35u, 5u, 10u, 16u, 36u, 23u, 34u, 6u, 9u, 13u, 20u, 1u, 25u, 3u, 14u, 30u, 7u, 4u, 2u, 32u, 28u, 22u, 27u, 33u, 21u, 18u, 26u, 8u, 19u, 15u, 31u, 12u, 24u, 11u}},
    {4u, 3u, 24u, {4u, 8u, 15u, 10u, 3u, 9u, 11u, 6u, 14u, 5u, 1u, 2u, 16u, 12u, 7u, 13u}},
    {5u, 3u, 32u, {20u, 8u, 24u, 17u, 10u, 16u, 12u, 1u, 7u, 5u, 13u, 14u, 23u, 18u, 3u, 25u, 6u, 2u, 11u, 9u, 15u, 21u, 22u, 4u, 19u}},
    {6u, 3u, 40u, {1u, 20u, 5u, 25u, 10u, 16u, 32u, 36u, 35u, 28u, 3u, 13u, 17u, 4u, 9u, 23u, 30u, 14u, 33u, 7u, 29u, 6u, 21u, 22u, 27u, 11u, 34u, 2u, 12u, 26u, 19u, 15u, 31u, 18u, 24u, 8u}},
};

static const uint8_t ROTATION_DIGIT_GLYPHS[10][3] = {
    {0x1fu, 0x11u, 0x1fu},
    {0x02u, 0x1fu, 0x00u},
    {0x1du, 0x15u, 0x17u},
    {0x15u, 0x15u, 0x1fu},
    {0x07u, 0x04u, 0x1fu},
    {0x17u, 0x15u, 0x1du},
    {0x1fu, 0x15u, 0x1du},
    {0x01u, 0x1du, 0x03u},
    {0x1fu, 0x15u, 0x1fu},
    {0x17u, 0x15u, 0x1fu},
};

static const uint16_t ROTATION_TOP_SCORE_FRAMES[] = {
    220u, 221u, 222u, 223u, 224u, 225u, 226u, 227u, 228u, 229u,
    229u, 230u, 230u, 229u, 229u, 230u, 230u, 229u, 230u, 230u,
};

static const char ROTATION_INSTRUCTIONS[] =
    "Arrange the numbers in numerical order starting from 1. Rotate the numbers inside the frame with keys 1 and 3. Move the frame to another position with keys 2, 4, 6 and 8.";

static void open_rotation_menu_variant(app_t *app, rotation_menu_variant_t variant, rotation_item_kind_t selected_kind);
static void open_rotation_level(app_t *app);
static void open_rotation_top_score(app_t *app, uint32_t now);
static void open_rotation_instructions(app_t *app);
static void start_rotation_game(app_t *app, uint32_t now);
static void pause_rotation_to_menu(app_t *app);
static void finish_rotation_game(app_t *app, uint32_t now);
static void trigger_rotation_solved(app_t *app, uint32_t now);
static void rotation_animation_tick(app_t *app, uint32_t now);
static void rotation_start_animation(app_t *app, uint8_t phase, uint8_t pending_rotation, uint32_t now);
static void rotation_apply(app_t *app, bool clockwise);
static void rotation_move_cursor(app_t *app, int8_t dx, int8_t dy);
static void rotation_save_storage(const app_t *app);
static uint8_t rotation_menu_items(const app_t *app, rotation_menu_item_t *items, uint8_t cap);
static int8_t rotation_menu_index_for_kind(const app_t *app, rotation_item_kind_t kind);
static const rotation_level_t *rotation_level_record(uint8_t level_byte);
static uint8_t rotation_positions(const app_t *app);
static uint8_t rotation_ring_coords(uint8_t frame_size, rotation_coord_t *coords, uint8_t cap);
static int8_t rotation_selected_ring_index(const app_t *app, uint8_t row, uint8_t col);
static bool rotation_is_solved(const app_t *app);
static uint8_t rotation_instruction_lines(char lines[][32], uint8_t max_lines);
static uint8_t rotation_instruction_max_scroll(void);
static void draw_rotation_board(const app_t *app, framebuffer_t *fb);
static void draw_rotation_digit(framebuffer_t *fb, uint8_t digit, int x, int y);
static void draw_rotation_number(framebuffer_t *fb, uint8_t value, int ones_x, int y);
static void draw_rotation_message_text(framebuffer_t *fb, const char *a, const char *b, const char *c);

void rotation_app_init(app_t *app) {
    uint8_t level = 1u;
    uint16_t top_score = 0u;
    store_setting_get_u8(STORE_SETTING_GAMES_ROTATION_LEVEL, &level);
    store_setting_get_u16(STORE_SETTING_GAMES_ROTATION_TOP_SCORE, &top_score);
    if (level >= ROTATION_MAX_LEVELS) {
        level = 1u;
    }
    if (top_score > 9999u) {
        top_score = 9999u;
    }
    app->rotation_menu_variant = ROTATION_MENU_NEW;
    app->rotation_menu_selected = 0u;
    app->rotation_level_byte = level;
    app->rotation_level_draft_byte = level;
    app->rotation_top_score = top_score;
}

void open_rotation_menu(app_t *app) {
    open_rotation_menu_variant(app, (rotation_menu_variant_t)app->rotation_menu_variant, ROTATION_ITEM_NEW_GAME);
}

bool handle_rotation_menu_key(app_t *app, uint16_t key, uint32_t now) {
    rotation_menu_item_t items[6];
    uint8_t count = rotation_menu_items(app, items, (uint8_t)ARRAY_COUNT(items));
    if (count == 0u) {
        return true;
    }
    if (app->rotation_menu_selected >= count) {
        app->rotation_menu_selected = 0u;
        app->game_option_view_start = 0u;
    }
    if (key == KEY_C) {
        open_games_menu(app, 0u);
        return true;
    }
    if (key == KEY_UP || key == KEY_2) {
        ui_circular_list_step_3rows(count, -1,
                                    &app->rotation_menu_selected,
                                    &app->game_option_view_start);
        app->dirty = true;
        return true;
    }
    if (key == KEY_DOWN || key == KEY_8) {
        ui_circular_list_step_3rows(count, 1,
                                    &app->rotation_menu_selected,
                                    &app->game_option_view_start);
        app->dirty = true;
        return true;
    }
    if (key != KEY_NAVI && key != KEY_5) {
        return true;
    }

    rotation_item_kind_t kind = items[app->rotation_menu_selected].kind;
    if (kind == ROTATION_ITEM_NEW_GAME) {
        start_rotation_game(app, now);
    } else if (kind == ROTATION_ITEM_CONTINUE) {
        app->route = APP_ROUTE_ROTATION_PLAY;
        app->rotation_game_over = false;
        app->rotation_solved_deadline_ms = 0u;
        if (!app->rotation_ready || app->rotation_animation_steps_remaining > 0u) {
            app->rotation_next_animation_ms = now + ROTATION_ANIMATION_STEP_MS;
            app->rotation_next_idle_ms = 0u;
        } else {
            app->rotation_next_animation_ms = 0u;
            app->rotation_next_idle_ms = now + ROTATION_IDLE_TICK_MS;
        }
        app->dirty = true;
    } else if (kind == ROTATION_ITEM_LAST_VIEW) {
        app->route = APP_ROUTE_ROTATION_LAST_VIEW;
        app->dirty = true;
    } else if (kind == ROTATION_ITEM_LEVEL) {
        open_rotation_level(app);
    } else if (kind == ROTATION_ITEM_TOP_SCORE) {
        open_rotation_top_score(app, now);
    } else if (kind == ROTATION_ITEM_INSTRUCTIONS) {
        open_rotation_instructions(app);
    }
    return true;
}

bool handle_rotation_play_key(app_t *app, uint16_t key, uint32_t now) {
    if (app->route == APP_ROUTE_ROTATION_LAST_VIEW) {
        if (key == KEY_C || key == KEY_NAVI || key == KEY_5) {
            open_rotation_menu_variant(app, ROTATION_MENU_LAST_VIEW, ROTATION_ITEM_LAST_VIEW);
        }
        return true;
    }
    if (key == KEY_C || key == KEY_NAVI) {
        pause_rotation_to_menu(app);
        return true;
    }
    if (app->rotation_game_over || !app->rotation_ready || app->rotation_animation_steps_remaining > 0u) {
        return true;
    }

    if (key == KEY_UP || key == KEY_2) {
        rotation_move_cursor(app, 0, -1);
    } else if (key == KEY_DOWN || key == KEY_8) {
        rotation_move_cursor(app, 0, 1);
    } else if (key == KEY_4) {
        rotation_move_cursor(app, -1, 0);
    } else if (key == KEY_6) {
        rotation_move_cursor(app, 1, 0);
    } else if (key == KEY_1 || key == KEY_7) {
        rotation_apply(app, false);
        rotation_start_animation(app, 1u, 0u, now);
    } else if (key == KEY_3 || key == KEY_5 || key == KEY_9) {
        rotation_start_animation(app, 3u, 1u, now);
    }
    return true;
}

bool handle_rotation_level_key(app_t *app, uint16_t key, uint32_t now) {
    if (key == KEY_UP || key == KEY_2) {
        adjust_game_level(&app->rotation_level_draft_byte, ROTATION_MAX_LEVELS, 1);
        app->dirty = true;
        return true;
    }
    if (key == KEY_DOWN || key == KEY_8) {
        adjust_game_level(&app->rotation_level_draft_byte, ROTATION_MAX_LEVELS, -1);
        app->dirty = true;
        return true;
    }
    if (key == KEY_NAVI || key == KEY_5) {
        app->rotation_level_byte = app->rotation_level_draft_byte;
        app->rotation_menu_variant = ROTATION_MENU_ACTIVE;
        rotation_save_storage(app);
        open_rotation_menu_variant(app, ROTATION_MENU_ACTIVE, ROTATION_ITEM_LEVEL);
        return true;
    }
    if (key == KEY_C) {
        app->rotation_level_draft_byte = app->rotation_level_byte;
        open_rotation_menu_variant(app, (rotation_menu_variant_t)app->rotation_menu_variant, ROTATION_ITEM_LEVEL);
        return true;
    }
    (void)now;
    return true;
}

bool handle_rotation_instructions_key(app_t *app, uint16_t key, uint32_t now) {
    uint8_t max_scroll = rotation_instruction_max_scroll();
    if ((key == KEY_UP || key == KEY_2) && app->rotation_instructions_scroll > 0u) {
        app->rotation_instructions_scroll--;
        app->dirty = true;
    } else if ((key == KEY_DOWN || key == KEY_8) && app->rotation_instructions_scroll < max_scroll) {
        app->rotation_instructions_scroll++;
        app->dirty = true;
    } else if (key == KEY_C || key == KEY_NAVI || key == KEY_5) {
        open_rotation_menu_variant(app, (rotation_menu_variant_t)app->rotation_menu_variant, ROTATION_ITEM_INSTRUCTIONS);
    }
    (void)now;
    return true;
}

bool handle_rotation_message_key(app_t *app, uint16_t key, uint32_t now) {
    if (key == KEY_C || key == KEY_NAVI || key == KEY_5) {
        rotation_item_kind_t item = app->route == APP_ROUTE_ROTATION_TOP_SCORE ? ROTATION_ITEM_TOP_SCORE : ROTATION_ITEM_LAST_VIEW;
        rotation_menu_variant_t variant = app->route == APP_ROUTE_ROTATION_RESULT ? ROTATION_MENU_LAST_VIEW :
            (rotation_menu_variant_t)app->rotation_menu_variant;
        open_rotation_menu_variant(app, variant, item);
    }
    (void)now;
    return true;
}

bool tick_rotation(app_t *app, uint32_t now) {
    bool changed = false;
    if (app->route == APP_ROUTE_ROTATION_PLAY) {
        if (app->rotation_solved_deadline_ms != 0u &&
            time_diff_ms(now, app->rotation_solved_deadline_ms) >= 0) {
            finish_rotation_game(app, now);
            changed = true;
        } else if (!app->rotation_game_over && app->rotation_next_animation_ms != 0u &&
                   time_diff_ms(now, app->rotation_next_animation_ms) >= 0) {
            rotation_animation_tick(app, now);
            changed = true;
        } else if (!app->rotation_game_over && app->rotation_ready &&
                   app->rotation_animation_steps_remaining == 0u &&
                   app->rotation_next_idle_ms != 0u &&
                   time_diff_ms(now, app->rotation_next_idle_ms) >= 0) {
            if (app->rotation_elapsed_ticks < 9999u) {
                app->rotation_elapsed_ticks++;
            }
            app->rotation_next_idle_ms = now + ROTATION_IDLE_TICK_MS;
        }
    }
    if (app->route == APP_ROUTE_ROTATION_RESULT &&
        app->rotation_result_deadline_ms != 0u &&
        time_diff_ms(now, app->rotation_result_deadline_ms) >= 0) {
        open_rotation_menu_variant(app, ROTATION_MENU_LAST_VIEW, ROTATION_ITEM_LAST_VIEW);
        changed = true;
    }
    if (app->route == APP_ROUTE_ROTATION_TOP_SCORE &&
        time_diff_ms(now, app->rotation_top_score_last_frame_ms + ROTATION_TOP_SCORE_FRAME_MS) >= 0) {
        app->rotation_top_score_last_frame_ms = now;
        if (app->rotation_top_score_frame + 1u < ARRAY_COUNT(ROTATION_TOP_SCORE_FRAMES)) {
            app->rotation_top_score_frame++;
            changed = true;
        }
    }
    return changed;
}

void render_rotation_menu(const app_t *app, framebuffer_t *fb) {
    rotation_menu_item_t items[6];
    const char *labels[6];
    uint8_t count = rotation_menu_items(app, items, (uint8_t)ARRAY_COUNT(items));
    for (uint8_t i = 0; i < count; i++) {
        labels[i] = items[i].label;
    }
    char crumb[10];
    draw_flat_list_circular_view(fb, labels, count,
                                 app->rotation_menu_selected,
                                 app->game_option_view_start,
                                 ui_breadcrumb_path(crumb, sizeof(crumb), "6-1", (unsigned)(app->rotation_menu_selected + 1u)),
                                 "Select");
}

void render_rotation_play(const app_t *app, framebuffer_t *fb) {
    draw_rotation_board(app, fb);
}

void render_rotation_level(const app_t *app, framebuffer_t *fb) {
    draw_game_level_selector(fb, app->rotation_level_draft_byte, ROTATION_MAX_LEVELS);
}

void render_rotation_instructions(const app_t *app, framebuffer_t *fb) {
    fb_clear(fb, false);
    char lines[18][32];
    uint8_t count = rotation_instruction_lines(lines, (uint8_t)ARRAY_COUNT(lines));
    uint8_t start = app->rotation_instructions_scroll;
    if (start > count) {
        start = 0u;
    }
    const font_t *font = asset_font(FONT_FS1);
    for (uint8_t row = 0; row < 3u && start + row < count; row++) {
        fb_text(fb, font, lines[start + row], 0, 7 + row * 9, true, FB_WIDTH);
    }
    draw_softkey(fb, "Back");
}

void render_rotation_top_score(const app_t *app, framebuffer_t *fb) {
    fb_clear(fb, false);
    char score[8];
    snprintf(score, sizeof(score), "%u", (unsigned)app->rotation_top_score);
    draw_rotation_message_text(fb, "Top score:", score, 0);
    uint8_t frame = app->rotation_top_score_frame;
    if (frame >= ARRAY_COUNT(ROTATION_TOP_SCORE_FRAMES)) {
        frame = (uint8_t)(ARRAY_COUNT(ROTATION_TOP_SCORE_FRAMES) - 1u);
    }
    fb_bitmap(fb, ROTATION_TOP_SCORE_FRAMES[frame], 63, 0, true, true);
}

void render_rotation_result(const app_t *app, framebuffer_t *fb) {
    fb_clear(fb, false);
    char score[8];
    snprintf(score, sizeof(score), "%u", (unsigned)app->rotation_score);
    draw_rotation_message_text(fb,
                               ts_or(0x157u, "Game over!"),
                               app->rotation_result_top_score ? "TOP SCORE:" : "Your score:",
                               score);
}

static void open_rotation_menu_variant(app_t *app, rotation_menu_variant_t variant, rotation_item_kind_t selected_kind) {
    app->route = APP_ROUTE_ROTATION_MENU;
    app->rotation_menu_variant = (uint8_t)variant;
    int8_t selected = rotation_menu_index_for_kind(app, selected_kind);
    app->rotation_menu_selected = selected >= 0 ? (uint8_t)selected : 0u;
    app->game_option_view_start = app->rotation_menu_selected;
    app->rotation_next_animation_ms = 0u;
    app->rotation_next_idle_ms = 0u;
    app->dirty = true;
}

static void open_rotation_level(app_t *app) {
    app->route = APP_ROUTE_ROTATION_LEVEL;
    app->rotation_level_draft_byte = app->rotation_level_byte;
    app->dirty = true;
}

static void open_rotation_top_score(app_t *app, uint32_t now) {
    app->route = APP_ROUTE_ROTATION_TOP_SCORE;
    app->rotation_top_score_frame = 0u;
    app->rotation_top_score_last_frame_ms = now;
    app->dirty = true;
}

static void open_rotation_instructions(app_t *app) {
    app->route = APP_ROUTE_ROTATION_INSTRUCTIONS;
    app->rotation_instructions_scroll = 0u;
    app->dirty = true;
}

static void start_rotation_game(app_t *app, uint32_t now) {
    const rotation_level_t *level = rotation_level_record(app->rotation_level_byte);
    app->route = APP_ROUTE_ROTATION_PLAY;
    app->rotation_menu_variant = ROTATION_MENU_ACTIVE;
    memset(app->rotation_board, 0, sizeof(app->rotation_board));
    memcpy(app->rotation_board, level->cells, (size_t)level->board_span * (size_t)level->board_span);
    app->rotation_cursor_x = 0u;
    app->rotation_cursor_y = 0u;
    app->rotation_ready_ticks_remaining = level->ready_ticks;
    app->rotation_ready = app->rotation_ready_ticks_remaining == 0u;
    app->rotation_elapsed_ticks = 0u;
    app->rotation_score = 0u;
    app->rotation_animation_phase = 0u;
    app->rotation_animation_steps_remaining = 0u;
    app->rotation_pending_rotation = 0u;
    app->rotation_game_over = false;
    app->rotation_result_top_score = false;
    app->rotation_solved_deadline_ms = 0u;
    app->rotation_result_deadline_ms = 0u;
    app->rotation_next_animation_ms = app->rotation_ready ? 0u : now + ROTATION_ANIMATION_STEP_MS;
    app->rotation_next_idle_ms = app->rotation_ready ? now + ROTATION_IDLE_TICK_MS : 0u;
    app->dirty = true;
}

static void pause_rotation_to_menu(app_t *app) {
    open_rotation_menu_variant(app, ROTATION_MENU_CONTINUE, ROTATION_ITEM_CONTINUE);
}

static void finish_rotation_game(app_t *app, uint32_t now) {
    app->route = APP_ROUTE_ROTATION_RESULT;
    app->rotation_solved_deadline_ms = 0u;
    app->rotation_result_deadline_ms = now + ROTATION_RESULT_DISMISS_MS;
    app->rotation_menu_variant = ROTATION_MENU_LAST_VIEW;
    app->rotation_result_top_score = app->rotation_score > app->rotation_top_score;
    if (app->rotation_result_top_score) {
        app->rotation_top_score = app->rotation_score;
        rotation_save_storage(app);
        /* Shared result helper 0x0029a2a0 plays tone 0x14 on a new TOP SCORE. */
        play_game_system_tone(20u);
    }
    app->dirty = true;
}

static void trigger_rotation_solved(app_t *app, uint32_t now) {
    app->rotation_game_over = true;
    uint32_t divisor = (uint32_t)app->rotation_elapsed_ticks + 10u;
    uint32_t score = (10000u / divisor) * ((uint32_t)app->rotation_level_byte + 1u);
    app->rotation_score = score > 9999u ? 9999u : (uint16_t)score;
    play_game_system_tone(19u);
    app->rotation_solved_deadline_ms = now + ROTATION_SOLVE_SETTLE_MS;
    app->rotation_next_animation_ms = 0u;
    app->rotation_next_idle_ms = 0u;
    app->dirty = true;
}

static void rotation_animation_tick(app_t *app, uint32_t now) {
    if (!app->rotation_ready && app->rotation_ready_ticks_remaining > 0u) {
        app->rotation_ready_ticks_remaining--;
        if (app->rotation_ready_ticks_remaining == 0u) {
            app->rotation_ready = true;
            app->rotation_next_animation_ms = 0u;
            app->rotation_next_idle_ms = now + ROTATION_IDLE_TICK_MS;
        } else {
            app->rotation_next_animation_ms = now + ROTATION_ANIMATION_STEP_MS;
        }
        app->dirty = true;
        return;
    }

    if (app->rotation_animation_steps_remaining == 0u) {
        app->rotation_next_animation_ms = 0u;
        return;
    }

    if (app->rotation_animation_phase == 1u) {
        app->rotation_animation_phase = 2u;
        app->rotation_animation_steps_remaining = 2u;
        app->rotation_next_animation_ms = now + ROTATION_ANIMATION_STEP_MS;
    } else if (app->rotation_animation_phase == 2u) {
        app->rotation_animation_phase = 5u;
        app->rotation_animation_steps_remaining = 1u;
        app->rotation_next_animation_ms = now + ROTATION_ANIMATION_STEP_MS;
    } else if (app->rotation_animation_phase == 3u) {
        app->rotation_animation_phase = 4u;
        app->rotation_animation_steps_remaining = 2u;
        app->rotation_next_animation_ms = now + ROTATION_ANIMATION_STEP_MS;
    } else if (app->rotation_animation_phase == 4u) {
        if (app->rotation_pending_rotation != 0u) {
            rotation_apply(app, true);
        }
        app->rotation_pending_rotation = 0u;
        app->rotation_animation_phase = 5u;
        app->rotation_animation_steps_remaining = 1u;
        app->rotation_next_animation_ms = now + ROTATION_ANIMATION_STEP_MS;
    } else {
        app->rotation_animation_phase = 0u;
        app->rotation_animation_steps_remaining = 0u;
        app->rotation_pending_rotation = 0u;
        app->rotation_next_animation_ms = 0u;
        if (rotation_is_solved(app)) {
            trigger_rotation_solved(app, now);
        } else {
            app->rotation_next_idle_ms = now + ROTATION_IDLE_TICK_MS;
        }
    }
    app->dirty = true;
}

static void rotation_start_animation(app_t *app, uint8_t phase, uint8_t pending_rotation, uint32_t now) {
    app->rotation_next_idle_ms = 0u;
    app->rotation_animation_phase = phase;
    app->rotation_animation_steps_remaining = 3u;
    app->rotation_pending_rotation = pending_rotation;
    app->rotation_next_animation_ms = now + ROTATION_ANIMATION_STEP_MS;
    app->dirty = true;
}

static void rotation_apply(app_t *app, bool clockwise) {
    const rotation_level_t *level = rotation_level_record(app->rotation_level_byte);
    uint8_t span = level->board_span;
    rotation_coord_t coords[8];
    uint8_t count = rotation_ring_coords(level->frame_size, coords, (uint8_t)ARRAY_COUNT(coords));
    uint8_t values[8];
    for (uint8_t i = 0; i < count; i++) {
        uint8_t row = (uint8_t)(app->rotation_cursor_y + coords[i].row);
        uint8_t col = (uint8_t)(app->rotation_cursor_x + coords[i].col);
        values[i] = app->rotation_board[row * span + col];
    }
    for (uint8_t i = 0; i < count; i++) {
        uint8_t src = clockwise ? (uint8_t)((i + count - 1u) % count) : (uint8_t)((i + 1u) % count);
        uint8_t row = (uint8_t)(app->rotation_cursor_y + coords[i].row);
        uint8_t col = (uint8_t)(app->rotation_cursor_x + coords[i].col);
        app->rotation_board[row * span + col] = values[src];
    }
}

static void rotation_move_cursor(app_t *app, int8_t dx, int8_t dy) {
    uint8_t positions = rotation_positions(app);
    int16_t x = (int16_t)app->rotation_cursor_x + dx;
    int16_t y = (int16_t)app->rotation_cursor_y + dy;
    while (x < 0) {
        x += positions;
    }
    while (y < 0) {
        y += positions;
    }
    app->rotation_cursor_x = (uint8_t)(x % positions);
    app->rotation_cursor_y = (uint8_t)(y % positions);
    app->dirty = true;
}

static void rotation_save_storage(const app_t *app) {
    store_setting_set_u8(STORE_SETTING_GAMES_ROTATION_LEVEL, app->rotation_level_byte);
    store_setting_set_u16(STORE_SETTING_GAMES_ROTATION_TOP_SCORE, app->rotation_top_score);
}

static uint8_t rotation_menu_items(const app_t *app, rotation_menu_item_t *items, uint8_t cap) {
    uint8_t count = 0u;
#define ADD_ITEM(k, text) do { \
    if (count < cap) { \
        items[count].kind = (k); \
        items[count].label = (text); \
        count++; \
    } \
} while (0)
    /* Dispatch is by .kind, not the label text, so the label is localized
     * directly (English literal kept inline as the fallback + documentation). */
    ADD_ITEM(ROTATION_ITEM_LEVEL, ts_or(0x164u, "Level"));
    if (app->rotation_menu_variant == ROTATION_MENU_CONTINUE) {
        ADD_ITEM(ROTATION_ITEM_CONTINUE, ts_or(0x160u, "Continue"));
    }
    if (app->rotation_menu_variant == ROTATION_MENU_LAST_VIEW) {
        ADD_ITEM(ROTATION_ITEM_LAST_VIEW, ts_or(0x163u, "Last view"));
    }
    ADD_ITEM(ROTATION_ITEM_NEW_GAME, ts_or(0x165u, "New game"));
    ADD_ITEM(ROTATION_ITEM_TOP_SCORE, ts_or(0x161u, "Top score"));
    ADD_ITEM(ROTATION_ITEM_INSTRUCTIONS, ts_or(0x162u, "Instructions"));
#undef ADD_ITEM
    return count;
}

static int8_t rotation_menu_index_for_kind(const app_t *app, rotation_item_kind_t kind) {
    rotation_menu_item_t items[6];
    uint8_t count = rotation_menu_items(app, items, (uint8_t)ARRAY_COUNT(items));
    for (uint8_t i = 0; i < count; i++) {
        if (items[i].kind == kind) {
            return (int8_t)i;
        }
    }
    return -1;
}

static const rotation_level_t *rotation_level_record(uint8_t level_byte) {
    uint8_t index = level_byte < ROTATION_MAX_LEVELS ? level_byte : 1u;
    return &ROTATION_LEVELS[index];
}

static uint8_t rotation_positions(const app_t *app) {
    const rotation_level_t *level = rotation_level_record(app->rotation_level_byte);
    return (uint8_t)(level->board_span - level->frame_size + 1u);
}

static uint8_t rotation_ring_coords(uint8_t frame_size, rotation_coord_t *coords, uint8_t cap) {
    uint8_t count = 0u;
#define ADD_COORD(r, c) do { \
    if (count < cap) { \
        coords[count].row = (r); \
        coords[count].col = (c); \
        count++; \
    } \
} while (0)
    for (uint8_t col = 0u; col < frame_size; col++) {
        ADD_COORD(0u, col);
    }
    for (uint8_t row = 1u; row < frame_size; row++) {
        ADD_COORD(row, (uint8_t)(frame_size - 1u));
    }
    for (int8_t col = (int8_t)frame_size - 2; col >= 0; col--) {
        ADD_COORD((uint8_t)(frame_size - 1u), (uint8_t)col);
    }
    for (int8_t row = (int8_t)frame_size - 2; row >= 1; row--) {
        ADD_COORD((uint8_t)row, 0u);
    }
#undef ADD_COORD
    return count;
}

static int8_t rotation_selected_ring_index(const app_t *app, uint8_t row, uint8_t col) {
    const rotation_level_t *level = rotation_level_record(app->rotation_level_byte);
    if (row < app->rotation_cursor_y || col < app->rotation_cursor_x) {
        return -1;
    }
    uint8_t local_row = (uint8_t)(row - app->rotation_cursor_y);
    uint8_t local_col = (uint8_t)(col - app->rotation_cursor_x);
    if (local_row >= level->frame_size || local_col >= level->frame_size) {
        return -1;
    }
    rotation_coord_t coords[8];
    uint8_t count = rotation_ring_coords(level->frame_size, coords, (uint8_t)ARRAY_COUNT(coords));
    for (uint8_t i = 0; i < count; i++) {
        if (coords[i].row == local_row && coords[i].col == local_col) {
            return (int8_t)i;
        }
    }
    return -1;
}

static bool rotation_is_solved(const app_t *app) {
    const rotation_level_t *level = rotation_level_record(app->rotation_level_byte);
    uint8_t expected = 1u;
    for (uint8_t row = 0u; row < level->board_span; row++) {
        for (uint8_t col = 0u; col < level->board_span; col++) {
            if (app->rotation_board[row * level->board_span + col] != expected) {
                return false;
            }
            expected++;
        }
    }
    return true;
}

static uint8_t rotation_instruction_lines(char lines[][32], uint8_t max_lines) {
    return wrap_text_lines_ex(asset_font(FONT_FS1), ts_or(0x40du, ROTATION_INSTRUCTIONS), FB_WIDTH, (char *)lines, 32u, max_lines);
}

static uint8_t rotation_instruction_max_scroll(void) {
    char lines[18][32];
    uint8_t count = rotation_instruction_lines(lines, (uint8_t)ARRAY_COUNT(lines));
    return count > 3u ? (uint8_t)(count - 3u) : 0u;
}

static void draw_rotation_board(const app_t *app, framebuffer_t *fb) {
    fb_clear(fb, false);
    const rotation_level_t *level = rotation_level_record(app->rotation_level_byte);
    int digit_ones_x = 48 - (int)level->board_span * 7;
    int digit_y = 26 - (int)level->board_span * 4;
    int frame_x = digit_ones_x - 6 + (int)app->rotation_cursor_x * 14;
    int frame_y = digit_y - 2 + (int)app->rotation_cursor_y * 8;
    int frame_w = (int)level->frame_size * 14 - 3;
    int frame_h = (int)level->frame_size * 8;
    fb_rect(fb, frame_x, frame_y, frame_w, frame_h, true);
    fb_fill_rect(fb, frame_x + 1, frame_y + 1, frame_w - 2, frame_h - 2, false);

    rotation_coord_t coords[8];
    uint8_t ring_count = rotation_ring_coords(level->frame_size, coords, (uint8_t)ARRAY_COUNT(coords));
    for (uint8_t row = 0u; row < level->board_span; row++) {
        for (uint8_t col = 0u; col < level->board_span; col++) {
            int offset_x = 0;
            int offset_y = 0;
            int8_t ring_index = rotation_selected_ring_index(app, row, col);
            if (ring_index >= 0 && app->rotation_animation_phase >= 1u && app->rotation_animation_phase <= 4u) {
                uint8_t next = (uint8_t)(((uint8_t)ring_index + 1u) % ring_count);
                int8_t delta_col = (int8_t)coords[next].col - (int8_t)coords[ring_index].col;
                int8_t delta_row = (int8_t)coords[next].row - (int8_t)coords[ring_index].row;
                bool large = app->rotation_animation_phase == 1u || app->rotation_animation_phase == 4u;
                if (delta_col < 0) {
                    offset_x = large ? -10 : -4;
                } else if (delta_col > 0) {
                    offset_x = large ? 10 : 4;
                }
                if (delta_row < 0) {
                    offset_y = large ? -6 : -2;
                } else if (delta_row > 0) {
                    offset_y = large ? 6 : 2;
                }
            }
            draw_rotation_number(fb,
                                 app->rotation_board[row * level->board_span + col],
                                 digit_ones_x + (int)col * 14 + offset_x,
                                 digit_y + (int)row * 8 + offset_y);
        }
    }
}

static void draw_rotation_digit(framebuffer_t *fb, uint8_t digit, int x, int y) {
    if (digit > 9u) {
        digit = 0u;
    }
    for (uint8_t col = 0u; col < 3u; col++) {
        uint8_t bits = ROTATION_DIGIT_GLYPHS[digit][col];
        for (uint8_t row = 0u; row < 5u; row++) {
            if ((bits & (uint8_t)(1u << row)) != 0u) {
                fb_pixel(fb, x + col, y + row, true);
            }
        }
    }
}

static void draw_rotation_number(framebuffer_t *fb, uint8_t value, int ones_x, int y) {
    if (value >= 10u) {
        draw_rotation_digit(fb, (uint8_t)(value / 10u), ones_x - 4, y);
        draw_rotation_digit(fb, (uint8_t)(value % 10u), ones_x, y);
    } else {
        draw_rotation_digit(fb, value, ones_x, y);
    }
}

static void draw_rotation_message_text(framebuffer_t *fb, const char *a, const char *b, const char *c) {
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

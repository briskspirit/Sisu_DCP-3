#include "apps/pacman_app.h"
#include "apps/game_common.h"

#include <stdio.h>
#include <string.h>

#include "apps/games_app.h"
#include "apps/profiles_app.h"
#include "audio/audio_levels.h"
#include "services/core1_services.h"
#include "services/input_keys.h"
#include "storage/store_service.h"
#include "services/timebase.h"
#include "services/strings.h"

#define PACMAN_MAX_LEVELS 7u
#define PACMAN_CELL 4u
#define PACMAN_COLS 21u
#define PACMAN_ROWS 12u
#define PACMAN_GHOST_COUNT 3u
#define PACMAN_POWER_TICKS 34u
#define PACMAN_RESULT_DISMISS_MS 3850u
#define PACMAN_TOP_SCORE_FRAME_MS 160u
#define PACMAN_RNG_FALLBACK_SEED 1u

typedef enum {
    PACMAN_MENU_NEW = 0,
    PACMAN_MENU_ACTIVE,
    PACMAN_MENU_CONTINUE,
    PACMAN_MENU_LAST_VIEW,
} pacman_menu_variant_t;

typedef enum {
    PACMAN_ITEM_LEVEL = 0,
    PACMAN_ITEM_CONTINUE,
    PACMAN_ITEM_LAST_VIEW,
    PACMAN_ITEM_NEW_GAME,
    PACMAN_ITEM_TOP_SCORE,
    PACMAN_ITEM_INSTRUCTIONS,
} pacman_item_kind_t;

typedef enum {
    PACMAN_DIR_UP = 0,
    PACMAN_DIR_DOWN,
    PACMAN_DIR_LEFT,
    PACMAN_DIR_RIGHT,
} pacman_direction_t;

typedef struct {
    pacman_item_kind_t kind;
    const char *label;
} pacman_menu_item_t;

static const uint16_t PACMAN_SPEED_MS[PACMAN_MAX_LEVELS] = {240u, 215u, 190u, 170u, 150u, 135u, 120u};
static const char *const PACMAN_MAZE[PACMAN_ROWS] = {
    "#####################",
    "#.........#.........#",
    "#.###.###.#.###.###.#",
    "#o#.....#...#.....#o#",
    "#.###.#.#####.#.###.#",
    "#.....#.......#.....#",
    "###.#.###...###.#.###",
    "#.....#.......#.....#",
    "#.###.#.#####.#.###.#",
    "#o#.....#...#.....#o#",
    "#.........#.........#",
    "#####################",
};
static const uint8_t PACMAN_GHOST_START_X[PACMAN_GHOST_COUNT] = {10u, 9u, 11u};
static const uint8_t PACMAN_GHOST_START_Y[PACMAN_GHOST_COUNT] = {5u, 5u, 5u};
static const uint8_t PACMAN_GHOST_START_DIR[PACMAN_GHOST_COUNT] = {PACMAN_DIR_LEFT, PACMAN_DIR_UP, PACMAN_DIR_DOWN};
static const uint16_t PACMAN_TOP_SCORE_FRAMES[] = {
    220u, 221u, 222u, 223u, 224u, 225u, 226u, 227u, 228u, 229u,
    229u, 230u, 230u, 229u, 229u, 230u, 230u, 229u, 230u, 230u,
};
static const char PACMAN_INSTRUCTIONS[] =
    "Eat all dots while avoiding the ghosts. Use keys 2, 4, 6 and 8 to steer. Large dots let you chase ghosts for a short time. Press C to pause.";

static void open_pacman_menu_variant(app_t *app, pacman_menu_variant_t variant, pacman_item_kind_t selected_kind);
static void open_pacman_level(app_t *app);
static void open_pacman_top_score(app_t *app, uint32_t now);
static void open_pacman_instructions(app_t *app);
static void start_pacman_game(app_t *app, uint32_t now);
static void pause_pacman_to_menu(app_t *app);
static void pacman_reset_actors(app_t *app);
static void pacman_build_pellets(app_t *app);
static void pacman_step(app_t *app, uint32_t now);
static bool pacman_eat_pellet(app_t *app, uint32_t now);
static bool pacman_handle_collisions(app_t *app, uint32_t now);
static void pacman_move_ghosts(app_t *app);
static uint8_t pacman_choose_ghost_direction(app_t *app, uint8_t ghost);
static void pacman_reset_ghost(app_t *app, uint8_t ghost);
static void pacman_finish_game(app_t *app, bool won, uint32_t now);
static bool pacman_can_move(uint8_t x, uint8_t y, uint8_t direction);
static bool pacman_passable(uint8_t x, uint8_t y);
static char pacman_maze_char(uint8_t x, uint8_t y);
static uint8_t pacman_opposite(uint8_t direction);
static void pacman_delta(uint8_t direction, int8_t *dx, int8_t *dy);
static uint8_t pacman_pellet_index(uint8_t x, uint8_t y);
static bool pacman_pellet_get(const app_t *app, uint8_t x, uint8_t y);
static void pacman_pellet_set(app_t *app, uint8_t x, uint8_t y, bool value);
static bool pacman_cell_blocked(uint8_t x, uint8_t y);
static uint16_t pacman_random(app_t *app);
static uint32_t pacman_seed(uint32_t now);
static uint16_t pacman_speed_ms(const app_t *app);
static void pacman_save_storage(const app_t *app);
static uint8_t pacman_menu_items(const app_t *app, pacman_menu_item_t *items, uint8_t cap);
static int8_t pacman_menu_index_for_kind(const app_t *app, pacman_item_kind_t kind);
static void play_pacman_tone(app_t *app, uint8_t kind);
static uint8_t pacman_instruction_lines(char lines[][32], uint8_t max_lines);
static uint8_t pacman_instruction_max_scroll(void);
static void draw_pacman_board(const app_t *app, framebuffer_t *fb);
static void draw_pacman_cell_sprite(framebuffer_t *fb, uint8_t x, uint8_t y, const uint8_t rows[4]);
static void draw_pacman_player(const app_t *app, framebuffer_t *fb);
static void draw_pacman_ghost(const app_t *app, framebuffer_t *fb, uint8_t ghost);
static void draw_pacman_message_text(framebuffer_t *fb, const char *a, const char *b, const char *c);

void pacman_app_init(app_t *app) {
    uint8_t level = 0u;
    uint16_t top_score = 0u;
    store_setting_get_u8(STORE_SETTING_GAMES_PACMAN_LEVEL, &level);
    store_setting_get_u16(STORE_SETTING_GAMES_PACMAN_TOP_SCORE, &top_score);
    if (level >= PACMAN_MAX_LEVELS) {
        level = 0u;
    }
    if (top_score > 9999u) {
        top_score = 9999u;
    }
    app->pacman_menu_variant = PACMAN_MENU_NEW;
    app->pacman_menu_selected = 0u;
    app->pacman_level_byte = level;
    app->pacman_level_draft_byte = level;
    app->pacman_top_score = top_score;
}

void open_pacman_menu(app_t *app) {
    open_pacman_menu_variant(app, (pacman_menu_variant_t)app->pacman_menu_variant, PACMAN_ITEM_NEW_GAME);
}

bool handle_pacman_menu_key(app_t *app, uint16_t key, uint32_t now) {
    pacman_menu_item_t items[6];
    uint8_t count = pacman_menu_items(app, items, (uint8_t)ARRAY_COUNT(items));
    if (count == 0u) {
        return true;
    }
    if (app->pacman_menu_selected >= count) {
        app->pacman_menu_selected = 0u;
        app->game_option_view_start = 0u;
    }
    if (key == KEY_C) {
        open_games_menu(app, 5u);
        return true;
    }
    if (key == KEY_UP || key == KEY_2) {
        ui_circular_list_step_3rows(count, -1,
                                    &app->pacman_menu_selected,
                                    &app->game_option_view_start);
        app->dirty = true;
        return true;
    }
    if (key == KEY_DOWN || key == KEY_8) {
        ui_circular_list_step_3rows(count, 1,
                                    &app->pacman_menu_selected,
                                    &app->game_option_view_start);
        app->dirty = true;
        return true;
    }
    if (key != KEY_NAVI && key != KEY_5) {
        return true;
    }
    pacman_item_kind_t kind = items[app->pacman_menu_selected].kind;
    if (kind == PACMAN_ITEM_NEW_GAME) {
        start_pacman_game(app, now);
    } else if (kind == PACMAN_ITEM_CONTINUE) {
        app->route = APP_ROUTE_PACMAN_PLAY;
        app->pacman_game_over = false;
        app->pacman_next_tick_ms = now + pacman_speed_ms(app);
        app->dirty = true;
    } else if (kind == PACMAN_ITEM_LAST_VIEW) {
        app->route = APP_ROUTE_PACMAN_LAST_VIEW;
        app->dirty = true;
    } else if (kind == PACMAN_ITEM_LEVEL) {
        open_pacman_level(app);
    } else if (kind == PACMAN_ITEM_TOP_SCORE) {
        open_pacman_top_score(app, now);
    } else if (kind == PACMAN_ITEM_INSTRUCTIONS) {
        open_pacman_instructions(app);
    }
    return true;
}

bool handle_pacman_play_key(app_t *app, uint16_t key, uint32_t now) {
    if (app->route == APP_ROUTE_PACMAN_LAST_VIEW) {
        if (key == KEY_C || key == KEY_NAVI || key == KEY_5) {
            open_pacman_menu_variant(app, PACMAN_MENU_LAST_VIEW, PACMAN_ITEM_LAST_VIEW);
        }
        return true;
    }
    if (key == KEY_C || key == KEY_NAVI) {
        pause_pacman_to_menu(app);
        return true;
    }
    if (app->pacman_game_over) {
        return true;
    }
    if (key == KEY_UP || key == KEY_2) {
        app->pacman_pending_direction = PACMAN_DIR_UP;
        app->dirty = true;
    } else if (key == KEY_DOWN || key == KEY_8) {
        app->pacman_pending_direction = PACMAN_DIR_DOWN;
        app->dirty = true;
    } else if (key == KEY_4) {
        app->pacman_pending_direction = PACMAN_DIR_LEFT;
        app->dirty = true;
    } else if (key == KEY_6) {
        app->pacman_pending_direction = PACMAN_DIR_RIGHT;
        app->dirty = true;
    }
    (void)now;
    return true;
}

bool handle_pacman_level_key(app_t *app, uint16_t key, uint32_t now) {
    if (key == KEY_UP || key == KEY_2) {
        adjust_game_level(&app->pacman_level_draft_byte, PACMAN_MAX_LEVELS, 1);
        app->dirty = true;
        return true;
    }
    if (key == KEY_DOWN || key == KEY_8) {
        adjust_game_level(&app->pacman_level_draft_byte, PACMAN_MAX_LEVELS, -1);
        app->dirty = true;
        return true;
    }
    if (key == KEY_NAVI || key == KEY_5) {
        app->pacman_level_byte = app->pacman_level_draft_byte;
        app->pacman_menu_variant = PACMAN_MENU_ACTIVE;
        pacman_save_storage(app);
        open_pacman_menu_variant(app, PACMAN_MENU_ACTIVE, PACMAN_ITEM_LEVEL);
        return true;
    }
    if (key == KEY_C) {
        app->pacman_level_draft_byte = app->pacman_level_byte;
        open_pacman_menu_variant(app, (pacman_menu_variant_t)app->pacman_menu_variant, PACMAN_ITEM_LEVEL);
        return true;
    }
    (void)now;
    return true;
}

bool handle_pacman_instructions_key(app_t *app, uint16_t key, uint32_t now) {
    uint8_t max_scroll = pacman_instruction_max_scroll();
    if ((key == KEY_UP || key == KEY_2) && app->pacman_instructions_scroll > 0u) {
        app->pacman_instructions_scroll--;
        app->dirty = true;
    } else if ((key == KEY_DOWN || key == KEY_8) && app->pacman_instructions_scroll < max_scroll) {
        app->pacman_instructions_scroll++;
        app->dirty = true;
    } else if (key == KEY_C || key == KEY_NAVI || key == KEY_5) {
        open_pacman_menu_variant(app, (pacman_menu_variant_t)app->pacman_menu_variant, PACMAN_ITEM_INSTRUCTIONS);
    }
    (void)now;
    return true;
}

bool handle_pacman_message_key(app_t *app, uint16_t key, uint32_t now) {
    if (key == KEY_C || key == KEY_NAVI || key == KEY_5) {
        pacman_item_kind_t item = app->route == APP_ROUTE_PACMAN_TOP_SCORE ? PACMAN_ITEM_TOP_SCORE : PACMAN_ITEM_LAST_VIEW;
        pacman_menu_variant_t variant = app->route == APP_ROUTE_PACMAN_RESULT ? PACMAN_MENU_LAST_VIEW :
            (pacman_menu_variant_t)app->pacman_menu_variant;
        open_pacman_menu_variant(app, variant, item);
    }
    (void)now;
    return true;
}

bool tick_pacman(app_t *app, uint32_t now) {
    bool changed = false;
    if (app->route == APP_ROUTE_PACMAN_PLAY && !app->pacman_game_over &&
        app->pacman_next_tick_ms != 0u && time_diff_ms(now, app->pacman_next_tick_ms) >= 0) {
        pacman_step(app, now);
        changed = true;
    }
    if (app->route == APP_ROUTE_PACMAN_RESULT &&
        app->pacman_result_deadline_ms != 0u &&
        time_diff_ms(now, app->pacman_result_deadline_ms) >= 0) {
        open_pacman_menu_variant(app, PACMAN_MENU_LAST_VIEW, PACMAN_ITEM_LAST_VIEW);
        changed = true;
    }
    if (app->route == APP_ROUTE_PACMAN_TOP_SCORE &&
        time_diff_ms(now, app->pacman_top_score_last_frame_ms + PACMAN_TOP_SCORE_FRAME_MS) >= 0) {
        app->pacman_top_score_last_frame_ms = now;
        if (app->pacman_top_score_frame + 1u < ARRAY_COUNT(PACMAN_TOP_SCORE_FRAMES)) {
            app->pacman_top_score_frame++;
            changed = true;
        }
    }
    return changed;
}

void render_pacman_menu(const app_t *app, framebuffer_t *fb) {
    pacman_menu_item_t items[6];
    const char *labels[6];
    uint8_t count = pacman_menu_items(app, items, (uint8_t)ARRAY_COUNT(items));
    for (uint8_t i = 0u; i < count; i++) {
        labels[i] = items[i].label;
    }
    char crumb[10];
    draw_flat_list_circular_view(fb, labels, count,
                                 app->pacman_menu_selected,
                                 app->game_option_view_start,
                                 ui_breadcrumb_path(crumb, sizeof(crumb), "6-6", (unsigned)(app->pacman_menu_selected + 1u)),
                                 "Select");
}

void render_pacman_play(const app_t *app, framebuffer_t *fb) {
    draw_pacman_board(app, fb);
}

void render_pacman_level(const app_t *app, framebuffer_t *fb) {
    draw_game_level_selector(fb, app->pacman_level_draft_byte, PACMAN_MAX_LEVELS);
}

void render_pacman_instructions(const app_t *app, framebuffer_t *fb) {
    fb_clear(fb, false);
    char lines[18][32];
    uint8_t count = pacman_instruction_lines(lines, (uint8_t)ARRAY_COUNT(lines));
    uint8_t start = app->pacman_instructions_scroll;
    if (start > count) {
        start = 0u;
    }
    const font_t *font = asset_font(FONT_FS1);
    for (uint8_t row = 0u; row < 3u && start + row < count; row++) {
        fb_text(fb, font, lines[start + row], 0, 7 + row * 9, true, FB_WIDTH);
    }
    draw_softkey(fb, "Back");
}

void render_pacman_top_score(const app_t *app, framebuffer_t *fb) {
    fb_clear(fb, false);
    char score[8];
    snprintf(score, sizeof(score), "%u", (unsigned)app->pacman_top_score);
    draw_pacman_message_text(fb, "Top score:", score, 0);
    uint8_t frame = app->pacman_top_score_frame;
    if (frame >= ARRAY_COUNT(PACMAN_TOP_SCORE_FRAMES)) {
        frame = (uint8_t)(ARRAY_COUNT(PACMAN_TOP_SCORE_FRAMES) - 1u);
    }
    fb_bitmap(fb, PACMAN_TOP_SCORE_FRAMES[frame], 63, 0, true, true);
}

void render_pacman_result(const app_t *app, framebuffer_t *fb) {
    fb_clear(fb, false);
    char score[8];
    snprintf(score, sizeof(score), "%u", (unsigned)app->pacman_score);
    draw_pacman_message_text(fb,
                             ts_or(0x157u, "Game over!"),
                             app->pacman_result_won ? "YOU WON!" :
                             (app->pacman_result_top_score ? "TOP SCORE:" : "Your score:"),
                             score);
}

static void open_pacman_menu_variant(app_t *app, pacman_menu_variant_t variant, pacman_item_kind_t selected_kind) {
    app->route = APP_ROUTE_PACMAN_MENU;
    app->pacman_menu_variant = (uint8_t)variant;
    int8_t selected = pacman_menu_index_for_kind(app, selected_kind);
    app->pacman_menu_selected = selected >= 0 ? (uint8_t)selected : 0u;
    app->game_option_view_start = app->pacman_menu_selected;
    app->dirty = true;
}

static void open_pacman_level(app_t *app) {
    app->route = APP_ROUTE_PACMAN_LEVEL;
    app->pacman_level_draft_byte = app->pacman_level_byte;
    app->dirty = true;
}

static void open_pacman_top_score(app_t *app, uint32_t now) {
    app->route = APP_ROUTE_PACMAN_TOP_SCORE;
    app->pacman_top_score_frame = 0u;
    app->pacman_top_score_last_frame_ms = now;
    app->dirty = true;
}

static void open_pacman_instructions(app_t *app) {
    app->route = APP_ROUTE_PACMAN_INSTRUCTIONS;
    app->pacman_instructions_scroll = 0u;
    app->dirty = true;
}

static void start_pacman_game(app_t *app, uint32_t now) {
    app->route = APP_ROUTE_PACMAN_PLAY;
    app->pacman_menu_variant = PACMAN_MENU_ACTIVE;
    app->pacman_score = 0u;
    app->pacman_lives = 3u;
    app->pacman_power_ticks = 0u;
    app->pacman_tick_count = 0u;
    app->pacman_game_over = false;
    app->pacman_result_top_score = false;
    app->pacman_result_won = false;
    app->pacman_result_deadline_ms = 0u;
    app->pacman_rng_seed = pacman_seed(now);
    pacman_build_pellets(app);
    pacman_reset_actors(app);
    app->pacman_next_tick_ms = now + pacman_speed_ms(app);
    app->dirty = true;
}

static void pause_pacman_to_menu(app_t *app) {
    app->pacman_next_tick_ms = 0u;
    open_pacman_menu_variant(app, PACMAN_MENU_CONTINUE, PACMAN_ITEM_CONTINUE);
}

static void pacman_reset_actors(app_t *app) {
    app->pacman_player_x = 10u;
    app->pacman_player_y = 9u;
    app->pacman_direction = PACMAN_DIR_LEFT;
    app->pacman_pending_direction = PACMAN_DIR_LEFT;
    for (uint8_t i = 0u; i < PACMAN_GHOST_COUNT; i++) {
        pacman_reset_ghost(app, i);
    }
}

static void pacman_build_pellets(app_t *app) {
    memset(app->pacman_pellets, 0, sizeof(app->pacman_pellets));
    app->pacman_pellets_remaining = 0u;
    for (uint8_t y = 0u; y < PACMAN_ROWS; y++) {
        for (uint8_t x = 0u; x < PACMAN_COLS; x++) {
            char cell = pacman_maze_char(x, y);
            if ((cell == '.' || cell == 'o') && !pacman_cell_blocked(x, y)) {
                pacman_pellet_set(app, x, y, true);
                app->pacman_pellets_remaining++;
            }
        }
    }
}

static void pacman_step(app_t *app, uint32_t now) {
    if (app->route != APP_ROUTE_PACMAN_PLAY || app->pacman_game_over) {
        return;
    }
    app->pacman_tick_count++;
    if (pacman_can_move(app->pacman_player_x, app->pacman_player_y, app->pacman_pending_direction)) {
        app->pacman_direction = app->pacman_pending_direction;
    }
    if (pacman_can_move(app->pacman_player_x, app->pacman_player_y, app->pacman_direction)) {
        int8_t dx = 0;
        int8_t dy = 0;
        pacman_delta(app->pacman_direction, &dx, &dy);
        app->pacman_player_x = (uint8_t)(app->pacman_player_x + dx);
        app->pacman_player_y = (uint8_t)(app->pacman_player_y + dy);
    }
    if (pacman_eat_pellet(app, now)) {
        return;
    }
    if (pacman_handle_collisions(app, now)) {
        app->pacman_next_tick_ms = now + pacman_speed_ms(app);
        return;
    }
    pacman_move_ghosts(app);
    (void)pacman_handle_collisions(app, now);
    if (app->pacman_power_ticks > 0u) {
        app->pacman_power_ticks--;
    }
    app->pacman_next_tick_ms = now + pacman_speed_ms(app);
    app->dirty = true;
}

static bool pacman_eat_pellet(app_t *app, uint32_t now) {
    if (!pacman_pellet_get(app, app->pacman_player_x, app->pacman_player_y)) {
        return false;
    }
    char cell = pacman_maze_char(app->pacman_player_x, app->pacman_player_y);
    pacman_pellet_set(app, app->pacman_player_x, app->pacman_player_y, false);
    if (app->pacman_pellets_remaining > 0u) {
        app->pacman_pellets_remaining--;
    }
    if (cell == 'o') {
        app->pacman_score = (uint16_t)(app->pacman_score + 50u);
        app->pacman_power_ticks = PACMAN_POWER_TICKS;
        play_pacman_tone(app, 2u);
    } else {
        app->pacman_score = (uint16_t)(app->pacman_score + 10u);
        play_pacman_tone(app, (uint8_t)(app->pacman_tick_count & 1u));
    }
    if (app->pacman_score > 9999u) {
        app->pacman_score = 9999u;
    }
    if (app->pacman_pellets_remaining == 0u) {
        pacman_finish_game(app, true, now);
        return true;
    }
    app->dirty = true;
    return false;
}

static bool pacman_handle_collisions(app_t *app, uint32_t now) {
    for (uint8_t i = 0u; i < PACMAN_GHOST_COUNT; i++) {
        if (app->pacman_ghost_x[i] != app->pacman_player_x ||
            app->pacman_ghost_y[i] != app->pacman_player_y) {
            continue;
        }
        if (app->pacman_power_ticks > 0u) {
            app->pacman_score = (uint16_t)(app->pacman_score + 200u);
            if (app->pacman_score > 9999u) {
                app->pacman_score = 9999u;
            }
            pacman_reset_ghost(app, i);
            play_pacman_tone(app, 3u);
            app->dirty = true;
            continue;
        }
        if (app->pacman_lives > 0u) {
            app->pacman_lives--;
        }
        if (app->pacman_lives == 0u) {
            pacman_finish_game(app, false, now);
            return true;
        }
        play_pacman_tone(app, 4u);
        pacman_reset_actors(app);
        app->pacman_power_ticks = 0u;
        app->dirty = true;
        return true;
    }
    return false;
}

static void pacman_move_ghosts(app_t *app) {
    uint8_t ghost_stride = app->pacman_power_ticks > 0u || app->pacman_level_byte < 2u ? 2u : 1u;
    for (uint8_t i = 0u; i < PACMAN_GHOST_COUNT; i++) {
        if (((uint16_t)app->pacman_tick_count + i) % ghost_stride != 0u) {
            continue;
        }
        uint8_t direction = pacman_choose_ghost_direction(app, i);
        app->pacman_ghost_direction[i] = direction;
        if (pacman_can_move(app->pacman_ghost_x[i], app->pacman_ghost_y[i], direction)) {
            int8_t dx = 0;
            int8_t dy = 0;
            pacman_delta(direction, &dx, &dy);
            app->pacman_ghost_x[i] = (uint8_t)(app->pacman_ghost_x[i] + dx);
            app->pacman_ghost_y[i] = (uint8_t)(app->pacman_ghost_y[i] + dy);
        }
    }
}

static uint8_t pacman_choose_ghost_direction(app_t *app, uint8_t ghost) {
    static const uint8_t directions[] = {PACMAN_DIR_UP, PACMAN_DIR_LEFT, PACMAN_DIR_RIGHT, PACMAN_DIR_DOWN};
    uint8_t options[4];
    uint8_t option_count = 0u;
    uint8_t gx = app->pacman_ghost_x[ghost];
    uint8_t gy = app->pacman_ghost_y[ghost];
    uint8_t reverse = pacman_opposite(app->pacman_ghost_direction[ghost]);
    for (uint8_t i = 0u; i < ARRAY_COUNT(directions); i++) {
        uint8_t direction = directions[i];
        if (pacman_can_move(gx, gy, direction)) {
            options[option_count++] = direction;
        }
    }
    if (option_count > 1u) {
        uint8_t filtered[4];
        uint8_t filtered_count = 0u;
        for (uint8_t i = 0u; i < option_count; i++) {
            if (options[i] != reverse) {
                filtered[filtered_count++] = options[i];
            }
        }
        if (filtered_count > 0u) {
            memcpy(options, filtered, filtered_count);
            option_count = filtered_count;
        }
    }
    if (option_count == 0u) {
        return app->pacman_ghost_direction[ghost];
    }

    bool frightened = app->pacman_power_ticks > 0u;
    int16_t best_score = frightened ? -32767 : 32767;
    uint8_t best[4];
    uint8_t best_count = 0u;
    for (uint8_t i = 0u; i < option_count; i++) {
        int8_t dx = 0;
        int8_t dy = 0;
        pacman_delta(options[i], &dx, &dy);
        int16_t nx = (int16_t)gx + dx;
        int16_t ny = (int16_t)gy + dy;
        int16_t dist = (int16_t)((nx > app->pacman_player_x ? nx - app->pacman_player_x : app->pacman_player_x - nx) +
                                  (ny > app->pacman_player_y ? ny - app->pacman_player_y : app->pacman_player_y - ny));
        bool better = frightened ? dist > best_score : dist < best_score;
        if (better) {
            best_score = dist;
            best_count = 0u;
            best[best_count++] = options[i];
        } else if (dist == best_score && best_count < ARRAY_COUNT(best)) {
            best[best_count++] = options[i];
        }
    }
    return best[pacman_random(app) % best_count];
}

static void pacman_reset_ghost(app_t *app, uint8_t ghost) {
    uint8_t i = ghost % PACMAN_GHOST_COUNT;
    app->pacman_ghost_x[i] = PACMAN_GHOST_START_X[i];
    app->pacman_ghost_y[i] = PACMAN_GHOST_START_Y[i];
    app->pacman_ghost_direction[i] = PACMAN_GHOST_START_DIR[i];
}

static void pacman_finish_game(app_t *app, bool won, uint32_t now) {
    app->route = APP_ROUTE_PACMAN_RESULT;
    app->pacman_game_over = true;
    app->pacman_menu_variant = PACMAN_MENU_LAST_VIEW;
    app->pacman_result_deadline_ms = now + PACMAN_RESULT_DISMISS_MS;
    app->pacman_result_won = won;
    app->pacman_result_top_score = app->pacman_score > app->pacman_top_score;
    if (app->pacman_result_top_score) {
        app->pacman_top_score = app->pacman_score;
        pacman_save_storage(app);
    }
    play_pacman_tone(app, won ? 5u : 4u);
    app->dirty = true;
}

static bool pacman_can_move(uint8_t x, uint8_t y, uint8_t direction) {
    int8_t dx = 0;
    int8_t dy = 0;
    pacman_delta(direction, &dx, &dy);
    int16_t nx = (int16_t)x + dx;
    int16_t ny = (int16_t)y + dy;
    if (nx < 0 || ny < 0 || nx >= (int16_t)PACMAN_COLS || ny >= (int16_t)PACMAN_ROWS) {
        return false;
    }
    return pacman_passable((uint8_t)nx, (uint8_t)ny);
}

static bool pacman_passable(uint8_t x, uint8_t y) {
    return pacman_maze_char(x, y) != '#';
}

static char pacman_maze_char(uint8_t x, uint8_t y) {
    if (x >= PACMAN_COLS || y >= PACMAN_ROWS) {
        return '#';
    }
    return PACMAN_MAZE[y][x];
}

static uint8_t pacman_opposite(uint8_t direction) {
    if (direction == PACMAN_DIR_UP) {
        return PACMAN_DIR_DOWN;
    }
    if (direction == PACMAN_DIR_DOWN) {
        return PACMAN_DIR_UP;
    }
    if (direction == PACMAN_DIR_LEFT) {
        return PACMAN_DIR_RIGHT;
    }
    return PACMAN_DIR_LEFT;
}

static void pacman_delta(uint8_t direction, int8_t *dx, int8_t *dy) {
    *dx = 0;
    *dy = 0;
    if (direction == PACMAN_DIR_UP) {
        *dy = -1;
    } else if (direction == PACMAN_DIR_DOWN) {
        *dy = 1;
    } else if (direction == PACMAN_DIR_LEFT) {
        *dx = -1;
    } else if (direction == PACMAN_DIR_RIGHT) {
        *dx = 1;
    }
}

static uint8_t pacman_pellet_index(uint8_t x, uint8_t y) {
    return (uint8_t)(y * PACMAN_COLS + x);
}

static bool pacman_pellet_get(const app_t *app, uint8_t x, uint8_t y) {
    uint8_t index = pacman_pellet_index(x, y);
    return (app->pacman_pellets[index >> 3] & (uint8_t)(1u << (index & 7u))) != 0u;
}

static void pacman_pellet_set(app_t *app, uint8_t x, uint8_t y, bool value) {
    uint8_t index = pacman_pellet_index(x, y);
    uint8_t mask = (uint8_t)(1u << (index & 7u));
    if (value) {
        app->pacman_pellets[index >> 3] |= mask;
    } else {
        app->pacman_pellets[index >> 3] &= (uint8_t)~mask;
    }
}

static bool pacman_cell_blocked(uint8_t x, uint8_t y) {
    if (x == 10u && y == 9u) {
        return true;
    }
    for (uint8_t i = 0u; i < PACMAN_GHOST_COUNT; i++) {
        if (x == PACMAN_GHOST_START_X[i] && y == PACMAN_GHOST_START_Y[i]) {
            return true;
        }
    }
    return false;
}

static uint16_t pacman_random(app_t *app) {
    app->pacman_rng_seed = app->pacman_rng_seed * 1103515245u + 12345u;
    return (uint16_t)((app->pacman_rng_seed >> 16) & 0x7fffu);
}

static uint32_t pacman_seed(uint32_t now) {
    uint32_t seed = now ^ (time_ticks8() * 1103515245u) ^ 0x29afe8u ^ 0x2ac001u;
    return seed == 0u ? PACMAN_RNG_FALLBACK_SEED : seed;
}

static uint16_t pacman_speed_ms(const app_t *app) {
    uint8_t index = app->pacman_level_byte < PACMAN_MAX_LEVELS ? app->pacman_level_byte : 0u;
    return PACMAN_SPEED_MS[index];
}

static void pacman_save_storage(const app_t *app) {
    store_setting_set_u8(STORE_SETTING_GAMES_PACMAN_LEVEL, app->pacman_level_byte);
    store_setting_set_u16(STORE_SETTING_GAMES_PACMAN_TOP_SCORE, app->pacman_top_score);
}

static uint8_t pacman_menu_items(const app_t *app, pacman_menu_item_t *items, uint8_t cap) {
    uint8_t count = 0u;
#define ADD_ITEM(k, text) do { \
    if (count < cap) { \
        items[count].kind = (k); \
        items[count].label = (text); \
        count++; \
    } \
} while (0)
    /* Labels are display-only (menu dispatch is by item .kind, not label text),
     * so localize by v6.00 SID directly. These are the ROM game-menu strings. */
    ADD_ITEM(PACMAN_ITEM_LEVEL, ts_or(0x164u, "Level"));
    if (app->pacman_menu_variant == PACMAN_MENU_CONTINUE) {
        ADD_ITEM(PACMAN_ITEM_CONTINUE, ts_or(0x160u, "Continue"));
    }
    if (app->pacman_menu_variant == PACMAN_MENU_LAST_VIEW) {
        ADD_ITEM(PACMAN_ITEM_LAST_VIEW, ts_or(0x163u, "Last view"));
    }
    ADD_ITEM(PACMAN_ITEM_NEW_GAME, ts_or(0x165u, "New game"));
    ADD_ITEM(PACMAN_ITEM_TOP_SCORE, ts_or(0x161u, "Top score"));
    ADD_ITEM(PACMAN_ITEM_INSTRUCTIONS, ts_or(0x162u, "Instructions"));
#undef ADD_ITEM
    return count;
}

static int8_t pacman_menu_index_for_kind(const app_t *app, pacman_item_kind_t kind) {
    pacman_menu_item_t items[6];
    uint8_t count = pacman_menu_items(app, items, (uint8_t)ARRAY_COUNT(items));
    for (uint8_t i = 0u; i < count; i++) {
        if (items[i].kind == kind) {
            return (int8_t)i;
        }
    }
    return -1;
}

static void play_pacman_tone(app_t *app, uint8_t kind) {
    uint8_t level = game_audio_level();
    if (level != AUDIO_LEVEL_SILENT) {
        core1_post_command(CORE1_CMD_AUDIO_PACMAN_TONE, audio_arg(kind, level));
    }
    (void)app;
}

static uint8_t pacman_instruction_lines(char lines[][32], uint8_t max_lines) {
    return wrap_text_lines_ex(asset_font(FONT_FS1), PACMAN_INSTRUCTIONS, FB_WIDTH, (char *)lines, 32u, max_lines);
}

static uint8_t pacman_instruction_max_scroll(void) {
    char lines[18][32];
    uint8_t count = pacman_instruction_lines(lines, (uint8_t)ARRAY_COUNT(lines));
    return count > 3u ? (uint8_t)(count - 3u) : 0u;
}

static void draw_pacman_board(const app_t *app, framebuffer_t *fb) {
    fb_clear(fb, false);
    for (uint8_t y = 0u; y < PACMAN_ROWS; y++) {
        for (uint8_t x = 0u; x < PACMAN_COLS; x++) {
            char cell = pacman_maze_char(x, y);
            if (cell == '#') {
                fb_fill_rect(fb, x * PACMAN_CELL, y * PACMAN_CELL, PACMAN_CELL, PACMAN_CELL, true);
            } else if (pacman_pellet_get(app, x, y)) {
                if (cell == 'o') {
                    fb_fill_rect(fb, x * PACMAN_CELL + 1, y * PACMAN_CELL + 1, 2, 2, true);
                } else {
                    fb_pixel(fb, x * PACMAN_CELL + 2, y * PACMAN_CELL + 2, true);
                }
            }
        }
    }
    for (uint8_t i = 0u; i < PACMAN_GHOST_COUNT; i++) {
        draw_pacman_ghost(app, fb, i);
    }
    draw_pacman_player(app, fb);
}

static void draw_pacman_cell_sprite(framebuffer_t *fb, uint8_t x, uint8_t y, const uint8_t rows[4]) {
    int px = x * PACMAN_CELL;
    int py = y * PACMAN_CELL;
    for (uint8_t yy = 0u; yy < 4u; yy++) {
        for (uint8_t xx = 0u; xx < 4u; xx++) {
            if ((rows[yy] & (uint8_t)(1u << (3u - xx))) != 0u) {
                fb_pixel(fb, px + xx, py + yy, true);
            }
        }
    }
}

static void draw_pacman_player(const app_t *app, framebuffer_t *fb) {
    static const uint8_t closed[4] = {0x06u, 0x0fu, 0x0fu, 0x06u};
    static const uint8_t right[4] = {0x06u, 0x0cu, 0x0cu, 0x06u};
    static const uint8_t left[4] = {0x06u, 0x03u, 0x03u, 0x06u};
    static const uint8_t up[4] = {0x09u, 0x0fu, 0x06u, 0x00u};
    static const uint8_t down[4] = {0x00u, 0x06u, 0x0fu, 0x09u};
    const uint8_t *rows = closed;
    if ((app->pacman_tick_count & 1u) == 0u) {
        if (app->pacman_direction == PACMAN_DIR_RIGHT) {
            rows = right;
        } else if (app->pacman_direction == PACMAN_DIR_LEFT) {
            rows = left;
        } else if (app->pacman_direction == PACMAN_DIR_UP) {
            rows = up;
        } else if (app->pacman_direction == PACMAN_DIR_DOWN) {
            rows = down;
        }
    }
    draw_pacman_cell_sprite(fb, app->pacman_player_x, app->pacman_player_y, rows);
}

static void draw_pacman_ghost(const app_t *app, framebuffer_t *fb, uint8_t ghost) {
    static const uint8_t normal[4] = {0x06u, 0x0fu, 0x0fu, 0x0au};
    static const uint8_t blink[4] = {0x06u, 0x09u, 0x0fu, 0x0au};
    bool flashing = app->pacman_power_ticks > 0u &&
                    app->pacman_power_ticks < 10u &&
                    (app->pacman_tick_count & 1u) != 0u;
    draw_pacman_cell_sprite(fb,
                            app->pacman_ghost_x[ghost],
                            app->pacman_ghost_y[ghost],
                            flashing ? blink : normal);
}

static void draw_pacman_message_text(framebuffer_t *fb, const char *a, const char *b, const char *c) {
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

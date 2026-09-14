#include "apps/snake_app.h"
#include "apps/game_common.h"

#include <stdio.h>
#include <string.h>

#include "apps/games_app.h"
#include "services/input_keys.h"
#include "storage/store_service.h"
#include "services/timebase.h"
#include "services/strings.h"

#define SNAKE_COLS 20u
#define SNAKE_ROWS 11u
#define SNAKE_MAX_CELLS (SNAKE_COLS * SNAKE_ROWS)
#define SNAKE_INITIAL_LEN 9u
#define SNAKE_INITIAL_FOOD_X 10u
#define SNAKE_INITIAL_FOOD_Y 5u
#define SNAKE_RNG_FALLBACK_SEED 1u
#define SNAKE_CRASH_SETTLE_MS 200u
#define SNAKE_RESULT_DISMISS_MS 3850u
#define SNAKE_TOP_SCORE_FRAME_MS 160u

typedef enum {
    SNAKE_MENU_NEW = 0,
    SNAKE_MENU_ACTIVE,
    SNAKE_MENU_CONTINUE,
    SNAKE_MENU_LAST_VIEW,
} snake_menu_variant_t;

typedef enum {
    SNAKE_DIR_UP = 0,
    SNAKE_DIR_RIGHT,
    SNAKE_DIR_DOWN,
    SNAKE_DIR_LEFT,
} snake_direction_t;

typedef enum {
    SNAKE_ITEM_LEVEL = 0,
    SNAKE_ITEM_CONTINUE,
    SNAKE_ITEM_LAST_VIEW,
    SNAKE_ITEM_NEW_GAME,
    SNAKE_ITEM_1_PLAYER,
    SNAKE_ITEM_2_PLAYERS,
    SNAKE_ITEM_TOP_SCORE,
    SNAKE_ITEM_INSTRUCTIONS,
} snake_item_kind_t;

typedef struct {
    snake_item_kind_t kind;
    const char *label;
} snake_menu_item_t;

static const uint8_t SNAKE_SPEED_BYTES[] = {66u, 48u, 38u, 30u, 23u, 18u, 14u, 11u, 9u};
static const uint16_t SNAKE_TOP_SCORE_FRAMES[] = {
    220u, 221u, 222u, 223u, 224u, 225u, 226u, 227u, 228u, 229u,
    229u, 230u, 230u, 229u, 229u, 230u, 230u, 229u, 230u, 230u,
};
static const char SNAKE_INSTRUCTIONS[] =
    "Make the snake grow longer by directing it to the food. Use the keys 2, 4, 6 and 8. You cannot stop the snake or make it go backwards. Try not to hit the walls or the tail.";

static void open_snake_menu_variant(app_t *app, snake_menu_variant_t variant, snake_item_kind_t selected_kind);
static void open_snake_level(app_t *app);
static void open_snake_top_score(app_t *app, uint32_t now);
static void open_snake_instructions(app_t *app);
static void start_snake_game(app_t *app, uint32_t now);
static void pause_snake_to_menu(app_t *app);
static void finish_snake_game(app_t *app, uint32_t now);
static void trigger_snake_crash(app_t *app, uint32_t now);
static bool snake_step(app_t *app, uint32_t now);
static void snake_save_storage(const app_t *app);
static uint8_t snake_menu_items(const app_t *app, snake_menu_item_t *items, uint8_t cap);
static int8_t snake_menu_index_for_kind(const app_t *app, snake_item_kind_t kind);
static uint16_t snake_tick_ms(const app_t *app);
static uint16_t snake_score_increment(const app_t *app);
static bool snake_opposite(uint8_t a, uint8_t b);
static bool snake_contains(const app_t *app, uint8_t x, uint8_t y, uint8_t start_index);
static uint16_t snake_random(app_t *app);
static void snake_place_food(app_t *app);
static uint32_t snake_seed(uint32_t now);
static uint8_t snake_instruction_lines(char lines[][32], uint8_t max_lines);
static uint8_t snake_instruction_max_scroll(void);
static void draw_snake_food(framebuffer_t *fb, uint8_t x, uint8_t y);
static void draw_snake_body(framebuffer_t *fb, const app_t *app);
static void draw_snake_message_text(framebuffer_t *fb, const char *a, const char *b, const char *c);

void snake_app_init(app_t *app) {
    uint8_t level = 1u;
    uint16_t top_score = 0u;
    store_setting_get_u8(STORE_SETTING_GAMES_SNAKE_LEVEL, &level);
    store_setting_get_u16(STORE_SETTING_GAMES_SNAKE_TOP_SCORE, &top_score);
    if (level >= ARRAY_COUNT(SNAKE_SPEED_BYTES)) {
        level = 1u;
    }
    if (top_score > 9999u) {
        top_score = 9999u;
    }
    app->snake_menu_variant = SNAKE_MENU_NEW;
    app->snake_menu_selected = 0u;
    app->snake_level_byte = level;
    app->snake_level_draft_byte = level;
    app->snake_top_score = top_score;
    app->snake_food_x = SNAKE_INITIAL_FOOD_X;
    app->snake_food_y = SNAKE_INITIAL_FOOD_Y;
    app->snake_direction = SNAKE_DIR_RIGHT;
    app->snake_pending_direction = SNAKE_DIR_RIGHT;
    app->snake_rng_seed = SNAKE_RNG_FALLBACK_SEED;
}

void open_snake_menu(app_t *app) {
    open_snake_menu_variant(app, (snake_menu_variant_t)app->snake_menu_variant, SNAKE_ITEM_NEW_GAME);
}

bool handle_snake_menu_key(app_t *app, uint16_t key, uint32_t now) {
    snake_menu_item_t items[8];
    uint8_t count = snake_menu_items(app, items, (uint8_t)ARRAY_COUNT(items));
    if (count == 0u) {
        return true;
    }
    if (app->snake_menu_selected >= count) {
        app->snake_menu_selected = 0u;
        app->game_option_view_start = 0u;
    }
    if (key == KEY_C) {
        open_games_menu(app, 1u);
        return true;
    }
    if (key == KEY_UP || key == KEY_2) {
        ui_circular_list_step_3rows(count, -1,
                                    &app->snake_menu_selected,
                                    &app->game_option_view_start);
        app->dirty = true;
        return true;
    }
    if (key == KEY_DOWN || key == KEY_8) {
        ui_circular_list_step_3rows(count, 1,
                                    &app->snake_menu_selected,
                                    &app->game_option_view_start);
        app->dirty = true;
        return true;
    }
    if (key != KEY_NAVI && key != KEY_5) {
        return true;
    }

    snake_item_kind_t kind = items[app->snake_menu_selected].kind;
    if (kind == SNAKE_ITEM_NEW_GAME || kind == SNAKE_ITEM_1_PLAYER) {
        start_snake_game(app, now);
    } else if (kind == SNAKE_ITEM_CONTINUE) {
        app->route = APP_ROUTE_SNAKE_PLAY;
        app->snake_game_over = false;
        app->snake_crash_deadline_ms = 0u;
        app->snake_next_tick_ms = now + snake_tick_ms(app);
        app->dirty = true;
    } else if (kind == SNAKE_ITEM_LAST_VIEW) {
        app->route = APP_ROUTE_SNAKE_LAST_VIEW;
        app->dirty = true;
    } else if (kind == SNAKE_ITEM_LEVEL) {
        open_snake_level(app);
    } else if (kind == SNAKE_ITEM_TOP_SCORE) {
        open_snake_top_score(app, now);
    } else if (kind == SNAKE_ITEM_INSTRUCTIONS) {
        open_snake_instructions(app);
    } else if (kind == SNAKE_ITEM_2_PLAYERS) {
        app->dirty = true;
    }
    return true;
}

bool handle_snake_play_key(app_t *app, uint16_t key, uint32_t now) {
    if (app->route == APP_ROUTE_SNAKE_LAST_VIEW) {
        if (key == KEY_C || key == KEY_NAVI || key == KEY_5) {
            open_snake_menu_variant(app, SNAKE_MENU_LAST_VIEW, SNAKE_ITEM_LAST_VIEW);
        }
        return true;
    }
    if (app->snake_game_over) {
        return true;
    }
    uint8_t direction = 0xffu;
    if (key == KEY_UP || key == KEY_2) {
        direction = SNAKE_DIR_UP;
    } else if (key == KEY_DOWN || key == KEY_8) {
        direction = SNAKE_DIR_DOWN;
    } else if (key == KEY_4) {
        direction = SNAKE_DIR_LEFT;
    } else if (key == KEY_6) {
        direction = SNAKE_DIR_RIGHT;
    } else if (key == KEY_C || key == KEY_NAVI) {
        pause_snake_to_menu(app);
        return true;
    }
    if (direction != 0xffu && !snake_opposite(direction, app->snake_direction)) {
        app->snake_pending_direction = direction;
        app->dirty = true;
    }
    (void)now;
    return true;
}

bool handle_snake_level_key(app_t *app, uint16_t key, uint32_t now) {
    if (key == KEY_UP || key == KEY_2) {
        adjust_game_level(&app->snake_level_draft_byte,
                          (uint8_t)ARRAY_COUNT(SNAKE_SPEED_BYTES), 1);
        app->dirty = true;
        return true;
    }
    if (key == KEY_DOWN || key == KEY_8) {
        adjust_game_level(&app->snake_level_draft_byte,
                          (uint8_t)ARRAY_COUNT(SNAKE_SPEED_BYTES), -1);
        app->dirty = true;
        return true;
    }
    if (key == KEY_NAVI || key == KEY_5) {
        app->snake_level_byte = app->snake_level_draft_byte;
        app->snake_menu_variant = SNAKE_MENU_ACTIVE;
        snake_save_storage(app);
        open_snake_menu_variant(app, SNAKE_MENU_ACTIVE, SNAKE_ITEM_LEVEL);
        return true;
    }
    if (key == KEY_C) {
        app->snake_level_draft_byte = app->snake_level_byte;
        open_snake_menu_variant(app, (snake_menu_variant_t)app->snake_menu_variant, SNAKE_ITEM_LEVEL);
        return true;
    }
    (void)now;
    return true;
}

bool handle_snake_instructions_key(app_t *app, uint16_t key, uint32_t now) {
    uint8_t max_scroll = snake_instruction_max_scroll();
    if ((key == KEY_UP || key == KEY_2) && app->snake_instructions_scroll > 0u) {
        app->snake_instructions_scroll--;
        app->dirty = true;
    } else if ((key == KEY_DOWN || key == KEY_8) && app->snake_instructions_scroll < max_scroll) {
        app->snake_instructions_scroll++;
        app->dirty = true;
    } else if (key == KEY_C || key == KEY_NAVI || key == KEY_5) {
        open_snake_menu_variant(app, (snake_menu_variant_t)app->snake_menu_variant, SNAKE_ITEM_INSTRUCTIONS);
    }
    (void)now;
    return true;
}

bool handle_snake_message_key(app_t *app, uint16_t key, uint32_t now) {
    if (key == KEY_C || key == KEY_NAVI || key == KEY_5) {
        snake_item_kind_t item = app->route == APP_ROUTE_SNAKE_TOP_SCORE ? SNAKE_ITEM_TOP_SCORE : SNAKE_ITEM_LAST_VIEW;
        snake_menu_variant_t variant = app->route == APP_ROUTE_SNAKE_RESULT ? SNAKE_MENU_LAST_VIEW :
            (snake_menu_variant_t)app->snake_menu_variant;
        open_snake_menu_variant(app, variant, item);
        return true;
    }
    (void)now;
    return true;
}

bool tick_snake(app_t *app, uint32_t now) {
    bool changed = false;
    if (app->route == APP_ROUTE_SNAKE_PLAY) {
        if (app->snake_crash_deadline_ms != 0u &&
            time_diff_ms(now, app->snake_crash_deadline_ms) >= 0) {
            finish_snake_game(app, now);
            changed = true;
        } else if (!app->snake_game_over && app->snake_next_tick_ms != 0u &&
                   time_diff_ms(now, app->snake_next_tick_ms) >= 0) {
            changed = snake_step(app, now) || changed;
        }
    }
    if (app->route == APP_ROUTE_SNAKE_RESULT &&
        app->snake_result_deadline_ms != 0u &&
        time_diff_ms(now, app->snake_result_deadline_ms) >= 0) {
        open_snake_menu_variant(app, SNAKE_MENU_LAST_VIEW, SNAKE_ITEM_LAST_VIEW);
        changed = true;
    }
    if (app->route == APP_ROUTE_SNAKE_TOP_SCORE &&
        time_diff_ms(now, app->snake_top_score_last_frame_ms + SNAKE_TOP_SCORE_FRAME_MS) >= 0) {
        app->snake_top_score_last_frame_ms = now;
        if (app->snake_top_score_frame + 1u < ARRAY_COUNT(SNAKE_TOP_SCORE_FRAMES)) {
            app->snake_top_score_frame++;
            changed = true;
        }
    }
    return changed;
}

void render_snake_menu(const app_t *app, framebuffer_t *fb) {
    snake_menu_item_t items[8];
    const char *labels[8];
    uint8_t count = snake_menu_items(app, items, (uint8_t)ARRAY_COUNT(items));
    for (uint8_t i = 0; i < count; i++) {
        labels[i] = items[i].label;
    }
    char crumb[10];
    draw_flat_list_circular_view(fb, labels, count,
                                 app->snake_menu_selected,
                                 app->game_option_view_start,
                                 ui_breadcrumb_path(crumb, sizeof(crumb), "6-2", (unsigned)(app->snake_menu_selected + 1u)),
                                 "Select");
}

void render_snake_play(const app_t *app, framebuffer_t *fb) {
    fb_clear(fb, false);
    fb_rect(fb, 0, 0, 83, 47, true);
    draw_snake_food(fb, app->snake_food_x, app->snake_food_y);
    draw_snake_body(fb, app);
}

void render_snake_level(const app_t *app, framebuffer_t *fb) {
    draw_game_level_selector(fb, app->snake_level_draft_byte,
                             (uint8_t)ARRAY_COUNT(SNAKE_SPEED_BYTES));
}

void render_snake_instructions(const app_t *app, framebuffer_t *fb) {
    fb_clear(fb, false);
    char lines[18][32];
    uint8_t count = snake_instruction_lines(lines, (uint8_t)ARRAY_COUNT(lines));
    uint8_t start = app->snake_instructions_scroll;
    if (start > count) {
        start = 0u;
    }
    const font_t *font = asset_font(FONT_FS1);
    for (uint8_t row = 0; row < 3u && start + row < count; row++) {
        fb_text(fb, font, lines[start + row], 0, 7 + row * 9, true, FB_WIDTH);
    }
    draw_softkey(fb, "Back");
}

void render_snake_top_score(const app_t *app, framebuffer_t *fb) {
    fb_clear(fb, false);
    char score[8];
    snprintf(score, sizeof(score), "%u", (unsigned)app->snake_top_score);
    draw_snake_message_text(fb, "Top score:", score, 0);
    uint8_t frame = app->snake_top_score_frame;
    if (frame >= ARRAY_COUNT(SNAKE_TOP_SCORE_FRAMES)) {
        frame = (uint8_t)(ARRAY_COUNT(SNAKE_TOP_SCORE_FRAMES) - 1u);
    }
    fb_bitmap(fb, SNAKE_TOP_SCORE_FRAMES[frame], 63, 0, true, true);
}

void render_snake_result(const app_t *app, framebuffer_t *fb) {
    fb_clear(fb, false);
    char score[8];
    snprintf(score, sizeof(score), "%u", (unsigned)app->snake_score);
    draw_snake_message_text(fb,
                            ts_or(0x157u, "Game over!"),
                            app->snake_result_top_score ? "TOP SCORE:" : "Your score:",
                            score);
}

static void open_snake_menu_variant(app_t *app, snake_menu_variant_t variant, snake_item_kind_t selected_kind) {
    app->route = APP_ROUTE_SNAKE_MENU;
    app->snake_menu_variant = (uint8_t)variant;
    int8_t selected = snake_menu_index_for_kind(app, selected_kind);
    app->snake_menu_selected = selected >= 0 ? (uint8_t)selected : 0u;
    app->game_option_view_start = app->snake_menu_selected;
    app->dirty = true;
}

static void open_snake_level(app_t *app) {
    app->route = APP_ROUTE_SNAKE_LEVEL;
    app->snake_level_draft_byte = app->snake_level_byte;
    app->dirty = true;
}

static void open_snake_top_score(app_t *app, uint32_t now) {
    app->route = APP_ROUTE_SNAKE_TOP_SCORE;
    app->snake_top_score_frame = 0u;
    app->snake_top_score_last_frame_ms = now;
    app->dirty = true;
}

static void open_snake_instructions(app_t *app) {
    app->route = APP_ROUTE_SNAKE_INSTRUCTIONS;
    app->snake_instructions_scroll = 0u;
    app->dirty = true;
}

static void start_snake_game(app_t *app, uint32_t now) {
    app->route = APP_ROUTE_SNAKE_PLAY;
    app->snake_menu_variant = SNAKE_MENU_ACTIVE;
    app->snake_score = 0u;
    app->snake_body_len = SNAKE_INITIAL_LEN;
    for (uint8_t i = 0; i < SNAKE_INITIAL_LEN; i++) {
        app->snake_body_x[i] = i;
        app->snake_body_y[i] = SNAKE_ROWS - 1u;
    }
    app->snake_food_x = SNAKE_INITIAL_FOOD_X;
    app->snake_food_y = SNAKE_INITIAL_FOOD_Y;
    app->snake_direction = SNAKE_DIR_RIGHT;
    app->snake_pending_direction = SNAKE_DIR_RIGHT;
    app->snake_rng_seed = snake_seed(now);
    app->snake_game_over = false;
    app->snake_result_top_score = false;
    app->snake_crash_deadline_ms = 0u;
    app->snake_result_deadline_ms = 0u;
    app->snake_next_tick_ms = now + snake_tick_ms(app);
    app->dirty = true;
}

static void pause_snake_to_menu(app_t *app) {
    app->snake_next_tick_ms = 0u;
    open_snake_menu_variant(app, SNAKE_MENU_CONTINUE, SNAKE_ITEM_CONTINUE);
}

static void finish_snake_game(app_t *app, uint32_t now) {
    app->route = APP_ROUTE_SNAKE_RESULT;
    app->snake_crash_deadline_ms = 0u;
    app->snake_result_deadline_ms = now + SNAKE_RESULT_DISMISS_MS;
    app->snake_menu_variant = SNAKE_MENU_LAST_VIEW;
    app->snake_result_top_score = app->snake_score > app->snake_top_score;
    if (app->snake_result_top_score) {
        app->snake_top_score = app->snake_score;
        snake_save_storage(app);
    }
    play_game_system_tone(app->snake_result_top_score ? 20u : 17u);
    app->dirty = true;
}

static void trigger_snake_crash(app_t *app, uint32_t now) {
    /* Leave the collision quiet during the settle; the result screen owns the
     * game-over melody so audio and the dialog begin together. */
    app->snake_game_over = true;
    app->snake_next_tick_ms = 0u;
    app->snake_crash_deadline_ms = now + SNAKE_CRASH_SETTLE_MS;
    app->dirty = true;
}

static bool snake_step(app_t *app, uint32_t now) {
    if (app->snake_body_len == 0u) {
        start_snake_game(app, now);
        return true;
    }
    app->snake_direction = app->snake_pending_direction;
    int8_t dx = 0;
    int8_t dy = 0;
    if (app->snake_direction == SNAKE_DIR_UP) {
        dy = -1;
    } else if (app->snake_direction == SNAKE_DIR_DOWN) {
        dy = 1;
    } else if (app->snake_direction == SNAKE_DIR_LEFT) {
        dx = -1;
    } else {
        dx = 1;
    }
    int16_t next_x = (int16_t)app->snake_body_x[app->snake_body_len - 1u] + dx;
    int16_t next_y = (int16_t)app->snake_body_y[app->snake_body_len - 1u] + dy;
    bool ate = next_x == app->snake_food_x && next_y == app->snake_food_y;
    uint8_t collision_start = ate ? 0u : 1u;
    if (next_x < 0 || next_x >= (int16_t)SNAKE_COLS || next_y < 0 || next_y >= (int16_t)SNAKE_ROWS ||
        snake_contains(app, (uint8_t)next_x, (uint8_t)next_y, collision_start)) {
        trigger_snake_crash(app, now);
        return true;
    }
    if (!ate) {
        for (uint8_t i = 1u; i < app->snake_body_len; i++) {
            app->snake_body_x[i - 1u] = app->snake_body_x[i];
            app->snake_body_y[i - 1u] = app->snake_body_y[i];
        }
    } else if (app->snake_body_len < SNAKE_MAX_CELLS) {
        app->snake_body_len++;
    }
    app->snake_body_x[app->snake_body_len - 1u] = (uint8_t)next_x;
    app->snake_body_y[app->snake_body_len - 1u] = (uint8_t)next_y;
    if (ate) {
        uint16_t score = (uint16_t)(app->snake_score + snake_score_increment(app));
        app->snake_score = score > 9999u ? 9999u : score;
        snake_place_food(app);
        play_game_system_tone(16u);
    }
    app->snake_next_tick_ms = now + snake_tick_ms(app);
    app->dirty = true;
    return true;
}

static void snake_save_storage(const app_t *app) {
    store_setting_set_u8(STORE_SETTING_GAMES_SNAKE_LEVEL, app->snake_level_byte);
    store_setting_set_u16(STORE_SETTING_GAMES_SNAKE_TOP_SCORE, app->snake_top_score);
}

static uint8_t snake_menu_items(const app_t *app, snake_menu_item_t *items, uint8_t cap) {
    uint8_t count = 0u;
#define ADD_ITEM(k, text) do { \
    if (count < cap) { \
        items[count].kind = (k); \
        items[count].label = (text); \
        count++; \
    } \
} while (0)
    /* Labels are render-only (menu dispatch is by .kind), so localize here. */
    ADD_ITEM(SNAKE_ITEM_LEVEL, ts_or(0x164u, "Level"));
    if (app->snake_menu_variant == SNAKE_MENU_CONTINUE) {
        ADD_ITEM(SNAKE_ITEM_CONTINUE, ts_or(0x160u, "Continue"));
    }
    if (app->snake_menu_variant == SNAKE_MENU_LAST_VIEW) {
        ADD_ITEM(SNAKE_ITEM_LAST_VIEW, ts_or(0x163u, "Last view"));
    }
    ADD_ITEM(SNAKE_ITEM_NEW_GAME, ts_or(0x165u, "New game"));
    ADD_ITEM(SNAKE_ITEM_TOP_SCORE, ts_or(0x161u, "Top score"));
    ADD_ITEM(SNAKE_ITEM_INSTRUCTIONS, ts_or(0x162u, "Instructions"));
#undef ADD_ITEM
    return count;
}

static int8_t snake_menu_index_for_kind(const app_t *app, snake_item_kind_t kind) {
    snake_menu_item_t items[8];
    uint8_t count = snake_menu_items(app, items, (uint8_t)ARRAY_COUNT(items));
    for (uint8_t i = 0; i < count; i++) {
        if (items[i].kind == kind) {
            return (int8_t)i;
        }
    }
    return -1;
}

static uint16_t snake_tick_ms(const app_t *app) {
    uint8_t index = app->snake_level_byte;
    if (index >= ARRAY_COUNT(SNAKE_SPEED_BYTES)) {
        index = 1u;
    }
    return (uint16_t)SNAKE_SPEED_BYTES[index] * 10u;
}

static uint16_t snake_score_increment(const app_t *app) {
    uint8_t index = app->snake_level_byte;
    if (index >= ARRAY_COUNT(SNAKE_SPEED_BYTES)) {
        index = 1u;
    }
    return (uint16_t)index + 1u;
}

static bool snake_opposite(uint8_t a, uint8_t b) {
    return (a == SNAKE_DIR_UP && b == SNAKE_DIR_DOWN) ||
           (a == SNAKE_DIR_DOWN && b == SNAKE_DIR_UP) ||
           (a == SNAKE_DIR_LEFT && b == SNAKE_DIR_RIGHT) ||
           (a == SNAKE_DIR_RIGHT && b == SNAKE_DIR_LEFT);
}

static bool snake_contains(const app_t *app, uint8_t x, uint8_t y, uint8_t start_index) {
    for (uint8_t i = start_index; i < app->snake_body_len; i++) {
        if (app->snake_body_x[i] == x && app->snake_body_y[i] == y) {
            return true;
        }
    }
    return false;
}

static uint16_t snake_random(app_t *app) {
    app->snake_rng_seed = app->snake_rng_seed * 1103515245u + 12345u;
    return (uint16_t)((app->snake_rng_seed >> 16) & 0x7fffu);
}

static void snake_place_food(app_t *app) {
    for (uint8_t attempt = 0u; attempt < 32u; attempt++) {
        uint8_t x = (uint8_t)(snake_random(app) % SNAKE_COLS);
        uint8_t y = (uint8_t)(snake_random(app) % SNAKE_ROWS);
        if (!snake_contains(app, x, y, 0u)) {
            app->snake_food_x = x;
            app->snake_food_y = y;
            return;
        }
    }
    for (uint8_t y = 0u; y < SNAKE_ROWS; y++) {
        for (uint8_t x = 0u; x < SNAKE_COLS; x++) {
            if (!snake_contains(app, x, y, 0u)) {
                app->snake_food_x = x;
                app->snake_food_y = y;
                return;
            }
        }
    }
}

static uint32_t snake_seed(uint32_t now) {
    uint32_t seed = now ^ (time_ticks8() * 1103515245u) ^ 0x29afe8u;
    return seed == 0u ? SNAKE_RNG_FALLBACK_SEED : seed;
}

static uint8_t snake_instruction_lines(char lines[][32], uint8_t max_lines) {
    return wrap_text_lines_ex(asset_font(FONT_FS1), ts_or(0x40eu, SNAKE_INSTRUCTIONS), FB_WIDTH, (char *)lines, 32u, max_lines);
}

static uint8_t snake_instruction_max_scroll(void) {
    char lines[18][32];
    uint8_t count = snake_instruction_lines(lines, (uint8_t)ARRAY_COUNT(lines));
    return count > 3u ? (uint8_t)(count - 3u) : 0u;
}

static void draw_snake_food(framebuffer_t *fb, uint8_t x, uint8_t y) {
    int px = 2 + (int)x * 4;
    int py = 2 + (int)y * 4;
    fb_pixel(fb, px + 1, py, true);
    fb_pixel(fb, px, py + 1, true);
    fb_pixel(fb, px + 2, py + 1, true);
    fb_pixel(fb, px + 1, py + 2, true);
}

static void draw_snake_body(framebuffer_t *fb, const app_t *app) {
    for (uint8_t i = 0; i < app->snake_body_len; i++) {
        int x = 2 + (int)app->snake_body_x[i] * 4;
        int y = 2 + (int)app->snake_body_y[i] * 4;
        fb_fill_rect(fb, x, y, 3, 3, true);
    }
    for (uint8_t i = 0; i + 1u < app->snake_body_len; i++) {
        uint8_t ax = app->snake_body_x[i];
        uint8_t ay = app->snake_body_y[i];
        uint8_t bx = app->snake_body_x[i + 1u];
        uint8_t by = app->snake_body_y[i + 1u];
        int a_px = 2 + (int)ax * 4;
        int a_py = 2 + (int)ay * 4;
        int b_px = 2 + (int)bx * 4;
        int b_py = 2 + (int)by * 4;
        if (ay == by && ((ax + 1u == bx) || (bx + 1u == ax))) {
            int x = a_px < b_px ? a_px + 3 : b_px + 3;
            fb_fill_rect(fb, x, a_py, 1, 3, true);
        } else if (ax == bx && ((ay + 1u == by) || (by + 1u == ay))) {
            int y = a_py < b_py ? a_py + 3 : b_py + 3;
            fb_fill_rect(fb, a_px, y, 3, 1, true);
        }
    }
}

static void draw_snake_message_text(framebuffer_t *fb, const char *a, const char *b, const char *c) {
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

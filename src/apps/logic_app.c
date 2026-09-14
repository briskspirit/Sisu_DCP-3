#include "apps/logic_app.h"
#include "apps/game_common.h"

#include <stdio.h>
#include <string.h>

#include "apps/games_app.h"
#include "apps/memory_app.h"
#include "services/input_keys.h"
#include "storage/store_service.h"
#include "services/strings.h"
#include "services/timebase.h"

#define LOGIC_MAX_LEVELS 8u
#define LOGIC_MAX_ATTEMPTS 10u
#define LOGIC_MAX_PEGS 5u
#define LOGIC_MAX_FIGURES 10u
#define LOGIC_TILE_SIZE 7u
#define LOGIC_TILE_STEP 8u
#define LOGIC_RESULT_DISMISS_MS 3850u
#define LOGIC_RNG_FALLBACK_SEED 1u

typedef enum {
    LOGIC_MENU_NEW = 0,
    LOGIC_MENU_ACTIVE,
    LOGIC_MENU_CONTINUE,
    LOGIC_MENU_LAST_VIEW,
} logic_menu_variant_t;

typedef enum {
    LOGIC_ITEM_LEVEL = 0,
    LOGIC_ITEM_CONTINUE,
    LOGIC_ITEM_LAST_VIEW,
    LOGIC_ITEM_NEW_GAME,
    LOGIC_ITEM_INSTRUCTIONS,
} logic_item_kind_t;

typedef struct {
    logic_item_kind_t kind;
    const char *label;
} logic_menu_item_t;

typedef struct {
    uint8_t length;
    uint8_t figures;
} logic_level_t;

static const char LOGIC_INSTRUCTIONS[] =
    "Try to guess the right combination of figures. Move the cursor with keys 2, 4 and 8, select a figure with key 5. Check your guess with *. A right figure in the right place is marked with a rectangle, a right figure in a wrong place with a line.";

static void open_logic_menu_variant(app_t *app, logic_menu_variant_t variant, logic_item_kind_t selected_kind);
static void open_logic_level(app_t *app);
static void open_logic_instructions(app_t *app);
static void start_logic_game(app_t *app, uint32_t now);
static void pause_logic_to_menu(app_t *app);
static void finish_logic_solved(app_t *app, uint8_t attempt, uint32_t now);
static void logic_move_peg(app_t *app, int8_t delta);
static void logic_copy_previous(app_t *app);
static void logic_cycle_figure(app_t *app);
static void logic_check_guess(app_t *app, uint32_t now);
static uint8_t logic_score_guess(const uint8_t *guess, const uint8_t *solution, uint8_t length);
static void logic_generate_sprite_pool(app_t *app, uint8_t count);
static uint16_t logic_random(app_t *app);
static uint32_t logic_seed(uint32_t now);
static void logic_save_storage(const app_t *app);
static logic_level_t logic_level_record(uint8_t level_byte);
static uint8_t logic_row_index(uint8_t attempt, uint8_t peg);
static uint8_t logic_menu_items(const app_t *app, logic_menu_item_t *items, uint8_t cap);
static int8_t logic_menu_index_for_kind(const app_t *app, logic_item_kind_t kind);
static uint8_t logic_instruction_lines(char lines[][32], uint8_t max_lines);
static uint8_t logic_instruction_max_scroll(void);
static void draw_logic_board(const app_t *app, framebuffer_t *fb);
static void draw_logic_sprite(framebuffer_t *fb, uint8_t code, int x, int y, bool selected);
static void draw_logic_clue(framebuffer_t *fb, int x, uint8_t clue);
static void draw_logic_message_text(framebuffer_t *fb, const char *a, const char *b);

void logic_app_init(app_t *app) {
    uint8_t level = 0u;
    store_setting_get_u8(STORE_SETTING_GAMES_LOGIC_LEVEL, &level);
    if (level >= LOGIC_MAX_LEVELS) {
        level = 0u;
    }
    app->logic_menu_variant = LOGIC_MENU_NEW;
    app->logic_menu_selected = 0u;
    app->logic_level_byte = level;
    app->logic_level_draft_byte = level;
}

void open_logic_menu(app_t *app) {
    open_logic_menu_variant(app, (logic_menu_variant_t)app->logic_menu_variant, LOGIC_ITEM_NEW_GAME);
}

bool handle_logic_menu_key(app_t *app, uint16_t key, uint32_t now) {
    logic_menu_item_t items[5];
    uint8_t count = logic_menu_items(app, items, (uint8_t)ARRAY_COUNT(items));
    if (count == 0u) {
        return true;
    }
    if (app->logic_menu_selected >= count) {
        app->logic_menu_selected = 0u;
        app->game_option_view_start = 0u;
    }
    if (key == KEY_C) {
        open_games_menu(app, 4u);
        return true;
    }
    if (key == KEY_UP || key == KEY_2) {
        ui_circular_list_step_3rows(count, -1,
                                    &app->logic_menu_selected,
                                    &app->game_option_view_start);
        app->dirty = true;
        return true;
    }
    if (key == KEY_DOWN || key == KEY_8) {
        ui_circular_list_step_3rows(count, 1,
                                    &app->logic_menu_selected,
                                    &app->game_option_view_start);
        app->dirty = true;
        return true;
    }
    if (key != KEY_NAVI && key != KEY_5) {
        return true;
    }
    logic_item_kind_t kind = items[app->logic_menu_selected].kind;
    if (kind == LOGIC_ITEM_NEW_GAME) {
        start_logic_game(app, now);
    } else if (kind == LOGIC_ITEM_CONTINUE) {
        app->route = APP_ROUTE_LOGIC_PLAY;
        app->logic_game_over = false;
        app->dirty = true;
    } else if (kind == LOGIC_ITEM_LAST_VIEW) {
        app->route = APP_ROUTE_LOGIC_LAST_VIEW;
        app->dirty = true;
    } else if (kind == LOGIC_ITEM_LEVEL) {
        open_logic_level(app);
    } else if (kind == LOGIC_ITEM_INSTRUCTIONS) {
        open_logic_instructions(app);
    }
    return true;
}

bool handle_logic_play_key(app_t *app, uint16_t key, uint32_t now) {
    if (app->route == APP_ROUTE_LOGIC_LAST_VIEW) {
        if (key == KEY_C || key == KEY_NAVI || key == KEY_5) {
            open_logic_menu_variant(app, LOGIC_MENU_LAST_VIEW, LOGIC_ITEM_LAST_VIEW);
        }
        return true;
    }
    if (key == KEY_C || key == KEY_NAVI) {
        pause_logic_to_menu(app);
        return true;
    }
    if (app->logic_game_over) {
        return true;
    }
    if (key == KEY_UP || key == KEY_2) {
        logic_move_peg(app, -1);
    } else if (key == KEY_DOWN || key == KEY_8) {
        logic_move_peg(app, 1);
    } else if (key == KEY_4) {
        logic_copy_previous(app);
    } else if (key == KEY_5) {
        logic_cycle_figure(app);
    } else if (key == KEY_STAR) {
        logic_check_guess(app, now);
    }
    return true;
}

bool handle_logic_level_key(app_t *app, uint16_t key, uint32_t now) {
    if (key == KEY_UP || key == KEY_2) {
        adjust_game_level(&app->logic_level_draft_byte, LOGIC_MAX_LEVELS, 1);
        app->dirty = true;
        return true;
    }
    if (key == KEY_DOWN || key == KEY_8) {
        adjust_game_level(&app->logic_level_draft_byte, LOGIC_MAX_LEVELS, -1);
        app->dirty = true;
        return true;
    }
    if (key == KEY_NAVI || key == KEY_5) {
        app->logic_level_byte = app->logic_level_draft_byte;
        app->logic_menu_variant = LOGIC_MENU_ACTIVE;
        logic_save_storage(app);
        open_logic_menu_variant(app, LOGIC_MENU_ACTIVE, LOGIC_ITEM_LEVEL);
        return true;
    }
    if (key == KEY_C) {
        app->logic_level_draft_byte = app->logic_level_byte;
        open_logic_menu_variant(app, (logic_menu_variant_t)app->logic_menu_variant, LOGIC_ITEM_LEVEL);
        return true;
    }
    (void)now;
    return true;
}

bool handle_logic_instructions_key(app_t *app, uint16_t key, uint32_t now) {
    uint8_t max_scroll = logic_instruction_max_scroll();
    if ((key == KEY_UP || key == KEY_2) && app->logic_instructions_scroll > 0u) {
        app->logic_instructions_scroll--;
        app->dirty = true;
    } else if ((key == KEY_DOWN || key == KEY_8) && app->logic_instructions_scroll < max_scroll) {
        app->logic_instructions_scroll++;
        app->dirty = true;
    } else if (key == KEY_C || key == KEY_NAVI || key == KEY_5) {
        open_logic_menu_variant(app, (logic_menu_variant_t)app->logic_menu_variant, LOGIC_ITEM_INSTRUCTIONS);
    }
    (void)now;
    return true;
}

bool handle_logic_message_key(app_t *app, uint16_t key, uint32_t now) {
    if (key == KEY_C || key == KEY_NAVI || key == KEY_5) {
        open_logic_menu_variant(app, LOGIC_MENU_LAST_VIEW, LOGIC_ITEM_LAST_VIEW);
    }
    (void)now;
    return true;
}

bool tick_logic(app_t *app, uint32_t now) {
    if (app->route == APP_ROUTE_LOGIC_RESULT &&
        app->logic_result_deadline_ms != 0u &&
        time_diff_ms(now, app->logic_result_deadline_ms) >= 0) {
        open_logic_menu_variant(app, LOGIC_MENU_LAST_VIEW, LOGIC_ITEM_LAST_VIEW);
        return true;
    }
    return false;
}

void render_logic_menu(const app_t *app, framebuffer_t *fb) {
    logic_menu_item_t items[5];
    const char *labels[5];
    uint8_t count = logic_menu_items(app, items, (uint8_t)ARRAY_COUNT(items));
    for (uint8_t i = 0u; i < count; i++) {
        labels[i] = items[i].label;
    }
    char crumb[10];
    draw_flat_list_circular_view(fb, labels, count,
                                 app->logic_menu_selected,
                                 app->game_option_view_start,
                                 ui_breadcrumb_path(crumb, sizeof(crumb), "6-5", (unsigned)(app->logic_menu_selected + 1u)),
                                 "Select");
}

void render_logic_play(const app_t *app, framebuffer_t *fb) {
    draw_logic_board(app, fb);
}

void render_logic_level(const app_t *app, framebuffer_t *fb) {
    draw_game_level_selector(fb, app->logic_level_draft_byte, LOGIC_MAX_LEVELS);
}

void render_logic_instructions(const app_t *app, framebuffer_t *fb) {
    fb_clear(fb, false);
    char lines[24][32];
    uint8_t count = logic_instruction_lines(lines, (uint8_t)ARRAY_COUNT(lines));
    uint8_t start = app->logic_instructions_scroll;
    if (start > count) {
        start = 0u;
    }
    const font_t *font = asset_font(FONT_FS1);
    for (uint8_t row = 0u; row < 3u && start + row < count; row++) {
        fb_text(fb, font, lines[start + row], 0, 7 + row * 9, true, FB_WIDTH);
    }
    draw_softkey(fb, "Back");
}

void render_logic_result(const app_t *app, framebuffer_t *fb) {
    fb_clear(fb, false);
    /* First line via games-block SID 0x157 (as in rotation/react/snake/pacman);
     * "YOU WON!" has no standalone v6.00 record -> kept English (pacman does the
     * same for its dynamic second line). */
    draw_logic_message_text(fb, ts_or(0x157u, "Game over!"), "YOU WON!");
}

static void open_logic_menu_variant(app_t *app, logic_menu_variant_t variant, logic_item_kind_t selected_kind) {
    app->route = APP_ROUTE_LOGIC_MENU;
    app->logic_menu_variant = (uint8_t)variant;
    int8_t selected = logic_menu_index_for_kind(app, selected_kind);
    app->logic_menu_selected = selected >= 0 ? (uint8_t)selected : 0u;
    app->game_option_view_start = app->logic_menu_selected;
    app->dirty = true;
}

static void open_logic_level(app_t *app) {
    app->route = APP_ROUTE_LOGIC_LEVEL;
    app->logic_level_draft_byte = app->logic_level_byte;
    app->dirty = true;
}

static void open_logic_instructions(app_t *app) {
    app->route = APP_ROUTE_LOGIC_INSTRUCTIONS;
    app->logic_instructions_scroll = 0u;
    app->dirty = true;
}

static void start_logic_game(app_t *app, uint32_t now) {
    logic_level_t level = logic_level_record(app->logic_level_byte);
    memset(app->logic_rows, 0, sizeof(app->logic_rows));
    memset(app->logic_clues, 0, sizeof(app->logic_clues));
    memset(app->logic_solution, 0, sizeof(app->logic_solution));
    memset(app->logic_sprite_pool, 0, sizeof(app->logic_sprite_pool));
    app->route = APP_ROUTE_LOGIC_PLAY;
    app->logic_menu_variant = LOGIC_MENU_ACTIVE;
    app->logic_checked_count = 0u;
    app->logic_attempt_index = 0u;
    app->logic_peg_index = 0u;
    app->logic_game_over = false;
    app->logic_result_deadline_ms = 0u;
    app->logic_rng_seed = logic_seed(now);
    for (uint8_t i = 0u; i < level.length; i++) {
        app->logic_solution[i] = (uint8_t)((logic_random(app) % level.figures) + 1u);
    }
    logic_generate_sprite_pool(app, level.figures);
    app->dirty = true;
}

static void pause_logic_to_menu(app_t *app) {
    open_logic_menu_variant(app, LOGIC_MENU_CONTINUE, LOGIC_ITEM_CONTINUE);
}

static void finish_logic_solved(app_t *app, uint8_t attempt, uint32_t now) {
    app->route = APP_ROUTE_LOGIC_RESULT;
    app->logic_game_over = true;
    app->logic_checked_count = attempt < LOGIC_MAX_ATTEMPTS ? attempt : 0u;
    app->logic_result_deadline_ms = now + LOGIC_RESULT_DISMISS_MS;
    app->logic_menu_variant = LOGIC_MENU_LAST_VIEW;
    /* Shared result helper 0x0029a2a0 plays tone 0x13 on Logic "YOU WON". */
    play_game_system_tone(19u);
    app->dirty = true;
}

static void logic_move_peg(app_t *app, int8_t delta) {
    logic_level_t level = logic_level_record(app->logic_level_byte);
    int16_t peg = (int16_t)app->logic_peg_index + delta;
    while (peg < 0) {
        peg += level.length;
    }
    app->logic_peg_index = (uint8_t)(peg % level.length);
    app->dirty = true;
}

static void logic_copy_previous(app_t *app) {
    logic_level_t level = logic_level_record(app->logic_level_byte);
    if (app->logic_attempt_index == 0u) {
        app->dirty = true;
        return;
    }
    uint8_t peg = app->logic_peg_index;
    if (peg >= level.length) {
        peg = 0u;
    }
    app->logic_rows[logic_row_index(app->logic_attempt_index, peg)] =
        app->logic_rows[logic_row_index((uint8_t)(app->logic_attempt_index - 1u), peg)];
    app->dirty = true;
}

static void logic_cycle_figure(app_t *app) {
    logic_level_t level = logic_level_record(app->logic_level_byte);
    uint8_t peg = app->logic_peg_index;
    if (peg >= level.length) {
        peg = 0u;
    }
    uint8_t *value = &app->logic_rows[logic_row_index(app->logic_attempt_index, peg)];
    *value = (uint8_t)((*value + 1u) % (level.figures + 1u));
    app->dirty = true;
}

static void logic_check_guess(app_t *app, uint32_t now) {
    logic_level_t level = logic_level_record(app->logic_level_byte);
    uint8_t attempt = app->logic_attempt_index;
    if (attempt >= LOGIC_MAX_ATTEMPTS) {
        attempt = 0u;
    }
    const uint8_t *row = &app->logic_rows[logic_row_index(attempt, 0u)];
    uint8_t clue = logic_score_guess(row, app->logic_solution, level.length);
    play_game_system_tone(16u);
    app->logic_clues[attempt] = clue;
    if (app->logic_checked_count < attempt + 1u) {
        app->logic_checked_count = (uint8_t)(attempt + 1u);
    }
    if (clue == 0xffu) {
        finish_logic_solved(app, attempt, now);
        return;
    }
    if (attempt < LOGIC_MAX_ATTEMPTS - 1u) {
        app->logic_attempt_index = (uint8_t)(attempt + 1u);
    } else {
        memmove(app->logic_rows,
                &app->logic_rows[LOGIC_MAX_PEGS],
                (LOGIC_MAX_ATTEMPTS - 1u) * LOGIC_MAX_PEGS);
        memset(&app->logic_rows[(LOGIC_MAX_ATTEMPTS - 1u) * LOGIC_MAX_PEGS], 0, LOGIC_MAX_PEGS);
        memmove(app->logic_clues, &app->logic_clues[1], LOGIC_MAX_ATTEMPTS - 1u);
        app->logic_clues[LOGIC_MAX_ATTEMPTS - 1u] = clue;
        app->logic_attempt_index = LOGIC_MAX_ATTEMPTS - 1u;
        app->logic_checked_count = LOGIC_MAX_ATTEMPTS - 1u;
    }
    app->logic_peg_index = 0u;
    app->dirty = true;
}

static uint8_t logic_score_guess(const uint8_t *guess, const uint8_t *solution, uint8_t length) {
    uint8_t exact = 0u;
    uint8_t misplaced = 0u;
    uint8_t guess_counts[LOGIC_MAX_FIGURES + 1u];
    uint8_t solution_counts[LOGIC_MAX_FIGURES + 1u];
    memset(guess_counts, 0, sizeof(guess_counts));
    memset(solution_counts, 0, sizeof(solution_counts));
    for (uint8_t i = 0u; i < length; i++) {
        uint8_t g = guess[i];
        uint8_t s = solution[i];
        if (g == s) {
            exact++;
        } else {
            if (g > 0u && g <= LOGIC_MAX_FIGURES) {
                guess_counts[g]++;
            }
            if (s > 0u && s <= LOGIC_MAX_FIGURES) {
                solution_counts[s]++;
            }
        }
    }
    if (exact >= length) {
        return 0xffu;
    }
    for (uint8_t i = 1u; i <= LOGIC_MAX_FIGURES; i++) {
        misplaced = (uint8_t)(misplaced + (guess_counts[i] < solution_counts[i] ? guess_counts[i] : solution_counts[i]));
    }
    return (uint8_t)(((exact & 0x0fu) << 4) | (misplaced & 0x0fu));
}

static void logic_generate_sprite_pool(app_t *app, uint8_t count) {
    bool used[0x4a];
    memset(used, 0, sizeof(used));
    uint8_t written = 0u;
    while (written < count) {
        uint8_t code = (uint8_t)((logic_random(app) % 0x49u) + 1u);
        if (code >= ARRAY_COUNT(used) || used[code] || memory_game_card_sprite(code) == 0) {
            continue;
        }
        used[code] = true;
        app->logic_sprite_pool[written++] = code;
    }
}

static uint16_t logic_random(app_t *app) {
    app->logic_rng_seed = app->logic_rng_seed * 1103515245u + 12345u;
    return (uint16_t)((app->logic_rng_seed >> 16) & 0x7fffu);
}

static uint32_t logic_seed(uint32_t now) {
    uint32_t seed = now ^ (time_ticks8() * 1103515245u) ^ 0x29afe8u ^ 0x241780u;
    return seed == 0u ? LOGIC_RNG_FALLBACK_SEED : seed;
}

static void logic_save_storage(const app_t *app) {
    store_setting_set_u8(STORE_SETTING_GAMES_LOGIC_LEVEL, app->logic_level_byte);
}

static logic_level_t logic_level_record(uint8_t level_byte) {
    uint8_t byte = level_byte < LOGIC_MAX_LEVELS ? level_byte : 0u;
    logic_level_t level = {
        .length = byte < 4u ? 4u : 5u,
        .figures = (uint8_t)(((byte & 3u) << 1u) + 4u),
    };
    return level;
}

static uint8_t logic_row_index(uint8_t attempt, uint8_t peg) {
    return (uint8_t)(attempt * LOGIC_MAX_PEGS + peg);
}

static uint8_t logic_menu_items(const app_t *app, logic_menu_item_t *items, uint8_t cap) {
    uint8_t count = 0u;
    /* The label is display-only (dispatch is by .kind, never strcmp'd), so it
     * is localized in place via ts_or(sid, English). SIDs are the games-block
     * v6.00 string ids; the English literal stays as the fallback. */
#define ADD_ITEM(k, sid, text) do { \
    if (count < cap) { \
        items[count].kind = (k); \
        items[count].label = ts_or((sid), (text)); \
        count++; \
    } \
} while (0)
    ADD_ITEM(LOGIC_ITEM_LEVEL, 0x164u, "Level");
    if (app->logic_menu_variant == LOGIC_MENU_CONTINUE) {
        ADD_ITEM(LOGIC_ITEM_CONTINUE, 0x160u, "Continue");
    }
    if (app->logic_menu_variant == LOGIC_MENU_LAST_VIEW) {
        ADD_ITEM(LOGIC_ITEM_LAST_VIEW, 0x163u, "Last view");
    }
    ADD_ITEM(LOGIC_ITEM_NEW_GAME, 0x165u, "New game");
    ADD_ITEM(LOGIC_ITEM_INSTRUCTIONS, 0x162u, "Instructions");
#undef ADD_ITEM
    return count;
}

static int8_t logic_menu_index_for_kind(const app_t *app, logic_item_kind_t kind) {
    logic_menu_item_t items[5];
    uint8_t count = logic_menu_items(app, items, (uint8_t)ARRAY_COUNT(items));
    for (uint8_t i = 0u; i < count; i++) {
        if (items[i].kind == kind) {
            return (int8_t)i;
        }
    }
    return -1;
}

static uint8_t logic_instruction_lines(char lines[][32], uint8_t max_lines) {
    return wrap_text_lines_ex(asset_font(FONT_FS1), ts_or(0x40au, LOGIC_INSTRUCTIONS), FB_WIDTH, (char *)lines, 32u, max_lines);
}

static uint8_t logic_instruction_max_scroll(void) {
    char lines[24][32];
    uint8_t count = logic_instruction_lines(lines, (uint8_t)ARRAY_COUNT(lines));
    return count > 3u ? (uint8_t)(count - 3u) : 0u;
}

static void draw_logic_board(const app_t *app, framebuffer_t *fb) {
    fb_clear(fb, false);
    logic_level_t level = logic_level_record(app->logic_level_byte);
    uint8_t checked = app->logic_checked_count > LOGIC_MAX_ATTEMPTS ? LOGIC_MAX_ATTEMPTS : app->logic_checked_count;
    if (checked == 0u) {
        for (uint8_t i = 0u; i < level.figures; i++) {
            draw_logic_sprite(fb, app->logic_sprite_pool[i], i * LOGIC_TILE_STEP, 0, false);
        }
    } else {
        for (uint8_t attempt = 0u; attempt < checked; attempt++) {
            draw_logic_clue(fb, attempt * LOGIC_TILE_STEP, app->logic_clues[attempt]);
        }
    }
    fb_hline(fb, 0, 7, 80, true);
    uint8_t active_attempt = app->logic_attempt_index >= LOGIC_MAX_ATTEMPTS ? 0u : app->logic_attempt_index;
    uint8_t active_peg = app->logic_peg_index >= level.length ? 0u : app->logic_peg_index;
    for (uint8_t attempt = 0u; attempt < LOGIC_MAX_ATTEMPTS; attempt++) {
        for (uint8_t peg = 0u; peg < level.length; peg++) {
            uint8_t value = app->logic_rows[logic_row_index(attempt, peg)];
            uint8_t code = value > 0u && value <= LOGIC_MAX_FIGURES ? app->logic_sprite_pool[value - 1u] : 0u;
            bool selected = app->route == APP_ROUTE_LOGIC_PLAY &&
                            !app->logic_game_over &&
                            attempt == active_attempt &&
                            peg == active_peg;
            draw_logic_sprite(fb, code, attempt * LOGIC_TILE_STEP, 9 + peg * LOGIC_TILE_STEP, selected);
        }
    }
}

static void draw_logic_sprite(framebuffer_t *fb, uint8_t code, int x, int y, bool selected) {
    const uint8_t *sprite = memory_game_card_sprite(code);
    if (sprite != 0) {
        fb_blit_vlsb(fb, sprite, x, y, LOGIC_TILE_SIZE, LOGIC_TILE_SIZE, true, true);
    }
    if (selected) {
        for (uint8_t yy = 0u; yy < LOGIC_TILE_SIZE; yy++) {
            for (uint8_t xx = 0u; xx < LOGIC_TILE_SIZE; xx++) {
                fb_pixel(fb, x + xx, y + yy, !fb_get_pixel(fb, x + xx, y + yy));
            }
        }
    }
}

static void draw_logic_clue(framebuffer_t *fb, int x, uint8_t clue) {
    if (clue == 0u || clue == 0xffu) {
        return;
    }
    uint8_t exact = (uint8_t)(clue >> 4);
    uint8_t misplaced = (uint8_t)(clue & 0x0fu);
    uint8_t y = 0u;
    for (uint8_t i = 0u; i < exact && i < LOGIC_MAX_PEGS; i++) {
        fb_rect(fb, x + 2, y, 3, 2, true);
        y = (uint8_t)(y + 2u);
    }
    for (uint8_t i = 0u; i < misplaced && i < LOGIC_MAX_PEGS; i++) {
        fb_hline(fb, x + 2, y, 3, true);
        y = (uint8_t)(y + 2u);
    }
}

static void draw_logic_message_text(framebuffer_t *fb, const char *a, const char *b) {
    const font_t *font = asset_font(FONT_FS0);
    fb_text(fb, font, a, 0, 3, true, FB_WIDTH);
    fb_text(fb, font, b, 0, 16, true, FB_WIDTH);
}

#include "apps/memory_app.h"
#include "apps/game_common.h"

#include <stdio.h>
#include <string.h>

#include "apps/games_app.h"
#include "services/input_keys.h"
#include "storage/store_service.h"
#include "services/timebase.h"
#include "services/strings.h"

#define MEMORY_MAX_LEVELS 5u
#define MEMORY_TILE_SIZE 7u
#define MEMORY_TILE_STEP 8u
#define MEMORY_FLAG_REVEALED 0x01u
#define MEMORY_FLAG_MATCHED 0x02u
#define MEMORY_RESULT_DISMISS_MS 3850u
#define MEMORY_TOP_SCORE_FRAME_MS 160u
#define MEMORY_RNG_FALLBACK_SEED 1u

typedef enum {
    MEMORY_MENU_NEW = 0,
    MEMORY_MENU_ACTIVE,
    MEMORY_MENU_CONTINUE,
    MEMORY_MENU_LAST_VIEW,
} memory_menu_variant_t;

typedef enum {
    MEMORY_ITEM_LEVEL = 0,
    MEMORY_ITEM_CONTINUE,
    MEMORY_ITEM_LAST_VIEW,
    MEMORY_ITEM_NEW_GAME,
    MEMORY_ITEM_TOP_SCORE,
    MEMORY_ITEM_INSTRUCTIONS,
} memory_item_kind_t;

typedef struct {
    memory_item_kind_t kind;
    const char *label;
} memory_menu_item_t;

typedef struct {
    uint8_t cols;
    uint8_t rows;
} memory_level_t;

static const memory_level_t MEMORY_LEVELS[] = {
    {2u, 2u},
    {4u, 3u},
    {6u, 4u},
    {8u, 5u},
    {10u, 6u},
};

static const uint8_t MEMORY_CARD_SPRITES[0x4a][MEMORY_TILE_SIZE] = {
    [0x01] = {0x00u, 0x3eu, 0x2au, 0x3eu, 0x2au, 0x3eu, 0x00u},
    [0x02] = {0x26u, 0x19u, 0x01u, 0x1eu, 0x20u, 0x27u, 0x1bu},
    [0x03] = {0x18u, 0x38u, 0x38u, 0x1eu, 0x3au, 0x3cu, 0x18u},
    [0x04] = {0x50u, 0x30u, 0x68u, 0x14u, 0x0au, 0x05u, 0x03u},
    [0x05] = {0x7fu, 0x41u, 0x55u, 0x55u, 0x55u, 0x41u, 0x7fu},
    [0x06] = {0x00u, 0x1eu, 0x3bu, 0x31u, 0x3bu, 0x3fu, 0x1eu},
    [0x07] = {0x06u, 0x09u, 0x11u, 0x22u, 0x11u, 0x09u, 0x06u},
    [0x08] = {0x06u, 0x0fu, 0x1fu, 0x3eu, 0x1fu, 0x0fu, 0x06u},
    [0x09] = {0x3eu, 0x41u, 0x55u, 0x51u, 0x55u, 0x41u, 0x3eu},
    [0x0a] = {0x3cu, 0x43u, 0x48u, 0x43u, 0x48u, 0x43u, 0x3cu},
    [0x0b] = {0x38u, 0x44u, 0x32u, 0x09u, 0x05u, 0x05u, 0x02u},
    [0x0c] = {0x0eu, 0x31u, 0x45u, 0x59u, 0x45u, 0x31u, 0x0eu},
    [0x0d] = {0x10u, 0x20u, 0x4au, 0x7du, 0x4au, 0x20u, 0x10u},
    [0x0e] = {0x1cu, 0x14u, 0x77u, 0x41u, 0x77u, 0x14u, 0x1cu},
    [0x0f] = {0x00u, 0x20u, 0x70u, 0x70u, 0x3fu, 0x02u, 0x0cu},
    [0x10] = {0x00u, 0x00u, 0x20u, 0x50u, 0x50u, 0x3fu, 0x00u},
    [0x11] = {0x00u, 0x3eu, 0x43u, 0x43u, 0x43u, 0x3eu, 0x00u},
    [0x12] = {0x00u, 0x20u, 0x50u, 0x5fu, 0x52u, 0x25u, 0x00u},
    [0x13] = {0x00u, 0x08u, 0x22u, 0x1cu, 0x41u, 0x3eu, 0x00u},
    [0x14] = {0x02u, 0x04u, 0x08u, 0x77u, 0x08u, 0x04u, 0x02u},
    [0x15] = {0x10u, 0x1cu, 0x1au, 0x1au, 0x1au, 0x1cu, 0x10u},
    [0x16] = {0x00u, 0x0eu, 0x31u, 0x41u, 0x49u, 0x22u, 0x00u},
    [0x17] = {0x0cu, 0x14u, 0x24u, 0x24u, 0x24u, 0x14u, 0x0cu},
    [0x18] = {0x00u, 0x05u, 0x05u, 0x7fu, 0x05u, 0x05u, 0x00u},
    [0x19] = {0x1cu, 0x3eu, 0x7fu, 0x6fu, 0x4du, 0x0eu, 0x0cu},
    [0x1a] = {0x08u, 0x14u, 0x22u, 0x77u, 0x14u, 0x14u, 0x1cu},
    [0x1b] = {0x1cu, 0x2eu, 0x5du, 0x57u, 0x5du, 0x2eu, 0x1cu},
    [0x1c] = {0x1cu, 0x2eu, 0x4bu, 0x4fu, 0x4bu, 0x2eu, 0x1cu},
    [0x1d] = {0x08u, 0x7cu, 0x0eu, 0x6fu, 0x0eu, 0x7cu, 0x08u},
    [0x1e] = {0x10u, 0x38u, 0x50u, 0x42u, 0x3du, 0x02u, 0x00u},
    [0x1f] = {0x08u, 0x13u, 0x23u, 0x2cu, 0x23u, 0x13u, 0x08u},
    [0x20] = {0x00u, 0x00u, 0x7cu, 0x74u, 0x7fu, 0x00u, 0x00u},
    [0x21] = {0x3cu, 0x74u, 0x7cu, 0x70u, 0x7fu, 0x30u, 0x10u},
    [0x22] = {0x08u, 0x1cu, 0x2au, 0x77u, 0x2au, 0x1cu, 0x08u},
    [0x23] = {0x65u, 0x15u, 0x0eu, 0x1fu, 0x0eu, 0x15u, 0x65u},
    [0x24] = {0x0cu, 0x12u, 0x12u, 0x0cu, 0x12u, 0x12u, 0x0cu},
    [0x25] = {0x20u, 0x50u, 0x56u, 0x56u, 0x54u, 0x54u, 0x48u},
    [0x26] = {0x0cu, 0x08u, 0x49u, 0x5du, 0x7eu, 0x55u, 0x19u},
    [0x27] = {0x0cu, 0x0cu, 0x3fu, 0x3fu, 0x0cu, 0x0cu, 0x0cu},
    [0x28] = {0x65u, 0x15u, 0x0au, 0x11u, 0x0au, 0x15u, 0x65u},
    [0x29] = {0x0cu, 0x12u, 0x21u, 0x2du, 0x2du, 0x22u, 0x1cu},
    [0x2a] = {0x30u, 0x48u, 0x44u, 0x43u, 0x48u, 0x48u, 0x30u},
    [0x2b] = {0x08u, 0x2au, 0x14u, 0x63u, 0x14u, 0x2au, 0x08u},
    [0x2c] = {0x3eu, 0x41u, 0x01u, 0x19u, 0x45u, 0x26u, 0x18u},
    [0x2d] = {0x1cu, 0x22u, 0x26u, 0x24u, 0x12u, 0x32u, 0x3eu},
    [0x2e] = {0x10u, 0x10u, 0x1cu, 0x12u, 0x12u, 0x12u, 0x1cu},
    [0x2f] = {0x22u, 0x55u, 0x3eu, 0x14u, 0x3eu, 0x55u, 0x22u},
    [0x30] = {0x1fu, 0x3du, 0x3fu, 0x1cu, 0x3fu, 0x3fu, 0x1cu},
    [0x31] = {0x08u, 0x0bu, 0x0bu, 0x7fu, 0x08u, 0x38u, 0x38u},
    [0x32] = {0x1cu, 0x62u, 0x0du, 0x61u, 0x0du, 0x62u, 0x1cu},
    [0x33] = {0x7fu, 0x22u, 0x7fu, 0x22u, 0x7fu, 0x22u, 0x7fu},
    [0x34] = {0x18u, 0x7cu, 0x1cu, 0x1cu, 0x7fu, 0x06u, 0x06u},
    [0x35] = {0x00u, 0x7fu, 0x3eu, 0x1cu, 0x08u, 0x00u, 0x00u},
    [0x36] = {0x00u, 0x38u, 0x08u, 0x08u, 0x08u, 0x3fu, 0x00u},
    [0x37] = {0x06u, 0x3eu, 0x06u, 0x06u, 0x06u, 0x3eu, 0x06u},
    [0x38] = {0x12u, 0x3fu, 0x12u, 0x3fu, 0x12u, 0x3fu, 0x12u},
    [0x39] = {0x20u, 0x10u, 0x32u, 0x65u, 0x7bu, 0x05u, 0x02u},
    [0x3a] = {0x02u, 0x24u, 0x10u, 0x13u, 0x10u, 0x24u, 0x02u},
    [0x3b] = {0x1cu, 0x22u, 0x41u, 0x47u, 0x4fu, 0x3eu, 0x1cu},
    [0x3c] = {0x1cu, 0x2au, 0x41u, 0x6fu, 0x49u, 0x22u, 0x1cu},
    [0x3d] = {0x1eu, 0x0cu, 0x1eu, 0x3fu, 0x3fu, 0x1au, 0x0cu},
    [0x3e] = {0x04u, 0x04u, 0x3eu, 0x6du, 0x4fu, 0x4eu, 0x38u},
    [0x3f] = {0x1cu, 0x22u, 0x4du, 0x53u, 0x61u, 0x20u, 0x00u},
    [0x40] = {0x7fu, 0x51u, 0x1du, 0x50u, 0x55u, 0x45u, 0x7fu},
    [0x41] = {0x1cu, 0x22u, 0x49u, 0x5du, 0x49u, 0x22u, 0x1cu},
    [0x42] = {0x08u, 0x08u, 0x08u, 0x08u, 0x2au, 0x1cu, 0x08u},
    [0x43] = {0x04u, 0x06u, 0x06u, 0x3fu, 0x46u, 0x26u, 0x04u},
    [0x44] = {0x55u, 0x2au, 0x55u, 0x2au, 0x55u, 0x2au, 0x55u},
    [0x45] = {0x08u, 0x14u, 0x22u, 0x41u, 0x22u, 0x14u, 0x08u},
    [0x46] = {0x01u, 0x05u, 0x15u, 0x55u, 0x15u, 0x05u, 0x01u},
    [0x47] = {0x36u, 0x49u, 0x49u, 0x36u, 0x49u, 0x49u, 0x36u},
    [0x48] = {0x36u, 0x22u, 0x36u, 0x22u, 0x36u, 0x22u, 0x36u},
    [0x49] = {0x00u, 0x06u, 0x08u, 0x7fu, 0x08u, 0x06u, 0x00u},
};

static const uint16_t MEMORY_TOP_SCORE_FRAMES[] = {
    220u, 221u, 222u, 223u, 224u, 225u, 226u, 227u, 228u, 229u,
    229u, 230u, 230u, 229u, 229u, 230u, 230u, 229u, 230u, 230u,
};

static const char MEMORY_INSTRUCTIONS[] =
    "Reveal pictures to find pairs with as few tries as possible. Move the cursor with keys 2, 4, 6, 8, *, #. Use key 5 to reveal pictures. Once found, pairs stay visible.";

static void open_memory_menu_variant(app_t *app, memory_menu_variant_t variant, memory_item_kind_t selected_kind);
static void open_memory_level(app_t *app);
static void open_memory_top_score(app_t *app, uint32_t now);
static void open_memory_instructions(app_t *app);
static void start_memory_game(app_t *app, uint32_t now);
static void pause_memory_to_menu(app_t *app);
static void finish_memory_game(app_t *app, uint32_t now);
static void memory_reveal_current(app_t *app, uint32_t now);
static void memory_move_cursor(app_t *app, int8_t dx, int8_t dy);
static void memory_move_linear(app_t *app, int8_t delta);
static bool memory_clear_pending_mismatch(app_t *app);
static void memory_generate_board(app_t *app, uint32_t now);
static uint16_t memory_random(app_t *app);
static uint32_t memory_seed(uint32_t now);
static void memory_save_storage(const app_t *app);
static uint8_t memory_menu_items(const app_t *app, memory_menu_item_t *items, uint8_t cap);
static int8_t memory_menu_index_for_kind(const app_t *app, memory_item_kind_t kind);
static const memory_level_t *memory_level_record(uint8_t level_byte);
static uint8_t memory_card_count(const app_t *app);
static uint8_t memory_board_index(const app_t *app, uint8_t col, uint8_t row);
static uint8_t memory_linear_index(const app_t *app, uint8_t col, uint8_t row);
static uint8_t memory_matched_count(const app_t *app);
static uint8_t memory_instruction_lines(char lines[][32], uint8_t max_lines);
static uint8_t memory_instruction_max_scroll(void);
static void draw_memory_board(const app_t *app, framebuffer_t *fb);
static void draw_memory_tile(const app_t *app, framebuffer_t *fb, uint8_t index, int x, int y, bool selected);
static void draw_memory_message_text(framebuffer_t *fb, const char *a, const char *b, const char *c);

void memory_app_init(app_t *app) {
    uint8_t level = 0u;
    uint16_t top_score = 0u;
    store_setting_get_u8(STORE_SETTING_GAMES_MEMORY_LEVEL, &level);
    store_setting_get_u16(STORE_SETTING_GAMES_MEMORY_TOP_SCORE, &top_score);
    if (level >= MEMORY_MAX_LEVELS) {
        level = 0u;
    }
    if (top_score > 9999u) {
        top_score = 9999u;
    }
    app->memory_menu_variant = MEMORY_MENU_NEW;
    app->memory_menu_selected = 0u;
    app->memory_level_byte = level;
    app->memory_level_draft_byte = level;
    app->memory_top_score = top_score;
    app->memory_first_pick_index = -1;
    app->memory_pending_a = -1;
    app->memory_pending_b = -1;
}

const uint8_t *memory_game_card_sprite(uint8_t code) {
    return code < ARRAY_COUNT(MEMORY_CARD_SPRITES) ? MEMORY_CARD_SPRITES[code] : 0;
}

void open_memory_menu(app_t *app) {
    open_memory_menu_variant(app, (memory_menu_variant_t)app->memory_menu_variant, MEMORY_ITEM_NEW_GAME);
}

bool handle_memory_menu_key(app_t *app, uint16_t key, uint32_t now) {
    memory_menu_item_t items[6];
    uint8_t count = memory_menu_items(app, items, (uint8_t)ARRAY_COUNT(items));
    if (count == 0u) {
        return true;
    }
    if (app->memory_menu_selected >= count) {
        app->memory_menu_selected = 0u;
        app->game_option_view_start = 0u;
    }
    if (key == KEY_C) {
        open_games_menu(app, 2u);
        return true;
    }
    if (key == KEY_UP || key == KEY_2) {
        ui_circular_list_step_3rows(count, -1,
                                    &app->memory_menu_selected,
                                    &app->game_option_view_start);
        app->dirty = true;
        return true;
    }
    if (key == KEY_DOWN || key == KEY_8) {
        ui_circular_list_step_3rows(count, 1,
                                    &app->memory_menu_selected,
                                    &app->game_option_view_start);
        app->dirty = true;
        return true;
    }
    if (key != KEY_NAVI && key != KEY_5) {
        return true;
    }

    memory_item_kind_t kind = items[app->memory_menu_selected].kind;
    if (kind == MEMORY_ITEM_NEW_GAME) {
        start_memory_game(app, now);
    } else if (kind == MEMORY_ITEM_CONTINUE) {
        app->route = APP_ROUTE_MEMORY_PLAY;
        app->memory_game_over = false;
        app->dirty = true;
    } else if (kind == MEMORY_ITEM_LAST_VIEW) {
        app->route = APP_ROUTE_MEMORY_LAST_VIEW;
        app->dirty = true;
    } else if (kind == MEMORY_ITEM_LEVEL) {
        open_memory_level(app);
    } else if (kind == MEMORY_ITEM_TOP_SCORE) {
        open_memory_top_score(app, now);
    } else if (kind == MEMORY_ITEM_INSTRUCTIONS) {
        open_memory_instructions(app);
    }
    return true;
}

bool handle_memory_play_key(app_t *app, uint16_t key, uint32_t now) {
    if (app->route == APP_ROUTE_MEMORY_LAST_VIEW) {
        if (key == KEY_C || key == KEY_NAVI || key == KEY_5) {
            open_memory_menu_variant(app, MEMORY_MENU_LAST_VIEW, MEMORY_ITEM_LAST_VIEW);
        }
        return true;
    }
    if (key == KEY_C || key == KEY_NAVI) {
        pause_memory_to_menu(app);
        return true;
    }
    if (app->memory_game_over) {
        return true;
    }
    if (key == KEY_UP || key == KEY_2) {
        memory_move_cursor(app, 0, -1);
    } else if (key == KEY_DOWN || key == KEY_8) {
        memory_move_cursor(app, 0, 1);
    } else if (key == KEY_4) {
        memory_move_cursor(app, -1, 0);
    } else if (key == KEY_6) {
        memory_move_cursor(app, 1, 0);
    } else if (key == KEY_STAR) {
        memory_move_linear(app, -1);
    } else if (key == KEY_HASH) {
        memory_move_linear(app, 1);
    } else if (key == KEY_5) {
        memory_reveal_current(app, now);
    }
    return true;
}

bool handle_memory_level_key(app_t *app, uint16_t key, uint32_t now) {
    if (key == KEY_UP || key == KEY_2) {
        adjust_game_level(&app->memory_level_draft_byte, MEMORY_MAX_LEVELS, 1);
        app->dirty = true;
        return true;
    }
    if (key == KEY_DOWN || key == KEY_8) {
        adjust_game_level(&app->memory_level_draft_byte, MEMORY_MAX_LEVELS, -1);
        app->dirty = true;
        return true;
    }
    if (key == KEY_NAVI || key == KEY_5) {
        app->memory_level_byte = app->memory_level_draft_byte;
        app->memory_menu_variant = MEMORY_MENU_ACTIVE;
        memory_save_storage(app);
        open_memory_menu_variant(app, MEMORY_MENU_ACTIVE, MEMORY_ITEM_LEVEL);
        return true;
    }
    if (key == KEY_C) {
        app->memory_level_draft_byte = app->memory_level_byte;
        open_memory_menu_variant(app, (memory_menu_variant_t)app->memory_menu_variant, MEMORY_ITEM_LEVEL);
        return true;
    }
    (void)now;
    return true;
}

bool handle_memory_instructions_key(app_t *app, uint16_t key, uint32_t now) {
    uint8_t max_scroll = memory_instruction_max_scroll();
    if ((key == KEY_UP || key == KEY_2) && app->memory_instructions_scroll > 0u) {
        app->memory_instructions_scroll--;
        app->dirty = true;
    } else if ((key == KEY_DOWN || key == KEY_8) && app->memory_instructions_scroll < max_scroll) {
        app->memory_instructions_scroll++;
        app->dirty = true;
    } else if (key == KEY_C || key == KEY_NAVI || key == KEY_5) {
        open_memory_menu_variant(app, (memory_menu_variant_t)app->memory_menu_variant, MEMORY_ITEM_INSTRUCTIONS);
    }
    (void)now;
    return true;
}

bool handle_memory_message_key(app_t *app, uint16_t key, uint32_t now) {
    if (key == KEY_C || key == KEY_NAVI || key == KEY_5) {
        memory_item_kind_t item = app->route == APP_ROUTE_MEMORY_TOP_SCORE ? MEMORY_ITEM_TOP_SCORE : MEMORY_ITEM_LAST_VIEW;
        memory_menu_variant_t variant = app->route == APP_ROUTE_MEMORY_RESULT ? MEMORY_MENU_LAST_VIEW :
            (memory_menu_variant_t)app->memory_menu_variant;
        open_memory_menu_variant(app, variant, item);
    }
    (void)now;
    return true;
}

bool tick_memory(app_t *app, uint32_t now) {
    bool changed = false;
    if (app->route == APP_ROUTE_MEMORY_RESULT &&
        app->memory_result_deadline_ms != 0u &&
        time_diff_ms(now, app->memory_result_deadline_ms) >= 0) {
        open_memory_menu_variant(app, MEMORY_MENU_LAST_VIEW, MEMORY_ITEM_LAST_VIEW);
        changed = true;
    }
    if (app->route == APP_ROUTE_MEMORY_TOP_SCORE &&
        time_diff_ms(now, app->memory_top_score_last_frame_ms + MEMORY_TOP_SCORE_FRAME_MS) >= 0) {
        app->memory_top_score_last_frame_ms = now;
        if (app->memory_top_score_frame + 1u < ARRAY_COUNT(MEMORY_TOP_SCORE_FRAMES)) {
            app->memory_top_score_frame++;
            changed = true;
        }
    }
    return changed;
}

void render_memory_menu(const app_t *app, framebuffer_t *fb) {
    memory_menu_item_t items[6];
    const char *labels[6];
    uint8_t count = memory_menu_items(app, items, (uint8_t)ARRAY_COUNT(items));
    for (uint8_t i = 0; i < count; i++) {
        labels[i] = items[i].label;
    }
    char crumb[10];
    draw_flat_list_circular_view(fb, labels, count,
                                 app->memory_menu_selected,
                                 app->game_option_view_start,
                                 ui_breadcrumb_path(crumb, sizeof(crumb), "6-3", (unsigned)(app->memory_menu_selected + 1u)),
                                 "Select");
}

void render_memory_play(const app_t *app, framebuffer_t *fb) {
    draw_memory_board(app, fb);
}

void render_memory_level(const app_t *app, framebuffer_t *fb) {
    draw_game_level_selector(fb, app->memory_level_draft_byte, MEMORY_MAX_LEVELS);
}

void render_memory_instructions(const app_t *app, framebuffer_t *fb) {
    fb_clear(fb, false);
    char lines[18][32];
    uint8_t count = memory_instruction_lines(lines, (uint8_t)ARRAY_COUNT(lines));
    uint8_t start = app->memory_instructions_scroll;
    if (start > count) {
        start = 0u;
    }
    const font_t *font = asset_font(FONT_FS1);
    for (uint8_t row = 0; row < 3u && start + row < count; row++) {
        fb_text(fb, font, lines[start + row], 0, 7 + row * 9, true, FB_WIDTH);
    }
    draw_softkey(fb, "Back");
}

void render_memory_top_score(const app_t *app, framebuffer_t *fb) {
    fb_clear(fb, false);
    char score[8];
    snprintf(score, sizeof(score), "%u", (unsigned)app->memory_top_score);
    draw_memory_message_text(fb, "Top score:", score, 0);
    uint8_t frame = app->memory_top_score_frame;
    if (frame >= ARRAY_COUNT(MEMORY_TOP_SCORE_FRAMES)) {
        frame = (uint8_t)(ARRAY_COUNT(MEMORY_TOP_SCORE_FRAMES) - 1u);
    }
    fb_bitmap(fb, MEMORY_TOP_SCORE_FRAMES[frame], 63, 0, true, true);
}

void render_memory_result(const app_t *app, framebuffer_t *fb) {
    fb_clear(fb, false);
    char score[8];
    snprintf(score, sizeof(score), "%u", (unsigned)app->memory_score);
    /* "Game over!" via standalone SID 0x157, consistent with the other games;
     * "TOP SCORE:"/"Your score:" are second-line fragments of the composite
     * result records (0x168/0x16a) with no standalone SID, so kept literal. */
    draw_memory_message_text(fb,
                             ts_or(0x157u, "Game over!"),
                             app->memory_result_top_score ? "TOP SCORE:" : "Your score:",
                             score);
}

static void open_memory_menu_variant(app_t *app, memory_menu_variant_t variant, memory_item_kind_t selected_kind) {
    app->route = APP_ROUTE_MEMORY_MENU;
    app->memory_menu_variant = (uint8_t)variant;
    int8_t selected = memory_menu_index_for_kind(app, selected_kind);
    app->memory_menu_selected = selected >= 0 ? (uint8_t)selected : 0u;
    app->game_option_view_start = app->memory_menu_selected;
    app->dirty = true;
}

static void open_memory_level(app_t *app) {
    app->route = APP_ROUTE_MEMORY_LEVEL;
    app->memory_level_draft_byte = app->memory_level_byte;
    app->dirty = true;
}

static void open_memory_top_score(app_t *app, uint32_t now) {
    app->route = APP_ROUTE_MEMORY_TOP_SCORE;
    app->memory_top_score_frame = 0u;
    app->memory_top_score_last_frame_ms = now;
    app->dirty = true;
}

static void open_memory_instructions(app_t *app) {
    app->route = APP_ROUTE_MEMORY_INSTRUCTIONS;
    app->memory_instructions_scroll = 0u;
    app->dirty = true;
}

static void start_memory_game(app_t *app, uint32_t now) {
    app->route = APP_ROUTE_MEMORY_PLAY;
    app->memory_menu_variant = MEMORY_MENU_ACTIVE;
    app->memory_cursor_col = 0u;
    app->memory_cursor_row = 0u;
    app->memory_first_pick_index = -1;
    app->memory_pending_a = -1;
    app->memory_pending_b = -1;
    app->memory_pending_mismatch = false;
    app->memory_score = 0u;
    app->memory_game_over = false;
    app->memory_result_top_score = false;
    app->memory_result_deadline_ms = 0u;
    memory_generate_board(app, now);
    app->dirty = true;
}

static void pause_memory_to_menu(app_t *app) {
    open_memory_menu_variant(app, MEMORY_MENU_CONTINUE, MEMORY_ITEM_CONTINUE);
}

static void finish_memory_game(app_t *app, uint32_t now) {
    const memory_level_t *level = memory_level_record(app->memory_level_byte);
    uint16_t tries = app->memory_score;
    uint16_t score_limit = (uint16_t)(((uint16_t)level->cols * (uint16_t)level->rows * 7u) / 4u);
    uint16_t final_score = tries >= score_limit ? 0u : (uint16_t)(score_limit - tries);
    app->memory_score = final_score;
    app->route = APP_ROUTE_MEMORY_RESULT;
    app->memory_game_over = true;
    app->memory_result_deadline_ms = now + MEMORY_RESULT_DISMISS_MS;
    app->memory_menu_variant = MEMORY_MENU_LAST_VIEW;
    app->memory_result_top_score = final_score > 0u && final_score > app->memory_top_score;
    if (app->memory_result_top_score) {
        app->memory_top_score = final_score;
        memory_save_storage(app);
        /* Shared result helper 0x0029a2a0 plays tone 0x14 on a new TOP SCORE. */
        play_game_system_tone(20u);
    }
    app->dirty = true;
}

static void memory_reveal_current(app_t *app, uint32_t now) {
    (void)memory_clear_pending_mismatch(app);
    uint8_t index = memory_board_index(app, app->memory_cursor_col, app->memory_cursor_row);
    if ((app->memory_flags[index] & MEMORY_FLAG_MATCHED) != 0u) {
        app->dirty = true;
        return;
    }
    if ((app->memory_flags[index] & MEMORY_FLAG_REVEALED) != 0u) {
        if (app->memory_first_pick_index == (int8_t)index) {
            memory_move_linear(app, 1);
        }
        app->dirty = true;
        return;
    }

    app->memory_flags[index] |= MEMORY_FLAG_REVEALED;
    if (app->memory_first_pick_index < 0 ||
        (app->memory_flags[(uint8_t)app->memory_first_pick_index] & MEMORY_FLAG_REVEALED) == 0u) {
        app->memory_first_pick_index = (int8_t)index;
        app->dirty = true;
        return;
    }

    uint8_t first_index = (uint8_t)app->memory_first_pick_index;
    app->memory_first_pick_index = -1;
    if (app->memory_score < 9999u) {
        app->memory_score++;
    }
    if (app->memory_cards[first_index] == app->memory_cards[index]) {
        app->memory_flags[first_index] |= MEMORY_FLAG_REVEALED | MEMORY_FLAG_MATCHED;
        app->memory_flags[index] |= MEMORY_FLAG_REVEALED | MEMORY_FLAG_MATCHED;
        play_game_system_tone(16u);
        if (memory_matched_count(app) >= memory_card_count(app)) {
            finish_memory_game(app, now);
            return;
        }
    } else {
        app->memory_pending_mismatch = true;
        app->memory_pending_a = (int8_t)first_index;
        app->memory_pending_b = (int8_t)index;
    }
    app->dirty = true;
}

static void memory_move_cursor(app_t *app, int8_t dx, int8_t dy) {
    memory_clear_pending_mismatch(app);
    const memory_level_t *level = memory_level_record(app->memory_level_byte);
    int16_t col = (int16_t)app->memory_cursor_col + dx;
    int16_t row = (int16_t)app->memory_cursor_row + dy;
    while (col < 0) {
        col += level->cols;
    }
    while (row < 0) {
        row += level->rows;
    }
    app->memory_cursor_col = (uint8_t)(col % level->cols);
    app->memory_cursor_row = (uint8_t)(row % level->rows);
    app->dirty = true;
}

static void memory_move_linear(app_t *app, int8_t delta) {
    memory_clear_pending_mismatch(app);
    const memory_level_t *level = memory_level_record(app->memory_level_byte);
    uint8_t count = (uint8_t)(level->cols * level->rows);
    int16_t index = (int16_t)memory_linear_index(app, app->memory_cursor_col, app->memory_cursor_row) + delta;
    while (index < 0) {
        index += count;
    }
    index %= count;
    app->memory_cursor_col = (uint8_t)(index % level->cols);
    app->memory_cursor_row = (uint8_t)(index / level->cols);
    app->dirty = true;
}

static bool memory_clear_pending_mismatch(app_t *app) {
    if (!app->memory_pending_mismatch) {
        return false;
    }
    if (app->memory_pending_a >= 0) {
        uint8_t index = (uint8_t)app->memory_pending_a;
        if ((app->memory_flags[index] & MEMORY_FLAG_MATCHED) == 0u) {
            app->memory_flags[index] &= (uint8_t)~MEMORY_FLAG_REVEALED;
        }
    }
    if (app->memory_pending_b >= 0) {
        uint8_t index = (uint8_t)app->memory_pending_b;
        if ((app->memory_flags[index] & MEMORY_FLAG_MATCHED) == 0u) {
            app->memory_flags[index] &= (uint8_t)~MEMORY_FLAG_REVEALED;
        }
    }
    app->memory_pending_mismatch = false;
    app->memory_pending_a = -1;
    app->memory_pending_b = -1;
    return true;
}

static void memory_generate_board(app_t *app, uint32_t now) {
    const memory_level_t *level = memory_level_record(app->memory_level_byte);
    uint8_t count = (uint8_t)(level->cols * level->rows);
    uint8_t pair_count = (uint8_t)(count / 2u);
    uint8_t pool[0x49];
    memset(app->memory_cards, 0, sizeof(app->memory_cards));
    memset(app->memory_flags, 0, sizeof(app->memory_flags));
    for (uint8_t i = 0u; i < (uint8_t)ARRAY_COUNT(pool); i++) {
        pool[i] = (uint8_t)(i + 1u);
    }
    app->memory_rng_seed = memory_seed(now);
    for (int8_t i = (int8_t)ARRAY_COUNT(pool) - 1; i > 0; i--) {
        uint8_t swap = (uint8_t)(memory_random(app) % (uint8_t)(i + 1));
        uint8_t tmp = pool[(uint8_t)i];
        pool[(uint8_t)i] = pool[swap];
        pool[swap] = tmp;
    }
    for (uint8_t i = 0u; i < pair_count; i++) {
        app->memory_cards[i * 2u] = pool[i];
        app->memory_cards[i * 2u + 1u] = pool[i];
    }
    for (int8_t i = (int8_t)count - 1; i > 0; i--) {
        uint8_t swap = (uint8_t)(memory_random(app) % (uint8_t)(i + 1));
        uint8_t tmp = app->memory_cards[(uint8_t)i];
        app->memory_cards[(uint8_t)i] = app->memory_cards[swap];
        app->memory_cards[swap] = tmp;
    }
}

static uint16_t memory_random(app_t *app) {
    app->memory_rng_seed = app->memory_rng_seed * 1103515245u + 12345u;
    return (uint16_t)((app->memory_rng_seed >> 16) & 0x7fffu);
}

static uint32_t memory_seed(uint32_t now) {
    uint32_t seed = now ^ (time_ticks8() * 1103515245u) ^ 0x29afe8u ^ 0x240bd8u;
    return seed == 0u ? MEMORY_RNG_FALLBACK_SEED : seed;
}

static void memory_save_storage(const app_t *app) {
    store_setting_set_u8(STORE_SETTING_GAMES_MEMORY_LEVEL, app->memory_level_byte);
    store_setting_set_u16(STORE_SETTING_GAMES_MEMORY_TOP_SCORE, app->memory_top_score);
}

static uint8_t memory_menu_items(const app_t *app, memory_menu_item_t *items, uint8_t cap) {
    uint8_t count = 0u;
#define ADD_ITEM(k, text) do { \
    if (count < cap) { \
        items[count].kind = (k); \
        items[count].label = (text); \
        count++; \
    } \
} while (0)
    /* Labels are render-only (dispatch is by .kind), so the localized string is
     * stored directly; ts_or() returns a flash pointer valid for process life.
     * SIDs are the standalone v6.00 menu-label records (index+58). */
    ADD_ITEM(MEMORY_ITEM_LEVEL, ts_or(0x164u, "Level"));
    if (app->memory_menu_variant == MEMORY_MENU_CONTINUE) {
        ADD_ITEM(MEMORY_ITEM_CONTINUE, ts_or(0x160u, "Continue"));
    }
    if (app->memory_menu_variant == MEMORY_MENU_LAST_VIEW) {
        ADD_ITEM(MEMORY_ITEM_LAST_VIEW, ts_or(0x163u, "Last view"));
    }
    ADD_ITEM(MEMORY_ITEM_NEW_GAME, ts_or(0x165u, "New game"));
    ADD_ITEM(MEMORY_ITEM_TOP_SCORE, ts_or(0x161u, "Top score"));
    ADD_ITEM(MEMORY_ITEM_INSTRUCTIONS, ts_or(0x162u, "Instructions"));
#undef ADD_ITEM
    return count;
}

static int8_t memory_menu_index_for_kind(const app_t *app, memory_item_kind_t kind) {
    memory_menu_item_t items[6];
    uint8_t count = memory_menu_items(app, items, (uint8_t)ARRAY_COUNT(items));
    for (uint8_t i = 0u; i < count; i++) {
        if (items[i].kind == kind) {
            return (int8_t)i;
        }
    }
    return -1;
}

static const memory_level_t *memory_level_record(uint8_t level_byte) {
    uint8_t index = level_byte < MEMORY_MAX_LEVELS ? level_byte : 0u;
    return &MEMORY_LEVELS[index];
}

static uint8_t memory_card_count(const app_t *app) {
    const memory_level_t *level = memory_level_record(app->memory_level_byte);
    return (uint8_t)(level->cols * level->rows);
}

static uint8_t memory_board_index(const app_t *app, uint8_t col, uint8_t row) {
    const memory_level_t *level = memory_level_record(app->memory_level_byte);
    return (uint8_t)(col * level->rows + row);
}

static uint8_t memory_linear_index(const app_t *app, uint8_t col, uint8_t row) {
    const memory_level_t *level = memory_level_record(app->memory_level_byte);
    return (uint8_t)(row * level->cols + col);
}

static uint8_t memory_matched_count(const app_t *app) {
    uint8_t count = memory_card_count(app);
    uint8_t matched = 0u;
    for (uint8_t i = 0u; i < count; i++) {
        if ((app->memory_flags[i] & MEMORY_FLAG_MATCHED) != 0u) {
            matched++;
        }
    }
    return matched;
}

static uint8_t memory_instruction_lines(char lines[][32], uint8_t max_lines) {
    return wrap_text_lines_ex(asset_font(FONT_FS1), ts_or(0x40cu, MEMORY_INSTRUCTIONS), FB_WIDTH, (char *)lines, 32u, max_lines);
}

static uint8_t memory_instruction_max_scroll(void) {
    char lines[18][32];
    uint8_t count = memory_instruction_lines(lines, (uint8_t)ARRAY_COUNT(lines));
    return count > 3u ? (uint8_t)(count - 3u) : 0u;
}

static void draw_memory_board(const app_t *app, framebuffer_t *fb) {
    fb_clear(fb, false);
    const memory_level_t *level = memory_level_record(app->memory_level_byte);
    int x0 = ((int)FB_WIDTH - (int)level->cols * (int)MEMORY_TILE_STEP) / 2 - 2;
    int y0 = ((int)FB_HEIGHT - (int)level->rows * (int)MEMORY_TILE_STEP) / 2;
    if (x0 < 0) {
        x0 = 0;
    }
    if (y0 < 0) {
        y0 = 0;
    }
    for (uint8_t row = 0u; row < level->rows; row++) {
        for (uint8_t col = 0u; col < level->cols; col++) {
            uint8_t index = memory_board_index(app, col, row);
            draw_memory_tile(app,
                             fb,
                             index,
                             x0 + (int)col * (int)MEMORY_TILE_STEP,
                             y0 + (int)row * (int)MEMORY_TILE_STEP,
                             col == app->memory_cursor_col && row == app->memory_cursor_row);
        }
    }
}

static void draw_memory_tile(const app_t *app, framebuffer_t *fb, uint8_t index, int x, int y, bool selected) {
    bool visible = (app->memory_flags[index] & (MEMORY_FLAG_REVEALED | MEMORY_FLAG_MATCHED)) != 0u;
    uint8_t code = app->memory_cards[index];
    for (uint8_t col = 0u; col < MEMORY_TILE_SIZE; col++) {
        uint8_t bits = visible && code < ARRAY_COUNT(MEMORY_CARD_SPRITES) ? MEMORY_CARD_SPRITES[code][col] : 0xffu;
        for (uint8_t row = 0u; row < MEMORY_TILE_SIZE; row++) {
            fb_pixel(fb, x + col, y + row, (bits & (uint8_t)(1u << row)) != 0u);
        }
    }
    if (selected) {
        for (uint8_t yy = 0u; yy < MEMORY_TILE_SIZE; yy++) {
            for (uint8_t xx = 0u; xx < MEMORY_TILE_SIZE; xx++) {
                fb_pixel(fb, x + xx, y + yy, !fb_get_pixel(fb, x + xx, y + yy));
            }
        }
    }
}

static void draw_memory_message_text(framebuffer_t *fb, const char *a, const char *b, const char *c) {
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

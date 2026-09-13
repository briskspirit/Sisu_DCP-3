#include "apps/games_app.h"

#include "apps/dialogs_app.h"
#include "apps/main_menu_app.h"
#include "apps/logic_app.h"
#include "apps/memory_app.h"
#include "apps/pacman_app.h"
#include "apps/react_app.h"
#include "apps/rotation_app.h"
#include "apps/snake_app.h"
#include "services/feature_gates.h"
#include "services/strings.h"
#include "services/input_keys.h"

/* Localize by v6.00 string id, English fallback: sid 0 (no 1:1 match / the text
 * is ambiguous across contexts) or a sid with no record (ts()==NULL, clone-only)
 * both return the supplied literal. Dispatch here is by index (games_menu_
 * selected), never by label text, so the English label[] is display-only. */
static const char *L(uint16_t sid, const char *en) {
    if (sid != 0u) {
        const char *t = ts(sid);
        if (t != 0) {
            return t;
        }
    }
    return en;
}

/* Pac Man is a documented intentional bonus row appended after the stock
 * games (see CHANGELOG); React/Logic are the gamesExtraRows-gated pair.
 * sid = v6.00 string id (index+58); 0 = keep the English literal because the
 * text is clone-only (Pac Man) or maps to multiple SIDs (Memory: 0x15c game /
 * 0x382 -> ambiguous, do not guess). */
typedef struct {
    const char *label;   /* English fallback + documentation */
    uint16_t sid;
} game_label_t;

static const game_label_t GAMES_LABELS[] = {
    {"Rotation", 0x166u},
    {"Snake", 0x167u},
    {"Memory", 0x15cu},   /* games block: Logic 0x15a, React 0x15b, Memory 0x15c (0x382 is the SMS-memory one) */
    {"React", 0x15bu},
    {"Logic", 0x15au},
    {"Pac Man", 0u},  /* clone-only bonus row; no v6.00 record */
};

/* SET.8: EEPROM config 0x21 masks 5 games (0x001f) vs 3 (0x0007). */
static bool games_item_visible(uint8_t raw) {
    if (raw == 3u || raw == 4u) {
        return feature_gate_visible(FEATURE_GATE_GAMES_EXTRA_ROWS);
    }
    return true;
}

static uint8_t games_visible_count(void) {
    uint8_t count = 0u;
    for (uint8_t i = 0; i < ARRAY_COUNT(GAMES_LABELS); i++) {
        if (games_item_visible(i)) {
            count++;
        }
    }
    return count;
}

static uint8_t games_raw_at_visible(uint8_t visible) {
    uint8_t seen = 0u;
    for (uint8_t i = 0; i < ARRAY_COUNT(GAMES_LABELS); i++) {
        if (!games_item_visible(i)) {
            continue;
        }
        if (seen == visible) {
            return i;
        }
        seen++;
    }
    return 0u;
}

static uint8_t games_visible_index(uint8_t raw) {
    uint8_t visible = 0u;
    for (uint8_t i = 0; i < raw && i < ARRAY_COUNT(GAMES_LABELS); i++) {
        if (games_item_visible(i)) {
            visible++;
        }
    }
    return visible;
}

static uint8_t games_normalized_raw(uint8_t raw) {
    if (raw >= ARRAY_COUNT(GAMES_LABELS) || !games_item_visible(raw)) {
        return 0u;
    }
    return raw;
}

void games_app_init(app_t *app) {
    app->games_menu_selected = 0u;
    memory_app_init(app);
    react_app_init(app);
    rotation_app_init(app);
    logic_app_init(app);
    pacman_app_init(app);
    snake_app_init(app);
}

void open_games_menu(app_t *app, uint8_t selected) {
    app->route = APP_ROUTE_GAMES_MENU;
    app->games_menu_selected = games_normalized_raw(selected);
    app->game_option_view_start = games_visible_index(app->games_menu_selected);
    app->dirty = true;
}

bool handle_games_menu_key(app_t *app, uint16_t key, uint32_t now) {
    if (key == KEY_C) {
        open_main_menu_at(app, 5u, now);
        return true;
    }
    if (key == KEY_UP || key == KEY_DOWN) {
        uint8_t count = games_visible_count();
        uint8_t visible = games_visible_index(games_normalized_raw(app->games_menu_selected));
        ui_circular_list_step_3rows(count, key == KEY_UP ? -1 : 1,
                                    &visible,
                                    &app->game_option_view_start);
        app->games_menu_selected = games_raw_at_visible(visible);
        app->dirty = true;
        return true;
    }
    if (key == KEY_NAVI) {
        if (app->games_menu_selected == 0u) {
            open_rotation_menu(app);
        } else if (app->games_menu_selected == 1u) {
            open_snake_menu(app);
        } else if (app->games_menu_selected == 2u) {
            open_memory_menu(app);
        } else if (app->games_menu_selected == 3u) {
            open_react_menu(app);
        } else if (app->games_menu_selected == 4u) {
            open_logic_menu(app);
        } else if (app->games_menu_selected == 5u) {
            open_pacman_menu(app);
        }
        return true;
    }
    return true;
}

void render_games_menu(const app_t *app, framebuffer_t *fb) {
    const char *labels[ARRAY_COUNT(GAMES_LABELS)];
    uint8_t count = games_visible_count();
    for (uint8_t i = 0; i < count; i++) {
        const game_label_t *entry = &GAMES_LABELS[games_raw_at_visible(i)];
        labels[i] = L(entry->sid, entry->label);
    }
    uint8_t raw = games_normalized_raw(app->games_menu_selected);
    char breadcrumb[8];
    /* Counter keeps the raw STORED ordinal even when gated rows are hidden. */
    draw_flat_list_circular_view(
        fb,
        labels,
        count,
        games_visible_index(raw),
        app->game_option_view_start,
        ui_breadcrumb_path(breadcrumb, sizeof(breadcrumb), "6", (unsigned)(raw + 1u)),
        "Select");
}

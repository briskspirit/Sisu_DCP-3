#include "apps/main_menu_app.h"

#include "app_internal.h"
#include "apps/calculator_app.h"
#include "apps/call_divert_app.h"
#include "apps/call_register_app.h"
#include "apps/clock_app.h"
#include "apps/dialogs_app.h"
#include "apps/games_app.h"
#include "apps/messages_app.h"
#include "apps/net_monitor_app.h"
#include "apps/phonebook_app.h"
#include "apps/profiles_app.h"
#include "apps/settings_app.h"
#include "apps/standby_app.h"
#include "apps/tones_app.h"
#include "services/feature_gates.h"
#include "services/input_keys.h"
#include "services/strings.h"
#include "services/timebase.h"

#define STANDBY_MENU_STAR_ARM_MS 1800u

#define MENU_TEXT_X 0
#define MENU_TEXT_Y 7
#define MENU_TEXT_W 76
#define MENU_VISUAL_X 10
#define MENU_VISUAL_Y 23

typedef struct {
    const char *label;   /* English fallback + documentation */
    uint16_t sid;        /* v6.00 string id; 0 = clone-only (Net Monitor) */
    uint8_t ordinal;
    const uint16_t *frames;
    uint8_t frame_count;
    uint16_t frame_delay_ms;
    uint16_t start_delay_ms;
    uint16_t static_bitmap;
    feature_gate_id_t gate;
} menu_entry_t;

static const uint16_t MENU_ANIM_14[] = {55u, 56u, 57u, 58u, 55u, 56u, 57u, 58u, 55u};
static const uint16_t MENU_ANIM_15[] = {59u, 59u, 60u, 61u, 62u, 63u, 64u, 65u, 66u, 67u, 68u, 69u, 59u, 70u, 71u, 72u, 72u, 71u, 70u, 59u};
static const uint16_t MENU_ANIM_16[] = {73u, 74u, 75u, 76u, 77u, 78u, 79u, 80u, 81u, 82u, 83u, 84u, 73u};
static const uint16_t MENU_ANIM_17[] = {85u, 86u, 87u, 88u, 89u, 90u, 91u, 92u, 92u};
static const uint16_t MENU_ANIM_18[] = {93u, 94u, 95u, 96u, 97u, 98u, 99u, 100u, 101u, 102u, 103u, 93u};
static const uint16_t MENU_ANIM_19[] = {104u, 105u, 106u, 107u, 108u, 109u, 110u, 111u, 112u, 113u, 114u, 115u, 116u, 104u};
static const uint16_t MENU_ANIM_20[] = {117u, 118u, 119u, 120u, 121u, 122u, 123u, 124u, 125u, 126u, 127u, 128u, 129u, 117u};
static const uint16_t MENU_ANIM_21[] = {130u, 131u, 132u, 133u, 134u, 135u, 136u, 137u, 130u};
static const uint16_t MENU_ANIM_22[] = {138u, 139u, 140u, 141u, 142u, 143u, 144u, 145u, 146u, 147u, 148u, 149u, 150u, 151u, 152u, 153u, 154u, 155u, 138u};
static const uint16_t MENU_ANIM_23[] = {156u, 157u, 158u, 159u, 160u, 161u, 162u, 162u, 161u, 160u, 159u, 158u, 157u, 156u};

/* sid = v6.00 string id for the menu title (index+58); 0 = clone-only. Net
 * Monitor has no localized ROM string (it is the one sanctioned deviation). */
static const menu_entry_t MAIN_MENU[] = {
    {"Phone book", 0x2ceu, 1u, MENU_ANIM_14, (uint8_t)ARRAY_COUNT(MENU_ANIM_14), 160u, 960u, 0u, FEATURE_GATE_COUNT},
    {"Messages", 0x06eu, 2u, MENU_ANIM_15, (uint8_t)ARRAY_COUNT(MENU_ANIM_15), 160u, 960u, 0u, FEATURE_GATE_COUNT},
    {"Call register", 0x098u, 3u, MENU_ANIM_16, (uint8_t)ARRAY_COUNT(MENU_ANIM_16), 160u, 960u, 0u, FEATURE_GATE_COUNT},
    {"Settings", 0x2a7u, 4u, MENU_ANIM_17, (uint8_t)ARRAY_COUNT(MENU_ANIM_17), 200u, 1200u, 0u, FEATURE_GATE_COUNT},
    {"Call divert", 0x094u, 5u, MENU_ANIM_18, (uint8_t)ARRAY_COUNT(MENU_ANIM_18), 160u, 960u, 0u, FEATURE_GATE_CALL_DIVERT},
    {"Games", 0x15du, 6u, MENU_ANIM_19, (uint8_t)ARRAY_COUNT(MENU_ANIM_19), 160u, 960u, 0u, FEATURE_GATE_COUNT},
    {"Calculator", 0x088u, 7u, MENU_ANIM_20, (uint8_t)ARRAY_COUNT(MENU_ANIM_20), 240u, 1440u, 0u, FEATURE_GATE_COUNT},
    {"Clock", 0x247u, 8u, MENU_ANIM_21, (uint8_t)ARRAY_COUNT(MENU_ANIM_21), 200u, 1200u, 0u, FEATURE_GATE_COUNT},
    {"Tones", 0x2f8u, 9u, MENU_ANIM_22, (uint8_t)ARRAY_COUNT(MENU_ANIM_22), 160u, 960u, 0u, FEATURE_GATE_COUNT},
    {"Profiles", 0x1cbu, 10u, MENU_ANIM_23, (uint8_t)ARRAY_COUNT(MENU_ANIM_23), 200u, 1200u, 0u, FEATURE_GATE_PROFILES},
    {"Net monitor", 0u, 11u, 0, 0u, 0u, 0u, 163u, FEATURE_GATE_NET_MONITOR},
};

static void leave_menu(app_t *app);
static void reset_menu_animation(app_t *app, uint32_t now_ms);
static const menu_entry_t *current_menu_entry(const app_t *app);
static const char *menu_entry_title(const menu_entry_t *entry);
static uint16_t current_menu_bitmap_id(const app_t *app);
static bool main_menu_entry_visible(uint8_t raw);
static uint8_t main_menu_visible_count(void);
static uint8_t main_menu_visible_index_for_raw(uint8_t raw);
static uint8_t main_menu_raw_at_visible(uint8_t visible);
static uint8_t main_menu_normalized_raw(uint8_t raw);
static void main_menu_step(app_t *app, int8_t delta, uint32_t now_ms);

void open_main_menu(app_t *app, uint32_t now_ms) {
    open_main_menu_at(app, 0u, now_ms);
}

void open_main_menu_at(app_t *app, uint8_t selected, uint32_t now_ms) {
    app->route = APP_ROUTE_MAIN_MENU;
    app->menu_index = main_menu_normalized_raw(selected);
    app->menu_keyguard_armed_until_ms = now_ms + STANDBY_MENU_STAR_ARM_MS;
    app->menu_keyguard_armed = true;
    reset_menu_animation(app, now_ms);
    app->dirty = true;
}

bool handle_main_menu_key(app_t *app, uint16_t key, uint32_t now_ms) {
    bool menu_star_armed = app->menu_keyguard_armed &&
        time_diff_ms(now_ms, app->menu_keyguard_armed_until_ms) <= 0;
    if (menu_star_armed) {
        if (key == KEY_STAR) {
            lock_standby_keyguard(app, now_ms);
            return true;
        }
        app->menu_keyguard_armed = false;
    }
    if (key == KEY_C) {
        leave_menu(app);
        return true;
    }
    if (key == KEY_UP) {
        main_menu_step(app, -1, now_ms);
        return true;
    }
    if (key == KEY_DOWN) {
        main_menu_step(app, 1, now_ms);
        return true;
    }
    if (key == KEY_NAVI) {
        if (current_menu_entry(app)->ordinal == 1u) {
            open_phonebook_menu(app, PHONEBOOK_MENU_ROOT, 0u);
            return true;
        }
        if (current_menu_entry(app)->ordinal == 2u) {
            open_messages_menu(app, 0u);
            return true;
        }
        if (current_menu_entry(app)->ordinal == 3u) {
            open_call_register_menu(app, CALL_REGISTER_MENU_ROOT, 0u);
            return true;
        }
        if (current_menu_entry(app)->ordinal == 4u) {
            open_settings_menu(app, SETTINGS_MENU_ROOT, 0u);
            return true;
        }
        if (current_menu_entry(app)->ordinal == 5u) {
            open_call_divert_menu(app, CALL_DIVERT_MENU_ROOT, 0u);
            return true;
        }
        if (current_menu_entry(app)->ordinal == 6u) {
            /* Games descriptor 0x002dbedc confmode/confid = 1 -> default first row (Rotation). */
            open_games_menu(app, 0u);
            return true;
        }
        if (current_menu_entry(app)->ordinal == 7u) {
            open_calculator(app, now_ms);
            return true;
        }
        if (current_menu_entry(app)->ordinal == 8u) {
            open_clock_root(app);
            return true;
        }
        if (current_menu_entry(app)->ordinal == 9u) {
            open_tones_menu(app, 0u);
            return true;
        }
        if (current_menu_entry(app)->ordinal == 10u) {
            open_profiles_menu(app, profile_active_index());
            return true;
        }
        if (current_menu_entry(app)->ordinal == 11u) {
            open_net_monitor(app, now_ms);
            return true;
        }
        open_display(app, 2u, menu_entry_title(current_menu_entry(app)), "not ported", 0, APP_ROUTE_MAIN_MENU, now_ms);
        return true;
    }
    return false;
}

bool tick_main_menu(app_t *app, uint32_t now_ms) {
    const menu_entry_t *entry = current_menu_entry(app);
    if (entry == 0 || entry->frame_count <= 1u || app->menu_frame_index + 1u >= entry->frame_count) {
        return false;
    }

    if (!app->menu_animation_playing) {
        if (time_diff_ms(now_ms, app->menu_selected_ms + entry->start_delay_ms) < 0) {
            return false;
        }
        app->menu_animation_playing = true;
        app->menu_frame_index = 1u;
        app->menu_next_frame_ms = now_ms + entry->frame_delay_ms;
        return true;
    }

    if (time_diff_ms(now_ms, app->menu_next_frame_ms) < 0) {
        return false;
    }
    app->menu_frame_index++;
    app->menu_next_frame_ms += entry->frame_delay_ms;
    if (app->menu_frame_index + 1u >= entry->frame_count) {
        app->menu_animation_playing = false;
    }
    return true;
}

void render_main_menu(const app_t *app, framebuffer_t *fb) {
    fb_clear(fb, false);
    const menu_entry_t *entry = current_menu_entry(app);
    /* Hardware-validated: the root-menu title is FS0 large/bold, centered
     * in the title window with a +1px ceiling-center bias, position counted
     * pixel-for-pixel on a real Nokia 3210. (A 2026-06-10 attempt to "pin"
     * this to FS2 from window 32's <FS2> attr tag was WRONG — the menu title
     * composer overrides the window's default font; the handset shows FS0.) */
    const font_t *title_font = asset_font(FONT_FS0);
    const char *title = menu_entry_title(entry);
    int title_width = asset_text_width(title_font, title);
    int title_x = MENU_TEXT_X + ((MENU_TEXT_W - title_width + 1) / 2) + 1;
    if (title_x < MENU_TEXT_X) {
        title_x = MENU_TEXT_X;
    }
    fb_text(fb, title_font, title, title_x, MENU_TEXT_Y, true, MENU_TEXT_W - (title_x - MENU_TEXT_X));

    uint16_t bitmap_id = current_menu_bitmap_id(app);
    if (bitmap_id != 0u) {
        fb_bitmap(fb, bitmap_id, MENU_VISUAL_X, MENU_VISUAL_Y, true, true);
    }

    draw_menu_position(fb, main_menu_visible_index_for_raw(app->menu_index), main_menu_visible_count(), entry->ordinal);
    draw_softkey(fb, "Select");
}

static void leave_menu(app_t *app) {
    app->route = APP_ROUTE_STANDBY;
    app->menu_keyguard_armed = false;
    app->dirty = true;
}

static void reset_menu_animation(app_t *app, uint32_t now_ms) {
    app->menu_frame_index = 0u;
    app->menu_animation_playing = false;
    app->menu_selected_ms = now_ms;
    app->menu_next_frame_ms = 0u;
}

static const menu_entry_t *current_menu_entry(const app_t *app) {
    return &MAIN_MENU[main_menu_normalized_raw(app->menu_index)];
}

/* Localized menu title: the v6.00 string for entry->sid, or the English label
 * for a clone-only entry (sid 0) / any sid with no record. */
static const char *menu_entry_title(const menu_entry_t *entry) {
    if (entry->sid != 0u) {
        const char *localized = ts(entry->sid);
        if (localized != 0) {
            return localized;
        }
    }
    return entry->label;
}

static uint16_t current_menu_bitmap_id(const app_t *app) {
    const menu_entry_t *entry = current_menu_entry(app);
    if (entry->frames != 0 && entry->frame_count > 0u) {
        uint8_t index = app->menu_frame_index;
        if (index >= entry->frame_count) {
            index = (uint8_t)(entry->frame_count - 1u);
        }
        return entry->frames[index];
    }
    return entry->static_bitmap;
}

static bool main_menu_entry_visible(uint8_t raw) {
    if (raw >= ARRAY_COUNT(MAIN_MENU)) {
        return false;
    }
    feature_gate_id_t gate = MAIN_MENU[raw].gate;
    return gate == FEATURE_GATE_COUNT || feature_gate_visible(gate);
}

static uint8_t main_menu_visible_count(void) {
    uint8_t count = 0u;
    for (uint8_t i = 0; i < ARRAY_COUNT(MAIN_MENU); i++) {
        if (main_menu_entry_visible(i)) {
            count++;
        }
    }
    return count == 0u ? 1u : count;
}

static uint8_t main_menu_visible_index_for_raw(uint8_t raw) {
    uint8_t visible = 0u;
    for (uint8_t i = 0; i < ARRAY_COUNT(MAIN_MENU); i++) {
        if (!main_menu_entry_visible(i)) {
            continue;
        }
        if (i == main_menu_normalized_raw(raw)) {
            return visible;
        }
        visible++;
    }
    return 0u;
}

static uint8_t main_menu_raw_at_visible(uint8_t visible) {
    uint8_t seen = 0u;
    for (uint8_t i = 0; i < ARRAY_COUNT(MAIN_MENU); i++) {
        if (!main_menu_entry_visible(i)) {
            continue;
        }
        if (seen == visible) {
            return i;
        }
        seen++;
    }
    return 0u;
}

static uint8_t main_menu_normalized_raw(uint8_t raw) {
    if (raw < ARRAY_COUNT(MAIN_MENU) && main_menu_entry_visible(raw)) {
        return raw;
    }
    for (uint8_t offset = 0; offset < ARRAY_COUNT(MAIN_MENU); offset++) {
        uint8_t candidate = (uint8_t)((raw + offset) % ARRAY_COUNT(MAIN_MENU));
        if (main_menu_entry_visible(candidate)) {
            return candidate;
        }
    }
    return 0u;
}

static void main_menu_step(app_t *app, int8_t delta, uint32_t now_ms) {
    uint8_t count = main_menu_visible_count();
    uint8_t visible = main_menu_visible_index_for_raw(app->menu_index);
    visible = (uint8_t)((visible + count + delta) % count);
    app->menu_index = main_menu_raw_at_visible(visible);
    reset_menu_animation(app, now_ms);
    app->dirty = true;
}

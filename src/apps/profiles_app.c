#include "apps/profiles_app.h"

#include <stdio.h>

#include "apps/dialogs_app.h"
#include "apps/main_menu_app.h"
#include "apps/tones_app.h"
#include "services/input_keys.h"
#include "services/strings.h"
#include "services/timebase.h"

#define PROFILE_COUNT 4u             /* user-selectable profiles (menu) */
#define PROFILE_HEADSET_INDEX 4u     /* auto-only accessory profile (not in the menu) */
#define PROFILE_TOTAL 5u             /* incl. the Headset accessory profile */
#define PROFILE_OPTION_COUNT 2u
#define PROFILE_SWITCH_DELAY_MS 1536u

enum {
    PROFILES_VIEW_LIST = 0,
    PROFILES_VIEW_OPTIONS = 1,
};

/* Each menu label carries its v6.00 string id (SID) for localization; the
 * English literal stays inline as BOTH the fallback and documentation. These
 * labels are pure display -- never a strcmp dispatch or storage key (dispatch
 * is by index, storage by the PROFILE_SETTING_KEYS enums) -- so localizing the
 * rendered/returned copy is behavior-neutral. */
typedef struct {
    const char *label;   /* English fallback + documentation */
    uint16_t sid;        /* v6.00 string id; 0 = no 1:1 match, keep the literal */
} profile_label_t;

/* Localize by SID with English fallback: sid 0 (no match) or a sid with no
 * record (ts()==NULL, clone-only) both return the supplied literal. */
static const char *L(uint16_t sid, const char *en) {
    if (sid != 0u) {
        const char *t = ts(sid);
        if (t != 0) {
            return t;
        }
    }
    return en;
}

/* The four user-selectable profiles map 1:1 to the contiguous v6.00 profile-name
 * block 0x2c9-0x2cc (Discreet/Personal/Loud/Silent); three of the four are
 * unambiguous and pin the block, so "Silent" resolves to 0x2cc here (not the
 * other-model profile block's 0x1cf or the SMS block's 0x2ab "Silent").
 * "Headset" is the accessory-profile name at 0x1ce. */
static const profile_label_t PROFILE_LABELS[PROFILE_TOTAL] = {
    {"Personal", 0x2cau},
    {"Silent", 0x2ccu},
    {"Discreet", 0x2c9u},
    {"Loud", 0x2cbu},
    {"Headset", 0x1ceu},
};

/* Profiles options submenu: Personalise=0x1c9 is unambiguous and pins the
 * profiles block; Activate=0x1ca is the immediately-adjacent record in that
 * same block (not the generic 0x3b/0x3c "Activate"). */
static const profile_label_t PROFILE_OPTION_LABELS[PROFILE_OPTION_COUNT] = {
    {"Activate", 0x1cau},
    {"Personalise", 0x1c9u},
};

static const uint8_t PROFILE_DEFAULTS[PROFILE_TOTAL][PROFILE_SETTING_COUNT] = {
    {1u, 52u, 9u, 0u, 1u, 4u, 0u},
    {4u, 52u, 8u, 0u, 255u, 255u, 0u},
    {1u, 52u, 8u, 0u, 1u, 4u, 0u},
    {1u, 52u, 9u, 0u, 1u, 4u, 0u},
    {1u, 52u, 8u, 0u, 1u, 4u, 0u}, /* Headset: factory values from v6.00 (alert=ring, vol=8) */
};

static const store_setting_key_t PROFILE_SETTING_KEYS[PROFILE_TOTAL][PROFILE_SETTING_COUNT] = {
    {
        STORE_SETTING_PROFILE_PERSONAL_INCOMING_ALERT,
        STORE_SETTING_PROFILE_PERSONAL_RINGING_TONE,
        STORE_SETTING_PROFILE_PERSONAL_RINGING_VOLUME,
        STORE_SETTING_PROFILE_PERSONAL_MESSAGE_ALERT,
        STORE_SETTING_PROFILE_PERSONAL_KEYPAD_TONES,
        STORE_SETTING_PROFILE_PERSONAL_WARNING_GAME_TONES,
        STORE_SETTING_PROFILE_PERSONAL_VIBRATING_ALERT,
    },
    {
        STORE_SETTING_PROFILE_SILENT_INCOMING_ALERT,
        STORE_SETTING_PROFILE_SILENT_RINGING_TONE,
        STORE_SETTING_PROFILE_SILENT_RINGING_VOLUME,
        STORE_SETTING_PROFILE_SILENT_MESSAGE_ALERT,
        STORE_SETTING_PROFILE_SILENT_KEYPAD_TONES,
        STORE_SETTING_PROFILE_SILENT_WARNING_GAME_TONES,
        STORE_SETTING_PROFILE_SILENT_VIBRATING_ALERT,
    },
    {
        STORE_SETTING_PROFILE_DISCREET_INCOMING_ALERT,
        STORE_SETTING_PROFILE_DISCREET_RINGING_TONE,
        STORE_SETTING_PROFILE_DISCREET_RINGING_VOLUME,
        STORE_SETTING_PROFILE_DISCREET_MESSAGE_ALERT,
        STORE_SETTING_PROFILE_DISCREET_KEYPAD_TONES,
        STORE_SETTING_PROFILE_DISCREET_WARNING_GAME_TONES,
        STORE_SETTING_PROFILE_DISCREET_VIBRATING_ALERT,
    },
    {
        STORE_SETTING_PROFILE_LOUD_INCOMING_ALERT,
        STORE_SETTING_PROFILE_LOUD_RINGING_TONE,
        STORE_SETTING_PROFILE_LOUD_RINGING_VOLUME,
        STORE_SETTING_PROFILE_LOUD_MESSAGE_ALERT,
        STORE_SETTING_PROFILE_LOUD_KEYPAD_TONES,
        STORE_SETTING_PROFILE_LOUD_WARNING_GAME_TONES,
        STORE_SETTING_PROFILE_LOUD_VIBRATING_ALERT,
    },
    {
        STORE_SETTING_PROFILE_HEADSET_INCOMING_ALERT,
        STORE_SETTING_PROFILE_HEADSET_RINGING_TONE,
        STORE_SETTING_PROFILE_HEADSET_RINGING_VOLUME,
        STORE_SETTING_PROFILE_HEADSET_MESSAGE_ALERT,
        STORE_SETTING_PROFILE_HEADSET_KEYPAD_TONES,
        STORE_SETTING_PROFILE_HEADSET_WARNING_GAME_TONES,
        STORE_SETTING_PROFILE_HEADSET_VIBRATING_ALERT,
    },
};

static const store_setting_key_t PROFILE_ACTIVE_KEYS[PROFILE_SETTING_COUNT] = {
    STORE_SETTING_PROFILE_INCOMING_ALERT,
    STORE_SETTING_PROFILE_RINGING_TONE,
    STORE_SETTING_PROFILE_RINGING_VOLUME,
    STORE_SETTING_PROFILE_MESSAGE_ALERT,
    STORE_SETTING_PROFILE_KEYPAD_TONES,
    STORE_SETTING_PROFILE_WARNING_GAME_TONES,
    STORE_SETTING_PROFILE_VIBRATING_ALERT,
};

static uint8_t normalize_profile_index(uint8_t index);
static uint8_t profile_menu_index(uint8_t index);
static profile_setting_kind_t normalize_profile_setting_kind(profile_setting_kind_t kind);
static bool profile_setting_value_valid(profile_setting_kind_t kind, uint8_t value);
static uint8_t normalize_profile_setting_value(uint8_t profile_index, profile_setting_kind_t kind, uint8_t value);
static void set_active_profile(uint8_t index);
static void profile_sync_active_settings(void);
static void activate_selected_profile(app_t *app, uint32_t now);
static void finish_profile_activation(app_t *app, uint32_t now);

void profiles_app_init(app_t *app) {
    (void)app;
    /* If powered off while the Headset profile was active, the persisted active
     * index is the headset index (4) -- not a user-selectable profile. Restore the
     * saved user profile so the Profiles menu selection stays in range; the boot
     * insert-edge re-activates Headset if one is actually still connected. */
    if (profile_active_index() == PROFILE_HEADSET_INDEX) {
        profile_restore_from_headset();
    }
    uint8_t active = profile_active_index();
    store_setting_set_u8(STORE_SETTING_PROFILE_ACTIVE, active);
    profile_sync_active_settings();
}

void open_profiles_menu(app_t *app, uint8_t selected) {
    app->route = APP_ROUTE_PROFILES_MENU;
    app->profiles_menu_kind = PROFILES_VIEW_LIST;
    app->profiles_selected_index = profile_menu_index(selected);
    app->profiles_option_index = 0u;
    app->dirty = true;
}

void open_profiles_options(app_t *app, uint8_t selected_profile) {
    app->route = APP_ROUTE_PROFILES_MENU;
    app->profiles_menu_kind = PROFILES_VIEW_OPTIONS;
    app->profiles_selected_index = profile_menu_index(selected_profile);
    app->profiles_option_index = 0u;
    app->dirty = true;
}

bool handle_profiles_key(app_t *app, uint16_t key, uint32_t now) {
    if (app->profiles_menu_kind == PROFILES_VIEW_OPTIONS) {
        if (key == KEY_UP || key == KEY_DOWN) {
            app->profiles_option_index ^= 1u;
            app->dirty = true;
            return true;
        }
        if (key == KEY_C) {
            open_profiles_menu(app, app->profiles_selected_index);
            return true;
        }
        if (key == KEY_NAVI) {
            if (app->profiles_option_index == 0u) {
                activate_selected_profile(app, now);
            } else {
                open_tones_personalise(app, app->profiles_selected_index, 0u);
            }
            return true;
        }
        return true;
    }

    if (key == KEY_UP) {
        app->profiles_selected_index = app->profiles_selected_index == 0u
            ? (uint8_t)(PROFILE_COUNT - 1u)
            : (uint8_t)(app->profiles_selected_index - 1u);
        app->dirty = true;
        return true;
    }
    if (key == KEY_DOWN) {
        app->profiles_selected_index = (uint8_t)((app->profiles_selected_index + 1u) % PROFILE_COUNT);
        app->dirty = true;
        return true;
    }
    if (key == KEY_C) {
        open_main_menu_at(app, 9u, now);
        return true;
    }
    if (key == KEY_NAVI) {
        open_profiles_options(app, app->profiles_selected_index);
        return true;
    }
    return true;
}

bool tick_profiles(app_t *app, uint32_t now) {
    if (app->profiles_switch_started_ms == 0u) {
        return false;
    }
    if (app->route != APP_ROUTE_DISPLAY_MESSAGE || app->display_record_id != 4u) {
        app->profiles_switch_started_ms = 0u;
        return false;
    }
    if (time_diff_ms(now, app->profiles_switch_started_ms + PROFILE_SWITCH_DELAY_MS) < 0) {
        return false;
    }
    finish_profile_activation(app, now);
    return true;
}

void render_profiles(const app_t *app, framebuffer_t *fb) {
    char breadcrumb[8];
    if (app->profiles_menu_kind == PROFILES_VIEW_OPTIONS) {
        /* Localized copy for display; .label stays the inline fallback. The
         * "Select" softkey is ambiguous across 3 context SIDs -> kept English. */
        const char *localized[PROFILE_OPTION_COUNT];
        for (unsigned i = 0; i < PROFILE_OPTION_COUNT; i++) {
            localized[i] = L(PROFILE_OPTION_LABELS[i].sid, PROFILE_OPTION_LABELS[i].label);
        }
        draw_flat_list(fb,
                       localized,
                       PROFILE_OPTION_COUNT,
                       app->profiles_option_index,
                       ui_breadcrumb_path(breadcrumb, sizeof(breadcrumb), "10", (unsigned)(app->profiles_option_index + 1u)),
                       "Select");
        return;
    }
    /* Localized copy for display; the "Options" softkey is ambiguous across 3
     * context SIDs -> kept English. Width/centering runs on the localized text. */
    const char *localized[PROFILE_COUNT];
    for (unsigned i = 0; i < PROFILE_COUNT; i++) {
        localized[i] = L(PROFILE_LABELS[i].sid, PROFILE_LABELS[i].label);
    }
    draw_flat_list(fb,
                   localized,
                   PROFILE_COUNT,
                   app->profiles_selected_index,
                   ui_breadcrumb_path(breadcrumb, sizeof(breadcrumb), "10", (unsigned)(app->profiles_selected_index + 1u)),
                   "Options");
}

uint8_t profile_count(void) {
    return PROFILE_COUNT;
}

uint8_t profile_active_index(void) {
    uint8_t active = 0u;
    store_setting_get_u8(STORE_SETTING_PROFILE_ACTIVE, &active);
    return normalize_profile_index(active);
}

const char *profile_label(uint8_t profile_index) {
    const profile_label_t *entry = &PROFILE_LABELS[normalize_profile_index(profile_index)];
    return L(entry->sid, entry->label);
}

const char *profile_active_standby_label(void) {
    uint8_t active = profile_active_index();
    if (active == 0u) {
        return "";
    }
    return L(PROFILE_LABELS[active].sid, PROFILE_LABELS[active].label);
}

bool profile_active_is_silent(void) {
    return profile_active_index() == 1u;
}

void profile_activate(uint8_t profile_index) {
    set_active_profile(profile_index);
}

uint8_t profile_default_setting_value(uint8_t profile_index, profile_setting_kind_t kind) {
    return PROFILE_DEFAULTS[normalize_profile_index(profile_index)]
                           [normalize_profile_setting_kind(kind)];
}

uint8_t profile_get_tone_setting(uint8_t profile_index, profile_setting_kind_t kind) {
    profile_index = normalize_profile_index(profile_index);
    kind = normalize_profile_setting_kind(kind);
    uint8_t value = PROFILE_DEFAULTS[profile_index][kind];
    store_setting_get_u8(PROFILE_SETTING_KEYS[profile_index][kind], &value);
    return normalize_profile_setting_value(profile_index, kind, value);
}

void profile_set_tone_setting(uint8_t profile_index, profile_setting_kind_t kind, uint8_t value) {
    profile_index = normalize_profile_index(profile_index);
    kind = normalize_profile_setting_kind(kind);
    value = normalize_profile_setting_value(profile_index, kind, value);
    store_setting_set_u8(PROFILE_SETTING_KEYS[profile_index][kind], value);
    if (profile_index == profile_active_index()) {
        store_setting_set_u8(PROFILE_ACTIVE_KEYS[kind], value);
    }
}

static void profile_sync_active_settings(void) {
    uint8_t active = profile_active_index();
    for (uint8_t i = 0; i < PROFILE_SETTING_COUNT; i++) {
        store_setting_set_u8(PROFILE_ACTIVE_KEYS[i],
                             profile_get_tone_setting(active, (profile_setting_kind_t)i));
    }
}

static uint8_t normalize_profile_index(uint8_t index) {
    /* PROFILE_TOTAL includes the auto-only Headset profile (index 4); the menu
     * still iterates only PROFILE_COUNT, so Headset is never user-selectable. */
    return index < PROFILE_TOTAL ? index : 0u;
}

static uint8_t profile_menu_index(uint8_t index) {
    if (index < PROFILE_COUNT) {
        return index;
    }
    if (index == PROFILE_HEADSET_INDEX) {
        uint8_t saved = 0u;
        if (store_setting_get_u8(STORE_SETTING_PROFILE_SAVED, &saved) ==
                STORE_STATUS_OK &&
            saved < PROFILE_COUNT) {
            return saved;
        }
    }
    return 0u;
}

void profile_activate_headset(void) {
    uint8_t active = profile_active_index();
    if (active == PROFILE_HEADSET_INDEX) {
        return; /* already on the headset profile */
    }
    store_setting_set_u8(STORE_SETTING_PROFILE_SAVED, active); /* save user profile to restore */
    set_active_profile(PROFILE_HEADSET_INDEX);
}

void profile_restore_from_headset(void) {
    if (profile_active_index() != PROFILE_HEADSET_INDEX) {
        return; /* not on the headset profile -- nothing to restore */
    }
    uint8_t saved = 0u;
    store_setting_get_u8(STORE_SETTING_PROFILE_SAVED, &saved);
    if (saved >= PROFILE_COUNT) {
        saved = 0u; /* only restore a user-selectable profile (default Personal) */
    }
    store_setting_set_u8(STORE_SETTING_PROFILE_SAVED, 0xffu);
    set_active_profile(saved);
}

void profiles_restore_factory_defaults(void) {
    /* v6.00 restore dispatcher 0x00292878 resets the per-profile tone
     * configuration. Keep profile selection state intact: an inserted Headset
     * remains the active accessory profile, while all five editable profiles
     * return to their traced defaults. */
    for (uint8_t profile = 0u; profile < PROFILE_TOTAL; profile++) {
        for (uint8_t kind = 0u; kind < PROFILE_SETTING_COUNT; kind++) {
            store_setting_set_u8(PROFILE_SETTING_KEYS[profile][kind],
                                 PROFILE_DEFAULTS[profile][kind]);
        }
    }
    profile_sync_active_settings();
}

static profile_setting_kind_t normalize_profile_setting_kind(profile_setting_kind_t kind) {
    return kind < PROFILE_SETTING_COUNT ? kind : PROFILE_SETTING_VIBRATING_ALERT;
}

static bool profile_setting_value_valid(profile_setting_kind_t kind, uint8_t value) {
    switch (kind) {
    case PROFILE_SETTING_INCOMING_ALERT:
        return value == 1u || value == 2u || value == 4u || value == 5u || value == 6u;
    case PROFILE_SETTING_RINGING_TONE:
        return value == 18u || value == 19u || value == 20u || value == 21u ||
               value == 23u || value == 25u || value == 26u || value == 31u ||
               value == 34u || value == 38u || value == 42u || value == 43u ||
               value == 47u || value == 48u || value == 50u || value == 52u ||
               value == 54u || value == 55u || value == 60u || value == 62u ||
               value == 63u || value == 64u || value == 69u || value == 73u ||
               value == 76u || value == 78u || value == 85u || value == 90u ||
               value == 91u || value == 93u || value == 97u || value == 98u ||
               value == 99u || value == 102u || value == 103u || value == 104u ||
               value == 106u || value == 107u || value == 108u || value == 109u;
    case PROFILE_SETTING_RINGING_VOLUME:
        return value >= 6u && value <= 10u;
    case PROFILE_SETTING_MESSAGE_ALERT:
        return value <= 4u;
    case PROFILE_SETTING_KEYPAD_TONES:
        return value == 0u || value == 1u || value == 2u || value == 255u;
    case PROFILE_SETTING_WARNING_GAME_TONES:
        return value == 4u || value == 255u;
    case PROFILE_SETTING_VIBRATING_ALERT:
        return value == 0u || value == 1u;
    default:
        return false;
    }
}

static uint8_t normalize_profile_setting_value(uint8_t profile_index, profile_setting_kind_t kind, uint8_t value) {
    return profile_setting_value_valid(kind, value)
        ? value
        : PROFILE_DEFAULTS[normalize_profile_index(profile_index)][normalize_profile_setting_kind(kind)];
}

static void set_active_profile(uint8_t index) {
    store_setting_set_u8(STORE_SETTING_PROFILE_ACTIVE, normalize_profile_index(index));
    profile_sync_active_settings();
}

static void activate_selected_profile(app_t *app, uint32_t now) {
    app->profiles_pending_index = normalize_profile_index(app->profiles_selected_index);
    app->profiles_switch_started_ms = now;
    app->profiles_menu_kind = PROFILES_VIEW_LIST;
    /* v6.00 SID 0x3b7 = "Switching\nprofile" (exact, unambiguous multiline). */
    open_display_sid(app, 4u, 0x3b7u, "Switching\nprofile", APP_ROUTE_PROFILES_MENU, now);
}

static void finish_profile_activation(app_t *app, uint32_t now) {
    app->profiles_switch_started_ms = 0u;
    app->profiles_selected_index = normalize_profile_index(app->profiles_pending_index);
    app->profiles_menu_kind = PROFILES_VIEW_LIST;
    set_active_profile(app->profiles_selected_index);
    /* The v6.00 message SID 0x1cc is "Selected\nprofile:\n%U" -- its 3rd line is
     * a dynamic %U substitution (the profile name), which open_display_sid cannot
     * fill. The static lines have no standalone SID, so keep the English literals;
     * the profile-name line localizes via profile_label() below. */
    open_display(app,
                 3u,
                 "Selected",
                 "profile:",
                 profile_label(app->profiles_selected_index),
                 APP_ROUTE_PROFILES_MENU,
                 now);
}

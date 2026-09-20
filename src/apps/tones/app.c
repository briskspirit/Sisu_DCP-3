#include "apps/tones_app.h"
#include "tones_internal.h"
#include "ui/menu_visible.h"

#include <stdbool.h>
#include <stdio.h>

#include "audio/audio_levels.h"
#include "apps/dialogs_app.h"
#include "apps/main_menu_app.h"
#include "apps/profiles_app.h"
#include "services/core1_services.h"
#include "services/feature_gates.h"
#include "services/strings.h"
#include "generated/tones.h"
#include "services/input_keys.h"
#include "storage/store_service.h"

/* v6.00 ringing-tone and ringing-volume editors stop the old preview on event
 * 0x038c, then arm timer 0x30 for 0x80 ticks. The phone's timer quantum is
 * 8 ms, giving a restartable 1024 ms debounce before event 0x059b plays it. */
#define TONES_RING_PREVIEW_DELAY_MS 1024u

/* A plain menu label paired with its v6.00 string id (SID) for localization.
 * The English `label` stays BOTH the fallback and any strcmp/parse key; only a
 * rendered copy is localized (via ts_or). sid 0 = no 1:1 v6.00 match (the ROM
 * string wraps/spaces differently or is ambiguous across contexts) -> keep the
 * English literal. */
typedef struct {
    const char *label;
    uint16_t sid;
} tones_label_t;

typedef struct {
    const char *label;
    uint8_t value;
    int8_t ringtone_index;
    uint16_t sid; /* v6.00 SID for the special slots (Own/Received tone); 0 = a
                   * melody catalogue title with no v6.00 record -> keep literal */
} ringtone_option_t;

typedef struct {
    const char *label;
    uint8_t value;
    uint16_t sid; /* v6.00 SID for the localized display copy; 0 = keep literal */
} tone_option_t;

/* Pass 2: these root labels ARE this app's screens (verified against the v6.00 UI
 * traces), so each resolves to its exact SID. The fallback keeps the clone's
 * spacing, but wrap_text_lines respects the ROM string's own "\n" at runtime. */
static const tones_label_t TONES_ROOT_LABELS[] = {
    {"Incoming call\nalert", 0x26au},    /* ROM 618 "Incoming\ncall alert" */
    {"Ringing tone", 0x26bu},            /* ROM 619 "Ringing\ntone" */
    {"Composer", 0x0e3u},                /* ROM 227 "Composer" */
    {"Ringing volume", 0x26du},          /* ROM 621 "Ringing\nvolume" */
    {"Message alert\ntone", 0x368u},     /* ROM 872 "Message\nalert tone" */
    {"Keypad tones", 0x197u},            /* ROM 407 "Keypad\ntones" */
    {"Warning and\ngame tones", 0x403u}, /* ROM 1027 "Warning and game tones" */
    {"Vibrating alert", 0x3f4u},         /* ROM 1012 "Vibrating\nalert" */
};

static const tone_option_t INCOMING_ALERT_OPTIONS[] = {
    /* SIDs disambiguated by string-table block: records 613..617 (0x265..0x269)
     * are exactly this menu's five options (Ascending/Beep once/Off/Ring once/
     * Ringing), adjacent to the already-pinned 0x268/0x269. The other "Ascending"
     * (0x36a) and "Beep once" (0x369) belong to the Message-alert block. */
    {"Ringing", 1u, 0x269u},
    {"Ascending", 6u, 0x265u},
    {"Ring once", 5u, 0x268u},
    {"Beep once", 2u, 0x266u},
    {"Off", 4u, 0x267u},
};
static const tone_option_t RINGING_VOLUME_OPTIONS[] = {
    {"Level 1", 6u, 0u}, /* ROM 408/622 "Level %N" is a format string, not this literal -> keep */
    {"Level 2", 7u, 0u},
    {"Level 3", 8u, 0u},
    {"Level 4", 9u, 0u},
    {"Level 5", 10u, 0u},
};
static const tone_option_t MESSAGE_ALERT_OPTIONS[] = {
    {"No tone", 0u, 0x36bu}, /* ROM 875 (the message-alert block 875/876/877); 0x0ce=206 is the phonebook picker's "No tone" */
    {"Standard", 1u, 0x36du},
    {"Special", 2u, 0x36cu},
    {"Beep once", 3u, 0x369u}, /* message-alert block (not the 0x266 incoming one) */
    {"Ascending", 4u, 0x36au}, /* message-alert block (not the 0x265 incoming one) */
};
static const tone_option_t KEYPAD_TONE_OPTIONS[] = {
    {"Off", 255u, 0u},   /* ambiguous -> keep literal */
    {"Level 1", 0u, 0u}, /* ROM "Level %N" format -> keep literal */
    {"Level 2", 1u, 0u},
    {"Level 3", 2u, 0u},
};
static const tone_option_t ON_OFF_OPTIONS[] = {
    {"On", 4u, 0u},    /* ambiguous -> keep literal */
    {"Off", 255u, 0u}, /* ambiguous -> keep literal */
};
static const tone_option_t VIBRA_OPTIONS[] = {
    {"On", 1u, 0u},  /* ambiguous -> keep literal */
    {"Off", 0u, 0u}, /* ambiguous -> keep literal */
};

/* .sid: melody catalogue titles have no v6.00 string record (sid 0 -> literal);
 * only the two special slots "Received tone"/"Own tone" are localized UI strings. */
static const ringtone_option_t RINGING_TONE_OPTIONS[] = {
    {"Ring ring", 20u, 0, 0u},
    {"Low", 21u, 1, 0u},
    {"Intro", 26u, 2, 0u},
    {"Mosquito", 23u, 3, 0u},
    {"Samba", 73u, 4, 0u},
    {"Bee", 25u, 5, 0u},
    {"Nokia tune", 52u, 6, 0u},
    {"City bird", 31u, 7, 0u},
    {"Attraction", 63u, 8, 0u},
    {"Cladoceran", 69u, 9, 0u},
    {"Dawn", 64u, 10, 0u},
    {"Kick", 38u, 11, 0u},
    {"Bumblebee", 108u, 12, 0u},
    {"That's it!", 47u, 13, 0u},
    {"Chase", 34u, 14, 0u},
    {"Mexican Hat Dance", 98u, 15, 0u},
    {"Entertainer", 99u, 16, 0u},
    {"Playground", 43u, 17, 0u},
    {"Fuga", 54u, 18, 0u},
    {"Rocket", 50u, 19, 0u},
    {"Happy return", 42u, 20, 0u},
    {"Valkyrie", 103u, 21, 0u},
    {"Auld Lang Syne", 102u, 22, 0u},
    {"Polska", 62u, 23, 0u},
    {"Progress", 48u, 24, 0u},
    {"Jumping", 78u, 25, 0u},
    {"Menuet", 55u, 26, 0u},
    {"William Tell", 60u, 27, 0u},
    {"Charleston", 76u, 28, 0u},
    {"Matilda", 93u, 29, 0u},
    {"Brave Scotland", 91u, 30, 0u},
    {"Reveille", 97u, 31, 0u},
    {"Jingle bells", 90u, 32, 0u},
    {"Four seasons", 85u, 33, 0u},
    {"Bossanova", 104u, 34, 0u},
    {"HipHop", 106u, 35, 0u},
    {"Salsa", 107u, 36, 0u},
    {"Hurdy-gurdy", 109u, 37, 0u},
    {"Received tone", 18u, -1, 0x227u}, /* ROM 551 "Received tone" */
    {"Own tone", 19u, -1, 0x0fau},      /* ROM 250 "Own tone" */
};

static profile_setting_kind_t tones_profile_kind(tones_setting_kind_t kind);
static uint8_t tones_setting_count(tones_setting_kind_t kind);
static const char *tones_setting_label(tones_setting_kind_t kind, uint8_t index);
static uint8_t tones_setting_value(tones_setting_kind_t kind, uint8_t index);
static uint8_t tones_default_value(const app_t *app, tones_setting_kind_t kind);
static uint8_t tones_current_value(const app_t *app, tones_setting_kind_t kind);
static uint8_t tones_option_index_for_value(const app_t *app, tones_setting_kind_t kind, uint8_t value);
static const char *tones_value_label(const app_t *app, tones_setting_kind_t kind, uint8_t value);
static tones_setting_kind_t tones_kind_for_root(uint8_t selected);
static bool tones_root_is_setting(uint8_t selected);
static void open_tones_setting(app_t *app, tones_setting_kind_t kind);
static void save_tones_setting(app_t *app, uint32_t now);
static void commit_tones_setting(app_t *app, uint32_t now, bool confirmed_level5);
static void preview_tones_setting(const app_t *app, tones_setting_kind_t kind, uint8_t value, uint8_t selected);
static void preview_current_selection(const app_t *app);
static void stop_tones_preview(void);
static void cancel_tones_preview(app_t *app);
static void schedule_current_selection_preview(app_t *app, uint32_t now);
static void post_ringtone_preview(const app_t *app, uint8_t ringtone_index, uint8_t level);
static void post_ringtone_preview_by_value(const app_t *app, uint8_t ringtone_value, uint8_t level);
static bool tones_vibra_preview_enabled(const app_t *app);
static uint8_t current_ringing_volume(const app_t *app);
static void tones_return_to_parent(app_t *app, uint32_t now);
static bool tones_root_item_visible(uint8_t raw);
static uint8_t tones_root_visible_count(void);
static uint8_t tones_root_visible_index(uint8_t raw);
static uint8_t tones_root_raw_at_visible(uint8_t visible);
static uint8_t tones_root_normalized_raw(uint8_t raw);
static void tones_menu_step(app_t *app, int8_t delta);
static bool ringing_tone_row_visible(uint8_t raw);
static uint8_t ringing_tone_visible_count(void);
static uint8_t ringing_tone_raw_at_visible(uint8_t visible);
void open_tones_menu(app_t *app, uint8_t selected) {
    app->tones_preview_pending = false;
    app->route = APP_ROUTE_TONES_MENU;
    app->tones_from_profiles = false;
    app->tones_profile_index = profile_active_index();
    app->tones_menu_selected = tones_root_normalized_raw(selected);
    app->dirty = true;
}

void open_tones_personalise(app_t *app, uint8_t profile_index, uint8_t selected) {
    app->tones_preview_pending = false;
    app->route = APP_ROUTE_TONES_MENU;
    app->tones_from_profiles = true;
    app->tones_profile_index = profile_index < profile_count() ? profile_index : 0u;
    app->tones_menu_selected = tones_root_normalized_raw(selected);
    app->dirty = true;
}

bool handle_tones_menu_key(app_t *app, uint16_t key, uint32_t now) {
    if (key == KEY_C) {
        tones_return_to_parent(app, now);
        return true;
    }
    if (key == KEY_UP) {
        tones_menu_step(app, -1);
        return true;
    }
    if (key == KEY_DOWN) {
        tones_menu_step(app, 1);
        return true;
    }
    if (key == KEY_NAVI) {
        if (!tones_root_is_setting(app->tones_menu_selected)) {
            open_tone_composer(app, now);
            return true;
        }
        open_tones_setting(app, tones_kind_for_root(app->tones_menu_selected));
        return true;
    }
    return false;
}

bool handle_tones_setting_key(app_t *app, uint16_t key, uint32_t now) {
    tones_setting_kind_t kind = (tones_setting_kind_t)app->tones_setting_kind;
    uint8_t count = tones_setting_count(kind);
    if (count == 0u) {
        app->route = APP_ROUTE_TONES_MENU;
        app->dirty = true;
        return true;
    }
    if (key == KEY_C) {
        cancel_tones_preview(app);
        app->route = APP_ROUTE_TONES_MENU;
        app->dirty = true;
        return true;
    }
    if (key == KEY_UP) {
        ui_circular_list_step_3rows(count, -1,
                                    &app->tones_setting_selected,
                                    &app->tones_setting_view_start);
        schedule_current_selection_preview(app, now);
        app->dirty = true;
        return true;
    }
    if (key == KEY_DOWN) {
        ui_circular_list_step_3rows(count, 1,
                                    &app->tones_setting_selected,
                                    &app->tones_setting_view_start);
        schedule_current_selection_preview(app, now);
        app->dirty = true;
        return true;
    }
    if (key == KEY_NAVI) {
        save_tones_setting(app, now);
        return true;
    }
    return false;
}

void render_tones_menu(const app_t *app, framebuffer_t *fb) {
    uint8_t selected = tones_root_normalized_raw(app->tones_menu_selected);
    uint8_t visible_selected = tones_root_visible_index(selected);

    char breadcrumb[16];
    if (app->tones_from_profiles) {
        snprintf(breadcrumb,
                 sizeof(breadcrumb),
                 "10-%u-2-%u",
                 (unsigned)(app->tones_profile_index + 1u),
                 (unsigned)(selected + 1u));
    } else {
        snprintf(breadcrumb, sizeof(breadcrumb), "9-%u", (unsigned)(selected + 1u));
    }

    char preview[18];
    preview[0] = '\0';
    if (tones_root_is_setting(selected)) {
        tones_setting_kind_t kind = tones_kind_for_root(selected);
        snprintf(preview, sizeof(preview), "%s", tones_value_label(app, kind, tones_current_value(app, kind)));
    }

    draw_static_page_list(fb,
                          ts_or(TONES_ROOT_LABELS[selected].sid, TONES_ROOT_LABELS[selected].label),
                          preview,
                          visible_selected,
                          tones_root_visible_count(),
                          breadcrumb,
                          "Select");
}

void render_tones_setting(const app_t *app, framebuffer_t *fb) {
    tones_setting_kind_t kind = (tones_setting_kind_t)app->tones_setting_kind;
    uint8_t count = tones_setting_count(kind);
    const char *labels[40];
    if (count > ARRAY_COUNT(labels)) {
        count = ARRAY_COUNT(labels);
    }
    for (uint8_t i = 0; i < count; i++) {
        labels[i] = tones_setting_label(kind, i);
    }
    char parent[16];
    if (app->tones_from_profiles) {
        snprintf(parent,
                 sizeof(parent),
                 "10-%u-2-%u",
                 (unsigned)(app->tones_profile_index + 1u),
                 (unsigned)(app->tones_menu_selected + 1u));
    } else {
        snprintf(parent,
                 sizeof(parent),
                 "9-%u",
                 (unsigned)(app->tones_menu_selected + 1u));
    }
    char breadcrumb[20];
    ui_breadcrumb_path(breadcrumb, sizeof(breadcrumb), parent,
                       (unsigned)(app->tones_setting_selected + 1u));
    draw_flat_list_circular_view(fb, labels, count,
                                 app->tones_setting_selected,
                                 app->tones_setting_view_start,
                                 breadcrumb, "OK");
}

static profile_setting_kind_t tones_profile_kind(tones_setting_kind_t kind) {
    switch (kind) {
    case TONES_SETTING_INCOMING_ALERT:
        return PROFILE_SETTING_INCOMING_ALERT;
    case TONES_SETTING_RINGING_TONE:
        return PROFILE_SETTING_RINGING_TONE;
    case TONES_SETTING_RINGING_VOLUME:
        return PROFILE_SETTING_RINGING_VOLUME;
    case TONES_SETTING_MESSAGE_ALERT:
        return PROFILE_SETTING_MESSAGE_ALERT;
    case TONES_SETTING_KEYPAD_TONES:
        return PROFILE_SETTING_KEYPAD_TONES;
    case TONES_SETTING_WARNING_GAME_TONES:
        return PROFILE_SETTING_WARNING_GAME_TONES;
    case TONES_SETTING_VIBRATING_ALERT:
    default:
        return PROFILE_SETTING_VIBRATING_ALERT;
    }
}

/* SET.9: Received tone (18) / Own tone (19) are conditional runtime slots —
 * rows are present only when such a melody exists. */
static bool ringing_tone_row_visible(uint8_t raw) {
    uint8_t value = RINGING_TONE_OPTIONS[raw].value;
    if (value == 18u) {
        return store_own_tone_used(1u);
    }
    if (value == 19u) {
        return store_own_tone_used(0u);
    }
    return true;
}

static uint8_t ringing_tone_visible_count(void) {
    return ui_menu_visible_count(
        ringing_tone_row_visible, (uint8_t)ARRAY_COUNT(RINGING_TONE_OPTIONS));
}

static uint8_t ringing_tone_raw_at_visible(uint8_t visible) {
    return ui_menu_raw_at_visible(
        ringing_tone_row_visible, (uint8_t)ARRAY_COUNT(RINGING_TONE_OPTIONS),
        visible);
}

static uint8_t tones_setting_count(tones_setting_kind_t kind) {
    switch (kind) {
    case TONES_SETTING_INCOMING_ALERT:
        return (uint8_t)ARRAY_COUNT(INCOMING_ALERT_OPTIONS);
    case TONES_SETTING_RINGING_TONE:
        return ringing_tone_visible_count();
    case TONES_SETTING_RINGING_VOLUME:
        return (uint8_t)ARRAY_COUNT(RINGING_VOLUME_OPTIONS);
    case TONES_SETTING_MESSAGE_ALERT:
        return (uint8_t)ARRAY_COUNT(MESSAGE_ALERT_OPTIONS);
    case TONES_SETTING_KEYPAD_TONES:
        return (uint8_t)ARRAY_COUNT(KEYPAD_TONE_OPTIONS);
    case TONES_SETTING_WARNING_GAME_TONES:
        return (uint8_t)ARRAY_COUNT(ON_OFF_OPTIONS);
    case TONES_SETTING_VIBRATING_ALERT:
    default:
        return (uint8_t)ARRAY_COUNT(VIBRA_OPTIONS);
    }
}

static const char *tones_setting_label(tones_setting_kind_t kind, uint8_t index) {
    switch (kind) {
    case TONES_SETTING_INCOMING_ALERT:
        return ts_or(INCOMING_ALERT_OPTIONS[index].sid, INCOMING_ALERT_OPTIONS[index].label);
    case TONES_SETTING_RINGING_TONE: {
        /* Melody titles are catalogue content (sid 0 -> literal); only the special
         * "Own tone"/"Received tone" slots carry a v6.00 SID and get localized. */
        const ringtone_option_t *opt =
            &RINGING_TONE_OPTIONS[ringing_tone_raw_at_visible(index)];
        return ts_or(opt->sid, opt->label);
    }
    case TONES_SETTING_RINGING_VOLUME:
        return ts_or(RINGING_VOLUME_OPTIONS[index].sid, RINGING_VOLUME_OPTIONS[index].label);
    case TONES_SETTING_MESSAGE_ALERT:
        return ts_or(MESSAGE_ALERT_OPTIONS[index].sid, MESSAGE_ALERT_OPTIONS[index].label);
    case TONES_SETTING_KEYPAD_TONES:
        return ts_or(KEYPAD_TONE_OPTIONS[index].sid, KEYPAD_TONE_OPTIONS[index].label);
    case TONES_SETTING_WARNING_GAME_TONES:
        return ts_or(ON_OFF_OPTIONS[index].sid, ON_OFF_OPTIONS[index].label);
    case TONES_SETTING_VIBRATING_ALERT:
    default:
        return ts_or(VIBRA_OPTIONS[index].sid, VIBRA_OPTIONS[index].label);
    }
}

static uint8_t tones_setting_value(tones_setting_kind_t kind, uint8_t index) {
    switch (kind) {
    case TONES_SETTING_INCOMING_ALERT:
        return INCOMING_ALERT_OPTIONS[index].value;
    case TONES_SETTING_RINGING_TONE:
        return RINGING_TONE_OPTIONS[ringing_tone_raw_at_visible(index)].value;
    case TONES_SETTING_RINGING_VOLUME:
        return RINGING_VOLUME_OPTIONS[index].value;
    case TONES_SETTING_MESSAGE_ALERT:
        return MESSAGE_ALERT_OPTIONS[index].value;
    case TONES_SETTING_KEYPAD_TONES:
        return KEYPAD_TONE_OPTIONS[index].value;
    case TONES_SETTING_WARNING_GAME_TONES:
        return ON_OFF_OPTIONS[index].value;
    case TONES_SETTING_VIBRATING_ALERT:
    default:
        return VIBRA_OPTIONS[index].value;
    }
}

static uint8_t tones_default_value(const app_t *app, tones_setting_kind_t kind) {
    return profile_default_setting_value(app->tones_profile_index, tones_profile_kind(kind));
}

static uint8_t tones_current_value(const app_t *app, tones_setting_kind_t kind) {
    return profile_get_tone_setting(app->tones_profile_index, tones_profile_kind(kind));
}

static uint8_t tones_option_index_for_value(const app_t *app, tones_setting_kind_t kind, uint8_t value) {
    uint8_t count = tones_setting_count(kind);
    for (uint8_t i = 0; i < count; i++) {
        if (tones_setting_value(kind, i) == value) {
            return i;
        }
    }
    uint8_t fallback = tones_default_value(app, kind);
    for (uint8_t i = 0; i < count; i++) {
        if (tones_setting_value(kind, i) == fallback) {
            return i;
        }
    }
    return 0u;
}

static const char *tones_value_label(const app_t *app, tones_setting_kind_t kind, uint8_t value) {
    uint8_t index = tones_option_index_for_value(app, kind, value);
    return tones_setting_label(kind, index);
}

static tones_setting_kind_t tones_kind_for_root(uint8_t selected) {
    switch (selected) {
    case 0:
        return TONES_SETTING_INCOMING_ALERT;
    case 1:
        return TONES_SETTING_RINGING_TONE;
    case 3:
        return TONES_SETTING_RINGING_VOLUME;
    case 4:
        return TONES_SETTING_MESSAGE_ALERT;
    case 5:
        return TONES_SETTING_KEYPAD_TONES;
    case 6:
        return TONES_SETTING_WARNING_GAME_TONES;
    case 7:
    default:
        return TONES_SETTING_VIBRATING_ALERT;
    }
}

static bool tones_root_is_setting(uint8_t selected) {
    return selected != 2u;
}

static void open_tones_setting(app_t *app, tones_setting_kind_t kind) {
    app->tones_preview_pending = false;
    app->route = APP_ROUTE_TONES_SETTING;
    app->tones_setting_kind = (uint8_t)kind;
    app->tones_setting_selected = tones_option_index_for_value(app, kind, tones_current_value(app, kind));
    app->tones_setting_view_start = app->tones_setting_selected;
    app->dirty = true;
}

static void save_tones_setting(app_t *app, uint32_t now) {
    tones_setting_kind_t kind = (tones_setting_kind_t)app->tones_setting_kind;
    uint8_t selected = app->tones_setting_selected;
    uint8_t count = tones_setting_count(kind);
    if (selected >= count) {
        selected = 0u;
    }
    uint8_t value = tones_setting_value(kind, selected);
    cancel_tones_preview(app);

    /* v6.00 0x00277594..0x002775dc: raw ringing-volume value 10 is not
     * persisted immediately. It opens bool dialog 44 with SID 0x264 and the
     * stock OK/Back action; only the OK continuation reaches the save path. */
    if (kind == TONES_SETTING_RINGING_VOLUME && value == 10u) {
        open_confirm_sid(app,
                         CONFIRM_CONTEXT_TONES_RINGING_VOLUME,
                         0x264u,
                         "Note:\nVERY LOUD RINGING",
                         2);
        return;
    }

    commit_tones_setting(app, now, false);
}

static void commit_tones_setting(app_t *app, uint32_t now, bool confirmed_level5) {
    tones_setting_kind_t kind = (tones_setting_kind_t)app->tones_setting_kind;
    uint8_t selected = app->tones_setting_selected;
    uint8_t count = tones_setting_count(kind);
    if (selected >= count) {
        selected = 0u;
    }
    uint8_t value = tones_setting_value(kind, selected);
    profile_set_tone_setting(app->tones_profile_index, tones_profile_kind(kind), value);

    if (confirmed_level5) {
        /* v6.00's accepted Level-5 continuation at 0x002775e8 saves, then
         * opens display record 3 with SID 0x132 ("Done"). */
        open_display_sid(app, 3u, 0x132u, "Done", APP_ROUTE_TONES_MENU, now);
        return;
    }

    open_display(app,
                 3u,
                 kind == TONES_SETTING_RINGING_TONE ? ts_or(0x0e6u, "Tone saved") : "Saved", /* "Saved" ROM 662/948 ambiguous -> keep literal */
                 0,
                 0,
                 APP_ROUTE_TONES_MENU,
                 now);
}

void tones_confirm_ringing_volume(app_t *app, bool accepted, uint32_t now) {
    app->confirm_context = CONFIRM_CONTEXT_NONE;
    if (!accepted) {
        app->route = APP_ROUTE_TONES_SETTING;
        app->dirty = true;
        return;
    }

    /* The confirmation is reachable only for this exact selection. Fail back
     * to the selector if stale UI state is ever delivered here. */
    tones_setting_kind_t kind = (tones_setting_kind_t)app->tones_setting_kind;
    uint8_t selected = app->tones_setting_selected;
    if (kind != TONES_SETTING_RINGING_VOLUME ||
        selected >= tones_setting_count(kind) ||
        tones_setting_value(kind, selected) != 10u) {
        app->route = APP_ROUTE_TONES_SETTING;
        app->dirty = true;
        return;
    }

    commit_tones_setting(app, now, true);
}

static void preview_tones_setting(const app_t *app, tones_setting_kind_t kind, uint8_t value, uint8_t selected) {
    switch (kind) {
    case TONES_SETTING_RINGING_TONE: {
        uint8_t raw = ringing_tone_raw_at_visible(selected);
        if (RINGING_TONE_OPTIONS[raw].ringtone_index >= 0) {
            post_ringtone_preview(app,
                                  (uint8_t)RINGING_TONE_OPTIONS[raw].ringtone_index,
                                  tones_current_ringing_audio_level(app));
        } else post_ringtone_preview_by_value(app, RINGING_TONE_OPTIONS[raw].value,
                                             tones_current_ringing_audio_level(app));
        break;
    }
    case TONES_SETTING_RINGING_VOLUME:
        post_ringtone_preview_by_value(app,
                                       tones_current_value(app, TONES_SETTING_RINGING_TONE),
                                       audio_level_from_ringing_volume(value));
        break;
    case TONES_SETTING_MESSAGE_ALERT:
        if (value != 0u) {
            core1_post_command(CORE1_CMD_AUDIO_TONES_SYSTEM_PREVIEW,
                               audio_arg((uint8_t)(28u + value), tones_current_ringing_audio_level(app)));
        }
        break;
    case TONES_SETTING_KEYPAD_TONES:
        if (value != 255u) {
            core1_post_command(CORE1_CMD_AUDIO_TONES_CLICK_PREVIEW,
                               audio_arg(0u, audio_level_from_keypad_tones(value)));
        }
        break;
    case TONES_SETTING_INCOMING_ALERT:
    case TONES_SETTING_WARNING_GAME_TONES:
    case TONES_SETTING_VIBRATING_ALERT:
    default:
        break;
    }
}

static void preview_current_selection(const app_t *app) {
    tones_setting_kind_t kind = (tones_setting_kind_t)app->tones_setting_kind;
    uint8_t selected = app->tones_setting_selected;
    if (selected >= tones_setting_count(kind)) {
        selected = 0u;
    }
    preview_tones_setting(app, kind, tones_setting_value(kind, selected), selected);
}

static void stop_tones_preview(void) {
    core1_post_command(CORE1_CMD_AUDIO_TONES_PREVIEW_STOP, 0u);
}

static void cancel_tones_preview(app_t *app) {
    app->tones_preview_pending = false;
    stop_tones_preview();
}

static void schedule_current_selection_preview(app_t *app, uint32_t now) {
    tones_setting_kind_t kind = (tones_setting_kind_t)app->tones_setting_kind;

    /* Selection motion always silences the preceding row immediately. Only the
     * two PPM-ringtone editors use the ROM's delayed 0x059b play event; message
     * alerts and keypad clicks retain their own immediate editor behavior. */
    cancel_tones_preview(app);
    if (kind == TONES_SETTING_RINGING_TONE || kind == TONES_SETTING_RINGING_VOLUME) {
        app->tones_preview_due_ms = now + TONES_RING_PREVIEW_DELAY_MS;
        app->tones_preview_pending = true;
        return;
    }
    preview_current_selection(app);
}

bool tick_tones_setting(app_t *app, uint32_t now) {
    if (!app->tones_preview_pending) {
        return false;
    }
    if (app->route != APP_ROUTE_TONES_SETTING) {
        app->tones_preview_pending = false;
        return false;
    }
    if ((int32_t)(now - app->tones_preview_due_ms) < 0) {
        return false;
    }

    app->tones_preview_pending = false;
    preview_current_selection(app);
    return true;
}

static void post_ringtone_preview(const app_t *app, uint8_t ringtone_index, uint8_t level) {
    /* Tones-menu ringtone preview uses the magnetic buzzer, bench-confirmed on
     * the original with its earpiece removed. The shared v6.00 0x0a decoder still
     * requests rhythmic lights and pulses vibra when the ACTIVE profile enables
     * it; menu backlight ownership can visually mask the light requests while a
     * keypress keeps the display continuously lit. */
    core1_post_command(CORE1_CMD_AUDIO_RINGTONE_MENU_PREVIEW,
                       audio_arg_with_marker_vibra(ringtone_index,
                                                   level,
                                                   tones_vibra_preview_enabled(app)));
}

static void post_ringtone_preview_by_value(const app_t *app, uint8_t ringtone_value, uint8_t level) {
    if (ringtone_value == 18u || ringtone_value == 19u) {
        static store_own_tone_t tone;
        if (store_own_tone_get(ringtone_value == 18u ? 1u : 0u, &tone) == STORE_STATUS_OK && tone.packed_len != 0u)
            core1_post_audio_packed_tone_preview(tone.packed, tone.packed_len, level);
        return;
    }
    for (uint8_t i = 0; i < ARRAY_COUNT(RINGING_TONE_OPTIONS); i++) {
        if (RINGING_TONE_OPTIONS[i].value == ringtone_value &&
            RINGING_TONE_OPTIONS[i].ringtone_index >= 0) {
            post_ringtone_preview(app, (uint8_t)RINGING_TONE_OPTIONS[i].ringtone_index, level);
            return;
        }
    }
}

uint8_t ringing_tone_catalogue_count(void) {
    return ringing_tone_visible_count();
}

const char *ringing_tone_catalogue_label(uint8_t index) {
    if (index >= ringing_tone_visible_count()) {
        return "";
    }
    const ringtone_option_t *opt = &RINGING_TONE_OPTIONS[ringing_tone_raw_at_visible(index)];
    return ts_or(opt->sid, opt->label);
}

uint8_t ringing_tone_catalogue_value(uint8_t index) {
    return index < ringing_tone_visible_count()
        ? RINGING_TONE_OPTIONS[ringing_tone_raw_at_visible(index)].value
        : 0u;
}

const char *ringing_tone_value_label(uint8_t value) {
    for (uint8_t i = 0; i < ARRAY_COUNT(RINGING_TONE_OPTIONS); i++) {
        if (RINGING_TONE_OPTIONS[i].value == value) {
            return ts_or(RINGING_TONE_OPTIONS[i].sid, RINGING_TONE_OPTIONS[i].label);
        }
    }
    return 0;
}

/* Browse preview for the phonebook assign-tone picker: same loudspeaker
 * preview path as the Tones ringing-tone picker, levels/vibra from the
 * ACTIVE profile (the picker is not profile-editing). */
void preview_ringing_tone_value_active_profile(uint8_t value) {
    stop_tones_preview();
    if (value == 18u || value == 19u) {
        post_ringtone_preview_by_value(NULL, value, audio_level_from_ringing_volume(
            profile_get_tone_setting(profile_active_index(), PROFILE_SETTING_RINGING_VOLUME)));
        return;
    }
    for (uint8_t i = 0; i < ARRAY_COUNT(RINGING_TONE_OPTIONS); i++) {
        if (RINGING_TONE_OPTIONS[i].value == value && RINGING_TONE_OPTIONS[i].ringtone_index >= 0) {
            uint8_t active = profile_active_index();
            uint8_t volume = profile_get_tone_setting(active, PROFILE_SETTING_RINGING_VOLUME);
            bool vibra = profile_get_tone_setting(active, PROFILE_SETTING_VIBRATING_ALERT) != 0u;
            core1_post_command(CORE1_CMD_AUDIO_RINGTONE_MENU_PREVIEW,
                               audio_arg_with_marker_vibra(
                                   (uint8_t)RINGING_TONE_OPTIONS[i].ringtone_index,
                                   audio_level_from_ringing_volume(volume),
                                   vibra));
            return;
        }
    }
}

void stop_ringing_tone_preview(void) {
    stop_tones_preview();
}

static bool tones_vibra_preview_enabled(const app_t *app) {
    return profile_get_tone_setting(app->tones_profile_index, PROFILE_SETTING_VIBRATING_ALERT) != 0u;
}

static uint8_t current_ringing_volume(const app_t *app) {
    return tones_current_value(app, TONES_SETTING_RINGING_VOLUME);
}

uint8_t tones_current_ringing_audio_level(const app_t *app) {
    return audio_level_from_ringing_volume(current_ringing_volume(app));
}

static void tones_return_to_parent(app_t *app, uint32_t now) {
    if (app->tones_from_profiles) {
        open_profiles_options(app, app->profiles_selected_index);
        app->profiles_option_index = 1u;
    } else {
        open_main_menu_at(app, 8u, now);
    }
}

static bool tones_root_item_visible(uint8_t raw) {
    if (raw >= ARRAY_COUNT(TONES_ROOT_LABELS)) {
        return false;
    }
    if (raw == 7u) {
        return feature_gate_visible(FEATURE_GATE_TONES_VIBRATING_ALERT);
    }
    return true;
}

static uint8_t tones_root_visible_count(void) {
    uint8_t count = ui_menu_visible_count(
        tones_root_item_visible, (uint8_t)ARRAY_COUNT(TONES_ROOT_LABELS));
    return count == 0u ? 1u : count;
}

static uint8_t tones_root_visible_index(uint8_t raw) {
    return ui_menu_visible_index(
        tones_root_item_visible, (uint8_t)ARRAY_COUNT(TONES_ROOT_LABELS),
        tones_root_normalized_raw(raw));
}

static uint8_t tones_root_raw_at_visible(uint8_t visible) {
    return ui_menu_raw_at_visible(
        tones_root_item_visible, (uint8_t)ARRAY_COUNT(TONES_ROOT_LABELS),
        visible);
}

static uint8_t tones_root_normalized_raw(uint8_t raw) {
    return ui_menu_normalized_raw(
        tones_root_item_visible, (uint8_t)ARRAY_COUNT(TONES_ROOT_LABELS), raw);
}

static void tones_menu_step(app_t *app, int8_t delta) {
    uint8_t count = tones_root_visible_count();
    uint8_t visible = tones_root_visible_index(app->tones_menu_selected);
    visible = (uint8_t)((visible + count + delta) % count);
    app->tones_menu_selected = tones_root_raw_at_visible(visible);
    app->dirty = true;
}

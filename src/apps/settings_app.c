#include "apps/settings_app.h"

#include <stdio.h>
#include <string.h>

#include "apps/dialogs_app.h"
#include "apps/main_menu_app.h"
#include "apps/profiles_app.h"
#include "services/feature_gates.h"
#include "services/board_diag_service.h"
#include "services/input_keys.h"
#include "storage/store_service.h"
#include "services/strings.h"
#include "services/timebase.h"

#define SETTINGS_PENDING_NONE 0u
#define SETTINGS_PENDING_NETWORK_SEARCH 1u
#define SETTINGS_PENDING_CALL_WAITING 2u
#define SETTINGS_PENDING_DELAY_MS 1800u

typedef enum {
    SETTINGS_VALUE_AUTOMATIC_REDIAL = 0,
    SETTINGS_VALUE_SPEED_DIALLING,
    SETTINGS_VALUE_CALL_WAITING,
    SETTINGS_VALUE_OWN_NUMBER,
    SETTINGS_VALUE_PHONE_LINE,
    SETTINGS_VALUE_AUTOMATIC_ANSWER,
    SETTINGS_VALUE_LANGUAGE,
    SETTINGS_VALUE_CELL_INFO,
    SETTINGS_VALUE_NETWORK,
    SETTINGS_VALUE_LIGHTS,
    SETTINGS_VALUE_CONFIRM_SIM,
    SETTINGS_VALUE_PIN_REQUEST,
    SETTINGS_VALUE_FIXED_DIALLING,
    SETTINGS_VALUE_CLOSED_USER_GROUP,
    SETTINGS_VALUE_PHONE_SECURITY,
} settings_value_kind_t;

typedef struct {
    const char *label;
    uint8_t value;
} settings_option_t;

/* A localizable menu label. The English literal is the fallback (and, since the
 * settings menus dispatch purely by index -- never strcmp on the label -- it is
 * safe to render a localized copy while the array entry stays English). `sid` is
 * the v6.00 string id (index+58). sid 0 = no exact 1:1 v6.00 record for this
 * app's screens (the text is ambiguous across contexts) -> keep English. The
 * ROM's own per-language line split (which the renderer honors via '\n') is
 * carried in the label literal so ts(sid) and the fallback agree. */
typedef struct {
    const char *label;
    uint16_t sid;
} settings_label_t;

/* Localize by SID with English fallback: sid 0 (no 1:1 match) or a sid with no
 * record (ts()==NULL, clone-only) both return the supplied English literal. */
static const char *L(uint16_t sid, const char *en) {
    if (sid != 0u) {
        const char *t = ts(sid);
        if (t != 0) {
            return t;
        }
    }
    return en;
}

static const settings_label_t SETTINGS_ROOT_LABELS[] = {
    {"Call\nsettings", 0x9eu},
    {"Phone\nsettings", 0x22du},
    {"Security\nsettings", 0x2a2u},
    {"Restore\nfactory\nsettings", 0x1c2u},
};
static const settings_label_t SETTINGS_CALL_LABELS[] = {
    {"Automatic\nredial", 0x9bu},
    {"Speed\ndialling", 0x219u},
    {"Call waiting\noptions", 0xa5u},
    {"Own number\nsending", 0xd6u},
    {"Phone line\nin use", 0x51u},
    {"Automatic\nanswer", 0x59u},
};
static const settings_label_t SETTINGS_PHONE_LABELS[] = {
    {"Language", 0u},           /* not in this app's SID map (ambiguous 0xb1 / 0x19f) -> English */
    {"Cell info\ndisplay", 0x1acu},
    {"Welcome\nnote", 0x3e9u},
    {"Network\nselection", 0x3b8u},
    {"Lights", 0x1a3u},
    {"Confirm SIM service actions", 0x2bau},  /* ROM is single-line; the renderer word-wraps */
};
static const settings_label_t SETTINGS_SECURITY_LABELS[] = {
    {"PIN code\nrequest", 0x238u},
    {"Fixed\ndialling", 0x1bfu},
    {"Closed\nuser group", 0xdau},
    {"Phone\nsecurity", 0x2d1u},
    {"Change\naccess codes", 0xc8u},
    {"Phone line\nchange", 0x4bu},
};
static const settings_label_t SETTINGS_ACCESS_LABELS[] = {
    {"Change\nsecurity code", 0x29cu},
    {"Change\nPIN code", 0x236u},
    {"Change\nPIN2 code", 0x232u},
};
/* Save/Erase both resolve to many context-specific SIDs -> ambiguous, kept
 * English (dispatch here is index-based, so this stays a plain literal array). */
static const char *const SETTINGS_WELCOME_OPTIONS[] = {"Save", "Erase"};

static const settings_option_t OPTIONS_ON_OFF[] = {{"On", 1u}, {"Off", 0u}};
static const settings_option_t OPTIONS_OFF_ON[] = {{"Off", 0u}, {"On", 1u}};
static const settings_option_t OPTIONS_OWN_NUMBER[] = {{"Preset", 0u}, {"On", 1u}, {"Off", 2u}};
static const settings_option_t OPTIONS_PHONE_LINE[] = {{"Line 1", 0u}, {"Line 2", 1u}};
static const settings_option_t OPTIONS_NETWORK[] = {{"Automatic", 0u}, {"Manual", 1u}};
static const settings_option_t OPTIONS_LIGHTS[] = {{"On", 1u}, {"Automatic", 0u}};
static const settings_option_t OPTIONS_CONFIRM_SIM[] = {{"Asked", 1u}, {"Not asked", 0u}};
static const settings_option_t OPTIONS_PIN_REQUEST[] = {{"On", 0u}, {"Off", 1u}};
static const settings_option_t OPTIONS_FIXED_DIALLING[] = {{"On", 1u}, {"Off", 0u}, {"Numbers", 2u}};
static const settings_option_t OPTIONS_PHONE_SECURITY[] = {{"Off", 17u}, {"On", 57u}};
static const settings_option_t OPTIONS_CALL_WAITING[] = {{"Activate", 4u}, {"Cancel", 2u}, {"Status", 3u}};
/* SET.1: the Language menu lists exactly the languages COMPILED into this
 * build (the generated strings registry), never a hardcoded set -- an
 * English-only build shows one language. Entry 0 is v6.00's auto-select
 * option: value 0 is the "automatic" sentinel; records_for(0) resolves to
 * English, so until real auto-detect logic exists it shows English.
 * "Automatic" is localized (SID 0x1a5); the registry self-names are NOT
 * (each shows its own name in its own script). */
#define LANGUAGE_OPTION_CAP 40u
static settings_option_t s_language_options[1u + LANGUAGE_OPTION_CAP];
static uint8_t s_language_option_count;

static void build_language_options(void) {
    if (s_language_option_count != 0u) {
        return;
    }
    s_language_options[0].label = "Automatic";
    s_language_options[0].value = 0x00u;
    uint8_t count = strings_language_count();
    if (count > LANGUAGE_OPTION_CAP) {
        count = LANGUAGE_OPTION_CAP;
    }
    for (uint8_t i = 0u; i < count; i++) {
        s_language_options[1u + i].label = strings_language_self_name_at(i);
        s_language_options[1u + i].value = strings_language_id_at(i);
    }
    s_language_option_count = (uint8_t)(1u + count);
}

static const settings_label_t *settings_menu_labels(settings_menu_kind_t kind);
static uint8_t settings_menu_count(settings_menu_kind_t kind);
static bool settings_menu_item_visible(settings_menu_kind_t kind, uint8_t raw);
static uint8_t settings_menu_visible_count(settings_menu_kind_t kind);
static uint8_t settings_menu_visible_index(settings_menu_kind_t kind, uint8_t raw);
static uint8_t settings_menu_raw_at_visible(settings_menu_kind_t kind, uint8_t visible);
static uint8_t settings_menu_normalized_raw(settings_menu_kind_t kind, uint8_t raw);
static void settings_menu_step(app_t *app, int8_t delta);
static const char *settings_menu_breadcrumb(settings_menu_kind_t kind, uint8_t selected, char *scratch, size_t scratch_cap);
static const char *settings_menu_preview(settings_menu_kind_t kind, uint8_t selected, char *scratch, size_t scratch_cap);
static bool settings_menu_has_preview(settings_menu_kind_t kind, uint8_t selected);
static void open_settings_value(app_t *app, settings_value_kind_t kind, uint8_t parent_selected);
static const settings_option_t *settings_value_options(settings_value_kind_t kind, uint8_t *out_count);
static store_setting_key_t settings_value_store_key(settings_value_kind_t kind);
static uint8_t settings_current_value(settings_value_kind_t kind);
static uint8_t settings_selected_for_value(settings_value_kind_t kind, uint8_t value);
static const char *settings_value_label(settings_value_kind_t kind, uint8_t value, char *scratch, size_t scratch_cap);
static uint16_t settings_value_sid(settings_value_kind_t kind, uint8_t index);
static bool apply_settings_value(app_t *app, uint32_t now);
static void open_welcome_note_editor(app_t *app, uint32_t now);
static void open_settings_access_editor(app_t *app, uint8_t kind, uint32_t now);
static const char *settings_access_prompt(uint8_t kind, uint8_t step);
static uint8_t settings_access_max_len(uint8_t kind);
static void return_to_settings_parent(app_t *app);
static void settings_show_saved(app_t *app, uint32_t now);

void settings_restore_factory_defaults(void) {
    store_setting_set_u8(STORE_SETTING_SETTINGS_AUTOMATIC_REDIAL, 0u);
    store_setting_set_u8(STORE_SETTING_SETTINGS_SPEED_DIALLING, 0u);
    store_setting_set_u8(STORE_SETTING_SETTINGS_PHONE_LINE, 0u);
    store_setting_set_u8(STORE_SETTING_CALL_OWN_NUMBER_SENDING, 0u);
    store_setting_set_u8(STORE_SETTING_SETTINGS_AUTOMATIC_ANSWER, 0u);
    store_setting_set_u8(STORE_SETTING_SETTINGS_CELL_INFO_DISPLAY, 0u);
    store_setting_set_u8(STORE_SETTING_SETTINGS_NETWORK_SELECTION, 0u);
    store_setting_set_u8(STORE_SETTING_SETTINGS_LIGHTS, 0u);
    store_setting_set_u8(STORE_SETTING_SETTINGS_CONFIRM_SIM_ACTIONS, 0u);
    store_setting_set_u8(STORE_SETTING_SETTINGS_PIN_CODE_REQUEST, 0u);
    store_setting_set_u8(STORE_SETTING_SETTINGS_FIXED_DIALLING, 0u);
    store_setting_set_u8(STORE_SETTING_SETTINGS_CLOSED_USER_GROUP, 0u);
    store_setting_set_u8(STORE_SETTING_SETTINGS_PHONE_SECURITY, 17u);
    store_setting_set_u8(STORE_SETTING_SETTINGS_PHONE_LINE_CHANGE_ALLOWED, 1u);
    store_setting_set_u8(STORE_SETTING_SYSTEM_LANGUAGE, 0u);
    store_setting_set_text(STORE_SETTING_SYSTEM_WELCOME_NOTE, "");
    profiles_restore_factory_defaults();

    /* Automatic currently resolves to English until SIM/network language
     * selection exists, but preserve the sentinel so future auto-detect starts
     * working without another factory reset. Apply it before the confirmation
     * note is rendered. */
    strings_set_language(0u);
}

void open_settings_menu(app_t *app, settings_menu_kind_t kind, uint8_t selected) {
    app->route = APP_ROUTE_SETTINGS_MENU;
    app->settings_menu_kind = (uint8_t)kind;
    app->settings_menu_selected = settings_menu_normalized_raw(kind, selected);
    app->dirty = true;
}

bool handle_settings_menu_key(app_t *app, uint16_t key, uint32_t now) {
    settings_menu_kind_t kind = (settings_menu_kind_t)app->settings_menu_kind;
    uint8_t count = settings_menu_visible_count(kind);
    if (count == 0u) {
        return true;
    }
    app->settings_menu_selected = settings_menu_normalized_raw(kind, app->settings_menu_selected);
    if (key == KEY_UP) {
        settings_menu_step(app, -1);
        return true;
    }
    if (key == KEY_DOWN) {
        settings_menu_step(app, 1);
        return true;
    }
    if (key == KEY_C) {
        if (kind == SETTINGS_MENU_ROOT) {
            open_main_menu_at(app, 3u, now);
        } else if (kind == SETTINGS_MENU_ACCESS_CODES) {
            open_settings_menu(app, SETTINGS_MENU_SECURITY, 4u);
        } else {
            open_settings_menu(app, SETTINGS_MENU_ROOT, (uint8_t)(kind - 1u));
        }
        return true;
    }
    if (key != KEY_NAVI) {
        return true;
    }

    uint8_t selected = app->settings_menu_selected;
    if (kind == SETTINGS_MENU_ROOT) {
        if (selected == 0u) {
            open_settings_menu(app, SETTINGS_MENU_CALL, 0u);
        } else if (selected == 1u) {
            open_settings_menu(app, SETTINGS_MENU_PHONE, 0u);
        } else if (selected == 2u) {
            open_settings_menu(app, SETTINGS_MENU_SECURITY, 0u);
        } else {
            settings_restore_factory_defaults();
            open_display_sid(app, 3u, 0x1c3u, "Settings\nrestored", APP_ROUTE_SETTINGS_MENU, now);
        }
        return true;
    }

    if (kind == SETTINGS_MENU_CALL) {
        static const settings_value_kind_t map[] = {
            SETTINGS_VALUE_AUTOMATIC_REDIAL,
            SETTINGS_VALUE_SPEED_DIALLING,
            SETTINGS_VALUE_CALL_WAITING,
            SETTINGS_VALUE_OWN_NUMBER,
            SETTINGS_VALUE_PHONE_LINE,
            SETTINGS_VALUE_AUTOMATIC_ANSWER,
        };
        open_settings_value(app, map[selected], selected);
        return true;
    }
    if (kind == SETTINGS_MENU_PHONE) {
        if (selected == 2u) {
            open_welcome_note_editor(app, now);
            return true;
        }
        static const settings_value_kind_t map[] = {
            SETTINGS_VALUE_LANGUAGE,
            SETTINGS_VALUE_CELL_INFO,
            SETTINGS_VALUE_LANGUAGE,
            SETTINGS_VALUE_NETWORK,
            SETTINGS_VALUE_LIGHTS,
            SETTINGS_VALUE_CONFIRM_SIM,
        };
        open_settings_value(app, map[selected], selected);
        return true;
    }
    if (kind == SETTINGS_MENU_SECURITY) {
        if (selected == 4u) {
            open_settings_menu(app, SETTINGS_MENU_ACCESS_CODES, 0u);
            return true;
        }
        if (selected == 5u) {
            uint8_t allowed = 1u;
            store_setting_get_u8(STORE_SETTING_SETTINGS_PHONE_LINE_CHANGE_ALLOWED, &allowed);
            allowed = allowed ? 0u : 1u;
            store_setting_set_u8(STORE_SETTING_SETTINGS_PHONE_LINE_CHANGE_ALLOWED, allowed);
            if (allowed) {
                open_display_sid(app, 3u, 0x4du, "Line\nchange\nallowed", APP_ROUTE_SETTINGS_MENU, now);
            } else {
                open_display_sid(app, 3u, 0x4fu, "Line\nchange\nnot allowed", APP_ROUTE_SETTINGS_MENU, now);
            }
            return true;
        }
        static const settings_value_kind_t map[] = {
            SETTINGS_VALUE_PIN_REQUEST,
            SETTINGS_VALUE_FIXED_DIALLING,
            SETTINGS_VALUE_CLOSED_USER_GROUP,
            SETTINGS_VALUE_PHONE_SECURITY,
        };
        open_settings_value(app, map[selected], selected);
        return true;
    }
    if (kind == SETTINGS_MENU_ACCESS_CODES) {
        open_settings_access_editor(app, selected, now);
        return true;
    }
    return true;
}

bool handle_settings_value_key(app_t *app, uint16_t key, uint32_t now) {
    uint8_t count = 0u;
    settings_value_options((settings_value_kind_t)app->settings_value_kind, &count);
    if (count == 0u) {
        return true;
    }
    if (app->settings_value_selected >= count) {
        app->settings_value_selected = 0u;
        app->settings_value_view_start = 0u;
    }
    if (key == KEY_UP) {
        ui_circular_list_step_3rows(count, -1,
                                    &app->settings_value_selected,
                                    &app->settings_value_view_start);
        app->dirty = true;
        return true;
    }
    if (key == KEY_DOWN) {
        ui_circular_list_step_3rows(count, 1,
                                    &app->settings_value_selected,
                                    &app->settings_value_view_start);
        app->dirty = true;
        return true;
    }
    if (key == KEY_C) {
        return_to_settings_parent(app);
        return true;
    }
    if (key == KEY_NAVI) {
        return apply_settings_value(app, now);
    }
    return true;
}

bool handle_settings_welcome_options_key(app_t *app, uint16_t key, uint32_t now) {
    if (key == KEY_UP || key == KEY_DOWN) {
        app->settings_welcome_option_selected ^= 1u;
        app->dirty = true;
        return true;
    }
    if (key == KEY_C) {
        app->route = APP_ROUTE_EDITOR;
        app->dirty = true;
        return true;
    }
    if (key != KEY_NAVI) {
        return true;
    }
    if (app->settings_welcome_option_selected == 0u) {
        if (app->editor_value[0] != '\0') {
            store_setting_set_text(STORE_SETTING_SYSTEM_WELCOME_NOTE, app->editor_value);
        }
        close_editor(app);
        open_settings_menu(app, SETTINGS_MENU_PHONE, 2u);
        open_display_sid(app, 3u, 0x402u, "Welcome\nnote\nsaved", APP_ROUTE_SETTINGS_MENU, now);
    } else {
        store_setting_set_text(STORE_SETTING_SYSTEM_WELCOME_NOTE, "");
        close_editor(app);
        open_settings_menu(app, SETTINGS_MENU_PHONE, 2u);
        open_display_sid(app, 3u, 0x400u, "Welcome\nnote\nerased", APP_ROUTE_SETTINGS_MENU, now);
    }
    return true;
}

bool tick_settings(app_t *app, uint32_t now) {
    if (app->settings_pending_action == SETTINGS_PENDING_NONE) {
        return false;
    }
    if (app->route != APP_ROUTE_DISPLAY_MESSAGE) {
        app->settings_pending_action = SETTINGS_PENDING_NONE;
        return false;
    }
    if (time_diff_ms(now, app->settings_pending_started_ms + SETTINGS_PENDING_DELAY_MS) < 0) {
        return false;
    }
    uint8_t action = app->settings_pending_action;
    app->settings_pending_action = SETTINGS_PENDING_NONE;
    if (action == SETTINGS_PENDING_NETWORK_SEARCH) {
        open_settings_menu(app, SETTINGS_MENU_PHONE, 3u);
        open_display_sid(app, 0u, 0x207u, "No\nnetwork\nfound", APP_ROUTE_SETTINGS_MENU, now);
    } else if (action == SETTINGS_PENDING_CALL_WAITING) {
        open_settings_menu(app, SETTINGS_MENU_CALL, 2u);
        /* SET.7: "Request not confirmed" is record 0x0c (info bitmap 0053); the
         * message text is v6.00 SID 0x3e2. */
        open_display_sid(app, 12u, 0x3e2u, "Request\nnot\nconfirmed", APP_ROUTE_SETTINGS_MENU, now);
    }
    return true;
}

void render_settings_menu(const app_t *app, framebuffer_t *fb) {
    settings_menu_kind_t kind = (settings_menu_kind_t)app->settings_menu_kind;
    const settings_label_t *labels = settings_menu_labels(kind);
    uint8_t count = settings_menu_visible_count(kind);
    uint8_t selected = settings_menu_normalized_raw(kind, app->settings_menu_selected);
    uint8_t selected_visible = settings_menu_visible_index(kind, selected);
    if (count == 0u) {
        fb_clear(fb, false);
        return;
    }
    char breadcrumb[10];
    char preview[18];
    /* The menu item name localizes; the preview (current value) is localized
     * per value type by settings_menu_preview -- notably the Language preview
     * keeps each language's own self-name and must not be translated. */
    draw_static_page_list(fb,
                          L(labels[selected].sid, labels[selected].label),
                          settings_menu_has_preview(kind, selected)
                              ? settings_menu_preview(kind, selected, preview, sizeof(preview))
                              : "",
                          selected_visible,
                          count,
                          settings_menu_breadcrumb(kind, selected, breadcrumb, sizeof(breadcrumb)),
                          "Select");
}

void render_settings_value(const app_t *app, framebuffer_t *fb) {
    uint8_t count = 0u;
    const settings_option_t *options = settings_value_options((settings_value_kind_t)app->settings_value_kind, &count);
    /* Must hold the largest value list: the registry-driven Language picker
     * ("Automatic" + up to LANGUAGE_OPTION_CAP compiled languages). A smaller
     * buffer would render truncated while keys still navigate the full set. */
    const char *labels[1u + LANGUAGE_OPTION_CAP];
    if (count > ARRAY_COUNT(labels)) {
        count = ARRAY_COUNT(labels);
    }
    settings_value_kind_t kind = (settings_value_kind_t)app->settings_value_kind;
    for (uint8_t i = 0; i < count; i++) {
        labels[i] = ts_or(settings_value_sid(kind, i), options[i].label);
    }
    /* Value pickers continue the parent menu's numeric path (e.g. Language ->
     * "4-2-1-N"), like every other menu level -- not a bare item number. */
    char parent[10];
    settings_menu_breadcrumb((settings_menu_kind_t)app->settings_value_parent_kind,
                             app->settings_value_parent_selected, parent, sizeof(parent));
    char crumb[16];
    snprintf(crumb, sizeof(crumb), "%s-%u", parent, (unsigned)(app->settings_value_selected + 1u));
    draw_flat_list_circular_view(fb, labels, count,
                                 app->settings_value_selected,
                                 app->settings_value_view_start,
                                 crumb, "OK");
}

void render_settings_welcome_options(const app_t *app, framebuffer_t *fb) {
    draw_flat_list(fb,
                   SETTINGS_WELCOME_OPTIONS,
                   (uint8_t)ARRAY_COUNT(SETTINGS_WELCOME_OPTIONS),
                   app->settings_welcome_option_selected,
                   "",
                   "OK");
}

void settings_submit_welcome_editor(app_t *app, uint32_t now) {
    (void)now;
    app->settings_welcome_option_selected = 0u;
    app->route = APP_ROUTE_SETTINGS_WELCOME_OPTIONS;
    app->dirty = true;
}

void settings_cancel_editor(app_t *app, uint32_t now) {
    (void)now;
    close_editor(app);
    open_settings_menu(app, (settings_menu_kind_t)app->settings_value_parent_kind, app->settings_value_parent_selected);
}

void settings_submit_pin_request_editor(app_t *app, uint32_t now) {
    size_t len = strlen(app->editor_value);
    if (len < 4u || len > 8u) {
        open_display_sid(app, 0u, 0xdcu, "Code\nerror", APP_ROUTE_EDITOR, now);
        return;
    }
    store_setting_set_u8(STORE_SETTING_SETTINGS_PIN_CODE_REQUEST, app->settings_pending_value);
    close_editor(app);
    open_settings_menu(app, SETTINGS_MENU_SECURITY, 0u);
    settings_show_saved(app, now);
}

void settings_submit_access_code_editor(app_t *app, uint32_t now) {
    size_t len = strlen(app->editor_value);
    if (len == 0u || len > settings_access_max_len(app->settings_access_kind)) {
        open_display_sid(app, 0u, 0x145u, "Code\nerror", APP_ROUTE_EDITOR, now);
        return;
    }
    if (app->settings_access_step == 0u) {
        if (app->settings_access_kind == 0u) {
            /* The current security code is validated against the persisted
             * code (PIN/PIN2 validation stays SIM-owned and modeled). */
            char code[STORE_TEXT_MAX + 1u];
            store_setting_get_text(STORE_SETTING_SECURITY_CODE, code, sizeof(code));
            if (strcmp(app->editor_value, code) != 0) {
                open_display_sid(app, 0u, 0x145u, "Code\nerror", APP_ROUTE_EDITOR, now);
                return;
            }
        }
        app->settings_access_step = 1u;
        open_editor(app,
                    settings_access_prompt(app->settings_access_kind, 1u),
                    "",
                    settings_access_max_len(app->settings_access_kind),
                    EDITOR_KIND_NUMBER,
                    EDITOR_CONTEXT_SETTINGS_ACCESS_CODE,
                    false,
                    now);
        return;
    }
    if (app->settings_access_step == 1u) {
        copy_text(app->settings_access_new_code, sizeof(app->settings_access_new_code), app->editor_value);
        app->settings_access_step = 2u;
        open_editor(app,
                    settings_access_prompt(app->settings_access_kind, 2u),
                    "",
                    settings_access_max_len(app->settings_access_kind),
                    EDITOR_KIND_NUMBER,
                    EDITOR_CONTEXT_SETTINGS_ACCESS_CODE,
                    false,
                    now);
        return;
    }
    if (strcmp(app->settings_access_new_code, app->editor_value) != 0) {
        open_display_sid(app, 0u, 0xddu, "Codes\ndo not\nmatch", APP_ROUTE_EDITOR, now);
        return;
    }
    close_editor(app);
    open_settings_menu(app, SETTINGS_MENU_ACCESS_CODES, app->settings_access_kind);
    if (app->settings_access_kind == 0u) {
        /* Persist the new security code so security-gated flows honor it. */
        store_setting_set_text(STORE_SETTING_SECURITY_CODE, app->settings_access_new_code);
        open_display_sid(app, 3u, 0x29bu, "Security\ncode\nchanged", APP_ROUTE_SETTINGS_MENU, now);
    } else if (app->settings_access_kind == 1u) {
        open_display_sid(app, 3u, 0x237u, "PIN code changed", APP_ROUTE_SETTINGS_MENU, now);
    } else {
        open_display_sid(app, 3u, 0x233u, "PIN2 code changed", APP_ROUTE_SETTINGS_MENU, now);
    }
}

static const settings_label_t *settings_menu_labels(settings_menu_kind_t kind) {
    switch (kind) {
    case SETTINGS_MENU_CALL: return SETTINGS_CALL_LABELS;
    case SETTINGS_MENU_PHONE: return SETTINGS_PHONE_LABELS;
    case SETTINGS_MENU_SECURITY: return SETTINGS_SECURITY_LABELS;
    case SETTINGS_MENU_ACCESS_CODES: return SETTINGS_ACCESS_LABELS;
    case SETTINGS_MENU_ROOT:
    default: return SETTINGS_ROOT_LABELS;
    }
}

static uint8_t settings_menu_count(settings_menu_kind_t kind) {
    switch (kind) {
    case SETTINGS_MENU_CALL: return (uint8_t)ARRAY_COUNT(SETTINGS_CALL_LABELS);
    case SETTINGS_MENU_PHONE: return (uint8_t)ARRAY_COUNT(SETTINGS_PHONE_LABELS);
    case SETTINGS_MENU_SECURITY: return (uint8_t)ARRAY_COUNT(SETTINGS_SECURITY_LABELS);
    case SETTINGS_MENU_ACCESS_CODES: return (uint8_t)ARRAY_COUNT(SETTINGS_ACCESS_LABELS);
    case SETTINGS_MENU_ROOT:
    default: return (uint8_t)ARRAY_COUNT(SETTINGS_ROOT_LABELS);
    }
}

static bool settings_menu_item_visible(settings_menu_kind_t kind, uint8_t raw) {
    if (raw >= settings_menu_count(kind)) {
        return false;
    }
    if (kind == SETTINGS_MENU_CALL && raw == 4u) {
        return feature_gate_visible(FEATURE_GATE_CALL_SETTINGS_PHONE_LINE_IN_USE);
    }
    if (kind == SETTINGS_MENU_CALL && raw == 5u) {
        /* 1:1: Automatic answer is accessory-gated -- the row is only shown with a
         * headset connected. (It stays inert: v6.00 exposed the menu but never
         * wired a runtime auto-answer trigger, so we keep it inert too.) */
        return board_diag_headset_inserted() &&
               feature_gate_visible(FEATURE_GATE_CALL_SETTINGS_AUTOMATIC_ANSWER);
    }
    if (kind == SETTINGS_MENU_PHONE && raw == 4u) {
        return feature_gate_visible(FEATURE_GATE_PHONE_SETTINGS_LIGHTS);
    }
    if (kind == SETTINGS_MENU_SECURITY && raw == 5u) {
        return feature_gate_visible(FEATURE_GATE_SECURITY_PHONE_LINE_CHANGE);
    }
    return true;
}

static uint8_t settings_menu_visible_count(settings_menu_kind_t kind) {
    uint8_t count = 0u;
    uint8_t raw_count = settings_menu_count(kind);
    for (uint8_t i = 0; i < raw_count; i++) {
        if (settings_menu_item_visible(kind, i)) {
            count++;
        }
    }
    return count;
}

static uint8_t settings_menu_visible_index(settings_menu_kind_t kind, uint8_t raw) {
    uint8_t visible = 0u;
    raw = settings_menu_normalized_raw(kind, raw);
    uint8_t raw_count = settings_menu_count(kind);
    for (uint8_t i = 0; i < raw_count; i++) {
        if (!settings_menu_item_visible(kind, i)) {
            continue;
        }
        if (i == raw) {
            return visible;
        }
        visible++;
    }
    return 0u;
}

static uint8_t settings_menu_raw_at_visible(settings_menu_kind_t kind, uint8_t visible) {
    uint8_t seen = 0u;
    uint8_t raw_count = settings_menu_count(kind);
    for (uint8_t i = 0; i < raw_count; i++) {
        if (!settings_menu_item_visible(kind, i)) {
            continue;
        }
        if (seen == visible) {
            return i;
        }
        seen++;
    }
    return 0u;
}

static uint8_t settings_menu_normalized_raw(settings_menu_kind_t kind, uint8_t raw) {
    uint8_t raw_count = settings_menu_count(kind);
    if (raw < raw_count && settings_menu_item_visible(kind, raw)) {
        return raw;
    }
    for (uint8_t offset = 0; offset < raw_count; offset++) {
        uint8_t candidate = (uint8_t)((raw + offset) % raw_count);
        if (settings_menu_item_visible(kind, candidate)) {
            return candidate;
        }
    }
    return 0u;
}

static void settings_menu_step(app_t *app, int8_t delta) {
    settings_menu_kind_t kind = (settings_menu_kind_t)app->settings_menu_kind;
    uint8_t count = settings_menu_visible_count(kind);
    if (count == 0u) {
        return;
    }
    uint8_t visible = settings_menu_visible_index(kind, app->settings_menu_selected);
    visible = (uint8_t)((visible + count + delta) % count);
    app->settings_menu_selected = settings_menu_raw_at_visible(kind, visible);
    app->dirty = true;
}

static const char *settings_menu_breadcrumb(settings_menu_kind_t kind, uint8_t selected, char *scratch, size_t scratch_cap) {
    if (kind == SETTINGS_MENU_CALL) {
        snprintf(scratch, scratch_cap, "4-1-%u", (unsigned)(selected + 1u));
    } else if (kind == SETTINGS_MENU_PHONE) {
        snprintf(scratch, scratch_cap, "4-2-%u", (unsigned)(selected + 1u));
    } else if (kind == SETTINGS_MENU_SECURITY) {
        snprintf(scratch, scratch_cap, "4-3-%u", (unsigned)(selected + 1u));
    } else if (kind == SETTINGS_MENU_ACCESS_CODES) {
        snprintf(scratch, scratch_cap, "4-3-5-%u", (unsigned)(selected + 1u));
    } else {
        snprintf(scratch, scratch_cap, "4-%u", (unsigned)(selected + 1u));
    }
    return scratch;
}

static const char *settings_menu_preview(settings_menu_kind_t kind, uint8_t selected, char *scratch, size_t scratch_cap) {
    scratch[0] = '\0';
    if (kind == SETTINGS_MENU_CALL) {
        static const settings_value_kind_t map[] = {
            SETTINGS_VALUE_AUTOMATIC_REDIAL,
            SETTINGS_VALUE_SPEED_DIALLING,
            SETTINGS_VALUE_CALL_WAITING,
            SETTINGS_VALUE_OWN_NUMBER,
            SETTINGS_VALUE_PHONE_LINE,
            SETTINGS_VALUE_AUTOMATIC_ANSWER,
        };
        if (selected == 2u) {
            return "";
        }
        return settings_value_label(map[selected], settings_current_value(map[selected]), scratch, scratch_cap);
    }
    if (kind == SETTINGS_MENU_PHONE) {
        static const settings_value_kind_t map[] = {
            SETTINGS_VALUE_LANGUAGE,
            SETTINGS_VALUE_CELL_INFO,
            SETTINGS_VALUE_LANGUAGE,
            SETTINGS_VALUE_NETWORK,
            SETTINGS_VALUE_LIGHTS,
            SETTINGS_VALUE_CONFIRM_SIM,
        };
        if (selected == 2u) {
            store_setting_get_text(STORE_SETTING_SYSTEM_WELCOME_NOTE, scratch, (uint8_t)scratch_cap);
            return scratch;
        }
        return settings_value_label(map[selected], settings_current_value(map[selected]), scratch, scratch_cap);
    }
    if (kind == SETTINGS_MENU_SECURITY) {
        static const settings_value_kind_t map[] = {
            SETTINGS_VALUE_PIN_REQUEST,
            SETTINGS_VALUE_FIXED_DIALLING,
            SETTINGS_VALUE_CLOSED_USER_GROUP,
            SETTINGS_VALUE_PHONE_SECURITY,
        };
        if (selected < ARRAY_COUNT(map)) {
            return settings_value_label(map[selected], settings_current_value(map[selected]), scratch, scratch_cap);
        }
    }
    return "";
}

static bool settings_menu_has_preview(settings_menu_kind_t kind, uint8_t selected) {
    return (kind == SETTINGS_MENU_CALL && selected != 2u) ||
           (kind == SETTINGS_MENU_PHONE) ||
           (kind == SETTINGS_MENU_SECURITY && selected < 4u);
}

static void open_settings_value(app_t *app, settings_value_kind_t kind, uint8_t parent_selected) {
    app->settings_value_kind = (uint8_t)kind;
    app->settings_value_parent_kind = app->settings_menu_kind;
    app->settings_value_parent_selected = parent_selected;
    app->settings_value_selected = settings_selected_for_value(kind, settings_current_value(kind));
    app->settings_value_view_start = app->settings_value_selected;
    app->route = APP_ROUTE_SETTINGS_VALUE;
    app->dirty = true;
}

static const settings_option_t *settings_value_options(settings_value_kind_t kind, uint8_t *out_count) {
    const settings_option_t *options = OPTIONS_OFF_ON;
    uint8_t count = (uint8_t)ARRAY_COUNT(OPTIONS_OFF_ON);
    switch (kind) {
    case SETTINGS_VALUE_AUTOMATIC_REDIAL:
    case SETTINGS_VALUE_SPEED_DIALLING:
    case SETTINGS_VALUE_CELL_INFO:
        options = OPTIONS_ON_OFF;
        count = (uint8_t)ARRAY_COUNT(OPTIONS_ON_OFF);
        break;
    case SETTINGS_VALUE_OWN_NUMBER:
    case SETTINGS_VALUE_CLOSED_USER_GROUP:
        options = OPTIONS_OWN_NUMBER;
        count = (uint8_t)ARRAY_COUNT(OPTIONS_OWN_NUMBER);
        break;
    case SETTINGS_VALUE_PHONE_LINE:
        options = OPTIONS_PHONE_LINE;
        count = (uint8_t)ARRAY_COUNT(OPTIONS_PHONE_LINE);
        break;
    case SETTINGS_VALUE_NETWORK:
        options = OPTIONS_NETWORK;
        count = (uint8_t)ARRAY_COUNT(OPTIONS_NETWORK);
        break;
    case SETTINGS_VALUE_LIGHTS:
        options = OPTIONS_LIGHTS;
        count = (uint8_t)ARRAY_COUNT(OPTIONS_LIGHTS);
        break;
    case SETTINGS_VALUE_CONFIRM_SIM:
        options = OPTIONS_CONFIRM_SIM;
        count = (uint8_t)ARRAY_COUNT(OPTIONS_CONFIRM_SIM);
        break;
    case SETTINGS_VALUE_PIN_REQUEST:
        options = OPTIONS_PIN_REQUEST;
        count = (uint8_t)ARRAY_COUNT(OPTIONS_PIN_REQUEST);
        break;
    case SETTINGS_VALUE_FIXED_DIALLING:
        options = OPTIONS_FIXED_DIALLING;
        count = (uint8_t)ARRAY_COUNT(OPTIONS_FIXED_DIALLING);
        break;
    case SETTINGS_VALUE_PHONE_SECURITY:
        options = OPTIONS_PHONE_SECURITY;
        count = (uint8_t)ARRAY_COUNT(OPTIONS_PHONE_SECURITY);
        break;
    case SETTINGS_VALUE_CALL_WAITING:
        options = OPTIONS_CALL_WAITING;
        count = (uint8_t)ARRAY_COUNT(OPTIONS_CALL_WAITING);
        break;
    case SETTINGS_VALUE_LANGUAGE:
        build_language_options();
        options = s_language_options;
        count = s_language_option_count;
        break;
    case SETTINGS_VALUE_AUTOMATIC_ANSWER:
        /* SET.4: conf-0xaa lists On(1) before Off(0). */
        options = OPTIONS_ON_OFF;
        count = (uint8_t)ARRAY_COUNT(OPTIONS_ON_OFF);
        break;
    default:
        break;
    }
    *out_count = count;
    return options;
}

/* v6.00 per-picker value-label SIDs, read from the settings config-picker
 * descriptors: each picker's On/Off/etc. is a DISTINCT context SID (redial On
 * 0x9d vs speed-dial On 0x218 vs auto-answer On 0x5b), so they can't share one
 * table. Parallel to each kind's settings_value_options() order. Kinds absent
 * here return 0 -> keep the literal: Language (self-names must not translate)
 * and the supplementary/SIM pickers (Call waiting / Fixed dialling / Phone
 * security -- modeled, and their labels are cross-context ambiguous). */
static uint16_t settings_value_sid(settings_value_kind_t kind, uint8_t index) {
    static const uint16_t REDIAL[]  = {0x9du, 0x9cu};           /* On, Off */
    static const uint16_t SPEED[]   = {0x218u, 0x217u};         /* On, Off */
    static const uint16_t CELL[]    = {0x1aeu, 0x1adu};         /* On, Off */
    static const uint16_t ANSWER[]  = {0x5bu, 0x5au};           /* On, Off */
    static const uint16_t OWNNUM[]  = {0xd0u, 0xd2u, 0xd1u};    /* Preset, On, Off */
    static const uint16_t CUG[]     = {0x116u, 0x118u, 0x11au}; /* Preset, On, Off */
    static const uint16_t LINE[]    = {0x52u, 0x4au};           /* Line 1, Line 2 */
    static const uint16_t NET[]     = {0x5du, 0x1a8u};          /* Automatic, Manual */
    static const uint16_t LIGHTS[]  = {0x1a4u, 0x1a5u};         /* On, Automatic */
    static const uint16_t CONFIRM[] = {0x2bcu, 0x2bbu};         /* Asked, Not asked */
    static const uint16_t PIN[]     = {0x23bu, 0x23au};         /* On, Off */
    static const uint16_t CWAIT[]   = {0x3cu, 0xa9u, 0x3aau};   /* Activate, Cancel, Status (CW's own set, not divert's 0x3b/0xa8/0xca) */
    static const uint16_t FDIAL[]   = {0x14du, 0x14cu, 0x14au}; /* On, Off, Numbers (dedicated FD block) */
    static const uint16_t PSEC[]    = {0x2d2u, 0x2d3u};         /* Off, On (dedicated block after title 0x2d1) */
    const uint16_t *table = 0;
    uint8_t n = 0u;
    switch (kind) {
    case SETTINGS_VALUE_AUTOMATIC_REDIAL:  table = REDIAL;  n = 2u; break;
    case SETTINGS_VALUE_SPEED_DIALLING:    table = SPEED;   n = 2u; break;
    case SETTINGS_VALUE_CELL_INFO:         table = CELL;    n = 2u; break;
    case SETTINGS_VALUE_AUTOMATIC_ANSWER:  table = ANSWER;  n = 2u; break;
    case SETTINGS_VALUE_OWN_NUMBER:        table = OWNNUM;  n = 3u; break;
    case SETTINGS_VALUE_CLOSED_USER_GROUP: table = CUG;     n = 3u; break;
    case SETTINGS_VALUE_PHONE_LINE:        table = LINE;    n = 2u; break;
    case SETTINGS_VALUE_NETWORK:           table = NET;     n = 2u; break;
    case SETTINGS_VALUE_LIGHTS:            table = LIGHTS;  n = 2u; break;
    case SETTINGS_VALUE_CONFIRM_SIM:       table = CONFIRM; n = 2u; break;
    case SETTINGS_VALUE_PIN_REQUEST:       table = PIN;     n = 2u; break;
    case SETTINGS_VALUE_CALL_WAITING:      table = CWAIT;   n = 3u; break;
    case SETTINGS_VALUE_FIXED_DIALLING:    table = FDIAL;   n = 3u; break;
    case SETTINGS_VALUE_PHONE_SECURITY:    table = PSEC;    n = 2u; break;
    case SETTINGS_VALUE_LANGUAGE:          return index == 0u ? 0x1a5u : 0u; /* Automatic localized; self-names literal */
    default:                               return 0u;
    }
    return (index < n) ? table[index] : 0u;
}

static store_setting_key_t settings_value_store_key(settings_value_kind_t kind) {
    switch (kind) {
    case SETTINGS_VALUE_AUTOMATIC_REDIAL: return STORE_SETTING_SETTINGS_AUTOMATIC_REDIAL;
    case SETTINGS_VALUE_SPEED_DIALLING: return STORE_SETTING_SETTINGS_SPEED_DIALLING;
    case SETTINGS_VALUE_OWN_NUMBER: return STORE_SETTING_CALL_OWN_NUMBER_SENDING;
    case SETTINGS_VALUE_PHONE_LINE: return STORE_SETTING_SETTINGS_PHONE_LINE;
    case SETTINGS_VALUE_AUTOMATIC_ANSWER: return STORE_SETTING_SETTINGS_AUTOMATIC_ANSWER;
    case SETTINGS_VALUE_LANGUAGE: return STORE_SETTING_SYSTEM_LANGUAGE;
    case SETTINGS_VALUE_CELL_INFO: return STORE_SETTING_SETTINGS_CELL_INFO_DISPLAY;
    case SETTINGS_VALUE_NETWORK: return STORE_SETTING_SETTINGS_NETWORK_SELECTION;
    case SETTINGS_VALUE_LIGHTS: return STORE_SETTING_SETTINGS_LIGHTS;
    case SETTINGS_VALUE_CONFIRM_SIM: return STORE_SETTING_SETTINGS_CONFIRM_SIM_ACTIONS;
    case SETTINGS_VALUE_PIN_REQUEST: return STORE_SETTING_SETTINGS_PIN_CODE_REQUEST;
    case SETTINGS_VALUE_FIXED_DIALLING: return STORE_SETTING_SETTINGS_FIXED_DIALLING;
    case SETTINGS_VALUE_CLOSED_USER_GROUP: return STORE_SETTING_SETTINGS_CLOSED_USER_GROUP;
    case SETTINGS_VALUE_PHONE_SECURITY: return STORE_SETTING_SETTINGS_PHONE_SECURITY;
    case SETTINGS_VALUE_CALL_WAITING:
    default: return STORE_SETTING_SETTINGS_AUTOMATIC_REDIAL;
    }
}

static uint8_t settings_current_value(settings_value_kind_t kind) {
    uint8_t value = 0u;
    if (kind == SETTINGS_VALUE_CALL_WAITING) {
        return 4u;
    }
    store_setting_get_u8(settings_value_store_key(kind), &value);
    return value;
}

static uint8_t settings_selected_for_value(settings_value_kind_t kind, uint8_t value) {
    uint8_t count = 0u;
    const settings_option_t *options = settings_value_options(kind, &count);
    for (uint8_t i = 0; i < count; i++) {
        if (options[i].value == value) {
            return i;
        }
    }
    return 0u;
}

static const char *settings_value_label(settings_value_kind_t kind, uint8_t value, char *scratch, size_t scratch_cap) {
    uint8_t count = 0u;
    const settings_option_t *options = settings_value_options(kind, &count);
    for (uint8_t i = 0; i < count; i++) {
        if (options[i].value == value) {
            return ts_or(settings_value_sid(kind, i), options[i].label);
        }
    }
    copy_text(scratch, scratch_cap, ts_or(settings_value_sid(kind, 0u), options[0].label));
    return scratch;
}

static bool apply_settings_value(app_t *app, uint32_t now) {
    uint8_t count = 0u;
    settings_value_kind_t kind = (settings_value_kind_t)app->settings_value_kind;
    const settings_option_t *options = settings_value_options(kind, &count);
    if (count == 0u) {
        return true;
    }
    if (app->settings_value_selected >= count) {
        app->settings_value_selected = 0u;
    }
    uint8_t value = options[app->settings_value_selected].value;
    if (kind == SETTINGS_VALUE_CALL_WAITING) {
        app->settings_pending_action = SETTINGS_PENDING_CALL_WAITING;
        app->settings_pending_started_ms = now;
        open_display_sid(app, 35u, 0x2a4u, "Requesting", APP_ROUTE_SETTINGS_MENU, now);
        return true;
    }
    if (kind == SETTINGS_VALUE_NETWORK && value == 1u) {
        app->settings_pending_action = SETTINGS_PENDING_NETWORK_SEARCH;
        app->settings_pending_started_ms = now;
        open_display_sid(app, 35u, 0x299u, "Searching", APP_ROUTE_SETTINGS_MENU, now);
        return true;
    }
    if (kind == SETTINGS_VALUE_PIN_REQUEST) {
        app->settings_pending_value = value;
        open_editor(app, ts_or(0x22fu, "Enter\nPIN code:"), "", 8u, EDITOR_KIND_NUMBER, EDITOR_CONTEXT_SETTINGS_PIN_REQUEST, false, now);
        return true;
    }
    if (kind == SETTINGS_VALUE_FIXED_DIALLING) {
        open_display_sid(app, 2u, 0x297u, "SIM card\nnot ready", APP_ROUTE_SETTINGS_MENU, now);
        return true;
    }
    if (kind == SETTINGS_VALUE_PHONE_SECURITY) {
        open_display_sid(app, 2u, 0x29au, "Security\ncode:", APP_ROUTE_SETTINGS_MENU, now);
        return true;
    }
    if (kind == SETTINGS_VALUE_PHONE_LINE) {
        /* SET.3: SID 0x0241 "Line 1\nselected" / 0x0053 "Line 2\nselected". */
        store_setting_set_u8(settings_value_store_key(kind), value);
        return_to_settings_parent(app);
        if (value == 1u) {
            open_display_sid(app, 3u, 0x53u, "Line 2\nselected", APP_ROUTE_SETTINGS_MENU, now);
        } else {
            open_display_sid(app, 3u, 0x241u, "Line 1\nselected", APP_ROUTE_SETTINGS_MENU, now);
        }
        return true;
    }
    store_setting_set_u8(settings_value_store_key(kind), value);
    if (kind == SETTINGS_VALUE_LANGUAGE) {
        /* SET.1: the new language drives the whole UI immediately. */
        strings_set_language(value);
    }
    settings_show_saved(app, now);
    return true;
}

static void open_welcome_note_editor(app_t *app, uint32_t now) {
    char note[STORE_TEXT_MAX + 1u];
    store_setting_get_text(STORE_SETTING_SYSTEM_WELCOME_NOTE, note, sizeof(note));
    app->settings_value_parent_kind = (uint8_t)SETTINGS_MENU_PHONE;
    app->settings_value_parent_selected = 2u;
    open_editor(app,
                "Welcome note:",
                note,
                36u,
                EDITOR_KIND_TEXT,
                EDITOR_CONTEXT_SETTINGS_WELCOME_NOTE,
                true,
                now);
}

static void open_settings_access_editor(app_t *app, uint8_t kind, uint32_t now) {
    app->settings_access_kind = kind;
    app->settings_access_step = 0u;
    app->settings_value_parent_kind = (uint8_t)SETTINGS_MENU_ACCESS_CODES;
    app->settings_value_parent_selected = kind;
    app->settings_access_new_code[0] = '\0';
    open_editor(app,
                settings_access_prompt(kind, 0u),
                "",
                settings_access_max_len(kind),
                EDITOR_KIND_NUMBER,
                EDITOR_CONTEXT_SETTINGS_ACCESS_CODE,
                false,
                now);
}

static const char *settings_access_prompt(uint8_t kind, uint8_t step) {
    /* SET.6: full traced prompt strings, each localized by its v6.00 SID. The
     * step-0 "Security code:" editor title resolves to 0x29a -- the clone had
     * flattened the ROM's "Security\ncode:" onto one line; the renderer honors
     * the '\n', so using the SID restores the ROM's exact per-language text. */
    if (kind == 0u) {
        return step == 0u ? ts_or(0x29au, "Security\ncode:")
                          : (step == 1u ? ts_or(0x29du, "Enter new\nsecurity code:")
                                        : ts_or(0x29eu, "Verify new\nsecurity code:"));
    }
    if (kind == 1u) {
        return step == 0u ? ts_or(0x214u, "Current\nPIN code:")
                          : (step == 1u ? ts_or(0x1dbu, "Enter new\nPIN code:")
                                        : ts_or(0x3ecu, "Verify new\nPIN code:"));
    }
    return step == 0u ? ts_or(0x215u, "Current\nPIN2 code:")
                      : (step == 1u ? ts_or(0x1dcu, "Enter new\nPIN2 code:")
                                    : ts_or(0x3edu, "Verify new\nPIN2 code:"));
}

static uint8_t settings_access_max_len(uint8_t kind) {
    return kind == 0u ? 10u : 8u;
}

static void return_to_settings_parent(app_t *app) {
    open_settings_menu(app,
                       (settings_menu_kind_t)app->settings_value_parent_kind,
                       app->settings_value_parent_selected);
}

static void settings_show_saved(app_t *app, uint32_t now) {
    return_to_settings_parent(app);
    /* v6.00 confirms a setting change with the generic "Done" note (SID 0x132),
     * not "Saved" (which is for storing a number/text). Bench-confirmed on the
     * language picker. */
    open_display_sid(app, 3u, 0x132u, "Done", APP_ROUTE_SETTINGS_MENU, now);
}

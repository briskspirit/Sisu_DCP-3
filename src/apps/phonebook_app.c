#include "apps/phonebook_app.h"
#include "ui/menu_visible.h"

#include <stdio.h>
#include <string.h>

#include "apps/calls_app.h"
#include "apps/dialogs_app.h"
#include "apps/messages_app.h"
#include "apps/tones_app.h"
#include "services/feature_gates.h"
#include "services/input_keys.h"
#include "services/key_utils.h"
#include "services/modem_service.h"
#include "services/phone_match.h"
#include "storage/store_service.h"
#include "services/strings.h"
#include "services/timebase.h"

#define PHONEBOOK_ERASE_ALL_PROGRESS_MS 11000u
/* The longest write path is CPBS (5 s) + CPBW (10 s) + CPBR (15 s). Keep a
 * defensive UI bound above that service-owned chain; normal command errors and
 * timeouts arrive earlier as tokenized terminal results. */
#define PHONEBOOK_OPERATION_TIMEOUT_MS 45000u
#define PHONEBOOK_SEND_TIMEOUT_MS 45000u

static const char *const PHONEBOOK_ROOT_LABELS[] = {
    "Search",
    "Service Nos.",
    "Add entry",
    "Erase",
    "Edit",
    "Assign tone",
    "Send entry",
    "Options",
    "Speed dials",
    "Info numbers",
};
static const char *const PHONEBOOK_ERASE_LABELS[] = {"One by one", "Erase all"};
static const char *const PHONEBOOK_OPTIONS_LABELS[] = {"Type of view", "Memory status"};
static const char *const PHONEBOOK_TYPE_VIEW_LABELS[] = {"Name list", "Name, number", "Large font"};
/* PB.2: assign-tone picker = No tone + Preset + the full ringing-tone
 * catalogue (traced list source 0x00276d9c), with preview while browsing. */
#define PHONEBOOK_TONE_FIXED_ROWS 2u
static const char *const PHONEBOOK_SPEED_OPTION_LABELS[] = {"Call", "Change", "Erase"};
static const char *const PHONEBOOK_EDIT_CHOICE_LABELS[] = {"Replace", "Create entry"};
static const uint8_t PHONEBOOK_SPEED_KEYS[] = {2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u};

/* v6.00 string ids (SID), parallel to the label arrays above. 0 = no 1:1 match
 * (the English text maps to several context SIDs -- ambiguous -- or has no ROM
 * record) so the English literal is kept. The label[] arrays remain the single
 * source of truth for indexing/gating/dispatch (e.g. PHONEBOOK_SPEED_OPTION_LABELS
 * is a strcmp key in handle_phonebook_speed_options_key); only the RENDERED copy
 * is localized, via L(). */
static const uint16_t PHONEBOOK_ROOT_SIDS[] = {
    0x14fu,   /* Search       (per-app trace map: 0x14f, not 737/892) */
    0x2a6u,   /* Service Nos. */
    0x295u,   /* Add entry    (per-app trace map: 0x295, not 0x148) */
    0x1b3u,   /* Erase        (phonebook menu block, adjacent to Speed dials 0x1af; trace) */
    0x1b2u,   /* Edit         (phonebook menu block; trace: "Edit" @0x1b2) */
    0x0cdu,   /* Assign tone */
    0x2c2u,   /* Send entry  */
    0x1b5u,   /* Options      (phonebook menu block) */
    0x1afu,   /* Speed dials  (per-app trace map: 0x1af, not 0x1b6) */
    0x1c0u,   /* Info numbers */
};
static const uint16_t PHONEBOOK_ERASE_SIDS[] = {
    0x1c1u,   /* One by one */
    0x1bdu,   /* Erase all    (per-app trace map: 0x1bd, not 0x149) */
};
static const uint16_t PHONEBOOK_OPTIONS_SIDS[] = {
    0x1bbu,   /* Type of view */
    0x1bcu,   /* Memory status (ROM record 0x1bc = "Memory\nstatus") */
};
static const uint16_t PHONEBOOK_TYPE_VIEW_SIDS[] = {
    0x1b8u,   /* Name list */
    0x1b9u,   /* Name, number */
    0x1bau,   /* Large font */
};
static const uint16_t PHONEBOOK_SPEED_OPTION_SIDS[] = {
    0u,       /* Call   -> ambiguous */
    0x3a1u,   /* Change */
    0u,       /* Erase  -> ambiguous */
};

/* Localize by SID with English fallback: sid 0 (ambiguous / no 1:1 match) or a
 * sid with no record (ts()==NULL, clone-only) returns the supplied literal. */
static const char *L(uint16_t sid, const char *en) {
    if (sid != 0u) {
        const char *t = ts(sid);
        if (t != 0) {
            return t;
        }
    }
    return en;
}

static void build_phonebook_visible(app_t *app, const char *query);
static bool phonebook_current_entry(const app_t *app, modem_phonebook_entry_t *entry, uint16_t *cache_position);
static uint16_t find_phonebook_entry_position(const char *name, const char *number, uint16_t fallback);
static void begin_phonebook_edit(app_t *app, const modem_phonebook_entry_t *entry, uint16_t cache_position, uint32_t now);
static void begin_phonebook_erase(app_t *app, const modem_phonebook_entry_t *entry, uint16_t cache_position);
static void begin_phonebook_assign(app_t *app, const modem_phonebook_entry_t *entry, uint16_t cache_position);
static void begin_phonebook_send(app_t *app, const modem_phonebook_entry_t *entry, uint16_t cache_position, uint32_t now);
static void open_phonebook_send_recipient_editor(app_t *app, const char *value, uint32_t now);
static void open_phonebook_speed_dials(app_t *app);
static uint16_t selected_phonebook_speed_contact(const app_t *app);
static void refresh_speed_selected(app_t *app);
static uint8_t phonebook_view_mode(void);
static void set_phonebook_view_mode(uint8_t mode);
static const char *phonebook_label_text(phonebook_label_t label);
static const char *phonebook_context_return_softkey(phonebook_context_t context);
static app_route_t phonebook_empty_return_route(phonebook_context_t context);
static bool phonebook_entry_matches(const modem_phonebook_entry_t *entry, const char *query);
static bool phonebook_root_item_visible(uint8_t raw);
static uint8_t phonebook_menu_visible_count(phonebook_menu_kind_t kind);
static uint8_t phonebook_menu_visible_index(phonebook_menu_kind_t kind, uint8_t raw);
static uint8_t phonebook_menu_raw_at_visible(phonebook_menu_kind_t kind, uint8_t visible);
static uint8_t phonebook_menu_normalized_raw(phonebook_menu_kind_t kind, uint8_t raw);
static void phonebook_menu_step(app_t *app, int8_t delta);
static uint8_t phonebook_tone_picker_count(void);
static const char *phonebook_tone_picker_label(uint8_t row);
static uint8_t phonebook_tone_picker_value(uint8_t row);
static const char *phonebook_tone_value_text(uint8_t value);
static void phonebook_return_to_parent_menu(app_t *app);
static void phonebook_clear_request(app_t *app);
static void phonebook_claim_request_display(app_t *app);
static bool phonebook_expected_operation(const app_t *app,
                                         modem_phonebook_op_t *out);

void render_phonebook_menu(const app_t *app, framebuffer_t *fb) {
    char breadcrumb[14];
    const char *const *labels = PHONEBOOK_ROOT_LABELS;
    const uint16_t *sids = PHONEBOOK_ROOT_SIDS;
    const char *visible_labels[ARRAY_COUNT(PHONEBOOK_ROOT_LABELS)];
    uint16_t visible_sids[ARRAY_COUNT(PHONEBOOK_ROOT_LABELS)];
    uint8_t count = phonebook_menu_visible_count((phonebook_menu_kind_t)app->phonebook_menu_kind);
    uint8_t selected_raw = phonebook_menu_normalized_raw((phonebook_menu_kind_t)app->phonebook_menu_kind,
                                                         app->phonebook_menu_selected);
    uint8_t selected_visible = phonebook_menu_visible_index((phonebook_menu_kind_t)app->phonebook_menu_kind,
                                                           selected_raw);
    if (app->phonebook_menu_kind == PHONEBOOK_MENU_ERASE) {
        labels = PHONEBOOK_ERASE_LABELS;
        sids = PHONEBOOK_ERASE_SIDS;
        ui_breadcrumb_path(breadcrumb, sizeof(breadcrumb), "1-4", (unsigned)(selected_raw + 1u));
    } else if (app->phonebook_menu_kind == PHONEBOOK_MENU_OPTIONS) {
        /* PB.1: descriptor 0x002d09d8 dispmode 0x80 = one-entry static page;
         * "Type of view" (flags 0x08) previews its current value lower-right. */
        ui_breadcrumb_path(breadcrumb, sizeof(breadcrumb), "1-8", (unsigned)(selected_raw + 1u));
        uint8_t view = phonebook_view_mode();
        const char *preview = selected_raw == 0u
            ? L(PHONEBOOK_TYPE_VIEW_SIDS[view], PHONEBOOK_TYPE_VIEW_LABELS[view])
            : 0;
        uint8_t opt = selected_raw < count ? selected_raw : 0u;
        draw_static_page_list(fb,
                              L(PHONEBOOK_OPTIONS_SIDS[opt], PHONEBOOK_OPTIONS_LABELS[opt]),
                              preview,
                              selected_raw,
                              count,
                              breadcrumb,
                              "Select");
        return;
    } else if (app->phonebook_menu_kind == PHONEBOOK_MENU_TYPE_VIEW) {
        labels = PHONEBOOK_TYPE_VIEW_LABELS;
        sids = PHONEBOOK_TYPE_VIEW_SIDS;
        ui_breadcrumb_path(breadcrumb, sizeof(breadcrumb), "1-8-1", (unsigned)(selected_raw + 1u));
    } else {
        for (uint8_t i = 0; i < count; i++) {
            uint8_t raw = phonebook_menu_raw_at_visible(PHONEBOOK_MENU_ROOT, i);
            visible_labels[i] = PHONEBOOK_ROOT_LABELS[raw];
            visible_sids[i] = PHONEBOOK_ROOT_SIDS[raw];
        }
        labels = visible_labels;
        sids = visible_sids;
        /* Counter uses the raw STORED ordinal even when gated rows are hidden. */
        ui_breadcrumb_path(breadcrumb, sizeof(breadcrumb), "1", (unsigned)(selected_raw + 1u));
    }
    /* Render the localized copy; labels[]/sids[] stay aligned (visible index ==
     * SID index for every kind), and the label arrays are untouched for
     * indexing/gating/dispatch. */
    const char *localized[ARRAY_COUNT(PHONEBOOK_ROOT_LABELS)];
    if (count > (uint8_t)ARRAY_COUNT(localized)) {
        count = (uint8_t)ARRAY_COUNT(localized);
    }
    for (uint8_t i = 0; i < count; i++) {
        localized[i] = L(sids[i], labels[i]);
    }
    draw_flat_list(fb, localized, count, selected_visible, breadcrumb, "Select");
}

void render_phonebook_list(const app_t *app, framebuffer_t *fb) {
    fb_clear(fb, false);
    if (app->phonebook_visible_count == 0u) {
        draw_text_block(fb, asset_font(FONT_FS0), ts_or(0x276u, "No phone\nnumbers"),
                        0, 9, FB_WIDTH, 13, 3u);
        return;
    }
    const font_t *small = asset_font(FONT_FS2);
    const font_t *large = asset_font(FONT_FS0);
    uint8_t view = phonebook_view_mode();
    if (app->phonebook_context == PHONEBOOK_CONTEXT_ASSIGN) {
        view = 1u;
    }
    if (view == 0u) {
        /* Name-list view keeps the slot-04 bitmap-0030 top-strip icon. */
        fb_bitmap(fb, 30u, 0, 0, true, true);
        uint16_t start = app->phonebook_list_selected > 2u ? app->phonebook_list_selected - 2u : 0u;
        if (app->phonebook_visible_count > 3u && start > app->phonebook_visible_count - 3u) {
            start = app->phonebook_visible_count - 3u;
        }
        for (uint8_t row = 0; row < 3u && start + row < app->phonebook_visible_count; row++) {
            uint16_t pos = app->phonebook_visible_indices[start + row];
            modem_phonebook_entry_t entry;
            if (!modem_service_phonebook_entry(pos, &entry)) {
                continue;
            }
            int y = 8 + row * 9;
            bool selected = (start + row) == app->phonebook_list_selected;
            const char *label = entry.name[0] ? entry.name : entry.number;
            if (selected) {
                fb_fill_rect(fb, 0, y - 1, 84, 9, true);
                char scroll[44];
                fb_text(fb, small, ui_marquee_text(small, label, 81, scroll, sizeof(scroll)), 2, y, false, 81);
            } else {
                fb_text(fb, small, label, 2, y, true, 81);
            }
        }
        draw_softkey(fb, phonebook_label_text((phonebook_label_t)app->phonebook_pending_label));
        return;
    }
    modem_phonebook_entry_t entry;
    if (!phonebook_current_entry(app, &entry, 0)) {
        return;
    }
    fb_bitmap(fb, 30u, 0, 0, true, true);
    const char *name = entry.name[0] ? entry.name : entry.number;
    char lower[STORE_TEXT_MAX + 1u];
    copy_text(lower, sizeof(lower), entry.number);
    lower[sizeof(lower) - 1u] = '\0';
    if (app->phonebook_context == PHONEBOOK_CONTEXT_ASSIGN) {
        copy_text(lower, sizeof(lower),
                  phonebook_tone_value_text(store_phonebook_get_contact_tone_value(entry.index)));
    }
    if (view == 1u) {
        fb_text(fb, small, name, 0, 7, true, FB_WIDTH);
        draw_text_right(fb, small, lower, 28, FB_WIDTH);
    } else if (entry.name[0] != '\0') {
        fb_text(fb, large, name, 1, 7, true, FB_WIDTH - 1);
    } else {
        /* Large-font view number fallback sits at the traced y=24. */
        draw_text_right(fb, small, lower, 24, FB_WIDTH);
    }
    draw_softkey(fb, phonebook_context_return_softkey((phonebook_context_t)app->phonebook_context));
}

void render_phonebook_memory(const app_t *app, framebuffer_t *fb) {
    (void)app;
    fb_clear(fb, false);
    const font_t *font = asset_font(FONT_FS2);
    uint16_t used = modem_service_phonebook_count();
    uint16_t free = used >= MODEM_PHONEBOOK_MAX_RECORDS
        ? 0u
        : (uint16_t)(MODEM_PHONEBOOK_MAX_RECORDS - used);
    char line[18];
    fb_text(fb, font, ts_or(0x2acu, "SIM card:"), 0, 7, true, FB_WIDTH);
    snprintf(line, sizeof(line), "%u free", (unsigned)free);
    fb_text(fb, font, line, 0, 16, true, FB_WIDTH);
    snprintf(line, sizeof(line), "%u in use", (unsigned)used);
    fb_text(fb, font, line, 0, 25, true, FB_WIDTH);
    draw_softkey(fb, ts_or(0x2e9u, "OK"));
}

void render_phonebook_tone_picker(const app_t *app, framebuffer_t *fb) {
    const char *labels[48];
    uint8_t count = phonebook_tone_picker_count();
    if (count > (uint8_t)ARRAY_COUNT(labels)) {
        count = (uint8_t)ARRAY_COUNT(labels);
    }
    for (uint8_t i = 0; i < count; i++) {
        labels[i] = phonebook_tone_picker_label(i);
    }
    char breadcrumb[12];
    snprintf(breadcrumb, sizeof(breadcrumb), "1-6-%u", (unsigned)(app->phonebook_tone_selected + 1u));
    draw_flat_list(fb, labels, count, app->phonebook_tone_selected, breadcrumb, ts_or(0x2d7u, "Assign"));
}

void render_phonebook_speed_dials(const app_t *app, framebuffer_t *fb) {
    fb_clear(fb, false);
    const font_t *small = asset_font(FONT_FS2);
    const font_t *large = asset_font(FONT_FS0);
    fb_bitmap(fb, 30u, 0, 0, true, true);
    uint8_t key = PHONEBOOK_SPEED_KEYS[app->phonebook_speed_selected];
    uint16_t contact_index = selected_phonebook_speed_contact(app);
    char line[18];
    snprintf(line, sizeof(line), "Key %u:", (unsigned)key);
    fb_text(fb, small, line, 2, 7, true, 80);
    const char *value = ts_or(0x3a0u, "(empty)");
    char value_buf[MODEM_PHONEBOOK_NAME_MAX + 1u];
    modem_phonebook_entry_t entry;
    for (uint16_t i = 0; i < modem_service_phonebook_count(); i++) {
        if (modem_service_phonebook_entry(i, &entry) && entry.index == contact_index) {
            copy_text(value_buf, sizeof(value_buf), entry.name[0] ? entry.name : entry.number);
            value_buf[sizeof(value_buf) - 1u] = '\0';
            value = value_buf;
            break;
        }
    }
    /* Overflowing values scroll as the 1024 ms marquee (1:1 with the ROM), not a
     * static left-clipped tail; codepoint-aware so UTF-8 names aren't split. */
    char scroll[44];
    fb_text(fb, large, ui_marquee_text(large, value, 80, scroll, sizeof(scroll)), 2, 16, true, 80);
    draw_position_indicator(fb, app->phonebook_speed_selected, (uint8_t)ARRAY_COUNT(PHONEBOOK_SPEED_KEYS), "");
    /* "Options" has no per-app SID; draw_softkey localizes it via ts_softkey
     * (the framework menu-bar block, SID 0x2ea). */
    draw_softkey(fb, contact_index == STORE_SPEED_DIAL_EMPTY ? ts_or(0x2d7u, "Assign") : "Options");
}

void render_phonebook_speed_options(const app_t *app, framebuffer_t *fb) {
    /* Localize only the rendered copy; PHONEBOOK_SPEED_OPTION_LABELS stays the
     * strcmp dispatch key in handle_phonebook_speed_options_key. */
    const char *localized[ARRAY_COUNT(PHONEBOOK_SPEED_OPTION_LABELS)];
    for (uint8_t i = 0; i < (uint8_t)ARRAY_COUNT(PHONEBOOK_SPEED_OPTION_LABELS); i++) {
        localized[i] = L(PHONEBOOK_SPEED_OPTION_SIDS[i], PHONEBOOK_SPEED_OPTION_LABELS[i]);
    }
    draw_flat_list(fb, localized, (uint8_t)ARRAY_COUNT(PHONEBOOK_SPEED_OPTION_LABELS), app->phonebook_speed_options_selected, UI_BREADCRUMB_NONE, "Select");
}

void render_phonebook_edit_choice(const app_t *app, framebuffer_t *fb) {
    draw_flat_list(fb, PHONEBOOK_EDIT_CHOICE_LABELS, (uint8_t)ARRAY_COUNT(PHONEBOOK_EDIT_CHOICE_LABELS), app->phonebook_edit_choice_selected, UI_BREADCRUMB_NONE, "Select");
}

void render_phonebook_security(const app_t *app, framebuffer_t *fb) {
    fb_clear(fb, false);
    const font_t *font = asset_font(FONT_FS2);
    fb_bitmap(fb, 14u, 0, 0, true, true);
    draw_text_block(fb, font, ts_or(0x29au, "Security\ncode:"), 0, 7, FB_WIDTH, 9, 2u);
    char stars[12];
    uint8_t len = (uint8_t)strlen(app->editor_value);
    if (len >= sizeof(stars)) {
        len = (uint8_t)(sizeof(stars) - 1u);
    }
    memset(stars, '*', len);
    stars[len] = '\0';
    fb_text(fb, font, stars, 0, 28, true, FB_WIDTH);
    draw_softkey(fb, ts_or(0x2e9u, "OK"));
}

bool handle_phonebook_menu_key(app_t *app, uint16_t key, uint32_t now) {
    phonebook_menu_kind_t kind = (phonebook_menu_kind_t)app->phonebook_menu_kind;
    uint8_t count = phonebook_menu_visible_count(kind);
    if (count == 0u) {
        return true;
    }
    app->phonebook_menu_selected = phonebook_menu_normalized_raw(kind, app->phonebook_menu_selected);
    if (key == KEY_UP) {
        phonebook_menu_step(app, -1);
        return true;
    }
    if (key == KEY_DOWN) {
        phonebook_menu_step(app, 1);
        return true;
    }
    if (key == KEY_C) {
        if (app->phonebook_menu_kind == PHONEBOOK_MENU_ROOT) {
            app->route = APP_ROUTE_MAIN_MENU;
        } else if (app->phonebook_menu_kind == PHONEBOOK_MENU_ERASE) {
            open_phonebook_menu(app, PHONEBOOK_MENU_ROOT, 3u);
        } else if (app->phonebook_menu_kind == PHONEBOOK_MENU_TYPE_VIEW) {
            open_phonebook_menu(app, PHONEBOOK_MENU_OPTIONS, 0u);
        } else {
            open_phonebook_menu(app, PHONEBOOK_MENU_ROOT, 7u);
        }
        app->dirty = true;
        return true;
    }
    if (key != KEY_NAVI) {
        return true;
    }

    if (app->phonebook_menu_kind == PHONEBOOK_MENU_ROOT) {
        switch (app->phonebook_menu_selected) {
        case 0:
            open_editor(app, ts_or(0x283u, "Name:"), "", 16u, EDITOR_KIND_TEXT, EDITOR_CONTEXT_PHONEBOOK_SEARCH, false, now);
            return true;
        case 1:
            /* SET.5: Service Nos. never uses the plural "No phone numbers";
             * with no SIM list backend the failure branch shows "Not
             * allowed" (SID 0x020d). */
            open_display_sid(app, 0u, 0x20du, "Not\nallowed", APP_ROUTE_PHONEBOOK_MENU, now);
            return true;
        case 2:
            app->editor_draft_name[0] = '\0';
            app->editor_draft_number[0] = '\0';
            open_editor(app, ts_or(0x283u, "Name:"), "", 16u, EDITOR_KIND_TEXT, EDITOR_CONTEXT_PHONEBOOK_ADD_NAME, true, now);
            return true;
        case 3:
            open_phonebook_menu(app, PHONEBOOK_MENU_ERASE, 0u);
            return true;
        case 4:
            start_phonebook_list(app, PHONEBOOK_LABEL_EDIT, "1-5", PHONEBOOK_CONTEXT_EDIT, 0u, now);
            return true;
        case 5:
            start_phonebook_list(app, PHONEBOOK_LABEL_ASSIGN, "1-6", PHONEBOOK_CONTEXT_ASSIGN, 0u, now);
            return true;
        case 6:
            start_phonebook_list(app, PHONEBOOK_LABEL_SEND, "1-7", PHONEBOOK_CONTEXT_SEND, 0u, now);
            return true;
        case 7:
            open_phonebook_menu(app, PHONEBOOK_MENU_OPTIONS, 0u);
            return true;
        case 8:
            open_phonebook_speed_dials(app);
            return true;
        default:
            /* SET.5: Info numbers — same "Not allowed" failure branch. */
            open_display_sid(app, 0u, 0x20du, "Not\nallowed", APP_ROUTE_PHONEBOOK_MENU, now);
            return true;
        }
    }
    if (app->phonebook_menu_kind == PHONEBOOK_MENU_ERASE) {
        if (app->phonebook_menu_selected == 0u) {
            start_phonebook_list(app, PHONEBOOK_LABEL_ERASE, "1-4-1", PHONEBOOK_CONTEXT_ERASE, 0u, now);
        } else {
            open_confirm_sid(app, CONFIRM_CONTEXT_PHONEBOOK_ERASE_ALL, 0x56u, "Are you\nsure?", 2);
        }
        return true;
    }
    if (app->phonebook_menu_kind == PHONEBOOK_MENU_OPTIONS) {
        if (app->phonebook_menu_selected == 0u) {
            open_phonebook_menu(app, PHONEBOOK_MENU_TYPE_VIEW, phonebook_view_mode());
        } else {
            app->route = APP_ROUTE_PHONEBOOK_MEMORY;
            app->dirty = true;
        }
        return true;
    }
    if (app->phonebook_menu_kind == PHONEBOOK_MENU_TYPE_VIEW) {
        set_phonebook_view_mode(app->phonebook_menu_selected);
        /* PB.3: type-of-view Saved uses record 0x1a (3080 ms); PB.6: return
         * to the Options page, not the main menu. */
        open_phonebook_menu(app, PHONEBOOK_MENU_OPTIONS, 0u);
        open_display_sid(app, 26u, 0x296u, "Saved", APP_ROUTE_PHONEBOOK_MENU, now);
        return true;
    }
    return true;
}

bool handle_phonebook_list_key(app_t *app, uint16_t key, uint32_t now) {
    if (app->phonebook_visible_count == 0u) {
        if (key == KEY_C || key == KEY_NAVI) {
            phonebook_return_to_parent_menu(app);
        }
        return true;
    }
    if (key == KEY_UP) {
        app->phonebook_list_selected = app->phonebook_list_selected == 0u
            ? (uint16_t)(app->phonebook_visible_count - 1u)
            : (uint16_t)(app->phonebook_list_selected - 1u);
        app->dirty = true;
        return true;
    }
    if (key == KEY_DOWN) {
        app->phonebook_list_selected = (uint16_t)((app->phonebook_list_selected + 1u) % app->phonebook_visible_count);
        app->dirty = true;
        return true;
    }
    if (key == KEY_C) {
        if (app->phonebook_context == PHONEBOOK_CONTEXT_SEND_RECIPIENT) {
            open_phonebook_send_recipient_editor(app, app->sms_recipient_prefill, now);
        } else if (app->phonebook_context == PHONEBOOK_CONTEXT_SMS_RECIPIENT) {
            open_sms_recipient_editor(app, app->sms_recipient_prefill, now);
        } else if (app->phonebook_context == PHONEBOOK_CONTEXT_PICTURE_RECIPIENT) {
            open_picture_recipient_editor(app, app->sms_recipient_prefill, now);
        } else if (app->phonebook_context == PHONEBOOK_CONTEXT_TONE_COMPOSER_RECIPIENT) {
            open_tone_composer_recipient_editor(app, app->sms_recipient_prefill, now);
        } else if (app->phonebook_context == PHONEBOOK_CONTEXT_SPEED_ASSIGN) {
            open_phonebook_speed_dials(app);
        } else if (app->phonebook_context == PHONEBOOK_CONTEXT_STANDBY) {
            app->route = APP_ROUTE_STANDBY;
            app->dirty = true;
        } else if (app->phonebook_context == PHONEBOOK_CONTEXT_IN_CALL) {
            app->route = APP_ROUTE_CALL;
            app->dirty = true;
        } else if (app->phonebook_context == PHONEBOOK_CONTEXT_IN_CALL_NEW_CALL) {
            app->route = APP_ROUTE_CALL;
            app->dirty = true;
        } else {
            /* PB.6: C from a contact list returns to the Phone book menu
             * level it was launched from, not the main menu. */
            phonebook_return_to_parent_menu(app);
        }
        return true;
    }
    if (key != KEY_NAVI) {
        return true;
    }
    modem_phonebook_entry_t entry;
    uint16_t cache_position = 0u;
    if (!phonebook_current_entry(app, &entry, &cache_position)) {
        return true;
    }
    switch (app->phonebook_context) {
    case PHONEBOOK_CONTEXT_STANDBY:
    case PHONEBOOK_CONTEXT_SEARCH:
    case PHONEBOOK_CONTEXT_IN_CALL:
    case PHONEBOOK_CONTEXT_IN_CALL_NEW_CALL:
        if (entry.number[0] == '\0') {
            open_display_sid(app, 0u, 0x208u, "No phone\nnumber", APP_ROUTE_PHONEBOOK_LIST, now);
            return true;
        }
        close_editor(app);
        bool in_call_context = app->phonebook_context == PHONEBOOK_CONTEXT_IN_CALL ||
                               app->phonebook_context == PHONEBOOK_CONTEXT_IN_CALL_NEW_CALL;
        /* Same in-call New-call preservation as the dialogs editor path (the
         * live call becomes HELD; C's waiting fields survive the reset). */
        call_newcall_preserve_t preserve;
        call_newcall_snapshot(app, now, &preserve);
        bool handled = start_outgoing_call(app,
                                           entry.number,
                                           entry.name,
                                           now,
                                           in_call_context ? APP_ROUTE_CALL : APP_ROUTE_PHONEBOOK_LIST);
        if (in_call_context && app->route == APP_ROUTE_CALL) {
            call_newcall_restore_held(app, &preserve);
        }
        return handled;
    case PHONEBOOK_CONTEXT_EDIT:
        begin_phonebook_edit(app, &entry, cache_position, now);
        return true;
    case PHONEBOOK_CONTEXT_ERASE:
        begin_phonebook_erase(app, &entry, cache_position);
        return true;
    case PHONEBOOK_CONTEXT_ASSIGN:
        begin_phonebook_assign(app, &entry, cache_position);
        return true;
    case PHONEBOOK_CONTEXT_SEND:
        begin_phonebook_send(app, &entry, cache_position, now);
        return true;
    case PHONEBOOK_CONTEXT_SPEED_ASSIGN: {
        store_phonebook_set_speed_dial(app->phonebook_speed_key, entry.index);
        open_phonebook_speed_dials(app);
        /* PB.5: SID 0x0057 "Speed dial\nkey %N\nsaved" carries the key digit. */
        open_display_sid_num(app, 3u, 0x57u, "Speed dial\nkey %N\nsaved",
                             (unsigned)app->phonebook_speed_key, APP_ROUTE_PHONEBOOK_SPEED_DIALS, now);
        return true;
    }
    case PHONEBOOK_CONTEXT_SEND_RECIPIENT:
        copy_text(app->sms_recipient_prefill, sizeof(app->sms_recipient_prefill), entry.number);
        open_phonebook_send_recipient_editor(app, entry.number, now);
        return true;
    case PHONEBOOK_CONTEXT_SMS_RECIPIENT:
        copy_text(app->sms_recipient_prefill, sizeof(app->sms_recipient_prefill), entry.number);
        open_sms_recipient_editor(app, entry.number, now);
        return true;
    case PHONEBOOK_CONTEXT_PICTURE_RECIPIENT:
        copy_text(app->sms_recipient_prefill, sizeof(app->sms_recipient_prefill), entry.number);
        open_picture_recipient_editor(app, entry.number, now);
        return true;
    case PHONEBOOK_CONTEXT_TONE_COMPOSER_RECIPIENT:
        copy_text(app->sms_recipient_prefill, sizeof(app->sms_recipient_prefill), entry.number);
        open_tone_composer_recipient_editor(app, entry.number, now);
        return true;
    default:
        return true;
    }
}

bool handle_phonebook_tone_key(app_t *app, uint16_t key, uint32_t now) {
    uint8_t count = phonebook_tone_picker_count();
    if (key == KEY_UP || key == KEY_DOWN) {
        if (key == KEY_UP) {
            app->phonebook_tone_selected = app->phonebook_tone_selected == 0u
                ? (uint8_t)(count - 1u)
                : (uint8_t)(app->phonebook_tone_selected - 1u);
        } else {
            app->phonebook_tone_selected = (uint8_t)((app->phonebook_tone_selected + 1u) % count);
        }
        /* PB.2: preview while browsing; No tone/Preset rows just stop audio. */
        preview_ringing_tone_value_active_profile(phonebook_tone_picker_value(app->phonebook_tone_selected));
        app->dirty = true;
        return true;
    }
    if (key == KEY_C) {
        stop_ringing_tone_preview();
        show_phonebook_list(app, PHONEBOOK_LABEL_ASSIGN, "1-6", PHONEBOOK_CONTEXT_ASSIGN, app->phonebook_pending_selected);
        return true;
    }
    if (key == KEY_NAVI) {
        stop_ringing_tone_preview();
        modem_phonebook_entry_t entry;
        if (modem_service_phonebook_entry(app->phonebook_pending_index, &entry)) {
            store_phonebook_set_contact_tone_value(entry.index, phonebook_tone_picker_value(app->phonebook_tone_selected));
        }
        show_phonebook_list(app, PHONEBOOK_LABEL_ASSIGN, "1-6", PHONEBOOK_CONTEXT_ASSIGN, app->phonebook_pending_selected);
        open_display_sid(app, 3u, 0x0e6u, "Tone\nsaved", APP_ROUTE_PHONEBOOK_LIST, now);
        return true;
    }
    return true;
}

bool handle_phonebook_speed_key(app_t *app, uint16_t key, uint32_t now) {
    if (key == KEY_UP) {
        app->phonebook_speed_selected = app->phonebook_speed_selected == 0u
            ? (uint8_t)(ARRAY_COUNT(PHONEBOOK_SPEED_KEYS) - 1u)
            : (uint8_t)(app->phonebook_speed_selected - 1u);
        app->dirty = true;
        return true;
    }
    if (key == KEY_DOWN) {
        app->phonebook_speed_selected = (uint8_t)((app->phonebook_speed_selected + 1u) % ARRAY_COUNT(PHONEBOOK_SPEED_KEYS));
        app->dirty = true;
        return true;
    }
    if (key == KEY_C) {
        app->route = APP_ROUTE_MAIN_MENU;
        app->dirty = true;
        return true;
    }
    if (key == KEY_NAVI) {
        app->phonebook_speed_key = PHONEBOOK_SPEED_KEYS[app->phonebook_speed_selected];
        if (selected_phonebook_speed_contact(app) == STORE_SPEED_DIAL_EMPTY) {
            start_phonebook_list(app, PHONEBOOK_LABEL_ASSIGN, "1-9", PHONEBOOK_CONTEXT_SPEED_ASSIGN, 0u, now);
        } else {
            app->phonebook_speed_options_selected = 0u;
            app->route = APP_ROUTE_PHONEBOOK_SPEED_DIAL_OPTIONS;
            app->dirty = true;
        }
        return true;
    }
    return true;
}

bool handle_phonebook_speed_options_key(app_t *app, uint16_t key, uint32_t now) {
    if (key == KEY_UP) {
        app->phonebook_speed_options_selected = app->phonebook_speed_options_selected == 0u
            ? (uint8_t)(ARRAY_COUNT(PHONEBOOK_SPEED_OPTION_LABELS) - 1u)
            : (uint8_t)(app->phonebook_speed_options_selected - 1u);
        app->dirty = true;
        return true;
    }
    if (key == KEY_DOWN) {
        app->phonebook_speed_options_selected = (uint8_t)((app->phonebook_speed_options_selected + 1u) % ARRAY_COUNT(PHONEBOOK_SPEED_OPTION_LABELS));
        app->dirty = true;
        return true;
    }
    if (key == KEY_C) {
        app->route = APP_ROUTE_PHONEBOOK_SPEED_DIALS;
        app->dirty = true;
        return true;
    }
    if (key != KEY_NAVI) {
        return true;
    }
    const char *option = PHONEBOOK_SPEED_OPTION_LABELS[app->phonebook_speed_options_selected];
    if (strcmp(option, "Change") == 0) {
        start_phonebook_list(app, PHONEBOOK_LABEL_ASSIGN, "1-9", PHONEBOOK_CONTEXT_SPEED_ASSIGN, 0u, now);
    } else if (strcmp(option, "Erase") == 0) {
        store_phonebook_clear_speed_dial(app->phonebook_speed_key);
        open_phonebook_speed_dials(app);
        /* PB.5: SID 0x039f "Speed dial\nkey %N\nerased". */
        open_display_sid_num(app, 3u, 0x39fu, "Speed dial\nkey %N\nerased",
                             (unsigned)app->phonebook_speed_key, APP_ROUTE_PHONEBOOK_SPEED_DIALS, now);
    } else {
        uint16_t contact_index = selected_phonebook_speed_contact(app);
        modem_phonebook_entry_t entry;
        for (uint16_t i = 0; i < modem_service_phonebook_count(); i++) {
            if (modem_service_phonebook_entry(i, &entry) && entry.index == contact_index) {
                (void)start_outgoing_call(app, entry.number, entry.name, now,
                                          APP_ROUTE_PHONEBOOK_SPEED_DIAL_OPTIONS);
                break;
            }
        }
    }
    return true;
}

bool handle_phonebook_edit_choice_key(app_t *app, uint16_t key, uint32_t now) {
    if (key == KEY_UP || key == KEY_DOWN) {
        app->phonebook_edit_choice_selected = (uint8_t)(1u - app->phonebook_edit_choice_selected);
        app->dirty = true;
        return true;
    }
    if (key == KEY_C) {
        open_editor(app, ts_or(0x28du, "Number:"), app->editor_draft_number, 30u, EDITOR_KIND_NUMBER, EDITOR_CONTEXT_PHONEBOOK_EDIT_NUMBER, true, now);
        return true;
    }
    if (key == KEY_NAVI) {
        modem_phonebook_entry_t entry;
        if (modem_service_phonebook_entry(app->phonebook_pending_index, &entry)) {
            if (app->phonebook_edit_choice_selected == 1u) {
                start_phonebook_add_with_context(app, app->editor_draft_name, app->editor_draft_number,
                                                 PHONEBOOK_LABEL_EDIT, "1-5", PHONEBOOK_CONTEXT_EDIT, now);
            } else {
                start_phonebook_update(app, entry.index, app->editor_draft_name, app->editor_draft_number, now);
            }
        }
        return true;
    }
    return true;
}

bool handle_phonebook_security_key(app_t *app, uint16_t key, uint32_t now) {
    if (key == KEY_C) {
        if (app->editor_value[0] != '\0') {
            editor_delete_one(app);
        } else {
            app->route = APP_ROUTE_MAIN_MENU;
        }
        app->dirty = true;
        return true;
    }
    if (key == KEY_NAVI) {
        char code[STORE_TEXT_MAX + 1u];
        store_setting_get_text(STORE_SETTING_SECURITY_CODE, code, sizeof(code));
        if (strcmp(app->editor_value, code) == 0) {
            app->phonebook_erase_all_started_ms = now;
            app->phonebook_pending_kind = PHONEBOOK_PENDING_LIST;
            app->phonebook_pending_label = PHONEBOOK_LABEL_ERASE;
            app->phonebook_context = PHONEBOOK_CONTEXT_ERASE;
            copy_text(app->phonebook_pending_path, sizeof(app->phonebook_pending_path), "1-4-1");
            uint32_t request_id = 0u;
            if (modem_service_request_phonebook_list(&request_id)) {
                app->phonebook_request_id = request_id;
                app->phonebook_request_started_ms = now;
                open_display_sid(app, PHONEBOOK_REQUEST_DISPLAY_RECORD_ID,
                                 0x0ccu, "Erasing\nmemory",
                                 APP_ROUTE_MAIN_MENU, now);
                phonebook_claim_request_display(app);
            } else {
                app->phonebook_erase_all_started_ms = 0u;
                phonebook_clear_request(app);
                open_display_sid(app, 0u, 0x2b4u, "SIM card\nbusy",
                                 APP_ROUTE_MAIN_MENU, now);
            }
        } else {
            open_display_sid(app, 0u, 0x145u, "Code\nerror", APP_ROUTE_PHONEBOOK_SECURITY, now);
        }
        return true;
    }
    char digit = key_digit(key);
    if (digit >= '0' && digit <= '9' && strlen(app->editor_value) < 10u) {
        size_t len = strlen(app->editor_value);
        app->editor_value[len] = digit;
        app->editor_value[len + 1u] = '\0';
        app->dirty = true;
    }
    return true;
}

static void phonebook_clear_request(app_t *app) {
    app->phonebook_pending_kind = PHONEBOOK_PENDING_NONE;
    app->phonebook_request_id = 0u;
    app->phonebook_request_started_ms = 0u;
    if (app->phonebook_erase_all_started_ms == 0u) {
        app->phonebook_wait_display_request_id = 0u;
    }
}

static void phonebook_claim_request_display(app_t *app) {
    app->phonebook_wait_display_request_id = app->phonebook_request_id;
}

static bool phonebook_expected_operation(const app_t *app,
                                         modem_phonebook_op_t *out) {
    if (app == NULL || out == NULL) {
        return false;
    }
    switch ((phonebook_pending_t)app->phonebook_pending_kind) {
    case PHONEBOOK_PENDING_LIST:
    case PHONEBOOK_PENDING_SEARCH:
        *out = MODEM_PHONEBOOK_OP_LIST;
        return true;
    case PHONEBOOK_PENDING_ADD:
        *out = MODEM_PHONEBOOK_OP_ADD;
        return true;
    case PHONEBOOK_PENDING_UPDATE:
        *out = MODEM_PHONEBOOK_OP_UPDATE;
        return true;
    case PHONEBOOK_PENDING_DELETE:
        *out = MODEM_PHONEBOOK_OP_DELETE;
        return true;
    case PHONEBOOK_PENDING_NONE:
    default:
        return false;
    }
}

bool poll_phonebook(app_t *app, uint32_t now) {
    /* Call/alarm/power routing runs before this poll. Detach immediately so a
     * same-tick terminal cannot replace its higher-priority screen. */
    if (app->phonebook_erase_all_started_ms != 0u &&
        !phonebook_erase_all_display_owned(app)) {
        app->phonebook_erase_all_started_ms = 0u;
        app->phonebook_wait_display_request_id = 0u;
    }
    if (app->phonebook_pending_kind != PHONEBOOK_PENDING_NONE &&
        !phonebook_request_display_owned(app)) {
        app->phonebook_erase_all_started_ms = 0u;
        phonebook_clear_request(app);
    }

    modem_phonebook_result_t result;
    bool matching_result = false;
    modem_phonebook_op_t expected = MODEM_PHONEBOOK_OP_NONE;
    while (modem_service_pop_phonebook_result(&result)) {
        if (!phonebook_expected_operation(app, &expected) ||
            result.request_id != app->phonebook_request_id) {
            continue;
        }
        matching_result = true;
        break;
    }

    if (!matching_result) {
        if (app->phonebook_pending_kind != PHONEBOOK_PENDING_NONE &&
            app->route == APP_ROUTE_DISPLAY_MESSAGE &&
            time_diff_ms(now, app->phonebook_request_started_ms +
                                  PHONEBOOK_OPERATION_TIMEOUT_MS) >= 0) {
            app_route_t back = phonebook_empty_return_route(
                (phonebook_context_t)app->phonebook_context);
            app->phonebook_erase_all_started_ms = 0u;
            phonebook_clear_request(app);
            open_display_sid(app, 2u, 0x297u, "SIM card\nnot ready",
                             back, now);
            return true;
        }
        return false;
    }

    /* A matching id with the wrong operation is an internal contract fault,
     * never permission to enter another operation's UI branch. */
    if (result.kind != expected) {
        result.outcome = MODEM_PHONEBOOK_OUTCOME_ERROR;
    }
    if (result.outcome != MODEM_PHONEBOOK_OUTCOME_OK) {
        app->phonebook_erase_all_started_ms = 0u;
        app_route_t back = phonebook_empty_return_route(
            (phonebook_context_t)app->phonebook_context);
        phonebook_clear_request(app);
        if (result.sim_not_ready) {
            open_display_sid(app, 2u, 0x297u, "SIM card\nnot ready", back,
                             now);
        } else {
            open_display_sid(app, 0u, 0x20du, "Not\nallowed", back, now);
        }
        return true;
    }

    if (app->phonebook_erase_all_started_ms != 0u) {
        phonebook_clear_request(app);
        build_phonebook_visible(app, "");
        if (app->phonebook_visible_count > 0u) {
            modem_phonebook_entry_t entry;
            if (!modem_service_phonebook_entry(
                    app->phonebook_visible_indices[0], &entry)) {
                app->phonebook_erase_all_started_ms = 0u;
                open_display_sid(app, 0u, 0x20du, "Not\nallowed",
                                 APP_ROUTE_MAIN_MENU, now);
                return true;
            }
            if (start_phonebook_delete(app, entry.index, now)) {
                app->phonebook_erase_all_started_ms = now;
                return true;
            }
            app->phonebook_erase_all_started_ms = 0u;
            return true;
        }
        if (time_diff_ms(now, app->phonebook_erase_all_started_ms +
                                 PHONEBOOK_ERASE_ALL_PROGRESS_MS) >= 0) {
            app->phonebook_erase_all_started_ms = 0u;
            /* PB.3 record 0x1a (3080 ms); PB.6 return to the Phone book menu. */
            open_phonebook_menu(app, PHONEBOOK_MENU_ROOT, 3u);
            open_display_sid(app, 26u, 0x22au, "Memory\nerased",
                             APP_ROUTE_PHONEBOOK_MENU, now);
        }
        return true;
    }

    phonebook_pending_t pending =
        (phonebook_pending_t)app->phonebook_pending_kind;
    if (pending == PHONEBOOK_PENDING_LIST ||
        pending == PHONEBOOK_PENDING_SEARCH) {
        bool searched = pending == PHONEBOOK_PENDING_SEARCH &&
                        app->phonebook_search_query[0] != '\0';
        show_phonebook_list(app,
                            (phonebook_label_t)app->phonebook_pending_label,
                            app->phonebook_pending_path,
                            (phonebook_context_t)app->phonebook_context,
                            app->phonebook_pending_selected);
        if (app->phonebook_visible_count == 0u) {
            /* PB.4: a non-empty query miss is SID 0x0284 "Name\nnot found",
             * distinct from the empty-store "No phone numbers" (SID 0x0276). */
            app_route_t back = phonebook_empty_return_route(
                (phonebook_context_t)app->phonebook_context);
            if (searched) {
                open_display_sid(app, 2u, 0x284u, "Name\nnot found", back,
                                 now);
            } else {
                open_display_sid(app, 2u, 0x276u, "No phone\nnumbers", back,
                                 now);
            }
        }
    } else if (pending == PHONEBOOK_PENDING_ADD ||
               pending == PHONEBOOK_PENDING_UPDATE) {
        /* PB.3: Add-entry "Saved" uses record 0x1a (movs r0,#0x1a in the add
         * handler, 3080 ms); Edit "Saved" keeps record 3 (1536 ms). */
        uint8_t saved_record = pending == PHONEBOOK_PENDING_ADD ? 26u : 3u;
        if (app->phonebook_context == PHONEBOOK_CONTEXT_CALL_REGISTER_SAVE) {
            phonebook_clear_request(app);
            open_display_sid(app, saved_record, 0x296u, "Saved",
                             APP_ROUTE_CALL_REGISTER_LIST, now);
            return true;
        }
        uint16_t selected = app->phonebook_pending_selected;
        if (pending == PHONEBOOK_PENDING_ADD) {
            selected = find_phonebook_entry_position(
                app->editor_draft_name, app->editor_draft_number, selected);
        }
        show_phonebook_list(app,
                            (phonebook_label_t)app->phonebook_pending_label,
                            app->phonebook_pending_path,
                            (phonebook_context_t)app->phonebook_context,
                            selected);
        open_display_sid(app, saved_record, 0x296u, "Saved",
                         APP_ROUTE_PHONEBOOK_LIST, now);
    } else if (pending == PHONEBOOK_PENDING_DELETE) {
        show_phonebook_list(app, PHONEBOOK_LABEL_ERASE, "1-4-1",
                            PHONEBOOK_CONTEXT_ERASE,
                            app->phonebook_pending_selected);
        if (app->phonebook_visible_count == 0u) {
            /* PB.6: empty-after-erase returns to the Phone book menu. */
            open_phonebook_menu(app, PHONEBOOK_MENU_ROOT, 3u);
            open_display_sid(app, 2u, 0x276u, "No phone\nnumbers",
                             APP_ROUTE_PHONEBOOK_MENU, now);
        } else {
            open_display_sid(app, 44u, 0x278u, "Erased",
                             APP_ROUTE_PHONEBOOK_LIST, now);
        }
    }
    phonebook_clear_request(app);
    return true;
}

bool tick_phonebook_erase_all(app_t *app, uint32_t now) {
    if (app->phonebook_erase_all_started_ms == 0u) {
        return false;
    }
    if (!phonebook_erase_all_display_owned(app)) {
        app->phonebook_erase_all_started_ms = 0u;
        app->phonebook_wait_display_request_id = 0u;
        return false;
    }
    if (app->phonebook_pending_kind != PHONEBOOK_PENDING_NONE ||
        time_diff_ms(now, app->phonebook_erase_all_started_ms + PHONEBOOK_ERASE_ALL_PROGRESS_MS) < 0) {
        return false;
    }
    app->phonebook_erase_all_started_ms = 0u;
    app->phonebook_wait_display_request_id = 0u;
    /* PB.3 record 0x1a (3080 ms); PB.6 return to the Phone book menu. */
    open_phonebook_menu(app, PHONEBOOK_MENU_ROOT, 3u);
    open_display_sid(app, 26u, 0x22au, "Memory\nerased", APP_ROUTE_PHONEBOOK_MENU, now);
    return true;
}

bool tick_phonebook_send(app_t *app, uint32_t now) {
    if (app->phonebook_send_request_id == 0u) {
        return false;
    }
    bool ui_owned = app->phonebook_send_waiting;
    bool detached = false;
    if (ui_owned &&
        (app->route != APP_ROUTE_DISPLAY_MESSAGE ||
         app->display_record_id != 46u ||
         app->display_return_route != APP_ROUTE_PHONEBOOK_LIST)) {
        app->phonebook_send_waiting = false;
        ui_owned = false;
        detached = true;
    }
    /* Use the per-operation send result, not the global sent-counter, so a
     * concurrent SMS send completing elsewhere cannot be misread as this
     * phonebook send succeeding (mirrors messages_app.c). */
    modem_sms_send_result_t result;
    bool matching = modem_service_pop_sms_send_result(
        app->phonebook_send_request_id, &result);
    if (matching) {
        app->phonebook_send_waiting = false;
        app->phonebook_send_request_id = 0u;
        if (!ui_owned) {
            return true;
        }
        if (result.kind != MODEM_SMS_REQUEST_SEND_TEXT) {
            result.outcome = MODEM_SMS_OUTCOME_ERROR;
        }
        if (result.outcome == MODEM_SMS_OUTCOME_OK) {
            open_display_sid(app, 47u, 0x2a8u, "Message\nsent", APP_ROUTE_PHONEBOOK_LIST, now);
        } else if (result.outcome == MODEM_SMS_OUTCOME_UNCERTAIN) {
            open_display_sid(app, 0u, 0x229u, "Result\nunknown",
                             APP_ROUTE_PHONEBOOK_LIST, now);
        } else {
            open_display_sid(app, 0u, 0x359u, "Message\nsending\nfailed", APP_ROUTE_PHONEBOOK_LIST, now);
        }
        return true;
    }
    if (ui_owned &&
        time_diff_ms(now, app->phonebook_send_started_ms +
                              PHONEBOOK_SEND_TIMEOUT_MS) >= 0) {
        app->phonebook_send_waiting = false;
        open_display_sid(app, 0u, 0x359u, "Message\nsending\nfailed", APP_ROUTE_PHONEBOOK_LIST, now);
        return true;
    }
    return detached;
}

void open_phonebook_menu(app_t *app, phonebook_menu_kind_t kind, uint8_t selected) {
    app->route = APP_ROUTE_PHONEBOOK_MENU;
    app->phonebook_menu_kind = (uint8_t)kind;
    app->phonebook_menu_selected = phonebook_menu_normalized_raw(kind, selected);
    app->dirty = true;
}

void start_phonebook_list(app_t *app, phonebook_label_t label, const char *path, phonebook_context_t context, uint16_t selected, uint32_t now) {
    app->phonebook_pending_kind = PHONEBOOK_PENDING_LIST;
    app->phonebook_pending_label = (uint8_t)label;
    app->phonebook_context = (uint8_t)context;
    app->phonebook_pending_selected = selected;
    copy_text(app->phonebook_pending_path, sizeof(app->phonebook_pending_path), path);
    app->phonebook_pending_path[sizeof(app->phonebook_pending_path) - 1u] = '\0';
    app->phonebook_search_query[0] = '\0';
    uint32_t request_id = 0u;
    if (!modem_service_request_phonebook_list(&request_id)) {
        phonebook_clear_request(app);
        open_display_sid(app, 0u, 0x2b4u, "SIM card\nbusy", phonebook_empty_return_route(context), now);
    } else {
        app->phonebook_request_id = request_id;
        app->phonebook_request_started_ms = now;
        open_display_sid(app, PHONEBOOK_REQUEST_DISPLAY_RECORD_ID, 0x33du,
                         "Opening", phonebook_empty_return_route(context), now);
        phonebook_claim_request_display(app);
    }
}

void start_phonebook_search(app_t *app, const char *query, uint32_t now) {
    app->phonebook_pending_kind = PHONEBOOK_PENDING_SEARCH;
    app->phonebook_pending_label = PHONEBOOK_LABEL_CALL;
    app->phonebook_context = PHONEBOOK_CONTEXT_SEARCH;
    app->phonebook_pending_selected = 0u;
    copy_text(app->phonebook_pending_path, sizeof(app->phonebook_pending_path), "1-1");
    copy_text(app->phonebook_search_query, sizeof(app->phonebook_search_query), query);
    app->phonebook_search_query[sizeof(app->phonebook_search_query) - 1u] = '\0';
    close_editor(app);
    uint32_t request_id = 0u;
    if (!modem_service_request_phonebook_list(&request_id)) {
        phonebook_clear_request(app);
        open_display_sid(app, 0u, 0x2b4u, "SIM card\nbusy", phonebook_empty_return_route(PHONEBOOK_CONTEXT_SEARCH), now);
    } else {
        app->phonebook_request_id = request_id;
        app->phonebook_request_started_ms = now;
        open_display_sid(app, PHONEBOOK_REQUEST_DISPLAY_RECORD_ID, 0x33du,
                         "Opening",
                         phonebook_empty_return_route(PHONEBOOK_CONTEXT_SEARCH),
                         now);
        phonebook_claim_request_display(app);
    }
}

void start_phonebook_add(app_t *app, const char *name, const char *number, uint32_t now) {
    start_phonebook_add_with_context(app, name, number, PHONEBOOK_LABEL_CALL, "1-3", PHONEBOOK_CONTEXT_SEARCH, now);
}

void start_phonebook_add_with_context(app_t *app, const char *name, const char *number, phonebook_label_t label, const char *path, phonebook_context_t context, uint32_t now) {
    /* Snapshot name/number BEFORE close_editor: some callers pass app->editor_value
     * as `number`, and close_editor() zeroes editor_value -- reading `number` after
     * would then see an empty string, which the modem add rejects ("SIM card busy").
     * Copy first so the request always gets the real values regardless of aliasing. */
    char name_buf[MODEM_PHONEBOOK_NAME_MAX + 1u];
    char number_buf[MODEM_PHONE_MAX + 1u];
    copy_text(name_buf, sizeof(name_buf), name);
    copy_text(number_buf, sizeof(number_buf), number);
    app->phonebook_pending_kind = PHONEBOOK_PENDING_ADD;
    app->phonebook_pending_label = (uint8_t)label;
    app->phonebook_context = (uint8_t)context;
    app->phonebook_pending_selected = 0u;
    copy_text(app->phonebook_pending_path, sizeof(app->phonebook_pending_path), path);
    close_editor(app);
    uint32_t request_id = 0u;
    if (!modem_service_request_phonebook_add(
            name_buf, number_buf, &request_id)) {
        phonebook_clear_request(app);
        open_display_sid(app,
                         0u,
                         0x2b4u,
                         "SIM card\nbusy",
                         phonebook_empty_return_route(context),
                         now);
    } else {
        app->phonebook_request_id = request_id;
        app->phonebook_request_started_ms = now;
        open_display(app,
                     PHONEBOOK_REQUEST_DISPLAY_RECORD_ID,
                     "Saving",
                     0,
                     0,
                     context == PHONEBOOK_CONTEXT_CALL_REGISTER_SAVE ? APP_ROUTE_CALL_REGISTER_LIST : APP_ROUTE_PHONEBOOK_LIST,
                     now);
        phonebook_claim_request_display(app);
    }
}

void start_phonebook_update(app_t *app, uint16_t index, const char *name, const char *number, uint32_t now) {
    /* Snapshot name/number before close_editor -- see start_phonebook_add_with_context. */
    char name_buf[MODEM_PHONEBOOK_NAME_MAX + 1u];
    char number_buf[MODEM_PHONE_MAX + 1u];
    copy_text(name_buf, sizeof(name_buf), name);
    copy_text(number_buf, sizeof(number_buf), number);
    app->phonebook_pending_kind = PHONEBOOK_PENDING_UPDATE;
    app->phonebook_pending_label = PHONEBOOK_LABEL_EDIT;
    app->phonebook_context = PHONEBOOK_CONTEXT_EDIT;
    app->phonebook_pending_selected = app->phonebook_list_selected;
    copy_text(app->phonebook_pending_path, sizeof(app->phonebook_pending_path), "1-5");
    close_editor(app);
    uint32_t request_id = 0u;
    if (!modem_service_request_phonebook_update(
            index, name_buf, number_buf, &request_id)) {
        phonebook_clear_request(app);
        open_display_sid(app, 0u, 0x2b4u, "SIM card\nbusy", APP_ROUTE_MAIN_MENU, now);
    } else {
        app->phonebook_request_id = request_id;
        app->phonebook_request_started_ms = now;
        open_display(app, PHONEBOOK_REQUEST_DISPLAY_RECORD_ID, "Saving", 0,
                     0, APP_ROUTE_PHONEBOOK_LIST, now);
        phonebook_claim_request_display(app);
    }
}

bool start_phonebook_delete(app_t *app, uint16_t index, uint32_t now) {
    app->phonebook_pending_kind = PHONEBOOK_PENDING_DELETE;
    app->phonebook_pending_selected = app->phonebook_list_selected;
    uint32_t request_id = 0u;
    if (!modem_service_request_phonebook_delete(index, &request_id)) {
        phonebook_clear_request(app);
        open_display_sid(app, 0u, 0x2b4u, "SIM card\nbusy", APP_ROUTE_MAIN_MENU, now);
        return false;
    } else {
        app->phonebook_request_id = request_id;
        app->phonebook_request_started_ms = now;
        open_display(app, PHONEBOOK_REQUEST_DISPLAY_RECORD_ID, "Erasing", 0,
                     0, APP_ROUTE_PHONEBOOK_LIST, now);
        phonebook_claim_request_display(app);
    }
    return true;
}

void show_phonebook_list(app_t *app, phonebook_label_t label, const char *path, phonebook_context_t context, uint16_t selected) {
    app->phonebook_context = (uint8_t)context;
    copy_text(app->phonebook_pending_path, sizeof(app->phonebook_pending_path), path);
    app->phonebook_pending_path[sizeof(app->phonebook_pending_path) - 1u] = '\0';
    app->phonebook_pending_label = (uint8_t)label;
    build_phonebook_visible(app, app->phonebook_pending_kind == PHONEBOOK_PENDING_SEARCH ? app->phonebook_search_query : "");
    if (selected >= app->phonebook_visible_count && app->phonebook_visible_count > 0u) {
        selected = (uint16_t)(app->phonebook_visible_count - 1u);
    }
    app->phonebook_list_selected = selected;
    app->route = APP_ROUTE_PHONEBOOK_LIST;
    app->dirty = true;
}

static void build_phonebook_visible(app_t *app, const char *query) {
    app->phonebook_visible_count = 0u;
    uint16_t count = modem_service_phonebook_count();
    for (uint16_t i = 0; i < count && app->phonebook_visible_count < MODEM_PHONEBOOK_MAX_RECORDS; i++) {
        modem_phonebook_entry_t entry;
        if (modem_service_phonebook_entry(i, &entry) && phonebook_entry_matches(&entry, query)) {
            app->phonebook_visible_indices[app->phonebook_visible_count++] = i;
        }
    }
}

static bool phonebook_current_entry(const app_t *app, modem_phonebook_entry_t *entry, uint16_t *cache_position) {
    if (app->phonebook_visible_count == 0u || app->phonebook_list_selected >= app->phonebook_visible_count) {
        return false;
    }
    uint16_t pos = app->phonebook_visible_indices[app->phonebook_list_selected];
    if (cache_position != 0) {
        *cache_position = pos;
    }
    return modem_service_phonebook_entry(pos, entry);
}

static uint16_t find_phonebook_entry_position(const char *name, const char *number, uint16_t fallback) {
    uint16_t match = fallback;
    uint16_t count = modem_service_phonebook_count();
    for (uint16_t i = 0; i < count; i++) {
        modem_phonebook_entry_t entry;
        if (!modem_service_phonebook_entry(i, &entry)) {
            continue;
        }
        if (strcmp(entry.number, number != 0 ? number : "") == 0 &&
            strcmp(entry.name, name != 0 ? name : "") == 0) {
            match = i;
        }
    }
    return match;
}

static void begin_phonebook_edit(app_t *app, const modem_phonebook_entry_t *entry, uint16_t cache_position, uint32_t now) {
    app->phonebook_pending_index = cache_position;
    copy_text(app->editor_original_name, sizeof(app->editor_original_name), entry->name);
    copy_text(app->editor_draft_name, sizeof(app->editor_draft_name), entry->name);
    open_editor(app, ts_or(0x283u, "Name:"), entry->name, 16u, EDITOR_KIND_TEXT, EDITOR_CONTEXT_PHONEBOOK_EDIT_NAME, true, now);
}

static void begin_phonebook_erase(app_t *app, const modem_phonebook_entry_t *entry, uint16_t cache_position) {
    app->phonebook_pending_index = cache_position;
    open_confirm(app, CONFIRM_CONTEXT_PHONEBOOK_ERASE, ts_or(0x277u, "Erase?"), entry->name[0] ? entry->name : entry->number, 0, 4);
}

static void begin_phonebook_assign(app_t *app, const modem_phonebook_entry_t *entry, uint16_t cache_position) {
    app->phonebook_pending_index = cache_position;
    app->phonebook_pending_selected = app->phonebook_list_selected;
    uint8_t value = store_phonebook_get_contact_tone_value(entry->index);
    uint8_t count = phonebook_tone_picker_count();
    app->phonebook_tone_selected = 1u; /* Preset */
    for (uint8_t i = 0; i < count; i++) {
        if (phonebook_tone_picker_value(i) == value) {
            app->phonebook_tone_selected = i;
            break;
        }
    }
    app->route = APP_ROUTE_PHONEBOOK_TONE_PICKER;
    app->dirty = true;
}

static void begin_phonebook_send(app_t *app, const modem_phonebook_entry_t *entry, uint16_t cache_position, uint32_t now) {
    (void)entry;
    app->phonebook_pending_index = cache_position;
    app->phonebook_pending_selected = app->phonebook_list_selected;
    app->sms_recipient_prefill[0] = '\0';
    open_phonebook_send_recipient_editor(app, "", now);
}

static void open_phonebook_send_recipient_editor(app_t *app, const char *value, uint32_t now) {
    open_editor(app, "Enter number:", value, 21u, EDITOR_KIND_NUMBER, EDITOR_CONTEXT_PHONEBOOK_SEND_RECIPIENT, true, now);
    update_phonebook_send_recipient_softkey(app);
}

void update_phonebook_send_recipient_softkey(app_t *app) {
    app->dirty = true;
}

static void open_phonebook_speed_dials(app_t *app) {
    refresh_speed_selected(app);
    app->route = APP_ROUTE_PHONEBOOK_SPEED_DIALS;
    app->dirty = true;
}

static void refresh_speed_selected(app_t *app) {
    if (app->phonebook_speed_selected >= ARRAY_COUNT(PHONEBOOK_SPEED_KEYS)) {
        app->phonebook_speed_selected = 0u;
    }
}

static uint8_t phonebook_view_mode(void) {
    uint8_t value = 0u;
    store_setting_get_u8(STORE_SETTING_PHONEBOOK_VIEW_MODE, &value);
    if (value > 2u) {
        value = 2u;
    }
    return value;
}

static void set_phonebook_view_mode(uint8_t mode) {
    if (mode > 2u) {
        mode = 2u;
    }
    store_setting_set_u8(STORE_SETTING_PHONEBOOK_VIEW_MODE, mode);
}

static const char *phonebook_label_text(phonebook_label_t label) {
    /* Softkey captions; "Edit"/"Erase"/"Call" have no per-app SID in the map
     * (draw_softkey resolves them via ts_softkey, the framework menu-bar block). */
    switch (label) {
    case PHONEBOOK_LABEL_EDIT: return "Edit";
    case PHONEBOOK_LABEL_ERASE: return "Erase";
    case PHONEBOOK_LABEL_ASSIGN: return ts_or(0x2d7u, "Assign");
    case PHONEBOOK_LABEL_SEND: return ts_or(0x2d4u, "Send");
    case PHONEBOOK_LABEL_OK: return ts_or(0x2e9u, "OK");
    case PHONEBOOK_LABEL_CALL:
    default: return "Call";
    }
}

static const char *phonebook_context_return_softkey(phonebook_context_t context) {
    /* Softkey captions; "Edit"/"Erase"/"Call" have no per-app SID in the map
     * (draw_softkey resolves them via ts_softkey, the framework menu-bar block). */
    switch (context) {
    case PHONEBOOK_CONTEXT_EDIT: return "Edit";
    case PHONEBOOK_CONTEXT_ERASE: return "Erase";
    case PHONEBOOK_CONTEXT_ASSIGN:
    case PHONEBOOK_CONTEXT_SPEED_ASSIGN: return ts_or(0x2d7u, "Assign");
    case PHONEBOOK_CONTEXT_SEND: return ts_or(0x2d4u, "Send");
    case PHONEBOOK_CONTEXT_SEND_RECIPIENT: return ts_or(0x2e9u, "OK");
    case PHONEBOOK_CONTEXT_SMS_RECIPIENT: return ts_or(0x2e9u, "OK");
    case PHONEBOOK_CONTEXT_PICTURE_RECIPIENT: return ts_or(0x2e9u, "OK");
    case PHONEBOOK_CONTEXT_TONE_COMPOSER_RECIPIENT: return ts_or(0x2e9u, "OK");
    case PHONEBOOK_CONTEXT_SEARCH:
    case PHONEBOOK_CONTEXT_IN_CALL:
    case PHONEBOOK_CONTEXT_IN_CALL_NEW_CALL:
    default: return "Call";
    }
}

static app_route_t phonebook_empty_return_route(phonebook_context_t context) {
    if (context == PHONEBOOK_CONTEXT_SEND_RECIPIENT ||
        context == PHONEBOOK_CONTEXT_SMS_RECIPIENT ||
        context == PHONEBOOK_CONTEXT_PICTURE_RECIPIENT ||
        context == PHONEBOOK_CONTEXT_TONE_COMPOSER_RECIPIENT) {
        return APP_ROUTE_EDITOR;
    }
    if (context == PHONEBOOK_CONTEXT_SPEED_ASSIGN) {
        return APP_ROUTE_PHONEBOOK_SPEED_DIALS;
    }
    if (context == PHONEBOOK_CONTEXT_STANDBY) {
        return APP_ROUTE_STANDBY;
    }
    if (context == PHONEBOOK_CONTEXT_CALL_REGISTER_SAVE) {
        return APP_ROUTE_CALL_REGISTER_LIST;
    }
    if (context == PHONEBOOK_CONTEXT_IN_CALL) {
        return APP_ROUTE_CALL;
    }
    if (context == PHONEBOOK_CONTEXT_IN_CALL_NEW_CALL) {
        return APP_ROUTE_CALL;
    }
    /* PB.6: empty-store notices return to the Phone book menu level. */
    return APP_ROUTE_PHONEBOOK_MENU;
}

/* PB.6: back out of a contact list to the Phone book menu level it was
 * launched from, with the launching row still highlighted. */
static void phonebook_return_to_parent_menu(app_t *app) {
    phonebook_context_t context = (phonebook_context_t)app->phonebook_context;
    if (context == PHONEBOOK_CONTEXT_ERASE) {
        open_phonebook_menu(app, PHONEBOOK_MENU_ERASE, 0u);
        return;
    }
    uint8_t row = 0u; /* Search */
    if (context == PHONEBOOK_CONTEXT_EDIT) {
        row = 4u;
    } else if (context == PHONEBOOK_CONTEXT_ASSIGN) {
        row = 5u;
    } else if (context == PHONEBOOK_CONTEXT_SEND) {
        row = 6u;
    }
    open_phonebook_menu(app, PHONEBOOK_MENU_ROOT, row);
}

static uint8_t phonebook_tone_picker_count(void) {
    return (uint8_t)(PHONEBOOK_TONE_FIXED_ROWS + ringing_tone_catalogue_count());
}

static const char *phonebook_tone_picker_label(uint8_t row) {
    if (row == 0u) {
        return ts_or(0x0ceu, "No tone");
    }
    if (row == 1u) {
        return ts_or(0x0cfu, "Preset");
    }
    return ringing_tone_catalogue_label((uint8_t)(row - PHONEBOOK_TONE_FIXED_ROWS));
}

static uint8_t phonebook_tone_picker_value(uint8_t row) {
    if (row == 0u) {
        return STORE_CONTACT_TONE_NO_TONE;
    }
    if (row == 1u) {
        return STORE_CONTACT_TONE_PRESET;
    }
    return ringing_tone_catalogue_value((uint8_t)(row - PHONEBOOK_TONE_FIXED_ROWS));
}

static const char *phonebook_tone_value_text(uint8_t value) {
    if (value == STORE_CONTACT_TONE_NO_TONE) {
        return ts_or(0x0ceu, "No tone");
    }
    if (value == STORE_CONTACT_TONE_PRESET) {
        return ts_or(0x0cfu, "Preset");
    }
    const char *label = ringing_tone_value_label(value);
    return label != 0 ? label : ts_or(0x0cfu, "Preset");
}

static bool phonebook_entry_matches(const modem_phonebook_entry_t *entry, const char *query) {
    if (query == 0 || query[0] == '\0') {
        return true;
    }
    const char *name = entry->name[0] ? entry->name : entry->number;
    for (uint8_t i = 0; query[i] != '\0'; i++) {
        char a = name[i];
        char b = query[i];
        if (a >= 'a' && a <= 'z') {
            a = (char)(a - ('a' - 'A'));
        }
        if (b >= 'a' && b <= 'z') {
            b = (char)(b - ('a' - 'A'));
        }
        if (a != b) {
            return false;
        }
    }
    return true;
}

static bool phonebook_root_item_visible(uint8_t raw) {
    if (raw >= ARRAY_COUNT(PHONEBOOK_ROOT_LABELS)) {
        return false;
    }
    if (raw == 1u) {
        return feature_gate_visible(FEATURE_GATE_PHONEBOOK_SERVICE_NOS);
    }
    if (raw == 9u) {
        return feature_gate_visible(FEATURE_GATE_PHONEBOOK_INFO_NUMBERS);
    }
    return true;
}

static uint8_t phonebook_menu_visible_count(phonebook_menu_kind_t kind) {
    if (kind == PHONEBOOK_MENU_ROOT) {
        return ui_menu_visible_count(
            phonebook_root_item_visible,
            (uint8_t)ARRAY_COUNT(PHONEBOOK_ROOT_LABELS));
    }
    if (kind == PHONEBOOK_MENU_ERASE) {
        return (uint8_t)ARRAY_COUNT(PHONEBOOK_ERASE_LABELS);
    }
    if (kind == PHONEBOOK_MENU_OPTIONS) {
        return (uint8_t)ARRAY_COUNT(PHONEBOOK_OPTIONS_LABELS);
    }
    return (uint8_t)ARRAY_COUNT(PHONEBOOK_TYPE_VIEW_LABELS);
}

static uint8_t phonebook_menu_visible_index(phonebook_menu_kind_t kind, uint8_t raw) {
    if (kind != PHONEBOOK_MENU_ROOT) {
        return raw >= phonebook_menu_visible_count(kind) ? 0u : raw;
    }
    return ui_menu_visible_index(
        phonebook_root_item_visible,
        (uint8_t)ARRAY_COUNT(PHONEBOOK_ROOT_LABELS),
        phonebook_menu_normalized_raw(kind, raw));
}

static uint8_t phonebook_menu_raw_at_visible(phonebook_menu_kind_t kind, uint8_t visible) {
    if (kind != PHONEBOOK_MENU_ROOT) {
        uint8_t count = phonebook_menu_visible_count(kind);
        return visible < count ? visible : 0u;
    }
    return ui_menu_raw_at_visible(
        phonebook_root_item_visible,
        (uint8_t)ARRAY_COUNT(PHONEBOOK_ROOT_LABELS), visible);
}

static uint8_t phonebook_menu_normalized_raw(phonebook_menu_kind_t kind, uint8_t raw) {
    if (kind != PHONEBOOK_MENU_ROOT) {
        uint8_t count = phonebook_menu_visible_count(kind);
        return count != 0u && raw < count ? raw : 0u;
    }
    return ui_menu_normalized_raw(
        phonebook_root_item_visible,
        (uint8_t)ARRAY_COUNT(PHONEBOOK_ROOT_LABELS), raw);
}

static void phonebook_menu_step(app_t *app, int8_t delta) {
    phonebook_menu_kind_t kind = (phonebook_menu_kind_t)app->phonebook_menu_kind;
    uint8_t count = phonebook_menu_visible_count(kind);
    if (count == 0u) {
        return;
    }
    uint8_t visible = phonebook_menu_visible_index(kind, app->phonebook_menu_selected);
    visible = (uint8_t)((visible + count + delta) % count);
    app->phonebook_menu_selected = phonebook_menu_raw_at_visible(kind, visible);
    app->dirty = true;
}

void resolve_contact_name(const char *number, char *dst, size_t cap) {
    copy_text(dst, cap, "");
    if (number == 0 || number[0] == '\0') {
        return;
    }
    uint16_t count = modem_service_phonebook_count();
    bool found = false;
    for (uint16_t i = 0; i < count; i++) {
        modem_phonebook_entry_t entry;
        if (!modem_service_phonebook_entry(i, &entry)) {
            continue;
        }
        if (!phone_match_numbers(entry.number, number)) {
            continue;
        }
        if (!found) {
            found = true;
            copy_text(dst, cap, entry.name);
        } else if (strcmp(dst, entry.name) != 0) {
            /* v6.00 ambiguity rule (ROM 0x2580c0): two tail-matches with
             * DIFFERENT names -> resolve to no name; the UI then falls back
             * to the raw number. Identical duplicate entries still resolve. */
            copy_text(dst, cap, "");
            return;
        }
    }
}

static uint16_t selected_phonebook_speed_contact(const app_t *app) {
    uint16_t value = STORE_SPEED_DIAL_EMPTY;
    store_phonebook_get_speed_dial(PHONEBOOK_SPEED_KEYS[app->phonebook_speed_selected], &value);
    return value;
}

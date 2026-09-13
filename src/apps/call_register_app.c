#include "apps/call_register_app.h"

#include <stdio.h>
#include <string.h>

#include "apps/calls_app.h"
#include "apps/dialogs_app.h"
#include "apps/messages_app.h"
#include "apps/phonebook_app.h"
#include "services/input_keys.h"
#include "services/modem_service.h"
#include "services/strings.h"

/* Menu/option labels carry the v6.00 string id (SID) for localization alongside
 * the English literal, which stays BOTH the fallback AND the strcmp
 * dispatch/preview key (see handle_call_register_options_key,
 * call_register_preview). The label[] entry is never localized in place -- only
 * the rendered copy is, built via cr_loc() at draw time. sid 0 = no exact 1:1
 * v6.00 match: either ambiguous across contexts (many SIDs) or a
 * space-vs-newline mismatch with the ROM string; fall back to the literal. */
typedef struct {
    const char *label;
    uint16_t sid;
} cr_label_t;

/* Localize by SID with English fallback: sid 0 (no 1:1 match) or a sid with no
 * record (ts()==NULL, clone-only) both return the supplied English literal. */
static const char *cr_loc(const cr_label_t *e) {
    if (e->sid != 0u) {
        const char *t = ts(e->sid);
        if (t != 0) {
            return t;
        }
    }
    return e->label;
}

static const cr_label_t CALL_REGISTER_ROOT_LABELS[] = {
    {"Missed\ncalls", 0x1c6u},
    {"Received\ncalls", 0x256u},
    {"Dialled\nnumbers", 0x255u},
    {"Erase recent\ncall lists", 0x1beu},
    {"Show call\nduration", 0x0a1u},
    {"Show call\ncosts", 0x092u},
    {"Call cost settings", 0x093u},
    {"Prepaid credit", 0x109u},
};
static const cr_label_t CALL_REGISTER_ERASE_LABELS[] = {
    {"All", 0x13fu},   /* v6.00 erase-all list label, SID 319 (trace-disambiguated) */
    {"Missed", 0x142u},
    {"Dialled", 0x141u},
    {"Received", 0x144u},
};
static const cr_label_t CALL_REGISTER_OPTIONS_LABELS[] = {
    {"Call", 0u},          /* no bare-"Call" v6.00 record in this pool; keep literal */
    {"Send SMS", 0x3aeu},
    {"Time of call", 0x3b0u},
    {"Edit number", 0x3adu},
    {"Save", 0x3afu},         /* call-register Options "Save", SID 943 (trace-disambiguated) */
    {"Erase", 0x3acu},        /* call-register Options "Erase", SID 940 (trace-disambiguated) */
    {"View number", 0x3b1u},  /* call-register Options "View number", SID 945 (trace-disambiguated) */
};
static const cr_label_t CALL_REGISTER_DURATION_LABELS[] = {
    {"Last call duration", 0x1a1u},   /* ROM "Last call\nduration" (SID 417) */
    {"All calls' duration", 0x049u},  /* ROM "All calls'\nduration" (SID 73) */
    {"Received calls' duration", 0x0a0u},
    {"Dialled calls' duration", 0x09fu},
    {"Clear timers", 0x262u},         /* ROM "Clear\ntimers" (SID 610) */
};
static const cr_label_t CALL_REGISTER_COST_LABELS[] = {
    {"Last call cost", 0x1a0u},   /* ROM "Last call\ncost" (SID 416) */
    {"All calls' cost", 0x048u},  /* ROM "All calls'\ncost" (SID 72) */
    {"Clear counters", 0x261u},   /* ROM "Clear\ncounters" (SID 609) */
};
static const cr_label_t CALL_REGISTER_COST_SETTINGS_LABELS[] = {
    {"Call costs'\nlimit", 0x091u},
    {"Show\ncosts in", 0x11du},
};
static const cr_label_t CALL_REGISTER_PREPAID_LABELS[] = {
    {"Credit info display", 0x10du},
    {"Credit available", 0x103u},
    {"Last event costs", 0x101u},
    {"Recharge status", 0x10cu},
};
static const cr_label_t CALL_REGISTER_CREDIT_INFO_LABELS[] = {
    {"On", 0x10bu},   /* v6.00 "On" (SID 267) / "Off" (SID 266), trace-disambiguated */
    {"Off", 0x10au},
};
static const cr_label_t CALL_REGISTER_RECHARGE_LABELS[] = {
    {"Last recharge:", 0x108u},
    {"Expiry date:", 0x102u},
};

static void open_call_register_options(app_t *app);
static void open_call_register_detail(app_t *app, call_register_detail_mode_t mode, const char *title, const char *value, app_route_t return_route);
static void open_call_register_metric(app_t *app, call_register_metric_kind_t kind);
static const cr_label_t *call_register_menu_labels(call_register_menu_kind_t kind);
static uint8_t call_register_menu_count(call_register_menu_kind_t kind);
static const char *call_register_menu_breadcrumb(call_register_menu_kind_t kind, uint8_t selected, char *scratch, size_t scratch_cap);
static const char *call_register_preview(call_register_menu_kind_t kind, const char *label, char *scratch, size_t scratch_cap);
static bool call_register_menu_has_preview(call_register_menu_kind_t kind, const char *label);
static store_call_list_t call_register_selected_list_kind(const app_t *app);
static bool call_register_current_record(const app_t *app, store_call_record_t *record);
static const char *call_register_caption(store_call_list_t kind, uint8_t index, char *scratch, size_t cap);
static void format_call_record_time(const store_call_record_t *record, char *dst, size_t cap);
static void format_duration(uint32_t seconds, char *dst, size_t cap, bool hms);
static const char *call_register_metric_value(call_register_metric_kind_t kind, uint8_t selected, char *scratch, size_t scratch_cap);
static bool call_register_metric_has_preview(call_register_metric_kind_t kind, uint8_t selected);
static void call_register_clear_recent(app_t *app, uint8_t selected, uint32_t now);
static void call_register_start_call(app_t *app, const store_call_record_t *record, uint32_t now);
static void call_register_save_record(app_t *app, const store_call_record_t *record, uint32_t now);

void render_call_register_menu(const app_t *app, framebuffer_t *fb) {
    call_register_menu_kind_t kind = (call_register_menu_kind_t)app->call_register_menu_kind;
    const cr_label_t *labels = call_register_menu_labels(kind);
    uint8_t count = call_register_menu_count(kind);
    uint8_t selected = app->call_register_menu_selected;
    if (count == 0u) {
        fb_clear(fb, false);
        return;
    }
    if (selected >= count) {
        selected = 0u;
    }

    char breadcrumb[10];
    char preview[18];
    const char *label = labels[selected].label;      /* English: preview strcmp key */
    const char *shown = cr_loc(&labels[selected]);   /* localized: displayed copy */
    const char *preview_text = call_register_preview(kind, label, preview, sizeof(preview));
    draw_static_page_list(fb,
                          shown,
                          call_register_menu_has_preview(kind, label) ? preview_text : "",
                          selected,
                          count,
                          call_register_menu_breadcrumb(kind, selected, breadcrumb, sizeof(breadcrumb)),
                          kind == CALL_REGISTER_MENU_CREDIT_INFO ? "OK" : "Select");
}

void render_call_register_list(const app_t *app, framebuffer_t *fb) {
    fb_clear(fb, false);
    const font_t *small = asset_font(FONT_FS2);
    const font_t *large = asset_font(FONT_FS0);
    fb_bitmap(fb, 30u, 0, 0, true, true);

    store_call_record_t record;
    if (!call_register_current_record(app, &record)) {
        draw_text_block(fb, large, ts_or(0x208u, "No phone\nnumber"), 0, 9, FB_WIDTH, 13, 3u);
        draw_softkey(fb, "OK");
        return;
    }

    char caption[18];
    fb_text(fb,
            small,
            call_register_caption((store_call_list_t)app->call_register_list_kind,
                                  (uint8_t)(app->call_register_list_selected + 1u),
                                  caption,
                                  sizeof(caption)),
            0,
            7,
            true,
            76);
    if (record.name[0] != '\0') {
        /* F2.2: long names scroll as the 1024 ms marquee, not a static tail. */
        char scroll[44];
        fb_text(fb, large, ui_marquee_text(large, record.name, 80, scroll, sizeof(scroll)), 0, 16, true, 80);
    } else if (record.number[0] != '\0') {
        draw_right_text_box(fb, large, record.number, 0, 23, 84);
    } else {
        /* v6.00: a no-CLI MISSED call is stored with the sentinel name "(????)"
         * (Missed-register add 0x257870), but that sentinel is NEVER shown --
         * the list-line render (ROM 0x24921a) detects "(????)" and substitutes
         * SID 939 "(no number)" (emit-descriptor 0x2d640d = 04 03 ab). We store
         * an empty name+number and render "(no number)" directly for the same
         * on-screen result (byte-verified 2026-07-08). */
        char scroll[16];
        fb_text(fb, large, ui_marquee_text(large, ts_or(0x3abu, "(no number)"), 80, scroll, sizeof(scroll)), 0, 16, true, 80);
    }
    draw_softkey(fb, app->call_register_navi_call ? "Call" : ts_or(0x2eau, "Options"));
}

/* v6.00: the call-register Options menu is built CONDITIONALLY (ROM 0x248a40),
 * not shown as a fixed 7-item group. Call / Send SMS / Edit number / Save each
 * require a non-empty number; View number requires a non-empty name AND number;
 * Time of call and Erase are always present. So a withheld/no-CLI (numberless)
 * entry offers ONLY "Time of call" and "Erase" -- the number-consuming options
 * are hidden, not shown-then-errored (byte-verified 2026-07-08; the ROM has no
 * "No number found" path in the call register). Order matches the ROM builder. */
static uint8_t call_register_visible_options(const store_call_record_t *record, const cr_label_t **out) {
    bool has_number = record->number[0] != '\0';
    bool has_name = record->name[0] != '\0';
    uint8_t n = 0u;
    if (has_number) {
        out[n++] = &CALL_REGISTER_OPTIONS_LABELS[0]; /* Call */
        out[n++] = &CALL_REGISTER_OPTIONS_LABELS[1]; /* Send SMS */
    }
    out[n++] = &CALL_REGISTER_OPTIONS_LABELS[2];     /* Time of call */
    if (has_number) {
        out[n++] = &CALL_REGISTER_OPTIONS_LABELS[3]; /* Edit number */
        out[n++] = &CALL_REGISTER_OPTIONS_LABELS[4]; /* Save */
    }
    out[n++] = &CALL_REGISTER_OPTIONS_LABELS[5];     /* Erase */
    if (has_name && has_number) {
        out[n++] = &CALL_REGISTER_OPTIONS_LABELS[6]; /* View number */
    }
    return n;
}

void render_call_register_options(const app_t *app, framebuffer_t *fb) {
    store_call_record_t record;
    if (!call_register_current_record(app, &record)) {
        fb_clear(fb, false);
        return;
    }
    const cr_label_t *entries[ARRAY_COUNT(CALL_REGISTER_OPTIONS_LABELS)];
    uint8_t count = call_register_visible_options(&record, entries);
    uint8_t selected = app->call_register_options_selected;
    if (count != 0u && selected >= count) {
        selected = (uint8_t)(count - 1u);
    }
    const char *shown[ARRAY_COUNT(CALL_REGISTER_OPTIONS_LABELS)];
    for (uint8_t i = 0; i < count; i++) {
        shown[i] = cr_loc(entries[i]);
    }
    draw_flat_list(fb, shown, count, selected, "", "Select");
}

void render_call_register_detail(const app_t *app, framebuffer_t *fb) {
    fb_clear(fb, false);
    const font_t *small = asset_font(FONT_FS2);
    const font_t *preview = asset_font(FONT_FS1);
    draw_center_text_box(fb, small, app->call_register_detail_title, 0, 0, FB_WIDTH);
    if (app->call_register_detail_mode == CALL_REGISTER_DETAIL_NUMBER) {
        fb_text(fb, small, ts_or(0x28du, "Number:"), 2, 12, true, 78);
        char scroll[44];
        /* v6.00: a numberless (no-CLI) entry has no number to show; the ROM
         * substitutes SID 939 "(no number)" (SID-index 0x371, call-register pool
         * 0x25df38) rather than a bare placeholder -- same string the list row
         * shows. "(????)" is only an internal store sentinel, never displayed. */
        const char *number_text = app->call_register_detail_value[0] ? app->call_register_detail_value : ts_or(0x3abu, "(no number)");
        fb_text(fb,
                preview,
                ui_marquee_text(preview, number_text, 80, scroll, sizeof(scroll)),
                2,
                23,
                true,
                80);
        draw_softkey(fb, ts_or(0x2eau, "Options"));
        return;
    }
    draw_center_text_box(fb, preview, app->call_register_detail_value[0] ? app->call_register_detail_value : "-", 4, 18, 76);
    draw_softkey(fb, ts_or(0x2d8u, "Back"));
}

void render_call_register_metric(const app_t *app, framebuffer_t *fb) {
    call_register_metric_kind_t kind = (call_register_metric_kind_t)app->call_register_metric_kind;
    const cr_label_t *labels = kind == CALL_REGISTER_METRIC_COSTS ? CALL_REGISTER_COST_LABELS : CALL_REGISTER_DURATION_LABELS;
    uint8_t count = kind == CALL_REGISTER_METRIC_COSTS
        ? (uint8_t)ARRAY_COUNT(CALL_REGISTER_COST_LABELS)
        : (uint8_t)ARRAY_COUNT(CALL_REGISTER_DURATION_LABELS);
    uint8_t selected = app->call_register_metric_selected;
    if (selected >= count) {
        selected = 0u;
    }
    char breadcrumb[8];
    char preview[18];
    snprintf(breadcrumb, sizeof(breadcrumb), "3-%u-%u",
             kind == CALL_REGISTER_METRIC_COSTS ? 6u : 5u,
             (unsigned)(selected + 1u));
    draw_static_page_list(fb,
                          cr_loc(&labels[selected]),
                          call_register_metric_has_preview(kind, selected)
                              ? call_register_metric_value(kind, selected, preview, sizeof(preview))
                              : "",
                          selected,
                          count,
                          breadcrumb,
                          "Select");
}

bool handle_call_register_menu_key(app_t *app, uint16_t key, uint32_t now) {
    call_register_menu_kind_t kind = (call_register_menu_kind_t)app->call_register_menu_kind;
    uint8_t count = call_register_menu_count(kind);
    if (count == 0u) {
        return true;
    }
    if (app->call_register_menu_selected >= count) {
        app->call_register_menu_selected = 0u;
    }
    if (key == KEY_UP) {
        app->call_register_menu_selected = app->call_register_menu_selected == 0u
            ? (uint8_t)(count - 1u)
            : (uint8_t)(app->call_register_menu_selected - 1u);
        app->dirty = true;
        return true;
    }
    if (key == KEY_DOWN) {
        app->call_register_menu_selected = (uint8_t)((app->call_register_menu_selected + 1u) % count);
        app->dirty = true;
        return true;
    }
    if (key == KEY_C) {
        if (kind == CALL_REGISTER_MENU_ROOT) {
            app->menu_index = 2u;
            app->route = APP_ROUTE_MAIN_MENU;
        } else if (kind == CALL_REGISTER_MENU_ERASE) {
            open_call_register_menu(app, CALL_REGISTER_MENU_ROOT, 3u);
        } else if (kind == CALL_REGISTER_MENU_COST_SETTINGS) {
            open_call_register_menu(app, CALL_REGISTER_MENU_ROOT, 6u);
        } else if (kind == CALL_REGISTER_MENU_PREPAID) {
            open_call_register_menu(app, CALL_REGISTER_MENU_ROOT, 7u);
        } else if (kind == CALL_REGISTER_MENU_CREDIT_INFO) {
            open_call_register_menu(app, CALL_REGISTER_MENU_PREPAID, 0u);
        } else {
            open_call_register_menu(app, CALL_REGISTER_MENU_PREPAID, 3u);
        }
        app->dirty = true;
        return true;
    }
    if (key != KEY_NAVI) {
        return true;
    }

    if (kind == CALL_REGISTER_MENU_ROOT) {
        switch (app->call_register_menu_selected) {
        case 0:
            open_call_register_list(app, STORE_CALL_LIST_MISSED, APP_ROUTE_CALL_REGISTER_MENU, false, 0u, now);
            return true;
        case 1:
            open_call_register_list(app, STORE_CALL_LIST_RECEIVED, APP_ROUTE_CALL_REGISTER_MENU, false, 0u, now);
            return true;
        case 2:
            open_call_register_list(app, STORE_CALL_LIST_DIALLED, APP_ROUTE_CALL_REGISTER_MENU, false, 0u, now);
            return true;
        case 3:
            open_call_register_menu(app, CALL_REGISTER_MENU_ERASE, 0u);
            return true;
        case 4:
            open_call_register_metric(app, CALL_REGISTER_METRIC_DURATION);
            return true;
        case 5:
            open_call_register_metric(app, CALL_REGISTER_METRIC_COSTS);
            return true;
        case 6:
            open_call_register_menu(app, CALL_REGISTER_MENU_COST_SETTINGS, 0u);
            return true;
        case 7:
            open_call_register_menu(app, CALL_REGISTER_MENU_PREPAID, 0u);
            return true;
        default:
            return true;
        }
    }
    if (kind == CALL_REGISTER_MENU_ERASE) {
        call_register_clear_recent(app, app->call_register_menu_selected, now);
        return true;
    }
    if (kind == CALL_REGISTER_MENU_COST_SETTINGS) {
        open_display_sid(app, 2u, 0x297u, "SIM card\nnot ready", APP_ROUTE_CALL_REGISTER_MENU, now);
        return true;
    }
    if (kind == CALL_REGISTER_MENU_PREPAID) {
        if (app->call_register_menu_selected == 0u) {
            uint8_t enabled = 0u;
            store_setting_get_u8(STORE_SETTING_CALL_CREDIT_INFO_DISPLAY, &enabled);
            open_call_register_menu(app, CALL_REGISTER_MENU_CREDIT_INFO, enabled ? 0u : 1u);
        } else if (app->call_register_menu_selected == 3u) {
            open_call_register_menu(app, CALL_REGISTER_MENU_RECHARGE_STATUS, 0u);
        } else {
            open_display_sid(app, 2u, 0x297u, "SIM card\nnot ready", APP_ROUTE_CALL_REGISTER_MENU, now);
        }
        return true;
    }
    if (kind == CALL_REGISTER_MENU_CREDIT_INFO) {
        store_setting_set_u8(STORE_SETTING_CALL_CREDIT_INFO_DISPLAY,
                             app->call_register_menu_selected == 0u ? 1u : 0u);
        open_call_register_menu(app, CALL_REGISTER_MENU_PREPAID, 0u);
        open_display_sid(app, 3u, 0x3b4u, "Saved", APP_ROUTE_CALL_REGISTER_MENU, now);
        return true;
    }
    open_display_sid(app, 2u, 0x297u, "SIM card\nnot ready", APP_ROUTE_CALL_REGISTER_MENU, now);
    return true;
}

bool handle_call_register_list_key(app_t *app, uint16_t key, uint32_t now) {
    uint8_t count = store_call_count((store_call_list_t)app->call_register_list_kind);
    if (count == 0u) {
        if (key == KEY_C || key == KEY_NAVI) {
            app->route = app->call_register_return_route;
            app->dirty = true;
        }
        return true;
    }
    if (key == KEY_UP) {
        app->call_register_list_selected = app->call_register_list_selected == 0u
            ? (uint8_t)(count - 1u)
            : (uint8_t)(app->call_register_list_selected - 1u);
        app->dirty = true;
        return true;
    }
    if (key == KEY_DOWN) {
        app->call_register_list_selected = (uint8_t)((app->call_register_list_selected + 1u) % count);
        app->dirty = true;
        return true;
    }
    if (key == KEY_C) {
        app->route = app->call_register_return_route;
        app->dirty = true;
        return true;
    }
    if (key == KEY_NAVI) {
        store_call_record_t record;
        if (!call_register_current_record(app, &record)) {
            return true;
        }
        if (app->call_register_navi_call) {
            call_register_start_call(app, &record, now);
        } else {
            open_call_register_options(app);
        }
        return true;
    }
    return true;
}

bool handle_call_register_options_key(app_t *app, uint16_t key, uint32_t now) {
    store_call_record_t record;
    const cr_label_t *labels[ARRAY_COUNT(CALL_REGISTER_OPTIONS_LABELS)];
    uint8_t count = call_register_current_record(app, &record)
        ? call_register_visible_options(&record, labels)
        : 0u;
    if (count == 0u) {
        app->route = APP_ROUTE_CALL_REGISTER_LIST;
        app->dirty = true;
        return true;
    }
    if (app->call_register_options_selected >= count) {
        app->call_register_options_selected = (uint8_t)(count - 1u);
    }
    if (key == KEY_UP) {
        app->call_register_options_selected = app->call_register_options_selected == 0u
            ? (uint8_t)(count - 1u)
            : (uint8_t)(app->call_register_options_selected - 1u);
        app->dirty = true;
        return true;
    }
    if (key == KEY_DOWN) {
        app->call_register_options_selected = (uint8_t)((app->call_register_options_selected + 1u) % count);
        app->dirty = true;
        return true;
    }
    if (key == KEY_C) {
        app->route = APP_ROUTE_CALL_REGISTER_LIST;
        app->dirty = true;
        return true;
    }
    if (key != KEY_NAVI) {
        return true;
    }
    const char *label = labels[app->call_register_options_selected]->label;
    if (strcmp(label, "Call") == 0) {
        call_register_start_call(app, &record, now);
    } else if (strcmp(label, "Send SMS") == 0) {
        open_sms_composer(app, "", record.number, now);
        open_sms_recipient_editor(app, record.number, now);
    } else if (strcmp(label, "Time of call") == 0) {
        char value[18];
        format_call_record_time(&record, value, sizeof(value));
        open_call_register_detail(app, CALL_REGISTER_DETAIL_TIME, ts_or(0x3b0u, "Time of call"), value, APP_ROUTE_CALL_REGISTER_OPTIONS);
    } else if (strcmp(label, "Edit number") == 0) {
        open_editor(app,
                    ts_or(0x28du, "Number:"),
                    record.number,
                    30u,
                    EDITOR_KIND_NUMBER,
                    EDITOR_CONTEXT_CALL_REGISTER_EDIT_NUMBER,
                    true,
                    now);
    } else if (strcmp(label, "Save") == 0) {
        call_register_save_record(app, &record, now);
    } else if (strcmp(label, "Erase") == 0) {
        open_confirm(app, CONFIRM_CONTEXT_CALL_REGISTER_ERASE, ts_or(0x277u, "Erase?"), 0, 0, 4);
    } else if (strcmp(label, "View number") == 0) {
        open_call_register_detail(app,
                                  CALL_REGISTER_DETAIL_NUMBER,
                                  record.name[0] ? record.name : record.number,
                                  record.number,
                                  APP_ROUTE_CALL_REGISTER_OPTIONS);
    }
    return true;
}

bool handle_call_register_detail_key(app_t *app, uint16_t key, uint32_t now) {
    (void)now;
    if (key == KEY_C || key == KEY_NAVI) {
        app->route = app->call_register_detail_return_route;
        app->dirty = true;
    }
    return true;
}

bool handle_call_register_metric_key(app_t *app, uint16_t key, uint32_t now) {
    call_register_metric_kind_t kind = (call_register_metric_kind_t)app->call_register_metric_kind;
    uint8_t count = kind == CALL_REGISTER_METRIC_COSTS
        ? (uint8_t)ARRAY_COUNT(CALL_REGISTER_COST_LABELS)
        : (uint8_t)ARRAY_COUNT(CALL_REGISTER_DURATION_LABELS);
    if (key == KEY_UP) {
        app->call_register_metric_selected = app->call_register_metric_selected == 0u
            ? (uint8_t)(count - 1u)
            : (uint8_t)(app->call_register_metric_selected - 1u);
        app->dirty = true;
        return true;
    }
    if (key == KEY_DOWN) {
        app->call_register_metric_selected = (uint8_t)((app->call_register_metric_selected + 1u) % count);
        app->dirty = true;
        return true;
    }
    if (key == KEY_C) {
        open_call_register_menu(app, CALL_REGISTER_MENU_ROOT, kind == CALL_REGISTER_METRIC_COSTS ? 5u : 4u);
        return true;
    }
    if (key != KEY_NAVI) {
        return true;
    }
    if (kind == CALL_REGISTER_METRIC_DURATION && app->call_register_metric_selected == 4u) {
        /* CALL.3: Clear timers zeroes the persistent accounting fields
         * (0x071b-0x071e); it is not a permanent latch, so new calls count
         * again immediately. */
        store_setting_set_u32(STORE_SETTING_CALL_DURATION_LAST, 0u);
        store_setting_set_u32(STORE_SETTING_CALL_DURATION_ALL, 0u);
        store_setting_set_u32(STORE_SETTING_CALL_DURATION_RECEIVED, 0u);
        store_setting_set_u32(STORE_SETTING_CALL_DURATION_DIALLED, 0u);
        open_display_sid(app, 3u, 0x3cau, "Timers\ncleared", APP_ROUTE_CALL_REGISTER_METRIC, now);
        return true;
    }
    if (kind == CALL_REGISTER_METRIC_COSTS && app->call_register_metric_selected == 2u) {
        store_setting_set_u8(STORE_SETTING_CALL_COUNTERS_CLEARED, 1u);
        open_display_sid(app, 3u, 0x100u, "Counters\ncleared", APP_ROUTE_CALL_REGISTER_METRIC, now);
        return true;
    }
    char value[18];
    const cr_label_t *labels = kind == CALL_REGISTER_METRIC_COSTS ? CALL_REGISTER_COST_LABELS : CALL_REGISTER_DURATION_LABELS;
    open_call_register_detail(app,
                              CALL_REGISTER_DETAIL_METRIC,
                              cr_loc(&labels[app->call_register_metric_selected]),
                              call_register_metric_value(kind, app->call_register_metric_selected, value, sizeof(value)),
                              APP_ROUTE_CALL_REGISTER_METRIC);
    return true;
}

void open_call_register_menu(app_t *app, call_register_menu_kind_t kind, uint8_t selected) {
    uint8_t count = call_register_menu_count(kind);
    app->route = APP_ROUTE_CALL_REGISTER_MENU;
    app->call_register_menu_kind = (uint8_t)kind;
    app->call_register_menu_selected = count != 0u && selected < count ? selected : 0u;
    app->dirty = true;
}

void open_call_register_list(app_t *app, store_call_list_t kind, app_route_t return_route, bool navi_call, uint8_t selected, uint32_t now) {
    uint8_t count = store_call_count(kind);
    if (kind == STORE_CALL_LIST_DIALLED && return_route == APP_ROUTE_STANDBY && count == 0u) {
        open_display_sid(app, 2u, 0x27eu, "No last\ndialled\nnumbers", APP_ROUTE_STANDBY, now);
        return;
    }
    if (kind == STORE_CALL_LIST_MISSED) {
        app->missed_call_pending = false;
        app->missed_call_pending_count = 0u;
    }
    app->route = APP_ROUTE_CALL_REGISTER_LIST;
    app->call_register_list_kind = (uint8_t)kind;
    app->call_register_return_route = return_route;
    app->call_register_navi_call = navi_call ? 1u : 0u;
    app->call_register_list_selected = count != 0u && selected < count ? selected : 0u;
    app->dirty = true;
}

static void open_call_register_options(app_t *app) {
    app->call_register_options_selected = 0u;
    app->route = APP_ROUTE_CALL_REGISTER_OPTIONS;
    app->dirty = true;
}

static void open_call_register_detail(app_t *app, call_register_detail_mode_t mode, const char *title, const char *value, app_route_t return_route) {
    app->call_register_detail_mode = (uint8_t)mode;
    copy_text(app->call_register_detail_title, sizeof(app->call_register_detail_title), title);
    copy_text(app->call_register_detail_value, sizeof(app->call_register_detail_value), value);
    app->call_register_detail_return_route = return_route;
    app->route = APP_ROUTE_CALL_REGISTER_DETAIL;
    app->dirty = true;
}

static void open_call_register_metric(app_t *app, call_register_metric_kind_t kind) {
    app->call_register_metric_kind = (uint8_t)kind;
    app->call_register_metric_selected = 0u;
    app->route = APP_ROUTE_CALL_REGISTER_METRIC;
    app->dirty = true;
}

static const cr_label_t *call_register_menu_labels(call_register_menu_kind_t kind) {
    switch (kind) {
    case CALL_REGISTER_MENU_ERASE: return CALL_REGISTER_ERASE_LABELS;
    case CALL_REGISTER_MENU_COST_SETTINGS: return CALL_REGISTER_COST_SETTINGS_LABELS;
    case CALL_REGISTER_MENU_PREPAID: return CALL_REGISTER_PREPAID_LABELS;
    case CALL_REGISTER_MENU_CREDIT_INFO: return CALL_REGISTER_CREDIT_INFO_LABELS;
    case CALL_REGISTER_MENU_RECHARGE_STATUS: return CALL_REGISTER_RECHARGE_LABELS;
    case CALL_REGISTER_MENU_ROOT:
    default: return CALL_REGISTER_ROOT_LABELS;
    }
}

static uint8_t call_register_menu_count(call_register_menu_kind_t kind) {
    switch (kind) {
    case CALL_REGISTER_MENU_ERASE: return (uint8_t)ARRAY_COUNT(CALL_REGISTER_ERASE_LABELS);
    case CALL_REGISTER_MENU_COST_SETTINGS: return (uint8_t)ARRAY_COUNT(CALL_REGISTER_COST_SETTINGS_LABELS);
    case CALL_REGISTER_MENU_PREPAID: return (uint8_t)ARRAY_COUNT(CALL_REGISTER_PREPAID_LABELS);
    case CALL_REGISTER_MENU_CREDIT_INFO: return (uint8_t)ARRAY_COUNT(CALL_REGISTER_CREDIT_INFO_LABELS);
    case CALL_REGISTER_MENU_RECHARGE_STATUS: return (uint8_t)ARRAY_COUNT(CALL_REGISTER_RECHARGE_LABELS);
    case CALL_REGISTER_MENU_ROOT:
    default: return (uint8_t)ARRAY_COUNT(CALL_REGISTER_ROOT_LABELS);
    }
}

static const char *call_register_menu_breadcrumb(call_register_menu_kind_t kind, uint8_t selected, char *scratch, size_t scratch_cap) {
    switch (kind) {
    case CALL_REGISTER_MENU_ERASE:
        snprintf(scratch, scratch_cap, "3-4-%u", (unsigned)(selected + 1u));
        break;
    case CALL_REGISTER_MENU_COST_SETTINGS:
        snprintf(scratch, scratch_cap, "3-7-%u", (unsigned)(selected + 1u));
        break;
    case CALL_REGISTER_MENU_PREPAID:
        snprintf(scratch, scratch_cap, "3-8-%u", (unsigned)(selected + 1u));
        break;
    case CALL_REGISTER_MENU_CREDIT_INFO:
        snprintf(scratch, scratch_cap, "%u", (unsigned)(selected + 1u));
        break;
    case CALL_REGISTER_MENU_RECHARGE_STATUS:
        snprintf(scratch, scratch_cap, "3-8-4-%u", (unsigned)(selected + 1u));
        break;
    case CALL_REGISTER_MENU_ROOT:
    default:
        snprintf(scratch, scratch_cap, "3-%u", (unsigned)(selected + 1u));
        break;
    }
    return scratch;
}

static const char *call_register_preview(call_register_menu_kind_t kind, const char *label, char *scratch, size_t scratch_cap) {
    scratch[0] = '\0';
    if (kind == CALL_REGISTER_MENU_COST_SETTINGS) {
        if (strcmp(label, "Call costs'\nlimit") == 0) {
            copy_text(scratch, scratch_cap, ts_or(0x10au, "Off"));
        } else if (strcmp(label, "Show\ncosts in") == 0) {
            copy_text(scratch, scratch_cap, ts_or(0x11cu, "Units"));
        }
    } else if (kind == CALL_REGISTER_MENU_PREPAID) {
        if (strcmp(label, "Credit info display") == 0) {
            uint8_t enabled = 0u;
            store_setting_get_u8(STORE_SETTING_CALL_CREDIT_INFO_DISPLAY, &enabled);
            copy_text(scratch, scratch_cap, enabled ? ts_or(0x10bu, "On") : ts_or(0x10au, "Off"));
        } else if (strcmp(label, "Credit available") == 0 || strcmp(label, "Last event costs") == 0) {
            copy_text(scratch, scratch_cap, "0.00");
        }
    } else if (kind == CALL_REGISTER_MENU_RECHARGE_STATUS) {
        copy_text(scratch, scratch_cap, "--.--");
    }
    return scratch;
}

static bool call_register_menu_has_preview(call_register_menu_kind_t kind, const char *label) {
    return kind == CALL_REGISTER_MENU_COST_SETTINGS ||
           kind == CALL_REGISTER_MENU_RECHARGE_STATUS ||
           (kind == CALL_REGISTER_MENU_PREPAID &&
            (strcmp(label, "Credit info display") == 0 ||
             strcmp(label, "Credit available") == 0 ||
             strcmp(label, "Last event costs") == 0));
}

static store_call_list_t call_register_selected_list_kind(const app_t *app) {
    if (app->call_register_list_kind >= STORE_CALL_LIST_COUNT) {
        return STORE_CALL_LIST_MISSED;
    }
    return (store_call_list_t)app->call_register_list_kind;
}

static bool call_register_current_record(const app_t *app, store_call_record_t *record) {
    store_call_list_t kind = call_register_selected_list_kind(app);
    uint8_t count = store_call_count(kind);
    if (count == 0u) {
        return false;
    }
    uint8_t selected = app->call_register_list_selected >= count ? (uint8_t)(count - 1u) : app->call_register_list_selected;
    return store_call_get(kind, selected, record) == STORE_STATUS_OK;
}

static const char *call_register_caption(store_call_list_t kind, uint8_t index, char *scratch, size_t cap) {
    /* v6.00 list headers "Missed %N:" / "Received %N:" / "Dialled %N:" with the
     * record ordinal substituted for the "%N" token. */
    uint16_t sid = 0x1c8u;
    const char *fallback = "Missed %N:";
    if (kind == STORE_CALL_LIST_RECEIVED) {
        sid = 0x258u;
        fallback = "Received %N:";
    } else if (kind == STORE_CALL_LIST_DIALLED) {
        sid = 0x121u;
        fallback = "Dialled %N:";
    }
    const char *tmpl = ts(sid);
    if (tmpl == 0) {
        tmpl = fallback;
    }
    char num[8];
    snprintf(num, sizeof(num), "%u", (unsigned)index);
    size_t di = 0u;
    for (size_t i = 0u; tmpl[i] != '\0' && di + 1u < cap;) {
        if (tmpl[i] == '%' && tmpl[i + 1u] == 'N') {
            for (size_t k = 0u; num[k] != '\0' && di + 1u < cap; k++) {
                scratch[di++] = num[k];
            }
            i += 2u;
        } else {
            scratch[di++] = tmpl[i++];
        }
    }
    scratch[di] = '\0';
    return scratch;
}

static void format_call_record_time(const store_call_record_t *record, char *dst, size_t cap) {
    if (record == 0) {
        copy_text(dst, cap, "-");
        return;
    }
    if (record->duration_seconds > 0u) {
        format_duration(record->duration_seconds, dst, cap, false);
        return;
    }
    if (record->datetime.year != 0u) {
        snprintf(dst, cap, "%02u:%02u", (unsigned)record->datetime.hour, (unsigned)record->datetime.minute);
        return;
    }
    copy_text(dst, cap, "-");
}

static void format_duration(uint32_t seconds, char *dst, size_t cap, bool hms) {
    uint32_t hours = seconds / 3600u;
    uint32_t minutes = (seconds % 3600u) / 60u;
    uint32_t rest = seconds % 60u;
    if (hms || hours > 0u) {
        snprintf(dst, cap, "%lu:%02lu:%02lu",
                 (unsigned long)hours,
                 (unsigned long)minutes,
                 (unsigned long)rest);
    } else {
        snprintf(dst, cap, "%02lu:%02lu", (unsigned long)minutes, (unsigned long)rest);
    }
}

static const char *call_register_metric_value(call_register_metric_kind_t kind, uint8_t selected, char *scratch, size_t scratch_cap) {
    uint8_t cleared = 0u;
    if (kind == CALL_REGISTER_METRIC_COSTS) {
        store_setting_get_u8(STORE_SETTING_CALL_COUNTERS_CLEARED, &cleared);
        if (cleared) {
            copy_text(scratch, scratch_cap, "0.00");
        } else if (selected == 0u) {
            copy_text(scratch, scratch_cap, "0.40");
        } else {
            copy_text(scratch, scratch_cap, "1.20");
        }
        return scratch;
    }

    /* CALL.3: durations come from the persistent accounting fields
     * (0x071b-0x071e), not from summing the erasable recent-call lists. */
    uint32_t seconds = 0u;
    if (selected == 0u) {
        store_setting_get_u32(STORE_SETTING_CALL_DURATION_LAST, &seconds);
    } else if (selected == 1u) {
        store_setting_get_u32(STORE_SETTING_CALL_DURATION_ALL, &seconds);
    } else if (selected == 2u) {
        store_setting_get_u32(STORE_SETTING_CALL_DURATION_RECEIVED, &seconds);
    } else if (selected == 3u) {
        store_setting_get_u32(STORE_SETTING_CALL_DURATION_DIALLED, &seconds);
    }
    format_duration(seconds, scratch, scratch_cap, true);
    return scratch;
}

static bool call_register_metric_has_preview(call_register_metric_kind_t kind, uint8_t selected) {
    return (kind == CALL_REGISTER_METRIC_DURATION && selected < 4u) ||
           (kind == CALL_REGISTER_METRIC_COSTS && selected < 2u);
}

static void call_register_clear_recent(app_t *app, uint8_t selected, uint32_t now) {
    if (selected == 0u) {
        store_call_clear(STORE_CALL_LIST_MISSED);
        store_call_clear(STORE_CALL_LIST_RECEIVED);
        store_call_clear(STORE_CALL_LIST_DIALLED);
        app->missed_call_pending = false;
        app->missed_call_pending_count = 0u;
        open_call_register_menu(app, CALL_REGISTER_MENU_ROOT, 3u);
        open_display_sid(app, 3u, 0x047u, "All\ncall lists\nerased", APP_ROUTE_CALL_REGISTER_MENU, now);
    } else if (selected == 1u) {
        store_call_clear(STORE_CALL_LIST_MISSED);
        app->missed_call_pending = false;
        app->missed_call_pending_count = 0u;
        open_call_register_menu(app, CALL_REGISTER_MENU_ROOT, 3u);
        open_display_sid(app, 3u, 0x1c7u, "Missed\ncalls\nerased", APP_ROUTE_CALL_REGISTER_MENU, now);
    } else if (selected == 2u) {
        store_call_clear(STORE_CALL_LIST_DIALLED);
        open_call_register_menu(app, CALL_REGISTER_MENU_ROOT, 3u);
        open_display_sid(app, 3u, 0x1a2u, "Dialled\nnumbers\nerased", APP_ROUTE_CALL_REGISTER_MENU, now);
    } else {
        store_call_clear(STORE_CALL_LIST_RECEIVED);
        open_call_register_menu(app, CALL_REGISTER_MENU_ROOT, 3u);
        open_display_sid(app, 3u, 0x257u, "Received\ncalls\nerased", APP_ROUTE_CALL_REGISTER_MENU, now);
    }
}

static void call_register_start_call(app_t *app, const store_call_record_t *record, uint32_t now) {
    if (record == 0 || record->number[0] == '\0') {
        open_display_sid(app, 0u, 0x208u, "No phone\nnumber", APP_ROUTE_CALL_REGISTER_LIST, now);
        return;
    }
    (void)start_outgoing_call(app, record->number, record->name, now, APP_ROUTE_CALL_REGISTER_LIST);
}

static void call_register_save_record(app_t *app, const store_call_record_t *record, uint32_t now) {
    if (record == 0 || record->number[0] == '\0') {
        open_display_sid(app, 0u, 0x208u, "No phone\nnumber", APP_ROUTE_CALL_REGISTER_LIST, now);
        return;
    }
    char name[MODEM_PHONEBOOK_NAME_MAX + 1u];
    if (record->name[0] != '\0') {
        copy_text(name, sizeof(name), record->name);
    } else {
        snprintf(name, sizeof(name), "Call %u", (unsigned)(modem_service_phonebook_count() + 1u));
    }
    start_phonebook_add_with_context(app,
                                     name,
                                     record->number,
                                     PHONEBOOK_LABEL_CALL,
                                     "3",
                                     PHONEBOOK_CONTEXT_CALL_REGISTER_SAVE,
                                     now);
}

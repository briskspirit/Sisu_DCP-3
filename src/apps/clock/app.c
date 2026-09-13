#include "apps/clock_app.h"
#include "clock_internal.h"

#include <stdio.h>
#include <string.h>

#include "apps/clock_date_logic.h"
#include "apps/dialogs_app.h"
#include "services/input_keys.h"
#include "ui/ui.h"
#include "hal/rtc_alarm_hal.h"
#include "storage/store_service.h"
#include "services/strings.h"

/* Menu labels carry the v6.00 string id (SID) for localization alongside the
 * English literal, which stays BOTH the fallback AND the strcmp dispatch key
 * (see apply_clock_menu_label). So the label[] entry is never localized in
 * place -- only the rendered copy is (built via L() at draw time). sid 0 = no
 * exact 1:1 v6.00 match (the ROM string is multiline or the text is ambiguous
 * across contexts), fall back to the English literal. */
typedef struct {
    const char *label;
    uint16_t sid;
} clock_label_t;

/* Localize by SID, English fallback: sid 0 (no 1:1 match) or a sid with no
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

static const clock_label_t CLOCK_ROOT_LABELS[] = {
    {"Alarm clock", 0x2c1u},     /* clock menu descriptor item 1 -> SID 0x2c1 "Alarm\nclock" (ref 0402c1) */
    {"Clock settings", 0x2d0u},  /* ROM "Clock\nsettings" (wrap_text_lines honours \n) */
    {"Date setting", 0x24fu},
};
static const clock_label_t CLOCK_ALARM_LABELS[] = {
    {"On", 0x3fu},   /* SID from the clock-alarm trace (label stays the dispatch key) */
    {"Off", 0x3eu},
};
static const clock_label_t CLOCK_VISIBLE_LABELS[] = {
    {"Hide clock", 0x24au},
    {"Set the time", 0x24eu},
    {"Time format", 0x248u},
};
static const clock_label_t CLOCK_HIDDEN_LABELS[] = {
    {"Show clock", 0x251u},
    {"Set the time", 0x24eu},
    {"Time format", 0x248u},
};
static const clock_label_t CLOCK_TIME_FORMAT_LABELS[] = {
    {"24-hour", 0x245u},
    {"12-hour", 0x243u},
};
static const clock_label_t CLOCK_AM_PM_LABELS[] = {
    {"am", 0x246u},
    {"pm", 0x24bu},
};

static void current_clock_datetime(rtc_datetime_t *dt);
static void format_clock_preview(char *dst, size_t cap);
static uint8_t clock_menu_count(clock_menu_kind_t kind);
static const clock_label_t *clock_menu_labels(clock_menu_kind_t kind);
static const char *clock_menu_breadcrumb(clock_menu_kind_t kind, uint8_t selected, char *scratch, size_t scratch_cap);
static const char *clock_menu_softkey(clock_menu_kind_t kind);
static bool apply_clock_menu_label(app_t *app, const char *label, uint32_t now);
static void cancel_clock_menu(app_t *app, uint32_t now);
static bool clock_setting_u16(store_setting_key_t key, uint16_t *value);

void render_clock_menu(const app_t *app, framebuffer_t *fb) {
    clock_menu_kind_t kind = (clock_menu_kind_t)app->clock_menu_kind;
    uint8_t selected = app->clock_menu_selected;
    if (kind != CLOCK_MENU_ROOT) {
        char breadcrumb[8];
        const clock_label_t *labels = clock_menu_labels(kind);
        uint8_t count = clock_menu_count(kind);
        if (selected >= count) {
            selected = 0u;
        }
        /* Render the localized copy; labels[].label stays English for dispatch. */
        const char *localized[3];
        if (count > (uint8_t)ARRAY_COUNT(localized)) {
            count = (uint8_t)ARRAY_COUNT(localized);
        }
        for (uint8_t i = 0; i < count; i++) {
            localized[i] = L(labels[i].sid, labels[i].label);
        }
        draw_flat_list(fb,
                       localized,
                       count,
                       selected,
                       clock_menu_breadcrumb(kind, selected, breadcrumb, sizeof(breadcrumb)),
                       clock_menu_softkey(kind));
        return;
    }

    fb_clear(fb, false);
    const font_t *font = asset_font(FONT_FS2);
    const font_t *preview_font = asset_font(FONT_FS1);
    if (selected >= ARRAY_COUNT(CLOCK_ROOT_LABELS)) {
        selected = 0u;
    }
    const clock_label_t *root_entry = &CLOCK_ROOT_LABELS[selected];
    /* Localize for display; width/wrap below operate on the localized string. */
    const char *label = L(root_entry->sid, root_entry->label);
    /* Holds a localized preview value ("Time not set"/"Clock hidden" flattened;
     * GREE reaches ~34 bytes). draw_right_text_box then fits it to the box. */
    char preview[48];
    char breadcrumb[5];
    snprintf(breadcrumb, sizeof(breadcrumb), "8-%u", (unsigned)(selected + 1u));
    preview[0] = '\0';
    if (selected == 0u) {
        clock_format_alarm_preview(preview, sizeof(preview));
    } else if (selected == 1u) {
        format_clock_preview(preview, sizeof(preview));
    } else {
        clock_format_date_preview(preview, sizeof(preview));
    }

    char lines[3][32];
    uint8_t line_count = wrap_text_lines_ex(font, label, 80, (char *)lines, 32u, preview[0] != '\0' ? 2u : 3u);
    for (uint8_t i = 0; i < line_count; i++) {
        fb_text(fb, font, lines[i], 0, 7 + i * 9, true, 80);
    }
    if (preview[0] != '\0') {
        draw_right_text_box(fb, preview_font, preview, 0, 31, 78);
    }
    draw_position_indicator(fb, selected, (uint8_t)ARRAY_COUNT(CLOCK_ROOT_LABELS), breadcrumb);
    draw_softkey(fb, "Select");
}

bool handle_clock_menu_key(app_t *app, uint16_t key, uint32_t now) {
    clock_menu_kind_t kind = (clock_menu_kind_t)app->clock_menu_kind;
    uint8_t count = clock_menu_count(kind);
    if (count == 0u) {
        return true;
    }
    if (app->clock_menu_selected >= count) {
        app->clock_menu_selected = 0u;
    }
    if (key == KEY_UP) {
        app->clock_menu_selected = app->clock_menu_selected == 0u
            ? (uint8_t)(count - 1u)
            : (uint8_t)(app->clock_menu_selected - 1u);
        app->dirty = true;
        return true;
    }
    if (key == KEY_DOWN) {
        app->clock_menu_selected = (uint8_t)((app->clock_menu_selected + 1u) % count);
        app->dirty = true;
        return true;
    }
    if (key == KEY_C) {
        cancel_clock_menu(app, now);
        return true;
    }
    if (key != KEY_NAVI) {
        return true;
    }

    if (kind == CLOCK_MENU_ROOT) {
        uint8_t time_set = 1u;
        clock_setting_u8(STORE_SETTING_CLOCK_TIME_SET, &time_set);
        if (app->clock_menu_selected == 0u) {
            if (!time_set) {
                clock_editor_require_setup(app, now, true);
            } else {
                clock_open_menu(app, CLOCK_MENU_ALARM, 0u);
            }
        } else if (app->clock_menu_selected == 1u) {
            uint8_t visible = 1u;
            clock_setting_u8(STORE_SETTING_CLOCK_SHOW_STANDBY, &visible);
            clock_open_menu(app, visible ? CLOCK_MENU_VISIBLE : CLOCK_MENU_HIDDEN, 0u);
        } else {
            if (!time_set) {
                clock_editor_require_setup(app, now, true);
            } else {
                clock_editor_open(app, CLOCK_EDITOR_DATE, 0, 0u, now);
            }
        }
        return true;
    }

    const clock_label_t *labels = clock_menu_labels(kind);
    return apply_clock_menu_label(app, labels[app->clock_menu_selected].label, now);
}

void open_clock_root(app_t *app) {
    /* Fresh entry from the main menu: no setup chain can be in progress, so clear
     * clock_setup_pending. This is the safety net for the flag outliving its
     * screen -- an incoming call (open_incoming_call preempts the route whenever
     * route != APP_ROUTE_CALL) or the one-shot alarm firing mid-chain hijacks the
     * route to STANDBY without ever reaching cancel/commit_clock_editor, leaving
     * the flag stranded true. Every route into the destructive cancel/commit
     * branches passes through here first (except the boot flow, which arms the
     * flag itself), so clearing it here stops a stale chain from wiping a
     * later, unrelated clock edit (cancel would clear the clock + display;
     * commit-date would force the display on). */
    app->clock_setup_pending = false;
    app->route = APP_ROUTE_CLOCK_MENU;
    app->clock_menu_kind = (uint8_t)CLOCK_MENU_ROOT;
    app->clock_menu_selected = 0u;
    app->dirty = true;
}

void clock_open_menu(app_t *app, clock_menu_kind_t kind, uint8_t selected) {
    uint8_t count = clock_menu_count(kind);
    app->route = APP_ROUTE_CLOCK_MENU;
    app->clock_menu_kind = (uint8_t)kind;
    app->clock_menu_selected = count != 0u && selected < count ? selected : 0u;
    app->dirty = true;
}

void load_clock_into_rtc(void) {
    uint8_t hour = 18u;
    uint8_t minute = 21u;
    uint8_t day = 1u;
    uint8_t month = 5u;
    uint16_t year = 2026u;
    uint8_t alarm_enabled = 0u;
    uint8_t alarm_hour = 7u;
    uint8_t alarm_minute = 30u;
    clock_setting_u8(STORE_SETTING_CLOCK_HOUR, &hour);
    clock_setting_u8(STORE_SETTING_CLOCK_MINUTE, &minute);
    clock_setting_u8(STORE_SETTING_CLOCK_DATE_DAY, &day);
    clock_setting_u8(STORE_SETTING_CLOCK_DATE_MONTH, &month);
    clock_setting_u16(STORE_SETTING_CLOCK_DATE_YEAR, &year);
    if (hour > 23u || minute > 59u || !clock_date_valid(day, month, year)) {
        hour = 18u;
        minute = 21u;
        day = 1u;
        month = 5u;
        year = 2026u;
    }
    rtc_datetime_t dt = {year, month, day, hour, minute, 0u};
    /* The RV-8803 keeps time while powered (battery in), so on a normal reboot it
     * is the source of truth. When the chip reports lost data (battery removed /
     * caps drained -- this board has no RTC backup cell), the stored time is
     * stale, so mark the clock UNSET: start_clock_boot_setup_if_needed() then
     * prompts to re-enter it (1:1 with the original after a battery pull) instead
     * of silently showing a stale/default time. The RTC is still seeded from the
     * stored/default value so the set-time editor opens on a sane starting point.
     * The alarm config below is always re-applied from storage. */
    if (!rtc_alarm_hal_time_valid()) {
        store_setting_set_u8(STORE_SETTING_CLOCK_TIME_SET, 0u);
        rtc_alarm_hal_set_datetime(&dt);
    }
    clock_setting_u8(STORE_SETTING_CLOCK_ALARM_ENABLED, &alarm_enabled);
    clock_setting_u8(STORE_SETTING_CLOCK_ALARM_HOUR, &alarm_hour);
    clock_setting_u8(STORE_SETTING_CLOCK_ALARM_MINUTE, &alarm_minute);
    if (alarm_hour > 23u || alarm_minute > 59u) {
        alarm_hour = 7u;
        alarm_minute = 30u;
    }
    rtc_alarm_hal_set_daily_alarm(alarm_hour, alarm_minute, alarm_enabled != 0u);
}

void update_standby_clock(app_t *app) {
    uint8_t time_set = 1u;
    uint8_t visible = 1u;
    uint8_t format24 = 1u;
    clock_setting_u8(STORE_SETTING_CLOCK_TIME_SET, &time_set);
    clock_setting_u8(STORE_SETTING_CLOCK_SHOW_STANDBY, &visible);
    clock_setting_u8(STORE_SETTING_CLOCK_FORMAT_24H, &format24);
    app->clock_text[0] = '\0';
    if (time_set && visible) {
        rtc_datetime_t dt;
        current_clock_datetime(&dt);
        if (format24) {
            snprintf(app->clock_text, sizeof(app->clock_text), "%02u:%02u", (unsigned)dt.hour, (unsigned)dt.minute);
        } else {
            uint8_t display_hour = (uint8_t)(dt.hour % 12u);
            if (display_hour == 0u) {
                display_hour = 12u;
            }
            snprintf(app->clock_text, sizeof(app->clock_text), "%02u:%02u%c",
                     (unsigned)display_hour,
                     (unsigned)dt.minute,
                     dt.hour < 12u ? 'A' : 'P');
        }
    }
    uint8_t alarm_enabled = 0u;
    clock_setting_u8(STORE_SETTING_CLOCK_ALARM_ENABLED, &alarm_enabled);
    app->clock_alarm_enabled = alarm_enabled != 0u;
}

void clock_set_alarm(app_t *app, uint8_t hour, uint8_t minute, bool enabled) {
    uint8_t stored_hour = 7u;
    uint8_t stored_minute = 30u;
    clock_setting_u8(STORE_SETTING_CLOCK_ALARM_HOUR, &stored_hour);
    clock_setting_u8(STORE_SETTING_CLOCK_ALARM_MINUTE, &stored_minute);
    if (enabled) {
        if (hour > 23u || minute > 59u) {
            return;
        }
        stored_hour = hour;
        stored_minute = minute;
        store_setting_set_u8(STORE_SETTING_CLOCK_ALARM_HOUR, hour);
        store_setting_set_u8(STORE_SETTING_CLOCK_ALARM_MINUTE, minute);
    }
    store_setting_set_u8(STORE_SETTING_CLOCK_ALARM_ENABLED, enabled ? 1u : 0u);
    rtc_alarm_hal_set_daily_alarm(stored_hour, stored_minute, enabled);
    /* The standby icon is backed by this app-local mirror. Keep it in lockstep
     * with the accepted alarm setting instead of waiting for the 1 s status poll.
     * In particular, a powered-off alarm's first C press disables the one-shot
     * before opening "Activate phone for calls?"; if the user accepts immediately,
     * the old cached true value must not survive into the first standby frame. */
    if (app != 0) {
        app->clock_alarm_enabled = enabled;
    }
}

static void current_clock_datetime(rtc_datetime_t *dt) {
    rtc_alarm_hal_get_datetime(dt);
}

void format_clock_time(char *dst, size_t cap, uint8_t hour, uint8_t minute, bool pad_hour, bool suffix) {
    uint8_t format24 = 1u;
    clock_setting_u8(STORE_SETTING_CLOCK_FORMAT_24H, &format24);
    if (format24) {
        snprintf(dst, cap, "%02u:%02u", (unsigned)hour, (unsigned)minute);
        return;
    }
    uint8_t display_hour = (uint8_t)(hour % 12u);
    if (display_hour == 0u) {
        display_hour = 12u;
    }
    if (suffix) {
        /* Alarm builder 0x0027ab6c appends the FS3 A/P marker glyph like the
         * standby strip, not " am"/" pm" words. */
        (void)pad_hour;
        snprintf(dst, cap,
                 "%02u:%02u%c",
                 (unsigned)display_hour,
                 (unsigned)minute,
                 hour < 12u ? 'A' : 'P');
    } else {
        snprintf(dst, cap,
                 pad_hour ? "%02u:%02u" : "%u:%02u",
                 (unsigned)display_hour,
                 (unsigned)minute);
    }
}

static void format_clock_preview(char *dst, size_t cap) {
    uint8_t time_set = 1u;
    uint8_t visible = 1u;
    clock_setting_u8(STORE_SETTING_CLOCK_TIME_SET, &time_set);
    clock_setting_u8(STORE_SETTING_CLOCK_SHOW_STANDBY, &visible);
    if (!time_set || !visible) {
        /* Not set, or the clock display is turned off -> a placeholder, not a
         * value (the "Time not set" / "Clock hidden" text is a standby/dialog
         * state, not the menu preview). */
        copy_text(dst, cap, "--:--");
        return;
    }
    rtc_datetime_t dt;
    current_clock_datetime(&dt);
    format_clock_time(dst, cap, dt.hour, dt.minute, false, true);
}

void clock_format_alarm_preview(char *dst, size_t cap) {
    uint8_t enabled = 0u;
    uint8_t hour = 7u;
    uint8_t minute = 30u;
    clock_setting_u8(STORE_SETTING_CLOCK_ALARM_ENABLED, &enabled);
    if (!enabled) {
        copy_text(dst, cap, ts_or(0x3eu, "Off"));
        return;
    }
    clock_setting_u8(STORE_SETTING_CLOCK_ALARM_HOUR, &hour);
    clock_setting_u8(STORE_SETTING_CLOCK_ALARM_MINUTE, &minute);
    if (hour > 23u || minute > 59u) {
        copy_text(dst, cap, ts_or(0x3eu, "Off"));
        return;
    }
    format_clock_time(dst, cap, hour, minute, false, true);
}

void clock_format_date_preview(char *dst, size_t cap) {
    /* Date is set together with the time (one CLOCK_TIME_SET flag). Not set ->
     * placeholder. (Also feeds the date editor's initial value: the dashes carry
     * no digits, so the editor falls back to its dd.mm.yyyy template.) */
    uint8_t time_set = 1u;
    clock_setting_u8(STORE_SETTING_CLOCK_TIME_SET, &time_set);
    if (!time_set) {
        copy_text(dst, cap, "--.--.----");
        return;
    }
    rtc_datetime_t dt;
    current_clock_datetime(&dt);
    uint8_t day = dt.day > 31u ? 31u : dt.day;
    uint8_t month = dt.month > 12u ? 12u : dt.month;
    uint16_t year = dt.year > 9999u ? 9999u : dt.year;
    snprintf(dst, cap, "%02u.%02u.%04u", (unsigned)day, (unsigned)month, (unsigned)year);
}

static uint8_t clock_menu_count(clock_menu_kind_t kind) {
    switch (kind) {
    case CLOCK_MENU_ROOT: return (uint8_t)ARRAY_COUNT(CLOCK_ROOT_LABELS);
    case CLOCK_MENU_ALARM: return (uint8_t)ARRAY_COUNT(CLOCK_ALARM_LABELS);
    case CLOCK_MENU_VISIBLE:
    case CLOCK_MENU_HIDDEN: return (uint8_t)ARRAY_COUNT(CLOCK_VISIBLE_LABELS);
    case CLOCK_MENU_TIME_FORMAT: return (uint8_t)ARRAY_COUNT(CLOCK_TIME_FORMAT_LABELS);
    case CLOCK_MENU_AM_PM: return (uint8_t)ARRAY_COUNT(CLOCK_AM_PM_LABELS);
    default: return 0u;
    }
}

static const clock_label_t *clock_menu_labels(clock_menu_kind_t kind) {
    switch (kind) {
    case CLOCK_MENU_ALARM: return CLOCK_ALARM_LABELS;
    case CLOCK_MENU_VISIBLE: return CLOCK_VISIBLE_LABELS;
    case CLOCK_MENU_HIDDEN: return CLOCK_HIDDEN_LABELS;
    case CLOCK_MENU_TIME_FORMAT: return CLOCK_TIME_FORMAT_LABELS;
    case CLOCK_MENU_AM_PM: return CLOCK_AM_PM_LABELS;
    case CLOCK_MENU_ROOT:
    default: return CLOCK_ROOT_LABELS;
    }
}

static const char *clock_menu_breadcrumb(clock_menu_kind_t kind, uint8_t selected, char *scratch, size_t scratch_cap) {
    if (kind == CLOCK_MENU_ALARM) {
        snprintf(scratch, scratch_cap, "8-1-%u", (unsigned)(selected + 1u));
    } else if (kind == CLOCK_MENU_VISIBLE || kind == CLOCK_MENU_HIDDEN) {
        snprintf(scratch, scratch_cap, "8-2-%u", (unsigned)(selected + 1u));
    } else {
        snprintf(scratch, scratch_cap, "%u", (unsigned)(selected + 1u));
    }
    return scratch;
}

static const char *clock_menu_softkey(clock_menu_kind_t kind) {
    /* "Select" has no v6.00 record for this app (kept literal); "OK" = SID 0x2e9. */
    return (kind == CLOCK_MENU_VISIBLE || kind == CLOCK_MENU_HIDDEN || kind == CLOCK_MENU_ROOT) ? "Select" : ts_or(0x2e9u, "OK");
}

static bool apply_clock_menu_label(app_t *app, const char *label, uint32_t now) {
    if (strcmp(label, "Show clock") == 0) {
        uint8_t time_set = 1u;
        clock_setting_u8(STORE_SETTING_CLOCK_TIME_SET, &time_set);
        if (!time_set) {
            /* Nothing to show until the clock exists -> jump straight into the
             * Time editor (no "needs to be set first" note here, owner-confirmed:
             * it's redundant when the user asked to show the clock). Completing
             * the Time->Date chain enables the display. */
            clock_editor_require_setup(app, now, false);
            return true;
        }
        store_setting_set_u8(STORE_SETTING_CLOCK_SHOW_STANDBY, 1u);
        update_standby_clock(app);
        clock_open_menu(app, CLOCK_MENU_VISIBLE, 0u);
        open_display_sid(app, 3u, 0x252u, "Clock\ndisplayed", APP_ROUTE_CLOCK_MENU, now);
        return true;
    }
    if (strcmp(label, "Hide clock") == 0) {
        store_setting_set_u8(STORE_SETTING_CLOCK_SHOW_STANDBY, 0u);
        update_standby_clock(app);
        clock_open_menu(app, CLOCK_MENU_HIDDEN, 0u);
        open_display_sid(app, 3u, 0x249u, "Clock\nhidden", APP_ROUTE_CLOCK_MENU, now);
        return true;
    }
    if (strcmp(label, "Set the time") == 0) {
        clock_editor_open(app, CLOCK_EDITOR_TIME, 0, 0u, now);
        return true;
    }
    if (strcmp(label, "Time format") == 0) {
        uint8_t format24 = 1u;
        clock_setting_u8(STORE_SETTING_CLOCK_FORMAT_24H, &format24);
        clock_open_menu(app, CLOCK_MENU_TIME_FORMAT, format24 ? 0u : 1u);
        return true;
    }
    if (strcmp(label, "24-hour") == 0 || strcmp(label, "12-hour") == 0) {
        store_status_t status = store_setting_set_u8(STORE_SETTING_CLOCK_FORMAT_24H,
                                                      strcmp(label, "24-hour") == 0 ? 1u : 0u);
        if (status != STORE_STATUS_OK && status != STORE_STATUS_NOT_READY) {
            open_display_sid(app, 0u, 0x3b3u, "Not\nsaved", APP_ROUTE_CLOCK_MENU, now);
            return true;
        }
        update_standby_clock(app);
        app->clock_menu_kind = (uint8_t)CLOCK_MENU_ROOT;
        app->clock_menu_selected = 1u;
        bool is24 = strcmp(label, "24-hour") == 0;
        open_display_sid(app,
                         3u,
                         is24 ? 0x244u : 0x242u,
                         is24 ? "24-hour\nclock\nselected" : "12-hour\nclock\nselected",
                         APP_ROUTE_CLOCK_MENU,
                         now);
        return true;
    }
    if (strcmp(label, "On") == 0) {
        clock_editor_open(app, CLOCK_EDITOR_ALARM, 0, 0u, now);
        return true;
    }
    if (strcmp(label, "Off") == 0) {
        clock_set_alarm(app, 0u, 0u, false);
        update_standby_clock(app);
        app->clock_menu_kind = (uint8_t)CLOCK_MENU_ROOT;
        app->clock_menu_selected = 0u;
        open_display_sid(app, 3u, 0x41u, "Alarm\noff", APP_ROUTE_CLOCK_MENU, now);
        return true;
    }
    if (strcmp(label, "am") == 0 || strcmp(label, "pm") == 0) {
        return clock_editor_commit_am_pm(app, label, now);
    }
    return false;
}

static void cancel_clock_menu(app_t *app, uint32_t now) {
    clock_menu_kind_t kind = (clock_menu_kind_t)app->clock_menu_kind;
    if (kind == CLOCK_MENU_TIME_FORMAT) {
        uint8_t visible = 1u;
        uint8_t time_set = 1u;
        clock_setting_u8(STORE_SETTING_CLOCK_SHOW_STANDBY, &visible);
        clock_setting_u8(STORE_SETTING_CLOCK_TIME_SET, &time_set);
        clock_open_menu(app, (visible && time_set) ? CLOCK_MENU_VISIBLE : CLOCK_MENU_HIDDEN, 2u);
        return;
    }
    if (kind == CLOCK_MENU_AM_PM) {
        clock_editor_open(app,
                          app->clock_pending_kind == CLOCK_EDITOR_ALARM ? CLOCK_EDITOR_ALARM : CLOCK_EDITOR_TIME,
                          app->clock_pending_time,
                          0u,
                          now);
        return;
    }
    if (kind == CLOCK_MENU_ALARM) {
        app->clock_menu_selected = 0u;
    }
    app->menu_index = 7u;
    app->route = APP_ROUTE_MAIN_MENU;
    app->dirty = true;
}

/* BOOT: RTC-lost flow (handset-modeled, IMG_3179.MOV — trigger is not
 * binary-pinned): cold boot with no valid stored clock runs Time: editor ->
 * "Time is set" -> Date: editor -> standby. Returns true when it took over. */
static bool clock_setting_u16(store_setting_key_t key, uint16_t *value) {
    return store_setting_get_u16(key, value) == STORE_STATUS_OK;
}

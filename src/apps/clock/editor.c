#include "apps/clock_app.h"
#include "clock_internal.h"

#include <stdio.h>
#include <string.h>

#include "audio/audio_levels.h"
#include "apps/clock_date_logic.h"
#include "apps/dialogs_app.h"
#include "apps/profiles_app.h"
#include "hal/rtc_alarm_hal.h"
#include "services/core1_services.h"
#include "services/input_keys.h"
#include "services/key_utils.h"
#include "services/strings.h"
#include "services/timebase.h"
#include "storage/store_service.h"
#include "ui/ui.h"

#define CLOCK_EDITOR_BLINK_MS 512u

static void play_clock_warning_tone(uint8_t index);
static rtc_datetime_write_status_t request_clock_time(uint8_t hour, uint8_t minute);
static rtc_datetime_write_status_t request_clock_date(uint8_t day, uint8_t month, uint16_t year);
static bool own_clock_datetime_request(app_t *app,
                                       rtc_datetime_write_status_t status,
                                       uint32_t now);
static void finish_clock_datetime_commit(app_t *app, uint32_t now, bool show_ui);
static void show_clock_datetime_failure(app_t *app, uint32_t now);
static void cancel_clock_editor(app_t *app, uint32_t now);
static bool commit_clock_editor(app_t *app, uint32_t now);
static void clock_editor_display_value(char *dst, size_t cap, const char *value, clock_editor_kind_t kind);
static void clock_editor_localized_display(char *dst, size_t cap, const char *value, clock_editor_kind_t kind);
static bool clock_editor_input_digit(app_t *app, uint8_t digit, uint32_t now);
static uint8_t clock_editor_digit_count(clock_editor_kind_t kind);
static uint8_t clock_editor_char_index(clock_editor_kind_t kind, uint8_t digit_index);
static bool clock_editor_candidate_allowed(clock_editor_kind_t kind,
                                           uint8_t digit_index,
                                           uint8_t digit,
                                           const char *value,
                                           bool format24);
static bool parse_clock_time_text(const char *value,
                                  bool hour12,
                                  uint8_t *out_hour,
                                  uint8_t *out_minute);
static bool field_range_possible(const char *digits,
                                 uint8_t start,
                                 uint8_t len,
                                 uint16_t minimum,
                                 uint16_t maximum);
static bool field_complete(const char *digits, uint8_t start, uint8_t len);
static uint16_t parse_fixed_digits(const char *digits, uint8_t start, uint8_t len);

void render_clock_editor(const app_t *app, framebuffer_t *fb) {
    fb_clear(fb, false);
    clock_editor_kind_t kind = (clock_editor_kind_t)app->clock_editor_kind;
    const font_t *title_font = asset_font(FONT_FS2);
    const font_t *value_font = asset_font(FONT_FS0);
    const char *title = kind == CLOCK_EDITOR_DATE
                            ? ts_or(0x08bu, "Date:")
                            : (kind == CLOCK_EDITOR_ALARM ? ts_or(0x046u, "Set alarm time:") : ts_or(0x24cu, "Time:"));
    fb_bitmap(fb, 14u, 0, 0, true, true);
    fb_text(fb, title_font, title, 0, 7, true, FB_WIDTH);
    fb_rect(fb, 0, 16, 84, 21, true);

    char disp[32];
    clock_editor_localized_display(disp, sizeof(disp), app->clock_editor_value, kind);
    int value_w = asset_text_width(value_font, disp);
    int tx = 2 + 80 - value_w;
    if (tx < 2) {
        tx = 2;
    }
    fb_text(fb, value_font, disp, tx, 24, true, 80 - (tx - 2));
    if (app->clock_editor_cursor_visible) {
        /* The cursor sits at digit slot cursor_digit == codepoint char_index in
         * the (localized) display; walk `disp` there so the prefix width is
         * measured in the language actually on screen, not the ASCII buffer. */
        uint8_t cp_target = clock_editor_char_index(kind, app->clock_editor_cursor_digit);
        char prefix[32];
        const char *p = disp;
        uint8_t ci = 0u;
        while (ci < cp_target && *p != '\0') {
            (void)asset_next_codepoint(&p);
            ci++;
        }
        size_t copy_len = (size_t)(p - disp);
        if (copy_len >= sizeof(prefix)) {
            copy_len = sizeof(prefix) - 1u;
        }
        memcpy(prefix, disp, copy_len);
        prefix[copy_len] = '\0';
        int cx = tx + asset_text_width(value_font, prefix) - 1;
        if (cx < 1) {
            cx = 1;
        } else if (cx > 82) {
            cx = 82;
        }
        fb_vline(fb, cx, 23, value_font->height + 1, true);
    }
    draw_softkey(fb, ts_or(0x2e9u, "OK"));
}

bool handle_clock_editor_key(app_t *app, uint16_t key, uint32_t now) {
    if (app->clock_datetime_commit_pending) {
        /* The exact editor contents are the transaction's user intent. Do not
         * let another key mutate them while the HAL is retrying that write. */
        return true;
    }
    if (key == KEY_NAVI) {
        return commit_clock_editor(app, now);
    }
    if (key == KEY_C) {
        cancel_clock_editor(app, now);
        return true;
    }
    if (key == KEY_UP) {
        uint8_t limit = clock_editor_digit_count((clock_editor_kind_t)app->clock_editor_kind);
        if (app->clock_editor_cursor_digit < limit) {
            app->clock_editor_cursor_digit++;
        }
        app->clock_editor_cursor_visible = true;
        app->clock_editor_last_cursor_ms = now;
        app->dirty = true;
        return true;
    }
    if (key == KEY_DOWN) {
        if (app->clock_editor_cursor_digit > 0u) {
            app->clock_editor_cursor_digit--;
        }
        app->clock_editor_cursor_visible = true;
        app->clock_editor_last_cursor_ms = now;
        app->dirty = true;
        return true;
    }
    char digit = key_digit(key);
    if (digit >= '0' && digit <= '9') {
        /* Rejected (impossible-prefix) digit beeps warning tone 05 in v6.00. */
        if (!clock_editor_input_digit(app, (uint8_t)(digit - '0'), now)) {
            play_clock_warning_tone(5u);
        }
        app->dirty = true;
        return true;
    }
    return true;
}

bool tick_clock_editor(app_t *app, uint32_t now) {
    if (time_diff_ms(now, app->clock_editor_last_cursor_ms + CLOCK_EDITOR_BLINK_MS) < 0) {
        return false;
    }
    app->clock_editor_cursor_visible = !app->clock_editor_cursor_visible;
    app->clock_editor_last_cursor_ms = now;
    return true;
}

bool poll_clock_datetime_commit(app_t *app, uint32_t now) {
    if (!app->clock_datetime_commit_pending) {
        return false;
    }
    rtc_datetime_write_status_t status = rtc_alarm_hal_datetime_write_status();
    if (status == RTC_DATETIME_WRITE_PENDING) {
        return false;
    }

    bool owns_ui = app->route == APP_ROUTE_CLOCK_EDITOR;
    app->clock_datetime_commit_pending = false;
    if (status == RTC_DATETIME_WRITE_COMMITTED) {
        finish_clock_datetime_commit(app, now, owns_ui);
    } else if (owns_ui) {
        show_clock_datetime_failure(app, now);
    }
    return true;
}

void clock_editor_open(app_t *app, clock_editor_kind_t kind, const char *initial, uint8_t cursor_digit, uint32_t now) {
    char value[11];
    if (initial == 0) {
        if (kind == CLOCK_EDITOR_DATE) {
            clock_format_date_preview(value, sizeof(value));
        } else if (kind == CLOCK_EDITOR_ALARM) {
            uint8_t hour = 7u;
            uint8_t minute = 30u;
            clock_setting_u8(STORE_SETTING_CLOCK_ALARM_HOUR, &hour);
            clock_setting_u8(STORE_SETTING_CLOCK_ALARM_MINUTE, &minute);
            snprintf(value, sizeof(value), "%02u:%02u", (unsigned)hour, (unsigned)minute);
        } else {
            uint8_t time_set = 1u;
            clock_setting_u8(STORE_SETTING_CLOCK_TIME_SET, &time_set);
            if (!time_set) {
                /* Not set yet (e.g. boot with the RTC battery pulled): start
                 * empty so the editor shows the hh:mm template, not an
                 * uninitialized 00:00 / garbage reading. */
                value[0] = '\0';
            } else {
                rtc_datetime_t dt;
                rtc_alarm_hal_get_datetime(&dt);
                snprintf(value, sizeof(value), "%02u:%02u", (unsigned)dt.hour, (unsigned)dt.minute);
            }
        }
    } else {
        copy_text(value, sizeof(value), initial);
    }
    uint8_t format24 = 1u;
    clock_setting_u8(STORE_SETTING_CLOCK_FORMAT_24H, &format24);
    if (!format24 && kind != CLOCK_EDITOR_DATE) {
        uint8_t hour = 0u;
        uint8_t minute = 0u;
        if (parse_clock_time_text(value, false, &hour, &minute)) {
            uint8_t display_hour = (uint8_t)(hour % 12u);
            if (display_hour == 0u) {
                display_hour = 12u;
            }
            snprintf(value, sizeof(value), "%02u:%02u", (unsigned)display_hour, (unsigned)minute);
        }
    }
    clock_editor_display_value(app->clock_editor_value, sizeof(app->clock_editor_value), value, kind);
    app->clock_editor_kind = (uint8_t)kind;
    app->clock_editor_cursor_digit = cursor_digit;
    if (app->clock_editor_cursor_digit > clock_editor_digit_count(kind)) {
        app->clock_editor_cursor_digit = clock_editor_digit_count(kind);
    }
    app->clock_editor_cursor_visible = true;
    app->clock_editor_last_cursor_ms = now;
    app->route = APP_ROUTE_CLOCK_EDITOR;
    app->dirty = true;
}

/* A clock action that needs the clock already set (Alarm, Date setting, Show
 * clock) was chosen while the clock is unset: send the user to set the time
 * first -- "otherwise there's no clock to show" (owner-confirmed). We open the
 * empty Time: editor, then show the "Clock needs to be set first" note (0x250,
 * record 2, auto-dismiss) whose return route drops back INTO that editor. */
void clock_editor_require_setup(app_t *app, uint32_t now, bool show_note) {
    /* Setting the clock from unset is a mandatory Time->Date chain (same state
     * as the boot RTC-lost setup): mark it pending so committing the time chains
     * into the Date editor, completing it enables the standby display, and
     * dismissing it leaves the clock unset with a "Time not set" note. Date and
     * Alarm show the "Clock needs to be set first" note en route; Show clock
     * jumps straight in (owner: the note is redundant there). */
    app->clock_setup_pending = true;
    clock_editor_open(app, CLOCK_EDITOR_TIME, 0, 0u, now); /* !time_set -> empty -> mask */
    if (show_note) {
        open_display_sid(app, 2u, 0x250u, "Clock\nneeds to\nbe set first", APP_ROUTE_CLOCK_EDITOR, now);
    }
}

static void play_clock_warning_tone(uint8_t index) {
    uint8_t level = profile_get_tone_setting(profile_active_index(), PROFILE_SETTING_WARNING_GAME_TONES);
    if (level == 255u) {
        return;
    }
    if (level > AUDIO_LEVEL_MAX) {
        level = AUDIO_LEVEL_MAX;
    }
    core1_post_command(CORE1_CMD_AUDIO_SYSTEM_TONE, audio_arg(index, level));
}

static rtc_datetime_write_status_t request_clock_time(uint8_t hour, uint8_t minute) {
    if (hour > 23u || minute > 59u) {
        return RTC_DATETIME_WRITE_FAILED;
    }
    rtc_datetime_t dt;
    rtc_alarm_hal_get_datetime(&dt);
    dt.hour = hour;
    dt.minute = minute;
    dt.second = 0u;
    return rtc_alarm_hal_set_datetime(&dt);
}

static rtc_datetime_write_status_t request_clock_date(uint8_t day, uint8_t month, uint16_t year) {
    if (!clock_date_valid(day, month, year)) {
        return RTC_DATETIME_WRITE_FAILED;
    }
    rtc_datetime_t dt;
    rtc_alarm_hal_get_datetime(&dt);
    dt.year = year;
    dt.month = month;
    dt.day = day;
    return rtc_alarm_hal_set_datetime(&dt);
}

static bool own_clock_datetime_request(app_t *app,
                                       rtc_datetime_write_status_t status,
                                       uint32_t now) {
    if (status == RTC_DATETIME_WRITE_COMMITTED) {
        finish_clock_datetime_commit(app, now, true);
        return true;
    }
    if (status == RTC_DATETIME_WRITE_PENDING) {
        app->clock_datetime_commit_pending = true;
        /* A 12-hour commit starts in the AM/PM menu. Return ownership to the
         * still-populated editor while the bounded HAL retry is in flight. */
        app->route = APP_ROUTE_CLOCK_EDITOR;
        app->dirty = true;
        return true;
    }
    show_clock_datetime_failure(app, now);
    return true;
}

static void finish_clock_datetime_commit(app_t *app, uint32_t now, bool show_ui) {
    rtc_datetime_t dt;
    rtc_alarm_hal_get_datetime(&dt);
    clock_editor_kind_t kind = (clock_editor_kind_t)app->clock_editor_kind;
    if (kind == CLOCK_EDITOR_DATE) {
        (void)store_setting_set_u8(STORE_SETTING_CLOCK_DATE_DAY, dt.day);
        (void)store_setting_set_u8(STORE_SETTING_CLOCK_DATE_MONTH, dt.month);
        (void)store_setting_set_u16(STORE_SETTING_CLOCK_DATE_YEAR, dt.year);
        if (app->clock_setup_pending) {
            /* The Time->Date chain becomes committed only after both writes have
             * reached the external RTC. */
            app->clock_setup_pending = false;
            (void)store_setting_set_u8(STORE_SETTING_CLOCK_TIME_SET, 1u);
            (void)store_setting_set_u8(STORE_SETTING_CLOCK_SHOW_STANDBY, 1u);
            update_standby_clock(app);
            if (show_ui) {
                open_display_sid(app, 3u, 0x89u, "Date\nis set", APP_ROUTE_STANDBY, now);
            }
            return;
        }
        if (show_ui) {
            app->clock_menu_kind = (uint8_t)CLOCK_MENU_ROOT;
            app->clock_menu_selected = 2u;
            open_display_sid(app, 3u, 0x89u, "Date\nis set", APP_ROUTE_CLOCK_MENU, now);
        }
        return;
    }

    /* Deliberately does NOT set CLOCK_TIME_SET: a clock is "set" only when the
     * mandatory Time->Date chain completes. Editing an already-set clock leaves
     * the existing flag intact. */
    (void)store_setting_set_u8(STORE_SETTING_CLOCK_HOUR, dt.hour);
    (void)store_setting_set_u8(STORE_SETTING_CLOCK_MINUTE, dt.minute);
    update_standby_clock(app);
    if (!show_ui) {
        return;
    }
    if (app->clock_setup_pending) {
        clock_editor_open(app, CLOCK_EDITOR_DATE, "", 0u, now);
        open_display_sid(app, 42u, 0x254u, "Time\nis set", APP_ROUTE_CLOCK_EDITOR, now);
        return;
    }
    app->clock_menu_kind = (uint8_t)CLOCK_MENU_ROOT;
    app->clock_menu_selected = 1u;
    open_display_sid(app, 42u, 0x254u, "Time\nis set", APP_ROUTE_CLOCK_MENU, now);
}

static void show_clock_datetime_failure(app_t *app, uint32_t now) {
    app->clock_datetime_commit_pending = false;
    open_display_sid(app, 2u, 0x3a9u, "Time up,\noperation\nnot done",
                     APP_ROUTE_CLOCK_EDITOR, now);
}

bool start_clock_boot_setup_if_needed(app_t *app, uint32_t now) {
    uint8_t time_set = 1u;
    clock_setting_u8(STORE_SETTING_CLOCK_TIME_SET, &time_set);
    if (time_set) {
        return false;
    }
    /* Only nag for the clock at boot if the user actually wants one on standby.
     * Dismissing this setup (C at the Time/Date step) turns CLOCK_SHOW_STANDBY off
     * (see cancel_clock_editor), so a later cold boot must NOT re-prompt -- else
     * unsetting "Show clock" has no effect and the phone nags every boot. A fresh
     * phone still prompts (SHOW_STANDBY defaults on); re-enabling "Show clock" from
     * the menu re-triggers the Time editor. A failed read stays pessimistic (prompt). */
    uint8_t show_standby = 1u;
    clock_setting_u8(STORE_SETTING_CLOCK_SHOW_STANDBY, &show_standby);
    if (!show_standby) {
        return false;
    }
    app->clock_setup_pending = true;
    clock_editor_open(app, CLOCK_EDITOR_TIME, 0, 0u, now);
    return true;
}

static void cancel_clock_editor(app_t *app, uint32_t now) {
    clock_editor_kind_t kind = (clock_editor_kind_t)app->clock_editor_kind;
    if (app->clock_setup_pending) {
        /* Dismissing the mandatory Time->Date setup (C at the Time OR Date step)
         * leaves the clock UNSET: an incomplete chain is not a valid clock, so
         * clear CLOCK_TIME_SET (a time entered before the abort is discarded),
         * turn the standby display OFF, and show the "Time not set" note (0x253,
         * record 2) before returning to standby. */
        app->clock_setup_pending = false;
        store_setting_set_u8(STORE_SETTING_CLOCK_TIME_SET, 0u);
        store_setting_set_u8(STORE_SETTING_CLOCK_SHOW_STANDBY, 0u);
        update_standby_clock(app);
        open_display_sid(app, 2u, 0x253u, "Time\nnot set", APP_ROUTE_STANDBY, now);
        return;
    }
    if (kind == CLOCK_EDITOR_ALARM) {
        clock_open_menu(app, CLOCK_MENU_ALARM, 0u);
        return;
    }
    if (kind == CLOCK_EDITOR_TIME) {
        uint8_t visible = 1u;
        uint8_t time_set = 1u;
        clock_setting_u8(STORE_SETTING_CLOCK_SHOW_STANDBY, &visible);
        clock_setting_u8(STORE_SETTING_CLOCK_TIME_SET, &time_set);
        clock_open_menu(app, (visible && time_set) ? CLOCK_MENU_VISIBLE : CLOCK_MENU_HIDDEN, 1u);
        return;
    }
    (void)now;
    app->menu_index = 7u;
    app->route = APP_ROUTE_MAIN_MENU;
    app->dirty = true;
}

static bool commit_clock_editor(app_t *app, uint32_t now) {
    clock_editor_kind_t kind = (clock_editor_kind_t)app->clock_editor_kind;
    if (kind == CLOCK_EDITOR_DATE) {
        uint8_t day = 0u;
        uint8_t month = 0u;
        uint16_t year = 0u;
        clock_date_resolution_t resolution = clock_date_resolve(
            app->clock_editor_value, app->clock_setup_pending, &day, &month, &year);
        if (resolution == CLOCK_DATE_INVALID) {
            open_display_sid(app, 2u, 0x8cu, "Invalid\ndate", APP_ROUTE_CLOCK_EDITOR, now);
            return true;
        }
        return own_clock_datetime_request(
            app, request_clock_date(day, month, year), now);
    }

    uint8_t format24 = 1u;
    clock_setting_u8(STORE_SETTING_CLOCK_FORMAT_24H, &format24);
    bool hour12 = format24 == 0u;
    uint8_t hour = 0u;
    uint8_t minute = 0u;
    if (!parse_clock_time_text(app->clock_editor_value, hour12, &hour, &minute)) {
        open_display_sid(app, 2u, 0x8du, "Invalid\ntime", APP_ROUTE_CLOCK_EDITOR, now);
        return true;
    }
    if (hour12) {
        app->clock_pending_kind = app->clock_editor_kind;
        snprintf(app->clock_pending_time, sizeof(app->clock_pending_time), "%02u:%02u", (unsigned)hour, (unsigned)minute);
        /* The am/pm picker preselects from the value being edited (stored
         * alarm time for the alarm editor, the running clock for Time:). */
        uint8_t reference_hour = 0u;
        if (kind == CLOCK_EDITOR_ALARM) {
            uint8_t alarm_hour = 7u;
            clock_setting_u8(STORE_SETTING_CLOCK_ALARM_HOUR, &alarm_hour);
            reference_hour = alarm_hour;
        } else {
            rtc_datetime_t dt;
            rtc_alarm_hal_get_datetime(&dt);
            reference_hour = dt.hour;
        }
        clock_open_menu(app, CLOCK_MENU_AM_PM, reference_hour >= 12u ? 1u : 0u);
        return true;
    }
    if (kind == CLOCK_EDITOR_ALARM) {
        clock_set_alarm(app, hour, minute, true);
        update_standby_clock(app);
        app->clock_menu_kind = (uint8_t)CLOCK_MENU_ROOT;
        app->clock_menu_selected = 0u;
        open_display_sid(app, 3u, 0x42u, "Alarm\non", APP_ROUTE_CLOCK_MENU, now);
        return true;
    }
    return own_clock_datetime_request(app, request_clock_time(hour, minute), now);
}

bool clock_editor_commit_am_pm(app_t *app, const char *marker, uint32_t now) {
    uint8_t hour = 0u;
    uint8_t minute = 0u;
    if (!parse_clock_time_text(app->clock_pending_time, true, &hour, &minute)) {
        open_display_sid(app, 2u, 0x8du, "Invalid\ntime", APP_ROUTE_MAIN_MENU, now);
        return true;
    }
    if (strcmp(marker, "am") == 0) {
        if (hour == 12u) {
            hour = 0u;
        }
    } else if (strcmp(marker, "pm") == 0) {
        if (hour != 12u) {
            hour = (uint8_t)(hour + 12u);
        }
    }
    if (app->clock_pending_kind == CLOCK_EDITOR_ALARM) {
        clock_set_alarm(app, hour, minute, true);
        update_standby_clock(app);
        app->clock_pending_time[0] = '\0';
        app->clock_pending_kind = 0u;
        app->clock_menu_kind = (uint8_t)CLOCK_MENU_ROOT;
        app->clock_menu_selected = 0u;
        open_display_sid(app, 3u, 0x42u, "Alarm\non", APP_ROUTE_CLOCK_MENU, now);
        return true;
    }
    rtc_datetime_write_status_t status = request_clock_time(hour, minute);
    app->clock_pending_time[0] = '\0';
    app->clock_pending_kind = 0u;
    return own_clock_datetime_request(app, status, now);
}

static void clock_editor_display_value(char *dst, size_t cap, const char *value, clock_editor_kind_t kind) {
    /* NOT localized despite v6.00 SIDs existing (hh:mm=0x3c9, dd.mm.yyyy=0x11f):
     * this is a positional template -- digits are written into fixed BYTE offsets
     * (TIME_POS/DATE_POS {0,1,3,4,...}) and parsed in fixed d-m-y order. Other
     * languages' masks shift those offsets (multibyte glyphs, e.g. GREE "ΩΩ:ΛΛ" /
     * RUSS "чч.мм") or reorder fields (HUNG/LITH year-first "yyyy.mm.dd"), which
     * would desync digit placement and corrupt UTF-8. Keep the ASCII template. */
    const char *mask = kind == CLOCK_EDITOR_DATE ? "dd.mm.yyyy" : "hh:mm";
    const uint8_t *positions = 0;
    static const uint8_t TIME_POS[] = {0u, 1u, 3u, 4u};
    static const uint8_t DATE_POS[] = {0u, 1u, 3u, 4u, 6u, 7u, 8u, 9u};
    uint8_t count = kind == CLOCK_EDITOR_DATE ? (uint8_t)ARRAY_COUNT(DATE_POS) : (uint8_t)ARRAY_COUNT(TIME_POS);
    positions = kind == CLOCK_EDITOR_DATE ? DATE_POS : TIME_POS;
    copy_text(dst, cap, mask);
    uint8_t digit_index = 0u;
    for (size_t i = 0; value != 0 && value[i] != '\0' && digit_index < count; i++) {
        if (value[i] >= '0' && value[i] <= '9') {
            dst[positions[digit_index++]] = value[i];
        }
    }
}

/* True if `mask` has the day-first template layout our slot map assumes:
 * a separator (single ASCII non-alphanumeric) at codepoint 2 (time) or at
 * codepoints 2 and 5 (date). Year-first locales (HUNG "éééé.hh.nn", LITH
 * "MMMM.mm.dd", ARAB "____/__/__") put separators elsewhere, so localizing
 * their DATE mask would drop digits into the wrong field -- those fall back to
 * the ASCII template. Every time mask is 2-sep-2, so time always qualifies. */
static bool clock_editor_template_standard(const char *mask, clock_editor_kind_t kind) {
    static const uint8_t TIME_SEP[] = {2u};
    static const uint8_t DATE_SEP[] = {2u, 5u};
    const uint8_t *seps = kind == CLOCK_EDITOR_DATE ? DATE_SEP : TIME_SEP;
    uint8_t nseps = kind == CLOCK_EDITOR_DATE ? 2u : 1u;
    const char *p = mask;
    uint8_t idx = 0u;
    uint8_t si = 0u;
    while (*p != '\0' && si < nseps) {
        const char *start = p;
        uint16_t cp = asset_next_codepoint(&p);
        if (idx == seps[si]) {
            bool single = (size_t)(p - start) == 1u;
            bool alnum = (cp >= '0' && cp <= '9') || (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z');
            if (!single || alnum) {
                return false;
            }
            si++;
        }
        idx++;
    }
    return si == nseps;
}

/* Render the editor value into the LOCALIZED template for display only: EN
 * "hh:mm"/"dd.mm.yyyy", RUSS "чч.мм"/"дд.мм.гггг", etc. (SIDs 0x3c9/0x11f).
 * The ASCII buffer above stays the source of truth for input/validation/commit
 * (fixed byte offsets); here we only skin it for the screen. Digits fill the
 * mask's digit slots by CODEPOINT position {0,1,3,4}(time)/{0,1,3,4,6,7,8,9}
 * (date) -- the layout every day-first locale shares -- so a multibyte mask
 * (Cyrillic is 2 bytes/glyph) never desyncs the digit placement. */
static void clock_editor_localized_display(char *dst, size_t cap, const char *value, clock_editor_kind_t kind) {
    static const uint8_t TIME_POS[] = {0u, 1u, 3u, 4u};
    static const uint8_t DATE_POS[] = {0u, 1u, 3u, 4u, 6u, 7u, 8u, 9u};
    const uint8_t *positions = kind == CLOCK_EDITOR_DATE ? DATE_POS : TIME_POS;
    uint8_t count = kind == CLOCK_EDITOR_DATE ? 8u : 4u;
    const char *ascii = kind == CLOCK_EDITOR_DATE ? "dd.mm.yyyy" : "hh:mm";
    const char *mask = kind == CLOCK_EDITOR_DATE ? ts_or(0x11fu, ascii) : ts_or(0x3c9u, ascii);
    if (!clock_editor_template_standard(mask, kind)) {
        mask = ascii; /* year-first locale: keep correct field order, unlocalized */
    }

    char digits[8];
    uint8_t n = 0u;
    for (size_t i = 0; value != 0 && value[i] != '\0' && n < count; i++) {
        if (value[i] >= '0' && value[i] <= '9') {
            digits[n++] = value[i];
        }
    }

    size_t out = 0u;
    uint8_t cp_index = 0u;
    const char *p = mask;
    while (*p != '\0' && out + 1u < cap) {
        const char *start = p;
        (void)asset_next_codepoint(&p); /* advances p past exactly one codepoint */
        int slot = -1;
        for (uint8_t k = 0; k < count; k++) {
            if (positions[k] == cp_index) {
                slot = (int)k;
                break;
            }
        }
        if (slot >= 0 && (uint8_t)slot < n) {
            dst[out++] = digits[slot];
        } else {
            for (const char *b = start; b < p && out + 1u < cap; b++) {
                dst[out++] = *b;
            }
        }
        cp_index++;
    }
    dst[out] = '\0';
}

static bool clock_editor_input_digit(app_t *app, uint8_t digit, uint32_t now) {
    clock_editor_kind_t kind = (clock_editor_kind_t)app->clock_editor_kind;
    uint8_t limit = clock_editor_digit_count(kind);
    if (app->clock_editor_cursor_digit >= limit) {
        return true;
    }
    uint8_t format24 = 1u;
    clock_setting_u8(STORE_SETTING_CLOCK_FORMAT_24H, &format24);
    if (!clock_editor_candidate_allowed(kind, app->clock_editor_cursor_digit, digit, app->clock_editor_value, format24 != 0u)) {
        app->clock_editor_cursor_visible = true;
        app->clock_editor_last_cursor_ms = now;
        return false;
    }
    app->clock_editor_value[clock_editor_char_index(kind, app->clock_editor_cursor_digit)] = (char)('0' + digit);
    app->clock_editor_cursor_digit++;
    app->clock_editor_cursor_visible = true;
    app->clock_editor_last_cursor_ms = now;
    return true;
}

static uint8_t clock_editor_digit_count(clock_editor_kind_t kind) {
    return kind == CLOCK_EDITOR_DATE ? 8u : 4u;
}

static uint8_t clock_editor_char_index(clock_editor_kind_t kind, uint8_t digit_index) {
    static const uint8_t TIME_POS[] = {0u, 1u, 3u, 4u};
    static const uint8_t DATE_POS[] = {0u, 1u, 3u, 4u, 6u, 7u, 8u, 9u};
    if (kind == CLOCK_EDITOR_DATE) {
        return digit_index >= ARRAY_COUNT(DATE_POS) ? 10u : DATE_POS[digit_index];
    }
    return digit_index >= ARRAY_COUNT(TIME_POS) ? 5u : TIME_POS[digit_index];
}

static bool clock_editor_candidate_allowed(clock_editor_kind_t kind, uint8_t digit_index, uint8_t digit, const char *value, bool format24) {
    char display[11];
    char digits[8];
    uint8_t count = clock_editor_digit_count(kind);
    clock_editor_display_value(display, sizeof(display), value, kind);
    display[clock_editor_char_index(kind, digit_index)] = (char)('0' + digit);
    memset(digits, 0, sizeof(digits));
    for (uint8_t i = 0; i < count; i++) {
        /* Positions after the cursor are stale pre-fill (the editor opens on the
         * current value), not yet re-entered -- treat them as blank so a fresh
         * leading digit is validated against wildcards, not the old trailing
         * digit. Otherwise typing '1' over "18:.." is judged as "18" > 12 and
         * rejected (12h), and likewise '2' over "18:.." as "28" > 23 (24h). */
        if (i > digit_index) {
            continue;
        }
        char ch = display[clock_editor_char_index(kind, i)];
        digits[i] = (ch >= '0' && ch <= '9') ? ch : 0;
    }
    if (kind != CLOCK_EDITOR_DATE) {
        uint8_t min_hour = format24 ? 0u : 1u;
        uint8_t max_hour = format24 ? 23u : 12u;
        if (!field_range_possible(digits, 0u, 2u, min_hour, max_hour) ||
            !field_range_possible(digits, 2u, 2u, 0u, 59u)) {
            return false;
        }
        if (field_complete(digits, 0u, 2u)) {
            uint16_t hour = parse_fixed_digits(digits, 0u, 2u);
            if (hour < min_hour || hour > max_hour) {
                return false;
            }
        }
        if (field_complete(digits, 2u, 2u)) {
            uint16_t minute = parse_fixed_digits(digits, 2u, 2u);
            if (minute > 59u) {
                return false;
            }
        }
        return true;
    }
    if (!field_range_possible(digits, 0u, 2u, 1u, 31u) ||
        !field_range_possible(digits, 2u, 2u, 1u, 12u) ||
        !field_range_possible(digits, 4u, 4u, 1999u, 2090u)) {
        return false;
    }
    if (field_complete(digits, 0u, 2u)) {
        uint16_t day = parse_fixed_digits(digits, 0u, 2u);
        if (day < 1u || day > 31u) {
            return false;
        }
    }
    if (field_complete(digits, 2u, 2u)) {
        uint16_t month = parse_fixed_digits(digits, 2u, 2u);
        if (month < 1u || month > 12u) {
            return false;
        }
    }
    if (field_complete(digits, 4u, 4u)) {
        uint16_t year = parse_fixed_digits(digits, 4u, 4u);
        if (year < 1999u || year > 2090u) {
            return false;
        }
        if (field_complete(digits, 0u, 2u) && field_complete(digits, 2u, 2u)) {
            return clock_date_valid((uint8_t)parse_fixed_digits(digits, 0u, 2u),
                                    (uint8_t)parse_fixed_digits(digits, 2u, 2u),
                                    year);
        }
    }
    return true;
}

static bool parse_clock_time_text(const char *value, bool hour12, uint8_t *out_hour, uint8_t *out_minute) {
    char digits[5];
    uint8_t len = 0u;
    for (size_t i = 0; value != 0 && value[i] != '\0'; i++) {
        if (value[i] >= '0' && value[i] <= '9') {
            if (len >= 4u) {
                return false;
            }
            digits[len++] = value[i];
        }
    }
    if (len != 4u) {
        return false;
    }
    digits[4] = '\0';
    uint8_t hour = (uint8_t)((digits[0] - '0') * 10 + (digits[1] - '0'));
    uint8_t minute = (uint8_t)((digits[2] - '0') * 10 + (digits[3] - '0'));
    if ((hour12 && (hour < 1u || hour > 12u)) || (!hour12 && hour > 23u) || minute > 59u) {
        return false;
    }
    *out_hour = hour;
    *out_minute = minute;
    return true;
}

static bool field_range_possible(const char *digits, uint8_t start, uint8_t len, uint16_t minimum, uint16_t maximum) {
    uint16_t low = 0u;
    uint16_t high = 0u;
    for (uint8_t i = 0; i < len; i++) {
        char ch = digits[start + i];
        low = (uint16_t)(low * 10u + (ch != 0 ? (uint16_t)(ch - '0') : 0u));
        high = (uint16_t)(high * 10u + (ch != 0 ? (uint16_t)(ch - '0') : 9u));
    }
    return low <= maximum && high >= minimum;
}

static bool field_complete(const char *digits, uint8_t start, uint8_t len) {
    for (uint8_t i = 0; i < len; i++) {
        if (digits[start + i] == 0) {
            return false;
        }
    }
    return true;
}

static uint16_t parse_fixed_digits(const char *digits, uint8_t start, uint8_t len) {
    uint16_t value = 0u;
    for (uint8_t i = 0; i < len; i++) {
        value = (uint16_t)(value * 10u + (uint16_t)(digits[start + i] - '0'));
    }
    return value;
}

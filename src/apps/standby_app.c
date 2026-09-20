#include "apps/standby_app.h"

#include <stdio.h>
#include <string.h>

#include "apps/call_divert_app.h"
#include "apps/call_register_app.h"
#include "apps/calls_app.h"
#include "apps/clock_app.h"
#include "apps/dialogs_app.h"
#include "apps/main_menu_app.h"
#include "apps/messages_app.h"
#include "apps/phonebook_app.h"
#include "apps/profiles_app.h"
#include "apps/service_codes_app.h"
#include "apps/tones_app.h"
#include "services/input_keys.h"
#include "services/key_utils.h"
#include "ui/status_chrome.h"
#include "storage/store_service.h"
#include "services/timebase.h"
#include "services/strings.h"

#define STANDBY_INPUT_MAX_CHARS 30u
#define STANDBY_DIAL_X 6
#define STANDBY_DIAL_Y 7
#define STANDBY_DIAL_W 72
#define STANDBY_DIAL_H 30
#define STANDBY_DIAL_BIG_X_OFFSET (-1)
#define STANDBY_DIAL_BIG_Y_OFFSET 1
#define STANDBY_DIAL_BIG_CHARS_PER_ROW 8u
#define STANDBY_DIAL_SMALL_CHARS_PER_ROW 11u
#define STANDBY_DIAL_SMALL_AFTER_CHARS 16u
#define STANDBY_STAR_CYCLE_MS 1200u
#define STANDBY_PROMPT_MS 2500u
#define STANDBY_KEYGUARD_SEQUENCE_MS 1800u
#define STANDBY_KEYGUARD_ACTIVE_RECORD_ID 0x03u
#define STANDBY_KEYGUARD_LOCKED_RECORD_ID 0x16u
#define STANDBY_KEYGUARD_HELP_RECORD_ID 0x17u
#define STANDBY_STATUS_X 6
#define STANDBY_STATUS_WIDTH 72
/* v6.00 compositor 0x002a59fc collects into a 0x1a-entry scratch array, applies
 * the width gate, then explicitly clamps the placed/drawn record count to six. */
#define STANDBY_STATUS_RECORD_LIMIT 6u

typedef struct {
    uint16_t raw_width;
    uint8_t gap_count;
    uint8_t record_count;
    bool full;
} standby_status_fit_t;

static bool is_star_cycle_char(char ch);
static void append_standby_char(app_t *app, char ch, uint32_t now_ms);
static void clear_standby_input(app_t *app);
static void open_keyguard_help(app_t *app, uint32_t now);
static void set_standby_message_for(app_t *app, const char *text, uint32_t now_ms, uint32_t duration_ms);
static void render_standby_input(const app_t *app, framebuffer_t *fb);
static bool standby_status_admit_bitmap(standby_status_fit_t *fit,
                                        bool active, int width);
static bool standby_status_admit_text(standby_status_fit_t *fit,
                                      bool active, int width);
static modem_message_waiting_category_t
standby_message_waiting_notice(const app_t *app);

bool handle_keyguard_key(app_t *app, uint16_t key, uint32_t now) {
    if (key == KEY_NAVI) {
        app->unlock_armed = true;
        app->unlock_armed_until_ms = now + STANDBY_KEYGUARD_SEQUENCE_MS;
        set_standby_message_for(app, ts_or(0x19cu, "Now\npress *"), now, STANDBY_KEYGUARD_SEQUENCE_MS);
        app->dirty = true;
        return true;
    }
    if (key == KEY_STAR &&
        app->unlock_armed &&
        time_diff_ms(now, app->unlock_armed_until_ms) <= 0) {
        app->keyguard_locked = false;
        app->unlock_armed = false;
        app->unlock_armed_until_ms = 0u;
        app->standby_message[0] = '\0';
        open_display_sid(app, STANDBY_KEYGUARD_ACTIVE_RECORD_ID, 0x19du, "Keypad\nactive", APP_ROUTE_STANDBY, now);
        app->dirty = true;
        return true;
    }
    if (app->unlock_armed &&
        time_diff_ms(now, app->unlock_armed_until_ms) > 0) {
        app->unlock_armed = false;
        app->unlock_armed_until_ms = 0u;
    }
    open_keyguard_help(app, now);
    return true;
}

void lock_standby_keyguard(app_t *app, uint32_t now) {
    clear_standby_input(app);
    app->menu_keyguard_armed = false;
    app->keyguard_locked = true;
    app->unlock_armed = false;
    app->unlock_armed_until_ms = 0u;
    app->standby_message[0] = '\0';
    open_display_sid(app, STANDBY_KEYGUARD_LOCKED_RECORD_ID, 0x19bu, "Keypad\nlocked", APP_ROUTE_STANDBY, now);
    app->dirty = true;
}

bool standby_clear_all_hold_event(const app_t *app,
                                  const input_event_t *event) {
    return app != NULL && event != NULL &&
        event->type == EVENT_KEY_HOLD && event->code == KEY_C &&
        app->input_len != 0u;
}

bool handle_standby_key(app_t *app, const input_event_t *event) {
    uint16_t key = event->code;
    uint32_t now = event->when_ms;

    if (event->type == EVENT_KEY_HOLD && key == KEY_1 &&
        app->input_len == 1u && app->input_text[0] == '1') {
        return start_standby_call(app, now);
    }
    if (event->type == EVENT_KEY_HOLD && app->input_len == 1u) {
        char digit = key_digit(key);
        if (digit >= '2' && digit <= '9' && app->input_text[0] == digit) {
            uint8_t enabled = 0u;
            store_setting_get_u8(STORE_SETTING_SETTINGS_SPEED_DIALLING, &enabled);
            if (enabled) {
                return start_standby_call(app, now);
            }
        }
    }

    if (app->clock_alarm_mode == 3u && app->input_len == 0u) {
        /* Snooze-active overlay: the left softkey reads Stop (KEY_NAVI); C also
         * dismisses. Both stop the alarm and show the "Snooze off" confirmation. */
        if (key == KEY_NAVI || key == KEY_C) {
            dismiss_clock_snooze(app, now);
            return true;
        }
    }

    if (app->missed_call_pending && app->input_len == 0u) {
        if (key == KEY_NAVI) {
            app->missed_call_pending = false;
            app->missed_call_pending_count = 0u;
            open_call_register_list(app, STORE_CALL_LIST_MISSED, APP_ROUTE_STANDBY, true, 0u, now);
            return true;
        }
        if (key == KEY_C) {
            app->missed_call_pending = false;
            app->missed_call_pending_count = 0u;
            app->dirty = true;
            return true;
        }
    }

    modem_message_waiting_category_t waiting_notice =
        standby_message_waiting_notice(app);
    if (waiting_notice != MODEM_MESSAGE_WAITING_ALL &&
        app->input_len == 0u && (key == KEY_NAVI || key == KEY_C)) {
        app->message_waiting_notice_mask &=
            (uint8_t)~modem_message_waiting_category_bit(waiting_notice);
        app->dirty = true;
        return true;
    }

    if (app->picture_notice_id != 0u && app->input_len == 0u) {
        if (key == KEY_NAVI) {
            (void)messages_picture_open_received(app, now);
            return true;
        }
        if (key == KEY_C) {
            if (messages_picture_open_received(app, now)) {
                messages_picture_ask_save(app);
            }
            return true;
        }
    }

    if (app->ringtone_notice_id != 0u && app->input_len == 0u &&
        (key == KEY_NAVI || key == KEY_C)) {
        open_received_tone(app);
        return true;
    }

    if (app->sms_received_pending && app->input_len == 0u) {
        if (key == KEY_NAVI) {
            app->sms_received_pending = false;
            app->sms_received_pending_count = 0u;
            open_messages_mailbox(app, MESSAGES_KIND_INBOX, now);
            return true;
        }
        if (key == KEY_C) {
            app->sms_received_pending = false;
            app->sms_received_pending_count = 0u;
            app->dirty = true;
            return true;
        }
    }

    if (key == KEY_NAVI) {
        if (app->input_len == 0u) {
            open_main_menu(app, now);
            return true;
        }
        if (app->input_action == APP_STANDBY_ACTION_SAVE) {
            copy_text(app->editor_draft_number, sizeof(app->editor_draft_number), app->input_text);
            clear_standby_input(app);
            open_editor(app, ts_or(0x283u, "Name:"), "", 16u, EDITOR_KIND_TEXT, EDITOR_CONTEXT_PHONEBOOK_ADD_NAME, true, now);
            return true;
        } else {
            return start_standby_call(app, now);
        }
    }

    if (key == KEY_UP || key == KEY_DOWN) {
        if (app->input_len > 0u) {
            app->input_action = app->input_action == APP_STANDBY_ACTION_CALL
                ? APP_STANDBY_ACTION_SAVE
                : APP_STANDBY_ACTION_CALL;
            app->dirty = true;
            return true;
        }
        if (key == KEY_UP) {
            open_call_register_list(app, STORE_CALL_LIST_DIALLED, APP_ROUTE_STANDBY, true, 0u, now);
        } else {
            start_phonebook_list(app, PHONEBOOK_LABEL_CALL, "1-1", PHONEBOOK_CONTEXT_STANDBY, 0u, now);
        }
        return true;
    }

    if (key == KEY_C && app->input_len > 0u) {
        if (standby_clear_all_hold_event(app, event)) {
            app->input_len = 0u;
        } else {
            app->input_len--;
        }
        app->input_text[app->input_len] = '\0';
        app->input_action = APP_STANDBY_ACTION_CALL;
        app->star_cycle_until_ms = 0u;
        app->dirty = true;
        return true;
    }

    if (event->type == EVENT_KEY_HOLD) {
        return true;
    }

    char digit = key_digit(key);
    if (digit != 0) {
        append_standby_char(app, digit, now);
        if (handle_standby_service_code(app, now)) {
            return true;
        }
        app->dirty = true;
        return true;
    }
    return false;
}

bool tick_standby(app_t *app, uint32_t now_ms) {
    bool changed = false;
    if (app->standby_message[0] != '\0' && time_diff_ms(now_ms, app->standby_message_until_ms) >= 0) {
        app->standby_message[0] = '\0';
        changed = true;
    }
    if (app->unlock_armed &&
        time_diff_ms(now_ms, app->unlock_armed_until_ms) >= 0) {
        app->unlock_armed = false;
        app->unlock_armed_until_ms = 0u;
        changed = true;
    }
    if (app->star_cycle_until_ms != 0u && time_diff_ms(now_ms, app->star_cycle_until_ms) >= 0) {
        app->star_cycle_until_ms = 0u;
    }
    return changed;
}

static bool is_star_cycle_char(char ch) {
    return ch == '*' || ch == '+' || ch == 'p' || ch == 'w';
}

static void append_standby_char(app_t *app, char ch, uint32_t now_ms) {
    if (ch == '*') {
        static const char STAR_SYMBOLS[] = {'*', '+', 'p', 'w'};
        bool cycle_active = app->input_len > 0u
            && app->star_cycle_until_ms != 0u
            && time_diff_ms(now_ms, app->star_cycle_until_ms) <= 0
            && is_star_cycle_char(app->input_text[app->input_len - 1u]);
        if (cycle_active) {
            app->star_cycle_index = (uint8_t)((app->star_cycle_index + 1u) % ARRAY_COUNT(STAR_SYMBOLS));
            app->input_text[app->input_len - 1u] = STAR_SYMBOLS[app->star_cycle_index];
        } else if (app->input_len < STANDBY_INPUT_MAX_CHARS) {
            app->star_cycle_index = 0u;
            app->input_text[app->input_len++] = STAR_SYMBOLS[0];
            app->input_text[app->input_len] = '\0';
        }
        app->star_cycle_until_ms = now_ms + STANDBY_STAR_CYCLE_MS;
        if (app->star_cycle_until_ms == 0u) {
            /* Reserve 0 as the "no cycle armed" sentinel: a wrap landing on 0
             * would read the armed window as inactive. Clamp to 1. */
            app->star_cycle_until_ms = 1u;
        }
    } else if (app->input_len < STANDBY_INPUT_MAX_CHARS) {
        app->input_text[app->input_len++] = ch;
        app->input_text[app->input_len] = '\0';
        app->star_cycle_until_ms = 0u;
    }
    app->input_action = APP_STANDBY_ACTION_CALL;
    app->standby_message[0] = '\0';
}

void set_standby_message(app_t *app, const char *text, uint32_t now_ms) {
    set_standby_message_for(app, text, now_ms, STANDBY_PROMPT_MS);
}

static void clear_standby_input(app_t *app) {
    app->input_len = 0u;
    app->input_text[0] = '\0';
    app->input_action = APP_STANDBY_ACTION_CALL;
    app->star_cycle_index = 0u;
    app->star_cycle_until_ms = 0u;
}

static void open_keyguard_help(app_t *app, uint32_t now) {
    open_display_sid(app, STANDBY_KEYGUARD_HELP_RECORD_ID, 0x240u, "Press\nUnlock\nand then *", APP_ROUTE_STANDBY, now);
    app->dirty = true;
}

static void set_standby_message_for(app_t *app, const char *text, uint32_t now_ms, uint32_t duration_ms) {
    copy_text(app->standby_message, sizeof(app->standby_message), text);
    app->standby_message[sizeof(app->standby_message) - 1u] = '\0';
    app->standby_message_until_ms = now_ms + duration_ms;
}

static bool standby_status_admit(standby_status_fit_t *fit, bool active,
                                 int width, bool text) {
    if (!active || fit->full || width <= 0) {
        return false;
    }

    uint16_t raw_width = (uint16_t)(fit->raw_width + (uint16_t)width +
                                    (text ? 3u : 0u));
    uint8_t gap_count = fit->gap_count;
    if (text) {
        if (gap_count < 3u) {
            gap_count++;
        }
    } else if (gap_count == 0u) {
        gap_count = 1u;
    }

    /* v6.00 FUN_002a59fc admits records by priority and stops at the first
     * record whose measured width no longer fits the 72-pixel status window. */
    if ((uint16_t)(raw_width - gap_count) > STANDBY_STATUS_WIDTH) {
        fit->full = true;
        return false;
    }

    fit->raw_width = raw_width;
    fit->gap_count = gap_count;
    fit->record_count++;
    if (fit->record_count >= STANDBY_STATUS_RECORD_LIMIT) {
        fit->full = true;
    }
    return true;
}

static bool standby_status_admit_bitmap(standby_status_fit_t *fit,
                                        bool active, int width) {
    return standby_status_admit(fit, active, width, false);
}

static bool standby_status_admit_text(standby_status_fit_t *fit,
                                      bool active, int width) {
    return standby_status_admit(fit, active, width, true);
}

/* Build a counted notice from a localized template. v6.00 uses "%N" in its
 * missed-call/SMS records and "%S" in fax/e-mail MWI records; both slots hold
 * the decimal count here. Token position varies by language. */
static void format_count_notice(char *dst, size_t cap, uint16_t sid,
                                const char *fallback, unsigned count) {
    const char *tmpl = ts(sid);
    if (tmpl == 0) {
        tmpl = fallback;
    }
    char num[6];
    snprintf(num, sizeof(num), "%u", count);
    size_t di = 0u;
    for (size_t i = 0u; tmpl[i] != '\0' && di + 1u < cap;) {
        if (tmpl[i] == '%' &&
            (tmpl[i + 1u] == 'N' || tmpl[i + 1u] == 'S')) {
            for (size_t k = 0u; num[k] != '\0' && di + 1u < cap; k++) {
                dst[di++] = num[k];
            }
            i += 2u;
        } else {
            dst[di++] = tmpl[i++];
        }
    }
    dst[di] = '\0';
}

static modem_message_waiting_category_t
standby_message_waiting_notice(const app_t *app) {
    /* v6.00 standby format slots 0x0a (fax) and 0x0b (e-mail) are selected in
     * ascending slot order. Both are text-only, silent, Exit-action notices. */
    static const modem_message_waiting_category_t ORDER[] = {
        MODEM_MESSAGE_WAITING_FAX,
        MODEM_MESSAGE_WAITING_EMAIL,
    };
    for (uint8_t i = 0u; i < ARRAY_COUNT(ORDER); i++) {
        uint8_t bit = modem_message_waiting_category_bit(ORDER[i]);
        if ((app->message_waiting_notice_mask & bit) != 0u) {
            return ORDER[i];
        }
    }
    return MODEM_MESSAGE_WAITING_ALL;
}

static void format_message_waiting_notice(const app_t *app,
                                          char *dst, size_t cap) {
    modem_message_waiting_category_t category =
        standby_message_waiting_notice(app);
    if (category == MODEM_MESSAGE_WAITING_ALL) {
        dst[0] = '\0';
        return;
    }
    unsigned count = app->message_waiting_notice_count[category];
    if (category == MODEM_MESSAGE_WAITING_FAX) {
        if (count <= 1u) {
            copy_text(dst, cap, ts_or(0x146u, "New fax\nmessage"));
        } else {
            format_count_notice(dst, cap, 0x147u,
                                "%S\nnew fax\nmessages", count);
        }
    } else if (count <= 1u) {
        copy_text(dst, cap, ts_or(0x139u, "New e-mail\nmessage"));
    } else {
        format_count_notice(dst, cap, 0x13au,
                            "%S\nnew e-mail\nmessages", count);
    }
}

void render_standby(const app_t *app, framebuffer_t *fb) {
    fb_clear(fb, false);
    draw_status(fb, app->signal_bars);

    const bitmap_t *lock = asset_bitmap(17u);
    const bitmap_t *voice_mail = asset_bitmap(26u);
    const bitmap_t *envelope = asset_bitmap(9u);
    const bitmap_t *fax_mail = asset_bitmap(1u);
    const bitmap_t *email_mail = asset_bitmap(2u);
    const bitmap_t *silent = asset_bitmap(22u);
    const bitmap_t *divert = asset_bitmap(18u);
    const bitmap_t *alarm = asset_bitmap(31u);
    int lock_w = lock != NULL ? lock->width : 16;
    int voice_mail_w = voice_mail != NULL ? voice_mail->width : 18;
    int envelope_w = envelope != NULL ? envelope->width : 15;
    int fax_mail_w = fax_mail != NULL ? fax_mail->width : 16;
    int email_mail_w = email_mail != NULL ? email_mail->width : 10;
    int silent_w = silent != NULL ? silent->width : 10;
    int divert_w = divert != NULL ? divert->width : 15;
    int alarm_w = alarm != NULL ? alarm->width : 7;
    const font_t *clock_font = asset_font(FONT_FS3);
    int clock_w = app->clock_text[0] != '\0'
        ? asset_text_width(clock_font, app->clock_text) : 0;
    if (clock_w > STANDBY_STATUS_WIDTH) {
        clock_w = STANDBY_STATUS_WIDTH;
    }

    /* Admission follows v6.00 priorities. Placement below follows its layer
     * order, which is deliberately different from priority order. */
    standby_status_fit_t fit = {0};
    bool voice_mail_waiting =
        app->message_waiting
            .category[MODEM_MESSAGE_WAITING_VOICE_LINE_1].active ||
        app->message_waiting
            .category[MODEM_MESSAGE_WAITING_VOICE_LINE_2].active;
    bool show_lock = standby_status_admit_bitmap(
        &fit, app->keyguard_locked, lock_w);                 /* P1, L2 */
    bool show_voice_mail = standby_status_admit_bitmap(
        &fit, voice_mail_waiting, voice_mail_w);             /* P3, L5 */
    bool show_envelope = standby_status_admit_bitmap(
        &fit, app->sms_unread_count > 0u, envelope_w);      /* P4, L6 */
    bool show_fax_mail = standby_status_admit_bitmap(
        &fit,
        app->message_waiting.category[MODEM_MESSAGE_WAITING_FAX].active,
        fax_mail_w);                                        /* P5, L7 */
    bool show_email_mail = standby_status_admit_bitmap(
        &fit,
        app->message_waiting.category[MODEM_MESSAGE_WAITING_EMAIL].active,
        email_mail_w);                                      /* P6, L8 */
    bool show_silent = standby_status_admit_bitmap(
        &fit, profile_active_is_silent(), silent_w);        /* P7, L9 */
    bool show_divert = standby_status_admit_bitmap(
        &fit, call_divert_any_active(app), divert_w);       /* P8, L4 */
    bool show_clock = standby_status_admit_text(
        &fit, app->clock_text[0] != '\0', clock_w);         /* P13, L1 */
    bool show_alarm = standby_status_admit_bitmap(
        &fit, app->clock_alarm_enabled, alarm_w);           /* P14, L2 */

    int status_left = STANDBY_STATUS_X;
    int status_right = STANDBY_STATUS_X + STANDBY_STATUS_WIDTH;
    if (show_clock) {
        status_right -= clock_w + 3;
        fb_text(fb, clock_font, app->clock_text, status_right + 3, 0,
                true, clock_w);
    }
    if (show_lock) {
        fb_bitmap(fb, 17u, status_left, 0, true, true);
        status_left += lock_w;
    }
    if (show_alarm) {
        status_right -= alarm_w;
        fb_bitmap(fb, 31u, status_right, 0, true, true);
    }
    if (show_divert) {
        fb_bitmap(fb, 18u, status_left, 0, true, true);
        status_left += divert_w;
    }
    if (show_voice_mail) {
        fb_bitmap(fb, 26u, status_left, 0, true, true);
        status_left += voice_mail_w;
    }
    if (show_envelope) {
        fb_bitmap(fb, 9u, status_left, 0, true, true);
        status_left += envelope_w;
    }
    if (show_fax_mail) {
        fb_bitmap(fb, 1u, status_left, 0, true, true);
        status_left += fax_mail_w;
    }
    if (show_email_mail) {
        fb_bitmap(fb, 2u, status_left, 0, true, true);
        status_left += email_mail_w;
    }
    if (show_silent) {
        fb_bitmap(fb, 22u, status_left, 0, true, true);
    }

    if (app->input_len > 0u) {
        render_standby_input(app, fb);
    } else if (app->clock_alarm_mode == 3u) {
        /* Snooze-active overlay: persists on standby until Stop or the snooze
         * re-fires (SID 0x044 "Snooze\nactive"). */
        draw_notice(fb, asset_font(FONT_FS2), ts_or(0x044u, "Snooze\nactive"));
    } else if (app->missed_call_pending && app->missed_call_pending_count > 0u) {
        char notice[48];
        uint8_t count = app->missed_call_pending_count;
        if (count <= 1u) {
            copy_text(notice, sizeof(notice), ts_or(0x216u, "1\nmissed\ncall"));
        } else {
            format_count_notice(notice, sizeof(notice), 0x096u, "%N\nmissed\ncalls", (unsigned)count);
        }
        draw_notice(fb, asset_font(FONT_FS2), notice);
    } else if (standby_message_waiting_notice(app) !=
               MODEM_MESSAGE_WAITING_ALL) {
        char notice[48];
        format_message_waiting_notice(app, notice, sizeof(notice));
        draw_notice(fb, asset_font(FONT_FS2), notice);
    } else if (app->picture_notice_id != 0u) {
        messages_picture_draw_notice(fb);
    } else if (app->ringtone_notice_id != 0u) {
        draw_notice(fb, asset_font(FONT_FS2), ts_or(0x222u, "Ringing\ntone\nreceived"));
    } else if (app->sms_received_pending) {
        char notice[48];
        uint8_t count = app->sms_received_pending_count == 0u ? 1u : app->sms_received_pending_count;
        if (count <= 1u) {
            copy_text(notice, sizeof(notice), ts_or(0x338u, "1\nmessage\nreceived"));
        } else {
            format_count_notice(notice, sizeof(notice), 0x339u, "%N\nmessages\nreceived", (unsigned)count);
        }
        /* The slot-0x03 envelope is drawn by the status strip above. */
        draw_notice(fb, asset_font(FONT_FS2), notice);
    } else if (app->standby_message[0] != '\0') {
        draw_notice(fb, asset_font(FONT_FS2), app->standby_message);
    } else if (app->sim_missing) {
        /* Original v6.00 central-status path: window 0x4c, FS2, x=6, w=72.
         * SID 0x2b2 is "Insert\nSIM card" (ENGL record index 632). */
        draw_notice(fb, asset_font(FONT_FS2),
                    ts_or(0x2b2u, "Insert\nSIM card"));
    } else if (app->signal_bars > 0u && app->operator_name[0] != '\0') {
        draw_center_text_box(fb, asset_font(FONT_FS2), app->operator_name, 6, 12, 72);
        const char *profile_label_text = profile_active_standby_label();
        if (profile_label_text[0] != '\0') {
            /* Real-3210 check 2026-07-10: the standby profile name ("Headset")
             * renders in the small BOLD face -- FS2, same as the operator name
             * above it -- not the plain small FS1. */
            draw_center_text_box(fb, asset_font(FONT_FS2), profile_label_text, 6, 22, 72);
        }
    }

    if (app->clock_alarm_mode == 3u) {
        /* Snooze-active Stop outranks the keyguard's Unlock: a locked phone must
         * still be able to stop the snooze (the auto-snooze drops to a locked
         * standby). Key handling in app_router mirrors this priority. */
        draw_softkey(fb, ts_or(0x2f1u, "Stop"));
    } else if (app->keyguard_locked) {
        draw_softkey(fb, ts_or(0x19au, "Unlock"));
    } else if (app->input_len > 0u) {
        draw_softkey(fb, app->input_action == APP_STANDBY_ACTION_SAVE ? "Save" : ts_or(0x2d9u, "Call"));
    } else if (app->missed_call_pending && app->missed_call_pending_count > 0u) {
        draw_softkey(fb, "List");
    } else if (standby_message_waiting_notice(app) !=
               MODEM_MESSAGE_WAITING_ALL) {
        draw_softkey(fb, "Exit");
    } else if (app->picture_notice_id != 0u) {
        draw_softkey(fb, "View");
    } else if (app->ringtone_notice_id != 0u) {
        draw_softkey(fb, "Options");
    } else if (app->sms_received_pending) {
        draw_softkey(fb, "Read");
    } else {
        draw_softkey(fb, "Menu");
    }

}

static void render_standby_input(const app_t *app, framebuffer_t *fb) {
    if (app->input_len <= STANDBY_DIAL_SMALL_AFTER_CHARS) {
        const font_t *font = asset_font(FONT_FS0);
        int bottom_y = STANDBY_DIAL_Y + STANDBY_DIAL_H - font->height - 1 + STANDBY_DIAL_BIG_Y_OFFSET;
        int top_y = bottom_y - font->height - 2;
        uint8_t split = app->input_len > STANDBY_DIAL_BIG_CHARS_PER_ROW
            ? (uint8_t)(app->input_len - STANDBY_DIAL_BIG_CHARS_PER_ROW)
            : 0u;
        if (split > 0u) {
            draw_string_slice_right(fb, font, app->input_text, 0u, split,
                                    STANDBY_DIAL_X + STANDBY_DIAL_BIG_X_OFFSET, top_y, STANDBY_DIAL_W);
        }
        draw_string_slice_right(fb, font, app->input_text, split, (uint8_t)(app->input_len - split),
                                STANDBY_DIAL_X + STANDBY_DIAL_BIG_X_OFFSET, bottom_y, STANDBY_DIAL_W);
        return;
    }

    const font_t *font = asset_font(FONT_FS2);
    int pitch = font->height + 1;
    int bottom_y = STANDBY_DIAL_Y + STANDBY_DIAL_H - font->height;
    uint8_t remaining = app->input_len;
    uint8_t starts[3];
    uint8_t lens[3];
    uint8_t rows = 0u;
    while (remaining > 0u && rows < 3u) {
        uint8_t len = remaining > STANDBY_DIAL_SMALL_CHARS_PER_ROW ? STANDBY_DIAL_SMALL_CHARS_PER_ROW : remaining;
        remaining = (uint8_t)(remaining - len);
        starts[2u - rows] = remaining;
        lens[2u - rows] = len;
        rows++;
    }
    uint8_t first = (uint8_t)(3u - rows);
    int y = bottom_y - (int)(rows - 1u) * pitch;
    for (uint8_t i = first; i < 3u; i++) {
        draw_string_slice_right(fb, font, app->input_text, starts[i], lens[i], STANDBY_DIAL_X, y, STANDBY_DIAL_W);
        y += pitch;
    }
}

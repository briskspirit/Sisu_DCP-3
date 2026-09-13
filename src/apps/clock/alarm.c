#include "apps/clock_app.h"
#include "clock_internal.h"

#include "audio/audio_levels.h"
#include "apps/clock_alarm_logic.h"
#include "apps/dialogs_app.h"
#include "apps/power_app.h"
#include "apps/powerup_app.h"
#include "apps/profiles_app.h"
#include "hal/rtc_alarm_hal.h"
#include "services/core1_services.h"
#include "services/board_diag_service.h"
#include "services/input_keys.h"
#include "services/modem_service.h"
#include "services/strings.h"
#include "services/timebase.h"
#include "ui/status_chrome.h"
#include "ui/ui.h"

#define CLOCK_ALARM_FRAME_MS 200u
#define CLOCK_ALARM_RUNTIME_MS 61680u
#define CLOCK_ALARM_SNOOZE_NOTICE_MS 1536u
#define CLOCK_ALARM_BACKLIGHT_BLINK_MS 512u
#define CLOCK_SNOOZE_MINUTES 5u
#define CLOCK_ALARM_TONE_INDEX 12u
#define CLOCK_ALARM_PROMPT_TONE_INDEX 10u
static const uint16_t CLOCK_ALARM_FRAMES[] = {211u, 212u, 213u, 214u, 215u, 216u, 217u, 218u, 219u};
static void open_clock_alarm_runtime(app_t *app, uint32_t now);
static void finish_clock_alarm(app_t *app, bool stop_alarm);
static void open_clock_alarm_activate_prompt(app_t *app, uint32_t now);
static void accept_clock_alarm_activate_prompt(app_t *app, uint32_t now);
static void decline_clock_alarm_activate_prompt(app_t *app, uint32_t now);
static void start_clock_alarm_tone(uint8_t mode);
static void stop_clock_alarm_tone(void);
void render_clock_alarm(const app_t *app, framebuffer_t *fb) {
    fb_clear(fb, false);
    const font_t *font = asset_font(FONT_FS2);
    if (app->clock_alarm_mode == 2u) {
        /* PPM SID 0x0043 "Activate\nphone\nfor calls?": wrap the localized text
         * to the window, left-aligned FS2 at 9px pitch. Wrapping (not just
         * splitting on '\n') is required because some languages, e.g. Greek,
         * store this as one long line with no breaks. */
        char lines[3][32];
        uint8_t count = wrap_text_lines_ex(font, ts_or(0x043u, "Activate\nphone\nfor calls?"),
                                           FB_WIDTH, (char *)lines, 32u, 3u);
        for (uint8_t i = 0; i < count; i++) {
            fb_text(fb, font, lines[i], 0, 7 + i * 9, true, FB_WIDTH);
        }
        draw_softkey(fb, ts_or(0x2f7u, "Yes"));
        return;
    }

    /* An alarm that woke the soft-off phone shows the normal battery chrome and
     * the aerial shell, but no registration bars: that path deliberately leaves
     * the modem powered down. The small alarm glyph matches hidden-clock standby. */
    draw_status(fb, app->clock_alarm_power_off_wake ? 0u : app->signal_bars);
    if (app->clock_alarm_power_off_wake) {
        draw_alarm_indicator(fb, 78);
    }
    /* Wrap the alarm/snooze text to the window-64 box; honors the ROM '\n'
     * breaks and wraps long localized lines instead of clipping them. */
    char lines[3][32];
    uint8_t count = wrap_text_lines_ex(font, app->clock_alarm_text, 72, (char *)lines, 32u, 3u);
    if (app->clock_alarm_mode == 3u) {
        /* Alarm descriptor 0x002dd2a4 mode-3 attr 0x45 <FS2><EV>: left-aligned,
         * line block vertically centered in the window-64 box x6 y7 w72 h30. */
        int block_h = count > 0u ? (int)(count - 1u) * 9 + (int)font->height : 0;
        int first_y = 7 + (30 - block_h) / 2;
        for (uint8_t i = 0; i < count; i++) {
            fb_text(fb, font, lines[i], 6, first_y + i * 9, true, 72);
        }
    } else if (app->backlight_force_on) {
        /* Mode 1: the "Alarm! hh:mm" line blinks with the 512 ms backlight phase
         * (ROM SID 0x0040 carries a leading blink-control byte); the bell
         * animation below keeps drawing steadily. */
        for (uint8_t i = 0; i < count; i++) {
            draw_center_text_box(fb, font, lines[i], 6, 7 + i * 9, 72);
        }
    }
    if (app->clock_alarm_mode != 3u) {
        uint8_t idx = app->clock_alarm_frame_index;
        if (idx >= ARRAY_COUNT(CLOCK_ALARM_FRAMES)) {
            idx = 0u;
        }
        const bitmap_t *bitmap = asset_bitmap(CLOCK_ALARM_FRAMES[idx]);
        if (bitmap != 0) {
            int x = 6 + (72 - bitmap->width) / 2;
            int ay = 7 + 30 - bitmap->height - 1;
            fb_bitmap(fb, CLOCK_ALARM_FRAMES[idx], x, ay, true, true);
        }
    }
    draw_softkey(fb, app->clock_alarm_mode == 3u ? ts_or(0x2f1u, "Stop") : ts_or(0x2f0u, "Snooze"));
}
/* Enter snooze: stop the alarm, reschedule 5 min (minute-quantized like the
 * ROM's +300s / whole-minute set), and show the "Snooze active" state. Shared
 * by the manual Snooze softkey and the 61.68 s auto-snooze (v6.00 timer 0x52 ->
 * snooze branch). A power-off wake snoozes and powers back down, as the manual
 * path did. */
static void enter_clock_alarm_snooze(app_t *app, uint32_t now) {
    stop_clock_alarm_tone();
    rtc_alarm_hal_snooze_minutes(CLOCK_SNOOZE_MINUTES);
    if (app->clock_alarm_power_off_wake) {
        app->clock_alarm_power_off_wake = false;
        app->backlight_force_active = false;
        app->backlight_force_on = false;
        power_off(app, now);
        return;
    }
    /* Snooze-active is a persistent STANDBY overlay (mode 3 = the flag): the
    * phone is usable, standby shows "Snooze active" with the left softkey as
    * Stop, until the snooze re-fires or the user dismisses it. */
    app->clock_alarm_mode = 3u;
    app->backlight_force_active = false;
    app->backlight_force_on = false;
    /* Only claim the screen when the alarm owns it. Auto-snooze UNDER a call
     * (the suppressed path) must leave the call's route intact -- the mode-3
     * "Snooze active" standby overlay surfaces when the call ends. */
    if (app->route == APP_ROUTE_CLOCK_ALARM) {
        app->route = APP_ROUTE_STANDBY;
    }
    update_standby_clock(app);
    app->dirty = true;
}

/* Dismiss the snooze (Stop / Clear from standby): stop the one-shot alarm and
 * confirm with the "Snooze off" checkmark note (SID 0x045). */
void dismiss_clock_snooze(app_t *app, uint32_t now) {
    stop_clock_alarm_tone();
    clock_set_alarm(app, 0u, 0u, false);
    app->clock_alarm_mode = 0u;
    app->clock_alarm_power_off_wake = false;
    app->backlight_force_active = false;
    app->backlight_force_on = false;
    open_display_sid(app, 3u, 0x045u, "Snooze\noff", APP_ROUTE_STANDBY, now);
}

bool handle_clock_alarm_key(app_t *app, uint16_t key, uint32_t now) {
    if (app->clock_alarm_mode == 2u) {
        if (key == KEY_NAVI) {
            accept_clock_alarm_activate_prompt(app, now);
            return true;
        }
        if (key == KEY_C) {
            decline_clock_alarm_activate_prompt(app, now);
            return true;
        }
        return true;
    }

    if (key == KEY_NAVI) {
        if (app->clock_alarm_mode == 3u) {
            finish_clock_alarm(app, true);
            return true;
        }
        enter_clock_alarm_snooze(app, now);
        return true;
    }
    if (key == KEY_C && app->clock_alarm_mode != 3u) {
        if (app->clock_alarm_power_off_wake) {
            clock_set_alarm(app, 0u, 0u, false);
            open_clock_alarm_activate_prompt(app, now);
        } else {
            finish_clock_alarm(app, true);
        }
        return true;
    }
    return true;
}
/* A call foreground (incoming, connected, or the in-call options menu) outranks
 * the alarm overlay in v6.00 -- while one owns the display the alarm is
 * suppressed, never hijacked. */
static bool alarm_route_is_call_surface(const app_t *app) {
    return app->route == APP_ROUTE_INCOMING_CALL ||
           app->route == APP_ROUTE_CALL ||
           app->route == APP_ROUTE_CALL_OPTIONS;
}

bool poll_clock_alarm(app_t *app, uint32_t now) {
    clock_alarm_inputs_t in = {
        .alarm_due = false,
        .route_owns_alarm = (app->route == APP_ROUTE_CLOCK_ALARM ||
                             app->route == APP_ROUTE_POWERUP),
        .route_power_off = (app->route == APP_ROUTE_POWER_OFF),
        .route_call_surface = alarm_route_is_call_surface(app),
        .alarm_mode = app->clock_alarm_mode,
        .suppress_timer_elapsed =
            app->clock_alarm_mode == CLOCK_ALARM_MODE_SUPPRESSED_BY_CALL &&
            time_diff_ms(now, app->clock_alarm_started_ms + CLOCK_ALARM_RUNTIME_MS) >= 0,
    };
    /* alarm_due() services the chip + advances the snooze re-fire, so poll it
     * only when the result can matter (matches the original early-return: the
     * alarm-owns-screen case is driven by tick_clock_alarm, not here). */
    if (!in.route_owns_alarm) {
        in.alarm_due = rtc_alarm_hal_alarm_due();
    }

    switch (clock_alarm_decide(&in)) {
    case CLOCK_ALARM_ACTION_POWERON:
        /* Alarm while "off": the codec was put in power-off standby, so bring
         * it back before the ring (blocking ~500 ms re-init; the original also
         * takes a moment to switch itself on before sounding the alarm). */
        core1_services_codec_init();
        app->clock_alarm_power_off_wake = true;
        start_powerup_no_logo(app, now);
        return true;
    case CLOCK_ALARM_ACTION_FIRE:
        open_clock_alarm_runtime(app, now);
        return true;
    case CLOCK_ALARM_ACTION_YIELD_TO_CALL:
        /* The alarm was ringing when an incoming call took the display. The
         * call's open path (call_session_reset -> stop_call_tone) already
         * stopped the alarm tone AND disarmed its vibra markers before the ring
         * started, so DO NOT touch audio here -- a generic AUDIO_STOP would kill
         * the ring that is now playing. This path only hands the DISPLAY back:
         * drop the alarm's backlight force. clock_alarm_started_ms is preserved
         * so the 61.68 s auto-snooze keeps running underneath, and alarm_due
         * stays latched so the alarm re-fires from standby when the call ends. */
        app->clock_alarm_mode = CLOCK_ALARM_MODE_SUPPRESSED_BY_CALL;
        app->backlight_force_active = false;
        app->backlight_force_on = false;
        app->dirty = true;
        return true;
    case CLOCK_ALARM_ACTION_AUTOSNOOZE_UNDER_CALL:
        /* Call outlasted the runtime window: snooze underneath it (route
         * preserved -- enter_clock_alarm_snooze only takes the screen when the
         * alarm owns it). "Snooze active" greets the user after the call. */
        enter_clock_alarm_snooze(app, now);
        return true;
    case CLOCK_ALARM_ACTION_NONE:
    default:
        return false;
    }
}

bool tick_clock_alarm(app_t *app, uint32_t now) {
    bool changed = false;
    if (app->clock_alarm_mode == 2u) {
        if (time_diff_ms(now, app->clock_alarm_started_ms + CLOCK_ALARM_RUNTIME_MS) >= 0) {
            decline_clock_alarm_activate_prompt(app, now);
            return true;
        }
        return false;
    }
    /* mode 3 (snooze-active) is a standby overlay, not an alarm-route screen, so
     * it never reaches this tick. */
    if (time_diff_ms(now, app->clock_alarm_last_frame_ms + CLOCK_ALARM_FRAME_MS) >= 0) {
        app->clock_alarm_frame_index = (uint8_t)((app->clock_alarm_frame_index + 1u) % ARRAY_COUNT(CLOCK_ALARM_FRAMES));
        app->clock_alarm_last_frame_ms = now;
        changed = true;
    }
    if (time_diff_ms(now, app->clock_alarm_last_backlight_ms + CLOCK_ALARM_BACKLIGHT_BLINK_MS) >= 0) {
        app->backlight_force_on = !app->backlight_force_on;
        app->clock_alarm_last_backlight_ms = now;
        changed = true;
    }
    if (time_diff_ms(now, app->clock_alarm_started_ms + CLOCK_ALARM_RUNTIME_MS) >= 0) {
        /* v6.00 alarm runtime timer 0x52 (61.68 s): after one ignored window the
         * alarm AUTO-SNOOZES (5 min) via the snooze branch. It does not loop a
         * secondary beep (tone 10) or disarm the alarm here. */
        enter_clock_alarm_snooze(app, now);
        return true;
    }
    return changed;
}
static void open_clock_alarm_runtime(app_t *app, uint32_t now) {
    char preview[24];   /* localized "Off" (0x3e) reaches 16 bytes; a time is shorter */
    uint8_t fired_hour = 0u;
    uint8_t fired_minute = 0u;
    if (rtc_alarm_hal_alarm_event_time(&fired_hour, &fired_minute)) {
        /* The RTC event owns the displayed time. In particular, a snooze fire
         * carries its temporary target even though the HAL has already restored
         * the configured one-shot compare for final dismissal. */
        format_clock_time(preview, sizeof(preview), fired_hour, fired_minute, false, true);
    } else {
        /* Defensive fallback for a synthetic/legacy event without time evidence. */
        clock_format_alarm_preview(preview, sizeof(preview));
    }
    /* v6.00 alarm-runtime text SID 0x0040 = "\x01<Alarm!>\n%U": localized
     * ("Alarm!"/"Подъем!"/...), a leading 0x01 blink-control byte (stripped here;
     * blink is applied in render), and "%U" = the alarm time. Some languages add
     * a '\n' after the control byte -- kept for the renderer's wrap. */
    const char *tmpl = ts_or(0x040u, "Alarm!\n%U");
    while (*tmpl != '\0' && (uint8_t)*tmpl < 0x20u && *tmpl != '\n') {
        tmpl++;
    }
    ui_format_named(app->clock_alarm_text, sizeof(app->clock_alarm_text),
                    tmpl, preview);
    app->clock_alarm_mode = 1u;
    app->clock_alarm_started_ms = now;
    app->clock_alarm_last_frame_ms = now;
    app->clock_alarm_last_backlight_ms = now;
    app->clock_alarm_frame_index = 0u;
    app->backlight_force_active = true;
    app->backlight_force_on = true;
    app->route = APP_ROUTE_CLOCK_ALARM;
    start_clock_alarm_tone(app->clock_alarm_mode);
    app->dirty = true;
}

static void finish_clock_alarm(app_t *app, bool stop_alarm) {
    stop_clock_alarm_tone();
    if (stop_alarm) {
        clock_set_alarm(app, 0u, 0u, false);
    } else {
        rtc_alarm_hal_clear_alarm_event();
    }
    app->backlight_force_active = false;
    app->backlight_force_on = false;
    app->clock_alarm_mode = 0u;
    app->clock_alarm_power_off_wake = false;
    app->route = APP_ROUTE_STANDBY;
    update_standby_clock(app);
    app->dirty = true;
}

static void open_clock_alarm_activate_prompt(app_t *app, uint32_t now) {
    stop_clock_alarm_tone();
    copy_text(app->clock_alarm_text, sizeof(app->clock_alarm_text), ts_or(0x043u, "Activate\nphone\nfor calls?"));
    app->clock_alarm_mode = 2u;
    app->clock_alarm_started_ms = now;
    app->clock_alarm_last_frame_ms = now;
    app->clock_alarm_last_backlight_ms = now;
    app->clock_alarm_frame_index = 0u;
    app->backlight_force_active = true;
    app->backlight_force_on = true;
    app->route = APP_ROUTE_CLOCK_ALARM;
    start_clock_alarm_tone(app->clock_alarm_mode);
    app->dirty = true;
}

static void accept_clock_alarm_activate_prompt(app_t *app, uint32_t now) {
    stop_clock_alarm_tone();
    rtc_alarm_hal_clear_alarm_event();
    app->clock_alarm_mode = 0u;
    app->clock_alarm_power_off_wake = false;
    app->backlight_force_active = false;
    app->backlight_force_on = false;
    if (!board_diag_battery_power_on_allowed()) {
        /* This is the same user-requested power-on as the physical key path.
         * Keep the alarm wake itself available, but do not launch the Telit
         * load when the bounded terminal-voltage admission refuses it. */
        power_off(app, now);
        return;
    }
    modem_service_power_on();
    start_powerup_no_logo(app, now);
}

static void decline_clock_alarm_activate_prompt(app_t *app, uint32_t now) {
    stop_clock_alarm_tone();
    rtc_alarm_hal_clear_alarm_event();
    app->clock_alarm_mode = 0u;
    app->clock_alarm_power_off_wake = false;
    app->backlight_force_active = false;
    app->backlight_force_on = false;
    power_off(app, now);
}

static void start_clock_alarm_tone(uint8_t mode) {
    if (mode == 1u) {
        /* M1: the alarm tone (index 12) carries 0a01/0afe vibra markers. The
         * profile gate is part of the same command as the stream start, so a
         * prior stream completion cannot disarm this alarm between two posts. */
        uint8_t vibra = profile_get_tone_setting(profile_active_index(), PROFILE_SETTING_VIBRATING_ALERT);
        core1_post_command(CORE1_CMD_AUDIO_SYSTEM_TONE_LOOP,
                           audio_arg_with_marker_vibra(CLOCK_ALARM_TONE_INDEX,
                                                       AUDIO_LEVEL_MAX,
                                                       vibra != 0u));
    } else if (mode == 2u) {
        core1_post_command(CORE1_CMD_AUDIO_SYSTEM_TONE, audio_arg(CLOCK_ALARM_PROMPT_TONE_INDEX, AUDIO_LEVEL_MAX));
    }
}

static void stop_clock_alarm_tone(void) {
    core1_post_command(CORE1_CMD_AUDIO_STOP, 0u);
}

#include "apps/powerup_app.h"

#include "app_internal.h"
#include "apps/clock_app.h"
#include "audio/audio_levels.h"
#include "services/board_diag_service.h"
#include "services/core1_services.h"
#include "ui/status_chrome.h"
#include "storage/store_service.h"
#include "services/timebase.h"

#define POWERUP_ALL_PIXELS_MS 1000u
#define POWERUP_BLANK_PLAIN_MS 550u
#define POWERUP_BLANK_BATTERY_PRE_MS 2100u
#define POWERUP_BOOT_DISPLAY_DURATION_MS 6168u
/* Boot logo = display-message record 0x29 -> animation 24 (frames 265-274,
 * mode "once"). Its record has frame_delay_ticks = 0 (raw 0x002dccfc, the
 * +0x06 high byte is 0x00; cf. anim 05 = 0x05 -> 200 ms via the framework's
 * ticks*40 ms model). BUT the per-frame delay for frame_delay_ticks = 0 is
 * NOT cleanly determined from the framework: the advance loop (0x002b3222)
 * loads the countdown from record +0x04 (start delay) and the common
 * frame-advance path has no confirmable countdown reload, so 0 could be
 * "advance every 40 ms tick" or a default sentinel — and the boot
 * direct-caller remains unpinned. Since the static RE is
 * inconclusive here and 200 ms matches the handset observation, keep 200 ms
 * rather than an unconfirmed 40 ms inference. The 6168 ms lifetime IS pinned. */
#define POWERUP_BOOT_FRAME_DELAY_MS 200u
#define POWERUP_BLANK_BATTERY_POST_MS 480u
#define POWERUP_WELCOME_NOTE_MS 5136u

#ifndef WELCOME_NOTE_TEXT
#define WELCOME_NOTE_TEXT ""
#endif

static const uint16_t POWERUP_BOOT_FRAMES[] = {265u, 266u, 267u, 268u, 269u, 270u, 271u, 272u, 273u, 274u};

static void start_powerup_common(app_t *app, uint32_t now_ms, bool skip_boot_logo);
static void enter_powerup_stage(app_t *app, app_powerup_stage_t stage, uint32_t now_ms);
static void play_powerup_key_click(const app_t *app);
static void finish_powerup(app_t *app, uint32_t now_ms);
static void render_powerup_welcome(const app_t *app, framebuffer_t *fb);

void start_powerup(app_t *app, uint32_t now_ms) {
    start_powerup_common(app, now_ms, false);
}

void start_powerup_no_logo(app_t *app, uint32_t now_ms) {
    start_powerup_common(app, now_ms, true);
}

static void start_powerup_common(app_t *app, uint32_t now_ms, bool skip_boot_logo) {
    app->route = APP_ROUTE_POWERUP;
    app->backlight_force_active = false;
    app->backlight_force_on = false;
    app->powerup_stage = APP_POWERUP_ALL_PIXELS;
    app->powerup_skip_boot_logo = skip_boot_logo;
    app->powerup_frame_index = 0u;
    app->powerup_deadline_ms = now_ms + POWERUP_ALL_PIXELS_MS;
    app->powerup_boot_started_ms = 0u;
    app->dirty = true;

    copy_text(app->welcome_note, sizeof(app->welcome_note), WELCOME_NOTE_TEXT);
    if (store_service_ready()) {
        store_setting_get_text(STORE_SETTING_SYSTEM_WELCOME_NOTE, app->welcome_note, sizeof(app->welcome_note));
    }
}

bool tick_powerup(app_t *app, uint32_t now_ms) {
    bool changed = false;
    while (app->route == APP_ROUTE_POWERUP && time_diff_ms(now_ms, app->powerup_deadline_ms) >= 0) {
        switch (app->powerup_stage) {
        case APP_POWERUP_ALL_PIXELS:
            enter_powerup_stage(app, APP_POWERUP_BLANK, now_ms);
            changed = true;
            break;
        case APP_POWERUP_BLANK:
            play_powerup_key_click(app);
            enter_powerup_stage(app, APP_POWERUP_BATTERY_PRE, now_ms);
            changed = true;
            break;
        case APP_POWERUP_BATTERY_PRE:
            if (app->powerup_skip_boot_logo) {
                enter_powerup_stage(app, APP_POWERUP_BATTERY_POST, now_ms);
            } else {
                enter_powerup_stage(app, app->welcome_note[0] != '\0' ? APP_POWERUP_WELCOME_NOTE : APP_POWERUP_BOOT_LOGO, now_ms);
            }
            changed = true;
            break;
        case APP_POWERUP_BOOT_LOGO: {
            uint32_t elapsed = now_ms - app->powerup_boot_started_ms;
            if (elapsed >= POWERUP_BOOT_DISPLAY_DURATION_MS) {
                enter_powerup_stage(app, APP_POWERUP_BATTERY_POST, now_ms);
                changed = true;
                break;
            }
            uint32_t desired = elapsed / POWERUP_BOOT_FRAME_DELAY_MS;
            if (desired >= ARRAY_COUNT(POWERUP_BOOT_FRAMES)) {
                desired = ARRAY_COUNT(POWERUP_BOOT_FRAMES) - 1u;
            }
            if ((uint8_t)desired != app->powerup_frame_index) {
                app->powerup_frame_index = (uint8_t)desired;
                changed = true;
            }
            if (app->powerup_frame_index >= ARRAY_COUNT(POWERUP_BOOT_FRAMES) - 1u) {
                app->powerup_deadline_ms = app->powerup_boot_started_ms + POWERUP_BOOT_DISPLAY_DURATION_MS;
            } else {
                app->powerup_deadline_ms = app->powerup_boot_started_ms
                    + ((uint32_t)app->powerup_frame_index + 1u) * POWERUP_BOOT_FRAME_DELAY_MS;
            }
            return changed;
        }
        case APP_POWERUP_WELCOME_NOTE:
            enter_powerup_stage(app, APP_POWERUP_BATTERY_POST, now_ms);
            changed = true;
            break;
        case APP_POWERUP_BATTERY_POST:
        case APP_POWERUP_DONE:
        default:
            finish_powerup(app, now_ms);
            changed = true;
            break;
        }
    }
    return changed;
}

void render_powerup(const app_t *app, framebuffer_t *fb) {
    if (app->powerup_stage == APP_POWERUP_ALL_PIXELS) {
        fb_clear(fb, true);
        return;
    }

    fb_clear(fb, false);
    if (app->powerup_stage == APP_POWERUP_BATTERY_PRE || app->powerup_stage == APP_POWERUP_BATTERY_POST) {
        draw_battery(fb, board_diag_battery_level_bars());
    } else if (app->powerup_stage == APP_POWERUP_BOOT_LOGO) {
        uint8_t index = app->powerup_frame_index;
        if (index >= ARRAY_COUNT(POWERUP_BOOT_FRAMES)) {
            index = (uint8_t)(ARRAY_COUNT(POWERUP_BOOT_FRAMES) - 1u);
        }
        fb_bitmap(fb, POWERUP_BOOT_FRAMES[index], 0, 0, true, false);
    } else if (app->powerup_stage == APP_POWERUP_WELCOME_NOTE) {
        render_powerup_welcome(app, fb);
    }
}

static void enter_powerup_stage(app_t *app, app_powerup_stage_t stage, uint32_t now_ms) {
    app->powerup_stage = stage;
    app->powerup_frame_index = 0u;
    switch (stage) {
    case APP_POWERUP_BLANK:
        app->powerup_deadline_ms = now_ms + POWERUP_BLANK_PLAIN_MS;
        break;
    case APP_POWERUP_BATTERY_PRE:
        app->powerup_deadline_ms = now_ms + POWERUP_BLANK_BATTERY_PRE_MS;
        break;
    case APP_POWERUP_BOOT_LOGO:
        app->powerup_boot_started_ms = now_ms;
        app->powerup_deadline_ms = now_ms + POWERUP_BOOT_FRAME_DELAY_MS;
        break;
    case APP_POWERUP_WELCOME_NOTE:
        app->powerup_deadline_ms = now_ms + POWERUP_WELCOME_NOTE_MS;
        break;
    case APP_POWERUP_BATTERY_POST:
        app->powerup_deadline_ms = now_ms + POWERUP_BLANK_BATTERY_POST_MS;
        break;
    case APP_POWERUP_ALL_PIXELS:
        app->powerup_deadline_ms = now_ms + POWERUP_ALL_PIXELS_MS;
        break;
    case APP_POWERUP_DONE:
    default:
        finish_powerup(app, now_ms);
        break;
    }
}

static void play_powerup_key_click(const app_t *app) {
    /* v6.00 sends the power key through the generic key-down event 0x1391;
     * its audio dispatcher stages tone 00/mode 02, the same 900 Hz click as C.
     * Handset observation places the deferred click after the blank screen and
     * immediately before the battery compositor appears. Alarm/no-logo wakes
     * have no physical power-key event, so they remain silent here. */
    if (app->powerup_skip_boot_logo) {
        return;
    }

    uint8_t setting = 1u;
    (void)store_setting_get_u8(STORE_SETTING_PROFILE_KEYPAD_TONES, &setting);
    uint8_t level = audio_level_from_keypad_tones(setting);
    if (level != AUDIO_LEVEL_SILENT) {
        core1_post_command(CORE1_CMD_AUDIO_CLICK, audio_arg(0u, level));
    }
}

static void finish_powerup(app_t *app, uint32_t now_ms) {
    app->route = APP_ROUTE_STANDBY;
    app->powerup_stage = APP_POWERUP_DONE;
    app->powerup_skip_boot_logo = false;
    app->powerup_frame_index = 0u;
    app->powerup_deadline_ms = 0u;
    app->powerup_boot_started_ms = 0u;
    /* The power-key activity deadline starts before the long boot animation.
     * Start a fresh full lights interval when the first interactive screen
     * appears, including the RTC-lost Time editor path. */
    app->backlight_activity_pending = true;
    app->backlight_activity_ms = now_ms;
    /* RTC-lost flow: with no valid stored clock, boot runs the Time:/Date:
     * editors before reaching standby (handset-modeled, see CHANGELOG). */
    (void)start_clock_boot_setup_if_needed(app, now_ms);
}

static void render_powerup_welcome(const app_t *app, framebuffer_t *fb) {
    const font_t *font = asset_font(FONT_FS0);
    draw_text_block(fb, font, app->welcome_note, 1, 7, 83, 13, 2u);
}

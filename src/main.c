#include "app.h"
#include "app_internal.h"
#include "app_status_runtime.h"
#include "audio/audio_levels.h"
#include "audio/audio_service.h"
#include "services/backlight_calibration.h"
#include "services/backlight_service.h"
#include "services/board_diag_service.h"
#include "hal/board.h"
#include "hal/battery_hal.h"
#include "services/core1_services.h"
#include "services/debug_console.h"
#include "services/event_queue.h"
#include "services/feature_gates.h"
#include "ui/framebuffer.h"
#include "hal/keypad.h"
#include "hal/tca8418_hal.h"
#include "hal/lcd_pcd8544.h"
#include "hal/accessory_hal.h"
#include "apps/calls_app.h"
#include "apps/power_app.h"
#include "apps/profiles_app.h"
#include "apps/standby_app.h"
#include "services/lcd_calibration.h"
#include "services/log.h"
#include "hal/modem_uart_hal.h"
#include "services/modem_service.h"
#include "services/netmon_diag_service.h"
#include "hal/power_button_hal.h"
#include "hal/power_sleep_hal.h"
#include "hal/rtc_alarm_hal.h"
#include "services/power_sleep.h"
#include "services/runtime_watchdog.h"
#include "services/shared_3v8_service.h"
#include "services/shared_irq_service.h"
#include "services/standby_sleep.h"
#include "services/stack_monitor.h"
#include "storage/store_service.h"
#include "services/phonebook_service.h"
#include "services/strings.h"
#include "services/timebase.h"
#include "services/usb_service.h"
#include "sisu_build_config.h"
#if SISU_STORAGE_POWERCUT_BENCH
#include "diag/storage_powercut_bench.h"
#endif
#include "pico/stdlib.h"

#include <string.h>

#ifndef PROFILE_BOOT
#define PROFILE_BOOT 0
#endif

#define STORE_AUDIO_GUARD_MS STORE_COMMIT_AUDIO_GUARD_MS

#define KEYGUARD_LOCK_TONE_INDEX 10u

#if PROFILE_BOOT
static void run_profile_once(lcd_pcd8544_t *lcd, const app_t *app, framebuffer_t *fb);
#endif
/* BOOTSEL gate VBUS probe: Rev B2 has direct active-high GP28 sensing. */
static bool bootsel_vbus_probe(bool *vbus_high) {
    *vbus_high = power_sleep_hal_service_vbus_raw() ||
                 usb_service_connected();
    return true;
}

static bool should_wake_backlight_for_event(const app_t *app, const input_event_t *event);
static bool keypad_audio_command_for_event(const app_t *app,
                                           const input_event_t *event,
                                           core1_cmd_t *out_cmd,
                                           uint16_t *out_arg);
static bool should_play_dtmf_for_key(const app_t *app, uint16_t key);
static bool is_menu_keyguard_lock_key(const app_t *app, uint16_t key, uint32_t now);
static bool is_keyguard_unlock_star_key(const app_t *app, uint16_t key, uint32_t now);
static bool is_dtmf_key(uint16_t key);
static bool is_composer_note_key(uint16_t key);
static uint8_t current_keypad_audio_level(void);
static bool lights_setting_always_on(void);
static void sync_display_power(app_t *app, lcd_pcd8544_t *lcd,
                               framebuffer_t *fb);

static lcd_pcd8544_t s_lcd;
static framebuffer_t s_fb;
static keypad_t s_keypad;
static power_button_t s_power_button;
static event_queue_t s_queue;
static app_t s_app;
static uint16_t s_active_dtmf_keys;

int main(void) {
    /* Capture a prior runtime timeout before disarming any inherited ROM,
     * reflash, or application watchdog deadline. */
    runtime_watchdog_boot_capture();
    stack_monitor_core0_init();
    board_set_system_clock();
    /* FIRST: consume the POWMAN wake evidence (dormant wake vs cold boot)
     * before anything else can disturb it. */
    power_sleep_boot_capture();
    board_init();
    shared_3v8_service_init();
    power_sleep_hal_init(0u);
    /* The service image lets GP28 own the dynamic USB lifecycle. The release
     * image always leaves the controller/PHY parked; GP28 is sampled only by
     * the recovery gate below. */
    usb_service_init(power_sleep_hal_service_vbus_raw(), 0u);
    power_wake_cause_t wake_cause = power_sleep_wake_cause();
    bool release_bootsel_hold_consumed = false;
    uint32_t release_bootsel_released_ms = 0u;
#if SISU_RELEASE_BUILD
    /* Release firmware deliberately ignores service VBUS as a normal wake
     * source. Its recovery gesture is therefore: connect USB first, then hold
     * Power. GP7 wakes the RP; this boot samples GP28 and applies the same 5 s
     * BOOTSEL gate used on a cold boot. UNKNOWN preserves the gesture if the
     * transient POWMAN source evidence was lost but the button is still low. */
    bool release_bootsel_wake =
        (wake_cause == POWER_WAKE_BUTTON ||
         wake_cause == POWER_WAKE_DORMANT_UNKNOWN) &&
        power_sleep_hal_service_vbus_raw() &&
        power_sleep_hal_power_button_asserted();
#else
    bool release_bootsel_wake = false;
#endif
    if (wake_cause == POWER_WAKE_COLD || release_bootsel_wake) {
        /* Direct GP28 is authoritative; an already enumerated host is a
         * conservative second observation in the CDC-enabled service image. */
        board_enter_bootsel_if_power_held(bootsel_vbus_probe);
        if (release_bootsel_wake) {
            /* A release before the 5 s BOOTSEL threshold may still be a valid
             * ordinary 1.2 s power-on hold. Capture the release time now; do
             * not wait until app_init and accidentally credit startup work. */
            release_bootsel_hold_consumed = true;
            release_bootsel_released_ms = time_ms();
        }
    }

    log_set_level(LOG_LEVEL_INFO);
    LOGI("boot", "Sisu DCP-3 firmware");
    runtime_watchdog_boot_evidence_t watchdog_evidence;
    runtime_watchdog_get_boot_evidence(&watchdog_evidence);
    if (watchdog_evidence.timeout_reset) {
        LOGW("watchdog", "runtime timeout: phase=%s(%u) stamp=%08lx",
             runtime_watchdog_phase_name(watchdog_evidence.phase),
             (unsigned)watchdog_evidence.phase,
             (unsigned long)watchdog_evidence.raw_phase_stamp);
    }
    debug_console_init();
    debug_console_set_phone_power_on(power_on);
    if (wake_cause == POWER_WAKE_COLD) {
        /* Dormant wakes skip the blocking ~500 ms codec init: the codec sits
         * in power-off standby and every wake consumer re-inits on demand
         * (power_on, the alarm-ring path). This also minimizes the time
         * spent awake after a non-power-on shared-IRQ event. */
        if (!core1_services_codec_init()) {
            LOGW("boot", "NAU88C22 codec init failed; continuing without codec audio");
        }
    }

    event_queue_init(&s_queue);
    keypad_init(&s_keypad);
    accessory_hal_init(); /* after keypad_init: HEAD_INT is on the TCA8418 */
    power_button_init(&s_power_button);
    if (wake_cause != POWER_WAKE_COLD && power_button_scan_raw()) {
        /* The press that woke us predates this boot: credit it so the 1.2 s
         * power-on hold measures from the physical press. The raw level also
         * covers a proven dormant wake whose transient source decode was lost. */
        power_button_seed_held(&s_power_button, 0u);
    }
    backlight_service_init(time_ms());
    rtc_alarm_hal_init();
    power_sleep_boot_probe(); /* AF/TF snapshot BEFORE app_init re-arms the alarm */
    lcd_init(&s_lcd);
    modem_service_init();
    core1_services_start(modem_service_voice_transport_available());
#if SISU_STORAGE_POWERCUT_BENCH
    storage_powercut_bench_run(&s_lcd, &s_fb);
#endif
    store_service_init();
    phonebook_service_init();
    if (!backlight_calibration_service_init()) {
        LOGW("boot", "backlight calibration apply failed; using 100%%");
    }
    if (!lcd_calibration_service_init()) {
        LOGW("boot", "LCD calibration apply failed; keeping controller fallback");
    }
    {
        /* Drive UI localization from the stored language id before the first
         * render (SET.1). Falls back to English if unset/unknown. */
        uint8_t lang_id = 1u;
        (void)store_setting_get_u8(STORE_SETTING_SYSTEM_LANGUAGE, &lang_id);
        strings_set_language(lang_id);
    }
    feature_gates_reset_defaults();
    app_init(&s_app);
#if SISU_RELEASE_BUILD
    if (release_bootsel_hold_consumed &&
        release_bootsel_released_ms >= POWER_BUTTON_POWER_ON_HOLD_MS) {
        /* board_enter_bootsel_if_power_held consumed the physical hold and the
         * button was released before keypad scanning began. Preserve the
         * normal power-on gesture instead of swallowing it. */
        (void)power_on(&s_app, release_bootsel_released_ms);
    }
#else
    (void)release_bootsel_hold_consumed;
    (void)release_bootsel_released_ms;
#endif
    netmon_diag_service_init(time_ms());
    power_sleep_boot_finish(); /* alarm-fire re-inject AFTER the app re-arm */
    /* All three GP42 sources are initialized now. The GPIO ISR only latched
     * any earlier falling edge; source I/O begins here on core 0. */
    shared_irq_service_init(time_ms());
    standby_sleep_init(time_ms64());

#if PROFILE_BOOT
    run_profile_once(&s_lcd, &s_app, &s_fb);
#endif

    sync_display_power(&s_app, &s_lcd, &s_fb);
    runtime_watchdog_start();

    absolute_time_t next_tick = delayed_by_ms(get_absolute_time(), SYSTEM_TICK_MS);
    uint16_t prev_raw_keys = 0u;
    bool prev_raw_power = false;
#if SISU_RELEASE_BUILD
    bool runtime_bootsel_hold_active = false;
    uint32_t runtime_bootsel_hold_since_ms = 0u;
#endif
    while (true) {
        runtime_watchdog_note_phase(RUNTIME_WATCHDOG_PHASE_MAIN_LOOP);
        absolute_time_t loop_started = get_absolute_time();
        uint32_t now = time_ms();
        power_sleep_hal_poll(now);
#if SISU_RELEASE_BUILD
        bool service_vbus = false;
#else
        bool service_vbus = power_sleep_hal_service_vbus_present();
        if (service_vbus) {
            /* USB enumeration is not reliable at the 6 MHz quiet operating
             * point. Restore full speed before bringing its clock/PHY back. */
            board_exit_xosc_lowpower();
        }
#endif
        usb_service_poll(service_vbus, now);
        shared_irq_service_poll(now);
        uint16_t raw_keys = keypad_scan_raw();
        bool raw_power = power_button_scan_raw();
#if SISU_RELEASE_BUILD
        /* A just-powered-off phone spends about two seconds in rollbackable
         * soft-off before P1.7. Preserve the USB+Power recovery gesture in
         * that window too. The tracker may survive the ordinary 1.2 s event
         * briefly powering the phone on; releasing there keeps that normal
         * power-on, while a continuous 5 s hold transfers to BOOTSEL. Never
         * start this gesture from an already-on route. */
        bool runtime_bootsel_vbus = power_sleep_hal_service_vbus_raw();
        if (!runtime_bootsel_hold_active) {
            if (s_app.route == APP_ROUTE_POWER_OFF && raw_power &&
                runtime_bootsel_vbus) {
                runtime_bootsel_hold_active = true;
                runtime_bootsel_hold_since_ms = now;
            }
        } else if (!raw_power || !runtime_bootsel_vbus) {
            runtime_bootsel_hold_active = false;
        } else if (time_diff_ms(
                       now,
                       runtime_bootsel_hold_since_ms +
                           POWER_BUTTON_BOOTSEL_HOLD_MS) >= 0) {
            board_reset_to_bootsel();
        }
#endif
        /* Defer flash commits on input ACTIVITY (key/power state changes), not
         * while a key is merely held. Re-arming the guard every tick a raw key
         * was down starved commits indefinitely, so a held / auto-repeating /
         * stuck key never let a pending setting persist. Edge-detecting
         * keeps the ~180 ms post-activity guard around press/release bursts while
         * letting commits proceed during a steady hold. */
        if (raw_keys != prev_raw_keys || raw_power != prev_raw_power) {
            store_service_defer_commits_until(now + STORE_AUDIO_GUARD_MS);
        }
        prev_raw_keys = raw_keys;
        prev_raw_power = raw_power;
        keypad_feed(&s_keypad, raw_keys, &s_queue, now);
        power_button_feed(&s_power_button, raw_power, &s_queue, now);

        input_event_t event;
        while (event_queue_pop(&s_queue, &event)) {
            if ((event.type == EVENT_KEY_DOWN || event.type == EVENT_KEY_HOLD) &&
                should_wake_backlight_for_event(&s_app, &event)) {
                backlight_service_notify_activity(event.when_ms);
                store_service_defer_commits_until(event.when_ms + STORE_AUDIO_GUARD_MS);
            }
            /* KEY_POWER is synthetic and overlaps matrix-key bits. It still
             * follows the ordinary key-down audio policy (v6.00 uses the same
             * click payload as C), but it must never enter the held-DTMF mask. */
            core1_cmd_t audio_cmd = CORE1_CMD_NONE;
            uint16_t audio_arg_value = 0u;
            if (keypad_audio_command_for_event(&s_app, &event, &audio_cmd,
                                               &audio_arg_value)) {
                core1_post_command(audio_cmd, audio_arg_value);
                if (audio_cmd == CORE1_CMD_AUDIO_DTMF &&
                    event.code != KEY_POWER) {
                    s_active_dtmf_keys |= event.code;
                }
            } else if (event.type == EVENT_KEY_UP &&
                       event.code != KEY_POWER) {
                if ((s_active_dtmf_keys & event.code) != 0u) {
                    s_active_dtmf_keys &= (uint16_t)~event.code;
                    core1_post_command(
                        CORE1_CMD_AUDIO_STOP,
                        audio_arg_for_key(event.code, AUDIO_LEVEL_SILENT));
                }
                store_service_defer_commits_until(
                    event.when_ms + STORE_AUDIO_GUARD_MS);
            }
            app_handle_event(&s_app, &event);
        }

        accessory_hal_poll(now);
        bool headset_now = false;
        if (accessory_hal_take_insert_change(&headset_now)) {
            /* Auto-activate the Headset profile on insert, restore the user
             * profile on removal (1:1: accessory profile auto-activation).
             * Act on the EFFECTIVE state (physical OR the Net Monitor force
             * override), not the raw edge: with the force latched on, a
             * physical removal must not strand the codec/profile on handset. */
            bool effective = accessory_hal_headset_inserted();
            if (effective) {
                profile_activate_headset();
            } else {
                profile_restore_from_headset();
            }
            /* Re-route live call audio to/from the headset (no-op when idle). */
            modem_service_accessory_changed(effective);
            s_app.dirty = true;
        }
        if (accessory_hal_take_hook_press() && calls_app_handle_hook(&s_app, now)) {
            s_app.dirty = true; /* HDC-5 hook = answer-on-ring / hang-up-in-call */
        }

        bool backlight_on = backlight_service_is_on();
        /* This cadence is opportunistic: when the powered-on phone is dormant,
         * this loop is not executing and no battery deadline wakes it. Any time
         * the phone is already running, a 1 Hz sample is effectively free. */
        bool battery_app_awake = s_app.route != APP_ROUTE_POWER_OFF;
        bool battery_high_load =
            modem_service_battery_high_load_active() ||
            audio_service_is_active();
        battery_hal_set_sample_mode(battery_sample_mode_select(
            battery_high_load, battery_app_awake));
        modem_service_set_signal_sampling(backlight_on, now);
        modem_service_tick(now);
        if (app_tick(&s_app, now)) {
            s_app.dirty = true;
        }
        netmon_diag_service_poll(now);
        if (s_app.backlight_activity_pending) {
            backlight_service_notify_activity(s_app.backlight_activity_ms);
            s_app.backlight_activity_pending = false;
        }
        backlight_service_set_always_on(lights_setting_always_on(), now);
        if (s_app.backlight_force_active) {
            backlight_service_force_level(s_app.backlight_force_on);
        } else {
            backlight_service_release_force(now);
        }
        if (board_diag_backlight_override_active()) {
            backlight_service_force_level(board_diag_backlight_override_on());
        }
        /* M7: ringtone light markers flash the backlight in sync with the melody
         * (1:1 -- every stock ring pulses the keypad/display lights, ungated by the
         * vibra setting). core1 publishes the marker state; mirror it while the ring
         * plays, then let the force/release above stand once the ring ends. */
        static bool s_ring_light_active = false;
        if (audio_service_ring_light()) {
            s_ring_light_active = true;
        }
        if (s_ring_light_active) {
            if (audio_service_is_active()) {
                backlight_service_force_level(audio_service_ring_light());
            } else {
                /* Ring ended: release our flash force so it doesn't linger (the
                 * app force/release block can't undo it -- release_force always
                 * lights the backlight). Returns the panel to normal timeout. */
                backlight_service_release_force(now);
                s_ring_light_active = false;
            }
        }
        backlight_service_tick(now);
        /* Don't start a flash commit while core1 is producing audio: the commit
         * parks core1 for the erase and would starve the audio DMA refill. The
         * existing key-activity guard only covered keypresses. */
        if (audio_service_is_active()) {
            store_service_defer_commits_until(now + STORE_AUDIO_GUARD_MS);
        }
        store_service_tick(now);
        if (store_service_write_window_open(now)) phonebook_service_tick();
        debug_console_tick(&s_app);
        core1_services_audio_gate_tick(now); /* A2+R2: park idle audio plumbing */
        if (s_app.route != APP_ROUTE_POWER_OFF) {
            /* Self-heal a runtime codec I2C wedge -- but NOT in soft-off: a
             * wedge that tripped the latch during graceful power-down must not
             * re-power the codec. Power-on re-inits it explicitly. */
            core1_services_codec_recover_tick(now);
        }
        /* R3b sleep gating, armed ONCE from the running main loop instead of
         * board_init: by the second tick core0 has completed at least one real
         * WFI enter/exit, so the clock controller's sleep qualification is
         * provably resynced and clearing SLEEP_EN_XIP is safe (see board.c --
         * arming it at boot on a post-flash REBOOT2 boot gated the flash clock
         * instantly under the stale "asleep" qualification). */
        static uint8_t s_sleep_gating_arm_ticks;
        if (s_sleep_gating_arm_ticks < 2u && ++s_sleep_gating_arm_ticks == 2u) {
            board_set_sleep_gating(true);
        }
        if (s_app.route == APP_ROUTE_POWER_OFF) {
            runtime_watchdog_note_phase(RUNTIME_WATCHDOG_PHASE_POWER_OFF);
        }
        power_sleep_tick(s_app.route == APP_ROUTE_POWER_OFF,
                         s_app.battery_charger_connected,
                         now); /* may never return (dormant off state) */
        /* Quiet clock-down: while the screen is dim in standby with
         * nothing to render or play -- and, in the service image, no USB cable -- run from
         * XOSC/2 at 6 MHz with PLL_SYS off, restoring the instant anything changes.
         * Placed before the render so a dirty frame is drawn at full clock. The modem
         * UART stays alive at 6 MHz so incoming-call/SMS URCs still arrive; the 125 Hz
         * tick continues (input still caught within 8 ms). `clkdown` console toggle
         * gates it (default on).
         *
         * The USB block is based on direct GP28, not stack connection state: USB
         * *enumeration* cannot complete at the quiet operating point, so gating
         * on enumeration would
         * deadlock -- a cable plugged into an already clocked-down phone would never
         * enumerate (the gate can't see a host that can't come up because we're
         * clocked down). GP28 reflects the physical plug independent of
         * enumeration, so a later plug goes full clock within one poll and enumerates
         * normally. A live USB host remains a second conservative source. */
#if SISU_RELEASE_BUILD
        bool usb_present = false;
#else
        bool usb_present = power_sleep_hal_service_vbus_present() ||
                           usb_service_host_live();
#endif
        /* modem_uart_hal_tx_idle(): don't reparent clk_peri while a background AT
         * command is still shifting out (the write path only queues into the FIFO) --
         * a mid-transmit re-baud corrupts the command. TX drains in ~1-2 ms << the 8 ms
         * tick, so this just defers clock-down one tick after a send. */
        if (board_clkdown_enabled() && s_app.route == APP_ROUTE_STANDBY &&
            !backlight_service_is_on() && !audio_service_is_active() && !s_app.dirty &&
            !usb_present && modem_uart_hal_tx_idle()) {
            board_enter_xosc_lowpower();
        } else {
            board_exit_xosc_lowpower();
        }
        sync_display_power(&s_app, &s_lcd, &s_fb);
        stack_monitor_core0_sample(now);

        /* Feed only after a complete healthy scheduling turn. Dormant gates
         * RP2350's TICKS block, so the armed counter pauses for the long sleep
         * and resumes on clock restoration without a special long timeout. */
        runtime_watchdog_feed(RUNTIME_WATCHDOG_PHASE_IDLE);

        uint32_t standby_wake_mask = 0u;
        bool standby_app_idle = !s_app.dirty &&
                                !s_app.backlight_activity_pending &&
                                s_active_dtmf_keys == 0u &&
                                event_queue_empty(&s_queue);
        runtime_watchdog_note_phase(RUNTIME_WATCHDOG_PHASE_STANDBY);
        if (standby_sleep_try_enter(
                s_app.route == APP_ROUTE_STANDBY,
                !backlight_service_is_on(),
                standby_app_idle,
                !usb_present,
                app_status_next_wake_ms(&s_app, now),
                &standby_wake_mask)) {
            /* A human power press normally remains low through clock restore and
             * the next scan. Preserve the edge-only case too: if it was released
             * during wake-up, synthesize the same DOWN event the raw scanner
             * would have emitted, avoiding a swallowed quick tap. */
            if ((standby_wake_mask &
                 STANDBY_SLEEP_WAKE_POWER_BUTTON) != 0u &&
                !power_button_scan_raw()) {
                (void)event_queue_push(&s_queue, EVENT_KEY_DOWN, KEY_POWER, 0u,
                                       time_ms());
            }
            prev_raw_keys = 0u;
            prev_raw_power = false;
            next_tick = delayed_by_ms(get_absolute_time(), SYSTEM_TICK_MS);
            continue;
        }

        runtime_watchdog_note_phase(RUNTIME_WATCHDOG_PHASE_IDLE);

        int64_t loop_duration_us =
            absolute_time_diff_us(loop_started, get_absolute_time());
        netmon_diag_service_note_main_loop(
            loop_duration_us > (int64_t)UINT32_MAX
                ? UINT32_MAX : (uint32_t)loop_duration_us,
            SYSTEM_TICK_MS * 1000u);
        sleep_until(next_tick);
        next_tick = delayed_by_ms(next_tick, SYSTEM_TICK_MS);
        /* Catch-up clamp: if an overlong iteration (e.g. a flash sector erase in
         * store_service_tick) left the schedule in the past, resync to one tick
         * from now instead of spinning to replay the missed ticks. Safe because
         * all timing is wall-clock (every tick_/poll_ re-reads now = time_ms()
         * and compares absolute deadlines), so dropped ticks change nothing but
         * the wasted catch-up burst. Keep the 8 ms cadence -- it mirrors the
         * original's UI tick granularity and input responsiveness. */
        if (time_reached(next_tick)) {
            next_tick = delayed_by_ms(get_absolute_time(), SYSTEM_TICK_MS);
        }
    }
}

static void sync_display_power(app_t *app, lcd_pcd8544_t *lcd,
                               framebuffer_t *fb) {
    bool sleep_display = power_off_display_should_sleep(app);

    if (sleep_display) {
        if (!lcd_is_powered_down()) {
            /* PCD8544 requires zeroed display RAM before PD for its specified
             * power-down current. Keep this independent of RP dormant entry:
             * a USB service cable may keep CDC awake while the phone is off. */
            fb_clear(fb, false);
            lcd_show(lcd, fb);
            lcd_power_down();
        }
        app->dirty = false;
        return;
    }

    if (lcd_is_powered_down()) {
        /* Wake restores TC/bias/Vop and normal mode. Force a full frame because
         * the off transition deliberately replaced display RAM with zeros. */
        lcd_power_up();
        app->dirty = true;
    }
    if (app->dirty) {
        app_render(app, fb);
        lcd_show(lcd, fb);
        app->dirty = false;
    }
}

static bool should_wake_backlight_for_event(const app_t *app, const input_event_t *event) {
    if (app == 0 || event == 0) {
        return false;
    }
    if (event->type != EVENT_KEY_DOWN && event->type != EVENT_KEY_HOLD) {
        return false;
    }
    if (!app->keyguard_locked) {
        return true;
    }
    return is_keyguard_unlock_star_key(app, event->code, event->when_ms);
}

static bool lights_setting_always_on(void) {
    uint8_t lights = 0u;
    if (store_setting_get_u8(STORE_SETTING_SETTINGS_LIGHTS, &lights) != STORE_STATUS_OK) {
        return false;
    }
    return lights != 0u;
}

static bool keypad_audio_command_for_event(const app_t *app,
                                           const input_event_t *event,
                                           core1_cmd_t *out_cmd,
                                           uint16_t *out_arg) {
    if (app == 0 || event == 0 || out_cmd == 0 || out_arg == 0) {
        return false;
    }
    /* On the original handset, holding C over a typed standby number emits a
     * second click when clear-all fires. The measured press-to-second-click
     * interval is 497 ms; keypad.c supplies that one-shot 500 ms HOLD event. */
    bool standby_clear_hold = app->route == APP_ROUTE_STANDBY &&
        standby_clear_all_hold_event(app, event);
    if ((event->type != EVENT_KEY_DOWN && !standby_clear_hold) ||
        app->route == APP_ROUTE_POWER_OFF ||
        app->route == APP_ROUTE_POWERUP ||
        app->route == APP_ROUTE_INCOMING_CALL) {
        /* No keypad click while a call is ringing: the ring stream and a keypad
         * click share the single synth voice, so a click would clobber the ring
         * (start_click -> buzzer_hal_off) and silence the rest of the incoming
         * call -- the incoming screen only acts on Answer/Reject/Up-Down anyway,
         * and v6.00 makes only Up/Down "Silent" stop the ring (trace-verified,
         * b65ff4e), so a digit press must not silence it. */
        return false;
    }

    uint8_t level = current_keypad_audio_level();
    if (level == AUDIO_LEVEL_SILENT) {
        return false;
    }

    if (standby_clear_hold) {
        *out_cmd = CORE1_CMD_AUDIO_CLICK;
        *out_arg = audio_arg_for_key(event->code, level);
        return true;
    }

    if (is_menu_keyguard_lock_key(app, event->code, event->when_ms)) {
        *out_cmd = CORE1_CMD_AUDIO_SYSTEM_TONE;
        *out_arg = audio_arg(KEYGUARD_LOCK_TONE_INDEX, level);
        return true;
    }

    if (app->keyguard_locked) {
        if (is_keyguard_unlock_star_key(app, event->code, event->when_ms)) {
            *out_cmd = CORE1_CMD_AUDIO_CLICK;
            *out_arg = audio_arg_for_key(event->code, level);
            return true;
        }
        return false;
    }

    if (app->route == APP_ROUTE_TONE_COMPOSER && is_composer_note_key(event->code)) {
        return false;
    }

    *out_cmd = should_play_dtmf_for_key(app, event->code) ? CORE1_CMD_AUDIO_DTMF : CORE1_CMD_AUDIO_CLICK;
    *out_arg = audio_arg_for_key(event->code, level);
    return true;
}

static bool should_play_dtmf_for_key(const app_t *app, uint16_t key) {
    if (app == 0 || app->keyguard_locked || !is_dtmf_key(key)) {
        return false;
    }
    if (app->route == APP_ROUTE_STANDBY) {
        return true;
    }
    if (app->route == APP_ROUTE_CALL && app->call_connected_ms != 0u) {
        return true;
    }
    return app->route == APP_ROUTE_EDITOR && app->editor_context == EDITOR_CONTEXT_IN_CALL_DTMF;
}

static bool is_menu_keyguard_lock_key(const app_t *app, uint16_t key, uint32_t now) {
    return app != 0 &&
        app->route == APP_ROUTE_MAIN_MENU &&
        key == KEY_STAR &&
        app->menu_keyguard_armed &&
        time_diff_ms(now, app->menu_keyguard_armed_until_ms) <= 0;
}

static bool is_keyguard_unlock_star_key(const app_t *app, uint16_t key, uint32_t now) {
    return app != 0 &&
        app->route == APP_ROUTE_STANDBY &&
        app->keyguard_locked &&
        key == KEY_STAR &&
        app->unlock_armed &&
        time_diff_ms(now, app->unlock_armed_until_ms) <= 0;
}

static bool is_dtmf_key(uint16_t key) {
    switch (key) {
    case KEY_1:
    case KEY_2:
    case KEY_3:
    case KEY_4:
    case KEY_5:
    case KEY_6:
    case KEY_7:
    case KEY_8:
    case KEY_9:
    case KEY_STAR:
    case KEY_0:
    case KEY_HASH:
        return true;
    default:
        return false;
    }
}

static bool is_composer_note_key(uint16_t key) {
    switch (key) {
    case KEY_1:
    case KEY_2:
    case KEY_3:
    case KEY_4:
    case KEY_5:
    case KEY_6:
    case KEY_7:
    case KEY_0:
        return true;
    default:
        return false;
    }
}

static uint8_t current_keypad_audio_level(void) {
    uint8_t value = 1u;
    store_setting_get_u8(STORE_SETTING_PROFILE_KEYPAD_TONES, &value);
    return audio_level_from_keypad_tones(value);
}

#if PROFILE_BOOT
static void run_profile_once(lcd_pcd8544_t *lcd, const app_t *app, framebuffer_t *fb) {
    app_t bench = *app;
    uint32_t start;
    uint32_t elapsed;

    bench.route = APP_ROUTE_STANDBY;
    bench.powerup_stage = APP_POWERUP_DONE;

    start = time_ms();
    for (uint32_t i = 0; i < 100u; i++) {
        app_render(&bench, fb);
    }
    elapsed = time_ms() - start;
    LOGI("bench", "standby idle render x100: %lu ms", (unsigned long)elapsed);

    strcpy(bench.input_text, "123456");
    bench.input_len = 6;
    start = time_ms();
    for (uint32_t i = 0; i < 100u; i++) {
        app_render(&bench, fb);
    }
    elapsed = time_ms() - start;
    LOGI("bench", "standby 6-digit render x100: %lu ms", (unsigned long)elapsed);

    strcpy(bench.input_text, "123456789012345678901234567890");
    bench.input_len = 30;
    start = time_ms();
    for (uint32_t i = 0; i < 100u; i++) {
        app_render(&bench, fb);
    }
    elapsed = time_ms() - start;
    LOGI("bench", "standby 30-digit render x100: %lu ms", (unsigned long)elapsed);

    app_render(&bench, fb);
    start = time_ms();
    for (uint32_t i = 0; i < 100u; i++) {
        lcd_show(lcd, fb);
    }
    elapsed = time_ms() - start;
    LOGI("bench", "lcd flush x100: %lu ms", (unsigned long)elapsed);
}
#endif

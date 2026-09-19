#include "app_status_runtime.h"

#include <string.h>

#include "apps/clock_app.h"
#include "apps/clock_alarm_logic.h"
#include "apps/battery_status_logic.h"
#include "apps/dialogs_app.h"
#include "apps/power_app.h"
#include "apps/profiles_app.h"
#include "audio/audio_levels.h"
#include "hal/rtc_alarm_hal.h"
#include "services/board_diag_service.h"
#include "services/battery_learning_service.h"
#include "services/battery_charge_supervisor_service.h"
#include "services/core1_services.h"
#include "services/modem_service.h"
#include "storage/store_service.h"
#include "ui/status_chrome.h"
#include "ui/signal_bars.h"
#include "services/timebase.h"
#include "ui/ui.h"

#define APP_STATUS_PERIOD_MS 1000u
/* Charging bar frame timer 0x0001, 0x0040 ticks = 512 ms (0x002abf08). */
#define BATTERY_CHARGE_FRAME_MS 512u
/* Battery low/empty cadence cloned 1:1 from v6.00 EM (ROM 0x21de4c):
 * "Battery empty" -> hard power-off ~2.056 s later (timer 0xe4, 0x101 ticks);
 * "Battery low" REPEATS -- immediate on entering the zone, then the fast
 * interval once (ROM 60 VBAT cycles, ~1 min, the post-power-up/charger-removal
 * window) and the steady interval thereafter (ROM 600 cycles, ~10 min). */
#define BATTERY_EMPTY_OFF_DELAY_MS 2056u
#define BATTERY_LOW_WARN_FAST_MS 61000u
#define BATTERY_LOW_WARN_STEADY_MS 614000u
#define MODEM_PROVISION_RECORD_RETRY_MS 1000u

_Static_assert(MODEM_MESSAGE_WAITING_CATEGORY_COUNT <= 8u,
               "message-waiting notice mask exceeds uint8_t");

static battery_charge_transition_state_t s_charge_transition_state;

void app_status_runtime_init(app_t *app) {
    load_clock_into_rtc();
    if (rtc_alarm_hal_snooze_active()) {
        /* P1.7 discards app RAM. Reconstruct the persistent standby overlay
         * from the RTC's tagged live compare on an unrelated wake/power-on. */
        app->clock_alarm_mode = CLOCK_ALARM_MODE_SNOOZE_ACTIVE;
    }
    board_diag_service_init(
        time_ms(),
        battery_charge_supervisor_service_boot_inhibit_required());
    battery_learning_service_init();
    battery_charge_supervisor_service_init();
    battery_charge_transition_init(&s_charge_transition_state);
    app->operator_name[0] = '\0';
    sim_presence_ui_init(&app->sim_presence_ui);
    app->sim_missing = false;
    app->modem_provision_version_recorded = 0u;
    app->modem_provision_record_retry_ms = 0u;
    app->signal_bars = 0u;
    signal_bars_filter_init(&app->signal_bars_filter);
    app->call_divert_unconditional_active = false;
    memset(&app->message_waiting, 0, sizeof(app->message_waiting));
    app->message_waiting_notice_mask = 0u;
    memset(app->message_waiting_notice_count, 0,
           sizeof(app->message_waiting_notice_count));
    battery_raw_collapse_evidence_init(
        &app->battery_raw_collapse_evidence);
    battery_endpoint_state_init(&app->battery_endpoint_state);
    app->battery_endpoint_kind = BATTERY_ENDPOINT_NONE;
    app->battery_capacity_voltage_disagreement = false;
    app->battery_bars = board_diag_battery_level_bars();
    app->battery_anim_level = app->battery_bars;
    status_chrome_set_battery(app->battery_bars);
    update_standby_clock(app);
}

static void battery_play_charger_insert_tone(const app_t *app,
                                             bool powered_off) {
    uint8_t level = profile_get_tone_setting(profile_active_index(), PROFILE_SETTING_WARNING_GAME_TONES);
    if (level == 255u) {
        return;
    }
    if (level > AUDIO_LEVEL_MAX) {
        level = AUDIO_LEVEL_MAX;
    }
    /* Soft-off put the analog codec into full standby. A normal sound post can
     * only undo the lighter playback-idle gate, so restore the codec before the
     * powered-off charger chirp. Charger presence prevents the phone from
     * re-entering P1.7 until removal; the ordinary audio gate may idle it after
     * the one-shot finishes. */
    if (powered_off && !core1_services_codec_init()) {
        return;
    }
    bool call_active = app->last_modem_call_state != MODEM_CALL_IDLE;
    uint8_t index = battery_charger_insert_tone_index(call_active);
    core1_post_command(CORE1_CMD_AUDIO_SYSTEM_TONE,
                       audio_arg(index, level));
}

/* Clear an armed "Battery empty" power-off countdown WITHOUT powering off.
 * Charger insertion is the deliberate recovery path; an accepted EMPTY stays
 * latched across unloaded voltage rebound. Record 0x0f has no auto-close, so if
 * it is still on screen it must be dismissed here, else the phone sits frozen
 * on "Battery empty" and shows no charging animation. */
static void battery_cancel_empty_countdown(app_t *app) {
    if (app->battery_empty_off_ms == 0u) {
        return;
    }
    app->battery_empty_off_ms = 0u;
    if (app->route == APP_ROUTE_DISPLAY_MESSAGE && app->display_record_id == 15u) {
        return_from_display(app);
    }
}

/* Battery/charger runtime. Behavior pinned from the original's traced charging flow:
 * plug with no call -> tone 0x0a + "Charging" record 0x12 (powered on only);
 * the alternate call-state path uses tone 0x0b. Powered-off insertion also
 * plays tone 0x0a after restoring the codec, but keeps the bar-only display;
 * charge bars
 * fill from empty to full (0->4) and repeat every 512 ms; full -> record 0x0e
 * while powered on, or dark LCD/codec standby while powered off;
 * low -> record 0x10 (tone 07); empty -> record 0x0f (tone 08) then power-off.
 * Runs in every route so powered-off charging keeps animating. */
bool poll_battery(app_t *app, uint32_t now_ms) {
    board_diag_service_poll(now_ms);
    /* Freeze the learner's pre-charge evidence before the learner advances its
     * charge segment. An enforcing supervisor reports FULL only after its stop
     * latch is durable and /CE reads back disabled. */
    uint32_t supervisor_result =
        battery_charge_supervisor_service_poll(now_ms);
    bool changed = false;
    bool battery_valid = board_diag_battery_valid();
    bool charger = board_diag_charger_connected();
    bool charge_status_valid = board_diag_charge_status_valid();
    battery_charge_transition_t charge_transition =
        battery_charge_transition(
            &s_charge_transition_state,
            charge_status_valid,
            board_diag_charger_state(),
            app->battery_charge_active,
            now_ms);
    battery_charge_supervisor_service_note_legacy(
        charge_transition.active, charge_transition.completed);
    bool charge_active = charge_transition.active;
    board_diag_snapshot_t board;
    board_diag_get_snapshot(&board);
    bool charge_recovery_active = battery_charge_recovery_active(
        board.charge_status_valid &&
            board.charge_state == BOARD_DIAG_CHARGE_ACTIVE,
        board.charger_enable_valid,
        board.charger_enabled,
        board.ltc_sample_valid && board.ltc_continuity_valid &&
            board.ltc_current_polarity_verified,
        board.ltc_current_ua);
    battery_charge_supervisor_service_snapshot_t supervisor;
    battery_charge_supervisor_service_get_snapshot(&supervisor);
    bool supervisor_completed = false;
    if ((supervisor_result &
         BATTERY_CHARGE_SUPERVISOR_RESULT_FULL_QUALIFIED) != 0u) {
        supervisor_completed = supervisor.model.full_qualified;
    }
    battery_charge_completion_decision_t completion =
        battery_charge_completion_decision(
            charge_transition.completed, supervisor_completed,
            supervisor.model.policy == BATTERY_CHARGE_POLICY_OBSERVE);
    bool completed = completion.ui_completed;
    bool maintenance_restarted =
        (supervisor_result &
         BATTERY_CHARGE_SUPERVISOR_RESULT_MAINTENANCE_RESTARTED) != 0u;
    uint32_t learning_events = completion.learn_full
        ? BATTERY_LEARNING_EVENT_FULL : BATTERY_LEARNING_EVENT_NONE;
    uint16_t charge_factor_permille =
        battery_charge_supervisor_service_effective_charge_factor_permille();
    uint32_t learning_result = battery_learning_service_poll(
        now_ms, learning_events, charge_factor_permille);
    if ((learning_result & BATTERY_LEARNING_RESULT_FULL_ANCHORED) != 0u) {
        /* Endpoint evidence is rare and expensive to reproduce. Make both the
         * supervisor terminal and learner anchor durable before UI work. */
        (void)store_service_flush_all();
    }
    battery_learning_service_snapshot_t learner;
    battery_learning_service_get_snapshot(&learner);
    uint8_t bars = battery_display_bars(
        learner.model.soc_bars_valid, learner.model.soc_bars,
        battery_valid, board_diag_battery_level_bars(), app->battery_bars);
    uint16_t mv_fresh = board_diag_battery_millivolts_fresh();
    uint32_t battery_sample_sequence =
        board_diag_battery_sample_sequence();
    bool powered_off = app->route == APP_ROUTE_POWER_OFF;
    bool contact_service = app->route == APP_ROUTE_CONTACT_SERVICE;
    bool standby = app->route == APP_ROUTE_STANDBY;
    bool empty_action_started = false;

    /* A numeric zero is not a low battery on Rev B2: it means no authoritative
     * gauge sample exists yet. Validity changes only after LTC2959 data
     * passes its settling and plausibility checks. */
    /* The modem owns detection of an intended +3V8 supply failure. Only turn
     * that physical event into the original's Battery-empty UX when independent
     * fresh LTC evidence says the pack is marginal and it is not recovering.
     * Keep the verdict latched through the existing ~2 s shutdown countdown;
     * terminal voltage can rebound as soon as the modem load disappears. If the
     * LTC is still settling, leave the one-shot pending until evidence exists. */
    bool supply_failure = false;
    if (battery_valid || charger || powered_off) {
        supply_failure = modem_service_take_supply_power_failure();
    }
    bool modem_supply_collapse = supply_failure &&
        board_diag_battery_supply_failure_indicates_empty();

    if (charger != app->battery_charger_connected) {
        app->battery_charger_connected = charger;
        if (charger) {
            app->battery_full_notified = false;
            /* Event 0x0724 -> expression 0x051e selects tone 0x0a in the
             * ordinary state. The "Charging" dialog remains powered-on only. */
            battery_play_charger_insert_tone(app, powered_off);
            if (standby) {
                open_display_sid(app, 18u, 0xc9u, "Charging", APP_ROUTE_STANDBY, now_ms);
            }
        }
        changed = true;
    }

    battery_full_notice_decision_t full_notice = battery_full_notice_step(
        app->battery_full_notified, completed, maintenance_restarted);
    app->battery_full_notified = full_notice.notified;

    if (charge_active != app->battery_charge_active || completed) {
        bool powered_off_session_ended =
            powered_off && app->battery_charge_active && !charge_active;
        app->battery_charge_active = charge_active;
        app->battery_anim_level = charge_active ? 0u : bars;
        app->battery_anim_ms = now_ms;
        if (full_notice.show) {
            if (!powered_off && !contact_service &&
                app->route != APP_ROUTE_DISPLAY_MESSAGE) {
                open_display_sid(
                    app, 14u, 0u, "Battery\nfull", app->route, now_ms);
            }
        }
        if (powered_off_session_ended) {
            /* The insertion chirp initialized the codec from full standby.
             * Charging completion/fault/detach ends the only powered-off audio
             * owner, so restore the deep analog standby along with the LCD. */
            core1_services_codec_standby();
        }
        changed = true;
    }

    if (bars != app->battery_bars) {
        app->battery_bars = bars;
        changed = true;
    }

    /* Warning state comes from the v6.00 dual-domain classifier: terminal/fast
     * evidence, the 65 mA reference estimate, and its 20-second healthy-entry
     * qualification are all owned by battery_gauge_logic. The app also owns the
     * independent Rev B2 operating floor. Outside a call it matches the 2.10 V
     * power-on gate, leaving enough margin to persist and shut down cleanly;
     * during a call only the lower emergency floor may end the call. */
    bool call_active = app->last_modem_call_state != MODEM_CALL_IDLE;
    uint16_t shutdown_threshold_mv =
        battery_empty_shutdown_threshold_mv(call_active);
    bool raw_collapse_confirmed = battery_raw_collapse_evidence_update(
        &app->battery_raw_collapse_evidence,
        battery_valid && !charge_recovery_active && !powered_off,
        battery_sample_sequence,
        mv_fresh,
        shutdown_threshold_mv);
    bool gauge_empty = battery_valid && board_diag_battery_empty();
    bool gauge_low = battery_valid && board_diag_battery_low();
    bool anchored_capacity_has_charge =
        battery_anchored_remaining_has_charge(
            learner.model.remaining_capacity_valid,
            learner.model.soc_confidence == BATTERY_SOC_CONFIDENCE_ANCHORED,
            learner.model.remaining_capacity_nah);
    battery_endpoint_evidence_t endpoint_evidence = {
        .battery_sample_valid = battery_valid,
        .charge_recovery_active = charge_recovery_active,
        .powered_off = powered_off,
        .call_active = call_active,
        .gauge_low = gauge_low,
        .gauge_empty = gauge_empty,
        .operating_floor_confirmed = raw_collapse_confirmed,
        .modem_supply_collapse = modem_supply_collapse,
        .capacity_prediction_exhausted =
            learner.model.capacity_prediction_exhausted,
        .anchored_capacity_has_charge = anchored_capacity_has_charge,
        .shutdown_armed = app->battery_empty_off_ms != 0u,
    };
    battery_endpoint_decision_t endpoint = battery_endpoint_step(
        &app->battery_endpoint_state, &endpoint_evidence);
    app->battery_endpoint_kind = endpoint.kind;
    app->battery_capacity_voltage_disagreement =
        endpoint.capacity_voltage_disagreement;
    bool empty_zone = endpoint.shutdown;
    bool low_zone = battery_valid && (gauge_low || endpoint.force_low);
    if (battery_charge_floor_release_needed(
            charger, charge_recovery_active, low_zone, empty_zone)) {
        (void)battery_charge_supervisor_service_release_for_battery_floor();
    }
    battery_warning_action_t warning_action = battery_warning_action(
        battery_valid, charge_recovery_active, powered_off, empty_zone,
        low_zone);

    if (warning_action == BATTERY_WARNING_ACTION_SUPPRESS) {
        /* Verified positive charge re-arms the warning and cancels a countdown;
         * physical VIN alone is deliberately insufficient. */
        app->battery_low_notified = false;
        battery_cancel_empty_countdown(app);
    } else if (warning_action == BATTERY_WARNING_ACTION_EMPTY) {
        /* Natural EMPTY requires physical voltage/rail evidence. Coulomb zero
         * alone remains an observable prediction so an unexpectedly healthy
         * pack can run on and teach a larger capacity; outside a call, its
         * conjunction with committed gauge LOW closes the endpoint before the
         * loaded rail reaches the hard floor. During a call, trusted remaining
         * charge converts Nokia EMPTY or modem-rail collapse into LOW/load-sag
         * evidence; only the 1.8 V emergency floor may still force shutdown,
         * and that safety action is not offered to the learner as a capacity
         * endpoint. Re-polling one cached floor sample cannot confirm either
         * threshold, and changing call policy resets the evidence streak. An
         * accepted shutdown remains latched through the ~2 s countdown;
         * verified positive charging is the cancellation path. */
        app->battery_low_notified = false;
        if (app->battery_empty_off_ms == 0u) {
            if (endpoint.learn_empty) {
                uint32_t endpoint_result =
                    battery_learning_service_poll(
                    now_ms, BATTERY_LEARNING_EVENT_EMPTY,
                    charge_factor_permille);
                if ((endpoint_result & BATTERY_LEARNING_RESULT_PERSIST) != 0u) {
                    (void)store_service_flush_all();
                }
                battery_learning_service_get_snapshot(&learner);
                bars = battery_display_bars(
                    learner.model.soc_bars_valid,
                    learner.model.soc_bars,
                    battery_valid,
                    board_diag_battery_level_bars(),
                    app->battery_bars);
                if (bars != app->battery_bars) {
                    app->battery_bars = bars;
                    changed = true;
                }
            }
            empty_action_started = true;
            /* Arm the hard-off countdown REGARDLESS of route: the over-discharge
             * safety cutoff must not depend on the screen being free. Nesting the
             * arm inside the route!=DISPLAY_MESSAGE guard (as before) let a
             * permanent progress/error dialog trap the pack below EMPTY with no
             * shutdown, and delayed the ~2 s cutoff behind any long-lived notice.
             * Reserve 0 as the disarmed sentinel: if now + delay lands exactly on
             * 0 the countdown would read as disarmed and never fire. Clamp to 1. */
            app->battery_empty_off_ms = now_ms + BATTERY_EMPTY_OFF_DELAY_MS;
            if (app->battery_empty_off_ms == 0u) {
                app->battery_empty_off_ms = 1u;
            }
            changed = true;
        }
    } else if (warning_action == BATTERY_WARNING_ACTION_HOLD) {
        /* A failed/settling conversion is absence of evidence, not recovery.
         * Preserve a LOW cadence or EMPTY countdown already in flight. */
    } else if (warning_action == BATTERY_WARNING_ACTION_LOW) {
        /* Low zone: immediate first warning on entry, then repeat on the
         * countdown (fast once after a transition, steady thereafter). */
        battery_cancel_empty_countdown(app);
        if (!app->battery_low_notified) {
            app->battery_low_notified = true;
            app->battery_low_warn_fast = true;
            app->battery_low_next_warn_ms = now_ms; /* fire this poll */
        }
        if (time_diff_ms(now_ms, app->battery_low_next_warn_ms) >= 0 &&
            !contact_service &&
            app->route != APP_ROUTE_DISPLAY_MESSAGE) {
            open_display_sid(app, 16u, 0x62u, "Battery\nlow", app->route, now_ms);
            app->battery_low_next_warn_ms =
                now_ms + (app->battery_low_warn_fast ? BATTERY_LOW_WARN_FAST_MS
                                                     : BATTERY_LOW_WARN_STEADY_MS);
            app->battery_low_warn_fast = false;
            changed = true;
        }
    } else {
        /* Healthy classifier state: clear the notification schedule. */
        app->battery_low_notified = false;
        battery_cancel_empty_countdown(app);
    }

    if (app->battery_empty_off_ms != 0u) {
        /* The endpoint is more valuable than the notice if the pack is already
         * on its knee. Push the learner's journaled EMPTY/capacity record to flash
         * before posting audio or changing the display. Every countdown poll
         * retries a transient failure, and soft-off retries again before dormant
         * RAM loss. */
        (void)store_service_flush_all();
    }
    if (empty_action_started) {
        /* Don't stomp an in-progress display; the countdown remains armed and
         * powers off regardless of whether the notice can be shown. */
        if (!contact_service && app->route != APP_ROUTE_DISPLAY_MESSAGE) {
            open_display_sid(
                app, 15u, 0x259u, "Battery\nempty", app->route, now_ms);
        }
    }

    if (app->battery_empty_off_ms != 0u && time_diff_ms(now_ms, app->battery_empty_off_ms) >= 0) {
        app->battery_empty_off_ms = 0u;
        if (!powered_off) {
            power_off(app, now_ms);
            changed = true;
        }
    }

    if (charge_active) {
        if (time_diff_ms(now_ms, app->battery_anim_ms + BATTERY_CHARGE_FRAME_MS) >= 0) {
            app->battery_anim_ms = now_ms;
            /* Full 0->4 sweep, repeating (owner bench: the original's charging
             * animation always fills empty->full regardless of the actual level).
             * It never freezes into a false "full" mid-charge -- the wrap back to
             * 0 keeps it scrolling while STAT says charging; the static-full
             * display comes from the STAT termination edge above. */
            app->battery_anim_level = app->battery_anim_level >= 4u
                ? 0u
                : (uint8_t)(app->battery_anim_level + 1u);
            changed = true;
        }
        status_chrome_set_battery(app->battery_anim_level);
    } else {
        status_chrome_set_battery(bars);
    }
    return changed;
}

uint32_t app_status_next_wake_ms(const app_t *app, uint32_t now_ms) {
    if (app == NULL || !app->battery_charge_active) {
        return UINT32_MAX;
    }
    uint32_t deadline_ms = app->battery_anim_ms + BATTERY_CHARGE_FRAME_MS;
    int32_t remaining_ms = time_diff_ms(deadline_ms, now_ms);
    return remaining_ms <= 0 ? 1u : (uint32_t)remaining_ms;
}

bool poll_app_status(app_t *app, uint32_t now_ms) {
    bool changed = false;
    if (app->route == APP_ROUTE_POWER_OFF) {
        return false;
    }
    if (time_diff_ms(now_ms, app->last_status_ms + APP_STATUS_PERIOD_MS) < 0) {
        return false;
    }

    app->last_status_ms = now_ms;

    modem_status_t status;
    modem_service_get_status(&status);

    if (status.provisioning_verified &&
        status.provisioning_schema_version != 0u &&
        app->modem_provision_version_recorded !=
            status.provisioning_schema_version &&
        store_service_ready() &&
        time_diff_ms(now_ms, app->modem_provision_record_retry_ms) >= 0) {
        uint16_t stored_version = 0u;
        if (store_setting_get_u16(
                STORE_SETTING_SYSTEM_MODEM_PROVISION_VERSION,
                &stored_version) == STORE_STATUS_OK &&
            (stored_version == status.provisioning_schema_version ||
             store_setting_set_u16(
                 STORE_SETTING_SYSTEM_MODEM_PROVISION_VERSION,
                 status.provisioning_schema_version) == STORE_STATUS_OK)) {
            /* This is a diagnostic completion record only. The modem service
             * intentionally re-queries every row on every later boot. */
            app->modem_provision_version_recorded =
                status.provisioning_schema_version;
            app->modem_provision_record_retry_ms = 0u;
        } else {
            /* Storage may be busy with another journal commit. This record is
             * diagnostic only, so retry slowly instead of hammering admission
             * on every 100 ms status poll. */
            app->modem_provision_record_retry_ms =
                now_ms + MODEM_PROVISION_RECORD_RETRY_MS;
        }
    }

    bool sim_missing = sim_presence_ui_update(
        &app->sim_presence_ui, status.at_ready, status.sim_checked,
        status.sim_present, now_ms);
    if (sim_missing != app->sim_missing) {
        app->sim_missing = sim_missing;
        changed = true;
    }
    bool network_identity_available =
        status.at_ready && !sim_missing && status.network_registered;
    if (!network_identity_available) {
        /* A vanished PWRMON/AT channel invalidates the last serving network
         * immediately. Keeping the app cache here makes a failed or powered-off
         * modem look registered until a later successful startup. Missing SIM
         * uses the same zero-bar/blank-operator identity while its dedicated
         * standby message remains authoritative. Reset the metric filter too so
         * recovery starts from the new cell rather than an old LTE EMA. */
        if (app->signal_bars != 0u) {
            app->signal_bars = 0u;
            changed = true;
        }
        if (app->operator_name[0] != '\0') {
            app->operator_name[0] = '\0';
            changed = true;
        }
        signal_bars_filter_init(&app->signal_bars_filter);
    } else {
        bool has_rssi = status.rssi != 99u;
        uint8_t fallback_bars = has_rssi
            ? status_bars_from_rssi(status.rssi)
            : 0u;
        /* LTE bars use an atomic RSRP + SINR/RSRQ sample. Registration remains
         * the UI authority: no service is zero bars, while a registered phone
         * never ambiguously renders zero during a temporary metric gap. */
        uint8_t bars = signal_bars_filter_update(
            &app->signal_bars_filter, status.network_registered,
            &status.signal, fallback_bars);
        if (bars != app->signal_bars) {
            app->signal_bars = bars;
            changed = true;
        }
        if (strcmp(app->operator_name, status.operator_name) != 0) {
            copy_text(app->operator_name, sizeof(app->operator_name),
                      status.operator_name);
            app->operator_name[sizeof(app->operator_name) - 1u] = '\0';
            changed = true;
        }
    }
    bool divert_active = status.at_ready && !sim_missing &&
        status.call_forward_unconditional_known &&
        status.call_forward_unconditional_active;
    if (divert_active != app->call_divert_unconditional_active) {
        app->call_divert_unconditional_active = divert_active;
        changed = true;
    }
    modem_message_waiting_status_t next_message_waiting = {0};
    if (status.at_ready && !sim_missing) {
        next_message_waiting = status.message_waiting;
    }
    for (uint8_t i = 0u; i < MODEM_MESSAGE_WAITING_CATEGORY_COUNT; i++) {
        modem_message_waiting_state_t before =
            app->message_waiting.category[i];
        modem_message_waiting_state_t after =
            next_message_waiting.category[i];
        if (i == MODEM_MESSAGE_WAITING_FAX ||
            i == MODEM_MESSAGE_WAITING_EMAIL) {
            uint8_t bit = modem_message_waiting_category_bit(
                (modem_message_waiting_category_t)i);
            if (!after.active) {
                if ((app->message_waiting_notice_mask & bit) != 0u) {
                    app->message_waiting_notice_mask &= (uint8_t)~bit;
                    changed = true;
                }
                app->message_waiting_notice_count[i] = 0u;
            } else if (!before.active || after.count > before.count) {
                app->message_waiting_notice_mask |= bit;
                app->message_waiting_notice_count[i] = after.count;
                app->backlight_activity_pending = true;
                app->backlight_activity_ms = now_ms;
                changed = true;
            } else if ((app->message_waiting_notice_mask & bit) != 0u) {
                app->message_waiting_notice_count[i] = after.count;
            }
        }
        if (before.active != after.active || before.count != after.count) {
            changed = true;
        }
    }
    app->message_waiting = next_message_waiting;

    char before[sizeof(app->clock_text)];
    bool alarm_before = app->clock_alarm_enabled;
    copy_text(before, sizeof(before), app->clock_text);
    update_standby_clock(app);
    if (strcmp(before, app->clock_text) != 0 || alarm_before != app->clock_alarm_enabled) {
        changed = true;
    }

    return changed;
}

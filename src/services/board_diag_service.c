#include "services/board_diag_service.h"

#include <limits.h>

#include "audio/nau88c22_codec.h"
#include "audio/vibra_hal.h"
#include "hal/accessory_hal.h"
#include "hal/battery_hal.h"
#include "hal/board.h"
#include "hal/board_irq_hal.h"
#include "hal/ltc2959_hal.h"
#include "hal/modem_power_monitor_hal.h"
#include "hal/power_sleep_hal.h"
#include "hal/rtc_alarm_hal.h"
#include "hal/tca8418_hal.h"
#include "services/backlight_service.h"
#include "services/charger_control_service.h"
#include "services/shared_irq_service.h"
#include "services/timebase.h"

#define BOARD_DIAG_SIM_STEP_MS 1000u
#define BOARD_DIAG_SIM_CHARGE_STEP_MV 6u
#define BOARD_DIAG_SIM_DISCHARGE_STEP_MV 1u
#define BOARD_DIAG_MODEM_MONITOR_POLL_MS 1000u
#define BOARD_DIAG_FORCE_SAMPLE_MS 250u
/* Charger-sim discharge/charge range for the NetMon bench tool. [BP] 2-cell NiMH:
 * MIN below EMPTY (1900) so the sim walks through LOW/EMPTY/hard-off, MAX above
 * BAR4 (2575) for a "full" pack. Were stale Li-ion values (3450-4150) that pinned
 * the gauge at 4 bars and masked low/empty until reboot. (Rev-B Li-ion will need a
 * chemistry-aware range keyed on s_chemistry.) */
#define BOARD_DIAG_BATTERY_MV_MIN 1700u
#define BOARD_DIAG_BATTERY_MV_MAX 2800u

static uint16_t s_battery_mv;
static uint16_t s_battery_mv_raw;
static uint16_t s_battery_mv_fresh;
static uint32_t s_battery_source_sequence;
static uint32_t s_battery_sample_sequence;
static uint32_t s_force_sample_ms;
static bool s_battery_valid;
static bool s_charger_connected;
static bool s_charger_forced;
static board_diag_chemistry_t s_chemistry = BOARD_DIAG_CHEM_NI_MH;
static bool s_vbus_present;
static bool s_vbus_forced;
static bool s_headset_inserted;
static bool s_headset_hook_pressed;
static uint16_t s_headset_hook_mv = 3300u;
static bool s_tca_present;
static bool s_backlight_override;
static bool s_backlight_override_on;
static bool s_vibra_test_enabled;
static uint32_t s_last_sim_ms;
static uint8_t s_chr_stat1 = 1u;
static uint8_t s_chr_stat2 = 1u;
static uint16_t s_modem_status_adc_raw;
static uint16_t s_modem_status_pin_mv;
static bool s_modem_monitor_polled;
static uint32_t s_last_modem_monitor_ms;
static bool s_batt_force_active;   /* debug: hold s_battery_mv at a forced value */
static uint16_t s_batt_force_mv;

static uint8_t level_bars_from_mv(uint16_t mv);
static board_diag_charge_state_t charge_state_from_inputs(bool present,
                                                          uint8_t stat1,
                                                          uint8_t stat2);
static void poll_modem_monitor(uint32_t now_ms, bool force);
static void refresh_cached_inputs(void);
static void step_forced_battery(uint32_t now_ms);
static void note_battery_sample(void);

static uint32_t saturating_add_u32(uint32_t a, uint32_t b) {
    return a > UINT32_MAX - b ? UINT32_MAX : a + b;
}

void board_diag_service_init(uint32_t now_ms,
                             bool supervisor_inhibit_at_boot) {
    battery_hal_init();
    modem_power_monitor_hal_init();
    battery_hal_poll(now_ms);
    s_modem_monitor_polled = false;
    s_last_modem_monitor_ms = now_ms;
    poll_modem_monitor(now_ms, true);
    s_battery_mv = battery_hal_millivolts();
    s_battery_mv_raw = battery_hal_millivolts_raw();
    s_battery_mv_fresh = battery_hal_millivolts_fresh();
    s_battery_source_sequence = battery_hal_sample_sequence();
    s_battery_sample_sequence = 1u;
    s_force_sample_ms = now_ms;
    s_battery_valid = battery_hal_valid();
    s_charger_connected = battery_hal_charger_connected();
    s_charger_forced = false;
    s_chemistry = BOARD_DIAG_CHEM_NI_MH;
    s_tca_present = false;
    (void)tca8418_hal_set_charger_li_ion_mode(false);
    charger_control_service_init(
        now_ms, supervisor_inhibit_at_boot ? CHARGER_INHIBIT_SUPERVISOR : 0u);
    s_vbus_present = board_service_vbus_present();
    s_vbus_forced = false;
    s_headset_inserted = false;
    s_headset_hook_pressed = false;
    s_headset_hook_mv = 3300u;
    s_backlight_override = false;
    s_backlight_override_on = false;
    s_vibra_test_enabled = false;
    s_last_sim_ms = now_ms;
    s_chr_stat1 = 1u;
    s_chr_stat2 = 1u;
    refresh_cached_inputs();
}

void board_diag_service_poll(uint32_t now_ms) {
    battery_hal_poll(now_ms);
    uint32_t source_sequence = battery_hal_sample_sequence();
    if (source_sequence != s_battery_source_sequence) {
        s_battery_source_sequence = source_sequence;
        if (!s_charger_forced && !s_batt_force_active) {
            note_battery_sample();
        }
    }
    poll_modem_monitor(now_ms, false);
    charger_control_service_poll(now_ms);
    refresh_cached_inputs();
    if (s_charger_forced) {
        s_battery_valid = true;
        step_forced_battery(now_ms);
        s_battery_mv_raw = s_battery_mv;   /* sim: no load model */
        s_battery_mv_fresh = s_battery_mv; /* sim: hard-off tracks the forced level */
        return;
    }
    s_charger_connected = battery_hal_charger_connected();
    if (s_batt_force_active) {
        /* debug override (battmv): drive both the gauge and the raw path so
         * the low/empty AND the raw hard-off can be exercised from the console */
        s_battery_mv = s_batt_force_mv;
        s_battery_mv_raw = s_batt_force_mv;
        s_battery_mv_fresh = s_batt_force_mv;
        s_battery_valid = true;
        if (time_diff_ms(now_ms,
                         s_force_sample_ms + BOARD_DIAG_FORCE_SAMPLE_MS) >= 0) {
            s_force_sample_ms = now_ms;
            note_battery_sample();
        }
        return;
    }
    s_battery_valid = battery_hal_valid();
    s_battery_mv = battery_hal_millivolts();
    s_battery_mv_raw = battery_hal_millivolts_raw();
    s_battery_mv_fresh = battery_hal_millivolts_fresh();
}

static void poll_modem_monitor(uint32_t now_ms, bool force) {
    if (!force && s_modem_monitor_polled &&
        time_diff_ms(
            now_ms,
            s_last_modem_monitor_ms +
                BOARD_DIAG_MODEM_MONITOR_POLL_MS) < 0) {
        return;
    }
    s_modem_monitor_polled = true;
    s_last_modem_monitor_ms = now_ms;
    (void)modem_power_monitor_hal_read(
        &s_modem_status_adc_raw,
        &s_modem_status_pin_mv);
}

uint16_t board_diag_battery_millivolts(void) {
    return s_battery_mv;
}

uint16_t board_diag_battery_millivolts_raw(void) {
    return s_battery_mv_raw;
}

uint16_t board_diag_battery_millivolts_fresh(void) {
    return s_battery_mv_fresh;
}

uint32_t board_diag_battery_sample_sequence(void) {
    return s_battery_sample_sequence;
}

bool board_diag_battery_valid(void) {
    return s_battery_valid;
}

uint8_t board_diag_battery_level_bars(void) {
    /* Production uses the load-normalized, robust-filtered Nokia projection.
     * Debug/sim overrides intentionally bypass the estimator so one requested
     * voltage still moves the icon and warning paths deterministically. */
    if (!s_battery_valid) {
        return 0u;
    }
    if (s_batt_force_active || s_charger_forced) {
        return level_bars_from_mv(s_battery_mv);
    }
    return battery_hal_level_bars();
}

bool board_diag_battery_low(void) {
    if (!s_battery_valid) {
        return false;
    }
    if (s_batt_force_active || s_charger_forced) {
        return s_battery_mv >= VBAT_EMPTY_MV && s_battery_mv < VBAT_LOW_MV;
    }
    return battery_hal_low();
}

bool board_diag_battery_empty(void) {
    if (!s_battery_valid) {
        return false;
    }
    if (s_batt_force_active || s_charger_forced) {
        return s_battery_mv < VBAT_EMPTY_MV;
    }
    return battery_hal_empty();
}

bool board_diag_battery_supply_failure_indicates_empty(void) {
    /* PG loss is not enough to call the pack empty: a healthy-voltage board or
     * regulator fault must stay a modem fault. Conversely, use fresh terminal
     * voltage rather than the compensated/slow display estimate so a marginal
     * pack cannot have its physical collapse hidden by load compensation. */
    return s_battery_valid && s_battery_mv_fresh < VBAT_LOW_MV;
}

static bool charger_can_recover_pack(bool charger_present) {
    if (!charger_present) {
        return false;
    }
    charger_control_snapshot_t control;
    charger_control_service_get_snapshot(&control);
    if (!control.readback_valid || !control.requested_enabled ||
        !control.actual_enabled) {
        return false;
    }
    if (s_charger_forced) {
        return s_battery_mv < BOARD_DIAG_BATTERY_MV_MAX;
    }
    return battery_hal_charge_status_valid() &&
        battery_hal_charger_state_from_inputs(
            true, battery_hal_charge_status()) ==
            BATTERY_CHARGER_ACTIVE;
}

board_diag_power_on_status_t board_diag_battery_power_on_status(void) {
    bool recovery_active = charger_can_recover_pack(
        s_charger_connected);
    if (s_batt_force_active || s_charger_forced) {
        return battery_hal_power_on_gate_from_mv(
                   s_battery_mv, recovery_active) == BATTERY_POWER_ON_OK
            ? BOARD_DIAG_POWER_ON_ALLOWED : BOARD_DIAG_POWER_ON_REFUSED;
    }
    /* The rolling qualifier deliberately tolerates failed conversions, so do
     * not let one invalid latest snapshot discard its bounded history. Cached
     * charger presence is trusted only alongside a valid battery snapshot;
     * otherwise the fresh GP43 observation below must prove it. */
    battery_power_on_gate_t gate = battery_hal_power_on_gate(
        s_battery_valid && recovery_active);
    if (gate == BATTERY_POWER_ON_OK) {
        return BOARD_DIAG_POWER_ON_ALLOWED;
    }
    /* Sample VIN synchronously, but only an enabled BQ ACTIVE state may bypass
     * the low-voltage gate. A completed, inhibited, or faulted charger cannot
     * support modem startup merely because its adapter is still attached. */
    bool charger_present =
        battery_hal_charger_input_present_now(time_ms());
    if (charger_can_recover_pack(charger_present)) {
        return BOARD_DIAG_POWER_ON_ALLOWED;
    }
    return gate == BATTERY_POWER_ON_PENDING
        ? BOARD_DIAG_POWER_ON_PENDING : BOARD_DIAG_POWER_ON_REFUSED;
}

bool board_diag_battery_power_on_allowed(void) {
    return board_diag_battery_power_on_status() == BOARD_DIAG_POWER_ON_ALLOWED;
}

bool board_diag_headset_inserted(void) {
    return s_headset_inserted;
}

bool board_diag_charger_connected(void) {
    return s_charger_connected;
}

bool board_diag_charge_status_valid(void) {
    return s_charger_forced ||
           battery_hal_charge_status_valid();
}

battery_charger_state_t board_diag_charger_state(void) {
    if (!s_charger_connected) {
        return BATTERY_CHARGER_DETACHED;
    }
    charger_control_snapshot_t control;
    charger_control_service_get_snapshot(&control);
    if (control.readback_valid && !control.actual_enabled) {
        /* STAT1/2 are not a completion oracle while /CE is inhibited. Mapping
         * the disabled charger to DETACHED keeps an intentional software stop
         * out of the BQ-FULL path; the supervisor publishes FULL separately
         * only after its durable latch and /CE readback both succeed. */
        return BATTERY_CHARGER_DETACHED;
    }
    if (s_charger_forced) {
        return control.actual_enabled &&
                       s_battery_mv < BOARD_DIAG_BATTERY_MV_MAX
                   ? BATTERY_CHARGER_ACTIVE
                   : BATTERY_CHARGER_FULL;
    }
    return battery_hal_charger_state_from_inputs(
        true, battery_hal_charge_status());
}

bool board_diag_charger_enable_readback(bool *enabled) {
    charger_control_snapshot_t control;
    charger_control_service_get_snapshot(&control);
    if (enabled != NULL) {
        *enabled = control.actual_enabled;
    }
    return control.readback_valid;
}

void board_diag_get_battery_learning_observation(
    uint32_t now_ms, battery_learning_observation_t *out) {
    if (out == NULL) {
        return;
    }
    ltc2959_snapshot_t ltc;
    ltc2959_hal_get_snapshot(&ltc);
    *out = (battery_learning_observation_t){
        .now_ms = now_ms,
        .sample_sequence = ltc.sample_sequence,
        .gauge_session = ltc.gauge_session,
        .acr_raw = ltc.acr_raw,
        .session_delta_nah = ltc.session_delta_nah,
        .terminal_mv = ltc.voltage_mv,
        .current_ua = ltc.current_ua,
        .temperature_mdegc = ltc.temperature_mdegc,
        .reference_mv = s_battery_mv,
        .sample_valid = ltc.sample_valid,
        .current_valid = ltc.sample_valid &&
                         ltc.current_polarity_verified,
        .continuity_valid = ltc.continuity_valid,
        .reference_valid = s_battery_valid,
        .charger_connected = s_charger_connected,
        .charge_active = board_diag_charge_status_valid() &&
                         board_diag_charger_state() ==
                             BATTERY_CHARGER_ACTIVE,
        .authoritative = !s_batt_force_active && !s_charger_forced,
    };
}

void board_diag_get_snapshot(board_diag_snapshot_t *out) {
    if (out == 0) {
        return;
    }
    out->battery_mv = s_battery_mv;
    out->battery_valid = s_battery_valid;
    out->battery_bars = board_diag_battery_level_bars();
    out->battery_forced = s_batt_force_active;
    out->battery_low = board_diag_battery_low();
    out->battery_empty = board_diag_battery_empty();
    out->battery_correction_mv =
        s_batt_force_active || s_charger_forced
            ? 0
            : battery_hal_load_correction_mv();
    out->battery_window_current_ua =
        s_batt_force_active || s_charger_forced
            ? 0
            : battery_hal_window_current_ua();
    out->battery_window_current_valid =
        !s_batt_force_active && !s_charger_forced &&
        battery_hal_window_current_valid();
    out->battery_power_on_samples =
        battery_hal_power_on_sample_count();
    out->battery_power_on_attempts =
        battery_hal_power_on_attempt_count();
    out->battery_power_on_average_mv =
        battery_hal_power_on_average_mv();
    ltc2959_snapshot_t ltc;
    ltc2959_hal_get_snapshot(&ltc);
    out->ltc_present = ltc.present;
    out->ltc_configured = ltc.configured;
    out->ltc_sample_valid = ltc.sample_valid;
    out->ltc_continuity_valid = ltc.continuity_valid;
    out->ltc_current_polarity_verified =
        ltc.current_polarity_verified;
    out->ltc_voltage_mv = ltc.voltage_mv;
    out->ltc_current_ua = ltc.current_ua;
    out->ltc_acr_raw = ltc.acr_raw;
    out->ltc_session_delta_nah = ltc.session_delta_nah;
    out->ltc_temperature_mdegc = ltc.temperature_mdegc;
    out->ltc_status = ltc.status_latched;
    out->ltc_i2c_errors = ltc.i2c_error_count;
    out->ltc_ara_errors = ltc.ara_error_count;
    out->ltc_gauge_session = ltc.gauge_session;
    out->charger_connected = s_charger_connected;
    out->charger_adc_raw = battery_hal_charger_adc_raw();
    out->charger_pin_mv = battery_hal_charger_pin_mv();
    out->charger_input_mv = battery_hal_charger_input_mv();
    out->charge_status_valid = board_diag_charge_status_valid();
    out->charger_forced = s_charger_forced;
    charger_control_snapshot_t charger_control;
    charger_control_service_get_snapshot(&charger_control);
    out->charger_enabled_requested = charger_control.requested_enabled;
    out->charger_enabled = charger_control.actual_enabled;
    out->charger_enable_valid = charger_control.readback_valid;
    out->charger_inhibit_owner_mask =
        charger_control.inhibit_owner_mask;
    out->charger_control_attempts = charger_control.apply_attempts;
    out->charger_control_failures = charger_control.apply_failures;
    out->charger_control_mismatches = charger_control.readback_mismatches;
    out->chr_stat1 = s_chr_stat1;
    out->chr_stat2 = s_chr_stat2;
    if (charger_control.readback_valid &&
        !charger_control.actual_enabled) {
        out->charge_state = BOARD_DIAG_CHARGE_DISABLED;
    } else if (s_charger_forced) {
        out->charge_state = s_charger_connected ?
            (s_battery_mv >= BOARD_DIAG_BATTERY_MV_MAX ? BOARD_DIAG_CHARGE_FULL : BOARD_DIAG_CHARGE_ACTIVE) :
            BOARD_DIAG_CHARGE_DETACHED;
    } else {
        out->charge_state = charge_state_from_inputs(
            s_charger_connected, s_chr_stat1, s_chr_stat2);
    }
    out->chemistry = s_chemistry;
    out->simulated = s_charger_forced;
    out->vbus_present = s_vbus_present;
    out->vbus_forced = s_vbus_forced;
    out->headset_inserted = s_headset_inserted;
    out->headset_forced =
        accessory_hal_debug_get_override() != ACCESSORY_DEBUG_OVERRIDE_AUTO;
    out->headset_hook_pressed = s_headset_hook_pressed;
    out->headset_hook_mv = s_headset_hook_mv;
    out->sys_int_asserted =
        power_sleep_hal_shared_irq_asserted();
    out->tca_present = s_tca_present;
    out->tps63020_pwm_mode =
        board_3v8_rail_force_pwm_enabled();
    out->rail_3v8_power_good =
        board_3v8_rail_power_good();
    out->rail_3v8_enabled =
        board_3v8_rail_enabled();
    out->modem_status_monitor_available =
        MODEM_STATUS_MONITOR_AVAILABLE != 0u;
    out->modem_status_adc_raw = s_modem_status_adc_raw;
    out->modem_status_pin_mv = s_modem_status_pin_mv;
    out->modem_dtr_level = board_modem_dtr_level();
    out->modem_ri_level = board_modem_ri_level();
    out->modem_pwr_control_asserted =
        board_modem_on_off_asserted();
    out->modem_hw_shutdown_asserted = board_modem_hw_shutdown_asserted();
    board_irq_snapshot_t gpio_irq;
    board_irq_hal_get_snapshot(&gpio_irq);
    out->modem_ri_edges = gpio_irq.modem_ri_falling_edges;
    out->shared_irq_edges = gpio_irq.shared_falling_edges;
    out->service_vbus_edges = gpio_irq.service_vbus_edges;
    shared_irq_service_snapshot_t shared_irq;
    shared_irq_service_get_snapshot(&shared_irq);
    out->shared_irq_drains = shared_irq.drain_count;
    out->shared_irq_stuck = shared_irq.stuck_count;
    out->shared_irq_tca_events =
        shared_irq.tca_service_count;
    out->shared_irq_rtc_events =
        saturating_add_u32(shared_irq.rtc_alarm_events,
                           shared_irq.rtc_timer_events);
    out->shared_irq_ltc_events = shared_irq.ltc_alerts;
    for (uint8_t i = 0u; i < 3u; i++) {
        out->shared_irq_source_errors[i] =
            shared_irq.source_errors[i];
    }
    out->shared_irq_last_serviced =
        shared_irq.last_serviced_mask;
    out->shared_irq_last_errors =
        shared_irq.last_error_mask;
    out->shared_irq_last_tca_status =
        shared_irq.last_tca_int_status;
    out->shared_irq_last_rtc_flags =
        shared_irq.last_rtc_flags;
    out->shared_irq_last_ltc_status =
        shared_irq.last_ltc_status;
    out->backlight_on = backlight_service_is_on();
    out->backlight_override = s_backlight_override;
    out->backlight_override_on = s_backlight_override_on;
    out->vibra_pin_configured = VIBRA_PIN_CONFIGURED != 0u;
    out->vibra_test_enabled = s_vibra_test_enabled;
    out->vibra_strength = VIBRA_HAL_STRENGTH_STOCK;
    out->buzzer_pin_configured = BUZZER_PIN_CONFIGURED != 0u;
    out->codec_present = nau88c22_codec_ready();
    out->rtc_present = rtc_alarm_hal_chip_available();
}

void board_diag_debug_force_charger_connected(bool connected,
                                               uint32_t now_ms) {
    if (!s_charger_forced && !s_battery_valid) {
        s_battery_mv = 2400u;
        s_battery_mv_raw = s_battery_mv;
        s_battery_mv_fresh = s_battery_mv;
    }
    s_charger_forced = true;
    s_battery_valid = true;
    s_charger_connected = connected;
    s_last_sim_ms = now_ms;
    note_battery_sample();
}

void board_diag_debug_clear_charger_force(void) {
    bool was_forced = s_charger_forced;
    s_charger_forced = false;
    s_charger_connected = battery_hal_charger_connected();
    if (!s_batt_force_active) {
        s_battery_valid = battery_hal_valid();
        s_battery_mv = battery_hal_millivolts();
        s_battery_mv_raw = battery_hal_millivolts_raw();
        s_battery_mv_fresh = battery_hal_millivolts_fresh();
    }
    if (was_forced) {
        note_battery_sample();
    }
}

bool board_diag_debug_set_charger_enabled(bool enabled) {
    return charger_control_service_set_inhibit(
        CHARGER_INHIBIT_DEBUG, !enabled);
}

bool board_diag_restore_charger_default(void) {
    return charger_control_service_set_inhibit(
        CHARGER_INHIBIT_DEBUG, false);
}

bool board_diag_debug_set_tps63020_pwm_mode(bool enabled) {
    board_3v8_rail_set_force_pwm(enabled);
    return board_3v8_rail_force_pwm_enabled();
}

void board_diag_debug_force_battery_mv(uint16_t mv) {
    s_batt_force_active = true;
    s_batt_force_mv = mv;
    s_battery_mv = mv;
    s_battery_mv_raw = mv;
    s_battery_mv_fresh = mv;
    s_battery_valid = true;
    s_force_sample_ms = time_ms();
    note_battery_sample();
}

void board_diag_debug_clear_battery_force(void) {
    bool was_forced = s_batt_force_active;
    s_batt_force_active = false;
    if (!s_charger_forced) {
        s_battery_valid = battery_hal_valid();
        s_battery_mv = battery_hal_millivolts();
        s_battery_mv_raw = battery_hal_millivolts_raw();
        s_battery_mv_fresh = battery_hal_millivolts_fresh();
    }
    if (was_forced) {
        note_battery_sample();
    }
}

void board_diag_debug_set_backlight_override(bool active, bool on) {
    s_backlight_override = active;
    s_backlight_override_on = active && on;
}

bool board_diag_backlight_override_active(void) {
    return s_backlight_override;
}

bool board_diag_backlight_override_on(void) {
    return s_backlight_override_on;
}

void board_diag_debug_set_vibra_test(bool enabled) {
    s_vibra_test_enabled = enabled;
}

uint8_t board_diag_vibra_strength(void) {
    return VIBRA_HAL_STRENGTH_STOCK;
}

const char *board_diag_chemistry_text(board_diag_chemistry_t chemistry) {
    switch (chemistry) {
    case BOARD_DIAG_CHEM_LI_ION:
        return "Li-ion";
    case BOARD_DIAG_CHEM_NI_MH:
        return "NiMH";
    case BOARD_DIAG_CHEM_ALK:
        return "Alk";
    default:
        return "---";
    }
}

const char *board_diag_charge_state_text(board_diag_charge_state_t state) {
    switch (state) {
    case BOARD_DIAG_CHARGE_DETACHED:
        return "detached";
    case BOARD_DIAG_CHARGE_ACTIVE:
        return "charging";
    case BOARD_DIAG_CHARGE_FULL:
        return "full";
    case BOARD_DIAG_CHARGE_FAULT:
        return "fault";
    case BOARD_DIAG_CHARGE_DISABLED:
        return "disabled";
    default:
        return "---";
    }
}

static uint8_t level_bars_from_mv(uint16_t mv) {
    return battery_hal_level_bars_from_mv(mv); /* single source of truth in battery_hal */
}

static board_diag_charge_state_t charge_state_from_inputs(bool present,
                                                          uint8_t stat1,
                                                          uint8_t stat2) {
    switch (battery_hal_charger_state_from_inputs(
        present,
        battery_hal_charge_status_decode(stat1 != 0u, stat2 != 0u))) {
    case BATTERY_CHARGER_ACTIVE:
        return BOARD_DIAG_CHARGE_ACTIVE;
    case BATTERY_CHARGER_FULL:
        return BOARD_DIAG_CHARGE_FULL;
    case BATTERY_CHARGER_FAULT:
        return BOARD_DIAG_CHARGE_FAULT;
    case BATTERY_CHARGER_DETACHED:
    default:
        return BOARD_DIAG_CHARGE_DETACHED;
    }
}

static void refresh_cached_inputs(void) {
    if (!s_vbus_forced) {
        s_vbus_present = board_service_vbus_present();
    }
    bool stat1_high = true;
    bool stat2_high = true;
    bool status_valid = battery_hal_charge_status_levels(
        &stat1_high, &stat2_high);
    s_chr_stat1 = stat1_high ? 1u : 0u;
    s_chr_stat2 = stat2_high ? 1u : 0u;
    /* Headset insert + hook come from accessory_hal (HEAD_INT + GP41/ADC1),
     * not synthesized here. The press edge is consumed by call control, so the
     * diagnostic shows only the live insert state + hook line voltage. */
    s_headset_inserted = accessory_hal_headset_inserted();
    s_headset_hook_pressed = accessory_hal_hook_pressed();
    s_headset_hook_mv = accessory_hal_hook_mv();
    if (status_valid) {
        s_tca_present = true;
    }
}

static void step_forced_battery(uint32_t now_ms) {
    if (time_diff_ms(now_ms, s_last_sim_ms + BOARD_DIAG_SIM_STEP_MS) < 0) {
        return;
    }
    s_last_sim_ms = now_ms;
    note_battery_sample();
    charger_control_snapshot_t control;
    charger_control_service_get_snapshot(&control);
    if (s_charger_connected && control.actual_enabled) {
        if (s_battery_mv + BOARD_DIAG_SIM_CHARGE_STEP_MV < BOARD_DIAG_BATTERY_MV_MAX) {
            s_battery_mv = (uint16_t)(s_battery_mv + BOARD_DIAG_SIM_CHARGE_STEP_MV);
        } else {
            s_battery_mv = BOARD_DIAG_BATTERY_MV_MAX;
        }
    } else if (s_battery_mv > BOARD_DIAG_BATTERY_MV_MIN + BOARD_DIAG_SIM_DISCHARGE_STEP_MV) {
        s_battery_mv = (uint16_t)(s_battery_mv - BOARD_DIAG_SIM_DISCHARGE_STEP_MV);
    } else {
        s_battery_mv = BOARD_DIAG_BATTERY_MV_MIN;
    }
}

static void note_battery_sample(void) {
    s_battery_sample_sequence++;
    if (s_battery_sample_sequence == 0u) {
        s_battery_sample_sequence = 1u;
    }
}

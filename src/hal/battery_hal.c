#include "hal/battery_hal.h"

#include "hardware/adc.h"
#include "hardware/gpio.h"
#include "hal/bq25171_stat.h"
#include "hal/battery_gauge_logic.h"
#include "hal/battery_poll_logic.h"
#include "hal/board.h"
#include "hal/charger_input_logic.h"
#include "hal/ltc2959_hal.h"
#include "hal/tca8418_hal.h"
#include "services/timebase.h"

#define CHARGER_ADC_SETTLE_SAMPLES 4u
#define CHARGER_ADC_BURST_SAMPLES 8u
/* A STAT edge can precede VIN_CHRG_DET's analog decay. Keep a short bounded
 * confirmation window so an immediate first sample cannot defer removal until
 * the 60 s maintenance read. Four 80 ms-spaced samples still give UI feedback
 * well inside one second and cost power only on a real charger transition. */
#define CHARGER_IRQ_SETTLE_MS (3u * BATTERY_ADC_CONFIRM_MS)

_Static_assert(BATTERY_ADC_CONFIRM_MS >= CHARGER_INPUT_DEBOUNCE_MS,
               "ADC confirmation cadence must satisfy charger debounce");

#ifndef SISU_LTC2959_VOLTAGE_AUTHORITATIVE
/* Production CMake defines this explicitly. Keep an ad-hoc compile fail-safe:
 * raw telemetry remains visible, but battery warnings and shutdown policy do
 * not run unless the build deliberately opts in. */
#define SISU_LTC2959_VOLTAGE_AUTHORITATIVE 0
#endif

static charger_input_filter_t s_charger_filter;
static uint16_t s_charger_adc_raw;
static uint16_t s_charger_pin_mv;
static uint32_t s_charger_input_mv;
static battery_charge_status_t s_charge_status;
static bool s_charge_status_valid;
static bool s_stat1_high;
static bool s_stat2_high;
static bool s_battery_valid;
static uint16_t s_battery_mv;
static uint16_t s_battery_mv_raw;
static uint16_t s_battery_mv_fresh;
static uint8_t s_battery_bars;
static battery_gauge_state_t s_gauge_state;
static battery_gauge_snapshot_t s_gauge_snapshot;
static battery_power_on_qualifier_t s_power_on_qualifier;
static uint32_t s_ltc_conversion_sequence;
static uint32_t s_ltc_sample_sequence;
static uint32_t s_ltc_gauge_session;
static battery_sample_mode_t s_sample_mode;
static battery_poll_schedule_t s_poll_schedule;
static bool s_charger_irq_settling;
static uint32_t s_charger_irq_settle_until_ms;

static void sample_charger_input_adc(void) {
    adc_select_input(CHARGER_INPUT_ADC_INPUT);
    for (uint32_t i = 0u; i < CHARGER_ADC_SETTLE_SAMPLES; i++) {
        (void)adc_read();
    }
    uint32_t adc_sum = 0u;
    for (uint32_t i = 0u; i < CHARGER_ADC_BURST_SAMPLES; i++) {
        adc_sum += adc_read();
    }
    s_charger_adc_raw =
        (uint16_t)(adc_sum / CHARGER_ADC_BURST_SAMPLES);
    s_charger_pin_mv =
        charger_input_pin_mv_from_adc_raw(s_charger_adc_raw);
    s_charger_input_mv =
        charger_input_mv_from_adc_raw(s_charger_adc_raw);
}

static void sample_charger_input(uint32_t now_ms, bool force_confirm) {
    sample_charger_input_adc();
    (void)charger_input_filter_update(
        &s_charger_filter, s_charger_input_mv, now_ms, NULL);
    battery_poll_note_adc(
        &s_poll_schedule, now_ms,
        s_charger_filter.candidate_active || force_confirm);
}

void battery_hal_init(void) {
    adc_init();
    adc_gpio_init(CHARGER_INPUT_ADC_PIN);
    gpio_set_input_enabled(CHARGER_INPUT_ADC_PIN, false);
    charger_input_filter_init(&s_charger_filter, false);
    s_charger_adc_raw = 0u;
    s_charger_pin_mv = 0u;
    s_charger_input_mv = 0u;
    s_charge_status = BATTERY_CHARGE_IDLE;
    s_charge_status_valid = false;
    s_stat1_high = true;
    s_stat2_high = true;
    s_battery_valid = false;
    s_battery_mv = 0u;
    s_battery_mv_raw = 0u;
    s_battery_mv_fresh = 0u;
    s_battery_bars = 0u;
    battery_gauge_init(&s_gauge_state);
    s_gauge_snapshot = (battery_gauge_snapshot_t){0};
    battery_power_on_qualifier_init(&s_power_on_qualifier);
    s_ltc_conversion_sequence = 0u;
    s_ltc_sample_sequence = 0u;
    s_ltc_gauge_session = 0u;
    s_sample_mode = BATTERY_SAMPLE_MODE_QUIET;
    s_charger_irq_settling = false;
    s_charger_irq_settle_until_ms = 0u;
    battery_poll_schedule_init(&s_poll_schedule);
    (void)ltc2959_hal_init(0u);
    battery_hal_poll(0u);
}

void battery_hal_set_sample_mode(battery_sample_mode_t mode) {
    if (mode > BATTERY_SAMPLE_MODE_HIGH_LOAD) {
        mode = BATTERY_SAMPLE_MODE_QUIET;
    }
    s_sample_mode = mode;
}

void battery_hal_poll(uint32_t now_ms) {
    bool charger_irq = tca8418_hal_take_charger_status_change();
    if (charger_irq) {
        s_charger_irq_settling = true;
        s_charger_irq_settle_until_ms = now_ms + CHARGER_IRQ_SETTLE_MS;
    }
    if (charger_irq || battery_poll_status_due(&s_poll_schedule, now_ms)) {
        battery_charge_status_t previous = s_charge_status;
        bool was_valid = s_charge_status_valid;
        bool stat1_high = true;
        bool stat2_high = true;
        if (tca8418_hal_charger_status(&stat1_high, &stat2_high)) {
            s_stat1_high = stat1_high;
            s_stat2_high = stat2_high;
            s_charge_status = battery_hal_charge_status_decode(
                stat1_high, stat2_high);
            s_charge_status_valid = true;
        } else {
            s_charge_status_valid = false;
        }
        battery_poll_note_status(
            &s_poll_schedule,
            now_ms,
            s_charge_status_valid &&
                (!was_valid || s_charge_status != previous));
    }

    /* A raw STAT transition is also evidence that VIN may have appeared or
     * vanished. Do not wait for the 60 s analog-maintenance cadence, including
     * when the decoded status did not change or its I2C read failed. */
    if (charger_irq) {
        battery_poll_request_adc(&s_poll_schedule, now_ms);
    }

    if (battery_poll_adc_due(&s_poll_schedule, now_ms)) {
        sample_charger_input(now_ms, s_charger_irq_settling);
        if (s_charger_irq_settling &&
            time_diff_ms(now_ms, s_charger_irq_settle_until_ms) >= 0) {
            s_charger_irq_settling = false;
        }
    }

    bool charging = s_charge_status_valid &&
                    s_charge_status == BATTERY_CHARGE_ACTIVE;
    uint32_t sample_period_ms =
        battery_sample_period_ms(s_sample_mode, charging);
    if (battery_power_on_qualifier_sample_count(&s_power_on_qualifier) <
            BATTERY_POWER_ON_REQUIRED_VALID &&
        battery_power_on_qualifier_attempt_count(&s_power_on_qualifier) <
            BATTERY_POWER_ON_MAX_ATTEMPTS &&
        sample_period_ms > BATTERY_SAMPLE_HIGH_LOAD_MS) {
        /* The original admits power from five valid conversions out of at most
         * ten attempts. Finish that bounded acquisition while core 0 is already
         * awake; this deadline is never added to dormant wake ownership. */
        sample_period_ms = BATTERY_SAMPLE_HIGH_LOAD_MS;
    }
    ltc2959_hal_poll(now_ms, sample_period_ms);
    ltc2959_snapshot_t ltc;
    ltc2959_hal_get_snapshot(&ltc);
    if (ltc.gauge_session != s_ltc_gauge_session) {
        s_ltc_gauge_session = ltc.gauge_session;
        battery_gauge_init(&s_gauge_state);
        s_gauge_snapshot = (battery_gauge_snapshot_t){0};
        battery_power_on_qualifier_init(&s_power_on_qualifier);
        s_battery_valid = false;
    }
    if (ltc.conversion_sequence != s_ltc_conversion_sequence) {
        s_ltc_conversion_sequence = ltc.conversion_sequence;
        battery_power_on_qualifier_observe(
            &s_power_on_qualifier, ltc.gauge_session,
            ltc.conversion_sequence, ltc.last_conversion_sample_valid,
            ltc.voltage_mv);
    }
    if (ltc.sample_sequence != s_ltc_sample_sequence) {
        s_ltc_sample_sequence = ltc.sample_sequence;
        if (ltc.sample_valid) {
            s_battery_mv_raw = ltc.voltage_mv;
            s_battery_mv_fresh = ltc.voltage_mv;
        }
        battery_gauge_observation_t observation = {
            .now_ms = now_ms,
            .sample_sequence = ltc.sample_sequence,
            .gauge_session = ltc.gauge_session,
            .terminal_mv = ltc.voltage_mv,
            .current_ua = ltc.current_ua,
            .session_delta_nah = ltc.session_delta_nah,
            .sample_valid = ltc.sample_valid,
            .current_valid = ltc.sample_valid &&
                             ltc.current_polarity_verified,
            .continuity_valid = ltc.continuity_valid,
            .charge_active = charging,
        };
        (void)battery_gauge_update(
            &s_gauge_state, battery_gauge_nimh_profile(), &observation);
        battery_gauge_get_snapshot(&s_gauge_state, &s_gauge_snapshot);
        if (s_gauge_snapshot.valid) {
            s_battery_mv = s_gauge_snapshot.reference_mv;
            s_battery_bars = s_gauge_snapshot.bars;
        }
    }
    s_battery_valid =
        SISU_LTC2959_VOLTAGE_AUTHORITATIVE != 0 &&
        ltc.sample_valid &&
        s_gauge_snapshot.valid;
}

bool battery_hal_standby_ready(void) {
    return !s_charger_irq_settling &&
           !s_charger_filter.candidate_active;
}

bool battery_hal_valid(void) {
    return s_battery_valid;
}

uint16_t battery_hal_millivolts(void) {
    return s_battery_mv;
}

uint16_t battery_hal_millivolts_raw(void) {
    return s_battery_mv_raw;
}

uint16_t battery_hal_millivolts_fresh(void) {
    return s_battery_mv_fresh;
}

uint32_t battery_hal_sample_sequence(void) {
    return s_ltc_sample_sequence;
}

uint8_t battery_hal_level_bars(void) {
    return s_battery_valid ? s_battery_bars : 0u;
}

bool battery_hal_low(void) {
    return s_battery_valid &&
           s_gauge_snapshot.warning == BATTERY_GAUGE_WARNING_LOW;
}

bool battery_hal_empty(void) {
    return s_battery_valid &&
           s_gauge_snapshot.warning == BATTERY_GAUGE_WARNING_EMPTY;
}

int16_t battery_hal_load_correction_mv(void) {
    return s_gauge_snapshot.correction_mv;
}

int32_t battery_hal_window_current_ua(void) {
    return s_gauge_snapshot.window_current_ua;
}

bool battery_hal_window_current_valid(void) {
    return s_gauge_snapshot.window_current_valid;
}

uint8_t battery_hal_power_on_sample_count(void) {
    return battery_power_on_qualifier_sample_count(&s_power_on_qualifier);
}

uint8_t battery_hal_power_on_attempt_count(void) {
    return battery_power_on_qualifier_attempt_count(&s_power_on_qualifier);
}

uint16_t battery_hal_power_on_average_mv(void) {
    return battery_power_on_qualifier_average_mv(&s_power_on_qualifier);
}

bool battery_hal_charger_connected(void) {
    return battery_hal_charger_present();
}

bool battery_hal_charger_present(void) {
    return s_charger_filter.stable_present;
}

bool battery_hal_charger_input_present_now(uint32_t now_ms) {
    sample_charger_input(now_ms, false);
    return s_charger_input_mv >= CHARGER_INPUT_ATTACH_MV;
}

uint16_t battery_hal_charger_adc_raw(void) {
    return s_charger_adc_raw;
}

uint16_t battery_hal_charger_pin_mv(void) {
    return s_charger_pin_mv;
}

uint32_t battery_hal_charger_input_mv(void) {
    return s_charger_input_mv;
}

battery_charge_status_t battery_hal_charge_status(void) {
    return s_charge_status;
}

bool battery_hal_charge_status_valid(void) {
    /* STAT=11 means either charge complete or charger disabled/removed. VIN
     * falls before its debounced presence bit, so accepting STAT=11 while a
     * detach is pending briefly turns every unplug into "Battery full". Keep
     * the previous app state through that bounded window; once VIN qualifies
     * absent, DETACHED wins independently of status validity. */
    return s_charge_status_valid &&
           !charger_input_filter_detach_pending(&s_charger_filter);
}

bool battery_hal_charge_status_levels(bool *stat1_high, bool *stat2_high) {
    if (stat1_high != NULL) {
        *stat1_high = s_stat1_high;
    }
    if (stat2_high != NULL) {
        *stat2_high = s_stat2_high;
    }
    return s_charge_status_valid;
}

uint8_t battery_hal_level_bars_from_mv(uint16_t mv) {
    return battery_gauge_bars_from_mv(mv);
}

battery_power_on_gate_t battery_hal_power_on_gate_from_mv(uint16_t mv,
                                                          bool charger) {
    if (charger) {
        return BATTERY_POWER_ON_OK;
    }
    return (mv < VBAT_POWERON_MIN_MV) ? BATTERY_POWER_ON_REFUSE
                                      : BATTERY_POWER_ON_OK;
}

battery_power_on_gate_t battery_hal_power_on_gate(bool charger) {
    if (charger) {
        return BATTERY_POWER_ON_OK;
    }
    if (!battery_power_on_qualifier_ready(&s_power_on_qualifier)) {
        return BATTERY_POWER_ON_REFUSE;
    }
    return battery_hal_power_on_gate_from_mv(
        battery_power_on_qualifier_average_mv(&s_power_on_qualifier), false);
}

battery_charge_status_t battery_hal_charge_status_decode(bool stat1_high,
                                                         bool stat2_high) {
    return bq25171_stat_decode(stat1_high, stat2_high);
}

battery_charger_state_t battery_hal_charger_state_from_inputs(
    bool charger_present,
    battery_charge_status_t status) {
    return bq25171_charger_state_from_inputs(charger_present, status);
}

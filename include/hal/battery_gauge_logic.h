#ifndef BATTERY_GAUGE_LOGIC_H
#define BATTERY_GAUGE_LOGIC_H

#include <stdbool.h>
#include <stdint.h>

/* Pure Rev B2 battery-estimator policy. The Nokia-derived constants and state
 * machine live here; battery_hal only supplies coherent LTC2959 observations.
 * No SDK or hardware dependency is permitted in this interface. */

/* Active NSE-8 NiMH PM-0x205 voltage record. These thresholds are calibrated
 * in Nokia's 65 mA reference-load domain, not at open circuit. */
#define VBAT_BAR4_MV 2575u
#define VBAT_BAR3_MV 2530u
#define VBAT_BAR2_MV 2425u
#define VBAT_BAR1_MV 1900u
#define VBAT_LOW_MV 2320u
#define VBAT_LOW_TERMINAL_QUALIFY_MV 2230u
#define VBAT_EMPTY_MV VBAT_BAR1_MV
#define VBAT_POWERON_MIN_MV 2100u

/* Active Nokia NiMH load model and timing. */
#define BATTERY_GAUGE_REFERENCE_CURRENT_MA 65
#define BATTERY_GAUGE_EFFECTIVE_RESISTANCE_MOHM 190
#define BATTERY_GAUGE_CHARGE_COMPENSATION_PERCENT 85
#define BATTERY_GAUGE_MAX_COMPENSATION_MV 150
#define BATTERY_GAUGE_MODEL_PERIOD_MS 1000u
#define BATTERY_GAUGE_MEDIAN_WINDOW_MS 15000u
#define BATTERY_GAUGE_WARNING_ENTRY_MS 20000u
#define BATTERY_GAUGE_STARTUP_BIAS_MV 23u
#define BATTERY_GAUGE_RISE_REARM_NAH INT64_C(150000000)

#define BATTERY_GAUGE_MEDIAN_CAPACITY 15u
#define BATTERY_GAUGE_CURRENT_HISTORY_CAPACITY 8u
#define BATTERY_POWER_ON_REQUIRED_VALID 5u
#define BATTERY_POWER_ON_MAX_ATTEMPTS 10u

typedef enum {
    BATTERY_GAUGE_WARNING_HEALTHY = 0,
    BATTERY_GAUGE_WARNING_LOW,
    BATTERY_GAUGE_WARNING_EMPTY,
} battery_gauge_warning_t;

typedef struct {
    uint16_t reference_current_ma;
    uint16_t effective_resistance_mohm;
    uint16_t max_compensation_mv;
    uint8_t charge_compensation_percent;
    int64_t rise_rearm_nah;
} battery_gauge_profile_t;

typedef struct {
    uint32_t now_ms;
    uint32_t sample_sequence;
    uint32_t gauge_session;
    uint16_t terminal_mv;
    int32_t current_ua;        /* public convention: +charge, -discharge */
    int64_t session_delta_nah; /* same sign convention as current */
    bool sample_valid;
    bool current_valid;
    bool continuity_valid;
    bool charge_active;
} battery_gauge_observation_t;

typedef struct {
    bool valid;
    uint16_t terminal_mv;
    uint16_t fast_terminal_mv;
    uint16_t symmetric_terminal_mv;
    uint16_t reference_mv;
    int16_t correction_mv;
    int32_t window_current_ua;
    bool window_current_valid;
    uint8_t bars;
    battery_gauge_warning_t warning;
    uint32_t model_sequence;
} battery_gauge_snapshot_t;

typedef struct {
    uint8_t committed;
    int64_t rearm_anchor_nah;
    bool seeded;
    bool rearm_anchor_valid;
} battery_bar_state_t;

typedef struct {
    battery_gauge_warning_t committed;
    uint32_t pending_since_ms;
    bool seeded;
    bool pending;
} battery_warning_state_t;

typedef struct {
    uint16_t value_mv;
    uint32_t observed_ms;
} battery_gauge_median_point_t;

typedef struct {
    int64_t charge_nah;
    uint32_t observed_ms;
} battery_gauge_current_point_t;

typedef struct {
    battery_gauge_snapshot_t snapshot;
    battery_bar_state_t bar;
    battery_warning_state_t warning;
    battery_gauge_median_point_t median[BATTERY_GAUGE_MEDIAN_CAPACITY];
    battery_gauge_current_point_t current_history[
        BATTERY_GAUGE_CURRENT_HISTORY_CAPACITY];
    int32_t fast_peak_x10;
    int32_t fast_symmetric_x10;
    int32_t slow_reference_x10;
    uint32_t gauge_session;
    uint32_t last_sample_sequence;
    uint32_t last_model_ms;
    uint8_t median_count;
    uint8_t current_history_count;
    bool session_seen;
    bool sample_seen;
    bool model_seeded;
} battery_gauge_state_t;

typedef struct {
    uint16_t samples_mv[BATTERY_POWER_ON_REQUIRED_VALID];
    uint32_t gauge_session;
    uint32_t last_attempt_sequence;
    uint32_t sum_mv;
    uint8_t sample_count;
    uint8_t next_sample;
    uint8_t attempts;
    bool session_seen;
    bool sample_seen;
} battery_power_on_qualifier_t;

const battery_gauge_profile_t *battery_gauge_nimh_profile(void);

/* Direct PM-0x205 ladder helpers. */
uint8_t battery_gauge_bars_from_mv(uint16_t mv);
uint16_t battery_gauge_bar_floor_mv(uint8_t bar);

/* Signed correction from terminal voltage to the profile's reference-load
 * domain. window_current_ua is used conservatively when valid. */
int16_t battery_gauge_load_correction_mv(
    const battery_gauge_profile_t *profile,
    int32_t current_ua,
    int32_t window_current_ua,
    bool window_current_valid,
    bool charge_active);

void battery_gauge_bar_state_init(battery_bar_state_t *state);
uint8_t battery_gauge_bar_step(
    battery_bar_state_t *state,
    const battery_gauge_profile_t *profile,
    uint16_t reference_mv,
    bool charge_active,
    bool continuity_valid,
    int64_t session_delta_nah);

void battery_gauge_warning_state_init(battery_warning_state_t *state);
battery_gauge_warning_t battery_gauge_warning_step(
    battery_warning_state_t *state,
    uint32_t now_ms,
    uint16_t terminal_mv,
    uint16_t fast_terminal_mv,
    uint16_t symmetric_terminal_mv,
    uint16_t reference_mv);

void battery_gauge_init(battery_gauge_state_t *state);
/* Consume one distinct LTC observation. Sub-second observations remain
 * available as fresh safety evidence in the HAL; the Nokia-derived estimator
 * advances at most once per second. Returns true when model output changed. */
bool battery_gauge_update(
    battery_gauge_state_t *state,
    const battery_gauge_profile_t *profile,
    const battery_gauge_observation_t *observation);
void battery_gauge_get_snapshot(
    const battery_gauge_state_t *state,
    battery_gauge_snapshot_t *out);

void battery_power_on_qualifier_init(battery_power_on_qualifier_t *state);
void battery_power_on_qualifier_observe(
    battery_power_on_qualifier_t *state,
    uint32_t gauge_session,
    uint32_t attempt_sequence,
    bool sample_valid,
    uint16_t terminal_mv);
/* Ready after five valid observations, or after ten attempts with at least one
 * valid observation, matching the v6.00 bounded acquisition shape. */
bool battery_power_on_qualifier_ready(
    const battery_power_on_qualifier_t *state);
uint8_t battery_power_on_qualifier_sample_count(
    const battery_power_on_qualifier_t *state);
uint8_t battery_power_on_qualifier_attempt_count(
    const battery_power_on_qualifier_t *state);
uint16_t battery_power_on_qualifier_average_mv(
    const battery_power_on_qualifier_t *state);

#endif

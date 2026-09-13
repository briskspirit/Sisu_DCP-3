#ifndef BATTERY_STATUS_LOGIC_H
#define BATTERY_STATUS_LOGIC_H

#include <stdbool.h>
#include <stdint.h>

#define BATTERY_RAW_COLLAPSE_CONFIRM_SAMPLES 2u
/* Rev B2's ordinary operating floor. It deliberately matches the power-on
 * admission threshold: below this point the pack cannot be trusted to sustain
 * the 3V8 modem rail or an orderly endpoint commit. */
#define BATTERY_OPERATIONAL_EMPTY_MV 2100u
/* During a call, preserve the original UX rule that LOW alone never ends the
 * call. This lower emergency floor remains independent of compensation.
 * [BP] until converter-dropout testing closes the true hardware boundary. */
#define BATTERY_RAW_COLLAPSE_MV 1800u

typedef struct {
    uint32_t last_sequence;
    uint16_t last_threshold_mv;
    uint8_t consecutive_low;
    bool sequence_seen;
} battery_raw_collapse_evidence_t;

void battery_raw_collapse_evidence_init(
    battery_raw_collapse_evidence_t *evidence);
/* Returns true only after distinct consecutive samples are below threshold.
 * Re-polling one cached sample cannot manufacture confirmation. */
bool battery_raw_collapse_evidence_update(
    battery_raw_collapse_evidence_t *evidence,
    bool sample_valid,
    uint32_t sample_sequence,
    uint16_t sample_mv,
    uint16_t threshold_mv);

/* Ordinary idle operation shuts down before the measured Rev B2 brownout
 * region. A live call transaction retains only the hard emergency floor. */
uint16_t battery_empty_shutdown_threshold_mv(bool call_active);

typedef enum {
    BATTERY_ENDPOINT_NONE = 0,
    /* Coulomb projection reached zero, but physical EMPTY is not confirmed. */
    BATTERY_ENDPOINT_CAPACITY_PENDING,
    /* Voltage or modem-rail evidence conflicts with trusted remaining charge. */
    BATTERY_ENDPOINT_LOAD_SAG,
    /* A physical endpoint is qualified and may be learned as EMPTY. */
    BATTERY_ENDPOINT_NATURAL_EMPTY,
    /* The electrical floor requires shutdown but must not teach capacity. */
    BATTERY_ENDPOINT_EMERGENCY_SHUTDOWN,
} battery_endpoint_kind_t;

typedef struct {
    battery_endpoint_kind_t armed_kind;
    bool load_sag_latched;
} battery_endpoint_state_t;

typedef struct {
    bool battery_sample_valid;
    /* Physical VIN is not recovery evidence. This is true only while the
     * enabled charger is actively delivering positive current to the pack. */
    bool charge_recovery_active;
    bool powered_off;
    bool call_active;
    bool gauge_low;
    bool gauge_empty;
    bool operating_floor_confirmed;
    bool modem_supply_collapse;
    bool capacity_prediction_exhausted;
    bool anchored_capacity_has_charge;
    bool shutdown_armed;
} battery_endpoint_evidence_t;

typedef struct {
    battery_endpoint_kind_t kind;
    bool shutdown;
    bool learn_empty;
    bool force_low;
    bool capacity_voltage_disagreement;
} battery_endpoint_decision_t;

void battery_endpoint_state_init(battery_endpoint_state_t *state);

/* Fuse independent coulomb, voltage, load-context, and rail evidence. Capacity
 * exhaustion alone is a prediction; outside a call, a separately committed
 * voltage LOW corroborates it early enough to create a learned endpoint.
 * Trusted remaining charge suppresses call/load-sag EMPTY, while the absolute
 * operating floor retains authority to stop unsafe hardware operation. */
battery_endpoint_decision_t battery_endpoint_step(
    battery_endpoint_state_t *state,
    const battery_endpoint_evidence_t *evidence);

typedef enum {
    BATTERY_WARNING_ACTION_SUPPRESS = 0,
    BATTERY_WARNING_ACTION_EMPTY,
    BATTERY_WARNING_ACTION_HOLD,
    BATTERY_WARNING_ACTION_LOW,
    BATTERY_WARNING_ACTION_HEALTHY,
} battery_warning_action_t;

/* Select the app-side notification action after the estimator and independent
 * safety paths have published their evidence. Missing gauge data must preserve
 * an already-armed LOW/EMPTY state; it is not proof of recovery. */
battery_warning_action_t battery_warning_action(
    bool battery_sample_valid,
    bool charge_recovery_active,
    bool powered_off,
    bool empty_evidence,
    bool low_evidence);

/* A connected or STAT-active charger can still leave the battery discharging.
 * Suppress LOW/EMPTY only when control, status, and LTC current agree that the
 * pack is actually recovering. */
bool battery_charge_recovery_active(
    bool charger_active,
    bool charger_enable_valid,
    bool charger_enabled,
    bool current_valid,
    int32_t current_ua);

/* Remaining charge is safety evidence even when the icon quantizer cannot
 * publish bars. UI validity must never decide whether an in-call voltage sag
 * is allowed to teach EMPTY. */
bool battery_anchored_remaining_has_charge(
    bool remaining_valid,
    bool soc_anchored,
    uint64_t remaining_nah);

/* A physically attached but non-recovering charger must not leave the charge
 * supervisor's /CE latch holding a low pack off. */
bool battery_charge_floor_release_needed(
    bool charger_present,
    bool charge_recovery_active,
    bool low_zone,
    bool empty_zone);

/* Keep the last qualified standby icon across a transient gauge gap. The
 * diagnostic API still reports invalidity separately and must not present this
 * cached value as a fresh measurement. */
uint8_t battery_display_bars(
    bool soc_bars_valid,
    uint8_t soc_bars,
    bool battery_sample_valid,
    uint8_t measured_bars,
    uint8_t cached_bars);

typedef struct {
    bool notified;
    bool show;
} battery_full_notice_decision_t;

typedef struct {
    bool ui_completed;
    bool learn_full;
} battery_charge_completion_decision_t;

/* Legacy BQ completion remains useful while the supervisor is observe-only.
 * An enforcing supervisor owns both the UI and learner FULL decisions. */
battery_charge_completion_decision_t battery_charge_completion_decision(
    bool legacy_completed,
    bool supervisor_completed,
    bool legacy_completion_allowed);

/* A physical attach re-arms the app latch directly. A successful attached
 * maintenance restart is the only no-detach event that starts another genuine
 * charge generation, so it must re-arm the next qualified FULL notice. If a
 * stale completion and restart are ever observed together, restart wins and
 * no old FULL is replayed. */
battery_full_notice_decision_t battery_full_notice_step(
    bool already_notified,
    bool charge_completed,
    bool maintenance_restarted);

/* v6.00 charger event 0x0724 selects the higher tone 10 in the ordinary
 * no-call state (expression 0x051e). Its alternate call-state branch selects
 * tone 11 (expressions 0x051f/0x0520). */
uint8_t battery_charger_insert_tone_index(bool call_active);

#endif

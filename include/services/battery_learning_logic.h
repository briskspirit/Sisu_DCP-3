#ifndef BATTERY_LEARNING_LOGIC_H
#define BATTERY_LEARNING_LOGIC_H

#include <stdbool.h>
#include <stdint.h>

/* Battery health and state-of-charge supervisor. It learns capacity and
 * resistance, maintains the ACR-backed SOC ledger, and publishes SOC bars plus
 * an unconfirmed capacity-exhaustion prediction. A physical endpoint event is
 * required before the prediction may become learned EMPTY evidence. */

#define BATTERY_LEARNING_CAPACITY_HISTORY_COUNT 3u
#define BATTERY_LEARNING_RESISTANCE_BIN_COUNT 3u
#define BATTERY_LEARNING_RESISTANCE_WINDOW_COUNT 9u
#define BATTERY_LEARNING_SOC_BOOTSTRAP_SAMPLE_COUNT 5u

/* Shared physical-accounting domain. The charge supervisor chooses the
 * calibrated value; the SOC ledger must project charge with that same value. */
#define BATTERY_CHARGE_FACTOR_MIN_PERMILLE 1000u
#define BATTERY_CHARGE_FACTOR_MAX_PERMILLE 2000u

#define BATTERY_LEARNING_EVENT_NONE 0u
#define BATTERY_LEARNING_EVENT_FULL (1u << 0)
#define BATTERY_LEARNING_EVENT_EMPTY (1u << 1)

#define BATTERY_LEARNING_RESULT_NONE 0u
#define BATTERY_LEARNING_RESULT_PERSIST (1u << 0)
#define BATTERY_LEARNING_RESULT_FULL_ANCHORED (1u << 1)
#define BATTERY_LEARNING_RESULT_CAPACITY_ACCEPTED (1u << 2)
#define BATTERY_LEARNING_RESULT_CAPACITY_REJECTED (1u << 3)
#define BATTERY_LEARNING_RESULT_RESISTANCE_UPDATED (1u << 4)
#define BATTERY_LEARNING_RESULT_PACK_RESET (1u << 5)
#define BATTERY_LEARNING_RESULT_SOC_BOOTSTRAPPED (1u << 6)
#define BATTERY_LEARNING_RESULT_SOC_REANCHORED (1u << 7)
#define BATTERY_LEARNING_RESULT_SOC_INVALIDATED (1u << 8)

typedef enum {
    BATTERY_CAPACITY_CONFIDENCE_PRIOR = 0,
    BATTERY_CAPACITY_CONFIDENCE_OBSERVED,
    BATTERY_CAPACITY_CONFIDENCE_LEARNED,
    BATTERY_CAPACITY_CONFIDENCE_CONFLICTED,
} battery_capacity_confidence_t;

typedef enum {
    BATTERY_SOC_PROVENANCE_UNKNOWN = 0,
    BATTERY_SOC_PROVENANCE_BOOTSTRAP_VOLTAGE,
    BATTERY_SOC_PROVENANCE_TRACKED,
    BATTERY_SOC_PROVENANCE_ANCHORED_FULL,
    BATTERY_SOC_PROVENANCE_ANCHORED_EMPTY,
} battery_soc_provenance_t;

typedef enum {
    BATTERY_SOC_CONFIDENCE_NONE = 0,
    BATTERY_SOC_CONFIDENCE_PROVISIONAL,
    BATTERY_SOC_CONFIDENCE_ANCHORED,
} battery_soc_confidence_t;

typedef enum {
    BATTERY_RESISTANCE_BIN_HIGH = 0,
    BATTERY_RESISTANCE_BIN_MID,
    BATTERY_RESISTANCE_BIN_LOW,
} battery_resistance_bin_t;

typedef enum {
    /* Profile 1 used the LTC2959 data-sheet ACR scale. Do not restore its
     * capacity or absolute-anchor evidence under calibrated scale revision 2. */
    BATTERY_LEARNING_PROFILE_NIMH_2S = 2,
} battery_learning_profile_id_t;

typedef struct {
    battery_learning_profile_id_t profile_id;
    uint16_t nominal_capacity_mah;
    uint16_t capacity_min_mah;
    uint16_t capacity_max_mah;
    int32_t learning_temperature_min_mdegc;
    int32_t learning_temperature_max_mdegc;
    int32_t resistance_temperature_min_mdegc;
    int32_t resistance_temperature_max_mdegc;
    uint16_t resistance_step_min_ma;
    uint16_t resistance_min_mohm;
    uint16_t resistance_max_mohm;
    uint16_t resistance_persist_delta_mohm;
    uint16_t capacity_consistency_percent;
    int64_t recharge_invalidate_nah;
    uint16_t acr_lsb_nah;
    uint16_t charge_factor_permille;
} battery_learning_profile_t;

typedef struct {
    uint32_t now_ms;
    uint32_t sample_sequence;
    uint32_t gauge_session;
    uint32_t acr_raw;
    int64_t session_delta_nah;
    uint16_t terminal_mv;
    int32_t current_ua; /* +charge, -discharge */
    int32_t temperature_mdegc;
    uint16_t reference_mv; /* load-normalized battery-gauge domain */
    bool sample_valid;
    bool current_valid;
    bool continuity_valid;
    bool reference_valid;
    bool charger_connected;
    bool charge_active;
    bool authoritative;
} battery_learning_observation_t;

/* Health evidence restored deliberately through the development console.
 * A provision never imports a live ACR/full anchor: those values are tied to
 * one physical LTC gauge session and cannot survive battery removal safely.
 * natural_empty_valid is different: it records an explicitly observed pack
 * endpoint and carries no absolute ACR value. */
typedef struct {
    uint16_t capacity_mah;
    /* Zero means unknown; otherwise each value must fit the active profile. */
    uint16_t resistance_mohm[BATTERY_LEARNING_RESISTANCE_BIN_COUNT];
    bool natural_empty_valid;
} battery_learning_provision_t;

/* Journaled per-pack state. The storage layer serializes these fields
 * explicitly; this C layout is not an on-flash ABI. */
typedef struct {
    uint8_t profile_id;
    uint16_t nominal_capacity_mah;
    uint16_t capacity_history_mah[
        BATTERY_LEARNING_CAPACITY_HISTORY_COUNT];
    uint8_t capacity_history_count;
    uint8_t capacity_history_next;
    uint16_t accepted_capacity_cycles;
    uint16_t rejected_capacity_cycles;
    uint16_t last_capacity_mah;
    uint16_t resistance_mohm[BATTERY_LEARNING_RESISTANCE_BIN_COUNT];
    uint16_t resistance_sample_count[
        BATTERY_LEARNING_RESISTANCE_BIN_COUNT];
    uint32_t pack_generation;
    uint32_t full_anchor_acr_raw;
    int64_t full_anchor_nah;
    int32_t cycle_min_temperature_mdegc;
    int32_t cycle_max_temperature_mdegc;
    bool full_anchor_valid;
    bool capacity_cycle_qualified;
    bool natural_empty_valid;
    bool soc_valid;
    bool soc_charge_segment;
    uint16_t soc_capacity_mah;
    uint16_t soc_charge_factor_permille;
    uint16_t soc_bootstrap_reference_mv;
    battery_soc_provenance_t soc_anchor_provenance;
    battery_soc_confidence_t soc_confidence;
    int64_t soc_anchor_session_delta_nah;
    int64_t soc_anchor_remaining_nah;
} battery_learning_persisted_t;

typedef struct {
    uint16_t nominal_capacity_mah;
    uint16_t learned_capacity_mah;
    uint16_t last_capacity_mah;
    uint16_t capacity_spread_mah;
    uint16_t capacity_history_mah[
        BATTERY_LEARNING_CAPACITY_HISTORY_COUNT];
    uint8_t capacity_history_count;
    uint8_t capacity_history_next;
    uint16_t remaining_capacity_mah;
    uint16_t state_of_health_percent;
    uint16_t cycle_discharged_mah;
    uint64_t cycle_discharged_nah;
    uint64_t capacity_overrun_nah;
    uint64_t remaining_capacity_nah;
    uint16_t soc_capacity_mah;
    uint8_t state_of_charge_percent;
    battery_capacity_confidence_t capacity_confidence;
    bool learned_capacity_valid;
    bool remaining_capacity_valid;
    /* The ACR projection reached the current capacity estimate, but no
     * physical EMPTY endpoint has been accepted yet. The full anchor remains
     * live so later voltage evidence can measure upward capacity honestly. */
    bool capacity_prediction_exhausted;
    bool soc_bars_valid;
    uint8_t soc_bars;
    battery_soc_provenance_t soc_provenance;
    battery_soc_confidence_t soc_confidence;
    bool soc_charge_segment;
    uint16_t soc_bootstrap_reference_mv;
    bool full_anchor_valid;
    bool capacity_cycle_qualified;
    bool natural_empty_valid;
    uint16_t accepted_capacity_cycles;
    uint16_t rejected_capacity_cycles;
    uint32_t pack_generation;
    bool gauge_session_bound;
    uint32_t gauge_session;
    uint16_t resistance_mohm[BATTERY_LEARNING_RESISTANCE_BIN_COUNT];
    uint16_t resistance_sample_count[
        BATTERY_LEARNING_RESISTANCE_BIN_COUNT];
    battery_resistance_bin_t current_resistance_bin;
    bool current_resistance_bin_valid;
} battery_learning_snapshot_t;

typedef struct {
    battery_learning_persisted_t persisted;
    battery_learning_snapshot_t snapshot;
    uint16_t resistance_window[BATTERY_LEARNING_RESISTANCE_BIN_COUNT]
                              [BATTERY_LEARNING_RESISTANCE_WINDOW_COUNT];
    uint16_t resistance_persisted_mohm[
        BATTERY_LEARNING_RESISTANCE_BIN_COUNT];
    uint16_t resistance_persisted_count[
        BATTERY_LEARNING_RESISTANCE_BIN_COUNT];
    uint8_t resistance_window_count[BATTERY_LEARNING_RESISTANCE_BIN_COUNT];
    uint32_t last_sample_sequence;
    uint32_t last_gauge_session;
    uint32_t previous_ms;
    int64_t latest_session_delta_nah;
    uint16_t previous_terminal_mv;
    uint16_t latest_terminal_mv;
    int32_t previous_current_ua;
    bool session_seen;
    bool sample_seen;
    bool latest_observation_valid;
    bool previous_resistance_sample_valid;
    uint32_t soc_bootstrap_reference_sum_mv;
    uint8_t soc_bootstrap_sample_count;
} battery_learning_state_t;

const battery_learning_profile_t *battery_learning_nimh_profile(void);

/* Coarse initialization only. The input is the Nokia 65 mA reference-load
 * voltage, not open-circuit voltage. The result is deliberately tagged
 * PROVISIONAL and cannot authorize anchored-only charge termination. */
uint8_t battery_learning_bootstrap_soc_percent(uint16_t reference_mv);

void battery_learning_persisted_defaults(
    battery_learning_persisted_t *persisted,
    const battery_learning_profile_t *profile);
bool battery_learning_persisted_valid(
    const battery_learning_persisted_t *persisted,
    const battery_learning_profile_t *profile);
void battery_learning_init(
    battery_learning_state_t *state,
    const battery_learning_profile_t *profile,
    const battery_learning_persisted_t *persisted);

/* Replace per-pack health evidence while preserving the already-observed
 * gauge-session identity. The capacity is restored as one observed sample,
 * not promoted to multi-cycle learned confidence. */
bool battery_learning_provision(
    battery_learning_state_t *state,
    const battery_learning_profile_t *profile,
    const battery_learning_provision_t *provision);

/* Append one independently recovered full-to-empty measurement without
 * replacing any other pack evidence. expected_accepted_cycles makes an
 * operator retry fail atomically after the first successful append. Existing
 * endpoint/SOC evidence is rebased to the updated capacity estimate. */
bool battery_learning_recover_capacity_cycle(
    battery_learning_state_t *state,
    const battery_learning_profile_t *profile,
    uint16_t capacity_mah,
    uint16_t expected_accepted_cycles);

/* Restore a historically captured FULL endpoint from the same live LTC
 * session. This is a guarded postmortem path for evidence recorded before the
 * firmware learned to promote capacity-qualified charge termination itself. */
bool battery_learning_recover_full_endpoint(
    battery_learning_state_t *state,
    const battery_learning_profile_t *profile,
    const battery_learning_observation_t *current_observation,
    uint32_t full_acr_raw,
    int64_t full_session_delta_nah,
    uint16_t expected_accepted_cycles);

uint32_t battery_learning_update(
    battery_learning_state_t *state,
    const battery_learning_profile_t *profile,
    const battery_learning_observation_t *observation,
    uint32_t events);

void battery_learning_get_snapshot(
    const battery_learning_state_t *state,
    battery_learning_snapshot_t *out);
void battery_learning_export_persisted(
    const battery_learning_state_t *state,
    battery_learning_persisted_t *out);

#endif

#ifndef BATTERY_CHARGE_SUPERVISOR_LOGIC_H
#define BATTERY_CHARGE_SUPERVISOR_LOGIC_H

#include <stdbool.h>
#include <stdint.h>

#include "services/battery_charge_logic.h"
#include "services/battery_learning_logic.h"

/* Pure charge-session policy model. The service layer owns persistence and
 * charger control; this logic only decides when that authority is justified. */

#define BATTERY_CHARGE_SUPERVISOR_MINUTE_SAMPLE_CAP 64u
#define BATTERY_CHARGE_SUPERVISOR_TRACE_INTERVAL_MS 60000u
#define BATTERY_CHARGE_REVB2_SAFETY_INPUT_FACTOR_PERMILLE 1750u
#define BATTERY_CHARGE_MAINTENANCE_RESTART_SOC_PERCENT 80u
#define BATTERY_CHARGE_MAINTENANCE_RESTART_MV 2600u
#define BATTERY_CHARGE_MAINTENANCE_CE_RESET_MS 1000u

#define BATTERY_CHARGE_SUPERVISOR_RESULT_NONE 0u
#define BATTERY_CHARGE_SUPERVISOR_RESULT_ATTACHED (1u << 0)
#define BATTERY_CHARGE_SUPERVISOR_RESULT_ADMITTED (1u << 1)
#define BATTERY_CHARGE_SUPERVISOR_RESULT_DETACHED (1u << 2)
#define BATTERY_CHARGE_SUPERVISOR_RESULT_BQ_COMPLETE (1u << 3)
#define BATTERY_CHARGE_SUPERVISOR_RESULT_TERMINAL (1u << 4)
#define BATTERY_CHARGE_SUPERVISOR_RESULT_DEGRADED (1u << 5)
#define BATTERY_CHARGE_SUPERVISOR_RESULT_MINUTE (1u << 6)
#define BATTERY_CHARGE_SUPERVISOR_RESULT_RESTORED (1u << 7)
#define BATTERY_CHARGE_SUPERVISOR_RESULT_STOP_REQUESTED (1u << 8)
#define BATTERY_CHARGE_SUPERVISOR_RESULT_SOFTWARE_STOPPED (1u << 9)
#define BATTERY_CHARGE_SUPERVISOR_RESULT_FULL_QUALIFIED (1u << 10)
#define BATTERY_CHARGE_SUPERVISOR_RESULT_MAINTENANCE_ARMED (1u << 11)
#define BATTERY_CHARGE_SUPERVISOR_RESULT_MAINTENANCE_RESTARTED (1u << 12)
#define BATTERY_CHARGE_SUPERVISOR_RESULT_BQ_RECOVERY_ARMED (1u << 13)

#define BATTERY_CHARGE_BLOCK_POLICY_OBSERVE (1u << 0)
#define BATTERY_CHARGE_BLOCK_NOT_ADMITTED (1u << 1)
#define BATTERY_CHARGE_BLOCK_SAMPLE_INVALID (1u << 2)
#define BATTERY_CHARGE_BLOCK_CURRENT_INVALID (1u << 3)
#define BATTERY_CHARGE_BLOCK_CONTINUITY_LOST (1u << 4)
#define BATTERY_CHARGE_BLOCK_NON_AUTHORITATIVE (1u << 5)
#define BATTERY_CHARGE_BLOCK_CONTROL_UNCONFIRMED (1u << 6)
#define BATTERY_CHARGE_BLOCK_CHEMISTRY (1u << 7)
#define BATTERY_CHARGE_BLOCK_HARDWARE_PROFILE (1u << 8)
#define BATTERY_CHARGE_BLOCK_UNKNOWN_DEFICIT (1u << 9)
#define BATTERY_CHARGE_BLOCK_CAPACITY_CONFIDENCE (1u << 10)
#define BATTERY_CHARGE_BLOCK_FACTOR_UNLEARNED (1u << 11)
#define BATTERY_CHARGE_BLOCK_CURVE_UNCALIBRATED (1u << 12)
#define BATTERY_CHARGE_BLOCK_NO_ACTIVE_HISTORY (1u << 13)
#define BATTERY_CHARGE_BLOCK_STATUS_INVALID (1u << 14)
#define BATTERY_CHARGE_BLOCK_SOC_CONFIDENCE (1u << 15)
#define BATTERY_CHARGE_BLOCK_STALE_ADMISSION (1u << 16)

typedef enum {
    BATTERY_CHARGE_POLICY_OBSERVE = 0,
    BATTERY_CHARGE_POLICY_ENFORCE_ANCHORED,
    BATTERY_CHARGE_POLICY_ENFORCE_BOOTSTRAP,
} battery_charge_supervisor_policy_t;

typedef enum {
    BATTERY_CHARGE_CHEMISTRY_UNKNOWN = 0,
    BATTERY_CHARGE_CHEMISTRY_NIMH_2S,
    BATTERY_CHARGE_CHEMISTRY_LI_ION_1S,
} battery_charge_supervisor_chemistry_t;

typedef enum {
    BATTERY_CHARGE_PHASE_DETACHED = 0,
    BATTERY_CHARGE_PHASE_QUALIFYING,
    BATTERY_CHARGE_PHASE_CHARGING,
    BATTERY_CHARGE_PHASE_COMPLETE_BQ,
    BATTERY_CHARGE_PHASE_COMPLETE_SOFTWARE,
    BATTERY_CHARGE_PHASE_STOPPED_SAFETY,
    BATTERY_CHARGE_PHASE_FAULT,
    BATTERY_CHARGE_PHASE_DEGRADED,
    BATTERY_CHARGE_PHASE_STOPPING,
} battery_charge_supervisor_phase_t;

typedef enum {
    BATTERY_CHARGE_TERMINAL_NONE = 0,
    BATTERY_CHARGE_TERMINAL_BQ_COMPLETE_AFTER_ACTIVE,
    BATTERY_CHARGE_TERMINAL_BQ_ALREADY_FULL_AT_ATTACH,
    BATTERY_CHARGE_TERMINAL_SOFTWARE_COULOMB_FULL,
    BATTERY_CHARGE_TERMINAL_SOFTWARE_CURVE_FULL,
    BATTERY_CHARGE_TERMINAL_SOFTWARE_COULOMB_AND_CURVE_FULL,
    BATTERY_CHARGE_TERMINAL_SAFETY_CHARGE_LIMIT,
    BATTERY_CHARGE_TERMINAL_SAFETY_TIME_LIMIT,
    BATTERY_CHARGE_TERMINAL_BOARD_TEMPERATURE_LIMIT,
    BATTERY_CHARGE_TERMINAL_BQ_FAULT,
    BATTERY_CHARGE_TERMINAL_CONTROL_READBACK_FAILED,
    BATTERY_CHARGE_TERMINAL_GAUGE_CONTINUITY_LOST,
    BATTERY_CHARGE_TERMINAL_MANUAL_DEBUG_STOP,
    BATTERY_CHARGE_TERMINAL_DETACHED,
    /* Keep new persisted enum values appended so older journal values retain
     * their meaning. */
    BATTERY_CHARGE_TERMINAL_BQ_FAULT_AFTER_COULOMB_FULL,
    /* Durable /CE-off phase used to reset a completed BQ session before an
     * attached maintenance charge. This is control state, never a FULL claim. */
    BATTERY_CHARGE_TERMINAL_MAINTENANCE_REARM,
} battery_charge_supervisor_terminal_t;

typedef enum {
    BATTERY_CHARGE_CANDIDATE_NONE = 0,
    BATTERY_CHARGE_CANDIDATE_BQ_COMPLETE,
    BATTERY_CHARGE_CANDIDATE_SOFTWARE_COULOMB_FULL,
} battery_charge_supervisor_candidate_t;

typedef struct {
    battery_charge_supervisor_chemistry_t chemistry;
    uint8_t hardware_profile_id;
    uint32_t nominal_charge_current_ua;
    /* Programmed BQ timeout. Thermal regulation and suspended fault time can
     * extend wall-clock completion, so this is profile identity rather than a
     * software deadline. */
    uint32_t nominal_backup_timer_ms;
    uint16_t charge_factor_prior_permille;
    uint16_t admission_current_min_ma;
    uint32_t completion_confirm_ms;
    uint16_t safety_input_factor_permille;
    uint32_t control_confirm_ms;
} battery_charge_supervisor_profile_t;

typedef struct {
    /* True only when the learner evidence belongs to this observation's LTC
     * session. Admission must not consume a previous pack's cached SOC. */
    bool source_current;
    bool capacity_valid;
    uint16_t capacity_mah;
    battery_capacity_confidence_t capacity_confidence;
    bool remaining_valid;
    uint16_t remaining_mah;
    uint64_t remaining_nah;
    battery_soc_provenance_t soc_provenance;
    battery_soc_confidence_t soc_confidence;
    bool natural_empty_start;
    bool resistance_valid;
    uint16_t resistance_mohm;
    uint32_t pack_generation;
    uint16_t charge_factor_permille;
    bool charge_factor_confident;
} battery_charge_supervisor_admission_t;

typedef struct {
    uint32_t now_ms;
    uint32_t sample_sequence;
    uint32_t gauge_session;
    uint32_t acr_raw;
    int64_t session_delta_nah;
    uint16_t terminal_mv;
    uint16_t reference_mv; /* filtered Nokia 65 mA load domain */
    int32_t current_ua; /* positive into the battery */
    int32_t temperature_mdegc; /* board/LTC temperature, not pack NTC */
    bool sample_valid;
    bool reference_valid;
    bool current_valid;
    bool continuity_valid;
    bool authoritative;
    bool charger_present;
    uint32_t charger_input_mv;
    bool status_valid;
    battery_charger_state_t charger_state;
    bool charger_enable_requested;
    bool charger_enable_readback;
    bool charger_enable_valid;
    uint8_t inhibit_owner_mask;
    bool supervisor_inhibit_active;
    battery_charge_supervisor_chemistry_t chemistry;
    uint8_t hardware_profile_id;
    battery_charge_supervisor_admission_t admission;
} battery_charge_supervisor_observation_t;

typedef struct {
    uint32_t at_ms;
    uint32_t elapsed_ms;
    uint16_t terminal_median_mv;
    uint16_t compensated_median_mv;
    int32_t current_average_ua;
    int32_t temperature_average_mdegc;
    int64_t net_input_nah;
} battery_charge_supervisor_minute_t;

/* Transition-only NVM state. Storage encodes these fields explicitly; this C
 * layout is not an on-flash ABI. Minute samples and live integration state are
 * intentionally absent. */
typedef struct {
    uint8_t hardware_profile_id;
    battery_charge_supervisor_chemistry_t chemistry;
    uint32_t charge_generation;
    bool active_session_valid;
    uint32_t pack_generation;
    uint32_t gauge_session;
    uint32_t start_acr_raw;
    int64_t start_session_delta_nah;
    bool frozen_capacity_valid;
    uint16_t frozen_capacity_mah;
    battery_capacity_confidence_t frozen_capacity_confidence;
    bool frozen_remaining_valid;
    uint16_t frozen_remaining_mah;
    uint64_t frozen_remaining_nah;
    battery_soc_provenance_t frozen_soc_provenance;
    battery_soc_confidence_t frozen_soc_confidence;
    bool frozen_resistance_valid;
    uint16_t frozen_resistance_mohm;
    bool deficit_valid;
    uint16_t deficit_mah;
    uint64_t deficit_nah;
    uint16_t charge_factor_permille;
    bool charge_factor_confident;
    bool target_valid;
    uint64_t target_input_nah;
    uint64_t safety_input_nah;
    battery_charge_supervisor_policy_t configured_policy;
    uint16_t configured_charge_factor_permille;
    bool configured_charge_factor_confident;
    bool supervisor_inhibit_latched;
    battery_charge_supervisor_terminal_t latched_stop_reason;
    bool maintenance_rearm_pending;
    /* Loop guard for automatic /CE resets. It remains set until a physical
     * detach or independently qualified FULL proves a new safe boundary. */
    bool completion_rearm_used;
    bool last_terminal_valid;
    bool last_terminal_full_qualified;
    bool last_terminal_net_input_valid;
    bool last_terminal_voltage_valid;
    bool last_terminal_trace_complete;
    battery_charge_supervisor_terminal_t last_terminal_reason;
    uint32_t last_terminal_generation;
    uint32_t last_terminal_elapsed_ms;
    int64_t last_terminal_net_input_nah;
    uint16_t last_terminal_mv;
    uint16_t last_curve_peak_mv;
    uint16_t last_curve_drop_mv;
    int16_t last_curve_slope_mv_per_min;
} battery_charge_supervisor_persisted_t;

typedef struct {
    battery_charge_supervisor_policy_t policy;
    battery_charge_supervisor_phase_t phase;
    battery_charge_supervisor_terminal_t terminal_reason;
    battery_charge_supervisor_candidate_t candidate;
    uint32_t blockers;
    uint32_t charge_generation;
    uint32_t pack_generation;
    uint32_t gauge_session;
    uint8_t hardware_profile_id;
    battery_charge_supervisor_chemistry_t chemistry;
    bool attached;
    bool admitted;
    bool session_authoritative;
    bool full_candidate;
    bool full_qualified;
    uint32_t elapsed_ms;
    uint32_t start_acr_raw;
    int64_t start_session_delta_nah;
    int64_t net_input_nah;
    uint64_t positive_crosscheck_nah;
    uint64_t negative_crosscheck_nah;
    bool frozen_capacity_valid;
    uint16_t frozen_capacity_mah;
    battery_capacity_confidence_t frozen_capacity_confidence;
    bool frozen_remaining_valid;
    uint16_t frozen_remaining_mah;
    uint64_t frozen_remaining_nah;
    battery_soc_provenance_t frozen_soc_provenance;
    battery_soc_confidence_t frozen_soc_confidence;
    bool frozen_resistance_valid;
    uint16_t frozen_resistance_mohm;
    bool deficit_valid;
    uint16_t deficit_mah;
    uint64_t deficit_nah;
    uint16_t charge_factor_permille;
    bool charge_factor_confident;
    bool target_valid;
    bool target_full_capacity;
    uint64_t target_input_nah;
    uint64_t safety_input_nah;
    bool stop_requested;
    battery_charge_supervisor_terminal_t stop_reason;
    uint16_t terminal_mv;
    uint16_t compensated_mv;
    int32_t current_ua;
    int32_t temperature_mdegc;
    bool curve_valid;
    uint16_t curve_peak_mv;
    uint16_t curve_drop_mv;
    int16_t curve_slope_mv_per_min;
    uint32_t minute_count;
    battery_charge_supervisor_minute_t latest_minute;
} battery_charge_supervisor_snapshot_t;

typedef struct {
    battery_charge_supervisor_snapshot_t snapshot;
    bool physically_attached;
    bool detach_generation_reusable;
    bool generation_authoritative;
    bool had_active;
    bool full_pending;
    bool stop_control_seen;
    bool sample_seen;
    bool integration_sample_valid;
    bool previous_minute_valid;
    uint32_t detached_at_ms;
    uint32_t attached_at_ms;
    uint32_t admitted_at_ms;
    uint32_t full_since_ms;
    uint32_t stop_requested_ms;
    uint32_t last_sample_sequence;
    uint32_t last_sample_ms;
    int32_t last_current_ua;
    uint32_t minute_started_ms;
    uint16_t minute_terminal_mv[BATTERY_CHARGE_SUPERVISOR_MINUTE_SAMPLE_CAP];
    uint16_t minute_compensated_mv[BATTERY_CHARGE_SUPERVISOR_MINUTE_SAMPLE_CAP];
    uint8_t minute_sample_count;
    int64_t minute_current_sum_ua;
    int64_t minute_temperature_sum_mdegc;
    uint16_t previous_minute_compensated_mv;
} battery_charge_supervisor_state_t;

const battery_charge_supervisor_profile_t *
battery_charge_supervisor_revb2_profile(void);

void battery_charge_supervisor_init(
    battery_charge_supervisor_state_t *state,
    battery_charge_supervisor_policy_t policy);

uint32_t battery_charge_supervisor_update(
    battery_charge_supervisor_state_t *state,
    const battery_charge_supervisor_profile_t *profile,
    const battery_charge_supervisor_observation_t *observation);

void battery_charge_supervisor_get_snapshot(
    const battery_charge_supervisor_state_t *state,
    battery_charge_supervisor_snapshot_t *out);

void battery_charge_supervisor_persisted_defaults(
    battery_charge_supervisor_persisted_t *persisted);
bool battery_charge_supervisor_persisted_valid(
    const battery_charge_supervisor_persisted_t *persisted);

/* Terminal reasons that may carry qualified-FULL provenance. Some reasons,
 * notably BQ completion, still require an explicit persisted qualification. */
bool battery_charge_supervisor_terminal_qualifies_full(
    battery_charge_supervisor_terminal_t terminal);

/* Reconstruct an admitted observe/enforce session after an RP reset. Returns
 * false without mutating state unless every continuity/ownership identity
 * still matches the persisted admission. */
bool battery_charge_supervisor_restore_active(
    battery_charge_supervisor_state_t *state,
    battery_charge_supervisor_policy_t policy,
    const battery_charge_supervisor_profile_t *profile,
    const battery_charge_supervisor_persisted_t *persisted,
    const battery_charge_supervisor_observation_t *observation);

void battery_charge_supervisor_reject_restore(
    battery_charge_supervisor_state_t *state,
    battery_charge_supervisor_policy_t policy,
    uint32_t generation,
    const battery_charge_supervisor_profile_t *profile,
    const battery_charge_supervisor_observation_t *observation);

void battery_charge_supervisor_seed_generation(
    battery_charge_supervisor_state_t *state, uint32_t generation);

#endif

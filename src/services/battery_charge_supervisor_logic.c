#include "services/battery_charge_supervisor_logic.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>

#define NANOAMP_HOURS_PER_MILLIAMP_HOUR UINT64_C(1000000)
#define SAMPLE_GAP_MAX_MS 10000u
#define ATTACH_BOUNCE_REUSE_MS 500u

/* Hardware-profile IDs are evidence-domain identities, not revisions of the
 * algorithm. Never reuse an ID after changing ISET, the backup timer, or the
 * charge path: old curve/factor evidence must not authorize a new circuit. */
static const battery_charge_supervisor_profile_t REVB2_PROFILE = {
    .chemistry = BATTERY_CHARGE_CHEMISTRY_NIMH_2S,
    /* Profile 2 used the data-sheet ACR scale. A persisted charge baseline is
     * unit-bearing, so scale revision 2 requires a new evidence domain. Both
     * profiles describe the fitted 1.10 kOhm ISET and 24 kOhm CHM_TMR parts. */
    .hardware_profile_id = 3u,
    .nominal_charge_current_ua = 273000u,
    .nominal_backup_timer_ms = 8u * 60u * 60u * 1000u,
    /* Provisional product-family prior: the first uninterrupted profile-3
     * trace reached its compensated-voltage plateau at 1520.5 mAh input
     * against the provisioned 1242 mAh capacity (1.224x). This remains an
     * unconfident diagnostic target until controlled discharges validate it. */
    .charge_factor_prior_permille = 1224u,
    .admission_current_min_ma = 1u,
    .completion_confirm_ms = BATTERY_CHARGE_COMPLETE_CONFIRM_MS,
    /* Independent capacity-scaled ACR ceiling near the BQ's nominal
     * 273 mA * 8 h timer envelope for the expected pack range. It is a safety
     * stop, never a FULL claim; the hardware timer remains a separate backstop. */
    .safety_input_factor_permille =
        BATTERY_CHARGE_REVB2_SAFETY_INPUT_FACTOR_PERMILLE,
    .control_confirm_ms = 5000u,
};

static uint32_t next_nonzero_u32(uint32_t value) {
    value++;
    return value == 0u ? 1u : value;
}

static int64_t saturating_sub_i64(int64_t a, int64_t b) {
    if (b > 0 && a < INT64_MIN + b) {
        return INT64_MIN;
    }
    if (b < 0 && a > INT64_MAX + b) {
        return INT64_MAX;
    }
    return a - b;
}

static uint64_t saturating_add_u64(uint64_t a, uint64_t b) {
    return a > UINT64_MAX - b ? UINT64_MAX : a + b;
}

static uint16_t rounded_nah_to_mah(uint64_t value) {
    uint64_t rounded = value / NANOAMP_HOURS_PER_MILLIAMP_HOUR;
    if (value % NANOAMP_HOURS_PER_MILLIAMP_HOUR >=
        NANOAMP_HOURS_PER_MILLIAMP_HOUR / 2u) {
        rounded++;
    }
    return rounded > UINT16_MAX ? UINT16_MAX : (uint16_t)rounded;
}

static uint64_t scaled_nah(uint64_t value, uint16_t factor_permille) {
    if (factor_permille == 0u) {
        return 0u;
    }
    if (value > UINT64_MAX / factor_permille) {
        return UINT64_MAX;
    }
    return value * factor_permille / 1000u;
}

static bool target_uses_full_capacity(
    const battery_charge_supervisor_snapshot_t *snapshot) {
    return snapshot->target_valid && snapshot->frozen_capacity_valid &&
        snapshot->deficit_valid &&
        snapshot->deficit_nah ==
            (uint64_t)snapshot->frozen_capacity_mah *
                NANOAMP_HOURS_PER_MILLIAMP_HOUR;
}

bool battery_charge_supervisor_terminal_qualifies_full(
    battery_charge_supervisor_terminal_t terminal) {
    return terminal == BATTERY_CHARGE_TERMINAL_BQ_COMPLETE_AFTER_ACTIVE ||
        terminal == BATTERY_CHARGE_TERMINAL_SOFTWARE_COULOMB_FULL ||
        terminal == BATTERY_CHARGE_TERMINAL_SOFTWARE_CURVE_FULL ||
        terminal ==
            BATTERY_CHARGE_TERMINAL_SOFTWARE_COULOMB_AND_CURVE_FULL ||
        terminal ==
            BATTERY_CHARGE_TERMINAL_BQ_FAULT_AFTER_COULOMB_FULL;
}

static bool expected_stop_control(
    const battery_charge_supervisor_state_t *state,
    const battery_charge_supervisor_observation_t *observation) {
    return state->snapshot.phase == BATTERY_CHARGE_PHASE_STOPPING &&
        state->snapshot.stop_requested &&
        observation->supervisor_inhibit_active &&
        observation->charger_enable_valid &&
        !observation->charger_enable_requested &&
        !observation->charger_enable_readback;
}

static uint16_t median_u16(const uint16_t *values, uint8_t count) {
    uint16_t sorted[BATTERY_CHARGE_SUPERVISOR_MINUTE_SAMPLE_CAP];
    if (values == NULL || count == 0u ||
        count > BATTERY_CHARGE_SUPERVISOR_MINUTE_SAMPLE_CAP) {
        return 0u;
    }
    for (uint8_t i = 0u; i < count; i++) {
        sorted[i] = values[i];
    }
    for (uint8_t i = 1u; i < count; i++) {
        uint16_t value = sorted[i];
        uint8_t j = i;
        while (j > 0u && value < sorted[j - 1u]) {
            sorted[j] = sorted[j - 1u];
            j--;
        }
        sorted[j] = value;
    }
    return sorted[count / 2u];
}

static uint16_t compensated_voltage_mv(
    const battery_charge_supervisor_observation_t *observation,
    const battery_charge_supervisor_snapshot_t *snapshot) {
    if (!snapshot->admitted || !snapshot->session_authoritative ||
        !observation->current_valid || observation->current_ua <= 0 ||
        !snapshot->frozen_resistance_valid) {
        return observation->terminal_mv;
    }
    int64_t drop_mv =
        ((int64_t)observation->current_ua *
         snapshot->frozen_resistance_mohm + 500000) /
        1000000;
    if (drop_mv <= 0) {
        return observation->terminal_mv;
    }
    return drop_mv >= observation->terminal_mv
        ? 0u : (uint16_t)(observation->terminal_mv - drop_mv);
}

static void reset_minute_window(battery_charge_supervisor_state_t *state,
                                uint32_t now_ms) {
    state->minute_started_ms = now_ms;
    state->minute_sample_count = 0u;
    state->minute_current_sum_ua = 0;
    state->minute_temperature_sum_mdegc = 0;
}

static void reset_session_fields(battery_charge_supervisor_state_t *state) {
    battery_charge_supervisor_policy_t policy = state->snapshot.policy;
    uint32_t generation = state->snapshot.charge_generation;
    memset(&state->snapshot, 0, sizeof(state->snapshot));
    state->snapshot.policy = policy;
    state->snapshot.phase = BATTERY_CHARGE_PHASE_DETACHED;
    state->snapshot.terminal_reason = BATTERY_CHARGE_TERMINAL_DETACHED;
    state->snapshot.charge_generation = generation;
    state->snapshot.blockers =
        (policy == BATTERY_CHARGE_POLICY_OBSERVE
             ? BATTERY_CHARGE_BLOCK_POLICY_OBSERVE : 0u) |
        BATTERY_CHARGE_BLOCK_NOT_ADMITTED;
    state->had_active = false;
    state->full_pending = false;
    state->stop_control_seen = false;
    state->sample_seen = false;
    state->integration_sample_valid = false;
    state->previous_minute_valid = false;
    state->minute_sample_count = 0u;
    state->minute_current_sum_ua = 0;
    state->minute_temperature_sum_mdegc = 0;
}

static uint32_t blockers_for_observation(
    const battery_charge_supervisor_state_t *state,
    const battery_charge_supervisor_profile_t *profile,
    const battery_charge_supervisor_observation_t *observation) {
    uint32_t blockers = state->snapshot.policy ==
            BATTERY_CHARGE_POLICY_OBSERVE
        ? BATTERY_CHARGE_BLOCK_POLICY_OBSERVE : 0u;
    if (!state->snapshot.admitted) {
        blockers |= BATTERY_CHARGE_BLOCK_NOT_ADMITTED;
    }
    if (!observation->admission.source_current) {
        blockers |= BATTERY_CHARGE_BLOCK_STALE_ADMISSION;
    }
    if (!observation->sample_valid) {
        blockers |= BATTERY_CHARGE_BLOCK_SAMPLE_INVALID;
    }
    if (!observation->current_valid) {
        blockers |= BATTERY_CHARGE_BLOCK_CURRENT_INVALID;
    }
    if (!observation->continuity_valid) {
        blockers |= BATTERY_CHARGE_BLOCK_CONTINUITY_LOST;
    }
    if (!observation->authoritative ||
        (state->snapshot.admitted &&
         !state->snapshot.session_authoritative)) {
        blockers |= BATTERY_CHARGE_BLOCK_NON_AUTHORITATIVE;
    }
    if ((!observation->charger_enable_valid ||
         !observation->charger_enable_requested ||
         !observation->charger_enable_readback) &&
        !expected_stop_control(state, observation)) {
        blockers |= BATTERY_CHARGE_BLOCK_CONTROL_UNCONFIRMED;
    }
    if (observation->chemistry != profile->chemistry) {
        blockers |= BATTERY_CHARGE_BLOCK_CHEMISTRY;
    }
    if (observation->hardware_profile_id != profile->hardware_profile_id) {
        blockers |= BATTERY_CHARGE_BLOCK_HARDWARE_PROFILE;
    }
    if (!state->snapshot.deficit_valid) {
        blockers |= BATTERY_CHARGE_BLOCK_UNKNOWN_DEFICIT;
    }
    if (!state->snapshot.frozen_capacity_valid ||
        state->snapshot.frozen_capacity_confidence ==
            BATTERY_CAPACITY_CONFIDENCE_PRIOR ||
        state->snapshot.frozen_capacity_confidence ==
            BATTERY_CAPACITY_CONFIDENCE_CONFLICTED) {
        blockers |= BATTERY_CHARGE_BLOCK_CAPACITY_CONFIDENCE;
    }
    if ((state->snapshot.policy == BATTERY_CHARGE_POLICY_ENFORCE_ANCHORED &&
         state->snapshot.frozen_soc_confidence !=
             BATTERY_SOC_CONFIDENCE_ANCHORED &&
         !target_uses_full_capacity(&state->snapshot)) ||
        (state->snapshot.policy == BATTERY_CHARGE_POLICY_ENFORCE_BOOTSTRAP &&
         state->snapshot.frozen_soc_confidence ==
             BATTERY_SOC_CONFIDENCE_NONE &&
         !target_uses_full_capacity(&state->snapshot))) {
        blockers |= BATTERY_CHARGE_BLOCK_SOC_CONFIDENCE;
    }
    if (!state->snapshot.charge_factor_confident) {
        blockers |= BATTERY_CHARGE_BLOCK_FACTOR_UNLEARNED;
    }
    if (!state->had_active) {
        blockers |= BATTERY_CHARGE_BLOCK_NO_ACTIVE_HISTORY;
    }
    if (!observation->status_valid) {
        blockers |= BATTERY_CHARGE_BLOCK_STATUS_INVALID;
    }
    return blockers;
}

static void freeze_admission(
    battery_charge_supervisor_state_t *state,
    const battery_charge_supervisor_profile_t *profile,
    const battery_charge_supervisor_observation_t *observation) {
    battery_charge_supervisor_snapshot_t *snapshot = &state->snapshot;
    snapshot->phase = BATTERY_CHARGE_PHASE_CHARGING;
    snapshot->admitted = true;
    snapshot->session_authoritative =
        state->generation_authoritative && observation->authoritative;
    snapshot->pack_generation = observation->admission.pack_generation;
    snapshot->gauge_session = observation->gauge_session;
    snapshot->hardware_profile_id = observation->hardware_profile_id;
    snapshot->chemistry = observation->chemistry;
    snapshot->start_acr_raw = observation->acr_raw;
    snapshot->start_session_delta_nah = observation->session_delta_nah;
    snapshot->frozen_capacity_valid = observation->admission.capacity_valid;
    snapshot->frozen_capacity_mah = observation->admission.capacity_mah;
    snapshot->frozen_capacity_confidence =
        observation->admission.capacity_confidence;
    snapshot->frozen_remaining_valid =
        observation->admission.remaining_valid;
    snapshot->frozen_remaining_mah =
        observation->admission.remaining_mah;
    snapshot->frozen_remaining_nah =
        observation->admission.remaining_nah;
    snapshot->frozen_soc_provenance =
        observation->admission.soc_provenance;
    snapshot->frozen_soc_confidence =
        observation->admission.soc_confidence;
    snapshot->frozen_resistance_valid =
        observation->admission.resistance_valid;
    snapshot->frozen_resistance_mohm =
        observation->admission.resistance_mohm;
    snapshot->charge_factor_permille =
        observation->admission.charge_factor_permille != 0u
            ? observation->admission.charge_factor_permille
            : profile->charge_factor_prior_permille;
    snapshot->charge_factor_confident =
        observation->admission.charge_factor_confident;

    if (snapshot->frozen_capacity_valid &&
        snapshot->frozen_remaining_valid &&
        snapshot->frozen_remaining_nah <=
            (uint64_t)snapshot->frozen_capacity_mah *
                NANOAMP_HOURS_PER_MILLIAMP_HOUR) {
        snapshot->deficit_nah =
            (uint64_t)snapshot->frozen_capacity_mah *
                NANOAMP_HOURS_PER_MILLIAMP_HOUR -
            snapshot->frozen_remaining_nah;
        snapshot->deficit_mah = rounded_nah_to_mah(snapshot->deficit_nah);
        snapshot->deficit_valid = true;
    } else if (snapshot->frozen_capacity_valid) {
        /* Unknown starting SOC is not a reason to surrender charge control.
         * Replacing one complete observed capacity is a conservative upper
         * bound on the missing charge regardless of where the pack started. */
        snapshot->deficit_mah = snapshot->frozen_capacity_mah;
        snapshot->deficit_nah =
            (uint64_t)snapshot->frozen_capacity_mah *
            NANOAMP_HOURS_PER_MILLIAMP_HOUR;
        snapshot->deficit_valid = true;
    }
    if (snapshot->deficit_valid) {
        snapshot->target_input_nah = scaled_nah(
            snapshot->deficit_nah, snapshot->charge_factor_permille);
        snapshot->target_valid = true;
        snapshot->target_full_capacity =
            target_uses_full_capacity(snapshot);
    }
    if (snapshot->frozen_capacity_valid) {
        snapshot->safety_input_nah = scaled_nah(
            (uint64_t)snapshot->frozen_capacity_mah *
                NANOAMP_HOURS_PER_MILLIAMP_HOUR,
            profile->safety_input_factor_permille);
    }
    state->admitted_at_ms = observation->now_ms;
    state->had_active = true;
    state->stop_control_seen = false;
    state->integration_sample_valid = false;
    reset_minute_window(state, observation->now_ms);
}

static void mark_degraded(
    battery_charge_supervisor_state_t *state,
    battery_charge_supervisor_terminal_t reason) {
    state->snapshot.phase = BATTERY_CHARGE_PHASE_DEGRADED;
    state->snapshot.terminal_reason = reason;
    state->snapshot.session_authoritative = false;
    state->snapshot.full_qualified = false;
    state->full_pending = false;
}

static bool coulomb_full_evidence(
    const battery_charge_supervisor_state_t *state,
    const battery_charge_supervisor_observation_t *observation) {
    const battery_charge_supervisor_snapshot_t *snapshot = &state->snapshot;
    if (!state->had_active || !snapshot->admitted ||
        !snapshot->session_authoritative || !snapshot->target_valid ||
        snapshot->net_input_nah < 0 ||
        (uint64_t)snapshot->net_input_nah < snapshot->target_input_nah ||
        !snapshot->frozen_capacity_valid ||
        (!target_uses_full_capacity(snapshot) &&
         (snapshot->frozen_capacity_confidence ==
              BATTERY_CAPACITY_CONFIDENCE_PRIOR ||
          snapshot->frozen_capacity_confidence ==
              BATTERY_CAPACITY_CONFIDENCE_CONFLICTED)) ||
        (!snapshot->charge_factor_confident &&
         !target_uses_full_capacity(snapshot))) {
        return false;
    }
    return observation->sample_valid && observation->current_valid &&
        observation->continuity_valid && observation->authoritative &&
        observation->gauge_session == snapshot->gauge_session &&
        observation->admission.source_current &&
        observation->admission.pack_generation == snapshot->pack_generation;
}

static void integrate_distinct_sample(
    battery_charge_supervisor_state_t *state,
    const battery_charge_supervisor_observation_t *observation) {
    if (!state->snapshot.admitted || !observation->sample_valid ||
        !observation->current_valid || !observation->continuity_valid) {
        state->integration_sample_valid = false;
        return;
    }
    if (state->integration_sample_valid) {
        uint32_t elapsed_ms = observation->now_ms - state->last_sample_ms;
        if (elapsed_ms <= SAMPLE_GAP_MAX_MS) {
            int64_t current = state->last_current_ua;
            uint64_t magnitude = current < 0
                ? (uint64_t)(-(current + 1)) + 1u : (uint64_t)current;
            uint64_t delta_nah = magnitude * elapsed_ms / 3600u;
            if (current >= 0) {
                state->snapshot.positive_crosscheck_nah =
                    saturating_add_u64(
                        state->snapshot.positive_crosscheck_nah,
                        delta_nah);
            } else {
                state->snapshot.negative_crosscheck_nah =
                    saturating_add_u64(
                        state->snapshot.negative_crosscheck_nah,
                        delta_nah);
            }
        } else {
            reset_minute_window(state, observation->now_ms);
        }
    }
    state->last_sample_ms = observation->now_ms;
    state->last_current_ua = observation->current_ua;
    state->integration_sample_valid = true;
}

static uint32_t observe_minute(
    battery_charge_supervisor_state_t *state,
    const battery_charge_supervisor_observation_t *observation) {
    if (!state->snapshot.admitted || !observation->sample_valid ||
        !observation->current_valid || !observation->continuity_valid) {
        return 0u;
    }
    uint32_t result = 0u;
    if ((uint32_t)(observation->now_ms - state->minute_started_ms) >=
            BATTERY_CHARGE_SUPERVISOR_TRACE_INTERVAL_MS &&
        state->minute_sample_count != 0u) {
        uint8_t count = state->minute_sample_count;
        battery_charge_supervisor_minute_t minute = {
            .at_ms = observation->now_ms,
            .elapsed_ms = observation->now_ms - state->admitted_at_ms,
            .terminal_median_mv = median_u16(
                state->minute_terminal_mv, count),
            .compensated_median_mv = median_u16(
                state->minute_compensated_mv, count),
            .current_average_ua = (int32_t)(
                state->minute_current_sum_ua / count),
            .temperature_average_mdegc = (int32_t)(
                state->minute_temperature_sum_mdegc / count),
            .net_input_nah = state->snapshot.net_input_nah,
        };
        state->snapshot.latest_minute = minute;
        state->snapshot.minute_count =
            next_nonzero_u32(state->snapshot.minute_count);
        if (!state->previous_minute_valid ||
            minute.compensated_median_mv > state->snapshot.curve_peak_mv) {
            state->snapshot.curve_peak_mv =
                minute.compensated_median_mv;
        }
        state->snapshot.curve_drop_mv =
            state->snapshot.curve_peak_mv >= minute.compensated_median_mv
                ? (uint16_t)(state->snapshot.curve_peak_mv -
                             minute.compensated_median_mv)
                : 0u;
        if (state->previous_minute_valid) {
            int32_t slope =
                (int32_t)minute.compensated_median_mv -
                state->previous_minute_compensated_mv;
            if (slope > INT16_MAX) {
                slope = INT16_MAX;
            } else if (slope < INT16_MIN) {
                slope = INT16_MIN;
            }
            state->snapshot.curve_slope_mv_per_min = (int16_t)slope;
            state->snapshot.curve_valid = true;
        }
        state->previous_minute_compensated_mv =
            minute.compensated_median_mv;
        state->previous_minute_valid = true;
        reset_minute_window(state, observation->now_ms);
        result |= BATTERY_CHARGE_SUPERVISOR_RESULT_MINUTE;
    }

    if (state->minute_sample_count <
        BATTERY_CHARGE_SUPERVISOR_MINUTE_SAMPLE_CAP) {
        uint8_t index = state->minute_sample_count++;
        state->minute_terminal_mv[index] = observation->terminal_mv;
        state->minute_compensated_mv[index] =
            state->snapshot.compensated_mv;
        state->minute_current_sum_ua += observation->current_ua;
        state->minute_temperature_sum_mdegc +=
            observation->temperature_mdegc;
    }
    return result;
}

const battery_charge_supervisor_profile_t *
battery_charge_supervisor_revb2_profile(void) {
    return &REVB2_PROFILE;
}

void battery_charge_supervisor_init(
    battery_charge_supervisor_state_t *state,
    battery_charge_supervisor_policy_t policy) {
    if (state == NULL) {
        return;
    }
    if (policy > BATTERY_CHARGE_POLICY_ENFORCE_BOOTSTRAP) {
        policy = BATTERY_CHARGE_POLICY_OBSERVE;
    }
    memset(state, 0, sizeof(*state));
    state->snapshot.policy = policy;
    state->snapshot.phase = BATTERY_CHARGE_PHASE_DETACHED;
    state->snapshot.terminal_reason = BATTERY_CHARGE_TERMINAL_DETACHED;
    state->snapshot.blockers =
        (policy == BATTERY_CHARGE_POLICY_OBSERVE
             ? BATTERY_CHARGE_BLOCK_POLICY_OBSERVE : 0u) |
        BATTERY_CHARGE_BLOCK_NOT_ADMITTED;
}

uint32_t battery_charge_supervisor_update(
    battery_charge_supervisor_state_t *state,
    const battery_charge_supervisor_profile_t *profile,
    const battery_charge_supervisor_observation_t *observation) {
    if (state == NULL || observation == NULL) {
        return BATTERY_CHARGE_SUPERVISOR_RESULT_NONE;
    }
    if (profile == NULL) {
        profile = &REVB2_PROFILE;
    }
    uint32_t result = 0u;

    if (!observation->charger_present) {
        if (state->physically_attached) {
            state->physically_attached = false;
            state->detach_generation_reusable = true;
            state->detached_at_ms = observation->now_ms;
            reset_session_fields(state);
            result |= BATTERY_CHARGE_SUPERVISOR_RESULT_DETACHED |
                      BATTERY_CHARGE_SUPERVISOR_RESULT_TERMINAL;
        }
        return result;
    }

    if (!state->physically_attached) {
        bool reuse_generation = state->detach_generation_reusable &&
            (uint32_t)(observation->now_ms - state->detached_at_ms) <=
                ATTACH_BOUNCE_REUSE_MS;
        if (!reuse_generation) {
            state->snapshot.charge_generation = next_nonzero_u32(
                state->snapshot.charge_generation);
        }
        state->detach_generation_reusable = false;
        state->physically_attached = true;
        state->attached_at_ms = observation->now_ms;
        state->snapshot.phase = BATTERY_CHARGE_PHASE_QUALIFYING;
        state->snapshot.terminal_reason = BATTERY_CHARGE_TERMINAL_NONE;
        state->snapshot.attached = true;
        state->snapshot.chemistry = observation->chemistry;
        state->snapshot.hardware_profile_id =
            observation->hardware_profile_id;
        state->snapshot.session_authoritative =
            observation->authoritative;
        state->generation_authoritative = observation->authoritative &&
            observation->inhibit_owner_mask == 0u &&
            (!observation->charger_enable_valid ||
             (observation->charger_enable_requested &&
              observation->charger_enable_readback));
        state->full_pending = false;
        result |= BATTERY_CHARGE_SUPERVISOR_RESULT_ATTACHED;
    }

    battery_charge_supervisor_snapshot_t *snapshot = &state->snapshot;
    bool session_live = snapshot->phase == BATTERY_CHARGE_PHASE_QUALIFYING ||
                        snapshot->phase == BATTERY_CHARGE_PHASE_CHARGING ||
                        snapshot->phase == BATTERY_CHARGE_PHASE_STOPPING;
    snapshot->attached = true;
    if (session_live) {
        snapshot->terminal_mv = observation->terminal_mv;
        snapshot->current_ua = observation->current_ua;
        snapshot->temperature_mdegc = observation->temperature_mdegc;
        snapshot->elapsed_ms = snapshot->admitted
            ? observation->now_ms - state->admitted_at_ms
            : observation->now_ms - state->attached_at_ms;
        if (!observation->authoritative ||
            (!expected_stop_control(state, observation) &&
             (observation->inhibit_owner_mask != 0u ||
              (observation->charger_enable_valid &&
               (!observation->charger_enable_requested ||
                !observation->charger_enable_readback))))) {
            state->generation_authoritative = false;
            snapshot->session_authoritative = false;
        }
    }

    bool distinct_sample = session_live &&
        (!state->sample_seen ||
         state->last_sample_sequence != observation->sample_sequence);
    if (distinct_sample) {
        state->sample_seen = true;
        state->last_sample_sequence = observation->sample_sequence;
    }

    if (session_live && snapshot->admitted && observation->sample_valid &&
        observation->continuity_valid) {
        snapshot->net_input_nah = saturating_sub_i64(
            observation->session_delta_nah,
            snapshot->start_session_delta_nah);
    }

    if (session_live && observation->status_valid &&
        observation->charger_state == BATTERY_CHARGER_FAULT) {
        if (coulomb_full_evidence(state, observation)) {
            snapshot->phase = BATTERY_CHARGE_PHASE_COMPLETE_BQ;
            snapshot->terminal_reason =
                BATTERY_CHARGE_TERMINAL_BQ_FAULT_AFTER_COULOMB_FULL;
            snapshot->candidate =
                BATTERY_CHARGE_CANDIDATE_SOFTWARE_COULOMB_FULL;
            snapshot->full_candidate = true;
            snapshot->full_qualified = true;
            result |= BATTERY_CHARGE_SUPERVISOR_RESULT_FULL_QUALIFIED;
        } else {
            snapshot->phase = BATTERY_CHARGE_PHASE_FAULT;
            snapshot->terminal_reason = BATTERY_CHARGE_TERMINAL_BQ_FAULT;
            snapshot->full_qualified = false;
        }
        state->full_pending = false;
        result |= BATTERY_CHARGE_SUPERVISOR_RESULT_TERMINAL;
    } else if (snapshot->phase == BATTERY_CHARGE_PHASE_CHARGING &&
               (!observation->continuity_valid ||
                observation->gauge_session != snapshot->gauge_session ||
                !observation->admission.source_current ||
                observation->admission.pack_generation !=
                    snapshot->pack_generation)) {
        mark_degraded(
            state, BATTERY_CHARGE_TERMINAL_GAUGE_CONTINUITY_LOST);
        result |= BATTERY_CHARGE_SUPERVISOR_RESULT_DEGRADED |
                  BATTERY_CHARGE_SUPERVISOR_RESULT_TERMINAL;
    } else if (snapshot->phase == BATTERY_CHARGE_PHASE_CHARGING &&
               observation->sample_valid &&
               !observation->current_valid) {
        mark_degraded(
            state, BATTERY_CHARGE_TERMINAL_GAUGE_CONTINUITY_LOST);
        result |= BATTERY_CHARGE_SUPERVISOR_RESULT_DEGRADED |
                  BATTERY_CHARGE_SUPERVISOR_RESULT_TERMINAL;
    } else if (snapshot->phase == BATTERY_CHARGE_PHASE_CHARGING &&
               !observation->authoritative) {
        mark_degraded(state, BATTERY_CHARGE_TERMINAL_MANUAL_DEBUG_STOP);
        result |= BATTERY_CHARGE_SUPERVISOR_RESULT_DEGRADED |
                  BATTERY_CHARGE_SUPERVISOR_RESULT_TERMINAL;
    } else if (snapshot->phase == BATTERY_CHARGE_PHASE_CHARGING &&
               observation->charger_enable_valid &&
               (!observation->charger_enable_requested ||
                !observation->charger_enable_readback)) {
        mark_degraded(state, BATTERY_CHARGE_TERMINAL_MANUAL_DEBUG_STOP);
        result |= BATTERY_CHARGE_SUPERVISOR_RESULT_DEGRADED |
                  BATTERY_CHARGE_SUPERVISOR_RESULT_TERMINAL;
    }

    if (snapshot->phase == BATTERY_CHARGE_PHASE_QUALIFYING &&
        observation->status_valid &&
        observation->charger_state == BATTERY_CHARGER_ACTIVE &&
        observation->sample_valid && observation->current_valid &&
        observation->continuity_valid && observation->authoritative &&
        observation->charger_enable_valid &&
        observation->charger_enable_requested &&
        observation->charger_enable_readback &&
        observation->admission.source_current &&
        observation->chemistry == profile->chemistry &&
        observation->hardware_profile_id == profile->hardware_profile_id &&
        observation->current_ua >=
            (int32_t)profile->admission_current_min_ma * 1000) {
        freeze_admission(state, profile, observation);
        result |= BATTERY_CHARGE_SUPERVISOR_RESULT_ADMITTED;
    }

    if (session_live) {
        snapshot->compensated_mv =
            compensated_voltage_mv(observation, snapshot);
    }

    if (distinct_sample) {
        integrate_distinct_sample(state, observation);
        result |= observe_minute(state, observation);
    }

    bool can_observe_completion = observation->status_valid &&
        observation->charger_state == BATTERY_CHARGER_FULL &&
        observation->charger_enable_valid &&
        observation->charger_enable_requested &&
        observation->charger_enable_readback;
    if ((snapshot->phase == BATTERY_CHARGE_PHASE_CHARGING ||
         snapshot->phase == BATTERY_CHARGE_PHASE_QUALIFYING) &&
        can_observe_completion) {
        snapshot->candidate = BATTERY_CHARGE_CANDIDATE_BQ_COMPLETE;
        snapshot->full_candidate = true;
        if (!state->full_pending) {
            state->full_pending = true;
            state->full_since_ms = observation->now_ms;
        } else if ((uint32_t)(observation->now_ms - state->full_since_ms) >=
                   profile->completion_confirm_ms) {
            snapshot->phase = BATTERY_CHARGE_PHASE_COMPLETE_BQ;
            snapshot->terminal_reason = state->had_active
                ? BATTERY_CHARGE_TERMINAL_BQ_COMPLETE_AFTER_ACTIVE
                : BATTERY_CHARGE_TERMINAL_BQ_ALREADY_FULL_AT_ATTACH;
            snapshot->full_qualified =
                coulomb_full_evidence(state, observation);
            state->full_pending = false;
            result |= BATTERY_CHARGE_SUPERVISOR_RESULT_BQ_COMPLETE |
                      BATTERY_CHARGE_SUPERVISOR_RESULT_TERMINAL;
            if (snapshot->full_qualified) {
                result |=
                    BATTERY_CHARGE_SUPERVISOR_RESULT_FULL_QUALIFIED;
            }
        }
    } else if (snapshot->phase == BATTERY_CHARGE_PHASE_CHARGING ||
               snapshot->phase == BATTERY_CHARGE_PHASE_QUALIFYING) {
        state->full_pending = false;
        snapshot->full_candidate = false;
        if (snapshot->candidate == BATTERY_CHARGE_CANDIDATE_BQ_COMPLETE) {
            snapshot->candidate = BATTERY_CHARGE_CANDIDATE_NONE;
        }
    }

    snapshot->blockers = blockers_for_observation(state, profile, observation);

    if (snapshot->phase == BATTERY_CHARGE_PHASE_CHARGING &&
        !snapshot->full_candidate && snapshot->net_input_nah >= 0) {
        uint64_t input_nah = (uint64_t)snapshot->net_input_nah;
        if (snapshot->target_valid &&
            input_nah >= snapshot->target_input_nah) {
            snapshot->candidate =
                BATTERY_CHARGE_CANDIDATE_SOFTWARE_COULOMB_FULL;
        }

        if (snapshot->candidate ==
                BATTERY_CHARGE_CANDIDATE_SOFTWARE_COULOMB_FULL &&
            snapshot->blockers == 0u) {
            snapshot->phase = BATTERY_CHARGE_PHASE_STOPPING;
            snapshot->stop_requested = true;
            snapshot->stop_reason =
                BATTERY_CHARGE_TERMINAL_SOFTWARE_COULOMB_FULL;
            state->stop_control_seen = false;
            result |= BATTERY_CHARGE_SUPERVISOR_RESULT_STOP_REQUESTED;
        } else {
            const uint32_t safety_ignored =
                BATTERY_CHARGE_BLOCK_UNKNOWN_DEFICIT |
                BATTERY_CHARGE_BLOCK_CAPACITY_CONFIDENCE |
                BATTERY_CHARGE_BLOCK_FACTOR_UNLEARNED |
                BATTERY_CHARGE_BLOCK_CURVE_UNCALIBRATED |
                BATTERY_CHARGE_BLOCK_SOC_CONFIDENCE;
            bool safety_authorized =
                snapshot->policy != BATTERY_CHARGE_POLICY_OBSERVE &&
                snapshot->safety_input_nah != 0u &&
                input_nah >= snapshot->safety_input_nah &&
                (snapshot->blockers & ~safety_ignored) == 0u;
            if (safety_authorized) {
                snapshot->phase = BATTERY_CHARGE_PHASE_STOPPING;
                snapshot->stop_requested = true;
                snapshot->stop_reason =
                    BATTERY_CHARGE_TERMINAL_SAFETY_CHARGE_LIMIT;
                state->stop_control_seen = false;
                result |= BATTERY_CHARGE_SUPERVISOR_RESULT_STOP_REQUESTED;
            }
        }
    }

    if (snapshot->phase == BATTERY_CHARGE_PHASE_STOPPING) {
        if (observation->supervisor_inhibit_active) {
            if (!state->stop_control_seen) {
                state->stop_control_seen = true;
                state->stop_requested_ms = observation->now_ms;
            }
            if (observation->charger_enable_valid &&
                !observation->charger_enable_requested &&
                !observation->charger_enable_readback) {
                bool software_full = snapshot->stop_reason ==
                    BATTERY_CHARGE_TERMINAL_SOFTWARE_COULOMB_FULL;
                snapshot->phase = software_full
                    ? BATTERY_CHARGE_PHASE_COMPLETE_SOFTWARE
                    : BATTERY_CHARGE_PHASE_STOPPED_SAFETY;
                snapshot->terminal_reason = snapshot->stop_reason;
                snapshot->full_qualified = software_full &&
                    snapshot->session_authoritative;
                result |= BATTERY_CHARGE_SUPERVISOR_RESULT_SOFTWARE_STOPPED |
                          BATTERY_CHARGE_SUPERVISOR_RESULT_TERMINAL;
                if (snapshot->full_qualified) {
                    result |=
                        BATTERY_CHARGE_SUPERVISOR_RESULT_FULL_QUALIFIED;
                }
            } else if ((uint32_t)(observation->now_ms -
                                  state->stop_requested_ms) >=
                       profile->control_confirm_ms) {
                snapshot->phase = BATTERY_CHARGE_PHASE_FAULT;
                snapshot->terminal_reason =
                    BATTERY_CHARGE_TERMINAL_CONTROL_READBACK_FAILED;
                snapshot->full_qualified = false;
                result |= BATTERY_CHARGE_SUPERVISOR_RESULT_TERMINAL;
            }
        }
    }
    return result;
}

void battery_charge_supervisor_get_snapshot(
    const battery_charge_supervisor_state_t *state,
    battery_charge_supervisor_snapshot_t *out) {
    if (out == NULL) {
        return;
    }
    *out = state != NULL ? state->snapshot
                         : (battery_charge_supervisor_snapshot_t){0};
}

void battery_charge_supervisor_persisted_defaults(
    battery_charge_supervisor_persisted_t *persisted) {
    if (persisted != NULL) {
        memset(persisted, 0, sizeof(*persisted));
    }
}

bool battery_charge_supervisor_persisted_valid(
    const battery_charge_supervisor_persisted_t *persisted) {
    if (persisted == NULL ||
        persisted->chemistry > BATTERY_CHARGE_CHEMISTRY_LI_ION_1S ||
        persisted->configured_policy >
            BATTERY_CHARGE_POLICY_ENFORCE_BOOTSTRAP ||
        persisted->frozen_capacity_confidence >
            BATTERY_CAPACITY_CONFIDENCE_CONFLICTED ||
        persisted->frozen_soc_provenance >
            BATTERY_SOC_PROVENANCE_ANCHORED_EMPTY ||
        persisted->frozen_soc_confidence >
            BATTERY_SOC_CONFIDENCE_ANCHORED ||
        persisted->last_terminal_reason >
            BATTERY_CHARGE_TERMINAL_MAINTENANCE_REARM ||
        persisted->latched_stop_reason >
            BATTERY_CHARGE_TERMINAL_MAINTENANCE_REARM) {
        return false;
    }
    if ((persisted->configured_charge_factor_confident &&
         (persisted->configured_charge_factor_permille <
              BATTERY_CHARGE_FACTOR_MIN_PERMILLE ||
          persisted->configured_charge_factor_permille >
              BATTERY_CHARGE_FACTOR_MAX_PERMILLE)) ||
        (!persisted->configured_charge_factor_confident &&
         persisted->configured_charge_factor_permille != 0u &&
         (persisted->configured_charge_factor_permille <
              BATTERY_CHARGE_FACTOR_MIN_PERMILLE ||
          persisted->configured_charge_factor_permille >
              BATTERY_CHARGE_FACTOR_MAX_PERMILLE))) {
        return false;
    }
    if (persisted->maintenance_rearm_pending) {
        if (!persisted->supervisor_inhibit_latched ||
            persisted->latched_stop_reason !=
                BATTERY_CHARGE_TERMINAL_MAINTENANCE_REARM ||
            persisted->active_session_valid ||
            !persisted->last_terminal_valid ||
            !persisted->completion_rearm_used) {
            return false;
        }
    } else if (persisted->supervisor_inhibit_latched) {
        if ((persisted->latched_stop_reason !=
                 BATTERY_CHARGE_TERMINAL_SOFTWARE_COULOMB_FULL &&
             persisted->latched_stop_reason !=
                 BATTERY_CHARGE_TERMINAL_SAFETY_CHARGE_LIMIT) ||
            !persisted->active_session_valid) {
            return false;
        }
    } else if (persisted->latched_stop_reason !=
               BATTERY_CHARGE_TERMINAL_NONE) {
        return false;
    }
    if (persisted->active_session_valid) {
        uint64_t capacity_nah =
            (uint64_t)persisted->frozen_capacity_mah *
            NANOAMP_HOURS_PER_MILLIAMP_HOUR;
        if (persisted->charge_generation == 0u ||
            persisted->hardware_profile_id == 0u ||
            persisted->chemistry == BATTERY_CHARGE_CHEMISTRY_UNKNOWN ||
            (persisted->target_valid && !persisted->deficit_valid) ||
            (persisted->frozen_capacity_valid &&
             persisted->frozen_capacity_mah == 0u) ||
            (persisted->frozen_remaining_valid &&
             (!persisted->frozen_capacity_valid ||
              persisted->frozen_remaining_nah > capacity_nah ||
              persisted->frozen_soc_provenance ==
                  BATTERY_SOC_PROVENANCE_UNKNOWN ||
              persisted->frozen_soc_confidence ==
                  BATTERY_SOC_CONFIDENCE_NONE ||
              persisted->frozen_remaining_mah !=
                  rounded_nah_to_mah(persisted->frozen_remaining_nah))) ||
            (persisted->frozen_resistance_valid &&
             persisted->frozen_resistance_mohm == 0u) ||
            (persisted->deficit_valid &&
             (persisted->deficit_nah > capacity_nah ||
              persisted->deficit_mah !=
                  rounded_nah_to_mah(persisted->deficit_nah))) ||
            (persisted->frozen_remaining_valid &&
             persisted->deficit_valid &&
             persisted->deficit_nah !=
                 capacity_nah - persisted->frozen_remaining_nah) ||
            persisted->charge_factor_permille <
                BATTERY_CHARGE_FACTOR_MIN_PERMILLE ||
            persisted->charge_factor_permille >
                BATTERY_CHARGE_FACTOR_MAX_PERMILLE ||
            (persisted->target_valid &&
             persisted->target_input_nah != scaled_nah(
                 persisted->deficit_nah,
                 persisted->charge_factor_permille)) ||
            (persisted->frozen_capacity_valid &&
             persisted->safety_input_nah == 0u)) {
            return false;
        }
    }
    if (persisted->last_terminal_valid &&
        (persisted->hardware_profile_id == 0u ||
         persisted->chemistry == BATTERY_CHARGE_CHEMISTRY_UNKNOWN ||
         persisted->last_terminal_reason == BATTERY_CHARGE_TERMINAL_NONE ||
         persisted->last_terminal_generation == 0u)) {
        return false;
    }
    if (!persisted->last_terminal_valid &&
        (persisted->last_terminal_net_input_valid ||
         persisted->last_terminal_voltage_valid ||
         persisted->last_terminal_trace_complete ||
         persisted->last_terminal_full_qualified ||
         persisted->completion_rearm_used)) {
        return false;
    }
    if (persisted->last_terminal_full_qualified &&
        !battery_charge_supervisor_terminal_qualifies_full(
            persisted->last_terminal_reason)) {
        return false;
    }
    return true;
}

void battery_charge_supervisor_seed_generation(
    battery_charge_supervisor_state_t *state, uint32_t generation) {
    if (state != NULL && !state->physically_attached &&
        state->snapshot.phase == BATTERY_CHARGE_PHASE_DETACHED) {
        state->snapshot.charge_generation = generation;
    }
}

bool battery_charge_supervisor_restore_active(
    battery_charge_supervisor_state_t *state,
    battery_charge_supervisor_policy_t policy,
    const battery_charge_supervisor_profile_t *profile,
    const battery_charge_supervisor_persisted_t *persisted,
    const battery_charge_supervisor_observation_t *observation) {
    if (state == NULL || persisted == NULL || observation == NULL ||
        !persisted->active_session_valid ||
        !battery_charge_supervisor_persisted_valid(persisted)) {
        return false;
    }
    if (profile == NULL) {
        profile = &REVB2_PROFILE;
    }
    bool stopped_latched = persisted->supervisor_inhibit_latched;
    bool control_matches = stopped_latched
        ? observation->supervisor_inhibit_active &&
              !observation->charger_enable_requested &&
              !observation->charger_enable_readback
        : observation->inhibit_owner_mask == 0u &&
              observation->charger_enable_requested &&
              observation->charger_enable_readback;
    bool charger_state_matches = stopped_latched ||
        observation->charger_state == BATTERY_CHARGER_ACTIVE ||
        observation->charger_state == BATTERY_CHARGER_FULL;
    uint64_t expected_safety_nah = scaled_nah(
        (uint64_t)persisted->frozen_capacity_mah *
            NANOAMP_HOURS_PER_MILLIAMP_HOUR,
        profile->safety_input_factor_permille);
    if (!observation->charger_present || !observation->status_valid ||
        !charger_state_matches ||
        !observation->sample_valid || !observation->current_valid ||
        !observation->continuity_valid || !observation->authoritative ||
        !observation->charger_enable_valid || !control_matches ||
        observation->chemistry != persisted->chemistry ||
        observation->chemistry != profile->chemistry ||
        observation->hardware_profile_id !=
            persisted->hardware_profile_id ||
        observation->hardware_profile_id != profile->hardware_profile_id ||
        observation->gauge_session != persisted->gauge_session ||
        !observation->admission.source_current ||
        observation->admission.pack_generation !=
            persisted->pack_generation ||
        (persisted->frozen_capacity_valid &&
         persisted->safety_input_nah != expected_safety_nah)) {
        return false;
    }

    battery_charge_supervisor_state_t restored;
    battery_charge_supervisor_init(&restored, policy);
    battery_charge_supervisor_snapshot_t *snapshot = &restored.snapshot;
    bool terminal_stop_persisted = stopped_latched &&
        persisted->last_terminal_valid &&
        persisted->last_terminal_generation == persisted->charge_generation &&
        persisted->last_terminal_reason == persisted->latched_stop_reason;
    snapshot->phase = terminal_stop_persisted
        ? (persisted->latched_stop_reason ==
                   BATTERY_CHARGE_TERMINAL_SOFTWARE_COULOMB_FULL
               ? BATTERY_CHARGE_PHASE_COMPLETE_SOFTWARE
               : BATTERY_CHARGE_PHASE_STOPPED_SAFETY)
        : (stopped_latched ? BATTERY_CHARGE_PHASE_STOPPING
                           : BATTERY_CHARGE_PHASE_CHARGING);
    snapshot->terminal_reason = terminal_stop_persisted
        ? persisted->latched_stop_reason
        : BATTERY_CHARGE_TERMINAL_NONE;
    snapshot->charge_generation = persisted->charge_generation;
    snapshot->pack_generation = persisted->pack_generation;
    snapshot->gauge_session = persisted->gauge_session;
    snapshot->hardware_profile_id = persisted->hardware_profile_id;
    snapshot->chemistry = persisted->chemistry;
    snapshot->attached = true;
    snapshot->admitted = true;
    snapshot->session_authoritative = true;
    snapshot->start_acr_raw = persisted->start_acr_raw;
    snapshot->start_session_delta_nah =
        persisted->start_session_delta_nah;
    snapshot->net_input_nah = saturating_sub_i64(
        observation->session_delta_nah,
        persisted->start_session_delta_nah);
    snapshot->frozen_capacity_valid = persisted->frozen_capacity_valid;
    snapshot->frozen_capacity_mah = persisted->frozen_capacity_mah;
    snapshot->frozen_capacity_confidence =
        persisted->frozen_capacity_confidence;
    snapshot->frozen_remaining_valid = persisted->frozen_remaining_valid;
    snapshot->frozen_remaining_mah = persisted->frozen_remaining_mah;
    snapshot->frozen_remaining_nah = persisted->frozen_remaining_nah;
    snapshot->frozen_soc_provenance = persisted->frozen_soc_provenance;
    snapshot->frozen_soc_confidence = persisted->frozen_soc_confidence;
    snapshot->frozen_resistance_valid =
        persisted->frozen_resistance_valid;
    snapshot->frozen_resistance_mohm =
        persisted->frozen_resistance_mohm;
    snapshot->deficit_valid = persisted->deficit_valid;
    snapshot->deficit_mah = persisted->deficit_mah;
    snapshot->deficit_nah = persisted->deficit_nah;
    snapshot->charge_factor_permille =
        persisted->charge_factor_permille;
    snapshot->charge_factor_confident =
        persisted->charge_factor_confident;
    snapshot->target_valid = persisted->target_valid;
    snapshot->target_input_nah = persisted->target_input_nah;
    snapshot->target_full_capacity = target_uses_full_capacity(snapshot);
    snapshot->safety_input_nah = persisted->safety_input_nah;
    snapshot->stop_requested = stopped_latched;
    snapshot->stop_reason = stopped_latched
        ? persisted->latched_stop_reason
        : BATTERY_CHARGE_TERMINAL_NONE;
    snapshot->candidate = persisted->latched_stop_reason ==
            BATTERY_CHARGE_TERMINAL_SOFTWARE_COULOMB_FULL
        ? BATTERY_CHARGE_CANDIDATE_SOFTWARE_COULOMB_FULL
        : BATTERY_CHARGE_CANDIDATE_NONE;
    snapshot->full_qualified = terminal_stop_persisted &&
        persisted->latched_stop_reason ==
            BATTERY_CHARGE_TERMINAL_SOFTWARE_COULOMB_FULL;
    snapshot->terminal_mv = observation->terminal_mv;
    snapshot->current_ua = observation->current_ua;
    snapshot->temperature_mdegc = observation->temperature_mdegc;
    snapshot->compensated_mv =
        compensated_voltage_mv(observation, snapshot);

    restored.physically_attached = true;
    restored.generation_authoritative = true;
    restored.had_active = true;
    restored.stop_control_seen = stopped_latched &&
        !terminal_stop_persisted;
    restored.stop_requested_ms = observation->now_ms;
    restored.sample_seen = true;
    restored.last_sample_sequence = observation->sample_sequence;
    restored.admitted_at_ms = observation->now_ms;
    restored.attached_at_ms = observation->now_ms;
    reset_minute_window(&restored, observation->now_ms);
    snapshot->blockers = blockers_for_observation(
        &restored, profile, observation);
    *state = restored;
    return true;
}

void battery_charge_supervisor_reject_restore(
    battery_charge_supervisor_state_t *state,
    battery_charge_supervisor_policy_t policy,
    uint32_t generation,
    const battery_charge_supervisor_profile_t *profile,
    const battery_charge_supervisor_observation_t *observation) {
    if (state == NULL || observation == NULL ||
        !observation->charger_present) {
        return;
    }
    if (profile == NULL) {
        profile = &REVB2_PROFILE;
    }
    battery_charge_supervisor_init(state, policy);
    state->physically_attached = true;
    state->generation_authoritative = false;
    state->attached_at_ms = observation->now_ms;
    state->snapshot.phase = BATTERY_CHARGE_PHASE_DEGRADED;
    state->snapshot.terminal_reason =
        BATTERY_CHARGE_TERMINAL_GAUGE_CONTINUITY_LOST;
    state->snapshot.charge_generation = generation;
    state->snapshot.attached = true;
    state->snapshot.session_authoritative = false;
    state->snapshot.terminal_mv = observation->terminal_mv;
    state->snapshot.current_ua = observation->current_ua;
    state->snapshot.temperature_mdegc = observation->temperature_mdegc;
    state->snapshot.chemistry = observation->chemistry;
    state->snapshot.hardware_profile_id = observation->hardware_profile_id;
    state->snapshot.blockers = blockers_for_observation(
        state, profile, observation);
}

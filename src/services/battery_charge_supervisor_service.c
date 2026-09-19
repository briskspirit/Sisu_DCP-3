#include "services/battery_charge_supervisor_service.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>

#include "services/battery_learning_service.h"
#include "services/board_diag_service.h"
#include "services/charger_control_service.h"
#include "storage/store_service.h"

static battery_charge_supervisor_state_t s_state;
static battery_charge_supervisor_persisted_t s_pending_persisted;
static battery_charge_supervisor_service_snapshot_t s_snapshot;
static battery_charge_supervisor_minute_t
    s_trace[BATTERY_CHARGE_SUPERVISOR_TRACE_CAP];
static bool s_restore_pending;
static bool s_release_inhibit_pending;
static bool s_incompatible_latch;
static battery_charge_maintenance_phase_t s_maintenance_phase;
static uint32_t s_maintenance_disabled_at_ms;

static uint32_t saturating_increment_u32(uint32_t value) {
    return value == UINT32_MAX ? value : value + 1u;
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

static battery_charge_supervisor_chemistry_t map_chemistry(
    board_diag_chemistry_t chemistry) {
    switch (chemistry) {
    case BOARD_DIAG_CHEM_NI_MH:
        return BATTERY_CHARGE_CHEMISTRY_NIMH_2S;
    case BOARD_DIAG_CHEM_LI_ION:
        return BATTERY_CHARGE_CHEMISTRY_LI_ION_1S;
    case BOARD_DIAG_CHEM_ALK:
    case BOARD_DIAG_CHEM_COUNT:
        return BATTERY_CHARGE_CHEMISTRY_UNKNOWN;
    }
    return BATTERY_CHARGE_CHEMISTRY_UNKNOWN;
}

static battery_charge_supervisor_admission_t map_admission(
    const battery_learning_snapshot_t *learning,
    uint32_t gauge_session) {
    /* A freshly initialized learner may safely consume its persisted ACR
     * origin before its first poll only when the HAL reports session zero,
     * which is the learner's own "same powered gauge" rule. Any nonzero or
     * changed session must be processed by the learner before admission. */
    bool source_current = learning->gauge_session_bound
        ? learning->gauge_session == gauge_session
        : gauge_session == 0u;
    uint16_t capacity_mah = learning->soc_capacity_mah;
    if (capacity_mah == 0u) {
        capacity_mah = learning->learned_capacity_valid
            ? learning->learned_capacity_mah
            : learning->nominal_capacity_mah;
    }
    bool prediction_unconfirmed =
        learning->capacity_prediction_exhausted;
    battery_charge_supervisor_admission_t admission = {
        .source_current = source_current,
        /* Capacity and SOC are deliberately independent here. Observed capacity
         * can authorize a complete-capacity replacement target even when
         * starting SOC is unknown; exact remaining charge permits an earlier
         * deficit target. A prior-only capacity owns only the safety ceiling. */
        .capacity_valid = capacity_mah != 0u,
        .capacity_mah = capacity_mah,
        /* Once discharge has run through the estimate without a physical
         * endpoint, neither that capacity nor clamped-zero remaining charge may
         * authorize a software charge stop. Keep the learner's full anchor live
         * for later measurement, but admit this charge as conflicted/unknown so
         * BQ hardware remains the termination authority. */
        .capacity_confidence = prediction_unconfirmed
            ? BATTERY_CAPACITY_CONFIDENCE_CONFLICTED
            : learning->capacity_confidence,
        .remaining_valid = learning->remaining_capacity_valid &&
            !prediction_unconfirmed,
        .remaining_mah = learning->remaining_capacity_mah,
        .remaining_nah = learning->remaining_capacity_nah,
        .soc_provenance = learning->soc_provenance,
        .soc_confidence = learning->soc_confidence,
        .natural_empty_start = learning->natural_empty_valid,
        .pack_generation = learning->pack_generation,
        .charge_factor_permille =
            s_pending_persisted.configured_charge_factor_permille,
        .charge_factor_confident =
            s_pending_persisted.configured_charge_factor_confident,
    };
    if (learning->current_resistance_bin_valid) {
        battery_resistance_bin_t bin = learning->current_resistance_bin;
        if ((unsigned)bin < BATTERY_LEARNING_RESISTANCE_BIN_COUNT &&
            learning->resistance_mohm[bin] != 0u) {
            admission.resistance_valid = true;
            admission.resistance_mohm = learning->resistance_mohm[bin];
        }
    }
    return admission;
}

static battery_charger_state_t map_charger_state(
    board_diag_charge_state_t state) {
    switch (state) {
    case BOARD_DIAG_CHARGE_ACTIVE:
        return BATTERY_CHARGER_ACTIVE;
    case BOARD_DIAG_CHARGE_FULL:
        return BATTERY_CHARGER_FULL;
    case BOARD_DIAG_CHARGE_FAULT:
        return BATTERY_CHARGER_FAULT;
    case BOARD_DIAG_CHARGE_DISABLED:
    case BOARD_DIAG_CHARGE_DETACHED:
        return BATTERY_CHARGER_DETACHED;
    }
    return BATTERY_CHARGER_DETACHED;
}

static void collect_observation(
    uint32_t now_ms,
    const battery_charge_supervisor_profile_t *profile,
    battery_charge_supervisor_observation_t *out) {
    battery_learning_observation_t gauge;
    board_diag_get_battery_learning_observation(now_ms, &gauge);
    board_diag_snapshot_t board;
    board_diag_get_snapshot(&board);
    battery_learning_service_snapshot_t learner;
    battery_learning_service_get_snapshot(&learner);

    *out = (battery_charge_supervisor_observation_t){
        .now_ms = now_ms,
        .sample_sequence = gauge.sample_sequence,
        .gauge_session = gauge.gauge_session,
        .acr_raw = gauge.acr_raw,
        .session_delta_nah = gauge.session_delta_nah,
        .terminal_mv = gauge.terminal_mv,
        .reference_mv = gauge.reference_mv,
        .current_ua = gauge.current_ua,
        .temperature_mdegc = gauge.temperature_mdegc,
        .sample_valid = gauge.sample_valid,
        .reference_valid = gauge.reference_valid,
        .current_valid = gauge.current_valid,
        .continuity_valid = gauge.continuity_valid,
        .authoritative = gauge.authoritative,
        .charger_present = board.charger_connected,
        .charger_input_mv = board.charger_input_mv,
        .status_valid = board.charge_status_valid,
        .charger_state = map_charger_state(board.charge_state),
        .charger_enable_requested = board.charger_enabled_requested,
        .charger_enable_readback = board.charger_enabled,
        .charger_enable_valid = board.charger_enable_valid,
        .inhibit_owner_mask = board.charger_inhibit_owner_mask,
        .supervisor_inhibit_active =
            (board.charger_inhibit_owner_mask &
             CHARGER_INHIBIT_SUPERVISOR) != 0u,
        .chemistry = map_chemistry(board.chemistry),
        .hardware_profile_id = profile->hardware_profile_id,
        .admission = map_admission(&learner.model, gauge.gauge_session),
    };
}

static void append_trace(
    const battery_charge_supervisor_minute_t *minute) {
    s_trace[s_snapshot.trace_next] = *minute;
    s_snapshot.trace_next = (uint8_t)(
        (s_snapshot.trace_next + 1u) % BATTERY_CHARGE_SUPERVISOR_TRACE_CAP);
    if (s_snapshot.trace_count < BATTERY_CHARGE_SUPERVISOR_TRACE_CAP) {
        s_snapshot.trace_count++;
    }
}

static void refresh_snapshot(void) {
    battery_charge_supervisor_get_snapshot(&s_state, &s_snapshot.model);
    s_snapshot.restore_pending = s_restore_pending;
    s_snapshot.release_inhibit_pending = s_release_inhibit_pending;
    s_snapshot.maintenance_phase = s_maintenance_phase;
    s_snapshot.persisted = s_pending_persisted;
}

static void try_persist(void) {
    if (!s_snapshot.persistence_pending || !store_service_ready()) {
        return;
    }
    store_status_t status =
        store_battery_charge_supervisor_set(&s_pending_persisted);
    if (status == STORE_STATUS_OK) {
        s_snapshot.persistence_pending = false;
    } else {
        s_snapshot.persistence_failures = saturating_increment_u32(
            s_snapshot.persistence_failures);
    }
}

static void request_persist(
    const battery_charge_supervisor_persisted_t *persisted) {
    if (!battery_charge_supervisor_persisted_valid(persisted)) {
        s_snapshot.persistence_failures = saturating_increment_u32(
            s_snapshot.persistence_failures);
        return;
    }
    s_pending_persisted = *persisted;
    s_snapshot.persistence_pending = true;
    s_snapshot.persistence_requests = saturating_increment_u32(
        s_snapshot.persistence_requests);
}

static bool supervisor_store_durable(void) {
    if (s_snapshot.storage_unavailable || s_snapshot.persistence_pending || !store_service_ready()) {
        return false;
    }
    store_diag_snapshot_t store;
    store_service_get_diag(&store);
    uint16_t mask = (uint16_t)(
        UINT16_C(1) << STORE_UNIT_BATTERY_CHARGE_SUPERVISOR);
    return store.ready && (store.dirty_mask & mask) == 0u &&
        (store.degraded_mask & mask) == 0u;
}

static void service_control_preflight(void) {
    bool inhibit = s_pending_persisted.supervisor_inhibit_latched;
    if ((inhibit && !supervisor_store_durable()) ||
        (!inhibit && !s_release_inhibit_pending)) {
        return;
    }

    charger_control_snapshot_t control;
    charger_control_service_get_snapshot(&control);
    bool owned = (control.inhibit_owner_mask &
                  CHARGER_INHIBIT_SUPERVISOR) != 0u;
    bool target_matches = inhibit ? !control.requested_enabled
                                  : control.requested_enabled;
    bool readback_matches = control.readback_valid &&
        (inhibit ? !control.actual_enabled : control.actual_enabled);
    if (owned == inhibit && target_matches && readback_matches) {
        if (!inhibit) {
            s_release_inhibit_pending = false;
        }
        return;
    }

    s_snapshot.control_requests = saturating_increment_u32(
        s_snapshot.control_requests);
    if (!charger_control_service_set_inhibit(
            CHARGER_INHIBIT_SUPERVISOR, inhibit)) {
        s_snapshot.control_failures = saturating_increment_u32(
            s_snapshot.control_failures);
        return;
    }
    if (!inhibit) {
        s_release_inhibit_pending = false;
    }
}

static void clear_active_record(
    battery_charge_supervisor_persisted_t *persisted) {
    persisted->active_session_valid = false;
    persisted->pack_generation = 0u;
    persisted->gauge_session = 0u;
    persisted->start_acr_raw = 0u;
    persisted->start_session_delta_nah = 0;
    persisted->frozen_capacity_valid = false;
    persisted->frozen_capacity_mah = 0u;
    persisted->frozen_capacity_confidence =
        BATTERY_CAPACITY_CONFIDENCE_PRIOR;
    persisted->frozen_remaining_valid = false;
    persisted->frozen_remaining_mah = 0u;
    persisted->frozen_remaining_nah = 0u;
    persisted->frozen_soc_provenance = BATTERY_SOC_PROVENANCE_UNKNOWN;
    persisted->frozen_soc_confidence = BATTERY_SOC_CONFIDENCE_NONE;
    persisted->frozen_resistance_valid = false;
    persisted->frozen_resistance_mohm = 0u;
    persisted->deficit_valid = false;
    persisted->deficit_mah = 0u;
    persisted->deficit_nah = 0u;
    persisted->charge_factor_permille = 0u;
    persisted->charge_factor_confident = false;
    persisted->target_valid = false;
    persisted->target_input_nah = 0u;
    persisted->safety_input_nah = 0u;
}

static bool maintenance_soc_ready(
    const battery_charge_supervisor_observation_t *observation) {
    const battery_charge_supervisor_admission_t *admission =
        &observation->admission;
    if (!observation->sample_valid || !observation->current_valid ||
        !observation->continuity_valid || !observation->authoritative ||
        !admission->source_current || !admission->capacity_valid ||
        !admission->remaining_valid ||
        admission->capacity_confidence ==
            BATTERY_CAPACITY_CONFIDENCE_PRIOR ||
        admission->capacity_confidence ==
            BATTERY_CAPACITY_CONFIDENCE_CONFLICTED ||
        admission->soc_confidence != BATTERY_SOC_CONFIDENCE_ANCHORED ||
        admission->capacity_mah == 0u) {
        return false;
    }
    uint64_t capacity_nah =
        (uint64_t)admission->capacity_mah * UINT64_C(1000000);
    return admission->remaining_nah <=
        capacity_nah * BATTERY_CHARGE_MAINTENANCE_RESTART_SOC_PERCENT /
            100u;
}

static bool maintenance_full_hold_available(void) {
    bool complete_phase =
        s_snapshot.model.phase == BATTERY_CHARGE_PHASE_COMPLETE_BQ ||
        s_snapshot.model.phase == BATTERY_CHARGE_PHASE_COMPLETE_SOFTWARE;
    bool durable_full = s_pending_persisted.last_terminal_valid &&
        s_pending_persisted.last_terminal_full_qualified &&
        battery_charge_supervisor_terminal_qualifies_full(
            s_pending_persisted.last_terminal_reason);
    return complete_phase &&
        (s_snapshot.model.full_qualified || durable_full);
}

static void update_maintenance_readiness(
    const battery_charge_supervisor_observation_t *observation) {
    s_snapshot.maintenance_full_hold = maintenance_full_hold_available();
    s_snapshot.maintenance_soc_ready = maintenance_soc_ready(observation);
    s_snapshot.maintenance_voltage_mv = observation->reference_mv;
    s_snapshot.maintenance_voltage_ready = observation->reference_valid &&
        observation->reference_mv < BATTERY_CHARGE_MAINTENANCE_RESTART_MV;
}

static bool unqualified_bq_recovery_ready(
    uint32_t result,
    const battery_charge_supervisor_profile_t *profile,
    const battery_charge_supervisor_observation_t *observation) {
    battery_charge_supervisor_terminal_t reason =
        s_snapshot.model.terminal_reason;
    return
        (result & BATTERY_CHARGE_SUPERVISOR_RESULT_BQ_COMPLETE) != 0u &&
        (result & BATTERY_CHARGE_SUPERVISOR_RESULT_FULL_QUALIFIED) == 0u &&
        s_snapshot.model.phase == BATTERY_CHARGE_PHASE_COMPLETE_BQ &&
        (reason == BATTERY_CHARGE_TERMINAL_BQ_COMPLETE_AFTER_ACTIVE ||
         reason == BATTERY_CHARGE_TERMINAL_BQ_ALREADY_FULL_AT_ATTACH) &&
        s_maintenance_phase == BATTERY_CHARGE_MAINTENANCE_IDLE &&
        !s_release_inhibit_pending &&
        !s_pending_persisted.supervisor_inhibit_latched &&
        !s_pending_persisted.maintenance_rearm_pending &&
        !s_pending_persisted.completion_rearm_used &&
        !s_pending_persisted.last_terminal_full_qualified &&
        s_pending_persisted.configured_policy ==
            BATTERY_CHARGE_POLICY_ENFORCE_ANCHORED &&
        s_pending_persisted.configured_charge_factor_confident &&
        observation->charger_present && observation->status_valid &&
        observation->charger_state == BATTERY_CHARGER_FULL &&
        observation->charger_enable_valid &&
        observation->charger_enable_requested &&
        observation->charger_enable_readback &&
        observation->inhibit_owner_mask == 0u &&
        observation->chemistry == profile->chemistry &&
        observation->hardware_profile_id == profile->hardware_profile_id &&
        maintenance_soc_ready(observation);
}

static void request_maintenance_inhibit(void) {
    battery_charge_supervisor_persisted_t persisted =
        s_pending_persisted;
    clear_active_record(&persisted);
    persisted.supervisor_inhibit_latched = true;
    persisted.latched_stop_reason =
        BATTERY_CHARGE_TERMINAL_MAINTENANCE_REARM;
    persisted.maintenance_rearm_pending = true;
    persisted.completion_rearm_used = true;
    /* One qualified FULL authorizes one maintenance attempt. A failed BQ
     * restart must not become an endless /CE pulse loop. */
    persisted.last_terminal_full_qualified = false;
    request_persist(&persisted);
    s_maintenance_phase = BATTERY_CHARGE_MAINTENANCE_WAIT_INHIBIT;
}

static void request_maintenance_release(void) {
    battery_charge_supervisor_persisted_t persisted =
        s_pending_persisted;
    persisted.supervisor_inhibit_latched = false;
    persisted.latched_stop_reason = BATTERY_CHARGE_TERMINAL_NONE;
    persisted.maintenance_rearm_pending = false;
    request_persist(&persisted);
    s_release_inhibit_pending = true;
    s_maintenance_phase = BATTERY_CHARGE_MAINTENANCE_WAIT_RELEASE;
    /* Re-enabling charge is the fail-open direction. Do it immediately even
     * if the cleared journal record still needs a flash retry. */
    service_control_preflight();
}

static void reset_model_for_attached_rearm(void) {
    battery_charge_supervisor_policy_t policy =
        s_pending_persisted.configured_policy;
    uint32_t generation = s_pending_persisted.charge_generation;
    battery_charge_supervisor_init(&s_state, policy);
    battery_charge_supervisor_seed_generation(&s_state, generation);
    s_snapshot.restored_after_reset = false;
}

static void copy_active_record(
    battery_charge_supervisor_persisted_t *persisted,
    const battery_charge_supervisor_snapshot_t *model) {
    persisted->hardware_profile_id = model->hardware_profile_id;
    persisted->chemistry = model->chemistry;
    persisted->charge_generation = model->charge_generation;
    persisted->active_session_valid = true;
    persisted->pack_generation = model->pack_generation;
    persisted->gauge_session = model->gauge_session;
    persisted->start_acr_raw = model->start_acr_raw;
    persisted->start_session_delta_nah = model->start_session_delta_nah;
    persisted->frozen_capacity_valid = model->frozen_capacity_valid;
    persisted->frozen_capacity_mah = model->frozen_capacity_mah;
    persisted->frozen_capacity_confidence =
        model->frozen_capacity_confidence;
    persisted->frozen_remaining_valid = model->frozen_remaining_valid;
    persisted->frozen_remaining_mah = model->frozen_remaining_mah;
    persisted->frozen_remaining_nah = model->frozen_remaining_nah;
    persisted->frozen_soc_provenance = model->frozen_soc_provenance;
    persisted->frozen_soc_confidence = model->frozen_soc_confidence;
    persisted->frozen_resistance_valid = model->frozen_resistance_valid;
    persisted->frozen_resistance_mohm = model->frozen_resistance_mohm;
    persisted->deficit_valid = model->deficit_valid;
    persisted->deficit_mah = model->deficit_mah;
    persisted->deficit_nah = model->deficit_nah;
    persisted->charge_factor_permille = model->charge_factor_permille;
    persisted->charge_factor_confident = model->charge_factor_confident;
    persisted->target_valid = model->target_valid;
    persisted->target_input_nah = model->target_input_nah;
    persisted->safety_input_nah = model->safety_input_nah;
}

static void persist_generation(
    const battery_charge_supervisor_snapshot_t *model) {
    battery_charge_supervisor_persisted_t persisted = s_pending_persisted;
    clear_active_record(&persisted);
    persisted.charge_generation = model->charge_generation;
    request_persist(&persisted);
}

static void persist_admission(
    const battery_charge_supervisor_snapshot_t *model) {
    battery_charge_supervisor_persisted_t persisted = s_pending_persisted;
    copy_active_record(&persisted, model);
    request_persist(&persisted);
}

static void persist_stop_request(
    const battery_charge_supervisor_snapshot_t *model) {
    battery_charge_supervisor_persisted_t persisted = s_pending_persisted;
    copy_active_record(&persisted, model);
    persisted.supervisor_inhibit_latched = true;
    persisted.latched_stop_reason = model->stop_reason;
    persisted.maintenance_rearm_pending = false;
    request_persist(&persisted);
}

static void persist_terminal(
    const battery_charge_supervisor_snapshot_t *model,
    battery_charge_supervisor_terminal_t reason,
    bool net_input_valid,
    bool voltage_valid,
    bool trace_complete) {
    battery_charge_supervisor_persisted_t persisted = s_pending_persisted;
    bool retain_latch = persisted.supervisor_inhibit_latched &&
        reason != BATTERY_CHARGE_TERMINAL_DETACHED;
    if (retain_latch) {
        copy_active_record(&persisted, model);
    } else {
        clear_active_record(&persisted);
        persisted.supervisor_inhibit_latched = false;
        persisted.latched_stop_reason = BATTERY_CHARGE_TERMINAL_NONE;
        persisted.maintenance_rearm_pending = false;
    }
    persisted.hardware_profile_id = model->hardware_profile_id;
    persisted.chemistry = model->chemistry;
    persisted.charge_generation = model->charge_generation;
    bool preserve_prior_full =
        reason == BATTERY_CHARGE_TERMINAL_BQ_ALREADY_FULL_AT_ATTACH &&
        !model->full_qualified &&
        persisted.last_terminal_full_qualified;
    if (!preserve_prior_full) {
        persisted.last_terminal_valid = true;
        persisted.last_terminal_full_qualified = model->full_qualified;
        persisted.last_terminal_net_input_valid = net_input_valid;
        persisted.last_terminal_voltage_valid = voltage_valid;
        persisted.last_terminal_trace_complete = trace_complete;
        persisted.last_terminal_reason = reason;
        persisted.last_terminal_generation = model->charge_generation;
        persisted.last_terminal_elapsed_ms = model->elapsed_ms;
        persisted.last_terminal_net_input_nah = model->net_input_nah;
        persisted.last_terminal_mv = model->terminal_mv;
        persisted.last_curve_peak_mv = model->curve_peak_mv;
        persisted.last_curve_drop_mv = model->curve_drop_mv;
        persisted.last_curve_slope_mv_per_min =
            model->curve_slope_mv_per_min;
    }
    if (reason == BATTERY_CHARGE_TERMINAL_DETACHED ||
        model->full_qualified) {
        persisted.completion_rearm_used = false;
    }
    request_persist(&persisted);
}

static void persist_reset_detach(
    const battery_charge_supervisor_observation_t *observation) {
    bool net_input_valid = observation->sample_valid &&
        observation->current_valid &&
        observation->continuity_valid &&
        observation->authoritative &&
        observation->gauge_session == s_pending_persisted.gauge_session;
    battery_charge_supervisor_snapshot_t terminal = {
        .charge_generation = s_pending_persisted.charge_generation,
        .hardware_profile_id = s_pending_persisted.hardware_profile_id,
        .chemistry = s_pending_persisted.chemistry,
        .terminal_mv = observation->sample_valid
            ? observation->terminal_mv : 0u,
    };
    if (net_input_valid) {
        terminal.net_input_nah = saturating_sub_i64(
            observation->session_delta_nah,
            s_pending_persisted.start_session_delta_nah);
    }
    persist_terminal(&terminal, BATTERY_CHARGE_TERMINAL_DETACHED,
                     net_input_valid, observation->sample_valid, false);
}

static bool restore_evidence_ready(
    const battery_charge_supervisor_observation_t *observation) {
    return observation->status_valid && observation->sample_valid &&
        observation->current_valid && observation->continuity_valid &&
        observation->charger_enable_valid;
}

static uint32_t service_maintenance_step(
    uint32_t now_ms,
    const battery_charge_supervisor_observation_t *observation,
    bool *run_model) {
    *run_model = false;
    if (!observation->charger_present) {
        battery_charge_supervisor_persisted_t persisted =
            s_pending_persisted;
        clear_active_record(&persisted);
        persisted.supervisor_inhibit_latched = false;
        persisted.latched_stop_reason = BATTERY_CHARGE_TERMINAL_NONE;
        persisted.maintenance_rearm_pending = false;
        persisted.completion_rearm_used = false;
        request_persist(&persisted);
        s_release_inhibit_pending = true;
        s_maintenance_phase = BATTERY_CHARGE_MAINTENANCE_IDLE;
        reset_model_for_attached_rearm();
        return BATTERY_CHARGE_SUPERVISOR_RESULT_DETACHED |
               BATTERY_CHARGE_SUPERVISOR_RESULT_TERMINAL;
    }

    bool supervisor_owned =
        (observation->inhibit_owner_mask & CHARGER_INHIBIT_SUPERVISOR) !=
        0u;
    bool control_disabled = supervisor_owned &&
        observation->charger_enable_valid &&
        !observation->charger_enable_requested &&
        !observation->charger_enable_readback;

    if (s_maintenance_phase ==
        BATTERY_CHARGE_MAINTENANCE_WAIT_INHIBIT) {
        if (supervisor_store_durable() &&
            s_pending_persisted.maintenance_rearm_pending &&
            control_disabled) {
            s_maintenance_disabled_at_ms = now_ms;
            s_maintenance_phase =
                BATTERY_CHARGE_MAINTENANCE_RESET_HOLD;
        }
        return BATTERY_CHARGE_SUPERVISOR_RESULT_NONE;
    }

    if (s_maintenance_phase == BATTERY_CHARGE_MAINTENANCE_RESET_HOLD) {
        if (!control_disabled) {
            s_maintenance_phase =
                BATTERY_CHARGE_MAINTENANCE_WAIT_INHIBIT;
        } else if ((uint32_t)(now_ms - s_maintenance_disabled_at_ms) >=
                   BATTERY_CHARGE_MAINTENANCE_CE_RESET_MS) {
            request_maintenance_release();
        }
        return BATTERY_CHARGE_SUPERVISOR_RESULT_NONE;
    }

    if (s_maintenance_phase == BATTERY_CHARGE_MAINTENANCE_WAIT_RELEASE &&
        supervisor_store_durable() &&
        !s_pending_persisted.maintenance_rearm_pending &&
        !supervisor_owned && observation->charger_enable_valid &&
        observation->charger_enable_requested &&
        observation->charger_enable_readback) {
        reset_model_for_attached_rearm();
        s_maintenance_phase = BATTERY_CHARGE_MAINTENANCE_IDLE;
        *run_model = true;
        return BATTERY_CHARGE_SUPERVISOR_RESULT_MAINTENANCE_RESTARTED;
    }
    return BATTERY_CHARGE_SUPERVISOR_RESULT_NONE;
}

bool battery_charge_supervisor_service_boot_inhibit_required(void) {
    battery_charge_supervisor_persisted_t stored;
    /* Unreadable is not an absent stop latch. Only a valid record (including
     * genuine first-use defaults) can authorize the initial enabled state. */
    return store_battery_charge_supervisor_get(&stored) != STORE_STATUS_OK ||
        !battery_charge_supervisor_persisted_valid(&stored) ||
        stored.supervisor_inhibit_latched;
}

void battery_charge_supervisor_service_init(void) {
    memset(&s_snapshot, 0, sizeof(s_snapshot));
    memset(s_trace, 0, sizeof(s_trace));
    battery_charge_supervisor_persisted_defaults(&s_pending_persisted);
    s_restore_pending = false;
    s_release_inhibit_pending = false;
    s_incompatible_latch = false;
    s_maintenance_phase = BATTERY_CHARGE_MAINTENANCE_IDLE;
    s_maintenance_disabled_at_ms = 0u;

    battery_charge_supervisor_persisted_t stored;
    store_status_t status = store_battery_charge_supervisor_get(&stored);
    const battery_charge_supervisor_profile_t *profile =
        battery_charge_supervisor_revb2_profile();
    bool structurally_valid = status == STORE_STATUS_OK &&
        battery_charge_supervisor_persisted_valid(&stored);
    s_snapshot.storage_unavailable = !structurally_valid;
    bool has_profile_evidence = structurally_valid &&
        (stored.active_session_valid || stored.last_terminal_valid ||
         stored.configured_policy != BATTERY_CHARGE_POLICY_OBSERVE ||
         stored.configured_charge_factor_permille != 0u);
    bool profile_valid = !has_profile_evidence ||
        (stored.hardware_profile_id == profile->hardware_profile_id &&
         stored.chemistry == profile->chemistry);
    bool valid = structurally_valid && profile_valid;
    if (valid) {
        s_pending_persisted = stored;
    } else if (structurally_valid && stored.supervisor_inhibit_latched) {
        /* A stop latch outranks an evidence-domain migration. Keep /CE off
         * until a real detach; only then may the obsolete record be retired. */
        s_pending_persisted = stored;
        s_incompatible_latch = true;
    } else if (structurally_valid) {
        /* Keep the diagnostic generation monotonic, but never carry an ACR
         * baseline or terminal summary across an evidence-domain change. */
        s_pending_persisted.charge_generation = stored.charge_generation;
    }
    battery_charge_supervisor_init(
        &s_state,
        valid ? s_pending_persisted.configured_policy
              : BATTERY_CHARGE_POLICY_OBSERVE);
    battery_charge_supervisor_seed_generation(
        &s_state, s_pending_persisted.charge_generation);
    s_restore_pending = valid && s_pending_persisted.active_session_valid;
    if (valid && s_pending_persisted.maintenance_rearm_pending) {
        s_restore_pending = false;
        s_maintenance_phase =
            BATTERY_CHARGE_MAINTENANCE_WAIT_INHIBIT;
    }
    s_snapshot.initialized = true;
    if (!valid && !s_incompatible_latch && !s_snapshot.storage_unavailable) {
        request_persist(&s_pending_persisted);
        try_persist();
    }
    refresh_snapshot();
}

bool battery_charge_supervisor_service_release_for_battery_floor(void) {
    if (!s_snapshot.initialized) {
        battery_charge_supervisor_service_init();
    }
    if (s_snapshot.storage_unavailable) return false;

    charger_control_snapshot_t control;
    charger_control_service_get_snapshot(&control);
    bool supervisor_owned =
        (control.inhibit_owner_mask & CHARGER_INHIBIT_SUPERVISOR) != 0u;
    bool latch_present =
        s_pending_persisted.supervisor_inhibit_latched;
    if (!latch_present && !supervisor_owned &&
        !s_release_inhibit_pending) {
        return false;
    }

    bool already_releasing = s_release_inhibit_pending && !latch_present;
    if (!already_releasing) {
        battery_charge_supervisor_persisted_t persisted;
        if (s_incompatible_latch) {
            uint32_t generation = s_pending_persisted.charge_generation;
            battery_charge_supervisor_persisted_defaults(&persisted);
            persisted.charge_generation = generation;
        } else {
            persisted = s_pending_persisted;
            clear_active_record(&persisted);
            persisted.supervisor_inhibit_latched = false;
            persisted.latched_stop_reason =
                BATTERY_CHARGE_TERMINAL_NONE;
            persisted.maintenance_rearm_pending = false;
        }
        request_persist(&persisted);
    }

    s_restore_pending = false;
    s_incompatible_latch = false;
    s_release_inhibit_pending = true;
    s_maintenance_phase = BATTERY_CHARGE_MAINTENANCE_WAIT_RELEASE;
    service_control_preflight();
    refresh_snapshot();
    return true;
}

uint32_t battery_charge_supervisor_service_poll(uint32_t now_ms) {
    if (!s_snapshot.initialized) {
        battery_charge_supervisor_service_init();
    }
    if (s_snapshot.storage_unavailable) {
        (void)charger_control_service_set_inhibit(CHARGER_INHIBIT_SUPERVISOR, true);
        return BATTERY_CHARGE_SUPERVISOR_RESULT_NONE;
    }

    try_persist();
    service_control_preflight();

    const battery_charge_supervisor_profile_t *profile =
        battery_charge_supervisor_revb2_profile();
    battery_charge_supervisor_observation_t observation;
    collect_observation(now_ms, profile, &observation);
    battery_charge_supervisor_snapshot_t before;
    battery_charge_supervisor_get_snapshot(&s_state, &before);
    bool active_record_before = s_pending_persisted.active_session_valid;
    bool reset_detach = false;
    bool restore_rejected = false;
    uint32_t result = BATTERY_CHARGE_SUPERVISOR_RESULT_NONE;

    if (s_incompatible_latch) {
        if (!observation.charger_present) {
            uint32_t generation = s_pending_persisted.charge_generation;
            battery_charge_supervisor_persisted_defaults(
                &s_pending_persisted);
            s_pending_persisted.charge_generation = generation;
            request_persist(&s_pending_persisted);
            s_release_inhibit_pending = true;
            s_incompatible_latch = false;
            try_persist();
            if (s_release_inhibit_pending) {
                service_control_preflight();
            }
        }
        refresh_snapshot();
        return BATTERY_CHARGE_SUPERVISOR_RESULT_NONE;
    }

    bool run_model = true;
    if (s_maintenance_phase != BATTERY_CHARGE_MAINTENANCE_IDLE) {
        result = service_maintenance_step(
            now_ms, &observation, &run_model);
        if (!run_model) {
            s_snapshot.update_count = saturating_increment_u32(
                s_snapshot.update_count);
            s_snapshot.last_model_result = result;
            if ((result & BATTERY_CHARGE_SUPERVISOR_RESULT_TERMINAL) != 0u) {
                s_snapshot.terminal_events = saturating_increment_u32(
                    s_snapshot.terminal_events);
            }
            try_persist();
            refresh_snapshot();
            update_maintenance_readiness(&observation);
            return result;
        }
    }

    if (s_restore_pending) {
        if (!observation.charger_present) {
            s_restore_pending = false;
            reset_detach = true;
            result = BATTERY_CHARGE_SUPERVISOR_RESULT_DETACHED |
                     BATTERY_CHARGE_SUPERVISOR_RESULT_TERMINAL;
        } else if (restore_evidence_ready(&observation)) {
            s_snapshot.restore_attempts = saturating_increment_u32(
                s_snapshot.restore_attempts);
            if (battery_charge_supervisor_restore_active(
                    &s_state, s_pending_persisted.configured_policy, profile,
                    &s_pending_persisted, &observation)) {
                s_snapshot.restore_successes = saturating_increment_u32(
                    s_snapshot.restore_successes);
                s_snapshot.restored_after_reset = true;
                result = BATTERY_CHARGE_SUPERVISOR_RESULT_RESTORED;
                battery_charge_supervisor_snapshot_t restored;
                battery_charge_supervisor_get_snapshot(&s_state, &restored);
                if (restored.phase ==
                    BATTERY_CHARGE_PHASE_COMPLETE_SOFTWARE) {
                    /* Re-emit the idempotent FULL edge after reset. This closes
                     * the narrow crash window where the durable stop record made
                     * it to flash but the learner's FULL anchor did not. */
                    result |=
                        BATTERY_CHARGE_SUPERVISOR_RESULT_SOFTWARE_STOPPED |
                        BATTERY_CHARGE_SUPERVISOR_RESULT_FULL_QUALIFIED;
                }
            } else {
                restore_rejected = true;
                battery_charge_supervisor_reject_restore(
                    &s_state, s_pending_persisted.configured_policy,
                    s_pending_persisted.charge_generation, profile,
                    &observation);
                s_snapshot.restore_failures = saturating_increment_u32(
                    s_snapshot.restore_failures);
                result = BATTERY_CHARGE_SUPERVISOR_RESULT_DEGRADED |
                         BATTERY_CHARGE_SUPERVISOR_RESULT_TERMINAL;
            }
            s_restore_pending = false;
        }
    } else {
        result |= battery_charge_supervisor_update(
            &s_state, profile, &observation);
    }

    refresh_snapshot();
    update_maintenance_readiness(&observation);
    uint8_t foreign_inhibits = (uint8_t)(
        observation.inhibit_owner_mask &
        (uint8_t)~CHARGER_INHIBIT_SUPERVISOR);
    bool maintenance_ready =
        result == BATTERY_CHARGE_SUPERVISOR_RESULT_NONE &&
        s_maintenance_phase == BATTERY_CHARGE_MAINTENANCE_IDLE &&
        observation.charger_present && observation.status_valid &&
        observation.charger_enable_valid && foreign_inhibits == 0u &&
        observation.chemistry == profile->chemistry &&
        observation.hardware_profile_id == profile->hardware_profile_id &&
        s_pending_persisted.configured_policy ==
            BATTERY_CHARGE_POLICY_ENFORCE_ANCHORED &&
        s_pending_persisted.configured_charge_factor_confident &&
        s_snapshot.maintenance_full_hold &&
        s_snapshot.maintenance_soc_ready &&
        s_snapshot.maintenance_voltage_ready &&
        supervisor_store_durable();
    if (maintenance_ready) {
        request_maintenance_inhibit();
        result |= BATTERY_CHARGE_SUPERVISOR_RESULT_MAINTENANCE_ARMED;
    }
    if (unqualified_bq_recovery_ready(result, profile, &observation)) {
        result |= BATTERY_CHARGE_SUPERVISOR_RESULT_BQ_RECOVERY_ARMED;
    }

    s_snapshot.update_count = saturating_increment_u32(
        s_snapshot.update_count);
    s_snapshot.last_model_result = result;
    if ((result & BATTERY_CHARGE_SUPERVISOR_RESULT_ATTACHED) != 0u) {
        s_snapshot.attach_events = saturating_increment_u32(
            s_snapshot.attach_events);
    }
    if ((result & BATTERY_CHARGE_SUPERVISOR_RESULT_ADMITTED) != 0u) {
        s_snapshot.admission_events = saturating_increment_u32(
            s_snapshot.admission_events);
    }
    if ((result & BATTERY_CHARGE_SUPERVISOR_RESULT_TERMINAL) != 0u) {
        s_snapshot.terminal_events = saturating_increment_u32(
            s_snapshot.terminal_events);
    }
    if ((result &
         BATTERY_CHARGE_SUPERVISOR_RESULT_MAINTENANCE_RESTARTED) != 0u) {
        s_snapshot.maintenance_rearm_events = saturating_increment_u32(
            s_snapshot.maintenance_rearm_events);
    }
    if ((result &
         BATTERY_CHARGE_SUPERVISOR_RESULT_BQ_RECOVERY_ARMED) != 0u) {
        s_snapshot.bq_recovery_events = saturating_increment_u32(
            s_snapshot.bq_recovery_events);
    }
    refresh_snapshot();
    if ((result & BATTERY_CHARGE_SUPERVISOR_RESULT_ATTACHED) != 0u &&
        s_snapshot.model.charge_generation != before.charge_generation) {
        s_snapshot.restored_after_reset = false;
    }
    if ((result & BATTERY_CHARGE_SUPERVISOR_RESULT_MINUTE) != 0u) {
        s_snapshot.minute_events = saturating_increment_u32(
            s_snapshot.minute_events);
        append_trace(&s_snapshot.model.latest_minute);
    }

    if (reset_detach) {
        bool latched_before =
            s_pending_persisted.supervisor_inhibit_latched;
        bool completed_latch =
            latched_before &&
            s_pending_persisted.last_terminal_valid &&
            s_pending_persisted.last_terminal_generation ==
                s_pending_persisted.charge_generation &&
            s_pending_persisted.last_terminal_reason ==
                s_pending_persisted.latched_stop_reason;
        if (completed_latch) {
            battery_charge_supervisor_persisted_t persisted =
                s_pending_persisted;
            clear_active_record(&persisted);
            persisted.supervisor_inhibit_latched = false;
            persisted.latched_stop_reason = BATTERY_CHARGE_TERMINAL_NONE;
            persisted.maintenance_rearm_pending = false;
            persisted.completion_rearm_used = false;
            request_persist(&persisted);
        } else {
            persist_reset_detach(&observation);
        }
        if (latched_before) {
            s_release_inhibit_pending = true;
        }
    } else if ((result &
                BATTERY_CHARGE_SUPERVISOR_RESULT_STOP_REQUESTED) != 0u) {
        persist_stop_request(&s_snapshot.model);
    } else if ((result & BATTERY_CHARGE_SUPERVISOR_RESULT_TERMINAL) != 0u) {
        if ((result & BATTERY_CHARGE_SUPERVISOR_RESULT_DETACHED) != 0u) {
            bool latched_before =
                s_pending_persisted.supervisor_inhibit_latched;
            bool completed_latch = latched_before &&
                s_pending_persisted.last_terminal_valid &&
                s_pending_persisted.last_terminal_generation ==
                    s_pending_persisted.charge_generation &&
                s_pending_persisted.last_terminal_reason ==
                    s_pending_persisted.latched_stop_reason;
            bool live_before =
                before.phase == BATTERY_CHARGE_PHASE_QUALIFYING ||
                before.phase == BATTERY_CHARGE_PHASE_CHARGING ||
                before.phase == BATTERY_CHARGE_PHASE_STOPPING;
            if (completed_latch) {
                battery_charge_supervisor_persisted_t persisted =
                    s_pending_persisted;
                clear_active_record(&persisted);
                persisted.supervisor_inhibit_latched = false;
                persisted.latched_stop_reason =
                    BATTERY_CHARGE_TERMINAL_NONE;
                persisted.maintenance_rearm_pending = false;
                persisted.completion_rearm_used = false;
                request_persist(&persisted);
            } else if (active_record_before ||
                       (live_before && before.admitted)) {
                battery_charge_supervisor_snapshot_t detached = before;
                bool net_input_valid = before.admitted &&
                    before.session_authoritative &&
                    observation.sample_valid &&
                    observation.current_valid &&
                    observation.continuity_valid &&
                    observation.authoritative &&
                    observation.gauge_session == before.gauge_session;
                if (net_input_valid) {
                    detached.net_input_nah = saturating_sub_i64(
                        observation.session_delta_nah,
                        before.start_session_delta_nah);
                }
                if (observation.sample_valid) {
                    detached.terminal_mv = observation.terminal_mv;
                }
                persist_terminal(
                    &detached, BATTERY_CHARGE_TERMINAL_DETACHED,
                    net_input_valid, observation.sample_valid,
                    !s_snapshot.restored_after_reset);
            }
            if (latched_before) {
                s_release_inhibit_pending = true;
            }
        } else {
            if (!(restore_rejected &&
                  s_pending_persisted.supervisor_inhibit_latched)) {
                persist_terminal(
                    &s_snapshot.model, s_snapshot.model.terminal_reason,
                    s_snapshot.model.admitted &&
                        s_snapshot.model.session_authoritative &&
                        observation.sample_valid &&
                        observation.current_valid &&
                        observation.continuity_valid &&
                        observation.authoritative &&
                        observation.gauge_session ==
                            s_snapshot.model.gauge_session,
                    observation.sample_valid,
                    !s_snapshot.restored_after_reset && !restore_rejected);
                if ((result &
                     BATTERY_CHARGE_SUPERVISOR_RESULT_BQ_RECOVERY_ARMED) !=
                    0u) {
                    request_maintenance_inhibit();
                }
            }
        }
    } else if ((result & BATTERY_CHARGE_SUPERVISOR_RESULT_ADMITTED) != 0u) {
        persist_admission(&s_snapshot.model);
    } else if ((result & BATTERY_CHARGE_SUPERVISOR_RESULT_ATTACHED) != 0u) {
        persist_generation(&s_snapshot.model);
    }
    if ((result & BATTERY_CHARGE_SUPERVISOR_RESULT_DETACHED) != 0u &&
        s_pending_persisted.completion_rearm_used) {
        battery_charge_supervisor_persisted_t persisted =
            s_pending_persisted;
        persisted.completion_rearm_used = false;
        request_persist(&persisted);
    }
    try_persist();
    if (s_release_inhibit_pending) {
        service_control_preflight();
    }
    refresh_snapshot();
    update_maintenance_readiness(&observation);
    s_snapshot.last_model_result = result;
    return result;
}

battery_charge_supervisor_config_status_t
battery_charge_supervisor_service_configure(
    battery_charge_supervisor_policy_t policy,
    uint16_t charge_factor_permille,
    bool factor_confident) {
    if (policy > BATTERY_CHARGE_POLICY_ENFORCE_BOOTSTRAP ||
        charge_factor_permille < BATTERY_CHARGE_FACTOR_MIN_PERMILLE ||
        charge_factor_permille > BATTERY_CHARGE_FACTOR_MAX_PERMILLE) {
        return BATTERY_CHARGE_CONFIG_INVALID_ARGUMENT;
    }
    if (!s_snapshot.initialized) {
        battery_charge_supervisor_service_init();
    }
    if (!store_service_ready() || s_snapshot.storage_unavailable) {
        return BATTERY_CHARGE_CONFIG_STORE_NOT_READY;
    }

    try_persist();
    board_diag_snapshot_t board;
    board_diag_get_snapshot(&board);
    if (s_snapshot.persistence_pending || s_restore_pending ||
        s_release_inhibit_pending || s_incompatible_latch ||
        s_maintenance_phase != BATTERY_CHARGE_MAINTENANCE_IDLE ||
        s_pending_persisted.maintenance_rearm_pending ||
        s_pending_persisted.active_session_valid ||
        s_pending_persisted.supervisor_inhibit_latched ||
        board.charger_connected ||
        (board.charger_inhibit_owner_mask &
         CHARGER_INHIBIT_SUPERVISOR) != 0u ||
        s_state.physically_attached) {
        return BATTERY_CHARGE_CONFIG_SESSION_ACTIVE;
    }

    const battery_charge_supervisor_profile_t *profile =
        battery_charge_supervisor_revb2_profile();
    battery_charge_supervisor_persisted_t candidate = s_pending_persisted;
    candidate.hardware_profile_id = profile->hardware_profile_id;
    candidate.chemistry = profile->chemistry;
    candidate.configured_policy = policy;
    candidate.configured_charge_factor_permille = charge_factor_permille;
    candidate.configured_charge_factor_confident = factor_confident;
    if (!battery_charge_supervisor_persisted_valid(&candidate)) {
        return BATTERY_CHARGE_CONFIG_INVALID_ARGUMENT;
    }
    if (store_battery_charge_supervisor_set(&candidate) != STORE_STATUS_OK) {
        return BATTERY_CHARGE_CONFIG_STORE_ERROR;
    }

    s_pending_persisted = candidate;
    s_snapshot.persistence_pending = false;
    s_snapshot.persistence_requests = saturating_increment_u32(
        s_snapshot.persistence_requests);
    battery_charge_supervisor_init(&s_state, policy);
    battery_charge_supervisor_seed_generation(
        &s_state, candidate.charge_generation);
    refresh_snapshot();
    return BATTERY_CHARGE_CONFIG_OK;
}

void battery_charge_supervisor_service_note_legacy(bool active,
                                                   bool completed) {
    if (!s_snapshot.initialized || s_snapshot.update_count == 0u ||
        s_snapshot.restore_pending ||
        s_snapshot.model.policy != BATTERY_CHARGE_POLICY_OBSERVE) {
        return;
    }
    bool model_active =
        s_snapshot.model.phase == BATTERY_CHARGE_PHASE_CHARGING;
    bool model_completed =
        (s_snapshot.last_model_result &
         BATTERY_CHARGE_SUPERVISOR_RESULT_BQ_COMPLETE) != 0u;
    s_snapshot.shadow_last_active_match = model_active == active;
    s_snapshot.shadow_last_completion_match = model_completed == completed;
    s_snapshot.shadow_compares = saturating_increment_u32(
        s_snapshot.shadow_compares);
    if (s_snapshot.shadow_last_active_match &&
        s_snapshot.shadow_last_completion_match) {
        s_snapshot.shadow_matches = saturating_increment_u32(
            s_snapshot.shadow_matches);
    } else {
        s_snapshot.shadow_mismatches = saturating_increment_u32(
            s_snapshot.shadow_mismatches);
    }
}

uint16_t
battery_charge_supervisor_service_effective_charge_factor_permille(void) {
    if (!s_snapshot.initialized) {
        battery_charge_supervisor_service_init();
    }
    uint16_t factor =
        s_pending_persisted.configured_charge_factor_permille;
    if (factor >= BATTERY_CHARGE_FACTOR_MIN_PERMILLE &&
        factor <= BATTERY_CHARGE_FACTOR_MAX_PERMILLE) {
        return factor;
    }
    return battery_charge_supervisor_revb2_profile()
        ->charge_factor_prior_permille;
}

void battery_charge_supervisor_service_get_snapshot(
    battery_charge_supervisor_service_snapshot_t *out) {
    if (out != NULL) {
        *out = s_snapshot;
    }
}

bool battery_charge_supervisor_service_get_trace(
    uint8_t index, battery_charge_supervisor_minute_t *out) {
    if (out == NULL || index >= s_snapshot.trace_count) {
        return false;
    }
    uint8_t oldest = s_snapshot.trace_count <
            BATTERY_CHARGE_SUPERVISOR_TRACE_CAP
        ? 0u : s_snapshot.trace_next;
    uint8_t slot = (uint8_t)(
        (oldest + index) % BATTERY_CHARGE_SUPERVISOR_TRACE_CAP);
    *out = s_trace[slot];
    return true;
}

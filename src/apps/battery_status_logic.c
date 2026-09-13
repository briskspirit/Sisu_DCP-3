#include "apps/battery_status_logic.h"

#include <stddef.h>

void battery_raw_collapse_evidence_init(
    battery_raw_collapse_evidence_t *evidence) {
    if (evidence != NULL) {
        *evidence = (battery_raw_collapse_evidence_t){0};
    }
}

bool battery_raw_collapse_evidence_update(
    battery_raw_collapse_evidence_t *evidence,
    bool sample_valid,
    uint32_t sample_sequence,
    uint16_t sample_mv,
    uint16_t threshold_mv) {
    if (evidence == NULL) {
        return false;
    }
    /* A call transition changes the meaning of a low sample. Never combine a
     * sample qualified against the idle floor with one qualified against the
     * lower in-call floor (or vice versa). */
    if (evidence->last_threshold_mv != threshold_mv) {
        evidence->last_threshold_mv = threshold_mv;
        evidence->consecutive_low = 0u;
        evidence->sequence_seen = false;
    }
    if (!sample_valid) {
        evidence->consecutive_low = 0u;
        evidence->sequence_seen = false;
        return false;
    }
    if (evidence->sequence_seen &&
        evidence->last_sequence == sample_sequence) {
        return evidence->consecutive_low >=
               BATTERY_RAW_COLLAPSE_CONFIRM_SAMPLES;
    }

    evidence->sequence_seen = true;
    evidence->last_sequence = sample_sequence;
    if (sample_mv < threshold_mv) {
        if (evidence->consecutive_low < UINT8_MAX) {
            evidence->consecutive_low++;
        }
    } else {
        evidence->consecutive_low = 0u;
    }
    return evidence->consecutive_low >=
           BATTERY_RAW_COLLAPSE_CONFIRM_SAMPLES;
}

uint16_t battery_empty_shutdown_threshold_mv(bool call_active) {
    return call_active ? BATTERY_RAW_COLLAPSE_MV
                       : BATTERY_OPERATIONAL_EMPTY_MV;
}

void battery_endpoint_state_init(battery_endpoint_state_t *state) {
    if (state != NULL) {
        *state = (battery_endpoint_state_t){0};
    }
}

battery_endpoint_decision_t battery_endpoint_step(
    battery_endpoint_state_t *state,
    const battery_endpoint_evidence_t *evidence) {
    battery_endpoint_decision_t decision = {0};
    if (evidence == NULL) {
        return decision;
    }

    if (evidence->charge_recovery_active || evidence->powered_off) {
        battery_endpoint_state_init(state);
        return decision;
    }

    if (evidence->shutdown_armed) {
        decision.kind = state != NULL &&
                        state->armed_kind != BATTERY_ENDPOINT_NONE
            ? state->armed_kind
            : BATTERY_ENDPOINT_EMERGENCY_SHUTDOWN;
        decision.shutdown = true;
        return decision;
    }

    bool trusted_charge_left = evidence->anchored_capacity_has_charge &&
        !evidence->capacity_prediction_exhausted;
    bool live_load_sag = trusted_charge_left &&
        ((evidence->call_active && evidence->gauge_empty) ||
         evidence->modem_supply_collapse);
    if (state != NULL) {
        if (live_load_sag) {
            state->load_sag_latched = true;
        } else if (evidence->battery_sample_valid &&
                   !evidence->gauge_empty &&
                   !evidence->modem_supply_collapse) {
            state->load_sag_latched = false;
        }
    }
    bool held_load_sag = trusted_charge_left && evidence->gauge_empty &&
        (evidence->call_active ||
         (state != NULL && state->load_sag_latched));
    bool load_sag = live_load_sag || held_load_sag;

    decision.capacity_voltage_disagreement =
        evidence->capacity_prediction_exhausted &&
        evidence->battery_sample_valid && !evidence->gauge_low &&
        !evidence->gauge_empty && !evidence->operating_floor_confirmed &&
        !evidence->modem_supply_collapse;

    bool gauge_endpoint = evidence->battery_sample_valid &&
        evidence->gauge_empty && !held_load_sag;
    /* Coulomb zero is not an endpoint by itself: a healthy pack may simply
     * have more capacity than the current estimate. Once the independently
     * debounced voltage classifier has also committed LOW, however, the two
     * domains corroborate an idle endpoint early enough to journal it before
     * the loaded rail reaches the hard floor. Keep an active call out of this
     * path; its terminal sag must either recover after the call or reach an
     * already-existing physical EMPTY/emergency criterion. */
    bool corroborated_capacity_endpoint =
        evidence->capacity_prediction_exhausted &&
        evidence->battery_sample_valid && evidence->gauge_low &&
        !evidence->gauge_empty && !evidence->call_active;
    bool floor_endpoint = evidence->operating_floor_confirmed &&
        (!evidence->call_active || !trusted_charge_left);
    bool collapse_endpoint = evidence->modem_supply_collapse &&
        !trusted_charge_left;
    if (gauge_endpoint || corroborated_capacity_endpoint ||
        floor_endpoint || collapse_endpoint) {
        decision.kind = BATTERY_ENDPOINT_NATURAL_EMPTY;
        decision.shutdown = true;
        decision.learn_empty = true;
        if (state != NULL) {
            state->armed_kind = decision.kind;
        }
        return decision;
    }

    if (evidence->operating_floor_confirmed) {
        decision.kind = BATTERY_ENDPOINT_EMERGENCY_SHUTDOWN;
        decision.shutdown = true;
        if (state != NULL) {
            state->armed_kind = decision.kind;
        }
        return decision;
    }

    if (load_sag) {
        decision.kind = BATTERY_ENDPOINT_LOAD_SAG;
        decision.force_low = true;
        return decision;
    }

    if (evidence->capacity_prediction_exhausted) {
        decision.kind = BATTERY_ENDPOINT_CAPACITY_PENDING;
        decision.force_low = true;
        return decision;
    }

    decision.force_low = evidence->gauge_low;
    return decision;
}

battery_warning_action_t battery_warning_action(
    bool battery_sample_valid,
    bool charge_recovery_active,
    bool powered_off,
    bool empty_evidence,
    bool low_evidence) {
    if (charge_recovery_active || powered_off) {
        return BATTERY_WARNING_ACTION_SUPPRESS;
    }
    if (empty_evidence) {
        return BATTERY_WARNING_ACTION_EMPTY;
    }
    if (!battery_sample_valid) {
        return BATTERY_WARNING_ACTION_HOLD;
    }
    return low_evidence ? BATTERY_WARNING_ACTION_LOW
                        : BATTERY_WARNING_ACTION_HEALTHY;
}

bool battery_charge_recovery_active(
    bool charger_active,
    bool charger_enable_valid,
    bool charger_enabled,
    bool current_valid,
    int32_t current_ua) {
    return charger_active && charger_enable_valid && charger_enabled &&
        current_valid && current_ua > 0;
}

bool battery_anchored_remaining_has_charge(
    bool remaining_valid,
    bool soc_anchored,
    uint64_t remaining_nah) {
    return remaining_valid && soc_anchored && remaining_nah != 0u;
}

bool battery_charge_floor_release_needed(
    bool charger_present,
    bool charge_recovery_active,
    bool low_zone,
    bool empty_zone) {
    return charger_present && !charge_recovery_active &&
        (low_zone || empty_zone);
}

uint8_t battery_display_bars(
    bool soc_bars_valid,
    uint8_t soc_bars,
    bool battery_sample_valid,
    uint8_t measured_bars,
    uint8_t cached_bars) {
    if (soc_bars_valid) {
        return soc_bars > 4u ? 4u : soc_bars;
    }
    return battery_sample_valid ? measured_bars : cached_bars;
}

battery_charge_completion_decision_t battery_charge_completion_decision(
    bool legacy_completed,
    bool supervisor_completed,
    bool legacy_completion_allowed) {
    return (battery_charge_completion_decision_t){
        .ui_completed = supervisor_completed ||
            (legacy_completed && legacy_completion_allowed),
        .learn_full = supervisor_completed,
    };
}

battery_full_notice_decision_t battery_full_notice_step(
    bool already_notified,
    bool charge_completed,
    bool maintenance_restarted) {
    if (maintenance_restarted) {
        return (battery_full_notice_decision_t){0};
    }
    bool show = charge_completed && !already_notified;
    return (battery_full_notice_decision_t){
        .notified = already_notified || show,
        .show = show,
    };
}

uint8_t battery_charger_insert_tone_index(bool call_active) {
    return call_active ? 11u : 10u;
}

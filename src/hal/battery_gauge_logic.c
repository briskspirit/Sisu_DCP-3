#include "hal/battery_gauge_logic.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>

#define BATTERY_GAUGE_CURRENT_WINDOW_TARGET_MS 5000u
#define BATTERY_GAUGE_CURRENT_WINDOW_MIN_MS 4000u
#define BATTERY_GAUGE_CURRENT_WINDOW_MAX_MS 7000u
#define BATTERY_GAUGE_MAX_CATCHUP_STEPS 60u

static const battery_gauge_profile_t NIMH_PROFILE = {
    .reference_current_ma = BATTERY_GAUGE_REFERENCE_CURRENT_MA,
    .effective_resistance_mohm = BATTERY_GAUGE_EFFECTIVE_RESISTANCE_MOHM,
    .max_compensation_mv = BATTERY_GAUGE_MAX_COMPENSATION_MV,
    .charge_compensation_percent =
        BATTERY_GAUGE_CHARGE_COMPENSATION_PERCENT,
    .rise_rearm_nah = BATTERY_GAUGE_RISE_REARM_NAH,
};

static const uint16_t BAR_FLOOR_MV[5] = {
    0u, VBAT_BAR1_MV, VBAT_BAR2_MV, VBAT_BAR3_MV, VBAT_BAR4_MV,
};

static int32_t clamp_i32(int32_t value, int32_t low, int32_t high) {
    if (value < low) {
        return low;
    }
    return value > high ? high : value;
}

static uint16_t clamp_mv_i32(int32_t value) {
    return (uint16_t)clamp_i32(value, 0, UINT16_MAX);
}

static int32_t iir_one_tenth_step(int32_t current, int32_t target) {
    int32_t error = target - current;
    if (error == 0) {
        return current;
    }
    int32_t step = error / 10;
    if (step == 0) {
        step = error > 0 ? 1 : -1;
    }
    return current + step;
}

static int32_t iir_one_tenth_steps(int32_t current, int32_t target,
                                   uint32_t steps) {
    for (uint32_t i = 0u; i < steps && current != target; i++) {
        current = iir_one_tenth_step(current, target);
    }
    return current;
}

static uint16_t x10_to_mv(int32_t value_x10) {
    if (value_x10 <= 0) {
        return 0u;
    }
    return clamp_mv_i32((value_x10 + 5) / 10);
}

static uint32_t model_catchup_steps(uint32_t elapsed_ms) {
    uint32_t steps = elapsed_ms / BATTERY_GAUGE_MODEL_PERIOD_MS;
    if (steps == 0u) {
        steps = 1u;
    }
    return steps > BATTERY_GAUGE_MAX_CATCHUP_STEPS
        ? BATTERY_GAUGE_MAX_CATCHUP_STEPS
        : steps;
}

static int32_t signed_round_divide(int64_t numerator, int32_t denominator) {
    if (numerator >= 0) {
        numerator += denominator / 2;
    } else {
        numerator -= denominator / 2;
    }
    int64_t result = numerator / denominator;
    if (result > INT32_MAX) {
        return INT32_MAX;
    }
    if (result < INT32_MIN) {
        return INT32_MIN;
    }
    return (int32_t)result;
}

static int32_t current_magnitude_ua(int32_t current_ua, bool charging) {
    int64_t magnitude = charging ? (int64_t)current_ua
                                 : -(int64_t)current_ua;
    if (magnitude <= 0) {
        return 0;
    }
    return magnitude > INT32_MAX ? INT32_MAX : (int32_t)magnitude;
}

static void median_seed(battery_gauge_state_t *state, uint16_t value_mv,
                        uint32_t now_ms) {
    state->median_count = BATTERY_GAUGE_MEDIAN_CAPACITY;
    for (uint8_t i = 0u; i < BATTERY_GAUGE_MEDIAN_CAPACITY; i++) {
        state->median[i].value_mv = value_mv;
        state->median[i].observed_ms = now_ms;
    }
}

static void median_prune(battery_gauge_state_t *state, uint32_t now_ms) {
    uint8_t kept = 0u;
    for (uint8_t i = 0u; i < state->median_count; i++) {
        if ((uint32_t)(now_ms - state->median[i].observed_ms) <=
            BATTERY_GAUGE_MEDIAN_WINDOW_MS) {
            state->median[kept++] = state->median[i];
        }
    }
    state->median_count = kept;
}

static void median_append(battery_gauge_state_t *state, uint16_t value_mv,
                          uint32_t now_ms) {
    median_prune(state, now_ms);
    if (state->median_count == BATTERY_GAUGE_MEDIAN_CAPACITY) {
        memmove(&state->median[0], &state->median[1],
                sizeof(state->median[0]) *
                    (BATTERY_GAUGE_MEDIAN_CAPACITY - 1u));
        state->median_count--;
    }
    state->median[state->median_count].value_mv = value_mv;
    state->median[state->median_count].observed_ms = now_ms;
    state->median_count++;
}

static uint16_t median_value(const battery_gauge_state_t *state) {
    uint16_t sorted[BATTERY_GAUGE_MEDIAN_CAPACITY];
    uint8_t count = state->median_count;
    if (count == 0u) {
        return 0u;
    }
    for (uint8_t i = 0u; i < count; i++) {
        sorted[i] = state->median[i].value_mv;
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

static void current_history_clear(battery_gauge_state_t *state) {
    state->current_history_count = 0u;
    state->snapshot.window_current_valid = false;
    state->snapshot.window_current_ua = 0;
}

static void current_history_prune(battery_gauge_state_t *state,
                                  uint32_t now_ms) {
    uint8_t kept = 0u;
    for (uint8_t i = 0u; i < state->current_history_count; i++) {
        if ((uint32_t)(now_ms - state->current_history[i].observed_ms) <=
            BATTERY_GAUGE_CURRENT_WINDOW_MAX_MS) {
            state->current_history[kept++] = state->current_history[i];
        }
    }
    state->current_history_count = kept;
}

static void update_window_current(battery_gauge_state_t *state,
                                  const battery_gauge_observation_t *obs) {
    if (!obs->continuity_valid || !obs->current_valid) {
        current_history_clear(state);
        return;
    }

    current_history_prune(state, obs->now_ms);
    int best = -1;
    uint32_t best_error = UINT32_MAX;
    uint32_t best_elapsed = 0u;
    for (uint8_t i = 0u; i < state->current_history_count; i++) {
        uint32_t elapsed =
            (uint32_t)(obs->now_ms - state->current_history[i].observed_ms);
        if (elapsed < BATTERY_GAUGE_CURRENT_WINDOW_MIN_MS ||
            elapsed > BATTERY_GAUGE_CURRENT_WINDOW_MAX_MS) {
            continue;
        }
        uint32_t error = elapsed > BATTERY_GAUGE_CURRENT_WINDOW_TARGET_MS
            ? elapsed - BATTERY_GAUGE_CURRENT_WINDOW_TARGET_MS
            : BATTERY_GAUGE_CURRENT_WINDOW_TARGET_MS - elapsed;
        if (error < best_error) {
            best = (int)i;
            best_error = error;
            best_elapsed = elapsed;
        }
    }

    state->snapshot.window_current_valid = false;
    if (best >= 0 && best_elapsed != 0u) {
        int64_t delta_nah =
            obs->session_delta_nah - state->current_history[best].charge_nah;
        int64_t positive_limit =
            (int64_t)INT32_MAX * best_elapsed / 3600;
        int64_t negative_limit =
            (int64_t)INT32_MIN * best_elapsed / 3600;
        int64_t average_ua;
        if (delta_nah > positive_limit) {
            average_ua = INT32_MAX;
        } else if (delta_nah < negative_limit) {
            average_ua = INT32_MIN;
        } else {
            average_ua = delta_nah * 3600 / (int64_t)best_elapsed;
        }
        state->snapshot.window_current_ua = (int32_t)average_ua;
        state->snapshot.window_current_valid = true;
    }

    if (state->current_history_count ==
        BATTERY_GAUGE_CURRENT_HISTORY_CAPACITY) {
        memmove(&state->current_history[0], &state->current_history[1],
                sizeof(state->current_history[0]) *
                    (BATTERY_GAUGE_CURRENT_HISTORY_CAPACITY - 1u));
        state->current_history_count--;
    }
    state->current_history[state->current_history_count].charge_nah =
        obs->session_delta_nah;
    state->current_history[state->current_history_count].observed_ms =
        obs->now_ms;
    state->current_history_count++;
}

static uint16_t bar_rise_threshold(uint8_t target_bar) {
    if (target_bar <= 1u) {
        return VBAT_BAR1_MV;
    }
    if (target_bar >= 4u) {
        /* ROM 0x27db5c has no threshold above the 100-percent slot, so it
         * extrapolates the top boundary by half the 100-to-75-percent spacing. */
        return (uint16_t)(VBAT_BAR4_MV +
            (VBAT_BAR4_MV - VBAT_BAR3_MV) / 2u);
    }
    uint16_t lower = battery_gauge_bar_floor_mv(target_bar);
    uint16_t upper = battery_gauge_bar_floor_mv((uint8_t)(target_bar + 1u));
    return (uint16_t)(lower + (upper - lower) / 2u);
}

static void bar_note_commit(battery_bar_state_t *state, uint8_t bars,
                            bool continuity_valid,
                            int64_t session_delta_nah) {
    state->committed = bars;
    state->seeded = true;
    state->rearm_anchor_valid = continuity_valid;
    state->rearm_anchor_nah = session_delta_nah;
}

static void bar_force_warning(battery_bar_state_t *state, uint8_t bars,
                              bool continuity_valid,
                              int64_t session_delta_nah) {
    if (!state->seeded || state->committed != bars) {
        bar_note_commit(state, bars, continuity_valid, session_delta_nah);
    }
}

const battery_gauge_profile_t *battery_gauge_nimh_profile(void) {
    return &NIMH_PROFILE;
}

uint16_t battery_gauge_bar_floor_mv(uint8_t bar) {
    return bar <= 4u ? BAR_FLOOR_MV[bar] : BAR_FLOOR_MV[4];
}

uint8_t battery_gauge_bars_from_mv(uint16_t mv) {
    for (uint8_t bar = 4u; bar > 0u; bar--) {
        if (mv >= BAR_FLOOR_MV[bar]) {
            return bar;
        }
    }
    return 0u;
}

int16_t battery_gauge_load_correction_mv(
    const battery_gauge_profile_t *profile,
    int32_t current_ua,
    int32_t window_current_ua,
    bool window_current_valid,
    bool charge_active) {
    if (profile == NULL) {
        profile = &NIMH_PROFILE;
    }

    int32_t discharge_ua = current_magnitude_ua(current_ua, false);
    int32_t charge_ua = current_magnitude_ua(current_ua, true);
    if (window_current_valid) {
        int32_t window_discharge_ua =
            current_magnitude_ua(window_current_ua, false);
        int32_t window_charge_ua =
            current_magnitude_ua(window_current_ua, true);
        if (window_discharge_ua < discharge_ua) {
            discharge_ua = window_discharge_ua;
        }
        if (window_charge_ua < charge_ua) {
            charge_ua = window_charge_ua;
        }
    }

    int32_t discharge_ma =
        (int32_t)(((int64_t)discharge_ua + 500) / 1000);
    int32_t charge_ma =
        (int32_t)(((int64_t)charge_ua + 500) / 1000);
    int32_t effective_ma = discharge_ma;
    if (charge_active) {
        effective_ma -=
            (charge_ma * profile->charge_compensation_percent + 50) / 100;
    }
    int32_t delta_ma = effective_ma - profile->reference_current_ma;
    int32_t correction = signed_round_divide(
        (int64_t)delta_ma * profile->effective_resistance_mohm, 1000);
    int32_t bound = profile->max_compensation_mv;
    if (bound > INT16_MAX) {
        bound = INT16_MAX;
    }
    correction = clamp_i32(correction, -bound, bound);
    return (int16_t)correction;
}

void battery_gauge_bar_state_init(battery_bar_state_t *state) {
    if (state != NULL) {
        *state = (battery_bar_state_t){0};
    }
}

uint8_t battery_gauge_bar_step(
    battery_bar_state_t *state,
    const battery_gauge_profile_t *profile,
    uint16_t reference_mv,
    bool charge_active,
    bool continuity_valid,
    int64_t session_delta_nah) {
    if (state == NULL) {
        return battery_gauge_bars_from_mv(reference_mv);
    }
    if (profile == NULL) {
        profile = &NIMH_PROFILE;
    }

    uint8_t target = battery_gauge_bars_from_mv(reference_mv);
    if (!state->seeded) {
        bar_note_commit(state, target, continuity_valid, session_delta_nah);
        return target;
    }
    if (!state->rearm_anchor_valid && continuity_valid) {
        state->rearm_anchor_valid = true;
        state->rearm_anchor_nah = session_delta_nah;
    }
    if (target <= state->committed) {
        if (target != state->committed) {
            bar_note_commit(state, target, continuity_valid, session_delta_nah);
        }
        return state->committed;
    }

    /* ROM 0x27db56..0x27db88 applies the midpoint only when the threshold scan
     * is attempting an adjacent upward index. It never promotes before the raw
     * scan reaches that level. In particular, the top step is 2597 mV, not the
     * 2552 mV midpoint used by 2->3. Keeping the rise check here also makes a
     * committed level stable inside its hysteresis band instead of alternating
     * up on the midpoint pass and down on the next direct scan. */
    if (target == (uint8_t)(state->committed + 1u) &&
        reference_mv < bar_rise_threshold(target)) {
        return state->committed;
    }

    bool rise_allowed = charge_active;
    if (!rise_allowed && continuity_valid && state->rearm_anchor_valid) {
        int64_t discharged_nah =
            state->rearm_anchor_nah - session_delta_nah;
        rise_allowed = discharged_nah >= profile->rise_rearm_nah;
    }
    if (rise_allowed) {
        bar_note_commit(state, target, continuity_valid, session_delta_nah);
    }
    return state->committed;
}

void battery_gauge_warning_state_init(battery_warning_state_t *state) {
    if (state != NULL) {
        *state = (battery_warning_state_t){0};
    }
}

battery_gauge_warning_t battery_gauge_warning_step(
    battery_warning_state_t *state,
    uint32_t now_ms,
    uint16_t terminal_mv,
    uint16_t fast_terminal_mv,
    uint16_t symmetric_terminal_mv,
    uint16_t reference_mv) {
    bool terminal_qualified =
        terminal_mv >= VBAT_LOW_TERMINAL_QUALIFY_MV ||
        symmetric_terminal_mv >= VBAT_LOW_TERMINAL_QUALIFY_MV;
    bool low_qualified =
        reference_mv >= VBAT_LOW_MV || fast_terminal_mv >= VBAT_LOW_MV;

    battery_gauge_warning_t instant;
    if (terminal_qualified && low_qualified) {
        instant = BATTERY_GAUGE_WARNING_HEALTHY;
    } else if (fast_terminal_mv >= VBAT_EMPTY_MV) {
        instant = BATTERY_GAUGE_WARNING_LOW;
    } else {
        instant = BATTERY_GAUGE_WARNING_EMPTY;
    }
    if (state == NULL) {
        return instant;
    }
    if (!state->seeded) {
        state->committed = instant;
        state->seeded = true;
        return instant;
    }
    if (instant == BATTERY_GAUGE_WARNING_HEALTHY) {
        state->committed = instant;
        state->pending = false;
        return instant;
    }
    if (state->committed != BATTERY_GAUGE_WARNING_HEALTHY) {
        state->committed = instant;
        state->pending = false;
        return instant;
    }
    if (!state->pending) {
        state->pending = true;
        state->pending_since_ms = now_ms;
        return state->committed;
    }
    if ((uint32_t)(now_ms - state->pending_since_ms) >=
        BATTERY_GAUGE_WARNING_ENTRY_MS) {
        state->committed = instant;
        state->pending = false;
    }
    return state->committed;
}

void battery_gauge_init(battery_gauge_state_t *state) {
    if (state == NULL) {
        return;
    }
    memset(state, 0, sizeof(*state));
    battery_gauge_bar_state_init(&state->bar);
    battery_gauge_warning_state_init(&state->warning);
}

bool battery_gauge_update(
    battery_gauge_state_t *state,
    const battery_gauge_profile_t *profile,
    const battery_gauge_observation_t *obs) {
    if (state == NULL || obs == NULL) {
        return false;
    }
    if (profile == NULL) {
        profile = &NIMH_PROFILE;
    }
    if (!state->session_seen || state->gauge_session != obs->gauge_session) {
        battery_gauge_init(state);
        state->session_seen = true;
        state->gauge_session = obs->gauge_session;
    }
    if (state->sample_seen &&
        state->last_sample_sequence == obs->sample_sequence) {
        return false;
    }
    state->sample_seen = true;
    state->last_sample_sequence = obs->sample_sequence;
    state->snapshot.terminal_mv = obs->terminal_mv;
    if (!obs->sample_valid) {
        state->snapshot.valid = false;
        return false;
    }
    state->snapshot.valid = state->model_seeded;
    if (state->model_seeded &&
        (uint32_t)(obs->now_ms - state->last_model_ms) <
            BATTERY_GAUGE_MODEL_PERIOD_MS) {
        state->snapshot.valid = true;
        return false;
    }

    update_window_current(state, obs);
    uint32_t steps = state->model_seeded
        ? model_catchup_steps((uint32_t)(obs->now_ms - state->last_model_ms))
        : 1u;
    int32_t target_x10 = (int32_t)obs->terminal_mv * 10;
    if (!state->model_seeded) {
        state->fast_peak_x10 = target_x10;
        state->fast_symmetric_x10 = target_x10;
    } else {
        if (target_x10 >= state->fast_peak_x10) {
            state->fast_peak_x10 = target_x10;
        } else {
            state->fast_peak_x10 = iir_one_tenth_steps(
                state->fast_peak_x10, target_x10, steps);
        }
        state->fast_symmetric_x10 = iir_one_tenth_steps(
            state->fast_symmetric_x10, target_x10, steps);
    }

    uint16_t fast_mv = x10_to_mv(state->fast_peak_x10);
    uint16_t symmetric_mv = x10_to_mv(state->fast_symmetric_x10);
    int32_t current_ua = obs->current_valid
        ? obs->current_ua
        : -(int32_t)profile->reference_current_ma * 1000;
    int16_t correction_mv = battery_gauge_load_correction_mv(
        profile, current_ua,
        state->snapshot.window_current_ua,
        state->snapshot.window_current_valid,
        obs->charge_active);
    uint16_t corrected_mv =
        clamp_mv_i32((int32_t)fast_mv + correction_mv);

    if (!state->model_seeded) {
        uint16_t seed_mv = clamp_mv_i32(
            (int32_t)corrected_mv +
            (int32_t)BATTERY_GAUGE_STARTUP_BIAS_MV);
        median_seed(state, seed_mv, obs->now_ms);
        state->slow_reference_x10 = (int32_t)seed_mv * 10;
    } else {
        median_append(state, corrected_mv, obs->now_ms);
        uint16_t robust_mv = median_value(state);
        state->slow_reference_x10 = iir_one_tenth_steps(
            state->slow_reference_x10, (int32_t)robust_mv * 10, steps);
    }

    uint16_t reference_mv = x10_to_mv(state->slow_reference_x10);
    uint8_t bars = battery_gauge_bar_step(
        &state->bar, profile, reference_mv, obs->charge_active,
        obs->continuity_valid, obs->session_delta_nah);
    battery_gauge_warning_t warning = battery_gauge_warning_step(
        &state->warning, obs->now_ms, obs->terminal_mv, fast_mv,
        symmetric_mv, reference_mv);
    if (warning == BATTERY_GAUGE_WARNING_LOW) {
        bars = 1u;
        bar_force_warning(&state->bar, bars, obs->continuity_valid,
                          obs->session_delta_nah);
    } else if (warning == BATTERY_GAUGE_WARNING_EMPTY) {
        bars = 0u;
        bar_force_warning(&state->bar, bars, obs->continuity_valid,
                          obs->session_delta_nah);
    }

    state->last_model_ms = obs->now_ms;
    state->model_seeded = true;
    state->snapshot.valid = true;
    state->snapshot.fast_terminal_mv = fast_mv;
    state->snapshot.symmetric_terminal_mv = symmetric_mv;
    state->snapshot.reference_mv = reference_mv;
    state->snapshot.correction_mv = correction_mv;
    state->snapshot.bars = bars;
    state->snapshot.warning = warning;
    state->snapshot.model_sequence++;
    return true;
}

void battery_gauge_get_snapshot(const battery_gauge_state_t *state,
                                battery_gauge_snapshot_t *out) {
    if (out == NULL) {
        return;
    }
    *out = state != NULL ? state->snapshot
                         : (battery_gauge_snapshot_t){0};
}

void battery_power_on_qualifier_init(battery_power_on_qualifier_t *state) {
    if (state != NULL) {
        memset(state, 0, sizeof(*state));
    }
}

void battery_power_on_qualifier_observe(
    battery_power_on_qualifier_t *state,
    uint32_t gauge_session,
    uint32_t attempt_sequence,
    bool sample_valid,
    uint16_t terminal_mv) {
    if (state == NULL) {
        return;
    }
    if (!state->session_seen || state->gauge_session != gauge_session) {
        battery_power_on_qualifier_init(state);
        state->session_seen = true;
        state->gauge_session = gauge_session;
    }
    if (state->sample_seen &&
        state->last_attempt_sequence == attempt_sequence) {
        return;
    }
    state->sample_seen = true;
    state->last_attempt_sequence = attempt_sequence;
    if (state->attempts < BATTERY_POWER_ON_MAX_ATTEMPTS) {
        state->attempts++;
    }
    if (!sample_valid) {
        return;
    }

    if (state->sample_count < BATTERY_POWER_ON_REQUIRED_VALID) {
        state->samples_mv[state->sample_count++] = terminal_mv;
        state->sum_mv += terminal_mv;
        if (state->sample_count == BATTERY_POWER_ON_REQUIRED_VALID) {
            state->next_sample = 0u;
        }
        return;
    }

    state->sum_mv -= state->samples_mv[state->next_sample];
    state->samples_mv[state->next_sample] = terminal_mv;
    state->sum_mv += terminal_mv;
    state->next_sample = (uint8_t)(
        (state->next_sample + 1u) % BATTERY_POWER_ON_REQUIRED_VALID);
}

bool battery_power_on_qualifier_ready(
    const battery_power_on_qualifier_t *state) {
    if (state == NULL || state->sample_count == 0u) {
        return false;
    }
    return state->sample_count >= BATTERY_POWER_ON_REQUIRED_VALID ||
           state->attempts >= BATTERY_POWER_ON_MAX_ATTEMPTS;
}

uint8_t battery_power_on_qualifier_sample_count(
    const battery_power_on_qualifier_t *state) {
    return state != NULL ? state->sample_count : 0u;
}

uint8_t battery_power_on_qualifier_attempt_count(
    const battery_power_on_qualifier_t *state) {
    return state != NULL ? state->attempts : 0u;
}

uint16_t battery_power_on_qualifier_average_mv(
    const battery_power_on_qualifier_t *state) {
    if (state == NULL || state->sample_count == 0u) {
        return 0u;
    }
    return (uint16_t)(state->sum_mv / state->sample_count);
}

#include "services/battery_learning_logic.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>

#define NIMH_NOMINAL_CAPACITY_MAH 1225u
#define NIMH_CAPACITY_MIN_MAH 300u
#define NIMH_CAPACITY_MAX_MAH 2000u
#define CAPACITY_LEARNING_TEMP_MIN_MDEGC 10000
#define CAPACITY_LEARNING_TEMP_MAX_MDEGC 40000
#define RESISTANCE_LEARNING_TEMP_MIN_MDEGC 10000
#define RESISTANCE_LEARNING_TEMP_MAX_MDEGC 35000
#define RESISTANCE_STEP_MIN_MA 200u
#define RESISTANCE_MIN_MOHM 50u
#define RESISTANCE_MAX_MOHM 1000u
#define RESISTANCE_PERSIST_DELTA_MOHM 25u
#define CAPACITY_CONSISTENCY_PERCENT 20u
#define RECHARGE_INVALIDATE_NAH INT64_C(5000000)
#define NIMH_ACR_LSB_NAH 3825u
#define NIMH_CHARGE_FACTOR_PERMILLE 1224u
#define RESISTANCE_STEP_MIN_MS 100u
#define RESISTANCE_STEP_MAX_MS 1500u
#define RESISTANCE_DROP_MIN_MV 5u
#define RESISTANCE_DROP_MAX_MV 300u
#define RESISTANCE_COUNT_PERSIST_INTERVAL 128u
#define RESISTANCE_HIGH_FLOOR_MV 2530u
#define RESISTANCE_MID_FLOOR_MV 2320u
#define NAH_PER_MAH INT64_C(1000000)

static const battery_learning_profile_t NIMH_PROFILE = {
    .profile_id = BATTERY_LEARNING_PROFILE_NIMH_2S,
    .nominal_capacity_mah = NIMH_NOMINAL_CAPACITY_MAH,
    .capacity_min_mah = NIMH_CAPACITY_MIN_MAH,
    .capacity_max_mah = NIMH_CAPACITY_MAX_MAH,
    .learning_temperature_min_mdegc =
        CAPACITY_LEARNING_TEMP_MIN_MDEGC,
    .learning_temperature_max_mdegc =
        CAPACITY_LEARNING_TEMP_MAX_MDEGC,
    .resistance_temperature_min_mdegc =
        RESISTANCE_LEARNING_TEMP_MIN_MDEGC,
    .resistance_temperature_max_mdegc =
        RESISTANCE_LEARNING_TEMP_MAX_MDEGC,
    .resistance_step_min_ma = RESISTANCE_STEP_MIN_MA,
    .resistance_min_mohm = RESISTANCE_MIN_MOHM,
    .resistance_max_mohm = RESISTANCE_MAX_MOHM,
    .resistance_persist_delta_mohm =
        RESISTANCE_PERSIST_DELTA_MOHM,
    .capacity_consistency_percent = CAPACITY_CONSISTENCY_PERCENT,
    .recharge_invalidate_nah = RECHARGE_INVALIDATE_NAH,
    .acr_lsb_nah = NIMH_ACR_LSB_NAH,
    .charge_factor_permille = NIMH_CHARGE_FACTOR_PERMILLE,
};

static uint16_t saturating_increment_u16(uint16_t value) {
    return value == UINT16_MAX ? value : (uint16_t)(value + 1u);
}

static uint16_t absolute_difference_u16(uint16_t a, uint16_t b) {
    return a >= b ? (uint16_t)(a - b) : (uint16_t)(b - a);
}

static uint64_t positive_difference_i64(int64_t high, int64_t low) {
    if (high <= low) {
        return 0u;
    }
    /* Unsigned conversion makes the full INT64_MIN..INT64_MAX difference
     * domain total without signed overflow. */
    return (uint64_t)high - (uint64_t)low;
}

static bool increase_exceeds_i64(int64_t value, int64_t baseline,
                                 int64_t threshold) {
    return threshold >= 0 &&
        positive_difference_i64(value, baseline) > (uint64_t)threshold;
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

static int64_t saturating_add_i64(int64_t a, int64_t b) {
    if (b > 0 && a > INT64_MAX - b) {
        return INT64_MAX;
    }
    if (b < 0 && a < INT64_MIN - b) {
        return INT64_MIN;
    }
    return a + b;
}

static int32_t modular_u32_delta(uint32_t newer, uint32_t older) {
    uint32_t modular = newer - older;
    if (modular <= INT32_MAX) {
        return (int32_t)modular;
    }
    uint32_t magnitude = UINT32_MAX - modular + 1u;
    if (magnitude == UINT32_C(0x80000000)) {
        return INT32_MIN;
    }
    return -(int32_t)magnitude;
}

static int64_t capacity_nah(uint16_t capacity_mah) {
    return (int64_t)capacity_mah * NAH_PER_MAH;
}

static uint64_t rounded_nah_to_mah(uint64_t nah) {
    uint64_t whole = nah / (uint64_t)NAH_PER_MAH;
    uint64_t remainder = nah % (uint64_t)NAH_PER_MAH;
    return whole + (remainder >= (uint64_t)NAH_PER_MAH / 2u ? 1u : 0u);
}

static bool temperature_in_range(int32_t value, int32_t low, int32_t high) {
    return value >= low && value <= high;
}

static battery_resistance_bin_t resistance_bin_for_mv(uint16_t mv) {
    if (mv >= RESISTANCE_HIGH_FLOOR_MV) {
        return BATTERY_RESISTANCE_BIN_HIGH;
    }
    return mv >= RESISTANCE_MID_FLOOR_MV
        ? BATTERY_RESISTANCE_BIN_MID
        : BATTERY_RESISTANCE_BIN_LOW;
}

static uint16_t median_u16(const uint16_t *values, uint8_t count) {
    uint16_t sorted[BATTERY_LEARNING_RESISTANCE_WINDOW_COUNT];
    if (values == NULL || count == 0u ||
        count > BATTERY_LEARNING_RESISTANCE_WINDOW_COUNT) {
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

static uint16_t learned_capacity_mah(
    const battery_learning_persisted_t *persisted) {
    uint8_t count = persisted->capacity_history_count;
    if (count == 0u) {
        return 0u;
    }
    if (count == 1u) {
        return persisted->capacity_history_mah[0];
    }
    if (count == 2u) {
        return (uint16_t)(
            ((uint32_t)persisted->capacity_history_mah[0] +
             persisted->capacity_history_mah[1] + 1u) /
            2u);
    }
    return median_u16(persisted->capacity_history_mah,
                      BATTERY_LEARNING_CAPACITY_HISTORY_COUNT);
}

static uint16_t capacity_spread_mah(
    const battery_learning_persisted_t *persisted) {
    uint8_t count = persisted->capacity_history_count;
    if (count < 2u) {
        return 0u;
    }
    uint16_t low = persisted->capacity_history_mah[0];
    uint16_t high = low;
    for (uint8_t i = 1u; i < count; i++) {
        if (persisted->capacity_history_mah[i] < low) {
            low = persisted->capacity_history_mah[i];
        }
        if (persisted->capacity_history_mah[i] > high) {
            high = persisted->capacity_history_mah[i];
        }
    }
    return (uint16_t)(high - low);
}

static battery_capacity_confidence_t capacity_confidence(
    const battery_learning_persisted_t *persisted,
    const battery_learning_profile_t *profile);

static uint16_t capacity_estimate_mah(
    const battery_learning_persisted_t *persisted,
    const battery_learning_profile_t *profile) {
    battery_capacity_confidence_t confidence =
        capacity_confidence(persisted, profile);
    if (confidence == BATTERY_CAPACITY_CONFIDENCE_OBSERVED ||
        confidence == BATTERY_CAPACITY_CONFIDENCE_LEARNED) {
        return learned_capacity_mah(persisted);
    }
    return persisted->nominal_capacity_mah;
}

static int64_t soc_project_remaining_nah(
    const battery_learning_persisted_t *persisted,
    int64_t session_delta_nah) {
    if (!persisted->soc_valid) {
        return 0;
    }
    int64_t delta = saturating_sub_i64(
        session_delta_nah, persisted->soc_anchor_session_delta_nah);
    if (persisted->soc_charge_segment && delta > 0) {
        uint16_t factor = persisted->soc_charge_factor_permille;
        if (factor == 0u) {
            return persisted->soc_anchor_remaining_nah;
        }
        uint64_t positive = (uint64_t)delta;
        /* Quotient/remainder form preserves floor(delta * 1000 / factor)
         * without overflowing for an adversarial but valid int64_t delta. */
        uint64_t scaled = (positive / factor) * 1000u +
            ((positive % factor) * 1000u) / factor;
        delta = scaled > (uint64_t)INT64_MAX ? INT64_MAX : (int64_t)scaled;
    }
    int64_t remaining = saturating_add_i64(
        persisted->soc_anchor_remaining_nah, delta);
    int64_t maximum = capacity_nah(persisted->soc_capacity_mah);
    if (remaining < 0) {
        return 0;
    }
    return remaining > maximum ? maximum : remaining;
}

static void soc_set_anchor(
    battery_learning_persisted_t *persisted,
    uint16_t capacity_mah,
    int64_t session_delta_nah,
    int64_t remaining_nah,
    bool charge_segment,
    uint16_t charge_factor_permille,
    uint16_t bootstrap_reference_mv,
    battery_soc_provenance_t provenance,
    battery_soc_confidence_t confidence) {
    int64_t maximum = capacity_nah(capacity_mah);
    if (remaining_nah < 0) {
        remaining_nah = 0;
    } else if (remaining_nah > maximum) {
        remaining_nah = maximum;
    }
    persisted->soc_valid = true;
    persisted->soc_charge_segment = charge_segment;
    persisted->soc_capacity_mah = capacity_mah;
    persisted->soc_charge_factor_permille = charge_factor_permille;
    persisted->soc_bootstrap_reference_mv = bootstrap_reference_mv;
    persisted->soc_anchor_provenance = provenance;
    persisted->soc_confidence = confidence;
    persisted->soc_anchor_session_delta_nah = session_delta_nah;
    persisted->soc_anchor_remaining_nah = remaining_nah;
}

static void soc_clear(battery_learning_persisted_t *persisted) {
    persisted->soc_valid = false;
    persisted->soc_charge_segment = false;
    persisted->soc_capacity_mah = 0u;
    persisted->soc_charge_factor_permille = 0u;
    persisted->soc_bootstrap_reference_mv = 0u;
    persisted->soc_anchor_provenance = BATTERY_SOC_PROVENANCE_UNKNOWN;
    persisted->soc_confidence = BATTERY_SOC_CONFIDENCE_NONE;
    persisted->soc_anchor_session_delta_nah = 0;
    persisted->soc_anchor_remaining_nah = 0;
}

uint8_t battery_learning_bootstrap_soc_percent(uint16_t reference_mv) {
    /* Deliberately coarse: these are the four recovered v6.00 bar thresholds in
     * the same 65 mA reference-load domain. Until the controlled discharge trace
     * calibrates a richer table, selecting the lower bucket avoids claiming
     * precision that a flat NiMH voltage curve cannot provide. */
    if (reference_mv < 1900u) {
        return 0u;
    }
    if (reference_mv < 2425u) {
        return 25u;
    }
    if (reference_mv < 2530u) {
        return 50u;
    }
    if (reference_mv < 2575u) {
        return 75u;
    }
    return 100u;
}

static battery_capacity_confidence_t capacity_confidence(
    const battery_learning_persisted_t *persisted,
    const battery_learning_profile_t *profile) {
    if (persisted->capacity_history_count == 0u) {
        return BATTERY_CAPACITY_CONFIDENCE_PRIOR;
    }
    if (persisted->capacity_history_count <
        BATTERY_LEARNING_CAPACITY_HISTORY_COUNT) {
        return BATTERY_CAPACITY_CONFIDENCE_OBSERVED;
    }
    uint32_t allowed =
        (uint32_t)persisted->nominal_capacity_mah *
        profile->capacity_consistency_percent / 100u;
    return capacity_spread_mah(persisted) <= allowed
        ? BATTERY_CAPACITY_CONFIDENCE_LEARNED
        : BATTERY_CAPACITY_CONFIDENCE_CONFLICTED;
}

static void refresh_snapshot(battery_learning_state_t *state,
                             const battery_learning_profile_t *profile) {
    battery_learning_snapshot_t *snapshot = &state->snapshot;
    const battery_learning_persisted_t *persisted = &state->persisted;
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->nominal_capacity_mah = persisted->nominal_capacity_mah;
    snapshot->learned_capacity_mah = learned_capacity_mah(persisted);
    snapshot->learned_capacity_valid =
        persisted->capacity_history_count != 0u;
    snapshot->last_capacity_mah = persisted->last_capacity_mah;
    snapshot->capacity_spread_mah = capacity_spread_mah(persisted);
    memcpy(snapshot->capacity_history_mah,
           persisted->capacity_history_mah,
           sizeof(snapshot->capacity_history_mah));
    snapshot->capacity_history_count = persisted->capacity_history_count;
    snapshot->capacity_history_next = persisted->capacity_history_next;
    snapshot->capacity_confidence = capacity_confidence(persisted, profile);
    snapshot->full_anchor_valid = persisted->full_anchor_valid;
    snapshot->capacity_cycle_qualified =
        persisted->capacity_cycle_qualified;
    snapshot->natural_empty_valid = persisted->natural_empty_valid;
    snapshot->soc_confidence = persisted->soc_confidence;
    snapshot->soc_charge_segment = persisted->soc_charge_segment;
    snapshot->soc_capacity_mah = persisted->soc_capacity_mah;
    snapshot->soc_bootstrap_reference_mv =
        persisted->soc_bootstrap_reference_mv;
    snapshot->accepted_capacity_cycles =
        persisted->accepted_capacity_cycles;
    snapshot->rejected_capacity_cycles =
        persisted->rejected_capacity_cycles;
    snapshot->pack_generation = persisted->pack_generation;
    snapshot->gauge_session_bound = state->session_seen;
    snapshot->gauge_session = state->last_gauge_session;
    memcpy(snapshot->resistance_mohm, persisted->resistance_mohm,
           sizeof(snapshot->resistance_mohm));
    memcpy(snapshot->resistance_sample_count,
           persisted->resistance_sample_count,
           sizeof(snapshot->resistance_sample_count));
    snapshot->current_resistance_bin =
        resistance_bin_for_mv(state->latest_terminal_mv);
    snapshot->current_resistance_bin_valid =
        state->latest_observation_valid;

    if (snapshot->learned_capacity_valid &&
        persisted->nominal_capacity_mah != 0u) {
        uint32_t soh =
            ((uint32_t)snapshot->learned_capacity_mah * 100u +
             persisted->nominal_capacity_mah / 2u) /
            persisted->nominal_capacity_mah;
        snapshot->state_of_health_percent =
            soh > UINT16_MAX ? UINT16_MAX : (uint16_t)soh;
    }

    if (persisted->full_anchor_valid && state->latest_observation_valid) {
        uint64_t discharged_nah = positive_difference_i64(
            persisted->full_anchor_nah, state->latest_session_delta_nah);
        snapshot->cycle_discharged_nah = discharged_nah;
        uint64_t discharged_mah = discharged_nah / (uint64_t)NAH_PER_MAH;
        if (discharged_mah > UINT16_MAX) {
            discharged_mah = UINT16_MAX;
        }
        snapshot->cycle_discharged_mah = (uint16_t)discharged_mah;

        uint64_t expected_nah =
            (uint64_t)capacity_estimate_mah(persisted, profile) *
            (uint64_t)NAH_PER_MAH;
        if (discharged_nah > expected_nah) {
            snapshot->capacity_overrun_nah =
                discharged_nah - expected_nah;
        }
    }

    if (!persisted->soc_valid || !state->latest_observation_valid) {
        return;
    }
    int64_t remaining = soc_project_remaining_nah(
        persisted, state->latest_session_delta_nah);
    snapshot->remaining_capacity_nah = (uint64_t)remaining;
    snapshot->remaining_capacity_mah = (uint16_t)(
        ((uint64_t)remaining + (uint64_t)NAH_PER_MAH / 2u) /
        (uint64_t)NAH_PER_MAH);
    snapshot->remaining_capacity_valid = true;
    snapshot->soc_provenance =
        state->latest_session_delta_nah ==
                persisted->soc_anchor_session_delta_nah
            ? persisted->soc_anchor_provenance
            : BATTERY_SOC_PROVENANCE_TRACKED;
    int64_t maximum = capacity_nah(persisted->soc_capacity_mah);
    snapshot->state_of_charge_percent = maximum == 0
        ? 0u
        : (uint8_t)(((uint64_t)remaining * 100u +
                     (uint64_t)maximum / 2u) /
                    (uint64_t)maximum);
    if (snapshot->learned_capacity_valid && maximum > 0 &&
        snapshot->capacity_confidence !=
            BATTERY_CAPACITY_CONFIDENCE_CONFLICTED) {
        snapshot->soc_bars_valid = true;
        if (remaining == 0) {
            snapshot->soc_bars = 0u;
        } else {
            uint64_t bars = ((uint64_t)remaining * 4u +
                             (uint64_t)maximum - 1u) /
                            (uint64_t)maximum;
            snapshot->soc_bars = bars > 4u ? 4u : (uint8_t)bars;
        }
        snapshot->capacity_prediction_exhausted =
            !persisted->soc_charge_segment &&
            persisted->soc_confidence == BATTERY_SOC_CONFIDENCE_ANCHORED &&
            remaining == 0 &&
            persisted->full_anchor_valid &&
            persisted->capacity_cycle_qualified;
    }
}

static void clear_pack_learning(battery_learning_state_t *state) {
    uint8_t profile_id = state->persisted.profile_id;
    uint16_t nominal = state->persisted.nominal_capacity_mah;
    uint32_t generation = state->persisted.pack_generation + 1u;
    memset(&state->persisted, 0, sizeof(state->persisted));
    state->persisted.profile_id = profile_id;
    state->persisted.nominal_capacity_mah = nominal;
    state->persisted.pack_generation = generation;
    memset(state->resistance_window, 0, sizeof(state->resistance_window));
    memset(state->resistance_window_count, 0,
           sizeof(state->resistance_window_count));
    memset(state->resistance_persisted_mohm, 0,
           sizeof(state->resistance_persisted_mohm));
    memset(state->resistance_persisted_count, 0,
           sizeof(state->resistance_persisted_count));
    state->previous_resistance_sample_valid = false;
    state->latest_observation_valid = false;
    state->soc_bootstrap_reference_sum_mv = 0u;
    state->soc_bootstrap_sample_count = 0u;
}

static void add_capacity_sample(battery_learning_persisted_t *persisted,
                                uint16_t capacity_mah) {
    if (persisted->capacity_history_count <
        BATTERY_LEARNING_CAPACITY_HISTORY_COUNT) {
        uint8_t index = persisted->capacity_history_count;
        persisted->capacity_history_mah[index] = capacity_mah;
        persisted->capacity_history_count++;
        persisted->capacity_history_next =
            (uint8_t)(persisted->capacity_history_count %
                      BATTERY_LEARNING_CAPACITY_HISTORY_COUNT);
    } else {
        uint8_t index = persisted->capacity_history_next;
        persisted->capacity_history_mah[index] = capacity_mah;
        persisted->capacity_history_next =
            (uint8_t)((index + 1u) %
                      BATTERY_LEARNING_CAPACITY_HISTORY_COUNT);
    }
    persisted->last_capacity_mah = capacity_mah;
    persisted->accepted_capacity_cycles =
        saturating_increment_u16(persisted->accepted_capacity_cycles);
}

static int32_t discharge_load_ma(int32_t current_ua) {
    if (current_ua >= 0) {
        return 0;
    }
    int64_t magnitude = -(int64_t)current_ua;
    magnitude = (magnitude + 500) / 1000;
    return magnitude > INT32_MAX ? INT32_MAX : (int32_t)magnitude;
}

static uint32_t observe_resistance_step(
    battery_learning_state_t *state,
    const battery_learning_profile_t *profile,
    const battery_learning_observation_t *observation) {
    uint32_t result = BATTERY_LEARNING_RESULT_NONE;
    bool eligible = observation->sample_valid &&
        observation->current_valid && observation->continuity_valid &&
        observation->authoritative && !observation->charger_connected &&
        !observation->charge_active &&
        temperature_in_range(
            observation->temperature_mdegc,
            profile->resistance_temperature_min_mdegc,
            profile->resistance_temperature_max_mdegc);

    if (eligible && state->previous_resistance_sample_valid) {
        uint32_t elapsed_ms =
            observation->now_ms - state->previous_ms;
        int32_t previous_load_ma =
            discharge_load_ma(state->previous_current_ua);
        int32_t current_load_ma =
            discharge_load_ma(observation->current_ua);
        int32_t load_step_ma = current_load_ma - previous_load_ma;
        int32_t voltage_drop_mv =
            (int32_t)state->previous_terminal_mv -
            observation->terminal_mv;
        if (elapsed_ms >= RESISTANCE_STEP_MIN_MS &&
            elapsed_ms <= RESISTANCE_STEP_MAX_MS &&
            load_step_ma >= profile->resistance_step_min_ma &&
            voltage_drop_mv >= (int32_t)RESISTANCE_DROP_MIN_MV &&
            voltage_drop_mv <= (int32_t)RESISTANCE_DROP_MAX_MV) {
            int64_t numerator = (int64_t)voltage_drop_mv * 1000;
            uint32_t resistance_mohm = (uint32_t)(
                (numerator + load_step_ma / 2) / load_step_ma);
            if (resistance_mohm >= profile->resistance_min_mohm &&
                resistance_mohm <= profile->resistance_max_mohm) {
                uint16_t midpoint_mv = (uint16_t)(
                    ((uint32_t)state->previous_terminal_mv +
                     observation->terminal_mv) /
                    2u);
                battery_resistance_bin_t bin =
                    resistance_bin_for_mv(midpoint_mv);
                uint8_t count = state->resistance_window_count[bin];
                state->resistance_window[bin][count++] =
                    (uint16_t)resistance_mohm;
                state->resistance_window_count[bin] = count;
                state->persisted.resistance_sample_count[bin] =
                    saturating_increment_u16(
                        state->persisted.resistance_sample_count[bin]);
                if (count == BATTERY_LEARNING_RESISTANCE_WINDOW_COUNT) {
                    uint16_t estimate = median_u16(
                        state->resistance_window[bin], count);
                    uint16_t old = state->persisted.resistance_mohm[bin];
                    state->persisted.resistance_mohm[bin] = estimate;
                    state->resistance_window_count[bin] = 0u;
                    result |= BATTERY_LEARNING_RESULT_RESISTANCE_UPDATED;
                    uint16_t count_since_persist = (uint16_t)(
                        state->persisted.resistance_sample_count[bin] -
                        state->resistance_persisted_count[bin]);
                    if (old == 0u ||
                        absolute_difference_u16(
                            estimate,
                            state->resistance_persisted_mohm[bin]) >=
                            profile->resistance_persist_delta_mohm ||
                        count_since_persist >=
                            RESISTANCE_COUNT_PERSIST_INTERVAL) {
                        result |= BATTERY_LEARNING_RESULT_PERSIST;
                    }
                }
            }
        }
    }

    state->previous_resistance_sample_valid = eligible;
    if (eligible) {
        state->previous_ms = observation->now_ms;
        state->previous_terminal_mv = observation->terminal_mv;
        state->previous_current_ua = observation->current_ua;
    }
    return result;
}

const battery_learning_profile_t *battery_learning_nimh_profile(void) {
    return &NIMH_PROFILE;
}

void battery_learning_persisted_defaults(
    battery_learning_persisted_t *persisted,
    const battery_learning_profile_t *profile) {
    if (persisted == NULL) {
        return;
    }
    if (profile == NULL) {
        profile = &NIMH_PROFILE;
    }
    memset(persisted, 0, sizeof(*persisted));
    persisted->profile_id = (uint8_t)profile->profile_id;
    persisted->nominal_capacity_mah = profile->nominal_capacity_mah;
}

bool battery_learning_persisted_valid(
    const battery_learning_persisted_t *persisted,
    const battery_learning_profile_t *profile) {
    if (persisted == NULL) {
        return false;
    }
    if (profile == NULL) {
        profile = &NIMH_PROFILE;
    }
    if (persisted->profile_id != (uint8_t)profile->profile_id ||
        persisted->nominal_capacity_mah != profile->nominal_capacity_mah ||
        persisted->capacity_history_count >
            BATTERY_LEARNING_CAPACITY_HISTORY_COUNT ||
        persisted->capacity_history_next >=
            BATTERY_LEARNING_CAPACITY_HISTORY_COUNT ||
        (persisted->capacity_history_count <
             BATTERY_LEARNING_CAPACITY_HISTORY_COUNT &&
         persisted->capacity_history_next !=
             persisted->capacity_history_count) ||
        persisted->accepted_capacity_cycles <
            persisted->capacity_history_count ||
        (persisted->capacity_cycle_qualified &&
         !persisted->full_anchor_valid) ||
        (persisted->natural_empty_valid &&
         persisted->full_anchor_valid)) {
        return false;
    }
    if ((persisted->capacity_history_count == 0u) !=
        (persisted->last_capacity_mah == 0u)) {
        return false;
    }
    for (uint8_t i = 0u;
         i < persisted->capacity_history_count; i++) {
        uint16_t capacity = persisted->capacity_history_mah[i];
        if (capacity < profile->capacity_min_mah ||
            capacity > profile->capacity_max_mah) {
            return false;
        }
    }
    if (persisted->last_capacity_mah != 0u &&
        (persisted->last_capacity_mah < profile->capacity_min_mah ||
         persisted->last_capacity_mah > profile->capacity_max_mah)) {
        return false;
    }
    for (uint8_t i = 0u;
         i < BATTERY_LEARNING_RESISTANCE_BIN_COUNT; i++) {
        uint16_t resistance = persisted->resistance_mohm[i];
        if (resistance != 0u &&
            (resistance < profile->resistance_min_mohm ||
             resistance > profile->resistance_max_mohm)) {
            return false;
        }
    }
    if (persisted->full_anchor_valid &&
        persisted->cycle_min_temperature_mdegc >
            persisted->cycle_max_temperature_mdegc) {
        return false;
    }
    if (persisted->soc_anchor_provenance >
            BATTERY_SOC_PROVENANCE_ANCHORED_EMPTY ||
        persisted->soc_confidence > BATTERY_SOC_CONFIDENCE_ANCHORED) {
        return false;
    }
    if (!persisted->soc_valid) {
        return !persisted->soc_charge_segment &&
            persisted->soc_capacity_mah == 0u &&
            persisted->soc_charge_factor_permille == 0u &&
            persisted->soc_bootstrap_reference_mv == 0u &&
            persisted->soc_anchor_provenance ==
                BATTERY_SOC_PROVENANCE_UNKNOWN &&
            persisted->soc_confidence == BATTERY_SOC_CONFIDENCE_NONE &&
            persisted->soc_anchor_session_delta_nah == 0 &&
            persisted->soc_anchor_remaining_nah == 0;
    }
    int64_t soc_capacity = capacity_nah(persisted->soc_capacity_mah);
    if (persisted->soc_capacity_mah < profile->capacity_min_mah ||
        persisted->soc_capacity_mah > profile->capacity_max_mah ||
        persisted->soc_charge_factor_permille < 1000u ||
        persisted->soc_charge_factor_permille > 2000u ||
        persisted->soc_anchor_provenance ==
            BATTERY_SOC_PROVENANCE_UNKNOWN ||
        persisted->soc_confidence == BATTERY_SOC_CONFIDENCE_NONE ||
        persisted->soc_anchor_remaining_nah < 0 ||
        persisted->soc_anchor_remaining_nah > soc_capacity) {
        return false;
    }
    if (persisted->soc_confidence == BATTERY_SOC_CONFIDENCE_PROVISIONAL) {
        if (persisted->soc_bootstrap_reference_mv == 0u ||
            (persisted->soc_anchor_provenance !=
                 BATTERY_SOC_PROVENANCE_BOOTSTRAP_VOLTAGE &&
             persisted->soc_anchor_provenance !=
                 BATTERY_SOC_PROVENANCE_TRACKED)) {
            return false;
        }
    } else if (persisted->soc_bootstrap_reference_mv != 0u ||
               (persisted->soc_anchor_provenance !=
                    BATTERY_SOC_PROVENANCE_ANCHORED_FULL &&
                persisted->soc_anchor_provenance !=
                    BATTERY_SOC_PROVENANCE_ANCHORED_EMPTY &&
                persisted->soc_anchor_provenance !=
                    BATTERY_SOC_PROVENANCE_TRACKED)) {
        return false;
    }
    if (persisted->soc_anchor_provenance ==
            BATTERY_SOC_PROVENANCE_ANCHORED_FULL &&
        persisted->soc_anchor_remaining_nah != soc_capacity) {
        return false;
    }
    if (persisted->soc_anchor_provenance ==
            BATTERY_SOC_PROVENANCE_ANCHORED_EMPTY &&
        persisted->soc_anchor_remaining_nah != 0) {
        return false;
    }
    return true;
}

void battery_learning_init(
    battery_learning_state_t *state,
    const battery_learning_profile_t *profile,
    const battery_learning_persisted_t *persisted) {
    if (state == NULL) {
        return;
    }
    if (profile == NULL) {
        profile = &NIMH_PROFILE;
    }
    memset(state, 0, sizeof(*state));
    if (persisted != NULL) {
        state->persisted = *persisted;
    } else {
        battery_learning_persisted_defaults(&state->persisted, profile);
    }
    if (!battery_learning_persisted_valid(&state->persisted, profile)) {
        battery_learning_persisted_defaults(&state->persisted, profile);
    }
    /* BTL1 records predate the general SOC ledger. A qualified FULL anchor is
     * already an exact ACR-relative origin, so migrate it without inventing any
     * new evidence. Natural EMPTY is migrated on the first coherent sample,
     * when there is an ACR value to bind to. */
    if (!state->persisted.soc_valid &&
        state->persisted.full_anchor_valid) {
        uint16_t capacity = capacity_estimate_mah(
            &state->persisted, profile);
        soc_set_anchor(
            &state->persisted, capacity,
            state->persisted.full_anchor_nah,
            capacity_nah(capacity), false,
            profile->charge_factor_permille, 0u,
            BATTERY_SOC_PROVENANCE_ANCHORED_FULL,
            BATTERY_SOC_CONFIDENCE_ANCHORED);
    }
    for (uint8_t i = 0u;
         i < BATTERY_LEARNING_RESISTANCE_BIN_COUNT; i++) {
        state->resistance_persisted_mohm[i] =
            state->persisted.resistance_mohm[i];
        state->resistance_persisted_count[i] =
            state->persisted.resistance_sample_count[i];
    }
    refresh_snapshot(state, profile);
}

bool battery_learning_provision(
    battery_learning_state_t *state,
    const battery_learning_profile_t *profile,
    const battery_learning_provision_t *provision) {
    if (state == NULL || provision == NULL) {
        return false;
    }
    if (profile == NULL) {
        profile = &NIMH_PROFILE;
    }
    if (provision->capacity_mah < profile->capacity_min_mah ||
        provision->capacity_mah > profile->capacity_max_mah) {
        return false;
    }
    for (uint8_t i = 0u;
         i < BATTERY_LEARNING_RESISTANCE_BIN_COUNT; i++) {
        uint16_t resistance = provision->resistance_mohm[i];
        if (resistance != 0u &&
            (resistance < profile->resistance_min_mohm ||
             resistance > profile->resistance_max_mohm)) {
            return false;
        }
    }

    /* clear_pack_learning deliberately leaves session_seen and
     * last_gauge_session intact. The first ordinary sample after provisioning
     * must not interpret the same installed pack as another replacement. */
    clear_pack_learning(state);
    add_capacity_sample(&state->persisted, provision->capacity_mah);
    for (uint8_t i = 0u;
         i < BATTERY_LEARNING_RESISTANCE_BIN_COUNT; i++) {
        state->persisted.resistance_mohm[i] =
            provision->resistance_mohm[i];
        state->resistance_persisted_mohm[i] =
            provision->resistance_mohm[i];
    }
    state->persisted.natural_empty_valid =
        provision->natural_empty_valid;
    refresh_snapshot(state, profile);
    return true;
}

bool battery_learning_recover_capacity_cycle(
    battery_learning_state_t *state,
    const battery_learning_profile_t *profile,
    uint16_t capacity_mah,
    uint16_t expected_accepted_cycles) {
    if (state == NULL) {
        return false;
    }
    if (profile == NULL) {
        profile = &NIMH_PROFILE;
    }
    if (capacity_mah < profile->capacity_min_mah ||
        capacity_mah > profile->capacity_max_mah ||
        state->persisted.accepted_capacity_cycles !=
            expected_accepted_cycles ||
        expected_accepted_cycles == UINT16_MAX ||
        !battery_learning_persisted_valid(&state->persisted, profile)) {
        return false;
    }

    battery_learning_state_t candidate = *state;
    battery_learning_persisted_t before = candidate.persisted;
    int64_t old_remaining = before.soc_anchor_remaining_nah;
    int64_t reanchor_delta = before.soc_anchor_session_delta_nah;
    if (before.soc_valid && candidate.latest_observation_valid) {
        old_remaining = soc_project_remaining_nah(
            &before, candidate.latest_session_delta_nah);
        reanchor_delta = candidate.latest_session_delta_nah;
    }

    add_capacity_sample(&candidate.persisted, capacity_mah);
    uint16_t updated_capacity = capacity_estimate_mah(
        &candidate.persisted, profile);
    if (before.soc_valid) {
        uint16_t factor = before.soc_charge_factor_permille;
        if (candidate.persisted.full_anchor_valid) {
            soc_set_anchor(
                &candidate.persisted, updated_capacity,
                candidate.persisted.full_anchor_nah,
                capacity_nah(updated_capacity), false, factor, 0u,
                BATTERY_SOC_PROVENANCE_ANCHORED_FULL,
                BATTERY_SOC_CONFIDENCE_ANCHORED);
        } else if (before.soc_anchor_provenance ==
                       BATTERY_SOC_PROVENANCE_ANCHORED_EMPTY &&
                   old_remaining == 0) {
            soc_set_anchor(
                &candidate.persisted, updated_capacity, reanchor_delta, 0,
                before.soc_charge_segment, factor, 0u,
                BATTERY_SOC_PROVENANCE_ANCHORED_EMPTY,
                BATTERY_SOC_CONFIDENCE_ANCHORED);
        } else {
            uint64_t scaled_remaining = before.soc_capacity_mah == 0u
                ? 0u
                : ((uint64_t)old_remaining * updated_capacity +
                   before.soc_capacity_mah / 2u) /
                      before.soc_capacity_mah;
            soc_set_anchor(
                &candidate.persisted, updated_capacity, reanchor_delta,
                (int64_t)scaled_remaining, before.soc_charge_segment,
                factor, before.soc_bootstrap_reference_mv,
                BATTERY_SOC_PROVENANCE_TRACKED, before.soc_confidence);
        }
    }
    if (!battery_learning_persisted_valid(&candidate.persisted, profile)) {
        return false;
    }
    refresh_snapshot(&candidate, profile);
    *state = candidate;
    return true;
}

bool battery_learning_recover_full_endpoint(
    battery_learning_state_t *state,
    const battery_learning_profile_t *profile,
    const battery_learning_observation_t *current_observation,
    uint32_t full_acr_raw,
    int64_t full_session_delta_nah,
    uint16_t expected_accepted_cycles) {
    if (state == NULL || current_observation == NULL) {
        return false;
    }
    if (profile == NULL) {
        profile = &NIMH_PROFILE;
    }
    bool current_evidence_valid = current_observation->authoritative &&
        current_observation->sample_valid &&
        current_observation->current_valid &&
        current_observation->continuity_valid &&
        !current_observation->charger_connected &&
        !current_observation->charge_active &&
        current_observation->current_ua <= 0;
    int64_t raw_endpoint_delta_nah =
        (int64_t)modular_u32_delta(
            full_acr_raw, current_observation->acr_raw) *
        profile->acr_lsb_nah;
    int64_t reported_endpoint_delta_nah = saturating_sub_i64(
        full_session_delta_nah, current_observation->session_delta_nah);
    if (!current_evidence_valid || !state->session_seen ||
        state->last_gauge_session != current_observation->gauge_session ||
        state->persisted.accepted_capacity_cycles !=
            expected_accepted_cycles ||
        state->persisted.capacity_history_count == 0u ||
        capacity_confidence(&state->persisted, profile) ==
            BATTERY_CAPACITY_CONFIDENCE_CONFLICTED ||
        profile->acr_lsb_nah == 0u ||
        raw_endpoint_delta_nah < 0 ||
        raw_endpoint_delta_nah != reported_endpoint_delta_nah ||
        full_session_delta_nah < current_observation->session_delta_nah ||
        positive_difference_i64(
            full_session_delta_nah,
            current_observation->session_delta_nah) >
            (uint64_t)profile->capacity_max_mah * (uint64_t)NAH_PER_MAH ||
        !temperature_in_range(
            current_observation->temperature_mdegc,
            profile->learning_temperature_min_mdegc,
            profile->learning_temperature_max_mdegc)) {
        return false;
    }
    if (state->persisted.full_anchor_valid) {
        return state->persisted.full_anchor_acr_raw == full_acr_raw &&
            state->persisted.full_anchor_nah == full_session_delta_nah &&
            state->persisted.capacity_cycle_qualified;
    }

    battery_learning_state_t candidate = *state;
    candidate.persisted.natural_empty_valid = false;
    candidate.persisted.full_anchor_valid = true;
    candidate.persisted.capacity_cycle_qualified = true;
    candidate.persisted.full_anchor_acr_raw = full_acr_raw;
    candidate.persisted.full_anchor_nah = full_session_delta_nah;
    candidate.persisted.cycle_min_temperature_mdegc =
        current_observation->temperature_mdegc;
    candidate.persisted.cycle_max_temperature_mdegc =
        current_observation->temperature_mdegc;
    candidate.latest_session_delta_nah =
        current_observation->session_delta_nah;
    candidate.latest_terminal_mv = current_observation->terminal_mv;
    candidate.latest_observation_valid = true;
    candidate.previous_resistance_sample_valid = false;

    uint16_t capacity = capacity_estimate_mah(&candidate.persisted, profile);
    soc_set_anchor(
        &candidate.persisted, capacity, full_session_delta_nah,
        capacity_nah(capacity), false, profile->charge_factor_permille, 0u,
        BATTERY_SOC_PROVENANCE_ANCHORED_FULL,
        BATTERY_SOC_CONFIDENCE_ANCHORED);
    if (!battery_learning_persisted_valid(&candidate.persisted, profile)) {
        return false;
    }
    refresh_snapshot(&candidate, profile);
    *state = candidate;
    return true;
}

static uint32_t update_soc_ledger(
    battery_learning_state_t *state,
    const battery_learning_profile_t *profile,
    const battery_learning_observation_t *observation,
    bool distinct_sample,
    bool natural_empty_at_entry,
    bool full_endpoint,
    bool empty_endpoint) {
    battery_learning_persisted_t *persisted = &state->persisted;
    bool evidence_valid = observation->sample_valid &&
        observation->current_valid && observation->continuity_valid;

    if (distinct_sample && observation->sample_valid &&
        (!observation->current_valid || !observation->continuity_valid)) {
        if (persisted->soc_valid) {
            soc_clear(persisted);
            state->soc_bootstrap_reference_sum_mv = 0u;
            state->soc_bootstrap_sample_count = 0u;
            return BATTERY_LEARNING_RESULT_SOC_INVALIDATED |
                   BATTERY_LEARNING_RESULT_PERSIST;
        }
        return BATTERY_LEARNING_RESULT_NONE;
    }
    if (!evidence_valid) {
        return BATTERY_LEARNING_RESULT_NONE;
    }

    uint16_t capacity = capacity_estimate_mah(persisted, profile);
    if (full_endpoint) {
        soc_set_anchor(
            persisted, capacity, observation->session_delta_nah,
            capacity_nah(capacity), false, profile->charge_factor_permille,
            0u, BATTERY_SOC_PROVENANCE_ANCHORED_FULL,
            BATTERY_SOC_CONFIDENCE_ANCHORED);
        state->soc_bootstrap_reference_sum_mv = 0u;
        state->soc_bootstrap_sample_count = 0u;
        return BATTERY_LEARNING_RESULT_SOC_REANCHORED |
               BATTERY_LEARNING_RESULT_PERSIST;
    }
    if (empty_endpoint) {
        soc_set_anchor(
            persisted, capacity, observation->session_delta_nah, 0,
            false, profile->charge_factor_permille, 0u,
            BATTERY_SOC_PROVENANCE_ANCHORED_EMPTY,
            BATTERY_SOC_CONFIDENCE_ANCHORED);
        state->soc_bootstrap_reference_sum_mv = 0u;
        state->soc_bootstrap_sample_count = 0u;
        return BATTERY_LEARNING_RESULT_SOC_REANCHORED |
               BATTERY_LEARNING_RESULT_PERSIST;
    }

    uint32_t result = BATTERY_LEARNING_RESULT_NONE;
    if (!persisted->soc_valid && persisted->full_anchor_valid) {
        soc_set_anchor(
            persisted, capacity, persisted->full_anchor_nah,
            capacity_nah(capacity), false, profile->charge_factor_permille,
            0u, BATTERY_SOC_PROVENANCE_ANCHORED_FULL,
            BATTERY_SOC_CONFIDENCE_ANCHORED);
        result |= BATTERY_LEARNING_RESULT_SOC_REANCHORED |
                  BATTERY_LEARNING_RESULT_PERSIST;
    } else if (!persisted->soc_valid && natural_empty_at_entry) {
        soc_set_anchor(
            persisted, capacity, observation->session_delta_nah, 0,
            false, profile->charge_factor_permille, 0u,
            BATTERY_SOC_PROVENANCE_ANCHORED_EMPTY,
            BATTERY_SOC_CONFIDENCE_ANCHORED);
        result |= BATTERY_LEARNING_RESULT_SOC_REANCHORED |
                  BATTERY_LEARNING_RESULT_PERSIST;
    }

    bool charge_segment = observation->charger_connected &&
                          observation->charge_active;
    if (persisted->soc_valid &&
        persisted->soc_charge_segment != charge_segment) {
        int64_t remaining = soc_project_remaining_nah(
            persisted, observation->session_delta_nah);
        soc_set_anchor(
            persisted, persisted->soc_capacity_mah,
            observation->session_delta_nah, remaining, charge_segment,
            profile->charge_factor_permille,
            persisted->soc_bootstrap_reference_mv,
            BATTERY_SOC_PROVENANCE_TRACKED,
            persisted->soc_confidence);
        result |= BATTERY_LEARNING_RESULT_SOC_REANCHORED |
                  BATTERY_LEARNING_RESULT_PERSIST;
    }

    if (persisted->soc_valid) {
        state->soc_bootstrap_reference_sum_mv = 0u;
        state->soc_bootstrap_sample_count = 0u;
        return result;
    }

    bool bootstrap_sample = distinct_sample &&
        observation->reference_valid && !observation->charger_connected &&
        !observation->charge_active;
    if (!bootstrap_sample) {
        if (distinct_sample) {
            state->soc_bootstrap_reference_sum_mv = 0u;
            state->soc_bootstrap_sample_count = 0u;
        }
        return result;
    }
    state->soc_bootstrap_reference_sum_mv += observation->reference_mv;
    state->soc_bootstrap_sample_count++;
    if (state->soc_bootstrap_sample_count <
        BATTERY_LEARNING_SOC_BOOTSTRAP_SAMPLE_COUNT) {
        return result;
    }

    uint16_t reference_mv = (uint16_t)(
        state->soc_bootstrap_reference_sum_mv /
        BATTERY_LEARNING_SOC_BOOTSTRAP_SAMPLE_COUNT);
    uint8_t percent = battery_learning_bootstrap_soc_percent(reference_mv);
    int64_t remaining = (capacity_nah(capacity) * percent + 50) / 100;
    soc_set_anchor(
        persisted, capacity, observation->session_delta_nah, remaining,
        false, profile->charge_factor_permille, reference_mv,
        BATTERY_SOC_PROVENANCE_BOOTSTRAP_VOLTAGE,
        BATTERY_SOC_CONFIDENCE_PROVISIONAL);
    state->soc_bootstrap_reference_sum_mv = 0u;
    state->soc_bootstrap_sample_count = 0u;
    return result | BATTERY_LEARNING_RESULT_SOC_BOOTSTRAPPED |
           BATTERY_LEARNING_RESULT_PERSIST;
}

uint32_t battery_learning_update(
    battery_learning_state_t *state,
    const battery_learning_profile_t *profile,
    const battery_learning_observation_t *observation,
    uint32_t events) {
    if (state == NULL || observation == NULL) {
        return BATTERY_LEARNING_RESULT_NONE;
    }
    if (profile == NULL) {
        profile = &NIMH_PROFILE;
    }
    if (!observation->authoritative) {
        return BATTERY_LEARNING_RESULT_NONE;
    }

    uint32_t result = BATTERY_LEARNING_RESULT_NONE;
    if (!state->session_seen) {
        state->session_seen = true;
        state->last_gauge_session = observation->gauge_session;
        if (observation->gauge_session != 0u) {
            clear_pack_learning(state);
            result |= BATTERY_LEARNING_RESULT_PACK_RESET |
                      BATTERY_LEARNING_RESULT_PERSIST;
        }
    } else if (state->last_gauge_session != observation->gauge_session) {
        state->last_gauge_session = observation->gauge_session;
        clear_pack_learning(state);
        result |= BATTERY_LEARNING_RESULT_PACK_RESET |
                  BATTERY_LEARNING_RESULT_PERSIST;
    }
    bool natural_empty_at_entry = state->persisted.natural_empty_valid;

    bool distinct_sample =
        !state->sample_seen ||
        state->last_sample_sequence != observation->sample_sequence;
    bool coulomb_evidence_valid = observation->sample_valid &&
        observation->current_valid && observation->continuity_valid;
    if (!coulomb_evidence_valid) {
        /* LTC transport failure can invalidate a cached sample without
         * advancing sample_sequence. Drop live SOC/bin projection at once and
         * never bridge a resistance pair across that evidence gap. */
        state->latest_observation_valid = false;
        state->previous_resistance_sample_valid = false;
        if (observation->sample_valid && observation->continuity_valid &&
            !observation->current_valid &&
            state->persisted.full_anchor_valid &&
            state->persisted.capacity_cycle_qualified) {
            /* ACR direction is part of the capacity evidence contract.
             * Preserve the anchor for diagnostics, but never learn a cycle
             * after polarity has become unqualified. */
            state->persisted.capacity_cycle_qualified = false;
            result |= BATTERY_LEARNING_RESULT_PERSIST;
        }
    }
    if (distinct_sample) {
        state->sample_seen = true;
        state->last_sample_sequence = observation->sample_sequence;
        if (coulomb_evidence_valid) {
            state->latest_session_delta_nah =
                observation->session_delta_nah;
            state->latest_terminal_mv = observation->terminal_mv;
            state->latest_observation_valid = true;
            /* The supervisor polls before the learner and therefore freezes
             * this marker on the first admitted positive-current sample. Once
             * any untracked charge has entered the pack, however, a later
             * admission may no longer claim an exact natural-empty start. */
            if (state->persisted.natural_empty_valid &&
                observation->current_ua > 0) {
                state->persisted.natural_empty_valid = false;
                result |= BATTERY_LEARNING_RESULT_PERSIST;
            }
            if (state->persisted.full_anchor_valid) {
                if (!temperature_in_range(
                        observation->temperature_mdegc,
                        profile->learning_temperature_min_mdegc,
                        profile->learning_temperature_max_mdegc) &&
                    state->persisted.capacity_cycle_qualified) {
                    state->persisted.capacity_cycle_qualified = false;
                    result |= BATTERY_LEARNING_RESULT_PERSIST;
                }
                if (observation->temperature_mdegc <
                    state->persisted.cycle_min_temperature_mdegc) {
                    state->persisted.cycle_min_temperature_mdegc =
                        observation->temperature_mdegc;
                }
                if (observation->temperature_mdegc >
                    state->persisted.cycle_max_temperature_mdegc) {
                    state->persisted.cycle_max_temperature_mdegc =
                        observation->temperature_mdegc;
                }
                bool recharged =
                    (observation->charge_active &&
                     observation->current_ua > 0) ||
                    increase_exceeds_i64(
                        observation->session_delta_nah,
                        state->persisted.full_anchor_nah,
                        profile->recharge_invalidate_nah);
                if (recharged) {
                    state->persisted.full_anchor_valid = false;
                    state->persisted.capacity_cycle_qualified = false;
                    result |= BATTERY_LEARNING_RESULT_PERSIST;
                }
            }
        }
        result |= observe_resistance_step(state, profile, observation);
    }

    /* FULL and EMPTY are mutually exclusive app transitions. Ignore a corrupt
     * combined event rather than manufacturing a zero-capacity cycle. */
    bool event_conflict =
        (events & (BATTERY_LEARNING_EVENT_FULL |
                   BATTERY_LEARNING_EVENT_EMPTY)) ==
        (BATTERY_LEARNING_EVENT_FULL |
         BATTERY_LEARNING_EVENT_EMPTY);
    bool event_evidence_valid = observation->sample_valid &&
        observation->current_valid && observation->continuity_valid &&
        observation->charger_connected;
    bool full_endpoint = false;
    if (!event_conflict &&
        (events & BATTERY_LEARNING_EVENT_FULL) != 0u &&
        event_evidence_valid) {
        state->persisted.natural_empty_valid = false;
        state->persisted.full_anchor_valid = true;
        state->persisted.capacity_cycle_qualified =
            temperature_in_range(
                observation->temperature_mdegc,
                profile->learning_temperature_min_mdegc,
                profile->learning_temperature_max_mdegc);
        state->persisted.full_anchor_nah = observation->session_delta_nah;
        state->persisted.full_anchor_acr_raw = observation->acr_raw;
        state->persisted.cycle_min_temperature_mdegc =
            observation->temperature_mdegc;
        state->persisted.cycle_max_temperature_mdegc =
            observation->temperature_mdegc;
        state->latest_session_delta_nah = observation->session_delta_nah;
        state->latest_terminal_mv = observation->terminal_mv;
        state->latest_observation_valid = true;
        result |= BATTERY_LEARNING_RESULT_FULL_ANCHORED |
                  BATTERY_LEARNING_RESULT_PERSIST;
        full_endpoint = true;
    }

    bool empty_endpoint = false;
    if (!event_conflict &&
        (events & BATTERY_LEARNING_EVENT_EMPTY) != 0u &&
        observation->sample_valid && observation->current_valid &&
        observation->continuity_valid) {
        bool accepted = false;
        if (state->persisted.full_anchor_valid &&
            state->persisted.capacity_cycle_qualified) {
            uint64_t discharged_nah = positive_difference_i64(
                state->persisted.full_anchor_nah,
                observation->session_delta_nah);
            if (discharged_nah > 0u) {
                uint64_t rounded_mah = rounded_nah_to_mah(discharged_nah);
                if (rounded_mah >= profile->capacity_min_mah &&
                    rounded_mah <= profile->capacity_max_mah) {
                    add_capacity_sample(
                        &state->persisted, (uint16_t)rounded_mah);
                    accepted = true;
                }
            }
        }
        if (accepted) {
            result |= BATTERY_LEARNING_RESULT_CAPACITY_ACCEPTED;
        } else {
            state->persisted.rejected_capacity_cycles =
                saturating_increment_u16(
                    state->persisted.rejected_capacity_cycles);
            result |= BATTERY_LEARNING_RESULT_CAPACITY_REJECTED;
        }
        state->persisted.full_anchor_valid = false;
        state->persisted.capacity_cycle_qualified = false;
        state->persisted.natural_empty_valid =
            observation->current_ua <= 0;
        result |= BATTERY_LEARNING_RESULT_PERSIST;
        empty_endpoint = true;
    }

    result |= update_soc_ledger(
        state, profile, observation, distinct_sample,
        natural_empty_at_entry, full_endpoint, empty_endpoint);

    if ((result & BATTERY_LEARNING_RESULT_PERSIST) != 0u) {
        for (uint8_t i = 0u;
             i < BATTERY_LEARNING_RESISTANCE_BIN_COUNT; i++) {
            state->resistance_persisted_mohm[i] =
                state->persisted.resistance_mohm[i];
            state->resistance_persisted_count[i] =
                state->persisted.resistance_sample_count[i];
        }
    }
    refresh_snapshot(state, profile);
    return result;
}

void battery_learning_get_snapshot(
    const battery_learning_state_t *state,
    battery_learning_snapshot_t *out) {
    if (out == NULL) {
        return;
    }
    *out = state != NULL ? state->snapshot
                         : (battery_learning_snapshot_t){0};
}

void battery_learning_export_persisted(
    const battery_learning_state_t *state,
    battery_learning_persisted_t *out) {
    if (out == NULL) {
        return;
    }
    *out = state != NULL ? state->persisted
                         : (battery_learning_persisted_t){0};
}

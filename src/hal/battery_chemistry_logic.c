#include "hal/battery_chemistry_logic.h"

#include <string.h>

#define RETAINED_NIMH UINT32_C(0x424301fe)
#define RETAINED_LI_ION UINT32_C(0x424302fd)
#define RETAINED_NEW_PACK UINT32_C(0x424304fb)
#define RETAINED_UNCERTAIN UINT32_C(0x424308f7)
#define QUALIFY_SAMPLE_MIN_MS 200u
#define QUALIFY_SAMPLE_MAX_MS 10000u
#define QUALIFY_SPREAD_MV 50u

void battery_chemistry_init(battery_chemistry_state_t *state, uint32_t word) {
    memset(state, 0, sizeof(*state));
    if (word == RETAINED_NIMH) state->chemistry = BATTERY_CHEMISTRY_NIMH_2S;
    if (word == RETAINED_LI_ION) state->chemistry = BATTERY_CHEMISTRY_LI_ION_1S;
    state->new_pack = word == RETAINED_NEW_PACK;
}

uint32_t battery_chemistry_retained_word(const battery_chemistry_state_t *state) {
    switch (state->chemistry) {
    case BATTERY_CHEMISTRY_NIMH_2S: return RETAINED_NIMH;
    case BATTERY_CHEMISTRY_LI_ION_1S: return RETAINED_LI_ION;
    case BATTERY_CHEMISTRY_UNKNOWN:
        return state->new_pack ? RETAINED_NEW_PACK : RETAINED_UNCERTAIN;
    }
    return RETAINED_UNCERTAIN;
}

void battery_chemistry_power_lost(battery_chemistry_state_t *state,
                                  bool cold_insertion) {
    battery_chemistry_init(state, cold_insertion ? RETAINED_NEW_PACK
                                               : RETAINED_UNCERTAIN);
}

bool battery_chemistry_observe(battery_chemistry_state_t *state,
                               uint32_t now_ms, uint32_t sample_sequence,
                               bool valid, bool charger_present,
                               uint16_t terminal_mv) {
    if (state->chemistry != BATTERY_CHEMISTRY_UNKNOWN) return false;
    if (!valid || charger_present) {
        state->samples = 0u;
        state->candidate = BATTERY_CHEMISTRY_UNKNOWN;
    }
    if (!valid) return false;
    if (state->sequence_seen && state->last_sequence == sample_sequence) {
        return false;
    }
    state->sequence_seen = true;
    state->last_sequence = sample_sequence;
    /* Even charger-influenced high voltage revokes the NiMH assumption. It
     * cannot identify Li-ion, or become an unplugged sample when re-polled. */
    if (terminal_mv > BATTERY_CHEMISTRY_NIMH_MAX_MV) state->new_pack = false;
    if (charger_present) return false;
    battery_chemistry_t candidate = BATTERY_CHEMISTRY_UNKNOWN;
    if (terminal_mv >= BATTERY_CHEMISTRY_LI_ION_MIN_MV && terminal_mv <= 4300u) {
        candidate = BATTERY_CHEMISTRY_LI_ION_1S;
    } else if (state->new_pack && terminal_mv >= 1000u &&
               terminal_mv <= BATTERY_CHEMISTRY_NIMH_MAX_MV) {
        candidate = BATTERY_CHEMISTRY_NIMH_2S;
    }
    uint32_t elapsed = now_ms - state->last_sample_ms;
    if (candidate != state->candidate || candidate == BATTERY_CHEMISTRY_UNKNOWN ||
        elapsed > QUALIFY_SAMPLE_MAX_MS) {
        state->samples = 0u;
    }
    if (state->samples != 0u && elapsed < QUALIFY_SAMPLE_MIN_MS) return false;
    state->candidate = candidate;
    state->last_sample_ms = now_ms;
    if (candidate == BATTERY_CHEMISTRY_UNKNOWN) return false;
    if (state->samples == 0u) {
        state->minimum_mv = terminal_mv;
        state->maximum_mv = terminal_mv;
    }
    if (terminal_mv < state->minimum_mv) state->minimum_mv = terminal_mv;
    if (terminal_mv > state->maximum_mv) state->maximum_mv = terminal_mv;
    if ((uint16_t)(state->maximum_mv - state->minimum_mv) > QUALIFY_SPREAD_MV) {
        state->minimum_mv = state->maximum_mv = terminal_mv;
        state->samples = 0u;
    }
    if (++state->samples < BATTERY_CHEMISTRY_CONFIRM_SAMPLES) return false;
    state->chemistry = candidate;
    state->new_pack = false;
    return true;
}

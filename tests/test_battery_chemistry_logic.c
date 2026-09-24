#include <assert.h>
#include <stdio.h>

#include "hal/battery_chemistry_logic.h"

static bool qualify(battery_chemistry_state_t *state, uint16_t mv,
                    bool valid, bool charger) {
    bool changed = false;
    for (uint32_t i = 1; i <= 5; i++) {
        changed |= battery_chemistry_observe(state, i * 250u, i,
                                             valid, charger, mv);
    }
    return changed;
}

int main(void) {
    battery_chemistry_state_t state;
    battery_chemistry_init(&state, 0u);
    assert(!qualify(&state, 2900u, true, false));
    battery_chemistry_power_lost(&state, true);
    assert(qualify(&state, 2900u, true, false));
    uint32_t nimh = battery_chemistry_retained_word(&state);
    assert(state.chemistry == BATTERY_CHEMISTRY_NIMH_2S);
    assert(!qualify(&state, 4200u, true, false));
    assert(state.chemistry == BATTERY_CHEMISTRY_NIMH_2S);
    battery_chemistry_init(&state, nimh);
    assert(state.chemistry == BATTERY_CHEMISTRY_NIMH_2S);

    battery_chemistry_power_lost(&state, true);
    uint32_t fresh = battery_chemistry_retained_word(&state);
    battery_chemistry_init(&state, fresh);
    assert(state.new_pack);
    assert(!qualify(&state, 3599u, true, false));
    assert(qualify(&state, 3600u, true, false));
    assert(state.chemistry == BATTERY_CHEMISTRY_LI_ION_1S);
    uint32_t li_ion = battery_chemistry_retained_word(&state);
    battery_chemistry_init(&state, li_ion);
    assert(!qualify(&state, 2200u, true, false));
    assert(state.chemistry == BATTERY_CHEMISTRY_LI_ION_1S);
    assert(!qualify(&state, 0u, false, true));
    assert(state.chemistry == BATTERY_CHEMISTRY_LI_ION_1S);

    for (unsigned bit = 0; bit < 32; bit++) {
        battery_chemistry_init(&state, li_ion ^ (UINT32_C(1) << bit));
        assert(state.chemistry == BATTERY_CHEMISTRY_UNKNOWN);
        assert(!state.new_pack);
        battery_chemistry_init(&state, nimh ^ (UINT32_C(1) << bit));
        assert(state.chemistry == BATTERY_CHEMISTRY_UNKNOWN);
    }
    battery_chemistry_power_lost(&state, true);
    assert(!qualify(&state, 4200u, true, true));
    assert(!qualify(&state, 4200u, false, false));
    assert(!qualify(&state, 4400u, true, false));
    assert(!qualify(&state, 3400u, true, false));
    assert(!qualify(&state, 999u, true, false));
    for (uint32_t i = 0; i < 20; i++) {
        assert(!battery_chemistry_observe(&state, 2000u + i * 250u, 6u,
                                           true, false, 3800u));
    }
    assert(state.samples == 1u);
    battery_chemistry_power_lost(&state, true);
    for (uint32_t i = 1; i <= 20; i++) {
        assert(!battery_chemistry_observe(&state, i * 250u, i, true, false,
                                           i % 2u ? 3800u : 3900u));
    }
    battery_chemistry_power_lost(&state, true);
    for (uint32_t i = 1; i <= 20; i++) {
        assert(!battery_chemistry_observe(&state, i, i, true, false, 3800u));
    }
    assert(state.samples == 1u);
    /* Losing gauge power while RP retention survives is identity uncertainty,
     * not permission to reinterpret a discharged Li-ion pack as NiMH. */
    battery_chemistry_init(&state, li_ion);
    battery_chemistry_power_lost(&state, false);
    assert(state.chemistry == BATTERY_CHEMISTRY_UNKNOWN && !state.new_pack);
    assert(!qualify(&state, 2900u, true, false));
    uint32_t uncertain = battery_chemistry_retained_word(&state);
    assert(uncertain != 0u && uncertain != fresh);
    battery_chemistry_init(&state, uncertain);
    assert(!qualify(&state, 2900u, true, false));
    battery_chemistry_power_lost(&state, false);
    assert(!qualify(&state, 2900u, true, false));
    assert(qualify(&state, 3900u, true, false));
    assert(state.chemistry == BATTERY_CHEMISTRY_LI_ION_1S);
    for (unsigned bit = 0u; bit < 32u; bit++) {
        battery_chemistry_init(&state, uncertain ^ (UINT32_C(1) << bit));
        assert(state.chemistry == BATTERY_CHEMISTRY_UNKNOWN && !state.new_pack);
    }
    battery_chemistry_power_lost(&state, true);
    assert(!qualify(&state, 3400u, true, false));
    assert(!state.new_pack);
    assert(!qualify(&state, 2900u, true, false));
    battery_chemistry_power_lost(&state, true);
    assert(!battery_chemistry_observe(&state, 250u, 1u, true, false, 3800u));
    assert(state.samples == 1u && !state.new_pack);
    battery_chemistry_init(&state, battery_chemistry_retained_word(&state));
    assert(!qualify(&state, 2900u, true, false));
    assert(qualify(&state, 3900u, true, false));

    /* Charger-connected voltage cannot identify a pack, but a high reading
     * must still revoke the NiMH assumption before a later warm restart. */
    battery_chemistry_power_lost(&state, true);
    assert(!battery_chemistry_observe(&state, 250u, 1u, true, true, 3900u));
    assert(!state.new_pack && state.chemistry == BATTERY_CHEMISTRY_UNKNOWN);
    battery_chemistry_init(&state, battery_chemistry_retained_word(&state));
    assert(!qualify(&state, 2900u, true, false));

    /* A sample acquired with VIN present cannot become an identification
     * sample merely by re-polling it after unplug. */
    battery_chemistry_power_lost(&state, true);
    assert(!battery_chemistry_observe(&state, 250u, 1u, true, true, 2700u));
    assert(!battery_chemistry_observe(&state, 500u, 1u, true, false, 2700u));
    assert(state.samples == 0u);
    for (uint32_t i = 2u; i < 6u; i++) {
        assert(!battery_chemistry_observe(&state, i * 250u + 250u,
                                          i, true, false, 2700u));
    }
    assert(battery_chemistry_observe(&state, 1750u, 6u, true, false, 2700u));
    assert(state.chemistry == BATTERY_CHEMISTRY_NIMH_2S);
    puts("Battery chemistry latch tests passed");
    return 0;
}

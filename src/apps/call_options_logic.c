#include "apps/call_options_logic.h"

uint16_t call_options_mask_for_state(bool active, bool held, bool waiting,
                                     bool hold_toggle_available) {
    uint16_t mask;
    if (waiting && active && held) {
        mask = 0x6d8u;
    } else if (waiting && held) {
        mask = 0x666u;
    } else if (waiting) {
        mask = 0x6edu;
    } else if (active && held) {
        mask = 0x698u;
    } else if (held) {
        mask = 0x606u;
    } else {
        mask = 0x685u;
    }

    /* A coarse network hold toggle may share its operation with accepting a
     * waiting call. Nokia's otherwise-faithful Hold/Unhold entries must then
     * be suppressed. The service capability is a second veto for stale or
     * inconsistent app state. */
    if (waiting || !hold_toggle_available) {
        mask &= (uint16_t)~(CALL_OPT_HOLD | CALL_OPT_UNHOLD);
    }
    return mask;
}

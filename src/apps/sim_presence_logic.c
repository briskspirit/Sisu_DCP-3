#include "apps/sim_presence_logic.h"

#include <stddef.h>

void sim_presence_ui_init(sim_presence_ui_state_t *state) {
    if (state == NULL) {
        return;
    }
    *state = (sim_presence_ui_state_t){0};
}

bool sim_presence_ui_update(sim_presence_ui_state_t *state,
                            bool modem_ready,
                            bool sim_checked,
                            bool sim_present,
                            uint32_t now_ms) {
    if (state == NULL) {
        return false;
    }

    if (sim_checked && sim_present) {
        state->absent_candidate = false;
        state->missing = false;
        return false;
    }

    if (!modem_ready || !sim_checked) {
        state->absent_candidate = false;
        return state->missing;
    }

    if (state->missing) {
        return true;
    }
    if (!state->absent_candidate) {
        state->absent_candidate = true;
        state->absent_since_ms = now_ms;
        return false;
    }
    if ((uint32_t)(now_ms - state->absent_since_ms) >=
        SIM_PRESENCE_UI_ABSENT_DWELL_MS) {
        state->absent_candidate = false;
        state->missing = true;
    }
    return state->missing;
}

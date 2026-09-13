#ifndef SIM_PRESENCE_LOGIC_H
#define SIM_PRESENCE_LOGIC_H

#include <stdbool.h>
#include <stdint.h>

#define SIM_PRESENCE_UI_ABSENT_DWELL_MS 3000u

typedef struct {
    bool absent_candidate;
    bool missing;
    uint32_t absent_since_ms;
} sim_presence_ui_state_t;

void sim_presence_ui_init(sim_presence_ui_state_t *state);

/* Telit may report #QSS status 0 while RF is deliberately held at CFUN=4
 * during startup. Presence is accepted immediately, but absence becomes a
 * production UI state only after the modem is ready and the observation has
 * remained stable for the full dwell. */
bool sim_presence_ui_update(sim_presence_ui_state_t *state,
                            bool modem_ready,
                            bool sim_checked,
                            bool sim_present,
                            uint32_t now_ms);

#endif

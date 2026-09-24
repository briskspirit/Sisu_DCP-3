#ifndef BATTERY_CHEMISTRY_LOGIC_H
#define BATTERY_CHEMISTRY_LOGIC_H

#include <stdbool.h>
#include <stdint.h>

/* Rev B insertion contract: a new Li-ion pack is precharged, and the Nokia
 * charger is unplugged during a swap. Voltage cannot identify a depleted cell. */
#define BATTERY_CHEMISTRY_LI_ION_MIN_MV 3600u
#define BATTERY_CHEMISTRY_NIMH_MAX_MV 3200u
#define BATTERY_CHEMISTRY_CONFIRM_SAMPLES 5u

typedef enum {
    BATTERY_CHEMISTRY_UNKNOWN = 0,
    BATTERY_CHEMISTRY_NIMH_2S,
    BATTERY_CHEMISTRY_LI_ION_1S,
} battery_chemistry_t;

typedef struct {
    battery_chemistry_t chemistry;
    battery_chemistry_t candidate;
    uint32_t last_sequence;
    uint32_t last_sample_ms;
    uint16_t minimum_mv;
    uint16_t maximum_mv;
    uint8_t samples;
    bool sequence_seen;
    bool new_pack;
} battery_chemistry_state_t;

/* The word belongs in battery-powered retention, never ordinary flash. A lost
 * or corrupt word is not permission to identify a low-voltage pack as NiMH. */
void battery_chemistry_init(battery_chemistry_state_t *state, uint32_t word);
uint32_t battery_chemistry_retained_word(const battery_chemistry_state_t *state);
/* Gauge POR/UVLO alone is not insertion evidence. The HAL may grant the NiMH
 * fallback only at a fresh RP cold start with empty retention and gauge reset. */
void battery_chemistry_power_lost(battery_chemistry_state_t *state,
                                  bool cold_insertion);
bool battery_chemistry_observe(battery_chemistry_state_t *state,
                               uint32_t now_ms, uint32_t sample_sequence,
                               bool valid, bool charger_present,
                               uint16_t terminal_mv);

#endif

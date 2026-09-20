#ifndef BATTERY_HAL_H
#define BATTERY_HAL_H

#include <stdbool.h>
#include <stdint.h>

#include "hal/bq25171_stat.h"
#include "hal/battery_gauge_logic.h"
#include "hal/battery_runtime_logic.h"

/* Rev B2 battery / charger HAL. Board-1 P1-C validation qualified LTC2959
 * voltage as the user-facing source; builds may still disable that policy
 * explicitly for diagnostic isolation. Callers must test valid() before
 * interpreting it because gauge startup or an I2C fault can still make a
 * sample unavailable. Charger status comes from BQ25171 STAT1/STAT2 via
 * TCA8418; service VBUS is unrelated to charging. */

/* Power-on voltage gate (ROM single-tier, EM init 0x27dd30/0x27d5fc): below the
 * floor a flat pack with no charger silently refuses to boot; a charger always
 * admits recovery; a marginal-but-bootable pack boots and normal post-boot
 * low/empty supervision takes over. Rev B2 qualifies the terminal evidence
 * through the bounded LTC window documented below. */
typedef enum {
    BATTERY_POWER_ON_OK = 0,   /* >= VBAT_POWERON_MIN_MV, or charger present */
    BATTERY_POWER_ON_REFUSE,   /* < VBAT_POWERON_MIN_MV, no charger: silent hard refuse */
    BATTERY_POWER_ON_PENDING,  /* bounded startup qualification still acquiring */
} battery_power_on_gate_t;

void battery_hal_init(void);
/* Select acquisition policy for work the RP is already awake to perform. This
 * changes no standby deadline and is therefore not a wake source. */
void battery_hal_set_sample_mode(battery_sample_mode_t mode);
void battery_hal_poll(uint32_t now_ms);
/* False while a charger edge still needs the bounded VIN qualification. Once
 * the transition is published, the app supplies its animation deadline to the
 * powered-on dormant controller instead of holding the RP awake continuously. */
bool battery_hal_standby_ready(void);
bool battery_hal_valid(void);
uint16_t battery_hal_millivolts(void);
uint16_t battery_hal_millivolts_raw(void);
uint16_t battery_hal_millivolts_fresh(void);
uint32_t battery_hal_sample_sequence(void);
uint8_t battery_hal_level_bars(void);
uint8_t battery_hal_level_bars_from_mv(uint16_t mv);  /* same mapping, explicit mV */
bool battery_hal_low(void);
bool battery_hal_empty(void);
int16_t battery_hal_load_correction_mv(void);
int32_t battery_hal_window_current_ua(void);
bool battery_hal_window_current_valid(void);
uint8_t battery_hal_power_on_sample_count(void);
uint8_t battery_hal_power_on_attempt_count(void);
uint16_t battery_hal_power_on_average_mv(void);
bool battery_hal_charger_connected(void);
bool battery_hal_charger_present(void);
/* Fresh, un-debounced VIN_CHRG_DET sample for a conservative dormant-entry
 * veto. Also advances the normal charger-presence filter. A positive sample
 * must block sleep immediately; the UI still uses the debounced state. */
bool battery_hal_charger_input_present_now(uint32_t now_ms);
uint16_t battery_hal_charger_adc_raw(void);
uint16_t battery_hal_charger_pin_mv(void);
uint32_t battery_hal_charger_input_mv(void);
battery_charge_status_t battery_hal_charge_status(void);
bool battery_hal_charge_status_valid(void);
/* Last atomic raw BQ STAT pair. Outputs retain the last good levels when the
 * current observation is invalid; the return value reports current validity. */
bool battery_hal_charge_status_levels(bool *stat1_high, bool *stat2_high);

/* Production gate uses the rolling five-sample terminal-voltage qualifier.
 * Charger presence always admits recovery. The explicit-mV helper remains for
 * diagnostics that intentionally override the real gauge. */
battery_power_on_gate_t battery_hal_power_on_gate(bool charger);
battery_power_on_gate_t battery_hal_power_on_gate_from_mv(uint16_t mv, bool charger);

/* Decode BQ25171 STAT1/STAT2 (true = high/off, false = low/on) per Table 7-4.
 * Pure; shared by battery_hal and the board diagnostics so the two never drift.
 * STAT alone cannot distinguish full-while-plugged from unplugged (both read
 * (1,1) = IDLE) -- that distinction needs a separate charger-presence sense. */
battery_charge_status_t battery_hal_charge_status_decode(bool stat1_high, bool stat2_high);
battery_charger_state_t battery_hal_charger_state_from_inputs(
    bool charger_present,
    battery_charge_status_t status);

#endif

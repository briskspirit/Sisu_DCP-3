#ifndef CHARGER_INPUT_LOGIC_H
#define CHARGER_INPUT_LOGIC_H

#include <stdbool.h>
#include <stdint.h>

/* Rev B2 VIN_CHRG_DET: 1.33 MOhm high side, 100 kOhm low side.
 * The resulting charger-input ratio is (1330 + 100) / 100 = 14.3. */
#define CHARGER_INPUT_DIVIDER_TOP_KOHM 1330u
#define CHARGER_INPUT_DIVIDER_BOTTOM_KOHM 100u
#define CHARGER_INPUT_ADC_REF_MV 3300u
#define CHARGER_INPUT_ADC_FULL_SCALE 4095u

/* [BP] Conservative first-board thresholds. A nominal Nokia charger is well
 * above attach; the wide gap rejects ADC noise and connector discharge tails. */
#define CHARGER_INPUT_ATTACH_MV 2500u
#define CHARGER_INPUT_DETACH_MV 1800u
#define CHARGER_INPUT_DEBOUNCE_MS 80u

typedef struct {
    bool stable_present;
    bool candidate_present;
    bool candidate_active;
    uint32_t candidate_since_ms;
} charger_input_filter_t;

/* Convert a 12-bit GP43 sample to charger-connector millivolts. Values above
 * the ADC range saturate, and the result spans 0..47190 mV. */
uint32_t charger_input_mv_from_adc_raw(uint16_t raw);
uint16_t charger_input_pin_mv_from_adc_raw(uint16_t raw);

void charger_input_filter_init(charger_input_filter_t *filter,
                               bool initial_present);
bool charger_input_filter_update(charger_input_filter_t *filter,
                                 uint32_t input_mv,
                                 uint32_t now_ms,
                                 bool *changed);
/* True only during the debounce window after a previously-present input has
 * fallen below the detach threshold. A charger status transition to idle in
 * this window is removal evidence, not charge-complete evidence. */
bool charger_input_filter_detach_pending(
    const charger_input_filter_t *filter);

#endif

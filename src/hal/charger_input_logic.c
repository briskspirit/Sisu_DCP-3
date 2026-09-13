#include "hal/charger_input_logic.h"

#include <stddef.h>

static uint16_t clamp_adc_raw(uint16_t raw) {
    return raw > CHARGER_INPUT_ADC_FULL_SCALE
        ? CHARGER_INPUT_ADC_FULL_SCALE
        : raw;
}

uint16_t charger_input_pin_mv_from_adc_raw(uint16_t raw) {
    uint32_t clamped = clamp_adc_raw(raw);
    uint32_t numerator =
        clamped * CHARGER_INPUT_ADC_REF_MV +
        (CHARGER_INPUT_ADC_FULL_SCALE / 2u);
    return (uint16_t)(numerator / CHARGER_INPUT_ADC_FULL_SCALE);
}

uint32_t charger_input_mv_from_adc_raw(uint16_t raw) {
    uint32_t clamped = clamp_adc_raw(raw);
    uint32_t divider_sum =
        CHARGER_INPUT_DIVIDER_TOP_KOHM +
        CHARGER_INPUT_DIVIDER_BOTTOM_KOHM;
    uint64_t numerator =
        (uint64_t)clamped * CHARGER_INPUT_ADC_REF_MV * divider_sum;
    uint32_t denominator =
        CHARGER_INPUT_ADC_FULL_SCALE * CHARGER_INPUT_DIVIDER_BOTTOM_KOHM;
    return (uint32_t)((numerator + (denominator / 2u)) / denominator);
}

void charger_input_filter_init(charger_input_filter_t *filter,
                               bool initial_present) {
    if (filter == NULL) {
        return;
    }
    filter->stable_present = initial_present;
    filter->candidate_present = initial_present;
    filter->candidate_active = false;
    filter->candidate_since_ms = 0u;
}

bool charger_input_filter_update(charger_input_filter_t *filter,
                                 uint32_t input_mv,
                                 uint32_t now_ms,
                                 bool *changed) {
    if (changed != NULL) {
        *changed = false;
    }
    if (filter == NULL) {
        return false;
    }

    bool requested = filter->stable_present;
    if (!filter->stable_present && input_mv >= CHARGER_INPUT_ATTACH_MV) {
        requested = true;
    } else if (filter->stable_present &&
               input_mv <= CHARGER_INPUT_DETACH_MV) {
        requested = false;
    }

    if (requested == filter->stable_present) {
        filter->candidate_active = false;
        filter->candidate_present = filter->stable_present;
        return filter->stable_present;
    }

    if (!filter->candidate_active ||
        filter->candidate_present != requested) {
        filter->candidate_active = true;
        filter->candidate_present = requested;
        filter->candidate_since_ms = now_ms;
        return filter->stable_present;
    }

    if ((uint32_t)(now_ms - filter->candidate_since_ms) >=
        CHARGER_INPUT_DEBOUNCE_MS) {
        filter->stable_present = requested;
        filter->candidate_active = false;
        if (changed != NULL) {
            *changed = true;
        }
    }
    return filter->stable_present;
}

bool charger_input_filter_detach_pending(
    const charger_input_filter_t *filter) {
    return filter != NULL &&
           filter->stable_present &&
           filter->candidate_active &&
           !filter->candidate_present;
}

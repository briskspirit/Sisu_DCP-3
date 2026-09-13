#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "hal/charger_input_logic.h"

static void test_conversion(void) {
    assert(charger_input_pin_mv_from_adc_raw(0u) == 0u);
    assert(charger_input_mv_from_adc_raw(0u) == 0u);
    assert(charger_input_pin_mv_from_adc_raw(4095u) == 3300u);
    assert(charger_input_mv_from_adc_raw(4095u) == 47190u);
    assert(charger_input_mv_from_adc_raw(UINT16_MAX) == 47190u);

    /* A 5 V input lands near 350 mV at GP43. Permit one ADC LSB of rounding. */
    uint32_t five_v = charger_input_mv_from_adc_raw(434u);
    assert(five_v >= 4990u && five_v <= 5010u);
}

static void test_hysteresis_and_debounce(void) {
    charger_input_filter_t filter;
    bool changed = true;
    charger_input_filter_init(&filter, false);

    assert(!charger_input_filter_update(
        &filter, CHARGER_INPUT_ATTACH_MV - 1u, 10u, &changed));
    assert(!changed);
    assert(!charger_input_filter_update(
        &filter, CHARGER_INPUT_ATTACH_MV, 20u, &changed));
    assert(!changed);
    assert(!charger_input_filter_update(
        &filter, CHARGER_INPUT_ATTACH_MV, 99u, &changed));
    assert(!changed);
    assert(charger_input_filter_update(
        &filter, CHARGER_INPUT_ATTACH_MV, 100u, &changed));
    assert(changed);

    /* The hysteresis band cannot start a detach. */
    assert(charger_input_filter_update(
        &filter, CHARGER_INPUT_DETACH_MV + 1u, 110u, &changed));
    assert(!changed);

    /* A noisy excursion resets the candidate timer. */
    assert(charger_input_filter_update(
        &filter, CHARGER_INPUT_DETACH_MV, 120u, &changed));
    assert(charger_input_filter_detach_pending(&filter));
    assert(charger_input_filter_update(
        &filter, CHARGER_INPUT_DETACH_MV + 1u, 170u, &changed));
    assert(!charger_input_filter_detach_pending(&filter));
    assert(charger_input_filter_update(
        &filter, CHARGER_INPUT_DETACH_MV, 180u, &changed));
    assert(charger_input_filter_detach_pending(&filter));
    assert(charger_input_filter_update(
        &filter, CHARGER_INPUT_DETACH_MV, 259u, &changed));
    assert(charger_input_filter_detach_pending(&filter));
    assert(!changed);
    assert(!charger_input_filter_update(
        &filter, CHARGER_INPUT_DETACH_MV, 260u, &changed));
    assert(changed);
    assert(!charger_input_filter_detach_pending(&filter));
}

static void test_wrap_safe_debounce(void) {
    charger_input_filter_t filter;
    bool changed = false;
    charger_input_filter_init(&filter, false);

    uint32_t start = UINT32_MAX - 39u;
    assert(!charger_input_filter_update(
        &filter, CHARGER_INPUT_ATTACH_MV, start, &changed));
    assert(charger_input_filter_update(
        &filter, CHARGER_INPUT_ATTACH_MV, 40u, &changed));
    assert(changed);
}

int main(void) {
    test_conversion();
    test_hysteresis_and_debounce();
    test_wrap_safe_debounce();
    puts("charger input logic tests passed");
    return 0;
}

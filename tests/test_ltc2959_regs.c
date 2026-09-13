#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "hal/ltc2959_regs.h"

static void test_controls(void) {
    uint8_t sleep = ltc2959_adc_control(LTC2959_ACQUIRE_SLEEP);
    uint8_t smart = ltc2959_adc_control(LTC2959_ACQUIRE_SMART_SLEEP);
    uint8_t shot = ltc2959_adc_control(LTC2959_ACQUIRE_SINGLE_SHOT);
    assert(sleep == 0x00u);
    assert(smart == 0x20u);
    assert(shot == 0xa0u);
    assert(ltc2959_adc_control_uses_vdd(sleep));
    assert(ltc2959_adc_control_uses_vdd(smart));
    assert(ltc2959_adc_control_uses_vdd(shot));
    assert(!ltc2959_adc_control_uses_vdd(
        (uint8_t)(shot | LTC2959_ADC_VOLTAGE_SENSEN)));

    assert(ltc2959_coulomb_control(
               LTC2959_DEADBAND_20_UV, true) == 0x50u);
    assert(ltc2959_coulomb_control(
               LTC2959_DEADBAND_40_UV, true) == 0x90u);
    assert(ltc2959_coulomb_control(
               LTC2959_DEADBAND_80_UV, false) == 0xd8u);
}

static void test_alert_tracker_retries_ara_after_status_clears(void) {
    ltc2959_alert_tracker_t tracker;
    ltc2959_alert_tracker_init(&tracker);
    assert(!ltc2959_alert_tracker_needs_ara(&tracker));

    ltc2959_alert_tracker_observe_status(
        &tracker, LTC2959_STATUS_VOLTAGE_ALERT);
    assert(ltc2959_alert_tracker_needs_ara(&tracker));
    ltc2959_alert_tracker_note_ara(&tracker, false);
    assert(ltc2959_alert_tracker_needs_ara(&tracker));

    /* The next clear-on-read STATUS value is zero; the failed ARA obligation
     * must survive until a later ARA succeeds. */
    ltc2959_alert_tracker_observe_status(&tracker, 0u);
    assert(ltc2959_alert_tracker_needs_ara(&tracker));
    ltc2959_alert_tracker_note_ara(&tracker, true);
    assert(!ltc2959_alert_tracker_needs_ara(&tracker));

    ltc2959_alert_tracker_observe_status(
        &tracker, LTC2959_STATUS_CURRENT_ALERT);
    ltc2959_alert_tracker_force_released(&tracker);
    assert(!ltc2959_alert_tracker_needs_ara(&tracker));
}

static void test_ara_response_address_validation(void) {
    assert(ltc2959_ara_response_matches(0xc6u));
    assert(ltc2959_ara_response_matches(0xc7u));
    assert(!ltc2959_ara_response_matches(0xc4u));
    assert(!ltc2959_ara_response_matches(0xc8u));
}

static void test_decode_and_byte_order(void) {
    uint8_t bytes[LTC2959_SNAPSHOT_LEN];
    memset(bytes, 0, sizeof(bytes));
    bytes[LTC2959_REG_STATUS] = 0xa5u;
    bytes[LTC2959_REG_ADC_CONTROL] = 0x20u;
    bytes[LTC2959_REG_COULOMB_CONTROL] = 0x50u;
    bytes[LTC2959_REG_ACR_MSB + 0u] = 0x12u;
    bytes[LTC2959_REG_ACR_MSB + 1u] = 0x34u;
    bytes[LTC2959_REG_ACR_MSB + 2u] = 0x56u;
    bytes[LTC2959_REG_ACR_MSB + 3u] = 0x78u;
    bytes[LTC2959_REG_VOLTAGE_MSB + 0u] = 0x9au;
    bytes[LTC2959_REG_VOLTAGE_MSB + 1u] = 0xbcu;
    bytes[LTC2959_REG_CURRENT_MSB + 0u] = 0xfeu;
    bytes[LTC2959_REG_CURRENT_MSB + 1u] = 0xd4u;
    bytes[LTC2959_REG_TEMPERATURE_MSB + 0u] = 0x5cu;
    bytes[LTC2959_REG_TEMPERATURE_MSB + 1u] = 0x81u;

    ltc2959_raw_snapshot_t snap;
    assert(ltc2959_decode_snapshot(bytes, sizeof(bytes), &snap));
    assert(snap.status == 0xa5u);
    assert(snap.adc_control == 0x20u);
    assert(snap.coulomb_control == 0x50u);
    assert(snap.acr_raw == 0x12345678u);
    assert(snap.voltage_raw == 0x9abcu);
    assert(snap.current_raw == -300);
    assert(snap.temperature_raw == 0x5c81u);
    assert(!ltc2959_decode_snapshot(
        bytes, sizeof(bytes) - 1u, &snap));
    assert(!ltc2959_decode_snapshot(NULL, sizeof(bytes), &snap));
}

static void test_conversions(void) {
    assert(ltc2959_voltage_mv(0u) == 0u);
    assert(ltc2959_voltage_mv(UINT16_MAX) == 62599u);
    uint16_t raw_2500 = ltc2959_voltage_raw_from_mv(2500u);
    assert(ltc2959_voltage_mv(raw_2500) >= 2499u);
    assert(ltc2959_voltage_mv(raw_2500) <= 2501u);
    assert(ltc2959_voltage_raw_from_mv(62600u) == UINT16_MAX);
    assert(ltc2959_voltage_raw_from_mv(UINT32_MAX) == UINT16_MAX);

    assert(ltc2959_current_ua(
               0, LTC2959_RAW_POSITIVE_IS_CHARGING) == 0);
    assert(ltc2959_current_ua(
               1, LTC2959_RAW_POSITIVE_IS_CHARGING) == 298);
    assert(ltc2959_current_ua(
               -1, LTC2959_RAW_POSITIVE_IS_CHARGING) == -298);
    assert(ltc2959_current_ua(
               INT16_MAX, LTC2959_RAW_POSITIVE_IS_CHARGING) ==
           9748183);
    assert(ltc2959_current_ua(
               INT16_MIN, LTC2959_RAW_POSITIVE_IS_CHARGING) ==
           -9748480);
    assert(ltc2959_current_ua(
               100, LTC2959_RAW_NEGATIVE_IS_CHARGING) == -29750);

    assert(ltc2959_temperature_mdegc(0u) == -273150);
    assert(ltc2959_temperature_mdegc(UINT16_MAX) == 551837);
}

static void test_acr(void) {
    assert(LTC2959_ACR_DATASHEET_LSB_NAH == 2665u);
    assert(LTC2959_ACR_SCALE_REVISION == 2u);
    assert(ltc2959_acr_nah(1u) == 3825u);
    assert(ltc2959_acr_nah(UINT32_MAX) ==
           (uint64_t)UINT32_MAX * 3825u);
    assert(ltc2959_acr_delta(100u, 90u) == 10);
    assert(ltc2959_acr_delta(2u, UINT32_MAX - 2u) == 5);
    assert(ltc2959_acr_delta(UINT32_MAX - 2u, 2u) == -5);
    assert(ltc2959_acr_delta(0x80000000u, 0u) == INT32_MIN);
    assert(ltc2959_acr_delta_nah(100u, 90u) == 38250);
}

int main(void) {
    test_controls();
    test_alert_tracker_retries_ara_after_status_clears();
    test_ara_response_address_validation();
    test_decode_and_byte_order();
    test_conversions();
    test_acr();
    puts("LTC2959 register tests passed");
    return 0;
}

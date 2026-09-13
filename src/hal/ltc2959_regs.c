#include "hal/ltc2959_regs.h"

#include <limits.h>

void ltc2959_alert_tracker_init(ltc2959_alert_tracker_t *tracker) {
    if (tracker != NULL) {
        tracker->ara_pending = false;
    }
}

void ltc2959_alert_tracker_observe_status(
    ltc2959_alert_tracker_t *tracker,
    uint8_t status) {
    if (tracker != NULL && status != 0u) {
        tracker->ara_pending = true;
    }
}

bool ltc2959_alert_tracker_needs_ara(
    const ltc2959_alert_tracker_t *tracker) {
    return tracker != NULL && tracker->ara_pending;
}

void ltc2959_alert_tracker_note_ara(
    ltc2959_alert_tracker_t *tracker,
    bool succeeded) {
    if (tracker != NULL && succeeded) {
        tracker->ara_pending = false;
    }
}

void ltc2959_alert_tracker_force_released(
    ltc2959_alert_tracker_t *tracker) {
    if (tracker != NULL) {
        tracker->ara_pending = false;
    }
}

bool ltc2959_ara_response_matches(uint8_t response) {
    /* SMBus defines the low response bit as don't-care. The LTC2959 data sheet
     * illustrates it high, while the Rev B2 bench device returns it low. */
    return (response & 0xfeu) == LTC2959_ARA_RESPONSE_ADDRESS_BITS;
}

uint8_t ltc2959_adc_control(ltc2959_acquire_mode_t mode) {
    uint8_t mode_bits;
    switch (mode) {
    case LTC2959_ACQUIRE_SMART_SLEEP:
        mode_bits = LTC2959_ADC_MODE_SMART_SLEEP_BITS;
        break;
    case LTC2959_ACQUIRE_SINGLE_SHOT:
        mode_bits = LTC2959_ADC_MODE_SINGLE_SHOT_BITS;
        break;
    case LTC2959_ACQUIRE_SLEEP:
    default:
        mode_bits = LTC2959_ADC_MODE_SLEEP_BITS;
        break;
    }
    /* GPIO alert is open-drain. B[2]=0 selects VDD, which is VBATT on Rev B2. */
    return (uint8_t)(mode_bits | LTC2959_ADC_GPIO_ALERT_BITS);
}

uint8_t ltc2959_coulomb_control(ltc2959_deadband_t deadband,
                                bool count_enabled) {
    uint8_t deadband_bits;
    switch (deadband) {
    case LTC2959_DEADBAND_20_UV:
        deadband_bits = LTC2959_CC_DEADBAND_20UV_BITS;
        break;
    case LTC2959_DEADBAND_40_UV:
        deadband_bits = LTC2959_CC_DEADBAND_40UV_BITS;
        break;
    case LTC2959_DEADBAND_80_UV:
        deadband_bits = LTC2959_CC_DEADBAND_80UV_BITS;
        break;
    case LTC2959_DEADBAND_NONE:
    default:
        deadband_bits = LTC2959_CC_DEADBAND_NONE_BITS;
        break;
    }
    return (uint8_t)(
        deadband_bits |
        LTC2959_CC_RESERVED_DEFAULT_BITS |
        (count_enabled ? 0u : LTC2959_CC_DO_NOT_COUNT));
}

bool ltc2959_adc_control_uses_vdd(uint8_t control) {
    return (control & LTC2959_ADC_VOLTAGE_SENSEN) == 0u;
}

uint16_t ltc2959_decode_u16_be(const uint8_t bytes[2]) {
    return (uint16_t)(((uint16_t)bytes[0] << 8) | bytes[1]);
}

int16_t ltc2959_decode_i16_be(const uint8_t bytes[2]) {
    uint16_t raw = ltc2959_decode_u16_be(bytes);
    if ((raw & 0x8000u) == 0u) {
        return (int16_t)raw;
    }
    int32_t magnitude = 0x10000 - (int32_t)raw;
    return (int16_t)(-magnitude);
}

uint32_t ltc2959_decode_u32_be(const uint8_t bytes[4]) {
    return ((uint32_t)bytes[0] << 24) |
           ((uint32_t)bytes[1] << 16) |
           ((uint32_t)bytes[2] << 8) |
           bytes[3];
}

bool ltc2959_decode_snapshot(const uint8_t *bytes,
                             size_t length,
                             ltc2959_raw_snapshot_t *out) {
    if (bytes == NULL || out == NULL || length < LTC2959_SNAPSHOT_LEN) {
        return false;
    }
    out->status = bytes[LTC2959_REG_STATUS];
    out->adc_control = bytes[LTC2959_REG_ADC_CONTROL];
    out->coulomb_control = bytes[LTC2959_REG_COULOMB_CONTROL];
    out->acr_raw = ltc2959_decode_u32_be(&bytes[LTC2959_REG_ACR_MSB]);
    out->voltage_raw =
        ltc2959_decode_u16_be(&bytes[LTC2959_REG_VOLTAGE_MSB]);
    out->current_raw =
        ltc2959_decode_i16_be(&bytes[LTC2959_REG_CURRENT_MSB]);
    out->temperature_raw =
        ltc2959_decode_u16_be(&bytes[LTC2959_REG_TEMPERATURE_MSB]);
    return true;
}

uint16_t ltc2959_voltage_mv(uint16_t raw) {
    uint64_t numerator = (uint64_t)raw * 62600u + 32768u;
    return (uint16_t)(numerator / 65536u);
}

uint16_t ltc2959_voltage_raw_from_mv(uint32_t mv) {
    if (mv >= 62600u) {
        return UINT16_MAX;
    }
    uint64_t numerator = (uint64_t)mv * 65536u + 31300u;
    return (uint16_t)(numerator / 62600u);
}

int32_t ltc2959_current_ua(int16_t raw,
                          ltc2959_current_polarity_t polarity) {
    /* 2.975 uV per code across 10 mOhm = 297.5 uA per code. */
    int64_t scaled = (int64_t)raw * 2975;
    scaled += scaled >= 0 ? 5 : -5;
    int32_t ua = (int32_t)(scaled / 10);
    return polarity == LTC2959_RAW_NEGATIVE_IS_CHARGING ? -ua : ua;
}

int32_t ltc2959_temperature_mdegc(uint16_t raw) {
    int64_t kelvin_milli =
        ((int64_t)raw * 825000 + 32768) / 65536;
    return (int32_t)(kelvin_milli - 273150);
}

uint64_t ltc2959_acr_nah(uint32_t raw) {
    return (uint64_t)raw * LTC2959_ACR_LSB_NAH;
}

int32_t ltc2959_acr_delta(uint32_t newer, uint32_t older) {
    uint32_t modular = newer - older;
    if (modular <= INT32_MAX) {
        return (int32_t)modular;
    }
    uint32_t magnitude = UINT32_MAX - modular + 1u;
    if (magnitude == 0x80000000u) {
        return INT32_MIN;
    }
    return -(int32_t)magnitude;
}

int64_t ltc2959_acr_delta_nah(uint32_t newer, uint32_t older) {
    return (int64_t)ltc2959_acr_delta(newer, older) *
           LTC2959_ACR_LSB_NAH;
}

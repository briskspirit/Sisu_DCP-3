#ifndef LTC2959_REGS_H
#define LTC2959_REGS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LTC2959_I2C_ADDR 0x63u
#define LTC2959_ARA_I2C_ADDR 0x0cu
#define LTC2959_ARA_RESPONSE_ADDRESS_BITS 0xc6u

typedef enum {
    LTC2959_REG_STATUS = 0x00u,
    LTC2959_REG_ADC_CONTROL = 0x01u,
    LTC2959_REG_COULOMB_CONTROL = 0x02u,
    LTC2959_REG_ACR_MSB = 0x03u,
    LTC2959_REG_CHARGE_THRESHOLD_LOW = 0x07u,
    LTC2959_REG_CHARGE_THRESHOLD_HIGH = 0x0bu,
    LTC2959_REG_VOLTAGE_MSB = 0x0fu,
    LTC2959_REG_VOLTAGE_THRESHOLD_HIGH = 0x11u,
    LTC2959_REG_VOLTAGE_THRESHOLD_LOW = 0x13u,
    LTC2959_REG_MAX_VOLTAGE = 0x15u,
    LTC2959_REG_MIN_VOLTAGE = 0x17u,
    LTC2959_REG_CURRENT_MSB = 0x19u,
    LTC2959_REG_CURRENT_THRESHOLD_HIGH = 0x1bu,
    LTC2959_REG_CURRENT_THRESHOLD_LOW = 0x1du,
    LTC2959_REG_MAX_CURRENT = 0x1fu,
    LTC2959_REG_MIN_CURRENT = 0x21u,
    LTC2959_REG_TEMPERATURE_MSB = 0x23u,
    LTC2959_REG_TEMPERATURE_THRESHOLD_HIGH = 0x25u,
    LTC2959_REG_TEMPERATURE_THRESHOLD_LOW = 0x27u,
    LTC2959_REG_GPIO_MSB = 0x29u,
    LTC2959_REG_GPIO_THRESHOLD_HIGH = 0x2bu,
    LTC2959_REG_GPIO_THRESHOLD_LOW = 0x2du,
    LTC2959_REG_LAST = 0x2eu,
} ltc2959_reg_t;

#define LTC2959_STATUS_GPIO_ALERT (1u << 7)
#define LTC2959_STATUS_CURRENT_ALERT (1u << 6)
#define LTC2959_STATUS_CHARGE_OVERFLOW (1u << 5)
#define LTC2959_STATUS_TEMPERATURE_ALERT (1u << 4)
#define LTC2959_STATUS_CHARGE_HIGH (1u << 3)
#define LTC2959_STATUS_CHARGE_LOW (1u << 2)
#define LTC2959_STATUS_VOLTAGE_ALERT (1u << 1)
#define LTC2959_STATUS_UVLO (1u << 0)

#define LTC2959_ADC_MODE_MASK 0xe0u
#define LTC2959_ADC_MODE_SLEEP_BITS 0x00u
#define LTC2959_ADC_MODE_SMART_SLEEP_BITS 0x20u
#define LTC2959_ADC_MODE_SINGLE_SHOT_BITS 0xa0u
#define LTC2959_ADC_GPIO_MASK 0x18u
#define LTC2959_ADC_GPIO_ALERT_BITS 0x00u
#define LTC2959_ADC_GPIO_CHARGE_COMPLETE_BITS 0x08u
#define LTC2959_ADC_GPIO_ANALOG_NARROW_BITS 0x10u
#define LTC2959_ADC_GPIO_ANALOG_WIDE_BITS 0x18u
#define LTC2959_ADC_VOLTAGE_SENSEN (1u << 2)
#define LTC2959_ADC_RESERVED_MASK 0x03u
#define LTC2959_ADC_POR_DEFAULT 0x18u

#define LTC2959_CC_DEADBAND_MASK 0xc0u
#define LTC2959_CC_DEADBAND_NONE_BITS 0x00u
#define LTC2959_CC_DEADBAND_20UV_BITS 0x40u
#define LTC2959_CC_DEADBAND_40UV_BITS 0x80u
#define LTC2959_CC_DEADBAND_80UV_BITS 0xc0u
#define LTC2959_CC_RESERVED_DEFAULT_BITS 0x10u
#define LTC2959_CC_DO_NOT_COUNT (1u << 3)
#define LTC2959_CC_POR_DEFAULT 0x50u

#define LTC2959_ACR_POR_DEFAULT 0x80000000u
#define LTC2959_RSENSE_UOHM 10000u
/* Rev B2 calibration revision 2. The Rev. A data-sheet value scales to
 * 2665 nAh/count at 10 mOhm, but two independent LTC2959 implementations and
 * a controlled DUT capture observe only about 0.697 of current-time charge at
 * that scale. 3825 nAh/count makes the ACR agree with the independently
 * scaled current ADC to within 1%; see docs/ltc2959_acr_calibration.md. */
#define LTC2959_ACR_DATASHEET_LSB_NAH 2665u
#define LTC2959_ACR_LSB_NAH 3825u
#define LTC2959_ACR_SCALE_REVISION 2u
#define LTC2959_SNAPSHOT_FIRST_REG LTC2959_REG_STATUS
#define LTC2959_SNAPSHOT_LAST_REG LTC2959_REG_TEMPERATURE_MSB + 1u
#define LTC2959_SNAPSHOT_LEN \
    (LTC2959_SNAPSHOT_LAST_REG - LTC2959_SNAPSHOT_FIRST_REG + 1u)

typedef enum {
    LTC2959_ACQUIRE_SLEEP = 0,
    LTC2959_ACQUIRE_SMART_SLEEP,
    LTC2959_ACQUIRE_SINGLE_SHOT,
} ltc2959_acquire_mode_t;

typedef enum {
    LTC2959_RAW_POSITIVE_IS_CHARGING = 0,
    LTC2959_RAW_NEGATIVE_IS_CHARGING,
} ltc2959_current_polarity_t;

typedef enum {
    LTC2959_DEADBAND_NONE = 0,
    LTC2959_DEADBAND_20_UV,
    LTC2959_DEADBAND_40_UV,
    LTC2959_DEADBAND_80_UV,
} ltc2959_deadband_t;

typedef struct {
    uint8_t status;
    uint8_t adc_control;
    uint8_t coulomb_control;
    uint32_t acr_raw;
    uint16_t voltage_raw;
    int16_t current_raw;
    uint16_t temperature_raw;
} ltc2959_raw_snapshot_t;

/* STATUS is clear-on-read, while the GPIO alert pull-down is released only by
 * a successful SMBus ARA. Track that protocol obligation across later zero
 * STATUS reads so a transient ARA failure cannot strand the shared IRQ low. */
typedef struct {
    bool ara_pending;
} ltc2959_alert_tracker_t;

void ltc2959_alert_tracker_init(ltc2959_alert_tracker_t *tracker);
void ltc2959_alert_tracker_observe_status(
    ltc2959_alert_tracker_t *tracker,
    uint8_t status);
bool ltc2959_alert_tracker_needs_ara(
    const ltc2959_alert_tracker_t *tracker);
void ltc2959_alert_tracker_note_ara(
    ltc2959_alert_tracker_t *tracker,
    bool succeeded);
void ltc2959_alert_tracker_force_released(
    ltc2959_alert_tracker_t *tracker);
bool ltc2959_ara_response_matches(uint8_t response);

uint8_t ltc2959_adc_control(ltc2959_acquire_mode_t mode);
uint8_t ltc2959_coulomb_control(ltc2959_deadband_t deadband,
                                bool count_enabled);
bool ltc2959_adc_control_uses_vdd(uint8_t control);

uint16_t ltc2959_decode_u16_be(const uint8_t bytes[2]);
int16_t ltc2959_decode_i16_be(const uint8_t bytes[2]);
uint32_t ltc2959_decode_u32_be(const uint8_t bytes[4]);
bool ltc2959_decode_snapshot(const uint8_t *bytes,
                             size_t length,
                             ltc2959_raw_snapshot_t *out);

uint16_t ltc2959_voltage_mv(uint16_t raw);
uint16_t ltc2959_voltage_raw_from_mv(uint32_t mv);
int32_t ltc2959_current_ua(int16_t raw,
                          ltc2959_current_polarity_t polarity);
int32_t ltc2959_temperature_mdegc(uint16_t raw);
uint64_t ltc2959_acr_nah(uint32_t raw);
int32_t ltc2959_acr_delta(uint32_t newer, uint32_t older);
int64_t ltc2959_acr_delta_nah(uint32_t newer, uint32_t older);

#endif

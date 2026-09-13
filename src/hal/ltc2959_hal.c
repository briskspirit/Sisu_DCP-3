#include "hal/ltc2959_hal.h"

#include <string.h>

#include "hardware/i2c.h"
#include "hal/board.h"
#include "services/log.h"
#include "services/timebase.h"

#define LTC2959_I2C_FAIL_LIMIT 8u
#define LTC2959_SAMPLE_FAIL_LIMIT 3u
#define LTC2959_REINIT_MS 1000u
#define LTC2959_CONVERSION_WAIT_MS 3u
#define LTC2959_SETTLED_SAMPLES 2u
#define LTC2959_SETTLE_DELTA_MV 300u
#define LTC2959_REPLACEMENT_JUMP_MV 600u
#define LTC2959_PLAUSIBLE_MIN_MV 1000u
#define LTC2959_PLAUSIBLE_MAX_MV 4000u

typedef enum {
    LTC2959_STATE_DOWN = 0,
    LTC2959_STATE_READY,
    LTC2959_STATE_CONVERTING,
} ltc2959_state_t;

static ltc2959_snapshot_t s_snapshot;
static ltc2959_state_t s_state;
static uint8_t s_consecutive_failures;
static uint8_t s_consecutive_sample_failures;
static uint8_t s_settled_count;
static uint16_t s_previous_voltage_mv;
static uint32_t s_conversion_started_ms;
static uint32_t s_next_sample_ms;
static uint32_t s_sample_period_ms;
static uint32_t s_next_reinit_ms;
static bool s_session_zero_pending;
static bool s_debug_voltage_threshold_forced;
static ltc2959_alert_tracker_t s_alert_tracker;

static bool read_regs(uint8_t reg, uint8_t *dst, size_t len);
static bool write_regs(uint8_t reg, const uint8_t *src, size_t len);
static bool write_reg(uint8_t reg, uint8_t value);
static bool configure_device(void);
static bool start_conversion(uint32_t now_ms);
static bool finish_conversion(uint32_t now_ms);
static void process_snapshot(const ltc2959_raw_snapshot_t *raw);
static void note_i2c_result(bool ok);
static void note_sample_failure(uint32_t now_ms);
static void mark_new_session(void);
static bool write_session_zero(uint32_t *acr_out);
static void accept_session_zero(uint32_t acr);
static bool release_alert_with_ara(void);
static bool restore_voltage_thresholds(void);

bool ltc2959_hal_init(uint32_t now_ms) {
    /* A bus-recovery init is not a gauge reset. Preserve the software session
     * until the boot registers themselves provide reset/UVLO evidence. */
    uint32_t prior_errors = s_snapshot.i2c_error_count;
    uint32_t prior_ara_errors = s_snapshot.ara_error_count;
    uint32_t prior_session = s_snapshot.gauge_session;
    uint32_t prior_conversion_sequence = s_snapshot.conversion_sequence;
    uint32_t prior_sample_sequence = s_snapshot.sample_sequence;
    uint8_t prior_status_latched = s_snapshot.status_latched;
    bool prior_continuity_valid = s_snapshot.continuity_valid;
    bool prior_session_known =
        s_snapshot.present ||
        s_snapshot.configured ||
        s_snapshot.sample_valid ||
        prior_session != 0u ||
        prior_sample_sequence != 0u;
    memset(&s_snapshot, 0, sizeof(s_snapshot));
    s_snapshot.i2c_error_count = prior_errors;
    s_snapshot.ara_error_count = prior_ara_errors;
    s_snapshot.gauge_session = prior_session;
    s_snapshot.conversion_sequence = prior_conversion_sequence;
    s_snapshot.sample_sequence = prior_sample_sequence;
    s_snapshot.status_latched = prior_status_latched;
    s_snapshot.current_polarity_verified = true;
    s_state = LTC2959_STATE_DOWN;
    s_consecutive_failures = 0u;
    s_consecutive_sample_failures = 0u;
    s_settled_count = 0u;
    s_previous_voltage_mv = 0u;
    s_conversion_started_ms = 0u;
    s_next_sample_ms = now_ms;
    s_sample_period_ms = LTC2959_SAMPLE_DEFAULT_PERIOD_MS;
    s_next_reinit_ms = now_ms + LTC2959_REINIT_MS;
    s_debug_voltage_threshold_forced = false;
    ltc2959_alert_tracker_init(&s_alert_tracker);

    uint8_t boot[7];
    if (!read_regs(LTC2959_REG_STATUS, boot, sizeof(boot))) {
        return false;
    }
    s_snapshot.present = true;
    s_snapshot.status_latched |= boot[LTC2959_REG_STATUS];
    ltc2959_alert_tracker_observe_status(
        &s_alert_tracker, boot[LTC2959_REG_STATUS]);

    uint8_t adc_control = boot[LTC2959_REG_ADC_CONTROL];
    uint8_t cc_control = boot[LTC2959_REG_COULOMB_CONTROL];
    uint32_t acr = ltc2959_decode_u32_be(&boot[LTC2959_REG_ACR_MSB]);
    bool hardware_reset_evidence =
        (boot[LTC2959_REG_STATUS] & LTC2959_STATUS_UVLO) != 0u ||
        (adc_control == LTC2959_ADC_POR_DEFAULT &&
         cc_control == LTC2959_CC_POR_DEFAULT &&
         acr == LTC2959_ACR_POR_DEFAULT);
    if (hardware_reset_evidence && !s_session_zero_pending) {
        mark_new_session();
    }
    if (hardware_reset_evidence) {
        s_session_zero_pending = true;
    }
    if (!s_session_zero_pending) {
        s_snapshot.continuity_valid =
            prior_session_known ? prior_continuity_valid : true;
    }

    if (!configure_device()) {
        return false;
    }
    if (s_session_zero_pending) {
        if (!write_session_zero(&acr)) {
            s_snapshot.configured = false;
            return false;
        }
        accept_session_zero(acr);
    }
    s_snapshot.acr_raw = acr;
    s_snapshot.session_delta_nah =
        ltc2959_acr_delta_nah(acr, LTC2959_ACR_POR_DEFAULT);
    s_state = LTC2959_STATE_READY;
    return start_conversion(now_ms);
}

void ltc2959_hal_poll(uint32_t now_ms, uint32_t sample_period_ms) {
    if (sample_period_ms == 0u) {
        sample_period_ms = LTC2959_SAMPLE_DEFAULT_PERIOD_MS;
    }
    if (sample_period_ms < s_sample_period_ms &&
        s_state == LTC2959_STATE_READY) {
        uint32_t earlier_ms = now_ms + sample_period_ms;
        if (time_diff_ms(s_next_sample_ms, earlier_ms) > 0) {
            s_next_sample_ms = earlier_ms;
        }
    }
    s_sample_period_ms = sample_period_ms;

    if (s_state == LTC2959_STATE_DOWN) {
        if (time_diff_ms(now_ms, s_next_reinit_ms) >= 0) {
            s_next_reinit_ms = now_ms + LTC2959_REINIT_MS;
            (void)ltc2959_hal_init(now_ms);
        }
        return;
    }

    if (s_state == LTC2959_STATE_CONVERTING) {
        if ((uint32_t)(now_ms - s_conversion_started_ms) >=
            LTC2959_CONVERSION_WAIT_MS) {
            (void)finish_conversion(now_ms);
        }
        return;
    }

    if (time_diff_ms(now_ms, s_next_sample_ms) >= 0) {
        (void)start_conversion(now_ms);
    }
}

void ltc2959_hal_get_snapshot(ltc2959_snapshot_t *out) {
    if (out != NULL) {
        *out = s_snapshot;
    }
}

bool ltc2959_hal_start_new_session(void) {
    if (!s_snapshot.present || !s_snapshot.configured ||
        s_state == LTC2959_STATE_CONVERTING) {
        return false;
    }

    uint32_t acr = 0u;
    if (!write_session_zero(&acr)) {
        return false;
    }
    mark_new_session();
    accept_session_zero(acr);
    s_state = LTC2959_STATE_READY;
    s_next_sample_ms = time_ms();
    return true;
}

bool ltc2959_hal_prepare_dormant(void) {
    if (!s_snapshot.present || s_state == LTC2959_STATE_CONVERTING) {
        return false;
    }
    uint8_t control =
        ltc2959_adc_control(LTC2959_ACQUIRE_SMART_SLEEP);
    bool ok = write_reg(LTC2959_REG_ADC_CONTROL, control);
    if (ok) {
        s_state = LTC2959_STATE_READY;
        s_snapshot.configured = true;
    }
    return ok;
}

bool ltc2959_hal_resume_active(uint32_t now_ms) {
    if (!s_snapshot.present) {
        return ltc2959_hal_init(now_ms);
    }
    uint8_t control =
        ltc2959_adc_control(LTC2959_ACQUIRE_SLEEP);
    if (!write_reg(LTC2959_REG_ADC_CONTROL, control)) {
        return false;
    }
    s_snapshot.configured = true;
    s_state = LTC2959_STATE_READY;
    s_next_sample_ms = now_ms;
    return start_conversion(now_ms);
}

void ltc2959_hal_service_alert(ltc2959_irq_result_t *out) {
    ltc2959_irq_result_t result;
    memset(&result, 0, sizeof(result));
    if (!s_snapshot.present) {
        result.transport_ok = true;
        if (out != NULL) {
            *out = result;
        }
        return;
    }

    uint8_t status = 0u;
    if (!read_regs(LTC2959_REG_STATUS, &status, 1u)) {
        if (out != NULL) {
            *out = result;
        }
        return;
    }
    s_snapshot.status_latched |= status;
    result.status = status;
    ltc2959_alert_tracker_observe_status(&s_alert_tracker, status);
    if (!ltc2959_alert_tracker_needs_ara(&s_alert_tracker)) {
        result.transport_ok = true;
        if (out != NULL) {
            *out = result;
        }
        return;
    }

    result.did_work = status != 0u;
    bool operation_ok = release_alert_with_ara();
    ltc2959_alert_tracker_note_ara(
        &s_alert_tracker, operation_ok);
    if ((status & LTC2959_STATUS_UVLO) != 0u) {
        if (!s_session_zero_pending) {
            mark_new_session();
        }
        s_session_zero_pending = true;
        s_snapshot.configured = false;
        uint32_t acr = 0u;
        if (configure_device() && write_session_zero(&acr)) {
            accept_session_zero(acr);
            s_state = LTC2959_STATE_READY;
            s_next_sample_ms = time_ms();
        } else {
            operation_ok = false;
            s_snapshot.configured = false;
            s_state = LTC2959_STATE_DOWN;
            s_next_reinit_ms = time_ms() + LTC2959_REINIT_MS;
        }
    }
    if (s_debug_voltage_threshold_forced &&
        (status & LTC2959_STATUS_VOLTAGE_ALERT) != 0u) {
        if (!restore_voltage_thresholds()) {
            operation_ok = false;
        }
    }
    result.ara_released =
        !ltc2959_alert_tracker_needs_ara(&s_alert_tracker);
    result.transport_ok = operation_ok && result.ara_released;
    result.more_work = !result.ara_released;
    if (out != NULL) {
        *out = result;
    }
}

bool ltc2959_hal_debug_force_voltage_alert(void) {
    if (!s_snapshot.present) {
        return false;
    }
    uint16_t threshold = s_snapshot.sample_valid
        ? ltc2959_voltage_raw_from_mv(
              s_snapshot.voltage_mv > 100u
                  ? (uint32_t)s_snapshot.voltage_mv - 100u
                  : 0u)
        : 0u;
    uint8_t bytes[2] = {
        (uint8_t)(threshold >> 8),
        (uint8_t)threshold,
    };
    bool ok = write_regs(
                  LTC2959_REG_VOLTAGE_THRESHOLD_HIGH,
                  bytes,
                  sizeof(bytes)) &&
              start_conversion(time_ms());
    s_debug_voltage_threshold_forced = ok;
    return ok;
}

static bool read_regs(uint8_t reg, uint8_t *dst, size_t len) {
    if (dst == NULL || len == 0u) {
        return false;
    }
    int written = i2c_write_timeout_us(
        BOARD_I2C_PORT,
        LTC2959_I2C_ADDR,
        &reg,
        1u,
        true,
        BOARD_I2C_TIMEOUT_US);
    if (written != 1) {
        note_i2c_result(false);
        return false;
    }
    bool ok = i2c_read_timeout_us(
                  BOARD_I2C_PORT,
                  LTC2959_I2C_ADDR,
                  dst,
                  len,
                  false,
                  BOARD_I2C_TIMEOUT_US) == (int)len;
    note_i2c_result(ok);
    return ok;
}

static bool write_regs(uint8_t reg, const uint8_t *src, size_t len) {
    if (src == NULL || len == 0u || len > 4u) {
        return false;
    }
    uint8_t bytes[5];
    bytes[0] = reg;
    memcpy(&bytes[1], src, len);
    bool ok = i2c_write_timeout_us(
                  BOARD_I2C_PORT,
                  LTC2959_I2C_ADDR,
                  bytes,
                  len + 1u,
                  false,
                  BOARD_I2C_TIMEOUT_US) == (int)(len + 1u);
    note_i2c_result(ok);
    return ok;
}

static bool write_reg(uint8_t reg, uint8_t value) {
    return write_regs(reg, &value, 1u);
}

static bool configure_device(void) {
    /* Move GPIO out of alert mode first. Per the datasheet this immediately
     * releases a stale alert pull-down, so old thresholds cannot hold GP42
     * while their all-range defaults are restored. */
    uint8_t analog_sleep =
        (uint8_t)(LTC2959_ADC_MODE_SLEEP_BITS |
                  LTC2959_ADC_GPIO_ANALOG_WIDE_BITS);
    if (!write_reg(LTC2959_REG_ADC_CONTROL, analog_sleep)) {
        return false;
    }
    /* Selecting analog input immediately releases any old alert pull-down,
     * even if its ARA transaction failed. */
    ltc2959_alert_tracker_force_released(&s_alert_tracker);

    uint8_t cc =
        ltc2959_coulomb_control(LTC2959_DEADBAND_20_UV, true);
    if (!write_reg(LTC2959_REG_COULOMB_CONTROL, cc)) {
        return false;
    }
    uint8_t charge_low[4] = {0u, 0u, 0u, 0u};
    uint8_t charge_high[4] = {0xffu, 0xffu, 0xffu, 0xffu};
    uint8_t signed_limits[4] = {0x7fu, 0xffu, 0x80u, 0x00u};
    uint8_t unsigned_limits[4] = {0xffu, 0xffu, 0u, 0u};
    if (!write_regs(
            LTC2959_REG_CHARGE_THRESHOLD_LOW,
            charge_low,
            sizeof(charge_low)) ||
        !write_regs(
            LTC2959_REG_CHARGE_THRESHOLD_HIGH,
            charge_high,
            sizeof(charge_high)) ||
        !restore_voltage_thresholds() ||
        !write_regs(
            LTC2959_REG_CURRENT_THRESHOLD_HIGH,
            signed_limits,
            sizeof(signed_limits)) ||
        !write_regs(
            LTC2959_REG_TEMPERATURE_THRESHOLD_HIGH,
            unsigned_limits,
            sizeof(unsigned_limits)) ||
        !write_regs(
            LTC2959_REG_GPIO_THRESHOLD_HIGH,
            signed_limits,
            sizeof(signed_limits))) {
        return false;
    }

    uint8_t desired[2] = {
        ltc2959_adc_control(LTC2959_ACQUIRE_SLEEP),
        cc,
    };
    if (!write_regs(LTC2959_REG_ADC_CONTROL, desired, sizeof(desired))) {
        return false;
    }
    uint8_t verify[2] = {0u, 0u};
    if (!read_regs(LTC2959_REG_ADC_CONTROL, verify, sizeof(verify)) ||
        verify[0] != desired[0] ||
        verify[1] != desired[1] ||
        !ltc2959_adc_control_uses_vdd(verify[0])) {
        s_snapshot.configured = false;
        return false;
    }
    s_snapshot.configured = true;
    s_debug_voltage_threshold_forced = false;
    return true;
}

static bool start_conversion(uint32_t now_ms) {
    if (s_state == LTC2959_STATE_CONVERTING ||
        !s_snapshot.present ||
        !s_snapshot.configured) {
        return false;
    }
    uint8_t control =
        ltc2959_adc_control(LTC2959_ACQUIRE_SINGLE_SHOT);
    if (!write_reg(LTC2959_REG_ADC_CONTROL, control)) {
        return false;
    }
    s_conversion_started_ms = now_ms;
    s_state = LTC2959_STATE_CONVERTING;
    return true;
}

static bool finish_conversion(uint32_t now_ms) {
    s_snapshot.conversion_sequence++;
    s_snapshot.last_conversion_sample_valid = false;
    uint8_t bytes[LTC2959_SNAPSHOT_LEN];
    if (!read_regs(
            LTC2959_SNAPSHOT_FIRST_REG, bytes, sizeof(bytes))) {
        note_sample_failure(now_ms);
        if (s_state != LTC2959_STATE_DOWN) {
            s_state = LTC2959_STATE_READY;
            s_next_sample_ms = now_ms + LTC2959_REINIT_MS;
        }
        return false;
    }

    ltc2959_raw_snapshot_t raw;
    if (!ltc2959_decode_snapshot(bytes, sizeof(bytes), &raw)) {
        note_sample_failure(now_ms);
        if (s_state != LTC2959_STATE_DOWN) {
            s_state = LTC2959_STATE_READY;
            s_next_sample_ms = now_ms + LTC2959_REINIT_MS;
        }
        return false;
    }
    s_consecutive_sample_failures = 0u;
    bool session_reset_evidence =
        (raw.status & LTC2959_STATUS_UVLO) != 0u ||
        (raw.adc_control == LTC2959_ADC_POR_DEFAULT &&
         raw.coulomb_control == LTC2959_CC_POR_DEFAULT &&
         raw.acr_raw == LTC2959_ACR_POR_DEFAULT);
    bool configuration_invalid =
        !ltc2959_adc_control_uses_vdd(raw.adc_control) ||
        raw.coulomb_control !=
            ltc2959_coulomb_control(LTC2959_DEADBAND_20_UV, true);
    s_snapshot.status_latched |= raw.status;
    if (session_reset_evidence || configuration_invalid ||
        s_session_zero_pending) {
        if (session_reset_evidence) {
            if (!s_session_zero_pending) {
                mark_new_session();
            }
            s_session_zero_pending = true;
        }
        s_snapshot.configured = false;
        if (!configure_device()) {
            s_state = LTC2959_STATE_DOWN;
            s_next_reinit_ms = now_ms + LTC2959_REINIT_MS;
            return false;
        }
        if (s_session_zero_pending) {
            uint32_t acr = 0u;
            if (!write_session_zero(&acr)) {
                s_snapshot.configured = false;
                s_state = LTC2959_STATE_DOWN;
                s_next_reinit_ms = now_ms + LTC2959_REINIT_MS;
                return false;
            }
            accept_session_zero(acr);
        }
        s_state = LTC2959_STATE_READY;
        s_next_sample_ms = now_ms + LTC2959_CONVERSION_WAIT_MS;
        return true;
    } else if (raw.status != 0u) {
        ltc2959_alert_tracker_observe_status(
            &s_alert_tracker, raw.status);
        bool ara_ok = release_alert_with_ara();
        ltc2959_alert_tracker_note_ara(
            &s_alert_tracker, ara_ok);
        if (s_debug_voltage_threshold_forced &&
            (raw.status & LTC2959_STATUS_VOLTAGE_ALERT) != 0u) {
            (void)restore_voltage_thresholds();
        }
    }

    process_snapshot(&raw);
    s_snapshot.last_conversion_sample_valid = s_snapshot.sample_valid;
    s_state = LTC2959_STATE_READY;
    uint32_t cadence = s_sample_period_ms;
    if (s_settled_count < LTC2959_SETTLED_SAMPLES) {
        cadence = LTC2959_CONVERSION_WAIT_MS;
    }
    s_next_sample_ms = now_ms + cadence;
    return true;
}

static void process_snapshot(const ltc2959_raw_snapshot_t *raw) {
    uint16_t voltage_mv = ltc2959_voltage_mv(raw->voltage_raw);
    bool plausible =
        voltage_mv >= LTC2959_PLAUSIBLE_MIN_MV &&
        voltage_mv <= LTC2959_PLAUSIBLE_MAX_MV;
    if (!plausible) {
        s_settled_count = 0u;
        s_snapshot.sample_valid = false;
        return;
    }

    if (s_previous_voltage_mv != 0u) {
        uint16_t delta = voltage_mv > s_previous_voltage_mv
            ? (uint16_t)(voltage_mv - s_previous_voltage_mv)
            : (uint16_t)(s_previous_voltage_mv - voltage_mv);
        if (delta > LTC2959_REPLACEMENT_JUMP_MV) {
            /* Battery replacement is identified by POR/UVLO. A voltage jump
             * alone can be modem load release, so use it only to re-settle the
             * displayed voltage and never destroy accumulated charge. */
            s_settled_count = 1u;
        } else if (delta <= LTC2959_SETTLE_DELTA_MV &&
                   s_settled_count < LTC2959_SETTLED_SAMPLES) {
            s_settled_count++;
        } else if (delta > LTC2959_SETTLE_DELTA_MV) {
            s_settled_count = 1u;
        }
    } else {
        s_settled_count = 1u;
    }
    s_previous_voltage_mv = voltage_mv;

    s_snapshot.voltage_mv = voltage_mv;
    s_snapshot.current_ua = ltc2959_current_ua(
        raw->current_raw, LTC2959_BOARD_CURRENT_POLARITY);
    s_snapshot.acr_raw = raw->acr_raw;
    s_snapshot.session_delta_nah =
        ltc2959_acr_delta_nah(raw->acr_raw, LTC2959_ACR_POR_DEFAULT);
    s_snapshot.temperature_mdegc =
        ltc2959_temperature_mdegc(raw->temperature_raw);
    s_snapshot.sample_valid =
        s_settled_count >= LTC2959_SETTLED_SAMPLES;
    s_snapshot.sample_sequence++;
}

static void note_i2c_result(bool ok) {
    if (ok) {
        s_consecutive_failures = 0u;
        return;
    }
    s_snapshot.i2c_error_count++;
    if (s_consecutive_failures < UINT8_MAX) {
        s_consecutive_failures++;
    }
    if (s_consecutive_failures >= LTC2959_I2C_FAIL_LIMIT) {
        if (s_snapshot.present) {
            LOGW("battery", "LTC2959 I2C unresponsive; retrying at 1 Hz");
        }
        s_snapshot.present = false;
        s_snapshot.configured = false;
        s_snapshot.sample_valid = false;
        s_state = LTC2959_STATE_DOWN;
    }
}

static void note_sample_failure(uint32_t now_ms) {
    if (s_consecutive_sample_failures < UINT8_MAX) {
        s_consecutive_sample_failures++;
    }
    if (s_consecutive_sample_failures < LTC2959_SAMPLE_FAIL_LIMIT) {
        return;
    }
    if (s_snapshot.present) {
        LOGW("battery", "LTC2959 sample reads failed; reinitializing");
    }
    s_snapshot.present = false;
    s_snapshot.configured = false;
    s_snapshot.sample_valid = false;
    s_state = LTC2959_STATE_DOWN;
    s_next_reinit_ms = now_ms + LTC2959_REINIT_MS;
}

static void mark_new_session(void) {
    s_snapshot.gauge_session++;
    s_snapshot.continuity_valid = false;
    s_snapshot.sample_valid = false;
    s_settled_count = 0u;
    s_previous_voltage_mv = 0u;
    s_snapshot.session_delta_nah = 0;
}

static bool write_session_zero(uint32_t *acr_out) {
    uint8_t target[4] = {
        (uint8_t)(LTC2959_ACR_POR_DEFAULT >> 24),
        (uint8_t)(LTC2959_ACR_POR_DEFAULT >> 16),
        (uint8_t)(LTC2959_ACR_POR_DEFAULT >> 8),
        (uint8_t)LTC2959_ACR_POR_DEFAULT,
    };
    if (!write_regs(LTC2959_REG_ACR_MSB, target, sizeof(target))) {
        return false;
    }

    uint8_t verify[4];
    if (!read_regs(LTC2959_REG_ACR_MSB, verify, sizeof(verify))) {
        return false;
    }
    uint32_t acr = ltc2959_decode_u32_be(verify);
    int32_t delta = ltc2959_acr_delta(acr, LTC2959_ACR_POR_DEFAULT);
    if (delta < -1 || delta > 1) {
        return false;
    }
    if (acr_out != NULL) {
        *acr_out = acr;
    }
    return true;
}

static void accept_session_zero(uint32_t acr) {
    s_snapshot.acr_raw = acr;
    s_snapshot.session_delta_nah =
        ltc2959_acr_delta_nah(acr, LTC2959_ACR_POR_DEFAULT);
    s_snapshot.continuity_valid = true;
    s_session_zero_pending = false;
}

static bool release_alert_with_ara(void) {
    uint8_t response = 0u;
    int read_result = i2c_read_timeout_us(
        BOARD_I2C_PORT,
        LTC2959_ARA_I2C_ADDR,
        &response,
        1u,
        false,
        BOARD_I2C_TIMEOUT_US);
    bool transport_ok = read_result == 1;
    bool ok = transport_ok && ltc2959_ara_response_matches(response);
    s_snapshot.ara_last_transport_ok = transport_ok;
    s_snapshot.ara_last_response = response;
    s_snapshot.ara_last_read_result = read_result;
    s_snapshot.ara_last_valid = ok;
    if (!ok) {
        s_snapshot.ara_error_count++;
    }
    return ok;
}

static bool restore_voltage_thresholds(void) {
    uint8_t limits[4] = {0xffu, 0xffu, 0u, 0u};
    bool ok = write_regs(
        LTC2959_REG_VOLTAGE_THRESHOLD_HIGH, limits, sizeof(limits));
    if (ok) {
        s_debug_voltage_threshold_forced = false;
    }
    return ok;
}

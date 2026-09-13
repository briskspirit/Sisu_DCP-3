#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "hal/ltc2959_hal.h"
#include "hardware/i2c.h"
#include "services/log.h"

static i2c_inst_t s_i2c;
i2c_inst_t *const i2c0 = &s_i2c;

static uint8_t s_regs[256];
static uint8_t s_selected_reg;
static uint32_t s_now_ms;
static uint8_t s_failed_snapshot_reads;
static int s_ara_read_result = 1;
static uint8_t s_ara_response = LTC2959_ARA_RESPONSE_ADDRESS_BITS;
static uint32_t s_acr_write_count;
static size_t s_last_acr_write_len;
static bool s_fail_acr_writes;

static void set_u16_be(uint8_t reg, uint16_t value) {
    s_regs[reg] = (uint8_t)(value >> 8);
    s_regs[(uint8_t)(reg + 1u)] = (uint8_t)value;
}

static void set_u32_be(uint8_t reg, uint32_t value) {
    s_regs[reg] = (uint8_t)(value >> 24);
    s_regs[(uint8_t)(reg + 1u)] = (uint8_t)(value >> 16);
    s_regs[(uint8_t)(reg + 2u)] = (uint8_t)(value >> 8);
    s_regs[(uint8_t)(reg + 3u)] = (uint8_t)value;
}

static void set_acr(uint32_t value) {
    set_u32_be(LTC2959_REG_ACR_MSB, value);
}

static void set_now(uint32_t now_ms) {
    s_now_ms = now_ms;
}

int i2c_write_timeout_us(i2c_inst_t *i2c,
                         uint8_t addr,
                         const uint8_t *src,
                         size_t len,
                         bool nostop,
                         unsigned int timeout_us) {
    (void)i2c;
    (void)nostop;
    (void)timeout_us;
    if (addr != LTC2959_I2C_ADDR || src == NULL || len == 0u) {
        return -1;
    }
    s_selected_reg = src[0];
    if (s_selected_reg == LTC2959_REG_ACR_MSB && len > 1u) {
        s_acr_write_count++;
        s_last_acr_write_len = len - 1u;
        if (s_fail_acr_writes) {
            return -1;
        }
    }
    for (size_t i = 1u; i < len; i++) {
        s_regs[(uint8_t)(s_selected_reg + i - 1u)] = src[i];
    }
    return (int)len;
}

int i2c_read_timeout_us(i2c_inst_t *i2c,
                        uint8_t addr,
                        uint8_t *dst,
                        size_t len,
                        bool nostop,
                        unsigned int timeout_us) {
    (void)i2c;
    (void)nostop;
    (void)timeout_us;
    if (addr == LTC2959_ARA_I2C_ADDR && dst != NULL && len == 1u) {
        if (s_ara_read_result == 1) {
            dst[0] = s_ara_response;
        }
        return s_ara_read_result;
    }
    if (addr != LTC2959_I2C_ADDR || dst == NULL || len == 0u) {
        return -1;
    }
    if (s_selected_reg == LTC2959_SNAPSHOT_FIRST_REG &&
        len == LTC2959_SNAPSHOT_LEN &&
        s_failed_snapshot_reads != 0u) {
        s_failed_snapshot_reads--;
        return -1;
    }
    memcpy(dst, &s_regs[s_selected_reg], len);
    if (s_selected_reg == LTC2959_REG_STATUS) {
        s_regs[LTC2959_REG_STATUS] = 0u;
    }
    return (int)len;
}

uint32_t time_ms(void) {
    return s_now_ms;
}

int32_t time_diff_ms(uint32_t a, uint32_t b) {
    return (int32_t)(a - b);
}

void log_write(log_level_t level, const char *tag, const char *fmt, ...) {
    (void)level;
    (void)tag;
    (void)fmt;
}

static void poll_at_period(uint32_t now_ms, uint32_t period_ms) {
    set_now(now_ms);
    ltc2959_hal_poll(now_ms, period_ms);
}

static void poll_at(uint32_t now_ms) {
    poll_at_period(now_ms, LTC2959_SAMPLE_DEFAULT_PERIOD_MS);
}

static void prepare_present_gauge(void) {
    memset(s_regs, 0, sizeof(s_regs));
    s_regs[LTC2959_REG_ADC_CONTROL] =
        ltc2959_adc_control(LTC2959_ACQUIRE_SLEEP);
    s_regs[LTC2959_REG_COULOMB_CONTROL] =
        ltc2959_coulomb_control(LTC2959_DEADBAND_20_UV, true);
    set_acr(0x80000001u);
    set_u16_be(
        LTC2959_REG_VOLTAGE_MSB,
        ltc2959_voltage_raw_from_mv(2650u));
    set_u16_be(LTC2959_REG_CURRENT_MSB, 0u);
    set_u16_be(LTC2959_REG_TEMPERATURE_MSB, 0x8000u);
    s_failed_snapshot_reads = 0u;
    s_ara_read_result = 1;
    s_ara_response = LTC2959_ARA_RESPONSE_ADDRESS_BITS;
    s_acr_write_count = 0u;
    s_last_acr_write_len = 0u;
    s_fail_acr_writes = false;
    set_now(0u);
    assert(ltc2959_hal_init(0u));
}

static void test_ara_evidence_and_pending_retry(void) {
    prepare_present_gauge();

    ltc2959_snapshot_t snapshot;
    ltc2959_hal_get_snapshot(&snapshot);
    uint32_t prior_ara_errors = snapshot.ara_error_count;

    s_regs[LTC2959_REG_STATUS] = LTC2959_STATUS_VOLTAGE_ALERT;
    s_ara_response = 0xc4u;
    ltc2959_irq_result_t result;
    ltc2959_hal_service_alert(&result);
    assert(!result.transport_ok);
    assert(result.did_work);
    assert(result.more_work);
    assert(!result.ara_released);

    ltc2959_hal_get_snapshot(&snapshot);
    assert(snapshot.ara_last_transport_ok);
    assert(!snapshot.ara_last_valid);
    assert(snapshot.ara_last_read_result == 1);
    assert(snapshot.ara_last_response == 0xc4u);
    assert(snapshot.ara_error_count == prior_ara_errors + 1u);

    /* STATUS cleared on the first read, but the failed ARA remains owed. */
    s_ara_response = LTC2959_ARA_RESPONSE_ADDRESS_BITS;
    ltc2959_hal_service_alert(&result);
    assert(result.transport_ok);
    assert(!result.did_work);
    assert(!result.more_work);
    assert(result.ara_released);
    ltc2959_hal_get_snapshot(&snapshot);
    assert(snapshot.ara_last_transport_ok);
    assert(snapshot.ara_last_valid);
    assert(snapshot.ara_last_read_result == 1);
    assert(snapshot.ara_last_response ==
           LTC2959_ARA_RESPONSE_ADDRESS_BITS);
    assert(snapshot.ara_error_count == prior_ara_errors + 1u);
}

static void test_ara_transport_failure_evidence(void) {
    prepare_present_gauge();

    ltc2959_snapshot_t snapshot;
    ltc2959_hal_get_snapshot(&snapshot);
    uint32_t prior_ara_errors = snapshot.ara_error_count;

    s_regs[LTC2959_REG_STATUS] = LTC2959_STATUS_CURRENT_ALERT;
    s_ara_read_result = -1;
    ltc2959_irq_result_t result;
    ltc2959_hal_service_alert(&result);
    assert(!result.transport_ok);
    assert(result.did_work);
    assert(result.more_work);
    assert(!result.ara_released);

    ltc2959_hal_get_snapshot(&snapshot);
    assert(!snapshot.ara_last_transport_ok);
    assert(!snapshot.ara_last_valid);
    assert(snapshot.ara_last_read_result == -1);
    assert(snapshot.ara_last_response == 0u);
    assert(snapshot.ara_error_count == prior_ara_errors + 1u);
}

static void test_stale_sample_recovery_preserves_coulomb_session(void) {
    memset(s_regs, 0, sizeof(s_regs));
    s_regs[LTC2959_REG_ADC_CONTROL] =
        ltc2959_adc_control(LTC2959_ACQUIRE_SLEEP);
    s_regs[LTC2959_REG_COULOMB_CONTROL] =
        ltc2959_coulomb_control(LTC2959_DEADBAND_20_UV, true);
    const uint32_t boot_acr = LTC2959_ACR_POR_DEFAULT + 50u;
    set_acr(boot_acr);
    set_u16_be(
        LTC2959_REG_VOLTAGE_MSB,
        ltc2959_voltage_raw_from_mv(2500u));
    set_u16_be(LTC2959_REG_CURRENT_MSB, 10u);
    set_u16_be(LTC2959_REG_TEMPERATURE_MSB, 0x8000u);

    set_now(0u);
    assert(ltc2959_hal_init(0u));
    poll_at(3u);
    poll_at(6u);
    poll_at(9u);

    ltc2959_snapshot_t snapshot;
    ltc2959_hal_get_snapshot(&snapshot);
    assert(snapshot.present);
    assert(snapshot.configured);
    assert(snapshot.sample_valid);
    assert(snapshot.current_polarity_verified);
    assert(snapshot.conversion_sequence == 2u);
    assert(snapshot.last_conversion_sample_valid);
    assert(snapshot.sample_sequence == 2u);
    assert(snapshot.continuity_valid);
    assert(snapshot.session_delta_nah ==
           (int64_t)50 * LTC2959_ACR_LSB_NAH);

    set_acr(boot_acr + 100u);
    poll_at(5009u);
    poll_at(5012u);
    ltc2959_hal_get_snapshot(&snapshot);
    assert(snapshot.sample_sequence == 3u);
    assert(snapshot.session_delta_nah ==
           (int64_t)150 * LTC2959_ACR_LSB_NAH);

    s_failed_snapshot_reads = 3u;
    poll_at(10012u);
    poll_at(10015u);
    ltc2959_hal_get_snapshot(&snapshot);
    assert(snapshot.present);
    assert(snapshot.sample_valid);

    poll_at(11015u);
    poll_at(11018u);
    ltc2959_hal_get_snapshot(&snapshot);
    assert(snapshot.present);
    assert(snapshot.sample_valid);

    poll_at(12018u);
    poll_at(12021u);
    ltc2959_hal_get_snapshot(&snapshot);
    assert(!snapshot.present);
    assert(!snapshot.configured);
    assert(!snapshot.sample_valid);
    assert(snapshot.conversion_sequence == 6u);
    assert(!snapshot.last_conversion_sample_valid);
    assert(snapshot.sample_sequence == 3u);

    set_acr(boot_acr + 150u);
    poll_at(13021u);
    ltc2959_hal_get_snapshot(&snapshot);
    assert(snapshot.present);
    assert(snapshot.configured);
    assert(!snapshot.sample_valid);
    assert(snapshot.sample_sequence == 3u);
    assert(snapshot.continuity_valid);
    assert(snapshot.session_delta_nah ==
           (int64_t)200 * LTC2959_ACR_LSB_NAH);

    poll_at(13024u);
    poll_at(13027u);
    poll_at(13030u);
    ltc2959_hal_get_snapshot(&snapshot);
    assert(snapshot.sample_valid);
    assert(snapshot.conversion_sequence == 8u);
    assert(snapshot.last_conversion_sample_valid);
    assert(snapshot.sample_sequence == 5u);
    assert(snapshot.session_delta_nah ==
           (int64_t)200 * LTC2959_ACR_LSB_NAH);
}

static void test_explicit_new_session_uses_one_burst(void) {
    prepare_present_gauge();
    poll_at(3u);
    set_acr(LTC2959_ACR_POR_DEFAULT - 500u);
    poll_at(5003u);
    poll_at(5006u);

    ltc2959_snapshot_t before;
    ltc2959_hal_get_snapshot(&before);
    assert(before.session_delta_nah ==
           (int64_t)-500 * LTC2959_ACR_LSB_NAH);
    assert(ltc2959_hal_start_new_session());
    assert(s_acr_write_count == 1u);
    assert(s_last_acr_write_len == 4u);
    assert(ltc2959_decode_u32_be(&s_regs[LTC2959_REG_ACR_MSB]) ==
           LTC2959_ACR_POR_DEFAULT);

    ltc2959_snapshot_t after;
    ltc2959_hal_get_snapshot(&after);
    assert(after.gauge_session == before.gauge_session + 1u);
    assert(after.continuity_valid);
    assert(!after.sample_valid);
    assert(after.acr_raw == LTC2959_ACR_POR_DEFAULT);
    assert(after.session_delta_nah == 0);
}

static void test_uvlo_reseeds_uncertain_acr(void) {
    prepare_present_gauge();
    poll_at(3u);
    ltc2959_snapshot_t before;
    ltc2959_hal_get_snapshot(&before);

    set_acr(0x12345678u);
    s_regs[LTC2959_REG_STATUS] = LTC2959_STATUS_UVLO;
    ltc2959_irq_result_t result;
    ltc2959_hal_service_alert(&result);
    assert(result.transport_ok);
    assert(result.did_work);
    assert(s_acr_write_count == 1u);
    assert(s_last_acr_write_len == 4u);

    ltc2959_snapshot_t after;
    ltc2959_hal_get_snapshot(&after);
    assert(after.gauge_session == before.gauge_session + 1u);
    assert(after.continuity_valid);
    assert(after.acr_raw == LTC2959_ACR_POR_DEFAULT);
    assert(after.session_delta_nah == 0);
}

static void test_configuration_repair_preserves_acr(void) {
    prepare_present_gauge();
    set_acr(LTC2959_ACR_POR_DEFAULT - 300u);
    s_regs[LTC2959_REG_ADC_CONTROL] = LTC2959_ADC_POR_DEFAULT;
    poll_at(3u);
    assert(s_acr_write_count == 0u);
    assert(ltc2959_decode_u32_be(&s_regs[LTC2959_REG_ACR_MSB]) ==
           LTC2959_ACR_POR_DEFAULT - 300u);

    poll_at(6u);
    poll_at(9u);
    ltc2959_snapshot_t snapshot;
    ltc2959_hal_get_snapshot(&snapshot);
    assert(snapshot.session_delta_nah ==
           (int64_t)-300 * LTC2959_ACR_LSB_NAH);
}

static void test_failed_uvlo_reseed_is_retried(void) {
    prepare_present_gauge();
    poll_at(3u);
    set_acr(0x23456789u);
    s_regs[LTC2959_REG_STATUS] = LTC2959_STATUS_UVLO;
    s_fail_acr_writes = true;

    ltc2959_irq_result_t result;
    ltc2959_hal_service_alert(&result);
    assert(!result.transport_ok);
    ltc2959_snapshot_t failed;
    ltc2959_hal_get_snapshot(&failed);
    assert(!failed.configured);
    assert(!failed.continuity_valid);
    assert(s_acr_write_count == 1u);

    s_fail_acr_writes = false;
    poll_at(1003u);
    assert(s_acr_write_count == 2u);
    assert(ltc2959_decode_u32_be(&s_regs[LTC2959_REG_ACR_MSB]) ==
           LTC2959_ACR_POR_DEFAULT);
    ltc2959_snapshot_t recovered;
    ltc2959_hal_get_snapshot(&recovered);
    assert(recovered.configured);
    assert(recovered.continuity_valid);
    assert(recovered.session_delta_nah == 0);
}

static void test_shorter_period_advances_pending_deadline(void) {
    prepare_present_gauge();
    ltc2959_snapshot_t snapshot;
    ltc2959_hal_get_snapshot(&snapshot);
    uint32_t base_sequence = snapshot.sample_sequence;
    poll_at(3u); /* first sample */
    poll_at(6u); /* settling follow-up starts */
    poll_at(9u); /* second sample qualifies; default next = 5009 */

    ltc2959_hal_get_snapshot(&snapshot);
    assert(snapshot.sample_sequence == base_sequence + 2u);

    poll_at_period(10u, 250u);  /* advance pending deadline to 260 */
    poll_at_period(259u, 250u);
    ltc2959_hal_get_snapshot(&snapshot);
    assert(snapshot.sample_sequence == base_sequence + 2u);
    poll_at_period(260u, 250u); /* conversion starts */
    poll_at_period(262u, 250u);
    ltc2959_hal_get_snapshot(&snapshot);
    assert(snapshot.sample_sequence == base_sequence + 2u);
    poll_at_period(263u, 250u); /* distinct third sample */
    ltc2959_hal_get_snapshot(&snapshot);
    assert(snapshot.sample_sequence == base_sequence + 3u);
}

int main(void) {
    test_stale_sample_recovery_preserves_coulomb_session();
    test_explicit_new_session_uses_one_burst();
    test_uvlo_reseeds_uncertain_acr();
    test_configuration_repair_preserves_acr();
    test_failed_uvlo_reseed_is_retried();
    test_shorter_period_advances_pending_deadline();
    test_ara_evidence_and_pending_retry();
    test_ara_transport_failure_evidence();
    puts("LTC2959 HAL recovery tests passed");
    return 0;
}

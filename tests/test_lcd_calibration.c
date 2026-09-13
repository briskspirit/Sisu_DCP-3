#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "hal/lcd_pcd8544.h"
#include "services/lcd_calibration.h"
#include "storage/store_service.h"

static int s_failures;
static uint8_t s_vop;
static uint8_t s_tc;
static uint8_t s_bias;
static uint8_t s_stored_vop;
static uint32_t s_stored_tuning;
static store_status_t s_get_vop_status;
static store_status_t s_get_tuning_status;
static store_status_t s_set_vop_status;
static store_status_t s_set_tuning_status;
static unsigned s_apply_count;
static unsigned s_store_vop_count;
static unsigned s_store_tuning_count;

static void check(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        s_failures++;
    }
}

bool lcd_set_tuning(uint8_t vop, uint8_t temperature_coefficient,
                    uint8_t bias_system) {
    s_vop = vop;
    s_tc = temperature_coefficient;
    s_bias = bias_system;
    s_apply_count++;
    return true;
}

void lcd_get_tuning(uint8_t *vop, uint8_t *temperature_coefficient,
                    uint8_t *bias_system) {
    if (vop != NULL) {
        *vop = s_vop;
    }
    if (temperature_coefficient != NULL) {
        *temperature_coefficient = s_tc;
    }
    if (bias_system != NULL) {
        *bias_system = s_bias;
    }
}

store_status_t store_setting_get_u8(store_setting_key_t key,
                                    uint8_t *out_value) {
    check(key == STORE_SETTING_SYSTEM_LCD_VOP,
          "u8 read is the legacy LCD Vop mirror");
    if (s_get_vop_status == STORE_STATUS_OK && out_value != NULL) {
        *out_value = s_stored_vop;
    }
    return s_get_vop_status;
}

store_status_t store_setting_set_u8(store_setting_key_t key, uint8_t value) {
    check(key == STORE_SETTING_SYSTEM_LCD_VOP,
          "u8 write is the legacy LCD Vop mirror");
    s_store_vop_count++;
    if (s_set_vop_status == STORE_STATUS_OK) {
        s_stored_vop = value;
    }
    return s_set_vop_status;
}

store_status_t store_setting_get_u32(store_setting_key_t key,
                                     uint32_t *out_value) {
    check(key == STORE_SETTING_SYSTEM_LCD_TUNING,
          "u32 read is the atomic LCD tuple");
    if (s_get_tuning_status == STORE_STATUS_OK && out_value != NULL) {
        *out_value = s_stored_tuning;
    }
    return s_get_tuning_status;
}

store_status_t store_setting_set_u32(store_setting_key_t key, uint32_t value) {
    check(key == STORE_SETTING_SYSTEM_LCD_TUNING,
          "u32 write is the atomic LCD tuple");
    s_store_tuning_count++;
    if (s_set_tuning_status == STORE_STATUS_OK) {
        s_stored_tuning = value;
    }
    return s_set_tuning_status;
}

void log_write(int level, const char *tag, const char *fmt, ...) {
    (void)level;
    (void)tag;
    (void)fmt;
}

static void reset_fixture(void) {
    s_vop = 0u;
    s_tc = 0u;
    s_bias = 0u;
    s_stored_vop = LCD_CALIBRATION_STOCK_VOP;
    s_stored_tuning = LCD_CALIBRATION_STOCK_PACKED;
    s_get_vop_status = STORE_STATUS_OK;
    s_get_tuning_status = STORE_STATUS_OK;
    s_set_vop_status = STORE_STATUS_OK;
    s_set_tuning_status = STORE_STATUS_OK;
    s_apply_count = 0u;
    s_store_vop_count = 0u;
    s_store_tuning_count = 0u;
}

static void test_initialization(void) {
    reset_fixture();
    s_stored_vop = 54u;
    s_stored_tuning = LCD_CALIBRATION_PACK(54u, 3u, 5u);
    check(lcd_calibration_service_init(), "valid stored tuple applies");
    check(s_vop == 54u && s_tc == 3u && s_bias == 5u,
          "stored tuple applies field-for-field");

    reset_fixture();
    s_stored_vop = 54u;
    check(lcd_calibration_service_init(), "Vop-only board migrates");
    check(s_vop == 54u && s_tc == 1u && s_bias == 4u,
          "legacy Vop migration uses stock formula");
    check(s_store_tuning_count == 1u &&
              s_stored_tuning == LCD_CALIBRATION_PACK(54u, 1u, 4u),
          "legacy migration writes atomic tuple");

    reset_fixture();
    s_stored_vop = 128u;
    s_stored_tuning = 0xdeadbeefu;
    check(lcd_calibration_service_init(), "invalid storage falls back");
    check(s_vop == 63u && s_tc == 1u && s_bias == 4u,
          "invalid tuple and Vop fall back to stock");
    check(s_store_tuning_count == 1u &&
              s_stored_tuning == LCD_CALIBRATION_STOCK_PACKED,
          "invalid tuple is healed");

    reset_fixture();
    s_get_vop_status = STORE_STATUS_NOT_READY;
    s_get_tuning_status = STORE_STATUS_NOT_READY;
    check(lcd_calibration_service_init(), "unavailable store still applies");
    check(s_vop == 63u && s_tc == 1u && s_bias == 4u,
          "unavailable store uses stock");
    check(s_store_tuning_count == 0u,
          "unavailable store is not written");
}

static void test_preview_bounds_and_status(void) {
    reset_fixture();
    check(lcd_calibration_preview(0u), "level 0 accepted");
    check(s_vop == 47u, "level 0 maps to Vop 47");
    check(lcd_calibration_preview(31u), "level 31 accepted");
    check(s_vop == 78u, "level 31 maps to Vop 78");
    unsigned before = s_apply_count;
    check(!lcd_calibration_preview(32u), "level 32 rejected");
    check(s_apply_count == before, "rejected level does not touch HAL");

    check(lcd_calibration_preview_tuning(44u, 3u, 7u),
          "full raw tuple accepted");
    check(s_vop == 44u && s_tc == 3u && s_bias == 7u,
          "full raw tuple applies exactly");
    before = s_apply_count;
    check(!lcd_calibration_preview_tuning(128u, 3u, 7u),
          "out-of-range Vop rejected");
    check(!lcd_calibration_preview_tuning(44u, 4u, 7u),
          "out-of-range TC rejected");
    check(!lcd_calibration_preview_tuning(44u, 3u, 8u),
          "out-of-range bias rejected");
    check(s_apply_count == before, "rejected tuple does not touch HAL");

    s_stored_tuning = LCD_CALIBRATION_PACK(54u, 2u, 6u);
    lcd_calibration_status_t status;
    lcd_calibration_get_status(&status);
    check(!status.nokia_level_valid,
          "non-stock formula is outside Nokia level mapping");
    check(status.stored_vop_valid && status.stored_vop == 54u &&
              status.stored_temperature_coefficient == 2u &&
              status.stored_bias_system == 6u,
          "stored tuple reported independently");
    check(lcd_calibration_revert_stored(), "stored tuple can be restored");
    check(s_vop == 54u && s_tc == 2u && s_bias == 6u,
          "revert applies all stored fields");
    check(lcd_calibration_preview_stock(), "stock tuple preview applies");
    check(s_vop == 63u && s_tc == 1u && s_bias == 4u,
          "stock tuple is exact");
}

static void test_save(void) {
    reset_fixture();
    check(lcd_calibration_preview_tuning(54u, 3u, 6u),
          "custom save fixture applies");
    check(lcd_calibration_save_active(), "custom tuple saves");
    check(s_store_tuning_count == 1u &&
              s_stored_tuning == LCD_CALIBRATION_PACK(54u, 3u, 6u),
          "save writes atomic tuple");
    check(s_store_vop_count == 1u && s_stored_vop == 54u,
          "save updates the rollback Vop mirror");

    s_set_tuning_status = STORE_STATUS_STORAGE_ERROR;
    check(!lcd_calibration_save_active(), "tuple store error is reported");
    check(s_store_vop_count == 1u,
          "legacy mirror is not changed after tuple failure");
}

int main(void) {
    test_initialization();
    test_preview_bounds_and_status();
    test_save();
    if (s_failures != 0) {
        fprintf(stderr, "%d failures\n", s_failures);
        return 1;
    }
    printf("lcd_calibration tests passed\n");
    return 0;
}

#include "services/lcd_calibration.h"

#include <string.h>

#include "hal/lcd_pcd8544.h"
#include "services/log.h"
#include "storage/store_service.h"

_Static_assert(LCD_CALIBRATION_VOP_BASE + LCD_CALIBRATION_LEVEL_MAX <=
                   LCD_PCD8544_VOP_MAX,
               "Nokia LCD calibration range exceeds PCD8544 Vop range");
_Static_assert(LCD_CALIBRATION_VOP_MAX == LCD_PCD8544_VOP_MAX,
               "calibration service and LCD HAL Vop ranges differ");

static bool nokia_level_from_tuning(uint8_t vop,
                                    uint8_t temperature_coefficient,
                                    uint8_t bias_system,
                                    uint8_t *out_level) {
    if (out_level == NULL ||
        temperature_coefficient != LCD_CALIBRATION_STOCK_TEMPERATURE_COEFFICIENT ||
        bias_system != LCD_CALIBRATION_STOCK_BIAS_SYSTEM ||
        vop < LCD_CALIBRATION_VOP_BASE ||
        vop > LCD_CALIBRATION_VOP_BASE + LCD_CALIBRATION_LEVEL_MAX) {
        return false;
    }
    *out_level = (uint8_t)(vop - LCD_CALIBRATION_VOP_BASE);
    return true;
}

static bool unpack_tuning(uint32_t packed, uint8_t *vop,
                          uint8_t *temperature_coefficient,
                          uint8_t *bias_system) {
    if ((packed & LCD_CALIBRATION_PACKED_TAG_MASK) !=
        LCD_CALIBRATION_PACKED_TAG) {
        return false;
    }
    uint8_t unpacked_vop = (uint8_t)(packed & 0x7fu);
    uint8_t unpacked_tc = (uint8_t)((packed >> 7u) & 0x03u);
    uint8_t unpacked_bias = (uint8_t)((packed >> 9u) & 0x07u);
    if (unpacked_vop > LCD_PCD8544_VOP_MAX ||
        unpacked_tc > LCD_CALIBRATION_TEMPERATURE_COEFFICIENT_MAX ||
        unpacked_bias > LCD_CALIBRATION_BIAS_SYSTEM_MAX) {
        return false;
    }
    if (vop != NULL) {
        *vop = unpacked_vop;
    }
    if (temperature_coefficient != NULL) {
        *temperature_coefficient = unpacked_tc;
    }
    if (bias_system != NULL) {
        *bias_system = unpacked_bias;
    }
    return true;
}

bool lcd_calibration_service_init(void) {
    uint8_t vop = LCD_CALIBRATION_STOCK_VOP;
    uint8_t tc = LCD_CALIBRATION_STOCK_TEMPERATURE_COEFFICIENT;
    uint8_t bias = LCD_CALIBRATION_STOCK_BIAS_SYSTEM;
    uint8_t legacy_vop = LCD_CALIBRATION_STOCK_VOP;
    store_status_t legacy_status =
        store_setting_get_u8(STORE_SETTING_SYSTEM_LCD_VOP, &vop);
    legacy_vop = vop;

    uint32_t packed = LCD_CALIBRATION_STOCK_PACKED;
    store_status_t packed_status = store_setting_get_u32(
        STORE_SETTING_SYSTEM_LCD_TUNING, &packed);
    bool packed_valid = packed_status == STORE_STATUS_OK &&
                        unpack_tuning(packed, &vop, &tc, &bias);
    bool legacy_valid = legacy_status == STORE_STATUS_OK &&
                        legacy_vop <= LCD_PCD8544_VOP_MAX;

    /* A newly-added key reads its stock default even on an older settings
     * record. If the legacy Vop is non-stock while the tuple is still exactly
     * stock, migrate the deployed panel calibration instead of losing it. New
     * saves mirror Vop into both keys, so this condition cannot override an
     * intentionally saved stock tuple. */
    if (packed_valid && packed == LCD_CALIBRATION_STOCK_PACKED &&
        legacy_valid && legacy_vop != LCD_CALIBRATION_STOCK_VOP) {
        vop = legacy_vop;
        tc = LCD_CALIBRATION_STOCK_TEMPERATURE_COEFFICIENT;
        bias = LCD_CALIBRATION_STOCK_BIAS_SYSTEM;
        packed = LCD_CALIBRATION_PACK(vop, tc, bias);
        (void)store_setting_set_u32(STORE_SETTING_SYSTEM_LCD_TUNING, packed);
    } else if (!packed_valid) {
        if (legacy_valid) {
            vop = legacy_vop;
        } else {
            vop = LCD_CALIBRATION_STOCK_VOP;
        }
        tc = LCD_CALIBRATION_STOCK_TEMPERATURE_COEFFICIENT;
        bias = LCD_CALIBRATION_STOCK_BIAS_SYSTEM;
        LOGW("lcd", "invalid calibration tuple; using %u/%u/%u",
             (unsigned)vop, (unsigned)tc, (unsigned)bias);
        if (packed_status == STORE_STATUS_OK) {
            (void)store_setting_set_u32(
                STORE_SETTING_SYSTEM_LCD_TUNING,
                LCD_CALIBRATION_PACK(vop, tc, bias));
        }
    }
    return lcd_calibration_preview_tuning(vop, tc, bias);
}

bool lcd_calibration_preview(uint8_t level) {
    if (level > LCD_CALIBRATION_LEVEL_MAX) {
        return false;
    }
    return lcd_calibration_preview_vop(
        (uint8_t)(LCD_CALIBRATION_VOP_BASE + level));
}

bool lcd_calibration_preview_vop(uint8_t vop) {
    return lcd_calibration_preview_tuning(
        vop, LCD_CALIBRATION_STOCK_TEMPERATURE_COEFFICIENT,
        LCD_CALIBRATION_STOCK_BIAS_SYSTEM);
}

bool lcd_calibration_preview_tuning(uint8_t vop,
                                    uint8_t temperature_coefficient,
                                    uint8_t bias_system) {
    if (vop > LCD_PCD8544_VOP_MAX ||
        temperature_coefficient >
            LCD_CALIBRATION_TEMPERATURE_COEFFICIENT_MAX ||
        bias_system > LCD_CALIBRATION_BIAS_SYSTEM_MAX) {
        return false;
    }
    return lcd_set_tuning(vop, temperature_coefficient, bias_system);
}

bool lcd_calibration_preview_stock(void) {
    return lcd_calibration_preview_tuning(
        LCD_CALIBRATION_STOCK_VOP,
        LCD_CALIBRATION_STOCK_TEMPERATURE_COEFFICIENT,
        LCD_CALIBRATION_STOCK_BIAS_SYSTEM);
}

bool lcd_calibration_revert_stored(void) {
    uint32_t packed = 0u;
    uint8_t vop = 0u;
    uint8_t tc = 0u;
    uint8_t bias = 0u;
    if (store_setting_get_u32(STORE_SETTING_SYSTEM_LCD_TUNING, &packed) !=
            STORE_STATUS_OK ||
        !unpack_tuning(packed, &vop, &tc, &bias)) {
        return lcd_calibration_preview_stock();
    }
    return lcd_calibration_preview_tuning(vop, tc, bias);
}

bool lcd_calibration_save_active(void) {
    lcd_calibration_status_t status;
    lcd_calibration_get_status(&status);
    uint32_t packed = LCD_CALIBRATION_PACK(
        status.vop, status.temperature_coefficient, status.bias_system);
    if (store_setting_set_u32(STORE_SETTING_SYSTEM_LCD_TUNING, packed) !=
        STORE_STATUS_OK) {
        return false;
    }
    /* Migration mirror: keeps older firmware from reverting a calibrated
     * panel's Vop if a board is temporarily flashed back. */
    return store_setting_set_u8(STORE_SETTING_SYSTEM_LCD_VOP, status.vop) ==
           STORE_STATUS_OK;
}

void lcd_calibration_get_status(lcd_calibration_status_t *out_status) {
    if (out_status == NULL) {
        return;
    }
    memset(out_status, 0, sizeof(*out_status));
    lcd_get_tuning(&out_status->vop,
                   &out_status->temperature_coefficient,
                   &out_status->bias_system);
    out_status->nokia_level_valid =
        nokia_level_from_tuning(out_status->vop,
                                out_status->temperature_coefficient,
                                out_status->bias_system,
                                &out_status->nokia_level);

    uint32_t packed = 0u;
    uint8_t stored_vop = 0u;
    uint8_t stored_tc = 0u;
    uint8_t stored_bias = 0u;
    if (store_setting_get_u32(STORE_SETTING_SYSTEM_LCD_TUNING, &packed) ==
            STORE_STATUS_OK &&
        unpack_tuning(packed, &stored_vop, &stored_tc, &stored_bias)) {
        out_status->stored_vop_valid = true;
        out_status->stored_vop = stored_vop;
        out_status->stored_temperature_coefficient = stored_tc;
        out_status->stored_bias_system = stored_bias;
    }
}

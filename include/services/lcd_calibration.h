#ifndef LCD_CALIBRATION_H
#define LCD_CALIBRATION_H

#include <stdbool.h>
#include <stdint.h>

/* Nokia NSE-8 v6.00 stores a five-bit calibration level and derives Vop as
 * 47 + level. TC1 and bias 4 are fixed by the original firmware. */
#define LCD_CALIBRATION_LEVEL_MAX 31u
#define LCD_CALIBRATION_LEVEL_DEFAULT 16u
#define LCD_CALIBRATION_VOP_BASE 47u
#define LCD_CALIBRATION_VOP_MAX 127u
#define LCD_CALIBRATION_STOCK_VOP \
    (LCD_CALIBRATION_VOP_BASE + LCD_CALIBRATION_LEVEL_DEFAULT)
#define LCD_CALIBRATION_STOCK_TEMPERATURE_COEFFICIENT 1u
#define LCD_CALIBRATION_STOCK_BIAS_SYSTEM 4u
#define LCD_CALIBRATION_TEMPERATURE_COEFFICIENT_MAX 3u
#define LCD_CALIBRATION_BIAS_SYSTEM_MAX 7u
#define LCD_CALIBRATION_PACKED_TAG 0x4c010000u
#define LCD_CALIBRATION_PACKED_TAG_MASK 0xfffff000u
#define LCD_CALIBRATION_PACK(vop_, tc_, bias_) \
    (LCD_CALIBRATION_PACKED_TAG | ((uint32_t)(bias_) << 9u) | \
     ((uint32_t)(tc_) << 7u) | (uint32_t)(vop_))
#define LCD_CALIBRATION_STOCK_PACKED \
    LCD_CALIBRATION_PACK(LCD_CALIBRATION_STOCK_VOP, \
                         LCD_CALIBRATION_STOCK_TEMPERATURE_COEFFICIENT, \
                         LCD_CALIBRATION_STOCK_BIAS_SYSTEM)

typedef struct {
    bool nokia_level_valid;
    uint8_t nokia_level;
    bool stored_vop_valid;
    uint8_t stored_vop;
    uint8_t stored_temperature_coefficient;
    uint8_t stored_bias_system;
    uint8_t vop;
    uint8_t temperature_coefficient;
    uint8_t bias_system;
} lcd_calibration_status_t;

/* Call after both lcd_init() and store_service_init(). Applies the versioned
 * Vop/TC/bias tuple, migrates a deployed Vop-only setting, or uses stock. */
bool lcd_calibration_service_init(void);

/* Apply either a Nokia five-bit level or a raw controller Vop immediately,
 * always using the stock TC/bias profile. Both are previews until saved. */
bool lcd_calibration_preview(uint8_t level);
bool lcd_calibration_preview_vop(uint8_t vop);
bool lcd_calibration_preview_tuning(uint8_t vop,
                                    uint8_t temperature_coefficient,
                                    uint8_t bias_system);
bool lcd_calibration_preview_stock(void);
bool lcd_calibration_revert_stored(void);
bool lcd_calibration_save_active(void);
void lcd_calibration_get_status(lcd_calibration_status_t *out_status);

#endif

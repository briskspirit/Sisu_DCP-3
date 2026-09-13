#ifndef LCD_PCD8544_H
#define LCD_PCD8544_H

#include <stdbool.h>
#include <stdint.h>

#include "ui/framebuffer.h"

typedef struct {
    bool rotate_180;
    uint8_t out[FB_SIZE];
} lcd_pcd8544_t;

#define LCD_PCD8544_VOP_MAX 0x7fu
#define LCD_PCD8544_TEMP_COEFFICIENT_MAX 3u
#define LCD_PCD8544_BIAS_SYSTEM_MAX 7u

void lcd_init(lcd_pcd8544_t *lcd);
void lcd_show(lcd_pcd8544_t *lcd, const framebuffer_t *fb);
void lcd_power_down(void);
void lcd_power_up(void);
/* Reapply the full controller configuration without resetting or clearing
 * display RAM. The current power-down state is preserved. */
bool lcd_reapply_configuration(void);
/* Atomically update controller tuning, then reapply the full configuration.
 * Values are volatile; persistence belongs above the HAL. */
bool lcd_set_tuning(uint8_t vop, uint8_t temperature_coefficient,
                    uint8_t bias_system);
void lcd_get_tuning(uint8_t *vop, uint8_t *temperature_coefficient,
                    uint8_t *bias_system);
bool lcd_is_powered_down(void);

#endif

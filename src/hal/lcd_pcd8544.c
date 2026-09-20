#include "hal/lcd_pcd8544.h"

#include "hardware/gpio.h"
#include "hardware/spi.h"
#include "hal/board.h"
#include "pico/stdlib.h"

#define LCD_COMMAND 0u
#define LCD_DATA 1u

static uint8_t bitrev8(uint8_t value);
static void lcd_write(uint8_t kind, const uint8_t *data, size_t len);
static void lcd_cmd(uint8_t value);
static void lcd_set_pos(uint8_t x, uint8_t bank);
static void lcd_apply_configuration(void);

static uint8_t s_vop = LCD_CONTRAST & LCD_PCD8544_VOP_MAX;
static uint8_t s_temperature_coefficient =
    LCD_TEMPERATURE_COEFFICIENT & LCD_PCD8544_TEMP_COEFFICIENT_MAX;
static uint8_t s_bias_system = LCD_BIAS_SYSTEM & LCD_PCD8544_BIAS_SYSTEM_MAX;
static bool s_initialized;
static bool s_powered_down;

void lcd_init(lcd_pcd8544_t *lcd) {
    lcd_init_powered_down(lcd);
    lcd_power_up();
}

void lcd_init_powered_down(lcd_pcd8544_t *lcd) {
    lcd->rotate_180 = LCD_ROTATE_180 != 0u;

    gpio_init(LCD_PIN_CS);
    gpio_set_dir(LCD_PIN_CS, GPIO_OUT);
    gpio_put(LCD_PIN_CS, 1);
    gpio_init(LCD_PIN_DC);
    gpio_set_dir(LCD_PIN_DC, GPIO_OUT);
    gpio_put(LCD_PIN_DC, 0);
    gpio_init(LCD_PIN_RST);
    gpio_set_dir(LCD_PIN_RST, GPIO_OUT);

    gpio_put(LCD_PIN_RST, 0);
    sleep_ms(20);
    gpio_put(LCD_PIN_RST, 1);
    sleep_ms(20);

    s_vop = LCD_CONTRAST & LCD_PCD8544_VOP_MAX;
    s_temperature_coefficient =
        LCD_TEMPERATURE_COEFFICIENT & LCD_PCD8544_TEMP_COEFFICIENT_MAX;
    s_bias_system = LCD_BIAS_SYSTEM & LCD_PCD8544_BIAS_SYSTEM_MAX;
    /* Reset leaves PD asserted. Keep it set while configuring and clearing
     * undefined RAM, so an off-state wake cannot briefly activate the panel. */
    s_powered_down = true;
    s_initialized = true;
    lcd_apply_configuration();

    framebuffer_t blank;
    fb_clear(&blank, false);
    lcd_show(lcd, &blank);
}

void lcd_show(lcd_pcd8544_t *lcd, const framebuffer_t *fb) {
    lcd_set_pos(0, 0);
    if (lcd->rotate_180) {
        for (uint16_t i = 0; i < FB_SIZE; i++) {
            lcd->out[i] = bitrev8(fb->data[FB_SIZE - 1u - i]);
        }
        lcd_write(LCD_DATA, lcd->out, FB_SIZE);
    } else {
        lcd_write(LCD_DATA, fb->data, FB_SIZE);
    }
}

void lcd_power_down(void) {
    if (s_initialized) {
        lcd_cmd(0x24);
        s_powered_down = true;
    }
}

void lcd_power_up(void) {
    if (!s_initialized) {
        return;
    }
    s_powered_down = false;
    lcd_apply_configuration();
}

bool lcd_reapply_configuration(void) {
    if (!s_initialized) {
        return false;
    }
    lcd_apply_configuration();
    return true;
}

bool lcd_set_tuning(uint8_t vop, uint8_t temperature_coefficient,
                    uint8_t bias_system) {
    if (!s_initialized ||
        vop > LCD_PCD8544_VOP_MAX ||
        temperature_coefficient > LCD_PCD8544_TEMP_COEFFICIENT_MAX ||
        bias_system > LCD_PCD8544_BIAS_SYSTEM_MAX) {
        return false;
    }
    s_vop = vop;
    s_temperature_coefficient = temperature_coefficient;
    s_bias_system = bias_system;
    lcd_apply_configuration();
    return true;
}

void lcd_get_tuning(uint8_t *vop, uint8_t *temperature_coefficient,
                    uint8_t *bias_system) {
    if (vop != NULL) {
        *vop = s_vop;
    }
    if (temperature_coefficient != NULL) {
        *temperature_coefficient = s_temperature_coefficient;
    }
    if (bias_system != NULL) {
        *bias_system = s_bias_system;
    }
}

bool lcd_is_powered_down(void) {
    return s_powered_down;
}

static uint8_t bitrev8(uint8_t value) {
    value = (uint8_t)(((value & 0x55u) << 1) | ((value & 0xaau) >> 1));
    value = (uint8_t)(((value & 0x33u) << 2) | ((value & 0xccu) >> 2));
    value = (uint8_t)(((value & 0x0fu) << 4) | ((value & 0xf0u) >> 4));
    return value;
}

static void lcd_write(uint8_t kind, const uint8_t *data, size_t len) {
    gpio_put(LCD_PIN_DC, kind == LCD_DATA);
    gpio_put(LCD_PIN_CS, 0);
    spi_write_blocking(LCD_SPI_PORT, data, len);
    gpio_put(LCD_PIN_CS, 1);
}

static void lcd_cmd(uint8_t value) {
    lcd_write(LCD_COMMAND, &value, 1);
}

static void lcd_set_pos(uint8_t x, uint8_t bank) {
    lcd_cmd((uint8_t)(0x80u | (x & 0x7fu)));
    lcd_cmd((uint8_t)(0x40u | (bank & 0x07u)));
}

static void lcd_apply_configuration(void) {
    /* Keep PD asserted while changing extended registers if the display is
     * asleep. In the normal case this is the standard extended -> basic
     * sequence. Register order matches Nokia NSE-8 v6.00 exactly: TC, bias,
     * then Vop. No hardware reset is issued, so display RAM is preserved. */
    lcd_cmd(s_powered_down ? 0x25u : 0x21u);
    lcd_cmd((uint8_t)(0x04u | s_temperature_coefficient));
    lcd_cmd((uint8_t)(0x10u | s_bias_system));
    lcd_cmd((uint8_t)(0x80u | s_vop));
    lcd_cmd(s_powered_down ? 0x24u : 0x20u);
    if (!s_powered_down) {
        lcd_cmd(0x0cu);
    }
}

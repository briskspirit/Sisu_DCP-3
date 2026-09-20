#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "hal/board.h"
#include "hal/lcd_pcd8544.h"
#include "hardware/gpio.h"
#include "hardware/spi.h"

struct spi_inst { unsigned unused; };
static spi_inst_t s_spi;
spi_inst_t *const spi0 = &s_spi;
uint64_t host_test_time_us;

static int s_failures;
static bool s_pins[48];
static bool s_pd;
static bool s_extended;
static bool s_normal;
static unsigned s_resets;
static unsigned s_activations;
static unsigned s_activation_before_clear;
static unsigned s_data_bytes;
static size_t s_pos;
static uint8_t s_ram[FB_SIZE];
static uint8_t s_vop;
static uint8_t s_tc;
static uint8_t s_bias;

static void check(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        s_failures++;
    }
}

void gpio_init(unsigned int gpio) { (void)gpio; }
void gpio_set_dir(unsigned int gpio, bool output) {
    (void)gpio;
    (void)output;
}
void gpio_put(unsigned int gpio, bool value) {
    check(gpio < sizeof(s_pins) / sizeof(s_pins[0]), "valid GPIO");
    if (gpio >= sizeof(s_pins) / sizeof(s_pins[0])) return;
    s_pins[gpio] = value;
    if (gpio == LCD_PIN_RST && !value) {
        s_resets++;
        s_pd = true;
        s_extended = false;
        s_normal = false;
        s_pos = 0u;
        s_data_bytes = 0u;
        memset(s_ram, 0xa5, sizeof(s_ram));
    }
}

int spi_write_blocking(spi_inst_t *spi, const uint8_t *src, size_t len) {
    check(spi == spi0 && !s_pins[LCD_PIN_CS] && s_pins[LCD_PIN_RST],
          "SPI writes select the released LCD");
    for (size_t i = 0u; i < len; ++i) {
        uint8_t byte = src[i];
        if (s_pins[LCD_PIN_DC]) {
            s_ram[s_pos] = byte;
            s_pos = (s_pos + 1u) % FB_SIZE;
            s_data_bytes++;
        } else if ((byte & 0xf8u) == 0x20u) {
            bool next_pd = (byte & 0x04u) != 0u;
            if (s_pd && !next_pd) {
                s_activations++;
                if (s_data_bytes < FB_SIZE) s_activation_before_clear++;
            }
            s_pd = next_pd;
            s_extended = (byte & 0x01u) != 0u;
        } else if (s_extended) {
            if ((byte & 0x80u) != 0u) s_vop = byte & 0x7fu;
            else if ((byte & 0xf8u) == 0x10u) s_bias = byte & 0x07u;
            else if ((byte & 0xfcu) == 0x04u) s_tc = byte & 0x03u;
        } else if ((byte & 0x80u) != 0u) {
            s_pos = (s_pos / FB_WIDTH) * FB_WIDTH + (byte & 0x7fu);
        } else if ((byte & 0xf8u) == 0x40u) {
            s_pos = (byte & 0x07u) * FB_WIDTH + s_pos % FB_WIDTH;
        } else if ((byte & 0xf8u) == 0x08u) {
            s_normal = byte == 0x0cu;
        }
        check(s_pos < FB_SIZE, "display address is in range");
    }
    return (int)len;
}

int main(void) {
    lcd_pcd8544_t lcd;
    framebuffer_t fb;
    lcd_init_powered_down(&lcd);
    check(lcd_is_powered_down() && s_pd && !s_normal && s_activations == 0u,
          "off-state init never activates panel outputs");
    check(s_resets == 1u && s_data_bytes == FB_SIZE,
          "off-state init resets and clears the complete display RAM");
    fb_clear(&fb, false);
    check(memcmp(s_ram, fb.data, FB_SIZE) == 0, "undefined reset RAM is cleared");
    check(s_vop == LCD_CONTRAST && s_tc == LCD_TEMPERATURE_COEFFICIENT &&
              s_bias == LCD_BIAS_SYSTEM, "default tuning is applied while asleep");
    check(lcd_set_tuning(64u, 2u, 3u) && lcd_reapply_configuration(),
          "persisted calibration can be applied before power-on");
    check(s_vop == 64u && s_tc == 2u && s_bias == 3u && s_pd &&
              s_activations == 0u && s_resets == 1u,
          "calibration neither wakes nor resets the display");

    lcd_power_up();
    check(!lcd_is_powered_down() && !s_pd && s_normal && s_activations == 1u,
          "explicit wake enables the tuned display in normal mode");
    check(s_vop == 64u && s_tc == 2u && s_bias == 3u && s_resets == 1u,
          "wake preserves calibration and avoids another reset");
    fb.data[0] = 0x81u;
    fb.data[FB_SIZE - 1u] = 0x03u;
    lcd_show(&lcd, &fb);
    check(memcmp(s_ram, fb.data, FB_SIZE) == 0,
          "accepted wake can show a complete frame");
    lcd.rotate_180 = true;
    lcd_show(&lcd, &fb);
    check(s_ram[0] == 0xc0u && s_ram[FB_SIZE - 1u] == 0x81u,
          "rotation still reverses byte order and bits");
    fb_clear(&fb, false);
    lcd_show(&lcd, &fb);
    lcd_power_down();
    check(s_pd && lcd_is_powered_down(), "normal off transition powers down");
    check(lcd_reapply_configuration() && s_pd && s_activations == 1u,
          "later configuration refresh remains asleep");

    lcd_init(&lcd);
    check(!s_pd && !lcd_is_powered_down() && s_normal && s_activations == 2u,
          "diagnostic init still returns a powered-up display");
    check(s_activation_before_clear == 0u &&
              memcmp(s_ram, fb.data, FB_SIZE) == 0,
          "all init paths clear RAM before enabling the panel");
    if (s_failures != 0) return 1;
    puts("PASS: test_lcd_pcd8544");
    return 0;
}

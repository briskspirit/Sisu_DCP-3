#include <stdio.h>

#include "hal/board.h"
#include "hal/keypad.h"
#include "hal/lcd_pcd8544.h"
#include "hardware/gpio.h"
#include "pico/stdlib.h"
#include "ui/framebuffer.h"

static void draw_status(lcd_pcd8544_t *lcd, framebuffer_t *fb,
                        uint16_t keys, bool power_pressed) {
    char line[24];

    fb_clear(fb, false);
    fb_text5(fb, "REV B2 SAFE", 0, 0, true, FB_WIDTH);

    snprintf(line, sizeof line, "PG:%u VBUS:%u",
             board_3v8_rail_power_good() ? 1u : 0u,
             board_service_vbus_present() ? 1u : 0u);
    fb_text5(fb, line, 0, 10, true, FB_WIDTH);

    snprintf(line, sizeof line, "SYS:%u PWR:%u",
             gpio_get(SYS_INT_PIN) ? 1u : 0u,
             power_pressed ? 1u : 0u);
    fb_text5(fb, line, 0, 20, true, FB_WIDTH);

    snprintf(line, sizeof line, "KEY:%04X", (unsigned)keys);
    fb_text5(fb, line, 0, 30, true, FB_WIDTH);
    fb_text5(fb, "MODEM OFF", 0, 40, true, FB_WIDTH);
    lcd_show(lcd, fb);
}

int main(void) {
    board_set_system_clock();
    board_init();
    board_3v8_rail_set_enabled(false);
    board_3v8_rail_set_force_pwm(false);
    stdio_init_all();

    keypad_t keypad;
    keypad_init(&keypad);
    gpio_init(POWER_BUTTON_PIN);
    gpio_set_dir(POWER_BUTTON_PIN, GPIO_IN);
    gpio_pull_up(POWER_BUTTON_PIN);

    lcd_pcd8544_t lcd;
    framebuffer_t fb;
    lcd_init(&lcd);
    board_set_backlight(true);

    uint16_t previous_keys = UINT16_MAX;
    bool previous_power = false;
    bool first = true;
    while (true) {
        uint16_t keys = keypad_scan_raw();
        bool power_pressed = !gpio_get(POWER_BUTTON_PIN);
        if (first || keys != previous_keys || power_pressed != previous_power) {
            draw_status(&lcd, &fb, keys, power_pressed);
            printf("[revb2-safe] PG=%u VBUS=%u SYS=%u PWR=%u KEY=%04x\n",
                   board_3v8_rail_power_good() ? 1u : 0u,
                   board_service_vbus_present() ? 1u : 0u,
                   gpio_get(SYS_INT_PIN) ? 1u : 0u,
                   power_pressed ? 1u : 0u, (unsigned)keys);
            first = false;
            previous_keys = keys;
            previous_power = power_pressed;
        }
        sleep_ms(20u);
    }
}

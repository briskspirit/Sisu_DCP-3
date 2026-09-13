#include "hal/board_safe_gpio.h"

#include <stddef.h>

#include "hal/board.h"

typedef struct {
    uint8_t pin;
    bool inactive_level;
} safe_output_t;

typedef struct {
    uint8_t pin;
    bool input_enabled;
} safe_input_t;

void board_safe_gpio_apply(const board_safe_gpio_ops_t *ops) {
    static const safe_output_t outputs[] = {
        {BUZZER_PIN, BUZZER_ACTIVE_LOW != 0u},
        {PMIC_3V8_PIN_ENABLE, false},
        {VIBRA_PIN, VIBRA_ACTIVE_LOW != 0u},
        {LCD_PIN_BACKLIGHT, LCD_BACKLIGHT_ACTIVE_LOW != 0u},
        /* Keep the controller deselected and in reset until lcd_init() has
         * configured SPI and is ready to clear display RAM. This prevents the
         * unpowered-phone all-pixels haze seen while these pins floated during
         * the comparatively long cold-boot path. */
        {LCD_PIN_CS, true},
        {LCD_PIN_RST, false},
        {LCD_PIN_DC, false},
        {LCD_PIN_SCK, false},
        {LCD_PIN_MOSI, false},
        {AUDIO_CODEC_PIN_BCLK, false},
        {AUDIO_CODEC_PIN_LRC, false},
        {AUDIO_CODEC_PIN_DIN, false},
        {AUDIO_CODEC_PIN_MCLK, false},
        {PMIC_3V8_PIN_FORCE_PWM, false},
        {MODEM_PIN_DTR, false},
        {MODEM_PIN_ON_OFF, false},
        {MODEM_PIN_HW_SHUTDOWN, false},
    };
    static const safe_input_t inputs[] = {
        {MODEM_PIN_TX, false},
        {MODEM_PIN_RX, false},
        {MODEM_PIN_CTS, false},
        {MODEM_PIN_RTS, false},
        {PMIC_3V8_PIN_POWER_GOOD, true},
        {MODEM_I2S_PIN_CLK, false},
        {MODEM_I2S_PIN_WA, false},
        {MODEM_I2S_PIN_TXD, false},
        {MODEM_I2S_PIN_RXD, false},
        {AUDIO_CODEC_PIN_ADCOUT, false},
        {26u, false},
        {27u, false},
        {SERVICE_VBUS_PIN, true},
        {29u, false},
        {30u, false},
        {31u, false},
        {32u, false},
        {34u, false},
        {35u, false},
        {MODEM_PIN_RI, false},
        {MODEM_PIN_STATUS_ADC, false},
        {HEADSET_HOOK_ADC_PIN, false},
        {SYS_INT_PIN, true},
        {CHARGER_INPUT_ADC_PIN, false},
        {44u, false},
        {45u, false},
        {46u, false},
        {47u, false},
    };

    if (ops == NULL || ops->init == NULL || ops->put == NULL ||
        ops->set_dir == NULL || ops->disable_pulls == NULL ||
        ops->set_input_enabled == NULL) {
        return;
    }

    for (size_t i = 0u; i < sizeof outputs / sizeof outputs[0]; i++) {
        ops->init(outputs[i].pin);
        ops->put(outputs[i].pin, outputs[i].inactive_level);
        ops->disable_pulls(outputs[i].pin);
        ops->set_dir(outputs[i].pin, true);
        ops->set_input_enabled(outputs[i].pin, false);
    }
    for (size_t i = 0u; i < sizeof inputs / sizeof inputs[0]; i++) {
        ops->init(inputs[i].pin);
        ops->set_dir(inputs[i].pin, false);
        ops->disable_pulls(inputs[i].pin);
        ops->set_input_enabled(
            inputs[i].pin, inputs[i].input_enabled);
    }
}

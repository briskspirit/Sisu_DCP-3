#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "hal/board.h"
#include "hal/board_safe_gpio.h"

typedef enum {
    EVENT_INIT = 0,
    EVENT_PUT,
    EVENT_DIR,
    EVENT_PULLS_OFF,
    EVENT_INPUT_ENABLE,
} event_kind_t;

typedef struct {
    event_kind_t kind;
    uint8_t pin;
    bool value;
} event_t;

static event_t s_events[256];
static size_t s_event_count;
static int s_failures;

static void record(event_kind_t kind, uint8_t pin, bool value) {
    if (s_event_count < sizeof s_events / sizeof s_events[0]) {
        s_events[s_event_count++] = (event_t){kind, pin, value};
    }
}

static void mock_init(uint8_t pin) {
    record(EVENT_INIT, pin, false);
}

static void mock_put(uint8_t pin, bool level) {
    record(EVENT_PUT, pin, level);
}

static void mock_set_dir(uint8_t pin, bool output) {
    record(EVENT_DIR, pin, output);
}

static void mock_disable_pulls(uint8_t pin) {
    record(EVENT_PULLS_OFF, pin, false);
}

static void mock_set_input_enabled(uint8_t pin, bool enabled) {
    record(EVENT_INPUT_ENABLE, pin, enabled);
}

static void assert_true(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        s_failures++;
    }
}

static int first_event(event_kind_t kind, uint8_t pin, bool value) {
    for (size_t i = 0u; i < s_event_count; i++) {
        if (s_events[i].kind == kind && s_events[i].pin == pin &&
            s_events[i].value == value) {
            return (int)i;
        }
    }
    return -1;
}

static void assert_safe_output(uint8_t pin, bool level, const char *name) {
    int put = first_event(EVENT_PUT, pin, level);
    int dir = first_event(EVENT_DIR, pin, true);
    assert_true(put >= 0, name);
    assert_true(dir >= 0, name);
    assert_true(put >= 0 && dir >= 0 && put < dir,
                "inactive latch must precede output enable");
    assert_true(first_event(EVENT_INPUT_ENABLE, pin, false) >= 0,
                "output input receiver must be disabled");
}

static void assert_safe_input(uint8_t pin, bool enabled,
                              const char *name) {
    assert_true(first_event(EVENT_DIR, pin, false) >= 0, name);
    assert_true(first_event(EVENT_DIR, pin, true) < 0, name);
    assert_true(
        first_event(EVENT_INPUT_ENABLE, pin, enabled) >= 0, name);
}

static void test_revb2_safe_sequence(void) {
    const board_safe_gpio_ops_t ops = {
        .init = mock_init,
        .put = mock_put,
        .set_dir = mock_set_dir,
        .disable_pulls = mock_disable_pulls,
        .set_input_enabled = mock_set_input_enabled,
    };
    board_safe_gpio_apply(&ops);

    assert_safe_output(
        PMIC_3V8_PIN_ENABLE, false, "GP6 shared +3V8 rail is low");
    assert_safe_output(VIBRA_PIN, false, "GP22 vibra is low");
    assert_safe_output(
        LCD_PIN_BACKLIGHT, false, "GP23 backlight is low");
    assert_safe_output(
        PMIC_3V8_PIN_FORCE_PWM, false, "GP33 PWM force is low");
    assert_safe_output(MODEM_PIN_DTR, false, "GP36 DTR is low");
    assert_safe_output(
        MODEM_PIN_ON_OFF, false, "GP38 ON_OFF is deasserted");
    assert_safe_output(
        MODEM_PIN_HW_SHUTDOWN, false,
        "GP39 HW_SHUTDOWN is deasserted");
    assert_safe_output(
        LCD_PIN_CS, true, "LCD is deselected during cold boot");
    assert_safe_output(
        LCD_PIN_RST, false, "LCD stays in reset during cold boot");
    assert_safe_output(
        LCD_PIN_DC, false, "LCD command/data pin is held low");
    assert_safe_output(
        LCD_PIN_SCK, false, "LCD clock is held at SPI mode-0 idle");
    assert_safe_output(
        LCD_PIN_MOSI, false, "LCD data is held low during cold boot");

    assert_safe_input(
        PMIC_3V8_PIN_POWER_GOOD, true,
        "GP4 power-good remains a live digital input");
    assert_safe_input(
        SERVICE_VBUS_PIN, true,
        "GP28 service VBUS remains a live digital input");
    assert_safe_input(
        SYS_INT_PIN, true,
        "GP42 shared interrupt remains a live digital input");
    assert_safe_input(
        MODEM_PIN_RI, false,
        "GP37 RI is quiet while the modem is unpowered");
    assert_safe_input(
        CHARGER_INPUT_ADC_PIN, false,
        "GP43 charger sense is parked for ADC ownership");
    assert_safe_input(
        AUDIO_CODEC_PIN_ADCOUT, false,
        "codec ADCOUT is quiet until the I2S owner starts");

    static const uint8_t unconnected_pins[] = {
        26u, 27u, 29u, 30u, 31u, 32u, 34u, 35u, 44u, 45u, 46u, 47u,
    };
    for (size_t i = 0u;
         i < sizeof unconnected_pins / sizeof unconnected_pins[0];
         i++) {
        assert_safe_input(
            unconnected_pins[i], false,
            "unused Rev B2 pin is parked as a quiet input");
    }
}

static void test_revb2_pin_map(void) {
    assert_true(MODEM_PIN_TX == 0u && MODEM_PIN_RX == 1u,
                "UART data pins match Rev B2");
    assert_true(MODEM_PIN_CTS == 2u && MODEM_PIN_RTS == 3u,
                "GP2 is CTS input and GP3 is RTS output");
    assert_true(PMIC_3V8_PIN_POWER_GOOD == 4u &&
                    PMIC_3V8_PIN_ENABLE == 6u,
                "PMIC power-good/enable pins match Rev B2");
    assert_true(AUDIO_CODEC_PIN_ADCOUT == 12u &&
                    AUDIO_CODEC_PIN_LRC == 13u &&
                    AUDIO_CODEC_PIN_BCLK == 14u &&
                    AUDIO_CODEC_PIN_DIN == 15u,
                "codec pins match Rev B2");
    assert_true(VIBRA_PIN == 22u && LCD_PIN_BACKLIGHT == 23u,
                "vibra/backlight pins match Rev B2");
    assert_true(SERVICE_VBUS_PIN == 28u &&
                    PMIC_3V8_PIN_FORCE_PWM == 33u,
                "direct VBUS/PWM pins match Rev B2");
    assert_true(MODEM_PIN_DTR == 36u && MODEM_PIN_RI == 37u &&
                    MODEM_PIN_ON_OFF == 38u &&
                    MODEM_PIN_HW_SHUTDOWN == 39u,
                "modem control pins match Rev B2");
    assert_true(MODEM_PIN_STATUS_ADC == 40u &&
                    HEADSET_HOOK_ADC_PIN == 41u &&
                    SYS_INT_PIN == 42u &&
                    CHARGER_INPUT_ADC_PIN == 43u,
                "high-bank observation pins match Rev B2");
}

int main(void) {
    test_revb2_pin_map();
    test_revb2_safe_sequence();
    board_safe_gpio_apply(NULL);

    if (s_failures != 0) {
        fprintf(stderr, "%d failures\n", s_failures);
        return 1;
    }
    printf("board_safe_gpio tests passed\n");
    return 0;
}

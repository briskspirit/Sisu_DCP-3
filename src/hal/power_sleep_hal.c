#include "hal/power_sleep_hal.h"

#include <stddef.h>

#include "sisu_build_config.h"
#include "hal/board.h"
#include "hal/board_irq_hal.h"
#include "hal/service_vbus_logic.h"
#include "hardware/gpio.h"
#include "hardware/powman.h"
#include "hardware/regs/powman.h"
#include "hardware/structs/powman.h"

#define POWER_SLEEP_BUTTON_WAKE_SLOT 0u
#define POWER_SLEEP_SHARED_WAKE_SLOT 1u
#define POWER_SLEEP_VBUS_WAKE_SLOT 2u
#define POWER_SLEEP_UNUSED_WAKE_SLOT 3u
#if !SISU_RELEASE_BUILD
static service_vbus_filter_t s_vbus_filter;
#endif

/* PWRUP lives in the always-on domain, so disabling a slot does not restore
 * MODE/SOURCE and an edge STATUS latch survives the switched-core cycle.
 * Normalize the complete slot state before every arm/disarm; stale ownership
 * or polarity must never leak from a prior image or dormant entry. */
static void reset_wakeup_slots(void) {
    powman_disable_all_wakeups();
    for (uint slot = 0u; slot < 4u; slot++) {
        powman_clear_bits(&powman_hw->pwrup[slot],
                          POWMAN_PWRUP0_STATUS_BITS);
        powman_hw->pwrup[slot] = POWMAN_PASSWORD_BITS |
                                 POWMAN_PWRUP0_RESET;
    }
}

void power_sleep_hal_init(uint32_t now_ms) {
    (void)now_ms;
    gpio_init(SERVICE_VBUS_PIN);
    gpio_set_dir(SERVICE_VBUS_PIN, GPIO_IN);
    gpio_disable_pulls(SERVICE_VBUS_PIN);
#if !SISU_RELEASE_BUILD
    service_vbus_filter_init(&s_vbus_filter, board_service_vbus_present());
#endif
    board_irq_hal_init();
}

void power_sleep_hal_poll(uint32_t now_ms) {
#if SISU_RELEASE_BUILD
    (void)now_ms;
#else
    /* Consume the edge hint before sampling. If another edge arrives while the
     * filter is updated, leave it latched for the next core-0 poll. */
    (void)board_irq_hal_take_service_vbus_pending();
    (void)service_vbus_filter_update(
        &s_vbus_filter,
        board_service_vbus_present(),
        now_ms,
        NULL
    );
#endif
}

bool power_sleep_hal_service_vbus_raw(void) {
    return board_service_vbus_present();
}

bool power_sleep_hal_service_vbus_present(void) {
#if SISU_RELEASE_BUILD
    return false;
#else
    return s_vbus_filter.stable_present;
#endif
}

void power_sleep_hal_read_wake_evidence(
    power_sleep_hal_wake_evidence_t *out) {
    if (out == NULL) {
        return;
    }
    out->button =
        powman_hw->pwrup[POWER_SLEEP_BUTTON_WAKE_SLOT];
    out->shared_irq =
        powman_hw->pwrup[POWER_SLEEP_SHARED_WAKE_SLOT];
    out->service_vbus =
        powman_hw->pwrup[POWER_SLEEP_VBUS_WAKE_SLOT];
    out->unused_slot =
        powman_hw->pwrup[POWER_SLEEP_UNUSED_WAKE_SLOT];
}

void power_sleep_hal_arm_wake_sources(void) {
    /* POWMAN is always-on: a slot configured by an older image survives a
     * switched-core cycle. Start from a closed set so an unowned PWRUP3 or
     * alarm can neither wake us nor corrupt LAST_SWCORE_PWRUP evidence. */
    reset_wakeup_slots();

    gpio_set_dir(POWER_BUTTON_PIN, GPIO_IN);
    gpio_pull_up(POWER_BUTTON_PIN);
    gpio_set_input_enabled(POWER_BUTTON_PIN, true);

    gpio_set_dir(SYS_INT_PIN, GPIO_IN);
    gpio_disable_pulls(SYS_INT_PIN);
    gpio_set_input_enabled(SYS_INT_PIN, true);

#if !SISU_RELEASE_BUILD
    gpio_set_dir(SERVICE_VBUS_PIN, GPIO_IN);
    gpio_disable_pulls(SERVICE_VBUS_PIN);
    gpio_set_input_enabled(SERVICE_VBUS_PIN, true);
#endif

    powman_clear_bits(
        &powman_hw->pwrup[POWER_SLEEP_BUTTON_WAKE_SLOT],
        POWMAN_PWRUP0_STATUS_BITS
    );
    powman_clear_bits(
        &powman_hw->pwrup[POWER_SLEEP_SHARED_WAKE_SLOT],
        POWMAN_PWRUP1_STATUS_BITS
    );
    powman_clear_bits(&powman_hw->pwrup[POWER_SLEEP_VBUS_WAKE_SLOT],
                      POWMAN_PWRUP2_STATUS_BITS);
    powman_clear_bits(&powman_hw->pwrup[POWER_SLEEP_UNUSED_WAKE_SLOT],
                      POWMAN_PWRUP3_STATUS_BITS);

    /* Edge detectors preserve the required wake paths. The service gate
     * supplies an inactive settle baseline before this function is called;
     * STATUS then latches even a short RTC or peripheral IRQ pulse. The final
     * Rev B2 production combination (edge slots plus dynamic SLEEP_EN at the
     * P1.7 boundary) measures 0.84 mA. */
    powman_enable_gpio_wakeup(
        POWER_SLEEP_BUTTON_WAKE_SLOT,
        POWER_BUTTON_PIN,
        true,
        false
    );
    powman_enable_gpio_wakeup(
        POWER_SLEEP_SHARED_WAKE_SLOT,
        SYS_INT_PIN,
        true,
        false
    );
#if !SISU_RELEASE_BUILD
    powman_enable_gpio_wakeup(POWER_SLEEP_VBUS_WAKE_SLOT,
                              SERVICE_VBUS_PIN,
                              true,
                              true);
#endif
}

void power_sleep_hal_disarm_wake_sources(void) {
    reset_wakeup_slots();
}

bool power_sleep_hal_power_button_asserted(void) {
    return gpio_get(POWER_BUTTON_PIN) == 0;
}

bool power_sleep_hal_shared_irq_asserted(void) {
    return gpio_get(SYS_INT_PIN) == 0;
}

/* RP-driven net: stage SIO OUT/OE before selecting SIO, then disable pulls and
 * the input receiver. Pads retain this state through the P1.7 off window. */
static void park_pad_output(uint pin, bool high) {
    gpio_put(pin, high);
    gpio_set_dir(pin, GPIO_OUT);
    gpio_set_function(pin, GPIO_FUNC_SIO);
    gpio_disable_pulls(pin);
    gpio_set_input_enabled(pin, false);
}

/* Externally driven, tri-stated, analog, or unconnected net. */
static void park_pad_quiet(uint pin) {
    gpio_set_dir(pin, GPIO_IN);
    gpio_set_function(pin, GPIO_FUNC_SIO);
    gpio_disable_pulls(pin);
    gpio_set_input_enabled(pin, false);
}

void power_sleep_hal_park_pads(void) {
    /* Rev B2 driven nets that must latch low. GP38/GP39 high would assert the
     * module control FETs; GP6/GP33 high would enable/force the PMIC. */
    static const uint8_t park_out_low[] = {
        MODEM_PIN_TX,
        MODEM_PIN_RTS,
        PMIC_3V8_PIN_ENABLE,
        PMIC_3V8_PIN_FORCE_PWM,
        MODEM_PIN_DTR,
        MODEM_PIN_ON_OFF,
        MODEM_PIN_HW_SHUTDOWN,
        MODEM_I2S_PIN_RXD,
        AUDIO_CODEC_PIN_BCLK,
        AUDIO_CODEC_PIN_LRC,
        AUDIO_CODEC_PIN_DIN,
        LCD_PIN_SCK,
        LCD_PIN_MOSI,
        LCD_PIN_DC,
        AUDIO_CODEC_PIN_MCLK,
    };
    /* Schematic-confirmed external/analog inputs plus every unconnected RP2354B
     * GPIO: GP26/27, GP29-32, GP34/35, and GP44-47. */
    static const uint8_t park_quiet[] = {
        MODEM_PIN_RX,
        MODEM_PIN_CTS,
        PMIC_3V8_PIN_POWER_GOOD,
        MODEM_I2S_PIN_CLK,
        MODEM_I2S_PIN_WA,
        MODEM_I2S_PIN_TXD,
        AUDIO_CODEC_PIN_ADCOUT,
        26u,
        27u,
#if SISU_RELEASE_BUILD
        /* GP28 is sampled during boot for USB+Power BOOTSEL, but it is not a
         * release-image wake source and its receiver can be parked in P1.7. */
        SERVICE_VBUS_PIN,
#endif
        29u,
        30u,
        31u,
        32u,
        34u,
        35u,
        MODEM_PIN_RI,
        MODEM_PIN_STATUS_ADC,
        HEADSET_HOOK_ADC_PIN,
        CHARGER_INPUT_ADC_PIN,
        44u,
        45u,
        46u,
        47u,
    };
    static const uint8_t park_hold_high[] = {
        LCD_PIN_RST,
        LCD_PIN_CS,
    };

    for (size_t i = 0u;
         i < sizeof(park_out_low) / sizeof(park_out_low[0]);
         i++) {
        park_pad_output(park_out_low[i], false);
    }
    park_pad_output(BUZZER_PIN, BUZZER_ACTIVE_LOW != 0u);
    park_pad_output(VIBRA_PIN, VIBRA_ACTIVE_LOW != 0u);
    park_pad_output(
        LCD_PIN_BACKLIGHT, LCD_BACKLIGHT_ACTIVE_LOW != 0u
    );
    for (size_t i = 0u;
         i < sizeof(park_quiet) / sizeof(park_quiet[0]);
         i++) {
        park_pad_quiet(park_quiet[i]);
    }
    for (size_t i = 0u;
         i < sizeof(park_hold_high) / sizeof(park_hold_high[0]);
         i++) {
        park_pad_output(park_hold_high[i], true);
    }
}

#include "audio/vibra_hal.h"

#include "hardware/gpio.h"
#include "hardware/pwm.h"
#include "hal/board.h"

static uint8_t s_strength = VIBRA_HAL_STRENGTH_STOCK;
#if VIBRA_PIN_CONFIGURED
static uint s_slice;
static uint s_channel;
#endif
static bool s_ready;

void vibra_hal_init(void) {
#if VIBRA_PIN_CONFIGURED
    gpio_init(VIBRA_PIN);
    s_slice = pwm_gpio_to_slice_num(VIBRA_PIN);
    s_channel = pwm_gpio_to_channel(VIBRA_PIN);
    /* GP22 shares a PWM slice with the GP23 backlight. board_init() owns the
     * slice configuration because pwm_init() would clear both channel compare
     * registers. Establish OFF before handing the pad from SIO to PWM. */
    pwm_set_chan_level(s_slice, s_channel, 0u);
    gpio_set_function(VIBRA_PIN, GPIO_FUNC_PWM);
    s_ready = true;
#else
    s_ready = false;
#endif
}

void vibra_hal_set_strength(uint8_t strength) {
    s_strength = strength == 0u ? VIBRA_HAL_STRENGTH_STOCK : strength;
}

void vibra_hal_set(bool on) {
#if VIBRA_PIN_CONFIGURED
    if (!s_ready) {
        return;
    }
    pwm_set_chan_level(s_slice, s_channel, on ? s_strength : 0u);
#else
    (void)on;
#endif
}

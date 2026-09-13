#include "audio/buzzer_hal.h"

#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/pwm.h"
#include "hal/board.h"

#if BUZZER_PIN_CONFIGURED
static uint s_slice;
static uint s_channel;
static bool s_ready;
static uint16_t s_top;                       /* current PWM wrap+1 (0 = not driving) */
static uint8_t s_level = BUZZER_LEVEL_MAX;
static uint8_t s_debug_duty_percent = UINT8_MAX;

static void buzzer_clamp_inactive(void) {
    /* A disabled RP2350 PWM slice can leave its last output level on the pad.
     * That is unsafe for the low-side buzzer MOSFET: a frozen high applies DC
     * to the coil. Prepare the SIO latch/OE first, then move the pad off PWM so
     * idle is electrically defined regardless of the slice's stopped phase. */
    gpio_put(BUZZER_PIN, BUZZER_ACTIVE_LOW != 0u);
    gpio_set_dir(BUZZER_PIN, GPIO_OUT);
    gpio_set_function(BUZZER_PIN, GPIO_FUNC_SIO);
}

/* Drive level (0..10) -> PWM duty %. The even entries are audio levels 1..5;
 * odd entries provide monotonic interpolation if a finer envelope is added.
 * Rev B2 PodMic sweep, 2026-08-30: output climbs to about 24%, then falls, so
 * 50% is not the acoustic maximum for the fitted buzzer and low-side driver. */
static uint16_t buzzer_chan_level(uint16_t top, uint8_t level) {
    static const uint8_t DUTY_PCT[BUZZER_LEVEL_MAX + 1u] = {
        0u,
        1u, BUZZER_DUTY_AUDIO_LEVEL1_PERCENT,
        2u, BUZZER_DUTY_AUDIO_LEVEL2_PERCENT,
        4u, BUZZER_DUTY_AUDIO_LEVEL3_PERCENT,
        6u, BUZZER_DUTY_AUDIO_LEVEL4_PERCENT,
        14u, BUZZER_DUTY_AUDIO_LEVEL5_PERCENT,
    };
    if (level > BUZZER_LEVEL_MAX) {
        level = BUZZER_LEVEL_MAX;
    }
    uint8_t duty = s_debug_duty_percent == UINT8_MAX
        ? DUTY_PCT[level] : s_debug_duty_percent;
    return (uint16_t)(((uint32_t)top * duty) / 100u);
}
#endif

void buzzer_hal_init(void) {
#if BUZZER_PIN_CONFIGURED
    gpio_init(BUZZER_PIN);
    buzzer_clamp_inactive();
    s_debug_duty_percent = UINT8_MAX;
    s_slice = pwm_gpio_to_slice_num(BUZZER_PIN);
    s_channel = pwm_gpio_to_channel(BUZZER_PIN);

    pwm_config cfg = pwm_get_default_config();
    pwm_config_set_output_polarity(&cfg,
                                   s_channel == PWM_CHAN_A ? BUZZER_ACTIVE_LOW != 0u : false,
                                   s_channel == PWM_CHAN_B ? BUZZER_ACTIVE_LOW != 0u : false);
    pwm_init(s_slice, &cfg, false);
    pwm_set_chan_level(s_slice, s_channel, 0u);
    s_ready = true;
    buzzer_hal_off();
#endif
}

bool buzzer_hal_available(void) {
#if BUZZER_PIN_CONFIGURED
    return s_ready;
#else
    return false;
#endif
}

void buzzer_hal_set_freq(uint16_t hz) {
#if BUZZER_PIN_CONFIGURED
    if (!s_ready) {
        return;
    }
    if (hz == 0u) {
        buzzer_hal_off();
        return;
    }
    bool starting = s_top == 0u;
    /* freq = sysclk / (div * top); choose div so top fits the 16-bit counter. */
    uint32_t clk = clock_get_hz(clk_sys);
    uint32_t total = clk / hz;
    uint32_t div = (total + 0xffffu) / 0x10000u;
    if (div < 1u) {
        div = 1u;
    }
    if (div > 255u) {
        div = 255u;
    }
    uint32_t top = total / div;
    if (top < 2u) {
        top = 2u;
    }
    if (top > 0xffffu) {
        top = 0xffffu;
    }
    pwm_set_clkdiv_int_frac(s_slice, (uint8_t)div, 0u);
    pwm_set_wrap(s_slice, (uint16_t)(top - 1u));
    s_top = (uint16_t)top;
    pwm_set_chan_level(s_slice, s_channel, buzzer_chan_level(s_top, s_level));
    if (starting) {
        pwm_set_counter(s_slice, 0u);
        gpio_set_function(BUZZER_PIN, GPIO_FUNC_PWM);
    }
    pwm_set_enabled(s_slice, true);
#else
    (void)hz;
#endif
}

void buzzer_hal_set_level(uint8_t level) {
#if BUZZER_PIN_CONFIGURED
    if (level > BUZZER_LEVEL_MAX) {
        level = BUZZER_LEVEL_MAX;
    }
    s_level = level;
    /* Re-apply the duty live if a note is currently driving (s_top != 0), so the
     * ascending ramp is heard within the held note, not only at the next note. */
    if (s_ready && s_top != 0u) {
        pwm_set_chan_level(s_slice, s_channel, buzzer_chan_level(s_top, s_level));
    }
#else
    (void)level;
#endif
}

void buzzer_hal_debug_set_duty_percent(uint8_t percent) {
#if BUZZER_PIN_CONFIGURED
    if (percent != UINT8_MAX && percent > 50u) {
        percent = 50u;
    }
    s_debug_duty_percent = percent;
    if (s_ready && s_top != 0u) {
        pwm_set_chan_level(s_slice, s_channel, buzzer_chan_level(s_top, s_level));
    }
#else
    (void)percent;
#endif
}

void buzzer_hal_off(void) {
#if BUZZER_PIN_CONFIGURED
    if (!s_ready) {
        return;
    }
    s_top = 0u; /* not driving: set_level just stores until the next set_freq */
    pwm_set_chan_level(s_slice, s_channel, 0u);
    pwm_set_enabled(s_slice, false);
    buzzer_clamp_inactive();
#endif
}

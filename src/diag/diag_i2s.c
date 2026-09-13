/*
 * Rev B2 codec-only I2S diagnostic. No cellular-module rail, UART, DVI, or AT
 * command is touched. The power key cycles a digital earpiece tone, handset
 * microphone metering, and headset microphone metering.
 */

#include <stdio.h>

#include "audio/audio_i2s_hal.h"
#include "audio/nau88c22_codec.h"
#include "hal/board.h"
#include "hal/lcd_pcd8544.h"
#include "hardware/gpio.h"
#include "pico/stdlib.h"
#include "ui/framebuffer.h"

typedef enum {
    DIAG_HANDSET_TONE = 0,
    DIAG_HANDSET_MIC,
    DIAG_HEADSET_MIC,
    DIAG_MODE_COUNT,
} diag_mode_t;

static lcd_pcd8544_t s_lcd;
static framebuffer_t s_fb;
static volatile bool s_tone_enabled;
static uint8_t s_tone_phase;

static void fill_audio(void *ctx, int16_t *dst, uint32_t frame_count) {
    (void)ctx;
    for (uint32_t i = 0u; i < frame_count; i++) {
        int16_t sample = 0;
        if (s_tone_enabled) {
            sample = s_tone_phase < 8u ? 2048 : -2048;
            s_tone_phase = (uint8_t)((s_tone_phase + 1u) & 15u);
        }
        dst[i * 2u] = sample;
        dst[i * 2u + 1u] = sample;
    }
}

static const char *mode_name(diag_mode_t mode) {
    switch (mode) {
    case DIAG_HANDSET_TONE:
        return "HAND TONE";
    case DIAG_HANDSET_MIC:
        return "HAND MIC";
    case DIAG_HEADSET_MIC:
        return "HEAD MIC";
    default:
        return "UNKNOWN";
    }
}

static bool apply_mode(diag_mode_t mode) {
    s_tone_enabled = mode == DIAG_HANDSET_TONE;
    nau_route_t route =
        mode == DIAG_HEADSET_MIC ? NAU_ROUTE_HEADSET : NAU_ROUTE_HANDSET;
    bool ok = nau88c22_codec_set_route(route);
    ok = nau88c22_codec_set_mic_power(true, route == NAU_ROUTE_HEADSET) && ok;
    ok = nau88c22_codec_set_playback_idle(false) && ok;
    return ok;
}

static void draw(diag_mode_t mode, bool codec_ok, bool i2s_ok) {
    audio_i2s_stats_t stats = {0};
    audio_i2s_hal_get_stats(&stats);
    char line[24];

    fb_clear(&s_fb, false);
    fb_text5(&s_fb, "REV B2 I2S", 0, 0, true, FB_WIDTH);
    fb_text5(&s_fb, mode_name(mode), 0, 9, true, FB_WIDTH);
    snprintf(line, sizeof line, "C%u I%u T%lu",
             codec_ok ? 1u : 0u, i2s_ok ? 1u : 0u,
             (unsigned long)stats.tx_irq_count);
    fb_text5(&s_fb, line, 0, 18, true, FB_WIDTH);
    snprintf(line, sizeof line, "RX %lu", (unsigned long)stats.rx_irq_count);
    fb_text5(&s_fb, line, 0, 27, true, FB_WIDTH);
    snprintf(line, sizeof line, "PK %d/%d",
             (int)stats.peak_left, (int)stats.peak_right);
    fb_text5(&s_fb, line, 0, 36, true, FB_WIDTH);
    lcd_show(&s_lcd, &s_fb);
}

int main(void) {
    board_set_system_clock();
    board_init();
    board_3v8_rail_set_enabled(false);
    board_3v8_rail_set_force_pwm(false);
    stdio_init_all();

    gpio_init(POWER_BUTTON_PIN);
    gpio_set_dir(POWER_BUTTON_PIN, GPIO_IN);
    gpio_pull_up(POWER_BUTTON_PIN);

    lcd_init(&s_lcd);
    board_set_backlight(true);

    bool codec_ok = nau88c22_codec_init();
    diag_mode_t mode = DIAG_HANDSET_TONE;
    codec_ok = apply_mode(mode) && codec_ok;
    bool i2s_ok = audio_i2s_hal_init(fill_audio, 0);

    bool previous_button = false;
    uint32_t next_draw_ms = 0u;
    while (true) {
        bool button = !gpio_get(POWER_BUTTON_PIN);
        if (button && !previous_button) {
            mode = (diag_mode_t)(((unsigned)mode + 1u) %
                                 (unsigned)DIAG_MODE_COUNT);
            codec_ok = apply_mode(mode) && codec_ok;
        }
        previous_button = button;

        uint32_t now_ms = to_ms_since_boot(get_absolute_time());
        if ((int32_t)(now_ms - next_draw_ms) >= 0) {
            next_draw_ms = now_ms + 250u;
            draw(mode, codec_ok, i2s_ok);
            audio_i2s_stats_t stats;
            audio_i2s_hal_get_stats(&stats);
            printf("[i2s] mode=%s codec=%u i2s=%u tx=%lu rx=%lu peak=%d/%d\n",
                   mode_name(mode), codec_ok ? 1u : 0u, i2s_ok ? 1u : 0u,
                   (unsigned long)stats.tx_irq_count,
                   (unsigned long)stats.rx_irq_count,
                   (int)stats.peak_left, (int)stats.peak_right);
        }
        sleep_ms(10u);
    }
}

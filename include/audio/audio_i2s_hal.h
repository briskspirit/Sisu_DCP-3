#ifndef AUDIO_I2S_HAL_H
#define AUDIO_I2S_HAL_H

#include <stdbool.h>
#include <stdint.h>

typedef void (*audio_i2s_fill_fn_t)(void *ctx, int16_t *dst, uint32_t frame_count);

typedef struct {
    uint32_t tx_irq_count;
    uint32_t rx_irq_count;
    int16_t last_left;
    int16_t last_right;
    int16_t peak_left;  /* abs-max of the codec RX left slot over the last buffer */
    int16_t peak_right; /* abs-max of the right slot (= headset mic ADC) -- a live
                         * mic-level meter for HW-vs-SW headset-mic diagnosis */
    uint32_t abort_timeouts; /* aborts that did not reach a safe state within the guard */
} audio_i2s_stats_t;

bool audio_i2s_hal_init(audio_i2s_fill_fn_t fill, void *ctx);
void audio_i2s_hal_get_stats(audio_i2s_stats_t *out_stats);

/* Flash-write coordination: STOP the codec I2S DMA/PIO while core1's refill ISR
 * is masked for a flash erase/program, then rebuild it. The DMA address rings
 * independently contain a delayed ISR; quiesce prevents stale audio and IRQs.
 * Call quiesce before parking core1 (XIP up) and resume after the op (XIP
 * restored). Both run on core1, which owns these channels. */
/* Returns false if DMA did not reach a hardware-confirmed safe state. */
bool audio_i2s_hal_flash_quiesce(void);
void audio_i2s_hal_flash_resume(void);

#endif

#include "audio/audio_i2s_hal.h"

#include <string.h>

#include "audio_i2s.pio.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "audio/audio_bridge.h"
#include "hal/board.h"
#include "pico/platform.h"   /* __not_in_flash_func */

/* Upper bound on the group CHAN_ABORT completion poll. ~100k iterations is
 * roughly 4 ms at 153.6 MHz; a conforming abort clears in a few bus cycles. */
#define I2S_DMA_ABORT_SPIN_GUARD 100000u

/* 2 ms block (32 frames @ 16 kHz) so the codec DMA cadence matches the audio
 * bridge ring blocks (AUDIO_BRIDGE_BLOCK_FRAMES); ping-pong = 4 ms buffering. */
#define AUDIO_BUFFER_FRAMES 32u
#define AUDIO_BUFFER_HALFWORDS (AUDIO_BUFFER_FRAMES * 2u)
#define AUDIO_TX_DMA_CHANNEL_COUNT 2u
#define AUDIO_RX_DMA_CHANNEL_COUNT 2u
/* Both ping-pong halves occupy one 256-byte address ring. If core1 masks DMA
 * IRQs for longer than one ping-pong cycle, the hardware then wraps inside the
 * allocation instead of advancing into adjacent .bss. */
#define AUDIO_DMA_RING_BITS 8u
#define AUDIO_DMA_RING_BYTES (1u << AUDIO_DMA_RING_BITS)

_Static_assert(AUDIO_PIN_LRC + 1u == AUDIO_PIN_BCLK,
               "codec LRCLK/BCLK must be adjacent for two-bit side-set");
_Static_assert(AUDIO_PIN_LRC == 13u && AUDIO_PIN_BCLK == 14u,
               "audio_i2s.pio wait-gpio literals must match Rev B2 clocks");
_Static_assert(AUDIO_PIN_ADCOUT == 12u && AUDIO_PIN_DIN == 15u,
               "codec data directions must match Rev B2");

static void dma_irq0_handler(void);
static void fill_buffer(int16_t *dst, uint32_t frame_count);
static void capture_buffer_done(const int16_t *src, uint32_t halfword_count);

static PIO s_pio = pio0;
static uint s_tx_sm;
static uint s_rx_sm;
static uint s_tx_dma_channel[AUDIO_TX_DMA_CHANNEL_COUNT];
static uint s_rx_dma_channel[AUDIO_RX_DMA_CHANNEL_COUNT];
static bool s_ready;
static uint32_t s_tx_irq_count;
static uint32_t s_rx_irq_count;
static uint32_t s_abort_timeouts;
static bool s_quiesce_complete;
static int16_t s_tx_buffers[AUDIO_TX_DMA_CHANNEL_COUNT][AUDIO_BUFFER_HALFWORDS]
    __attribute__((aligned(AUDIO_DMA_RING_BYTES)));
static int16_t s_rx_buffers[AUDIO_RX_DMA_CHANNEL_COUNT][AUDIO_BUFFER_HALFWORDS]
    __attribute__((aligned(AUDIO_DMA_RING_BYTES)));
_Static_assert(sizeof(s_tx_buffers) == AUDIO_DMA_RING_BYTES,
               "codec TX DMA ring size must match its allocation");
_Static_assert(sizeof(s_rx_buffers) == AUDIO_DMA_RING_BYTES,
               "codec RX DMA ring size must match its allocation");
static int16_t s_last_rx_left;
static int16_t s_last_rx_right;
static int16_t s_rx_peak_left;  /* abs-max of the L slot over the last capture buffer */
static int16_t s_rx_peak_right; /* abs-max of the R slot (headset mic ADC) -- mic meter */
static audio_i2s_fill_fn_t s_fill;
static void *s_fill_ctx;
/* PIO entry PCs, captured in init so flash-pause resume can re-seed the SMs to a
 * known frame phase (deterministic L/R alignment after the pause). */
static uint s_tx_prog_entry;
static uint s_rx_prog_entry;

static bool audio_dma_busy(void) {
    for (uint i = 0; i < AUDIO_TX_DMA_CHANNEL_COUNT; i++) {
        if ((dma_channel_hw_addr(s_tx_dma_channel[i])->ctrl_trig &
             DMA_CH0_CTRL_TRIG_BUSY_BITS) != 0u) {
            return true;
        }
    }
    for (uint i = 0; i < AUDIO_RX_DMA_CHANNEL_COUNT; i++) {
        if ((dma_channel_hw_addr(s_rx_dma_channel[i])->ctrl_trig &
             DMA_CH0_CTRL_TRIG_BUSY_BITS) != 0u) {
            return true;
        }
    }
    return false;
}

bool audio_i2s_hal_init(audio_i2s_fill_fn_t fill, void *ctx) {
    if (s_ready) {
        return true;
    }

    s_fill = fill;
    s_fill_ctx = ctx;
    s_tx_sm = pio_claim_unused_sm(s_pio, true);
    s_rx_sm = pio_claim_unused_sm(s_pio, true);
    for (uint i = 0; i < AUDIO_TX_DMA_CHANNEL_COUNT; i++) {
        s_tx_dma_channel[i] = dma_claim_unused_channel(true);
    }
    for (uint i = 0; i < AUDIO_RX_DMA_CHANNEL_COUNT; i++) {
        s_rx_dma_channel[i] = dma_claim_unused_channel(true);
    }

    uint32_t sys_hz = clock_get_hz(clk_sys);
    uint32_t divider_8_24 = (uint32_t)(((uint64_t)sys_hz << 8) / ((uint64_t)AUDIO_SAMPLE_RATE * 64u));

    uint tx_offset = pio_add_program(s_pio, &audio_i2s_program);
    uint rx_offset = pio_add_program(s_pio, &audio_i2s_rx_program);
    s_tx_prog_entry = tx_offset + audio_i2s_offset_entry_point;
    s_rx_prog_entry = rx_offset + audio_i2s_rx_offset_entry_point;
    pio_sm_init(s_pio, s_tx_sm, s_tx_prog_entry, NULL);
    pio_sm_init(s_pio, s_rx_sm, s_rx_prog_entry, NULL);

    pio_sm_config tx_config = audio_i2s_program_get_default_config(tx_offset);
    sm_config_set_clkdiv_int_frac8(&tx_config, divider_8_24 >> 8, divider_8_24 & 0xffu);
    sm_config_set_out_pins(&tx_config, AUDIO_PIN_DIN, 1);
    sm_config_set_out_shift(&tx_config, false, true, 16);
    sm_config_set_fifo_join(&tx_config, PIO_FIFO_JOIN_TX);
    sm_config_set_sideset(&tx_config, 2, false, false);
    sm_config_set_sideset_pins(&tx_config, AUDIO_PIN_LRC);
    sm_config_set_wrap(&tx_config, tx_offset + audio_i2s_wrap_target, tx_offset + audio_i2s_wrap);
    pio_sm_set_config(s_pio, s_tx_sm, &tx_config);

    pio_sm_config rx_config = audio_i2s_rx_program_get_default_config(rx_offset);
    sm_config_set_in_pins(&rx_config, AUDIO_PIN_ADCOUT);
    sm_config_set_in_shift(&rx_config, false, true, 16);
    sm_config_set_fifo_join(&rx_config, PIO_FIFO_JOIN_RX);
    sm_config_set_wrap(&rx_config, rx_offset + audio_i2s_rx_wrap_target, rx_offset + audio_i2s_rx_wrap);
    pio_sm_set_config(s_pio, s_rx_sm, &rx_config);

    pio_sm_set_pins_with_mask(s_pio, s_tx_sm, 0u, 1u << AUDIO_PIN_BCLK);
    pio_sm_set_pindirs_with_mask(s_pio, s_tx_sm, 1u << AUDIO_PIN_BCLK, 1u << AUDIO_PIN_BCLK);
    pio_gpio_init(s_pio, AUDIO_PIN_BCLK);
    /* The RX SM synchronizes with WAIT GPIO on both clocks. They are outputs
     * from the TX SM, but their pad input paths must remain enabled for WAIT. */
    gpio_set_input_enabled(AUDIO_PIN_BCLK, true);

    pio_sm_set_pins_with_mask(s_pio, s_tx_sm, 0u, 1u << AUDIO_PIN_LRC);
    pio_sm_set_pindirs_with_mask(s_pio, s_tx_sm, 1u << AUDIO_PIN_LRC, 1u << AUDIO_PIN_LRC);
    pio_gpio_init(s_pio, AUDIO_PIN_LRC);
    gpio_set_input_enabled(AUDIO_PIN_LRC, true);

    pio_sm_set_pins_with_mask(s_pio, s_tx_sm, 0u, 1u << AUDIO_PIN_DIN);
    pio_sm_set_pindirs_with_mask(s_pio, s_tx_sm, 1u << AUDIO_PIN_DIN, 1u << AUDIO_PIN_DIN);
    pio_gpio_init(s_pio, AUDIO_PIN_DIN);
    gpio_set_input_enabled(AUDIO_PIN_DIN, false);

    pio_sm_set_pindirs_with_mask(s_pio, s_rx_sm, 0u, 1u << AUDIO_PIN_ADCOUT);
    pio_gpio_init(s_pio, AUDIO_PIN_ADCOUT);
    gpio_set_input_enabled(AUDIO_PIN_ADCOUT, true);

    fill_buffer(s_tx_buffers[0], AUDIO_BUFFER_FRAMES);
    fill_buffer(s_tx_buffers[1], AUDIO_BUFFER_FRAMES);
    memset(s_rx_buffers, 0, sizeof(s_rx_buffers));

    for (uint i = 0; i < AUDIO_TX_DMA_CHANNEL_COUNT; i++) {
        dma_channel_config config = dma_channel_get_default_config(s_tx_dma_channel[i]);
        channel_config_set_transfer_data_size(&config, DMA_SIZE_16);
        channel_config_set_chain_to(&config, s_tx_dma_channel[(i + 1u) % AUDIO_TX_DMA_CHANNEL_COUNT]);
        channel_config_set_read_increment(&config, true);
        channel_config_set_write_increment(&config, false);
        channel_config_set_ring(&config, false, AUDIO_DMA_RING_BITS);
        channel_config_set_dreq(&config, pio_get_dreq(s_pio, s_tx_sm, true));
        dma_channel_configure(
            s_tx_dma_channel[i],
            &config,
            &s_pio->txf[s_tx_sm],
            s_tx_buffers[i],
            AUDIO_BUFFER_HALFWORDS,
            false
        );
        dma_channel_acknowledge_irq0(s_tx_dma_channel[i]);
        dma_channel_set_irq0_enabled(s_tx_dma_channel[i], true);
    }
    for (uint i = 0; i < AUDIO_RX_DMA_CHANNEL_COUNT; i++) {
        dma_channel_config config = dma_channel_get_default_config(s_rx_dma_channel[i]);
        channel_config_set_transfer_data_size(&config, DMA_SIZE_16);
        channel_config_set_chain_to(&config, s_rx_dma_channel[(i + 1u) % AUDIO_RX_DMA_CHANNEL_COUNT]);
        channel_config_set_read_increment(&config, false);
        channel_config_set_write_increment(&config, true);
        channel_config_set_ring(&config, true, AUDIO_DMA_RING_BITS);
        channel_config_set_dreq(&config, pio_get_dreq(s_pio, s_rx_sm, false));
        dma_channel_configure(
            s_rx_dma_channel[i],
            &config,
            s_rx_buffers[i],
            &s_pio->rxf[s_rx_sm],
            AUDIO_BUFFER_HALFWORDS,
            false
        );
        dma_channel_acknowledge_irq0(s_rx_dma_channel[i]);
        dma_channel_set_irq0_enabled(s_rx_dma_channel[i], true);
    }
    irq_add_shared_handler(DMA_IRQ_0, dma_irq0_handler, PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY);
    /* Pin DMA_IRQ_0 (codec) and DMA_IRQ_1 (modem) to the same explicit NVIC
     * priority: the lock-free SPSC rings require neither endpoint to preempt the
     * other mid-index-update, so do not rely on the defaults happening to match. */
    irq_set_priority(DMA_IRQ_0, PICO_DEFAULT_IRQ_PRIORITY);
    irq_set_enabled(DMA_IRQ_0, true);

    s_quiesce_complete = true;
    s_ready = true;
    pio_sm_set_enabled(s_pio, s_rx_sm, true);
    dma_channel_start(s_rx_dma_channel[0]);
    pio_sm_set_enabled(s_pio, s_tx_sm, true);
    dma_channel_start(s_tx_dma_channel[0]);
    return true;
}

void audio_i2s_hal_get_stats(audio_i2s_stats_t *out_stats) {
    if (out_stats == 0) {
        return;
    }
    out_stats->tx_irq_count = s_tx_irq_count;
    out_stats->rx_irq_count = s_rx_irq_count;
    out_stats->last_left = s_last_rx_left;
    out_stats->last_right = s_last_rx_right;
    out_stats->peak_left = s_rx_peak_left;
    out_stats->peak_right = s_rx_peak_right;
    out_stats->abort_timeouts = s_abort_timeouts;
}

/* Flash-pause quiesce: STOP the codec DMA/PIO while core1's refill ISR is masked
 * for the erase. Address rings contain an unexpectedly late ISR, but a clean
 * stop still prevents stale audio/IRQs across the flash operation. Called on
 * core1 from core1_flash_park() with IRQs off and XIP still up. */
bool audio_i2s_hal_flash_quiesce(void) {
    if (!s_ready) {
        return true;
    }
    /* RP2350-E5 / datasheet 12.6.8.3: clear EN and disable CHAIN_TO on every
     * channel in the chain before aborting the complete set. Leaving CHAIN_TO
     * armed can restart a sibling as CHAN_ABORT clears. */
    uint32_t mask = 0;
    for (uint i = 0; i < AUDIO_TX_DMA_CHANNEL_COUNT; i++) {
        uint channel = s_tx_dma_channel[i];
        dma_channel_hw_t *hw = dma_channel_hw_addr(channel);
        uint32_t ctrl = hw->al1_ctrl;
        ctrl &= ~(DMA_CH0_CTRL_TRIG_EN_BITS | DMA_CH0_CTRL_TRIG_CHAIN_TO_BITS);
        ctrl |= (channel << DMA_CH0_CTRL_TRIG_CHAIN_TO_LSB) &
                DMA_CH0_CTRL_TRIG_CHAIN_TO_BITS;
        hw->al1_ctrl = ctrl;
        mask |= 1u << channel;
    }
    for (uint i = 0; i < AUDIO_RX_DMA_CHANNEL_COUNT; i++) {
        uint channel = s_rx_dma_channel[i];
        dma_channel_hw_t *hw = dma_channel_hw_addr(channel);
        uint32_t ctrl = hw->al1_ctrl;
        ctrl &= ~(DMA_CH0_CTRL_TRIG_EN_BITS | DMA_CH0_CTRL_TRIG_CHAIN_TO_BITS);
        ctrl |= (channel << DMA_CH0_CTRL_TRIG_CHAIN_TO_LSB) &
                DMA_CH0_CTRL_TRIG_CHAIN_TO_BITS;
        hw->al1_ctrl = ctrl;
        mask |= 1u << channel;
    }
    dma_hw->abort = mask;
    /* The Pico SDK's RP2350 dma_channel_abort() uses channel BUSY as the
     * safe-state oracle after writing CHAN_ABORT. Do the same for this complete
     * group under one bound. CHAN_ABORT is declared write-only by the SDK, so a
     * zero read can falsely report completion while a bus transfer is in flight. */
    uint32_t guard = I2S_DMA_ABORT_SPIN_GUARD;
    while (audio_dma_busy() && guard-- != 0u) {
        tight_loop_contents();
    }
    s_quiesce_complete = !audio_dma_busy();
    if (!s_quiesce_complete) {
        s_abort_timeouts++;
    }
    /* Stop the SMs so BCLK/WS stop and nothing keeps DREQ-ing while we are parked. */
    pio_sm_set_enabled(s_pio, s_tx_sm, false);
    pio_sm_set_enabled(s_pio, s_rx_sm, false);
    /* Drop the completion latch the abort raised so resume starts clean. */
    for (uint i = 0; i < AUDIO_TX_DMA_CHANNEL_COUNT; i++) {
        dma_channel_acknowledge_irq0(s_tx_dma_channel[i]);
    }
    for (uint i = 0; i < AUDIO_RX_DMA_CHANNEL_COUNT; i++) {
        dma_channel_acknowledge_irq0(s_rx_dma_channel[i]);
    }
    return s_quiesce_complete;
}

/* Flash-pause resume: rebuild the codec DMA/PIO from a known-good state (mirror
 * of the init tail). Called on core1 from core1_flash_park() after the spin exits,
 * with IRQs still off and XIP restored (core0 re-enabled it before clearing the
 * pause request). Clears any latched/NVIC-pending DMA_IRQ_0 so restore_interrupts
 * does not immediately vector a spurious completion into the freshly armed state. */
void audio_i2s_hal_flash_resume(void) {
    if (!s_ready || !s_quiesce_complete) {
        return;
    }
    fill_buffer(s_tx_buffers[0], AUDIO_BUFFER_FRAMES);
    fill_buffer(s_tx_buffers[1], AUDIO_BUFFER_FRAMES);
    memset(s_rx_buffers, 0, sizeof(s_rx_buffers));

    for (uint i = 0; i < AUDIO_TX_DMA_CHANNEL_COUNT; i++) {
        dma_channel_config config = dma_channel_get_default_config(s_tx_dma_channel[i]);
        channel_config_set_transfer_data_size(&config, DMA_SIZE_16);
        channel_config_set_chain_to(&config, s_tx_dma_channel[(i + 1u) % AUDIO_TX_DMA_CHANNEL_COUNT]);
        channel_config_set_read_increment(&config, true);
        channel_config_set_write_increment(&config, false);
        channel_config_set_ring(&config, false, AUDIO_DMA_RING_BITS);
        channel_config_set_dreq(&config, pio_get_dreq(s_pio, s_tx_sm, true));
        dma_channel_configure(s_tx_dma_channel[i], &config, &s_pio->txf[s_tx_sm],
                              s_tx_buffers[i], AUDIO_BUFFER_HALFWORDS, false);
        dma_channel_acknowledge_irq0(s_tx_dma_channel[i]);
        dma_channel_set_irq0_enabled(s_tx_dma_channel[i], true);
    }
    for (uint i = 0; i < AUDIO_RX_DMA_CHANNEL_COUNT; i++) {
        dma_channel_config config = dma_channel_get_default_config(s_rx_dma_channel[i]);
        channel_config_set_transfer_data_size(&config, DMA_SIZE_16);
        channel_config_set_chain_to(&config, s_rx_dma_channel[(i + 1u) % AUDIO_RX_DMA_CHANNEL_COUNT]);
        channel_config_set_read_increment(&config, false);
        channel_config_set_write_increment(&config, true);
        channel_config_set_ring(&config, true, AUDIO_DMA_RING_BITS);
        channel_config_set_dreq(&config, pio_get_dreq(s_pio, s_rx_sm, false));
        dma_channel_configure(s_rx_dma_channel[i], &config, s_rx_buffers[i],
                              &s_pio->rxf[s_rx_sm], AUDIO_BUFFER_HALFWORDS, false);
        dma_channel_acknowledge_irq0(s_rx_dma_channel[i]);
        dma_channel_set_irq0_enabled(s_rx_dma_channel[i], true);
    }
    irq_clear(DMA_IRQ_0);   /* drop any NVIC-pending set while core1 IRQs were masked */

    pio_sm_clear_fifos(s_pio, s_tx_sm);
    pio_sm_clear_fifos(s_pio, s_rx_sm);
    pio_sm_restart(s_pio, s_tx_sm);
    pio_sm_restart(s_pio, s_rx_sm);
    pio_sm_clkdiv_restart(s_pio, s_tx_sm);
    pio_sm_clkdiv_restart(s_pio, s_rx_sm);
    pio_sm_exec(s_pio, s_tx_sm, pio_encode_jmp(s_tx_prog_entry));
    pio_sm_exec(s_pio, s_rx_sm, pio_encode_jmp(s_rx_prog_entry));

    pio_sm_set_enabled(s_pio, s_rx_sm, true);
    dma_channel_start(s_rx_dma_channel[0]);
    pio_sm_set_enabled(s_pio, s_tx_sm, true);
    dma_channel_start(s_tx_dma_channel[0]);
}

static void __not_in_flash_func(dma_irq0_handler)(void) {
    uint32_t pending = dma_hw->ints0;
    for (uint i = 0; i < AUDIO_TX_DMA_CHANNEL_COUNT; i++) {
        uint channel = s_tx_dma_channel[i];
        uint32_t mask = 1u << channel;
        if ((pending & mask) == 0) {
            continue;
        }
        /* Acknowledge before the (relatively long) refill so an interrupt from
         * the other channel that arrives while we are filling is not lost: the
         * flag is cleared first, then re-asserts and re-triggers the IRQ if the
         * sibling channel completes during fill_buffer(). */
        dma_hw->ints0 = mask;
        fill_buffer(s_tx_buffers[i], AUDIO_BUFFER_FRAMES);
        s_tx_irq_count++;
        dma_channel_set_read_addr(channel, s_tx_buffers[i], false);
    }
    for (uint i = 0; i < AUDIO_RX_DMA_CHANNEL_COUNT; i++) {
        uint channel = s_rx_dma_channel[i];
        uint32_t mask = 1u << channel;
        if ((pending & mask) == 0) {
            continue;
        }
        dma_hw->ints0 = mask;
        capture_buffer_done(s_rx_buffers[i], AUDIO_BUFFER_HALFWORDS);
        s_rx_irq_count++;
        dma_channel_set_write_addr(channel, s_rx_buffers[i], false);
    }
}

static void fill_buffer(int16_t *dst, uint32_t frame_count) {
    if (s_fill != 0) {
        s_fill(s_fill_ctx, dst, frame_count);
    } else {
        memset(dst, 0, frame_count * 2u * sizeof(int16_t));
    }
}

static void capture_buffer_done(const int16_t *src, uint32_t halfword_count) {
    if (src == 0 || halfword_count < 2u) {
        return;
    }
    s_last_rx_left = src[halfword_count - 2u];
    s_last_rx_right = src[halfword_count - 1u];

    /* Live abs-peak per slot over this buffer. The right slot is the codec's
     * RIGHT ADC = the headset mic (mic_channel=1); watching peak_right during a
     * headset call tells HW from SW: it moves when the mic reaches the codec ADC
     * (so the fault is downstream, the modem uplink), stays flat if the mic never
     * gets there (HW wiring/bias or the [BP] right-input routing). Cheap: one pass. */
    int16_t pk_l = 0;
    int16_t pk_r = 0;
    for (uint32_t i = 0; i + 1u < halfword_count; i += 2u) {
        int32_t l = src[i];
        int32_t r = src[i + 1u];
        if (l < 0) { l = -l; }
        if (r < 0) { r = -r; }
        if (l > pk_l) { pk_l = (int16_t)(l > 32767 ? 32767 : l); }
        if (r > pk_r) { pk_r = (int16_t)(r > 32767 ? 32767 : r); }
    }
    s_rx_peak_left = pk_l;
    s_rx_peak_right = pk_r;

    /* Bridge uplink producer: while a call is active, extract the mic ADC
     * channel (left for handset/loudspeaker, right for headset) into a mono
     * block and push it to the uplink ring for the modem TX path. Dormant until
     * audio_bridge_start() (call connect). */
    if (audio_bridge_active()) {
        uint32_t frames = halfword_count / 2u;
        if (frames > AUDIO_BUFFER_FRAMES) {
            frames = AUDIO_BUFFER_FRAMES;
        }
        uint8_t ch = audio_bridge_uplink_mic_channel();
        int16_t mono[AUDIO_BUFFER_FRAMES];
        for (uint32_t i = 0; i < frames; i++) {
            mono[i] = src[(i * 2u) + ch];
        }
        audio_bridge_uplink_push(mono, (uint16_t)frames);
    }
}

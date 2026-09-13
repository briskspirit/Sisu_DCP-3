#include "audio/modem_i2s_hal.h"

#include <string.h>

#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "hardware/sync.h"
#include "modem_i2s.pio.h"
#include "audio/audio_bridge.h"
#include "audio/audio_i2s_hal.h" /* park the codec I2S DMA during resync's masked spin */
#include "hal/board.h"
#include "pico/platform.h"   /* __not_in_flash_func */

/* Upper bound on the group CHAN_ABORT completion poll. ~100k iterations is
 * roughly 4 ms at 153.6 MHz; a conforming abort clears a DREQ-stalled channel
 * in a few bus cycles. core1_flash_pause_begin restores full clock first. */
#define I2S_DMA_ABORT_SPIN_GUARD 100000u

/* 2 ms block (32 frames @ 16 kHz) to match the audio bridge ring blocks. */
#define MODEM_I2S_BUFFER_FRAMES 32u
#define MODEM_I2S_BUFFER_HALFWORDS (MODEM_I2S_BUFFER_FRAMES * 2u)
#define MODEM_I2S_RX_DMA_CHANNEL_COUNT 2u
#define MODEM_I2S_TX_DMA_CHANNEL_COUNT 2u
/* RX is 2 x 128 bytes; TX is 2 x 256 bytes because each I2S slot is staged as
 * a 32-bit PIO word. Hardware address wrapping is a containment boundary if a
 * masked refill IRQ misses more than one ping-pong cycle. */
#define MODEM_I2S_RX_DMA_RING_BITS 8u
#define MODEM_I2S_RX_DMA_RING_BYTES (1u << MODEM_I2S_RX_DMA_RING_BITS)
#define MODEM_I2S_TX_DMA_RING_BITS 9u
#define MODEM_I2S_TX_DMA_RING_BYTES (1u << MODEM_I2S_TX_DMA_RING_BITS)

/* The RX program captures word0 = WA-low and word1 = WA-high. The qualified
 * Telit DVI configuration drives downlink in both slots and samples uplink from
 * WA-low; fill_tx_buffer mirrors the mic into both slots. */
#define MODEM_DOWNLINK_SLOT 0u

/* BCLK liveness: declare inactive after this many consecutive idle polls. The
 * detector pushes one word per ~62.5 us frame, so even infrequent polling sees
 * many words while live; the caller's poll cadence sets the real timeout. The
 * consumer (audio_service_tick) polls ~every 1 ms, so 150 -> ~150 ms, generous
 * enough that a brief mid-call clock hiccup never trips the fallback teardown. */
#define MODEM_BCLK_IDLE_POLLS 150u

/* Self-correcting frame lock. A (re)lock is deterministic-clean now that
 * resync_lock() restarts the SM shift state (the historical "~50% racy lock" was
 * stale ISR/OSR residue re-rolling per attempt, not WA jitter), but a clock-start
 * transient can still land a bad first lock. capture_buffer_done() therefore
 * judges the slot-mismatch metric after a settle: <= OK => bit-aligned; otherwise
 * re-lock both SMs (they share the IRQ-4 grid, so they converge together), capped
 * per episode. Valid on real calls: the mode-14 downlink is dual mono by spec
 * (both WA slots identical), so mismatch ~0 <=> aligned, and the metric is
 * self-gating (a misaligned slot pair only reads ~equal while the line is
 * silent, where the garbage is inaudible anyway). */
#define MODEM_RELOCK_OK_MISMATCH 1000u
#define MODEM_RELOCK_SETTLE_BUFFERS 3u
#define MODEM_RELOCK_MAX_ATTEMPTS 24u
#define MODEM_FRAME_SYNC_IRQ 4u

static void modem_dma_irq1_handler(void);
static void capture_buffer_done(const int16_t *src, uint32_t halfword_count);
static void fill_tx_buffer(uint32_t *dst);
static void resync_lock(void);

static PIO s_pio = pio1;
static uint s_rx_sm;
static uint s_tx_sm;
static uint s_bclk_sm;
static uint s_rx_dma_channel[MODEM_I2S_RX_DMA_CHANNEL_COUNT];
static uint s_tx_dma_channel[MODEM_I2S_TX_DMA_CHANNEL_COUNT];
static bool s_ready;
static uint32_t s_rx_irq_count;
static uint32_t s_tx_irq_count;
static uint32_t s_abort_timeouts;
static bool s_quiesce_complete;
static int16_t s_rx_buffers[MODEM_I2S_RX_DMA_CHANNEL_COUNT][MODEM_I2S_BUFFER_HALFWORDS]
    __attribute__((aligned(MODEM_I2S_RX_DMA_RING_BYTES)));
/* TX is fed as 32-bit words (one per WA slot): the sample is left-justified into
 * the high 16 bits so the MSB-first (shift-left) OSR drives it out correctly. A
 * 16-bit DMA would land the sample in the low half, which shift-left never emits
 * -> GP11 stuck low. So the "halfword" count here is a 32-bit word count. */
static uint32_t s_tx_buffers[MODEM_I2S_TX_DMA_CHANNEL_COUNT][MODEM_I2S_BUFFER_HALFWORDS]
    __attribute__((aligned(MODEM_I2S_TX_DMA_RING_BYTES)));
_Static_assert(sizeof(s_rx_buffers) == MODEM_I2S_RX_DMA_RING_BYTES,
               "modem RX DMA ring size must match its allocation");
_Static_assert(sizeof(s_tx_buffers) == MODEM_I2S_TX_DMA_RING_BYTES,
               "modem TX DMA ring size must match its allocation");
static int16_t s_last_wa_low;
static int16_t s_last_wa_high;
static uint32_t s_last_slot_mismatch;
static uint16_t s_relock_settle;
static uint16_t s_relock_attempts;
static uint32_t s_relock_total;
static bool s_bclk_live;
static uint8_t s_bclk_idle_calls;
static volatile bool s_running;
/* PIO entry PCs captured in init so flash-pause resume re-seeds the RX/TX SMs to
 * their WA-wait entry (deterministic frame alignment after the pause). */
static uint s_rx_prog_entry;
static uint s_tx_prog_entry;
static uint s_bclk_prog_entry;

static bool modem_dma_busy(void) {
    for (uint i = 0; i < MODEM_I2S_RX_DMA_CHANNEL_COUNT; i++) {
        if ((dma_channel_hw_addr(s_rx_dma_channel[i])->ctrl_trig &
             DMA_CH0_CTRL_TRIG_BUSY_BITS) != 0u) {
            return true;
        }
    }
    for (uint i = 0; i < MODEM_I2S_TX_DMA_CHANNEL_COUNT; i++) {
        if ((dma_channel_hw_addr(s_tx_dma_channel[i])->ctrl_trig &
             DMA_CH0_CTRL_TRIG_BUSY_BITS) != 0u) {
            return true;
        }
    }
    return false;
}

bool modem_i2s_hal_init(void) {
    if (s_ready) {
        return true;
    }

    s_rx_sm = pio_claim_unused_sm(s_pio, true);
    s_tx_sm = pio_claim_unused_sm(s_pio, true);
    s_bclk_sm = pio_claim_unused_sm(s_pio, true);
    for (uint i = 0; i < MODEM_I2S_RX_DMA_CHANNEL_COUNT; i++) {
        s_rx_dma_channel[i] = dma_claim_unused_channel(true);
    }
    for (uint i = 0; i < MODEM_I2S_TX_DMA_CHANNEL_COUNT; i++) {
        s_tx_dma_channel[i] = dma_claim_unused_channel(true);
    }

    uint rx_offset = pio_add_program(s_pio, &modem_i2s_rx_program);
    uint tx_offset = pio_add_program(s_pio, &modem_i2s_tx_program);
    uint bclk_offset = pio_add_program(s_pio, &modem_bclk_count_program);
    s_rx_prog_entry = rx_offset + modem_i2s_rx_offset_entry_point;
    s_tx_prog_entry = tx_offset + modem_i2s_tx_offset_entry_point;
    s_bclk_prog_entry = bclk_offset + modem_bclk_count_offset_entry_point;
    pio_sm_init(s_pio, s_rx_sm, s_rx_prog_entry, NULL);
    pio_sm_init(s_pio, s_tx_sm, s_tx_prog_entry, NULL);
    pio_sm_init(s_pio, s_bclk_sm, s_bclk_prog_entry, NULL);

    pio_sm_config rx_config = modem_i2s_rx_program_get_default_config(rx_offset);
    sm_config_set_in_pins(&rx_config, MODEM_I2S_PIN_TXD);
    sm_config_set_in_shift(&rx_config, false, true, 16);
    sm_config_set_jmp_pin(&rx_config, MODEM_I2S_PIN_WA); /* 'jmp pin' tests WA for the BCLK-synchronous frame lock */
    sm_config_set_fifo_join(&rx_config, PIO_FIFO_JOIN_RX);
    sm_config_set_wrap(&rx_config, rx_offset + modem_i2s_rx_wrap_target, rx_offset + modem_i2s_rx_wrap);
    pio_sm_set_config(s_pio, s_rx_sm, &rx_config);

    pio_sm_config tx_config = modem_i2s_tx_program_get_default_config(tx_offset);
    sm_config_set_out_pins(&tx_config, MODEM_I2S_PIN_RXD, 1);
    sm_config_set_out_shift(&tx_config, false, true, 16);
    sm_config_set_jmp_pin(&tx_config, MODEM_I2S_PIN_WA); /* 'jmp pin' tests WA for the BCLK-synchronous frame lock */
    sm_config_set_fifo_join(&tx_config, PIO_FIFO_JOIN_TX);
    sm_config_set_wrap(&tx_config, tx_offset + modem_i2s_tx_wrap_target, tx_offset + modem_i2s_tx_wrap);
    pio_sm_set_config(s_pio, s_tx_sm, &tx_config);

    pio_sm_config bclk_config = modem_bclk_count_program_get_default_config(bclk_offset);
    sm_config_set_wrap(&bclk_config, bclk_offset + modem_bclk_count_wrap_target,
                       bclk_offset + modem_bclk_count_wrap);
    pio_sm_set_config(s_pio, s_bclk_sm, &bclk_config);

    /* GP8 BCLK, GP9 WA, GP10 TXD are inputs from the modem (both SMs wait on GP8/GP9);
     * GP11 RXD is our uplink output. */
    pio_gpio_init(s_pio, MODEM_I2S_PIN_CLK);
    pio_gpio_init(s_pio, MODEM_I2S_PIN_WA);
    pio_gpio_init(s_pio, MODEM_I2S_PIN_TXD);
    pio_gpio_init(s_pio, MODEM_I2S_PIN_RXD);
    gpio_set_input_enabled(MODEM_I2S_PIN_CLK, true);
    gpio_set_input_enabled(MODEM_I2S_PIN_WA, true);
    gpio_set_input_enabled(MODEM_I2S_PIN_TXD, true);
    gpio_set_input_enabled(MODEM_I2S_PIN_RXD, false);
    /* Pull the three modem-to-RP inputs low. With the modem rail off, the
     * partial-power-down level shifter tri-states; unpulled
     * pads would float mid-rail and burn input-buffer current. Low matches the
     * parked-clock idle, and the weak pulls are invisible against the shifter's
     * push-pull drive while a call is up. */
    gpio_pull_down(MODEM_I2S_PIN_CLK);
    gpio_pull_down(MODEM_I2S_PIN_WA);
    gpio_pull_down(MODEM_I2S_PIN_TXD);
    pio_sm_set_pindirs_with_mask(s_pio, s_rx_sm, 0u,
                                 (1u << MODEM_I2S_PIN_CLK) | (1u << MODEM_I2S_PIN_WA) |
                                     (1u << MODEM_I2S_PIN_TXD));
    pio_sm_set_pindirs_with_mask(s_pio, s_tx_sm, (1u << MODEM_I2S_PIN_RXD), (1u << MODEM_I2S_PIN_RXD));

    memset(s_rx_buffers, 0, sizeof(s_rx_buffers));
    for (uint i = 0; i < MODEM_I2S_TX_DMA_CHANNEL_COUNT; i++) {
        fill_tx_buffer(s_tx_buffers[i]);
    }

    for (uint i = 0; i < MODEM_I2S_RX_DMA_CHANNEL_COUNT; i++) {
        dma_channel_config dma_config = dma_channel_get_default_config(s_rx_dma_channel[i]);
        channel_config_set_transfer_data_size(&dma_config, DMA_SIZE_16);
        channel_config_set_chain_to(&dma_config, s_rx_dma_channel[(i + 1u) % MODEM_I2S_RX_DMA_CHANNEL_COUNT]);
        channel_config_set_read_increment(&dma_config, false);
        channel_config_set_write_increment(&dma_config, true);
        channel_config_set_ring(&dma_config, true, MODEM_I2S_RX_DMA_RING_BITS);
        channel_config_set_dreq(&dma_config, pio_get_dreq(s_pio, s_rx_sm, false));
        dma_channel_configure(s_rx_dma_channel[i],
                              &dma_config,
                              s_rx_buffers[i],
                              &s_pio->rxf[s_rx_sm],
                              MODEM_I2S_BUFFER_HALFWORDS,
                              false);
        dma_channel_acknowledge_irq1(s_rx_dma_channel[i]);
        dma_channel_set_irq1_enabled(s_rx_dma_channel[i], true);
    }
    for (uint i = 0; i < MODEM_I2S_TX_DMA_CHANNEL_COUNT; i++) {
        dma_channel_config dma_config = dma_channel_get_default_config(s_tx_dma_channel[i]);
        channel_config_set_transfer_data_size(&dma_config, DMA_SIZE_32);
        channel_config_set_chain_to(&dma_config, s_tx_dma_channel[(i + 1u) % MODEM_I2S_TX_DMA_CHANNEL_COUNT]);
        channel_config_set_read_increment(&dma_config, true);
        channel_config_set_write_increment(&dma_config, false);
        channel_config_set_ring(&dma_config, false, MODEM_I2S_TX_DMA_RING_BITS);
        channel_config_set_dreq(&dma_config, pio_get_dreq(s_pio, s_tx_sm, true));
        dma_channel_configure(s_tx_dma_channel[i],
                              &dma_config,
                              &s_pio->txf[s_tx_sm],
                              s_tx_buffers[i],
                              MODEM_I2S_BUFFER_HALFWORDS,
                              false);
        dma_channel_acknowledge_irq1(s_tx_dma_channel[i]);
        dma_channel_set_irq1_enabled(s_tx_dma_channel[i], true);
    }

    /* Modem DMA owns DMA_IRQ_1 outright; the codec is isolated on DMA_IRQ_0 and
     * no other subsystem routes a channel here. Use an exclusive handler so
     * enabling voice transport cannot exhaust the SDK's small global pool of
     * shared-handler chain slots (USB + GPIO already consume several before
     * core1 starts). Both DMA IRQs use the SAME explicit NVIC priority so the
     * elastic-ring endpoints never preempt each other, as required by the
     * lock-free SPSC rings in audio_bridge. */
    irq_set_exclusive_handler(DMA_IRQ_1, modem_dma_irq1_handler);
    irq_set_priority(DMA_IRQ_1, PICO_DEFAULT_IRQ_PRIORITY);
    irq_set_enabled(DMA_IRQ_1, true);

    s_quiesce_complete = true;
    s_running = false;
    s_ready = true;
    return true;
}

bool modem_i2s_hal_start(void) {
    if (!s_ready) {
        return false;
    }
    if (s_running) {
        /* A failed stop deliberately leaves running asserted to block low-power
         * entry, but quiesce_complete tells a later start that the transport is
         * not usable yet. The idle stop retry must finish first. */
        return s_quiesce_complete;
    }

    uint32_t save = save_and_disable_interrupts();
    /* Rebuild both DMA ping-pongs from their known stopped state. This is the
     * same path used after a flash park and gives every call a fresh FIFO,
     * address, chain, and PIO shift state before BCLK can arrive. */
    modem_i2s_hal_flash_resume();
    if (!s_quiesce_complete) {
        restore_interrupts(save);
        return false;
    }
    pio_sm_set_enabled(s_pio, s_bclk_sm, false);
    pio_sm_clear_fifos(s_pio, s_bclk_sm);
    pio_sm_restart(s_pio, s_bclk_sm);
    pio_sm_clkdiv_restart(s_pio, s_bclk_sm);
    pio_sm_exec(s_pio, s_bclk_sm, pio_encode_jmp(s_bclk_prog_entry));
    s_bclk_idle_calls = 0u;
    s_bclk_live = false;
    pio_sm_set_enabled(s_pio, s_bclk_sm, true);
    s_running = true;
    restore_interrupts(save);
    return true;
}

bool modem_i2s_hal_stop(void) {
    if (!s_ready || !s_running) {
        return true;
    }

    uint32_t save = save_and_disable_interrupts();
    pio_sm_set_enabled(s_pio, s_bclk_sm, false);
    bool stopped = modem_i2s_hal_flash_quiesce();
    if (stopped) {
        pio_sm_clear_fifos(s_pio, s_bclk_sm);
        pio_sm_restart(s_pio, s_bclk_sm);
        s_bclk_idle_calls = 0u;
        s_bclk_live = false;
        s_running = false;
    }
    restore_interrupts(save);
    return stopped;
}

bool modem_i2s_hal_running(void) {
    return s_running;
}

void modem_i2s_hal_get_stats(modem_i2s_stats_t *out_stats) {
    if (out_stats == 0) {
        return;
    }
    out_stats->rx_irq_count = s_rx_irq_count;
    out_stats->last_wa_low = s_last_wa_low;
    out_stats->last_wa_high = s_last_wa_high;
    out_stats->last_slot_mismatch = s_last_slot_mismatch;
    out_stats->relock_attempts = s_relock_attempts;
    out_stats->relock_total = s_relock_total;
    out_stats->abort_timeouts = s_abort_timeouts;
}

void modem_i2s_hal_tx_probe(modem_i2s_tx_probe_t *out) {
    if (out == 0 || !s_ready) {
        return;
    }
    out->tx_irq_count = s_tx_irq_count;
    out->tx_fifo_level = pio_sm_get_tx_fifo_level(s_pio, s_tx_sm);
    out->tx_prog_entry = s_tx_prog_entry;
    out->tx_sm_enabled = (s_pio->ctrl & (1u << s_tx_sm)) != 0u;
    out->dma0_busy = dma_channel_is_busy(s_tx_dma_channel[0]);
    out->dma1_busy = dma_channel_is_busy(s_tx_dma_channel[1]);
    out->dma0_tcount = dma_hw->ch[s_tx_dma_channel[0]].transfer_count;
    out->dma1_tcount = dma_hw->ch[s_tx_dma_channel[1]].transfer_count;
    out->dma0_ctrl = dma_hw->ch[s_tx_dma_channel[0]].ctrl_trig;
    out->dma0_read = dma_hw->ch[s_tx_dma_channel[0]].read_addr;
    out->pio_fdebug = s_pio->fdebug;
    uint32_t pc_min = 0xffffffffu;
    uint32_t pc_max = 0u;
    for (int k = 0; k < 64; k++) {
        uint32_t pc = pio_sm_get_pc(s_pio, s_tx_sm);
        if (pc < pc_min) {
            pc_min = pc;
        }
        if (pc > pc_max) {
            pc_max = pc;
        }
    }
    out->pc_min = pc_min;
    out->pc_max = pc_max;
}

/* Re-lock both SMs to the current frame with CLEAN state. Order matters:
 *  1. clear the frame-sync IRQ so TX can only catch RX's FRESH boundary signal;
 *  2. jmp both SMs to entry FIRST, so the RX bitloop stops pushing before the
 *     FIFO clear (a push racing the clear would leave one stale halfword ahead
 *     of the fresh stream);
 *  3. pio_sm_restart both: clears the ISR/OSR shift counters, ISR contents, wait
 *     latches and any latched EXEC. THIS IS THE LOAD-BEARING STEP -- a jmp alone
 *     re-seeds only the PC, and a partially shifted OSR/ISR survives into the new
 *     lock, offsetting that direction's word boundary by the residue (the
 *     "earpiece clean, mic white-noise" bug: BCLK parking low at call end slips
 *     one extra `out` through the TX loop, so TX carried a 1-bit residue into
 *     every next call while RX parked clean). Restart does not clear the OSR
 *     *contents*: the first uplink slot after a lock emits one stale-but-aligned
 *     sample -- harmless.
 *  4. clear the RX FIFO (drop mid-word capture leftovers).
 * Deliberately does NOT clear the TX FIFO: clearing a PIO FIFO frees slots
 * WITHOUT the space-DREQs a DREQ-paced DMA counts, which permanently stalls the
 * uplink DMA (verified on HW: DMA BUSY + FIFO empty + SM TXSTALL, silent
 * forever). RX is immune -- its DREQ is regenerated by the SM pushing. Stale TX
 * FIFO words are <=4 frames of old audio, emitted bit-aligned. Register writes
 * only -> safe from the core1 DMA IRQ where the self-correct calls it. */
static void resync_lock(void) {
    pio_interrupt_clear(s_pio, MODEM_FRAME_SYNC_IRQ);
    pio_sm_exec(s_pio, s_rx_sm, pio_encode_jmp(s_rx_prog_entry));
    pio_sm_exec(s_pio, s_tx_sm, pio_encode_jmp(s_tx_prog_entry));
    pio_sm_restart(s_pio, s_rx_sm);
    pio_sm_restart(s_pio, s_tx_sm);
    pio_sm_clear_fifos(s_pio, s_rx_sm);
}

bool modem_i2s_hal_resync(void) {
    if (!s_ready || !s_running) {
        return false;
    }
    /* Bridge-start re-arm: full deterministic reset via the proven flash-park
     * quiesce/resume pair (abort + reconfigure the DMA ping-pongs, clear both
     * FIFOs -- safe here because the DMA is reconfigured, which resets the DREQ
     * credits -- restart the SMs, re-seed, re-enable). This erases EVERY piece of
     * state carried from the previous call/loopback session (shift residue, FIFO
     * leftovers, DMA buffer position/slot parity), independent of whether the modem's
     * dynamic clocks are parked or already running. IRQs are masked so our own
     * DMA_IRQ_1 handler cannot observe the half-torn-down channels. The abort is
     * issued to each complete chain and polled as one group, then both HALs are
     * rebuilt while IRQs remain masked. Runs on core1 (owns pio1). */
    uint32_t save = save_and_disable_interrupts();
    /* Stop the externally clocked modem side first. In the old codec-first
     * ordering, a slow codec abort left this 2 ms ping-pong running with IRQ1
     * masked; after 4 ms its unwrapped WRITE_ADDR crossed directly into the
     * modem UART ring. Both sides now have hardware address rings as a second
     * containment layer, but this ordering also removes the observed window. */
    bool modem_stopped = modem_i2s_hal_flash_quiesce();
    bool audio_stopped = audio_i2s_hal_flash_quiesce();
    if (audio_stopped) {
        audio_i2s_hal_flash_resume();
    }
    if (modem_stopped) {
        modem_i2s_hal_flash_resume();
    }
    /* Reset the self-correct while the DMA IRQ is still masked, so the first
     * capture_buffer_done of the fresh session cannot observe stale counters:
     * it then monitors slot-mismatch (while the bridge is active) and re-locks
     * RX+TX together until aligned. Settle skips the lock transient before the
     * first judgement. */
    if (modem_stopped && audio_stopped) {
        s_relock_attempts = 0u;
        s_relock_settle = MODEM_RELOCK_SETTLE_BUFFERS;
    }
    restore_interrupts(save);
    return modem_stopped && audio_stopped;
}

bool modem_i2s_hal_bclk_poll(bool *fresh_edges) {
    if (fresh_edges != 0) {
        *fresh_edges = false;
    }
    if (!s_ready || !s_running) {
        return false;
    }
    bool edges_seen = false;
    while (!pio_sm_is_rx_fifo_empty(s_pio, s_bclk_sm)) {
        (void)pio_sm_get(s_pio, s_bclk_sm); /* drain the detector FIFO */
        edges_seen = true;
    }
    if (fresh_edges != 0) {
        *fresh_edges = edges_seen;
    }
    if (edges_seen) {
        s_bclk_idle_calls = 0u;
        s_bclk_live = true;
    } else if (s_bclk_live) {
        if (s_bclk_idle_calls < 0xffu) {
            s_bclk_idle_calls++;
        }
        if (s_bclk_idle_calls >= MODEM_BCLK_IDLE_POLLS) {
            s_bclk_live = false;
        }
    }
    return s_bclk_live;
}

void modem_i2s_hal_bclk_reset(void) {
    if (!s_ready || !s_running) {
        return;
    }
    while (!pio_sm_is_rx_fifo_empty(s_pio, s_bclk_sm)) {
        (void)pio_sm_get(s_pio, s_bclk_sm);
    }
    s_bclk_idle_calls = 0u;
    s_bclk_live = false;
}

/* Flash-pause quiesce/resume, mirroring the codec path: STOP the modem I2S
 * DMA/PIO while core1's refill ISR is masked for a flash erase. Hardware address
 * rings contain a delayed refill, while the clean stop prevents stale audio and
 * IRQs across the operation. The BCLK edge-counter SM has no DMA and stays up.
 * Both functions run on core1, which owns pio1. */
bool modem_i2s_hal_flash_quiesce(void) {
    if (!s_ready) {
        return true;
    }
    uint32_t mask = 0;
    for (uint i = 0; i < MODEM_I2S_RX_DMA_CHANNEL_COUNT; i++) {
        uint channel = s_rx_dma_channel[i];
        dma_channel_hw_t *hw = dma_channel_hw_addr(channel);
        uint32_t ctrl = hw->al1_ctrl;
        ctrl &= ~(DMA_CH0_CTRL_TRIG_EN_BITS | DMA_CH0_CTRL_TRIG_CHAIN_TO_BITS);
        ctrl |= (channel << DMA_CH0_CTRL_TRIG_CHAIN_TO_LSB) &
                DMA_CH0_CTRL_TRIG_CHAIN_TO_BITS;
        hw->al1_ctrl = ctrl;
        mask |= 1u << channel;
    }
    for (uint i = 0; i < MODEM_I2S_TX_DMA_CHANNEL_COUNT; i++) {
        uint channel = s_tx_dma_channel[i];
        dma_channel_hw_t *hw = dma_channel_hw_addr(channel);
        uint32_t ctrl = hw->al1_ctrl;
        ctrl &= ~(DMA_CH0_CTRL_TRIG_EN_BITS | DMA_CH0_CTRL_TRIG_CHAIN_TO_BITS);
        ctrl |= (channel << DMA_CH0_CTRL_TRIG_CHAIN_TO_LSB) &
                DMA_CH0_CTRL_TRIG_CHAIN_TO_BITS;
        hw->al1_ctrl = ctrl;
        mask |= 1u << channel;
    }
    dma_hw->abort = mask;
    /* Mirror the Pico SDK's RP2350 dma_channel_abort(): after the group abort,
     * channel BUSY is the safe-state oracle. Poll all four channels under one
     * bound. CHAN_ABORT is declared write-only by the SDK, so reading it can
     * report zero before an old transfer has actually retired. */
    uint32_t guard = I2S_DMA_ABORT_SPIN_GUARD;
    while (modem_dma_busy() && guard-- != 0u) {
        tight_loop_contents();
    }
    s_quiesce_complete = !modem_dma_busy();
    if (!s_quiesce_complete) {
        s_abort_timeouts++;
    }
    pio_sm_set_enabled(s_pio, s_rx_sm, false);
    pio_sm_set_enabled(s_pio, s_tx_sm, false);
    for (uint i = 0; i < MODEM_I2S_RX_DMA_CHANNEL_COUNT; i++) {
        dma_channel_acknowledge_irq1(s_rx_dma_channel[i]);
    }
    for (uint i = 0; i < MODEM_I2S_TX_DMA_CHANNEL_COUNT; i++) {
        dma_channel_acknowledge_irq1(s_tx_dma_channel[i]);
    }
    return s_quiesce_complete;
}

void modem_i2s_hal_flash_resume(void) {
    if (!s_ready || !s_quiesce_complete) {
        return;
    }
    memset(s_rx_buffers, 0, sizeof(s_rx_buffers));
    for (uint i = 0; i < MODEM_I2S_TX_DMA_CHANNEL_COUNT; i++) {
        fill_tx_buffer(s_tx_buffers[i]);
    }
    for (uint i = 0; i < MODEM_I2S_RX_DMA_CHANNEL_COUNT; i++) {
        dma_channel_config dma_config = dma_channel_get_default_config(s_rx_dma_channel[i]);
        channel_config_set_transfer_data_size(&dma_config, DMA_SIZE_16);
        channel_config_set_chain_to(&dma_config, s_rx_dma_channel[(i + 1u) % MODEM_I2S_RX_DMA_CHANNEL_COUNT]);
        channel_config_set_read_increment(&dma_config, false);
        channel_config_set_write_increment(&dma_config, true);
        channel_config_set_ring(&dma_config, true, MODEM_I2S_RX_DMA_RING_BITS);
        channel_config_set_dreq(&dma_config, pio_get_dreq(s_pio, s_rx_sm, false));
        dma_channel_configure(s_rx_dma_channel[i], &dma_config, s_rx_buffers[i],
                              &s_pio->rxf[s_rx_sm], MODEM_I2S_BUFFER_HALFWORDS, false);
        dma_channel_acknowledge_irq1(s_rx_dma_channel[i]);
        dma_channel_set_irq1_enabled(s_rx_dma_channel[i], true);
    }
    for (uint i = 0; i < MODEM_I2S_TX_DMA_CHANNEL_COUNT; i++) {
        dma_channel_config dma_config = dma_channel_get_default_config(s_tx_dma_channel[i]);
        channel_config_set_transfer_data_size(&dma_config, DMA_SIZE_32);
        channel_config_set_chain_to(&dma_config, s_tx_dma_channel[(i + 1u) % MODEM_I2S_TX_DMA_CHANNEL_COUNT]);
        channel_config_set_read_increment(&dma_config, true);
        channel_config_set_write_increment(&dma_config, false);
        channel_config_set_ring(&dma_config, false, MODEM_I2S_TX_DMA_RING_BITS);
        channel_config_set_dreq(&dma_config, pio_get_dreq(s_pio, s_tx_sm, true));
        dma_channel_configure(s_tx_dma_channel[i], &dma_config, &s_pio->txf[s_tx_sm],
                              s_tx_buffers[i], MODEM_I2S_BUFFER_HALFWORDS, false);
        dma_channel_acknowledge_irq1(s_tx_dma_channel[i]);
        dma_channel_set_irq1_enabled(s_tx_dma_channel[i], true);
    }
    irq_clear(DMA_IRQ_1);   /* drop any NVIC-pending set while core1 IRQs were masked */

    pio_sm_clear_fifos(s_pio, s_rx_sm);
    pio_sm_clear_fifos(s_pio, s_tx_sm);
    pio_sm_restart(s_pio, s_rx_sm);
    pio_sm_restart(s_pio, s_tx_sm);
    pio_sm_clkdiv_restart(s_pio, s_rx_sm);
    pio_sm_clkdiv_restart(s_pio, s_tx_sm);
    pio_sm_exec(s_pio, s_rx_sm, pio_encode_jmp(s_rx_prog_entry));
    pio_sm_exec(s_pio, s_tx_sm, pio_encode_jmp(s_tx_prog_entry));
    /* Drop a stale frame-sync flag: if the park landed in the ns window between
     * RX's `irq set 4` and TX consuming it, re-enabling TX at its wait would lock
     * it to the DEAD session's grid (uplink-only noise, invisible to slotmiss). */
    pio_interrupt_clear(s_pio, MODEM_FRAME_SYNC_IRQ);

    pio_sm_set_enabled(s_pio, s_rx_sm, true);
    dma_channel_start(s_rx_dma_channel[0]);
    pio_sm_set_enabled(s_pio, s_tx_sm, true);
    dma_channel_start(s_tx_dma_channel[0]);
}

static void __not_in_flash_func(modem_dma_irq1_handler)(void) {
    uint32_t pending = dma_hw->ints1;
    for (uint i = 0; i < MODEM_I2S_RX_DMA_CHANNEL_COUNT; i++) {
        uint channel = s_rx_dma_channel[i];
        uint32_t mask = 1u << channel;
        if ((pending & mask) == 0) {
            continue;
        }
        dma_hw->ints1 = mask;
        capture_buffer_done(s_rx_buffers[i], MODEM_I2S_BUFFER_HALFWORDS);
        s_rx_irq_count++;
        dma_channel_set_write_addr(channel, s_rx_buffers[i], false);
    }
    for (uint i = 0; i < MODEM_I2S_TX_DMA_CHANNEL_COUNT; i++) {
        uint channel = s_tx_dma_channel[i];
        uint32_t mask = 1u << channel;
        if ((pending & mask) == 0) {
            continue;
        }
        /* Ack before the (relatively long) refill, mirroring the codec handler. */
        dma_hw->ints1 = mask;
        fill_tx_buffer(s_tx_buffers[i]);
        s_tx_irq_count++;
        dma_channel_set_read_addr(channel, s_tx_buffers[i], false);
    }
}

static void capture_buffer_done(const int16_t *src, uint32_t halfword_count) {
    if (src == 0 || halfword_count < 2u) {
        return;
    }
    s_last_wa_low = src[halfword_count - 2u];
    s_last_wa_high = src[halfword_count - 1u];

    /* Frame-lock health: the mode-14 downlink is dual mono (both WA slots carry
     * the same word; loopback drives both too), so the paired-slot difference is
     * ~0 when bit-aligned and large when the lock is off by sub-16 bits. An
     * ISR-context relock can restart the stream at an odd halfword index of the
     * in-flight DMA buffer, swapping the slot pairing for good (64 halfwords per
     * buffer keeps parity); a swapped-but-aligned capture pairs word1[n] with
     * word0[n+1] -- adjacent samples, nonzero for loud audio -- so judge BOTH
     * pairings and take the minimum: aligned reads ~0 in one of them regardless
     * of parity, while bit-straddled garbage is large in both. */
    {
        int32_t max_even = 0; /* pairing (s[2k], s[2k+1]) */
        int32_t max_odd = 0;  /* pairing (s[2k+1], s[2k+2]) */
        for (uint32_t i = 0; i + 1u < halfword_count; i++) {
            int32_t d = (int32_t)src[i] - (int32_t)src[i + 1u];
            if (d < 0) {
                d = -d;
            }
            if ((i & 1u) == 0u) {
                if (d > max_even) {
                    max_even = d;
                }
            } else {
                if (d > max_odd) {
                    max_odd = d;
                }
            }
        }
        s_last_slot_mismatch = (uint32_t)((max_even < max_odd) ? max_even : max_odd);
    }

    /* Self-correct re-lock (re-enabled). The metric IS valid on a real call: the AT
     * manual's mode-14 row specifies the downlink in BOTH WA slots ("TX channel =
     * WA HIGH & LOW", dual mono), so word0==word1 whenever the capture is aligned,
     * regardless of content. The historical real-call thrash was the relock itself:
     * without pio_sm_restart each attempt carried a fresh random shift residue (a
     * lottery); a relock is now deterministic-clean, so the first attempt should
     * land and the cap is only a safety valve. Attempts reset on any clean buffer
     * so a mid-call clock hiccup opens a fresh correction episode. Runs in the DMA
     * IRQ; resync_lock is register-writes-only, so that is safe. */
    if (audio_bridge_active()) {
        if (s_relock_settle > 0u) {
            s_relock_settle--;
        } else if (s_last_slot_mismatch <= MODEM_RELOCK_OK_MISMATCH) {
            s_relock_attempts = 0u;
        } else if (s_relock_attempts < MODEM_RELOCK_MAX_ATTEMPTS) {
            resync_lock();
            s_relock_attempts++;
            s_relock_total++;
            s_relock_settle = MODEM_RELOCK_SETTLE_BUFFERS;
        }
    }

    /* Bridge downlink producer: while a call is active, extract the downlink WA
     * slot into a mono block and push it to the downlink ring for the codec-TX
     * mixer. Dormant until audio_bridge_start() (call connect). */
    if (audio_bridge_active()) {
        uint32_t frames = halfword_count / 2u;
        if (frames > MODEM_I2S_BUFFER_FRAMES) {
            frames = MODEM_I2S_BUFFER_FRAMES;
        }
        int16_t mono[MODEM_I2S_BUFFER_FRAMES];
        for (uint32_t i = 0; i < frames; i++) {
            mono[i] = src[(i * 2u) + MODEM_DOWNLINK_SLOT];
        }
        audio_bridge_downlink_push(mono, (uint16_t)frames);
    }
}

/* Build one TX block: uplink mic in the WA-low slot (word0), zeros in the
 * WA-high slot (word1). Feeds zeros until the bridge is active. */
static void fill_tx_buffer(uint32_t *dst) {
    int16_t mono[MODEM_I2S_BUFFER_FRAMES];
    if (audio_bridge_active()) {
        audio_bridge_uplink_pull(mono, MODEM_I2S_BUFFER_FRAMES);
    } else {
        memset(mono, 0, sizeof(mono));
    }
    for (uint32_t i = 0; i < MODEM_I2S_BUFFER_FRAMES; i++) {
        /* Left-justify the mic sample into the high 16 bits (MSB-first OSR), driven
         * into BOTH WA slots. RX and TX lock to one shared grid (IRQ 4), so uplink
         * and downlink align together -- both clean or both noise, never split. */
        uint32_t w = (uint32_t)(uint16_t)mono[i] << 16;
        dst[(i * 2u) + 0u] = w;
        dst[(i * 2u) + 1u] = w;
    }
}

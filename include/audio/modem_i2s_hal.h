#ifndef MODEM_I2S_HAL_H
#define MODEM_I2S_HAL_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t rx_irq_count;
    int16_t last_wa_low;
    int16_t last_wa_high;
    uint32_t last_slot_mismatch; /* min-over-parity max|slot pair diff| of the last RX buffer; ~0 = bit-aligned (dual-mono downlink), large = frame lock off */
    uint32_t relock_attempts;    /* self-correct re-locks in the current correction episode (0 = lock judged clean) */
    uint32_t relock_total;       /* cumulative self-correct re-locks since boot (bench telemetry) */
    uint32_t abort_timeouts;     /* aborts that did not reach a safe state within the guard */
} modem_i2s_stats_t;

bool modem_i2s_hal_init(void);
/* The hardware resources are claimed/configured by init, but PIO/DMA stays
 * parked until a voice session owns it. start/stop are core1-only and
 * idempotent. A failed stop leaves running=true so standby cannot cross an
 * incompletely quiesced DMA boundary; a later stop may retry. */
bool modem_i2s_hal_start(void);
bool modem_i2s_hal_stop(void);
bool modem_i2s_hal_running(void);
void modem_i2s_hal_get_stats(modem_i2s_stats_t *out_stats);

/* DIAGNOSTIC: snapshot the uplink TX state machine + its DMA to localize why GP11
 * isn't driving. pc_min==pc_max => SM stuck on one instruction (stalled); a moving
 * PC + rising tx_irq_count => SM is consuming the FIFO (running the txloop). */
typedef struct {
    uint32_t tx_irq_count;   /* TX DMA blocks completed (SM is consuming) */
    uint32_t tx_fifo_level;  /* 0 = DMA not feeding; full (8) = SM not consuming */
    uint32_t pc_min;
    uint32_t pc_max;
    uint32_t tx_prog_entry;
    uint32_t dma0_tcount;
    uint32_t dma1_tcount;
    uint32_t dma0_ctrl;   /* raw CTRL_TRIG: EN/BUSY/error bits + DREQ sel */
    uint32_t dma0_read;   /* read addr (advances 4B/word if the DMA moved) */
    uint32_t pio_fdebug;  /* TXSTALL/TXOVER etc. */
    bool tx_sm_enabled;
    bool dma0_busy;
    bool dma1_busy;
} modem_i2s_tx_probe_t;
void modem_i2s_hal_tx_probe(modem_i2s_tx_probe_t *out);

/* Drains the GP8 BCLK edge-counter FIFO and returns whether the modem is currently
 * clocking the I2S bus (audio path / call active). `fresh_edges` reports whether
 * this poll drained at least one sample, independently of the liveness holdover;
 * bridge-start qualification uses it to require continuously running clocks.
 * Call periodically from core1. Independent of AT URCs. */
bool modem_i2s_hal_bclk_poll(bool *fresh_edges);
/* Start a new clock-observation epoch: drain old detector samples and clear the
 * liveness latch. The next true result then proves an edge from this session,
 * rather than the 150 ms loss holdover from the previous one. core1 only. */
void modem_i2s_hal_bclk_reset(void);

/* Full deterministic re-arm at bridge start: abort + rebuild the DMA ping-pongs,
 * clear FIFOs, pio_sm_restart both SMs (erases ISR/OSR shift residue -- the state
 * a jmp re-seed alone carries over, which bit-offsets one direction into white
 * noise) and re-seed them to the WA-hunt entry. Erases everything left over from
 * the previous call/loopback session, whether the modem's dynamic clocks are parked or
 * already running. Also re-arms the slotmiss self-correct. core1 only. */
/* Returns false if either DMA side could not be stopped safely. */
bool modem_i2s_hal_resync(void);

/* Flash-write coordination: STOP the modem I2S DMA/PIO while core1's refill ISR
 * is masked for a flash erase/program, then rebuild it. The DMA address rings
 * independently contain a delayed ISR; quiesce prevents stale audio and IRQs.
 * Mirror of the codec path; call quiesce before parking core1 and resume after
 * the op. Both run on core1 (owns pio1). No-op until init. */
/* Returns false if DMA did not reach a hardware-confirmed safe state. */
bool modem_i2s_hal_flash_quiesce(void);
void modem_i2s_hal_flash_resume(void);

#endif

#ifndef AUDIO_BRIDGE_H
#define AUDIO_BRIDGE_H

#include <stdbool.h>
#include <stdint.h>

/* Modem <-> NAU88C22 voice bridge: two lock-free SPSC elastic rings that
 * resample between the codec clock domain (RP clk_sys) and modem clock
 * domain (modem), which are not phase-locked. See the Digital Voice And Codec
 * section of docs/revb2_hardware_contract.md.
 *
 * Each ring has exactly one producer and one consumer, each running in a DMA
 * IRQ on core1 (codec = DMA_IRQ_0, modem = DMA_IRQ_1) at equal NVIC priority, so
 * the two never preempt each other. Producers/consumers must NOT be called from
 * core0. All bridge data flow is gated by audio_bridge_active(). */

#define AUDIO_BRIDGE_BLOCK_FRAMES 32u /* 2 ms @ 16 kHz; matches the DMA block size */

/* Lifecycle (core1). start() resets both rings and activates; stop() deactivates. */
void audio_bridge_init(void);
void audio_bridge_start(void);
void audio_bridge_stop(void);
bool audio_bridge_active(void);

/* Sample-format flags published by core0 on a route change, read by core1.
 * route matches nau_route_t; uplink_mic_channel 0=left (handset), 1=right
 * (headset); right_invert selects RP-side right-slot inversion (headset). */
void audio_bridge_set_format(uint8_t route, uint8_t uplink_mic_channel, bool right_invert);
uint8_t audio_bridge_route(void);
uint8_t audio_bridge_uplink_mic_channel(void);
bool audio_bridge_right_invert(void);

/* Producers (core1 DMA IRQ context). */
void audio_bridge_downlink_push(const int16_t *samples, uint16_t count); /* modem RX -> codec TX mixer */
void audio_bridge_uplink_push(const int16_t *samples, uint16_t count);   /* codec RX -> modem TX */

/* Consumers (core1 DMA IRQ context). Always write exactly `count` samples to
 * dst (silence/last-sample on prime/underrun). At most one elastic slip
 * (drop/duplicate) per call to track the cross-domain rate difference. */
void audio_bridge_downlink_pull(int16_t *dst, uint16_t count);
void audio_bridge_uplink_pull(int16_t *dst, uint16_t count);

/* Diagnostics: ring health for the debug console. underflow/overflow are
 * cumulative since boot; depth is the instantaneous fill (samples). A climbing
 * downlink underflow = starving ring (robotic/choppy); a flat uplink depth of 0
 * during a call = the mic path is not feeding. */
typedef struct {
    bool active;
    uint16_t downlink_depth;
    uint32_t downlink_underflow;
    uint32_t downlink_overflow;
    uint16_t uplink_depth;
    uint32_t uplink_underflow;
    uint32_t uplink_overflow;
} audio_bridge_stats_t;
void audio_bridge_get_stats(audio_bridge_stats_t *out);

#endif

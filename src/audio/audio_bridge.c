#include "audio/audio_bridge.h"

#include "hardware/sync.h"

#include <string.h>

/* Ring capacity 128 samples (8 ms @ 16 kHz), power-of-two for mask indexing.
 * Target depth 4 ms; elastic band 2..6 ms. Slip at most ±1 sample/block. */
#define AUDIO_BRIDGE_RING_CAP 128u
#define AUDIO_BRIDGE_RING_MASK (AUDIO_BRIDGE_RING_CAP - 1u)
#define AUDIO_BRIDGE_TARGET 64u  /* 4 ms */
#define AUDIO_BRIDGE_HIGH_WM 96u /* 6 ms */
#define AUDIO_BRIDGE_LOW_WM 32u  /* 2 ms */

typedef struct {
    int16_t buf[AUDIO_BRIDGE_RING_CAP];
    volatile uint16_t head; /* producer-owned */
    volatile uint16_t tail; /* consumer-owned */
    int16_t last;           /* consumer-owned: last sample, repeated on underrun */
    bool primed;            /* consumer-owned: false until depth first reaches target */
    uint32_t overflow;      /* producer-owned diag */
    uint32_t underflow;     /* consumer-owned diag */
} audio_ring_t;

static audio_ring_t s_downlink; /* modem RX -> codec TX */
static audio_ring_t s_uplink;   /* codec RX -> modem TX */
static volatile bool s_active;

/* Sample-format flags packed into one 32-bit word: written by core0
 * (set_format), read by core1. A single aligned store/load is atomic, so a route
 * change publishes with no partial-update window -- the core1 readers can never
 * see a torn mix of old/new fields, even on a future mid-call route switch.
 * bits[7:0]=route, [15:8]=uplink mic channel, [16]=right invert. */
static volatile uint32_t s_format;

#define AUDIO_FMT_PACK(route, mic, rinv)                                \
    (((uint32_t)(route) & 0xffu) | (((uint32_t)((mic) & 1u)) << 8) |    \
     (((uint32_t)((rinv) ? 1u : 0u)) << 16))
#define AUDIO_FMT_ROUTE(w) ((uint8_t)((w) & 0xffu))
#define AUDIO_FMT_MIC(w) ((uint8_t)(((w) >> 8) & 1u))
#define AUDIO_FMT_RINVERT(w) ((((w) >> 16) & 1u) != 0u)

static void ring_reset(audio_ring_t *r) {
    r->head = 0u;
    r->tail = 0u;
    r->last = 0;
    r->primed = false;
    /* overflow/underflow counters are left cumulative for diagnostics. */
}

/* Single-producer push (core1 DMA IRQ). Commits samples before publishing head. */
static void ring_push(audio_ring_t *r, const int16_t *src, uint16_t count) {
    uint16_t head = r->head;
    uint16_t tail = r->tail; /* snapshot consumer index */
    for (uint16_t i = 0; i < count; i++) {
        uint16_t next = (uint16_t)((head + 1u) & AUDIO_BRIDGE_RING_MASK);
        if (next == tail) {
            r->overflow++;
            break; /* ring full: drop remainder (shouldn't happen with slip) */
        }
        r->buf[head] = src[i];
        head = next;
    }
    __dmb();
    r->head = head;
}

/* Single-consumer pull (core1 DMA IRQ). Always writes `count` samples to dst:
 * silence until primed, last-sample on underrun, and at most one slip/block. */
static void ring_pull(audio_ring_t *r, int16_t *dst, uint16_t count) {
    uint16_t head = r->head; /* snapshot producer index */
    uint16_t tail = r->tail;
    uint16_t depth = (uint16_t)((head - tail) & AUDIO_BRIDGE_RING_MASK);

    if (!r->primed) {
        if (depth >= AUDIO_BRIDGE_TARGET) {
            r->primed = true;
        } else {
            for (uint16_t i = 0; i < count; i++) {
                dst[i] = 0;
            }
            return;
        }
    }

    /* Elastic slip: drop one source sample if too full, duplicate one if too
     * empty. The two are mutually exclusive (HIGH_WM > LOW_WM). */
    if (depth > AUDIO_BRIDGE_HIGH_WM && depth > count) {
        tail = (uint16_t)((tail + 1u) & AUDIO_BRIDGE_RING_MASK);
        depth--;
    }
    bool insert = (depth < AUDIO_BRIDGE_LOW_WM);

    for (uint16_t i = 0; i < count; i++) {
        if (insert && i == 0u) {
            dst[i] = r->last; /* duplicate one sample to slow consumption */
            continue;
        }
        if (tail != head) {
            int16_t v = r->buf[tail];
            tail = (uint16_t)((tail + 1u) & AUDIO_BRIDGE_RING_MASK);
            r->last = v;
            dst[i] = v;
        } else {
            r->underflow++;
            dst[i] = r->last;   /* underrun: repeat last */
            r->primed = false;  /* re-prime so we don't keep glitching */
        }
    }
    __dmb();
    r->tail = tail;
}

void audio_bridge_init(void) {
    ring_reset(&s_downlink);
    ring_reset(&s_uplink);
    s_active = false;
    s_format = AUDIO_FMT_PACK(0u, 0u, false); /* handset, left ADC, no invert */
}

void audio_bridge_start(void) {
    ring_reset(&s_downlink);
    ring_reset(&s_uplink);
    __dmb();
    s_active = true;
}

void audio_bridge_stop(void) {
    s_active = false;
    __dmb();
    ring_reset(&s_downlink);
    ring_reset(&s_uplink);
}

bool audio_bridge_active(void) {
    return s_active;
}

void audio_bridge_set_format(uint8_t route, uint8_t uplink_mic_channel, bool right_invert) {
    s_format = AUDIO_FMT_PACK(route, uplink_mic_channel, right_invert);
    __dmb();
}

uint8_t audio_bridge_route(void) {
    return AUDIO_FMT_ROUTE(s_format);
}

uint8_t audio_bridge_uplink_mic_channel(void) {
    return AUDIO_FMT_MIC(s_format);
}

bool audio_bridge_right_invert(void) {
    return AUDIO_FMT_RINVERT(s_format);
}

static uint16_t ring_depth(const audio_ring_t *r) {
    return (uint16_t)((r->head - r->tail) & AUDIO_BRIDGE_RING_MASK);
}

void audio_bridge_get_stats(audio_bridge_stats_t *out) {
    if (out == 0) {
        return;
    }
    out->active = s_active;
    out->downlink_depth = ring_depth(&s_downlink);
    out->downlink_underflow = s_downlink.underflow;
    out->downlink_overflow = s_downlink.overflow;
    out->uplink_depth = ring_depth(&s_uplink);
    out->uplink_underflow = s_uplink.underflow;
    out->uplink_overflow = s_uplink.overflow;
}

void audio_bridge_downlink_push(const int16_t *samples, uint16_t count) {
    ring_push(&s_downlink, samples, count);
}

void audio_bridge_uplink_push(const int16_t *samples, uint16_t count) {
    ring_push(&s_uplink, samples, count);
}

void audio_bridge_downlink_pull(int16_t *dst, uint16_t count) {
    ring_pull(&s_downlink, dst, count);
}

void audio_bridge_uplink_pull(int16_t *dst, uint16_t count) {
    ring_pull(&s_uplink, dst, count);
}

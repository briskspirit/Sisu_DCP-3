#include "services/stack_monitor.h"
#include "services/stack_monitor_logic.h"

#include <stddef.h>
#include <stdint.h>

#define STACK_CANARY_WORD 0xa55a3cc3u
#define STACK_INIT_HEADROOM_BYTES 128u
#define STACK_SAMPLE_PERIOD_MS 1000u

extern uint8_t __StackBottom;
extern uint8_t __StackTop;
extern uint8_t __StackOneBottom;
extern uint8_t __StackOneTop;

typedef struct {
    uint32_t *bottom;
    uint32_t *top;
    uint32_t *fill_end;
    uint32_t next_sample_ms;
    volatile uint32_t peak_used_bytes;
    volatile bool initialized;
    volatile bool canary_intact;
} stack_monitor_state_t;

static stack_monitor_state_t s_core0;
static stack_monitor_state_t s_core1;

static inline uintptr_t current_sp(void) {
    uintptr_t sp;
    __asm volatile("mov %0, sp" : "=r"(sp));
    return sp;
}

static inline void publish_barrier(void) {
    __asm volatile("dmb" ::: "memory");
}

/* noinline makes the captured SP include this function's own frame. The extra
 * headroom keeps the fill loop away from that live frame and from compiler /
 * library variation during early startup. */
static __attribute__((noinline)) void monitor_init(
    stack_monitor_state_t *state, uint8_t *bottom_symbol,
    uint8_t *top_symbol) {
    uintptr_t bottom = ((uintptr_t)bottom_symbol + 3u) & ~(uintptr_t)3u;
    uintptr_t top = (uintptr_t)top_symbol & ~(uintptr_t)3u;
    uintptr_t sp = current_sp();
    uintptr_t fill_end = sp > STACK_INIT_HEADROOM_BYTES
        ? (sp - STACK_INIT_HEADROOM_BYTES) & ~(uintptr_t)3u : bottom;
    if (fill_end > top) {
        fill_end = top;
    }
    if (fill_end <= bottom) {
        state->initialized = false;
        state->canary_intact = false;
        return;
    }

    uint32_t *cursor = (uint32_t *)bottom;
    uint32_t *end = (uint32_t *)fill_end;
    while (cursor < end) {
        *cursor++ = STACK_CANARY_WORD;
    }
    state->bottom = (uint32_t *)bottom;
    state->top = (uint32_t *)top;
    state->fill_end = end;
    state->next_sample_ms = 0u;
    state->peak_used_bytes = (uint32_t)(top - fill_end);
    state->canary_intact = true;
    publish_barrier();
    state->initialized = true;
}

static void monitor_sample(stack_monitor_state_t *state, uint32_t now_ms) {
    if (!state->initialized ||
        (int32_t)(now_ms - state->next_sample_ms) < 0) {
        return;
    }
    state->next_sample_ms = now_ms + STACK_SAMPLE_PERIOD_MS;

    size_t word_count = (size_t)(state->fill_end - state->bottom);
    uint32_t stack_size = (uint32_t)((uintptr_t)state->top -
                                     (uintptr_t)state->bottom);
    stack_canary_scan_t scan = stack_canary_scan(
        state->bottom, word_count, STACK_CANARY_WORD, stack_size,
        state->peak_used_bytes);
    state->peak_used_bytes = scan.peak_used_bytes;
    state->canary_intact = scan.canary_intact;
    publish_barrier();
}

static stack_monitor_core_snapshot_t snapshot_core(
    const stack_monitor_state_t *state) {
    stack_monitor_core_snapshot_t out = {0};
    publish_barrier();
    out.initialized = state->initialized;
    out.canary_intact = state->canary_intact;
    if (!out.initialized) {
        return out;
    }
    out.size_bytes = (uint32_t)((uintptr_t)state->top -
                                (uintptr_t)state->bottom);
    out.peak_used_bytes = state->peak_used_bytes;
    out.minimum_margin_bytes = out.peak_used_bytes < out.size_bytes
        ? out.size_bytes - out.peak_used_bytes : 0u;
    return out;
}

void stack_monitor_core0_init(void) {
    monitor_init(&s_core0, &__StackBottom, &__StackTop);
}

void stack_monitor_core1_init(void) {
    monitor_init(&s_core1, &__StackOneBottom, &__StackOneTop);
}

void stack_monitor_core0_sample(uint32_t now_ms) {
    monitor_sample(&s_core0, now_ms);
}

void stack_monitor_core1_sample(uint32_t now_ms) {
    monitor_sample(&s_core1, now_ms);
}

void stack_monitor_get_snapshot(stack_monitor_snapshot_t *out) {
    if (out == NULL) {
        return;
    }
    out->core0 = snapshot_core(&s_core0);
    out->core1 = snapshot_core(&s_core1);
}

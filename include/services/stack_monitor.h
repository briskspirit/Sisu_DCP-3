#ifndef STACK_MONITOR_H
#define STACK_MONITOR_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool initialized;
    bool canary_intact;
    uint32_t size_bytes;
    uint32_t peak_used_bytes;
    uint32_t minimum_margin_bytes;
} stack_monitor_core_snapshot_t;

typedef struct {
    stack_monitor_core_snapshot_t core0;
    stack_monitor_core_snapshot_t core1;
} stack_monitor_snapshot_t;

/* Call each initializer at the start of that core's long-lived entry point.
 * The SDK's PICO_USE_STACK_GUARDS MSPLIM protection remains the hard stop;
 * this service records non-destructive high-water evidence for diagnostics. */
void stack_monitor_core0_init(void);
void stack_monitor_core1_init(void);
void stack_monitor_core0_sample(uint32_t now_ms);
void stack_monitor_core1_sample(uint32_t now_ms);
void stack_monitor_get_snapshot(stack_monitor_snapshot_t *out);

#endif

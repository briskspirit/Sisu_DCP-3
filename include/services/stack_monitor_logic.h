#ifndef STACK_MONITOR_LOGIC_H
#define STACK_MONITOR_LOGIC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    bool canary_intact;
    uint32_t peak_used_bytes;
} stack_canary_scan_t;

stack_canary_scan_t stack_canary_scan(const uint32_t *words,
                                      size_t word_count,
                                      uint32_t canary,
                                      uint32_t stack_size_bytes,
                                      uint32_t previous_peak_bytes);

#endif

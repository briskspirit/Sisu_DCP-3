#include "services/stack_monitor_logic.h"

stack_canary_scan_t stack_canary_scan(const uint32_t *words,
                                      size_t word_count,
                                      uint32_t canary,
                                      uint32_t stack_size_bytes,
                                      uint32_t previous_peak_bytes) {
    stack_canary_scan_t result = {
        .canary_intact = false,
        .peak_used_bytes = previous_peak_bytes,
    };
    if (words == NULL || word_count == 0u) {
        if (result.peak_used_bytes < stack_size_bytes) {
            result.peak_used_bytes = stack_size_bytes;
        }
        return result;
    }

    result.canary_intact = words[0] == canary;
    size_t first_changed = 0u;
    while (first_changed < word_count && words[first_changed] == canary) {
        first_changed++;
    }
    uint64_t untouched_bytes = (uint64_t)first_changed * sizeof(words[0]);
    uint32_t used_bytes = untouched_bytes < stack_size_bytes
        ? stack_size_bytes - (uint32_t)untouched_bytes : 0u;
    if (used_bytes > result.peak_used_bytes) {
        result.peak_used_bytes = used_bytes;
    }
    return result;
}

#ifndef HOST_TEST_PICO_STDLIB_H
#define HOST_TEST_PICO_STDLIB_H

#include <stdbool.h>
#include <stdint.h>

typedef uint64_t absolute_time_t;

extern uint64_t host_test_time_us;

static inline absolute_time_t make_timeout_time_us(uint64_t timeout_us) {
    return host_test_time_us + timeout_us;
}

static inline bool time_reached(absolute_time_t deadline) {
    return host_test_time_us >= deadline;
}

static inline void tight_loop_contents(void) {
    host_test_time_us++;
}

static inline void sleep_ms(uint32_t milliseconds) {
    host_test_time_us += (uint64_t)milliseconds * 1000u;
}

#endif

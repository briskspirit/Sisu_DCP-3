#ifndef TEST_STUB_HARDWARE_STRUCTS_WATCHDOG_H
#define TEST_STUB_HARDWARE_STRUCTS_WATCHDOG_H

#include <stdint.h>

typedef struct {
    uint32_t scratch[8];
} watchdog_hw_t;

extern watchdog_hw_t *watchdog_hw;

#endif

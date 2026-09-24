#ifndef HOST_TEST_HARDWARE_STRUCTS_POWMAN_H
#define HOST_TEST_HARDWARE_STRUCTS_POWMAN_H

#include <stdint.h>
typedef struct { uint32_t scratch[8]; uint32_t chip_reset; } powman_hw_t;
extern powman_hw_t *powman_hw;

#endif

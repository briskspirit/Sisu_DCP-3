#ifndef TEST_HARDWARE_ADDRESSMAP_H
#define TEST_HARDWARE_ADDRESSMAP_H
#include <stdint.h>
extern uint8_t test_xip_flash[];
#define XIP_BASE ((uintptr_t)test_xip_flash)
#endif

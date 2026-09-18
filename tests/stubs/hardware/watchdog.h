#ifndef TEST_STUB_HARDWARE_WATCHDOG_H
#define TEST_STUB_HARDWARE_WATCHDOG_H

#include <stdbool.h>
#include <stdint.h>

bool watchdog_enable_caused_reboot(void);
void watchdog_disable(void);
void watchdog_enable(uint32_t delay_ms, bool pause_on_debug);
void watchdog_update(void);
void watchdog_reboot(uint32_t pc, uint32_t sp, uint32_t delay_ms);

#endif

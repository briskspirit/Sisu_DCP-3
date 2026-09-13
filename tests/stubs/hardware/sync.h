/* Host test stub for the Pico SDK <hardware/sync.h>.
 *
 * audio_service.c uses only save_and_disable_interrupts()/restore_interrupts()
 * to guard the shared s_audio state. On the host these are no-ops -- the tests
 * are single-threaded, so the critical sections are trivially satisfied. This
 * stub is reachable via the test include path (tests/stubs); the real firmware
 * build resolves hardware/sync.h from the Pico SDK. */
#ifndef HOST_TEST_HARDWARE_SYNC_H
#define HOST_TEST_HARDWARE_SYNC_H

#include <stdint.h>

static inline uint32_t save_and_disable_interrupts(void) {
    return 0u;
}

static inline void restore_interrupts(uint32_t state) {
    (void)state;
}

#endif

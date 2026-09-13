#ifndef STANDBY_SLEEP_HAL_H
#define STANDBY_SLEEP_HAL_H

#include <stdbool.h>
#include <stdint.h>

enum {
    STANDBY_SLEEP_HAL_WAKE_MODEM_RI = 1u << 0,
    STANDBY_SLEEP_HAL_WAKE_SHARED_IRQ = 1u << 1,
    STANDBY_SLEEP_HAL_WAKE_POWER_BUTTON = 1u << 2,
    STANDBY_SLEEP_HAL_WAKE_SERVICE_VBUS = 1u << 3,
    STANDBY_SLEEP_HAL_WAKE_MAINTENANCE = 1u << 4,
    STANDBY_SLEEP_HAL_WAKE_UNKNOWN = 1u << 5,
};

typedef struct {
    uint32_t saved_irq_state;
    bool active;
} standby_sleep_hal_guard_t;

typedef struct {
    uint32_t wake_mask;
    uint64_t elapsed_ms;
    uint64_t wall_time_ms;
    bool entered_dormant;
} standby_sleep_hal_result_t;

typedef enum {
    STANDBY_SLEEP_HAL_ABORTED = 0,
    STANDBY_SLEEP_HAL_WOKE = 1,
} standby_sleep_hal_status_t;

void standby_sleep_hal_init(uint64_t now_ms);
/* Start a clean eligibility window. Historical raw edges are cleared, then the
 * persistent source levels, normal service latches, and fresh raw evidence are
 * sampled before the window may begin. */
uint32_t standby_sleep_hal_prepare_wake_sources(void);
bool standby_sleep_hal_wake_levels_clear(void);

/* Returns with core0 interrupts still in their saved-disabled guard. This lets
 * the service rebase wall time and release core1 before an IRQ observes time. */
standby_sleep_hal_status_t standby_sleep_hal_enter(
    uint32_t sleep_ms,
    standby_sleep_hal_result_t *result,
    standby_sleep_hal_guard_t *guard);
void standby_sleep_hal_finish(standby_sleep_hal_guard_t *guard);

#endif

#include "services/standby_sleep_logic.h"

#include <limits.h>
#include <stddef.h>

uint32_t standby_sleep_blockers(const standby_sleep_readiness_t *readiness) {
    if (readiness == NULL) {
        return UINT32_MAX;
    }

    uint32_t blockers = 0u;
    blockers |= readiness->standby_route ? 0u : STANDBY_SLEEP_BLOCK_ROUTE;
    blockers |= readiness->backlight_off ? 0u : STANDBY_SLEEP_BLOCK_BACKLIGHT;
    blockers |= readiness->app_idle ? 0u : STANDBY_SLEEP_BLOCK_APP_WORK;
    blockers |= readiness->audio_idle ? 0u : STANDBY_SLEEP_BLOCK_AUDIO;
    blockers |= readiness->core1_idle ? 0u : STANDBY_SLEEP_BLOCK_CORE1;
    blockers |= readiness->storage_idle ? 0u : STANDBY_SLEEP_BLOCK_STORAGE;
    blockers |= readiness->modem_asleep ? 0u : STANDBY_SLEEP_BLOCK_MODEM;
    blockers |= readiness->uart_idle ? 0u : STANDBY_SLEEP_BLOCK_UART;
    blockers |= readiness->usb_absent ? 0u : STANDBY_SLEEP_BLOCK_USB;
    blockers |= readiness->wake_levels_clear ? 0u
                                              : STANDBY_SLEEP_BLOCK_WAKE_LEVEL;
    blockers |= readiness->wake_latches_clear ? 0u
                                               : STANDBY_SLEEP_BLOCK_WAKE_LATCH;
    blockers |= readiness->quiet_clock ? 0u
                                        : STANDBY_SLEEP_BLOCK_QUIET_CLOCK;
    blockers |= readiness->accessory_idle ? 0u
                                          : STANDBY_SLEEP_BLOCK_ACCESSORY;
    blockers |= readiness->battery_idle ? 0u
                                        : STANDBY_SLEEP_BLOCK_BATTERY;
    return blockers;
}

bool standby_sleep_stable_for(uint64_t now_ms, uint64_t since_ms,
                              uint32_t minimum_ms) {
    return now_ms >= since_ms && now_ms - since_ms >= minimum_ms;
}

uint64_t standby_sleep_advance_deadline(uint64_t deadline_ms,
                                        uint64_t now_ms,
                                        uint32_t period_ms) {
    if (period_ms == 0u) {
        return UINT64_MAX;
    }
    if (deadline_ms == 0u) {
        return now_ms > UINT64_MAX - period_ms
                   ? UINT64_MAX
                   : now_ms + period_ms;
    }
    if (now_ms < deadline_ms || deadline_ms == UINT64_MAX) {
        return deadline_ms;
    }

    uint64_t periods = (now_ms - deadline_ms) / period_ms + 1u;
    if (periods > (UINT64_MAX - deadline_ms) / period_ms) {
        return UINT64_MAX;
    }
    return deadline_ms + periods * period_ms;
}

uint64_t standby_sleep_maintenance_deadline(
    uint64_t periodic_deadline_ms,
    uint64_t now_ms,
    uint32_t period_ms,
    bool wall_clock_valid,
    uint32_t wall_clock_remaining_ms) {
    if (!wall_clock_valid) {
        return standby_sleep_advance_deadline(
            periodic_deadline_ms, now_ms, period_ms);
    }
    if (period_ms == 0u) {
        return UINT64_MAX;
    }
    uint32_t remaining_ms = wall_clock_remaining_ms;
    if (remaining_ms == 0u || remaining_ms > period_ms) {
        remaining_ms = period_ms;
    }
    return now_ms > UINT64_MAX - remaining_ms
               ? UINT64_MAX
               : now_ms + remaining_ms;
}

uint32_t standby_sleep_deadline_remaining(uint64_t deadline_ms,
                                          uint64_t now_ms,
                                          uint32_t minimum_ms) {
    if (deadline_ms <= now_ms) {
        return minimum_ms;
    }
    uint64_t remaining = deadline_ms - now_ms;
    if (remaining > UINT32_MAX) {
        return UINT32_MAX;
    }
    uint32_t bounded = (uint32_t)remaining;
    return bounded < minimum_ms ? minimum_ms : bounded;
}

uint32_t standby_sleep_select_duration(uint32_t maintenance_ms,
                                       uint32_t app_wake_ms,
                                       uint32_t minimum_ms) {
    uint32_t selected = maintenance_ms < app_wake_ms
                            ? maintenance_ms
                            : app_wake_ms;
    return selected < minimum_ms ? minimum_ms : selected;
}

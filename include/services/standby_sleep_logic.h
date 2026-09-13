#ifndef STANDBY_SLEEP_LOGIC_H
#define STANDBY_SLEEP_LOGIC_H

#include <stdbool.h>
#include <stdint.h>

enum {
    STANDBY_SLEEP_BLOCK_ROUTE = 1u << 0,
    STANDBY_SLEEP_BLOCK_BACKLIGHT = 1u << 1,
    STANDBY_SLEEP_BLOCK_APP_WORK = 1u << 2,
    STANDBY_SLEEP_BLOCK_AUDIO = 1u << 3,
    STANDBY_SLEEP_BLOCK_CORE1 = 1u << 4,
    STANDBY_SLEEP_BLOCK_STORAGE = 1u << 5,
    STANDBY_SLEEP_BLOCK_MODEM = 1u << 6,
    STANDBY_SLEEP_BLOCK_UART = 1u << 7,
    STANDBY_SLEEP_BLOCK_USB = 1u << 8,
    STANDBY_SLEEP_BLOCK_WAKE_LEVEL = 1u << 9,
    STANDBY_SLEEP_BLOCK_WAKE_LATCH = 1u << 10,
    STANDBY_SLEEP_BLOCK_QUIET_CLOCK = 1u << 11,
    STANDBY_SLEEP_BLOCK_ACCESSORY = 1u << 12,
    STANDBY_SLEEP_BLOCK_BATTERY = 1u << 13,
};

typedef struct {
    bool standby_route;
    bool backlight_off;
    bool app_idle;
    bool audio_idle;
    bool core1_idle;
    bool storage_idle;
    bool modem_asleep;
    bool uart_idle;
    bool usb_absent;
    bool wake_levels_clear;
    bool wake_latches_clear;
    bool quiet_clock;
    bool accessory_idle;
    bool battery_idle;
} standby_sleep_readiness_t;

uint32_t standby_sleep_blockers(const standby_sleep_readiness_t *readiness);
bool standby_sleep_stable_for(uint64_t now_ms, uint64_t since_ms,
                              uint32_t minimum_ms);
uint64_t standby_sleep_advance_deadline(uint64_t deadline_ms,
                                        uint64_t now_ms,
                                        uint32_t period_ms);
uint64_t standby_sleep_maintenance_deadline(
    uint64_t periodic_deadline_ms,
    uint64_t now_ms,
    uint32_t period_ms,
    bool wall_clock_valid,
    uint32_t wall_clock_remaining_ms);
uint32_t standby_sleep_deadline_remaining(uint64_t deadline_ms,
                                          uint64_t now_ms,
                                          uint32_t minimum_ms);
uint32_t standby_sleep_select_duration(uint32_t maintenance_ms,
                                       uint32_t app_wake_ms,
                                       uint32_t minimum_ms);

#endif

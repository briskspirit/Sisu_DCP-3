#ifndef STANDBY_SLEEP_H
#define STANDBY_SLEEP_H

#include <stdbool.h>
#include <stdint.h>

enum {
    STANDBY_SLEEP_WAKE_MODEM = 1u << 0,
    STANDBY_SLEEP_WAKE_INPUT = 1u << 1,
    STANDBY_SLEEP_WAKE_POWER_BUTTON = 1u << 2,
    STANDBY_SLEEP_WAKE_SERVICE_USB = 1u << 3,
    STANDBY_SLEEP_WAKE_MAINTENANCE = 1u << 4,
    STANDBY_SLEEP_WAKE_UNKNOWN = 1u << 5,
    STANDBY_SLEEP_WAKE_APP_TIMER = 1u << 6,
};

#define STANDBY_SLEEP_BLOCK_COUNT 14u

typedef struct {
    bool enabled;
    bool eligibility_tracking;
    uint32_t last_blockers;
    uint32_t attempts;
    uint32_t entries;
    uint32_t prearm_aborts;
    uint32_t core1_pause_failures;
    uint32_t final_recheck_aborts;
    uint32_t wake_modem_ri;
    uint32_t wake_shared_irq;
    uint32_t wake_power_button;
    uint32_t wake_service_vbus;
    uint32_t wake_maintenance;
    uint32_t wake_unknown;
    uint32_t wake_app_timer;
    uint32_t last_wake_mask;
    uint64_t next_maintenance_ms;
    uint64_t last_sleep_ms;
    uint64_t total_sleep_ms;
    uint64_t max_sleep_ms;
    uint32_t unplug_sessions;
    uint32_t unplug_samples;
    uint32_t unplug_ready_samples;
    uint32_t unplug_blocker_union;
    uint32_t unplug_last_blockers;
    uint32_t unplug_blocker_samples[STANDBY_SLEEP_BLOCK_COUNT];
} standby_sleep_diag_t;

void standby_sleep_init(uint64_t now_ms);
void standby_sleep_set_enabled(bool enabled);

/* Called after the current frame has been synchronized. app_wake_ms limits the
 * sleep for a pending timed UI update (UINT32_MAX means none). True means an
 * entry attempt crossed the normal tick schedule (whether it slept or caught
 * an arm-window event), so main must resync next_tick and begin a fresh service
 * pass. wake_mask uses the service-level STANDBY_SLEEP_WAKE_* reasons above. */
bool standby_sleep_try_enter(bool standby_route,
                             bool backlight_off,
                             bool app_idle,
                             bool usb_absent,
                             uint32_t app_wake_ms,
                             uint32_t *wake_mask);
void standby_sleep_get_diag(standby_sleep_diag_t *out);

#endif

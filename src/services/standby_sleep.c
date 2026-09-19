#include "services/standby_sleep.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>

#include "audio/audio_service.h"
#include "hal/accessory_hal.h"
#include "hal/battery_hal.h"
#include "hal/board.h"
#include "hal/board_irq_hal.h"
#include "hal/modem_uart_hal.h"
#include "hal/rtc_alarm_hal.h"
#include "hal/standby_sleep_hal.h"
#include "services/core1_services.h"
#include "services/modem_service.h"
#include "services/standby_sleep_logic.h"
#include "services/timebase.h"
#include "storage/store_service.h"
#include "services/phonebook_service.h"

#define STANDBY_SLEEP_STABLE_MS 32u
#define STANDBY_SLEEP_RETRY_MS 100u
#define STANDBY_SLEEP_MAINTENANCE_MS 60000u
#define STANDBY_SLEEP_MIN_ALARM_MS 10u

static bool s_enabled = true;
static bool s_eligibility_tracking;
static bool s_unplug_tracking;
static uint64_t s_eligible_since_ms;
static uint64_t s_retry_not_before_ms;
static standby_sleep_diag_t s_diag;

static uint32_t increment_saturating(uint32_t value) {
    return value == UINT32_MAX ? value : value + 1u;
}

static void track_unplug_blockers(bool usb_absent, uint32_t blockers) {
    if (!usb_absent) {
        s_unplug_tracking = false;
        return;
    }
    if (!s_unplug_tracking) {
        s_unplug_tracking = true;
        s_diag.unplug_sessions = increment_saturating(s_diag.unplug_sessions);
        s_diag.unplug_samples = 0u;
        s_diag.unplug_ready_samples = 0u;
        s_diag.unplug_blocker_union = 0u;
        s_diag.unplug_last_blockers = 0u;
        memset(s_diag.unplug_blocker_samples, 0,
               sizeof(s_diag.unplug_blocker_samples));
    }

    s_diag.unplug_samples = increment_saturating(s_diag.unplug_samples);
    s_diag.unplug_last_blockers = blockers;
    s_diag.unplug_blocker_union |= blockers;
    if (blockers == 0u) {
        s_diag.unplug_ready_samples =
            increment_saturating(s_diag.unplug_ready_samples);
    }
    for (uint32_t bit = 0u; bit < STANDBY_SLEEP_BLOCK_COUNT; bit++) {
        if ((blockers & (1u << bit)) != 0u) {
            s_diag.unplug_blocker_samples[bit] = increment_saturating(
                s_diag.unplug_blocker_samples[bit]);
        }
    }
}

static uint32_t service_wake_mask(uint32_t hal_mask) {
    uint32_t mask = 0u;
    mask |= (hal_mask & STANDBY_SLEEP_HAL_WAKE_MODEM_RI) != 0u
                ? STANDBY_SLEEP_WAKE_MODEM : 0u;
    mask |= (hal_mask & STANDBY_SLEEP_HAL_WAKE_SHARED_IRQ) != 0u
                ? STANDBY_SLEEP_WAKE_INPUT : 0u;
    mask |= (hal_mask & STANDBY_SLEEP_HAL_WAKE_POWER_BUTTON) != 0u
                ? STANDBY_SLEEP_WAKE_POWER_BUTTON : 0u;
    mask |= (hal_mask & STANDBY_SLEEP_HAL_WAKE_SERVICE_VBUS) != 0u
                ? STANDBY_SLEEP_WAKE_SERVICE_USB : 0u;
    mask |= (hal_mask & STANDBY_SLEEP_HAL_WAKE_MAINTENANCE) != 0u
                ? STANDBY_SLEEP_WAKE_MAINTENANCE : 0u;
    mask |= (hal_mask & STANDBY_SLEEP_HAL_WAKE_UNKNOWN) != 0u
                ? STANDBY_SLEEP_WAKE_UNKNOWN : 0u;
    return mask;
}

static standby_sleep_readiness_t collect_readiness(
    bool standby_route,
    bool backlight_off,
    bool app_idle,
    bool usb_absent) {
    return (standby_sleep_readiness_t){
        .standby_route = standby_route,
        .backlight_off = backlight_off,
        .app_idle = app_idle,
        .audio_idle = !audio_service_is_active(),
        .core1_idle = core1_services_standby_ready(),
        .storage_idle = store_service_standby_ready() && phonebook_service_idle(),
        .modem_asleep = modem_service_transport_sleep_confirmed(),
        .uart_idle = modem_uart_hal_tx_idle() && modem_uart_hal_rx_idle(),
        .usb_absent = usb_absent,
        .wake_levels_clear = standby_sleep_hal_wake_levels_clear(),
        .wake_latches_clear = board_irq_hal_wake_latches_clear(),
        .quiet_clock = board_is_xosc_lowpower(),
        .accessory_idle = accessory_hal_standby_ready(),
        .battery_idle = battery_hal_standby_ready(),
    };
}

static void reset_eligibility(uint64_t retry_at_ms) {
    s_eligibility_tracking = false;
    s_eligible_since_ms = 0u;
    s_retry_not_before_ms = retry_at_ms;
    s_diag.eligibility_tracking = false;
}

static void note_wake(uint32_t wake_mask) {
    s_diag.last_wake_mask = wake_mask;
    if ((wake_mask & STANDBY_SLEEP_WAKE_MODEM) != 0u) {
        s_diag.wake_modem_ri = increment_saturating(s_diag.wake_modem_ri);
    }
    if ((wake_mask & STANDBY_SLEEP_WAKE_INPUT) != 0u) {
        s_diag.wake_shared_irq = increment_saturating(s_diag.wake_shared_irq);
    }
    if ((wake_mask & STANDBY_SLEEP_WAKE_POWER_BUTTON) != 0u) {
        s_diag.wake_power_button =
            increment_saturating(s_diag.wake_power_button);
    }
    if ((wake_mask & STANDBY_SLEEP_WAKE_SERVICE_USB) != 0u) {
        s_diag.wake_service_vbus =
            increment_saturating(s_diag.wake_service_vbus);
    }
    if ((wake_mask & STANDBY_SLEEP_WAKE_MAINTENANCE) != 0u) {
        s_diag.wake_maintenance =
            increment_saturating(s_diag.wake_maintenance);
    }
    if ((wake_mask & STANDBY_SLEEP_WAKE_UNKNOWN) != 0u) {
        s_diag.wake_unknown = increment_saturating(s_diag.wake_unknown);
    }
    if ((wake_mask & STANDBY_SLEEP_WAKE_APP_TIMER) != 0u) {
        s_diag.wake_app_timer = increment_saturating(s_diag.wake_app_timer);
    }
}

void standby_sleep_init(uint64_t now_ms) {
    memset(&s_diag, 0, sizeof(s_diag));
    s_enabled = true;
    s_unplug_tracking = false;
    s_diag.enabled = true;
    s_diag.next_maintenance_ms = standby_sleep_maintenance_deadline(
        0u, now_ms, STANDBY_SLEEP_MAINTENANCE_MS,
        rtc_alarm_hal_time_valid(),
        rtc_alarm_hal_ms_until_next_minute(now_ms));
    reset_eligibility(0u);
    standby_sleep_hal_init(now_ms);
}

void standby_sleep_set_enabled(bool enabled) {
    s_enabled = enabled;
    s_diag.enabled = enabled;
    reset_eligibility(0u);
}

bool standby_sleep_try_enter(bool standby_route,
                             bool backlight_off,
                             bool app_idle,
                             bool usb_absent,
                             uint32_t app_wake_ms,
                             uint32_t *wake_mask) {
    if (wake_mask != NULL) {
        *wake_mask = 0u;
    }
    uint64_t now_ms = time_ms64();
    /* The existing once-per-minute maintenance wake also owns the standby
     * clock refresh. Rebase it onto the RTC minute boundary instead of adding
     * a second app timer; invalid RTC time retains the monotonic cadence. */
    s_diag.next_maintenance_ms = standby_sleep_maintenance_deadline(
        s_diag.next_maintenance_ms, now_ms,
        STANDBY_SLEEP_MAINTENANCE_MS,
        rtc_alarm_hal_time_valid(),
        rtc_alarm_hal_ms_until_next_minute(now_ms));

    standby_sleep_readiness_t readiness = collect_readiness(
        standby_route, backlight_off, app_idle, usb_absent);
    uint32_t blockers = standby_sleep_blockers(&readiness);
    s_diag.last_blockers = blockers;
    track_unplug_blockers(usb_absent, blockers);
    if (!s_enabled || blockers != 0u) {
        reset_eligibility(0u);
        return false;
    }
    if (now_ms < s_retry_not_before_ms) {
        return false;
    }
    if (!s_eligibility_tracking) {
        /* Clear historical edge bits now, then require a full clean window.
         * The HAL first preserves any scan-to-prepare edge; enter() never ACKs
         * between its final check and arm. */
        uint32_t prepare_wake_mask = service_wake_mask(
            standby_sleep_hal_prepare_wake_sources());
        if (prepare_wake_mask != 0u) {
            s_diag.prearm_aborts =
                increment_saturating(s_diag.prearm_aborts);
            note_wake(prepare_wake_mask);
            if (wake_mask != NULL) {
                *wake_mask = prepare_wake_mask;
            }
            reset_eligibility(now_ms + STANDBY_SLEEP_RETRY_MS);
            return true;
        }
        s_eligibility_tracking = true;
        s_eligible_since_ms = now_ms;
        s_diag.eligibility_tracking = true;
        return false;
    }
    if (!standby_sleep_stable_for(now_ms, s_eligible_since_ms,
                                  STANDBY_SLEEP_STABLE_MS)) {
        return false;
    }

    s_diag.attempts = increment_saturating(s_diag.attempts);
    if (!core1_flash_pause_begin()) {
        s_diag.core1_pause_failures =
            increment_saturating(s_diag.core1_pause_failures);
        reset_eligibility(now_ms + STANDBY_SLEEP_RETRY_MS);
        return true;
    }

    /* flash_pause_begin restored full clock to make its bounded handshake
     * deterministic. With core1 now parked, return to the known 6 MHz launch
     * point so the dormant HAL's post-wake board_exit path is state-correct. */
    board_enter_xosc_lowpower();
    readiness = collect_readiness(standby_route, backlight_off, app_idle,
                                   usb_absent);
    blockers = standby_sleep_blockers(&readiness) &
               ~STANDBY_SLEEP_BLOCK_CORE1;
    s_diag.last_blockers = blockers;
    if (blockers != 0u) {
        s_diag.final_recheck_aborts =
            increment_saturating(s_diag.final_recheck_aborts);
        core1_flash_pause_end();
        reset_eligibility(now_ms + STANDBY_SLEEP_RETRY_MS);
        return true;
    }

    uint32_t maintenance_ms = standby_sleep_deadline_remaining(
        s_diag.next_maintenance_ms, now_ms, STANDBY_SLEEP_MIN_ALARM_MS);
    bool app_timer_selected = app_wake_ms < maintenance_ms;
    uint32_t remaining_ms = standby_sleep_select_duration(
        maintenance_ms, app_wake_ms, STANDBY_SLEEP_MIN_ALARM_MS);
    standby_sleep_hal_result_t result;
    standby_sleep_hal_guard_t guard;
    standby_sleep_hal_status_t status = standby_sleep_hal_enter(
        remaining_ms, &result, &guard);

    if (status == STANDBY_SLEEP_HAL_WOKE && result.entered_dormant) {
        timebase_rebase_ms(result.wall_time_ms);
        s_diag.entries = increment_saturating(s_diag.entries);
        s_diag.last_sleep_ms = result.elapsed_ms;
        if (UINT64_MAX - s_diag.total_sleep_ms < result.elapsed_ms) {
            s_diag.total_sleep_ms = UINT64_MAX;
        } else {
            s_diag.total_sleep_ms += result.elapsed_ms;
        }
        if (result.elapsed_ms > s_diag.max_sleep_ms) {
            s_diag.max_sleep_ms = result.elapsed_ms;
        }
    } else {
        s_diag.prearm_aborts = increment_saturating(s_diag.prearm_aborts);
    }
    uint32_t service_mask = service_wake_mask(result.wake_mask);
    if (app_timer_selected &&
        (service_mask & STANDBY_SLEEP_WAKE_MAINTENANCE) != 0u) {
        service_mask &= ~STANDBY_SLEEP_WAKE_MAINTENANCE;
        service_mask |= STANDBY_SLEEP_WAKE_APP_TIMER;
    }
    note_wake(service_mask);
    if (wake_mask != NULL) {
        *wake_mask = service_mask;
    }

    /* Keep IRQs masked through both handoffs: core1 sees the rebased time first,
     * then pending IO/UART IRQs may resume against a coherent two-core system. */
    core1_flash_pause_end();
    standby_sleep_hal_finish(&guard);
    reset_eligibility(time_ms64() + STANDBY_SLEEP_RETRY_MS);
    return true;
}

void standby_sleep_get_diag(standby_sleep_diag_t *out) {
    if (out != NULL) {
        *out = s_diag;
    }
}

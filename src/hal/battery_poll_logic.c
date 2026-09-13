#include "hal/battery_poll_logic.h"

#include <stddef.h>

static bool deadline_reached(uint32_t now_ms, uint32_t deadline_ms) {
    return (int32_t)(now_ms - deadline_ms) >= 0;
}

void battery_poll_schedule_init(battery_poll_schedule_t *schedule) {
    if (schedule == NULL) {
        return;
    }
    *schedule = (battery_poll_schedule_t){0};
}

bool battery_poll_status_due(const battery_poll_schedule_t *schedule,
                             uint32_t now_ms) {
    return schedule != NULL &&
           (!schedule->status_started ||
            deadline_reached(now_ms, schedule->next_status_ms));
}

void battery_poll_note_status(battery_poll_schedule_t *schedule,
                              uint32_t now_ms,
                              bool observation_changed) {
    if (schedule == NULL) {
        return;
    }
    schedule->status_started = true;
    schedule->next_status_ms = now_ms + BATTERY_STATUS_POLL_MS;
    if (observation_changed) {
        battery_poll_request_adc(schedule, now_ms);
    }
}

bool battery_poll_adc_due(const battery_poll_schedule_t *schedule,
                          uint32_t now_ms) {
    return schedule != NULL &&
           (!schedule->adc_started ||
            deadline_reached(now_ms, schedule->next_adc_ms));
}

void battery_poll_note_adc(battery_poll_schedule_t *schedule,
                           uint32_t now_ms,
                           bool transition_pending) {
    if (schedule == NULL) {
        return;
    }
    schedule->adc_started = true;
    schedule->next_adc_ms = now_ms +
        (transition_pending ? BATTERY_ADC_CONFIRM_MS
                            : BATTERY_ADC_MAINTENANCE_MS);
}

void battery_poll_request_adc(battery_poll_schedule_t *schedule,
                              uint32_t now_ms) {
    if (schedule == NULL) {
        return;
    }
    schedule->adc_started = true;
    schedule->next_adc_ms = now_ms;
}

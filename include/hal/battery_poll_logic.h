#ifndef BATTERY_POLL_LOGIC_H
#define BATTERY_POLL_LOGIC_H

#include <stdbool.h>
#include <stdint.h>

#define BATTERY_STATUS_POLL_MS 1000u
#define BATTERY_ADC_MAINTENANCE_MS 60000u
#define BATTERY_ADC_CONFIRM_MS 80u

typedef struct {
    uint32_t next_status_ms;
    uint32_t next_adc_ms;
    bool status_started;
    bool adc_started;
} battery_poll_schedule_t;

void battery_poll_schedule_init(battery_poll_schedule_t *schedule);
bool battery_poll_status_due(const battery_poll_schedule_t *schedule,
                             uint32_t now_ms);
void battery_poll_note_status(battery_poll_schedule_t *schedule,
                              uint32_t now_ms,
                              bool observation_changed);
bool battery_poll_adc_due(const battery_poll_schedule_t *schedule,
                          uint32_t now_ms);
void battery_poll_note_adc(battery_poll_schedule_t *schedule,
                           uint32_t now_ms,
                           bool transition_pending);
void battery_poll_request_adc(battery_poll_schedule_t *schedule,
                              uint32_t now_ms);

#endif

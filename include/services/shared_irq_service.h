#ifndef SHARED_IRQ_SERVICE_H
#define SHARED_IRQ_SERVICE_H

#include <stdbool.h>
#include <stdint.h>

#include "services/shared_irq_logic.h"

typedef struct {
    uint32_t drain_count;
    uint32_t total_rounds;
    uint32_t stuck_count;
    uint32_t tca_service_count;
    uint32_t tca_key_events;
    uint32_t tca_gpi_events;
    uint32_t rtc_alarm_events;
    uint32_t rtc_timer_events;
    uint32_t ltc_alerts;
    uint32_t ltc_ara_failures;
    uint32_t source_errors[3];
    uint8_t last_serviced_mask;
    uint8_t last_error_mask;
    uint8_t last_tca_int_status;
    uint8_t last_tca_gpi_status[3];
    uint8_t last_rtc_flags;
    uint8_t last_ltc_status;
    bool last_line_released;
} shared_irq_service_snapshot_t;

void shared_irq_service_init(uint32_t now_ms);
void shared_irq_service_poll(uint32_t now_ms);

/* Run the same bounded drain synchronously. Dormant preparation uses this
 * before arming wake sources; false means the physical line did not release. */
bool shared_irq_service_drain_now(
    shared_irq_drain_result_t *out);
void shared_irq_service_get_snapshot(
    shared_irq_service_snapshot_t *out);

#endif

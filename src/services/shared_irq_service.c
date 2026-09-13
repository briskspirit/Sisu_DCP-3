#include "services/shared_irq_service.h"

#include <limits.h>
#include <string.h>

#include "hal/board_irq_hal.h"
#include "hal/ltc2959_hal.h"
#include "hal/rtc_alarm_hal.h"
#include "hal/rv8803_hal.h"
#include "hal/tca8418_hal.h"
#include "services/timebase.h"

#define SHARED_IRQ_STUCK_RETRY_MS 100u

static shared_irq_service_snapshot_t s_snapshot;
static uint32_t s_retry_not_before_ms;
static bool s_initialized;

static void sat_increment(uint32_t *value) {
    if (*value != UINT32_MAX) {
        (*value)++;
    }
}

static void sat_add(uint32_t *value, uint32_t addend) {
    if (UINT32_MAX - *value < addend) {
        *value = UINT32_MAX;
    } else {
        *value += addend;
    }
}

static shared_irq_source_result_t service_tca(void *ctx) {
    (void)ctx;
    tca8418_irq_result_t tca;
    tca8418_hal_service_interrupt(&tca);
    if (tca.did_work) {
        s_snapshot.last_tca_int_status = tca.int_status;
        memcpy(
            s_snapshot.last_tca_gpi_status,
            tca.gpi_status,
            sizeof(s_snapshot.last_tca_gpi_status)
        );
    }
    if (tca.did_work) {
        sat_increment(&s_snapshot.tca_service_count);
    }
    sat_add(&s_snapshot.tca_key_events, tca.key_events);
    if (tca.gpi_status[0] != 0u ||
        tca.gpi_status[1] != 0u ||
        tca.gpi_status[2] != 0u) {
        sat_increment(&s_snapshot.tca_gpi_events);
    }
    if (!tca.transport_ok) {
        sat_increment(&s_snapshot.source_errors[0]);
    }
    return (shared_irq_source_result_t){
        .transport_ok = tca.transport_ok,
        .did_work = tca.did_work,
        .more_work = tca.more_work,
    };
}

static shared_irq_source_result_t service_rtc(void *ctx) {
    (void)ctx;
    rv8803_irq_result_t rtc;
    rv8803_hal_service_interrupt(&rtc);
    if (rtc.did_work) {
        s_snapshot.last_rtc_flags = rtc.flags;
    }
    if (rtc.alarm) {
        rtc_alarm_hal_capture_alarm_interrupt();
        sat_increment(&s_snapshot.rtc_alarm_events);
    }
    if (rtc.timer) {
        sat_increment(&s_snapshot.rtc_timer_events);
    }
    if (!rtc.transport_ok) {
        sat_increment(&s_snapshot.source_errors[1]);
    }
    return (shared_irq_source_result_t){
        .transport_ok = rtc.transport_ok,
        .did_work = rtc.did_work,
        .more_work = rtc.more_work,
    };
}

static shared_irq_source_result_t service_ltc(void *ctx) {
    (void)ctx;
    ltc2959_irq_result_t ltc;
    ltc2959_hal_service_alert(&ltc);
    if (ltc.did_work) {
        s_snapshot.last_ltc_status = ltc.status;
    }
    if (ltc.did_work) {
        sat_increment(&s_snapshot.ltc_alerts);
    }
    if (ltc.did_work && !ltc.ara_released) {
        sat_increment(&s_snapshot.ltc_ara_failures);
    }
    if (!ltc.transport_ok) {
        sat_increment(&s_snapshot.source_errors[2]);
    }
    return (shared_irq_source_result_t){
        .transport_ok = ltc.transport_ok,
        .did_work = ltc.did_work,
        .more_work = ltc.more_work,
    };
}

static bool line_asserted(void *ctx) {
    (void)ctx;
    return board_irq_hal_shared_asserted();
}

static shared_irq_drain_result_t drain(void) {
    const shared_irq_ops_t ops = {
        .tca = service_tca,
        .rtc = service_rtc,
        .ltc = service_ltc,
        .line_asserted = line_asserted,
        .ctx = NULL,
    };
    shared_irq_drain_result_t result =
        shared_irq_drain(&ops);
    sat_increment(&s_snapshot.drain_count);
    sat_add(&s_snapshot.total_rounds, result.rounds);
    if (result.stuck) {
        sat_increment(&s_snapshot.stuck_count);
    }
    s_snapshot.last_serviced_mask = result.serviced_mask;
    s_snapshot.last_error_mask = result.error_mask;
    s_snapshot.last_line_released = result.line_released;
    return result;
}

void shared_irq_service_init(uint32_t now_ms) {
    memset(&s_snapshot, 0, sizeof(s_snapshot));
    board_irq_hal_init();
    s_retry_not_before_ms = now_ms;
    s_initialized = true;
}

void shared_irq_service_poll(uint32_t now_ms) {
    if (!s_initialized) {
        return;
    }
    bool pending = board_irq_hal_take_shared_pending();
    bool asserted = board_irq_hal_shared_asserted();
    if (!pending &&
        (!asserted ||
         time_diff_ms(now_ms, s_retry_not_before_ms) < 0)) {
        return;
    }

    shared_irq_drain_result_t result = drain();
    s_retry_not_before_ms = result.line_released
        ? now_ms
        : now_ms + SHARED_IRQ_STUCK_RETRY_MS;
}

bool shared_irq_service_drain_now(
    shared_irq_drain_result_t *out) {
    if (!s_initialized) {
        return false;
    }
    (void)board_irq_hal_take_shared_pending();
    shared_irq_drain_result_t result = drain();
    if (out != NULL) {
        *out = result;
    }
    return result.line_released &&
           !result.stuck &&
           result.error_mask == 0u;
}

void shared_irq_service_get_snapshot(
    shared_irq_service_snapshot_t *out) {
    if (out != NULL) {
        *out = s_snapshot;
    }
}

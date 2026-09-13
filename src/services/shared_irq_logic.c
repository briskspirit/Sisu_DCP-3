#include "services/shared_irq_logic.h"

#include <stddef.h>

static shared_irq_source_result_t call_source(
    shared_irq_source_fn_t source,
    void *ctx) {
    if (source == NULL) {
        return (shared_irq_source_result_t){
            .transport_ok = true,
        };
    }
    return source(ctx);
}

shared_irq_drain_result_t shared_irq_drain(const shared_irq_ops_t *ops) {
    shared_irq_drain_result_t result = {0};
    if (ops == NULL || ops->line_asserted == NULL) {
        result.stuck = true;
        return result;
    }

    for (uint8_t round = 0u; round < SHARED_IRQ_MAX_ROUNDS; round++) {
        shared_irq_source_result_t tca =
            call_source(ops->tca, ops->ctx);
        shared_irq_source_result_t rtc =
            call_source(ops->rtc, ops->ctx);
        shared_irq_source_result_t ltc =
            call_source(ops->ltc, ops->ctx);
        uint8_t round_error_mask = 0u;
        result.rounds++;

        if (tca.did_work) {
            result.serviced_mask |= SHARED_IRQ_SOURCE_TCA;
        }
        if (rtc.did_work) {
            result.serviced_mask |= SHARED_IRQ_SOURCE_RTC;
        }
        if (ltc.did_work) {
            result.serviced_mask |= SHARED_IRQ_SOURCE_LTC;
        }
        if (!tca.transport_ok) {
            result.error_mask |= SHARED_IRQ_SOURCE_TCA;
            round_error_mask |= SHARED_IRQ_SOURCE_TCA;
        }
        if (!rtc.transport_ok) {
            result.error_mask |= SHARED_IRQ_SOURCE_RTC;
            round_error_mask |= SHARED_IRQ_SOURCE_RTC;
        }
        if (!ltc.transport_ok) {
            result.error_mask |= SHARED_IRQ_SOURCE_LTC;
            round_error_mask |= SHARED_IRQ_SOURCE_LTC;
        }

        bool line_low = ops->line_asserted(ops->ctx);
        bool more =
            tca.more_work || rtc.more_work || ltc.more_work;
        if (!line_low && !more) {
            result.line_released = true;
            return result;
        }

        /* If every source transport failed, another immediate round only
         * repeats bounded bus timeouts. Leave it to the paced retry service. */
        if (round_error_mask ==
            (SHARED_IRQ_SOURCE_TCA |
             SHARED_IRQ_SOURCE_RTC |
             SHARED_IRQ_SOURCE_LTC)) {
            break;
        }
    }

    result.stuck = true;
    return result;
}

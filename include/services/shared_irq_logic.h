#ifndef SHARED_IRQ_LOGIC_H
#define SHARED_IRQ_LOGIC_H

#include <stdbool.h>
#include <stdint.h>

#define SHARED_IRQ_SOURCE_TCA (1u << 0)
#define SHARED_IRQ_SOURCE_RTC (1u << 1)
#define SHARED_IRQ_SOURCE_LTC (1u << 2)
#define SHARED_IRQ_MAX_ROUNDS 4u

typedef struct {
    bool transport_ok;
    bool did_work;
    bool more_work;
} shared_irq_source_result_t;

typedef shared_irq_source_result_t (*shared_irq_source_fn_t)(void *ctx);
typedef bool (*shared_irq_line_fn_t)(void *ctx);

typedef struct {
    shared_irq_source_fn_t tca;
    shared_irq_source_fn_t rtc;
    shared_irq_source_fn_t ltc;
    shared_irq_line_fn_t line_asserted;
    void *ctx;
} shared_irq_ops_t;

typedef struct {
    uint8_t rounds;
    uint8_t serviced_mask;
    uint8_t error_mask;
    bool line_released;
    bool stuck;
} shared_irq_drain_result_t;

shared_irq_drain_result_t shared_irq_drain(const shared_irq_ops_t *ops);

#endif

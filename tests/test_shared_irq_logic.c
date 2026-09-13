#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "services/shared_irq_logic.h"

typedef struct {
    shared_irq_source_result_t script[3][SHARED_IRQ_MAX_ROUNDS];
    uint8_t calls[3];
    bool line[SHARED_IRQ_MAX_ROUNDS];
    uint8_t line_calls;
} fixture_t;

static shared_irq_source_result_t source(fixture_t *f, uint8_t index) {
    uint8_t call = f->calls[index]++;
    return f->script[index][call < SHARED_IRQ_MAX_ROUNDS
                                 ? call
                                 : SHARED_IRQ_MAX_ROUNDS - 1u];
}

static shared_irq_source_result_t tca(void *ctx) {
    return source(ctx, 0u);
}

static shared_irq_source_result_t rtc(void *ctx) {
    return source(ctx, 1u);
}

static shared_irq_source_result_t ltc(void *ctx) {
    return source(ctx, 2u);
}

static bool line_asserted(void *ctx) {
    fixture_t *f = ctx;
    uint8_t call = f->line_calls++;
    return f->line[call < SHARED_IRQ_MAX_ROUNDS
                       ? call
                       : SHARED_IRQ_MAX_ROUNDS - 1u];
}

static shared_irq_ops_t ops(fixture_t *f) {
    return (shared_irq_ops_t){
        .tca = tca,
        .rtc = rtc,
        .ltc = ltc,
        .line_asserted = line_asserted,
        .ctx = f,
    };
}

static void mark_all_ok(fixture_t *f) {
    for (uint8_t source_index = 0u; source_index < 3u;
         source_index++) {
        for (uint8_t round = 0u; round < SHARED_IRQ_MAX_ROUNDS;
             round++) {
            f->script[source_index][round].transport_ok = true;
        }
    }
}

static void test_each_source_and_combinations(void) {
    for (uint8_t mask = 1u; mask < 8u; mask++) {
        fixture_t f;
        memset(&f, 0, sizeof(f));
        mark_all_ok(&f);
        for (uint8_t source_index = 0u; source_index < 3u;
             source_index++) {
            f.script[source_index][0].did_work =
                (mask & (1u << source_index)) != 0u;
        }
        shared_irq_ops_t test_ops = ops(&f);
        shared_irq_drain_result_t result =
            shared_irq_drain(&test_ops);
        assert(result.line_released);
        assert(!result.stuck);
        assert(result.serviced_mask == mask);
        assert(f.calls[0] == 1u);
        assert(f.calls[1] == 1u);
        assert(f.calls[2] == 1u);
    }
}

static void test_reassert_and_bound(void) {
    fixture_t f;
    memset(&f, 0, sizeof(f));
    mark_all_ok(&f);
    f.script[0][0].did_work = true;
    f.script[1][0].more_work = true;
    f.script[1][1].did_work = true;
    f.line[0] = true;
    f.line[1] = false;
    shared_irq_ops_t test_ops = ops(&f);
    shared_irq_drain_result_t result = shared_irq_drain(&test_ops);
    assert(result.rounds == 2u);
    assert(result.line_released);
    assert(result.serviced_mask ==
           (SHARED_IRQ_SOURCE_TCA | SHARED_IRQ_SOURCE_RTC));

    memset(&f, 0, sizeof(f));
    mark_all_ok(&f);
    for (uint8_t i = 0u; i < SHARED_IRQ_MAX_ROUNDS; i++) {
        f.line[i] = true;
    }
    test_ops = ops(&f);
    result = shared_irq_drain(&test_ops);
    assert(result.rounds == SHARED_IRQ_MAX_ROUNDS);
    assert(result.stuck);
    assert(!result.line_released);
}

static void test_all_transport_failure_stops_early(void) {
    fixture_t f;
    memset(&f, 0, sizeof(f));
    f.line[0] = true;
    shared_irq_ops_t test_ops = ops(&f);
    shared_irq_drain_result_t result = shared_irq_drain(&test_ops);
    assert(result.rounds == 1u);
    assert(result.stuck);
    assert(result.error_mask == 7u);
}

static void test_historical_errors_do_not_stop_recovered_sources(void) {
    fixture_t f;
    memset(&f, 0, sizeof(f));
    mark_all_ok(&f);
    f.script[0][0].transport_ok = false;
    f.script[1][1].transport_ok = false;
    f.script[2][2].transport_ok = false;
    f.line[0] = true;
    f.line[1] = true;
    f.line[2] = true;
    f.line[3] = false;

    shared_irq_ops_t test_ops = ops(&f);
    shared_irq_drain_result_t result = shared_irq_drain(&test_ops);
    assert(result.rounds == 4u);
    assert(result.line_released);
    assert(!result.stuck);
    assert(result.error_mask == 7u);
}

int main(void) {
    test_each_source_and_combinations();
    test_reassert_and_bound();
    test_all_transport_failure_stops_early();
    test_historical_errors_do_not_stop_recovered_sources();
    puts("shared IRQ logic tests passed");
    return 0;
}

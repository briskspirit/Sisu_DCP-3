#ifndef MODEM_DIAG_ENGINE_H
#define MODEM_DIAG_ENGINE_H

#include <stdbool.h>
#include <stdint.h>

#include "services/modem_diag.h"

typedef struct {
    const char *command;
    uint32_t timeout_ms;
} modem_diag_dispatch_t;

/* Mutable Net Monitor query state. Callers serialize access; the engine knows
 * nothing about UART, locks, call priority, or the selected modem vendor. */
typedef struct {
    modem_diag_snapshot_t snapshot;
    modem_diag_snapshot_t shadow;
    const modem_diag_query_t *queries;
    modem_diag_group_finish_fn finish_group;
    modem_diag_group_t selected_group;
    modem_diag_group_t active_group;
    uint32_t selected_generation;
    uint32_t active_generation;
    uint32_t command_started_ms;
    uint32_t admissions;
    uint32_t coalesced;
    uint32_t cancelled;
    uint32_t completed;
    uint32_t command_failures;
    uint32_t command_timeouts;
    uint32_t malformed_lines;
    uint32_t max_command_latency_ms;
    uint8_t query_count_total;
    uint8_t query_ordinal;
    uint8_t active_query_count;
    uint8_t response_lines;
    bool response_invalid;
    bool pending;
    bool group_active;
} modem_diag_engine_t;

void modem_diag_engine_init(modem_diag_engine_t *engine,
                            const modem_diag_query_t *queries,
                            uint8_t query_count,
                            modem_diag_group_finish_fn finish_group,
                            bool backend_available);
bool modem_diag_engine_group_supported(const modem_diag_engine_t *engine,
                                       modem_diag_group_t group);
void modem_diag_engine_mark_unsupported(modem_diag_engine_t *engine,
                                        modem_diag_group_t group);
bool modem_diag_engine_same_subscription(const modem_diag_engine_t *engine,
                                         modem_diag_group_t group,
                                         uint32_t generation);
void modem_diag_engine_set_subscription(modem_diag_engine_t *engine,
                                        modem_diag_group_t group,
                                        uint32_t generation);
bool modem_diag_engine_request(modem_diag_engine_t *engine, uint32_t now_ms);
void modem_diag_engine_cancel_active(modem_diag_engine_t *engine,
                                     bool external_command,
                                     uint32_t now_ms);
bool modem_diag_engine_prepare_next(modem_diag_engine_t *engine,
                                    uint32_t now_ms,
                                    modem_diag_dispatch_t *out);
bool modem_diag_engine_parse_line(modem_diag_engine_t *engine,
                                  const char *line,
                                  bool protected_unprefixed_line);
void modem_diag_engine_finish_command(modem_diag_engine_t *engine,
                                      bool ok, bool timed_out,
                                      uint32_t now_ms);
void modem_diag_engine_invalidate_all(modem_diag_engine_t *engine,
                                      uint32_t now_ms);
void modem_diag_engine_copy_snapshot(const modem_diag_engine_t *engine,
                                     modem_diag_snapshot_t *out);
bool modem_diag_engine_has_work(const modem_diag_engine_t *engine);
bool modem_diag_engine_has_pending(const modem_diag_engine_t *engine);
uint32_t modem_diag_engine_selected_generation(
    const modem_diag_engine_t *engine);

#endif

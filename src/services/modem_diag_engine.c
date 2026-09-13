#include "services/modem_diag_engine.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>

#include "services/modem_at_util.h"

static const modem_diag_query_t *query_at(const modem_diag_engine_t *engine,
                                          modem_diag_group_t group,
                                          uint8_t ordinal,
                                          uint8_t *out_count) {
    uint8_t count = 0u;
    const modem_diag_query_t *found = NULL;
    if (engine != NULL && engine->queries != NULL) {
        for (uint8_t i = 0u; i < engine->query_count_total; i++) {
            const modem_diag_query_t *query = &engine->queries[i];
            if (query->group != group) {
                continue;
            }
            if (count == ordinal) {
                found = query;
            }
            count++;
        }
    }
    if (out_count != NULL) {
        *out_count = count;
    }
    return found;
}

static void clear_group(modem_diag_snapshot_t *snapshot,
                        modem_diag_group_t group) {
    if (snapshot == NULL) {
        return;
    }
    switch (group) {
    case MODEM_DIAG_GROUP_SERVING:
        memset(&snapshot->serving, 0, sizeof(snapshot->serving));
        break;
    case MODEM_DIAG_GROUP_REGISTRATION:
        memset(&snapshot->registration, 0, sizeof(snapshot->registration));
        break;
    case MODEM_DIAG_GROUP_RADIO_POLICY:
        memset(&snapshot->radio_policy, 0, sizeof(snapshot->radio_policy));
        break;
    case MODEM_DIAG_GROUP_TUNER:
        memset(&snapshot->tuner, 0, sizeof(snapshot->tuner));
        break;
    case MODEM_DIAG_GROUP_PACKET:
        memset(&snapshot->packet, 0, sizeof(snapshot->packet));
        break;
    case MODEM_DIAG_GROUP_SIM:
        memset(&snapshot->sim, 0, sizeof(snapshot->sim));
        break;
    case MODEM_DIAG_GROUP_VOICE:
        memset(&snapshot->voice, 0, sizeof(snapshot->voice));
        break;
    case MODEM_DIAG_GROUP_TEMPERATURE:
        memset(&snapshot->temperature, 0, sizeof(snapshot->temperature));
        break;
    case MODEM_DIAG_GROUP_IDENTITY:
        memset(&snapshot->identity, 0, sizeof(snapshot->identity));
        break;
    case MODEM_DIAG_GROUP_DVI:
        memset(&snapshot->dvi, 0, sizeof(snapshot->dvi));
        break;
    case MODEM_DIAG_GROUP_CALL_CAUSE:
        memset(&snapshot->call_cause, 0, sizeof(snapshot->call_cause));
        break;
    case MODEM_DIAG_GROUP_SMS_CONFIG:
        memset(&snapshot->sms, 0, sizeof(snapshot->sms));
        break;
    case MODEM_DIAG_GROUP_STORAGE_CAPS:
        memset(&snapshot->storage, 0, sizeof(snapshot->storage));
        break;
    case MODEM_DIAG_GROUP_NONE:
    case MODEM_DIAG_GROUP_COUNT:
        break;
    }
    if (group > MODEM_DIAG_GROUP_NONE && group < MODEM_DIAG_GROUP_COUNT) {
        snapshot->group[group].present_fields = 0u;
    }
}

static void commit_group(modem_diag_engine_t *engine,
                         modem_diag_group_t group) {
    switch (group) {
    case MODEM_DIAG_GROUP_SERVING:
        engine->snapshot.serving = engine->shadow.serving;
        break;
    case MODEM_DIAG_GROUP_REGISTRATION:
        engine->snapshot.registration = engine->shadow.registration;
        break;
    case MODEM_DIAG_GROUP_RADIO_POLICY:
        engine->snapshot.radio_policy = engine->shadow.radio_policy;
        break;
    case MODEM_DIAG_GROUP_TUNER:
        engine->snapshot.tuner = engine->shadow.tuner;
        break;
    case MODEM_DIAG_GROUP_PACKET:
        engine->snapshot.packet = engine->shadow.packet;
        break;
    case MODEM_DIAG_GROUP_SIM:
        engine->snapshot.sim = engine->shadow.sim;
        break;
    case MODEM_DIAG_GROUP_VOICE:
        engine->snapshot.voice = engine->shadow.voice;
        break;
    case MODEM_DIAG_GROUP_TEMPERATURE:
        engine->snapshot.temperature = engine->shadow.temperature;
        break;
    case MODEM_DIAG_GROUP_IDENTITY:
        engine->snapshot.identity = engine->shadow.identity;
        break;
    case MODEM_DIAG_GROUP_DVI:
        engine->snapshot.dvi = engine->shadow.dvi;
        break;
    case MODEM_DIAG_GROUP_CALL_CAUSE:
        engine->snapshot.call_cause = engine->shadow.call_cause;
        break;
    case MODEM_DIAG_GROUP_SMS_CONFIG:
        engine->snapshot.sms = engine->shadow.sms;
        break;
    case MODEM_DIAG_GROUP_STORAGE_CAPS:
        engine->snapshot.storage = engine->shadow.storage;
        break;
    case MODEM_DIAG_GROUP_NONE:
    case MODEM_DIAG_GROUP_COUNT:
        return;
    }
    engine->snapshot.group[group] = engine->shadow.group[group];
    engine->snapshot.updated_ms = engine->shadow.updated_ms;
}

static void mark_failure(modem_diag_engine_t *engine,
                         modem_diag_group_t group,
                         modem_diag_error_t error,
                         uint32_t now_ms) {
    if (engine == NULL || group <= MODEM_DIAG_GROUP_NONE ||
        group >= MODEM_DIAG_GROUP_COUNT) {
        return;
    }
    modem_diag_group_meta_t *meta = &engine->snapshot.group[group];
    meta->last_attempt_ms = now_ms;
    meta->last_error = error;
    if ((error == MODEM_DIAG_ERROR_REJECTED ||
         error == MODEM_DIAG_ERROR_TIMEOUT ||
         error == MODEM_DIAG_ERROR_MALFORMED) &&
        meta->consecutive_failures != UINT8_MAX) {
        meta->consecutive_failures++;
    }
    meta->state = meta->sequence != 0u
                      ? MODEM_DIAG_STATE_STALE
                      : MODEM_DIAG_STATE_ERROR;
    engine->snapshot.updated_ms = now_ms;
}

void modem_diag_engine_init(modem_diag_engine_t *engine,
                            const modem_diag_query_t *queries,
                            uint8_t query_count,
                            modem_diag_group_finish_fn finish_group,
                            bool backend_available) {
    if (engine == NULL) {
        return;
    }
    memset(engine, 0, sizeof(*engine));
    engine->queries = queries;
    engine->query_count_total = query_count;
    engine->finish_group = finish_group;
    engine->snapshot.backend_available = backend_available;
    for (uint8_t i = 1u; i < (uint8_t)MODEM_DIAG_GROUP_COUNT; i++) {
        engine->snapshot.group[i].state =
            queries != NULL ? MODEM_DIAG_STATE_UNKNOWN
                            : MODEM_DIAG_STATE_UNSUPPORTED;
    }
}

bool modem_diag_engine_group_supported(const modem_diag_engine_t *engine,
                                       modem_diag_group_t group) {
    uint8_t count = 0u;
    if (engine == NULL || group <= MODEM_DIAG_GROUP_NONE ||
        group >= MODEM_DIAG_GROUP_COUNT || engine->finish_group == NULL) {
        return false;
    }
    (void)query_at(engine, group, 0u, &count);
    return count != 0u;
}

void modem_diag_engine_mark_unsupported(modem_diag_engine_t *engine,
                                        modem_diag_group_t group) {
    if (engine == NULL || group <= MODEM_DIAG_GROUP_NONE ||
        group >= MODEM_DIAG_GROUP_COUNT) {
        return;
    }
    engine->snapshot.group[group].state = MODEM_DIAG_STATE_UNSUPPORTED;
    engine->snapshot.group[group].last_error = MODEM_DIAG_ERROR_UNAVAILABLE;
}

bool modem_diag_engine_same_subscription(const modem_diag_engine_t *engine,
                                         modem_diag_group_t group,
                                         uint32_t generation) {
    return engine != NULL && engine->selected_group == group &&
           engine->selected_generation == generation;
}

void modem_diag_engine_set_subscription(modem_diag_engine_t *engine,
                                        modem_diag_group_t group,
                                        uint32_t generation) {
    if (engine == NULL) {
        return;
    }
    engine->selected_group = group;
    engine->selected_generation = generation;
}

bool modem_diag_engine_request(modem_diag_engine_t *engine, uint32_t now_ms) {
    if (engine == NULL || engine->selected_group == MODEM_DIAG_GROUP_NONE ||
        engine->selected_generation == 0u) {
        return false;
    }
    if (engine->pending ||
        (engine->group_active &&
         engine->selected_group == engine->active_group &&
         engine->selected_generation == engine->active_generation)) {
        engine->coalesced++;
        return true;
    }
    engine->pending = true;
    engine->admissions++;
    modem_diag_group_meta_t *meta =
        &engine->snapshot.group[engine->selected_group];
    meta->state = MODEM_DIAG_STATE_PENDING;
    meta->last_attempt_ms = now_ms;
    return true;
}

void modem_diag_engine_cancel_active(modem_diag_engine_t *engine,
                                     bool external_command,
                                     uint32_t now_ms) {
    if (engine == NULL) {
        return;
    }
    bool had_work = engine->pending || engine->group_active || external_command;
    if (had_work) {
        engine->cancelled++;
        modem_diag_group_t group = engine->group_active
            ? engine->active_group : engine->selected_group;
        mark_failure(engine, group, MODEM_DIAG_ERROR_CANCELLED, now_ms);
    }
    engine->pending = false;
    engine->group_active = false;
    engine->active_group = MODEM_DIAG_GROUP_NONE;
    engine->active_generation = 0u;
    engine->query_ordinal = 0u;
    engine->active_query_count = 0u;
    engine->response_lines = 0u;
    engine->response_invalid = false;
}

bool modem_diag_engine_prepare_next(modem_diag_engine_t *engine,
                                    uint32_t now_ms,
                                    modem_diag_dispatch_t *out) {
    if (engine == NULL || out == NULL || !engine->pending ||
        engine->selected_group == MODEM_DIAG_GROUP_NONE ||
        engine->selected_generation == 0u) {
        return false;
    }
    if (!engine->group_active) {
        engine->shadow = engine->snapshot;
        clear_group(&engine->shadow, engine->selected_group);
        engine->active_group = engine->selected_group;
        engine->active_generation = engine->selected_generation;
        engine->query_ordinal = 0u;
        (void)query_at(engine, engine->active_group, 0u,
                       &engine->active_query_count);
        if (engine->active_query_count == 0u) {
            mark_failure(engine, engine->active_group,
                         MODEM_DIAG_ERROR_UNAVAILABLE, now_ms);
            engine->pending = false;
            return false;
        }
        engine->group_active = true;
        modem_diag_group_meta_t *meta =
            &engine->shadow.group[engine->active_group];
        meta->state = MODEM_DIAG_STATE_PENDING;
        meta->last_error = MODEM_DIAG_ERROR_NONE;
        meta->last_attempt_ms = now_ms;
        meta->present_fields = 0u;
    }

    const modem_diag_query_t *query =
        query_at(engine, engine->active_group, engine->query_ordinal, NULL);
    if (query == NULL || query->cmd == NULL || query->parse == NULL) {
        mark_failure(engine, engine->active_group,
                     MODEM_DIAG_ERROR_UNAVAILABLE, now_ms);
        engine->pending = false;
        engine->group_active = false;
        return false;
    }

    engine->pending = false;
    engine->response_lines = 0u;
    engine->response_invalid = false;
    engine->command_started_ms = now_ms;
    out->command = query->cmd;
    out->timeout_ms = query->timeout_ms;
    return true;
}

bool modem_diag_engine_parse_line(modem_diag_engine_t *engine,
                                  const char *line,
                                  bool protected_unprefixed_line) {
    if (engine == NULL || !engine->group_active || line == NULL ||
        engine->active_generation != engine->selected_generation ||
        engine->active_group != engine->selected_group) {
        return false;
    }
    const modem_diag_query_t *query =
        query_at(engine, engine->active_group, engine->query_ordinal, NULL);
    if (query == NULL ||
        (query->prefix != NULL &&
         !modem_at_starts_with(line, query->prefix)) ||
        (query->prefix == NULL && protected_unprefixed_line)) {
        return false;
    }
    modem_diag_line_result_t result = query->parse(line, &engine->shadow);
    if (result == MODEM_DIAG_LINE_ACCEPT) {
        if (engine->response_lines != UINT8_MAX) {
            engine->response_lines++;
        }
    } else if (result == MODEM_DIAG_LINE_INVALID) {
        engine->response_invalid = true;
        engine->malformed_lines++;
    }
    return result != MODEM_DIAG_LINE_IGNORE;
}

void modem_diag_engine_finish_command(modem_diag_engine_t *engine,
                                      bool ok, bool timed_out,
                                      uint32_t now_ms) {
    if (engine == NULL) {
        return;
    }
    uint32_t latency = now_ms - engine->command_started_ms;
    if (latency > engine->max_command_latency_ms) {
        engine->max_command_latency_ms = latency;
    }
    if (!engine->group_active) {
        return;
    }
    if (engine->active_generation != engine->selected_generation ||
        engine->active_group != engine->selected_group) {
        engine->group_active = false;
        engine->active_group = MODEM_DIAG_GROUP_NONE;
        return;
    }

    const modem_diag_query_t *query =
        query_at(engine, engine->active_group, engine->query_ordinal, NULL);
    bool response_ok = ok && !engine->response_invalid && query != NULL &&
                       engine->response_lines >= query->minimum_lines;
    /* Explicit ERROR may mean an optional query is unsupported. A timeout is a
     * transport failure and must fail the atomic group. A backend may also
     * isolate malformed output from a purely informational optional query; the
     * malformed-line counter still records that evidence. */
    if (!response_ok && query != NULL && query->optional && !timed_out &&
        (!engine->response_invalid || query->isolate_malformed)) {
        response_ok = true;
    }
    if (!response_ok) {
        modem_diag_error_t error = MODEM_DIAG_ERROR_REJECTED;
        if (timed_out) {
            error = MODEM_DIAG_ERROR_TIMEOUT;
            engine->command_timeouts++;
        } else if (engine->response_invalid ||
                   (ok && query != NULL &&
                    engine->response_lines < query->minimum_lines)) {
            error = MODEM_DIAG_ERROR_MALFORMED;
        } else {
            engine->command_failures++;
        }
        mark_failure(engine, engine->active_group, error, now_ms);
        engine->group_active = false;
        engine->active_group = MODEM_DIAG_GROUP_NONE;
        engine->pending = false;
        return;
    }

    engine->query_ordinal++;
    if (engine->query_ordinal < engine->active_query_count) {
        engine->pending = true;
        return;
    }

    bool complete = engine->finish_group != NULL &&
        engine->finish_group(engine->active_group, &engine->shadow);
    if (!complete) {
        engine->malformed_lines++;
        mark_failure(engine, engine->active_group,
                     MODEM_DIAG_ERROR_MALFORMED, now_ms);
    } else {
        modem_diag_group_meta_t *meta =
            &engine->shadow.group[engine->active_group];
        meta->state = MODEM_DIAG_STATE_FRESH;
        meta->last_error = MODEM_DIAG_ERROR_NONE;
        meta->last_attempt_ms = now_ms;
        meta->last_success_ms = now_ms;
        meta->consecutive_failures = 0u;
        meta->sequence++;
        if (meta->sequence == 0u) {
            meta->sequence = 1u;
        }
        engine->shadow.updated_ms = now_ms;
        commit_group(engine, engine->active_group);
        engine->completed++;
    }
    engine->group_active = false;
    engine->active_group = MODEM_DIAG_GROUP_NONE;
    engine->pending = false;
}

void modem_diag_engine_invalidate_all(modem_diag_engine_t *engine,
                                      uint32_t now_ms) {
    if (engine == NULL) {
        return;
    }
    for (uint8_t i = 1u; i < (uint8_t)MODEM_DIAG_GROUP_COUNT; i++) {
        modem_diag_group_meta_t *meta = &engine->snapshot.group[i];
        if (meta->state == MODEM_DIAG_STATE_UNSUPPORTED) {
            continue;
        }
        meta->state = meta->sequence != 0u
                          ? MODEM_DIAG_STATE_STALE
                          : MODEM_DIAG_STATE_UNKNOWN;
        meta->last_error = MODEM_DIAG_ERROR_UNAVAILABLE;
        meta->consecutive_failures = 0u;
    }
    engine->snapshot.updated_ms = now_ms;
}

void modem_diag_engine_copy_snapshot(const modem_diag_engine_t *engine,
                                     modem_diag_snapshot_t *out) {
    if (engine == NULL || out == NULL) {
        return;
    }
    *out = engine->snapshot;
    out->scheduler.generation = engine->selected_generation;
    out->scheduler.selected_group = engine->selected_group;
    out->scheduler.active_group = engine->group_active
        ? engine->active_group : MODEM_DIAG_GROUP_NONE;
    out->scheduler.active_query = engine->query_ordinal;
    out->scheduler.active_query_count = engine->active_query_count;
    out->scheduler.pending = engine->pending || engine->group_active;
    out->scheduler.admissions = engine->admissions;
    out->scheduler.coalesced = engine->coalesced;
    out->scheduler.cancelled = engine->cancelled;
    out->scheduler.completed = engine->completed;
    out->scheduler.command_failures = engine->command_failures;
    out->scheduler.command_timeouts = engine->command_timeouts;
    out->scheduler.malformed_lines = engine->malformed_lines;
    out->scheduler.max_command_latency_ms = engine->max_command_latency_ms;
}

bool modem_diag_engine_has_work(const modem_diag_engine_t *engine) {
    return engine != NULL && (engine->pending || engine->group_active);
}

bool modem_diag_engine_has_pending(const modem_diag_engine_t *engine) {
    return engine != NULL && engine->pending;
}

uint32_t modem_diag_engine_selected_generation(
    const modem_diag_engine_t *engine) {
    return engine != NULL ? engine->selected_generation : 0u;
}

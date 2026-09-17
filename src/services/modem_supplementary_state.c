#include "services/modem_supplementary_state.h"

#include <string.h>

typedef struct {
    bool needed;
    uint8_t retries_remaining;
    uint32_t generation;
    uint32_t not_before_ms;
    uint32_t in_flight_generation;
    uint8_t in_flight_retries_remaining;
} supplementary_refresh_t;

static supplementary_refresh_t
    s_refresh[MODEM_SUPPLEMENTARY_REFRESH_COUNT];
static modem_supplementary_cache_t s_cache;

static uint32_t s_call_forward_next_request_id;
static call_forward_result_t s_call_forward_result;
static bool s_call_forward_result_pending;
static bool s_call_forward_row_seen;
static bool s_call_forward_row_invalid;
static bool s_call_forward_voice_seen;
static bool s_call_forward_voice_active;
static char s_call_forward_voice_number[MODEM_PHONE_MAX + 1u];
static bool s_call_forward_voice_has_delay;
static uint8_t s_call_forward_voice_delay_seconds;
static uint8_t s_call_forward_step_count;
static uint8_t s_call_forward_step_index;
static bool s_call_forward_mutated;

static bool s_voice_mailbox_row_invalid;
static bool s_voice_mailbox_candidate_voice;
static char s_voice_mailbox_candidate[MODEM_PHONE_MAX + 1u];

static bool s_message_waiting_row_seen;
static bool s_message_waiting_row_invalid;
static modem_message_waiting_category_mask_t
    s_message_waiting_uncertain_mask;
static bool s_message_waiting_clear_all_seen;
static bool s_message_waiting_category_seen[MODEM_MESSAGE_WAITING_CATEGORY_COUNT];
static bool s_message_waiting_category_active[MODEM_MESSAGE_WAITING_CATEGORY_COUNT];
static uint16_t
    s_message_waiting_category_count[MODEM_MESSAGE_WAITING_CATEGORY_COUNT];

static void copy_bounded(char *dst, size_t dst_cap, const char *src) {
    if (dst == NULL || dst_cap == 0u) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    size_t length = strlen(src);
    if (length >= dst_cap) {
        length = dst_cap - 1u;
    }
    memcpy(dst, src, length);
    dst[length] = '\0';
}

static supplementary_refresh_t *refresh_for_kind(
    modem_supplementary_refresh_kind_t kind) {
    return (unsigned)kind < MODEM_SUPPLEMENTARY_REFRESH_COUNT
        ? &s_refresh[kind]
        : NULL;
}

static void refresh_reset(supplementary_refresh_t *refresh) {
    if (refresh == NULL) {
        return;
    }
    refresh->generation++;
    if (refresh->generation == 0u) {
        refresh->generation = 1u;
    }
    refresh->needed = false;
    refresh->retries_remaining = 0u;
    refresh->not_before_ms = 0u;
    refresh->in_flight_generation = 0u;
    refresh->in_flight_retries_remaining = 0u;
}

static void refresh_finish(modem_supplementary_refresh_kind_t kind,
                           bool ok, uint32_t now_ms) {
    supplementary_refresh_t *refresh = refresh_for_kind(kind);
    if (refresh == NULL) {
        return;
    }
    uint32_t generation = refresh->in_flight_generation;
    uint8_t retries_remaining = refresh->in_flight_retries_remaining;
    refresh->in_flight_generation = 0u;
    refresh->in_flight_retries_remaining = 0u;
    if (generation == 0u || refresh->generation != generation) {
        /* A newer observation already armed or resolved this authority. Its
         * decision must survive the older command final. */
        return;
    }
    if (ok) {
        refresh->retries_remaining = 0u;
        return;
    }
    if (retries_remaining > 0u) {
        refresh->needed = true;
        refresh->retries_remaining = (uint8_t)(retries_remaining - 1u);
        refresh->not_before_ms =
            now_ms + MODEM_SUPPLEMENTARY_REFRESH_RETRY_MS;
    } else {
        refresh->retries_remaining = 0u;
    }
}

static void refresh_resolved(modem_supplementary_refresh_kind_t kind) {
    refresh_reset(refresh_for_kind(kind));
}

static void call_forward_reset_collect(void) {
    s_call_forward_row_seen = false;
    s_call_forward_row_invalid = false;
    s_call_forward_voice_seen = false;
    s_call_forward_voice_active = false;
    s_call_forward_voice_number[0] = '\0';
    s_call_forward_voice_has_delay = false;
    s_call_forward_voice_delay_seconds = 0u;
}

static void voice_mailbox_reset_collect(void) {
    s_voice_mailbox_row_invalid = false;
    s_voice_mailbox_candidate_voice = false;
    s_voice_mailbox_candidate[0] = '\0';
}

static void message_waiting_reset_collect(void) {
    s_message_waiting_row_seen = false;
    s_message_waiting_row_invalid = false;
    s_message_waiting_uncertain_mask = 0u;
    s_message_waiting_clear_all_seen = false;
    memset(s_message_waiting_category_seen, 0,
           sizeof(s_message_waiting_category_seen));
    memset(s_message_waiting_category_active, 0,
           sizeof(s_message_waiting_category_active));
    memset(s_message_waiting_category_count, 0,
           sizeof(s_message_waiting_category_count));
}

void modem_supplementary_init(void) {
    memset(s_refresh, 0, sizeof(s_refresh));
    memset(&s_cache, 0, sizeof(s_cache));
    s_call_forward_next_request_id = 0u;
    memset(&s_call_forward_result, 0, sizeof(s_call_forward_result));
    s_call_forward_result_pending = false;
    modem_supplementary_reset_transient();
}

void modem_supplementary_reset_transient(void) {
    modem_supplementary_call_forward_reset();
    voice_mailbox_reset_collect();
    message_waiting_reset_collect();
    modem_supplementary_refresh_reset_all();
}

void modem_supplementary_invalidate_cache(void) {
    memset(&s_cache, 0, sizeof(s_cache));
}

void modem_supplementary_get_cache(modem_supplementary_cache_t *out) {
    if (out != NULL) {
        *out = s_cache;
    }
}

void modem_supplementary_refresh_reset_all(void) {
    for (uint8_t i = 0u; i < MODEM_SUPPLEMENTARY_REFRESH_COUNT; i++) {
        refresh_reset(&s_refresh[i]);
    }
}

void modem_supplementary_refresh_arm(
    modem_supplementary_refresh_kind_t kind, uint32_t now_ms) {
    supplementary_refresh_t *refresh = refresh_for_kind(kind);
    if (refresh == NULL) {
        return;
    }
    refresh->generation++;
    if (refresh->generation == 0u) {
        refresh->generation = 1u;
    }
    refresh->needed = true;
    refresh->retries_remaining = MODEM_SUPPLEMENTARY_REFRESH_RETRY_LIMIT;
    refresh->not_before_ms = now_ms;
}

bool modem_supplementary_refresh_due(
    modem_supplementary_refresh_kind_t kind, uint32_t now_ms) {
    supplementary_refresh_t *refresh = refresh_for_kind(kind);
    return refresh != NULL && refresh->needed &&
           (int32_t)(now_ms - refresh->not_before_ms) >= 0;
}

void modem_supplementary_refresh_begin(
    modem_supplementary_refresh_kind_t kind) {
    supplementary_refresh_t *refresh = refresh_for_kind(kind);
    if (refresh == NULL) {
        return;
    }
    refresh->needed = false;
    refresh->in_flight_generation = refresh->generation;
    refresh->in_flight_retries_remaining = refresh->retries_remaining;
}

uint32_t modem_supplementary_call_forward_next_request_id(void) {
    do {
        s_call_forward_next_request_id++;
    } while (s_call_forward_next_request_id == 0u);
    return s_call_forward_next_request_id;
}

bool modem_supplementary_call_forward_flags_finish(bool ok, uint32_t now_ms) {
    supplementary_refresh_t *refresh =
        refresh_for_kind(MODEM_SUPPLEMENTARY_REFRESH_CFU);
    bool changed = false;
    /* A real CFU flag (solicited or unsolicited) already resolves this
     * generation. A valid reply with no flag leaves the state unknown. */
    if (ok && refresh->in_flight_generation != 0u &&
        refresh->in_flight_generation == refresh->generation) {
        changed = s_cache.call_forward_unconditional_known;
        s_cache.call_forward_unconditional_known = false;
    }
    refresh_finish(MODEM_SUPPLEMENTARY_REFRESH_CFU, ok, now_ms);
    return changed;
}

void modem_supplementary_call_forward_publish_result(
    uint32_t request_id, const call_forward_request_t *request,
    call_forward_outcome_t outcome, bool status_known, bool active,
    const char *number, bool has_delay, uint8_t delay_seconds) {
    if (request == NULL || request_id == 0u) {
        return;
    }
    call_forward_result_t result;
    memset(&result, 0, sizeof(result));
    result.pending = true;
    result.request_id = request_id;
    result.request = *request;
    result.outcome = outcome;
    result.status_known = status_known;
    result.active = active;
    copy_bounded(result.number, sizeof(result.number), number);
    result.has_delay = has_delay;
    result.delay_seconds = delay_seconds;

    /* A detached unabortable request may complete after a newer visible one.
     * Preserve the newer result using serial-number ordering at uint32 wrap. */
    uint32_t id_delta = request_id - s_call_forward_result.request_id;
    if (!s_call_forward_result_pending ||
        (id_delta != 0u && id_delta < UINT32_C(0x80000000))) {
        s_call_forward_result = result;
        s_call_forward_result_pending = true;
    }
}

bool modem_supplementary_call_forward_pop_result(call_forward_result_t *out) {
    if (out == NULL || !s_call_forward_result_pending) {
        return false;
    }
    *out = s_call_forward_result;
    s_call_forward_result_pending = false;
    return true;
}

void modem_supplementary_call_forward_begin(uint8_t step_count) {
    modem_supplementary_call_forward_reset();
    s_call_forward_step_count = step_count;
}

void modem_supplementary_call_forward_reset(void) {
    call_forward_reset_collect();
    s_call_forward_step_count = 0u;
    s_call_forward_step_index = 0u;
    s_call_forward_mutated = false;
}

void modem_supplementary_call_forward_collect_invalid(void) {
    s_call_forward_row_invalid = true;
}

void modem_supplementary_call_forward_collect_row(
    const call_forward_row_t *row) {
    if (row == NULL) {
        modem_supplementary_call_forward_collect_invalid();
        return;
    }
    s_call_forward_row_seen = true;
    if ((row->class_mask & 1u) == 0u) {
        return;
    }
    if (s_call_forward_voice_seen) {
        bool conflict = s_call_forward_voice_active != row->active;
        if (row->active && s_call_forward_voice_active) {
            conflict = conflict ||
                strcmp(s_call_forward_voice_number,
                       row->has_number ? row->number : "") != 0 ||
                s_call_forward_voice_has_delay != row->has_delay ||
                (row->has_delay &&
                 s_call_forward_voice_delay_seconds != row->delay_seconds);
        }
        if (conflict) {
            s_call_forward_row_invalid = true;
        }
        return;
    }
    s_call_forward_voice_seen = true;
    s_call_forward_voice_active = row->active;
    if (row->has_number) {
        copy_bounded(s_call_forward_voice_number,
                     sizeof(s_call_forward_voice_number), row->number);
    }
    s_call_forward_voice_has_delay = row->has_delay;
    s_call_forward_voice_delay_seconds = row->delay_seconds;
}

bool modem_supplementary_call_forward_step_complete(
    bool ok, bool timed_out, bool query, uint8_t *next_step_out) {
    if (next_step_out != NULL) {
        *next_step_out = 0u;
    }
    if (!ok || timed_out) {
        return false;
    }
    if (!query) {
        s_call_forward_mutated = true;
    }
    uint8_t next_step = (uint8_t)(s_call_forward_step_index + 1u);
    if (next_step >= s_call_forward_step_count) {
        return false;
    }
    s_call_forward_step_index = next_step;
    if (next_step_out != NULL) {
        *next_step_out = next_step;
    }
    return true;
}

bool modem_supplementary_call_forward_cancel_uncertain(
    bool command_in_flight, bool query) {
    return s_call_forward_mutated || (command_in_flight && !query);
}

bool modem_supplementary_call_forward_finish(
    uint32_t request_id, const call_forward_request_t *request,
    bool ok, bool timed_out, bool network_registered, uint32_t now_ms,
    bool *status_updated_out) {
    if (status_updated_out != NULL) {
        *status_updated_out = false;
    }
    if (request == NULL) {
        return false;
    }

    bool query = request->action == CALL_FORWARD_ACTION_QUERY;
    bool internal_cfu_refresh = request_id == 0u && query &&
        request->reason == CALL_FORWARD_REASON_UNCONDITIONAL;
    bool mutation_possible = s_call_forward_mutated ||
                             (timed_out && !query);
    bool semantic_ok = ok;
    bool status_known = false;
    bool active = false;
    const char *number = NULL;
    bool has_delay = false;
    uint8_t delay_seconds = 0u;
    call_forward_outcome_t outcome;

    if (timed_out) {
        outcome = CALL_FORWARD_OUTCOME_RESULT_UNKNOWN;
        semantic_ok = false;
    } else if (!ok && s_call_forward_mutated) {
        outcome = CALL_FORWARD_OUTCOME_RESULT_UNKNOWN;
        semantic_ok = false;
    } else if (!ok) {
        outcome = network_registered ? CALL_FORWARD_OUTCOME_NOT_DONE
                                     : CALL_FORWARD_OUTCOME_NO_NETWORK;
    } else if (query &&
               (!s_call_forward_row_seen || s_call_forward_row_invalid)) {
        outcome = CALL_FORWARD_OUTCOME_RESULT_UNKNOWN;
        semantic_ok = false;
    } else {
        outcome = CALL_FORWARD_OUTCOME_SUCCESS;
        if (query) {
            status_known = true;
            active = s_call_forward_voice_seen &&
                     s_call_forward_voice_active;
            number = s_call_forward_voice_number;
            has_delay = s_call_forward_voice_has_delay;
            delay_seconds = s_call_forward_voice_delay_seconds;
        }
    }

    if (query && status_known &&
        request->reason == CALL_FORWARD_REASON_UNCONDITIONAL) {
        s_cache.call_forward_unconditional_known = true;
        s_cache.call_forward_unconditional_active = active;
        if (status_updated_out != NULL) {
            *status_updated_out = true;
        }
        if (!internal_cfu_refresh) {
            refresh_resolved(MODEM_SUPPLEMENTARY_REFRESH_CFU);
        }
    }
    if (internal_cfu_refresh) {
        refresh_finish(MODEM_SUPPLEMENTARY_REFRESH_CFU, status_known, now_ms);
    }
    if (!query && mutation_possible &&
        (request->reason == CALL_FORWARD_REASON_UNCONDITIONAL ||
         request->reason == CALL_FORWARD_REASON_ALL)) {
        s_cache.call_forward_unconditional_known = false;
        if (status_updated_out != NULL) {
            *status_updated_out = true;
        }
        modem_supplementary_refresh_arm(MODEM_SUPPLEMENTARY_REFRESH_CFU,
                                        now_ms);
    }

    modem_supplementary_call_forward_publish_result(
        request_id, request, outcome, status_known, active, number,
        has_delay, delay_seconds);
    return semantic_ok;
}

void modem_supplementary_voice_mailbox_begin(void) {
    voice_mailbox_reset_collect();
}

void modem_supplementary_voice_mailbox_collect_row(
    const modem_voice_mailbox_row_t *row) {
    if (row == NULL) {
        modem_supplementary_voice_mailbox_collect_invalid();
        return;
    }
    if (row->voice && !s_voice_mailbox_candidate_voice) {
        copy_bounded(s_voice_mailbox_candidate,
                     sizeof(s_voice_mailbox_candidate), row->number);
        s_voice_mailbox_candidate_voice = true;
    }
}

void modem_supplementary_voice_mailbox_collect_invalid(void) {
    s_voice_mailbox_row_invalid = true;
}

bool modem_supplementary_voice_mailbox_finish(bool ok, uint32_t now_ms) {
    bool result_ok = ok && !s_voice_mailbox_row_invalid;
    if (result_ok) {
        s_cache.voice_mailbox_known = true;
        if (s_voice_mailbox_candidate_voice) {
            copy_bounded(s_cache.voice_mailbox_number,
                         sizeof(s_cache.voice_mailbox_number),
                         s_voice_mailbox_candidate);
        } else {
            s_cache.voice_mailbox_number[0] = '\0';
        }
    }
    refresh_finish(MODEM_SUPPLEMENTARY_REFRESH_VOICE_MAILBOX, result_ok,
                   now_ms);
    voice_mailbox_reset_collect();
    return result_ok;
}

bool modem_supplementary_voice_mailbox_get(char *out, size_t out_cap) {
    if (out == NULL || out_cap == 0u) {
        return false;
    }
    bool available = s_cache.voice_mailbox_known &&
                     s_cache.voice_mailbox_number[0] != '\0' &&
                     strlen(s_cache.voice_mailbox_number) < out_cap;
    if (available) {
        copy_bounded(out, out_cap, s_cache.voice_mailbox_number);
    } else {
        out[0] = '\0';
    }
    return available;
}

void modem_supplementary_message_waiting_begin(void) {
    message_waiting_reset_collect();
}

void modem_supplementary_message_waiting_collect_row(
    modem_message_waiting_row_result_t result,
    const modem_aux_event_t *event) {
    if (event == NULL) {
        s_message_waiting_row_invalid = true;
        return;
    }
    if (result == MODEM_MESSAGE_WAITING_ROW_AMBIGUOUS) {
        modem_message_waiting_category_mask_t valid_mask = 0u;
        for (uint8_t i = 0u; i < MODEM_MESSAGE_WAITING_CATEGORY_COUNT; i++) {
            valid_mask |= modem_message_waiting_category_bit(
                (modem_message_waiting_category_t)i);
        }
        if (event->kind != MODEM_AUX_EVENT_MESSAGE_WAITING ||
            event->message_waiting_uncertain_mask == 0u ||
            (event->message_waiting_uncertain_mask &
             (modem_message_waiting_category_mask_t)~valid_mask) != 0u) {
            s_message_waiting_row_invalid = true;
            return;
        }
        s_message_waiting_row_seen = true;
        s_message_waiting_uncertain_mask |=
            event->message_waiting_uncertain_mask;
        return;
    }
    if (result != MODEM_MESSAGE_WAITING_ROW_VALID) {
        s_message_waiting_row_invalid = true;
        return;
    }

    s_message_waiting_row_seen = true;
    if (event->kind != MODEM_AUX_EVENT_MESSAGE_WAITING ||
        (event->message_waiting_category == MODEM_MESSAGE_WAITING_ALL &&
         event->active) ||
        (event->message_waiting_category != MODEM_MESSAGE_WAITING_ALL &&
         event->message_waiting_category >=
             MODEM_MESSAGE_WAITING_CATEGORY_COUNT)) {
        s_message_waiting_row_invalid = true;
        return;
    }
    if (event->message_waiting_category == MODEM_MESSAGE_WAITING_ALL) {
        if (s_message_waiting_clear_all_seen) {
            return;
        }
        for (uint8_t i = 0u; i < MODEM_MESSAGE_WAITING_CATEGORY_COUNT; i++) {
            if (s_message_waiting_category_seen[i]) {
                s_message_waiting_row_invalid = true;
                return;
            }
        }
        s_message_waiting_clear_all_seen = true;
        return;
    }
    if (s_message_waiting_clear_all_seen) {
        s_message_waiting_row_invalid = true;
        return;
    }
    uint8_t category = (uint8_t)event->message_waiting_category;
    if (s_message_waiting_category_seen[category] &&
        (s_message_waiting_category_active[category] != event->active ||
         (event->active &&
          s_message_waiting_category_count[category] != event->count))) {
        s_message_waiting_row_invalid = true;
        return;
    }
    s_message_waiting_category_seen[category] = true;
    s_message_waiting_category_active[category] = event->active;
    s_message_waiting_category_count[category] =
        event->active ? event->count : 0u;
}

bool modem_supplementary_message_waiting_finish(bool ok, uint32_t now_ms) {
    bool result_ok = ok && s_message_waiting_row_seen &&
                     !s_message_waiting_row_invalid;
    bool all_categories_authoritative = true;
    if (result_ok) {
        for (uint8_t i = 0u; i < MODEM_MESSAGE_WAITING_CATEGORY_COUNT; i++) {
            modem_message_waiting_category_mask_t bit =
                modem_message_waiting_category_bit(
                    (modem_message_waiting_category_t)i);
            bool authoritative = s_message_waiting_category_seen[i] ||
                (s_message_waiting_uncertain_mask & bit) == 0u;
            if (!authoritative) {
                all_categories_authoritative = false;
                continue;
            }
            modem_message_waiting_state_t *waiting =
                &s_cache.message_waiting.category[i];
            waiting->active = s_message_waiting_category_seen[i] &&
                              s_message_waiting_category_active[i];
            waiting->count = waiting->active
                ? s_message_waiting_category_count[i]
                : 0u;
        }
        s_cache.message_waiting_known = all_categories_authoritative;
    }
    refresh_finish(MODEM_SUPPLEMENTARY_REFRESH_MESSAGE_WAITING, result_ok,
                   now_ms);
    message_waiting_reset_collect();
    return result_ok;
}

bool modem_supplementary_apply_aux_event(const modem_aux_event_t *event) {
    if (event == NULL) {
        return false;
    }
    if (event->kind == MODEM_AUX_EVENT_MESSAGE_WAITING) {
        if ((event->message_waiting_category == MODEM_MESSAGE_WAITING_ALL &&
             event->active) ||
            (event->message_waiting_category != MODEM_MESSAGE_WAITING_ALL &&
             event->message_waiting_category >=
                 MODEM_MESSAGE_WAITING_CATEGORY_COUNT)) {
            return false;
        }
        if (event->message_waiting_category == MODEM_MESSAGE_WAITING_ALL) {
            memset(&s_cache.message_waiting, 0,
                   sizeof(s_cache.message_waiting));
        } else {
            modem_message_waiting_state_t *waiting =
                &s_cache.message_waiting
                     .category[event->message_waiting_category];
            waiting->active = event->active;
            waiting->count = event->active ? event->count : 0u;
        }
        s_cache.message_waiting_known = true;
        refresh_resolved(MODEM_SUPPLEMENTARY_REFRESH_MESSAGE_WAITING);
        return true;
    }
    if (event->kind == MODEM_AUX_EVENT_CFU_STATE) {
        s_cache.call_forward_unconditional_known = true;
        s_cache.call_forward_unconditional_active = event->active;
        refresh_resolved(MODEM_SUPPLEMENTARY_REFRESH_CFU);
        return true;
    }
    return false;
}

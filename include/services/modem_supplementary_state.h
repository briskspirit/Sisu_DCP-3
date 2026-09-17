#ifndef MODEM_SUPPLEMENTARY_STATE_H
#define MODEM_SUPPLEMENTARY_STATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "services/call_forward_types.h"

#define MODEM_SUPPLEMENTARY_REFRESH_RETRY_MS 1000u
#define MODEM_SUPPLEMENTARY_REFRESH_RETRY_LIMIT 1u

typedef enum {
    MODEM_SUPPLEMENTARY_REFRESH_CFU = 0,
    MODEM_SUPPLEMENTARY_REFRESH_VOICE_MAILBOX,
    MODEM_SUPPLEMENTARY_REFRESH_MESSAGE_WAITING,
    MODEM_SUPPLEMENTARY_REFRESH_COUNT,
} modem_supplementary_refresh_kind_t;

typedef struct {
    bool call_forward_unconditional_known;
    bool call_forward_unconditional_active;
    bool voice_mailbox_known;
    char voice_mailbox_number[MODEM_PHONE_MAX + 1u];
    bool message_waiting_known;
    modem_message_waiting_status_t message_waiting;
} modem_supplementary_cache_t;

/* This is a single-modem state owner. The outer modem service serializes calls
 * that cross its public API with the same status/request locks it used before
 * this extraction; the state machine itself remains host-testable and has no
 * transport, vendor, HAL, or storage dependency. */
void modem_supplementary_init(void);
void modem_supplementary_reset_transient(void);
void modem_supplementary_invalidate_cache(void);
void modem_supplementary_get_cache(modem_supplementary_cache_t *out);

void modem_supplementary_refresh_reset_all(void);
void modem_supplementary_refresh_arm(
    modem_supplementary_refresh_kind_t kind, uint32_t now_ms);
bool modem_supplementary_refresh_due(
    modem_supplementary_refresh_kind_t kind, uint32_t now_ms);
void modem_supplementary_refresh_begin(
    modem_supplementary_refresh_kind_t kind);
bool modem_supplementary_call_forward_flags_finish(bool ok, uint32_t now_ms);

uint32_t modem_supplementary_call_forward_next_request_id(void);
void modem_supplementary_call_forward_publish_result(
    uint32_t request_id, const call_forward_request_t *request,
    call_forward_outcome_t outcome, bool status_known, bool active,
    const char *number, bool has_delay, uint8_t delay_seconds);
bool modem_supplementary_call_forward_pop_result(call_forward_result_t *out);

void modem_supplementary_call_forward_begin(uint8_t step_count);
void modem_supplementary_call_forward_reset(void);
void modem_supplementary_call_forward_collect_row(
    const call_forward_row_t *row);
void modem_supplementary_call_forward_collect_invalid(void);
bool modem_supplementary_call_forward_step_complete(
    bool ok, bool timed_out, bool query, uint8_t *next_step_out);
bool modem_supplementary_call_forward_cancel_uncertain(
    bool command_in_flight, bool query);
bool modem_supplementary_call_forward_finish(
    uint32_t request_id, const call_forward_request_t *request,
    bool ok, bool timed_out, bool network_registered, uint32_t now_ms,
    bool *status_updated_out);

void modem_supplementary_voice_mailbox_begin(void);
void modem_supplementary_voice_mailbox_collect_row(
    const modem_voice_mailbox_row_t *row);
void modem_supplementary_voice_mailbox_collect_invalid(void);
bool modem_supplementary_voice_mailbox_finish(bool ok, uint32_t now_ms);
bool modem_supplementary_voice_mailbox_get(char *out, size_t out_cap);

void modem_supplementary_message_waiting_begin(void);
void modem_supplementary_message_waiting_collect_row(
    modem_message_waiting_row_result_t result,
    const modem_aux_event_t *event);
bool modem_supplementary_message_waiting_finish(bool ok, uint32_t now_ms);

/* Applies only CFU/MWI events. Incoming-diverted remains call-model evidence in
 * the outer service. False means the event was unrelated or malformed. */
bool modem_supplementary_apply_aux_event(const modem_aux_event_t *event);

#endif /* MODEM_SUPPLEMENTARY_STATE_H */

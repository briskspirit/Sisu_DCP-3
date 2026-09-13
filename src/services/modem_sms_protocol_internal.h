#ifndef MODEM_SMS_PROTOCOL_INTERNAL_H
#define MODEM_SMS_PROTOCOL_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "services/sms_types.h"

typedef enum {
    MODEM_SMS_PROTOCOL_NONE = 0,
    MODEM_SMS_PROTOCOL_SEND_TEXT,
    MODEM_SMS_PROTOCOL_SEND_BINARY,
    MODEM_SMS_PROTOCOL_SAVE,
    MODEM_SMS_PROTOCOL_MAILBOX,
    MODEM_SMS_PROTOCOL_READ,
    MODEM_SMS_PROTOCOL_DELETE,
} modem_sms_protocol_operation_t;

typedef enum {
    MODEM_SMS_COMMAND_CPMS = 0,
    MODEM_SMS_COMMAND_STATUS_PRESERVE,
    MODEM_SMS_COMMAND_STATUS_CONSUME,
    MODEM_SMS_COMMAND_CMGL,
    MODEM_SMS_COMMAND_CMGR,
    MODEM_SMS_COMMAND_CMGD,
    MODEM_SMS_COMMAND_CMGF_PDU,
    MODEM_SMS_COMMAND_CMGF_TEXT,
    MODEM_SMS_COMMAND_CMGS_PROMPT,
    MODEM_SMS_COMMAND_CMGS_FINAL,
    MODEM_SMS_COMMAND_CMGW_PROMPT,
    MODEM_SMS_COMMAND_CMGW_FINAL,
} modem_sms_command_kind_t;

typedef struct {
    const char *preserve_unread_cmd;
    const char *consume_unread_cmd;
    uint32_t timeout_ms;
} modem_sms_read_status_policy_t;

/* The root request owns every pointed-to buffer for the whole operation. The
 * protocol borrows this view only for the duration of one synchronous call and
 * never stores it. */
typedef struct {
    uint32_t request_id;
    modem_sms_protocol_operation_t operation;
    const char *number;
    const char *text;
    const uint8_t *binary;
    uint16_t binary_len;
    uint16_t dest_port;
    uint16_t source_port;
    modem_binary_sms_mode_t binary_mode;
    modem_sms_mailbox_t mailbox;
    const uint16_t *indices;
    uint8_t index_count;
    bool quarantined;
    uint32_t expected_identity_hash;
    modem_sms_read_status_policy_t read_status;
} modem_sms_protocol_request_t;

typedef enum {
    MODEM_SMS_ACTION_COMMAND = 0,
    MODEM_SMS_ACTION_BODY,
    MODEM_SMS_ACTION_ABORT_PROMPT,
    MODEM_SMS_ACTION_REQUEST_WAKE_CONTINUATION,
    MODEM_SMS_ACTION_CLEAR_MAILBOX,
    MODEM_SMS_ACTION_SELECTED_BEGIN,
    MODEM_SMS_ACTION_PUBLISH_MAILBOX,
    MODEM_SMS_ACTION_PUBLISH_READ,
    MODEM_SMS_ACTION_PUBLISH_SEND,
    MODEM_SMS_ACTION_PUBLISH_SAVE,
    MODEM_SMS_ACTION_PUBLISH_DELETE,
    MODEM_SMS_ACTION_APPEND_MAILBOX,
    MODEM_SMS_ACTION_ARRIVAL_SCAN_BEGIN,
    MODEM_SMS_ACTION_ARRIVAL_SCAN_COMMIT,
    MODEM_SMS_ACTION_INCREMENT_SENT,
    MODEM_SMS_ACTION_INCREMENT_COMMAND_ERRORS,
    MODEM_SMS_ACTION_COMPLETE,
} modem_sms_action_type_t;

typedef struct {
    modem_sms_action_type_t type;
    union {
        struct {
            modem_sms_command_kind_t kind;
            const char *command;
            uint32_t timeout_ms;
            uint32_t now_ms;
            bool raw;
            bool append_cr;
        } command;
        struct {
            const uint8_t *bytes;
            size_t length;
            modem_sms_command_kind_t final_kind;
            uint32_t timeout_ms;
            uint32_t now_ms;
            bool binary;
            uint8_t segment;
            uint8_t segment_total;
            uint8_t tpdu_len;
        } body;
        struct {
            uint32_t now_ms;
            bool restore_after_settle;
        } abort_prompt;
        struct {
            uint32_t now_ms;
        } wake_continuation;
        struct {
            uint16_t first_index;
            uint8_t index_count;
            bool quarantined;
        } selected_begin;
        struct {
            uint32_t request_id;
            modem_sms_request_kind_t kind;
            modem_sms_outcome_t outcome;
            bool sim_not_ready;
            bool complete;
            modem_sms_mailbox_t mailbox;
        } mailbox_result;
        struct {
            uint32_t request_id;
            modem_sms_request_kind_t kind;
            modem_sms_outcome_t outcome;
            bool sim_not_ready;
            uint32_t expected_identity_hash;
        } read_result;
        struct {
            uint32_t request_id;
            modem_sms_request_kind_t kind;
            modem_sms_outcome_t outcome;
            bool sim_not_ready;
        } result;
        struct {
            const modem_sms_record_t *record;
        } append_mailbox;
        struct {
            bool complete;
            bool inbox;
        } arrival_commit;
        struct {
            uint32_t request_id;
            modem_sms_request_kind_t kind;
            modem_sms_outcome_t outcome;
            bool command_ok;
        } complete;
    } data;
} modem_sms_protocol_action_t;

/* emit() consumes borrowed pointers synchronously. Its return value matters
 * only for APPEND_MAILBOX, where false means the bounded public cache is full.
 * The protocol receives no core context and retains no hook pointer. */
typedef struct {
    bool (*emit)(const modem_sms_protocol_action_t *action);
    bool (*sim_ready)(void);
    bool (*call_preempt_pending)(void);
} modem_sms_protocol_hooks_t;

void modem_sms_protocol_init(void);
void modem_sms_protocol_operation_finished(void);

bool modem_sms_protocol_begin(const modem_sms_protocol_request_t *request,
                              const modem_sms_protocol_hooks_t *hooks,
                              uint32_t now_ms);
bool modem_sms_protocol_parse_line(
    modem_sms_command_kind_t kind, const char *line, bool known_urc,
    bool final_text, const modem_sms_protocol_request_t *request,
    const modem_sms_protocol_hooks_t *hooks);
bool modem_sms_protocol_on_prompt(
    modem_sms_command_kind_t kind,
    const modem_sms_protocol_request_t *request,
    const modem_sms_protocol_hooks_t *hooks, uint32_t now_ms);

/* Root-host evidence hook. Emitting a command is not the same as crossing the
 * UART boundary because DTR/CTS may defer it; mutating cancellation semantics
 * may advance only from this callback. */
void modem_sms_protocol_command_dispatched(modem_sms_command_kind_t kind);
void modem_sms_protocol_on_final(
    modem_sms_command_kind_t kind, bool ok,
    const modem_sms_protocol_request_t *request,
    const modem_sms_protocol_hooks_t *hooks, uint32_t now_ms);
void modem_sms_protocol_on_timeout(
    modem_sms_command_kind_t kind,
    const modem_sms_protocol_request_t *request,
    const modem_sms_protocol_hooks_t *hooks, uint32_t now_ms);
void modem_sms_protocol_resume_after_wake(
    const modem_sms_protocol_request_t *request,
    const modem_sms_protocol_hooks_t *hooks, uint32_t now_ms);
void modem_sms_protocol_resume_after_prompt_abort(
    const modem_sms_protocol_request_t *request,
    const modem_sms_protocol_hooks_t *hooks, uint32_t now_ms);

/* Session cancellation uses protocol evidence rather than treating a body
 * already handed to the modem as safely retryable. */
modem_sms_outcome_t modem_sms_protocol_cancel_outcome(
    const modem_sms_protocol_request_t *request);
void modem_sms_protocol_cancel(void);

void modem_sms_protocol_line_dropped(void);
void modem_sms_protocol_reset_pending_arrivals(void);
bool modem_sms_protocol_track_pending_arrival(uint16_t index);

#endif /* MODEM_SMS_PROTOCOL_INTERNAL_H */

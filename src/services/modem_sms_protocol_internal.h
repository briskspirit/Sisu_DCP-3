#ifndef MODEM_SMS_PROTOCOL_INTERNAL_H
#define MODEM_SMS_PROTOCOL_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "services/sms_types.h"
#include "services/modem_sms_text.h"

typedef enum {
    MODEM_SMS_PROTOCOL_NONE = 0,
    MODEM_SMS_PROTOCOL_SEND_TEXT,
    MODEM_SMS_PROTOCOL_SEND_BINARY,
} modem_sms_protocol_operation_t;

typedef enum {
    MODEM_SMS_COMMAND_CMGF_PDU = 0,
    MODEM_SMS_COMMAND_CMGF_TEXT,
    MODEM_SMS_COMMAND_CMGS_PROMPT,
    MODEM_SMS_COMMAND_CMGS_FINAL,
    MODEM_SMS_COMMAND_PICTURE_TEXT_SETUP,
    MODEM_SMS_COMMAND_TEXT_SETUP,
} modem_sms_command_kind_t;

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
    bool picture_text_mode;
} modem_sms_protocol_request_t;

typedef enum {
    MODEM_SMS_ACTION_COMMAND = 0,
    MODEM_SMS_ACTION_BODY,
    MODEM_SMS_ACTION_ABORT_PROMPT,
    MODEM_SMS_ACTION_PUBLISH_SEND,
    MODEM_SMS_ACTION_INCREMENT_SENT,
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
            uint32_t request_id;
            modem_sms_request_kind_t kind;
            modem_sms_outcome_t outcome;
        } result;
        struct {
            uint32_t request_id;
            modem_sms_request_kind_t kind;
            modem_sms_outcome_t outcome;
            bool command_ok;
        } complete;
    } data;
} modem_sms_protocol_action_t;

/* Borrowed command/body pointers are consumed synchronously. */
typedef struct {
    bool (*emit)(const modem_sms_protocol_action_t *action);
    bool (*encode_text)(const char *text, modem_sms_text_t *out);
} modem_sms_protocol_hooks_t;

void modem_sms_protocol_init(void);

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
/* True when AT+CMGF=0 crossed the UART and no AT+CMGF=1 has completed since,
 * so cancelling now may leave the module in PDU mode. Read it BEFORE
 * modem_sms_protocol_cancel(), which resets the protocol state. */
bool modem_sms_protocol_pdu_mode_possible(void);
bool modem_sms_protocol_settings_restore_needed(void);
bool modem_sms_protocol_text_parameters_dirty(void);
void modem_sms_protocol_cancel(void);

#endif /* MODEM_SMS_PROTOCOL_INTERNAL_H */

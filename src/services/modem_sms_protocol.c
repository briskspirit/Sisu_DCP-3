#include "modem_sms_protocol_internal.h"

#include <stdio.h>
#include <string.h>
#include "modem_sms_state_internal.h"
#include "services/log.h"
#include "services/modem_at_util.h"
#include "services/sms_picture_codec.h"
#include "services/sms_submit_codec.h"

#define MODEM_SMS_BINARY_RESULT_TIMEOUT_MS 120000u

typedef struct {
    bool payload_submitted, send_confirmed;
    uint8_t binary_segments_accepted;
    modem_sms_outcome_t binary_outcome;
    bool pdu_mode_possible, picture_text_mode, text_parameters_dirty;
} modem_sms_protocol_state_t;
static modem_sms_protocol_state_t s_protocol;

static modem_sms_request_kind_t request_kind(const modem_sms_protocol_request_t *r) {
    if (r == NULL) return MODEM_SMS_REQUEST_NONE;
    return r->operation == MODEM_SMS_PROTOCOL_SEND_TEXT ? MODEM_SMS_REQUEST_SEND_TEXT :
        r->operation == MODEM_SMS_PROTOCOL_SEND_BINARY ? MODEM_SMS_REQUEST_SEND_BINARY : MODEM_SMS_REQUEST_NONE;
}
static bool request_valid(const modem_sms_protocol_request_t *r) {
    return r != NULL && r->request_id != 0u && r->number != NULL && r->number[0] != '\0' &&
        ((r->operation == MODEM_SMS_PROTOCOL_SEND_TEXT && r->text != NULL) ||
         (r->operation == MODEM_SMS_PROTOCOL_SEND_BINARY && r->binary != NULL && r->binary_len != 0u));
}

static bool emit(const modem_sms_protocol_hooks_t *hooks,
                 const modem_sms_protocol_action_t *action) {
    return hooks != NULL && hooks->emit != NULL && action != NULL &&
           hooks->emit(action);
}

static bool emit_command(const modem_sms_protocol_hooks_t *hooks,
                         modem_sms_command_kind_t kind, const char *command,
                         uint32_t timeout_ms, uint32_t now_ms, bool raw,
                         bool append_cr) {
    modem_sms_protocol_action_t action = {
        .type = MODEM_SMS_ACTION_COMMAND,
        .data.command = {
            .kind = kind,
            .command = command,
            .timeout_ms = timeout_ms,
            .now_ms = now_ms,
            .raw = raw,
            .append_cr = append_cr,
        },
    };
    return emit(hooks, &action);
}

static void emit_complete(const modem_sms_protocol_request_t *request,
                          const modem_sms_protocol_hooks_t *hooks,
                          modem_sms_outcome_t outcome, bool command_ok) {
    modem_sms_protocol_action_t action = {
        .type = MODEM_SMS_ACTION_COMPLETE,
        .data.complete = {
            .request_id = request != NULL ? request->request_id : 0u,
            .kind = request_kind(request),
            .outcome = outcome,
            .command_ok = command_ok,
        },
    };
    (void)emit(hooks, &action);
}

static void emit_send_result(const modem_sms_protocol_request_t *request,
                             const modem_sms_protocol_hooks_t *hooks,
                             modem_sms_outcome_t outcome) {
    modem_sms_protocol_action_t action = {
        .type = MODEM_SMS_ACTION_PUBLISH_SEND,
        .data.result = {
            .request_id = request != NULL ? request->request_id : 0u,
            .kind = request_kind(request),
            .outcome = outcome,
        },
    };
    (void)emit(hooks, &action);
}

static void emit_status_increment(const modem_sms_protocol_hooks_t *hooks,
                                  modem_sms_action_type_t type) {
    modem_sms_protocol_action_t action = { .type = type };
    (void)emit(hooks, &action);
}

static void binary_start_segment(
    const modem_sms_protocol_request_t *request,
    const modem_sms_protocol_hooks_t *hooks, uint32_t now_ms);

static void binary_restore_text(const modem_sms_protocol_hooks_t *hooks,
                                uint32_t now_ms,
                                modem_sms_outcome_t outcome) {
    s_protocol.binary_outcome = outcome;
    modem_sms_state_binary_set_send_ok(outcome == MODEM_SMS_OUTCOME_OK);
    (void)emit_command(hooks, MODEM_SMS_COMMAND_CMGF_TEXT,
                       s_protocol.picture_text_mode
                           ? "AT+CMGF=1;+CSMP=17,167,0,0" : "AT+CMGF=1",
                       5000u, now_ms, false, false);
}

static void binary_start(const modem_sms_protocol_request_t *request,
                         const modem_sms_protocol_hooks_t *hooks,
                         uint32_t now_ms) {
    uint8_t segment_total = modem_sms_state_binary_begin(request->binary_len);
    s_protocol.payload_submitted = false;
    s_protocol.send_confirmed = false;
    s_protocol.binary_segments_accepted = 0u;
    s_protocol.binary_outcome = MODEM_SMS_OUTCOME_NONE;
    s_protocol.picture_text_mode = request->picture_text_mode &&
        request->binary_mode == MODEM_BINARY_SMS_MODE_DCS04_PORT_FIRST &&
        request->dest_port == SMS_CODEC_PICTURE_PORT;
    LOGI("modem", "binary SMS start bytes=%u segments=%u dest_port=0x%04x src_port=0x%04x mode=%u",
         (unsigned)request->binary_len, (unsigned)segment_total,
         (unsigned)request->dest_port, (unsigned)request->source_port,
         (unsigned)request->binary_mode);
    (void)emit_command(hooks,
                       s_protocol.picture_text_mode
                           ? MODEM_SMS_COMMAND_PICTURE_TEXT_SETUP
                           : MODEM_SMS_COMMAND_CMGF_PDU,
                       s_protocol.picture_text_mode
                           ? "AT+CMGF=1;+CSMP=81,167,0,4" : "AT+CMGF=0",
                       5000u, now_ms, false, false);
}

static void binary_start_segment(
    const modem_sms_protocol_request_t *request,
    const modem_sms_protocol_hooks_t *hooks, uint32_t now_ms) {
    bool built = s_protocol.picture_text_mode
        ? modem_sms_state_picture_build_text_segment(
            request->number, request->binary, request->binary_len,
            request->dest_port, request->source_port)
        : modem_sms_state_binary_build_segment(
            request->number, request->binary, request->binary_len,
            request->dest_port, request->source_port,
            request->binary_mode);
    if (!built) {
        LOGW("modem", "binary SMS PDU build failed");
        binary_restore_text(
            hooks, now_ms,
            s_protocol.binary_segments_accepted != 0u
                ? MODEM_SMS_OUTCOME_UNCERTAIN
                : MODEM_SMS_OUTCOME_ERROR);
        return;
    }
    modem_sms_binary_segment_view_t segment;
    modem_sms_state_binary_segment_view(&segment);
    char command[MODEM_SMS_SENDER_MAX + 48u];
    if (s_protocol.picture_text_mode) {
        sms_submit_format_text_command(command, sizeof(command), "AT+CMGS",
                                        request->number, false);
    } else {
        snprintf(command, sizeof(command), "AT+CMGS=%u",
                 (unsigned)segment.tpdu_len);
    }
    (void)emit_command(hooks, MODEM_SMS_COMMAND_CMGS_PROMPT, command,
                       5000u, now_ms, false, false);
}


void modem_sms_protocol_init(void) { memset(&s_protocol, 0, sizeof(s_protocol)); }

bool modem_sms_protocol_begin(const modem_sms_protocol_request_t *request,
                              const modem_sms_protocol_hooks_t *hooks, uint32_t now_ms) {
    if (!request_valid(request) || hooks == NULL || hooks->emit == NULL) return false;
    memset(&s_protocol, 0, sizeof(s_protocol));
    if (request->operation == MODEM_SMS_PROTOCOL_SEND_BINARY) binary_start(request, hooks, now_ms);
    else modem_sms_protocol_resume_after_wake(request, hooks, now_ms);
    return true;
}

void modem_sms_protocol_resume_after_wake(const modem_sms_protocol_request_t *request,
    const modem_sms_protocol_hooks_t *hooks, uint32_t now_ms) {
    if (!request_valid(request) || request->operation != MODEM_SMS_PROTOCOL_SEND_TEXT) {
        emit_complete(request, hooks, MODEM_SMS_OUTCOME_ERROR, false);
        return;
    }
    /* No storage selection is needed for CMGS. The root transport still owns
     * DTR/CTS wake and defers this command until the UART is ready. */
    char command[MODEM_SMS_SENDER_MAX + 40u];
    sms_submit_format_text_command(command, sizeof(command), "AT+CMGS", request->number, false);
    (void)emit_command(hooks, MODEM_SMS_COMMAND_CMGS_PROMPT, command, 5000u, now_ms, false, false);
}

void modem_sms_protocol_resume_after_prompt_abort(const modem_sms_protocol_request_t *request,
    const modem_sms_protocol_hooks_t *hooks, uint32_t now_ms) {
    if (request != NULL && request->operation == MODEM_SMS_PROTOCOL_SEND_BINARY) {
        modem_sms_outcome_t outcome = s_protocol.binary_outcome;
        if (outcome == MODEM_SMS_OUTCOME_NONE) outcome = MODEM_SMS_OUTCOME_TIMEOUT;
        binary_restore_text(hooks, now_ms, outcome);
    } else emit_complete(request, hooks, MODEM_SMS_OUTCOME_ERROR, false);
}

bool modem_sms_protocol_on_prompt(modem_sms_command_kind_t kind,
    const modem_sms_protocol_request_t *request, const modem_sms_protocol_hooks_t *hooks, uint32_t now_ms) {
    if (!request_valid(request) || kind != MODEM_SMS_COMMAND_CMGS_PROMPT) return false;
    modem_sms_protocol_action_t action = {
        .type=MODEM_SMS_ACTION_BODY, .data.body={
            .final_kind=MODEM_SMS_COMMAND_CMGS_FINAL,
            .timeout_ms=request->operation == MODEM_SMS_PROTOCOL_SEND_BINARY
                ? MODEM_SMS_BINARY_RESULT_TIMEOUT_MS : 30000u,
            .now_ms=now_ms, .binary=request->operation == MODEM_SMS_PROTOCOL_SEND_BINARY,
        },
    };
    if (action.data.body.binary) {
        modem_sms_binary_segment_view_t segment;
        modem_sms_state_binary_segment_view(&segment);
        action.data.body.bytes = (const uint8_t *)segment.pdu_hex;
        action.data.body.length = strlen(segment.pdu_hex);
        action.data.body.segment = segment.segment;
        action.data.body.segment_total = segment.segment_total;
        action.data.body.tpdu_len = segment.tpdu_len;
    } else {
        action.data.body.bytes = (const uint8_t *)request->text;
        action.data.body.length = strlen(request->text);
    }
    bool emitted = emit(hooks, &action);
    if (emitted) s_protocol.payload_submitted = true;
    return emitted;
}

void modem_sms_protocol_command_dispatched(modem_sms_command_kind_t kind) {
    if (kind == MODEM_SMS_COMMAND_CMGF_PDU) s_protocol.pdu_mode_possible = true;
    if (kind == MODEM_SMS_COMMAND_PICTURE_TEXT_SETUP) s_protocol.text_parameters_dirty = true;
}
bool modem_sms_protocol_pdu_mode_possible(void) { return s_protocol.pdu_mode_possible; }
bool modem_sms_protocol_settings_restore_needed(void) {
    return s_protocol.pdu_mode_possible || s_protocol.text_parameters_dirty;
}
bool modem_sms_protocol_text_parameters_dirty(void) { return s_protocol.text_parameters_dirty; }

bool modem_sms_protocol_parse_line(modem_sms_command_kind_t kind, const char *line,
    bool known_urc, bool final_text, const modem_sms_protocol_request_t *request,
    const modem_sms_protocol_hooks_t *hooks) {
    (void)known_urc; (void)final_text; (void)request; (void)hooks;
    return kind == MODEM_SMS_COMMAND_CMGS_FINAL && line != NULL && modem_at_starts_with(line, "+CMGS:");
}

static void binary_finish(const modem_sms_protocol_request_t *request,
    const modem_sms_protocol_hooks_t *hooks, bool restored) {
    modem_sms_outcome_t outcome = s_protocol.binary_outcome;
    if (outcome == MODEM_SMS_OUTCOME_NONE) outcome = MODEM_SMS_OUTCOME_ERROR;
    if (outcome == MODEM_SMS_OUTCOME_OK) emit_status_increment(hooks, MODEM_SMS_ACTION_INCREMENT_SENT);
    emit_send_result(request, hooks, outcome);
    emit_complete(request, hooks, outcome, outcome == MODEM_SMS_OUTCOME_OK && restored);
}

void modem_sms_protocol_on_final(modem_sms_command_kind_t kind, bool ok,
    const modem_sms_protocol_request_t *request, const modem_sms_protocol_hooks_t *hooks, uint32_t now_ms) {
    if (!request_valid(request)) { emit_complete(request, hooks, MODEM_SMS_OUTCOME_ERROR, false); return; }
    if (kind == MODEM_SMS_COMMAND_CMGF_PDU && !ok) s_protocol.pdu_mode_possible = false;
    if (kind == MODEM_SMS_COMMAND_CMGF_TEXT && ok) {
        s_protocol.pdu_mode_possible = false;
        s_protocol.text_parameters_dirty = false;
    }
    switch (kind) {
    case MODEM_SMS_COMMAND_PICTURE_TEXT_SETUP:
        if (ok) binary_start_segment(request, hooks, now_ms);
        else binary_restore_text(hooks, now_ms, MODEM_SMS_OUTCOME_ERROR);
        return;
    case MODEM_SMS_COMMAND_CMGF_PDU:
        if (ok) binary_start_segment(request, hooks, now_ms);
        else {
            emit_send_result(request, hooks, MODEM_SMS_OUTCOME_ERROR);
            emit_complete(request, hooks, MODEM_SMS_OUTCOME_ERROR, false);
        }
        return;
    case MODEM_SMS_COMMAND_CMGF_TEXT:
        binary_finish(request, hooks, ok);
        return;
    case MODEM_SMS_COMMAND_CMGS_PROMPT:
        if (request->operation == MODEM_SMS_PROTOCOL_SEND_BINARY)
            binary_restore_text(hooks, now_ms, s_protocol.binary_segments_accepted != 0u
                ? MODEM_SMS_OUTCOME_UNCERTAIN : MODEM_SMS_OUTCOME_ERROR);
        else {
            emit_send_result(request, hooks, MODEM_SMS_OUTCOME_ERROR);
            emit_complete(request, hooks, MODEM_SMS_OUTCOME_ERROR, false);
        }
        return;
    case MODEM_SMS_COMMAND_CMGS_FINAL:
        s_protocol.payload_submitted = false;
        if (request->operation == MODEM_SMS_PROTOCOL_SEND_BINARY) {
            if (!ok) binary_restore_text(hooks, now_ms, s_protocol.binary_segments_accepted != 0u
                ? MODEM_SMS_OUTCOME_UNCERTAIN : MODEM_SMS_OUTCOME_ERROR);
            else {
                s_protocol.binary_segments_accepted++;
                if (modem_sms_state_binary_has_more(request->binary_len))
                    binary_start_segment(request, hooks, now_ms);
                else {
                    s_protocol.send_confirmed = true;
                    binary_restore_text(hooks, now_ms, MODEM_SMS_OUTCOME_OK);
                }
            }
        } else {
            s_protocol.send_confirmed = ok;
            if (ok) emit_status_increment(hooks, MODEM_SMS_ACTION_INCREMENT_SENT);
            emit_send_result(request, hooks, ok ? MODEM_SMS_OUTCOME_OK : MODEM_SMS_OUTCOME_ERROR);
            emit_complete(request, hooks, ok ? MODEM_SMS_OUTCOME_OK : MODEM_SMS_OUTCOME_ERROR, ok);
        }
        return;
    }
}

void modem_sms_protocol_on_timeout(modem_sms_command_kind_t kind,
    const modem_sms_protocol_request_t *request, const modem_sms_protocol_hooks_t *hooks, uint32_t now_ms) {
    if (!request_valid(request)) { emit_complete(request, hooks, MODEM_SMS_OUTCOME_ERROR, false); return; }
    bool binary = request->operation == MODEM_SMS_PROTOCOL_SEND_BINARY;
    if (kind == MODEM_SMS_COMMAND_CMGS_PROMPT) {
        /* Drain ESC's final before issuing any mode-restoration command. */
        modem_sms_protocol_action_t action = {
            .type=MODEM_SMS_ACTION_ABORT_PROMPT,
            .data.abort_prompt={.now_ms=now_ms, .restore_after_settle=binary},
        };
        (void)emit(hooks, &action);
        if (binary) {
            s_protocol.binary_outcome = s_protocol.binary_segments_accepted != 0u
                ? MODEM_SMS_OUTCOME_UNCERTAIN : MODEM_SMS_OUTCOME_TIMEOUT;
            return;
        }
    }
    if (binary && kind == MODEM_SMS_COMMAND_CMGF_TEXT) {
        binary_finish(request, hooks, false);
        return;
    }
    modem_sms_outcome_t outcome = kind == MODEM_SMS_COMMAND_CMGS_FINAL
        ? MODEM_SMS_OUTCOME_UNCERTAIN : MODEM_SMS_OUTCOME_TIMEOUT;
    if (binary) binary_restore_text(hooks, now_ms, outcome);
    else {
        emit_send_result(request, hooks, outcome);
        emit_complete(request, hooks, outcome, false);
    }
}

modem_sms_outcome_t modem_sms_protocol_cancel_outcome(const modem_sms_protocol_request_t *request) {
    if (!request_valid(request)) return MODEM_SMS_OUTCOME_CANCELLED;
    if (s_protocol.send_confirmed || s_protocol.binary_outcome == MODEM_SMS_OUTCOME_OK)
        return MODEM_SMS_OUTCOME_OK;
    return s_protocol.payload_submitted || s_protocol.binary_segments_accepted != 0u
        ? MODEM_SMS_OUTCOME_UNCERTAIN : MODEM_SMS_OUTCOME_CANCELLED;
}
void modem_sms_protocol_cancel(void) {
    modem_sms_state_binary_set_send_ok(false);
    memset(&s_protocol, 0, sizeof(s_protocol));
}

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
    modem_sms_outcome_t send_outcome;
    bool centre_seen, centre_invalid, sim_changed;
} modem_sms_protocol_state_t;
static modem_sms_protocol_state_t s_protocol;
static modem_sms_text_t s_text;
/* A cancelled operation must not erase the original SMSC before idle repair. */
static struct {
    bool dirty, centre_saved;
    uint8_t centre_type;
    char centre[MODEM_SMS_SENDER_MAX + 1u];
} s_restore;
static char s_parameter_command[128];

static modem_sms_request_kind_t request_kind(const modem_sms_protocol_request_t *r) {
    if (r == NULL) return MODEM_SMS_REQUEST_NONE;
    return r->operation == MODEM_SMS_PROTOCOL_SEND_TEXT ? MODEM_SMS_REQUEST_SEND_TEXT :
        r->operation == MODEM_SMS_PROTOCOL_SEND_BINARY ? MODEM_SMS_REQUEST_SEND_BINARY : MODEM_SMS_REQUEST_NONE;
}
static bool request_valid(const modem_sms_protocol_request_t *r) {
    return r != NULL && r->request_id != 0u && r->number != NULL && r->number[0] != '\0' &&
        (r->options == NULL || sms_send_options_valid(r->options)) &&
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

static void restore_text_parameters(const modem_sms_protocol_hooks_t *hooks,
                                uint32_t now_ms,
                                modem_sms_outcome_t outcome) {
    s_protocol.send_outcome = outcome;
    modem_sms_state_binary_set_send_ok(outcome == MODEM_SMS_OUTCOME_OK);
    (void)emit_command(hooks, MODEM_SMS_COMMAND_CMGF_TEXT,
                       modem_sms_protocol_restore_command(),
                       5000u, now_ms, false, false);
}

const char *modem_sms_protocol_restore_command(void) {
    if (s_restore.centre_saved) {
        snprintf(s_parameter_command, sizeof(s_parameter_command),
                 "AT+CMGF=1;+CSMP=17,167,0,0;+CSCA=\"%s\",%u",
                 s_restore.centre, (unsigned)s_restore.centre_type);
    } else {
        strcpy(s_parameter_command, "AT+CMGF=1;+CSMP=17,167,0,0");
    }
    return s_parameter_command;
}

void modem_sms_protocol_restore_confirmed(void) {
    memset(&s_restore, 0, sizeof(s_restore));
}

void modem_sms_protocol_sim_changed(void) {
    s_protocol.sim_changed = true;
    /* The old SIM's address is never written into the replacement session. */
    s_restore.centre_saved = false;
}

static void text_setup(const modem_sms_protocol_request_t *request,
                       const modem_sms_protocol_hooks_t *hooks, uint32_t now_ms) {
    /* TS 23.040 sections 9.2.3.9 and 9.2.3.12: group-3 fax, national
     * paging and Internet mail; relative validity's maximum is 63 weeks. */
    static const uint8_t pid[] = {0u, 0x22u, 0x26u, 0x32u};
    static const uint8_t validity[] = {11u, 71u, 167u, 169u, 173u, 255u};
    const sms_send_options_t *options = request->options;
    unsigned fo = options != NULL && options->reply_same_centre ? 145u : 17u;
    if (options != NULL && options->delivery_reports) fo |= 0x20u;
    unsigned vp = options != NULL ? validity[options->validity] : 167u;
    unsigned protocol = options != NULL ? pid[options->format] : 0u;
    int n = snprintf(s_parameter_command, sizeof(s_parameter_command),
                     "AT+CMGF=1;+CSMP=%u,%u,%u,%u", fo, vp, protocol, (unsigned)s_text.dcs);
    if (s_restore.centre_saved) {
        snprintf(s_parameter_command + n, sizeof(s_parameter_command) - (size_t)n,
                 ";+CSCA=\"%s\",%u", options->service_centre,
                 options->service_centre[0] == '+' ? 145u : 129u);
    }
    (void)emit_command(hooks, MODEM_SMS_COMMAND_TEXT_SETUP, s_parameter_command,
                       5000u, now_ms, false, false);
}

static void binary_start(const modem_sms_protocol_request_t *request,
                         const modem_sms_protocol_hooks_t *hooks,
                         uint32_t now_ms) {
    uint8_t segment_total = modem_sms_state_binary_begin(request->binary_len);
    s_protocol.payload_submitted = false;
    s_protocol.send_confirmed = false;
    s_protocol.binary_segments_accepted = 0u;
    s_protocol.send_outcome = MODEM_SMS_OUTCOME_NONE;
    LOGI("modem", "binary SMS start bytes=%u segments=%u dest_port=0x%04x src_port=0x%04x mode=%u",
         (unsigned)request->binary_len, (unsigned)segment_total,
         (unsigned)request->dest_port, (unsigned)request->source_port,
         (unsigned)request->binary_mode);
    bool gsm7 = request->binary_mode == MODEM_BINARY_SMS_MODE_GSM7_TEXT;
    bool udh = !gsm7 && (request->dest_port != 0u || request->source_port != 0u || segment_total > 1u);
    unsigned dcs = gsm7 ? 0u :
        request->binary_mode == MODEM_BINARY_SMS_MODE_F5_PORT_FIRST ||
        request->binary_mode == MODEM_BINARY_SMS_MODE_F5_CONCAT_FIRST_VP ? 245u : 4u;
    char command[48];
    snprintf(command, sizeof(command), "AT+CMGF=1;+CSMP=%u,167,0,%u", udh ? 81u : 17u, dcs);
    (void)emit_command(hooks, MODEM_SMS_COMMAND_BINARY_TEXT_SETUP, command, 5000u, now_ms, false, false);
}

static void binary_start_segment(
    const modem_sms_protocol_request_t *request,
    const modem_sms_protocol_hooks_t *hooks, uint32_t now_ms) {
    bool built = modem_sms_state_binary_build_segment(
            request->number, request->binary, request->binary_len,
            request->dest_port, request->source_port,
            request->binary_mode);
    if (!built) {
        LOGW("modem", "binary SMS user-data build failed");
        restore_text_parameters(
            hooks, now_ms,
            s_protocol.binary_segments_accepted != 0u
                ? MODEM_SMS_OUTCOME_UNCERTAIN
                : MODEM_SMS_OUTCOME_ERROR);
        return;
    }
    char command[MODEM_SMS_SENDER_MAX + 48u];
    sms_submit_format_text_command(command, sizeof(command), "AT+CMGS",
                                        request->number, false);
    (void)emit_command(hooks, MODEM_SMS_COMMAND_CMGS_PROMPT, command,
                       5000u, now_ms, false, false);
}


void modem_sms_protocol_init(void) {
    memset(&s_protocol, 0, sizeof(s_protocol));
    memset(&s_text, 0, sizeof(s_text));
    modem_sms_protocol_restore_confirmed();
}

bool modem_sms_protocol_begin(const modem_sms_protocol_request_t *request,
                              const modem_sms_protocol_hooks_t *hooks, uint32_t now_ms) {
    if (!request_valid(request) || hooks == NULL || hooks->emit == NULL || s_restore.dirty) return false;
    memset(&s_protocol, 0, sizeof(s_protocol));
    memset(&s_text, 0, sizeof(s_text));
    memset(&s_restore, 0, sizeof(s_restore));
    if (request->operation == MODEM_SMS_PROTOCOL_SEND_TEXT &&
        (hooks->encode_text == NULL || !hooks->encode_text(request->text, &s_text))) return false;
    if (request->operation == MODEM_SMS_PROTOCOL_SEND_BINARY) binary_start(request, hooks, now_ms);
    else if (request->options != NULL && request->options->service_centre[0] != '\0') {
        if (hooks->parse_service_centre == NULL) return false;
        (void)emit_command(hooks, MODEM_SMS_COMMAND_CENTRE_READ, "AT+CSCA?",
                           5000u, now_ms, false, false);
    } else if (s_text.dcs != 0u || request->options != NULL) text_setup(request, hooks, now_ms);
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
    if (request != NULL && (request->operation == MODEM_SMS_PROTOCOL_SEND_BINARY ||
                           s_restore.dirty)) {
        modem_sms_outcome_t outcome = s_protocol.send_outcome;
        if (outcome == MODEM_SMS_OUTCOME_NONE) outcome = MODEM_SMS_OUTCOME_TIMEOUT;
        restore_text_parameters(hooks, now_ms, outcome);
    } else emit_complete(request, hooks, MODEM_SMS_OUTCOME_ERROR, false);
}

bool modem_sms_protocol_on_prompt(modem_sms_command_kind_t kind,
    const modem_sms_protocol_request_t *request, const modem_sms_protocol_hooks_t *hooks, uint32_t now_ms) {
    if (!request_valid(request) || kind != MODEM_SMS_COMMAND_CMGS_PROMPT) return false;
    if (s_protocol.sim_changed) {
        modem_sms_protocol_action_t abort = {.type=MODEM_SMS_ACTION_ABORT_PROMPT,
            .data.abort_prompt={.now_ms=now_ms, .restore_after_settle=s_restore.dirty}};
        (void)emit(hooks, &abort);
        s_protocol.send_outcome = modem_sms_protocol_cancel_outcome(request);
        if (!s_restore.dirty) {
            emit_send_result(request, hooks, s_protocol.send_outcome);
            emit_complete(request, hooks, s_protocol.send_outcome, false);
        }
        return true;
    }
    modem_sms_protocol_action_t action = {
        .type=MODEM_SMS_ACTION_BODY, .data.body={
            .final_kind=MODEM_SMS_COMMAND_CMGS_FINAL,
            .timeout_ms=request->operation == MODEM_SMS_PROTOCOL_SEND_BINARY || s_text.multipart
                ? MODEM_SMS_BINARY_RESULT_TIMEOUT_MS : 30000u,
            .now_ms=now_ms, .binary=request->operation == MODEM_SMS_PROTOCOL_SEND_BINARY,
        },
    };
    if (action.data.body.binary) {
        modem_sms_binary_segment_view_t segment;
        modem_sms_state_binary_segment_view(&segment);
        action.data.body.bytes = (const uint8_t *)segment.body;
        action.data.body.length = strlen(segment.body);
        action.data.body.segment = segment.segment;
        action.data.body.segment_total = segment.segment_total;
    } else {
        action.data.body.bytes = s_text.body;
        action.data.body.length = s_text.length;
    }
    bool emitted = emit(hooks, &action);
    if (emitted) s_protocol.payload_submitted = true;
    return emitted;
}

void modem_sms_protocol_command_dispatched(modem_sms_command_kind_t kind) {
    if (kind == MODEM_SMS_COMMAND_BINARY_TEXT_SETUP || kind == MODEM_SMS_COMMAND_TEXT_SETUP)
        s_restore.dirty = true;
}
bool modem_sms_protocol_settings_restore_needed(void) {
    return s_restore.dirty;
}
bool modem_sms_protocol_text_parameters_dirty(void) { return s_restore.dirty; }

bool modem_sms_protocol_parse_line(modem_sms_command_kind_t kind, const char *line,
    bool known_urc, bool final_text, const modem_sms_protocol_request_t *request,
    const modem_sms_protocol_hooks_t *hooks) {
    (void)known_urc; (void)final_text; (void)request;
    if (kind == MODEM_SMS_COMMAND_CENTRE_READ && line != NULL &&
        modem_at_starts_with(line, "+CSCA:")) {
        if (s_protocol.centre_seen || hooks == NULL || hooks->parse_service_centre == NULL ||
            !hooks->parse_service_centre(line, s_restore.centre, sizeof(s_restore.centre),
                                         &s_restore.centre_type)) s_protocol.centre_invalid = true;
        s_protocol.centre_seen = true;
        return true;
    }
    return kind == MODEM_SMS_COMMAND_CMGS_FINAL && line != NULL && modem_at_starts_with(line, "+CMGS:");
}

static void finish_after_restore(const modem_sms_protocol_request_t *request,
    const modem_sms_protocol_hooks_t *hooks, bool restored) {
    modem_sms_outcome_t outcome = s_protocol.send_outcome;
    if (outcome == MODEM_SMS_OUTCOME_NONE) outcome = MODEM_SMS_OUTCOME_ERROR;
    if (outcome == MODEM_SMS_OUTCOME_OK) emit_status_increment(hooks, MODEM_SMS_ACTION_INCREMENT_SENT);
    emit_send_result(request, hooks, outcome);
    emit_complete(request, hooks, outcome, outcome == MODEM_SMS_OUTCOME_OK && restored);
}

void modem_sms_protocol_on_final(modem_sms_command_kind_t kind, bool ok,
    const modem_sms_protocol_request_t *request, const modem_sms_protocol_hooks_t *hooks, uint32_t now_ms) {
    if (!request_valid(request)) { emit_complete(request, hooks, MODEM_SMS_OUTCOME_ERROR, false); return; }
    if (s_protocol.sim_changed && kind != MODEM_SMS_COMMAND_CMGF_TEXT &&
        kind != MODEM_SMS_COMMAND_CMGS_FINAL) {
        modem_sms_outcome_t outcome = modem_sms_protocol_cancel_outcome(request);
        if (s_restore.dirty) restore_text_parameters(hooks, now_ms, outcome);
        else {
            emit_send_result(request, hooks, outcome);
            emit_complete(request, hooks, outcome, false);
        }
        return;
    }
    if (kind == MODEM_SMS_COMMAND_CMGF_TEXT && ok) {
        modem_sms_protocol_restore_confirmed();
    }
    switch (kind) {
    case MODEM_SMS_COMMAND_CENTRE_READ:
        if (ok && s_protocol.centre_seen && !s_protocol.centre_invalid) {
            s_restore.centre_saved = true;
            text_setup(request, hooks, now_ms);
        } else {
            emit_send_result(request, hooks, MODEM_SMS_OUTCOME_ERROR);
            emit_complete(request, hooks, MODEM_SMS_OUTCOME_ERROR, false);
        }
        return;
    case MODEM_SMS_COMMAND_TEXT_SETUP:
        if (ok) modem_sms_protocol_resume_after_wake(request, hooks, now_ms);
        else restore_text_parameters(hooks, now_ms, MODEM_SMS_OUTCOME_ERROR);
        return;
    case MODEM_SMS_COMMAND_BINARY_TEXT_SETUP:
        if (ok) binary_start_segment(request, hooks, now_ms);
        else restore_text_parameters(hooks, now_ms, MODEM_SMS_OUTCOME_ERROR);
        return;
    case MODEM_SMS_COMMAND_CMGF_TEXT:
        finish_after_restore(request, hooks, ok);
        return;
    case MODEM_SMS_COMMAND_CMGS_PROMPT:
        if (request->operation == MODEM_SMS_PROTOCOL_SEND_BINARY || s_restore.dirty)
            restore_text_parameters(hooks, now_ms, s_protocol.binary_segments_accepted != 0u
                ? MODEM_SMS_OUTCOME_UNCERTAIN : MODEM_SMS_OUTCOME_ERROR);
        else {
            emit_send_result(request, hooks, MODEM_SMS_OUTCOME_ERROR);
            emit_complete(request, hooks, MODEM_SMS_OUTCOME_ERROR, false);
        }
        return;
    case MODEM_SMS_COMMAND_CMGS_FINAL:
        s_protocol.payload_submitted = false;
        if (request->operation == MODEM_SMS_PROTOCOL_SEND_BINARY) {
            if (!ok) restore_text_parameters(hooks, now_ms, s_protocol.binary_segments_accepted != 0u
                ? MODEM_SMS_OUTCOME_UNCERTAIN : MODEM_SMS_OUTCOME_ERROR);
            else {
                s_protocol.binary_segments_accepted++;
                if (modem_sms_state_binary_has_more(request->binary_len)) {
                    if (s_protocol.sim_changed)
                        restore_text_parameters(hooks, now_ms, MODEM_SMS_OUTCOME_UNCERTAIN);
                    else binary_start_segment(request, hooks, now_ms);
                }
                else {
                    s_protocol.send_confirmed = true;
                    restore_text_parameters(hooks, now_ms, MODEM_SMS_OUTCOME_OK);
                }
            }
        } else {
            s_protocol.send_confirmed = ok;
            if (s_restore.dirty) {
                /* A multi-part CMGS may fail after accepting earlier parts. */
                restore_text_parameters(hooks, now_ms, ok ? MODEM_SMS_OUTCOME_OK :
                    s_text.multipart ? MODEM_SMS_OUTCOME_UNCERTAIN : MODEM_SMS_OUTCOME_ERROR);
                return;
            }
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
    bool restore = request->operation == MODEM_SMS_PROTOCOL_SEND_BINARY || s_restore.dirty;
    if (kind == MODEM_SMS_COMMAND_CMGS_PROMPT) {
        /* Drain ESC's final before issuing any mode-restoration command. */
        modem_sms_protocol_action_t action = {
            .type=MODEM_SMS_ACTION_ABORT_PROMPT,
            .data.abort_prompt={.now_ms=now_ms, .restore_after_settle=restore},
        };
        (void)emit(hooks, &action);
        if (restore) {
            s_protocol.send_outcome = s_protocol.binary_segments_accepted != 0u
                ? MODEM_SMS_OUTCOME_UNCERTAIN : MODEM_SMS_OUTCOME_TIMEOUT;
            return;
        }
    }
    if (restore && kind == MODEM_SMS_COMMAND_CMGF_TEXT) {
        finish_after_restore(request, hooks, false);
        return;
    }
    modem_sms_outcome_t outcome = kind == MODEM_SMS_COMMAND_CMGS_FINAL
        ? MODEM_SMS_OUTCOME_UNCERTAIN : MODEM_SMS_OUTCOME_TIMEOUT;
    if (restore) restore_text_parameters(hooks, now_ms, outcome);
    else {
        emit_send_result(request, hooks, outcome);
        emit_complete(request, hooks, outcome, false);
    }
}

modem_sms_outcome_t modem_sms_protocol_cancel_outcome(const modem_sms_protocol_request_t *request) {
    if (!request_valid(request)) return MODEM_SMS_OUTCOME_CANCELLED;
    if (s_protocol.send_confirmed || s_protocol.send_outcome == MODEM_SMS_OUTCOME_OK)
        return MODEM_SMS_OUTCOME_OK;
    return s_protocol.payload_submitted || s_protocol.binary_segments_accepted != 0u
        ? MODEM_SMS_OUTCOME_UNCERTAIN : MODEM_SMS_OUTCOME_CANCELLED;
}
void modem_sms_protocol_cancel(void) {
    modem_sms_state_binary_set_send_ok(false);
    memset(&s_protocol, 0, sizeof(s_protocol));
}

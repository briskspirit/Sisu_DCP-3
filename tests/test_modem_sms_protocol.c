#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "services/log.h"
#include "services/modem_sms_protocol_internal.h"
#include "services/modem_sms_state_internal.h"
#include "services/sms_submit_codec.h"

#define CAPTURE_MAX 16u
#define BODY_MAX 512u

typedef struct {
    modem_sms_action_type_t type;
    modem_sms_command_kind_t command_kind;
    char command[160];
    uint32_t timeout_ms;
    uint32_t now_ms;
    bool raw;
    bool append_cr;
    uint8_t body[BODY_MAX];
    size_t body_len;
    modem_sms_command_kind_t final_kind;
    uint32_t request_id;
    modem_sms_request_kind_t request_kind;
    modem_sms_outcome_t outcome;
    bool binary;
    bool ok;
    bool sim_not_ready;
    bool complete;
    bool restore_after_settle;
    modem_sms_record_t record;
} captured_action_t;

static captured_action_t s_actions[CAPTURE_MAX];
static size_t s_action_count;
static bool s_sim_ready;
static bool s_call_preempt;
static bool s_auto_dispatch_commands;
static int s_failures;

void log_write(log_level_t level, const char *tag, const char *fmt, ...) {
    (void)level;
    (void)tag;
    (void)fmt;
}

static void check(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        s_failures++;
    }
}

static void clear_actions(void) {
    memset(s_actions, 0, sizeof(s_actions));
    s_action_count = 0u;
}

static bool capture_emit(const modem_sms_protocol_action_t *action) {
    if (action == NULL || s_action_count >= CAPTURE_MAX) {
        return false;
    }
    captured_action_t *captured = &s_actions[s_action_count++];
    captured->type = action->type;
    switch (action->type) {
    case MODEM_SMS_ACTION_COMMAND:
        captured->command_kind = action->data.command.kind;
        captured->timeout_ms = action->data.command.timeout_ms;
        captured->now_ms = action->data.command.now_ms;
        captured->raw = action->data.command.raw;
        captured->append_cr = action->data.command.append_cr;
        snprintf(captured->command, sizeof(captured->command), "%s",
                 action->data.command.command != NULL
                     ? action->data.command.command : "");
        if (s_auto_dispatch_commands) {
            modem_sms_protocol_command_dispatched(
                action->data.command.kind);
        }
        break;
    case MODEM_SMS_ACTION_BODY:
        captured->body_len = action->data.body.length;
        if (captured->body_len > sizeof(captured->body)) {
            captured->body_len = sizeof(captured->body);
        }
        if (action->data.body.bytes != NULL) {
            memcpy(captured->body, action->data.body.bytes,
                   captured->body_len);
        }
        captured->final_kind = action->data.body.final_kind;
        captured->timeout_ms = action->data.body.timeout_ms;
        captured->now_ms = action->data.body.now_ms;
        captured->binary = action->data.body.binary;
        break;
    case MODEM_SMS_ACTION_ABORT_PROMPT:
        captured->now_ms = action->data.abort_prompt.now_ms;
        captured->restore_after_settle =
            action->data.abort_prompt.restore_after_settle;
        break;
    case MODEM_SMS_ACTION_PUBLISH_SEND:
    case MODEM_SMS_ACTION_PUBLISH_SAVE:
    case MODEM_SMS_ACTION_PUBLISH_DELETE:
        captured->request_id = action->data.result.request_id;
        captured->request_kind = action->data.result.kind;
        captured->outcome = action->data.result.outcome;
        captured->ok = action->data.result.outcome == MODEM_SMS_OUTCOME_OK;
        captured->sim_not_ready = action->data.result.sim_not_ready;
        break;
    case MODEM_SMS_ACTION_PUBLISH_READ:
        captured->request_id = action->data.read_result.request_id;
        captured->request_kind = action->data.read_result.kind;
        captured->outcome = action->data.read_result.outcome;
        captured->ok =
            action->data.read_result.outcome == MODEM_SMS_OUTCOME_OK;
        captured->sim_not_ready = action->data.read_result.sim_not_ready;
        break;
    case MODEM_SMS_ACTION_PUBLISH_MAILBOX:
        captured->request_id = action->data.mailbox_result.request_id;
        captured->request_kind = action->data.mailbox_result.kind;
        captured->outcome = action->data.mailbox_result.outcome;
        captured->ok =
            action->data.mailbox_result.outcome == MODEM_SMS_OUTCOME_OK;
        captured->sim_not_ready = action->data.mailbox_result.sim_not_ready;
        captured->complete = action->data.mailbox_result.complete;
        break;
    case MODEM_SMS_ACTION_APPEND_MAILBOX:
        if (action->data.append_mailbox.record == NULL) {
            return false;
        }
        captured->record = *action->data.append_mailbox.record;
        return modem_sms_state_mailbox_append(
            action->data.append_mailbox.record);
    case MODEM_SMS_ACTION_ARRIVAL_SCAN_COMMIT:
        captured->complete = action->data.arrival_commit.complete;
        break;
    case MODEM_SMS_ACTION_COMPLETE:
        captured->request_id = action->data.complete.request_id;
        captured->request_kind = action->data.complete.kind;
        captured->outcome = action->data.complete.outcome;
        captured->ok = action->data.complete.command_ok;
        break;
    case MODEM_SMS_ACTION_SELECTED_BEGIN:
        /* Mirror the root host's synchronous state effect so later protocol
         * calls exercise the real selected-read cursor. */
        modem_sms_state_selected_begin(
            action->data.selected_begin.first_index,
            action->data.selected_begin.index_count,
            action->data.selected_begin.quarantined);
        break;
    default:
        break;
    }
    return true;
}

static bool capture_sim_ready(void) {
    return s_sim_ready;
}

static bool capture_call_preempt(void) {
    return s_call_preempt;
}

static const modem_sms_protocol_hooks_t s_hooks = {
    .emit = capture_emit,
    .sim_ready = capture_sim_ready,
    .call_preempt_pending = capture_call_preempt,
};

static void reset_fixture(void) {
    modem_sms_state_init();
    modem_sms_protocol_init();
    s_sim_ready = true;
    s_call_preempt = false;
    s_auto_dispatch_commands = true;
    clear_actions();
}

static modem_sms_protocol_request_t text_request(void) {
    modem_sms_protocol_request_t request;
    memset(&request, 0, sizeof(request));
    request.request_id = UINT32_C(0x1001);
    request.operation = MODEM_SMS_PROTOCOL_SEND_TEXT;
    request.number = "+15551234567";
    request.text = "hello";
    return request;
}

static modem_sms_protocol_request_t inbox_request(void) {
    modem_sms_protocol_request_t request;
    memset(&request, 0, sizeof(request));
    request.request_id = UINT32_C(0x1002);
    request.operation = MODEM_SMS_PROTOCOL_MAILBOX;
    request.mailbox = MODEM_SMS_MAILBOX_INBOX;
    request.read_status.preserve_unread_cmd = "AT+PRESERVE";
    request.read_status.consume_unread_cmd = "AT+CONSUME";
    request.read_status.timeout_ms = 7000u;
    return request;
}

static modem_sms_message_t orphan_segment(uint16_t index, uint8_t sequence) {
    modem_sms_message_t message;
    memset(&message, 0, sizeof(message));
    message.index = index;
    message.binary = true;
    message.has_ports = true;
    message.has_concat = true;
    message.dest_port = SMS_CODEC_PICTURE_PORT;
    message.concat_ref = 0x55u;
    message.concat_total = 3u;
    message.concat_seq = sequence;
    message.binary_len = 1u;
    message.binary_data[0] = sequence;
    snprintf(message.status, sizeof(message.status), "REC UNREAD");
    snprintf(message.sender, sizeof(message.sender), "+15551230000");
    snprintf(message.timestamp, sizeof(message.timestamp),
             "26/08/21,12:34:56-16");
    return message;
}

static void check_command(size_t position, modem_sms_command_kind_t kind,
                          const char *command, uint32_t timeout_ms,
                          bool raw, bool append_cr, const char *message) {
    bool match = position < s_action_count &&
        s_actions[position].type == MODEM_SMS_ACTION_COMMAND &&
        s_actions[position].command_kind == kind &&
        strcmp(s_actions[position].command, command) == 0 &&
        s_actions[position].timeout_ms == timeout_ms &&
        s_actions[position].raw == raw &&
        s_actions[position].append_cr == append_cr;
    check(match, message);
}

static void test_text_send_and_sent_copy_failure(void) {
    reset_fixture();
    modem_sms_protocol_request_t request = text_request();
    check(modem_sms_protocol_begin(&request, &s_hooks, 100u) &&
              s_action_count == 1u,
          "text send begins without mutating a prior result channel");
    check_command(
        0u, MODEM_SMS_COMMAND_CPMS,
        "AT+CPMS=\"" MODEM_SMS_STORAGE "\",\"" MODEM_SMS_STORAGE
        "\",\"" MODEM_SMS_STORAGE "\"",
        5000u, false, false, "text send starts with exact CPMS command");

    clear_actions();
    modem_sms_protocol_on_final(
        MODEM_SMS_COMMAND_CPMS, true, &request, &s_hooks, 110u);
    check(s_action_count == 1u &&
              s_actions[0].type ==
                  MODEM_SMS_ACTION_REQUEST_WAKE_CONTINUATION,
          "CPMS success yields through the core wake qualifier");

    clear_actions();
    modem_sms_protocol_resume_after_wake(&request, &s_hooks, 120u);
    check_command(0u, MODEM_SMS_COMMAND_CMGS_PROMPT,
                  "AT+CMGS=\"+15551234567\",145", 5000u, true, true,
                  "qualified text send opens the exact CMGS prompt");

    clear_actions();
    check(modem_sms_protocol_on_prompt(
              MODEM_SMS_COMMAND_CMGS_PROMPT, &request, &s_hooks, 130u) &&
              s_action_count == 1u &&
              s_actions[0].type == MODEM_SMS_ACTION_BODY &&
              s_actions[0].body_len == 5u &&
              memcmp(s_actions[0].body, "hello", 5u) == 0 &&
              s_actions[0].final_kind == MODEM_SMS_COMMAND_CMGS_FINAL &&
              s_actions[0].timeout_ms == 30000u &&
              !s_actions[0].binary,
          "CMGS prompt emits the borrowed text body and final contract");

    clear_actions();
    modem_sms_protocol_on_final(
        MODEM_SMS_COMMAND_CMGS_FINAL, true, &request, &s_hooks, 140u);
    check(s_action_count == 2u &&
              s_actions[0].type == MODEM_SMS_ACTION_INCREMENT_SENT,
          "network acceptance increments the sent counter once");
    check_command(1u, MODEM_SMS_COMMAND_CMGW_PROMPT,
                  "AT+CMGW=\"+15551234567\",145,\"STO SENT\"",
                  5000u, true, true,
                  "network acceptance opens the exact sent-copy prompt");

    clear_actions();
    modem_sms_protocol_on_final(
        MODEM_SMS_COMMAND_CMGW_FINAL, false, &request, &s_hooks, 150u);
    check(s_action_count == 2u &&
              s_actions[0].type == MODEM_SMS_ACTION_PUBLISH_SEND &&
              s_actions[0].request_id == request.request_id &&
              s_actions[0].request_kind == MODEM_SMS_REQUEST_SEND_TEXT &&
              s_actions[0].outcome == MODEM_SMS_OUTCOME_OK &&
              s_actions[1].type == MODEM_SMS_ACTION_COMPLETE &&
              s_actions[1].request_id == request.request_id &&
              s_actions[1].outcome == MODEM_SMS_OUTCOME_OK &&
              s_actions[1].ok,
          "sent-copy failure cannot turn a delivered SMS into retryable failure");

    clear_actions();
    modem_sms_protocol_on_final(
        MODEM_SMS_COMMAND_CMGW_PROMPT, false, &request, &s_hooks, 160u);
    check(s_action_count == 2u &&
              s_actions[0].type == MODEM_SMS_ACTION_PUBLISH_SEND &&
              s_actions[0].outcome == MODEM_SMS_OUTCOME_OK &&
              s_actions[1].type == MODEM_SMS_ACTION_COMPLETE &&
              s_actions[1].outcome == MODEM_SMS_OUTCOME_OK &&
              s_actions[1].ok,
          "sent-copy prompt error preserves network-send success");

    clear_actions();
    modem_sms_protocol_on_timeout(
        MODEM_SMS_COMMAND_CMGW_PROMPT, &request, &s_hooks, 170u);
    check(s_action_count == 3u &&
              s_actions[0].type == MODEM_SMS_ACTION_ABORT_PROMPT &&
              !s_actions[0].restore_after_settle &&
              s_actions[1].type == MODEM_SMS_ACTION_PUBLISH_SEND &&
              s_actions[1].outcome == MODEM_SMS_OUTCOME_OK &&
              s_actions[2].type == MODEM_SMS_ACTION_COMPLETE &&
              s_actions[2].outcome == MODEM_SMS_OUTCOME_OK &&
              s_actions[2].ok,
          "lost sent-copy prompt aborts transport without inviting SMS retry");
}

static void test_binary_prompt_timeout_restore(void) {
    reset_fixture();
    static const uint8_t payload[] = {0x10u, 0x20u, 0x30u};
    modem_sms_protocol_request_t request;
    memset(&request, 0, sizeof(request));
    request.request_id = UINT32_C(0x1003);
    request.operation = MODEM_SMS_PROTOCOL_SEND_BINARY;
    request.number = "5550100";
    request.binary = payload;
    request.binary_len = sizeof(payload);
    request.dest_port = 0x158au;
    request.source_port = 0x158au;
    request.binary_mode = MODEM_BINARY_SMS_MODE_DCS04_PORT_FIRST;

    check(modem_sms_protocol_begin(&request, &s_hooks, 200u),
          "binary send begins");
    check_command(0u, MODEM_SMS_COMMAND_CMGF_PDU, "AT+CMGF=0", 5000u,
                  false, false, "binary send first enters PDU mode");
    clear_actions();
    modem_sms_protocol_on_final(
        MODEM_SMS_COMMAND_CMGF_PDU, true, &request, &s_hooks, 210u);
    check(s_action_count == 1u &&
              s_actions[0].type == MODEM_SMS_ACTION_COMMAND &&
              s_actions[0].command_kind == MODEM_SMS_COMMAND_CMGS_PROMPT &&
              strncmp(s_actions[0].command, "AT+CMGS=", 8u) == 0,
          "PDU mode builds a segment and opens its prompt");

    clear_actions();
    check(modem_sms_protocol_on_prompt(
              MODEM_SMS_COMMAND_CMGS_PROMPT, &request, &s_hooks, 220u) &&
              s_action_count == 1u &&
              s_actions[0].type == MODEM_SMS_ACTION_BODY &&
              s_actions[0].binary && s_actions[0].body_len != 0u &&
              s_actions[0].timeout_ms == 120000u,
          "binary prompt emits PDU hex under the long network deadline");

    reset_fixture();
    check(modem_sms_protocol_begin(&request, &s_hooks, 230u),
          "binary timeout fixture begins");
    clear_actions();
    modem_sms_protocol_on_timeout(
        MODEM_SMS_COMMAND_CMGS_PROMPT, &request, &s_hooks, 240u);
    check(s_action_count == 1u &&
              s_actions[0].type == MODEM_SMS_ACTION_ABORT_PROMPT &&
              s_actions[0].restore_after_settle,
          "lost binary prompt requests ESC plus deferred text restore");

    clear_actions();
    modem_sms_protocol_resume_after_prompt_abort(
        &request, &s_hooks, 750u);
    check_command(0u, MODEM_SMS_COMMAND_CMGF_TEXT, "AT+CMGF=1", 5000u,
                  false, false,
                  "post-ESC continuation restores text mode exactly once");
}

static void test_binary_cleanup_duplicate_wedge(void) {
    static const uint8_t payload[] = {0x10u};
    modem_sms_protocol_request_t request;
    memset(&request, 0, sizeof(request));
    request.request_id = UINT32_C(0x1004);
    request.operation = MODEM_SMS_PROTOCOL_SEND_BINARY;
    request.number = "5550100";
    request.binary = payload;
    request.binary_len = sizeof(payload);

    reset_fixture();
    check(modem_sms_protocol_begin(&request, &s_hooks, 250u),
          "accepted binary cleanup fixture begins");
    clear_actions();
    modem_sms_protocol_on_final(
        MODEM_SMS_COMMAND_CMGF_PDU, true, &request, &s_hooks, 251u);
    clear_actions();
    check(modem_sms_protocol_on_prompt(
              MODEM_SMS_COMMAND_CMGS_PROMPT, &request, &s_hooks, 252u),
          "accepted binary cleanup fixture submits its payload");
    clear_actions();
    modem_sms_protocol_on_final(
        MODEM_SMS_COMMAND_CMGS_FINAL, true, &request, &s_hooks, 253u);
    clear_actions();
    modem_sms_protocol_on_final(
        MODEM_SMS_COMMAND_CMGF_TEXT, false, &request, &s_hooks, 260u);
    check(s_action_count == 3u &&
              s_actions[0].type == MODEM_SMS_ACTION_INCREMENT_SENT &&
              s_actions[1].type == MODEM_SMS_ACTION_PUBLISH_SEND &&
              s_actions[1].outcome == MODEM_SMS_OUTCOME_OK &&
              s_actions[2].type == MODEM_SMS_ACTION_COMPLETE &&
              s_actions[2].outcome == MODEM_SMS_OUTCOME_OK &&
              !s_actions[2].ok,
          "failed text-restore final preserves accepted binary send without replay");

    reset_fixture();
    check(modem_sms_protocol_begin(&request, &s_hooks, 264u),
          "unsent binary cleanup fixture begins");
    clear_actions();
    modem_sms_protocol_on_final(
        MODEM_SMS_COMMAND_CMGF_PDU, false, &request, &s_hooks, 265u);
    clear_actions();
    modem_sms_protocol_on_final(
        MODEM_SMS_COMMAND_CMGF_TEXT, true, &request, &s_hooks, 261u);
    check(s_action_count == 2u &&
              s_actions[0].type == MODEM_SMS_ACTION_PUBLISH_SEND &&
              s_actions[0].outcome == MODEM_SMS_OUTCOME_ERROR &&
              s_actions[1].type == MODEM_SMS_ACTION_COMPLETE &&
              s_actions[1].outcome == MODEM_SMS_OUTCOME_ERROR &&
              !s_actions[1].ok,
          "successful cleanup cannot invent acceptance for an unsent binary SMS");

    reset_fixture();
    check(modem_sms_protocol_begin(&request, &s_hooks, 266u),
          "accepted binary timeout fixture begins");
    clear_actions();
    modem_sms_protocol_on_final(
        MODEM_SMS_COMMAND_CMGF_PDU, true, &request, &s_hooks, 267u);
    clear_actions();
    check(modem_sms_protocol_on_prompt(
              MODEM_SMS_COMMAND_CMGS_PROMPT, &request, &s_hooks, 268u),
          "accepted binary timeout fixture submits its payload");
    clear_actions();
    modem_sms_protocol_on_final(
        MODEM_SMS_COMMAND_CMGS_FINAL, true, &request, &s_hooks, 269u);
    clear_actions();
    modem_sms_protocol_on_timeout(
        MODEM_SMS_COMMAND_CMGF_TEXT, &request, &s_hooks, 262u);
    check(s_action_count == 3u &&
              s_actions[0].type == MODEM_SMS_ACTION_INCREMENT_SENT &&
              s_actions[1].type == MODEM_SMS_ACTION_PUBLISH_SEND &&
              s_actions[1].outcome == MODEM_SMS_OUTCOME_OK &&
              s_actions[2].type == MODEM_SMS_ACTION_COMPLETE &&
              s_actions[2].outcome == MODEM_SMS_OUTCOME_OK &&
              !s_actions[2].ok,
          "lost text-restore final consults accepted-send evidence before failing transport");

    reset_fixture();
    check(modem_sms_protocol_begin(&request, &s_hooks, 270u),
          "unsent binary timeout fixture begins");
    clear_actions();
    modem_sms_protocol_on_timeout(
        MODEM_SMS_COMMAND_CMGF_PDU, &request, &s_hooks, 271u);
    check_command(0u, MODEM_SMS_COMMAND_CMGF_TEXT, "AT+CMGF=1", 5000u,
                  false, false,
                  "PDU-mode timeout still attempts text-mode restoration");
    clear_actions();
    modem_sms_protocol_on_timeout(
        MODEM_SMS_COMMAND_CMGF_TEXT, &request, &s_hooks, 263u);
    check(s_action_count == 2u &&
              s_actions[0].type == MODEM_SMS_ACTION_PUBLISH_SEND &&
              s_actions[0].outcome == MODEM_SMS_OUTCOME_TIMEOUT &&
              s_actions[1].type == MODEM_SMS_ACTION_COMPLETE &&
              s_actions[1].outcome == MODEM_SMS_OUTCOME_TIMEOUT &&
              !s_actions[1].ok,
          "cleanup timeout without payload acceptance remains retry-safe");
}

static void advance_text_to_prompt(modem_sms_protocol_request_t *request,
                                   uint32_t now_ms) {
    check(modem_sms_protocol_begin(request, &s_hooks, now_ms),
          "text evidence fixture begins");
    clear_actions();
    modem_sms_protocol_on_final(
        MODEM_SMS_COMMAND_CPMS, true, request, &s_hooks, now_ms + 1u);
    clear_actions();
    modem_sms_protocol_resume_after_wake(
        request, &s_hooks, now_ms + 2u);
}

static void test_send_evidence_boundaries(void) {
    modem_sms_protocol_request_t request = text_request();

    reset_fixture();
    advance_text_to_prompt(&request, 280u);
    clear_actions();
    modem_sms_protocol_on_timeout(
        MODEM_SMS_COMMAND_CMGS_PROMPT, &request, &s_hooks, 283u);
    check(s_action_count == 3u &&
              s_actions[0].type == MODEM_SMS_ACTION_ABORT_PROMPT &&
              s_actions[1].type == MODEM_SMS_ACTION_PUBLISH_SEND &&
              s_actions[1].request_id == request.request_id &&
              s_actions[1].request_kind == MODEM_SMS_REQUEST_SEND_TEXT &&
              s_actions[1].outcome == MODEM_SMS_OUTCOME_TIMEOUT &&
              s_actions[2].type == MODEM_SMS_ACTION_COMPLETE &&
              s_actions[2].outcome == MODEM_SMS_OUTCOME_TIMEOUT,
          "a lost pre-body CMGS prompt remains safely retryable");

    reset_fixture();
    advance_text_to_prompt(&request, 290u);
    clear_actions();
    check(modem_sms_protocol_on_prompt(
              MODEM_SMS_COMMAND_CMGS_PROMPT, &request, &s_hooks, 293u),
          "text uncertainty fixture hands the body to the modem");
    clear_actions();
    modem_sms_protocol_on_timeout(
        MODEM_SMS_COMMAND_CMGS_FINAL, &request, &s_hooks, 294u);
    check(s_action_count == 2u &&
              s_actions[0].type == MODEM_SMS_ACTION_PUBLISH_SEND &&
              s_actions[0].request_id == request.request_id &&
              s_actions[0].outcome == MODEM_SMS_OUTCOME_UNCERTAIN &&
              s_actions[1].type == MODEM_SMS_ACTION_COMPLETE &&
              s_actions[1].outcome == MODEM_SMS_OUTCOME_UNCERTAIN &&
              !s_actions[1].ok,
          "a lost CMGS final is explicitly uncertain, never retryable error");

    reset_fixture();
    advance_text_to_prompt(&request, 300u);
    clear_actions();
    check(modem_sms_protocol_on_prompt(
              MODEM_SMS_COMMAND_CMGS_PROMPT, &request, &s_hooks, 303u),
          "sent-copy fixture hands the body to the modem");
    clear_actions();
    modem_sms_protocol_on_final(
        MODEM_SMS_COMMAND_CMGS_FINAL, true, &request, &s_hooks, 304u);
    clear_actions();
    modem_sms_protocol_on_timeout(
        MODEM_SMS_COMMAND_CMGW_FINAL, &request, &s_hooks, 305u);
    check(s_action_count == 2u &&
              s_actions[0].type == MODEM_SMS_ACTION_PUBLISH_SEND &&
              s_actions[0].outcome == MODEM_SMS_OUTCOME_OK &&
              s_actions[1].type == MODEM_SMS_ACTION_COMPLETE &&
              s_actions[1].outcome == MODEM_SMS_OUTCOME_OK &&
              s_actions[1].ok,
          "a lost sent-copy final cannot erase confirmed network delivery");
}

static void test_mutating_timeout_boundaries(void) {
    modem_sms_protocol_request_t save;
    memset(&save, 0, sizeof(save));
    save.request_id = UINT32_C(0x1010);
    save.operation = MODEM_SMS_PROTOCOL_SAVE;
    save.number = "+15550100";
    save.text = "draft";

    reset_fixture();
    check(modem_sms_protocol_begin(&save, &s_hooks, 310u),
          "save uncertainty fixture begins");
    clear_actions();
    modem_sms_protocol_on_final(
        MODEM_SMS_COMMAND_CPMS, true, &save, &s_hooks, 311u);
    clear_actions();
    modem_sms_protocol_resume_after_wake(&save, &s_hooks, 312u);
    clear_actions();
    check(modem_sms_protocol_on_prompt(
              MODEM_SMS_COMMAND_CMGW_PROMPT, &save, &s_hooks, 313u),
          "save uncertainty fixture hands the draft to the modem");
    clear_actions();
    modem_sms_protocol_on_timeout(
        MODEM_SMS_COMMAND_CMGW_FINAL, &save, &s_hooks, 314u);
    check(s_action_count == 2u &&
              s_actions[0].type == MODEM_SMS_ACTION_PUBLISH_SAVE &&
              s_actions[0].request_id == save.request_id &&
              s_actions[0].request_kind == MODEM_SMS_REQUEST_SAVE &&
              s_actions[0].outcome == MODEM_SMS_OUTCOME_UNCERTAIN &&
              s_actions[1].type == MODEM_SMS_ACTION_COMPLETE &&
              s_actions[1].outcome == MODEM_SMS_OUTCOME_UNCERTAIN,
          "a lost CMGW final reports uncertain draft persistence");

    static const uint16_t delete_index = 7u;
    modem_sms_protocol_request_t deletion;
    memset(&deletion, 0, sizeof(deletion));
    deletion.request_id = UINT32_C(0x1011);
    deletion.operation = MODEM_SMS_PROTOCOL_DELETE;
    deletion.indices = &delete_index;
    deletion.index_count = 1u;
    reset_fixture();
    check(modem_sms_protocol_begin(&deletion, &s_hooks, 320u),
          "delete uncertainty fixture begins");
    clear_actions();
    modem_sms_protocol_on_final(
        MODEM_SMS_COMMAND_CPMS, true, &deletion, &s_hooks, 321u);
    clear_actions();
    modem_sms_protocol_resume_after_wake(&deletion, &s_hooks, 322u);
    clear_actions();
    modem_sms_protocol_on_timeout(
        MODEM_SMS_COMMAND_CMGD, &deletion, &s_hooks, 323u);
    check(s_action_count == 2u &&
              s_actions[0].type == MODEM_SMS_ACTION_PUBLISH_DELETE &&
              s_actions[0].request_id == deletion.request_id &&
              s_actions[0].outcome == MODEM_SMS_OUTCOME_UNCERTAIN &&
              s_actions[1].type == MODEM_SMS_ACTION_COMPLETE &&
              s_actions[1].outcome == MODEM_SMS_OUTCOME_UNCERTAIN,
          "a lost CMGD final reports uncertain deletion");
}

static void test_binary_uncertainty_boundaries(void) {
    static uint8_t payload[MODEM_SMS_BINARY_CHUNK_MAX + 1u];
    memset(payload, 0x5au, sizeof(payload));
    modem_sms_protocol_request_t request;
    memset(&request, 0, sizeof(request));
    request.request_id = UINT32_C(0x1012);
    request.operation = MODEM_SMS_PROTOCOL_SEND_BINARY;
    request.number = "5550100";
    request.binary = payload;
    request.binary_len = 1u;
    request.dest_port = 0x158au;

    reset_fixture();
    check(modem_sms_protocol_begin(&request, &s_hooks, 330u),
          "binary final-timeout fixture begins");
    clear_actions();
    modem_sms_protocol_on_final(
        MODEM_SMS_COMMAND_CMGF_PDU, true, &request, &s_hooks, 331u);
    clear_actions();
    check(modem_sms_protocol_on_prompt(
              MODEM_SMS_COMMAND_CMGS_PROMPT, &request, &s_hooks, 332u),
          "binary final-timeout fixture submits its first segment");
    clear_actions();
    modem_sms_protocol_on_timeout(
        MODEM_SMS_COMMAND_CMGS_FINAL, &request, &s_hooks, 333u);
    check_command(0u, MODEM_SMS_COMMAND_CMGF_TEXT, "AT+CMGF=1", 5000u,
                  false, false,
                  "lost binary final restores text mode before publishing");
    clear_actions();
    modem_sms_protocol_on_final(
        MODEM_SMS_COMMAND_CMGF_TEXT, true, &request, &s_hooks, 334u);
    check(s_action_count == 2u &&
              s_actions[0].type == MODEM_SMS_ACTION_PUBLISH_SEND &&
              s_actions[0].request_id == request.request_id &&
              s_actions[0].request_kind == MODEM_SMS_REQUEST_SEND_BINARY &&
              s_actions[0].outcome == MODEM_SMS_OUTCOME_UNCERTAIN &&
              s_actions[1].type == MODEM_SMS_ACTION_COMPLETE &&
              s_actions[1].outcome == MODEM_SMS_OUTCOME_UNCERTAIN,
          "a lost binary CMGS final remains uncertain after cleanup");

    request.binary_len = sizeof(payload);
    reset_fixture();
    check(modem_sms_protocol_begin(&request, &s_hooks, 340u),
          "multipart binary partial-acceptance fixture begins");
    clear_actions();
    modem_sms_protocol_on_final(
        MODEM_SMS_COMMAND_CMGF_PDU, true, &request, &s_hooks, 341u);
    clear_actions();
    check(modem_sms_protocol_on_prompt(
              MODEM_SMS_COMMAND_CMGS_PROMPT, &request, &s_hooks, 342u),
          "multipart fixture submits segment one");
    clear_actions();
    modem_sms_protocol_on_final(
        MODEM_SMS_COMMAND_CMGS_FINAL, true, &request, &s_hooks, 343u);
    clear_actions();
    check(modem_sms_protocol_on_prompt(
              MODEM_SMS_COMMAND_CMGS_PROMPT, &request, &s_hooks, 344u),
          "multipart fixture submits segment two");
    clear_actions();
    modem_sms_protocol_on_final(
        MODEM_SMS_COMMAND_CMGS_FINAL, false, &request, &s_hooks, 345u);
    clear_actions();
    modem_sms_protocol_on_final(
        MODEM_SMS_COMMAND_CMGF_TEXT, true, &request, &s_hooks, 346u);
    check(s_action_count == 2u &&
              s_actions[0].type == MODEM_SMS_ACTION_PUBLISH_SEND &&
              s_actions[0].outcome == MODEM_SMS_OUTCOME_UNCERTAIN &&
              s_actions[1].type == MODEM_SMS_ACTION_COMPLETE &&
              s_actions[1].outcome == MODEM_SMS_OUTCOME_UNCERTAIN,
          "failure after one accepted binary segment cannot invite whole-message retry");
}

static void test_cancel_evidence(void) {
    modem_sms_protocol_request_t text = text_request();
    reset_fixture();
    check(modem_sms_protocol_begin(&text, &s_hooks, 350u) &&
              modem_sms_protocol_cancel_outcome(&text) ==
                  MODEM_SMS_OUTCOME_CANCELLED,
          "text cancellation before body submission is retry-safe");
    modem_sms_protocol_cancel();

    reset_fixture();
    advance_text_to_prompt(&text, 360u);
    clear_actions();
    check(modem_sms_protocol_on_prompt(
              MODEM_SMS_COMMAND_CMGS_PROMPT, &text, &s_hooks, 363u) &&
              modem_sms_protocol_cancel_outcome(&text) ==
                  MODEM_SMS_OUTCOME_UNCERTAIN,
          "text cancellation after body submission is uncertain");
    modem_sms_protocol_cancel();

    reset_fixture();
    advance_text_to_prompt(&text, 370u);
    clear_actions();
    check(modem_sms_protocol_on_prompt(
              MODEM_SMS_COMMAND_CMGS_PROMPT, &text, &s_hooks, 373u),
          "confirmed-send cancellation fixture submits its body");
    clear_actions();
    modem_sms_protocol_on_final(
        MODEM_SMS_COMMAND_CMGS_FINAL, true, &text, &s_hooks, 374u);
    check(modem_sms_protocol_cancel_outcome(&text) == MODEM_SMS_OUTCOME_OK,
          "cancellation after CMGS acceptance preserves confirmed delivery");
    modem_sms_protocol_cancel();
}

static void test_delete_dispatch_evidence(void) {
    static const uint16_t indices[] = {4u, 7u};
    modem_sms_protocol_request_t deletion;
    memset(&deletion, 0, sizeof(deletion));
    deletion.request_id = UINT32_C(0x1013);
    deletion.operation = MODEM_SMS_PROTOCOL_DELETE;
    deletion.indices = indices;
    deletion.index_count = 2u;

    reset_fixture();
    check(modem_sms_protocol_begin(&deletion, &s_hooks, 380u),
          "pre-dispatch delete fixture begins");
    clear_actions();
    s_auto_dispatch_commands = false;
    modem_sms_protocol_resume_after_wake(&deletion, &s_hooks, 381u);
    check(s_action_count == 1u &&
              s_actions[0].command_kind == MODEM_SMS_COMMAND_CMGD &&
              modem_sms_protocol_cancel_outcome(&deletion) ==
                  MODEM_SMS_OUTCOME_CANCELLED,
          "an emitted but UART-deferred first delete remains retry-safe");

    reset_fixture();
    check(modem_sms_protocol_begin(&deletion, &s_hooks, 390u),
          "dispatched delete fixture begins");
    clear_actions();
    modem_sms_protocol_resume_after_wake(&deletion, &s_hooks, 391u);
    check(modem_sms_protocol_cancel_outcome(&deletion) ==
              MODEM_SMS_OUTCOME_UNCERTAIN,
          "a delete that crossed UART is uncertain on cancellation");

    reset_fixture();
    check(modem_sms_protocol_begin(&deletion, &s_hooks, 400u),
          "partially applied delete fixture begins");
    clear_actions();
    modem_sms_protocol_resume_after_wake(&deletion, &s_hooks, 401u);
    s_auto_dispatch_commands = false;
    clear_actions();
    modem_sms_protocol_on_final(
        MODEM_SMS_COMMAND_CMGD, true, &deletion, &s_hooks, 402u);
    check(s_action_count == 1u &&
              s_actions[0].command_kind == MODEM_SMS_COMMAND_CMGD &&
              modem_sms_protocol_cancel_outcome(&deletion) ==
                  MODEM_SMS_OUTCOME_UNCERTAIN,
          "one accepted delete keeps the batch uncertain while its next command is deferred");
}

static void test_cpms_failure_and_mailbox_policy(void) {
    reset_fixture();
    modem_sms_protocol_request_t save;
    memset(&save, 0, sizeof(save));
    save.request_id = UINT32_C(0x1005);
    save.operation = MODEM_SMS_PROTOCOL_SAVE;
    save.text = "draft";
    check(modem_sms_protocol_begin(&save, &s_hooks, 300u),
          "save begins with no destination number");
    s_sim_ready = false;
    clear_actions();
    modem_sms_protocol_on_final(
        MODEM_SMS_COMMAND_CPMS, false, &save, &s_hooks, 310u);
    check(s_action_count == 2u &&
              s_actions[0].type == MODEM_SMS_ACTION_PUBLISH_SAVE &&
              !s_actions[0].ok && s_actions[0].sim_not_ready &&
              s_actions[1].type == MODEM_SMS_ACTION_COMPLETE &&
              !s_actions[1].ok,
          "CPMS failure publishes the SIM-not-ready save result");

    reset_fixture();
    modem_sms_protocol_request_t mailbox;
    memset(&mailbox, 0, sizeof(mailbox));
    mailbox.request_id = UINT32_C(0x1006);
    mailbox.operation = MODEM_SMS_PROTOCOL_MAILBOX;
    mailbox.mailbox = MODEM_SMS_MAILBOX_INBOX;
    mailbox.read_status.preserve_unread_cmd = "AT+PRESERVE";
    mailbox.read_status.consume_unread_cmd = "AT+CONSUME";
    mailbox.read_status.timeout_ms = 7000u;
    check(modem_sms_protocol_begin(&mailbox, &s_hooks, 320u) &&
              s_action_count == 3u &&
              s_actions[0].type == MODEM_SMS_ACTION_CLEAR_MAILBOX &&
              s_actions[1].type == MODEM_SMS_ACTION_ARRIVAL_SCAN_BEGIN,
          "inbox begin clears cache and opens an arrival scan");
    clear_actions();
    modem_sms_protocol_resume_after_wake(&mailbox, &s_hooks, 330u);
    check_command(0u, MODEM_SMS_COMMAND_STATUS_PRESERVE,
                  "AT+PRESERVE", 7000u, false, false,
                  "mailbox read uses the vendor-neutral preserve policy");
}

static void test_orphan_and_corrupt_mailbox_rows_settle(void) {
    reset_fixture();
    modem_sms_protocol_request_t mailbox = inbox_request();
    check(modem_sms_protocol_begin(&mailbox, &s_hooks, 350u),
          "orphan mailbox fixture begins");
    modem_sms_message_t second = orphan_segment(11u, 2u);
    modem_sms_message_t third = orphan_segment(12u, 3u);
    modem_sms_record_t record;
    check(!modem_sms_state_mailbox_prepare_record(&third, &record) &&
              !modem_sms_state_mailbox_prepare_record(&second, &record),
          "orphan fixture leaves one incomplete group before CMGL final");

    clear_actions();
    modem_sms_protocol_on_final(
        MODEM_SMS_COMMAND_CMGL, true, &mailbox, &s_hooks, 351u);
    check(s_action_count == 2u &&
              s_actions[0].type == MODEM_SMS_ACTION_APPEND_MAILBOX &&
              s_actions[0].record.quarantined &&
              s_actions[0].record.index_count == 2u &&
              s_actions[0].record.indices[0] == 11u &&
              s_actions[0].record.indices[1] == 12u &&
              s_actions[1].type == MODEM_SMS_ACTION_COMMAND &&
              s_actions[1].command_kind == MODEM_SMS_COMMAND_CMGF_TEXT &&
              modem_sms_state_mailbox_count() == 1u,
          "CMGL final publishes one bounded orphan quarantine before restore");

    clear_actions();
    modem_sms_protocol_on_final(
        MODEM_SMS_COMMAND_CMGF_TEXT, true, &mailbox, &s_hooks, 352u);
    check(s_action_count == 3u &&
              s_actions[0].type == MODEM_SMS_ACTION_ARRIVAL_SCAN_COMMIT &&
              s_actions[0].complete &&
              s_actions[1].type == MODEM_SMS_ACTION_PUBLISH_MAILBOX &&
              s_actions[1].ok && s_actions[1].complete &&
              s_actions[2].type == MODEM_SMS_ACTION_COMPLETE &&
              s_actions[2].ok,
          "orphan quarantine leaves the authoritative mailbox scan complete");

    reset_fixture();
    mailbox = inbox_request();
    check(modem_sms_protocol_begin(&mailbox, &s_hooks, 360u),
          "corrupt mailbox fixture begins");
    clear_actions();
    check(modem_sms_protocol_parse_line(
              MODEM_SMS_COMMAND_CMGL, "+CMGL: 7,0,\"\",21",
              false, false, &mailbox, &s_hooks) &&
              modem_sms_protocol_parse_line(
                  MODEM_SMS_COMMAND_CMGL, "NOT-A-PDU", false, false,
                  &mailbox, &s_hooks) &&
              s_action_count == 1u &&
              s_actions[0].type == MODEM_SMS_ACTION_APPEND_MAILBOX &&
              s_actions[0].record.quarantined &&
              s_actions[0].record.indices[0] == 7u &&
              !modem_sms_state_scan_incomplete(),
          "corrupt stored PDU becomes one non-poisoning Data-message row");
    clear_actions();
    modem_sms_protocol_on_final(
        MODEM_SMS_COMMAND_CMGL, true, &mailbox, &s_hooks, 361u);
    clear_actions();
    modem_sms_protocol_on_final(
        MODEM_SMS_COMMAND_CMGF_TEXT, true, &mailbox, &s_hooks, 362u);
    check(s_action_count == 3u &&
              s_actions[1].type == MODEM_SMS_ACTION_PUBLISH_MAILBOX &&
              s_actions[1].complete,
          "corrupt-row snapshot settles instead of requesting an immediate retry");
}

static void test_multipart_arrival_protocol_integration(void) {
    reset_fixture();
    check(modem_sms_protocol_track_pending_arrival(10u) &&
              modem_sms_protocol_track_pending_arrival(11u),
          "protocol fixture tracks both multipart storage indications");
    modem_sms_protocol_request_t mailbox = inbox_request();
    check(modem_sms_protocol_begin(&mailbox, &s_hooks, 370u),
          "multipart arrival mailbox scan begins");
    modem_sms_state_arrival_scan_begin(0u);
    clear_actions();

    uint8_t payload[MODEM_SMS_BINARY_CHUNK_MAX + 1u];
    memset(payload, 0x5au, sizeof(payload));
    sms_submit_pdu_t submit = {
        .number = "1234",
        .payload = payload,
        .payload_len = sizeof(payload),
        .dest_port = SMS_CODEC_PICTURE_PORT,
        .source_port = 0u,
        .mode = MODEM_BINARY_SMS_MODE_DCS04_PORT_FIRST,
        .segment = 1u,
        .segment_total = 2u,
        .reference = 0x66u,
    };
    for (uint8_t sequence = 0u; sequence < 2u; sequence++) {
        char pdu[SMS_SUBMIT_PDU_HEX_MAX + 1u];
        uint8_t tpdu_len = 0u;
        char header[40];
        check(sms_submit_pdu_build(
                  &submit, pdu, sizeof(pdu), &tpdu_len),
              "multipart arrival fixture builds a valid picture PDU");
        (void)snprintf(header, sizeof(header), "+CMGL: %u,0,\"\",%u",
                       (unsigned)(10u + sequence), (unsigned)tpdu_len);
        check(modem_sms_protocol_parse_line(
                  MODEM_SMS_COMMAND_CMGL, header, false, false,
                  &mailbox, &s_hooks) &&
                  modem_sms_protocol_parse_line(
                      MODEM_SMS_COMMAND_CMGL, pdu, false, false,
                      &mailbox, &s_hooks),
              "protocol streams one complete multipart mailbox row");
    }
    check(s_action_count == 1u &&
              s_actions[0].type == MODEM_SMS_ACTION_APPEND_MAILBOX &&
              s_actions[0].record.picture &&
              s_actions[0].record.index_count == 2u &&
              s_actions[0].record.indices[0] == 10u &&
              s_actions[0].record.indices[1] == 11u,
          "protocol publishes one logical record for both picture segments");
    uint32_t arrivals = 99u;
    check(modem_sms_state_arrival_scan_commit(
              true, true, 0u, &arrivals) && arrivals == 1u,
          "protocol integration contributes one user notification per picture");
}

static void test_multipart_text_protocol_integration(void) {
    /* Independent GSM 7-bit DELIVER fixtures with UDH 05 00 03 AC 02 01/02.
     * They model the two physical rows observed on the Telit bench without
     * embedding any captured number, timestamp, or message body. */
    static const char first_pdu[] =
        "00400A9151551000000000000000000000001D050003AC0201"
        "A0B56695494D0A8BC7A4131693CD6835DB0D9703";
    static const char second_pdu[] =
        "00400A9151551000000000000000000000002B050003AC0202"
        "C2E231B96C3EA3D3EA35BBED7EC3E3F239BD6EBFE3F37A68AD59655293452711";

    reset_fixture();
    check(modem_sms_protocol_track_pending_arrival(65u) &&
              modem_sms_protocol_track_pending_arrival(66u),
          "text multipart fixture tracks both physical indications");
    modem_sms_protocol_request_t mailbox = inbox_request();
    check(modem_sms_protocol_begin(&mailbox, &s_hooks, 380u),
          "text multipart mailbox scan begins");
    modem_sms_state_arrival_scan_begin(0u);
    clear_actions();

    check(modem_sms_protocol_parse_line(
              MODEM_SMS_COMMAND_CMGL, "+CMGL: 65,0,\"\",44",
              false, false, &mailbox, &s_hooks) &&
              modem_sms_protocol_parse_line(
                  MODEM_SMS_COMMAND_CMGL, first_pdu, false, false,
                  &mailbox, &s_hooks) &&
              modem_sms_protocol_parse_line(
                  MODEM_SMS_COMMAND_CMGL, "+CMGL: 66,0,\"\",56",
                  false, false, &mailbox, &s_hooks) &&
              modem_sms_protocol_parse_line(
                  MODEM_SMS_COMMAND_CMGL, second_pdu, false, false,
                  &mailbox, &s_hooks),
          "protocol decodes both concatenated GSM7 rows");
    check(s_action_count == 1u &&
              s_actions[0].type == MODEM_SMS_ACTION_APPEND_MAILBOX &&
              !s_actions[0].record.picture &&
              !s_actions[0].record.quarantined &&
              s_actions[0].record.index_count == 2u &&
              s_actions[0].record.indices[0] == 65u &&
              s_actions[0].record.indices[1] == 66u,
          "CMGL publishes one logical text row for two physical segments");
    modem_sms_record_t record = s_actions[0].record;
    uint32_t arrivals = 99u;
    check(modem_sms_state_arrival_scan_commit(
              true, true, 0u, &arrivals) && arrivals == 1u,
          "complete text multipart contributes one user notification");

    reset_fixture();
    uint32_t read_id = 0u;
    check(modem_sms_state_reserve_request(
              MODEM_SMS_REQUEST_READ, &read_id),
          "protocol text read reserves its real result channel");
    modem_sms_protocol_request_t read;
    memset(&read, 0, sizeof(read));
    read.request_id = read_id;
    read.operation = MODEM_SMS_PROTOCOL_READ;
    read.indices = record.indices;
    read.index_count = record.index_count;
    read.expected_identity_hash = record.identity_hash;
    read.read_status = mailbox.read_status;
    check(modem_sms_protocol_begin(&read, &s_hooks, 390u),
          "selected multipart text read begins");

    clear_actions();
    modem_sms_protocol_on_final(
        MODEM_SMS_COMMAND_CPMS, true, &read, &s_hooks, 391u);
    clear_actions();
    modem_sms_protocol_resume_after_wake(&read, &s_hooks, 392u);
    check_command(0u, MODEM_SMS_COMMAND_STATUS_CONSUME,
                  "AT+CONSUME", 7000u, false, false,
                  "selected read enters consume-unread policy");
    clear_actions();
    modem_sms_protocol_on_final(
        MODEM_SMS_COMMAND_STATUS_CONSUME, true, &read, &s_hooks, 393u);
    clear_actions();
    modem_sms_protocol_on_final(
        MODEM_SMS_COMMAND_CMGF_PDU, true, &read, &s_hooks, 394u);
    check_command(0u, MODEM_SMS_COMMAND_CMGR, "AT+CMGR=65", 5000u,
                  false, false, "selected read requests segment one");
    clear_actions();
    check(modem_sms_protocol_parse_line(
              MODEM_SMS_COMMAND_CMGR, "+CMGR: 0,\"\",44",
              false, false, &read, &s_hooks) &&
              modem_sms_protocol_parse_line(
                  MODEM_SMS_COMMAND_CMGR, first_pdu, false, false,
                  &read, &s_hooks),
          "selected read decodes segment one");
    modem_sms_protocol_on_final(
        MODEM_SMS_COMMAND_CMGR, true, &read, &s_hooks, 395u);
    check_command(0u, MODEM_SMS_COMMAND_CMGR, "AT+CMGR=66", 5000u,
                  false, false, "selected read advances to segment two");
    clear_actions();
    check(modem_sms_protocol_parse_line(
              MODEM_SMS_COMMAND_CMGR, "+CMGR: 0,\"\",56",
              false, false, &read, &s_hooks) &&
              modem_sms_protocol_parse_line(
                  MODEM_SMS_COMMAND_CMGR, second_pdu, false, false,
                  &read, &s_hooks),
          "selected read decodes segment two");
    modem_sms_protocol_on_final(
        MODEM_SMS_COMMAND_CMGR, true, &read, &s_hooks, 396u);
    check_command(0u, MODEM_SMS_COMMAND_CMGF_TEXT, "AT+CMGF=1", 5000u,
                  false, false, "selected read restores text mode");
    clear_actions();
    modem_sms_protocol_on_final(
        MODEM_SMS_COMMAND_CMGF_TEXT, true, &read, &s_hooks, 397u);
    check_command(0u, MODEM_SMS_COMMAND_STATUS_PRESERVE,
                  "AT+PRESERVE", 7000u, false, false,
                  "selected read restores preserve-unread policy");
    clear_actions();
    modem_sms_protocol_on_final(
        MODEM_SMS_COMMAND_STATUS_PRESERVE, true, &read, &s_hooks, 398u);
    check(s_action_count == 2u &&
              s_actions[0].type == MODEM_SMS_ACTION_PUBLISH_READ &&
              s_actions[0].ok &&
              s_actions[1].type == MODEM_SMS_ACTION_COMPLETE &&
              s_actions[1].ok,
          "selected multipart text reaches one successful terminal");

    bool mismatch = true;
    modem_sms_read_result_t result;
    check(modem_sms_state_selected_publish_result(
              read_id, MODEM_SMS_REQUEST_READ, MODEM_SMS_OUTCOME_OK,
              false, record.identity_hash, &mismatch) && !mismatch &&
              modem_sms_state_pop_read_result(read_id, &result) &&
              strcmp(result.message.text,
                     "P5MULTIBEGIN0123456789"
                     "abcdefghijklmnopqrstuvwxyzP5MULTIEND") == 0,
          "CMGR reassembles and publishes the exact complete text body");
}

static void test_read_preemption_and_delete_sequence(void) {
    reset_fixture();
    static const uint16_t read_indices[] = {8u, 9u};
    modem_sms_protocol_request_t read;
    memset(&read, 0, sizeof(read));
    read.request_id = UINT32_C(0x1007);
    read.operation = MODEM_SMS_PROTOCOL_READ;
    read.indices = read_indices;
    read.index_count = 2u;
    read.expected_identity_hash = UINT32_C(0x12345678);
    check(modem_sms_protocol_begin(&read, &s_hooks, 400u),
          "selected read begins");
    clear_actions();
    modem_sms_protocol_resume_after_wake(&read, &s_hooks, 410u);
    check_command(0u, MODEM_SMS_COMMAND_CMGF_PDU, "AT+CMGF=0", 5000u,
                  false, false, "selected read enters PDU mode");
    s_call_preempt = true;
    clear_actions();
    modem_sms_protocol_on_final(
        MODEM_SMS_COMMAND_CMGF_PDU, true, &read, &s_hooks, 420u);
    check_command(0u, MODEM_SMS_COMMAND_CMGF_TEXT, "AT+CMGF=1", 5000u,
                  false, false,
                  "pending call preempts before the first CMGR command");
    clear_actions();
    modem_sms_protocol_on_final(
        MODEM_SMS_COMMAND_CMGF_TEXT, true, &read, &s_hooks, 430u);
    check(s_action_count == 2u &&
              s_actions[0].type == MODEM_SMS_ACTION_PUBLISH_READ &&
              !s_actions[0].ok &&
              s_actions[1].type == MODEM_SMS_ACTION_COMPLETE &&
              !s_actions[1].ok,
          "preempted read publishes one failed terminal result");

    reset_fixture();
    static const uint16_t delete_indices[] = {4u, 7u};
    modem_sms_protocol_request_t deletion;
    memset(&deletion, 0, sizeof(deletion));
    deletion.request_id = UINT32_C(0x1008);
    deletion.operation = MODEM_SMS_PROTOCOL_DELETE;
    deletion.indices = delete_indices;
    deletion.index_count = 2u;
    check(modem_sms_protocol_begin(&deletion, &s_hooks, 500u),
          "delete begins");
    clear_actions();
    modem_sms_protocol_resume_after_wake(&deletion, &s_hooks, 510u);
    check_command(0u, MODEM_SMS_COMMAND_CMGD, "AT+CMGD=4", 5000u,
                  false, false, "delete starts at the first requested index");
    clear_actions();
    modem_sms_protocol_on_final(
        MODEM_SMS_COMMAND_CMGD, true, &deletion, &s_hooks, 520u);
    check_command(0u, MODEM_SMS_COMMAND_CMGD, "AT+CMGD=7", 5000u,
                  false, false, "delete advances in request order");
    clear_actions();
    modem_sms_protocol_on_final(
        MODEM_SMS_COMMAND_CMGD, false, &deletion, &s_hooks, 530u);
    check(s_action_count == 2u &&
              s_actions[0].type == MODEM_SMS_ACTION_PUBLISH_DELETE &&
              !s_actions[0].ok && !s_actions[0].sim_not_ready &&
              s_actions[1].type == MODEM_SMS_ACTION_COMPLETE &&
              !s_actions[1].ok,
          "multi-delete accumulates failure without misclassifying the SIM");
}

int main(void) {
    test_text_send_and_sent_copy_failure();
    test_binary_prompt_timeout_restore();
    test_binary_cleanup_duplicate_wedge();
    test_send_evidence_boundaries();
    test_mutating_timeout_boundaries();
    test_binary_uncertainty_boundaries();
    test_cancel_evidence();
    test_delete_dispatch_evidence();
    test_cpms_failure_and_mailbox_policy();
    test_orphan_and_corrupt_mailbox_rows_settle();
    test_multipart_arrival_protocol_integration();
    test_multipart_text_protocol_integration();
    test_read_preemption_and_delete_sequence();
    if (s_failures != 0) {
        fprintf(stderr, "test_modem_sms_protocol: %d failure(s)\n",
                s_failures);
        return 1;
    }
    puts("test_modem_sms_protocol: OK");
    return 0;
}

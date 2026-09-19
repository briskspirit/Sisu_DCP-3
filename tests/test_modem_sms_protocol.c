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
    uint16_t index;
} captured_action_t;

static captured_action_t s_actions[CAPTURE_MAX];
static size_t s_action_count;
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
        captured->request_id = action->data.result.request_id;
        captured->request_kind = action->data.result.kind;
        captured->outcome = action->data.result.outcome;
        captured->ok = action->data.result.outcome == MODEM_SMS_OUTCOME_OK;
        break;
    case MODEM_SMS_ACTION_COMPLETE:
        captured->request_id = action->data.complete.request_id;
        captured->request_kind = action->data.complete.kind;
        captured->outcome = action->data.complete.outcome;
        captured->ok = action->data.complete.command_ok;
        break;
    default:
        break;
    }
    return true;
}

static const modem_sms_protocol_hooks_t s_hooks = {
    .emit = capture_emit,
};

static void reset_fixture(void) {
    modem_sms_state_init();
    modem_sms_protocol_init();
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
static void advance_text_to_prompt(modem_sms_protocol_request_t *request, uint32_t now) {
    check(modem_sms_protocol_begin(request, &s_hooks, now), "text begin");
    check_command(0u, MODEM_SMS_COMMAND_CMGS_PROMPT, "AT+CMGS=\"+15551234567\",145",
                  5000u, false, false, "CMGS uses wake-aware command dispatch, without ME selection");
}
static void test_text_transport(void) {
    modem_sms_protocol_request_t r = text_request();
    reset_fixture();
    advance_text_to_prompt(&r, 1u);
    check(!modem_sms_protocol_settings_restore_needed(), "text send does not change receive format");
    clear_actions();
    check(modem_sms_protocol_on_prompt(MODEM_SMS_COMMAND_CMGS_PROMPT, &r, &s_hooks, 2u), "body accepted");
    check(s_action_count == 1u && s_actions[0].body_len == 5u &&
          memcmp(s_actions[0].body, "hello", 5u) == 0, "exact text body");
    clear_actions();
    modem_sms_protocol_on_final(MODEM_SMS_COMMAND_CMGS_FINAL, true, &r, &s_hooks, 3u);
    check(s_action_count == 3u && s_actions[0].type == MODEM_SMS_ACTION_INCREMENT_SENT &&
          s_actions[1].type == MODEM_SMS_ACTION_PUBLISH_SEND &&
          s_actions[1].outcome == MODEM_SMS_OUTCOME_OK &&
          s_actions[2].type == MODEM_SMS_ACTION_COMPLETE, "accepted send completes without CMGW");
    reset_fixture();
    advance_text_to_prompt(&r, 10u);
    clear_actions();
    modem_sms_protocol_on_timeout(MODEM_SMS_COMMAND_CMGS_PROMPT, &r, &s_hooks, 11u);
    check(s_action_count == 3u && s_actions[0].type == MODEM_SMS_ACTION_ABORT_PROMPT &&
          s_actions[1].outcome == MODEM_SMS_OUTCOME_TIMEOUT, "lost prompt is aborted and retry safe");
    reset_fixture();
    advance_text_to_prompt(&r, 20u);
    (void)modem_sms_protocol_on_prompt(MODEM_SMS_COMMAND_CMGS_PROMPT, &r, &s_hooks, 21u);
    clear_actions();
    modem_sms_protocol_on_timeout(MODEM_SMS_COMMAND_CMGS_FINAL, &r, &s_hooks, 22u);
    check(s_action_count == 2u && s_actions[0].outcome == MODEM_SMS_OUTCOME_UNCERTAIN,
          "lost final after body cannot authorize retry");
}
static void test_dispatch_evidence(void) {
    uint8_t byte = 0u;
    modem_sms_protocol_request_t r = {.request_id=3u, .operation=MODEM_SMS_PROTOCOL_SEND_BINARY,
        .number="123", .binary=&byte, .binary_len=1u};
    reset_fixture(); s_auto_dispatch_commands = false;
    check(modem_sms_protocol_begin(&r, &s_hooks, 1u) && !modem_sms_protocol_pdu_mode_possible(),
          "queued mode command is not dispatch evidence");
    modem_sms_protocol_command_dispatched(MODEM_SMS_COMMAND_CMGF_PDU);
    check(modem_sms_protocol_pdu_mode_possible(), "dispatch establishes possible PDU mode");
    modem_sms_protocol_on_timeout(MODEM_SMS_COMMAND_CMGF_PDU, &r, &s_hooks, 2u);
    modem_sms_protocol_on_timeout(MODEM_SMS_COMMAND_CMGF_TEXT, &r, &s_hooks, 3u);
    check(modem_sms_protocol_pdu_mode_possible(), "lost restore final retains repair obligation");
    modem_sms_protocol_on_final(MODEM_SMS_COMMAND_CMGF_TEXT, true, &r, &s_hooks, 4u);
    check(!modem_sms_protocol_settings_restore_needed(), "confirmed restore clears obligation");
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

static void test_picture_text_send(void) {
    reset_fixture();
    static const uint8_t payload[] = {0x10u, 0x20u, 0x30u};
    modem_sms_protocol_request_t request = {
        .request_id = 99u, .operation = MODEM_SMS_PROTOCOL_SEND_BINARY,
        .number = "5550100", .binary = payload, .binary_len = sizeof(payload),
        .dest_port = 0x158au, .source_port = 0u,
        .binary_mode = MODEM_BINARY_SMS_MODE_DCS04_PORT_FIRST,
        .picture_text_mode = true,
    };
    check(modem_sms_protocol_begin(&request, &s_hooks, 100u), "picture text send begins");
    check_command(0u, MODEM_SMS_COMMAND_PICTURE_TEXT_SETUP, "AT+CMGF=1;+CSMP=81,167,0,4",
                  5000u, false, false, "picture send keeps native reception in text mode");
    check(modem_sms_protocol_text_parameters_dirty(), "dispatched CSMP needs cleanup");
    clear_actions();
    modem_sms_protocol_on_final(MODEM_SMS_COMMAND_PICTURE_TEXT_SETUP, true, &request, &s_hooks, 110u);
    check(strcmp(s_actions[0].command, "AT+CMGS=\"5550100\"") == 0,
          "picture opens addressed text-mode prompt");
    clear_actions();
    check(modem_sms_protocol_on_prompt(MODEM_SMS_COMMAND_CMGS_PROMPT, &request, &s_hooks, 120u),
          "picture text prompt accepted");
    check(s_actions[0].body_len == 20u && memcmp(s_actions[0].body, "060504158A0000102030", 20u) == 0,
          "picture text body contains exact port UDH and octets, not a whole PDU");
    clear_actions();
    modem_sms_protocol_on_final(MODEM_SMS_COMMAND_CMGS_FINAL, true, &request, &s_hooks, 130u);
    check_command(0u, MODEM_SMS_COMMAND_CMGF_TEXT, "AT+CMGF=1;+CSMP=17,167,0,0",
                  5000u, false, false, "picture restores ordinary text parameters");
    modem_sms_protocol_on_final(MODEM_SMS_COMMAND_CMGF_TEXT, false, &request, &s_hooks, 140u);
    check(modem_sms_protocol_text_parameters_dirty() && modem_sms_protocol_settings_restore_needed() &&
              !modem_sms_protocol_pdu_mode_possible(),
          "failed cleanup remains repairable after an accepted send");

    reset_fixture();
    check(modem_sms_protocol_begin(&request, &s_hooks, 200u), "picture setup timeout fixture");
    clear_actions();
    modem_sms_protocol_on_timeout(MODEM_SMS_COMMAND_PICTURE_TEXT_SETUP, &request, &s_hooks, 210u);
    check_command(0u, MODEM_SMS_COMMAND_CMGF_TEXT, "AT+CMGF=1;+CSMP=17,167,0,0",
                  5000u, false, false, "partial compound setup is cleaned up after timeout");
    modem_sms_protocol_on_final(MODEM_SMS_COMMAND_CMGF_TEXT, true, &request, &s_hooks, 220u);
    check(!modem_sms_protocol_settings_restore_needed(), "confirmed cleanup clears repair obligation");
}
int main(void) {
    test_text_transport();
    test_dispatch_evidence();
    test_binary_prompt_timeout_restore();
    test_binary_cleanup_duplicate_wedge();
    test_binary_uncertainty_boundaries();
    test_cancel_evidence();
    test_picture_text_send();
    return s_failures != 0;
}

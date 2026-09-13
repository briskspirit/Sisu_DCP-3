#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "services/modem_phonebook_protocol_internal.h"

#define CAPTURE_MAX 8u

typedef struct {
    modem_phonebook_action_type_t type;
    modem_phonebook_command_kind_t command_kind;
    char command[128];
    uint32_t timeout_ms;
    uint32_t now_ms;
    modem_phonebook_entry_t entry;
    uint32_t request_id;
    modem_phonebook_op_t operation;
    bool publish;
    modem_phonebook_outcome_t outcome;
} captured_action_t;

static captured_action_t s_actions[CAPTURE_MAX];
static size_t s_action_count;
static uint32_t s_transport_counter;
static int s_fail_action = -1;
static int s_failures;
static uint32_t s_next_request_id;

static void check(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        s_failures++;
    }
}

static bool capture_emit(const modem_phonebook_protocol_action_t *action) {
    if (action == NULL || s_action_count >= CAPTURE_MAX) {
        return false;
    }
    captured_action_t *captured = &s_actions[s_action_count++];
    memset(captured, 0, sizeof(*captured));
    captured->type = action->type;
    switch (action->type) {
    case MODEM_PHONEBOOK_ACTION_COMMAND:
        captured->command_kind = action->data.command.kind;
        captured->timeout_ms = action->data.command.timeout_ms;
        captured->now_ms = action->data.command.now_ms;
        snprintf(captured->command, sizeof(captured->command), "%s",
                 action->data.command.command != NULL
                     ? action->data.command.command : "");
        break;
    case MODEM_PHONEBOOK_ACTION_APPEND_ENTRY:
        captured->entry = action->data.entry;
        break;
    case MODEM_PHONEBOOK_ACTION_COMPLETE:
        captured->request_id = action->data.complete.request_id;
        captured->operation = action->data.complete.operation;
        captured->outcome = action->data.complete.outcome;
        break;
    case MODEM_PHONEBOOK_ACTION_FINISH_REFRESH:
        captured->publish = action->data.refresh.publish;
        break;
    case MODEM_PHONEBOOK_ACTION_BEGIN_REFRESH:
        break;
    }
    return (int)action->type != s_fail_action;
}

static uint32_t transport_counter(void) {
    return s_transport_counter;
}

static const modem_phonebook_protocol_hooks_t s_hooks = {
    .emit = capture_emit,
    .transport_counter = transport_counter,
};

static void clear_actions(void) {
    memset(s_actions, 0, sizeof(s_actions));
    s_action_count = 0u;
    s_fail_action = -1;
}

static bool command_is(size_t position,
                       modem_phonebook_command_kind_t kind,
                       const char *command, uint32_t timeout_ms,
                       uint32_t now_ms) {
    return position < s_action_count &&
           s_actions[position].type == MODEM_PHONEBOOK_ACTION_COMMAND &&
           s_actions[position].command_kind == kind &&
           strcmp(s_actions[position].command, command) == 0 &&
           s_actions[position].timeout_ms == timeout_ms &&
           s_actions[position].now_ms == now_ms;
}

static modem_phonebook_protocol_request_t request_for(
    modem_phonebook_op_t operation, uint16_t index, const char *name,
    const char *number) {
    modem_phonebook_protocol_request_t request = {
        .request_id = ++s_next_request_id,
        .operation = operation,
        .index = index,
        .name = name,
        .number = number,
    };
    return request;
}

static void test_admission_and_storage_selection(void) {
    modem_phonebook_protocol_request_t request = request_for(
        MODEM_PHONEBOOK_OP_LIST, 0u, "", "");
    clear_actions();
    check(modem_phonebook_protocol_begin(&request, &s_hooks, 17u) &&
              s_action_count == 1u &&
              command_is(0u, MODEM_PHONEBOOK_COMMAND_CPBS,
                         "AT+CPBS=\"ME\"", 5000u, 17u),
          "every operation begins by selecting the exact ME store");

    clear_actions();
    request = request_for(MODEM_PHONEBOOK_OP_ADD, 0u, "A", "");
    check(!modem_phonebook_protocol_begin(&request, &s_hooks, 1u) &&
              s_action_count == 0u,
          "add rejects an empty number before emitting a command");
    request = request_for(MODEM_PHONEBOOK_OP_UPDATE, 0u, "A", "123");
    check(!modem_phonebook_protocol_begin(&request, &s_hooks, 1u),
          "update rejects index zero");
    request = request_for(MODEM_PHONEBOOK_OP_DELETE, 0u, "", "");
    check(!modem_phonebook_protocol_begin(&request, &s_hooks, 1u),
          "delete rejects index zero");
    request = request_for(MODEM_PHONEBOOK_OP_UPDATE,
                          MODEM_PHONEBOOK_LAST_INDEX + 1u, "A", "123");
    check(!modem_phonebook_protocol_begin(&request, &s_hooks, 1u),
          "update rejects indexes above the verified ME domain");
    request = request_for(MODEM_PHONEBOOK_OP_DELETE,
                          MODEM_PHONEBOOK_LAST_INDEX + 1u, "", "");
    check(!modem_phonebook_protocol_begin(&request, &s_hooks, 1u),
          "delete rejects indexes above the verified ME domain");
    request = request_for(MODEM_PHONEBOOK_OP_NONE, 0u, "", "");
    check(!modem_phonebook_protocol_begin(&request, &s_hooks, 1u) &&
              !modem_phonebook_protocol_begin(NULL, &s_hooks, 1u) &&
              !modem_phonebook_protocol_begin(&request, NULL, 1u),
          "invalid operation, request, and hook surfaces fail closed");
    request = request_for(MODEM_PHONEBOOK_OP_LIST, 0u, "", "");
    request.request_id = 0u;
    check(!modem_phonebook_protocol_begin(&request, &s_hooks, 1u),
          "an uncorrelated request cannot enter the protocol");
}

static void test_list_sequence_and_parser(void) {
    modem_phonebook_protocol_request_t request = request_for(
        MODEM_PHONEBOOK_OP_LIST, 0u, "", "");

    clear_actions();
    modem_phonebook_protocol_on_final(
        MODEM_PHONEBOOK_COMMAND_CPBS, true, &request, &s_hooks, 30u);
    check(s_action_count == 2u &&
              s_actions[0].type == MODEM_PHONEBOOK_ACTION_BEGIN_REFRESH &&
              command_is(1u, MODEM_PHONEBOOK_COMMAND_CPBR,
                         "AT+CPBR=1,500", 15000u, 30u),
          "successful store selection clears cache before the bounded read");

    clear_actions();
    check(modem_phonebook_protocol_parse_line(
              MODEM_PHONEBOOK_COMMAND_CPBS, "+CPBS: \"ME\",2,500",
              &s_hooks) &&
              s_action_count == 0u &&
              !modem_phonebook_protocol_parse_line(
                  MODEM_PHONEBOOK_COMMAND_CPBS, "+OTHER: 1", &s_hooks),
          "CPBS consumes only its solicited response prefix");

    clear_actions();
    check(modem_phonebook_protocol_parse_line(
              MODEM_PHONEBOOK_COMMAND_CPBR,
              "+CPBR: 7,\"+15551234567\",145,\"Ada\"", &s_hooks) &&
              s_action_count == 1u &&
              s_actions[0].type == MODEM_PHONEBOOK_ACTION_APPEND_ENTRY &&
              s_actions[0].entry.index == 7u &&
              strcmp(s_actions[0].entry.number, "+15551234567") == 0 &&
              strcmp(s_actions[0].entry.name, "Ada") == 0,
          "CPBR parser emits the exact typed row");

    clear_actions();
    check(modem_phonebook_protocol_parse_line(
              MODEM_PHONEBOOK_COMMAND_CPBR,
              "+CPBR: 8,\"5550008\",129", &s_hooks) &&
              s_action_count == 1u && s_actions[0].entry.index == 8u &&
              strcmp(s_actions[0].entry.number, "5550008") == 0 &&
              s_actions[0].entry.name[0] == '\0',
          "CPBR row without an alpha field keeps an empty name");

    check(!modem_phonebook_protocol_parse_line(
              MODEM_PHONEBOOK_COMMAND_CPBR, "+CMTI: \"ME\",4", &s_hooks) &&
              !modem_phonebook_protocol_parse_line(
                  MODEM_PHONEBOOK_COMMAND_CPBR, NULL, &s_hooks),
          "unrelated and null lines remain available to the outer router");

    clear_actions();
    check(modem_phonebook_protocol_parse_line(
              MODEM_PHONEBOOK_COMMAND_CPBR,
              "+CPBR: 250,\"1234567890123456789012345678901234567890\",129,"
              "\"ABCDEFGHIJKLMNOPQRSTUVWXYZABCDEFGHIJKLMN\"",
              &s_hooks) &&
              s_action_count == 1u &&
              strlen(s_actions[0].entry.number) == MODEM_PHONE_MAX &&
              strlen(s_actions[0].entry.name) == MODEM_PHONEBOOK_NAME_MAX,
          "oversized modem fields retain the established bounded truncation");

    clear_actions();
    modem_phonebook_protocol_on_final(
        MODEM_PHONEBOOK_COMMAND_CPBR, true, &request, &s_hooks, 40u);
    check(s_action_count == 2u &&
              s_actions[0].type == MODEM_PHONEBOOK_ACTION_FINISH_REFRESH &&
              s_actions[0].publish &&
              s_actions[1].type == MODEM_PHONEBOOK_ACTION_COMPLETE &&
              s_actions[1].request_id == request.request_id &&
              s_actions[1].operation == MODEM_PHONEBOOK_OP_LIST &&
              s_actions[1].outcome == MODEM_PHONEBOOK_OUTCOME_OK,
          "CPBR final publishes successful list completion");

    clear_actions();
    modem_phonebook_protocol_on_final(
        MODEM_PHONEBOOK_COMMAND_CPBS, true, &request, &s_hooks, 41u);
    clear_actions();
    check(modem_phonebook_protocol_parse_line(
              MODEM_PHONEBOOK_COMMAND_CPBR, "+CPBR: malformed", &s_hooks) &&
              s_action_count == 0u,
          "malformed solicited CPBR row is consumed without cache mutation");
    modem_phonebook_protocol_on_final(
        MODEM_PHONEBOOK_COMMAND_CPBR, true, &request, &s_hooks, 42u);
    check(s_action_count == 2u &&
              s_actions[0].type == MODEM_PHONEBOOK_ACTION_FINISH_REFRESH &&
              !s_actions[0].publish &&
              s_actions[1].type == MODEM_PHONEBOOK_ACTION_COMPLETE &&
              s_actions[1].request_id == request.request_id &&
              s_actions[1].outcome == MODEM_PHONEBOOK_OUTCOME_ERROR,
          "malformed row prevents a partial list from publishing success");

    clear_actions();
    modem_phonebook_protocol_on_final(
        MODEM_PHONEBOOK_COMMAND_CPBS, false, &request, &s_hooks, 43u);
    check(s_action_count == 1u &&
              s_actions[0].type == MODEM_PHONEBOOK_ACTION_COMPLETE &&
              s_actions[0].request_id == request.request_id &&
              s_actions[0].operation == MODEM_PHONEBOOK_OP_LIST &&
              s_actions[0].outcome == MODEM_PHONEBOOK_OUTCOME_ERROR,
          "CPBS failure completes without clearing the previous cache");
}

static void start_list_read(const modem_phonebook_protocol_request_t *request,
                            uint32_t now_ms) {
    clear_actions();
    modem_phonebook_protocol_on_final(
        MODEM_PHONEBOOK_COMMAND_CPBS, true, request, &s_hooks, now_ms);
    check(s_action_count == 2u &&
              s_actions[0].type == MODEM_PHONEBOOK_ACTION_BEGIN_REFRESH &&
              command_is(1u, MODEM_PHONEBOOK_COMMAND_CPBR,
                         "AT+CPBR=1,500", 15000u, now_ms),
          "integrity fixture starts a complete ME refresh");
    clear_actions();
}

static void check_failed_read_final(
    const modem_phonebook_protocol_request_t *request,
    const char *message) {
    clear_actions();
    modem_phonebook_protocol_on_final(
        MODEM_PHONEBOOK_COMMAND_CPBR, true, request, &s_hooks, 90u);
    check(s_action_count == 2u &&
              s_actions[0].type == MODEM_PHONEBOOK_ACTION_FINISH_REFRESH &&
              !s_actions[0].publish &&
              s_actions[1].type == MODEM_PHONEBOOK_ACTION_COMPLETE &&
              s_actions[1].request_id == request->request_id &&
              s_actions[1].outcome == MODEM_PHONEBOOK_OUTCOME_ERROR,
          message);
}

static void test_list_integrity_faults(void) {
    modem_phonebook_protocol_request_t request = request_for(
        MODEM_PHONEBOOK_OP_LIST, 0u, "", "");

    start_list_read(&request, 60u);
    check(modem_phonebook_protocol_parse_line(
              MODEM_PHONEBOOK_COMMAND_CPBR,
              "+CPBR: 500,\"5550500\",129,\"Last\"", &s_hooks),
          "the final verified ME index parses without narrowing");
    s_transport_counter++;
    check_failed_read_final(
        &request, "transport-integrity change rejects the staged snapshot");

    start_list_read(&request, 61u);
    s_fail_action = MODEM_PHONEBOOK_ACTION_APPEND_ENTRY;
    check(modem_phonebook_protocol_parse_line(
              MODEM_PHONEBOOK_COMMAND_CPBR,
              "+CPBR: 300,\"5550300\",129,\"High\"", &s_hooks),
          "append-failure row remains owned by the CPBR command");
    s_fail_action = -1;
    check_failed_read_final(
        &request, "cache-append failure cannot publish a partial list");

    start_list_read(&request, 62u);
    check(modem_phonebook_protocol_parse_line(
              MODEM_PHONEBOOK_COMMAND_CPBR,
              "+CPBR: 301,\"5550301\",129,\"Later\"", &s_hooks) &&
              modem_phonebook_protocol_parse_line(
                  MODEM_PHONEBOOK_COMMAND_CPBR,
                  "+CPBR: 300,\"5550300\",129,\"Earlier\"", &s_hooks),
          "out-of-order rows remain consumed by the active command");
    check_failed_read_final(
        &request, "duplicate or descending indexes invalidate the snapshot");

    start_list_read(&request, 63u);
    check(modem_phonebook_protocol_parse_line(
              MODEM_PHONEBOOK_COMMAND_CPBR,
              "+CPBR: 501,\"5550501\",129,\"Outside\"", &s_hooks),
          "out-of-domain row remains consumed by the active command");
    check_failed_read_final(
        &request, "an index outside the verified ME domain fails closed");

    start_list_read(&request, 64u);
    modem_phonebook_protocol_line_dropped();
    check_failed_read_final(
        &request, "line-framer loss invalidates the active phonebook read");
}

static void test_write_commands_and_readback(void) {
    modem_phonebook_protocol_request_t request = request_for(
        MODEM_PHONEBOOK_OP_ADD, 0u, "O\"Neil", "+15550000001");
    clear_actions();
    modem_phonebook_protocol_on_final(
        MODEM_PHONEBOOK_COMMAND_CPBS, true, &request, &s_hooks, 50u);
    check(s_action_count == 1u &&
              command_is(0u, MODEM_PHONEBOOK_COMMAND_CPBW,
                         "AT+CPBW=,\"+15550000001\",145,\"O'Neil\"",
                         10000u, 50u),
          "international add uses TOA 145 and sanitizes embedded quotes");

    request = request_for(MODEM_PHONEBOOK_OP_UPDATE, 12u, "Bob", "5550012");
    clear_actions();
    modem_phonebook_protocol_on_final(
        MODEM_PHONEBOOK_COMMAND_CPBS, true, &request, &s_hooks, 51u);
    check(s_action_count == 1u &&
              command_is(0u, MODEM_PHONEBOOK_COMMAND_CPBW,
                         "AT+CPBW=12,\"5550012\",129,\"Bob\"", 10000u,
                         51u),
          "national update uses its index and TOA 129");

    request = request_for(MODEM_PHONEBOOK_OP_DELETE, 12u, "", "");
    clear_actions();
    modem_phonebook_protocol_on_final(
        MODEM_PHONEBOOK_COMMAND_CPBS, true, &request, &s_hooks, 52u);
    check(s_action_count == 1u &&
              command_is(0u, MODEM_PHONEBOOK_COMMAND_CPBW,
                         "AT+CPBW=12", 10000u, 52u),
          "delete emits the index-only CPBW command");

    clear_actions();
    modem_phonebook_protocol_on_final(
        MODEM_PHONEBOOK_COMMAND_CPBW, true, &request, &s_hooks, 53u);
    check(s_action_count == 2u &&
              s_actions[0].type == MODEM_PHONEBOOK_ACTION_BEGIN_REFRESH &&
              command_is(1u, MODEM_PHONEBOOK_COMMAND_CPBR,
                         "AT+CPBR=1,500", 15000u, 53u),
          "successful write refreshes the complete cache before completion");

    clear_actions();
    modem_phonebook_protocol_on_final(
        MODEM_PHONEBOOK_COMMAND_CPBW, false, &request, &s_hooks, 54u);
    check(s_action_count == 1u &&
              s_actions[0].type == MODEM_PHONEBOOK_ACTION_COMPLETE &&
              s_actions[0].request_id == request.request_id &&
              s_actions[0].operation == MODEM_PHONEBOOK_OP_DELETE &&
              s_actions[0].outcome == MODEM_PHONEBOOK_OUTCOME_ERROR,
          "failed write publishes the original operation kind and failure");
}

static void test_command_failures_timeouts_and_cancel(void) {
    modem_phonebook_protocol_request_t list = request_for(
        MODEM_PHONEBOOK_OP_LIST, 0u, "", "");
    modem_phonebook_protocol_request_t add = request_for(
        MODEM_PHONEBOOK_OP_ADD, 0u, "Ada", "+15550000001");

    clear_actions();
    s_fail_action = MODEM_PHONEBOOK_ACTION_COMMAND;
    modem_phonebook_protocol_on_final(
        MODEM_PHONEBOOK_COMMAND_CPBS, true, &add, &s_hooks, 100u);
    check(s_action_count == 2u &&
              s_actions[0].type == MODEM_PHONEBOOK_ACTION_COMMAND &&
              s_actions[1].type == MODEM_PHONEBOOK_ACTION_COMPLETE &&
              s_actions[1].request_id == add.request_id &&
              s_actions[1].outcome == MODEM_PHONEBOOK_OUTCOME_ERROR,
          "a rejected CPBW dispatch terminates the accepted write");

    clear_actions();
    s_fail_action = MODEM_PHONEBOOK_ACTION_COMMAND;
    modem_phonebook_protocol_on_final(
        MODEM_PHONEBOOK_COMMAND_CPBS, true, &list, &s_hooks, 101u);
    check(s_action_count == 4u &&
              s_actions[0].type == MODEM_PHONEBOOK_ACTION_BEGIN_REFRESH &&
              s_actions[1].type == MODEM_PHONEBOOK_ACTION_COMMAND &&
              s_actions[2].type == MODEM_PHONEBOOK_ACTION_FINISH_REFRESH &&
              !s_actions[2].publish &&
              s_actions[3].type == MODEM_PHONEBOOK_ACTION_COMPLETE &&
              s_actions[3].request_id == list.request_id &&
              s_actions[3].outcome == MODEM_PHONEBOOK_OUTCOME_ERROR,
          "a rejected CPBR dispatch discards staging and terminates the list");

    const modem_phonebook_command_kind_t direct_timeouts[] = {
        MODEM_PHONEBOOK_COMMAND_CPBS,
        MODEM_PHONEBOOK_COMMAND_CPBW,
    };
    for (size_t i = 0u;
         i < sizeof(direct_timeouts) / sizeof(direct_timeouts[0]); i++) {
        clear_actions();
        modem_phonebook_protocol_on_timeout(
            direct_timeouts[i], &add, &s_hooks);
        check(s_action_count == 1u &&
                  s_actions[0].type == MODEM_PHONEBOOK_ACTION_COMPLETE &&
                  s_actions[0].request_id == add.request_id &&
                  s_actions[0].outcome == MODEM_PHONEBOOK_OUTCOME_TIMEOUT,
              "CPBS/CPBW timeout has one correlated terminal outcome");
    }

    start_list_read(&list, 102u);
    modem_phonebook_protocol_on_timeout(
        MODEM_PHONEBOOK_COMMAND_CPBR, &list, &s_hooks);
    check(s_action_count == 2u &&
              s_actions[0].type == MODEM_PHONEBOOK_ACTION_FINISH_REFRESH &&
              !s_actions[0].publish &&
              s_actions[1].type == MODEM_PHONEBOOK_ACTION_COMPLETE &&
              s_actions[1].request_id == list.request_id &&
              s_actions[1].outcome == MODEM_PHONEBOOK_OUTCOME_TIMEOUT,
          "CPBR timeout discards staging before its correlated terminal");

    start_list_read(&list, 103u);
    modem_phonebook_protocol_cancel(&s_hooks);
    check(s_action_count == 1u &&
              s_actions[0].type == MODEM_PHONEBOOK_ACTION_FINISH_REFRESH &&
              !s_actions[0].publish,
          "session cancellation discards an in-flight read without inventing a terminal");
    clear_actions();
    modem_phonebook_protocol_cancel(&s_hooks);
    check(s_action_count == 0u,
          "phonebook protocol cancellation is idempotent");
}

int main(void) {
    test_admission_and_storage_selection();
    test_list_sequence_and_parser();
    test_list_integrity_faults();
    test_write_commands_and_readback();
    test_command_failures_timeouts_and_cancel();
    if (s_failures == 0) {
        printf("test_modem_phonebook_protocol: OK\n");
    }
    return s_failures != 0;
}

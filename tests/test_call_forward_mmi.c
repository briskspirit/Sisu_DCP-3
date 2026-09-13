#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "services/call_forward_mmi.h"

static int failures;

static void check(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        failures++;
    }
}

static call_forward_request_t parse_valid(const char *text) {
    call_forward_request_t request;
    memset(&request, 0xa5, sizeof(request));
    check(call_forward_mmi_parse(text, &request) == CALL_FORWARD_MMI_VALID,
          text);
    return request;
}

int main(void) {
    call_forward_request_t request = parse_valid("**21*+15551234567#");
    check(request.reason == CALL_FORWARD_REASON_UNCONDITIONAL &&
              request.action == CALL_FORWARD_ACTION_REGISTER &&
              request.has_number &&
              strcmp(request.number, "+15551234567") == 0 &&
              !request.has_delay,
          "unconditional registration");

    request = parse_valid("**61*5551212**20#");
    check(request.reason == CALL_FORWARD_REASON_NO_REPLY &&
              request.has_delay && request.delay_seconds == 20u,
          "no-reply shorthand delay");
    request = parse_valid("**61*5551212*11*30#");
    check(request.has_delay && request.delay_seconds == 30u,
          "voice basic-service delay");
    request = parse_valid("**004*5551212**15#");
    check(request.reason == CALL_FORWARD_REASON_ALL_CONDITIONAL &&
              request.delay_seconds == 15u,
          "all-conditional registration");
    request = parse_valid("##002#");
    check(request.reason == CALL_FORWARD_REASON_ALL &&
              request.action == CALL_FORWARD_ACTION_ERASE,
          "cancel all");
    request = parse_valid("#002#");
    check(request.reason == CALL_FORWARD_REASON_ALL &&
              request.action == CALL_FORWARD_ACTION_DISABLE,
          "deactivate all");
    request = parse_valid("*002#");
    check(request.reason == CALL_FORWARD_REASON_ALL &&
              request.action == CALL_FORWARD_ACTION_ENABLE &&
              !request.has_number,
          "activate all registered destinations");
    request = parse_valid("*002*5551212#");
    check(request.reason == CALL_FORWARD_REASON_ALL &&
              request.action == CALL_FORWARD_ACTION_ENABLE &&
              request.has_number && strcmp(request.number, "5551212") == 0,
          "activate all with destination");
    request = parse_valid("**002*+15551234567#");
    check(request.reason == CALL_FORWARD_REASON_ALL &&
              request.action == CALL_FORWARD_ACTION_REGISTER &&
              request.has_number &&
              strcmp(request.number, "+15551234567") == 0,
          "register all with destination");
    request = parse_valid("*#67#");
    check(request.reason == CALL_FORWARD_REASON_BUSY &&
              request.action == CALL_FORWARD_ACTION_QUERY,
          "status query");
    request = parse_valid("*21#");
    check(request.action == CALL_FORWARD_ACTION_ENABLE,
          "enable registered destination");
    request = parse_valid("*21**11#");
    check(request.action == CALL_FORWARD_ACTION_ENABLE &&
              !request.has_number && !request.has_delay,
          "voice-qualified activation reuses registered destination");
    request = parse_valid("*61**11*20#");
    check(request.action == CALL_FORWARD_ACTION_ENABLE &&
              !request.has_number && request.has_delay &&
              request.delay_seconds == 20u,
          "voice-qualified activation updates no-reply delay");
    request = parse_valid("*61***25#");
    check(!request.has_number && request.has_delay &&
              request.delay_seconds == 25u,
          "activation delay accepts omitted basic-service field");
    request = parse_valid("*21*5551212#");
    check(request.action == CALL_FORWARD_ACTION_ENABLE &&
              request.has_number && strcmp(request.number, "5551212") == 0,
          "enable with a new destination");
    request = parse_valid("*61*5551212**25#");
    check(request.action == CALL_FORWARD_ACTION_ENABLE &&
              request.has_delay && request.delay_seconds == 25u,
          "enable no-reply with destination and delay");
    request = parse_valid("#62#");
    check(request.action == CALL_FORWARD_ACTION_DISABLE,
          "disable without erasure");
    request = parse_valid("##67#");
    check(request.action == CALL_FORWARD_ACTION_ERASE,
          "erase one condition");
    request = parse_valid("*#21**11#");
    check(request.action == CALL_FORWARD_ACTION_QUERY,
          "voice-qualified status query");

    check(call_forward_mmi_parse("*#06#", &request) ==
              CALL_FORWARD_MMI_NOT_MATCHED,
          "IMEI code remains available to service-code handler");
    check(call_forward_mmi_parse("#3370#", &request) ==
              CALL_FORWARD_MMI_NOT_MATCHED,
          "codec code remains available to service-code handler");
    check(call_forward_mmi_parse("**21#", &request) ==
              CALL_FORWARD_MMI_INVALID,
          "registration requires a destination");
    check(call_forward_mmi_parse("**21**11#", &request) ==
              CALL_FORWARD_MMI_INVALID,
          "voice qualification cannot replace a registration destination");
    check(call_forward_mmi_parse("**61*5551212*13*20#", &request) ==
              CALL_FORWARD_MMI_INVALID,
          "non-voice basic service rejected");
    check(call_forward_mmi_parse("**61*5551212**17#", &request) ==
              CALL_FORWARD_MMI_INVALID,
          "non-Nokia delay rejected");
    check(call_forward_mmi_parse("**61*5551212**4294967316#", &request) ==
              CALL_FORWARD_MMI_INVALID,
          "oversized decimal fields cannot wrap into a valid delay");
    check(call_forward_mmi_parse("**21*5551212**#", &request) ==
              CALL_FORWARD_MMI_INVALID,
          "empty trailing supplementary field rejected");
    check(call_forward_mmi_parse("*#21*11#", &request) ==
              CALL_FORWARD_MMI_INVALID,
          "basic service must occupy SIB, not SIA");
    check(call_forward_mmi_parse("*#004#", &request) ==
              CALL_FORWARD_MMI_INVALID,
          "combined status is unsupported");
    check(call_forward_mmi_parse("*#002#", &request) ==
              CALL_FORWARD_MMI_INVALID,
          "all-call status is unsupported by the modem");
    check(call_forward_mmi_parse("*004*5551212**15#", &request) ==
              CALL_FORWARD_MMI_INVALID,
          "all-conditional timer requires registration semantics");
    check(call_forward_mmi_parse("##002*11#", &request) ==
              CALL_FORWARD_MMI_INVALID,
          "cancel-all remains exact Nokia code");
    check(call_forward_mmi_parse("**21*12p3#", &request) ==
              CALL_FORWARD_MMI_INVALID,
          "dial modifiers rejected at the SS boundary");
    check(call_forward_mmi_parse("*21##", &request) ==
              CALL_FORWARD_MMI_INVALID,
          "recognized forwarding code with an extra final is consumed");
    check(call_forward_mmi_parse("*21+123#", &request) ==
              CALL_FORWARD_MMI_INVALID,
          "recognized forwarding code with malformed punctuation is consumed");
    check(call_forward_mmi_parse("*210#", &request) ==
              CALL_FORWARD_MMI_NOT_MATCHED,
          "a longer unrelated decimal service code remains available");

    if (failures != 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    puts("call-forward MMI tests passed");
    return 0;
}

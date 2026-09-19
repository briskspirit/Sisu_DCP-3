#include <stdio.h>
#include <string.h>
#include "services/modem_sms_state_internal.h"
#include "services/sms_submit_codec.h"
#include "services/sms_picture_codec.h"
static int s_failures;
static void check(bool condition, const char *message) {
    if (!condition) { fprintf(stderr, "FAIL: %s\n", message); s_failures++; }
}
static void test_terminal_result_latches(void) {
    modem_sms_state_init();
    uint32_t id = 0u, rejected = 99u;
    check(modem_sms_state_reserve_request(MODEM_SMS_REQUEST_SEND_TEXT, &id) && id != 0u,
          "send reserves an identity");
    check(!modem_sms_state_reserve_request(MODEM_SMS_REQUEST_SEND_BINARY, &rejected) && rejected == 0u,
          "shared send channel is single flight");
    modem_sms_send_result_t result;
    check(!modem_sms_state_publish_send_result(id + 1u, MODEM_SMS_REQUEST_SEND_TEXT, MODEM_SMS_OUTCOME_OK) &&
          !modem_sms_state_publish_send_result(id, MODEM_SMS_REQUEST_SEND_BINARY, MODEM_SMS_OUTCOME_OK),
          "foreign token or kind cannot complete request");
    check(modem_sms_state_publish_send_result(id, MODEM_SMS_REQUEST_SEND_TEXT, MODEM_SMS_OUTCOME_OK) &&
          !modem_sms_state_publish_send_result(id, MODEM_SMS_REQUEST_SEND_TEXT, MODEM_SMS_OUTCOME_ERROR),
          "one authoritative terminal");
    check(!modem_sms_state_pop_send_result(0u, &result) &&
          !modem_sms_state_pop_send_result(id + 1u, &result) &&
          !modem_sms_state_reserve_request(MODEM_SMS_REQUEST_SEND_TEXT, &rejected),
          "unconsumed terminal retains its owner");
    check(modem_sms_state_pop_send_result(id, &result) && result.request_id == id &&
          result.outcome == MODEM_SMS_OUTCOME_OK && !modem_sms_state_pop_send_result(id, &result),
          "exact owner consumes once");
    modem_sms_state_init();
    check(modem_sms_state_reserve_request(MODEM_SMS_REQUEST_SEND_BINARY, &rejected) && rejected != id,
          "restart cannot alias stale token");
    check(modem_sms_state_publish_send_result(rejected, MODEM_SMS_REQUEST_SEND_BINARY, MODEM_SMS_OUTCOME_UNCERTAIN) &&
          modem_sms_state_pop_send_result(rejected, &result) && result.outcome == MODEM_SMS_OUTCOME_UNCERTAIN,
          "uncertain transmission remains explicit");
}
static bool binary_segment_matches(sms_submit_pdu_t *expected,
                                   uint8_t expected_segment,
                                   uint8_t expected_total) {
    char expected_hex[SMS_SUBMIT_PDU_HEX_MAX + 1u];
    uint8_t expected_tpdu_len = 0u;
    if (!sms_submit_pdu_build(expected, expected_hex, sizeof(expected_hex),
                              &expected_tpdu_len)) {
        return false;
    }
    modem_sms_binary_segment_view_t actual;
    modem_sms_state_binary_segment_view(&actual);
    return strcmp(actual.pdu_hex, expected_hex) == 0 &&
           actual.tpdu_len == expected_tpdu_len &&
           actual.segment == expected_segment &&
           actual.segment_total == expected_total;
}

static void test_binary_submit_state(void) {
    modem_sms_state_init();
    static const char number[] = "+15551230000";
    uint8_t single[] = {0x01u, 0x23u, 0x45u, 0x67u, 0x89u};
    check(modem_sms_state_binary_begin(sizeof(single)) == 1u &&
              modem_sms_state_binary_has_more(sizeof(single)) &&
              !modem_sms_state_binary_send_ok(),
          "binary begin initializes one-segment traversal and result state");
    check(!modem_sms_state_binary_build_segment(
              NULL, single, sizeof(single), SMS_CODEC_PICTURE_PORT, 0u,
              MODEM_BINARY_SMS_MODE_DCS04_PORT_FIRST) &&
              modem_sms_state_binary_has_more(sizeof(single)),
          "failed PDU build cannot advance the binary cursor");
    check(modem_sms_state_binary_build_segment(
              number, single, sizeof(single), SMS_CODEC_PICTURE_PORT, 0u,
              MODEM_BINARY_SMS_MODE_DCS04_PORT_FIRST),
          "one-segment binary submit builds");
    sms_submit_pdu_t expected = {
        .number = number,
        .payload = single,
        .payload_len = sizeof(single),
        .dest_port = SMS_CODEC_PICTURE_PORT,
        .source_port = 0u,
        .mode = MODEM_BINARY_SMS_MODE_DCS04_PORT_FIRST,
        .segment = 1u,
        .segment_total = 1u,
        .reference = 1u,
    };
    check(binary_segment_matches(&expected, 1u, 1u) &&
              !modem_sms_state_binary_has_more(sizeof(single)),
          "one-segment state emits the codec's exact reference-one PDU");
    modem_sms_state_binary_segment_view(NULL);
    modem_sms_state_binary_set_send_ok(true);
    check(modem_sms_state_binary_send_ok(),
          "binary network acceptance is retained through text-mode restore");

    uint8_t multipart[130];
    for (uint16_t i = 0u; i < sizeof(multipart); i++) {
        multipart[i] = (uint8_t)i;
    }
    check(modem_sms_state_binary_begin(sizeof(multipart)) == 2u &&
              !modem_sms_state_binary_send_ok(),
          "next binary submit advances the shared reference and clears result");
    expected = (sms_submit_pdu_t){
        .number = number,
        .payload = multipart,
        .payload_len = sizeof(multipart),
        .dest_port = SMS_CODEC_PICTURE_PORT,
        .source_port = 0u,
        .mode = MODEM_BINARY_SMS_MODE_DCS04_PORT_FIRST,
        .segment = 1u,
        .segment_total = 2u,
        .reference = 2u,
    };
    check(modem_sms_state_binary_build_segment(
              number, multipart, sizeof(multipart), SMS_CODEC_PICTURE_PORT,
              0u, MODEM_BINARY_SMS_MODE_DCS04_PORT_FIRST) &&
              binary_segment_matches(&expected, 1u, 2u) &&
              modem_sms_state_binary_has_more(sizeof(multipart)) &&
              modem_sms_state_binary_build_segment(
                  number, multipart, sizeof(multipart),
                  SMS_CODEC_PICTURE_PORT, 0u,
                  MODEM_BINARY_SMS_MODE_DCS04_PORT_FIRST) &&
              binary_segment_matches(&expected, 2u, 2u) &&
              !modem_sms_state_binary_has_more(sizeof(multipart)),
          "multipart cursor emits exact ordered segments under one reference");

    for (uint16_t reference = 3u; reference <= UINT8_MAX; reference++) {
        (void)modem_sms_state_binary_begin(sizeof(multipart));
    }
    check(modem_sms_state_binary_begin(sizeof(multipart)) == 2u &&
              modem_sms_state_binary_build_segment(
                  number, multipart, sizeof(multipart),
                  SMS_CODEC_PICTURE_PORT, 0u,
                  MODEM_BINARY_SMS_MODE_DCS04_PORT_FIRST),
          "binary reference wraps through 255 without blocking a submit");
    expected = (sms_submit_pdu_t){
        .number = number,
        .payload = multipart,
        .payload_len = sizeof(multipart),
        .dest_port = SMS_CODEC_PICTURE_PORT,
        .source_port = 0u,
        .mode = MODEM_BINARY_SMS_MODE_DCS04_PORT_FIRST,
        .segment = 1u,
        .segment_total = 2u,
        .reference = 1u,
    };
    check(binary_segment_matches(&expected, 1u, 2u),
          "binary reference wraps from 255 to one, never zero");

    modem_sms_state_init();
    (void)modem_sms_state_binary_begin(sizeof(multipart));
    check(modem_sms_state_binary_build_segment(
              number, multipart, sizeof(multipart), SMS_CODEC_PICTURE_PORT,
              0u, MODEM_BINARY_SMS_MODE_DCS04_PORT_FIRST),
          "binary submit remains usable after service reinitialization");
    expected.reference = 2u;
    expected.position = 0u;
    expected.segment = 1u;
    check(binary_segment_matches(&expected, 1u, 2u),
          "service reinitialization preserves the process-lifetime reference");
}


int main(void) {
    test_terminal_result_latches();
    test_binary_submit_state();
    return s_failures != 0;
}

#include <stdio.h>
#include <string.h>

#include "services/modem_sms_state_internal.h"
#include "services/sms_identity.h"
#include "services/sms_submit_codec.h"

#define TEST_MULTIPART_ARRIVAL_CLAIM_MAX 16u

static int s_failures;

static void check(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        s_failures++;
    }
}

static modem_sms_record_t record_for(uint16_t index) {
    modem_sms_record_t record;
    memset(&record, 0, sizeof(record));
    record.indices[0] = index;
    record.index_count = 1u;
    record.identity_hash = UINT32_C(0x51000000) + index;
    snprintf(record.sender, sizeof(record.sender), "sender-%u",
             (unsigned)index);
    return record;
}

static modem_sms_message_t picture_segment_with_ref(
    uint16_t index, uint8_t sequence, uint8_t total, uint16_t reference,
    bool reference_16bit, uint8_t value, uint16_t length) {
    modem_sms_message_t message;
    memset(&message, 0, sizeof(message));
    message.index = index;
    message.binary = true;
    message.has_ports = true;
    message.has_concat = true;
    message.dest_port = SMS_CODEC_PICTURE_PORT;
    message.source_port = 0x158bu;
    message.concat_ref_16bit = reference_16bit;
    message.concat_ref = reference;
    message.concat_total = total;
    message.concat_seq = sequence;
    message.binary_len = length;
    snprintf(message.status, sizeof(message.status), "REC UNREAD");
    snprintf(message.sender, sizeof(message.sender), "+15551230000");
    snprintf(message.timestamp, sizeof(message.timestamp),
             "26/08/21,12:34:56-16");
    if (length <= sizeof(message.binary_data)) {
        memset(message.binary_data, value, length);
    }
    return message;
}

static modem_sms_message_t picture_segment(uint16_t index, uint8_t sequence,
                                           uint8_t total, uint8_t reference,
                                           uint8_t value, uint16_t length) {
    return picture_segment_with_ref(index, sequence, total, reference, false,
                                    value, length);
}

static modem_sms_message_t text_segment_with_ref(
    uint16_t index, uint8_t sequence, uint8_t total, uint16_t reference,
    bool reference_16bit, const char *text) {
    modem_sms_message_t message;
    memset(&message, 0, sizeof(message));
    message.index = index;
    message.has_concat = true;
    message.concat_ref_16bit = reference_16bit;
    message.concat_ref = reference;
    message.concat_total = total;
    message.concat_seq = sequence;
    snprintf(message.status, sizeof(message.status), "REC UNREAD");
    snprintf(message.sender, sizeof(message.sender), "+15551230000");
    snprintf(message.timestamp, sizeof(message.timestamp),
             "26/08/21,12:34:56-16");
    snprintf(message.text, sizeof(message.text), "%s",
             text != NULL ? text : "");
    return message;
}

static uint32_t multipart_text_identity(
    const modem_sms_message_t *message) {
    char marker[sizeof("Multipart text:1:65535:255")];
    snprintf(marker, sizeof(marker), "Multipart text:%u:%u:%u",
             message->concat_ref_16bit ? 1u : 0u,
             (unsigned)message->concat_ref,
             (unsigned)message->concat_total);
    return sms_identity_hash(message->sender, message->timestamp, marker);
}

static uint32_t reserve_request(modem_sms_request_kind_t kind,
                                const char *message) {
    uint32_t request_id = 0u;
    check(modem_sms_state_reserve_request(kind, &request_id) &&
              request_id != 0u,
          message);
    return request_id;
}

static void test_mailbox_capacity_and_index_zero(void) {
    modem_sms_state_init();
    for (uint16_t index = 0u; index < MODEM_SMS_RECORD_MAX; index++) {
        modem_sms_record_t record = record_for(index);
        check(modem_sms_state_mailbox_append(&record),
              "mailbox accepts every physical ME slot");
    }
    modem_sms_record_t overflow = record_for(999u);
    check(modem_sms_state_mailbox_count() == MODEM_SMS_RECORD_MAX &&
              !modem_sms_state_mailbox_append(&overflow),
          "mailbox represents 255 rows without count overflow");

    for (uint16_t position = 0u; position < MODEM_SMS_RECORD_MAX; position++) {
        modem_sms_record_t actual;
        check(modem_sms_state_mailbox_record((uint8_t)position, &actual) &&
                  actual.indices[0] == position &&
                  actual.identity_hash == UINT32_C(0x51000000) + position,
              "mailbox preserves every row including storage index zero");
    }
    check(!modem_sms_state_mailbox_record(MODEM_SMS_RECORD_MAX, &overflow) &&
              !modem_sms_state_mailbox_record(0u, NULL) &&
              !modem_sms_state_mailbox_append(NULL),
          "mailbox rejects invalid access without changing capacity");
    modem_sms_state_mailbox_clear();
    check(modem_sms_state_mailbox_count() == 0u &&
              !modem_sms_state_mailbox_record(0u, &overflow),
          "mailbox clear removes every visible row");
}

static void test_terminal_result_latches(void) {
    modem_sms_state_init();

    uint32_t mailbox_id = reserve_request(
        MODEM_SMS_REQUEST_MAILBOX, "mailbox request reserves an identity");
    modem_sms_mailbox_result_t mailbox;
    check(modem_sms_state_publish_mailbox_result(
              mailbox_id, MODEM_SMS_REQUEST_MAILBOX, MODEM_SMS_OUTCOME_OK,
              false, false, MODEM_SMS_MAILBOX_OUTBOX) &&
              !modem_sms_state_pop_mailbox_result(mailbox_id + 100u,
                                                   &mailbox) &&
              modem_sms_state_pop_mailbox_result(mailbox_id, &mailbox) &&
              mailbox.request_id == mailbox_id &&
              mailbox.kind == MODEM_SMS_REQUEST_MAILBOX &&
              mailbox.outcome == MODEM_SMS_OUTCOME_OK &&
              !mailbox.sim_not_ready && !mailbox.complete &&
              mailbox.mailbox == MODEM_SMS_MAILBOX_OUTBOX &&
              !modem_sms_state_pop_mailbox_result(mailbox_id, &mailbox),
          "mailbox result is typed and consumable exactly once");

    modem_sms_message_t message;
    memset(&message, 0, sizeof(message));
    message.index = 0u;
    snprintf(message.text, sizeof(message.text), "selected body");
    uint32_t read_id = reserve_request(
        MODEM_SMS_REQUEST_READ, "read request reserves an identity");
    modem_sms_read_result_t read;
    check(modem_sms_state_publish_read_result(
              read_id, MODEM_SMS_REQUEST_READ, MODEM_SMS_OUTCOME_OK, false,
              UINT32_C(0x12345678), UINT32_C(0x87654321), &message) &&
              !modem_sms_state_pop_read_result(read_id + 100u, &read) &&
              modem_sms_state_pop_read_result(read_id, &read) &&
              read.request_id == read_id &&
              read.kind == MODEM_SMS_REQUEST_READ &&
              read.outcome == MODEM_SMS_OUTCOME_OK &&
              read.request_identity_hash == UINT32_C(0x12345678) &&
              read.identity_hash == UINT32_C(0x87654321) &&
              read.message.index == 0u &&
              strcmp(read.message.text, "selected body") == 0 &&
              !modem_sms_state_pop_read_result(read_id, &read),
          "selected-read result preserves identity and index zero");

    read_id = reserve_request(
        MODEM_SMS_REQUEST_READ, "a consumed read channel can be reused");
    check(modem_sms_state_publish_read_result(
              read_id, MODEM_SMS_REQUEST_READ, MODEM_SMS_OUTCOME_TIMEOUT, true,
              UINT32_C(0x11223344), UINT32_C(0xffffffff), &message) &&
              modem_sms_state_pop_read_result(read_id, &read) &&
              read.outcome == MODEM_SMS_OUTCOME_TIMEOUT &&
              read.sim_not_ready &&
              read.request_identity_hash == UINT32_C(0x11223344) &&
              read.identity_hash == 0u && read.message.text[0] == '\0',
          "failed read cannot leak stale message payload");

    read_id = reserve_request(
        MODEM_SMS_REQUEST_READ, "read terminal survives scratch cleanup");
    check(modem_sms_state_publish_read_result(
              read_id, MODEM_SMS_REQUEST_READ, MODEM_SMS_OUTCOME_CANCELLED,
              false, UINT32_C(0x55667788), 0u, NULL),
          "read cancellation publishes before protocol cleanup");
    modem_sms_state_selected_begin(0u, 0u, false);
    check(modem_sms_state_pop_read_result(read_id, &read) &&
              read.request_id == read_id &&
              read.outcome == MODEM_SMS_OUTCOME_CANCELLED &&
              !modem_sms_state_pop_read_result(read_id, &read),
          "selected-message scratch reset cannot erase a terminal result");

    modem_sms_send_result_t send;
    uint32_t send_id = reserve_request(
        MODEM_SMS_REQUEST_SEND_TEXT, "text send reserves the shared channel");
    uint32_t rejected_id = UINT32_C(0xfeedbeef);
    check(!modem_sms_state_reserve_request(
              MODEM_SMS_REQUEST_SEND_BINARY, &rejected_id) &&
              rejected_id == UINT32_C(0xfeedbeef),
          "binary send cannot steal a text send's shared result channel");
    check(modem_sms_state_publish_send_result(
              send_id, MODEM_SMS_REQUEST_SEND_TEXT,
              MODEM_SMS_OUTCOME_UNCERTAIN) &&
              !modem_sms_state_publish_send_result(
                  send_id, MODEM_SMS_REQUEST_SEND_TEXT,
                  MODEM_SMS_OUTCOME_ERROR) &&
              !modem_sms_state_reserve_request(
                  MODEM_SMS_REQUEST_SEND_BINARY, &rejected_id) &&
              !modem_sms_state_pop_send_result(send_id + 100u, &send) &&
              modem_sms_state_pop_send_result(send_id, &send) &&
              send.request_id == send_id &&
              send.kind == MODEM_SMS_REQUEST_SEND_TEXT &&
              send.outcome == MODEM_SMS_OUTCOME_UNCERTAIN &&
              !modem_sms_state_pop_send_result(send_id, &send),
          "send terminal is exactly once and holds the channel until consumed");

    uint32_t save_id = reserve_request(
        MODEM_SMS_REQUEST_SAVE, "save reserves an independent channel");
    modem_sms_save_result_t save;
    check(modem_sms_state_publish_save_result(
              save_id, MODEM_SMS_REQUEST_SAVE, MODEM_SMS_OUTCOME_ERROR,
              true) &&
              !modem_sms_state_pop_save_result(save_id + 100u, &save) &&
              modem_sms_state_pop_save_result(save_id, &save) &&
              save.request_id == save_id &&
              save.kind == MODEM_SMS_REQUEST_SAVE &&
              save.outcome == MODEM_SMS_OUTCOME_ERROR &&
              save.sim_not_ready &&
              !modem_sms_state_pop_save_result(save_id, &save),
          "save result preserves SIM classification");

    uint32_t delete_id = reserve_request(
        MODEM_SMS_REQUEST_DELETE, "delete reserves an independent channel");
    modem_sms_delete_result_t deletion;
    check(modem_sms_state_publish_delete_result(
              delete_id, MODEM_SMS_REQUEST_DELETE, MODEM_SMS_OUTCOME_OK,
              false) &&
              !modem_sms_state_pop_delete_result(delete_id + 100u,
                                                  &deletion) &&
              modem_sms_state_pop_delete_result(delete_id, &deletion) &&
              deletion.request_id == delete_id &&
              deletion.kind == MODEM_SMS_REQUEST_DELETE &&
              deletion.outcome == MODEM_SMS_OUTCOME_OK &&
              !deletion.sim_not_ready &&
              !modem_sms_state_pop_delete_result(delete_id, &deletion),
          "delete result is consumable exactly once");

    uint32_t released_id = reserve_request(
        MODEM_SMS_REQUEST_MAILBOX, "release fixture reserves a request");
    check(!modem_sms_state_release_request(
              released_id, MODEM_SMS_REQUEST_READ) &&
              modem_sms_state_release_request(
                  released_id, MODEM_SMS_REQUEST_MAILBOX) &&
              !modem_sms_state_release_request(
                  released_id, MODEM_SMS_REQUEST_MAILBOX) &&
              !modem_sms_state_publish_mailbox_result(
                  released_id, MODEM_SMS_REQUEST_MAILBOX,
                  MODEM_SMS_OUTCOME_CANCELLED, false, false,
                  MODEM_SMS_MAILBOX_INBOX),
          "only the exact owner can release and a released token cannot publish");

    check(!modem_sms_state_reserve_request(MODEM_SMS_REQUEST_NONE, &rejected_id) &&
              !modem_sms_state_reserve_request(
                  MODEM_SMS_REQUEST_READ, NULL) &&
              !modem_sms_state_pop_mailbox_result(0u, NULL) &&
              !modem_sms_state_pop_read_result(0u, NULL) &&
              !modem_sms_state_pop_send_result(0u, NULL) &&
              !modem_sms_state_pop_save_result(0u, NULL) &&
              !modem_sms_state_pop_delete_result(0u, NULL),
          "result consumers reject null outputs");
}

static void test_scan_collection_integrity(void) {
    modem_sms_state_init();
    modem_sms_collected_view_t view;
    modem_sms_state_collection_view(&view);
    check(!view.collecting_body && !view.decoded &&
              view.list_status == UINT8_MAX && view.message->index == 0u,
          "scan starts with no active row");
    modem_sms_state_collection_view(NULL);

    modem_sms_state_mailbox_row_begin(true, 0u, true, 7u);
    modem_sms_state_collection_view(&view);
    check(view.collecting_body && !view.decoded &&
              view.list_status == 7u && view.message->index == 0u &&
              modem_sms_state_scan_raw_count() == 1u &&
              !modem_sms_state_scan_incomplete(),
          "mailbox collector preserves storage index zero");

    sms_codec_message_t decoded;
    memset(&decoded, 0, sizeof(decoded));
    decoded.binary = true;
    decoded.has_ports = true;
    decoded.has_concat = true;
    decoded.dest_port = SMS_CODEC_PICTURE_PORT;
    decoded.source_port = 0x158bu;
    decoded.concat_ref_16bit = true;
    decoded.concat_ref = 0x12a5u;
    decoded.concat_total = 3u;
    decoded.concat_seq = 2u;
    decoded.binary_len = 4u;
    decoded.binary_data[0] = 0x10u;
    decoded.binary_data[1] = 0x20u;
    decoded.binary_data[2] = 0x30u;
    decoded.binary_data[3] = 0x40u;
    snprintf(decoded.address, sizeof(decoded.address), "+15551234567");
    snprintf(decoded.timestamp, sizeof(decoded.timestamp),
             "26/08/21,01:02:03-16");
    modem_sms_state_collection_feed_decoded(&decoded);
    modem_sms_state_collection_set_status("REC UNREAD");
    modem_sms_state_collection_view(&view);
    check(!view.collecting_body && view.decoded &&
              view.message->index == 0u && view.message->binary &&
              view.message->has_ports && view.message->has_concat &&
              view.message->dest_port == SMS_CODEC_PICTURE_PORT &&
              view.message->source_port == 0x158bu &&
              view.message->concat_ref_16bit &&
              view.message->concat_ref == 0x12a5u &&
              view.message->concat_total == 3u &&
              view.message->concat_seq == 2u &&
              view.message->binary_len == 4u &&
              memcmp(view.message->binary_data, decoded.binary_data, 4u) == 0 &&
              strcmp(view.message->status, "REC UNREAD") == 0 &&
              strcmp(view.message->sender, decoded.address) == 0 &&
              strcmp(view.message->timestamp, decoded.timestamp) == 0,
          "decoded PDU copy preserves every service-message field");

    modem_sms_state_scan_reset();
    modem_sms_state_mailbox_row_begin(true, 1u, true, 0u);
    modem_sms_state_mailbox_row_begin(true, 2u, true, 1u);
    modem_sms_state_collection_view(&view);
    check(modem_sms_state_scan_incomplete() &&
              modem_sms_state_scan_raw_count() == 2u &&
              view.collecting_body && view.message->index == 2u,
          "interleaved mailbox headers invalidate but continue the scan");

    modem_sms_state_scan_reset();
    for (uint16_t index = 0u; index < MODEM_SMS_RECORD_MAX; index++) {
        modem_sms_state_mailbox_row_begin(true, index, true, 0u);
        modem_sms_state_collection_abort_body();
    }
    check(modem_sms_state_scan_raw_count() == MODEM_SMS_RECORD_MAX &&
              !modem_sms_state_scan_incomplete(),
          "raw scan count represents all 255 physical rows");
    modem_sms_state_mailbox_row_begin(true, 999u, true, 0u);
    check(modem_sms_state_scan_raw_count() == MODEM_SMS_RECORD_MAX &&
              modem_sms_state_scan_incomplete(),
          "a row beyond scan capacity cannot wrap the raw count");

    modem_sms_state_scan_reset();
    modem_sms_state_mailbox_row_begin(false, 0u, true, 0u);
    check(modem_sms_state_scan_raw_count() == 0u &&
              modem_sms_state_scan_incomplete(),
          "a malformed row header is not counted as a represented row");
    modem_sms_state_scan_reset();
    modem_sms_state_mailbox_row_begin(true, 3u, false, UINT8_MAX);
    check(modem_sms_state_scan_raw_count() == 1u &&
              modem_sms_state_scan_incomplete(),
          "an invalid status retains the row count but invalidates completeness");

    modem_sms_state_detail_row_begin(42u);
    modem_sms_state_detail_header("REC READ", "+15550000042",
                                  "26/08/21,02:03:04-16");
    modem_sms_state_collection_feed_text("first");
    modem_sms_state_collection_feed_text("second");
    modem_sms_state_collection_view(&view);
    check(view.collecting_body && !view.decoded &&
              view.message->index == 42u &&
              strcmp(view.message->status, "REC READ") == 0 &&
              strcmp(view.message->text, "first\nsecond") == 0,
          "text fallback retains CMGR metadata and joins body lines");
    modem_sms_state_collection_abort_body();
    modem_sms_state_collection_view(&view);
    check(!view.collecting_body,
          "collector abort retires only the pending body");
}

static void test_selected_read_assembly(void) {
    modem_sms_state_init();
    const uint16_t indices[] = {10u, 11u, 12u};
    uint32_t identity = sms_identity_hash(
        "+15551230000", "26/08/21,12:34:56-16", "Picture message");
    modem_sms_state_selected_begin(indices[0], 3u, false);

    uint16_t index = UINT16_MAX;
    check(!modem_sms_state_selected_next_index(indices, 3u, NULL) &&
              modem_sms_state_selected_next_index(indices, 3u, &index) &&
              index == 10u &&
              modem_sms_state_selected_next_index(indices, 3u, &index) &&
              index == 11u &&
              modem_sms_state_selected_next_index(indices, 3u, &index) &&
              index == 12u &&
              !modem_sms_state_selected_next_index(indices, 3u, &index),
          "selected-read cursor preserves requested order without null advance");

    modem_sms_message_t second = picture_segment(11u, 2u, 3u, 0x51u,
                                                  0x22u, 3u);
    modem_sms_message_t first = picture_segment(10u, 1u, 3u, 0x51u,
                                                 0x11u, 2u);
    modem_sms_message_t third = picture_segment(12u, 3u, 3u, 0x51u,
                                                 0x33u, 4u);
    check(modem_sms_state_selected_accept_segment(&second) &&
              !modem_sms_state_selected_complete() &&
              modem_sms_state_selected_accept_segment(&first) &&
              modem_sms_state_selected_accept_segment(&second) &&
              modem_sms_state_selected_accept_segment(&third) &&
              modem_sms_state_selected_complete(),
          "selected picture accepts out-of-order and duplicate segments");

    bool mismatch = true;
    uint32_t read_id = reserve_request(
        MODEM_SMS_REQUEST_READ, "selected picture reserves a read identity");
    check(modem_sms_state_selected_publish_result(
              read_id, MODEM_SMS_REQUEST_READ, MODEM_SMS_OUTCOME_OK, false,
              identity, &mismatch) && !mismatch,
          "complete selected picture passes identity verification");
    modem_sms_read_result_t result;
    check(modem_sms_state_pop_read_result(read_id, &result) &&
              result.request_id == read_id &&
              result.kind == MODEM_SMS_REQUEST_READ &&
              result.outcome == MODEM_SMS_OUTCOME_OK &&
              result.request_identity_hash == identity &&
              result.identity_hash == identity &&
              result.message.index == indices[0] &&
              result.message.binary_len ==
                  (uint16_t)(2u * MODEM_SMS_BINARY_CHUNK_MAX + 4u) &&
              result.message.binary_data[0] == 0x11u &&
              result.message.binary_data[1] == 0x11u &&
              result.message.binary_data[MODEM_SMS_BINARY_CHUNK_MAX] == 0x22u &&
              result.message.binary_data[2u * MODEM_SMS_BINARY_CHUNK_MAX] == 0x33u,
          "selected picture result preserves sparse fixed-chunk wire assembly");

    const uint16_t text_indices[] = {65u, 66u};
    modem_sms_message_t text_first = text_segment_with_ref(
        65u, 1u, 2u, 0xacu, false,
        "P5MULTIBEGIN01234567890123456789");
    modem_sms_message_t text_second = text_segment_with_ref(
        66u, 2u, 2u, 0xacu, false,
        "abcdefghijklmnopqrstuvwxyzP5MULTIEND");
    uint32_t text_identity = multipart_text_identity(&text_first);
    modem_sms_state_selected_begin(text_indices[0], 2u, false);
    check(modem_sms_state_selected_next_index(
              text_indices, 2u, &index) && index == 65u &&
              modem_sms_state_selected_accept_segment(&text_first) &&
              !modem_sms_state_selected_complete() &&
              modem_sms_state_selected_next_index(
                  text_indices, 2u, &index) && index == 66u &&
              modem_sms_state_selected_accept_segment(&text_second) &&
              modem_sms_state_selected_complete(),
          "selected multipart text accepts every segment in record order");
    mismatch = true;
    read_id = reserve_request(
        MODEM_SMS_REQUEST_READ,
        "multipart text reserves a read identity");
    check(modem_sms_state_selected_publish_result(
              read_id, MODEM_SMS_REQUEST_READ, MODEM_SMS_OUTCOME_OK,
              false, text_identity, &mismatch) && !mismatch &&
              modem_sms_state_pop_read_result(read_id, &result) &&
              result.outcome == MODEM_SMS_OUTCOME_OK &&
              strcmp(result.message.text,
                     "P5MULTIBEGIN01234567890123456789"
                     "abcdefghijklmnopqrstuvwxyzP5MULTIEND") == 0,
          "selected multipart text publishes one exact concatenated body");

    modem_sms_message_t text_message;
    memset(&text_message, 0, sizeof(text_message));
    text_message.index = 0u;
    snprintf(text_message.sender, sizeof(text_message.sender), "+15550000000");
    snprintf(text_message.timestamp, sizeof(text_message.timestamp),
             "26/08/21,04:05:06-16");
    snprintf(text_message.text, sizeof(text_message.text), "hello");
    const uint16_t index_zero[] = {0u};
    uint32_t wrong_identity = sms_identity_hash(
        text_message.sender, text_message.timestamp, "different");
    modem_sms_state_selected_begin(index_zero[0], 1u, false);
    read_id = reserve_request(
        MODEM_SMS_REQUEST_READ, "identity mismatch reserves a read identity");
    check(modem_sms_state_selected_accept_segment(&text_message) &&
              modem_sms_state_selected_complete() &&
              modem_sms_state_selected_publish_result(
                  read_id, MODEM_SMS_REQUEST_READ, MODEM_SMS_OUTCOME_OK,
                  false, wrong_identity, &mismatch) && mismatch &&
              modem_sms_state_pop_read_result(read_id, &result) &&
              result.request_id == read_id &&
              result.outcome == MODEM_SMS_OUTCOME_ERROR &&
              result.request_identity_hash == wrong_identity &&
              result.identity_hash == 0u && result.message.text[0] == '\0',
          "identity mismatch publishes failure without stale selected payload");

    modem_sms_state_selected_begin(indices[0], 3u, false);
    modem_sms_message_t invalid = picture_segment(10u, 0u, 3u, 0x51u,
                                                   0x44u, 1u);
    mismatch = true;
    read_id = reserve_request(
        MODEM_SMS_REQUEST_READ, "invalid segment reserves a read identity");
    check(!modem_sms_state_selected_accept_segment(&invalid) &&
              !modem_sms_state_selected_complete() &&
              modem_sms_state_selected_publish_result(
                  read_id, MODEM_SMS_REQUEST_READ, MODEM_SMS_OUTCOME_OK,
                  true, identity, &mismatch) && !mismatch &&
              modem_sms_state_pop_read_result(read_id, &result) &&
              result.request_id == read_id &&
              result.outcome == MODEM_SMS_OUTCOME_ERROR &&
              result.sim_not_ready,
          "invalid selected segment latches failure and preserves SIM evidence");

    modem_sms_message_t orphan_first = picture_segment(
        60u, 1u, 3u, 0x61u, 0x11u, 2u);
    modem_sms_message_t orphan_last = picture_segment(
        62u, 3u, 3u, 0x61u, 0x33u, 2u);
    const uint16_t orphan_indices[] = {60u, 62u};
    uint32_t data_identity = sms_identity_hash(
        orphan_first.sender, orphan_first.timestamp, "Data message");
    modem_sms_state_selected_begin(orphan_indices[0], 2u, true);
    read_id = reserve_request(
        MODEM_SMS_REQUEST_READ, "quarantine read reserves an identity");
    check(modem_sms_state_selected_accept_segment(&orphan_first) &&
              modem_sms_state_selected_accept_segment(&orphan_last) &&
              modem_sms_state_selected_complete() &&
              modem_sms_state_selected_publish_result(
                  read_id, MODEM_SMS_REQUEST_READ, MODEM_SMS_OUTCOME_OK,
                  false, data_identity, &mismatch) && !mismatch &&
              modem_sms_state_pop_read_result(read_id, &result) &&
              result.outcome == MODEM_SMS_OUTCOME_OK &&
              result.message.binary && !result.message.has_ports &&
              !result.message.has_concat && result.message.binary_len == 0u,
          "quarantined multipart read consumes observed rows as neutral data");

    modem_sms_message_t corrupt;
    memset(&corrupt, 0, sizeof(corrupt));
    corrupt.index = 70u;
    snprintf(corrupt.status, sizeof(corrupt.status), "REC UNREAD");
    modem_sms_record_t corrupt_record;
    modem_sms_message_t corrupt_neighbor = corrupt;
    corrupt_neighbor.index = 71u;
    check(modem_sms_state_mailbox_prepare_quarantine(
              &corrupt, &corrupt_record),
          "undecodable row creates a quarantine identity");
    uint32_t corrupt_identity = corrupt_record.identity_hash;
    check(modem_sms_state_mailbox_prepare_quarantine(
              &corrupt_neighbor, &corrupt_record) &&
              corrupt_record.identity_hash != corrupt_identity,
          "adjacent undecodable rows cannot share a stale-read identity");
    modem_sms_state_selected_begin(corrupt.index, 1u, true);
    read_id = reserve_request(
        MODEM_SMS_REQUEST_READ, "corrupt-row read reserves an identity");
    check(modem_sms_state_selected_accept_undecoded(&corrupt) &&
              modem_sms_state_selected_complete() &&
              modem_sms_state_selected_publish_result(
                  read_id, MODEM_SMS_REQUEST_READ, MODEM_SMS_OUTCOME_OK,
                  false, corrupt_identity, &mismatch) &&
              modem_sms_state_pop_read_result(read_id, &result) &&
              result.outcome == MODEM_SMS_OUTCOME_OK,
          "quarantined corrupt row can be opened and marked read");

    modem_sms_state_selected_begin(0u, 1u, false);
    check(!modem_sms_state_selected_next_index(NULL, 1u, &index) &&
              !modem_sms_state_selected_complete() &&
              !modem_sms_state_selected_accept_segment(NULL),
          "selected-read admission rejects missing indices and null segments");
}

static void test_mailbox_record_preparation(void) {
    modem_sms_state_init();
    modem_sms_message_t message;
    memset(&message, 0, sizeof(message));
    message.index = 0u;
    snprintf(message.status, sizeof(message.status), "REC UNREAD");
    snprintf(message.sender, sizeof(message.sender), "+15550000000");
    snprintf(message.timestamp, sizeof(message.timestamp),
             "26/08/21,05:06:07-16");
    snprintf(message.text, sizeof(message.text), "plain text");

    modem_sms_record_t record;
    check(modem_sms_state_mailbox_prepare_record(&message, &record) &&
              record.index_count == 1u && record.indices[0] == 0u &&
              !record.picture &&
              record.identity_hash == sms_identity_hash(
                  message.sender, message.timestamp, message.text) &&
              strcmp(record.status, message.status) == 0 &&
              strcmp(record.sender, message.sender) == 0 &&
              strcmp(record.timestamp, message.timestamp) == 0,
          "single text message prepares one index-zero mailbox record");

    message.binary = true;
    message.binary_len = 1u;
    message.binary_data[0] = 0x42u;
    check(modem_sms_state_mailbox_prepare_record(&message, &record) &&
              !record.picture &&
              record.identity_hash == sms_identity_hash(
                  message.sender, message.timestamp, "Data message"),
          "unported binary message uses the neutral data identity");

    message.has_ports = true;
    message.dest_port = SMS_CODEC_PICTURE_PORT;
    check(modem_sms_state_mailbox_prepare_record(&message, &record) &&
              record.picture &&
              record.identity_hash == sms_identity_hash(
                  message.sender, message.timestamp, "Picture message"),
          "single ungrouped picture message uses the picture identity");

    modem_sms_state_multipart_groups_reset();
    modem_sms_message_t text_second = text_segment_with_ref(
        66u, 2u, 2u, 0xacu, false, "second half");
    modem_sms_message_t text_first = text_segment_with_ref(
        65u, 1u, 2u, 0xacu, false, "first half ");
    snprintf(text_first.status, sizeof(text_first.status), "REC READ");
    check(!modem_sms_state_mailbox_prepare_record(&text_second, &record) &&
              modem_sms_state_multipart_groups_pending() &&
              modem_sms_state_mailbox_prepare_record(&text_first, &record) &&
              !record.picture && !record.quarantined &&
              record.index_count == 2u && record.indices[0] == 65u &&
              record.indices[1] == 66u &&
              strcmp(record.status, "REC UNREAD") == 0 &&
              record.identity_hash == multipart_text_identity(&text_first),
          "multipart text preserves wire order and any unread segment state");

    modem_sms_state_multipart_groups_reset();
    char long_first[201];
    char long_second[201];
    memset(long_first, 'A', sizeof(long_first) - 1u);
    memset(long_second, 'B', sizeof(long_second) - 1u);
    long_first[sizeof(long_first) - 1u] = '\0';
    long_second[sizeof(long_second) - 1u] = '\0';
    modem_sms_message_t text_oversize_first = text_segment_with_ref(
        70u, 1u, 2u, 0xadu, false, long_first);
    modem_sms_message_t text_oversize_second = text_segment_with_ref(
        71u, 2u, 2u, 0xadu, false, long_second);
    check(!modem_sms_state_mailbox_prepare_record(
              &text_oversize_first, &record) &&
              modem_sms_state_mailbox_prepare_record(
                  &text_oversize_second, &record) &&
              record.quarantined && !record.picture &&
              record.index_count == 2u && record.indices[0] == 70u &&
              record.indices[1] == 71u,
          "multipart text beyond the decoded-body bound settles as one Data row");

    modem_sms_state_multipart_groups_reset();
    modem_sms_message_t colliding_picture_first = picture_segment(
        80u, 1u, 2u, 0x90u, 0x11u, 1u);
    modem_sms_message_t colliding_picture_second = picture_segment(
        81u, 2u, 2u, 0x90u, 0x22u, 1u);
    modem_sms_message_t colliding_text_first = text_segment_with_ref(
        82u, 1u, 2u, 0x90u, false, "text one ");
    modem_sms_message_t colliding_text_second = text_segment_with_ref(
        83u, 2u, 2u, 0x90u, false, "text two");
    check(!modem_sms_state_mailbox_prepare_record(
              &colliding_picture_first, &record) &&
              !modem_sms_state_mailbox_prepare_record(
                  &colliding_text_first, &record) &&
              modem_sms_state_mailbox_prepare_record(
                  &colliding_picture_second, &record) &&
              record.picture && record.indices[0] == 80u &&
              record.indices[1] == 81u &&
              modem_sms_state_mailbox_prepare_record(
                  &colliding_text_second, &record) &&
              !record.picture && record.indices[0] == 82u &&
              record.indices[1] == 83u &&
              !modem_sms_state_multipart_groups_pending(),
          "picture and text groups cannot merge across an identical concat key");

    modem_sms_message_t oversized = picture_segment(
        8u, 2u, 3u, 0x70u, 0x66u,
        (uint16_t)(MODEM_SMS_BINARY_CHUNK_MAX + 1u));
    check(modem_sms_state_mailbox_prepare_record(&oversized, &record) &&
              !record.picture && record.quarantined &&
              record.index_count == 1u &&
              record.indices[0] == 8u &&
              !modem_sms_state_multipart_groups_pending() &&
              !modem_sms_state_scan_incomplete(),
          "oversized multipart chunk becomes a settled Data-message row");

    modem_sms_message_t too_many = picture_segment(
        9u, 1u, (uint8_t)(MODEM_SMS_SEGMENT_MAX + 1u), 0x71u,
        0x77u, 1u);
    check(modem_sms_state_mailbox_prepare_record(&too_many, &record) &&
              record.quarantined && !record.picture &&
              record.index_count == 1u && record.indices[0] == 9u &&
              !modem_sms_state_scan_incomplete(),
          "unsupported segment counts cannot poison the mailbox snapshot");

    modem_sms_state_scan_reset();
    modem_sms_message_t second = picture_segment(11u, 2u, 3u, 0x72u,
                                                  0x22u, 3u);
    snprintf(second.status, sizeof(second.status), "REC READ");
    snprintf(second.timestamp, sizeof(second.timestamp),
             "26/08/21,01:00:00-16");
    modem_sms_message_t first = picture_segment(10u, 1u, 3u, 0x72u,
                                                 0x11u, 2u);
    modem_sms_message_t third = picture_segment(12u, 3u, 3u, 0x72u,
                                                 0x33u, 4u);
    check(!modem_sms_state_mailbox_prepare_record(&second, &record) &&
              modem_sms_state_multipart_groups_pending() &&
              !modem_sms_state_scan_incomplete() &&
              !modem_sms_state_mailbox_prepare_record(&first, &record) &&
              !modem_sms_state_mailbox_prepare_record(&second, &record) &&
              modem_sms_state_mailbox_prepare_record(&third, &record),
          "picture grouping accepts out-of-order and duplicate segments");
    check(!modem_sms_state_multipart_groups_pending() && record.picture &&
              record.index_count == 3u && record.indices[0] == 10u &&
              record.indices[1] == 11u && record.indices[2] == 12u &&
              strcmp(record.status, first.status) == 0 &&
              strcmp(record.timestamp, first.timestamp) == 0 &&
              record.identity_hash == sms_identity_hash(
                  first.sender, first.timestamp, "Picture message"),
          "completed picture record uses sequence-one metadata and wire order");
    check(modem_sms_state_mailbox_count() == 0u &&
              modem_sms_state_mailbox_append(&record) &&
              modem_sms_state_mailbox_count() == 1u,
          "record preparation leaves the final cache append to the lock owner");

    modem_sms_state_multipart_groups_reset();
    modem_sms_message_t short_ref_first = picture_segment_with_ref(
        60u, 1u, 2u, 0x00cdu, false, 0x41u, 1u);
    modem_sms_message_t long_ref_first = picture_segment_with_ref(
        70u, 1u, 2u, 0xabcdu, true, 0x51u, 1u);
    modem_sms_message_t short_ref_second = picture_segment_with_ref(
        61u, 2u, 2u, 0x00cdu, false, 0x42u, 1u);
    modem_sms_message_t long_ref_second = picture_segment_with_ref(
        71u, 2u, 2u, 0xabcdu, true, 0x52u, 1u);
    check(!modem_sms_state_mailbox_prepare_record(
              &short_ref_first, &record) &&
              !modem_sms_state_mailbox_prepare_record(
                  &long_ref_first, &record) &&
              modem_sms_state_mailbox_prepare_record(
                  &short_ref_second, &record) &&
              record.indices[0] == 60u && record.indices[1] == 61u,
          "8-bit concat group survives a simultaneous 16-bit low-byte collision");
    check(modem_sms_state_mailbox_prepare_record(
              &long_ref_second, &record) &&
              record.indices[0] == 70u && record.indices[1] == 71u &&
              !modem_sms_state_multipart_groups_pending(),
          "16-bit concat group preserves its complete reference identity");

    const uint16_t mixed_indices[] = {60u, 71u};
    modem_sms_state_selected_begin(mixed_indices[0], 2u, false);
    check(modem_sms_state_selected_accept_segment(&short_ref_first) &&
              !modem_sms_state_selected_accept_segment(&long_ref_second) &&
              !modem_sms_state_selected_complete(),
          "selected assembly rejects an 8-bit/16-bit reference collision");

    modem_sms_state_scan_reset();
    modem_sms_message_t invalid = picture_segment(20u, 0u, 3u, 0x73u,
                                                   0x44u, 1u);
    check(modem_sms_state_mailbox_prepare_record(&invalid, &record) &&
              record.quarantined && record.indices[0] == 20u &&
              !modem_sms_state_scan_incomplete() &&
              !modem_sms_state_multipart_groups_pending(),
          "invalid multipart sequence remains visible without a retry loop");

    modem_sms_state_scan_reset();
    modem_sms_message_t overflow = picture_segment(23u, 4u, 4u, 0x74u,
                                                    0x55u, 1u);
    check(!modem_sms_state_mailbox_prepare_record(&overflow, &record) &&
              !modem_sms_state_scan_incomplete() &&
              modem_sms_state_multipart_groups_pending() &&
              modem_sms_state_multipart_group_take_quarantine(&record) &&
              record.quarantined && record.index_count == 1u &&
              record.indices[0] == 23u &&
              !modem_sms_state_multipart_groups_pending(),
          "fixed-chunk offset overflow settles through bounded quarantine");

    modem_sms_state_scan_reset();
    modem_sms_message_t missing_first_2 = picture_segment(
        31u, 2u, 3u, 0x75u, 0x22u, 1u);
    modem_sms_message_t missing_first_3 = picture_segment(
        32u, 3u, 3u, 0x75u, 0x33u, 1u);
    check(!modem_sms_state_mailbox_prepare_record(&missing_first_2, &record) &&
              !modem_sms_state_mailbox_prepare_record(&missing_first_3, &record) &&
              modem_sms_state_multipart_group_take_quarantine(&record) &&
              record.quarantined && record.index_count == 2u &&
              record.indices[0] == 31u && record.indices[1] == 32u,
          "missing-first group settles with observed indices in wire order");

    modem_sms_message_t missing_middle_1 = picture_segment(
        33u, 1u, 3u, 0x76u, 0x11u, 1u);
    modem_sms_message_t missing_middle_3 = picture_segment(
        35u, 3u, 3u, 0x76u, 0x33u, 1u);
    check(!modem_sms_state_mailbox_prepare_record(&missing_middle_3, &record) &&
              !modem_sms_state_mailbox_prepare_record(&missing_middle_1, &record) &&
              modem_sms_state_multipart_group_take_quarantine(&record) &&
              record.index_count == 2u && record.indices[0] == 33u &&
              record.indices[1] == 35u &&
              record.identity_hash == sms_identity_hash(
                  missing_middle_1.sender, missing_middle_1.timestamp,
                  "Data message"),
          "missing-middle group uses its lowest sequence as stable identity");

    modem_sms_message_t missing_last_1 = picture_segment(
        36u, 1u, 3u, 0x77u, 0x11u, 1u);
    modem_sms_message_t missing_last_2 = picture_segment(
        37u, 2u, 3u, 0x77u, 0x22u, 1u);
    check(!modem_sms_state_mailbox_prepare_record(&missing_last_1, &record) &&
              !modem_sms_state_mailbox_prepare_record(&missing_last_2, &record) &&
              modem_sms_state_multipart_group_take_quarantine(&record) &&
              record.index_count == 2u && record.indices[0] == 36u &&
              record.indices[1] == 37u &&
              !modem_sms_state_multipart_group_take_quarantine(&record),
          "missing-last group retires exactly once");

    modem_sms_message_t duplicate_a = picture_segment(
        40u, 1u, 2u, 0x78u, 0x11u, 1u);
    modem_sms_message_t duplicate_b = duplicate_a;
    duplicate_b.index = 41u;
    check(!modem_sms_state_mailbox_prepare_record(&duplicate_a, &record) &&
              modem_sms_state_mailbox_prepare_record(&duplicate_b, &record) &&
              record.quarantined && record.index_count == 1u &&
              record.indices[0] == 41u &&
              modem_sms_state_multipart_group_take_quarantine(&record) &&
              record.indices[0] == 40u,
          "conflicting duplicate sequence preserves both physical rows");

    modem_sms_message_t later_text;
    memset(&later_text, 0, sizeof(later_text));
    later_text.index = 50u;
    snprintf(later_text.text, sizeof(later_text.text), "later ordinary SMS");
    check(modem_sms_state_mailbox_prepare_record(&later_text, &record) &&
              !record.quarantined && !record.picture &&
              record.indices[0] == 50u &&
              !modem_sms_state_scan_incomplete(),
          "a later ordinary SMS remains representable after quarantines");

    modem_sms_state_scan_reset();
    check(!modem_sms_state_mailbox_prepare_record(NULL, &record) &&
              modem_sms_state_scan_incomplete(),
          "null mailbox message cannot produce an authoritative record");
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

static void test_pending_arrival_reconciliation(void) {
    modem_sms_state_init();
    for (uint16_t index = 0u; index < MODEM_SMS_RECORD_MAX; index++) {
        check(modem_sms_state_pending_arrival_track(index),
              "pending-arrival queue accepts every ME slot");
    }
    check(modem_sms_state_pending_arrival_track(0u) &&
              !modem_sms_state_pending_arrival_track(999u),
          "pending arrivals coalesce duplicates and bound overflow");

    modem_sms_state_pending_arrivals_reset();
    check(modem_sms_state_pending_arrival_track(0u) &&
              modem_sms_state_pending_arrival_track(7u) &&
              modem_sms_state_pending_arrival_track(9u),
          "pending-arrival fixture tracks distinct storage indices");
    modem_sms_state_arrival_scan_begin(UINT32_MAX);
    modem_sms_state_arrival_scan_note(0u, MODEM_SMS_ARRIVAL_USER);
    modem_sms_state_arrival_scan_note(7u, MODEM_SMS_ARRIVAL_FILTERED);
    modem_sms_state_arrival_scan_note(1234u, MODEM_SMS_ARRIVAL_USER);
    uint32_t user_arrivals = 99u;
    check(!modem_sms_state_arrival_scan_commit(
              true, true, 0u, &user_arrivals) && user_arrivals == 0u,
          "revision wrap cannot commit a crossed mailbox snapshot");

    modem_sms_state_arrival_scan_begin(0u);
    modem_sms_state_arrival_scan_note(0u, MODEM_SMS_ARRIVAL_USER);
    modem_sms_state_arrival_scan_note(7u, MODEM_SMS_ARRIVAL_FILTERED);
    check(!modem_sms_state_arrival_scan_commit(
              false, true, 0u, &user_arrivals) &&
              !modem_sms_state_arrival_scan_commit(
                  true, false, 0u, &user_arrivals),
          "incomplete and non-Inbox scans preserve pending arrivals");
    check(modem_sms_state_arrival_scan_commit(
              true, true, 0u, &user_arrivals) && user_arrivals == 1u,
          "matching complete Inbox scan publishes only user SMS arrivals");

    modem_sms_state_arrival_scan_begin(1u);
    check(modem_sms_state_arrival_scan_commit(
              true, true, 1u, &user_arrivals) && user_arrivals == 0u &&
              modem_sms_state_arrival_scan_commit(
                  true, true, 1u, NULL),
          "committed arrival queue is empty and null delta output is valid");
}

static void test_multipart_arrival_reconciliation(void) {
    modem_sms_state_init();
    check(modem_sms_state_pending_arrival_track(10u) &&
              modem_sms_state_pending_arrival_track(11u) &&
              modem_sms_state_pending_arrival_track(12u),
          "multipart arrival fixture tracks all three physical rows");
    modem_sms_state_arrival_scan_begin(7u);
    modem_sms_record_t record;
    modem_sms_message_t second = picture_segment(
        11u, 2u, 3u, 0x31u, 0x22u, 1u);
    modem_sms_message_t first = picture_segment(
        10u, 1u, 3u, 0x31u, 0x11u, 1u);
    modem_sms_message_t third = picture_segment(
        12u, 3u, 3u, 0x31u, 0x33u, 1u);
    check(!modem_sms_state_mailbox_prepare_record(&second, &record) &&
              !modem_sms_state_mailbox_prepare_record(&second, &record) &&
              !modem_sms_state_mailbox_prepare_record(&first, &record) &&
              modem_sms_state_mailbox_prepare_record(&third, &record),
          "out-of-order multipart arrival settles despite a replayed segment");
    uint32_t user_arrivals = 99u;
    check(modem_sms_state_arrival_scan_commit(
              true, true, 7u, &user_arrivals) && user_arrivals == 1u,
          "one logical picture contributes one user arrival, not three");

    modem_sms_state_multipart_groups_reset();
    check(modem_sms_state_pending_arrival_track(22u),
          "later-segment arrival fixture tracks only its new physical row");
    modem_sms_state_arrival_scan_begin(8u);
    modem_sms_message_t old_first = picture_segment(
        20u, 1u, 3u, 0x32u, 0x11u, 1u);
    modem_sms_message_t old_second = picture_segment(
        21u, 2u, 3u, 0x32u, 0x22u, 1u);
    modem_sms_message_t new_third = picture_segment(
        22u, 3u, 3u, 0x32u, 0x33u, 1u);
    check(!modem_sms_state_mailbox_prepare_record(&old_first, &record) &&
              !modem_sms_state_mailbox_prepare_record(&old_second, &record) &&
              modem_sms_state_mailbox_prepare_record(&new_third, &record) &&
              modem_sms_state_arrival_scan_commit(
                  true, true, 8u, &user_arrivals) && user_arrivals == 1u,
          "old segments cannot consume the arrival claim before a new segment");

    modem_sms_state_multipart_groups_reset();
    check(modem_sms_state_pending_arrival_track(30u) &&
              modem_sms_state_pending_arrival_track(31u) &&
              modem_sms_state_pending_arrival_track(40u) &&
              modem_sms_state_pending_arrival_track(41u),
          "two-picture fixture tracks both logical groups");
    modem_sms_state_arrival_scan_begin(9u);
    modem_sms_message_t a1 = picture_segment(
        30u, 1u, 2u, 0x41u, 0x11u, 1u);
    modem_sms_message_t b1 = picture_segment(
        40u, 1u, 2u, 0x42u, 0x21u, 1u);
    modem_sms_message_t a2 = picture_segment(
        31u, 2u, 2u, 0x41u, 0x12u, 1u);
    modem_sms_message_t b2 = picture_segment(
        41u, 2u, 2u, 0x42u, 0x22u, 1u);
    check(!modem_sms_state_mailbox_prepare_record(&a1, &record) &&
              !modem_sms_state_mailbox_prepare_record(&b1, &record) &&
              modem_sms_state_mailbox_prepare_record(&a2, &record) &&
              modem_sms_state_mailbox_prepare_record(&b2, &record) &&
              modem_sms_state_arrival_scan_commit(
                  true, true, 9u, &user_arrivals) && user_arrivals == 2u,
          "distinct logical picture groups each retain one notification");
}

static void test_multipart_arrival_across_scans(void) {
    modem_sms_state_init();
    modem_sms_record_t record;
    modem_sms_message_t first = text_segment_with_ref(
        65u, 1u, 2u, 0xacu, false, "first half ");
    modem_sms_message_t second = text_segment_with_ref(
        66u, 2u, 2u, 0xacu, false, "second half");

    check(modem_sms_state_pending_arrival_track(first.index),
          "first split-delivery segment is tracked");
    modem_sms_state_arrival_scan_begin(10u);
    check(!modem_sms_state_mailbox_prepare_record(&first, &record) &&
              modem_sms_state_multipart_group_take_quarantine(&record) &&
              record.quarantined && record.index_count == 1u,
          "first split-delivery scan settles its incomplete logical row");
    uint32_t arrivals = 99u;
    check(modem_sms_state_arrival_scan_commit(
              true, true, 10u, &arrivals) && arrivals == 1u,
          "first multipart scan emits the one user notification");

    modem_sms_state_multipart_groups_reset();
    check(modem_sms_state_pending_arrival_track(second.index),
          "later split-delivery segment is tracked independently");
    modem_sms_state_arrival_scan_begin(11u);
    /* Present the newly arrived segment first to prove suppression does not
     * depend on CMGL storage order placing the old anchor before it. */
    check(!modem_sms_state_mailbox_prepare_record(&second, &record) &&
              modem_sms_state_mailbox_prepare_record(&first, &record) &&
              !record.quarantined && record.index_count == 2u &&
              record.indices[0] == first.index &&
              record.indices[1] == second.index,
          "next scan upgrades the orphan into one complete text row");
    check(modem_sms_state_arrival_scan_commit(
              true, true, 11u, &arrivals) && arrivals == 0u,
          "later segments of an already-announced multipart emit no duplicate alert");

    modem_sms_state_multipart_groups_reset();
    modem_sms_message_t ordinary;
    memset(&ordinary, 0, sizeof(ordinary));
    ordinary.index = 67u;
    snprintf(ordinary.text, sizeof(ordinary.text), "ordinary follow-up");
    check(modem_sms_state_pending_arrival_track(ordinary.index),
          "ordinary follow-up is tracked after multipart completion");
    modem_sms_state_arrival_scan_begin(12u);
    check(modem_sms_state_mailbox_prepare_record(&ordinary, &record) &&
              modem_sms_state_arrival_scan_commit(
                  true, true, 12u, &arrivals) && arrivals == 1u,
          "multipart debounce never suppresses a later ordinary SMS");

    modem_sms_state_init();
    first = text_segment_with_ref(
        75u, 1u, 2u, 0xadu, false, "orphan removed ");
    second = text_segment_with_ref(
        76u, 2u, 2u, 0xadu, false, "late second");
    check(modem_sms_state_pending_arrival_track(first.index),
          "deleted-anchor fixture tracks its first segment");
    modem_sms_state_arrival_scan_begin(20u);
    check(!modem_sms_state_mailbox_prepare_record(&first, &record) &&
              modem_sms_state_multipart_group_take_quarantine(&record) &&
              modem_sms_state_arrival_scan_commit(
                  true, true, 20u, &arrivals) && arrivals == 1u,
          "deleted-anchor fixture announces its original orphan");
    check(modem_sms_state_pending_arrival_track(second.index),
          "late segment is tracked after its anchor was deleted");
    modem_sms_state_arrival_scan_begin(21u);
    check(!modem_sms_state_mailbox_prepare_record(&second, &record) &&
              modem_sms_state_multipart_group_take_quarantine(&record) &&
              modem_sms_state_arrival_scan_commit(
                  true, true, 21u, &arrivals) && arrivals == 1u,
          "missing physical anchor makes debounce fail open, never silent");

    modem_sms_state_init();
    for (uint8_t i = 0u; i <= TEST_MULTIPART_ARRIVAL_CLAIM_MAX; i++) {
        modem_sms_message_t claim_first = text_segment_with_ref(
            (uint16_t)(100u + i), 1u, 2u, (uint16_t)(0x100u + i),
            true, "first");
        check(modem_sms_state_pending_arrival_track(claim_first.index),
              "claim saturation tracks each first segment");
    }
    modem_sms_state_arrival_scan_begin(30u);
    for (uint8_t i = 0u; i <= TEST_MULTIPART_ARRIVAL_CLAIM_MAX; i++) {
        modem_sms_message_t claim_first = text_segment_with_ref(
            (uint16_t)(100u + i), 1u, 2u, (uint16_t)(0x100u + i),
            true, "first");
        check(!modem_sms_state_mailbox_prepare_record(
                  &claim_first, &record),
              "claim saturation retains each incomplete group");
    }
    while (modem_sms_state_multipart_group_take_quarantine(&record)) {
    }
    check(modem_sms_state_arrival_scan_commit(
              true, true, 30u, &arrivals) &&
              arrivals == TEST_MULTIPART_ARRIVAL_CLAIM_MAX + 1u,
          "claim saturation never suppresses a first notification");
    for (uint8_t i = 0u; i <= TEST_MULTIPART_ARRIVAL_CLAIM_MAX; i++) {
        modem_sms_message_t claim_second = text_segment_with_ref(
            (uint16_t)(140u + i), 2u, 2u, (uint16_t)(0x100u + i),
            true, "second");
        check(modem_sms_state_pending_arrival_track(claim_second.index),
              "claim saturation tracks each second segment");
    }
    modem_sms_state_arrival_scan_begin(31u);
    for (uint8_t i = 0u; i <= TEST_MULTIPART_ARRIVAL_CLAIM_MAX; i++) {
        modem_sms_message_t claim_first = text_segment_with_ref(
            (uint16_t)(100u + i), 1u, 2u, (uint16_t)(0x100u + i),
            true, "first");
        modem_sms_message_t claim_second = text_segment_with_ref(
            (uint16_t)(140u + i), 2u, 2u, (uint16_t)(0x100u + i),
            true, "second");
        check(!modem_sms_state_mailbox_prepare_record(
                  &claim_first, &record) &&
                  modem_sms_state_mailbox_prepare_record(
                      &claim_second, &record),
              "claim saturation rebuilds every complete logical message");
    }
    check(modem_sms_state_arrival_scan_commit(
              true, true, 31u, &arrivals) && arrivals == 1u,
          "a full claim table fails open for only the untracked logical message");
}

static void test_filtered_cleanup_lifecycle(void) {
    modem_sms_state_init();
    for (uint16_t index = 0u; index < MODEM_SMS_RECORD_MAX; index++) {
        check(modem_sms_state_filtered_queue(index),
              "filtered queue accepts every ME slot");
    }
    check(modem_sms_state_filtered_queue(0u) &&
              !modem_sms_state_filtered_queue(999u) &&
              modem_sms_state_filtered_count() == MODEM_SMS_RECORD_MAX,
          "filtered queue coalesces duplicates and bounds overflow");
    modem_sms_filtered_summary_t summary;
    modem_sms_state_filtered_finish(&summary);
    check(summary.count == MODEM_SMS_RECORD_MAX && !summary.delete_ok,
          "filtered overflow is retained in cleanup summary");

    modem_sms_state_filtered_reset();
    check(modem_sms_state_filtered_queue(0u) &&
              modem_sms_state_filtered_queue(7u) &&
              modem_sms_state_filtered_queue(9u) &&
              modem_sms_state_filtered_begin(),
          "filtered cleanup begins with the first queued control");
    uint16_t index = UINT16_MAX;
    check(modem_sms_state_filtered_active() &&
              modem_sms_state_filtered_current(&index) && index == 0u &&
              modem_sms_state_filtered_advance(true) &&
              modem_sms_state_filtered_current(&index) && index == 7u,
          "successful filtered delete advances in queue order");
    check(modem_sms_state_filtered_advance(false) &&
              modem_sms_state_filtered_current(&index) && index == 9u &&
              !modem_sms_state_filtered_advance(true),
          "failed filtered delete is recorded while later rows continue");
    modem_sms_state_filtered_finish(&summary);
    check(summary.count == 3u && !summary.delete_ok &&
              !modem_sms_state_filtered_active() &&
              !modem_sms_state_filtered_current(&index) &&
              !modem_sms_state_filtered_current(NULL),
          "filtered finish reports failure and retires active traversal");

    modem_sms_state_filtered_reset();
    check(!modem_sms_state_filtered_begin() &&
              modem_sms_state_filtered_queue(7u) &&
              modem_sms_state_filtered_begin(),
          "a later scan can requeue a previously failed control");
    modem_sms_state_filtered_note_failure();
    modem_sms_state_filtered_finish(&summary);
    check(summary.count == 1u && !summary.delete_ok,
          "timeout-style filtered failure remains retry-visible");
}

int main(void) {
    test_mailbox_capacity_and_index_zero();
    test_terminal_result_latches();
    test_scan_collection_integrity();
    test_selected_read_assembly();
    test_mailbox_record_preparation();
    test_binary_submit_state();
    test_pending_arrival_reconciliation();
    test_multipart_arrival_reconciliation();
    test_multipart_arrival_across_scans();
    test_filtered_cleanup_lifecycle();
    if (s_failures != 0) {
        return 1;
    }
    puts("test_modem_sms_state: OK");
    return 0;
}

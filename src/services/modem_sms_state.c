#include "modem_sms_state_internal.h"

#include <string.h>
#include "services/sms_submit_codec.h"

static char s_binary_pdu_hex[SMS_SUBMIT_PDU_HEX_MAX + 1u];
static uint16_t s_binary_position;
static uint8_t s_binary_tpdu_len, s_binary_segment, s_binary_segment_total;
/* Preserve the concatenation reference across modem session restarts. */
static uint8_t s_binary_reference;
static bool s_binary_send_ok;
static modem_sms_send_result_t s_send_result;
static bool s_send_result_pending;
static uint32_t s_next_request_id, s_reserved_request_id;
static modem_sms_request_kind_t s_reserved_kind;

static bool send_kind(modem_sms_request_kind_t kind) {
    return kind == MODEM_SMS_REQUEST_SEND_TEXT || kind == MODEM_SMS_REQUEST_SEND_BINARY;
}
static bool claim_terminal(uint32_t id, modem_sms_request_kind_t kind) {
    if (id == 0u || !send_kind(kind) || s_reserved_request_id != id || s_reserved_kind != kind) return false;
    s_reserved_request_id = 0u;
    s_reserved_kind = MODEM_SMS_REQUEST_NONE;
    return true;
}
void modem_sms_state_init(void) {
    memset(&s_send_result, 0, sizeof(s_send_result));
    s_send_result_pending = false;
    s_reserved_request_id = 0u;
    s_reserved_kind = MODEM_SMS_REQUEST_NONE;
}
bool modem_sms_state_reserve_request(modem_sms_request_kind_t kind, uint32_t *out) {
    if (out != NULL) *out = 0u;
    if (out == NULL || !send_kind(kind) || s_reserved_request_id != 0u || s_send_result_pending) return false;
    do { s_next_request_id++; } while (s_next_request_id == 0u);
    s_reserved_request_id = s_next_request_id;
    s_reserved_kind = kind;
    *out = s_reserved_request_id;
    return true;
}
bool modem_sms_state_release_request(uint32_t id, modem_sms_request_kind_t kind) {
    return claim_terminal(id, kind);
}

uint8_t modem_sms_state_binary_begin(uint16_t payload_len) {
    s_binary_position = 0u;
    s_binary_segment = 1u;
    s_binary_segment_total = (uint8_t)(
        (payload_len + (MODEM_SMS_BINARY_CHUNK_MAX - 1u)) /
        MODEM_SMS_BINARY_CHUNK_MAX);
    if (s_binary_segment_total == 0u) {
        s_binary_segment_total = 1u;
    }
    s_binary_reference++;
    if (s_binary_reference == 0u) {
        s_binary_reference = 1u;
    }
    s_binary_send_ok = false;
    return s_binary_segment_total;
}

bool modem_sms_state_binary_build_segment(
    const char *number, const uint8_t *payload, uint16_t payload_len,
    uint16_t dest_port, uint16_t source_port,
    modem_binary_sms_mode_t mode) {
    sms_submit_pdu_t submit = {
        .number = number,
        .payload = payload,
        .payload_len = payload_len,
        .position = s_binary_position,
        .dest_port = dest_port,
        .source_port = source_port,
        .mode = mode,
        .segment = s_binary_segment,
        .segment_total = s_binary_segment_total,
        .reference = s_binary_reference,
    };
    if (!sms_submit_pdu_build(&submit, s_binary_pdu_hex,
                              sizeof(s_binary_pdu_hex),
                              &s_binary_tpdu_len)) {
        return false;
    }
    s_binary_position = submit.position;
    s_binary_segment = submit.segment;
    return true;
}

void modem_sms_state_binary_segment_view(
    modem_sms_binary_segment_view_t *out) {
    if (out == NULL) {
        return;
    }
    out->pdu_hex = s_binary_pdu_hex;
    out->tpdu_len = s_binary_tpdu_len;
    /* sms_submit_pdu_build() already post-incremented s_binary_segment before
     * the '>' prompt fires. The -1u recovers the one-based segment just written.
     * Do NOT drop it: that is a real off-by-one, not display decoration. */
    out->segment = s_binary_segment > 0u
        ? (uint8_t)(s_binary_segment - 1u) : 0u;
    out->segment_total = s_binary_segment_total;
}

bool modem_sms_state_picture_build_text_segment(
    const char *number, const uint8_t *payload, uint16_t payload_len,
    uint16_t dest_port, uint16_t source_port) {
    sms_submit_pdu_t submit = {
        .number = number, .payload = payload, .payload_len = payload_len,
        .position = s_binary_position, .dest_port = dest_port,
        .source_port = source_port, .mode = MODEM_BINARY_SMS_MODE_DCS04_PORT_FIRST,
        .segment = s_binary_segment, .segment_total = s_binary_segment_total,
        .reference = s_binary_reference,
    };
    if (!sms_submit_picture_text_build(&submit, s_binary_pdu_hex,
                                       sizeof(s_binary_pdu_hex))) {
        return false;
    }
    s_binary_tpdu_len = 0u; /* The body is user data, not a TPDU. */
    s_binary_position = submit.position;
    s_binary_segment = submit.segment;
    return true;
}

bool modem_sms_state_binary_has_more(uint16_t payload_len) {
    return s_binary_position < payload_len;
}

void modem_sms_state_binary_set_send_ok(bool sent_ok) {
    s_binary_send_ok = sent_ok;
}

bool modem_sms_state_binary_send_ok(void) {
    return s_binary_send_ok;
}

bool modem_sms_state_publish_send_result(uint32_t request_id,
                                         modem_sms_request_kind_t kind,
                                         modem_sms_outcome_t outcome) {
    if ((kind != MODEM_SMS_REQUEST_SEND_TEXT &&
         kind != MODEM_SMS_REQUEST_SEND_BINARY) ||
        outcome == MODEM_SMS_OUTCOME_NONE ||
        !claim_terminal(request_id, kind)) {
        return false;
    }
    s_send_result = (modem_sms_send_result_t){
        .request_id = request_id,
        .kind = kind,
        .outcome = outcome,
    };
    s_send_result_pending = true;
    return true;
}

bool modem_sms_state_pop_send_result(uint32_t request_id,
                                     modem_sms_send_result_t *out) {
    if (request_id == 0u || out == NULL || !s_send_result_pending ||
        s_send_result.request_id != request_id) {
        return false;
    }
    *out = s_send_result;
    memset(&s_send_result, 0, sizeof(s_send_result));
    s_send_result_pending = false;
    return true;
}

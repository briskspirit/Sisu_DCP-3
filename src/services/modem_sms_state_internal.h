#ifndef MODEM_SMS_STATE_INTERNAL_H
#define MODEM_SMS_STATE_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>
#include "services/sms_types.h"

typedef struct {
    const char *pdu_hex;
    uint8_t tpdu_len, segment, segment_total;
} modem_sms_binary_segment_view_t;

/* Send-transport state only. Local mailbox state belongs to message_service.
 * The root modem service serializes cross-core result access with s_sms_lock. */
void modem_sms_state_init(void);
bool modem_sms_state_reserve_request(modem_sms_request_kind_t kind, uint32_t *request_id);
bool modem_sms_state_release_request(uint32_t request_id, modem_sms_request_kind_t kind);
bool modem_sms_state_publish_send_result(uint32_t request_id, modem_sms_request_kind_t kind,
                                         modem_sms_outcome_t outcome);
bool modem_sms_state_pop_send_result(uint32_t request_id, modem_sms_send_result_t *out);
uint8_t modem_sms_state_binary_begin(uint16_t payload_len);
bool modem_sms_state_binary_build_segment(const char *number, const uint8_t *payload,
    uint16_t payload_len, uint16_t dest_port, uint16_t source_port, modem_binary_sms_mode_t mode);
bool modem_sms_state_picture_build_text_segment(const char *number, const uint8_t *payload,
    uint16_t payload_len, uint16_t dest_port, uint16_t source_port);
void modem_sms_state_binary_segment_view(modem_sms_binary_segment_view_t *out);
bool modem_sms_state_binary_has_more(uint16_t payload_len);
void modem_sms_state_binary_set_send_ok(bool ok);
bool modem_sms_state_binary_send_ok(void);

#endif

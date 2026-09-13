#ifndef MODEM_SMS_STATE_INTERNAL_H
#define MODEM_SMS_STATE_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>

#include "services/sms_picture_codec.h"
#include "services/sms_types.h"

typedef enum {
    MODEM_SMS_ARRIVAL_UNSEEN = 0,
    MODEM_SMS_ARRIVAL_USER,
    MODEM_SMS_ARRIVAL_FILTERED,
} modem_sms_arrival_result_t;

typedef struct {
    uint8_t count;
    bool delete_ok;
} modem_sms_filtered_summary_t;

typedef struct {
    const modem_sms_message_t *message;
    uint8_t list_status;
    bool collecting_body;
    bool decoded;
} modem_sms_collected_view_t;

typedef struct {
    const char *pdu_hex;
    uint8_t tpdu_len;
    uint8_t segment;
    uint8_t segment_total;
} modem_sms_binary_segment_view_t;

/* Pure, single-modem SMS state. The modem service retains s_sms_lock and must
 * serialize every call that crosses cores. This module owns no transport,
 * vendor, status, or power behavior. */
void modem_sms_state_init(void);

/* Every admitted SMS request owns exactly one typed result slot. A channel is
 * single-flight until its terminal is consumed, which keeps the selected-read
 * payload and mailbox cache bounded and unambiguous without a large union
 * journal. */
bool modem_sms_state_reserve_request(modem_sms_request_kind_t kind,
                                     uint32_t *request_id_out);
bool modem_sms_state_release_request(uint32_t request_id,
                                     modem_sms_request_kind_t kind);

void modem_sms_state_mailbox_clear(void);
bool modem_sms_state_mailbox_append(const modem_sms_record_t *record);
uint8_t modem_sms_state_mailbox_count(void);
bool modem_sms_state_mailbox_record(uint8_t position,
                                    modem_sms_record_t *out);
void modem_sms_state_multipart_groups_reset(void);
bool modem_sms_state_multipart_groups_pending(void);
/* Retires one picture or text group that remained incomplete at the
 * authoritative CMGL final. The returned Data-message record owns every
 * observed index in wire order. */
bool modem_sms_state_multipart_group_take_quarantine(
    modem_sms_record_t *out);
/* Returns true only when one logical mailbox record is ready to append. A
 * valid multipart segment may be consumed while its group remains pending. */
bool modem_sms_state_mailbox_prepare_record(
    const modem_sms_message_t *message, modem_sms_record_t *out);
bool modem_sms_state_mailbox_prepare_quarantine(
    const modem_sms_message_t *message, modem_sms_record_t *out);

void modem_sms_state_scan_reset(void);
void modem_sms_state_scan_mark_incomplete(void);
bool modem_sms_state_scan_incomplete(void);
uint8_t modem_sms_state_scan_raw_count(void);
void modem_sms_state_collection_abort_body(void);
void modem_sms_state_mailbox_row_begin(bool index_valid, uint16_t index,
                                       bool status_valid,
                                       uint8_t list_status);
void modem_sms_state_detail_row_begin(uint16_t index);
void modem_sms_state_detail_header(const char *status, const char *sender,
                                   const char *timestamp);
void modem_sms_state_collection_feed_decoded(
    const sms_codec_message_t *decoded);
void modem_sms_state_collection_feed_text(const char *line);
void modem_sms_state_collection_set_status(const char *status);
void modem_sms_state_collection_view(modem_sms_collected_view_t *out);

void modem_sms_state_selected_begin(uint16_t first_index,
                                    uint8_t index_count,
                                    bool quarantined);
/* The request retains the immutable index array; this owner retains only its
 * traversal position and validates the span on every advance. */
bool modem_sms_state_selected_next_index(const uint16_t *indices,
                                         uint8_t index_count,
                                         uint16_t *index_out);
void modem_sms_state_selected_fail(void);
bool modem_sms_state_selected_accept_segment(
    const modem_sms_message_t *segment);
bool modem_sms_state_selected_accept_undecoded(
    const modem_sms_message_t *segment);
bool modem_sms_state_selected_complete(void);
bool modem_sms_state_selected_publish_result(uint32_t request_id,
                                             modem_sms_request_kind_t kind,
                                             modem_sms_outcome_t outcome,
                                             bool sim_not_ready,
                                             uint32_t expected_identity_hash,
                                             bool *identity_mismatch_out);

uint8_t modem_sms_state_binary_begin(uint16_t payload_len);
bool modem_sms_state_binary_build_segment(
    const char *number, const uint8_t *payload, uint16_t payload_len,
    uint16_t dest_port, uint16_t source_port,
    modem_binary_sms_mode_t mode);
void modem_sms_state_binary_segment_view(
    modem_sms_binary_segment_view_t *out);
bool modem_sms_state_binary_has_more(uint16_t payload_len);
void modem_sms_state_binary_set_send_ok(bool sent_ok);
bool modem_sms_state_binary_send_ok(void);

void modem_sms_state_pending_arrivals_reset(void);
bool modem_sms_state_pending_arrival_track(uint16_t index);
void modem_sms_state_arrival_scan_begin(uint32_t revision);
void modem_sms_state_arrival_scan_note(uint16_t index,
                                      modem_sms_arrival_result_t result);
bool modem_sms_state_arrival_scan_commit(bool complete, bool inbox,
                                         uint32_t current_revision,
                                         uint32_t *user_arrivals_out);

void modem_sms_state_filtered_reset(void);
bool modem_sms_state_filtered_queue(uint16_t index);
uint8_t modem_sms_state_filtered_count(void);
bool modem_sms_state_filtered_begin(void);
bool modem_sms_state_filtered_active(void);
bool modem_sms_state_filtered_current(uint16_t *index_out);
bool modem_sms_state_filtered_advance(bool delete_ok);
void modem_sms_state_filtered_note_failure(void);
void modem_sms_state_filtered_finish(modem_sms_filtered_summary_t *summary);

bool modem_sms_state_publish_mailbox_result(
    uint32_t request_id, modem_sms_request_kind_t kind,
    modem_sms_outcome_t outcome, bool sim_not_ready, bool complete,
    modem_sms_mailbox_t mailbox);
bool modem_sms_state_pop_mailbox_result(uint32_t request_id,
                                        modem_sms_mailbox_result_t *out);

bool modem_sms_state_publish_read_result(
    uint32_t request_id, modem_sms_request_kind_t kind,
    modem_sms_outcome_t outcome, bool sim_not_ready,
    uint32_t request_identity_hash, uint32_t identity_hash,
    const modem_sms_message_t *message);
bool modem_sms_state_pop_read_result(uint32_t request_id,
                                     modem_sms_read_result_t *out);

bool modem_sms_state_publish_send_result(uint32_t request_id,
                                         modem_sms_request_kind_t kind,
                                         modem_sms_outcome_t outcome);
bool modem_sms_state_pop_send_result(uint32_t request_id,
                                     modem_sms_send_result_t *out);

bool modem_sms_state_publish_save_result(uint32_t request_id,
                                         modem_sms_request_kind_t kind,
                                         modem_sms_outcome_t outcome,
                                         bool sim_not_ready);
bool modem_sms_state_pop_save_result(uint32_t request_id,
                                     modem_sms_save_result_t *out);

bool modem_sms_state_publish_delete_result(uint32_t request_id,
                                           modem_sms_request_kind_t kind,
                                           modem_sms_outcome_t outcome,
                                           bool sim_not_ready);
bool modem_sms_state_pop_delete_result(uint32_t request_id,
                                       modem_sms_delete_result_t *out);

#endif /* MODEM_SMS_STATE_INTERNAL_H */

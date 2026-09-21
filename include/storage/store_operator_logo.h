#ifndef STORE_OPERATOR_LOGO_H
#define STORE_OPERATOR_LOGO_H

#include "storage/store_service.h"
#include "services/operator_logo_codec.h"

store_status_t store_operator_logo_receive(const sms_codec_message_t *part, uint32_t now_ms);
store_status_t store_operator_logo_receive_pdu(const char *pdu, uint32_t now_ms);
/* OK proves this exact part is durable; CONFLICT marks an invalidated receipt,
 * while NOT_FOUND requires re-admission. Neither permits deleting the ME copy. */
store_status_t store_operator_logo_received_pdu_status(const char *pdu);
store_status_t store_operator_logo_commit_status(void);
void store_operator_logo_expire(uint32_t now_ms);
uint32_t store_operator_logo_pending_first(void);
store_status_t store_operator_logo_pending_get(uint32_t id, operator_logo_t *out);
store_status_t store_operator_logo_pending_save(uint32_t id);
store_status_t store_operator_logo_pending_discard(uint32_t id);
/* Returns only the last durably committed saved image. */
store_status_t store_operator_logo_get(operator_logo_t *out);
/* Clears the saved image, preserving receive receipts and pending messages.
 * OK queues the change; commit_status proves its durability. */
store_status_t store_operator_logo_clear(void);

#endif

#ifndef MESSAGE_SERVICE_H
#define MESSAGE_SERVICE_H

#include "services/message_types.h"

typedef enum { MESSAGE_OP_LIST, MESSAGE_OP_READ, MESSAGE_OP_SAVE, MESSAGE_OP_DELETE,
               MESSAGE_OP_COUNT } message_op_t;
typedef enum { MESSAGE_RESULT_OK, MESSAGE_RESULT_ERROR, MESSAGE_RESULT_FULL } message_result_code_t;
typedef struct {
    uint32_t request_id;
    uint32_t object_id;
    message_op_t kind;
    message_result_code_t outcome;
    uint16_t count;
    uint32_t revision;
} message_result_t;
typedef struct {
    bool ready;
    bool storage_error;
    bool full;
    uint16_t inbox, outbox, unread, pending;
    uint8_t queued;
    uint32_t revision, received, full_events, receive_errors, filtered_controls;
} message_status_t;

/* Core0 only. Receive admission copies the normalized DELIVER; it is not a
 * durability acknowledgement. tick runs in the shared flash-write window. */
void message_service_init(void);
void message_service_tick(uint32_t now);
bool message_service_idle(void);
bool message_service_receive(const char *pdu);
void message_service_note_receive_loss(void);
/* Queue a copy only after the modem confirms transmission. A storage failure
 * must not turn an accepted transmission into a retryable send failure. */
bool message_service_sent(const char *address, const char *text);
void message_service_get_status(message_status_t *out);
/* Synchronous access is restricted to the storage owner's safe window. */
bool message_service_entry(message_mailbox_t mailbox, uint16_t position,
                           message_metadata_t *out);
/* The caller owns rows until this token completes. No partial list is valid. */
bool message_service_request_list(message_mailbox_t mailbox, message_metadata_t *rows,
                                  uint16_t capacity, uint32_t *token);
bool message_service_request_read(message_mailbox_t mailbox, uint32_t id, uint32_t *token);
bool message_service_request_delete(message_mailbox_t mailbox, uint32_t id, uint32_t *token);
bool message_service_request_save(const char *address, const char *text, uint32_t *token);
/* One outstanding request per kind, including its unconsumed result. A read's
 * content is copied only for the matching token; other operations pass NULL. */
bool message_service_pop_result(uint32_t token, message_result_t *out,
                                message_content_t *content);

#endif

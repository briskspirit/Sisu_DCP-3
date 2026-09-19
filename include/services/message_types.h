#ifndef MESSAGE_TYPES_H
#define MESSAGE_TYPES_H

#include <stdbool.h>
#include <stdint.h>
#include "services/sms_types.h"

#define MESSAGE_MAILBOX_LIMIT 500u
#define MESSAGE_TEXT_MAX (MODEM_SMS_DECODED_TEXT_MAX * MODEM_SMS_SEGMENT_MAX)

typedef enum { MESSAGE_INBOX, MESSAGE_OUTBOX } message_mailbox_t;

typedef struct {
    uint32_t id;
    bool read;
    bool sent;
    bool binary;
    bool quarantined;
    char address[MODEM_SMS_SENDER_MAX + 1u];
    char timestamp[MODEM_SMS_TIMESTAMP_MAX + 1u];
} message_metadata_t;

typedef struct {
    message_metadata_t metadata;
    char text[MESSAGE_TEXT_MAX + 1u];
} message_content_t;

#endif

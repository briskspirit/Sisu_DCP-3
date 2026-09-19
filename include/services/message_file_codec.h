#ifndef MESSAGE_FILE_CODEC_H
#define MESSAGE_FILE_CODEC_H

#include <stddef.h>
#include "services/message_types.h"

/* Normal messages contain at most eight parts. Extra slots retain conflicting
 * duplicates as one quarantined object instead of splicing unrelated text. */
#define MESSAGE_FILE_PART_LIMIT 16u
#define MESSAGE_FILE_PDU_MAX 191u
#define MESSAGE_FILE_WIRE_MAX (8u + MESSAGE_FILE_PART_LIMIT * (1u + MESSAGE_FILE_PDU_MAX))
#define MESSAGE_FILE_DRAFT 1u
#define MESSAGE_FILE_QUARANTINED 2u
#define MESSAGE_FILE_SENT 4u

typedef struct {
    uint8_t size;
    uint8_t bytes[MESSAGE_FILE_PDU_MAX];
} message_file_part_t;

typedef struct {
    uint8_t flags;
    uint8_t count;
    union {
        message_file_part_t parts[MESSAGE_FILE_PART_LIMIT];
        struct {
            char address[MODEM_SMS_SENDER_MAX + 1u];
            char text[MODEM_SMS_TEXT_MAX + 1u];
        } draft;
    };
} message_file_t;

typedef enum {
    MESSAGE_MERGE_UNRELATED,
    MESSAGE_MERGE_ADDED,
    MESSAGE_MERGE_DUPLICATE,
    MESSAGE_MERGE_FULL,
} message_merge_t;

bool message_file_draft(message_file_t *file, const char *address, const char *text);
bool message_file_receive(message_file_t *file, const char *pdu);
message_merge_t message_file_merge(message_file_t *file, const char *pdu);
bool message_file_complete(const message_file_t *file);
bool message_file_encode(const message_file_t *file, uint8_t *dst, size_t cap, size_t *len);
bool message_file_decode(message_file_t *file, const uint8_t *src, size_t len);
bool message_file_metadata(const message_file_t *file, message_metadata_t *out);
bool message_file_content(const message_file_t *file, message_content_t *out);
/* Seconds since 2000-01-01, ignoring the display timestamp's absent zone.
 * Used only for ordering and a bounded same-SMSC concatenation window. */
uint32_t message_timestamp_seconds(const char *timestamp);

#endif

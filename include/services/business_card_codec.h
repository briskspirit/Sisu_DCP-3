#ifndef BUSINESS_CARD_CODEC_H
#define BUSINESS_CARD_CODEC_H

#include "services/message_file_codec.h"
#include "services/phonebook_types.h"

#define BUSINESS_CARD_PAYLOAD_MAX 1024u

typedef enum {
    BUSINESS_CARD_NONE,
    BUSINESS_CARD_VALID,
    BUSINESS_CARD_INVALID,
} business_card_result_t;

/* UTF-8 application text, sent through the normal GSM7/UCS2 SMS adapter. */
bool business_card_encode(const phonebook_entry_t *entry, char *out, size_t cap);
bool business_card_decode(const uint8_t *body, size_t len, bool utf8,
                          phonebook_entry_t *out);
/* Recognized malformed cards never fall through into the text Inbox.
 * Sole core0 owner; bounded static scratch, like message_file_codec. */
business_card_result_t business_card_file_decode(const message_file_t *file,
                                                phonebook_entry_t *out);
bool business_card_text_envelope(const char *text);

#endif

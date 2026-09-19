#ifndef SMS_PICTURE_CODEC_H
#define SMS_PICTURE_CODEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "services/picture_message_types.h"
#include "services/sms_types.h"

#define SMS_CODEC_ADDRESS_MAX MODEM_SMS_SENDER_MAX
#define SMS_CODEC_TEXT_MAX MODEM_SMS_DECODED_TEXT_MAX
#define SMS_CODEC_TIMESTAMP_MAX MODEM_SMS_TIMESTAMP_MAX
#define SMS_CODEC_BINARY_MAX MODEM_SMS_BINARY_MAX
#define SMS_CODEC_PICTURE_PORT 0x158au

typedef struct {
    bool submit;
    bool binary;
    bool has_ports;
    bool has_concat;
    bool concat_ref_16bit;
    bool udh_unhandled; /* malformed, duplicate or unsupported UDH information */
    bool trailing_data; /* bytes beyond the user-data length declared by TP-UDL */
    uint8_t pid;
    uint8_t dcs;
    uint16_t dest_port;
    uint16_t source_port;
    uint16_t concat_ref;
    uint8_t concat_total;
    uint8_t concat_seq;
    char address[SMS_CODEC_ADDRESS_MAX + 1u];
    char timestamp[SMS_CODEC_TIMESTAMP_MAX + 1u];
    /* Wire DCS makes these mutually exclusive. Keeping them overlaid also
     * leaves enough room for worst-case UTF-8 without growing this transient. */
    union {
        char text[SMS_CODEC_TEXT_MAX + 1u];
        uint8_t binary_data[SMS_CODEC_BINARY_MAX];
    };
    uint16_t binary_len;
} sms_codec_message_t;

bool sms_picture_payload_encode(const store_picture_message_t *message,
                                const char *text,
                                uint8_t *dst,
                                size_t cap,
                                uint16_t *out_len,
                                uint8_t *out_chunks);
bool sms_picture_payload_decode(const uint8_t *payload,
                                uint16_t payload_len,
                                store_picture_message_t *out_message);
bool sms_pdu_decode(const char *hex, sms_codec_message_t *out_message);
/* Strict UTF-8 conversions: no substitution or silent truncation. */
bool sms_gsm7_from_utf8(const char *text, uint8_t *out, size_t cap, size_t *length);
bool sms_ucs2_from_utf8(const char *text, uint8_t *out, size_t cap, size_t *length);

#endif

#ifndef SMS_SUBMIT_CODEC_H
#define SMS_SUBMIT_CODEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "services/sms_types.h"

#define SMS_SUBMIT_PDU_HEX_MAX 384u

/* A caller-owned view of one logical submit. The codec advances position and
 * segment only after a complete segment has been emitted; all other fields are
 * immutable transaction inputs. */
typedef struct {
    const char *number;
    const uint8_t *payload;
    uint16_t payload_len;
    uint16_t position;
    uint16_t dest_port;
    uint16_t source_port;
    modem_binary_sms_mode_t mode;
    uint8_t segment;
    uint8_t segment_total;
    uint8_t reference;
} sms_submit_pdu_t;

bool sms_submit_pdu_build(sms_submit_pdu_t *submit,
                          char *hex,
                          size_t hex_cap,
                          uint8_t *out_tpdu_len);

/* User data only for text-mode CMGS, preserving the same binary UDH.
 * Binary modes return hex octets; the GSM7 diagnostic mode returns characters. */
bool sms_submit_text_build(sms_submit_pdu_t *submit,
                           char *body, size_t body_cap);

/* Exposed as codec primitives so their 3GPP bounds and fixed vectors can be
 * tested without reaching through the modem service translation unit. */
uint8_t sms_submit_gsm7_code(uint8_t ch);
bool sms_submit_pack_gsm7(const uint8_t *src,
                          uint8_t septets,
                          uint8_t *dst,
                          uint8_t *out_len);
bool sms_submit_append_hex_byte(char *dst,
                                size_t cap,
                                size_t *pos,
                                uint8_t value);
bool sms_submit_append_address(uint8_t *dst,
                               size_t cap,
                               size_t *pos,
                               const char *number);

void sms_submit_format_text_command(char *dst,
                                    size_t dst_len,
                                    const char *command,
                                    const char *address,
                                    bool sent_status);

#endif

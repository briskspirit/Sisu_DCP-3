#ifndef MODEM_SMS_DIRECT_H
#define MODEM_SMS_DIRECT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "services/sms_deliver_codec.h"
#include "services/sms_control_filter.h"

#define MODEM_SMS_DIRECT_LINE_MAX 400u

typedef enum {
    MODEM_SMS_DIRECT_NOT_MINE = 0,
    MODEM_SMS_DIRECT_ACCEPTED,
    MODEM_SMS_DIRECT_REJECTED,
    /* A valid plain-text prefix: a later line break may complete the body.
     * Malformed or unsupported deliveries must return REJECTED instead. */
    MODEM_SMS_DIRECT_INCOMPLETE,
} modem_sms_direct_translate_result_t;

/* Vendor hook: header is the +CMT line; payload is the body as a byte range
 * (never a C string: a plain body may contain 0x00 or CR/LF). */
typedef modem_sms_direct_translate_result_t (*modem_sms_direct_translate_fn)(
    const char *header, const uint8_t *payload, size_t payload_len,
    sms_deliver_t *out);

typedef enum {
    MODEM_SMS_DIRECT_STEP_IGNORED = 0, /* line/byte is not part of a direct delivery */
    MODEM_SMS_DIRECT_STEP_HEADER,      /* header captured, payload expected next */
    MODEM_SMS_DIRECT_STEP_READY,       /* pdu_hex/tpdu_len filled */
    MODEM_SMS_DIRECT_STEP_REJECTED,    /* header+payload seen, no parser accepted */
    MODEM_SMS_DIRECT_STEP_FILTERED,    /* complete control; no storage or error */
} modem_sms_direct_step_t;

void modem_sms_direct_reset(void);
/* A delivery is in progress (payload expected, in line or RAW mode). */
bool modem_sms_direct_pending(void);
/* Reason for the last FILTERED step; cleared on reset or a new header. */
sms_control_filter_t modem_sms_direct_filter_reason(void);
/* RAW mode: the byte drain must hand every byte to modem_sms_direct_feed_raw
 * instead of the line framer until this returns false. */
bool modem_sms_direct_raw_active(void);
modem_sms_direct_step_t modem_sms_direct_feed(
    const char *line, modem_sms_direct_translate_fn vendor,
    char *pdu_hex, size_t pdu_hex_cap, uint8_t *tpdu_len_out);
modem_sms_direct_step_t modem_sms_direct_feed_raw(
    uint8_t byte, modem_sms_direct_translate_fn vendor,
    char *pdu_hex, size_t pdu_hex_cap, uint8_t *tpdu_len_out);
/* True once if the last raw feed ended at a bound without consuming byte.
 * The host must feed that same byte to its normal line/prompt parser. */
bool modem_sms_direct_raw_take_unconsumed(void);

/* Generic 3GPP 27.005 forms, exposed for tests. */
bool modem_sms_direct_parse_3gpp_pdu(const char *header, const uint8_t *payload,
                                     size_t payload_len, char *pdu_hex,
                                     size_t pdu_hex_cap, uint8_t *tpdu_len_out);
bool modem_sms_direct_parse_3gpp_text(const char *header, const uint8_t *payload,
                                      size_t payload_len, sms_deliver_t *out);
bool modem_sms_direct_hex_to_bytes(const uint8_t *hex, size_t hex_len,
                                   uint8_t *dst, size_t cap, size_t *out_len);
/* TP-UDL for a GSM-7 hex body of `octets` bytes whose header said `length`
 * septets: the value in {length, length+1, length-1} (in that order) whose
 * packed size ceil(udl*7/8) equals octets. The octet count is authoritative:
 * a module's text-form <length> can be off by one on UDH parts (bench). */
bool modem_sms_direct_gsm7_udl_for_octets(uint32_t length, size_t octets,
                                          uint8_t *udl_out);

#endif

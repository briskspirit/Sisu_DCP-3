#ifndef SMS_DELIVER_CODEC_H
#define SMS_DELIVER_CODEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "services/sms_picture_codec.h"

#define SMS_DELIVER_UD_MAX 140u
#define SMS_DELIVER_HEX_MAX 384u   /* "00" + up to 176 TPDU bytes as hex + NUL */

typedef struct {
    char address[SMS_CODEC_ADDRESS_MAX + 1u]; /* BCD digits (leading '+' allowed) or GSM text for TON 5 */
    uint8_t toa;        /* 0x91 international, 0x81 unknown/national, 0xD0 alphanumeric; 0 = derive */
    uint8_t scts[7];    /* TP-SCTS semi-octets, copied verbatim */
    uint8_t pid;
    uint8_t dcs;
    bool udhi;
    bool wdp;          /* vendor identified a WDP datagram; not encoded in TPDU */
    uint8_t udl;        /* septets for GSM-7 DCS, octets otherwise */
    uint8_t ud[SMS_DELIVER_UD_MAX];
    uint8_t ud_len;     /* octets used in ud[] */
} sms_deliver_t;

bool sms_deliver_scts_encode(uint16_t year, uint8_t month, uint8_t day,
                             uint8_t hour, uint8_t minute, uint8_t second,
                             int8_t tz_quarters, uint8_t out[7]);
bool sms_deliver_pack_gsm7(const uint8_t *codes, uint8_t septets,
                           uint8_t *dst, size_t cap, uint8_t *out_len);
/* Latin-1 byte -> GSM 03.38 code(s). Returns 0 when no mapping exists. */
uint8_t sms_deliver_gsm7_from_latin1(uint8_t ch, uint8_t out[2]);
bool sms_deliver_build(const sms_deliver_t *deliver, char *hex,
                       size_t hex_cap, uint8_t *out_tpdu_len);

#endif

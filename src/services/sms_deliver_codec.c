/* SMS-DELIVER (3GPP TS 23.040 9.2.2.1) TPDU builder. Pure: no modem or
 * vendor knowledge. Consumers rebuild a received message that the module
 * handed over out-of-band (+CMT) so it can be stored as an ordinary PDU. */

#include "services/sms_deliver_codec.h"

#include <string.h>

static uint8_t semi_octet(uint8_t value) {
    return (uint8_t)(((value % 10u) << 4) | (value / 10u));
}

bool sms_deliver_scts_encode(uint16_t year, uint8_t month, uint8_t day,
                             uint8_t hour, uint8_t minute, uint8_t second,
                             int8_t tz_quarters, uint8_t out[7]) {
    if (out == NULL || month < 1u || month > 12u || day < 1u || day > 31u ||
        hour > 23u || minute > 59u || second > 59u ||
        tz_quarters < -79 || tz_quarters > 79) {
        return false;
    }
    out[0] = semi_octet((uint8_t)(year % 100u));
    out[1] = semi_octet(month);
    out[2] = semi_octet(day);
    out[3] = semi_octet(hour);
    out[4] = semi_octet(minute);
    out[5] = semi_octet(second);
    uint8_t magnitude = (uint8_t)(tz_quarters < 0 ? -tz_quarters : tz_quarters);
    uint8_t tens = (uint8_t)(magnitude / 10u);
    if (tz_quarters < 0) {
        tens |= 0x08u; /* sign bit lives in the tens digit (23.040 9.2.3.11) */
    }
    out[6] = (uint8_t)(((magnitude % 10u) << 4) | tens);
    return true;
}

bool sms_deliver_pack_gsm7(const uint8_t *codes, uint8_t septets,
                           uint8_t *dst, size_t cap, uint8_t *out_len) {
    if (codes == NULL || dst == NULL || out_len == NULL || septets == 0u) {
        return false;
    }
    size_t bytes = ((size_t)septets * 7u + 7u) / 8u;
    if (bytes > cap) {
        return false;
    }
    memset(dst, 0, bytes);
    for (uint8_t i = 0u; i < septets; i++) {
        uint8_t value = (uint8_t)(codes[i] & 0x7fu);
        uint16_t bit = (uint16_t)i * 7u;
        size_t byte = bit / 8u;
        uint8_t shift = (uint8_t)(bit & 7u);
        dst[byte] |= (uint8_t)(value << shift);
        if (shift > 1u && byte + 1u < bytes) {
            dst[byte + 1u] |= (uint8_t)(value >> (8u - shift));
        }
    }
    *out_len = (uint8_t)bytes;
    return true;
}

/* GSM 03.38 default alphabet, Latin-1 code points that differ from ASCII. */
typedef struct { uint8_t latin1; uint8_t gsm; } gsm7_map_t;
static const gsm7_map_t GSM7_BASIC[] = {
    {0x40u, 0x00u}, {0xA3u, 0x01u}, {0x24u, 0x02u}, {0xA5u, 0x03u},
    {0xE8u, 0x04u}, {0xE9u, 0x05u}, {0xF9u, 0x06u}, {0xECu, 0x07u},
    {0xF2u, 0x08u}, {0xC7u, 0x09u}, {0xD8u, 0x0Bu}, {0xF8u, 0x0Cu},
    {0xC5u, 0x0Eu}, {0xE5u, 0x0Fu}, {0x5Fu, 0x11u}, {0xC6u, 0x1Cu},
    {0xE6u, 0x1Du}, {0xDFu, 0x1Eu}, {0xC9u, 0x1Fu}, {0xA4u, 0x24u},
    {0xA1u, 0x40u}, {0xC4u, 0x5Bu}, {0xD6u, 0x5Cu}, {0xD1u, 0x5Du},
    {0xDCu, 0x5Eu}, {0xA7u, 0x5Fu}, {0xBFu, 0x60u}, {0xE4u, 0x7Bu},
    {0xF6u, 0x7Cu}, {0xF1u, 0x7Du}, {0xFCu, 0x7Eu}, {0xE0u, 0x7Fu},
};
static const gsm7_map_t GSM7_EXT[] = {
    {0x5Eu, 0x14u}, {0x7Bu, 0x28u}, {0x7Du, 0x29u}, {0x5Cu, 0x2Fu},
    {0x5Bu, 0x3Cu}, {0x7Eu, 0x3Du}, {0x5Du, 0x3Eu}, {0x7Cu, 0x40u},
};

uint8_t sms_deliver_gsm7_from_latin1(uint8_t ch, uint8_t out[2]) {
    if (out == NULL) {
        return 0u;
    }
    for (size_t i = 0u; i < sizeof(GSM7_EXT) / sizeof(GSM7_EXT[0]); i++) {
        if (GSM7_EXT[i].latin1 == ch) {
            out[0] = 0x1Bu;
            out[1] = GSM7_EXT[i].gsm;
            return 2u;
        }
    }
    for (size_t i = 0u; i < sizeof(GSM7_BASIC) / sizeof(GSM7_BASIC[0]); i++) {
        if (GSM7_BASIC[i].latin1 == ch) {
            out[0] = GSM7_BASIC[i].gsm;
            return 1u;
        }
    }
    if (ch == 0x0Au || ch == 0x0Du || (ch >= 0x20u && ch < 0x7Fu)) {
        out[0] = ch; /* identical in ASCII and GSM 03.38 */
        return 1u;
    }
    return 0u;
}

/* TS 23.040 9.1.2.3 BCD digits: 0-9 plus '*' (0xA) and '#' (0xB). */
static bool bcd_digit(char c, uint8_t *out) {
    if (c >= '0' && c <= '9') {
        *out = (uint8_t)(c - '0');
    } else if (c == '*') {
        *out = 0x0Au;
    } else if (c == '#') {
        *out = 0x0Bu;
    } else {
        return false;
    }
    return true;
}

static bool address_is_numeric(const char *address) {
    const char *digits = address;
    if (*digits == '+') {
        digits++;
    }
    if (*digits == '\0') {
        return false;
    }
    for (const char *p = digits; *p != '\0'; p++) {
        uint8_t nibble;
        if (!bcd_digit(*p, &nibble)) {
            return false;
        }
    }
    return true;
}

/* TS 23.040 9.1.2.5: the address value is at most 10 octets. TON 5
 * (alphanumeric) packs GSM 7-bit septets into it and Address-Length counts
 * the useful semi-octets, ceil(septets * 7 / 4); numeric TONs pack BCD
 * digits with 0xF filler and Address-Length counts the digits. This is the
 * inverse of decode_address() in sms_picture_codec.c. */
#define ADDRESS_VALUE_OCTETS_MAX 10u
#define ADDRESS_ALPHA_SEPTETS_MAX 11u /* 77 bits fit 10 octets; 12 do not */

static bool append_address(uint8_t *pdu, size_t cap, size_t *pos,
                           const char *address, uint8_t toa) {
    uint8_t ton = (uint8_t)((toa >> 4) & 0x07u);
    if (ton == 5u) {
        size_t septets = strlen(address);
        if (septets == 0u || septets > ADDRESS_ALPHA_SEPTETS_MAX) {
            return false;
        }
        for (size_t i = 0u; i < septets; i++) {
            if ((uint8_t)address[i] >= 0x80u) {
                return false; /* GSM 03.38 codes only (+CSCS="GSM") */
            }
        }
        uint8_t packed[ADDRESS_VALUE_OCTETS_MAX];
        uint8_t packed_len = 0u;
        if (!sms_deliver_pack_gsm7((const uint8_t *)address, (uint8_t)septets,
                                   packed, sizeof(packed), &packed_len)) {
            return false;
        }
        if (*pos + 2u + packed_len > cap) {
            return false;
        }
        pdu[(*pos)++] = (uint8_t)((septets * 7u + 3u) / 4u);
        pdu[(*pos)++] = toa;
        memcpy(&pdu[*pos], packed, packed_len);
        *pos += packed_len;
        return true;
    }
    const char *digits = address;
    if (*digits == '+') {
        digits++;
    }
    size_t count = strlen(digits);
    if (count == 0u || count > 2u * ADDRESS_VALUE_OCTETS_MAX) {
        return false;
    }
    size_t bytes = (count + 1u) / 2u;
    if (*pos + 2u + bytes > cap) {
        return false;
    }
    pdu[(*pos)++] = (uint8_t)count;
    pdu[(*pos)++] = toa;
    for (size_t i = 0u; i < count; i += 2u) {
        uint8_t low, high = 0x0Fu;
        if (!bcd_digit(digits[i], &low) ||
            (i + 1u < count && !bcd_digit(digits[i + 1u], &high))) {
            return false;
        }
        pdu[(*pos)++] = (uint8_t)((high << 4) | low);
    }
    return true;
}

bool sms_deliver_build(const sms_deliver_t *deliver, char *hex,
                       size_t hex_cap, uint8_t *out_tpdu_len) {
    if (deliver == NULL || hex == NULL || out_tpdu_len == NULL ||
        deliver->ud_len > SMS_DELIVER_UD_MAX) {
        return false;
    }
    uint8_t pdu[1u + 176u];
    size_t pos = 0u;
    pdu[pos++] = 0x00u; /* SMSC: use the module/SIM default. */
    pdu[pos++] = (uint8_t)(0x04u | (deliver->udhi ? 0x40u : 0x00u)); /* SMS-DELIVER, TP-MMS */
    uint8_t toa = deliver->toa;
    if (toa == 0u) {
        /* No TOA given: a BCD number is international with '+', national
         * otherwise; anything else is alphanumeric text (TON 5). */
        if (!address_is_numeric(deliver->address)) {
            toa = 0xD0u;
        } else {
            toa = deliver->address[0] == '+' ? 0x91u : 0x81u;
        }
    }
    if (!append_address(pdu, sizeof(pdu), &pos, deliver->address, toa)) {
        return false;
    }
    if (pos + 2u + 7u + 1u + deliver->ud_len > sizeof(pdu)) {
        return false;
    }
    pdu[pos++] = deliver->pid;
    pdu[pos++] = deliver->dcs;
    memcpy(&pdu[pos], deliver->scts, 7u);
    pos += 7u;
    pdu[pos++] = deliver->udl;
    memcpy(&pdu[pos], deliver->ud, deliver->ud_len);
    pos += deliver->ud_len;

    static const char HEX[] = "0123456789ABCDEF";
    if (pos * 2u + 1u > hex_cap) {
        return false;
    }
    for (size_t i = 0u; i < pos; i++) {
        hex[i * 2u] = HEX[pdu[i] >> 4];
        hex[i * 2u + 1u] = HEX[pdu[i] & 0x0fu];
    }
    hex[pos * 2u] = '\0';
    *out_tpdu_len = (uint8_t)(pos - 1u);
    return true;
}

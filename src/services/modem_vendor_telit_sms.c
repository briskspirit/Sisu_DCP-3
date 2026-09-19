/* Telit LE910Cx direct-delivery forms for 3GPP2 (CDMA-style) SMS.
 *
 * The Verizon image delivers SMS over IMS in 3GPP2 form. With +CNMI=2,2 the
 * module hands them over as +CMT in a Telit-specific layout (LE910 V2 AT
 * guide, "Message Sending And Writing (3GPP2 mode)"), bench-verified on the
 * WWX 2026-09-16. Both forms are translated into a neutral SMS-DELIVER; the
 * generic service never sees teleservice ids or CDMA encodings. The mapping
 * table lives in docs/sms_direct_delivery_design.md ("Format mapping
 * (3GPP2 -> SMS-DELIVER)").
 *
 * Text form (+CSDH=1):
 *   +CMT: "<orig>","<callback>","<YYYYMMDDHHMMSS>",<tooa>,<tele_id>,<priority>,<enc>,<length>
 * PDU form (+CMGF=0):
 *   +CMT: "<orig>","<callback>",<len>
 *   <addr_len><toa><bcd...><YYMMDDhhmmss><tele_id:2><priority><enc><data_len><data>
 * where <addr_len> counts the <toa> byte plus the BCD bytes (bench: 06 81 +
 * five BCD bytes for a ten-digit number).
 *
 * Stack: this hook is reached through the vendor function pointer from the
 * core0 RX drain, which the stack gate cannot follow (it reserves a global
 * 512 B for indirect calls). The byte, code and header-field scratch is
 * therefore file-static: single instance, single context (the collector
 * that calls it already serialises deliveries), never re-entered. */

#include "modem_vendor_telit_internal.h"

#include <stdio.h>
#include <string.h>

#include "services/modem_sms_direct.h"
#include "services/sms_deliver_codec.h"

#define TELIT_TELE_WEMT 4101u           /* enhanced messaging: UDH present */
#define TELIT_TELE_WAP 4100u
#define TELIT_TELE_VMN 4099u            /* voice mail notification */
#define TELIT_TELE_VMN_ALT 262144u
#define TELIT_TELE_MAX 262144u

#define TELIT_3GPP2_TEXT_FIELDS 8u
#define TELIT_3GPP2_PDU_FIELDS 3u
#define TELIT_3GPP2_HEADER_FIELDS_MAX 10u
#define TELIT_3GPP2_DATE_DIGITS 14u
/* <toa> + <YYMMDDhhmmss> + <tele_id:2> + <priority> + <enc> + <data_len> */
#define TELIT_3GPP2_PDU_FIXED_BYTES 12u
#define TELIT_3GPP2_ADDR_BCD_MAX 10u    /* 20 digits, the DELIVER builder's cap */
#define TELIT_3GPP2_PDU_BYTES_MAX 256u  /* <len> is one byte */
#define TELIT_GSM7_SEPTETS_MAX 160u

enum { ENC_OCTET = 0u, ENC_ASCII = 2u, ENC_IA5 = 3u, ENC_UNICODE = 4u,
       ENC_LATIN1 = 8u, ENC_GSM7 = 9u };

/* Off-stack scratch (see the file comment). */
static uint8_t s_bytes[TELIT_3GPP2_PDU_BYTES_MAX]; /* decoded payload bytes */
static uint8_t s_codes[TELIT_GSM7_SEPTETS_MAX];    /* GSM 03.38 codes to pack */
static uint8_t s_ascii[TELIT_GSM7_SEPTETS_MAX];    /* unpacked 7-bit ASCII (PDU form) */
static telit_csv_view_t s_fields[TELIT_3GPP2_HEADER_FIELDS_MAX];
static char s_3gpp_header[MODEM_SMS_DIRECT_LINE_MAX];

bool telit_encode_sms_text(const char *text, modem_sms_text_t *out) {
    if (text == NULL || out == NULL || strlen(text) > MODEM_SMS_TEXT_MAX) return false;
    memset(out, 0, sizeof(*out));
    if (sms_gsm7_from_utf8(text, out->body, TELIT_GSM7_SEPTETS_MAX, &out->length)) {
        bool safe = true;
        for (size_t i = 0u; i < out->length; i++) {
            /* BS edits the prompt, ESC cancels, SUB submits prematurely. */
            if (out->body[i] == 0x08u || out->body[i] == 0x1au || out->body[i] == 0x1bu)
                safe = false;
        }
        if (safe) return true;
    }
    size_t bytes;
    if (!sms_ucs2_from_utf8(text, out->body, MODEM_SMS_TEXT_MAX * 2u, &bytes)) return false;
    /* CMGF remains 1 and CSCS remains GSM. Only outgoing TP-DCS changes;
     * UCS2 text prompts take ASCII hex, without enabling #CSCSEXT. */
    static const char hex[] = "0123456789ABCDEF";
    for (size_t i = bytes; i > 0u; i--) {
        uint8_t value = out->body[i - 1u];
        out->body[(i - 1u) * 2u] = (uint8_t)hex[value >> 4];
        out->body[(i - 1u) * 2u + 1u] = (uint8_t)hex[value & 15u];
    }
    out->length = bytes * 2u;
    out->body[out->length] = 0u;
    out->dcs = 8u;
    out->multipart = bytes > SMS_DELIVER_UD_MAX;
    return true;
}

static modem_sms_direct_translate_result_t translate_3gpp_text(
    const char *header, const uint8_t *payload, size_t payload_len,
    sms_deliver_t *out) {
    uint32_t fo, dcs, length;
    if (!telit_view_parse_u32(s_fields[4], 255u, &fo) ||
        !telit_view_parse_u32(s_fields[6], 255u, &dcs) ||
        !telit_view_parse_u32(s_fields[9], 255u, &length)) {
        return MODEM_SMS_DIRECT_REJECTED;
    }
    uint32_t group = dcs >> 4u;
    bool gsm7 = group <= 7u ? (dcs & 0x2Cu) == 0u :
        (group == 12u || group == 13u || (group == 15u && (dcs & 12u) == 0u));
    bool ucs2 = group == 14u || (group <= 7u && (dcs & 0x2Cu) == 8u);
    if (ucs2) {
        /* Telit counts UDH octets plus UCS2 characters, not TP-UD octets.
         * Captured multipart example: 7 UDH bytes + 66 chars = length 73,
         * but 139 TP-UD bytes. Hex bodies must end on this line. */
        const char *last = strrchr(header, ',');
        size_t bytes = 0u;
        if (last == NULL || (payload_len != 0u &&
            !modem_sms_direct_hex_to_bytes(payload, payload_len, s_bytes, SMS_DELIVER_UD_MAX, &bytes)))
            return MODEM_SMS_DIRECT_REJECTED;
        size_t udh = (fo & 0x40u) != 0u && bytes != 0u ? (size_t)s_bytes[0] + 1u : 0u;
        if (((fo & 0x40u) != 0u && udh == 0u) || udh > bytes ||
            ((bytes - udh) & 1u) != 0u || length != udh + (bytes - udh) / 2u)
            return MODEM_SMS_DIRECT_REJECTED;
        int n = snprintf(s_3gpp_header, sizeof(s_3gpp_header), "%.*s,%u",
                         (int)(last - header), header, (unsigned)bytes);
        return n >= 0 && (size_t)n < sizeof(s_3gpp_header) &&
            modem_sms_direct_parse_3gpp_text(s_3gpp_header, payload, payload_len, out)
                ? MODEM_SMS_DIRECT_ACCEPTED : MODEM_SMS_DIRECT_REJECTED;
    }
    if ((fo & 0x40u) != 0u || !gsm7) {
        return MODEM_SMS_DIRECT_NOT_MINE;
    }
    /* On this interface <length> counts displayed GSM characters, not
     * septets: the captured 51-character message has 53 bytes for {}.
     * Keep the generic parser strict and normalize only this vendor form. */
    if (payload_len > TELIT_GSM7_SEPTETS_MAX || length > TELIT_GSM7_SEPTETS_MAX) {
        return MODEM_SMS_DIRECT_REJECTED;
    }
    size_t chars = 0u;
    for (size_t i = 0u; i < payload_len; i++, chars++) {
        if (payload[i] >= 0x80u) return MODEM_SMS_DIRECT_REJECTED;
        if (payload[i] == 0x1bu) {
            if (++i == payload_len || payload[i] >= 0x80u || payload[i] == 0x1bu) {
                return MODEM_SMS_DIRECT_REJECTED;
            }
        }
    }
    const char *last = strrchr(header, ',');
    if (last == NULL) return MODEM_SMS_DIRECT_REJECTED;
    int n = snprintf(s_3gpp_header, sizeof(s_3gpp_header), "%.*s,%u",
                     (int)(last - header), header, (unsigned)payload_len);
    if (n < 0 || (size_t)n >= sizeof(s_3gpp_header) ||
        !modem_sms_direct_parse_3gpp_text(s_3gpp_header, payload, payload_len, out)) {
        return MODEM_SMS_DIRECT_REJECTED;
    }
    if (chars < length) return MODEM_SMS_DIRECT_INCOMPLETE;
    return chars == length ? MODEM_SMS_DIRECT_ACCEPTED : MODEM_SMS_DIRECT_REJECTED;
}

static bool digits_to_scts(const char *yyyymmddhhmmss, uint8_t out[7]) {
    if (strlen(yyyymmddhhmmss) != TELIT_3GPP2_DATE_DIGITS) {
        return false;
    }
    for (size_t i = 0u; i < TELIT_3GPP2_DATE_DIGITS; i++) {
        if (yyyymmddhhmmss[i] < '0' || yyyymmddhhmmss[i] > '9') {
            return false;
        }
    }
    /* YYYY, then MM DD hh mm ss as five two-digit fields. */
    unsigned v[6];
    const char *p = yyyymmddhhmmss;
    v[0] = (unsigned)((p[0] - '0') * 1000 + (p[1] - '0') * 100 +
                      (p[2] - '0') * 10 + (p[3] - '0'));
    for (size_t i = 1u; i < 6u; i++) {
        v[i] = (unsigned)((p[2u + i * 2u] - '0') * 10 + (p[3u + i * 2u] - '0'));
    }
    return sms_deliver_scts_encode((uint16_t)v[0], (uint8_t)v[1], (uint8_t)v[2],
                                   (uint8_t)v[3], (uint8_t)v[4], (uint8_t)v[5],
                                   0, out);
}

bool telit_unpack_ascii7(const uint8_t *data, size_t octets, size_t chars,
                         uint8_t *out) {
    if (data == NULL || out == NULL || chars > TELIT_GSM7_SEPTETS_MAX ||
        octets != (chars * 7u + 7u) / 8u) {
        return false;
    }
    for (size_t i = 0u; i < chars; i++) {
        uint8_t value = 0u;
        for (size_t b = 0u; b < 7u; b++) {
            size_t bit = i * 7u + b;
            uint8_t octet = data[bit / 8u];
            value = (uint8_t)((value << 1) | ((octet >> (7u - (bit & 7u))) & 1u));
        }
        out[i] = value;
    }
    return true;
}

/* Text -> GSM-7 packed, DCS 0x00. already_gsm: the bytes are GSM 03.38 codes
 * (the module applied +CSCS="GSM"); otherwise Latin-1 mapped to GSM with a
 * UCS2 fallback when a byte has no GSM mapping. */
static bool text_to_user_data(const uint8_t *bytes, size_t len, bool already_gsm,
                              sms_deliver_t *out) {
    size_t n = 0u;
    bool mappable = true;
    for (size_t i = 0u; i < len; i++) {
        uint8_t pair[2];
        uint8_t got;
        if (already_gsm) {
            if (bytes[i] >= 0x80u) {
                return false; /* GSM 03.38 codes are 7-bit */
            }
            pair[0] = bytes[i];
            got = 1u;
        } else {
            got = sms_deliver_gsm7_from_latin1(bytes[i], pair);
        }
        if (got == 0u || n + got > sizeof(s_codes)) {
            mappable = false;
            break;
        }
        memcpy(&s_codes[n], pair, got);
        n += got;
    }
    if (mappable) {
        out->dcs = 0x00u;
        out->udl = (uint8_t)n;
        if (n == 0u) {
            out->ud_len = 0u;
            return true;
        }
        return sms_deliver_pack_gsm7(s_codes, (uint8_t)n, out->ud, sizeof(out->ud),
                                     &out->ud_len);
    }
    if (len * 2u > SMS_DELIVER_UD_MAX) {
        return false;
    }
    out->dcs = 0x08u;
    for (size_t i = 0u; i < len; i++) {
        out->ud[i * 2u] = 0x00u;
        out->ud[i * 2u + 1u] = bytes[i];
    }
    out->ud_len = (uint8_t)(len * 2u);
    out->udl = out->ud_len;
    return true;
}

/* body: decoded user-data bytes (never hex): the text-form hex body after
 * hex_to_bytes, the text-form plain body, or the PDU-form <data>. length:
 * septets (GSM-7), UTF-16 code units (Unicode) or octets. */
static bool apply_encoding(uint32_t tele_id, uint32_t enc, uint32_t length,
                           const uint8_t *body, size_t body_len, bool text_form,
                           sms_deliver_t *out) {
    uint8_t gsm7_udl = 0u;
    switch (enc) {
    case ENC_GSM7:
        /* The text-form <length> is off by one on some WEMT parts (bench
         * 2026-09-16): the octet count is authoritative for the UDL. */
        if (!modem_sms_direct_gsm7_udl_for_octets(length, body_len, &gsm7_udl)) {
            return false;
        }
        /* Native WEMT may report 159 for a full 160-septet part: both occupy
         * 140 octets. Seven spare bits holding non-padding data prove that
         * the extra septet is real. Keep ambiguous zero/CR padding unchanged
         * (the live final part uses CR); do not apply this to 3GPP or PDU UDL. */
        if (text_form && tele_id == TELIT_TELE_WEMT &&
            gsm7_udl % 8u == 7u) {
            uint8_t tail = (uint8_t)(body[body_len - 1u] >> 1);
            if (tail != 0u && tail != '\r') {
                if ((uint32_t)gsm7_udl > length) {
                    return false;
                }
                gsm7_udl++;
            }
        }
        /* The module strips the UDH in PDU form (bench, enc 9 WEMT part). */
        out->udhi = text_form && tele_id == TELIT_TELE_WEMT && length != 0u;
        out->dcs = 0x00u;
        break;
    case ENC_UNICODE:
        if (body_len != (size_t)length * 2u) {
            return false;
        }
        out->dcs = 0x08u;
        break;
    case ENC_OCTET:
        if (body_len != length) {
            return false;
        }
        out->dcs = 0x04u;
        /* WEMT text delivery retains the port/concatenation UDH for octet
         * messages too. Native PDU delivery has already stripped it. */
        out->udhi = text_form && tele_id == TELIT_TELE_WEMT && length != 0u;
        out->wdp = tele_id == TELIT_TELE_WAP;
        break;
    case ENC_LATIN1:
        /* Text form: the module already mapped the text through +CSCS="GSM"
         * and the body is exactly <length> codes (CR/LF/0x00/ESC+x count
         * 1:1). PDU form: raw Latin-1 bytes, <data_len> checked by the caller. */
        return (!text_form || body_len == length) &&
               body_len <= TELIT_GSM7_SEPTETS_MAX &&
               text_to_user_data(body, body_len, text_form, out);
    case ENC_ASCII:
    case ENC_IA5:
        /* IS-637 7-bit ASCII. Text form: plain text through +CSCS="GSM" whose
         * <length> is the PACKED octet count, ceil(7*chars/8), and only that:
         * accepting a character-count match would let a body cut at a line
         * break past <length> pass as complete. PDU form: the caller
         * unpacked <data_len> characters; they are ASCII, mapped to GSM. */
        return (!text_form || (body_len * 7u + 7u) / 8u == length) &&
               body_len <= TELIT_GSM7_SEPTETS_MAX &&
               text_to_user_data(body, body_len, text_form, out);
    default:
        return false;
    }
    if (body_len > sizeof(out->ud)) {
        return false;
    }
    memcpy(out->ud, body, body_len);
    out->udl = enc == ENC_GSM7 ? gsm7_udl : (uint8_t)body_len;
    out->ud_len = (uint8_t)body_len;
    return true;
}

static modem_sms_direct_translate_result_t translate_text_form(
    const telit_csv_view_t fields[TELIT_3GPP2_TEXT_FIELDS], const uint8_t *payload,
    size_t payload_len, sms_deliver_t *out) {
    uint32_t tooa, tele_id, priority, enc, length;
    if (!telit_view_parse_u32(fields[3], 255u, &tooa) ||
        !telit_view_parse_u32(fields[4], TELIT_TELE_MAX, &tele_id) ||
        !telit_view_parse_u32(fields[5], 3u, &priority) ||
        !telit_view_parse_u32(fields[6], ENC_GSM7, &enc) ||
        !telit_view_parse_u32(fields[7], 255u, &length)) {
        return MODEM_SMS_DIRECT_REJECTED;
    }
    (void)priority;
    if (tele_id == TELIT_TELE_VMN || tele_id == TELIT_TELE_VMN_ALT) {
        return MODEM_SMS_DIRECT_REJECTED; /* MWI already arrives via #MWI */
    }
    memset(out, 0, sizeof(*out));
    char date[TELIT_3GPP2_DATE_DIGITS + 1u];
    if (!telit_view_copy_exact(out->address, sizeof(out->address), fields[0]) ||
        !telit_view_copy_exact(date, sizeof(date), fields[2]) ||
        !digits_to_scts(date, out->scts)) {
        return MODEM_SMS_DIRECT_REJECTED;
    }
    out->toa = (uint8_t)tooa;

    if (enc == ENC_ASCII || enc == ENC_IA5) {
        if (payload_len > TELIT_GSM7_SEPTETS_MAX ||
            length > (TELIT_GSM7_SEPTETS_MAX * 7u + 7u) / 8u) {
            return MODEM_SMS_DIRECT_REJECTED;
        }
        for (size_t i = 0u; i < payload_len; i++) {
            if (payload[i] >= 0x80u) {
                return MODEM_SMS_DIRECT_REJECTED;
            }
        }
        if ((payload_len * 7u + 7u) / 8u < length) {
            return MODEM_SMS_DIRECT_INCOMPLETE;
        }
    }

    bool plain;
    if (enc == ENC_LATIN1 || enc == ENC_ASCII || enc == ENC_IA5) {
        plain = true;
    } else if (enc == ENC_GSM7) {
        /* Plain GSM-charset text without UDH has exactly <length> chars; a
         * hex body has 2 * ceil(7 * <length> / 8), and the two never
         * coincide for length > 0 (design, "Format mapping"). */
        size_t packed = ((size_t)length * 7u + 7u) / 8u;
        plain = length != 0u && payload_len == length && payload_len != packed * 2u;
    } else {
        plain = false;
    }
    if (plain && enc == ENC_GSM7) {
        return text_to_user_data(payload, payload_len, true, out)
                   ? MODEM_SMS_DIRECT_ACCEPTED : MODEM_SMS_DIRECT_REJECTED;
    }
    const uint8_t *body = payload;
    size_t body_len = payload_len;
    if (!plain && payload_len != 0u) {
        if (!modem_sms_direct_hex_to_bytes(payload, payload_len, s_bytes,
                                           sizeof(s_bytes), &body_len)) {
            return MODEM_SMS_DIRECT_REJECTED;
        }
        body = s_bytes;
    }
    return apply_encoding(tele_id, enc, length, body, body_len, true, out)
               ? MODEM_SMS_DIRECT_ACCEPTED : MODEM_SMS_DIRECT_REJECTED;
}

static modem_sms_direct_translate_result_t translate_pdu_form(
    const telit_csv_view_t fields[TELIT_3GPP2_PDU_FIELDS], const uint8_t *payload,
    size_t payload_len, sms_deliver_t *out) {
    uint32_t length;
    if (!telit_view_parse_u32(fields[2], 255u, &length)) {
        return MODEM_SMS_DIRECT_REJECTED;
    }
    uint8_t *bytes = s_bytes;
    size_t n = 0u;
    if (!modem_sms_direct_hex_to_bytes(payload, payload_len, bytes, sizeof(s_bytes),
                                       &n) ||
        n != length) {
        return MODEM_SMS_DIRECT_REJECTED;
    }
    size_t pos = 0u;
    uint8_t addr_len = bytes[pos++];
    if (addr_len < 2u || addr_len - 1u > TELIT_3GPP2_ADDR_BCD_MAX ||
        pos + addr_len + (TELIT_3GPP2_PDU_FIXED_BYTES - 1u) > n) {
        return MODEM_SMS_DIRECT_REJECTED;
    }
    uint8_t toa = bytes[pos++];
    uint8_t bcd_bytes = (uint8_t)(addr_len - 1u);
    memset(out, 0, sizeof(*out));
    size_t d = 0u;
    for (uint8_t i = 0u; i < bcd_bytes; i++) {
        uint8_t low = (uint8_t)(bytes[pos + i] & 0x0fu);
        uint8_t high = (uint8_t)(bytes[pos + i] >> 4);
        if (low > 9u || (high > 9u && !(high == 0x0fu && i + 1u == bcd_bytes))) {
            return MODEM_SMS_DIRECT_REJECTED;
        }
        out->address[d++] = (char)('0' + low);
        if (high != 0x0fu) {
            out->address[d++] = (char)('0' + high);
        }
    }
    out->address[d] = '\0';
    pos += bcd_bytes;
    out->toa = toa;
    char date[TELIT_3GPP2_DATE_DIGITS + 1u];
    date[0] = '2';
    date[1] = '0';
    for (size_t i = 0u; i < 6u; i++) {
        uint8_t b = bytes[pos + i];
        date[2u + i * 2u] = (char)('0' + (b >> 4));
        date[3u + i * 2u] = (char)('0' + (b & 0x0fu));
    }
    date[TELIT_3GPP2_DATE_DIGITS] = '\0';
    pos += 6u;
    uint32_t tele_id = (uint32_t)((bytes[pos] << 8) | bytes[pos + 1u]);
    pos += 2u;
    pos++; /* priority */
    uint8_t enc = bytes[pos++];
    uint8_t data_len = bytes[pos++];
    if (tele_id == TELIT_TELE_VMN || tele_id == TELIT_TELE_VMN_ALT ||
        !digits_to_scts(date, out->scts)) {
        return MODEM_SMS_DIRECT_REJECTED;
    }
    const uint8_t *data = &bytes[pos];
    size_t avail = n - pos;
    if (enc == ENC_LATIN1 && avail != data_len) {
        return MODEM_SMS_DIRECT_REJECTED;
    }
    if (enc == ENC_ASCII || enc == ENC_IA5) {
        /* <data_len> is the character count; the data is those characters
         * packed 7 bits each, MSB-first (bench: 0x0F chars in 14 octets). */
        if (!telit_unpack_ascii7(data, avail, data_len, s_ascii)) {
            return MODEM_SMS_DIRECT_REJECTED;
        }
        for (size_t i = 0u; i < data_len; i++) {
            if (s_ascii[i] >= 0x80u) {
                return MODEM_SMS_DIRECT_REJECTED;
            }
        }
        return apply_encoding(tele_id, enc, data_len, s_ascii, data_len, false, out)
                   ? MODEM_SMS_DIRECT_ACCEPTED : MODEM_SMS_DIRECT_REJECTED;
    }
    /* PDU-form <data_len> is a byte count for Unicode; the text-form <length>
     * (and apply_encoding) count UTF-16 code units. GSM-7 counts septets in
     * both forms. The remaining bytes are the user data as delivered. */
    uint32_t length_units = enc == ENC_UNICODE ? (uint32_t)data_len / 2u : data_len;
    return apply_encoding(tele_id, enc, length_units, data, avail, false, out)
               ? MODEM_SMS_DIRECT_ACCEPTED : MODEM_SMS_DIRECT_REJECTED;
}

modem_sms_direct_translate_result_t telit_translate_direct_sms(
    const char *header, const uint8_t *payload, size_t payload_len,
    sms_deliver_t *out) {
    if (header == NULL || payload == NULL || out == NULL) {
        return MODEM_SMS_DIRECT_NOT_MINE;
    }
    size_t count = 0u;
    if (!telit_view_split_prefixed(header, "+CMT:", s_fields,
                                   TELIT_3GPP2_HEADER_FIELDS_MAX, &count)) {
        return MODEM_SMS_DIRECT_NOT_MINE;
    }
    /* 3GPP2 text: 8 fields with a 14-digit date; 3GPP2 PDU: 3 fields. The
     * standard 3GPP forms have 2 (PDU) or 10 (text, +CSDH=1) fields. */
    if (count == TELIT_3GPP2_TEXT_FIELDS &&
        telit_view_digits_only(s_fields[2], TELIT_3GPP2_DATE_DIGITS,
                               TELIT_3GPP2_DATE_DIGITS)) {
        return translate_text_form(s_fields, payload, payload_len, out);
    }
    if (count == TELIT_3GPP2_PDU_FIELDS) {
        return translate_pdu_form(s_fields, payload, payload_len, out);
    }
    if (count == TELIT_3GPP2_HEADER_FIELDS_MAX) {
        return translate_3gpp_text(header, payload, payload_len, out);
    }
    return MODEM_SMS_DIRECT_NOT_MINE;
}

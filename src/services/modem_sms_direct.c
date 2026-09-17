/* Direct SMS delivery collector (3GPP 27.005 +CMT, <mt>=2).
 *
 * A +CMT is a header line followed by the body. The collector remembers the
 * header, reads the body, hands header+body to the vendor hook first
 * (module-specific forms), then to the generic 3GPP text/PDU parsers, and
 * returns a SMS-DELIVER PDU ready for +CMGW or a filtered-control result.
 * Pure: no I/O, no vendor names.
 *
 * Body reading (RAW mode): the line path cannot carry a plain-text body
 * intact (the framer drops CR, splits on LF, maps 0x00 to a C terminator and
 * the service trims spaces), so a header whose last field <length> L is > 0
 * switches the collector to RAW mode: the service's byte drain hands every
 * byte here instead of to the framer. The first L bytes are always data.
 * Later line breaks are candidates: only an explicit INCOMPLETE from the
 * vendor keeps an invalid candidate open. Other failures end the delivery.
 * A bare LF that can extend an accepted body is preserved as data until its
 * CRLF terminator; this handles the two character counts that may share one
 * packed length. Hex bodies contain no CR/LF and end at their first candidate.
 * Encoding-specific length rules stay in the vendor/parser, not the reader. */

#include "services/modem_sms_direct.h"

#include <stdio.h>
#include <string.h>

#include "services/modem_at_util.h"
#include "services/sms_picture_codec.h"

#define HEADER_FIELD_MAX 48u  /* over-long fields truncate; only <alpha> (ignored) can exceed this */
#define HEADER_FIELDS_MAX 10u /* the 3GPP text form has exactly ten */
#define TPDU_MAX 176u
#define GSM7_SEPTETS_MAX 160u

typedef char header_field_t[HEADER_FIELD_MAX];

static char s_header[MODEM_SMS_DIRECT_LINE_MAX];
static bool s_pending;      /* line mode: the next framed line is the payload */
static bool s_raw_active;   /* RAW mode: bytes come through feed_raw */
static bool s_raw_cr_held;  /* a CR at/after <length> is waiting for its LF */
static bool s_raw_multiline; /* a candidate line break became body data */
static bool s_raw_unconsumed; /* the byte past a bound belongs to the framer */
static size_t s_raw_len;
static size_t s_raw_min;    /* <length>: bytes that are data regardless of value */
static uint8_t s_raw[MODEM_SMS_DIRECT_LINE_MAX];
/* Parser scratch lives here, not on the stack: the collector runs beneath the
 * core0 RX drain, whose stack budget these frames would otherwise exceed. The
 * module is single-instance and single-context (it already owns s_header). */
static header_field_t s_fields[HEADER_FIELDS_MAX]; /* text form + length probe */
static header_field_t s_pdu_fields[3];             /* PDU form: 2 fields + overflow */
static uint8_t s_pdu_bytes[1u + TPDU_MAX];
static char s_pdu_hex_scratch[MODEM_SMS_DIRECT_LINE_MAX + 1u]; /* NUL-terminated view for the decoder */
static sms_codec_message_t s_decoded;
static sms_deliver_t s_deliver;
static bool s_wdp;
static sms_control_filter_t s_filter_reason;

static void raw_leave(void) {
    s_raw_active = false;
    s_raw_cr_held = false;
    s_raw_multiline = false;
    s_raw_unconsumed = false;
    s_raw_len = 0u;
    s_raw_min = 0u;
}

/* Only plain bodies can contain CR/LF, and no supported plain encoding packs
 * more than 8 characters into 7 octets (encoding 2/3: <length> is the packed
 * octet count), so once more than ceil(8*L/7) + 1 bytes are in, no plain
 * body can still be pending: a continued body past this bound ends the
 * delivery instead of waiting for the cap or the deadline. Hex bodies are
 * longer than this but carry no CR/LF, so their own CRLF is their first
 * candidate and never continues after one. */
static size_t raw_plain_body_max(size_t length) {
    return (length * 8u + 6u) / 7u + 1u;
}

void modem_sms_direct_reset(void) {
    s_pending = false;
    s_header[0] = '\0';
    raw_leave();
    s_wdp = false;
    s_filter_reason = SMS_CONTROL_KEEP;
}

sms_control_filter_t modem_sms_direct_filter_reason(void) {
    return s_filter_reason;
}

bool modem_sms_direct_pending(void) {
    return s_pending || s_raw_active;
}

bool modem_sms_direct_raw_active(void) {
    return s_raw_active;
}

bool modem_sms_direct_raw_take_unconsumed(void) {
    bool unconsumed = s_raw_unconsumed;
    s_raw_unconsumed = false;
    return unconsumed;
}

bool modem_sms_direct_hex_to_bytes(const uint8_t *hex, size_t hex_len,
                                   uint8_t *dst, size_t cap, size_t *out_len) {
    if (hex == NULL || dst == NULL || out_len == NULL) {
        return false;
    }
    if (hex_len == 0u || (hex_len & 1u) != 0u || hex_len / 2u > cap) {
        return false;
    }
    for (size_t i = 0u; i < hex_len; i += 2u) {
        uint8_t value = 0u;
        for (size_t k = 0u; k < 2u; k++) {
            uint8_t c = hex[i + k];
            uint8_t nibble;
            if (c >= '0' && c <= '9') {
                nibble = (uint8_t)(c - '0');
            } else if (c >= 'A' && c <= 'F') {
                nibble = (uint8_t)(c - 'A' + 10);
            } else if (c >= 'a' && c <= 'f') {
                nibble = (uint8_t)(c - 'a' + 10);
            } else {
                return false;
            }
            value = (uint8_t)((value << 4) | nibble);
        }
        dst[i / 2u] = value;
    }
    *out_len = hex_len / 2u;
    return true;
}

bool modem_sms_direct_gsm7_udl_for_octets(uint32_t length, size_t octets,
                                          uint8_t *udl_out) {
    if (udl_out == NULL) {
        return false;
    }
    if (length == 0u) {
        /* An empty body has no part to be off by one on: exact. */
        *udl_out = 0u;
        return octets == 0u;
    }
    const int32_t deltas[3] = {0, 1, -1};
    for (size_t i = 0u; i < 3u; i++) {
        int64_t udl = (int64_t)length + deltas[i];
        if (udl < 0 || udl > GSM7_SEPTETS_MAX) {
            continue;
        }
        if (((size_t)udl * 7u + 7u) / 8u == octets) {
            *udl_out = (uint8_t)udl;
            return true;
        }
    }
    return false;
}

/* Split "+CMT: a,b,c" into at most max fields; quotes are stripped and a
 * quoted field may contain commas (the 3GPP <scts> does). Over-long fields
 * are truncated to HEADER_FIELD_MAX - 1. Returns the field count; the caller
 * has already verified the "+CMT:" prefix. */
static size_t split_header(const char *header, header_field_t *fields, size_t max) {
    const char *p = header + 5; /* "+CMT:" */
    while (*p == ' ') {
        p++;
    }
    size_t count = 0u;
    while (count < max) {
        size_t n = 0u;
        if (*p == '"') {
            p++;
            while (*p != '\0' && *p != '"') {
                if (n + 1u < HEADER_FIELD_MAX) {
                    fields[count][n++] = *p;
                }
                p++;
            }
            if (*p == '"') {
                p++;
            }
        } else {
            while (*p != '\0' && *p != ',') {
                if (n + 1u < HEADER_FIELD_MAX) {
                    fields[count][n++] = *p;
                }
                p++;
            }
        }
        fields[count][n] = '\0';
        count++;
        if (*p != ',') {
            break;
        }
        p++;
    }
    return count;
}

static bool parse_u32(const char *text, uint32_t max, uint32_t *out) {
    if (text == NULL || *text == '\0') {
        return false;
    }
    uint32_t value = 0u;
    for (const char *p = text; *p != '\0'; p++) {
        if (*p < '0' || *p > '9' || value > max / 10u) {
            return false;
        }
        value = value * 10u + (uint32_t)(*p - '0');
    }
    if (value > max) {
        return false;
    }
    *out = value;
    return true;
}

bool modem_sms_direct_parse_3gpp_pdu(const char *header, const uint8_t *payload,
                                     size_t payload_len, char *pdu_hex,
                                     size_t pdu_hex_cap, uint8_t *tpdu_len_out) {
    if (header == NULL || payload == NULL || pdu_hex == NULL || tpdu_len_out == NULL ||
        !modem_at_starts_with(header, "+CMT:")) {
        return false;
    }
    /* [<alpha>],<length>: <length> counts TPDU octets, excluding the SMSC.
     * A third field means this is not the PDU form. */
    size_t count = split_header(header, s_pdu_fields, 3u);
    uint32_t length = 0u;
    if (count != 2u || !parse_u32(s_pdu_fields[1], TPDU_MAX, &length) || length == 0u) {
        return false;
    }
    size_t byte_len = 0u;
    if (!modem_sms_direct_hex_to_bytes(payload, payload_len, s_pdu_bytes,
                                       sizeof(s_pdu_bytes), &byte_len) ||
        byte_len < 2u || (size_t)s_pdu_bytes[0] + 1u >= byte_len ||
        byte_len - 1u - s_pdu_bytes[0] != length) {
        return false;
    }
    /* The decoder wants a C string; validate from a static terminated copy
     * so the caller's buffer is written only for an accepted payload (the
     * RAW reader parses candidates speculatively). */
    if (payload_len >= sizeof(s_pdu_hex_scratch)) {
        return false;
    }
    memcpy(s_pdu_hex_scratch, payload, payload_len);
    s_pdu_hex_scratch[payload_len] = '\0';
    if (!sms_pdu_decode(s_pdu_hex_scratch, &s_decoded) || s_decoded.submit) {
        return false;
    }
    if (payload_len + 1u > pdu_hex_cap) {
        return false;
    }
    memcpy(pdu_hex, s_pdu_hex_scratch, payload_len + 1u);
    *tpdu_len_out = (uint8_t)length;
    return true;
}

/* "yy/MM/dd,hh:mm:ss+zz" (27.005 <scts>, zone in quarter hours) -> TP-SCTS */
static bool parse_scts(const char *text, uint8_t out[7]) {
    unsigned yy, mo, dd, hh, mi, ss, zz;
    char sign;
    if (sscanf(text, "%2u/%2u/%2u,%2u:%2u:%2u%c%2u",
               &yy, &mo, &dd, &hh, &mi, &ss, &sign, &zz) != 8 ||
        (sign != '+' && sign != '-') || zz > 79u) {
        return false;
    }
    int8_t quarters = (int8_t)(sign == '-' ? -(int)zz : (int)zz);
    return sms_deliver_scts_encode((uint16_t)(2000u + yy), (uint8_t)mo, (uint8_t)dd,
                                   (uint8_t)hh, (uint8_t)mi, (uint8_t)ss, quarters, out);
}

/* 27.005 3.1: with +CSDH=1 the text-form body is the message text itself
 * for the GSM 7-bit default alphabet and hex octets for 8-bit/UCS2 (or when
 * UDHI is set). Same DCS reading as dcs_kind() in sms_picture_codec.c. */
static bool dcs_is_gsm7(uint8_t dcs) {
    uint8_t group = (uint8_t)(dcs >> 4u);
    if (group <= 7u) {
        return (dcs & 0x2Cu) == 0u; /* uncompressed, default alphabet */
    }
    if (group == 0x0Cu || group == 0x0Du) {
        return true; /* MWI discard/store, GSM 7-bit */
    }
    if (group == 0x0Fu) {
        return (dcs & 0x0Cu) == 0u; /* data coding/message class, bit 3 reserved */
    }
    return false; /* 0x0E (MWI UCS2) and the reserved groups 8..B */
}

/* A plain body is GSM 03.38 codes (+CSCS="GSM"), one byte per septet. */
static bool plain_body_valid(const uint8_t *payload, size_t payload_len) {
    for (size_t i = 0u; i < payload_len; i++) {
        if (payload[i] >= 0x80u) {
            return false;
        }
    }
    return true;
}

bool modem_sms_direct_parse_3gpp_text(const char *header, const uint8_t *payload,
                                      size_t payload_len, sms_deliver_t *out) {
    if (header == NULL || payload == NULL || out == NULL ||
        !modem_at_starts_with(header, "+CMT:")) {
        return false;
    }
    header_field_t *fields = s_fields;
    size_t count = split_header(header, fields, HEADER_FIELDS_MAX);
    /* <oa>,<alpha>,<scts>,<tooa>,<fo>,<pid>,<dcs>,<sca>,<tosca>,<length> */
    uint32_t tooa, fo, pid, dcs, length;
    if (count != 10u || strchr(fields[2], '/') == NULL ||
        !parse_u32(fields[3], 255u, &tooa) || !parse_u32(fields[4], 255u, &fo) ||
        !parse_u32(fields[5], 255u, &pid) || !parse_u32(fields[6], 255u, &dcs) ||
        !parse_u32(fields[9], 255u, &length)) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    if (strlen(fields[0]) > SMS_CODEC_ADDRESS_MAX) {
        return false;
    }
    strcpy(out->address, fields[0]); /* digits or alphanumeric; <tooa> decides */
    out->toa = (uint8_t)tooa;
    if (!parse_scts(fields[2], out->scts)) {
        return false;
    }
    out->pid = (uint8_t)pid;
    out->dcs = (uint8_t)dcs;
    out->udhi = (fo & 0x40u) != 0u;
    out->udl = (uint8_t)length;
    bool seven_bit = dcs_is_gsm7((uint8_t)dcs);
    if (out->udhi || !seven_bit) {
        /* Hex body: the whole TP-UD. <length> is septets for GSM 7-bit
         * (UDH included), octets otherwise; either way it pins the octet count. */
        size_t n = 0u;
        size_t expected = seven_bit ? ((size_t)length * 7u + 7u) / 8u : (size_t)length;
        if (expected == 0u) {
            /* Empty body: the hex helper rejects an empty range by design. */
            out->ud_len = 0u;
            return payload_len == 0u;
        }
        if (!modem_sms_direct_hex_to_bytes(payload, payload_len, out->ud,
                                           sizeof(out->ud), &n)) {
            return false;
        }
        if (seven_bit) {
            /* <length> may be off by one on UDH parts; the octets decide. */
            if (!modem_sms_direct_gsm7_udl_for_octets(length, n, &out->udl)) {
                return false;
            }
        } else if (n != expected) {
            return false;
        }
        out->ud_len = (uint8_t)n;
        return true;
    }
    /* Plain body: exactly <length> GSM codes (the RAW reader guarantees the
     * byte count matches the wire; anything else is a malformed delivery). */
    if (payload_len != length || payload_len > GSM7_SEPTETS_MAX ||
        !plain_body_valid(payload, payload_len)) {
        return false;
    }
    if (payload_len == 0u) {
        out->ud_len = 0u;
        return true;
    }
    return sms_deliver_pack_gsm7(payload, (uint8_t)payload_len,
                                 out->ud, sizeof(out->ud), &out->ud_len);
}

/* The header's last comma-separated field is the body length in both text
 * forms and the TPDU length in both PDU forms. Form-agnostic: any header whose
 * last field parses as a number has a length; a header ending in <scts>
 * (+CSDH=0) has none and keeps the two-line behaviour. */
static bool header_length(const char *header, uint32_t *length) {
    size_t count = split_header(header, s_fields, HEADER_FIELDS_MAX);
    return count > 0u && parse_u32(s_fields[count - 1u], 255u, length);
}

static modem_sms_direct_translate_result_t collect(
    const uint8_t *payload, size_t payload_len,
    modem_sms_direct_translate_fn vendor,
    char *pdu_hex, size_t pdu_hex_cap, uint8_t *tpdu_len_out) {
    s_wdp = false;
    if (vendor != NULL) {
        memset(&s_deliver, 0, sizeof(s_deliver));
        modem_sms_direct_translate_result_t result =
            vendor(s_header, payload, payload_len, &s_deliver);
        if (result == MODEM_SMS_DIRECT_ACCEPTED) {
            s_wdp = s_deliver.wdp;
            return sms_deliver_build(&s_deliver, pdu_hex, pdu_hex_cap, tpdu_len_out)
                       ? MODEM_SMS_DIRECT_ACCEPTED : MODEM_SMS_DIRECT_REJECTED;
        }
        if (result != MODEM_SMS_DIRECT_NOT_MINE) {
            return result;
        }
    }
    if (modem_sms_direct_parse_3gpp_pdu(s_header, payload, payload_len, pdu_hex,
                                        pdu_hex_cap, tpdu_len_out)) {
        return MODEM_SMS_DIRECT_ACCEPTED;
    }
    if (modem_sms_direct_parse_3gpp_text(s_header, payload, payload_len, &s_deliver) &&
        sms_deliver_build(&s_deliver, pdu_hex, pdu_hex_cap, tpdu_len_out)) {
        return MODEM_SMS_DIRECT_ACCEPTED;
    }
    return MODEM_SMS_DIRECT_REJECTED;
}

/* Classification runs only after framing commits the body, never during
 * speculative CR/LF parsing. Reuse the normal decoder's static scratch. */
static modem_sms_direct_step_t completed_step(
    modem_sms_direct_translate_result_t result, const char *pdu_hex) {
    if (result != MODEM_SMS_DIRECT_ACCEPTED) {
        return MODEM_SMS_DIRECT_STEP_REJECTED;
    }
    s_filter_reason = sms_pdu_decode(pdu_hex, &s_decoded)
        ? sms_control_classify(&s_decoded, s_wdp) : SMS_CONTROL_KEEP;
    return s_filter_reason != SMS_CONTROL_KEEP
        ? MODEM_SMS_DIRECT_STEP_FILTERED : MODEM_SMS_DIRECT_STEP_READY;
}

modem_sms_direct_step_t modem_sms_direct_feed(
    const char *line, modem_sms_direct_translate_fn vendor,
    char *pdu_hex, size_t pdu_hex_cap, uint8_t *tpdu_len_out) {
    if (line == NULL || pdu_hex == NULL || tpdu_len_out == NULL) {
        return MODEM_SMS_DIRECT_STEP_IGNORED;
    }
    /* A header always (re)starts a delivery, even while one is pending: a
     * stale header then loses its payload instead of misparsing the new one.
     * A header can only arrive through the framer, so RAW mode cannot nest. */
    if (modem_at_starts_with(line, "+CMT:")) {
        s_filter_reason = SMS_CONTROL_KEEP;
        modem_at_copy_bounded(s_header, sizeof(s_header), line);
        raw_leave();
        s_pending = false;
        uint32_t length = 0u;
        if (!header_length(line, &length)) {
            s_pending = true; /* no <length>: the next framed line is the body */
            return MODEM_SMS_DIRECT_STEP_HEADER;
        }
        if (length == 0u) {
            /* The framer never delivers an empty line: complete on the header. */
            modem_sms_direct_translate_result_t result =
                collect(s_raw, 0u, vendor, pdu_hex, pdu_hex_cap, tpdu_len_out);
            return completed_step(result, pdu_hex);
        }
        s_raw_active = true;
        s_raw_min = length;
        return MODEM_SMS_DIRECT_STEP_HEADER;
    }
    if (s_raw_active || !s_pending) {
        return MODEM_SMS_DIRECT_STEP_IGNORED;
    }
    s_pending = false;
    modem_sms_direct_translate_result_t result = collect(
        (const uint8_t *)line, strlen(line), vendor, pdu_hex, pdu_hex_cap, tpdu_len_out);
    return completed_step(result, pdu_hex);
}

static bool raw_append(uint8_t byte) {
    if (s_raw_len >= sizeof(s_raw) ||
        (s_raw_multiline && s_raw_len >= raw_plain_body_max(s_raw_min))) {
        return false;
    }
    s_raw[s_raw_len++] = byte;
    return true;
}

modem_sms_direct_step_t modem_sms_direct_feed_raw(
    uint8_t byte, modem_sms_direct_translate_fn vendor,
    char *pdu_hex, size_t pdu_hex_cap, uint8_t *tpdu_len_out) {
    s_raw_unconsumed = false;
    if (!s_raw_active || pdu_hex == NULL || tpdu_len_out == NULL) {
        return MODEM_SMS_DIRECT_STEP_IGNORED;
    }
    if (s_raw_len >= s_raw_min) {
        /* The held CR is outside the candidate. A final rejection consumes
         * its terminator too, so the next header or command line stays intact. */
        if (byte == '\n') {
            modem_sms_direct_translate_result_t result = collect(
                s_raw, s_raw_len, vendor, pdu_hex, pdu_hex_cap, tpdu_len_out);
            if (result == MODEM_SMS_DIRECT_ACCEPTED && !s_raw_cr_held &&
                s_raw_len < sizeof(s_raw)) {
                /* A packed length can describe p or p+1 characters. If the
                 * LF itself fits, keep it: the module's CRLF follows later. */
                s_raw[s_raw_len] = '\n';
                if (collect(s_raw, s_raw_len + 1u, vendor, pdu_hex, pdu_hex_cap,
                            tpdu_len_out) == MODEM_SMS_DIRECT_ACCEPTED) {
                    s_raw_multiline = true;
                    if (raw_append('\n')) {
                        return MODEM_SMS_DIRECT_STEP_IGNORED;
                    }
                    raw_leave();
                    return MODEM_SMS_DIRECT_STEP_REJECTED;
                }
                /* Restore the accepted prefix after the speculative parse. */
                result = collect(s_raw, s_raw_len, vendor, pdu_hex, pdu_hex_cap,
                                 tpdu_len_out);
            }
            if (result != MODEM_SMS_DIRECT_INCOMPLETE) {
                raw_leave();
                return completed_step(result, pdu_hex);
            }
            s_raw_multiline = true;
            bool held = s_raw_cr_held;
            s_raw_cr_held = false;
            if ((held && !raw_append('\r')) || !raw_append('\n')) {
                raw_leave();
                return MODEM_SMS_DIRECT_STEP_REJECTED;
            }
            return MODEM_SMS_DIRECT_STEP_IGNORED;
        }
        if (s_raw_cr_held) {
            s_raw_cr_held = false;
            if (!raw_append('\r')) {
                raw_leave();
                s_raw_unconsumed = true;
                return MODEM_SMS_DIRECT_STEP_REJECTED;
            }
        }
        if (byte == '\r') {
            s_raw_cr_held = true;
            return MODEM_SMS_DIRECT_STEP_IGNORED;
        }
    }
    if (!raw_append(byte)) {
        /* Leave the first byte beyond the body bound for the host parser. */
        raw_leave();
        s_raw_unconsumed = true;
        return MODEM_SMS_DIRECT_STEP_REJECTED;
    }
    return MODEM_SMS_DIRECT_STEP_IGNORED;
}

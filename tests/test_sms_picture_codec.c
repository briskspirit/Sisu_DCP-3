#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "services/sms_picture_codec.h"

#define CHUNK_MAX 128u

static int s_failures;

static void assert_true(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        s_failures++;
    }
}

/* Convert a raw byte buffer to an uppercase-hex C string (oracle PDU builder). */
static void bytes_to_hex(const uint8_t *bytes, size_t n, char *out, size_t cap) {
    static const char HEX[] = "0123456789ABCDEF";
    size_t pos = 0u;
    for (size_t i = 0u; i < n; i++) {
        assert_true(pos + 2u < cap, "bytes_to_hex capacity");
        if (pos + 2u >= cap) {
            break;
        }
        out[pos++] = HEX[(bytes[i] >> 4) & 0x0fu];
        out[pos++] = HEX[bytes[i] & 0x0fu];
    }
    out[pos] = '\0';
}

static void append_hex_byte(char *dst, size_t cap, size_t *pos, uint8_t value) {
    static const char HEX[] = "0123456789ABCDEF";
    if (*pos + 2u >= cap) {
        s_failures++;
        return;
    }
    dst[(*pos)++] = HEX[(value >> 4) & 0x0fu];
    dst[(*pos)++] = HEX[value & 0x0fu];
    dst[*pos] = '\0';
}

static void append_address(uint8_t *dst, size_t cap, size_t *pos, const char *number) {
    uint8_t digits[SMS_CODEC_ADDRESS_MAX];
    uint8_t count = 0u;
    for (size_t i = 0u; number[i] != '\0'; i++) {
        if (number[i] >= '0' && number[i] <= '9') {
            digits[count++] = (uint8_t)(number[i] - '0');
        }
    }
    assert_true(*pos + 2u + (count + 1u) / 2u <= cap, "address capacity");
    dst[(*pos)++] = count;
    dst[(*pos)++] = 0x91u;
    for (uint8_t i = 0u; i < count; i = (uint8_t)(i + 2u)) {
        uint8_t low = digits[i];
        uint8_t high = (i + 1u < count) ? digits[i + 1u] : 0x0fu;
        dst[(*pos)++] = (uint8_t)(low | (high << 4));
    }
}

static void build_deliver_pdu(char *hex,
                              size_t cap,
                              const uint8_t *payload,
                              uint8_t payload_len,
                              uint8_t ref,
                              uint8_t total,
                              uint8_t seq) {
    uint8_t pdu[192];
    size_t pos = 0u;
    pdu[pos++] = 0x00u;
    pdu[pos++] = 0x40u;
    append_address(pdu, sizeof(pdu), &pos, "+12125550123");
    pdu[pos++] = 0x00u;
    pdu[pos++] = 0xf5u;
    pdu[pos++] = 0x62u;
    pdu[pos++] = 0x50u;
    pdu[pos++] = 0x71u;
    pdu[pos++] = 0x12u;
    pdu[pos++] = 0x34u;
    pdu[pos++] = 0x56u;
    pdu[pos++] = 0x00u;
    pdu[pos++] = (uint8_t)(12u + payload_len);
    pdu[pos++] = 0x0bu;
    pdu[pos++] = 0x05u;
    pdu[pos++] = 0x04u;
    pdu[pos++] = 0x15u;
    pdu[pos++] = 0x8au;
    pdu[pos++] = 0x00u;
    pdu[pos++] = 0x00u;
    pdu[pos++] = 0x00u;
    pdu[pos++] = 0x03u;
    pdu[pos++] = ref;
    pdu[pos++] = total;
    pdu[pos++] = seq;
    memcpy(&pdu[pos], payload, payload_len);
    pos += payload_len;

    size_t hex_pos = 0u;
    for (size_t i = 0u; i < pos; i++) {
        append_hex_byte(hex, cap, &hex_pos, pdu[i]);
    }
}

static void test_picture_payload_round_trip(void) {
    store_picture_message_t picture;
    memset(&picture, 0, sizeof(picture));
    picture.used = true;
    picture.width = STORE_PICTURE_WIDTH;
    picture.height = STORE_PICTURE_HEIGHT;
    picture.bitmap_len = STORE_PICTURE_BITMAP_BYTES;
    for (uint16_t i = 0u; i < picture.bitmap_len; i++) {
        picture.bitmap[i] = (uint8_t)(i ^ 0x5au);
    }
    strcpy(picture.text, "HELLO");

    uint8_t payload[SMS_CODEC_BINARY_MAX];
    uint16_t payload_len = 0u;
    uint8_t chunks = 0u;
    assert_true(sms_picture_payload_encode(&picture, picture.text, payload, sizeof(payload), &payload_len, &chunks),
                "picture payload encodes");
    assert_true(chunks == 3u, "picture payload spans three 128-byte chunks");

    store_picture_message_t decoded;
    memset(&decoded, 0, sizeof(decoded));
    assert_true(sms_picture_payload_decode(payload, payload_len, &decoded), "picture payload decodes");
    assert_true(decoded.used, "decoded picture is used");
    assert_true(decoded.width == STORE_PICTURE_WIDTH && decoded.height == STORE_PICTURE_HEIGHT, "decoded size");
    assert_true(strcmp(decoded.text, "HELLO") == 0, "decoded text");
    assert_true(memcmp(decoded.bitmap, picture.bitmap, STORE_PICTURE_BITMAP_BYTES) == 0, "decoded bitmap");
}

static void test_multipart_pdu_picture_decode(void) {
    store_picture_message_t picture;
    memset(&picture, 0, sizeof(picture));
    picture.used = true;
    picture.width = STORE_PICTURE_WIDTH;
    picture.height = STORE_PICTURE_HEIGHT;
    picture.bitmap_len = STORE_PICTURE_BITMAP_BYTES;
    for (uint16_t i = 0u; i < picture.bitmap_len; i++) {
        picture.bitmap[i] = (uint8_t)(0xffu - i);
    }
    strcpy(picture.text, "Saved text");

    uint8_t payload[SMS_CODEC_BINARY_MAX];
    uint16_t payload_len = 0u;
    uint8_t chunks = 0u;
    assert_true(sms_picture_payload_encode(&picture, picture.text, payload, sizeof(payload), &payload_len, &chunks),
                "payload encodes for PDU");

    uint8_t reassembled[SMS_CODEC_BINARY_MAX];
    memset(reassembled, 0, sizeof(reassembled));
    uint16_t total_len = 0u;
    uint8_t received = 0u;
    for (uint8_t seq = 1u; seq <= chunks; seq++) {
        uint16_t offset = (uint16_t)((seq - 1u) * CHUNK_MAX);
        uint8_t chunk_len = payload_len - offset > CHUNK_MAX ? CHUNK_MAX : (uint8_t)(payload_len - offset);
        char pdu_hex[420];
        build_deliver_pdu(pdu_hex, sizeof(pdu_hex), &payload[offset], chunk_len, 0x42u, chunks, seq);

        sms_codec_message_t decoded;
        memset(&decoded, 0, sizeof(decoded));
        assert_true(sms_pdu_decode(pdu_hex, &decoded), "PDU segment decodes");
        assert_true(decoded.binary, "PDU segment is binary");
        assert_true(decoded.has_ports, "PDU segment has ports");
        assert_true(decoded.dest_port == SMS_CODEC_PICTURE_PORT &&
                    decoded.source_port == 0x0000u,
                    "PDU segment uses picture-message application port");
        assert_true(decoded.has_concat && !decoded.concat_ref_16bit &&
                    decoded.concat_ref == 0x42u &&
                    decoded.concat_total == chunks && decoded.concat_seq == seq,
                    "PDU segment has concat metadata");
        memcpy(&reassembled[offset], decoded.binary_data, decoded.binary_len);
        if (offset + decoded.binary_len > total_len) {
            total_len = (uint16_t)(offset + decoded.binary_len);
        }
        received++;
    }
    assert_true(received == chunks && total_len == payload_len, "multipart payload reassembles");

    store_picture_message_t decoded_picture;
    memset(&decoded_picture, 0, sizeof(decoded_picture));
    assert_true(sms_picture_payload_decode(reassembled, total_len, &decoded_picture), "reassembled picture decodes");
    assert_true(strcmp(decoded_picture.text, "Saved text") == 0, "reassembled text");
    assert_true(memcmp(decoded_picture.bitmap, picture.bitmap, STORE_PICTURE_BITMAP_BYTES) == 0, "reassembled bitmap");
}

/* Pack an ASCII string into GSM 7-bit septets, LSB-first. Only chars whose GSM7
 * default-alphabet code equals their ASCII value (A-Z a-z 0-9 space and common
 * punctuation) are used here, so each septet code = the char value. Returns the
 * packed byte count (ceil(septets*7/8)). */
static uint8_t pack_gsm7(const char *text, uint8_t *out, size_t out_cap) {
    uint8_t septets = (uint8_t)strlen(text);
    uint8_t bytes = (uint8_t)((uint16_t)(septets * 7u + 7u) / 8u);
    assert_true(bytes <= out_cap, "gsm7 pack capacity");
    memset(out, 0, bytes);
    uint16_t bitpos = 0u;
    for (uint8_t i = 0u; i < septets; i++) {
        uint8_t code = (uint8_t)(text[i]) & 0x7fu;
        uint16_t byte = (uint16_t)(bitpos / 8u);
        uint8_t shift = (uint8_t)(bitpos & 7u);
        out[byte] |= (uint8_t)(code << shift);
        if (shift > 1u) {
            out[byte + 1u] |= (uint8_t)(code >> (8u - shift));
        }
        bitpos = (uint16_t)(bitpos + 7u);
    }
    return bytes;
}

static uint8_t pack_gsm7_codes(const uint8_t *codes, uint8_t septets,
                               uint8_t *out, size_t out_cap) {
    uint8_t bytes = (uint8_t)(((uint16_t)septets * 7u + 7u) / 8u);
    assert_true(codes != NULL && bytes <= out_cap,
                "raw gsm7 pack capacity");
    memset(out, 0, bytes);
    uint16_t bitpos = 0u;
    for (uint8_t i = 0u; i < septets; i++) {
        uint8_t code = codes[i] & 0x7fu;
        uint16_t byte = (uint16_t)(bitpos / 8u);
        uint8_t shift = (uint8_t)(bitpos & 7u);
        out[byte] |= (uint8_t)(code << shift);
        if (shift > 1u) {
            out[byte + 1u] |= (uint8_t)(code >> (8u - shift));
        }
        bitpos = (uint16_t)(bitpos + 7u);
    }
    return bytes;
}

static uint8_t pack_ucs2_codes(const uint16_t *codes, uint8_t count,
                               uint8_t *out, size_t out_cap) {
    size_t bytes = (size_t)count * 2u;
    assert_true(codes != NULL && bytes <= out_cap,
                "UCS2 pack capacity");
    for (uint8_t i = 0u; i < count; i++) {
        out[(size_t)i * 2u] = (uint8_t)(codes[i] >> 8u);
        out[(size_t)i * 2u + 1u] = (uint8_t)codes[i];
    }
    return (uint8_t)bytes;
}

static void build_raw_deliver(char *hex, size_t cap, uint8_t first,
                              uint8_t dcs, const uint8_t *payload,
                              uint8_t udl, uint8_t payload_bytes) {
    uint8_t pdu[192];
    size_t pos = 0u;
    pdu[pos++] = 0x00u; /* no SMSC */
    pdu[pos++] = first;
    append_address(pdu, sizeof(pdu), &pos, "1234");
    pdu[pos++] = 0x00u; /* TP-PID */
    pdu[pos++] = dcs;
    for (uint8_t i = 0u; i < 7u; i++) {
        pdu[pos++] = 0x00u; /* TP-SCTS */
    }
    pdu[pos++] = udl;
    assert_true((payload_bytes == 0u || payload != NULL) &&
                    pos + payload_bytes <= sizeof(pdu),
                "raw DELIVER fixture capacity");
    if (payload_bytes != 0u && payload != NULL &&
        pos + payload_bytes <= sizeof(pdu)) {
        memcpy(&pdu[pos], payload, payload_bytes);
        pos += payload_bytes;
    }
    bytes_to_hex(pdu, pos, hex, cap);
}

static void build_alphanumeric_originator_deliver(
    char *hex, size_t cap, const uint8_t *sender_codes,
    uint8_t sender_septets, uint8_t address_length) {
    uint8_t pdu[64];
    size_t pos = 0u;
    pdu[pos++] = 0x00u; /* no SMSC */
    pdu[pos++] = 0x00u; /* SMS-DELIVER */
    pdu[pos++] = address_length;
    pdu[pos++] = 0xd0u; /* TON=alphanumeric, reserved NPI bits zero */
    pos += pack_gsm7_codes(sender_codes, sender_septets, &pdu[pos],
                           sizeof(pdu) - pos);
    pdu[pos++] = 0x00u; /* TP-PID */
    pdu[pos++] = 0x00u; /* GSM 7-bit TP-DCS */
    for (uint8_t i = 0u; i < 7u; i++) {
        pdu[pos++] = 0x00u; /* TP-SCTS */
    }
    pdu[pos++] = 0x00u; /* empty TP-UD */
    bytes_to_hex(pdu, pos, hex, cap);
}

static uint8_t pack_gsm7_at_bit(const char *text, uint8_t *out,
                                size_t out_cap, uint16_t bitpos) {
    uint8_t septets = (uint8_t)strlen(text);
    uint16_t total_bits = (uint16_t)(bitpos + (uint16_t)septets * 7u);
    uint8_t bytes = (uint8_t)((total_bits + 7u) / 8u);
    assert_true(bytes <= out_cap, "offset gsm7 pack capacity");
    for (uint8_t i = 0u; i < septets; i++) {
        uint8_t code = (uint8_t)text[i] & 0x7fu;
        uint16_t byte = (uint16_t)(bitpos / 8u);
        uint8_t shift = (uint8_t)(bitpos & 7u);
        out[byte] |= (uint8_t)(code << shift);
        if (shift > 1u) {
            out[byte + 1u] |= (uint8_t)(code >> (8u - shift));
        }
        bitpos = (uint16_t)(bitpos + 7u);
    }
    return bytes;
}

static void build_gsm7_udh_deliver(char *hex, size_t cap,
                                   const uint8_t *udh, uint8_t udh_bytes,
                                   const char *text) {
    uint8_t pdu[192];
    size_t pos = 0u;
    pdu[pos++] = 0x00u;                     /* no SMSC */
    pdu[pos++] = 0x40u;                     /* SMS-DELIVER + TP-UDHI */
    append_address(pdu, sizeof(pdu), &pos, "1555010000");
    pdu[pos++] = 0x00u;                     /* TP-PID */
    pdu[pos++] = 0x00u;                     /* GSM 7-bit TP-DCS */
    for (uint8_t i = 0u; i < 7u; i++) {
        pdu[pos++] = 0x00u;                 /* TP-SCTS */
    }

    assert_true(udh != NULL && udh_bytes != 0u &&
                    (uint8_t)(udh[0] + 1u) == udh_bytes,
                "GSM7 UDH fixture is self-consistent");
    uint8_t header_septets = (uint8_t)(
        ((uint16_t)udh_bytes * 8u + 6u) / 7u);
    uint8_t text_septets = (uint8_t)strlen(text);
    pdu[pos++] = (uint8_t)(header_septets + text_septets);

    uint8_t user_data[160];
    memset(user_data, 0, sizeof(user_data));
    memcpy(user_data, udh, udh_bytes);
    uint8_t user_bytes = pack_gsm7_at_bit(
        text, user_data, sizeof(user_data), (uint16_t)header_septets * 7u);
    memcpy(&pdu[pos], user_data, user_bytes);
    pos += user_bytes;
    bytes_to_hex(pdu, pos, hex, cap);
}

/* Build a minimal GSM 7-bit (TP-DCS=0) SMS-DELIVER, no UDH. udl_override lets a
 * test set TP-UDL independently of the payload actually present (to exercise the
 * over-claim path); 0 uses the real septet count. */
static void build_gsm7_deliver(char *hex, size_t cap, const char *text, uint8_t udl_override) {
    uint8_t pdu[192];
    size_t pos = 0u;
    pdu[pos++] = 0x00u;                     /* SMSC length: none */
    pdu[pos++] = 0x00u;                     /* first octet: SMS-DELIVER, no UDHI */
    append_address(pdu, sizeof(pdu), &pos, "1234");
    pdu[pos++] = 0x00u;                     /* TP-PID */
    pdu[pos++] = 0x00u;                     /* TP-DCS: GSM 7-bit default alphabet */
    for (uint8_t i = 0u; i < 7u; i++) {     /* TP-SCTS */
        pdu[pos++] = 0x00u;
    }
    uint8_t payload[160];
    uint8_t septets = (uint8_t)strlen(text);
    uint8_t bytes = pack_gsm7(text, payload, sizeof(payload));
    pdu[pos++] = (udl_override != 0u) ? udl_override : septets; /* TP-UDL */
    memcpy(&pdu[pos], payload, bytes);
    pos += bytes;

    size_t hex_pos = 0u;
    for (size_t i = 0u; i < pos; i++) {
        append_hex_byte(hex, cap, &hex_pos, pdu[i]);
    }
}

static void test_gsm7_text_decode_round_trip(void) {
    char hex[420];
    const char *text = "Hello World 42";
    build_gsm7_deliver(hex, sizeof(hex), text, 0u);

    sms_codec_message_t decoded;
    memset(&decoded, 0, sizeof(decoded));
    assert_true(sms_pdu_decode(hex, &decoded), "GSM7 DELIVER decodes");
    assert_true(!decoded.binary, "GSM7 DELIVER is text, not binary");
    assert_true(strcmp(decoded.text, text) == 0, "GSM7 text round-trips");
}

static void test_gsm7_default_and_extension_utf8(void) {
    static const uint8_t codes[] = {
        'C', 'o', 's', 't', ' ', 0x1bu, 0x65u, ' ',
        0x1bu, 0x28u, 'x', 0x1bu, 0x29u, ' ',
        0x01u, 0x10u, 0x5bu,
    };
    uint8_t packed[32];
    uint8_t bytes = pack_gsm7_codes(codes, sizeof(codes), packed,
                                    sizeof(packed));
    char hex[160];
    build_raw_deliver(hex, sizeof(hex), 0x00u, 0x00u, packed,
                      sizeof(codes), bytes);

    sms_codec_message_t decoded;
    memset(&decoded, 0, sizeof(decoded));
    assert_true(sms_pdu_decode(hex, &decoded),
                "GSM7 default and extension text decodes");
    assert_true(strcmp(decoded.text,
                       "Cost \xe2\x82\xac {x} \xc2\xa3\xce\x94\xc3\x84") == 0,
                "GSM7 extension and non-ASCII default glyphs emit UTF-8");
}

static void test_gsm7_max_utf8_expansion(void) {
    uint8_t codes[MODEM_SMS_TEXT_MAX];
    memset(codes, 0x10u, sizeof(codes)); /* Greek capital delta: two UTF-8 bytes */
    uint8_t packed[140];
    uint8_t bytes = pack_gsm7_codes(codes, sizeof(codes), packed,
                                    sizeof(packed));
    char hex[420];
    build_raw_deliver(hex, sizeof(hex), 0x00u, 0x00u, packed,
                      sizeof(codes), bytes);

    sms_codec_message_t decoded;
    memset(&decoded, 0, sizeof(decoded));
    assert_true(sms_pdu_decode(hex, &decoded),
                "maximum-expansion GSM7 body decodes");
    assert_true(strlen(decoded.text) == MODEM_SMS_DECODED_TEXT_MAX,
                "160 two-byte GSM7 glyphs are retained without truncation");
    bool exact = true;
    for (size_t i = 0u; i < MODEM_SMS_TEXT_MAX; i++) {
        if ((uint8_t)decoded.text[i * 2u] != 0xceu ||
            (uint8_t)decoded.text[i * 2u + 1u] != 0x94u) {
            exact = false;
            break;
        }
    }
    assert_true(exact, "maximum-expansion GSM7 body stays codepoint exact");
}

static void test_ucs2_text_decode(void) {
    static const uint16_t codes[] = {
        'A', 0x00e9u, 0x03a9u, 0x0416u, 0x20acu,
    };
    uint8_t payload[32];
    uint8_t bytes = pack_ucs2_codes(codes, sizeof(codes) / sizeof(codes[0]),
                                    payload, sizeof(payload));
    char hex[160];
    build_raw_deliver(hex, sizeof(hex), 0x00u, 0x08u, payload,
                      bytes, bytes);

    sms_codec_message_t decoded;
    memset(&decoded, 0, sizeof(decoded));
    assert_true(sms_pdu_decode(hex, &decoded), "UCS2 body decodes");
    assert_true(!decoded.binary &&
                    strcmp(decoded.text,
                           "A\xc3\xa9\xce\xa9\xd0\x96\xe2\x82\xac") == 0,
                "UCS2 BMP codepoints emit exact bounded UTF-8");

    static const uint16_t fallback_codes[] = {
        0xfeffu, 'B', 0xd800u, 0x0000u, 0xffffu,
    };
    bytes = pack_ucs2_codes(
        fallback_codes, sizeof(fallback_codes) / sizeof(fallback_codes[0]),
        payload, sizeof(payload));
    build_raw_deliver(hex, sizeof(hex), 0x00u, 0x08u, payload,
                      bytes, bytes);
    memset(&decoded, 0, sizeof(decoded));
    assert_true(sms_pdu_decode(hex, &decoded),
                "UCS2 fallback fixture decodes");
    assert_true(strcmp(decoded.text, "B???") == 0,
                "BOM is skipped and non-glyph UCS2 values fall back safely");
}

static void test_ucs2_max_and_udh_decode(void) {
    uint16_t codes[70];
    for (size_t i = 0u; i < sizeof(codes) / sizeof(codes[0]); i++) {
        codes[i] = 0x4e2du;
    }
    uint8_t payload[140];
    uint8_t bytes = pack_ucs2_codes(codes, 70u, payload, sizeof(payload));
    char hex[420];
    build_raw_deliver(hex, sizeof(hex), 0x00u, 0x08u, payload,
                      bytes, bytes);

    sms_codec_message_t decoded;
    memset(&decoded, 0, sizeof(decoded));
    assert_true(sms_pdu_decode(hex, &decoded),
                "maximum-length UCS2 body decodes");
    assert_true(strlen(decoded.text) == 210u,
                "all 70 three-byte UCS2 glyphs are retained");

    const uint8_t concat_udh_ucs2[] = {
        0x05u, 0x00u, 0x03u, 0x42u, 0x02u, 0x01u, 0x04u, 0x16u,
    };
    build_raw_deliver(hex, sizeof(hex), 0x40u, 0x08u,
                      concat_udh_ucs2, sizeof(concat_udh_ucs2),
                      sizeof(concat_udh_ucs2));
    memset(&decoded, 0, sizeof(decoded));
    assert_true(sms_pdu_decode(hex, &decoded),
                "UCS2 body after concat UDH decodes");
    assert_true(decoded.has_concat && !decoded.concat_ref_16bit &&
                    decoded.concat_ref == 0x42u &&
                    decoded.concat_total == 2u && decoded.concat_seq == 1u &&
                    strcmp(decoded.text, "\xd0\x96") == 0,
                "UCS2 UDH metadata and payload offset are both preserved");

    const uint8_t odd_payload[] = {0x00u};
    build_raw_deliver(hex, sizeof(hex), 0x00u, 0x08u,
                      odd_payload, 1u, 1u);
    memset(&decoded, 0, sizeof(decoded));
    assert_true(!sms_pdu_decode(hex, &decoded),
                "odd-length UCS2 body is rejected");
}

static void test_gsm7_udh_decode(void) {
    char hex[420];
    const uint8_t port_udh[] = {
        0x06u, 0x05u, 0x04u, 0x15u, 0x78u, 0x00u, 0x00u,
    };
    build_gsm7_udh_deliver(hex, sizeof(hex), port_udh,
                           sizeof(port_udh), "STATE?state=Active");

    sms_codec_message_t decoded;
    memset(&decoded, 0, sizeof(decoded));
    assert_true(sms_pdu_decode(hex, &decoded),
                "GSM7 application-port DELIVER decodes");
    assert_true(!decoded.binary && decoded.has_ports,
                "GSM7 application-port metadata is retained");
    assert_true(decoded.dest_port == 0x1578u &&
                    decoded.source_port == 0u,
                "GSM7 application ports decode exactly");
    assert_true(strcmp(decoded.text, "STATE?state=Active") == 0,
                "GSM7 payload starts after the seven-byte UDH");

    /* Six UDH bytes occupy seven septets, leaving one mandatory fill bit before
     * the payload. This catches a decoder that merely skips whole UDH bytes. */
    const uint8_t concat_udh[] = {
        0x05u, 0x00u, 0x03u, 0x42u, 0x02u, 0x01u,
    };
    build_gsm7_udh_deliver(hex, sizeof(hex), concat_udh,
                           sizeof(concat_udh), "Fill bit");
    memset(&decoded, 0, sizeof(decoded));
    assert_true(sms_pdu_decode(hex, &decoded),
                "GSM7 concat-UDH DELIVER decodes");
    assert_true(decoded.has_concat && !decoded.concat_ref_16bit &&
                    decoded.concat_ref == 0x42u &&
                    decoded.concat_total == 2u && decoded.concat_seq == 1u,
                "GSM7 concat metadata decodes exactly");
    assert_true(strcmp(decoded.text, "Fill bit") == 0,
                "GSM7 decoder consumes the UDH fill bit");
}

static void test_gsm7_overclaim_udl_rejected(void) {
    /* TP-UDL claims 60 septets (needs 53 payload bytes) but only "HI" (2 bytes)
     * is present. The caller's `pos + bytes > pdu_len` check must reject
     * it (return false) without reading past the buffer or crashing. */
    char hex[420];
    build_gsm7_deliver(hex, sizeof(hex), "HI", 60u);

    sms_codec_message_t decoded;
    memset(&decoded, 0, sizeof(decoded));
    assert_true(!sms_pdu_decode(hex, &decoded), "over-claiming GSM7 UDL is rejected");

    uint8_t gsm_over_limit[141];
    memset(gsm_over_limit, 0x41u, sizeof(gsm_over_limit));
    build_raw_deliver(hex, sizeof(hex), 0x00u, 0x00u,
                      gsm_over_limit, 161u, sizeof(gsm_over_limit));
    memset(&decoded, 0, sizeof(decoded));
    assert_true(!sms_pdu_decode(hex, &decoded),
                "GSM7 TP-UDL above 160 septets is rejected");

    uint8_t octet_over_limit[141];
    memset(octet_over_limit, 0x41u, sizeof(octet_over_limit));
    build_raw_deliver(hex, sizeof(hex), 0x00u, 0x04u,
                      octet_over_limit, sizeof(octet_over_limit),
                      sizeof(octet_over_limit));
    memset(&decoded, 0, sizeof(decoded));
    assert_true(!sms_pdu_decode(hex, &decoded),
                "octet TP-UDL above 140 bytes is rejected");
}

/* =========================================================================
 * Hand-built known-good oracle PDUs (independent of the codec).
 * Hand-encoded from 3GPP TS 23.040 SMS-DELIVER layout.
 * ========================================================================= */

/* Oracle 1: A canonical 7-bit "hellohello" DELIVER taken from the de-facto
 * standard worked example.
 *   00       SMSC length = 0 (no SMSC)
 *   04       first octet: SMS-DELIVER (MTI=0), no UDHI
 *   0C       TP-OA address-length = 12 digits
 *   91       TOA = international (E.164)
 *   91 51 55 91 64 F6  -> "19155519466" (semi-octet swapped, F pad)
 *   00       TP-PID
 *   00       TP-DCS = GSM 7-bit
 *   00 00 00 00 00 00 00  TP-SCTS (zeroed)
 *   0A       TP-UDL = 10 septets
 *   E8329BFD4697D9EC37   packed "hellohello"
 */
static void test_oracle_gsm7_hellohello(void) {
    const char *hex =
        "0004" "0C91" "915155916466" "00" "00" "00000000000000" "0A" "E8329BFD4697D9EC37";
    /* fix: build precise oracle bytes instead of trusting the comment digits */
    sms_codec_message_t decoded;
    memset(&decoded, 0, sizeof(decoded));
    assert_true(sms_pdu_decode(hex, &decoded), "oracle hellohello decodes");
    assert_true(!decoded.binary, "oracle hellohello is text");
    assert_true(strcmp(decoded.text, "hellohello") == 0, "oracle hellohello text exact");
}

/* Oracle 2: classic 3GPP 7-bit example "hellohello" with the address taken
 * verbatim from the widely-published worked example PDU. Address bytes
 * 91 51 55 91 64 F6 with 12 digit count, international TOA, decode to
 * "19155519466" but that worked example actually carries 11 digits -> we use
 * the published 11-digit form here. */
static void test_oracle_address_international(void) {
    /* Hand-encode +13125551234 (11 digits), international TOA 0x91.
     * digits=11 (0x0B). Semi-octet (independently computed): 31 21 55 15 32 F4 */
    const char *hex =
        "00" "04" "0B91" "31215515" "32F4" "00" "00" "00000000000000" "00";
    /* TP-UDL = 0 septets, empty text */
    sms_codec_message_t decoded;
    memset(&decoded, 0, sizeof(decoded));
    assert_true(sms_pdu_decode(hex, &decoded), "international address PDU decodes");
    assert_true(strcmp(decoded.address, "+13125551234") == 0, "international address decodes with +");
    assert_true(decoded.text[0] == '\0', "zero-UDL text is empty");
}

/* National (non-international) TOA must NOT get a leading '+'. TOA 0xA1. */
static void test_oracle_address_national(void) {
    /* national number 1234 (4 digits, even). Semi-octet: 21 43 */
    const char *hex =
        "00" "04" "04A1" "2143" "00" "00" "00000000000000" "00";
    sms_codec_message_t decoded;
    memset(&decoded, 0, sizeof(decoded));
    assert_true(sms_pdu_decode(hex, &decoded), "national address PDU decodes");
    assert_true(strcmp(decoded.address, "1234") == 0, "national address has no '+'");
}

/* Odd-length BCD address: 3 digits "555" with F pad. international TOA. */
static void test_oracle_address_odd_length(void) {
    /* digits=3 (0x03). semi-octet of "555": 55 F5 */
    const char *hex =
        "00" "04" "0391" "55F5" "00" "00" "00000000000000" "00";
    sms_codec_message_t decoded;
    memset(&decoded, 0, sizeof(decoded));
    assert_true(sms_pdu_decode(hex, &decoded), "odd-length address PDU decodes");
    assert_true(strcmp(decoded.address, "+555") == 0, "odd-length address drops F pad");
}

static void test_alphanumeric_originator_addresses(void) {
    char hex[160];
    sms_codec_message_t decoded;

    /* Four septets occupy 28 bits: exactly seven useful semi-octets and four
     * value bytes. This independently pins the non-BCD length conversion. */
    static const uint8_t sisu_codes[] = {'S', 'I', 'S', 'U'};
    build_alphanumeric_originator_deliver(
        hex, sizeof(hex), sisu_codes, sizeof(sisu_codes), 7u);
    memset(&decoded, 0, sizeof(decoded));
    assert_true(sms_pdu_decode(hex, &decoded),
                "alphanumeric originator decodes");
    assert_true(strcmp(decoded.address, "SISU") == 0,
                "alphanumeric originator uses GSM7 rather than BCD or plus");

    /* '^' is ESC 0x14 in the GSM default extension table. Nine septets occupy
     * 63 bits and therefore sixteen useful semi-octets. */
    static const uint8_t extension_codes[] = {
        'I', 'N', 'F', 'O', 0x1bu, 0x14u, 'B', 'O', 'X',
    };
    build_alphanumeric_originator_deliver(
        hex, sizeof(hex), extension_codes, sizeof(extension_codes), 16u);
    memset(&decoded, 0, sizeof(decoded));
    assert_true(sms_pdu_decode(hex, &decoded),
                "alphanumeric extension originator decodes");
    assert_true(strcmp(decoded.address, "INFO^BOX") == 0,
                "alphanumeric originator consumes ESC as one extension glyph");

    static const uint8_t euro_codes[] = {
        'P', 'A', 'Y', 0x1bu, 0x65u,
    };
    build_alphanumeric_originator_deliver(
        hex, sizeof(hex), euro_codes, sizeof(euro_codes), 9u);
    memset(&decoded, 0, sizeof(decoded));
    assert_true(sms_pdu_decode(hex, &decoded),
                "alphanumeric euro originator decodes");
    assert_true(strcmp(decoded.address, "PAY\xe2\x82\xac") == 0,
                "alphanumeric euro is emitted as bounded UTF-8");
}

static void test_alphanumeric_originator_length_bounds(void) {
    char hex[160];
    sms_codec_message_t decoded;
    static const uint8_t four_codes[] = {'T', 'E', 'S', 'T'};

    /* Four septets need seven useful semi-octets. Eight falsely claims that a
     * fill-only nibble is address data and would shift TP-PID in a lax parser. */
    build_alphanumeric_originator_deliver(
        hex, sizeof(hex), four_codes, sizeof(four_codes), 8u);
    memset(&decoded, 0, sizeof(decoded));
    assert_true(!sms_pdu_decode(hex, &decoded),
                "non-canonical alphanumeric semi-octet length is rejected");

    static const uint8_t twelve_codes[] = {
        'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L',
    };
    /* Twelve septets need 21 useful semi-octets / 11 value bytes, exceeding
     * TS 23.040's ten-value-octet address maximum. */
    build_alphanumeric_originator_deliver(
        hex, sizeof(hex), twelve_codes, sizeof(twelve_codes), 21u);
    memset(&decoded, 0, sizeof(decoded));
    assert_true(!sms_pdu_decode(hex, &decoded),
                "overlength alphanumeric originator is rejected");
}

/* Address digit nibbles 0xA/0xB carry '*' and '#', while 0xC..0xE carry
 * 'a'..'c', per 3GPP TS 23.040 9.1.2.3. Only 0xF is filler. */
static void test_address_star_hash(void) {
    /* national number "*123": digits 0x0A 0x01 0x02 0x03; semi-octets 0x1A 0x32. */
    const char *hex = "000404A11A32" "00" "00" "00000000000000" "00";
    sms_codec_message_t decoded;
    memset(&decoded, 0, sizeof(decoded));
    assert_true(sms_pdu_decode(hex, &decoded), "star/hash address PDU decodes");
    assert_true(strcmp(decoded.address, "*123") == 0,
                "'*123' decodes with the '*' (0xA nibble -> '*')");

    const char *supplementary =
        "000405A1BADCFE" "00" "00" "00000000000000" "00";
    memset(&decoded, 0, sizeof(decoded));
    assert_true(sms_pdu_decode(supplementary, &decoded),
                "supplementary BCD address digits decode");
    assert_true(strcmp(decoded.address, "*#abc") == 0,
                "BCD A through E map to star, hash, and a through c");
}

/* Empty international address (0 digits, TOA 0x91) decodes to a lone "+".
 * Accepted degenerate behavior: a real network never sends a 0-digit address, and
 * rejecting it here would drop an otherwise-valid message, so we keep the message
 * (with a "+" sender) rather than fail the decode. */
static void test_address_empty_international(void) {
    const char *hex = "0004009100000000000000000000"; /* digits=00 toa=91, udl=00 */
    sms_codec_message_t decoded;
    memset(&decoded, 0, sizeof(decoded));
    bool ok = sms_pdu_decode(hex, &decoded);
    assert_true(ok, "empty-intl address PDU still decodes (message preserved)");
    assert_true(strcmp(decoded.address, "+") == 0, "0-digit intl address -> '+' (degenerate, accepted)");
}

/* GSM7 septet boundary sweep: lengths 1..160. Build each with pack_gsm7 and
 * confirm the codec recovers the exact string. Exercises every fill-bit phase
 * (the 7-into-8 carry at septet indices 8,16,... where a packed byte ends on a
 * septet boundary). */
static void test_gsm7_length_sweep(void) {
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789abcdefghijklmnopqrstuvwxyz "
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789abcdefghijklmnopqrstuvwxyz "
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    /* alphabet has >= 160 GSM7-safe chars */
    assert_true(strlen(alphabet) >= 160u, "sweep alphabet long enough");
    for (uint8_t len = 1u; len <= 160u; len++) {
        char text[161];
        memcpy(text, alphabet, len);
        text[len] = '\0';
        char hex[420];
        build_gsm7_deliver(hex, sizeof(hex), text, 0u);

        sms_codec_message_t decoded;
        memset(&decoded, 0, sizeof(decoded));
        bool ok = sms_pdu_decode(hex, &decoded);
        if (!ok) {
            char msg[64];
            snprintf(msg, sizeof(msg), "gsm7 sweep len=%u decodes", (unsigned)len);
            assert_true(false, msg);
            continue;
        }
        if (strcmp(decoded.text, text) != 0) {
            char msg[80];
            snprintf(msg, sizeof(msg), "gsm7 sweep len=%u round-trips", (unsigned)len);
            assert_true(false, msg);
        }
    }
}

/* Septet-boundary special case: when septets is a multiple of 8, the packed
 * byte count is exactly septets*7/8 and there is NO trailing partial byte. The
 * decoder must read exactly that many bytes and not over-read. len=8,16,...,160. */
static void test_gsm7_multiple_of_8(void) {
    static const char base[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZABCDEFGHIJKLMNOPQRSTUVWXYZABCDEFGHIJKLMNOP"
        "QRSTUVWXYZABCDEFGHIJKLMNOPQRSTUVWXYZABCDEFGHIJKLMNOPQRSTUVWXYZABCDEF"
        "GHIJKLMNOPQRSTUVWXYZABCDEFGHIJKLMNOP";
    for (uint8_t len = 8u; len <= 160u; len = (uint8_t)(len + 8u)) {
        char text[161];
        memcpy(text, base, len);
        text[len] = '\0';
        char hex[420];
        build_gsm7_deliver(hex, sizeof(hex), text, 0u);
        sms_codec_message_t decoded;
        memset(&decoded, 0, sizeof(decoded));
        assert_true(sms_pdu_decode(hex, &decoded), "gsm7 mult-of-8 decodes");
        assert_true(strcmp(decoded.text, text) == 0, "gsm7 mult-of-8 round-trips");
    }
}

/* Invalid hex input must be rejected without crashing. */
static void test_invalid_hex_rejected(void) {
    sms_codec_message_t decoded;
    memset(&decoded, 0, sizeof(decoded));
    assert_true(!sms_pdu_decode("00ZZ", &decoded), "non-hex char rejected");
    assert_true(!sms_pdu_decode("000", &decoded), "odd hex nibble count rejected");
    assert_true(!sms_pdu_decode("0", &decoded), "single nibble rejected");
    assert_true(!sms_pdu_decode("", &decoded), "empty hex rejected");
    assert_true(!sms_pdu_decode("00", &decoded), "one byte (too short) rejected");
    assert_true(!sms_pdu_decode("GG", &decoded), "leading non-hex rejected");
    /* Whitespace is tolerated by read_hex_byte; "00 04 ..." should still parse a
     * full valid PDU. */
    /* smsc=00 first=04 digits=04 toa=A1 addr=21 43 pid=00 dcs=00 scts(7) udl=00 */
    const char *spaced = "00 04 04 A1 21 43 00 00 00 00 00 00 00 00 00 00";
    assert_true(sms_pdu_decode(spaced, &decoded), "whitespace-separated hex parses");
    assert_true(strcmp(decoded.address, "1234") == 0, "whitespace PDU address ok");
}

/* NULL argument guards. */
static void test_null_guards(void) {
    sms_codec_message_t decoded;
    store_picture_message_t pic;
    uint8_t buf[16];
    uint16_t olen;
    uint8_t ochunks;
    assert_true(!sms_pdu_decode(0, &decoded), "decode null hex rejected");
    assert_true(!sms_pdu_decode("0004040000", 0), "decode null out rejected");
    assert_true(!sms_picture_payload_decode(0, 4u, &pic), "payload decode null in rejected");
    assert_true(!sms_picture_payload_decode(buf, 0u, &pic), "payload decode zero len rejected");
    assert_true(!sms_picture_payload_encode(0, "x", buf, sizeof(buf), &olen, &ochunks),
                "payload encode null message rejected");
    memset(&pic, 0, sizeof(pic));
    assert_true(!sms_picture_payload_encode(&pic, "x", 0, sizeof(buf), &olen, &ochunks),
                "payload encode null dst rejected");
    assert_true(!sms_picture_payload_encode(&pic, "x", buf, sizeof(buf), 0, &ochunks),
                "payload encode null out_len rejected");
    assert_true(!sms_picture_payload_encode(&pic, "x", buf, sizeof(buf), &olen, 0),
                "payload encode null out_chunks rejected");
}

/* Truncated PDUs at every prefix length must be rejected (never crash / read OOB).
 * Build one full valid binary picture PDU, then feed every truncation of it. */
static void test_truncated_pdus(void) {
    store_picture_message_t picture;
    memset(&picture, 0, sizeof(picture));
    picture.used = true;
    picture.width = STORE_PICTURE_WIDTH;
    picture.height = STORE_PICTURE_HEIGHT;
    picture.bitmap_len = STORE_PICTURE_BITMAP_BYTES;
    strcpy(picture.text, "T");
    uint8_t payload[SMS_CODEC_BINARY_MAX];
    uint16_t payload_len = 0u;
    uint8_t chunks = 0u;
    assert_true(sms_picture_payload_encode(&picture, picture.text, payload, sizeof(payload),
                                           &payload_len, &chunks),
                "truncation source encodes");
    uint16_t chunk_len = payload_len > CHUNK_MAX ? CHUNK_MAX : payload_len;
    char full[420];
    build_deliver_pdu(full, sizeof(full), payload, (uint8_t)chunk_len, 0x10u, chunks, 1u);

    size_t full_len = strlen(full);
    /* Truncate to every even-nibble prefix and ensure no crash. */
    for (size_t cut = 0u; cut < full_len; cut += 2u) {
        char part[420];
        memcpy(part, full, cut);
        part[cut] = '\0';
        sms_codec_message_t decoded;
        memset(&decoded, 0, sizeof(decoded));
        /* We only require: no sanitizer crash. The boolean result is unconstrained
         * for a truncated PDU; ASan/UBSan catch any OOB read inside the decode. */
        (void)sms_pdu_decode(part, &decoded);
    }
    /* The full PDU must still decode cleanly. */
    sms_codec_message_t decoded;
    memset(&decoded, 0, sizeof(decoded));
    assert_true(sms_pdu_decode(full, &decoded), "full PDU still decodes after truncation sweep");
    assert_true(decoded.binary && decoded.has_concat, "full PDU binary+concat");
}

/* Over-length hex (more bytes than pdu[] holds, 192) must be rejected, not
 * truncated-and-accepted. */
static void test_overlength_pdu_rejected(void) {
    char hex[600];
    size_t pos = 0u;
    for (size_t i = 0u; i < 210u; i++) { /* 210 bytes > 192 buffer */
        hex[pos++] = '0';
        hex[pos++] = '0';
    }
    hex[pos] = '\0';
    sms_codec_message_t decoded;
    memset(&decoded, 0, sizeof(decoded));
    assert_true(!sms_pdu_decode(hex, &decoded), "over-length (>192 byte) PDU rejected");
}

/* 8-bit binary DELIVER with UDHI but a UDH that consumes the entire UDL
 * (payload length 0). Must decode with binary_len == 0 and not over-read. */
static void test_binary_udh_only_no_payload(void) {
    /* Build: smsc=0, first=0x44 (DELIVER+UDHI), addr 1234 national, PID=0,
     * DCS=0xF5 (8-bit), SCTS=0. UDH = port IEI only:
     *   UDHL=06, IEI=05, IEDL=04, dest_hi dest_lo src_hi src_lo  (6 bytes after UDHL).
     * Total UDH = 7 bytes -> payload_offset=7. Set UDL=7 -> 0 payload bytes. */
    uint8_t pdu[64];
    size_t p = 0u;
    pdu[p++] = 0x00u; pdu[p++] = 0x44u;
    pdu[p++] = 0x04u; pdu[p++] = 0xA1u; pdu[p++] = 0x21u; pdu[p++] = 0x43u;
    pdu[p++] = 0x00u; pdu[p++] = 0xF5u;
    for (int i = 0; i < 7; i++) pdu[p++] = 0x00u;
    pdu[p++] = 0x07u; /* UDL = 7 (the whole UDH, no payload) */
    pdu[p++] = 0x06u; /* UDHL = 6 */
    pdu[p++] = 0x05u; /* IEI = app-port 16-bit */
    pdu[p++] = 0x04u; /* IEDL = 4 */
    pdu[p++] = 0x15u; pdu[p++] = 0x8Au; /* dest port 0x158A */
    pdu[p++] = 0x00u; pdu[p++] = 0x00u; /* src port 0 */
    char hex[160];
    bytes_to_hex(pdu, p, hex, sizeof(hex));
    sms_codec_message_t decoded;
    memset(&decoded, 0, sizeof(decoded));
    assert_true(sms_pdu_decode(hex, &decoded), "UDH-only binary decodes");
    assert_true(decoded.binary, "UDH-only is binary");
    assert_true(decoded.has_ports && decoded.dest_port == SMS_CODEC_PICTURE_PORT,
                "UDH-only ports parsed");
    assert_true(decoded.binary_len == 0u, "UDH-only payload length zero");
}

/* 8-bit binary DELIVER with UDHI set but a malformed UDH (UDHL larger than the
 * user data). FIXED: the decoder rejects it rather than treating the UDH header
 * bytes as binary payload (parse_udh consumed nothing -> payload_offset 0). */
static void test_binary_udh_malformed_rejected(void) {
    uint8_t pdu[64];
    size_t p = 0u;
    pdu[p++] = 0x00u; pdu[p++] = 0x44u; /* smsc=0, DELIVER+UDHI */
    pdu[p++] = 0x04u; pdu[p++] = 0xA1u; pdu[p++] = 0x21u; pdu[p++] = 0x43u; /* addr 1234 */
    pdu[p++] = 0x00u; pdu[p++] = 0xF5u; /* PID=0, DCS=0xF5 (8-bit) */
    for (int i = 0; i < 7; i++) pdu[p++] = 0x00u; /* SCTS */
    pdu[p++] = 0x03u; /* UDL = 3 */
    pdu[p++] = 0xFFu; pdu[p++] = 0xAAu; pdu[p++] = 0xBBu; /* UDHL=0xFF (absurd) + 2 bytes */
    char hex[160];
    bytes_to_hex(pdu, p, hex, sizeof(hex));
    sms_codec_message_t decoded;
    memset(&decoded, 0, sizeof(decoded));
    assert_true(!sms_pdu_decode(hex, &decoded), "malformed UDH (UDHL>UDL) is rejected");
}

/* SMS-SUBMIT (MTI=1) round trip for GSM7 text, with VPF=relative (1 byte). */
static void test_submit_gsm7_relative_vp(void) {
    /* smsc=0, first=0x11 (SUBMIT MTI=1, VPF=relative=0x10), MR=0,
     * DA len=4 national, PID=0, DCS=0, VP=0xAA (1 byte), UDL, packed text. */
    uint8_t pdu[64];
    size_t p = 0u;
    pdu[p++] = 0x00u;
    pdu[p++] = 0x11u; /* SUBMIT, VPF=10 (relative) */
    pdu[p++] = 0x00u; /* MR */
    pdu[p++] = 0x04u; /* DA digits */
    pdu[p++] = 0xA1u; /* national */
    pdu[p++] = 0x21u; pdu[p++] = 0x43u; /* 1234 */
    pdu[p++] = 0x00u; /* PID */
    pdu[p++] = 0x00u; /* DCS GSM7 */
    pdu[p++] = 0xAAu; /* VP relative, 1 byte */
    const char *text = "Hi42";
    uint8_t packed[16];
    uint8_t bytes = pack_gsm7(text, packed, sizeof(packed));
    pdu[p++] = (uint8_t)strlen(text); /* UDL */
    memcpy(&pdu[p], packed, bytes);
    p += bytes;
    char hex[160];
    bytes_to_hex(pdu, p, hex, sizeof(hex));
    sms_codec_message_t decoded;
    memset(&decoded, 0, sizeof(decoded));
    assert_true(sms_pdu_decode(hex, &decoded), "SUBMIT relative-VP decodes");
    assert_true(decoded.submit, "SUBMIT flag set");
    assert_true(!decoded.binary, "SUBMIT GSM7 is text");
    assert_true(strcmp(decoded.text, text) == 0, "SUBMIT GSM7 text round-trips");
    assert_true(strcmp(decoded.address, "1234") == 0, "SUBMIT DA decoded");
}

/* SUBMIT with VPF=absolute (7 bytes). */
static void test_submit_absolute_vp(void) {
    uint8_t pdu[64];
    size_t p = 0u;
    pdu[p++] = 0x00u;
    pdu[p++] = 0x19u; /* SUBMIT, VPF=11 (absolute) -> bits 4..3 = 11 */
    pdu[p++] = 0x00u; /* MR */
    pdu[p++] = 0x04u; pdu[p++] = 0xA1u; pdu[p++] = 0x21u; pdu[p++] = 0x43u;
    pdu[p++] = 0x00u; /* PID */
    pdu[p++] = 0x00u; /* DCS */
    for (int i = 0; i < 7; i++) pdu[p++] = 0x00u; /* absolute VP 7 bytes */
    const char *text = "OK";
    uint8_t packed[8];
    uint8_t bytes = pack_gsm7(text, packed, sizeof(packed));
    pdu[p++] = (uint8_t)strlen(text);
    memcpy(&pdu[p], packed, bytes);
    p += bytes;
    char hex[160];
    bytes_to_hex(pdu, p, hex, sizeof(hex));
    sms_codec_message_t decoded;
    memset(&decoded, 0, sizeof(decoded));
    assert_true(sms_pdu_decode(hex, &decoded), "SUBMIT absolute-VP decodes");
    assert_true(decoded.submit && strcmp(decoded.text, "OK") == 0, "SUBMIT absolute-VP text ok");
}

/* Telit LE910C1-WWX stores a text-mode draft saved without a recipient as a
 * SUBMIT PDU with TP-DA length 0 and national TOA 0x81. The layout mirrors
 * that AT+CMGW behaviour with a documentation SMSC number and body "Jadja". */
static void test_submit_telit_empty_destination(void) {
    const char *hex = "07912121550501F011FF00810000A705CA30591D06";
    sms_codec_message_t decoded;
    memset(&decoded, 0, sizeof(decoded));

    assert_true(sms_pdu_decode(hex, &decoded), "Telit empty-destination draft decodes");
    assert_true(decoded.submit, "Telit draft is SUBMIT");
    assert_true(decoded.address[0] == '\0', "Telit empty destination stays empty");
    assert_true(strcmp(decoded.text, "Jadja") == 0, "Telit draft body decodes as GSM7");
}

/* MTI = 2 or 3 (STATUS-REPORT / reserved) must be rejected. */
static void test_unsupported_mti_rejected(void) {
    sms_codec_message_t decoded;
    memset(&decoded, 0, sizeof(decoded));
    /* first octet 0x02 -> MTI=2 */
    assert_true(!sms_pdu_decode("0002040000000000", &decoded), "MTI=2 rejected");
    assert_true(!sms_pdu_decode("0003040000000000", &decoded), "MTI=3 rejected");
}

/* picture payload decode: malformed / truncated chunk headers must be rejected,
 * not over-read. */
static void test_picture_payload_malformed(void) {
    store_picture_message_t out;
    /* wrong magic */
    uint8_t bad_magic[8] = {0x31u, 0x02u, 0x00u, 0x04u, 0x00u, 0x00u, 0x00u, 0x00u};
    assert_true(!sms_picture_payload_decode(bad_magic, sizeof(bad_magic), &out),
                "wrong magic rejected");
    /* magic only, no chunk -> no picture -> reject */
    uint8_t magic_only[1] = {0x30u};
    assert_true(!sms_picture_payload_decode(magic_only, sizeof(magic_only), &out),
                "magic-only (no bitmap) rejected");
    /* chunk header claims length beyond buffer */
    uint8_t overclaim[6] = {0x30u, 0x02u, 0x10u, 0x00u, 0x00u, 0x00u};
    assert_true(!sms_picture_payload_decode(overclaim, sizeof(overclaim), &out),
                "chunk over-claim rejected");
    /* bitmap chunk with bad format marker */
    uint8_t bad_fmt[10] = {0x30u, 0x02u, 0x00u, 0x06u, 0x00u, STORE_PICTURE_WIDTH,
                           STORE_PICTURE_HEIGHT, 0x99u, 0x00u, 0x00u};
    assert_true(!sms_picture_payload_decode(bad_fmt, sizeof(bad_fmt), &out),
                "bitmap bad format byte rejected");
    uint8_t tiny[] = {0x30u, 0x02u, 0x00u, 0x05u, 0x00u, 8u, 1u, 1u, 0x80u, 0xffu};
    assert_true(sms_picture_payload_decode(tiny, sizeof(tiny) - 1u, &out), "small complete picture valid");
    assert_true(!sms_picture_payload_decode(tiny, sizeof(tiny), &out), "trailing partial chunk rejected");
    tiny[3] = 6u;
    assert_true(!sms_picture_payload_decode(tiny, sizeof(tiny), &out), "bitmap length must match geometry");
    tiny[3] = 5u;
    uint8_t duplicate[17];
    memcpy(duplicate, tiny, 9u);
    memcpy(duplicate + 9u, tiny + 1u, 8u);
    assert_true(!sms_picture_payload_decode(duplicate, sizeof(duplicate), &out), "multiple bitmaps are not silently replaced");
    uint8_t tall[12] = {0x30u, 0x02u, 0x00u, 8u, 0u, 1u, 29u, 1u};
    assert_true(!sms_picture_payload_decode(tall, sizeof(tall), &out), "unsupported geometry is not silently cropped");
}

/* picture payload encode: empty text (text_len==0) must still encode a valid
 * bitmap-only payload, and capacity-too-small must be rejected. */
static void test_picture_payload_encode_edges(void) {
    store_picture_message_t picture;
    memset(&picture, 0, sizeof(picture));
    picture.used = true;
    picture.width = STORE_PICTURE_WIDTH;
    picture.height = STORE_PICTURE_HEIGHT;
    picture.bitmap_len = STORE_PICTURE_BITMAP_BYTES;
    for (uint16_t i = 0u; i < picture.bitmap_len; i++) {
        picture.bitmap[i] = (uint8_t)(i * 7u + 1u);
    }
    uint8_t payload[SMS_CODEC_BINARY_MAX];
    uint16_t olen = 0u;
    uint8_t ochunks = 0u;
    /* NULL text == no text chunk */
    assert_true(sms_picture_payload_encode(&picture, 0, payload, sizeof(payload), &olen, &ochunks),
                "encode with null text ok");
    store_picture_message_t back;
    memset(&back, 0, sizeof(back));
    assert_true(sms_picture_payload_decode(payload, olen, &back), "null-text payload decodes");
    assert_true(back.text[0] == '\0', "null-text decodes to empty text");
    assert_true(memcmp(back.bitmap, picture.bitmap, STORE_PICTURE_BITMAP_BYTES) == 0,
                "null-text bitmap round-trips");

    /* capacity exactly one byte too small for the bitmap chunk -> reject, no overrun */
    /* full payload needs: 1 (magic) + 3 (bitmap hdr) + 4 (marker/w/h/fmt) + 252 = 260 */
    uint8_t small[259];
    assert_true(!sms_picture_payload_encode(&picture, 0, small, sizeof(small), &olen, &ochunks),
                "too-small capacity rejected");
    /* cap == 0 -> reject immediately */
    assert_true(!sms_picture_payload_encode(&picture, 0, small, 0u, &olen, &ochunks),
                "zero capacity rejected");
}

/* Concatenation IEI 0x08 carries a true 16-bit reference. */
static void test_concat_16bit_ref(void) {
    /* DELIVER+UDHI, national addr, DCS 0xF5, UDL covers UDH(0x08 concat) + 1 byte
     * payload. UDH: UDHL=06, IEI=08, IEDL=04, ref_hi=0xAB ref_lo=0xCD total=03 seq=02.
     * UDL = 1(UDHL) + 6 UDH content + 1 payload = 8. */
    uint8_t pdu[64];
    size_t p = 0u;
    pdu[p++] = 0x00u; pdu[p++] = 0x44u;
    pdu[p++] = 0x04u; pdu[p++] = 0xA1u; pdu[p++] = 0x21u; pdu[p++] = 0x43u;
    pdu[p++] = 0x00u; pdu[p++] = 0xF5u;
    for (int i = 0; i < 7; i++) pdu[p++] = 0x00u;
    pdu[p++] = 0x08u; /* UDL = UDHL(1)+6 UDH bytes + 1 payload = 8 */
    pdu[p++] = 0x06u; /* UDHL */
    pdu[p++] = 0x08u; /* IEI concat-16 */
    pdu[p++] = 0x04u; /* len */
    pdu[p++] = 0xABu; pdu[p++] = 0xCDu; /* 16-bit ref */
    pdu[p++] = 0x03u; /* total */
    pdu[p++] = 0x02u; /* seq */
    pdu[p++] = 0x7Eu; /* 1 payload byte */
    char hex[128];
    bytes_to_hex(pdu, p, hex, sizeof(hex));
    sms_codec_message_t decoded;
    memset(&decoded, 0, sizeof(decoded));
    assert_true(sms_pdu_decode(hex, &decoded), "concat-16 decodes");
    assert_true(decoded.has_concat && decoded.concat_ref_16bit,
                "concat-16 kind is preserved");
    assert_true(decoded.concat_ref == 0xABCDu,
                "concat-16 reference preserves both octets");
    assert_true(decoded.concat_total == 0x03u && decoded.concat_seq == 0x02u,
                "concat-16 total/seq");
    assert_true(decoded.binary_len == 1u && decoded.binary_data[0] == 0x7Eu,
                "concat-16 single payload byte");
}

/* Pin every DCS family the decoder accepts. Unsupported compressed/reserved
 * groups stay opaque so carrier data can never be rendered as user text. */
static void test_dcs_classification(void) {
    static const uint8_t binary_dcs[] = {
        0x04u, /* general 8-bit */
        0xf5u, /* class-1 8-bit */
        0x20u, /* compressed general alphabet */
        0x80u, /* reserved group */
        0x0cu, /* reserved general alphabet */
        0xf8u, /* reserved bit in class-coded group */
    };
    const uint8_t payload = 0x41u;
    char hex[96];
    for (size_t i = 0u; i < sizeof(binary_dcs); i++) {
        build_raw_deliver(hex, sizeof(hex), 0x00u, binary_dcs[i],
                          &payload, 1u, 1u);
        sms_codec_message_t decoded;
        memset(&decoded, 0, sizeof(decoded));
        assert_true(sms_pdu_decode(hex, &decoded),
                    "opaque DCS variant decodes");
        assert_true(decoded.binary, "8-bit/compressed/reserved DCS stays binary");
        assert_true(decoded.binary_len == 1u && decoded.binary_data[0] == 0x41u,
                    "opaque DCS payload is preserved exactly");
    }

    static const uint8_t gsm7_dcs[] = {0x00u, 0x40u, 0xd0u, 0xf0u};
    for (size_t i = 0u; i < sizeof(gsm7_dcs); i++) {
        build_raw_deliver(hex, sizeof(hex), 0x00u, gsm7_dcs[i],
                          &payload, 1u, 1u);
        sms_codec_message_t decoded;
        memset(&decoded, 0, sizeof(decoded));
        assert_true(sms_pdu_decode(hex, &decoded) && !decoded.binary &&
                        strcmp(decoded.text, "A") == 0,
                    "general/delete/MWI/class GSM7 DCS decodes as text");
    }

    static const uint8_t ucs2_dcs[] = {0x08u, 0x18u, 0x48u, 0xe8u};
    const uint8_t ucs2_a[] = {0x00u, 0x41u};
    for (size_t i = 0u; i < sizeof(ucs2_dcs); i++) {
        build_raw_deliver(hex, sizeof(hex), 0x00u, ucs2_dcs[i],
                          ucs2_a, sizeof(ucs2_a), sizeof(ucs2_a));
        sms_codec_message_t decoded;
        memset(&decoded, 0, sizeof(decoded));
        assert_true(sms_pdu_decode(hex, &decoded) && !decoded.binary &&
                        strcmp(decoded.text, "A") == 0,
                    "general/delete/MWI UCS2 DCS decodes as text");
    }
}

/* Maximum-length 8-bit binary single segment: UDL=255 would exceed pdu[] only if
 * present; build a 200-byte binary payload (fits 192? pdu[]=192). Use 150 to stay
 * within the 192-byte pdu buffer including headers. Verify exact round trip. */
static void test_binary_max_segment(void) {
    uint8_t pdu[192];
    size_t p = 0u;
    pdu[p++] = 0x00u; pdu[p++] = 0x04u; /* DELIVER no UDHI */
    pdu[p++] = 0x04u; pdu[p++] = 0xA1u; pdu[p++] = 0x21u; pdu[p++] = 0x43u;
    pdu[p++] = 0x00u; pdu[p++] = 0x04u; /* DCS 8-bit */
    for (int i = 0; i < 7; i++) pdu[p++] = 0x00u;
    /* TP-UD is capped at 140 octets independently of the decoder's larger PDU
     * scratch buffer. Picture-message segments remain below this after UDH. */
    uint8_t plen = 140u;
    pdu[p++] = plen; /* UDL */
    for (uint8_t i = 0u; i < plen; i++) {
        pdu[p++] = (uint8_t)(i ^ 0x33u);
    }
    char hex[512];
    bytes_to_hex(pdu, p, hex, sizeof(hex));
    sms_codec_message_t decoded;
    memset(&decoded, 0, sizeof(decoded));
    assert_true(sms_pdu_decode(hex, &decoded), "max binary segment decodes");
    assert_true(decoded.binary_len == plen, "max binary segment length");
    for (uint8_t i = 0u; i < plen; i++) {
        if (decoded.binary_data[i] != (uint8_t)(i ^ 0x33u)) {
            assert_true(false, "max binary segment byte mismatch");
            break;
        }
    }
}

/* SMSC field present (non-zero smsc_len) must be skipped correctly. */
static void test_smsc_present(void) {
    /* SMSC: len=07, then 91 + 6 addr bytes. Use "+12345678901" style SMSC; the
     * SMSC content is not surfaced, only correctly skipped. */
    uint8_t pdu[64];
    size_t p = 0u;
    pdu[p++] = 0x07u; /* SMSC length = 7 bytes follow */
    pdu[p++] = 0x91u; /* SMSC TOA */
    for (int i = 0; i < 6; i++) pdu[p++] = 0x21u; /* 6 SMSC addr bytes */
    pdu[p++] = 0x04u; /* first octet DELIVER */
    pdu[p++] = 0x04u; pdu[p++] = 0xA1u; pdu[p++] = 0x21u; pdu[p++] = 0x43u; /* OA 1234 */
    pdu[p++] = 0x00u; /* PID */
    pdu[p++] = 0x00u; /* DCS GSM7 */
    for (int i = 0; i < 7; i++) pdu[p++] = 0x00u;
    const char *text = "AB";
    uint8_t packed[8];
    uint8_t bytes = pack_gsm7(text, packed, sizeof(packed));
    pdu[p++] = (uint8_t)strlen(text);
    memcpy(&pdu[p], packed, bytes);
    p += bytes;
    char hex[160];
    bytes_to_hex(pdu, p, hex, sizeof(hex));
    sms_codec_message_t decoded;
    memset(&decoded, 0, sizeof(decoded));
    assert_true(sms_pdu_decode(hex, &decoded), "PDU with SMSC decodes");
    assert_true(strcmp(decoded.address, "1234") == 0, "SMSC skipped, OA correct");
    assert_true(strcmp(decoded.text, "AB") == 0, "SMSC PDU text correct");
}

int main(void) {
    test_picture_payload_round_trip();
    test_multipart_pdu_picture_decode();
    test_gsm7_text_decode_round_trip();
    test_gsm7_default_and_extension_utf8();
    test_gsm7_max_utf8_expansion();
    test_ucs2_text_decode();
    test_ucs2_max_and_udh_decode();
    test_gsm7_udh_decode();
    test_gsm7_overclaim_udl_rejected();
    test_oracle_gsm7_hellohello();
    test_oracle_address_international();
    test_oracle_address_national();
    test_oracle_address_odd_length();
    test_alphanumeric_originator_addresses();
    test_alphanumeric_originator_length_bounds();
    test_address_star_hash();
    test_address_empty_international();
    test_gsm7_length_sweep();
    test_gsm7_multiple_of_8();
    test_invalid_hex_rejected();
    test_null_guards();
    test_truncated_pdus();
    test_overlength_pdu_rejected();
    test_binary_udh_only_no_payload();
    test_binary_udh_malformed_rejected();
    test_submit_gsm7_relative_vp();
    test_submit_absolute_vp();
    test_submit_telit_empty_destination();
    test_unsupported_mti_rejected();
    test_picture_payload_malformed();
    test_picture_payload_encode_edges();
    test_concat_16bit_ref();
    test_dcs_classification();
    test_binary_max_segment();
    test_smsc_present();
    if (s_failures != 0) {
        fprintf(stderr, "%d failures\n", s_failures);
        return 1;
    }
    printf("sms_picture_codec tests passed\n");
    return 0;
}

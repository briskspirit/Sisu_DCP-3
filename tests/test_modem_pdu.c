/* Host unit tests for the neutral SMS SUBMIT / PDU encoder.
 *
 * Oracle discipline: expected values are hand-computed from 3GPP TS 23.040
 * (address semi-octet BCD, TOA 0x91/0x81, GSM 7-bit septet packing) and from
 * the codec's documented alphabet mapping -- never from observed output. The
 * GSM7 packing is verified with an independent unpacker.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "services/call_types.h"
#include "services/sms_submit_codec.h"

static int s_failures;

static void assert_true(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        s_failures++;
    }
}

/* ----------------------------------------------------------------------------
 * sms_submit_gsm7_code: per-char GSM 7-bit default-alphabet mapping (the subset
 * the firmware supports). Oracle straight from the function's documented intent:
 * A-Z, a-z, 0-9 pass through as their ASCII code; space=0x20, '+'=0x2B, '-'=0x2D,
 * '.'=0x2E; every other byte folds to space (0x20).
 * ------------------------------------------------------------------------- */
static void test_gsm7_code_alphabet(void) {
    for (uint8_t c = 'A'; c <= 'Z'; c++) {
        assert_true(sms_submit_gsm7_code(c) == c, "gsm7_code A-Z passthrough");
    }
    for (uint8_t c = 'a'; c <= 'z'; c++) {
        assert_true(sms_submit_gsm7_code(c) == c, "gsm7_code a-z passthrough");
    }
    for (uint8_t c = '0'; c <= '9'; c++) {
        assert_true(sms_submit_gsm7_code(c) == c, "gsm7_code 0-9 passthrough");
    }
    assert_true(sms_submit_gsm7_code(' ') == 0x20u, "gsm7_code space");
    assert_true(sms_submit_gsm7_code('+') == 0x2bu, "gsm7_code plus");
    assert_true(sms_submit_gsm7_code('-') == 0x2du, "gsm7_code minus");
    assert_true(sms_submit_gsm7_code('.') == 0x2eu, "gsm7_code dot");
    /* Unsupported chars fold to space. */
    assert_true(sms_submit_gsm7_code('@') == 0x20u, "gsm7_code '@' -> space");
    assert_true(sms_submit_gsm7_code('\n') == 0x20u, "gsm7_code newline -> space");
    assert_true(sms_submit_gsm7_code(0x00u) == 0x20u, "gsm7_code NUL -> space");
    assert_true(sms_submit_gsm7_code(0xffu) == 0x20u, "gsm7_code 0xff -> space");
    assert_true(sms_submit_gsm7_code('_') == 0x20u, "gsm7_code '_' -> space");
    /* ':' is between '9' and 'A' -> not supported -> space. */
    assert_true(sms_submit_gsm7_code(':') == 0x20u, "gsm7_code ':' -> space");
}

/* ----------------------------------------------------------------------------
 * Independent GSM7 unpacker: given the packed bytes and the septet count, recover
 * each 7-bit code LSB-first. This is derived from 3GPP TS 23.038 packing, written
 * fresh here (not the firmware's packer) so a round-trip mismatch is a real bug.
 * ------------------------------------------------------------------------- */
static void unpack_gsm7(const uint8_t *packed, uint8_t septets, uint8_t *out_codes) {
    for (uint8_t i = 0u; i < septets; i++) {
        uint16_t bit = (uint16_t)i * 7u;
        uint16_t byte = (uint16_t)(bit / 8u);
        uint8_t shift = (uint8_t)(bit & 7u);
        uint16_t v = (uint16_t)(packed[byte] >> shift);
        if (shift != 0u) {
            v |= (uint16_t)((uint16_t)packed[byte + 1u] << (8u - shift));
        }
        out_codes[i] = (uint8_t)(v & 0x7fu);
    }
}

/* sms_submit_pack_gsm7: sweep septet lengths 1..MODEM_SMS_TEXT_MAX and verify the
 * independent unpacker recovers exactly the codes that gsm7_code would assign,
 * and that out_len == ceil(septets*7/8). */
static void test_gsm7_pack_length_sweep(void) {
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789abcdefghijklmnopqrstuvwxyz "
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789abcdefghijklmnopqrstuvwxyz "
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789abcdefghij";
    assert_true(strlen(alphabet) >= MODEM_SMS_TEXT_MAX, "pack sweep alphabet long enough");

    for (uint8_t len = 1u; len <= MODEM_SMS_TEXT_MAX; len++) {
        uint8_t src[MODEM_SMS_TEXT_MAX];
        memcpy(src, alphabet, len);

        uint8_t dst[MODEM_SMS_TEXT_MAX];      /* >= ceil(160*7/8)=140 */
        memset(dst, 0xa5u, sizeof(dst));      /* poison to catch under-fill */
        uint8_t out_len = 0u;
        bool ok = sms_submit_pack_gsm7(src, len, dst, &out_len);
        if (!ok) {
            char msg[64];
            snprintf(msg, sizeof(msg), "pack len=%u returns true", (unsigned)len);
            assert_true(false, msg);
            continue;
        }
        uint8_t expect_bytes = (uint8_t)(((uint16_t)len * 7u + 7u) / 8u);
        if (out_len != expect_bytes) {
            char msg[64];
            snprintf(msg, sizeof(msg), "pack len=%u out_len=%u expect=%u",
                     (unsigned)len, (unsigned)out_len, (unsigned)expect_bytes);
            assert_true(false, msg);
        }

        uint8_t got[MODEM_SMS_TEXT_MAX];
        unpack_gsm7(dst, len, got);
        for (uint8_t i = 0u; i < len; i++) {
            uint8_t want = sms_submit_gsm7_code(src[i]);
            if (got[i] != want) {
                char msg[80];
                snprintf(msg, sizeof(msg),
                         "pack len=%u septet=%u got=0x%02x want=0x%02x",
                         (unsigned)len, (unsigned)i, got[i], want);
                assert_true(false, msg);
                break;
            }
        }
    }
}

/* Known-good fixed vector: "hellohello" (10 lowercase letters) packs to the
 * classic 3GPP example E8329BFD4697D9EC37 (9 bytes). Hand-verified from the
 * de-facto standard worked example; all chars are a-z so gsm7_code = ASCII. */
static void test_gsm7_pack_known_vector(void) {
    const char *text = "hellohello";
    uint8_t septets = (uint8_t)strlen(text);
    uint8_t dst[16];
    memset(dst, 0, sizeof(dst));
    uint8_t out_len = 0u;
    bool ok = sms_submit_pack_gsm7((const uint8_t *)text, septets, dst, &out_len);
    assert_true(ok, "known vector packs");
    static const uint8_t expect[] = {
        0xE8u, 0x32u, 0x9Bu, 0xFDu, 0x46u, 0x97u, 0xD9u, 0xECu, 0x37u
    };
    assert_true(out_len == sizeof(expect), "known vector length 9");
    assert_true(out_len == sizeof(expect) && memcmp(dst, expect, sizeof(expect)) == 0,
                "known vector bytes match E8329BFD4697D9EC37");
}

/* pack_gsm7 NULL / bounds guards. septets==0 and septets>MODEM_SMS_TEXT_MAX must
 * be rejected. */
static void test_gsm7_pack_guards(void) {
    uint8_t dst[160];
    uint8_t out_len = 0xffu;
    assert_true(!sms_submit_pack_gsm7(0, 1u, dst, &out_len), "pack null src rejected");
    assert_true(!sms_submit_pack_gsm7((const uint8_t *)"A", 1u, 0, &out_len), "pack null dst rejected");
    assert_true(!sms_submit_pack_gsm7((const uint8_t *)"A", 1u, dst, 0), "pack null out_len rejected");
    assert_true(!sms_submit_pack_gsm7((const uint8_t *)"A", 0u, dst, &out_len), "pack zero septets rejected");
    uint8_t src[MODEM_SMS_TEXT_MAX + 1u];
    memset(src, 'A', sizeof(src));
    assert_true(!sms_submit_pack_gsm7(src, (uint8_t)(MODEM_SMS_TEXT_MAX + 1u), dst, &out_len),
                "pack > TEXT_MAX septets rejected");
    /* exactly MODEM_SMS_TEXT_MAX is allowed */
    out_len = 0u;
    assert_true(sms_submit_pack_gsm7(src, (uint8_t)MODEM_SMS_TEXT_MAX, dst, &out_len),
                "pack == TEXT_MAX septets allowed");
}

/* ----------------------------------------------------------------------------
 * sms_submit_append_hex_byte: appends two uppercase hex chars. Bound check is
 * `*pos + 2 >= cap` (note: requires a byte to spare for NUL conceptually, so a
 * 4-char capacity can hold only 3 hex chars worth before refusing). Verify exact
 * digits, advancing pos by 2, and the off-by boundary.
 * ------------------------------------------------------------------------- */
static void test_append_hex_byte_values(void) {
    char buf[16];
    size_t pos = 0u;
    assert_true(sms_submit_append_hex_byte(buf, sizeof(buf), &pos, 0x00u), "hex 0x00 ok");
    assert_true(sms_submit_append_hex_byte(buf, sizeof(buf), &pos, 0xA5u), "hex 0xA5 ok");
    assert_true(sms_submit_append_hex_byte(buf, sizeof(buf), &pos, 0xFFu), "hex 0xFF ok");
    assert_true(pos == 6u, "hex pos advanced by 6");
    buf[pos] = '\0';
    assert_true(strcmp(buf, "00A5FF") == 0, "hex bytes serialized uppercase");
}

static void test_append_hex_byte_bounds(void) {
    /* cap so small that the very first byte is refused: need *pos+2 < cap, i.e.
     * cap must be > 2. cap==2 -> 0+2 >= 2 -> refused. */
    char buf[4];
    size_t pos = 0u;
    assert_true(!sms_submit_append_hex_byte(buf, 2u, &pos, 0x12u), "hex cap=2 refused");
    assert_true(pos == 0u, "hex refusal leaves pos untouched");
    /* cap==3: 0+2 >= 3 false -> writes 2 chars, pos=2. Next: 2+2 >=3 true -> refuse. */
    pos = 0u;
    assert_true(sms_submit_append_hex_byte(buf, 3u, &pos, 0x12u), "hex cap=3 first ok");
    assert_true(pos == 2u, "hex cap=3 pos=2");
    assert_true(!sms_submit_append_hex_byte(buf, 3u, &pos, 0x34u), "hex cap=3 second refused");
    /* NULL guards */
    pos = 0u;
    assert_true(!sms_submit_append_hex_byte(0, 16u, &pos, 0x12u), "hex null dst refused");
    assert_true(!sms_submit_append_hex_byte(buf, 16u, 0, 0x12u), "hex null pos refused");
}

/* ----------------------------------------------------------------------------
 * sms_submit_append_address: 3GPP TS 23.040 9.1.2.5 address field.
 *   [count][TOA][BCD semi-octets, F-padded].
 * TOA = 0x91 if a leading '+' (international), else 0x81 (national, ISDN).
 * Oracle BCD computed by hand below.
 * ------------------------------------------------------------------------- */

/* Decode an address field that append_address wrote, back into count/toa/digits,
 * so we can assert the hand-derived oracle structurally. */
static void check_address(const char *number,
                          uint8_t expect_count,
                          uint8_t expect_toa,
                          const char *expect_digits, /* the digit chars, in order */
                          const char *label) {
    uint8_t dst[64];
    memset(dst, 0xCCu, sizeof(dst));
    size_t pos = 0u;
    bool ok = sms_submit_append_address(dst, sizeof(dst), &pos, number);
    char msg[96];
    if (!ok) {
        snprintf(msg, sizeof(msg), "%s: append_address returned true", label);
        assert_true(false, msg);
        return;
    }
    uint8_t packed = (uint8_t)((expect_count + 1u) / 2u);
    /* field = 2 header bytes + packed BCD bytes */
    if (pos != (size_t)(2u + packed)) {
        snprintf(msg, sizeof(msg), "%s: field length pos=%zu expect=%u",
                 label, pos, (unsigned)(2u + packed));
        assert_true(false, msg);
    }
    if (dst[0] != expect_count) {
        snprintf(msg, sizeof(msg), "%s: count=%u expect=%u", label, dst[0], expect_count);
        assert_true(false, msg);
    }
    if (dst[1] != expect_toa) {
        snprintf(msg, sizeof(msg), "%s: toa=0x%02x expect=0x%02x", label, dst[1], expect_toa);
        assert_true(false, msg);
    }
    /* Reconstruct digit string from the BCD semi-octets. */
    char got[64];
    size_t gi = 0u;
    for (uint8_t b = 0u; b < packed; b++) {
        uint8_t byte = dst[2u + b];
        uint8_t lo = byte & 0x0fu;
        uint8_t hi = (byte >> 4) & 0x0fu;
        got[gi++] = (char)('0' + lo);   /* low nibble first */
        if (hi != 0x0fu) {
            got[gi++] = (char)('0' + hi);
        } else {
            /* F pad: must be the last nibble of an odd-length number */
            char m2[96];
            if (b != (uint8_t)(packed - 1u) || (expect_count & 1u) == 0u) {
                snprintf(m2, sizeof(m2), "%s: stray F pad nibble", label);
                assert_true(false, m2);
            }
        }
    }
    got[gi] = '\0';
    if (strcmp(got, expect_digits) != 0) {
        snprintf(msg, sizeof(msg), "%s: digits '%s' expect '%s'", label, got, expect_digits);
        assert_true(false, msg);
    }
}

static void test_address_international_even(void) {
    /* "+13125551234": 11 digits -> wait, that's 11 (odd). Use even: "+1312555123"
     * has 10 digits. digits: 1 3 1 2 5 5 5 1 2 3.
     * BCD (low,high per byte): 31 21 55 15 32 -> count=10(0x0A), toa=0x91. */
    check_address("+1312555123", 10u, 0x91u, "1312555123", "intl even-10");
}

static void test_address_international_odd(void) {
    /* "+13125551234": 11 digits (odd). last nibble F-padded. toa=0x91. */
    check_address("+13125551234", 11u, 0x91u, "13125551234", "intl odd-11");
}

static void test_address_national(void) {
    /* "1234": 4 digits, no '+'. toa=0x81. BCD: 21 43. */
    check_address("1234", 4u, 0x81u, "1234", "national-4");
}

static void test_address_national_odd(void) {
    /* "555": 3 digits, odd, national. toa=0x81. BCD: 55 F5. */
    check_address("555", 3u, 0x81u, "555", "national-odd-3");
}

static void test_address_single_digit(void) {
    /* "7": 1 digit. toa=0x81. BCD: F7 (low=7, high=F). */
    check_address("7", 1u, 0x81u, "7", "single-digit");
}

static void test_address_embedded_nondigits(void) {
    /* Non-digit separators are skipped; a '+' only counts as international when it
     * is the very first char before any digit. "+1 (555) 123" -> digits 1555123
     * (7 digits, odd), intl. */
    check_address("+1 (555) 123", 7u, 0x91u, "1555123", "intl with separators");
    /* A '+' NOT at the front (count!=0) is ignored and does not set international.
     * "44+5" -> digits "445" national. */
    check_address("44+5", 3u, 0x81u, "445", "mid-string plus ignored");
}

static void test_address_max_digits(void) {
    /* MODEM_PHONE_MAX digits exactly should pack. Build that many '9's. */
    char number[MODEM_PHONE_MAX + 2u];
    for (size_t i = 0u; i < MODEM_PHONE_MAX; i++) {
        number[i] = '9';
    }
    number[MODEM_PHONE_MAX] = '\0';
    char expect[MODEM_PHONE_MAX + 1u];
    memcpy(expect, number, MODEM_PHONE_MAX + 1u);
    check_address(number, (uint8_t)MODEM_PHONE_MAX, 0x81u, expect, "max-digits national");
}

static void test_address_over_max_rejected(void) {
    /* MODEM_PHONE_MAX+1 digits must be rejected (count >= MODEM_PHONE_MAX guard). */
    char number[MODEM_PHONE_MAX + 4u];
    for (size_t i = 0u; i < (size_t)(MODEM_PHONE_MAX + 1u); i++) {
        number[i] = '8';
    }
    number[MODEM_PHONE_MAX + 1u] = '\0';
    uint8_t dst[80];
    size_t pos = 0u;
    assert_true(!sms_submit_append_address(dst, sizeof(dst), &pos, number),
                "over-max-digit address rejected");
    assert_true(pos == 0u, "rejected address leaves pos untouched");
}

static void test_address_empty_rejected(void) {
    uint8_t dst[16];
    size_t pos = 0u;
    assert_true(!sms_submit_append_address(dst, sizeof(dst), &pos, ""), "empty address rejected");
    assert_true(!sms_submit_append_address(dst, sizeof(dst), &pos, "+"), "plus-only address rejected");
    assert_true(!sms_submit_append_address(dst, sizeof(dst), &pos, "abc"), "no-digit address rejected");
    assert_true(pos == 0u, "no-digit rejection leaves pos untouched");
    /* NULL guards */
    assert_true(!sms_submit_append_address(0, sizeof(dst), &pos, "1"), "null dst rejected");
    assert_true(!sms_submit_append_address(dst, sizeof(dst), 0, "1"), "null pos rejected");
    assert_true(!sms_submit_append_address(dst, sizeof(dst), &pos, 0), "null number rejected");
}

/* Capacity boundary: append_address must refuse when the field would overrun.
 * "1234" needs 2 header + 2 BCD = 4 bytes. cap==3 must refuse; cap==4 must fit. */
static void test_address_capacity_boundary(void) {
    uint8_t dst[8];
    size_t pos;
    pos = 0u;
    assert_true(!sms_submit_append_address(dst, 3u, &pos, "1234"), "address cap=3 refused");
    /* The early `*pos + 2 > cap` guard fires first; pos untouched. */
    assert_true(pos == 0u, "address cap=3 leaves pos untouched");
    pos = 0u;
    assert_true(sms_submit_append_address(dst, 4u, &pos, "1234"), "address cap=4 fits");
    assert_true(pos == 4u, "address cap=4 wrote 4 bytes");
    /* Odd case "555": needs 2 + 2 = 4 bytes too. With cap=3 (passes first guard
     * `*pos+2 > cap`? 0+2 > 3 is false, so it proceeds to the packed guard
     * `*pos + 2 + packed > cap` = 0+2+2=4 > 3 true -> refuse). */
    pos = 0u;
    assert_true(!sms_submit_append_address(dst, 3u, &pos, "555"), "odd address cap=3 refused via packed guard");
}

/* ----------------------------------------------------------------------------
 * sms_submit_pdu_build: full SUBMIT TPDU builder. Verify the leading bytes,
 * the address field,
 * the UDH (port / concat IEs), the DCS, UDL, the TP-DU length output, and that
 * the returned hex round-trips to the bytes we expect. We also confirm
 * caller-owned position / segment progress advances.
 * ------------------------------------------------------------------------- */

static sms_submit_pdu_t s_submit;
static char s_submit_number[MODEM_PHONE_MAX + 1u];
static uint8_t s_submit_payload[MODEM_SMS_BINARY_MAX];

/* Parse the returned hex string back to bytes for structural assertions. */
static size_t hex_to_bytes(const char *hex, uint8_t *out, size_t out_cap) {
    size_t n = 0u;
    for (size_t i = 0u; hex[i] != '\0' && hex[i + 1u] != '\0'; i += 2u) {
        if (n >= out_cap) break;
        char hi = hex[i], lo = hex[i + 1u];
        int hv = (hi >= '0' && hi <= '9') ? hi - '0' :
                 (hi >= 'A' && hi <= 'F') ? hi - 'A' + 10 : -1;
        int lv = (lo >= '0' && lo <= '9') ? lo - '0' :
                 (lo >= 'A' && lo <= 'F') ? lo - 'A' + 10 : -1;
        if (hv < 0 || lv < 0) break;
        out[n++] = (uint8_t)((hv << 4) | lv);
    }
    return n;
}

static void reset_binary_state(void) {
    memset(&s_submit, 0, sizeof(s_submit));
    memset(s_submit_number, 0, sizeof(s_submit_number));
    memset(s_submit_payload, 0, sizeof(s_submit_payload));
    s_submit.number = s_submit_number;
    s_submit.payload = s_submit_payload;
    s_submit.segment = 1u;
    s_submit.segment_total = 1u;
    s_submit.reference = 1u;
}

/* Single-segment 8-bit (DCS 0x04) with application ports (port-first mode). */
static void test_build_pdu_8bit_port_single(void) {
    reset_binary_state();
    strcpy(s_submit_number, "1234");           /* national -> TOA 0x81 */
    s_submit.mode = (uint8_t)MODEM_BINARY_SMS_MODE_DCS04_PORT_FIRST;
    s_submit.dest_port = 0x158Au;
    s_submit.source_port = 0x0000u;
    const uint8_t payload[] = {0xDEu, 0xADu, 0xBEu, 0xEFu};
    memcpy(s_submit_payload, payload, sizeof(payload));
    s_submit.payload_len = (uint16_t)sizeof(payload);

    char hex[SMS_SUBMIT_PDU_HEX_MAX];
    uint8_t tpdu_len = 0u;
    bool ok = sms_submit_pdu_build(&s_submit, hex, sizeof(hex), &tpdu_len);
    assert_true(ok, "8bit-port single builds");
    if (!ok) return;

    uint8_t b[180];
    size_t n = hex_to_bytes(hex, b, sizeof(b));

    /* Hand-derived expected TPDU:
     *  00            SMSC = use default
     *  41            SMS-SUBMIT, TP-UDHI set, no VP   (use_udh, no vp)
     *  00            TP-MR
     *  04 81 21 43   DA: 4 digits national, BCD 21 43
     *  00            TP-PID
     *  04            TP-DCS 8-bit, class clear
     *  UDL           = udh_total(7) + chunk(4) = 0x0B
     *  06            UDHL
     *  05 04 15 8A 00 00   port IE: dest 0x158A, src 0x0000
     *  DE AD BE EF   payload
     */
    static const uint8_t want[] = {
        0x00u, 0x41u, 0x00u, 0x04u, 0x81u, 0x21u, 0x43u, 0x00u, 0x04u,
        0x0Bu, 0x06u, 0x05u, 0x04u, 0x15u, 0x8Au, 0x00u, 0x00u,
        0xDEu, 0xADu, 0xBEu, 0xEFu
    };
    assert_true(n == sizeof(want), "8bit-port single byte count");
    assert_true(n == sizeof(want) && memcmp(b, want, n) == 0, "8bit-port single bytes match oracle");
    /* TP-DU length excludes the leading SMSC byte: pos-1. */
    assert_true(tpdu_len == (uint8_t)(sizeof(want) - 1u), "8bit-port single tpdu_len = bytes-1");
    /* state advanced past the consumed chunk */
    assert_true(s_submit.position == 4u, "8bit-port single pos advanced");
    assert_true(s_submit.segment == 2u, "8bit-port single segment incremented");
}

/* No ports, no concat: plain 8-bit single segment has NO UDH (first octet 0x01). */
static void test_build_pdu_8bit_no_udh(void) {
    reset_binary_state();
    strcpy(s_submit_number, "+15551234");      /* international -> TOA 0x91 */
    s_submit.mode = (uint8_t)MODEM_BINARY_SMS_MODE_DCS04_PORT_FIRST;
    s_submit.dest_port = 0u;
    s_submit.source_port = 0u;
    const uint8_t payload[] = {0x01u, 0x02u, 0x03u};
    memcpy(s_submit_payload, payload, sizeof(payload));
    s_submit.payload_len = 3u;

    char hex[SMS_SUBMIT_PDU_HEX_MAX];
    uint8_t tpdu_len = 0u;
    assert_true(sms_submit_pdu_build(&s_submit, hex, sizeof(hex), &tpdu_len), "8bit no-UDH builds");

    uint8_t b[64];
    size_t n = hex_to_bytes(hex, b, sizeof(b));
    /* +15551234 = 8 digits -> count 0x08 TOA 0x91, BCD 51 55 21 43.
     *  00 01 00 08 91 51 55 21 43 00 04 03 01 02 03   (UDL=3, no UDH) */
    static const uint8_t want[] = {
        0x00u, 0x01u, 0x00u, 0x08u, 0x91u, 0x51u, 0x55u, 0x21u, 0x43u,
        0x00u, 0x04u, 0x03u, 0x01u, 0x02u, 0x03u
    };
    assert_true(n == sizeof(want) && memcmp(b, want, n) == 0, "8bit no-UDH bytes match oracle");
    assert_true(tpdu_len == (uint8_t)(sizeof(want) - 1u), "8bit no-UDH tpdu_len");
}

/* F5 (DCS 0xF5) mode flag selects DCS 0xF5. */
static void test_build_pdu_f5_dcs(void) {
    reset_binary_state();
    strcpy(s_submit_number, "1234");
    s_submit.mode = (uint8_t)MODEM_BINARY_SMS_MODE_F5_PORT_FIRST;
    s_submit.dest_port = 0x158Au;
    s_submit.source_port = 0u;
    s_submit_payload[0] = 0xAAu;
    s_submit.payload_len = 1u;

    char hex[SMS_SUBMIT_PDU_HEX_MAX];
    uint8_t tpdu_len = 0u;
    assert_true(sms_submit_pdu_build(&s_submit, hex, sizeof(hex), &tpdu_len), "f5 builds");
    uint8_t b[64];
    hex_to_bytes(hex, b, sizeof(b));
    /* DCS byte is at index: 00,41,00, [04 81 21 43], 00(PID), DCS@idx8 */
    assert_true(b[8] == 0xF5u, "f5 mode -> DCS 0xF5");
}

/* Concat-first VP mode (F5 + VP): first octet has VP bit set (0x51), DCS 0xF5,
 * VP byte 0xA7 present, and when multi-segment the concat IE precedes payload. */
static void test_build_pdu_f5_concat_vp_multipart(void) {
    reset_binary_state();
    strcpy(s_submit_number, "1234");
    s_submit.mode = (uint8_t)MODEM_BINARY_SMS_MODE_F5_CONCAT_FIRST_VP;
    s_submit.dest_port = 0x158Au;     /* ports present too */
    s_submit.source_port = 0u;
    /* 200 bytes -> 2 segments (chunk max 128). */
    for (uint16_t i = 0u; i < 200u; i++) {
        s_submit_payload[i] = (uint8_t)i;
    }
    s_submit.payload_len = 200u;
    s_submit.segment_total = 2u;
    s_submit.segment = 1u;
    s_submit.reference = 0x77u;

    char hex[SMS_SUBMIT_PDU_HEX_MAX];
    uint8_t tpdu_len = 0u;
    assert_true(sms_submit_pdu_build(&s_submit, hex, sizeof(hex), &tpdu_len), "f5 concat-vp seg1 builds");
    uint8_t b[180];
    size_t n = hex_to_bytes(hex, b, sizeof(b));

    /* Layout for multipart + ports + VP, concat-first (port NOT first):
     *  00                SMSC
     *  51                SUBMIT + UDHI + VPF-relative bit (0x40|0x10|0x01)
     *  00                MR
     *  04 81 21 43       DA
     *  00                PID
     *  F5                DCS
     *  A7                VP (relative)
     *  UDL               = udh_total(0x0B+1=0x0C) + chunk(128) = 0x8C
     *  0B                UDHL (=0x0b)
     *  00 03 ref tot seq concat IE (concat-first ordering)
     *  05 04 dh dl sh sl port IE
     *  <128 payload>
     */
    assert_true(b[1] == 0x51u, "concat-vp first octet 0x51 (UDHI+VP)");
    assert_true(b[8] == 0xF5u, "concat-vp DCS 0xF5");
    assert_true(b[9] == 0xA7u, "concat-vp VP byte 0xA7");
    /* UDL at index 10 */
    uint8_t udh_total = (uint8_t)(0x0Bu + 1u);
    assert_true(b[10] == (uint8_t)(udh_total + 128u), "concat-vp UDL = udh_total + 128");
    assert_true(b[11] == 0x0Bu, "concat-vp UDHL 0x0B");
    /* concat IE first (concat-first mode): 00 03 ref total seq */
    assert_true(b[12] == 0x00u && b[13] == 0x03u, "concat IE header 00 03");
    assert_true(b[14] == 0x77u, "concat ref");
    assert_true(b[15] == 0x02u, "concat total");
    assert_true(b[16] == 0x01u, "concat seq");
    /* then port IE: 05 04 15 8A 00 00 */
    assert_true(b[17] == 0x05u && b[18] == 0x04u, "port IE header 05 04");
    assert_true(b[19] == 0x15u && b[20] == 0x8Au, "port IE dest 0x158A");
    assert_true(b[21] == 0x00u && b[22] == 0x00u, "port IE src 0x0000");
    /* payload begins at 23, first byte = 0x00 (binary[0]) */
    assert_true(n >= 24u && b[23] == 0x00u, "concat-vp first payload byte");
    assert_true(s_submit.position == 128u, "concat-vp seg1 consumed 128 bytes");

    /* Build segment 2: remaining 72 bytes. */
    s_submit.segment = 2u;
    uint8_t tpdu2 = 0u;
    assert_true(sms_submit_pdu_build(&s_submit, hex, sizeof(hex), &tpdu2), "f5 concat-vp seg2 builds");
    size_t n2 = hex_to_bytes(hex, b, sizeof(b));
    /* UDL = udh_total + 72 */
    assert_true(b[10] == (uint8_t)(udh_total + 72u), "concat-vp seg2 UDL = udh_total + 72");
    assert_true(b[16] == 0x02u, "concat-vp seg2 seq=2");
    /* first payload byte of seg2 = binary[128] = (uint8_t)128 = 0x80 */
    assert_true(b[23] == 0x80u, "concat-vp seg2 first payload byte");
    assert_true(s_submit.position == 200u, "concat-vp seg2 consumes to end");
    (void)n2;
}

/* Port-first multipart (DCS04_PORT_FIRST): UDH carries the port IE BEFORE the
 * concat IE, UDHL = 0x0B (port 6 + concat 5). First octet 0x41 (UDHI, no VP). */
static void test_build_pdu_8bit_port_first_multipart(void) {
    reset_binary_state();
    strcpy(s_submit_number, "1234");
    s_submit.mode = (uint8_t)MODEM_BINARY_SMS_MODE_DCS04_PORT_FIRST;
    s_submit.dest_port = 0x158Au;
    s_submit.source_port = 0x0000u;
    for (uint16_t i = 0u; i < 200u; i++) {
        s_submit_payload[i] = (uint8_t)(0xFFu - i);
    }
    s_submit.payload_len = 200u;
    s_submit.segment_total = 2u;
    s_submit.segment = 1u;
    s_submit.reference = 0x55u;

    char hex[SMS_SUBMIT_PDU_HEX_MAX];
    uint8_t tpdu_len = 0u;
    assert_true(sms_submit_pdu_build(&s_submit, hex, sizeof(hex), &tpdu_len), "port-first multipart builds");
    uint8_t b[180];
    hex_to_bytes(hex, b, sizeof(b));
    /*  00 41 00 04 81 21 43 00 04 UDL UDHL=0B [port IE 6] [concat IE 5] payload */
    assert_true(b[1] == 0x41u, "port-first multipart first octet 0x41 (UDHI no VP)");
    assert_true(b[8] == 0x04u, "port-first multipart DCS 0x04");
    assert_true(b[9] == (uint8_t)(0x0Cu + 128u), "port-first multipart UDL = 12+128");
    assert_true(b[10] == 0x0Bu, "port-first multipart UDHL 0x0B");
    /* port IE first */
    assert_true(b[11] == 0x05u && b[12] == 0x04u, "port IE header first");
    assert_true(b[13] == 0x15u && b[14] == 0x8Au, "port IE dest");
    assert_true(b[15] == 0x00u && b[16] == 0x00u, "port IE src");
    /* then concat IE */
    assert_true(b[17] == 0x00u && b[18] == 0x03u, "concat IE header after port");
    assert_true(b[19] == 0x55u && b[20] == 0x02u && b[21] == 0x01u, "concat ref/total/seq");
    /* payload begins at 22, first byte binary[0] = 0xFF */
    assert_true(b[22] == 0xFFu, "port-first multipart first payload byte");
}

/* GSM7 text mode: DCS 0x00, no UDH (gsm7 disables port/concat), payload is the
 * packed septets, UDL is the septet count (not byte count). */
static void test_build_pdu_gsm7_text(void) {
    reset_binary_state();
    strcpy(s_submit_number, "1234");
    s_submit.mode = (uint8_t)MODEM_BINARY_SMS_MODE_GSM7_TEXT;
    /* Even though ports are set, gsm7 mode must ignore them (use_port gated on
     * !gsm7_text). */
    s_submit.dest_port = 0x158Au;
    s_submit.source_port = 0u;
    const char *text = "hellohello";
    memcpy(s_submit_payload, text, strlen(text));
    s_submit.payload_len = (uint16_t)strlen(text);

    char hex[SMS_SUBMIT_PDU_HEX_MAX];
    uint8_t tpdu_len = 0u;
    assert_true(sms_submit_pdu_build(&s_submit, hex, sizeof(hex), &tpdu_len), "gsm7 text builds");
    uint8_t b[64];
    size_t n = hex_to_bytes(hex, b, sizeof(b));
    /*  00 01 00 04 81 21 43 00 00 0A E8329BFD4697D9EC37
     *  first octet 0x01 (no UDH, no VP), DCS 0x00, UDL=10 septets, packed = the
     *  known vector. */
    static const uint8_t want[] = {
        0x00u, 0x01u, 0x00u, 0x04u, 0x81u, 0x21u, 0x43u, 0x00u, 0x00u, 0x0Au,
        0xE8u, 0x32u, 0x9Bu, 0xFDu, 0x46u, 0x97u, 0xD9u, 0xECu, 0x37u
    };
    assert_true(n == sizeof(want) && memcmp(b, want, n) == 0, "gsm7 text bytes match oracle");
    assert_true(tpdu_len == (uint8_t)(sizeof(want) - 1u), "gsm7 text tpdu_len");
}

/* Build guards: NULL args, zero cap, empty binary, pos at/over end. */
static void test_build_pdu_guards(void) {
    reset_binary_state();
    strcpy(s_submit_number, "1234");
    s_submit_payload[0] = 0x11u;
    s_submit.payload_len = 1u;
    char hex[SMS_SUBMIT_PDU_HEX_MAX];
    uint8_t tpdu_len = 0u;
    assert_true(!sms_submit_pdu_build(NULL, hex, sizeof(hex), &tpdu_len),
                "build null transaction rejected");
    assert_true(!sms_submit_pdu_build(&s_submit, 0, sizeof(hex), &tpdu_len), "build null hex rejected");
    assert_true(!sms_submit_pdu_build(&s_submit, hex, 0u, &tpdu_len), "build zero cap rejected");
    assert_true(!sms_submit_pdu_build(&s_submit, hex, sizeof(hex), 0), "build null out rejected");

    reset_binary_state();
    strcpy(s_submit_number, "1234");
    s_submit.payload_len = 0u;       /* nothing to send */
    assert_true(!sms_submit_pdu_build(&s_submit, hex, sizeof(hex), &tpdu_len), "build empty binary rejected");

    reset_binary_state();
    strcpy(s_submit_number, "1234");
    s_submit_payload[0] = 0x11u;
    s_submit.payload_len = 1u;
    s_submit.position = 1u;                    /* already past end */
    assert_true(!sms_submit_pdu_build(&s_submit, hex, sizeof(hex), &tpdu_len), "build pos>=len rejected");

    /* Bad address inside build -> propagates false. */
    reset_binary_state();
    strcpy(s_submit_number, "noDigits");
    s_submit_payload[0] = 0x11u;
    s_submit.payload_len = 1u;
    assert_true(!sms_submit_pdu_build(&s_submit, hex, sizeof(hex), &tpdu_len), "build bad address rejected");
}

/* Tight hex capacity: a buffer too small for the full hex string must be rejected
 * (append_hex_byte fails mid-way) without overrunning -- ASan/UBSan watch. */
static void test_build_pdu_tight_hex_cap(void) {
    reset_binary_state();
    strcpy(s_submit_number, "1234");
    s_submit.mode = (uint8_t)MODEM_BINARY_SMS_MODE_DCS04_PORT_FIRST;
    s_submit.dest_port = 0x158Au;
    const uint8_t payload[] = {0xDEu, 0xADu, 0xBEu, 0xEFu};
    memcpy(s_submit_payload, payload, sizeof(payload));
    s_submit.payload_len = 4u;
    /* The full PDU is 21 bytes -> 42 hex chars + NUL. Give only 10 chars. */
    char hex[10];
    uint8_t tpdu_len = 0u;
    uint16_t position_before = s_submit.position;
    uint8_t segment_before = s_submit.segment;
    assert_true(!sms_submit_pdu_build(&s_submit, hex, sizeof(hex), &tpdu_len),
                "tight hex cap rejected without overrun");
    assert_true(s_submit.position == position_before &&
                    s_submit.segment == segment_before,
                "failed build does not consume caller progress");
}

/* GSM7 with one F5/text edge: a single-septet message packs to 1 byte, UDL=1. */
static void test_build_pdu_gsm7_single_char(void) {
    reset_binary_state();
    strcpy(s_submit_number, "1");
    s_submit.mode = (uint8_t)MODEM_BINARY_SMS_MODE_GSM7_TEXT;
    s_submit_payload[0] = (uint8_t)'A';
    s_submit.payload_len = 1u;
    char hex[SMS_SUBMIT_PDU_HEX_MAX];
    uint8_t tpdu_len = 0u;
    assert_true(sms_submit_pdu_build(&s_submit, hex, sizeof(hex), &tpdu_len), "gsm7 single builds");
    uint8_t b[32];
    size_t n = hex_to_bytes(hex, b, sizeof(b));
    /* 00 01 00 01 81 F1 00 00 01 41   ('1' addr -> count 1 toa 81 BCD F1; 'A'=0x41) */
    static const uint8_t want[] = {
        0x00u, 0x01u, 0x00u, 0x01u, 0x81u, 0xF1u, 0x00u, 0x00u, 0x01u, 0x41u
    };
    assert_true(n == sizeof(want) && memcmp(b, want, n) == 0, "gsm7 single bytes match oracle");
}

/* ----------------------------------------------------------------------------
 * sms_submit_format_text_command: build "AT+CMGS"/"AT+CMGW" command strings.
 *   empty number -> bare command (no '=').
 *   leading '+'  -> ...="<num>",145 [,"STO SENT"]
 *   else         -> ...="<num>"     [,"...",129,"STO SENT"]
 * ------------------------------------------------------------------------- */
static void test_address_command_variants(void) {
    char buf[96];

    sms_submit_format_text_command(buf, sizeof(buf), "AT+CMGS", "+15551234", false);
    assert_true(strcmp(buf, "AT+CMGS=\"+15551234\",145") == 0, "intl CMGS no-status");

    sms_submit_format_text_command(buf, sizeof(buf), "AT+CMGS", "5551234", false);
    assert_true(strcmp(buf, "AT+CMGS=\"5551234\"") == 0, "national CMGS no-status");

    sms_submit_format_text_command(buf, sizeof(buf), "AT+CMGW", "+15551234", true);
    assert_true(strcmp(buf, "AT+CMGW=\"+15551234\",145,\"STO SENT\"") == 0, "intl CMGW status");

    sms_submit_format_text_command(buf, sizeof(buf), "AT+CMGW", "5551234", true);
    assert_true(strcmp(buf, "AT+CMGW=\"5551234\",129,\"STO SENT\"") == 0, "national CMGW status");

    /* empty number -> bare command */
    sms_submit_format_text_command(buf, sizeof(buf), "AT+CMGS", "", false);
    assert_true(strcmp(buf, "AT+CMGS") == 0, "empty number -> bare command");

    /* NULL address treated as empty -> bare command */
    sms_submit_format_text_command(buf, sizeof(buf), "AT+CMGS", 0, false);
    assert_true(strcmp(buf, "AT+CMGS") == 0, "null address -> bare command");

    /* NULL/zero dst guards: must not crash. */
    sms_submit_format_text_command(0, sizeof(buf), "AT+CMGS", "1", false);
    sms_submit_format_text_command(buf, 0u, "AT+CMGS", "1", false);

    /* Tight dst buffer: snprintf truncates safely, always NUL-terminated. */
    char small[8];
    sms_submit_format_text_command(small, sizeof(small), "AT+CMGS", "+15551234", false);
    assert_true(small[sizeof(small) - 1u] == '\0', "tight buffer NUL-terminated");
    assert_true(strlen(small) < sizeof(small), "tight buffer not overrun");
}

int main(void) {
    test_gsm7_code_alphabet();
    test_gsm7_pack_length_sweep();
    test_gsm7_pack_known_vector();
    test_gsm7_pack_guards();

    test_append_hex_byte_values();
    test_append_hex_byte_bounds();

    test_address_international_even();
    test_address_international_odd();
    test_address_national();
    test_address_national_odd();
    test_address_single_digit();
    test_address_embedded_nondigits();
    test_address_max_digits();
    test_address_over_max_rejected();
    test_address_empty_rejected();
    test_address_capacity_boundary();

    test_build_pdu_8bit_port_single();
    test_build_pdu_8bit_no_udh();
    test_build_pdu_f5_dcs();
    test_build_pdu_f5_concat_vp_multipart();
    test_build_pdu_8bit_port_first_multipart();
    test_build_pdu_gsm7_text();
    test_build_pdu_guards();
    test_build_pdu_tight_hex_cap();
    test_build_pdu_gsm7_single_char();

    test_address_command_variants();

    if (s_failures != 0) {
        fprintf(stderr, "%d failures\n", s_failures);
        return 1;
    }
    printf("modem_pdu tests passed\n");
    return 0;
}

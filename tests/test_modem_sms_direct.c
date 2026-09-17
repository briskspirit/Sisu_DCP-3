#include <stdio.h>
#include <string.h>

#include "services/modem_sms_direct.h"
#include "services/sms_picture_codec.h"

static int s_failures;
static void check(bool ok, const char *msg) {
    if (!ok) { printf("FAIL: %s\n", msg); s_failures++; }
}

/* Payloads are byte ranges, never C strings: a plain body may contain 0x00. */
#define BYTES(s) (const uint8_t *)(s), strlen(s)

static modem_sms_direct_step_t feed_raw_cstr(const char *bytes,
                                             modem_sms_direct_translate_fn vendor,
                                             char *hex, size_t cap, uint8_t *tpdu);

static const char PDU_3GPP[] =
    "07919130079229F0040B918131926378F70000629061019569000744F57CAE56CF01";

static void test_pdu_form_passthrough(void) {
    char hex[SMS_DELIVER_HEX_MAX];
    uint8_t tpdu = 0u;
    check(modem_sms_direct_parse_3gpp_pdu("+CMT: ,26", BYTES(PDU_3GPP), hex, sizeof(hex), &tpdu) &&
              tpdu == 26u && strcmp(hex, PDU_3GPP) == 0,
          "standard +CMT PDU form is stored verbatim");
    check(modem_sms_direct_parse_3gpp_pdu("+CMT: \"\",26", BYTES(PDU_3GPP), hex, sizeof(hex), &tpdu),
          "quoted empty alpha accepted");
    check(!modem_sms_direct_parse_3gpp_pdu("+CMT: ,25", BYTES(PDU_3GPP), hex, sizeof(hex), &tpdu),
          "length must match the TPDU length");
    check(!modem_sms_direct_parse_3gpp_pdu("+CMT: ,26", BYTES("ZZ"), hex, sizeof(hex), &tpdu),
          "non-hex payload rejected");
    check(!modem_sms_direct_parse_3gpp_pdu("+CMT: \"7866910488\",\"\",25", BYTES(PDU_3GPP),
                                           hex, sizeof(hex), &tpdu),
          "three-field header is not the 3GPP PDU form");
    check(!modem_sms_direct_parse_3gpp_pdu("+CMT: ,26", (const uint8_t *)PDU_3GPP,
                                           strlen(PDU_3GPP) - 2u, hex, sizeof(hex), &tpdu),
          "payload length is explicit, not NUL-terminated");
}

static void test_text_form_gsm7(void) {
    sms_deliver_t d;
    check(modem_sms_direct_parse_3gpp_text(
              "+CMT: \"+18132936877\",,\"26/09/16,10:59:04-16\",145,4,0,0,\"+19037029920\",145,7",
              BYTES("Djssjjs"), &d),
          "3GPP text form parses");
    check(strcmp(d.address, "+18132936877") == 0 && d.toa == 0x91u, "address and TOA");
    check(d.scts[6] == 0x69u && d.scts[0] == 0x62u, "scts keeps the -16 zone");
    check(d.dcs == 0x00u && !d.udhi && d.udl == 7u && d.ud_len == 7u && d.ud[0] == 0x44u,
          "GSM text repacked into septets");
    check(!modem_sms_direct_parse_3gpp_text(
              "+CMT: \"7866910488\",\"\",\"20260916105524\",129,4098,0,8,9", BYTES("Dhdjdjdjs"), &d),
          "eight-field 3GPP2 header is not the 3GPP text form");
    check(!modem_sms_direct_parse_3gpp_text(
              "+CMT: \"+18132936877\",,\"26/09/16,10:59:04-16\",145,4,0,0,\"+19037029920\",145,7",
              BYTES("Djssjj"), &d),
          "plain body must be exactly <length> bytes");
    static const uint8_t high[7] = {'D', 'j', 0x80u, 's', 'j', 'j', 's'};
    check(!modem_sms_direct_parse_3gpp_text(
              "+CMT: \"+18132936877\",,\"26/09/16,10:59:04-16\",145,4,0,0,\"+19037029920\",145,7",
              high, sizeof(high), &d),
          "plain body bytes >= 0x80 are rejected");
    check(modem_sms_direct_parse_3gpp_text(
              "+CMT: \"AMAZON\",,\"26/09/16,10:59:04-16\",208,4,0,0,\"+19037029920\",145,7",
              BYTES("Djssjjs"), &d) && strcmp(d.address, "AMAZON") == 0 && d.toa == 0xD0u,
          "alphanumeric <oa> passes through with its <tooa>");
    char hex[SMS_DELIVER_HEX_MAX];
    uint8_t tpdu = 0u;
    sms_codec_message_t decoded;
    check(sms_deliver_build(&d, hex, sizeof(hex), &tpdu) && sms_pdu_decode(hex, &decoded) &&
              strcmp(decoded.address, "AMAZON") == 0 && strcmp(decoded.text, "Djssjjs") == 0,
          "an A2P message from an alphanumeric sender is rebuilt and decodes");
}

static void test_text_form_udh_and_ucs2(void) {
    sms_deliver_t d;
    check(modem_sms_direct_parse_3gpp_text(
              "+CMT: \"7866910488\",,\"26/09/16,10:55:53+00\",129,68,0,0,\"+19037029920\",145,23",
              BYTES("050003620202D46435599D9EABE7EAB99AAC26ABC9"), &d),
          "UDHI text form parses");
    check(d.udhi && d.udl == 23u && d.ud_len == 21u && d.ud[0] == 0x05u,
          "UDH hex body copied, UDL in septets");
    check(modem_sms_direct_parse_3gpp_text(
              "+CMT: \"7866910488\",,\"26/09/16,11:27:49+00\",129,4,0,8,\"+19037029920\",145,4",
              BYTES("D83DDE1C"), &d),
          "UCS2 text form parses");
    check(d.dcs == 0x08u && d.udl == 4u && d.ud_len == 4u && d.ud[0] == 0xD8u,
          "UCS2 hex body copied");

    /* <length> off by one on a GSM-7 UDH body (bench, WEMT part): the octet
     * count decides UDL, chosen from {L, L+1, L-1}. */
    char hex[SMS_DELIVER_HEX_MAX];
    uint8_t tpdu = 0u;
    sms_codec_message_t decoded;
    check(modem_sms_direct_parse_3gpp_text(
              "+CMT: \"7866910488\",,\"26/09/16,15:07:55+00\",129,68,0,0,\"+19037029920\",145,21",
              BYTES("050003FB0202D4E4349A8C26ABC96AB29A8C2603"), &d) &&
              d.udhi && d.udl == 22u && d.ud_len == 20u,
          "UDH text form with <length> off by one takes UDL from the octet count");
    modem_sms_direct_reset();
    check(modem_sms_direct_feed(
              "+CMT: \"7866910488\",,\"26/09/16,15:07:55+00\",129,68,0,0,\"+19037029920\",145,21",
              NULL, hex, sizeof(hex), &tpdu) == MODEM_SMS_DIRECT_STEP_HEADER &&
              feed_raw_cstr("050003FB0202D4E4349A8C26ABC96AB29A8C2603\r\n", NULL, hex,
                            sizeof(hex), &tpdu) == MODEM_SMS_DIRECT_STEP_READY &&
              sms_pdu_decode(hex, &decoded) && strcmp(decoded.text, "jdihdhdjdjdjdhd") == 0 &&
              decoded.has_concat && decoded.concat_ref == 0xFBu &&
              decoded.concat_total == 2u && decoded.concat_seq == 2u,
          "the off-by-one UDH body is READY and round-trips with its concat fields");
    check(!modem_sms_direct_parse_3gpp_text(
              "+CMT: \"7866910488\",,\"26/09/16,15:07:55+00\",129,68,0,0,\"+19037029920\",145,21",
              BYTES("050003FB0202D4E4349A8C26ABC96AB29A8C260300"), &d),
          "a GSM-7 hex body off by two octets is rejected");
    check(!modem_sms_direct_parse_3gpp_text(
              "+CMT: \"7866910488\",,\"26/09/16,11:27:49+00\",129,4,0,8,\"+19037029920\",145,5",
              BYTES("D83DDE1C"), &d),
          "UCS2 stays exact: a one-off <length> is rejected");
}

static uint8_t s_fake_last_payload[16];
static size_t s_fake_last_len;
static unsigned s_fake_calls;
static size_t s_fake_min_len; /* INCOMPLETE below this length (a longer body may still be valid) */
static size_t s_fake_packed_len; /* when set: mimic enc 2, <length> = packed octets */

static modem_sms_direct_translate_result_t fake_vendor(
    const char *header, const uint8_t *payload, size_t payload_len, sms_deliver_t *out) {
    if (strncmp(header, "+CMT: \"V\"", 9u) != 0) {
        return MODEM_SMS_DIRECT_NOT_MINE;
    }
    s_fake_calls++;
    s_fake_last_len = payload_len;
    memcpy(s_fake_last_payload, payload,
           payload_len < sizeof(s_fake_last_payload) ? payload_len : sizeof(s_fake_last_payload));
    if (payload_len == 3u && memcmp(payload, "bad", 3u) == 0) {
        return MODEM_SMS_DIRECT_REJECTED;
    }
    if (s_fake_packed_len != 0u) {
        size_t packed = (payload_len * 7u + 7u) / 8u;
        if (packed < s_fake_packed_len) {
            return MODEM_SMS_DIRECT_INCOMPLETE;
        }
        if (packed > s_fake_packed_len) {
            return MODEM_SMS_DIRECT_REJECTED;
        }
    } else if (payload_len < s_fake_min_len) {
        return MODEM_SMS_DIRECT_INCOMPLETE;
    }
    memset(out, 0, sizeof(*out));
    strcpy(out->address, "123");
    out->toa = 0x81u;
    out->dcs = 0x04u;
    out->ud[0] = 0x41u; out->ud_len = 1u; out->udl = 1u;
    return MODEM_SMS_DIRECT_ACCEPTED;
}

/* Feed wire bytes one at a time; returns the first non-IGNORED step (or
 * IGNORED when the whole run was accumulated). */
static modem_sms_direct_step_t feed_raw_bytes(const uint8_t *bytes, size_t len,
                                              modem_sms_direct_translate_fn vendor,
                                              char *hex, size_t cap, uint8_t *tpdu) {
    for (size_t i = 0u; i < len; i++) {
        modem_sms_direct_step_t step = modem_sms_direct_feed_raw(bytes[i], vendor, hex, cap, tpdu);
        if (step != MODEM_SMS_DIRECT_STEP_IGNORED) {
            return step;
        }
    }
    return MODEM_SMS_DIRECT_STEP_IGNORED;
}

static modem_sms_direct_step_t feed_raw_cstr(const char *bytes,
                                             modem_sms_direct_translate_fn vendor,
                                             char *hex, size_t cap, uint8_t *tpdu) {
    return feed_raw_bytes((const uint8_t *)bytes, strlen(bytes), vendor, hex, cap, tpdu);
}

static const char TEXT_HEADER_FMT[] =
    "+CMT: \"+18132936877\",,\"26/09/16,10:59:04-16\",145,4,0,0,\"+19037029920\",145,%u";

static bool raw_text_roundtrip(unsigned length, const uint8_t *body, size_t body_len,
                               const char *expected_text, const char *msg) {
    char header[160];
    char hex[SMS_DELIVER_HEX_MAX];
    uint8_t tpdu = 0u;
    sms_codec_message_t decoded;
    snprintf(header, sizeof(header), TEXT_HEADER_FMT, length);
    modem_sms_direct_reset();
    bool ok = modem_sms_direct_feed(header, fake_vendor, hex, sizeof(hex), &tpdu) ==
                  MODEM_SMS_DIRECT_STEP_HEADER &&
              modem_sms_direct_raw_active() && modem_sms_direct_pending();
    ok = ok && feed_raw_bytes(body, body_len, fake_vendor, hex, sizeof(hex), &tpdu) ==
                   MODEM_SMS_DIRECT_STEP_IGNORED;
    ok = ok && modem_sms_direct_feed_raw('\r', fake_vendor, hex, sizeof(hex), &tpdu) ==
                   MODEM_SMS_DIRECT_STEP_IGNORED;
    ok = ok && modem_sms_direct_feed_raw('\n', fake_vendor, hex, sizeof(hex), &tpdu) ==
                   MODEM_SMS_DIRECT_STEP_READY;
    ok = ok && !modem_sms_direct_raw_active() && !modem_sms_direct_pending();
    ok = ok && sms_pdu_decode(hex, &decoded) && strcmp(decoded.text, expected_text) == 0;
    check(ok, msg);
    return ok;
}

static void test_raw_reader(void) {
    char hex[SMS_DELIVER_HEX_MAX];
    uint8_t tpdu = 0u;
    sms_codec_message_t decoded;

    static const char multi[] = "Your code is 123456\r\nDo not share";
    raw_text_roundtrip(33u, (const uint8_t *)multi, sizeof(multi) - 1u, multi,
                       "a plain body with an embedded CRLF is read raw by <length>");
    raw_text_roundtrip(6u, (const uint8_t *)"Hello ", 6u, "Hello ",
                       "a trailing space survives");
    static const uint8_t at[3] = {'a', 0x00u, 'b'};
    raw_text_roundtrip(3u, at, sizeof(at), "a@b", "GSM 0x00 (@) is data, not a terminator");
    static const uint8_t bracket[3] = {'a', 0x1Bu, 0x3Cu};
    raw_text_roundtrip(3u, bracket, sizeof(bracket), "a[", "ESC-sequence bytes count 1:1");
    raw_text_roundtrip(2u, (const uint8_t *)"\r\n", 2u, "\r\n",
                       "a body that is only CRLF is data when <length> says so");

    /* Bare LF terminator. */
    char header[160];
    snprintf(header, sizeof(header), TEXT_HEADER_FMT, 2u);
    modem_sms_direct_reset();
    check(modem_sms_direct_feed(header, fake_vendor, hex, sizeof(hex), &tpdu) ==
                  MODEM_SMS_DIRECT_STEP_HEADER &&
              feed_raw_cstr("ab\n", fake_vendor, hex, sizeof(hex), &tpdu) ==
                  MODEM_SMS_DIRECT_STEP_READY &&
              sms_pdu_decode(hex, &decoded) && strcmp(decoded.text, "ab") == 0,
          "a bare LF at <length> terminates the body");

    /* A CR at/after <length> that is not followed by LF is data. The
     * candidate "ab\rc" has the wrong length and is finally rejected. */
    modem_sms_direct_feed(header, fake_vendor, hex, sizeof(hex), &tpdu);
    check(feed_raw_cstr("ab\rc\r\n", fake_vendor, hex, sizeof(hex), &tpdu) ==
                  MODEM_SMS_DIRECT_STEP_REJECTED &&
              !modem_sms_direct_raw_active(),
          "a CR not followed by LF stays in the payload (length mismatch rejects)");

    /* Hex bodies are longer than <length> and terminate at their own end. */
    modem_sms_direct_reset();
    check(modem_sms_direct_feed(
              "+CMT: \"7866910488\",,\"26/09/16,10:55:53+00\",129,68,0,0,\"+19037029920\",145,23",
              fake_vendor, hex, sizeof(hex), &tpdu) == MODEM_SMS_DIRECT_STEP_HEADER &&
              feed_raw_cstr("050003620202D46435599D9EABE7EAB99AAC26ABC9\r\n", fake_vendor,
                            hex, sizeof(hex), &tpdu) == MODEM_SMS_DIRECT_STEP_READY &&
              sms_pdu_decode(hex, &decoded) && decoded.has_concat && decoded.concat_seq == 2u,
          "a GSM-7+UDH hex body completes at its CRLF");
    check(modem_sms_direct_feed(
              "+CMT: \"7866910488\",,\"26/09/16,11:27:49+00\",129,4,0,8,\"+19037029920\",145,4",
              fake_vendor, hex, sizeof(hex), &tpdu) == MODEM_SMS_DIRECT_STEP_HEADER &&
              feed_raw_cstr("D83DDE1C\r\n", fake_vendor, hex, sizeof(hex), &tpdu) ==
                  MODEM_SMS_DIRECT_STEP_READY,
          "a UCS2 hex body completes at its CRLF");
    check(modem_sms_direct_feed("+CMT: ,26", fake_vendor, hex, sizeof(hex), &tpdu) ==
                  MODEM_SMS_DIRECT_STEP_HEADER && modem_sms_direct_raw_active() &&
              feed_raw_cstr(PDU_3GPP, fake_vendor, hex, sizeof(hex), &tpdu) ==
                  MODEM_SMS_DIRECT_STEP_IGNORED &&
              feed_raw_cstr("\r\n", fake_vendor, hex, sizeof(hex), &tpdu) ==
                  MODEM_SMS_DIRECT_STEP_READY && tpdu == 26u && strcmp(hex, PDU_3GPP) == 0,
          "the 3GPP PDU form is read raw and stored verbatim");

    /* Vendor hook receives the exact byte range. */
    s_fake_calls = 0u;
    check(modem_sms_direct_feed("+CMT: \"V\",3", fake_vendor, hex, sizeof(hex), &tpdu) ==
                  MODEM_SMS_DIRECT_STEP_HEADER &&
              feed_raw_bytes(at, sizeof(at), fake_vendor, hex, sizeof(hex), &tpdu) ==
                  MODEM_SMS_DIRECT_STEP_IGNORED &&
              feed_raw_cstr("\r\n", fake_vendor, hex, sizeof(hex), &tpdu) ==
                  MODEM_SMS_DIRECT_STEP_READY &&
              s_fake_calls == 1u && s_fake_last_len == 3u && s_fake_last_payload[1] == 0x00u,
          "the vendor hook gets the payload pointer and explicit length");
    check(modem_sms_direct_feed("+CMT: \"V\",3", fake_vendor, hex, sizeof(hex), &tpdu) ==
                  MODEM_SMS_DIRECT_STEP_HEADER &&
              feed_raw_cstr("bad\r\n", fake_vendor, hex, sizeof(hex), &tpdu) ==
                  MODEM_SMS_DIRECT_STEP_REJECTED && !modem_sms_direct_pending(),
          "a final vendor rejection ends the body at its line break");

    /* Cap without a terminator: REJECTED, RAW mode ends. */
    snprintf(header, sizeof(header), TEXT_HEADER_FMT, 5u);
    check(modem_sms_direct_feed(header, fake_vendor, hex, sizeof(hex), &tpdu) ==
              MODEM_SMS_DIRECT_STEP_HEADER, "cap fixture header");
    bool accumulated = true;
    for (unsigned i = 0u; i < MODEM_SMS_DIRECT_LINE_MAX; i++) {
        accumulated = accumulated &&
            modem_sms_direct_feed_raw('A', fake_vendor, hex, sizeof(hex), &tpdu) ==
                MODEM_SMS_DIRECT_STEP_IGNORED;
    }
    check(accumulated && modem_sms_direct_raw_active(),
          "bytes accumulate up to the cap");
    check(modem_sms_direct_feed_raw('A', fake_vendor, hex, sizeof(hex), &tpdu) ==
                  MODEM_SMS_DIRECT_STEP_REJECTED &&
              !modem_sms_direct_raw_active() && !modem_sms_direct_pending(),
          "one byte past the cap without a terminator rejects and leaves RAW mode");
    check(modem_sms_direct_feed("OK", fake_vendor, hex, sizeof(hex), &tpdu) ==
              MODEM_SMS_DIRECT_STEP_IGNORED, "after a cap rejection lines flow normally");

    /* Bytes >= 0x80 in a plain body are rejected (the framer would map them
     * to '?'; the raw reader keeps them so the parser can see them). */
    snprintf(header, sizeof(header), TEXT_HEADER_FMT, 2u);
    static const uint8_t high[2] = {'a', 0xE9u};
    check(modem_sms_direct_feed(header, fake_vendor, hex, sizeof(hex), &tpdu) ==
                  MODEM_SMS_DIRECT_STEP_HEADER &&
              feed_raw_bytes(high, 2u, fake_vendor, hex, sizeof(hex), &tpdu) ==
                  MODEM_SMS_DIRECT_STEP_IGNORED &&
              feed_raw_cstr("\r\n", fake_vendor, hex, sizeof(hex), &tpdu) ==
                  MODEM_SMS_DIRECT_STEP_REJECTED && !modem_sms_direct_raw_active(),
          "a plain body with a byte >= 0x80 is rejected at its line break");

    /* reset() leaves RAW mode. */
    check(modem_sms_direct_feed(header, fake_vendor, hex, sizeof(hex), &tpdu) ==
              MODEM_SMS_DIRECT_STEP_HEADER && modem_sms_direct_raw_active(), "reset fixture");
    modem_sms_direct_reset();
    check(!modem_sms_direct_raw_active() && !modem_sms_direct_pending() &&
              modem_sms_direct_feed_raw('x', fake_vendor, hex, sizeof(hex), &tpdu) ==
                  MODEM_SMS_DIRECT_STEP_IGNORED && !modem_sms_direct_raw_active(),
          "reset leaves RAW mode and a stray raw byte is ignored");
}

/* Only an explicitly incomplete prefix can continue past an invalid
 * candidate; a final rejection must leave subsequent lines alone. */
static void test_raw_speculative_termination(void) {
    char hex[SMS_DELIVER_HEX_MAX];
    uint8_t tpdu = 0u;
    sms_codec_message_t decoded;

    /* The vendor needs 16 bytes: the LF at byte 14 is body data. */
    modem_sms_direct_reset();
    s_fake_min_len = 16u;
    s_fake_calls = 0u;
    check(modem_sms_direct_feed("+CMT: \"V\",14", fake_vendor, hex, sizeof(hex), &tpdu) ==
                  MODEM_SMS_DIRECT_STEP_HEADER &&
              feed_raw_cstr("AAAAAAAAAAAAAA\nB", fake_vendor, hex, sizeof(hex), &tpdu) ==
                  MODEM_SMS_DIRECT_STEP_IGNORED &&
              modem_sms_direct_raw_active() && s_fake_calls == 1u,
          "an incomplete candidate at the first line break keeps accumulating");
    check(feed_raw_cstr("\r\n", fake_vendor, hex, sizeof(hex), &tpdu) ==
                  MODEM_SMS_DIRECT_STEP_READY &&
              s_fake_last_len == 16u && s_fake_last_payload[14] == '\n' &&
              s_fake_last_payload[15] == 'B' && !modem_sms_direct_raw_active(),
          "the body completes at the first line break whose candidate parses");
    s_fake_min_len = 0u;

    /* (e) an exact-length plain body: a LF before <length> is data and the
     * body terminates at <length> as before. */
    modem_sms_direct_reset();
    check(modem_sms_direct_feed(
              "+CMT: \"+18132936877\",,\"26/09/16,10:59:04-16\",145,4,0,0,\"+19037029920\",145,5",
              NULL, hex, sizeof(hex), &tpdu) == MODEM_SMS_DIRECT_STEP_HEADER &&
              feed_raw_cstr("ab\ncd\r\n", NULL, hex, sizeof(hex), &tpdu) ==
                  MODEM_SMS_DIRECT_STEP_READY &&
              sms_pdu_decode(hex, &decoded) && strcmp(decoded.text, "ab\ncd") == 0,
          "an exact-length plain body with an embedded LF is unchanged");

    /* Garbage enc-2-like body, L 14, LFs at 14 and 17: 14 packs to 13 < 14
     * (INCOMPLETE, continue); 17 packs to 15 > 14 (REJECTED): the body ends
     * at that line break, terminator consumed, the next line intact. */
    modem_sms_direct_reset();
    s_fake_packed_len = 14u;
    check(modem_sms_direct_feed("+CMT: \"V\",14", fake_vendor, hex, sizeof(hex), &tpdu) ==
                  MODEM_SMS_DIRECT_STEP_HEADER &&
              feed_raw_cstr("XXXXXXXXXXXXXX\nXX", fake_vendor, hex, sizeof(hex), &tpdu) ==
                  MODEM_SMS_DIRECT_STEP_IGNORED && modem_sms_direct_raw_active(),
          "a too-short candidate continues the body");
    check(feed_raw_cstr("\n", fake_vendor, hex, sizeof(hex), &tpdu) ==
                  MODEM_SMS_DIRECT_STEP_REJECTED && !modem_sms_direct_raw_active() &&
              !modem_sms_direct_raw_take_unconsumed() && s_fake_last_len == 17u &&
              modem_sms_direct_feed("OK", fake_vendor, hex, sizeof(hex), &tpdu) ==
                  MODEM_SMS_DIRECT_STEP_IGNORED && !modem_sms_direct_pending(),
          "a final rejection at a later line break ends the body there, next line intact");
    s_fake_packed_len = 0u;

    /* Bound: a body the vendor keeps calling too short cannot exceed
     * ceil(8*L/7) + 1 bytes (L 14 -> 17); the byte that trips the bound is
     * not part of the body and is handed back to the framer. */
    modem_sms_direct_reset();
    s_fake_min_len = 1000u;
    check(modem_sms_direct_feed("+CMT: \"V\",14", fake_vendor, hex, sizeof(hex), &tpdu) ==
                  MODEM_SMS_DIRECT_STEP_HEADER &&
              feed_raw_cstr("XXXXXXXXXXXXXX\nXX", fake_vendor, hex, sizeof(hex), &tpdu) ==
                  MODEM_SMS_DIRECT_STEP_IGNORED && modem_sms_direct_raw_active(),
          "after a too-short candidate the body may grow to the plain-body bound");
    check(modem_sms_direct_feed_raw('Y', fake_vendor, hex, sizeof(hex), &tpdu) ==
                  MODEM_SMS_DIRECT_STEP_REJECTED && !modem_sms_direct_raw_active() &&
              modem_sms_direct_raw_take_unconsumed() && !modem_sms_direct_raw_take_unconsumed(),
          "the byte past the bound ends the body and is handed back once");
    s_fake_min_len = 0u;

    /* A generic-parser rejection at p == L is final: the body ends there. */
    modem_sms_direct_reset();
    static const uint8_t bad_plain[5] = {'a', 'b', 0xE9u, 'c', 'd'};
    check(modem_sms_direct_feed(
              "+CMT: \"+18132936877\",,\"26/09/16,10:59:04-16\",145,4,0,0,\"+19037029920\",145,5",
              NULL, hex, sizeof(hex), &tpdu) == MODEM_SMS_DIRECT_STEP_HEADER &&
              feed_raw_bytes(bad_plain, 5u, NULL, hex, sizeof(hex), &tpdu) ==
                  MODEM_SMS_DIRECT_STEP_IGNORED &&
              feed_raw_cstr("\r\n", NULL, hex, sizeof(hex), &tpdu) ==
                  MODEM_SMS_DIRECT_STEP_REJECTED && !modem_sms_direct_raw_active() &&
              !modem_sms_direct_raw_take_unconsumed() &&
              modem_sms_direct_feed("OK", NULL, hex, sizeof(hex), &tpdu) ==
                  MODEM_SMS_DIRECT_STEP_IGNORED,
          "a generic-parser rejection at <length> terminates at that line break");

    /* A long hex body (no CR/LF) terminates at its own CRLF, well past the
     * plain-body bound, because that is its first candidate. */
    modem_sms_direct_reset();
    char ucs2[4u * 70u + 1u];
    for (size_t i = 0u; i < 70u; i++) {
        memcpy(&ucs2[i * 4u], "0041", 4u);
    }
    ucs2[4u * 70u] = '\0';
    check(modem_sms_direct_feed(
              "+CMT: \"7866910488\",,\"26/09/16,11:27:49+00\",129,4,0,8,\"+19037029920\",145,140",
              NULL, hex, sizeof(hex), &tpdu) == MODEM_SMS_DIRECT_STEP_HEADER &&
              feed_raw_cstr(ucs2, NULL, hex, sizeof(hex), &tpdu) == MODEM_SMS_DIRECT_STEP_IGNORED &&
              feed_raw_cstr("\r\n", NULL, hex, sizeof(hex), &tpdu) == MODEM_SMS_DIRECT_STEP_READY &&
              sms_pdu_decode(hex, &decoded) && strlen(decoded.text) == 70u,
          "a long UCS2 hex body still terminates at its own CRLF");

    /* (f) a body the vendor never finds long enough: rejected at the
     * plain-body bound (L 5 -> 7) on the byte past it; RAW mode over. */
    modem_sms_direct_reset();
    s_fake_min_len = 1000u;
    check(modem_sms_direct_feed("+CMT: \"V\",5", fake_vendor, hex, sizeof(hex), &tpdu) ==
              MODEM_SMS_DIRECT_STEP_HEADER, "never-valid fixture header");
    modem_sms_direct_step_t last = MODEM_SMS_DIRECT_STEP_IGNORED;
    unsigned fed = 0u;
    for (unsigned i = 0u; i <= MODEM_SMS_DIRECT_LINE_MAX; i++) {
        last = modem_sms_direct_feed_raw((uint8_t)(i % 7u == 6u ? '\n' : 'A'), fake_vendor,
                                         hex, sizeof(hex), &tpdu);
        fed++;
        if (last != MODEM_SMS_DIRECT_STEP_IGNORED) {
            break;
        }
    }
    check(last == MODEM_SMS_DIRECT_STEP_REJECTED && fed == 8u && !modem_sms_direct_raw_active() &&
              modem_sms_direct_raw_take_unconsumed() &&
              modem_sms_direct_feed("OK", fake_vendor, hex, sizeof(hex), &tpdu) ==
                  MODEM_SMS_DIRECT_STEP_IGNORED && !modem_sms_direct_pending(),
          "a body that never validates is rejected at the bound and lines flow again");
    s_fake_min_len = 0u;
}

static void test_collector(void) {
    char hex[SMS_DELIVER_HEX_MAX];
    uint8_t tpdu = 0u;
    modem_sms_direct_reset();
    check(modem_sms_direct_feed("+CEREG: 1,1", fake_vendor, hex, sizeof(hex), &tpdu) ==
              MODEM_SMS_DIRECT_STEP_IGNORED && !modem_sms_direct_pending(),
          "unrelated URC ignored");
    check(modem_sms_direct_feed("+CMT: ,26", fake_vendor, hex, sizeof(hex), &tpdu) ==
              MODEM_SMS_DIRECT_STEP_HEADER && modem_sms_direct_pending() &&
              modem_sms_direct_raw_active(),
          "a header with <length> > 0 enters RAW mode");
    check(modem_sms_direct_feed(PDU_3GPP, fake_vendor, hex, sizeof(hex), &tpdu) ==
              MODEM_SMS_DIRECT_STEP_IGNORED && modem_sms_direct_raw_active(),
          "line feed is ignored while RAW mode owns the bytes");
    modem_sms_direct_reset();

    /* A header whose last field is not a length (e.g. +CSDH=0) keeps the
     * two-line behaviour: the next line is the payload. */
    static const char NO_LENGTH[] = "+CMT: \"+1555\",,\"26/09/16,10:59:04-16\"";
    check(modem_sms_direct_feed(NO_LENGTH, fake_vendor, hex, sizeof(hex), &tpdu) ==
              MODEM_SMS_DIRECT_STEP_HEADER && modem_sms_direct_pending() &&
              !modem_sms_direct_raw_active(),
          "a header without a length field pends in line mode");
    check(modem_sms_direct_feed("Hello", fake_vendor, hex, sizeof(hex), &tpdu) ==
              MODEM_SMS_DIRECT_STEP_REJECTED && !modem_sms_direct_pending(),
          "the next line is consumed as the payload (no parser accepts it)");
    check(modem_sms_direct_feed(NO_LENGTH, NULL, hex, sizeof(hex), &tpdu) ==
              MODEM_SMS_DIRECT_STEP_HEADER &&
              modem_sms_direct_feed(NO_LENGTH, NULL, hex, sizeof(hex), &tpdu) ==
              MODEM_SMS_DIRECT_STEP_HEADER,
          "a header while pending replaces the stale header (payload lost, not misparsed)");
    modem_sms_direct_reset();
    check(!modem_sms_direct_pending(), "reset clears pending");
}

static void test_zero_length_body(void) {
    static const char TEXT_EMPTY[] =
        "+CMT: \"+18132936877\",,\"26/09/16,10:59:04-16\",145,4,0,0,\"+19037029920\",145,0";
    char hex[SMS_DELIVER_HEX_MAX];
    uint8_t tpdu = 0u;
    sms_codec_message_t decoded;
    sms_deliver_t d;

    /* The framer never delivers the empty payload line, so the header alone
     * must complete the delivery instead of eating the next line. */
    modem_sms_direct_reset();
    check(modem_sms_direct_feed(TEXT_EMPTY, fake_vendor, hex, sizeof(hex), &tpdu) ==
              MODEM_SMS_DIRECT_STEP_READY && !modem_sms_direct_pending() &&
              !modem_sms_direct_raw_active(),
          "3GPP text header with <length> 0 completes on the header line alone");
    check(sms_pdu_decode(hex, &decoded) && !decoded.submit &&
              decoded.text[0] == '\0' &&
              strcmp(decoded.address, "+18132936877") == 0,
          "empty-body PDU decodes with empty text");
    check(modem_sms_direct_feed("OK", fake_vendor, hex, sizeof(hex), &tpdu) ==
              MODEM_SMS_DIRECT_STEP_IGNORED,
          "the next line is not consumed as a payload");

    s_fake_calls = 0u;
    s_fake_last_len = 99u;
    check(modem_sms_direct_feed("+CMT: \"V\",0", fake_vendor, hex, sizeof(hex), &tpdu) ==
              MODEM_SMS_DIRECT_STEP_READY && !modem_sms_direct_pending() &&
              s_fake_calls == 1u && s_fake_last_len == 0u,
          "a vendor header with a trailing 0 is translated at once with an empty payload");

    check(modem_sms_direct_parse_3gpp_text(
              "+CMT: \"+18132936877\",,\"26/09/16,10:59:04-16\",145,4,0,8,\"+19037029920\",145,0",
              BYTES(""), &d) && d.dcs == 0x08u && d.udl == 0u && d.ud_len == 0u,
          "UCS2 text form accepts an empty body");
    check(!modem_sms_direct_parse_3gpp_text(
              "+CMT: \"+18132936877\",,\"26/09/16,10:59:04-16\",145,4,0,8,\"+19037029920\",145,0",
              BYTES("0041"), &d),
          "UCS2 text form rejects a body that contradicts <length> 0");
    check(modem_sms_direct_feed("+CMT: ,0", NULL, hex, sizeof(hex), &tpdu) ==
              MODEM_SMS_DIRECT_STEP_REJECTED && !modem_sms_direct_pending(),
          "PDU form with <length> 0 is rejected without pending");
    check(modem_sms_direct_feed("+CMT: \"+1555\",,\"26/09/16,10:59:04-16\"", NULL,
                                hex, sizeof(hex), &tpdu) == MODEM_SMS_DIRECT_STEP_HEADER &&
              modem_sms_direct_pending(),
          "a header whose last field is not a length still pends");
    modem_sms_direct_reset();
}

static void test_hex_helper(void) {
    uint8_t buf[4];
    size_t len = 0u;
    check(modem_sms_direct_hex_to_bytes(BYTES("d83dDE1C"), buf, sizeof(buf), &len) && len == 4u &&
              buf[0] == 0xD8u && buf[3] == 0x1Cu, "mixed-case hex decodes");
    check(!modem_sms_direct_hex_to_bytes(BYTES("D83"), buf, sizeof(buf), &len), "odd length rejected");
    check(!modem_sms_direct_hex_to_bytes(BYTES("D83DDE1C00"), buf, sizeof(buf), &len), "capacity enforced");
    check(!modem_sms_direct_hex_to_bytes(BYTES(""), buf, sizeof(buf), &len), "empty rejected");
    static const uint8_t embedded[4] = {'D', '8', 0x00u, '3'};
    check(!modem_sms_direct_hex_to_bytes(embedded, 4u, buf, sizeof(buf), &len),
          "an embedded NUL is a non-hex byte, not a terminator");
}

int main(void) {
    test_pdu_form_passthrough();
    test_text_form_gsm7();
    test_text_form_udh_and_ucs2();
    test_raw_reader();
    test_raw_speculative_termination();
    test_collector();
    test_zero_length_body();
    test_hex_helper();
    if (s_failures != 0) { printf("%d failures\n", s_failures); return 1; }
    printf("test_modem_sms_direct: all passed\n");
    return 0;
}

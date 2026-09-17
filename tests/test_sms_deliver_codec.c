#include <stdio.h>
#include <string.h>

#include "services/sms_deliver_codec.h"
#include "services/sms_picture_codec.h"

static int s_failures;
static void check(bool ok, const char *msg) {
    if (!ok) { printf("FAIL: %s\n", msg); s_failures++; }
}

static void test_scts_encode(void) {
    uint8_t scts[7];
    check(sms_deliver_scts_encode(2026u, 9u, 16u, 10u, 59u, 28u, 0, scts),
          "scts encodes");
    static const uint8_t expected[7] = {0x62, 0x90, 0x61, 0x01, 0x95, 0x82, 0x00};
    check(memcmp(scts, expected, 7u) == 0, "scts semi-octets 26/09/16 10:59:28 tz 0");
    check(sms_deliver_scts_encode(2026u, 9u, 16u, 10u, 59u, 4u, -16, scts),
          "scts encodes negative zone");
    check(scts[6] == 0x69, "tz -16 quarters -> 0x69 (sign in tens nibble)");
    check(!sms_deliver_scts_encode(2026u, 13u, 16u, 10u, 59u, 4u, 0, scts),
          "month 13 rejected");
}

static void test_pack_gsm7(void) {
    uint8_t packed[8];
    uint8_t len = 0u;
    check(sms_deliver_pack_gsm7((const uint8_t *)"Djssjjs", 7u, packed,
                                sizeof(packed), &len) && len == 7u,
          "packs 7 septets into 7 octets");
    static const uint8_t expected[7] = {0x44, 0xF5, 0x7C, 0xAE, 0x56, 0xCF, 0x01};
    check(memcmp(packed, expected, 7u) == 0, "packed bytes match the modem sample");
    check(!sms_deliver_pack_gsm7((const uint8_t *)"Djssjjs", 7u, packed, 6u, &len),
          "capacity is enforced");
}

static void test_latin1_map(void) {
    uint8_t out[2];
    check(sms_deliver_gsm7_from_latin1('A', out) == 1u && out[0] == 'A', "ASCII letter identity");
    check(sms_deliver_gsm7_from_latin1('@', out) == 1u && out[0] == 0x00u, "@ -> 0x00");
    check(sms_deliver_gsm7_from_latin1(0xE9u, out) == 1u && out[0] == 0x05u, "e-acute -> 0x05");
    check(sms_deliver_gsm7_from_latin1('[', out) == 2u && out[0] == 0x1Bu && out[1] == 0x3Cu,
          "[ -> ESC 0x3C");
    check(sms_deliver_gsm7_from_latin1(0xB5u, out) == 0u, "micro sign has no GSM mapping");
}

static void test_build_roundtrip_gsm7(void) {
    sms_deliver_t d;
    memset(&d, 0, sizeof(d));
    strcpy(d.address, "7866910488");
    d.toa = 0x81u;
    (void)sms_deliver_scts_encode(2026u, 9u, 16u, 10u, 59u, 28u, 0, d.scts);
    d.dcs = 0x00u;
    d.udl = 7u;
    (void)sms_deliver_pack_gsm7((const uint8_t *)"Djssjjs", 7u, d.ud, sizeof(d.ud), &d.ud_len);
    char hex[SMS_DELIVER_HEX_MAX];
    uint8_t tpdu_len = 0u;
    check(sms_deliver_build(&d, hex, sizeof(hex), &tpdu_len), "build ok");
    check(strcmp(hex, "00040A8187661940880000629061019582000744F57CAE56CF01") == 0,
          "hex matches the fixed vector");
    check(tpdu_len == 25u, "tpdu length excludes the SMSC byte");
    sms_codec_message_t decoded;
    check(sms_pdu_decode(hex, &decoded) && !decoded.submit &&
              strcmp(decoded.address, "7866910488") == 0 &&
              strcmp(decoded.text, "Djssjjs") == 0 &&
              strcmp(decoded.timestamp, "26/09/16,10:59:28") == 0,
          "phone decoder reads the built DELIVER back");
}

static void test_build_ucs2_and_udh(void) {
    sms_deliver_t d;
    memset(&d, 0, sizeof(d));
    strcpy(d.address, "+18132936877");
    d.toa = 0x91u;
    (void)sms_deliver_scts_encode(2026u, 9u, 16u, 11u, 27u, 49u, 0, d.scts);
    d.dcs = 0x08u;
    static const uint8_t emoji[4] = {0xD8, 0x3D, 0xDE, 0x1C};
    memcpy(d.ud, emoji, 4u); d.ud_len = 4u; d.udl = 4u;
    char hex[SMS_DELIVER_HEX_MAX];
    uint8_t tpdu_len = 0u;
    check(sms_deliver_build(&d, hex, sizeof(hex), &tpdu_len), "ucs2 build ok");
    check(strcmp(hex, "00040B918131926378F700086290611172940004D83DDE1C") == 0,
          "ucs2 hex matches the fixed vector");
    /* The phone decoder treats DCS 0x08 as UCS2 (not UTF-16): a surrogate
     * pair renders as two '?' placeholders, so only the address and the
     * per-code-unit shape are asserted here. */
    sms_codec_message_t decoded;
    check(sms_pdu_decode(hex, &decoded) && strcmp(decoded.address, "+18132936877") == 0 &&
              strcmp(decoded.text, "??") == 0,
          "emoji surrogates reach the decoder as two UCS2 code units");

    /* UDH concat part 2 of 2, ref 0x62, GSM-7 body "252ut9u9u952525" packed after the header */
    memset(&d, 0, sizeof(d));
    strcpy(d.address, "7866910488"); d.toa = 0x81u;
    (void)sms_deliver_scts_encode(2026u, 9u, 16u, 10u, 55u, 53u, 0, d.scts);
    d.dcs = 0x00u; d.udhi = true; d.udl = 23u;
    static const uint8_t ud[21] = {0x05,0x00,0x03,0x62,0x02,0x02,0xD4,0x64,0x35,0x59,0x9D,
                                   0x9E,0xAB,0xE7,0xEA,0xB9,0x9A,0xAC,0x26,0xAB,0xC9};
    memcpy(d.ud, ud, sizeof(ud)); d.ud_len = sizeof(ud);
    check(sms_deliver_build(&d, hex, sizeof(hex), &tpdu_len), "udh build ok");
    check(sms_pdu_decode(hex, &decoded) && decoded.has_concat &&
              decoded.concat_ref == 0x62u && decoded.concat_total == 2u &&
              decoded.concat_seq == 2u,
          "UDH concat fields survive the rebuild");
    check(hex[2] == '4' && hex[3] == '4', "first octet has UDHI (0x44)");
}

/* Build with the given address/TOA and read the address back through the
 * phone decoder. toa_byte_out receives the TOA octet actually written. */
static bool address_roundtrip(const char *address, uint8_t toa, char *decoded_address,
                              size_t cap, uint8_t *toa_byte_out) {
    sms_deliver_t d;
    memset(&d, 0, sizeof(d));
    strcpy(d.address, address);
    d.toa = toa;
    (void)sms_deliver_scts_encode(2026u, 9u, 16u, 10u, 59u, 28u, 0, d.scts);
    d.udl = 1u; d.ud_len = 1u; d.ud[0] = 0x41u;
    char hex[SMS_DELIVER_HEX_MAX];
    uint8_t tpdu_len = 0u;
    sms_codec_message_t decoded;
    if (!sms_deliver_build(&d, hex, sizeof(hex), &tpdu_len) ||
        !sms_pdu_decode(hex, &decoded)) {
        return false;
    }
    /* "00" SMSC, "04" first octet, then Address-Length and TOA. */
    unsigned toa_byte = 0u;
    if (sscanf(&hex[6], "%2x", &toa_byte) != 1) {
        return false;
    }
    *toa_byte_out = (uint8_t)toa_byte;
    strncpy(decoded_address, decoded.address, cap - 1u);
    decoded_address[cap - 1u] = '\0';
    return true;
}

static void test_alphanumeric_and_bcd_addresses(void) {
    char address[SMS_CODEC_ADDRESS_MAX + 1u];
    uint8_t toa = 0u;
    check(address_roundtrip("AMAZON", 0xD0u, address, sizeof(address), &toa) &&
              strcmp(address, "AMAZON") == 0 && toa == 0xD0u,
          "alphanumeric sender (TON 5) round-trips as packed GSM-7");
    check(address_roundtrip("+1800*#", 0x91u, address, sizeof(address), &toa) &&
              strcmp(address, "+1800*#") == 0 && toa == 0x91u,
          "* and # are BCD digits 0xA/0xB in an international number");
    check(address_roundtrip("1800*#", 0x81u, address, sizeof(address), &toa) &&
              strcmp(address, "1800*#") == 0 && toa == 0x81u,
          "* and # round-trip in a national number");
    /* A space is not a BCD digit: with no TOA given the address is
     * alphanumeric text, exactly as typed. */
    check(address_roundtrip("+1 800*#", 0u, address, sizeof(address), &toa) &&
              strcmp(address, "+1 800*#") == 0 && toa == 0xD0u,
          "a non-BCD address with no TOA derives TON 5 and round-trips verbatim");
    check(address_roundtrip("+18132936877", 0u, address, sizeof(address), &toa) &&
              strcmp(address, "+18132936877") == 0 && toa == 0x91u,
          "a + number with no TOA still derives international");
    check(address_roundtrip("12345678901", 0xD0u, address, sizeof(address), &toa) &&
              strcmp(address, "12345678901") == 0 && toa == 0xD0u,
          "11 alphanumeric septets (the 23.040 maximum) round-trip");
    check(!address_roundtrip("123456789012", 0xD0u, address, sizeof(address), &toa),
          "12 alphanumeric septets exceed the 10-octet address value");
    check(!address_roundtrip("AMAZON", 0x81u, address, sizeof(address), &toa),
          "a non-digit address with a numeric TOA is rejected");
    sms_deliver_t d;
    memset(&d, 0, sizeof(d));
    strcpy(d.address, "AM");
    d.address[2] = (char)0xE9;
    d.address[3] = '\0';
    d.toa = 0xD0u;
    char hex[SMS_DELIVER_HEX_MAX];
    uint8_t tpdu_len = 0u;
    check(!sms_deliver_build(&d, hex, sizeof(hex), &tpdu_len),
          "an alphanumeric address byte >= 0x80 is rejected");
}

static void test_build_rejects(void) {
    sms_deliver_t d;
    memset(&d, 0, sizeof(d));
    char hex[SMS_DELIVER_HEX_MAX];
    uint8_t tpdu_len = 0u;
    check(!sms_deliver_build(&d, hex, sizeof(hex), &tpdu_len), "empty address rejected");
    strcpy(d.address, "12ab");
    sms_codec_message_t decoded;
    check(sms_deliver_build(&d, hex, sizeof(hex), &tpdu_len) &&
              sms_pdu_decode(hex, &decoded) && strcmp(decoded.address, "12ab") == 0,
          "a non-digit address with no TOA builds as alphanumeric");
    d.toa = 0x81u;
    check(!sms_deliver_build(&d, hex, sizeof(hex), &tpdu_len),
          "a non-digit address with a numeric TOA is rejected");
    d.toa = 0u;
    strcpy(d.address, "123");
    d.ud_len = 141u;
    check(!sms_deliver_build(&d, hex, sizeof(hex), &tpdu_len), "oversize UD rejected");
    d.ud_len = 0u;
    check(sms_deliver_build(&d, hex, sizeof(hex), &tpdu_len), "empty UD still builds");
    check(!sms_deliver_build(&d, hex, 8u, &tpdu_len), "small hex buffer rejected");
}

int main(void) {
    test_scts_encode();
    test_pack_gsm7();
    test_latin1_map();
    test_build_roundtrip_gsm7();
    test_build_ucs2_and_udh();
    test_alphanumeric_and_bcd_addresses();
    test_build_rejects();
    if (s_failures != 0) { printf("%d failures\n", s_failures); return 1; }
    printf("test_sms_deliver_codec: all passed\n");
    return 0;
}

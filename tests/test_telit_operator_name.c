/* Host-only tests; link with modem_vendor_telit_operator.c, no modem needed. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../src/services/modem_vendor_telit_internal.h"

static unsigned s_failures;

static void check(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        s_failures++;
    }
}

static void expect_name(const char *line, const char *expected,
                         const char *message) {
    char out[TELIT_SIM_PROVIDER_NAME_UTF8_CAP] = "stale";
    check(telit_parse_sim_provider_name(line, out, sizeof(out)) &&
              strcmp(out, expected) == 0, message);
}

static void expect_invalid(const char *line, const char *message) {
    char out[TELIT_SIM_PROVIDER_NAME_UTF8_CAP] = "stale";
    check(!telit_parse_sim_provider_name(line, out, sizeof(out)) &&
              out[0] == '\0', message);
}

static void alpha_response(const uint8_t *alpha, size_t length,
                            unsigned sw1, unsigned sw2, bool quoted,
                            char line[96]) {
    uint8_t data[17];
    memset(data, 0xff, sizeof(data));
    data[0] = 0x03u;
    check(length <= 16u, "fixture fits EFSPN alpha field");
    if (length > 16u) {
        length = 16u;
    }
    if (length != 0u) {
        memcpy(data + 1u, alpha, length);
    }
    size_t used = (size_t)snprintf(line, 96u, "+CRSM: %u,%u,%s",
                                  sw1, sw2, quoted ? "\"" : "");
    for (size_t i = 0u; i < sizeof(data); i++) {
        used += (size_t)snprintf(line + used, 96u - used, "%02X", data[i]);
    }
    (void)snprintf(line + used, 96u - used, "%s", quoted ? "\"" : "");
}

static void expect_alpha(const uint8_t *alpha, size_t length,
                          const char *expected, const char *message) {
    char line[96];
    alpha_response(alpha, length, 144u, 0u, true, line);
    expect_name(line, expected, message);
    alpha_response(alpha, length, 144u, 0u, false, line);
    expect_name(line, expected, message);
}

static void reject_alpha(const uint8_t *alpha, size_t length,
                          const char *message) {
    char line[96];
    alpha_response(alpha, length, 144u, 0u, true, line);
    expect_invalid(line, message);
}

#define ALPHA(s) (const uint8_t *)(s), sizeof(s) - 1u

static void test_grammar_and_status(void) {
    expect_name("+CRSM: 144,0,0343617272696572FFFFFFFFFFFFFFFFFF", "Carrier",
                "documented unquoted response");
    expect_name("+CRSM:\t144 , 0 , \"0343617272696572ffffffffffffffffff\" \t",
                "Carrier", "quoted mixed-case hex with field whitespace");
    expect_name("+CRSM: 159,17,0343617272696572FFFFFFFFFFFFFFFFFF", "Carrier",
                "GSM 9F status with complete payload");
    expect_name("+CRSM: 144,0,FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF", "",
                "all-FF EFSPN is valid empty");
    expect_alpha(NULL, 0u, "", "empty name excludes display condition byte");
    expect_alpha(ALPHA("  US  Mobile  "), "US  Mobile", "trim only outer spaces");
    expect_alpha(ALPHA("                "), "", "blank alpha is valid empty");

    static const char *const malformed[] = {
        "", "OK", "ERROR", "#SPN: Carrier", "+CRSM:", "+CRSM: 144,0",
        "+CRSM: 144,0,", "+CRSM: 144,0,\"\"", "+CRSM: 144,0,00",
        "+CRSM: 144,0,FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF",
        "+CRSM: 144,0,FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF",
        "+CRSM: 144,0,FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFG",
        "+CRSM: 144,0,FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF,1",
        "+CRSM: 144,0,\"FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF",
        "+CRSM: 144,0,FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF\"",
        "+CRSM: 144,0,\"FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF\"junk",
        "+CRSM: +144,0,FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF",
        "+CRSM: 144,-0,FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF",
        "+CRSM: 144,256,FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF",
        "+CRSM: 0144,0,FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF",
        "+CRSM: 4294967296,0,FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF",
        "+CRSM: 0x90,0,FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF",
        "+CRSM: 144,0,FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF FF",
        "+CRSM: 144,0,FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF\rFF",
        "+CRSM: 144,0,FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF\nFF",
        "+CRSM: 144,0,FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF?FF",
        " +CRSM: 144,0,FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF",
    };
    for (size_t i = 0u; i < sizeof(malformed) / sizeof(malformed[0]); i++) {
        expect_invalid(malformed[i], "malformed response fails closed");
    }
    static const unsigned failures[][2] = {
        {144u, 1u}, {106u, 130u}, {148u, 4u}, {98u, 130u},
        {97u, 17u}, {108u, 17u}, {0u, 0u}, {256u, 0u},
    };
    char line[96];
    for (size_t i = 0u; i < sizeof(failures) / sizeof(failures[0]); i++) {
        alpha_response(ALPHA("Carrier"), failures[i][0], failures[i][1], false, line);
        expect_invalid(line, "SIM errors cannot validate even a complete payload");
    }
    for (unsigned sw2 = 0u; sw2 <= 255u; sw2++) {
        alpha_response(ALPHA("Carrier"), 159u, sw2, false, line);
        expect_name(line, "Carrier", "9Fxx sweep accepts only with complete EF data");
    }
}

static void test_gsm(void) {
    expect_alpha(ALPHA("A\0B"), "A@B", "GSM NUL safely survives hex transport");
    expect_alpha(ALPHA("\"A,B?\""), "\"A,B?\"", "quotes commas and real question marks survive");
    expect_alpha(ALPHA("\x01\x02\x03\x04\x05\x06\x07\x08\x09"),
                 "\xc2\xa3$\xc2\xa5\xc3\xa8\xc3\xa9\xc3\xb9"
                 "\xc3\xac\xc3\xb2\xc3\x87", "low GSM printable glyphs");
    expect_alpha(ALPHA("\x0b\x0c\x0e\x0f\x10\x11\x12\x13"),
                 "\xc3\x98\xc3\xb8\xc3\x85\xc3\xa5\xce\x94_"
                 "\xce\xa6\xce\x93", "GSM accented and Greek glyphs");
    expect_alpha(ALPHA("\x14\x15\x16\x17\x18\x19\x1a\x1c\x1d\x1e\x1f"),
                 "\xce\x9b\xce\xa9\xce\xa0\xce\xa8\xce\xa3\xce\x98"
                 "\xce\x9e\xc3\x86\xc3\xa6\xc3\x9f\xc3\x89",
                 "remaining low GSM printable glyphs");
    expect_alpha(ALPHA("$@[\\]^_`{|}~\x7f"),
                 "\xc2\xa4\xc2\xa1\xc3\x84\xc3\x96\xc3\x91\xc3\x9c"
                 "\xc2\xa7\xc2\xbf\xc3\xa4\xc3\xb6\xc3\xb1\xc3\xbc"
                 "\xc3\xa0", "upper GSM table is not ASCII or Latin-1");
    expect_alpha(ALPHA("\x1b\x14\x1b\x28\x1b\x29\x1b\x2f"
                       "\x1b\x3c\x1b\x3d\x1b\x3e\x1b\x40"),
                 "^{}\\[~]|", "eight escapes consume all 16 alpha bytes");
    expect_alpha(ALPHA("A\x1b\x65" "B"), "A\xe2\x82\xac" "B", "GSM euro");
    reject_alpha(ALPHA("A\rB"), "GSM CR rejected, never silently stripped");
    reject_alpha(ALPHA("A\nB"), "GSM LF rejected, never silently split");
    reject_alpha(ALPHA("A\xff" "B"), "nonpadding after GSM terminator rejected");
    reject_alpha(ALPHA("A\x80"), "high-bit GSM byte rejected");
    reject_alpha(ALPHA("A\x1b"), "escape followed by padding rejected");
    reject_alpha(ALPHA("123456789012345\x1b"), "escape at exact end rejected");
    for (unsigned value = 0u; value <= 255u; value++) {
        uint8_t alpha[] = {0x1bu, (uint8_t)value};
        bool defined = value == 0x14u || value == 0x28u || value == 0x29u ||
                       value == 0x2fu || value == 0x3cu || value == 0x3du ||
                       value == 0x3eu || value == 0x40u || value == 0x65u;
        char line[96];
        alpha_response(alpha, sizeof(alpha), 144u, 0u, true, line);
        char out[8] = "stale";
        bool valid = telit_parse_sim_provider_name(line, out, sizeof(out));
        check(defined ? valid : !valid && out[0] == '\0',
              "all extension bytes checked without replacement glyphs");
    }
}

static void test_ucs2(void) {
    expect_alpha(ALPHA("\x80\x00\x41\x00\x40\x4e\x2d\x65\x87"),
                 "A@\xe4\xb8\xad\xe6\x96\x87", "80 big-endian UCS2 mixed text");
    expect_alpha(ALPHA("\x80\x00\x20\x00\x41\x00\x20"), "A", "80 outer spaces trimmed");
    expect_alpha(ALPHA("\x80"), "", "80 empty FF-padded text");
    expect_alpha(ALPHA("\x81\x03\x13\x53\x95\xff"),
                 "S\xe0\xa6\x95\xe0\xa7\xbf", "81 half-page base and counted FF offset");
    expect_alpha(ALPHA("\x81\x04\x08\x90\x1b\x65\x00"),
                 "\xd0\x90\xe2\x82\xac@", "81 mixed UCS2 GSM escape and NUL");
    expect_alpha(ALPHA("\x82\x03\x4e\x2d\x80\x41\x81"),
                 "\xe4\xb8\xad" "A\xe4\xb8\xae", "82 unaligned 16-bit base");
    expect_alpha(ALPHA("\x82\x02\x90\x00\x80\xff"),
                 "\xe9\x80\x80\xe9\x81\xbf", "82 base above 7FFF and counted FF");
    expect_alpha(ALPHA("\x81\x00\x00"), "", "81 explicit zero length");
    expect_alpha(ALPHA("\x82\x00\x00\x00"), "", "82 explicit zero length");

    reject_alpha(ALPHA("\x80\x00\x00"), "80 embedded NUL rejected");
    reject_alpha(ALPHA("\x80\x00\x0d"), "80 CR rejected");
    reject_alpha(ALPHA("\x80\x00\x0a"), "80 LF rejected");
    reject_alpha(ALPHA("\x80\xd8\x00"), "80 surrogate rejected");
    reject_alpha(ALPHA("\x80\xff\xfe"), "80 noncharacter rejected");
    reject_alpha(ALPHA("\x80\xff\xff\x00\x41"), "80 data after padding rejected");
    reject_alpha(ALPHA("\x80\x00\x41\x00\x41\x00\x41\x00\x41"
                       "\x00\x41\x00\x41\x00\x41\x00"), "80 unpaired final byte rejected");
    reject_alpha(ALPHA("\x81\x0e\x00"), "81 length cannot exceed remaining 13 bytes");
    reject_alpha(ALPHA("\x82\x0d\x00\x00"), "82 length cannot exceed remaining 12 bytes");
    reject_alpha(ALPHA("\x81\x01\x00\x41\x42"), "81 data after declared length rejected");
    reject_alpha(ALPHA("\x82\x01\x00\x00\x41\x42"), "82 data after declared length rejected");
    reject_alpha(ALPHA("\x81\x01\x00\x1b"), "81 truncated escape rejected");
    reject_alpha(ALPHA("\x82\x01\xff\xff\xff"), "82 base-plus-offset cannot overflow");
    reject_alpha(ALPHA("\x82\x01\xd8\x00\x80"), "82 surrogate offset rejected");
    reject_alpha(ALPHA("\x83\x01\x02"), "unknown alpha coding rejected");

    static const uint16_t unsafe[] = {
        0x0000u, 0x001fu, 0x007fu, 0x0085u, 0x009fu, 0xd800u, 0xdfffu,
        0xfdd0u, 0xfdefu, 0xfffdu, 0xfffeu, 0xfeffu, 0x200eu, 0x200fu,
        0x2028u, 0x2029u, 0x202eu, 0x2066u, 0x2069u,
    };
    for (size_t i = 0u; i < sizeof(unsafe) / sizeof(unsafe[0]); i++) {
        uint8_t alpha[] = {0x80u, (uint8_t)(unsafe[i] >> 8u), (uint8_t)unsafe[i]};
        reject_alpha(alpha, sizeof(alpha), "unsafe Unicode scalar rejected");
    }
}

static void test_bounds(void) {
    expect_alpha(ALPHA("1234567890123456"), "1234567890123456", "16-byte alpha maximum");
    uint8_t alpha[16] = {0x81u, 13u, 0x9cu};
    memset(alpha + 3u, 0x80, 13u);
    char line[96];
    alpha_response(alpha, sizeof(alpha), 144u, 0u, true, line);
    char expected[40];
    for (size_t i = 0u; i < 13u; i++) {
        memcpy(expected + 3u * i, "\xe4\xb8\x80", 3u);
    }
    expected[39] = '\0';
    expect_name(line, expected, "81 maximum three-byte expansion");
    for (size_t cap = 1u; cap <= TELIT_SIM_PROVIDER_NAME_UTF8_CAP; cap++) {
        unsigned char guarded[TELIT_SIM_PROVIDER_NAME_UTF8_CAP + 2u];
        memset(guarded, 0xa5, sizeof(guarded));
        char *out = (char *)guarded + 1u;
        bool valid = telit_parse_sim_provider_name(line, out, cap);
        check(cap >= sizeof(expected) ? valid && strcmp(out, expected) == 0
                                      : !valid && out[0] == '\0',
              "capacity sweep never publishes partial UTF-8");
        check(guarded[0] == 0xa5u, "leading output canary preserved");
        for (size_t i = cap + 1u; i < sizeof(guarded); i++) {
            check(guarded[i] == 0xa5u, "trailing output canary preserved");
        }
    }
    memset(alpha, 0x05, sizeof(alpha));
    alpha_response(alpha, sizeof(alpha), 144u, 0u, false, line);
    for (size_t i = 0u; i < 16u; i++) {
        memcpy(expected + 2u * i, "\xc3\xa9", 2u);
    }
    expected[32] = '\0';
    expect_name(line, expected, "GSM maximum expansion");

    expect_invalid(NULL, "null input clears output");
    check(!telit_parse_sim_provider_name(line, NULL, 1u), "null output rejected");
    char sentinel = '!';
    check(!telit_parse_sim_provider_name(line, &sentinel, 0u) && sentinel == '!',
          "zero capacity does not write");
    check(!telit_parse_sim_provider_name(line, &sentinel, 1u) && sentinel == '\0',
          "nonempty name never truncates to valid empty");
    alpha_response(NULL, 0u, 144u, 0u, false, line);
    check(telit_parse_sim_provider_name(line, &sentinel, 1u) && sentinel == '\0',
          "valid empty name fits one byte");

    char too_long[98];
    memset(too_long, 'A', sizeof(too_long));
    memcpy(too_long, "+CRSM:", 6u);
    expect_invalid(too_long, "unterminated long input scan is bounded");
}

int main(void) {
    test_grammar_and_status();
    test_gsm();
    test_ucs2();
    test_bounds();
    if (s_failures != 0u) {
        fprintf(stderr, "%u Telit operator-name test(s) failed\n", s_failures);
        return 1;
    }
    puts("Telit operator-name tests passed");
    return 0;
}

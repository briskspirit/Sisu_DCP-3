/* Host bounds and behavior tests for the vendor-neutral AT line modules. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "services/modem_at_util.h"
#include "services/modem_line_framer.h"
#include "services/modem_line_parser.h"

static int s_failures;

static void check(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        s_failures++;
    }
}

static void test_transport_line_bound(void) {
    modem_line_framer_t framer;
    uint32_t dropped = 0u;
    modem_line_framer_reset(&framer);

    for (size_t i = 0u; i + 1u < MODEM_LINE_FRAMER_CAP; i++) {
        check(modem_line_framer_feed(&framer, (uint8_t)'A') ==
                  MODEM_LINE_FRAMER_NONE,
              "in-bound bytes do not emit early");
    }
    check(!modem_line_framer_overflowed(&framer) &&
              modem_line_framer_pending(&framer),
          "cap-minus-one line remains valid and pending");
    check(modem_line_framer_feed(&framer, (uint8_t)'\n') ==
              MODEM_LINE_FRAMER_LINE &&
              strlen(modem_line_framer_line(&framer)) ==
                  MODEM_LINE_FRAMER_CAP - 1u,
          "cap-minus-one line is delivered intact");

    for (size_t i = 0u; i < MODEM_LINE_FRAMER_CAP; i++) {
        (void)modem_line_framer_feed(&framer, (uint8_t)'B');
    }
    check(modem_line_framer_overflowed(&framer) && dropped == 0u,
          "overlong line is marked before its boundary");
    if (modem_line_framer_feed(&framer, (uint8_t)'\n') ==
        MODEM_LINE_FRAMER_DROPPED) {
        dropped++;
    }
    check(
              !modem_line_framer_pending(&framer) &&
              dropped == 1u,
          "overlong line is discarded and counted exactly once");
    check(modem_line_framer_feed(&framer, (uint8_t)'\n') ==
              MODEM_LINE_FRAMER_NONE &&
              dropped == 1u,
          "empty line does not double-count overflow");

    (void)modem_line_framer_feed(&framer, (uint8_t)'\r');
    (void)modem_line_framer_feed(&framer, 0x80u);
    check(modem_line_framer_feed(&framer, (uint8_t)'\n') ==
              MODEM_LINE_FRAMER_LINE &&
              strcmp(modem_line_framer_line(&framer), "?") == 0,
          "CR is ignored and non-ASCII input is sanitized");
    check(modem_line_framer_feed(NULL, (uint8_t)'x') ==
              MODEM_LINE_FRAMER_NONE &&
              modem_line_framer_line(NULL) == NULL &&
              !modem_line_framer_pending(NULL),
          "line framer null guards are inert");
}

static void test_bounded_helpers(void) {
    char small[5];
    modem_at_copy_bounded(small, sizeof(small), "abcdefgh");
    check(strcmp(small, "abcd") == 0,
          "copy_bounded truncates and terminates");
    modem_at_copy_bounded(small, sizeof(small), NULL);
    check(small[0] == '\0', "copy_bounded accepts a null source");
    modem_at_copy_bounded(NULL, sizeof(small), "x");
    modem_at_copy_bounded(small, 0u, "x");

    char quoted[6];
    modem_at_append_quoted(quoted, sizeof(quoted), "a\"bcdef");
    check(quoted[0] == '"' && quoted[sizeof(quoted) - 1u] == '\0',
          "append_quoted remains bounded");
    char one[1] = {'!'};
    modem_at_append_quoted(one, sizeof(one), "abc");
    check(one[0] == '\0', "append_quoted handles a one-byte destination");

    char hex[5];
    modem_at_copy_hex_token(hex, sizeof(hex), "\"ab12cdef\"");
    check(strcmp(hex, "AB12") == 0,
          "copy_hex_token strips, uppercases, and bounds");
    check(modem_at_parse_uint_token("42tail", 7u) == 42u,
          "parse_uint_token accepts a numeric prefix");
    check(modem_at_parse_uint_token("bad", 7u) == 7u,
          "parse_uint_token returns its fallback");
}

static void test_csv_parser(void) {
    const char *cursor = "\"Op Name\",,123456789";
    char field[8];
    check(modem_at_csv_next_field(&cursor, field, sizeof(field)) &&
              strcmp(field, "Op Name") == 0,
          "CSV strips quoted fields");
    check(modem_at_csv_next_field(&cursor, field, sizeof(field)) &&
              field[0] == '\0',
          "CSV preserves empty fields");
    check(modem_at_csv_next_field(&cursor, field, sizeof(field)) &&
              strcmp(field, "1234567") == 0,
          "CSV consumes and bounds long fields");
    check(!modem_at_csv_next_field(&cursor, field, sizeof(field)),
          "CSV reaches end");

    cursor = "\"unterminated";
    check(modem_at_csv_next_field(&cursor, field, sizeof(field)) &&
              strcmp(field, "untermi") == 0,
          "CSV safely consumes an unterminated quote");
    check(!modem_at_csv_next_field(NULL, field, sizeof(field)) &&
              !modem_at_csv_next_field(&cursor, NULL, sizeof(field)) &&
              !modem_at_csv_next_field(&cursor, field, 0u),
          "CSV rejects invalid outputs");
}

static void test_registration_and_operator(void) {
    modem_csq_line_t csq = {0u, 0u};
    check(modem_line_parse_csq("+CSQ: 31,7", &csq) &&
              csq.rssi == 31u && csq.ber == 7u,
          "CSQ values parsed");
    check(modem_line_parse_csq("+CSQ:  0 , 0  ", &csq) &&
              csq.rssi == 0u && csq.ber == 0u &&
              modem_line_parse_csq("+CSQ: 99,99", &csq) &&
              csq.rssi == 99u && csq.ber == 99u,
          "CSQ accepts both bounded endpoint and unknown-value encodings");
    csq.rssi = 31u;
    csq.ber = 7u;
    check(!modem_line_parse_csq("+CSQ: malformed", &csq) &&
              !modem_line_parse_csq("+CSQ: -1,0", &csq) &&
              !modem_line_parse_csq("+CSQ: +1,0", &csq) &&
              !modem_line_parse_csq("+CSQ: 32,0", &csq) &&
              !modem_line_parse_csq("+CSQ: 0,8", &csq) &&
              !modem_line_parse_csq("+CSQ: 100,99", &csq) &&
              !modem_line_parse_csq(
                  "+CSQ: 42949672960,0", &csq) &&
              !modem_line_parse_csq("+CSQ: 1,0junk", &csq) &&
              !modem_line_parse_csq("+CSQ: 1,0,3", &csq) &&
              !modem_line_parse_csq("noise +CSQ: 1,0", &csq) &&
              csq.rssi == 31u && csq.ber == 7u,
          "signed, out-of-range, overflowing, and trailing CSQ data are rejected atomically");

    unsigned status = 0xffu;
    check(modem_line_parse_cereg(
              "+CEREG: 5,\"56AB\",\"0A1B2C3D\",7", &status) &&
              status == 5u,
          "CEREG location URC reads stat from field one");
    check(modem_line_parse_cereg(
              "+CEREG: 2,1,\"56AB\",\"0A1B2C3D\",7", &status) &&
              status == 1u,
          "CEREG solicited response reads stat from field two");
    check(!modem_line_parse_cereg("CEREG malformed", &status) &&
              status == 1u,
          "CEREG without a colon leaves caller output unchanged");
    check(!modem_line_parse_cereg("+CEREG: malformed", &status) &&
              !modem_line_parse_cereg("+CEREG: 1junk", &status) &&
              !modem_line_parse_cereg("+CEREG: 2,x", &status) &&
              !modem_line_parse_cereg("+CEREG: -0", &status) &&
              !modem_line_parse_cereg("+CEREG: -1", &status) &&
              !modem_line_parse_cereg("+CEREG: +1", &status) &&
              !modem_line_parse_cereg("+CEREG: 4294967296", &status) &&
              status == 1u,
          "malformed, signed, and overflowing CEREG fields are rejected");
    check(modem_line_parse_cereg("+CEREG: 257", &status) &&
              status == 257u,
          "CEREG retains the untruncated policy value");

    char operator_name[17];
    modem_line_parse_cops("+COPS: 0,0,\"A Very Long Operator Name\",7",
                          operator_name, sizeof(operator_name));
    check(strlen(operator_name) == sizeof(operator_name) - 1u,
          "COPS operator is bounded");
    modem_line_parse_cops("+COPS: 0", operator_name,
                          sizeof(operator_name));
    check(operator_name[0] == '\0',
          "COPS without a name clears stale text");
}

static void test_sms_metadata(void) {
    check(strcmp(modem_line_sms_status_from_numeric(0u), "REC UNREAD") == 0 &&
              strcmp(modem_line_sms_status_from_numeric(3u), "STO SENT") == 0 &&
              modem_line_sms_status_from_numeric(4u) == NULL,
          "numeric SMS status domain is exact");

    uint16_t used = 0u;
    uint16_t total = 0u;
    check(modem_line_parse_cpms(
              "+CPMS: \"ME\",1,255,\"ME\",2,255,\"ME\",3,255",
              &used, &total) &&
              used == 3u && total == 255u,
          "CPMS selects the receive-store triple");
    check(!modem_line_parse_cpms("+CPMS: \"ME\",1,255", &used,
                                 &total),
          "short CPMS response is rejected");

    uint16_t index = 0u;
    check(modem_line_parse_cmti("+CMTI: \"ME\",42", "ME", &index) &&
              index == 42u,
          "CMTI parses the expected storage and index");
    check(!modem_line_parse_cmti("+CMTI: \"SM\",42", "ME", &index) &&
              !modem_line_parse_cmti("+CMTI: \"ME\",42 junk", "ME",
                                     &index),
          "CMTI rejects foreign storage and trailing data");

    /* These parsers are capacity-driven and deliberately do not own the
     * modem service's receive-record layout. Use independent test buffers. */
    char sms_status[16u] = "stale";
    char sender[40u] = "stale";
    char timestamp[32u] = "stale";
    modem_line_parse_cmgr_header(
        "+CMGR: 0,,123", sms_status, sizeof(sms_status), sender,
        sizeof(sender), timestamp, sizeof(timestamp));
    check(strcmp(sms_status, "REC UNREAD") == 0,
          "numeric CMGR unread status parsed");

    sms_status[0] = sender[0] = timestamp[0] = '\0';
    modem_line_parse_cmgr_header(
        "+CMGR: \"REC READ\",\"+15550000\",,\"24/01/02,03:04:05+00\"",
        sms_status, sizeof(sms_status), sender, sizeof(sender), timestamp,
        sizeof(timestamp));
    check(strcmp(sms_status, "REC READ") == 0 &&
              strcmp(sender, "+15550000") == 0 &&
              strcmp(timestamp, "24/01/02,03:04:05+00") == 0,
          "text CMGR fields parsed");

    sms_status[0] = '\0';
    modem_line_parse_cmgr_header(
        "+CMGR: 9,,5", sms_status, sizeof(sms_status), sender,
        sizeof(sender), timestamp, sizeof(timestamp));
    check(sms_status[0] == '\0',
          "out-of-domain CMGR status is rejected");
}

static void test_call_indications(void) {
    modem_cli_line_t cli;
    check(modem_line_parse_clip(
              "+CLIP: \"+15551234567\",145,,,,0", &cli) &&
              strcmp(cli.number, "+15551234567") == 0 &&
              cli.validity == 0u,
          "CLIP presents a typed caller identity");
    check(modem_line_parse_ccwa(
              "+CCWA: \"5552000\",129,1,\"Doe, Jane\",0", &cli) &&
              strcmp(cli.number, "5552000") == 0 && cli.validity == 0u,
          "CCWA accepts a quoted alpha containing a comma");
    check(!modem_line_is_ccwa_urc("+CCWA: 0") &&
              modem_line_is_ccwa_urc("+CCWA: \"5552000\",129,1"),
          "CCWA query and unsolicited forms stay distinct");
    check(modem_line_parse_clip("+CLIP: \"\",145,,,,0", &cli) &&
              cli.number[0] == '\0' && cli.validity == 2u,
          "empty CLIP identity becomes unavailable");
    check(!modem_line_parse_ccwa(
              "+CCWA: \"5552000\",129,1,\"unterminated,0", &cli),
          "unbalanced CCWA quotes are rejected");
}

static void test_sim_absent_error(void) {
    check(modem_line_is_sim_absent_error("+CME ERROR: 10") &&
              modem_line_is_sim_absent_error(
                  "+CME ERROR: SIM not inserted") &&
              !modem_line_is_sim_absent_error("+CME ERROR: 13") &&
              !modem_line_is_sim_absent_error("ERROR"),
          "only conclusive SIM-absent errors are accepted");
}

int main(void) {
    test_transport_line_bound();
    test_bounded_helpers();
    test_csv_parser();
    test_registration_and_operator();
    test_sms_metadata();
    test_call_indications();
    test_sim_absent_error();

    if (s_failures != 0) {
        fprintf(stderr, "%d parser test(s) failed\n", s_failures);
        return 1;
    }
    puts("modem line parser tests passed");
    return 0;
}

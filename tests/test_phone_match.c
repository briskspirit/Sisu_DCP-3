#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "services/phone_match.h"

static int s_failures;

static void assert_true(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        s_failures++;
    }
}

/* Oracle: the v6.00 display resolver rule as read out of the ROM (core
 * 0x00257fbc): match form = leading '+' kept + digits, truncated at the
 * first other char; a form >= 8 chars contributes its last 7 chars, a
 * shorter form contributes all of itself; match = exact compare of the
 * contributions. Expected values below are derived from that rule, not
 * from the implementation. */

static void test_form(void) {
    char buf[PHONE_MATCH_FORM_MAX];

    phone_match_form("+358501234567", buf, sizeof(buf));
    assert_true(strcmp(buf, "+358501234567") == 0, "form keeps leading + and digits");

    phone_match_form("050p123", buf, sizeof(buf));
    assert_true(strcmp(buf, "050") == 0, "form truncates at pause char");

    phone_match_form("050*123#", buf, sizeof(buf));
    assert_true(strcmp(buf, "050") == 0, "form truncates at star");

    phone_match_form("123+456", buf, sizeof(buf));
    assert_true(strcmp(buf, "123") == 0, "mid-string + truncates (only leading + kept)");

    phone_match_form("MYBANK", buf, sizeof(buf));
    assert_true(buf[0] == '\0', "alphanumeric sender forms empty");

    phone_match_form(0, buf, sizeof(buf));
    assert_true(buf[0] == '\0', "NULL forms empty");

    buf[0] = 'x';
    phone_match_form("12345", buf, 1u);
    assert_true(buf[0] == '\0', "cap 1 yields empty string");
}

static void test_local_vs_international(void) {
    /* The reported bug: trunk-local vs TON-international of the same
     * subscriber. Shared 7-char tail "1234567" -> match. */
    assert_true(phone_match_numbers("0501234567", "+358501234567"),
                "local 050 matches +358 50 (bug case)");
    assert_true(phone_match_numbers("+358501234567", "0501234567"),
                "match is symmetric");
    assert_true(phone_match_numbers("358501234567", "0501234567"),
                "00-stripped international matches local");
    /* NANP still works through the same rule (no special case needed). */
    assert_true(phone_match_numbers("15551234567", "5551234567"),
                "NANP 1-prefix matches via the 7-tail");
}

static void test_seven_tail_semantics(void) {
    assert_true(phone_match_numbers("0501234567", "0501234567"), "exact match");
    /* Only the last 7 characters participate for forms >= 8 chars -- the
     * original ignores everything before the window. */
    assert_true(phone_match_numbers("9991234567", "0501234567"),
                "differing prefixes, equal 7-tail -> match (original semantics)");
    assert_true(!phone_match_numbers("0501234567", "0501234568"),
                "last digit differs -> no match");
    /* A 7-digit form matches a longer number ending in it... */
    assert_true(phone_match_numbers("1234567", "+358501234567"),
                "7-digit form matches a longer tail");
    /* ...but a 6-digit form cannot (6 chars vs a 7-char window). */
    assert_true(!phone_match_numbers("234567", "+358501234567"),
                "6-digit form does not match a longer number");
    /* An 8-char '+'-form drops the '+' out of the window. */
    assert_true(phone_match_numbers("+1234567", "1234567"),
                "+7-digit (8 chars) tails to the 7 digits");
}

static void test_short_codes(void) {
    assert_true(phone_match_numbers("112", "112"), "short code exact match");
    assert_true(!phone_match_numbers("112", "112112"),
                "short code does not match a longer code (whole-string compare)");
    assert_true(!phone_match_numbers("+12345", "12345"),
                "short international keeps the + (degenerate case, no match)");
    assert_true(!phone_match_numbers("16100", "0501610 0"),
                "truncated form 0501610 vs 16100 do not match");
}

static void test_truncation_matching(void) {
    /* Stored numbers with pause/wait suffixes match on the prefix form,
     * exactly as the original's BCD nibble truncation behaves. */
    assert_true(phone_match_numbers("040123456p123", "+35840123456"),
                "pause suffix ignored, tail of the digits matches");
    assert_true(!phone_match_numbers("050*1234567", "0501234567"),
                "star truncates the form to 050 -> no match vs full number");
}

static void test_degenerate(void) {
    assert_true(!phone_match_numbers("", "0501234567"), "empty never matches");
    assert_true(!phone_match_numbers("0501234567", ""), "empty never matches (b)");
    assert_true(!phone_match_numbers("", ""), "two empties never match");
    assert_true(!phone_match_numbers("MYBANK", "MYBANK"),
                "alphanumeric senders never match (empty forms)");
    assert_true(!phone_match_numbers(0, "123"), "NULL never matches");
}

int main(void) {
    test_form();
    test_local_vs_international();
    test_seven_tail_semantics();
    test_short_codes();
    test_truncation_matching();
    test_degenerate();
    if (s_failures != 0) {
        fprintf(stderr, "%d failure(s)\n", s_failures);
        return 1;
    }
    printf("test_phone_match: all assertions passed\n");
    return 0;
}

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "apps/clock_date_logic.h"

static int s_failures;

static void assert_true(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        s_failures++;
    }
}

static void test_untouched_setup_date_defaults(void) {
    uint8_t day = 0u;
    uint8_t month = 0u;
    uint16_t year = 0u;
    clock_date_resolution_t result = clock_date_resolve(
        "dd.mm.yyyy", true, &day, &month, &year);
    assert_true(result == CLOCK_DATE_DEFAULTED, "untouched setup date is accepted");
    assert_true(day == 1u && month == 1u && year == 1999u,
                "untouched setup date becomes 01.01.1999");
}

static void test_default_is_setup_only_and_requires_no_digits(void) {
    uint8_t day = 0u;
    uint8_t month = 0u;
    uint16_t year = 0u;
    assert_true(clock_date_resolve("dd.mm.yyyy", false, &day, &month, &year) ==
                    CLOCK_DATE_INVALID,
                "normal date edit cannot select the default");
    assert_true(clock_date_resolve("01.mm.yyyy", true, &day, &month, &year) ==
                    CLOCK_DATE_INVALID,
                "partially entered setup date stays invalid");
    assert_true(clock_date_resolve("31.02.2026", true, &day, &month, &year) ==
                    CLOCK_DATE_INVALID,
                "complete invalid setup date stays invalid");
}

static void test_entered_date_wins_over_default(void) {
    uint8_t day = 0u;
    uint8_t month = 0u;
    uint16_t year = 0u;
    clock_date_resolution_t result = clock_date_resolve(
        "29.02.2028", true, &day, &month, &year);
    assert_true(result == CLOCK_DATE_ENTERED, "valid entered date is retained");
    assert_true(day == 29u && month == 2u && year == 2028u,
                "entered date fields parse in day-month-year order");
}

static void test_date_domain_and_output_guards(void) {
    uint8_t day = 0u;
    uint8_t month = 0u;
    uint16_t year = 0u;
    assert_true(clock_date_valid(1u, 1u, 1999u), "v6.00 epoch is valid");
    assert_true(!clock_date_valid(31u, 4u, 2026u), "April 31 is invalid");
    assert_true(!clock_date_parse("01.01.1999", NULL, &month, &year),
                "date parse rejects null outputs");
    assert_true(clock_date_resolve(NULL, true, &day, &month, &year) == CLOCK_DATE_INVALID,
                "null input is not treated as an untouched UI template");
}

int main(void) {
    test_untouched_setup_date_defaults();
    test_default_is_setup_only_and_requires_no_digits();
    test_entered_date_wins_over_default();
    test_date_domain_and_output_guards();

    if (s_failures == 0) {
        puts("test_clock_date_logic: all passed");
        return 0;
    }
    fprintf(stderr, "test_clock_date_logic: %d failure(s)\n", s_failures);
    return 1;
}

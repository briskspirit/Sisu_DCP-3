#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "tuner_logic.h"

static int failures;

static void check(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        failures++;
    }
}

static void check_branch(uint8_t digit, uint8_t rf_path,
                         bool gpio3, bool gpio2, const char *bands) {
    const telit_tuner_branch_t *branch =
        telit_tuner_branch_for_digit(digit);
    check(branch != NULL, "branch exists");
    if (branch == NULL) {
        return;
    }
    check(branch->rf_path == rf_path, "RF path follows keypad digit");
    check(branch->gpio3 == gpio3, "GPIO3 mapping");
    check(branch->gpio2 == gpio2, "GPIO2 mapping");
    check(strcmp(branch->bands, bands) == 0, "band label");
}

int main(void) {
    check(telit_tuner_branch_count() == 4u, "exactly four switch throws");
    check_branch(1u, 1u, false, false, "B2");
    check_branch(2u, 2u, true, false, "B12/B14");
    check_branch(3u, 3u, false, true, "B5");
    check_branch(4u, 4u, true, true, "B4");
    check(telit_tuner_branch_for_digit(0u) == NULL, "digit zero rejected");
    check(telit_tuner_branch_for_digit(5u) == NULL, "digit five rejected");

    const uint8_t ok_with_nul[] = {0u, 0u, ' ', 'O', 'K', ' '};
    check(telit_tuner_parse_final(ok_with_nul, sizeof ok_with_nul) ==
              TELIT_TUNER_FINAL_OK,
          "leading boot NULs accepted on final");
    check(telit_tuner_parse_final((const uint8_t *)"ERROR", 5u) ==
              TELIT_TUNER_FINAL_ERROR,
          "plain ERROR parsed");
    check(telit_tuner_parse_final(
              (const uint8_t *)"+CME ERROR: operation not allowed", 33u) ==
              TELIT_TUNER_FINAL_ERROR,
          "CME ERROR parsed");

    uint8_t cfun = 0u;
    check(telit_tuner_parse_cfun((const uint8_t *)"+CFUN: 4", 8u,
                                 &cfun) && cfun == 4u,
          "CFUN=4 parsed");
    check(!telit_tuner_parse_cfun((const uint8_t *)"+CFUN: 4 junk", 13u,
                                  &cfun),
          "CFUN trailing junk rejected");

    bool enabled = false;
    check(telit_tuner_parse_stuneant(
              (const uint8_t *)"#STUNEANT: 1", 13u, &enabled) && enabled,
          "STUNEANT enabled parsed");
    check(!telit_tuner_parse_stuneant(
              (const uint8_t *)"#STUNEANT: 2", 13u, &enabled),
          "invalid STUNEANT state rejected");

    uint8_t direction = 0u;
    bool level = false;
    check(telit_tuner_parse_gpio((const uint8_t *)"#GPIO: 1,1", 10u,
                                 &direction, &level) &&
              direction == 1u && level,
          "GPIO output-high parsed");
    check(telit_tuner_parse_gpio((const uint8_t *)"#GPIO: 1, 0", 11u,
                                 &direction, &level) &&
              direction == 1u && !level,
          "GPIO whitespace parsed");
    check(!telit_tuner_parse_gpio((const uint8_t *)"#GPIO: 1,2", 10u,
                                  &direction, &level),
          "invalid GPIO level rejected");

    if (failures != 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    puts("telit tuner logic tests passed");
    return 0;
}

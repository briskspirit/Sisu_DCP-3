#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "apps/call_options_logic.h"

static int s_failures;

static void check_mask(uint16_t actual, uint16_t expected,
                       const char *message) {
    if (actual != expected) {
        fprintf(stderr, "FAIL: %s (got 0x%03x, expected 0x%03x)\n",
                message, actual, expected);
        s_failures++;
    }
}

int main(void) {
    check_mask(call_options_mask_for_state(true, false, false, true),
               0x685u, "sole active preserves the v6.00 Hold menu");
    check_mask(call_options_mask_for_state(false, true, false, true),
               0x606u, "sole held preserves the v6.00 Unhold menu");
    check_mask(call_options_mask_for_state(true, true, false, false),
               0x698u, "active plus held preserves Swap and has no toggle");

    check_mask(call_options_mask_for_state(true, false, true, true),
               0x6ecu,
               "active plus waiting removes Hold but retains Answer/Reject");
    check_mask(call_options_mask_for_state(false, true, true, true),
               0x664u,
               "held plus waiting removes Unhold but retains Answer/Reject");
    check_mask(call_options_mask_for_state(true, true, true, true),
               0x6d8u,
               "three-leg menu retains Swap/Reject without Hold or Unhold");

    check_mask(call_options_mask_for_state(true, false, false, false),
               0x684u, "backend veto removes Hold from a sole active call");
    check_mask(call_options_mask_for_state(false, true, false, false),
               0x604u, "backend veto removes Unhold from a sole held call");

    if (s_failures != 0) {
        fprintf(stderr, "%d failure(s)\n", s_failures);
        return 1;
    }
    printf("test_call_options_logic: all assertions passed\n");
    return 0;
}

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "services/stack_monitor_logic.h"

static int s_failures;

static void check(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        s_failures++;
    }
}

int main(void) {
    const uint32_t marker = 0xa55a3cc3u;
    uint32_t words[8] = {
        marker, marker, marker, marker,
        marker, marker, marker, marker,
    };

    stack_canary_scan_t scan = stack_canary_scan(
        words, 8u, marker, 64u, 32u);
    check(scan.canary_intact && scan.peak_used_bytes == 32u,
          "untouched fill preserves the initializer's conservative peak");

    words[5] = 0u;
    scan = stack_canary_scan(words, 8u, marker, 64u, 32u);
    check(scan.canary_intact && scan.peak_used_bytes == 44u,
          "first changed word records the deepest touched address");

    words[5] = marker;
    words[0] = 0u;
    scan = stack_canary_scan(words, 8u, marker, 64u, 44u);
    check(!scan.canary_intact && scan.peak_used_bytes == 64u,
          "bottom corruption consumes the full stack and fails the canary");

    scan = stack_canary_scan(NULL, 0u, marker, 64u, 0u);
    check(!scan.canary_intact && scan.peak_used_bytes == 64u,
          "invalid monitor bounds fail closed");

    if (s_failures != 0) {
        return 1;
    }
    printf("stack monitor logic tests passed\n");
    return 0;
}

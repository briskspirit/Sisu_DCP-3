#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "hal/board.h"
#include "services/log.h"
#include "services/shared_3v8_service.h"

uint64_t host_test_time_us;

static bool s_rail_enabled;
static bool s_force_pwm;
static bool s_power_good;
static uint32_t s_enable_writes;
static int s_failures;

void board_3v8_rail_set_enabled(bool enabled) {
    s_rail_enabled = enabled;
    s_enable_writes++;
}

bool board_3v8_rail_power_good(void) {
    return s_power_good;
}

void board_3v8_rail_set_force_pwm(bool enabled) {
    s_force_pwm = enabled;
}

bool board_3v8_rail_enabled(void) {
    return s_rail_enabled;
}

void log_write(log_level_t level, const char *tag, const char *fmt, ...) {
    (void)level;
    (void)tag;
    (void)fmt;
}

static void check(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        s_failures++;
    }
}

static void reset_fixture(void) {
    s_rail_enabled = true;
    s_force_pwm = true;
    s_power_good = false;
    s_enable_writes = 0u;
    host_test_time_us = 0u;
    shared_3v8_service_init();
}

static void test_two_owner_overlap(void) {
    reset_fixture();
    check(!s_rail_enabled && !s_force_pwm, "init forces the physical rail safe");
    check(shared_3v8_service_owner_mask() == 0u, "init clears every owner");
    check(shared_3v8_service_transition_count() == 0u,
          "init starts a fresh transition generation");

    shared_3v8_service_set_required(SHARED_3V8_OWNER_AUDIO, true);
    check(s_rail_enabled, "first owner raises the rail");
    check(shared_3v8_service_transition_count() == 1u,
          "first owner records one physical transition");
    uint32_t writes_after_first = s_enable_writes;

    shared_3v8_service_set_required(SHARED_3V8_OWNER_AUDIO, true);
    check(s_enable_writes == writes_after_first,
          "idempotent acquire does not touch hardware");
    shared_3v8_service_set_required(SHARED_3V8_OWNER_MODEM, true);
    check(s_rail_enabled && shared_3v8_service_owner_mask() == 3u,
          "second owner overlaps without cycling the rail");
    check(shared_3v8_service_transition_count() == 1u,
          "overlap is not a physical transition");

    shared_3v8_service_set_required(SHARED_3V8_OWNER_DIAGNOSTIC, true);
    check(s_rail_enabled && shared_3v8_service_owner_mask() == 7u,
          "diagnostic hold overlaps without cycling the rail");
    shared_3v8_service_set_required(SHARED_3V8_OWNER_DIAGNOSTIC, false);

    shared_3v8_service_set_required(SHARED_3V8_OWNER_AUDIO, false);
    check(s_rail_enabled &&
              shared_3v8_service_owner_mask() == SHARED_3V8_OWNER_MODEM,
          "one owner cannot cut off the other");
    shared_3v8_service_set_required(SHARED_3V8_OWNER_MODEM, false);
    check(!s_rail_enabled && shared_3v8_service_owner_mask() == 0u,
          "last release lowers the rail");
    check(shared_3v8_service_transition_count() == 2u,
          "last release records the falling transition");
}

static void test_invalid_owner_and_force_off(void) {
    reset_fixture();
    shared_3v8_service_set_required(SHARED_3V8_OWNER_AUDIO, true);
    uint32_t generation = shared_3v8_service_transition_count();
    uint32_t writes = s_enable_writes;
    shared_3v8_service_set_required((shared_3v8_owner_t)0u, true);
    shared_3v8_service_set_required((shared_3v8_owner_t)3u, true);
    shared_3v8_service_set_required((shared_3v8_owner_t)8u, true);
    check(shared_3v8_service_owner_mask() == SHARED_3V8_OWNER_AUDIO,
          "invalid owners cannot change the mask");
    check(shared_3v8_service_transition_count() == generation &&
              s_enable_writes == writes,
          "invalid owners cannot touch hardware");

    shared_3v8_service_set_required(SHARED_3V8_OWNER_MODEM, true);
    shared_3v8_service_force_off();
    check(!s_rail_enabled && shared_3v8_service_owner_mask() == 0u,
          "force-off clears both owners and lowers the rail");
    check(shared_3v8_service_transition_count() == generation + 1u,
          "force-off records exactly one falling transition");
    shared_3v8_service_force_off();
    check(shared_3v8_service_transition_count() == generation + 1u,
          "idempotent force-off does not advance the generation");
}

static void test_power_good_wait(void) {
    reset_fixture();
    check(!shared_3v8_service_wait_power_good(10u),
          "PG wait fails immediately with no owner");
    check(host_test_time_us == 0u, "ownerless PG wait consumes no timeout");

    shared_3v8_service_set_required(SHARED_3V8_OWNER_AUDIO, true);
    check(!shared_3v8_service_wait_power_good(10u),
          "PG wait is bounded when PG stays low");
    check(host_test_time_us == 10u, "PG wait honors its timeout");
    s_power_good = true;
    check(shared_3v8_service_wait_power_good(10u),
          "PG wait succeeds immediately once PG is high");
    check(host_test_time_us == 10u, "ready PG adds no delay");
}

int main(void) {
    test_two_owner_overlap();
    test_invalid_owner_and_force_off();
    test_power_good_wait();
    if (s_failures != 0) {
        fprintf(stderr, "%d failures\n", s_failures);
        return 1;
    }
    puts("shared 3V8 service tests passed");
    return 0;
}

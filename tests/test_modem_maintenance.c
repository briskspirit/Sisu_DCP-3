#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "services/modem_maintenance.h"

static int s_failures;
static char s_record[64];
static unsigned s_write_count;
static unsigned s_flush_count;
static unsigned s_hold_count;
static unsigned s_online_count;

static void check(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        s_failures++;
    }
}

static bool write_record(const char *record) {
    snprintf(s_record, sizeof(s_record), "%s", record);
    s_write_count++;
    return true;
}

static bool flush_record(void) {
    s_flush_count++;
    return true;
}

static void hold_offline(void) {
    s_hold_count++;
}

static void begin_online(uint32_t now_ms) {
    (void)now_ms;
    s_online_count++;
}

static bool build_scan_set(uint16_t seconds, char *out, size_t cap) {
    return snprintf(out, cap, "SETSCAN:%u", (unsigned)seconds) > 0;
}

static modem_diag_line_result_t parse_scan(const char *line,
                                           uint16_t *seconds) {
    unsigned value = 0u;
    int consumed = 0;
    if (sscanf(line, "+SCAN:%u%n", &value, &consumed) != 1 ||
        line[consumed] != '\0' || value > UINT16_MAX) {
        return strncmp(line, "+SCAN:", 6u) == 0
                   ? MODEM_DIAG_LINE_INVALID
                   : MODEM_DIAG_LINE_IGNORE;
    }
    *seconds = (uint16_t)value;
    return MODEM_DIAG_LINE_ACCEPT;
}

static bool build_band_mode(uint8_t mode, char *out, size_t cap) {
    return snprintf(out, cap, "SETMODE:%u", (unsigned)mode) > 0;
}

static modem_diag_line_result_t parse_band_mode(const char *line,
                                                uint8_t *mode) {
    unsigned value = 0u;
    int consumed = 0;
    if (sscanf(line, "+MODE:%u%n", &value, &consumed) != 1 ||
        line[consumed] != '\0' || value > UINT8_MAX) {
        return strncmp(line, "+MODE:", 6u) == 0
                   ? MODEM_DIAG_LINE_INVALID
                   : MODEM_DIAG_LINE_IGNORE;
    }
    *mode = (uint8_t)value;
    return MODEM_DIAG_LINE_ACCEPT;
}

static bool build_band(const modem_band_config_t *config,
                       char *out, size_t cap) {
    return snprintf(out, cap, "SETBAND:%u,%u,%llX",
                    (unsigned)config->gsm, (unsigned)config->wcdma,
                    (unsigned long long)config->lte) > 0;
}

static modem_diag_line_result_t parse_band(const char *line,
                                           modem_band_config_t *config) {
    unsigned gsm = 0u;
    unsigned wcdma = 0u;
    unsigned long long lte = 0u;
    int consumed = 0;
    if (sscanf(line, "+BAND:%u,%u,%llX%n", &gsm, &wcdma, &lte,
               &consumed) != 3 || line[consumed] != '\0' ||
        gsm > UINT8_MAX || wcdma > UINT16_MAX || lte == 0u) {
        return strncmp(line, "+BAND:", 6u) == 0
                   ? MODEM_DIAG_LINE_INVALID
                   : MODEM_DIAG_LINE_IGNORE;
    }
    config->gsm = (uint8_t)gsm;
    config->wcdma = (uint16_t)wcdma;
    config->lte = (uint64_t)lte;
    return MODEM_DIAG_LINE_ACCEPT;
}

static bool select_band(uint8_t preset,
                        const modem_band_config_t *active,
                        modem_band_config_t *desired) {
    if (preset != 3u) {
        return false;
    }
    *desired = *active;
    desired->lte = UINT64_C(0x10);
    return true;
}

static bool build_function(uint8_t mode, char *out, size_t cap) {
    return snprintf(out, cap, "SETFUN:%u", (unsigned)mode) > 0;
}

static modem_diag_line_result_t parse_function(const char *line,
                                               uint8_t *mode) {
    return parse_band_mode(line, mode);
}

static const modem_maintenance_backend_t s_backend = {
    .supported = true,
    .scan_timer_query_cmd = "GETSCAN",
    .build_scan_timer_set = build_scan_set,
    .parse_scan_timer = parse_scan,
    .band_mode_query_cmd = "GETMODE",
    .build_band_mode_set = build_band_mode,
    .parse_band_mode = parse_band_mode,
    .band_nvm_query_cmd = "GETNVM",
    .band_ram_query_cmd = "GETRAM",
    .build_band_ram_set = build_band,
    .parse_band_nvm = parse_band,
    .parse_band_ram = parse_band,
    .band_preset = select_band,
    .function_query_cmd = "GETFUN",
    .build_function_set = build_function,
    .parse_function = parse_function,
};

static const modem_maintenance_hooks_t s_hooks = {
    .write_recovery_record = write_record,
    .flush_recovery_record = flush_record,
    .hold_sim_offline = hold_offline,
    .begin_sim_online = begin_online,
};

static void reset_fixture(void) {
    s_record[0] = '\0';
    s_write_count = 0u;
    s_flush_count = 0u;
    s_hold_count = 0u;
    s_online_count = 0u;
    modem_maintenance_init(&s_backend, &s_hooks);
}

static modem_maintenance_snapshot_t snapshot(void) {
    modem_maintenance_snapshot_t result;
    modem_maintenance_get_snapshot(&result);
    return result;
}

static void test_scan_success_and_call_cancel(void) {
    modem_maintenance_dispatch_t dispatch;
    reset_fixture();
    check(modem_maintenance_request_scan_timer(30u) &&
              modem_maintenance_prepare_next(1u, &dispatch) &&
              strcmp(dispatch.command, "SETSCAN:30") == 0 &&
              dispatch.timeout_ms == MODEM_MAINTENANCE_TIMEOUT_MS,
          "scan mutation dispatch is semantic and bounded");
    modem_maintenance_finish_command(true, false, 2u);
    check(modem_maintenance_prepare_next(3u, &dispatch) &&
              strcmp(dispatch.command, "GETSCAN") == 0 &&
              modem_maintenance_parse_expected_line("+SCAN:30"),
          "scan verification consumes its typed readback");
    modem_maintenance_finish_command(true, false, 4u);
    modem_maintenance_snapshot_t done = snapshot();
    check(done.state == MODEM_MAINTENANCE_DONE &&
              done.error == MODEM_MAINTENANCE_ERROR_NONE &&
              done.scan_timer_s == 30u && !modem_maintenance_has_sequence(),
          "matching scan readback completes the transaction");

    check(modem_maintenance_read_scan_timer(),
          "scan query admits after completion");
    modem_maintenance_cancel_for_call(false);
    check(!modem_maintenance_prepare_next(5u, &dispatch),
          "a call cancels an unmutated scan before dispatch");
    done = snapshot();
    check(done.state == MODEM_MAINTENANCE_DONE &&
              done.error == MODEM_MAINTENANCE_ERROR_CANCELLED,
          "pre-dispatch call cancellation is terminal and explicit");
}

static void test_band_record_precedes_mutation_and_cancel_waits(void) {
    modem_maintenance_dispatch_t dispatch;
    reset_fixture();
    check(modem_maintenance_start_band_test(3u) &&
              modem_maintenance_prepare_next(10u, &dispatch) &&
              strcmp(dispatch.command, "GETMODE") == 0 &&
              modem_maintenance_parse_expected_line("+MODE:0"),
          "band test first reads the active selection mode");
    modem_maintenance_finish_command(true, false, 11u);
    check(modem_maintenance_prepare_next(12u, &dispatch) &&
              strcmp(dispatch.command, "GETNVM") == 0 &&
              modem_maintenance_parse_expected_line("+BAND:5,27,123"),
          "band test reads the matching baseline bank");
    modem_maintenance_finish_command(true, false, 13u);
    modem_maintenance_snapshot_t marked = snapshot();
    check(marked.recovery_pending && s_write_count == 1u &&
              s_flush_count == 1u && strcmp(s_record, "B0,5,27,123") == 0,
          "durable original-band record precedes the first mutation");

    check(modem_maintenance_prepare_next(14u, &dispatch) &&
              strcmp(dispatch.command, "SETMODE:1") == 0,
          "RAM-mode mutation dispatches only after durable ownership");
    modem_maintenance_snapshot_t in_flight = snapshot();
    modem_maintenance_cancel(true);
    modem_maintenance_snapshot_t cancelled = snapshot();
    check(cancelled.step == in_flight.step &&
              cancelled.state == in_flight.state,
          "cancel cannot reinterpret an in-flight mutation response");
    modem_maintenance_finish_command(true, false, 15u);
    check(snapshot().state == MODEM_MAINTENANCE_RESTORING &&
              modem_maintenance_prepare_next(16u, &dispatch) &&
              strcmp(dispatch.command, "SETMODE:1") == 0,
          "the completed mutation transitions into verified restoration");
}

static void test_invalid_recovery_fails_closed(void) {
    modem_maintenance_dispatch_t dispatch;
    reset_fixture();
    check(modem_maintenance_recover_record("not-a-record", false) ==
              MODEM_MAINTENANCE_RECOVERY_FAIL_CLOSED &&
              modem_maintenance_recovery_checked() &&
              snapshot().recovery_pending,
          "invalid durable state enters fail-closed recovery");
    check(modem_maintenance_prepare_next(20u, &dispatch) &&
              strcmp(dispatch.command, "SETFUN:4") == 0 &&
              s_hold_count == 1u && modem_maintenance_holds_cfun4(),
          "fail-closed recovery disables RF and guards SIM observations");

    reset_fixture();
    check(modem_maintenance_recover_record("", false) ==
              MODEM_MAINTENANCE_RECOVERY_EMPTY &&
              modem_maintenance_recovery_checked() &&
              !modem_maintenance_has_sequence(),
          "an empty recovery slot is checked without scheduling work");
}

static void test_recovery_waits_for_in_flight_command(void) {
    modem_maintenance_dispatch_t dispatch;
    reset_fixture();
    check(modem_maintenance_start_band_test(3u) &&
              modem_maintenance_prepare_next(30u, &dispatch) &&
              strcmp(dispatch.command, "GETMODE") == 0,
          "recovery deferral fixture has a maintenance command in flight");
    modem_maintenance_snapshot_t before = snapshot();

    check(modem_maintenance_recover_record("B0,5,27,123", true) ==
              MODEM_MAINTENANCE_RECOVERY_RESTORE_STARTED,
          "valid recovery state is accepted while a command is in flight");
    modem_maintenance_snapshot_t deferred = snapshot();
    check(deferred.step == before.step && deferred.state == before.state &&
              deferred.recovery_pending,
          "recovery preserves the in-flight phase until its final arrives");
}

int main(void) {
    test_scan_success_and_call_cancel();
    test_band_record_precedes_mutation_and_cancel_waits();
    test_invalid_recovery_fails_closed();
    test_recovery_waits_for_in_flight_command();

    if (s_failures != 0) {
        fprintf(stderr, "%d modem maintenance test(s) failed\n", s_failures);
        return 1;
    }
    puts("modem maintenance tests passed");
    return 0;
}

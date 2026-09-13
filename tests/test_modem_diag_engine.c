#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "services/modem_diag_engine.h"

static int s_failures;
static unsigned s_finish_calls;
static bool s_finish_result = true;

static void check(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        s_failures++;
    }
}

static modem_diag_line_result_t parse_band(
    const char *line, modem_diag_snapshot_t *shadow) {
    char *end = NULL;
    unsigned long value = strtoul(line + 4, &end, 10);
    if (end == line + 4 || *end != '\0' || value > UINT16_MAX) {
        return MODEM_DIAG_LINE_INVALID;
    }
    shadow->serving.band = (uint16_t)value;
    shadow->group[MODEM_DIAG_GROUP_SERVING].present_fields |=
        MODEM_DIAG_SERVING_BAND;
    return MODEM_DIAG_LINE_ACCEPT;
}

static modem_diag_line_result_t parse_operator(
    const char *line, modem_diag_snapshot_t *shadow) {
    const char *value = line + 4;
    if (*value == '\0') {
        return MODEM_DIAG_LINE_INVALID;
    }
    snprintf(shadow->serving.operator_name,
             sizeof(shadow->serving.operator_name), "%s", value);
    shadow->group[MODEM_DIAG_GROUP_SERVING].present_fields |=
        MODEM_DIAG_SERVING_OPERATOR;
    return MODEM_DIAG_LINE_ACCEPT;
}

static modem_diag_line_result_t parse_identity(
    const char *line, modem_diag_snapshot_t *shadow) {
    if (*line == '\0') {
        return MODEM_DIAG_LINE_IGNORE;
    }
    snprintf(shadow->identity.model, sizeof(shadow->identity.model), "%s",
             line);
    shadow->group[MODEM_DIAG_GROUP_IDENTITY].present_fields |=
        MODEM_DIAG_ID_MODEL;
    return MODEM_DIAG_LINE_ACCEPT;
}

static modem_diag_line_result_t parse_optional(
    const char *line, modem_diag_snapshot_t *shadow) {
    (void)shadow;
    if (strcmp(line, "+OPTIONAL: bad") == 0 ||
        strcmp(line, "+STRICT: bad") == 0) {
        return MODEM_DIAG_LINE_INVALID;
    }
    return MODEM_DIAG_LINE_IGNORE;
}

static modem_diag_line_result_t parse_packet_attach(
    const char *line, modem_diag_snapshot_t *shadow) {
    if (strcmp(line, "+ATT: 1") != 0) {
        return MODEM_DIAG_LINE_INVALID;
    }
    shadow->packet.attached = 1u;
    shadow->group[MODEM_DIAG_GROUP_PACKET].present_fields |=
        MODEM_DIAG_PACKET_ATTACH;
    return MODEM_DIAG_LINE_ACCEPT;
}

static bool finish_group(modem_diag_group_t group,
                         modem_diag_snapshot_t *shadow) {
    (void)group;
    (void)shadow;
    s_finish_calls++;
    return s_finish_result;
}

static const modem_diag_query_t s_queries[] = {
    {
        .group = MODEM_DIAG_GROUP_SERVING,
        .cmd = "AT+A",
        .timeout_ms = 1000u,
        .prefix = "+A: ",
        .minimum_lines = 1u,
        .parse = parse_band,
    },
    {
        .group = MODEM_DIAG_GROUP_SERVING,
        .cmd = "AT+B",
        .timeout_ms = 2000u,
        .prefix = "+B: ",
        .minimum_lines = 1u,
        .parse = parse_operator,
    },
    {
        .group = MODEM_DIAG_GROUP_IDENTITY,
        .cmd = "AT+ID",
        .timeout_ms = 3000u,
        .prefix = NULL,
        .minimum_lines = 1u,
        .parse = parse_identity,
    },
    {
        .group = MODEM_DIAG_GROUP_REGISTRATION,
        .cmd = "AT+OPTIONAL",
        .timeout_ms = 4000u,
        .prefix = "+OPTIONAL: ",
        .minimum_lines = 1u,
        .optional = true,
        .isolate_malformed = true,
        .parse = parse_optional,
    },
    {
        .group = MODEM_DIAG_GROUP_VOICE,
        .cmd = "AT+STRICT",
        .timeout_ms = 4000u,
        .prefix = "+STRICT: ",
        .minimum_lines = 1u,
        .optional = true,
        .parse = parse_optional,
    },
};

static void init_engine(modem_diag_engine_t *engine) {
    s_finish_calls = 0u;
    s_finish_result = true;
    modem_diag_engine_init(engine, s_queries,
                           (uint8_t)(sizeof(s_queries) / sizeof(s_queries[0])),
                           finish_group, true);
}

static void test_support_and_subscription(void) {
    modem_diag_engine_t engine;
    modem_diag_snapshot_t snapshot;
    init_engine(&engine);

    check(modem_diag_engine_group_supported(
              &engine, MODEM_DIAG_GROUP_SERVING) &&
              modem_diag_engine_group_supported(
                  &engine, MODEM_DIAG_GROUP_IDENTITY) &&
              !modem_diag_engine_group_supported(
                  &engine, MODEM_DIAG_GROUP_TUNER),
          "support comes from the vendor query table");
    modem_diag_engine_mark_unsupported(&engine, MODEM_DIAG_GROUP_TUNER);
    modem_diag_engine_copy_snapshot(&engine, &snapshot);
    check(snapshot.backend_available &&
              snapshot.group[MODEM_DIAG_GROUP_SERVING].state ==
                  MODEM_DIAG_STATE_UNKNOWN &&
              snapshot.group[MODEM_DIAG_GROUP_TUNER].state ==
                  MODEM_DIAG_STATE_UNSUPPORTED &&
              snapshot.group[MODEM_DIAG_GROUP_TUNER].last_error ==
                  MODEM_DIAG_ERROR_UNAVAILABLE,
          "initial and unsupported states are published");

    modem_diag_engine_set_subscription(&engine, MODEM_DIAG_GROUP_SERVING, 7u);
    check(modem_diag_engine_same_subscription(
              &engine, MODEM_DIAG_GROUP_SERVING, 7u) &&
              !modem_diag_engine_same_subscription(
                  &engine, MODEM_DIAG_GROUP_SERVING, 8u),
          "subscription identity includes generation");
    check(modem_diag_engine_request(&engine, 100u) &&
              modem_diag_engine_request(&engine, 101u),
          "a duplicate request coalesces successfully");
    modem_diag_engine_copy_snapshot(&engine, &snapshot);
    check(snapshot.scheduler.admissions == 1u &&
              snapshot.scheduler.coalesced == 1u &&
              snapshot.scheduler.pending &&
              snapshot.group[MODEM_DIAG_GROUP_SERVING].state ==
                  MODEM_DIAG_STATE_PENDING &&
              snapshot.group[MODEM_DIAG_GROUP_SERVING].last_attempt_ms == 100u,
          "admission and coalescing metadata match the old scheduler");
}

static void test_atomic_group_and_stale_retention(void) {
    modem_diag_engine_t engine;
    modem_diag_snapshot_t snapshot;
    modem_diag_dispatch_t dispatch;
    init_engine(&engine);
    modem_diag_engine_set_subscription(&engine, MODEM_DIAG_GROUP_SERVING, 1u);
    check(modem_diag_engine_request(&engine, 10u) &&
              modem_diag_engine_prepare_next(&engine, 20u, &dispatch) &&
              strcmp(dispatch.command, "AT+A") == 0 &&
              dispatch.timeout_ms == 1000u,
          "first group command is prepared");
    check(!modem_diag_engine_parse_line(&engine, "+B: WRONG", false) &&
              modem_diag_engine_parse_line(&engine, "+A: 5", false),
          "only the active query prefix is consumed");
    modem_diag_engine_copy_snapshot(&engine, &snapshot);
    check(snapshot.serving.band == 0u && snapshot.scheduler.completed == 0u,
          "partial query data remains in the shadow");

    modem_diag_engine_finish_command(&engine, true, false, 30u);
    modem_diag_engine_copy_snapshot(&engine, &snapshot);
    check(snapshot.scheduler.pending &&
              snapshot.scheduler.active_group == MODEM_DIAG_GROUP_SERVING &&
              snapshot.scheduler.active_query == 1u,
          "a multi-command group yields between commands");
    check(modem_diag_engine_prepare_next(&engine, 40u, &dispatch) &&
              strcmp(dispatch.command, "AT+B") == 0 &&
              dispatch.timeout_ms == 2000u &&
              modem_diag_engine_parse_line(&engine, "+B: Carrier", false),
          "second group command is prepared and parsed");
    modem_diag_engine_finish_command(&engine, true, false, 55u);
    modem_diag_engine_copy_snapshot(&engine, &snapshot);
    check(snapshot.serving.band == 5u &&
              strcmp(snapshot.serving.operator_name, "Carrier") == 0 &&
              snapshot.group[MODEM_DIAG_GROUP_SERVING].present_fields ==
                  (MODEM_DIAG_SERVING_BAND | MODEM_DIAG_SERVING_OPERATOR) &&
              snapshot.group[MODEM_DIAG_GROUP_SERVING].state ==
                  MODEM_DIAG_STATE_FRESH &&
              snapshot.group[MODEM_DIAG_GROUP_SERVING].sequence == 1u &&
              snapshot.scheduler.completed == 1u &&
              snapshot.scheduler.max_command_latency_ms == 15u &&
              s_finish_calls == 1u,
          "the complete group publishes atomically");

    check(modem_diag_engine_request(&engine, 60u) &&
              modem_diag_engine_prepare_next(&engine, 61u, &dispatch) &&
              modem_diag_engine_parse_line(&engine, "+A: bad", false),
          "malformed refresh is consumed and classified");
    modem_diag_engine_finish_command(&engine, true, false, 62u);
    modem_diag_engine_copy_snapshot(&engine, &snapshot);
    check(snapshot.serving.band == 5u &&
              strcmp(snapshot.serving.operator_name, "Carrier") == 0 &&
              snapshot.group[MODEM_DIAG_GROUP_SERVING].state ==
                  MODEM_DIAG_STATE_STALE &&
              snapshot.group[MODEM_DIAG_GROUP_SERVING].last_error ==
                  MODEM_DIAG_ERROR_MALFORMED &&
              snapshot.group[MODEM_DIAG_GROUP_SERVING].sequence == 1u &&
              snapshot.group[MODEM_DIAG_GROUP_SERVING].consecutive_failures ==
                  1u &&
              snapshot.scheduler.malformed_lines == 1u,
          "failed refresh retains the last atomic payload as stale");
}

static void test_unprefixed_protection_and_generation(void) {
    modem_diag_engine_t engine;
    modem_diag_snapshot_t snapshot;
    modem_diag_dispatch_t dispatch;
    init_engine(&engine);
    modem_diag_engine_set_subscription(&engine, MODEM_DIAG_GROUP_IDENTITY, 9u);
    check(modem_diag_engine_request(&engine, 1u) &&
              modem_diag_engine_prepare_next(&engine, 2u, &dispatch) &&
              strcmp(dispatch.command, "AT+ID") == 0,
          "unprefixed identity command starts");
    check(!modem_diag_engine_parse_line(&engine, "RING", true) &&
              modem_diag_engine_parse_line(&engine, "LE910C1", false),
          "protected URCs cannot be swallowed by an unprefixed parser");
    modem_diag_engine_set_subscription(&engine, MODEM_DIAG_GROUP_IDENTITY, 10u);
    modem_diag_engine_finish_command(&engine, true, false, 5u);
    modem_diag_engine_copy_snapshot(&engine, &snapshot);
    check(snapshot.identity.model[0] == '\0' &&
              snapshot.scheduler.active_group == MODEM_DIAG_GROUP_NONE &&
              snapshot.scheduler.completed == 0u &&
              snapshot.scheduler.generation == 10u,
          "a generation change prevents a late command from publishing");
}

static void test_optional_error_timeout_and_finish_failure(void) {
    modem_diag_engine_t engine;
    modem_diag_snapshot_t snapshot;
    modem_diag_dispatch_t dispatch;
    init_engine(&engine);
    modem_diag_engine_set_subscription(&engine,
                                       MODEM_DIAG_GROUP_REGISTRATION, 3u);
    check(modem_diag_engine_request(&engine, 10u) &&
              modem_diag_engine_prepare_next(&engine, 11u, &dispatch),
          "optional query starts");
    modem_diag_engine_finish_command(&engine, false, false, 12u);
    modem_diag_engine_copy_snapshot(&engine, &snapshot);
    check(snapshot.group[MODEM_DIAG_GROUP_REGISTRATION].state ==
                  MODEM_DIAG_STATE_FRESH &&
              snapshot.group[MODEM_DIAG_GROUP_REGISTRATION].sequence == 1u &&
              snapshot.scheduler.completed == 1u &&
              snapshot.scheduler.command_failures == 0u,
          "explicit ERROR skips an optional query");

    check(modem_diag_engine_request(&engine, 20u) &&
              modem_diag_engine_prepare_next(&engine, 21u, &dispatch),
          "optional query can be retried");
    modem_diag_engine_finish_command(&engine, false, true, 25u);
    modem_diag_engine_copy_snapshot(&engine, &snapshot);
    check(snapshot.group[MODEM_DIAG_GROUP_REGISTRATION].state ==
                  MODEM_DIAG_STATE_STALE &&
              snapshot.group[MODEM_DIAG_GROUP_REGISTRATION].last_error ==
                  MODEM_DIAG_ERROR_TIMEOUT &&
              snapshot.scheduler.command_timeouts == 1u,
          "an optional timeout fails instead of masquerading as unsupported");

    modem_diag_engine_set_subscription(&engine, MODEM_DIAG_GROUP_IDENTITY, 4u);
    check(modem_diag_engine_request(&engine, 30u) &&
              modem_diag_engine_prepare_next(&engine, 31u, &dispatch) &&
              modem_diag_engine_parse_line(&engine, "LE910C1", false),
          "identity group reaches its finish gate");
    s_finish_result = false;
    modem_diag_engine_finish_command(&engine, true, false, 32u);
    modem_diag_engine_copy_snapshot(&engine, &snapshot);
    check(snapshot.identity.model[0] == '\0' &&
              snapshot.group[MODEM_DIAG_GROUP_IDENTITY].state ==
                  MODEM_DIAG_STATE_ERROR &&
              snapshot.group[MODEM_DIAG_GROUP_IDENTITY].last_error ==
                  MODEM_DIAG_ERROR_MALFORMED &&
              snapshot.scheduler.malformed_lines == 1u,
          "group validation failure cannot publish its shadow");
}

static void test_optional_malformed_isolation(void) {
    static const modem_diag_query_t packet_queries[] = {
        {
            .group = MODEM_DIAG_GROUP_PACKET,
            .cmd = "AT+ATT",
            .timeout_ms = 1000u,
            .prefix = "+ATT: ",
            .minimum_lines = 1u,
            .parse = parse_packet_attach,
        },
        {
            .group = MODEM_DIAG_GROUP_PACKET,
            .cmd = "AT+DETAIL",
            .timeout_ms = 1000u,
            .prefix = "+OPTIONAL: ",
            .minimum_lines = 0u,
            .optional = true,
            .isolate_malformed = true,
            .parse = parse_optional,
        },
    };
    modem_diag_engine_t engine;
    modem_diag_snapshot_t snapshot;
    modem_diag_dispatch_t dispatch;
    s_finish_calls = 0u;
    s_finish_result = true;
    modem_diag_engine_init(
        &engine, packet_queries,
        (uint8_t)(sizeof(packet_queries) / sizeof(packet_queries[0])),
        finish_group, true);
    modem_diag_engine_set_subscription(&engine, MODEM_DIAG_GROUP_PACKET, 1u);
    check(modem_diag_engine_request(&engine, 10u) &&
              modem_diag_engine_prepare_next(&engine, 11u, &dispatch) &&
              strcmp(dispatch.command, "AT+ATT") == 0 &&
              modem_diag_engine_parse_line(&engine, "+ATT: 1", false),
          "mandatory packet response is staged before optional detail");
    modem_diag_engine_finish_command(&engine, true, false, 12u);
    check(modem_diag_engine_prepare_next(&engine, 13u, &dispatch) &&
              strcmp(dispatch.command, "AT+DETAIL") == 0 &&
              modem_diag_engine_parse_line(&engine, "+OPTIONAL: bad", false),
          "isolated optional malformed response is consumed");
    modem_diag_engine_finish_command(&engine, true, false, 14u);
    modem_diag_engine_copy_snapshot(&engine, &snapshot);
    check(snapshot.group[MODEM_DIAG_GROUP_PACKET].state ==
                  MODEM_DIAG_STATE_FRESH &&
              snapshot.group[MODEM_DIAG_GROUP_PACKET].sequence == 1u &&
              snapshot.packet.attached == 1u &&
              (snapshot.group[MODEM_DIAG_GROUP_PACKET].present_fields &
               MODEM_DIAG_PACKET_ATTACH) != 0u &&
              snapshot.scheduler.completed == 1u &&
              snapshot.scheduler.command_failures == 0u &&
              snapshot.scheduler.malformed_lines == 1u,
          "isolated malformed detail preserves mandatory packet data");

    init_engine(&engine);
    modem_diag_engine_set_subscription(&engine, MODEM_DIAG_GROUP_VOICE, 2u);
    check(modem_diag_engine_request(&engine, 20u) &&
              modem_diag_engine_prepare_next(&engine, 21u, &dispatch) &&
              strcmp(dispatch.command, "AT+STRICT") == 0 &&
              modem_diag_engine_parse_line(&engine, "+STRICT: bad", false),
          "strict optional malformed response is consumed");
    modem_diag_engine_finish_command(&engine, true, false, 22u);
    modem_diag_engine_copy_snapshot(&engine, &snapshot);
    check(snapshot.group[MODEM_DIAG_GROUP_VOICE].state ==
                  MODEM_DIAG_STATE_ERROR &&
              snapshot.group[MODEM_DIAG_GROUP_VOICE].last_error ==
                  MODEM_DIAG_ERROR_MALFORMED &&
              snapshot.scheduler.completed == 0u &&
              snapshot.scheduler.command_failures == 0u &&
              snapshot.scheduler.malformed_lines == 1u,
          "malformed isolation remains opt-in instead of weakening all optionals");
}

static void test_cancel_and_invalidate(void) {
    modem_diag_engine_t engine;
    modem_diag_snapshot_t snapshot;
    modem_diag_dispatch_t dispatch;
    init_engine(&engine);
    modem_diag_engine_set_subscription(&engine, MODEM_DIAG_GROUP_SERVING, 5u);
    check(modem_diag_engine_request(&engine, 100u),
          "cancellation fixture is admitted");
    modem_diag_engine_cancel_active(&engine, false, 101u);
    modem_diag_engine_cancel_active(&engine, false, 102u);
    modem_diag_engine_copy_snapshot(&engine, &snapshot);
    check(snapshot.scheduler.cancelled == 1u &&
              snapshot.group[MODEM_DIAG_GROUP_SERVING].state ==
                  MODEM_DIAG_STATE_ERROR &&
              snapshot.group[MODEM_DIAG_GROUP_SERVING].last_error ==
                  MODEM_DIAG_ERROR_CANCELLED &&
              !snapshot.scheduler.pending,
          "only real pending work counts as a cancellation");

    check(modem_diag_engine_request(&engine, 110u) &&
              modem_diag_engine_prepare_next(&engine, 111u, &dispatch),
          "active cancellation fixture starts");
    modem_diag_engine_cancel_active(&engine, true, 112u);
    modem_diag_engine_finish_command(&engine, true, false, 120u);
    modem_diag_engine_copy_snapshot(&engine, &snapshot);
    check(snapshot.scheduler.cancelled == 2u &&
              snapshot.scheduler.max_command_latency_ms == 9u &&
              snapshot.scheduler.completed == 0u,
          "late final after cancellation updates latency but cannot publish");

    modem_diag_engine_mark_unsupported(&engine, MODEM_DIAG_GROUP_TUNER);
    modem_diag_engine_invalidate_all(&engine, 200u);
    modem_diag_engine_copy_snapshot(&engine, &snapshot);
    check(snapshot.group[MODEM_DIAG_GROUP_TUNER].state ==
                  MODEM_DIAG_STATE_UNSUPPORTED &&
              snapshot.group[MODEM_DIAG_GROUP_SERVING].state ==
                  MODEM_DIAG_STATE_UNKNOWN &&
              snapshot.group[MODEM_DIAG_GROUP_SERVING].last_error ==
                  MODEM_DIAG_ERROR_UNAVAILABLE &&
              snapshot.updated_ms == 200u,
          "global invalidation preserves unsupported groups");
}

int main(void) {
    test_support_and_subscription();
    test_atomic_group_and_stale_retention();
    test_unprefixed_protection_and_generation();
    test_optional_error_timeout_and_finish_failure();
    test_optional_malformed_isolation();
    test_cancel_and_invalidate();

    if (s_failures != 0) {
        fprintf(stderr, "%d modem diagnostic engine test(s) failed\n",
                s_failures);
        return 1;
    }
    puts("modem diagnostic engine tests passed");
    return 0;
}

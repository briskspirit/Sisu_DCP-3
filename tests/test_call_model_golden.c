/* End-to-end golden traces for the public CLCC call-model contract.
 *
 * The scenarios are derived from the locked projection/authority rules, not
 * from call_model_t internals. They drive requests, dispatch guards, command
 * outcomes, normalized events, CLCC snapshots, ticks, and release output only
 * through the public API used by modem_service.c. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "services/modem_call_model.h"

static const call_timing_t k_timing = {
    .clcc_keepalive_ms = 4000u,
    .clcc_confirm_ms = 300u,
    .leg_limbo_ms = 8000u,
    .txn_policy_ms = 40000u,
    .txn_abandon_ms = 120000u,
};

static int s_failures;

typedef struct {
    modem_call_state_t call_state;
    uint8_t active_call_id;
    bool call_on_hold;
    bool second_call_held;
    bool waiting_call;
    bool ring_active;
    bool caller_id_withheld;
    bool incoming_diverted;
    const char *incoming_number; /* NULL means the expected value is empty. */
    modem_call_result_t last_call_result;
    modem_call_result_t second_call_result;
    bool check_last_result;
    bool check_second_result;
} golden_t;

static void fail_u(const char *tag, const char *field,
                   unsigned got, unsigned expected) {
    fprintf(stderr, "FAIL [%s] %s (got %u, expected %u)\n",
            tag, field, got, expected);
    s_failures++;
}

static void expect_true(bool condition, const char *tag) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", tag);
        s_failures++;
    }
}

static void check_projection(call_model_t *model, const golden_t *golden,
                             const char *tag) {
    call_projection_t actual;
    call_model_project(model, &actual);

#define CHECK_U(field) do { \
    if ((unsigned)actual.field != (unsigned)golden->field) { \
        fail_u(tag, #field, (unsigned)actual.field, (unsigned)golden->field); \
    } \
} while (0)
    CHECK_U(call_state);
    CHECK_U(active_call_id);
    CHECK_U(call_on_hold);
    CHECK_U(second_call_held);
    CHECK_U(waiting_call);
    CHECK_U(ring_active);
    CHECK_U(caller_id_withheld);
    CHECK_U(incoming_diverted);
#undef CHECK_U

    const char *expected_number = golden->incoming_number != NULL
                                ? golden->incoming_number : "";
    if (strcmp(actual.incoming_number, expected_number) != 0) {
        fprintf(stderr, "FAIL [%s] incoming_number (got \"%s\", expected \"%s\")\n",
                tag, actual.incoming_number, expected_number);
        s_failures++;
    }
    if (golden->check_last_result &&
        actual.last_call_result != golden->last_call_result) {
        fail_u(tag, "last_call_result", (unsigned)actual.last_call_result,
               (unsigned)golden->last_call_result);
    }
    if (golden->check_second_result &&
        actual.second_call_result != golden->second_call_result) {
        fail_u(tag, "second_call_result", (unsigned)actual.second_call_result,
               (unsigned)golden->second_call_result);
    }
}

static void init_model(call_model_t *model) {
    call_model_init(model, &k_timing);
    call_model_set_now(model, 1000u);
}

static uint32_t dispatch_ok(call_model_t *model, call_txn_kind_t kind,
                            uint8_t target_id, bool second_mo,
                            const char *tag) {
    uint32_t token = call_model_request(model, kind, target_id, second_mo);
    expect_true(token != 0u, tag);
    if (token == 0u) return 0u;

    call_dispatch_guard_t guard = call_model_txn_dispatch_guard(model, token);
    expect_true(guard == CALL_DISPATCH_SEND, tag);
    if (guard != CALL_DISPATCH_SEND) return token;

    call_model_txn_dispatched(model, token);
    modem_cmd_result_t result = { .status = CMD_OK };
    call_model_txn_command_result(model, token, result);
    return token;
}

static void bring_up_mo_active(call_model_t *model, uint8_t id) {
    (void)dispatch_ok(model, CALL_TXN_DIAL, 0u, false,
                      "bring-up DIAL dispatches");
    call_model_on_event(model, id, true, CALL_LEG_DIALING, CALL_DIR_MO);
    call_model_on_event(model, id, true, CALL_LEG_ACTIVE, CALL_DIR_MO);
    call_model_tick(model);
}

static void clcc_row(call_model_t *model, uint8_t id, call_direction_t dir,
                     call_leg_state_t state, const char *number) {
    call_model_clcc_row(model, id, dir, CALL_MODE_VOICE, 0u, state, number);
}

/* A primary MO call survives a bare final and one missing snapshot. Only the
 * second matching clean absence removes it and publishes the terminal cause. */
static void scenario_single_mo_remote_hangup(void) {
    call_model_t model;
    init_model(&model);
    bring_up_mo_active(&model, 1u);
    check_projection(&model, &(golden_t){
        .call_state = MODEM_CALL_ACTIVE, .active_call_id = 1u,
        .last_call_result = MODEM_CALL_RESULT_CONNECTED,
        .second_call_result = MODEM_CALL_RESULT_NONE,
        .check_last_result = true, .check_second_result = true,
    }, "single MO connected");

    call_model_set_now(&model, 2000u);
    call_model_on_bare_final(&model, MODEM_CALL_RESULT_NO_CARRIER, false);
    call_model_tick(&model);
    check_projection(&model, &(golden_t){
        .call_state = MODEM_CALL_ACTIVE, .active_call_id = 1u,
        .last_call_result = MODEM_CALL_RESULT_CONNECTED,
        .second_call_result = MODEM_CALL_RESULT_NONE,
        .check_last_result = true, .check_second_result = true,
    }, "bare final is non-authoritative");

    call_model_set_now(&model, 2100u);
    call_model_clcc_begin(&model, 0u);
    call_model_clcc_ok(&model, 0u);
    call_model_tick(&model);
    check_projection(&model, &(golden_t){
        .call_state = MODEM_CALL_ACTIVE, .active_call_id = 1u,
        .last_call_result = MODEM_CALL_RESULT_CONNECTED,
        .second_call_result = MODEM_CALL_RESULT_NONE,
        .check_last_result = true, .check_second_result = true,
    }, "first clean absence retains the published role");

    call_model_set_now(&model, 2400u);
    call_model_clcc_begin(&model, 0u);
    call_model_clcc_ok(&model, 0u);
    call_model_tick(&model);
    check_projection(&model, &(golden_t){
        .call_state = MODEM_CALL_IDLE,
        .last_call_result = MODEM_CALL_RESULT_NO_CARRIER,
        .second_call_result = MODEM_CALL_RESULT_NONE,
        .check_last_result = true, .check_second_result = true,
    }, "second clean absence confirms remote hangup");
}

/* Incoming call: R2 -> R4 -> R6 -> clean local teardown. */
static void scenario_incoming_answer_local_hangup(void) {
    call_model_t model;
    init_model(&model);
    call_model_on_clip(&model, "+15551230000", 0u);
    call_model_on_ring(&model);
    call_model_on_event(&model, 2u, true, CALL_LEG_INCOMING, CALL_DIR_MT);
    call_model_tick(&model);
    check_projection(&model, &(golden_t){
        .call_state = MODEM_CALL_RINGING, .ring_active = true,
        .incoming_number = "+15551230000",
        .last_call_result = MODEM_CALL_RESULT_NONE,
        .second_call_result = MODEM_CALL_RESULT_NONE,
        .check_last_result = true, .check_second_result = true,
    }, "incoming call rings");

    (void)dispatch_ok(&model, CALL_TXN_ANSWER, 2u, false,
                      "incoming ANSWER dispatches");
    call_model_tick(&model);
    check_projection(&model, &(golden_t){
        .call_state = MODEM_CALL_ANSWERING,
        .incoming_number = "+15551230000",
        .last_call_result = MODEM_CALL_RESULT_NONE,
        .second_call_result = MODEM_CALL_RESULT_NONE,
        .check_last_result = true, .check_second_result = true,
    }, "incoming call is answering");

    call_model_on_event(&model, 2u, true, CALL_LEG_ACTIVE, CALL_DIR_MT);
    call_model_tick(&model);
    check_projection(&model, &(golden_t){
        .call_state = MODEM_CALL_ACTIVE, .active_call_id = 2u,
        .incoming_number = "+15551230000",
        .last_call_result = MODEM_CALL_RESULT_CONNECTED,
        .second_call_result = MODEM_CALL_RESULT_NONE,
        .check_last_result = true, .check_second_result = true,
    }, "answered incoming call is active");

    (void)dispatch_ok(&model, CALL_TXN_HANGUP, 2u, false,
                      "local HANGUP dispatches");
    call_model_on_event(&model, 2u, true, CALL_LEG_RELEASING, CALL_DIR_MT);
    call_model_tick(&model);
    check_projection(&model, &(golden_t){
        .call_state = MODEM_CALL_IDLE,
        .last_call_result = MODEM_CALL_RESULT_NO_CARRIER,
        .second_call_result = MODEM_CALL_RESULT_NONE,
        .check_last_result = true, .check_second_result = true,
    }, "bench-recorded: clean local hangup projects legacy NO_CARRIER");
}

/* A sole held call remains MODEM_CALL_ACTIVE and uses call_on_hold. */
static void scenario_hold_retrieve(void) {
    call_model_t model;
    init_model(&model);
    bring_up_mo_active(&model, 1u);

    (void)dispatch_ok(&model, CALL_TXN_HOLD, 1u, false,
                      "HOLD dispatches");
    call_model_on_event(&model, 1u, true, CALL_LEG_HELD, CALL_DIR_MO);
    call_model_tick(&model);
    check_projection(&model, &(golden_t){
        .call_state = MODEM_CALL_ACTIVE, .active_call_id = 1u,
        .call_on_hold = true,
        .last_call_result = MODEM_CALL_RESULT_CONNECTED,
        .second_call_result = MODEM_CALL_RESULT_NONE,
        .check_last_result = true, .check_second_result = true,
    }, "sole held call projects through R7");

    (void)dispatch_ok(&model, CALL_TXN_HOLD, 1u, false,
                      "retrieve dispatches");
    call_model_on_event(&model, 1u, true, CALL_LEG_ACTIVE, CALL_DIR_MO);
    call_model_tick(&model);
    check_projection(&model, &(golden_t){
        .call_state = MODEM_CALL_ACTIVE, .active_call_id = 1u,
        .last_call_result = MODEM_CALL_RESULT_CONNECTED,
        .second_call_result = MODEM_CALL_RESULT_NONE,
        .check_last_result = true, .check_second_result = true,
    }, "retrieve returns to R6");
}

/* Answer a waiting call and then swap the active/held roles. */
static void scenario_two_call_swap(void) {
    call_model_t model;
    init_model(&model);
    bring_up_mo_active(&model, 1u);
    call_model_on_clip(&model, "+15552220000", 0u);
    call_model_on_event(&model, 2u, true, CALL_LEG_WAITING, CALL_DIR_MT);
    call_model_tick(&model);

    (void)dispatch_ok(&model, CALL_TXN_WAIT_ANSWER, 2u, false,
                      "waiting ANSWER dispatches");
    call_model_on_event(&model, 1u, true, CALL_LEG_HELD, CALL_DIR_MO);
    call_model_on_event(&model, 2u, true, CALL_LEG_ACTIVE, CALL_DIR_MT);
    call_model_tick(&model);
    check_projection(&model, &(golden_t){
        .call_state = MODEM_CALL_ACTIVE, .active_call_id = 2u,
        .second_call_held = true,
        .incoming_number = "+15552220000",
        .last_call_result = MODEM_CALL_RESULT_CONNECTED,
        .second_call_result = MODEM_CALL_RESULT_NONE,
        .check_last_result = true, .check_second_result = true,
    }, "waiting answer yields active plus held");

    (void)dispatch_ok(&model, CALL_TXN_SWAP, 0u, false,
                      "SWAP dispatches");
    call_model_on_event(&model, 2u, true, CALL_LEG_HELD, CALL_DIR_MT);
    call_model_on_event(&model, 1u, true, CALL_LEG_ACTIVE, CALL_DIR_MO);
    call_model_tick(&model);
    check_projection(&model, &(golden_t){
        .call_state = MODEM_CALL_ACTIVE, .active_call_id = 1u,
        .second_call_held = true,
        .incoming_number = "+15552220000",
        .last_call_result = MODEM_CALL_RESULT_CONNECTED,
        .second_call_result = MODEM_CALL_RESULT_NONE,
        .check_last_result = true, .check_second_result = true,
    }, "swap reverses active and held roles");
}

/* Corrected #3: removing A while second-MO B is still dialing cannot publish
 * an IDLE gap. B remains a setup projection with no confirmed active id. */
static void scenario_active_hangs_up_while_second_dials(void) {
    call_model_t model;
    init_model(&model);
    bring_up_mo_active(&model, 1u);

    (void)dispatch_ok(&model, CALL_TXN_DIAL, 0u, true,
                      "second-MO DIAL dispatches");
    call_model_on_event(&model, 2u, true, CALL_LEG_DIALING, CALL_DIR_MO);
    call_model_tick(&model);
    check_projection(&model, &(golden_t){
        .call_state = MODEM_CALL_DIALING, .active_call_id = 1u,
        .check_last_result = false, .check_second_result = false,
    }, "second-MO setup keeps A as active id");

    call_model_on_bare_final(&model, MODEM_CALL_RESULT_NO_CARRIER, false);
    call_model_tick(&model);
    check_projection(&model, &(golden_t){
        .call_state = MODEM_CALL_DIALING, .active_call_id = 1u,
    }, "ambiguous bare final removes neither leg");

    call_model_set_now(&model, 1200u);
    call_model_clcc_begin(&model, 0u);
    clcc_row(&model, 2u, CALL_DIR_MO, CALL_LEG_DIALING, "+15559990000");
    call_model_clcc_ok(&model, 0u);
    call_model_tick(&model);
    call_model_set_now(&model, 1500u);
    call_model_clcc_begin(&model, 0u);
    clcc_row(&model, 2u, CALL_DIR_MO, CALL_LEG_DIALING, "+15559990000");
    call_model_clcc_ok(&model, 0u);
    call_model_tick(&model);
    check_projection(&model, &(golden_t){
        .call_state = MODEM_CALL_DIALING,
    }, "corrected #3: B setup survives A removal without IDLE");

    call_model_on_event(&model, 2u, true, CALL_LEG_ACTIVE, CALL_DIR_MO);
    call_model_tick(&model);
    check_projection(&model, &(golden_t){
        .call_state = MODEM_CALL_ACTIVE, .active_call_id = 2u,
    }, "remaining second-MO leg can still connect");

    /* Once A is gone, connected B is the sole foreground/primary call. Its
     * later remote release belongs to the primary result latch, not the
     * completed New-call setup latch. This is the exact C4 bench ordering. */
    call_model_on_event(&model, 2u, true, CALL_LEG_RELEASING, CALL_DIR_MO);
    call_model_tick(&model);
    check_projection(&model, &(golden_t){
        .call_state = MODEM_CALL_IDLE,
        .last_call_result = MODEM_CALL_RESULT_NO_CARRIER,
        .second_call_result = MODEM_CALL_RESULT_NONE,
        .check_last_result = true, .check_second_result = true,
    }, "sole connected second-MO departure owns the primary result latch");
}

/* Corrected latch routing: an unanswered second-MO leg owns only
 * second_call_result; the surviving original owns CONNECTED in the primary latch. */
static void scenario_second_mo_failure_original_survives(void) {
    call_model_t model;
    init_model(&model);
    bring_up_mo_active(&model, 1u);

    (void)dispatch_ok(&model, CALL_TXN_DIAL, 0u, true,
                      "failing second-MO DIAL dispatches");
    call_model_on_event(&model, 1u, true, CALL_LEG_HELD, CALL_DIR_MO);
    call_model_on_event(&model, 2u, true, CALL_LEG_DIALING, CALL_DIR_MO);
    call_model_tick(&model);

    call_model_on_event(&model, 2u, true, CALL_LEG_RELEASING, CALL_DIR_MO);
    call_model_on_event(&model, 1u, true, CALL_LEG_ACTIVE, CALL_DIR_MO);
    call_model_tick(&model);
    check_projection(&model, &(golden_t){
        .call_state = MODEM_CALL_ACTIVE, .active_call_id = 1u,
        .last_call_result = MODEM_CALL_RESULT_CONNECTED,
        .second_call_result = MODEM_CALL_RESULT_NO_ANSWER,
        .check_last_result = true, .check_second_result = true,
    }, "second-MO failure cannot steal the primary result latch");
}

/* Mandatory R9 corner: active + held + waiting never sets call_on_hold. */
static void scenario_three_leg_hold_flag(void) {
    call_model_t model;
    init_model(&model);
    bring_up_mo_active(&model, 1u);
    call_model_on_clip(&model, "+15552220000", 0u);
    call_model_on_event(&model, 2u, true, CALL_LEG_WAITING, CALL_DIR_MT);
    call_model_tick(&model);
    (void)dispatch_ok(&model, CALL_TXN_WAIT_ANSWER, 2u, false,
                      "three-leg setup ANSWER dispatches");
    call_model_on_event(&model, 1u, true, CALL_LEG_HELD, CALL_DIR_MO);
    call_model_on_event(&model, 2u, true, CALL_LEG_ACTIVE, CALL_DIR_MT);
    call_model_tick(&model);
    check_projection(&model, &(golden_t){
        .call_state = MODEM_CALL_ACTIVE, .active_call_id = 2u,
        .second_call_held = true,
        .incoming_number = "+15552220000",
        .last_call_result = MODEM_CALL_RESULT_CONNECTED,
        .check_last_result = true,
    }, "R8 bridge remains up with two calls");

    call_model_on_clip(&model, "+15553330000", 0u);
    call_model_on_event(&model, 3u, true, CALL_LEG_WAITING, CALL_DIR_MT);
    call_model_tick(&model);
    check_projection(&model, &(golden_t){
        .call_state = MODEM_CALL_ACTIVE, .active_call_id = 2u,
        .second_call_held = true, .waiting_call = true, .ring_active = true,
        .incoming_number = "+15553330000",
    }, "mandatory R9: three legs keep call_on_hold false");
}

/* Mandatory atomic confirmation: snapshot two removes A and reveals B in one
 * commit, so the public projection never passes through IDLE. */
static void scenario_confirmation_reveals_new_leg(void) {
    call_model_t model;
    init_model(&model);
    bring_up_mo_active(&model, 1u);
    check_projection(&model, &(golden_t){
        .call_state = MODEM_CALL_ACTIVE, .active_call_id = 1u,
        .last_call_result = MODEM_CALL_RESULT_CONNECTED,
        .check_last_result = true,
    }, "atomic replace precondition publishes A's active role");

    call_model_set_now(&model, 1100u);
    call_model_clcc_begin(&model, 0u);
    call_model_clcc_ok(&model, 0u);
    call_model_tick(&model);
    check_projection(&model, &(golden_t){
        .call_state = MODEM_CALL_ACTIVE, .active_call_id = 1u,
        .last_call_result = MODEM_CALL_RESULT_CONNECTED,
        .check_last_result = true,
    }, "atomic replace first absence retains A");

    call_model_set_now(&model, 1400u);
    call_model_clcc_begin(&model, 0u);
    clcc_row(&model, 2u, CALL_DIR_MT, CALL_LEG_ACTIVE, "+15554440000");
    call_model_clcc_ok(&model, 0u);
    call_model_tick(&model);
    check_projection(&model, &(golden_t){
        .call_state = MODEM_CALL_ACTIVE, .active_call_id = 2u,
        .last_call_result = MODEM_CALL_RESULT_CONNECTED,
        .check_last_result = true,
    }, "mandatory atomic replace publishes B without IDLE");
}

/* Corrected #5: an ACTIVE URC with unknown direction is tentative. */
static void scenario_lost_dialing_tentative_active(void) {
    call_model_t model;
    init_model(&model);
    (void)dispatch_ok(&model, CALL_TXN_DIAL, 0u, false,
                      "lost-DIALING DIAL dispatches");
    call_model_on_event(&model, 1u, true, CALL_LEG_ACTIVE, CALL_DIR_UNKNOWN);
    call_model_tick(&model);
    check_projection(&model, &(golden_t){
        .call_state = MODEM_CALL_DIALING,
    }, "corrected #5: tentative ACTIVE remains DIALING with id zero");

    call_model_set_now(&model, 1200u);
    call_model_clcc_begin(&model, 0u);
    clcc_row(&model, 1u, CALL_DIR_MO, CALL_LEG_ACTIVE, "+15559990000");
    call_model_clcc_ok(&model, 0u);
    call_model_tick(&model);
    check_projection(&model, &(golden_t){
        .call_state = MODEM_CALL_ACTIVE, .active_call_id = 1u,
        .last_call_result = MODEM_CALL_RESULT_CONNECTED,
        .check_last_result = true,
    }, "direction-confirmed ACTIVE can connect");
}

/* Mandatory hard-abandon corner: a bound setup leg becomes RELEASING and emits
 * an id-scoped cleanup request while the surviving active leg remains visible. */
static void scenario_abandon_bound_setup_leg(void) {
    call_model_t model;
    init_model(&model);
    bring_up_mo_active(&model, 1u);
    (void)dispatch_ok(&model, CALL_TXN_DIAL, 0u, true,
                      "abandoned second-MO DIAL dispatches");
    call_model_on_event(&model, 2u, true, CALL_LEG_DIALING, CALL_DIR_MO);
    call_model_tick(&model);
    check_projection(&model, &(golden_t){
        .call_state = MODEM_CALL_DIALING, .active_call_id = 1u,
    }, "abandon precondition: second-MO is setting up");

    call_model_set_now(&model, 1000u + k_timing.txn_abandon_ms + 1u);
    call_model_tick(&model);
    check_projection(&model, &(golden_t){
        .call_state = MODEM_CALL_ACTIVE, .active_call_id = 1u,
        .last_call_result = MODEM_CALL_RESULT_CONNECTED,
        .second_call_result = MODEM_CALL_RESULT_NO_ANSWER,
        .check_last_result = true, .check_second_result = true,
    }, "mandatory abandon keeps the surviving active projection");

    call_cleanup_release_t release = {0};
    expect_true(call_model_pop_release(&model, &release),
                "abandon emits an id-scoped release");
    expect_true(release.id == 2u && release.recover_held_survivor,
                "second-MO cleanup targets B and recovers held A");

    call_model_on_event(&model, 2u, true, CALL_LEG_RELEASING, CALL_DIR_MO);
    call_model_tick(&model);
    check_projection(&model, &(golden_t){
        .call_state = MODEM_CALL_ACTIVE, .active_call_id = 1u,
        .last_call_result = MODEM_CALL_RESULT_CONNECTED,
        .second_call_result = MODEM_CALL_RESULT_NO_ANSWER,
        .check_last_result = true, .check_second_result = true,
    }, "authoritative cleanup removes only the abandoned leg");
}

int main(void) {
    scenario_single_mo_remote_hangup();
    scenario_incoming_answer_local_hangup();
    scenario_hold_retrieve();
    scenario_two_call_swap();
    scenario_active_hangs_up_while_second_dials();
    scenario_second_mo_failure_original_survives();
    scenario_three_leg_hold_flag();
    scenario_confirmation_reveals_new_leg();
    scenario_lost_dialing_tentative_active();
    scenario_abandon_bound_setup_leg();

    if (s_failures != 0) {
        fprintf(stderr, "test_call_model_golden: %d failure(s)\n", s_failures);
        return 1;
    }
    printf("test_call_model_golden: OK (10 scenarios)\n");
    return 0;
}

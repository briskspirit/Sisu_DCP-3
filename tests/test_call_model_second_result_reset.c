/* Regression test: the second_call_result reset state machine must cover
 * every enumerated reset site (§9.5 / §16.9).
 *
 * §9.5 enumerates the second_call_result -> NONE reset sites: init, fresh-dial
 * ACCEPT, fresh-dial dispatch, New-call abandon, B connects, and IDLE / full-
 * teardown / off. A broken model implemented only init / DIAL-dispatch / abandon.
 * second_call_result is ASYMMETRIC to last_call_result: `last` is RETAINED across
 * IDLE (for the call log); `second` MUST clear at session boundaries so a stale
 * 2nd-MO failure never leaks. This golden pins the two MISSING enumerated resets,
 * each PROVEN to fail on the unfixed model:
 *   (A) IDLE / full-teardown: a finished session's 2nd-MO terminal (BUSY) must NOT
 *       leak across a later unrelated incoming.
 *   (B) fresh-dial ACCEPT: a queued-but-not-yet-dispatched New-call must start
 *       clean at ACCEPT time so the live consumer (calls_app.c:742) never observes
 *       a PRIOR stale BUSY and falsely rolls back the fresh New-call (§16.7
 *       "dequeued != sent").
 *
 * TDD discipline: drive the PUBLIC API only; never hand-poke struct
 * fields to force state. Single TU — #include the .c under test directly so the
 * pure model links with no hardware stubs.
 *   cc -std=c11 -I include -I tests/stubs -Wall -Wextra \
 *      -fsanitize=address,undefined -fno-sanitize-recover=all -g -O1 \
 *      tests/test_call_model_second_result_reset.c -o /tmp/t && /tmp/t
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../src/services/modem_call_model.c"
#include "call_timing_fixture.h"

static int s_failures;
static void check(bool cond, const char *msg) {
    if (!cond) { fprintf(stderr, "FAIL: %s\n", msg); s_failures++; }
}
static void tinit(call_model_t *m) {
    call_timing_t t = call_timing_fixture();
    call_model_init(m, &t);
}
static call_txn_t *find_txn(call_model_t *m, uint32_t token) {
    for (unsigned i = 0; i < MODEM_MAX_CALL_TRANSACTIONS; i++)
        if (m->txns[i].in_use && m->txns[i].token == token) return &m->txns[i];
    return NULL;
}

/* Facet A: A active, a 2nd-MO New-call B fails BUSY (CMD_ERROR), then A departs
 * -> the model reaches IDLE. published.second_call_result MUST be NONE at IDLE
 * (while last_call_result stays NO_CARRIER — RETAINED for the call log), and MUST
 * stay NONE across a later unrelated incoming. */
static void test_facetA_idle_clears_second(void) {
    call_model_t m; tinit(&m); call_model_set_now(&m, 1000u);
    /* A connects (last_call_result -> CONNECTED via the ACTIVE URC). */
    call_model_on_event(&m, 1u, true, CALL_LEG_ACTIVE, CALL_DIR_MO);
    /* a 2nd-MO New-call B is dialed and REJECTED BUSY by the network. */
    uint32_t tokB = call_model_request(&m, CALL_TXN_DIAL, 0u, true);
    call_model_txn_dispatched(&m, tokB);
    modem_cmd_result_t busy = { .status = CMD_ERROR, .has_call_result = true,
                                .call_result = MODEM_CALL_RESULT_BUSY };
    call_model_txn_command_result(&m, tokB, busy);
    check(m.published.second_call_result == MODEM_CALL_RESULT_BUSY,
          "facetA precondition: 2nd-MO CMD_ERROR BUSY latches second_call_result=BUSY");

    /* A departs (remote release) -> primary leg RELEASING -> evicted. */
    call_model_set_now(&m, 2000u);
    call_model_on_event(&m, 1u, true, CALL_LEG_RELEASING, CALL_DIR_UNKNOWN);
    call_model_tick(&m);
    call_projection_t p; call_model_project(&m, &p);
    check(p.call_state == MODEM_CALL_IDLE, "facetA: model reaches IDLE");
    check(p.last_call_result == MODEM_CALL_RESULT_NO_CARRIER,
          "facetA: last_call_result RETAINED across IDLE (NO_CARRIER, call log)");
    check(p.second_call_result == MODEM_CALL_RESULT_NONE,
          "facetA: second_call_result CLEARED at IDLE / full-teardown (§9.5)");

    /* a LATER unrelated incoming must NOT surface the finished session's BUSY. */
    call_model_on_ring(&m);
    call_model_on_clip(&m, "0401234567", 0u);
    call_model_project(&m, &p);
    check(p.call_state == MODEM_CALL_RINGING, "facetA: the unrelated incoming projects RINGING");
    check(p.second_call_result == MODEM_CALL_RESULT_NONE,
          "facetA: a stale 2nd-MO terminal does NOT leak across an unrelated incoming");
}

/* Facet B: with a stale second_call_result=BUSY still latched (a prior New-call's
 * outcome), a FRESH 2nd-MO DIAL that is only ACCEPTED (queued, NOT yet dispatched)
 * must clear second_call_result -> NONE at accept time — closing the window in
 * which calls_app.c:742 would roll back the legitimate New-call. */
static void test_facetB_accept_clears_second(void) {
    call_model_t m; tinit(&m); call_model_set_now(&m, 1000u);
    /* a live foreground call (the New-call is placed over it). */
    call_model_on_event(&m, 1u, true, CALL_LEG_ACTIVE, CALL_DIR_MO);
    /* a stale 2nd-MO terminal from a PRIOR New-call is still latched. */
    m.published.second_call_result = MODEM_CALL_RESULT_BUSY;

    /* the app requests a FRESH New-call; it is accepted but NOT dispatched yet
     * (dispatch may hold it behind a queued 15-120 s SMS — §16.7 dequeued != sent). */
    uint32_t tokD = call_model_request(&m, CALL_TXN_DIAL, 0u, true);
    check(tokD != 0u, "facetB precondition: the fresh New-call is accepted");
    check(find_txn(&m, tokD) != NULL && find_txn(&m, tokD)->state == TXN_PENDING,
          "facetB precondition: the New-call is PENDING (accepted, NOT yet dispatched)");
    /* the accept-time clear closed the false-rollback window: NONE at accept. */
    check(m.published.second_call_result == MODEM_CALL_RESULT_NONE,
          "facetB: a fresh 2nd-MO DIAL clears second_call_result at ACCEPT (§9.5), not only at dispatch");
    /* and the projection agrees (still in New-call SETUP over the live call). */
    call_projection_t p; call_model_project(&m, &p);
    check(p.second_call_result == MODEM_CALL_RESULT_NONE,
          "facetB: project() sees second_call_result NONE at accept");
}

/* Facet C: the same accepted-but-not-yet-dispatched window exists for a
 * PRIMARY dial. A pending DIAL already projects DIALING, so it must not carry
 * the previous primary call's terminal result into the new app episode while
 * the modem is still waking or another command is finishing. */
static void test_facetC_primary_accept_clears_last(void) {
    call_model_t m; tinit(&m); call_model_set_now(&m, 1000u);

    /* Complete one primary call through public events. Its remote release is
     * intentionally retained as NO_CARRIER across IDLE for the call log. */
    call_model_on_event(&m, 1u, true, CALL_LEG_ACTIVE, CALL_DIR_MO);
    call_model_set_now(&m, 2000u);
    call_model_on_event(&m, 1u, true, CALL_LEG_RELEASING, CALL_DIR_UNKNOWN);
    call_model_tick(&m);
    call_projection_t p; call_model_project(&m, &p);
    check(p.call_state == MODEM_CALL_IDLE,
          "facetC precondition: the previous primary call reached IDLE");
    check(p.last_call_result == MODEM_CALL_RESULT_NO_CARRIER,
          "facetC precondition: the previous primary result is retained");

    /* Admit a fresh primary dial but leave it PENDING, matching the DTR wake
     * gap seen on the Rev B2 Telit bench. */
    uint32_t tok = call_model_request(&m, CALL_TXN_DIAL, 0u, false);
    check(tok != 0u, "facetC precondition: fresh primary dial is accepted");
    check(find_txn(&m, tok) != NULL && find_txn(&m, tok)->state == TXN_PENDING,
          "facetC precondition: primary dial is pending, not dispatched");
    call_model_project(&m, &p);
    check(p.call_state == MODEM_CALL_DIALING,
          "facetC: an accepted primary dial projects DIALING");
    check(p.last_call_result == MODEM_CALL_RESULT_NONE,
          "facetC: accepted primary dial cannot expose the previous terminal result");

    /* A pre-dispatch queue cancellation ends the new attempt; it must not make
     * the superseded call's terminal result look like this attempt's failure. */
    call_model_txn_cancel(&m, tok);
    call_model_project(&m, &p);
    check(p.call_state == MODEM_CALL_IDLE,
          "facetC: cancelling the pending primary dial returns to IDLE");
    check(p.last_call_result == MODEM_CALL_RESULT_NONE,
          "facetC: cancellation does not resurrect the superseded result");
}

int main(void) {
    test_facetA_idle_clears_second();
    test_facetB_accept_clears_second();
    test_facetC_primary_accept_clears_last();
    if (s_failures != 0) { fprintf(stderr, "%d failure(s)\n", s_failures); return 1; }
    printf("test_call_model_second_result_reset: all assertions passed\n");
    return 0;
}

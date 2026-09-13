/* Regression test: a pre-ID WAIT_REJECT must not report SUCCEEDED prematurely
 * (§16.5).
 *
 * §16.5: "WAIT_REJECT / WAIT_ANSWER / HANGUP targeting the incoming/session bind
 * the pending-MT episode ... so they do NOT report SUCCEEDED while a pending-MT is
 * still present-and-projected." A broken txn_resolve CALL_TXN_WAIT_REJECT case set
 * TXN_SUCCEEDED whenever cm_find_leg(target 0)==NULL — with no pending-MT-active
 * hold, unlike its ANSWER (:1785) / WAIT_ANSWER (:1832) siblings — so a pre-ID
 * WAIT_REJECT (user rejects the waiting call before its ID-scoped event) resolved
 * SUCCEEDED on the very next tick while m.pending_mt.active was still true and no
 * WAITING leg had materialized. It was compounded because txn_bind_targets rebound
 * only ANSWER and WAIT_ANSWER (else continue), so a pre-ID WAIT_REJECT NEVER rebound
 * to a late-materializing WAITING leg (left orphaned/unrejected).
 *
 * This golden pins BOTH: the pre-ID WAIT_REJECT must HOLD (not SUCCEEDED) while the
 * episode is live/unmaterialized, and must REBIND to the WAITING leg once it appears
 * (its own reject semantics — reject, not answer — preserved). Discrimination:
 * fully-unfixed -> premature SUCCEEDED; resolve-fixed-only -> the rebind assertion
 * fails (target stays 0).
 *
 * TDD discipline: drive the PUBLIC API only; never hand-poke struct
 * fields to force state. Single TU — #include the .c under test directly.
 *   cc -std=c11 -I include -I tests/stubs -Wall -Wextra \
 *      -fsanitize=address,undefined -fno-sanitize-recover=all -g -O1 \
 *      tests/test_call_model_wait_reject_preid.c -o /tmp/t && /tmp/t
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
static call_leg_t *find_leg(call_model_t *m, uint8_t id) {
    for (unsigned i = 0; i < MODEM_MAX_CALL_LEGS; i++)
        if (m->legs[i].in_use && m->legs[i].id == id) return &m->legs[i];
    return NULL;
}

static void test_wait_reject_preid_holds_then_rebinds(void) {
    call_model_t m; tinit(&m); call_model_set_now(&m, 1000u);
    /* a live foreground call, then a pre-ID call-waiting episode (RING, no leg id yet). */
    call_model_on_event(&m, 1u, true, CALL_LEG_ACTIVE, CALL_DIR_MO);
    call_model_on_ring(&m);
    check(m.pending_mt.active, "precondition: the call-waiting episode is live");

    /* user rejects the waiting call BEFORE its ID-scoped event (pre-ID, target 0). */
    uint32_t tok = call_model_request(&m, CALL_TXN_WAIT_REJECT, 0u, false);
    check(find_txn(&m, tok) != NULL && find_txn(&m, tok)->target_id == 0u,
          "precondition: WAIT_REJECT is pre-ID (target 0)");
    check(call_model_txn_dispatch_guard(&m, tok) == CALL_DISPATCH_SEND,
          "guard: pre-ID WAIT_REJECT SENDs while the episode is live");
    call_model_txn_dispatched(&m, tok);
    modem_cmd_result_t ok = { .status = CMD_OK, .has_call_result = false,
                              .call_result = MODEM_CALL_RESULT_NONE };
    call_model_txn_command_result(&m, tok, ok);
    call_model_tick(&m);

    /* §16.5: it must NOT report SUCCEEDED while the pending-MT is still
     * present-and-projected and no WAITING leg has materialized. */
    call_txn_t *tx = find_txn(&m, tok);
    check(tx != NULL && tx->state != TXN_SUCCEEDED,
          "pre-ID WAIT_REJECT does NOT report SUCCEEDED while pending_mt is live");
    check(m.pending_mt.active, "the episode is still live (no waiting leg yet)");

    /* the WAITING leg finally materializes (its ID-scoped event lands). */
    call_model_on_event(&m, 2u, true, CALL_LEG_WAITING, CALL_DIR_MT);
    tx = find_txn(&m, tok);
    check(tx != NULL && tx->target_id == 2u,
          "the pre-ID WAIT_REJECT REBINDS to the late WAITING leg (not orphaned)");
    check(tx != NULL && tx->state != TXN_SUCCEEDED,
          "still not SUCCEEDED while its (rebound) waiting leg is present");

    /* the reject takes effect: the waiting leg departs -> SUCCEEDED. */
    call_model_on_event(&m, 2u, true, CALL_LEG_RELEASING, CALL_DIR_MT);
    tx = find_txn(&m, tok);
    check(tx == NULL || tx->state == TXN_SUCCEEDED,
          "WAIT_REJECT resolves SUCCEEDED once its rebound waiting leg is gone");
    check(find_leg(&m, 1u) != NULL,
          "the foreground call is untouched by the reject");
}

int main(void) {
    test_wait_reject_preid_holds_then_rebinds();
    if (s_failures != 0) { fprintf(stderr, "%d failure(s)\n", s_failures); return 1; }
    printf("test_call_model_wait_reject_preid: all assertions passed\n");
    return 0;
}

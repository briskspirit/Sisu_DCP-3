/* Regression test: a target_id-bound op's hard-abandon must release the op's
 * leg — a `bound_id`-only abandon/cleanup leg-resolution releases NOTHING.
 *
 * §16.5 / §16.11: every transient obligation must reach terminal/absent in bounded
 * time. A bound setup/teardown leg must NOT outlive its transaction. A broken
 * txn_deadlines hard-abandon resolved the leg-to-release via `t->bound_id` ONLY —
 * but only DIAL sets bound_id; ANSWER / WAIT_ANSWER / WAIT_REJECT / RELEASE_ACTIVE /
 * RELEASE_LEG bind `target_id`. So a wedged INCOMING call answered pre-ID (ATA sent,
 * no connect, no release URC, no clean CLCC) reaches the 120 s hard-abandon and the
 * leg is NEVER released: leg stays INCOMING, pending-MT stays bound, no reject queued,
 * projection stuck RINGING forever, the FAILED txn leaks its slot, future incoming
 * episodes are blocked. §16.5 ANSWER(0): "if it hits its hard deadline it RELEASES
 * the INCOMING leg + clears the bound pending-MT + queues a reject (never returns to
 * RINGING indefinitely)."
 *
 * ROOT: resolve the leg-to-abandon via the op's PARTICIPANT (cm_txn_owner_ref —
 * target_id for target ops, bound_id for DIAL), NOT bound_id directly, everywhere a
 * txn's leg is abandoned/cleanup-checked. This golden pins the ANSWER flagship plus
 * the WAIT_REJECT and RELEASE_LEG variants (the defect is structural to ALL
 * target_id ops), each PROVEN to fail on the unfixed model.
 *
 * TDD discipline: drive the PUBLIC API ONLY; never hand-poke struct
 * fields to force state. Single TU — #include the .c under test directly.
 *   cc -std=c11 -I include -I tests/stubs -Wall -Wextra \
 *      -fsanitize=address,undefined -fno-sanitize-recover=all -g -O1 \
 *      tests/test_call_model_txn_abandon.c -o /tmp/t && /tmp/t
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
/* Settle: advance the clock past the §5.3 limbo horizon + a couple of ticks so a
 * RELEASING (force-abandoned) leg is limbo-evicted and its terminal txn retires.
 * Uses ONLY the public tick/set_now — no struct poking. */
static void settle(call_model_t *m, uint32_t from_ms) {
    for (uint32_t t = from_ms; t <= from_ms + 20000u; t += 2000u) {
        call_model_set_now(m, t);
        call_model_tick(m);
    }
}

/* A wedged pre-ID ANSWER hits the hard deadline and MUST release
 * its bound INCOMING leg + clear the bound pending-MT + queue a reject + retire. */
static void test_answer_hard_abandon_releases_leg(void) {
    call_model_t m; tinit(&m); call_model_set_now(&m, 1000u);

    call_model_on_ring(&m);                              /* pre-ID incoming episode */
    uint32_t tok = call_model_request(&m, CALL_TXN_ANSWER, 0u, false);
    check(find_txn(&m, tok) != NULL && find_txn(&m, tok)->target_id == 0u,
          "precondition: ANSWER is admitted pre-ID (target 0)");
    check(call_model_txn_dispatch_guard(&m, tok) == CALL_DISPATCH_SEND,
          "precondition: pre-ID ANSWER SENDs while the episode is live");
    call_model_txn_dispatched(&m, tok);
    modem_cmd_result_t ok = { .status = CMD_OK, .has_call_result = false,
                              .call_result = MODEM_CALL_RESULT_NONE };
    call_model_txn_command_result(&m, tok, ok);
    /* the ID-scoped event lands: the ANSWER binds the materialized INCOMING leg. */
    call_model_on_event(&m, 2u, true, CALL_LEG_INCOMING, CALL_DIR_MT);
    check(find_txn(&m, tok) != NULL && find_txn(&m, tok)->target_id == 2u,
          "precondition: ANSWER binds the INCOMING leg (target_id=2)");
    check(m.pending_mt.active && m.pending_mt.bound_id == 2u,
          "precondition: pending-MT is bound to the INCOMING leg");

    /* the modem WEDGES: no connect, no release URC, no clean CLCC. Tick past the
     * 120 s hard-abandon deadline. */
    call_model_set_now(&m, 200000u);
    call_model_tick(&m);

    /* §16.5/§16.11: the INCOMING leg is released, the pending-MT cleared, a reject
     * queued, and the projection is NOT stuck RINGING. */
    call_leg_t *l2 = find_leg(&m, 2u);
    check(l2 == NULL || l2->state == CALL_LEG_RELEASING,
          "the hard-abandon RELEASES the bound INCOMING leg (RELEASING/absent, not stuck INCOMING)");
    check(!m.pending_mt.active,
          "the hard-abandon clears the bound pending-MT (§16.5)");
    call_cleanup_release_t release = {0};
    check(call_model_pop_release(&m, &release) && release.id == 2u &&
              !release.recover_held_survivor,
          "an id-scoped reject/release is queued for the abandoned leg");
    call_projection_t p; call_model_project(&m, &p);
    check(p.call_state != MODEM_CALL_RINGING,
          "the projection does NOT stay RINGING after the abandon");

    /* §16.11 bounded drain: the leg limbo-evicts and the FAILED txn retires. */
    settle(&m, 202000u);
    check(find_leg(&m, 2u) == NULL, "the abandoned leg drains (absent)");
    check(find_txn(&m, tok) == NULL, "the FAILED ANSWER txn retires (no slot leak)");
    call_model_project(&m, &p);
    check(p.call_state == MODEM_CALL_IDLE, "the model returns to IDLE");

    /* the stuck pending-MT no longer blocks a fresh episode. */
    call_model_on_ring(&m);
    check(m.pending_mt.active, "a later incoming can open a fresh episode");
}

/* A wedged WAIT_REJECT over a live call MUST release the WAITING
 * leg without tearing down the foreground ACTIVE call. */
static void test_wait_reject_hard_abandon_releases_leg(void) {
    call_model_t m; tinit(&m); call_model_set_now(&m, 1000u);

    call_model_on_event(&m, 1u, true, CALL_LEG_ACTIVE, CALL_DIR_MO);   /* foreground call */
    call_model_on_ring(&m);                                            /* call-waiting episode */
    call_model_on_event(&m, 2u, true, CALL_LEG_WAITING, CALL_DIR_MT);  /* the waiting leg */

    uint32_t tok = call_model_request(&m, CALL_TXN_WAIT_REJECT, 0u, false);
    check(find_txn(&m, tok) != NULL && find_txn(&m, tok)->target_id == 2u,
          "precondition: WAIT_REJECT resolves to the WAITING leg (target 2)");
    check(call_model_txn_dispatch_guard(&m, tok) == CALL_DISPATCH_SEND,
          "precondition: WAIT_REJECT SENDs (a live waiting leg to reject)");
    call_model_txn_dispatched(&m, tok);
    modem_cmd_result_t ok = { .status = CMD_OK, .has_call_result = false,
                              .call_result = MODEM_CALL_RESULT_NONE };
    call_model_txn_command_result(&m, tok, ok);

    call_model_set_now(&m, 200000u);   /* wedge: no release URC -> hit the hard deadline */
    call_model_tick(&m);

    call_leg_t *l2 = find_leg(&m, 2u);
    check(l2 == NULL || l2->state == CALL_LEG_RELEASING,
          "the WAIT_REJECT hard-abandon RELEASES the WAITING leg");
    call_cleanup_release_t release = {0};
    check(call_model_pop_release(&m, &release) && release.id == 2u &&
              !release.recover_held_survivor,
          "an id-scoped release is queued for the rejected waiting leg");
    check(find_leg(&m, 1u) != NULL && find_leg(&m, 1u)->state == CALL_LEG_ACTIVE,
          "the foreground ACTIVE call is UNTOUCHED by the reject abandon");

    settle(&m, 202000u);
    check(find_leg(&m, 2u) == NULL, "the rejected waiting leg drains (absent)");
    check(find_txn(&m, tok) == NULL, "the FAILED WAIT_REJECT txn retires");
    call_projection_t p; call_model_project(&m, &p);
    check(p.call_state == MODEM_CALL_ACTIVE && p.active_call_id == 1u && !p.waiting_call,
          "after the reject drains, the foreground call is the sole ACTIVE, no waiting");
}

/* A wedged RELEASE_LEG MUST force-release its target leg. */
static void test_release_leg_hard_abandon_releases_leg(void) {
    call_model_t m; tinit(&m); call_model_set_now(&m, 1000u);

    call_model_on_event(&m, 3u, true, CALL_LEG_ACTIVE, CALL_DIR_MO);
    uint32_t tok = call_model_request(&m, CALL_TXN_RELEASE_LEG, 3u, false);
    check(find_txn(&m, tok) != NULL && find_txn(&m, tok)->target_id == 3u,
          "precondition: RELEASE_LEG targets leg 3");
    check(call_model_txn_dispatch_guard(&m, tok) == CALL_DISPATCH_SEND,
          "precondition: RELEASE_LEG SENDs (leg 3 present)");
    call_model_txn_dispatched(&m, tok);
    modem_cmd_result_t ok = { .status = CMD_OK, .has_call_result = false,
                              .call_result = MODEM_CALL_RESULT_NONE };
    call_model_txn_command_result(&m, tok, ok);

    call_model_set_now(&m, 200000u);   /* wedge -> hard deadline */
    call_model_tick(&m);

    call_leg_t *l3 = find_leg(&m, 3u);
    check(l3 == NULL || l3->state == CALL_LEG_RELEASING,
          "the RELEASE_LEG hard-abandon RELEASES leg 3 (not stuck ACTIVE)");
    call_cleanup_release_t release = {0};
    check(call_model_pop_release(&m, &release) && release.id == 3u &&
              !release.recover_held_survivor,
          "an id-scoped release is queued for leg 3");

    settle(&m, 202000u);
    check(find_leg(&m, 3u) == NULL, "leg 3 drains (absent)");
    check(find_txn(&m, tok) == NULL, "the FAILED RELEASE_LEG txn retires");
}

int main(void) {
    test_answer_hard_abandon_releases_leg();
    test_wait_reject_hard_abandon_releases_leg();
    test_release_leg_hard_abandon_releases_leg();
    if (s_failures != 0) { fprintf(stderr, "%d failure(s)\n", s_failures); return 1; }
    printf("test_call_model_txn_abandon: all assertions passed\n");
    return 0;
}

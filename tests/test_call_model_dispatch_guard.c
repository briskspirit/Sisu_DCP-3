/* Regression test: the WAIT_REJECT dispatch-guard must be TOTAL (§16.5
 * MANDATORY dispatch guard — totality).
 *
 * §16.5: "STALE suppresses the write so a stale reject cannot physically affect a
 * DIFFERENT call." A broken WAIT_REJECT guard pre-ID branch tested only `!pending_mt
 * .active && no WAITING leg` — it OMITTED the episode-stale check that ANSWER and
 * WAIT_ANSWER carry. So a WAIT_REJECT queued behind a slow SMS against episode N,
 * whose episode N is then cleared and REPLACED by a live episode N+1, returns
 * CALL_DISPATCH_SEND — the integration then writes AT+CHLD=0 and PHYSICALLY rejects
 * episode N+1's call, a live call the user never targeted.
 *
 * ROOT (mirror the siblings + the resolver): the pre-ID branch must be
 *   if (!m->pending_mt.active || cm_txn_episode_stale(m, t))
 *       return cm_guard_resolve(m, t, CALL_DISPATCH_ALREADY_SATISFIED);
 * A normal (live, non-stale) reject still SENDs.
 *
 * Discrimination: ANSWER's guard (which already has the episode check) returns STALE
 * in the identical drive; the fixed WAIT_REJECT returns ALREADY_SATISFIED (a
 * destructive op whose goal — no waiting call — is met). The unfixed WAIT_REJECT
 * returns SEND.
 *
 * TDD discipline: drive the PUBLIC API ONLY; never poke struct fields.
 * Single TU — #include the .c directly.
 *   cc -std=c11 -I include -I tests/stubs -Wall -Wextra \
 *      -fsanitize=address,undefined -fno-sanitize-recover=all -g -O1 \
 *      tests/test_call_model_dispatch_guard.c -o /tmp/t && /tmp/t
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

/* A pre-ID WAIT_REJECT admitted against episode N must NOT SEND against a later
 * live episode N+1. ANSWER (episode-checked) is the control. */
static void test_wait_reject_guard_stale_across_episodes(void) {
    call_model_t m; tinit(&m); call_model_set_now(&m, 1000u);

    call_model_on_event(&m, 1u, true, CALL_LEG_ACTIVE, CALL_DIR_MO);   /* a live foreground call */
    call_model_on_ring(&m);                                            /* episode N (pre-ID) */
    check(m.pending_mt.active, "precondition: episode N is live");

    /* the user queues WAIT_REJECT(0) behind a slow SMS (admitted, PENDING, records N). */
    uint32_t tok = call_model_request(&m, CALL_TXN_WAIT_REJECT, 0u, false);
    uint32_t tokAns = call_model_request(&m, CALL_TXN_ANSWER, 0u, false);   /* control op, same episode */
    check(find_txn(&m, tok) != NULL && find_txn(&m, tok)->target_id == 0u &&
          find_txn(&m, tok)->state == TXN_PENDING,
          "precondition: WAIT_REJECT is pre-ID and PENDING (queued behind the SMS)");

    /* episode N's caller gives up -> the ring backstop clears the stale episode. */
    call_model_set_now(&m, 122000u);   /* past MODEM_RING_TIMEOUT_MS (120 s) from last ring */
    call_model_tick(&m);
    check(!m.pending_mt.active, "episode N cleared by the ring backstop");

    /* a NEW, different incoming call arrives: episode N+1 (a new generation). */
    call_model_on_ring(&m);
    check(m.pending_mt.active, "episode N+1 is live");

    /* the SMS finishes; the guard runs immediately before the AT write. */
    call_dispatch_guard_t vr = call_model_txn_dispatch_guard(&m, tok);
    check(vr != CALL_DISPATCH_SEND,
          "a pre-ID WAIT_REJECT admitted against episode N must NOT SEND against N+1");
    check(vr == CALL_DISPATCH_ALREADY_SATISFIED,
          "the stale reject resolves ALREADY_SATISFIED (its target waiting call is gone)");

    /* control: ANSWER — which already carries the episode-stale check — returns STALE. */
    call_dispatch_guard_t va = call_model_txn_dispatch_guard(&m, tokAns);
    check(va == CALL_DISPATCH_STALE,
          "control: ANSWER against the replaced episode returns STALE (guard is total)");

    /* episode N+1 is untouched: still live, and the foreground call survives. */
    check(m.pending_mt.active, "episode N+1 is NOT physically rejected");
    call_projection_t p; call_model_project(&m, &p);
    check(p.call_state == MODEM_CALL_ACTIVE && p.active_call_id == 1u,
          "the foreground call is untouched");
}

/* The normal single-live-episode reject still SENDs (the fix does not
 * over-suppress). */
static void test_wait_reject_guard_live_episode_sends(void) {
    call_model_t m; tinit(&m); call_model_set_now(&m, 1000u);

    call_model_on_event(&m, 1u, true, CALL_LEG_ACTIVE, CALL_DIR_MO);
    call_model_on_ring(&m);                                            /* the live episode */
    uint32_t tok = call_model_request(&m, CALL_TXN_WAIT_REJECT, 0u, false);
    check(call_model_txn_dispatch_guard(&m, tok) == CALL_DISPATCH_SEND,
          "a pre-ID reject against its OWN live episode still SENDs");
}

/* A backend's coarse hold toggle may also accept a waiting/held call. A HOLD
 * that was valid when queued must therefore be suppressed if call topology
 * changes before its bytes reach the transport. */
static void test_hold_guard_rechecks_sole_leg_at_dispatch(void) {
    { call_model_t m; tinit(&m); call_model_set_now(&m, 1000u);
      call_model_on_event(&m, 1u, true, CALL_LEG_ACTIVE, CALL_DIR_MO);
      uint32_t tok = call_model_request(&m, CALL_TXN_HOLD, 0u, false);
      check(call_model_hold_toggle_available(&m),
            "sole active leg is eligible for the hold toggle");
      check(call_model_txn_dispatch_guard(&m, tok) == CALL_DISPATCH_SEND,
            "sole active HOLD still sends"); }

    { call_model_t m; tinit(&m); call_model_set_now(&m, 1000u);
      call_model_on_event(&m, 1u, true, CALL_LEG_HELD, CALL_DIR_MO);
      uint32_t tok = call_model_request(&m, CALL_TXN_HOLD, 0u, false);
      check(call_model_hold_toggle_available(&m),
            "sole held leg is eligible for retrieve");
      check(call_model_txn_dispatch_guard(&m, tok) == CALL_DISPATCH_SEND,
            "sole held Unhold still sends"); }

    { call_model_t m; tinit(&m); call_model_set_now(&m, 1000u);
      call_model_on_event(&m, 1u, true, CALL_LEG_ACTIVE, CALL_DIR_MO);
      uint32_t tok = call_model_request(&m, CALL_TXN_HOLD, 0u, false);
      call_model_on_ring(&m);
      check(!call_model_hold_toggle_available(&m),
            "a pre-id incoming episode vetoes the hold toggle");
      check(call_model_txn_dispatch_guard(&m, tok) == CALL_DISPATCH_STALE,
            "queued HOLD is suppressed when a pre-id caller arrives");
      check(m.pending_mt.active,
            "suppressing queued HOLD leaves the incoming episode untouched"); }

    { call_model_t m; tinit(&m); call_model_set_now(&m, 1000u);
      call_model_on_event(&m, 1u, true, CALL_LEG_ACTIVE, CALL_DIR_MO);
      uint32_t tok = call_model_request(&m, CALL_TXN_HOLD, 0u, false);
      call_model_on_event(&m, 2u, true, CALL_LEG_WAITING, CALL_DIR_MT);
      check(!call_model_hold_toggle_available(&m),
            "a confirmed waiting leg vetoes the hold toggle");
      check(call_model_txn_dispatch_guard(&m, tok) == CALL_DISPATCH_STALE,
            "queued HOLD cannot answer a newly confirmed waiting leg"); }

    { call_model_t m; tinit(&m); call_model_set_now(&m, 1000u);
      call_model_on_event(&m, 1u, true, CALL_LEG_ACTIVE, CALL_DIR_MO);
      uint32_t tok = call_model_request(&m, CALL_TXN_HOLD, 0u, false);
      call_model_on_event(&m, 2u, true, CALL_LEG_HELD, CALL_DIR_MO);
      check(!call_model_hold_toggle_available(&m),
            "a second held leg vetoes the hold toggle");
      check(call_model_txn_dispatch_guard(&m, tok) == CALL_DISPATCH_STALE,
            "queued HOLD cannot swap to a newly observed held leg"); }
}

/* A dispatched, then abandoned second outgoing call can remain pre-ID while a
 * late modem leg is still possible. Every CHLD=2-class operation queued before
 * that cancellation must be suppressed at the final write guard. */
static void test_new_call_cleanup_suppresses_role_mutations(void) {
    { call_model_t m; tinit(&m); call_model_set_now(&m, 1000u);
      call_model_on_event(&m, 1u, true, CALL_LEG_ACTIVE, CALL_DIR_MO);
      uint32_t dial = call_model_request(&m, CALL_TXN_DIAL, 0u, true);
      call_model_txn_dispatched(&m, dial);
      uint32_t hold = call_model_request(&m, CALL_TXN_HOLD, 0u, false);
      call_model_new_call_abandoned(&m);
      check(call_model_new_call_cleanup_pending(&m, NULL),
            "abandoned pre-ID second-MO arms the cleanup gate");
      check(call_model_txn_dispatch_guard(&m, hold) == CALL_DISPATCH_STALE,
            "queued HOLD cannot cross an unbound New-call cleanup"); }

    { call_model_t m; tinit(&m); call_model_set_now(&m, 1000u);
      call_model_on_event(&m, 1u, true, CALL_LEG_ACTIVE, CALL_DIR_MO);
      call_model_on_event(&m, 2u, true, CALL_LEG_HELD, CALL_DIR_MO);
      uint32_t dial = call_model_request(&m, CALL_TXN_DIAL, 0u, true);
      call_model_txn_dispatched(&m, dial);
      uint32_t swap = call_model_request(&m, CALL_TXN_SWAP, 0u, false);
      call_model_new_call_abandoned(&m);
      check(call_model_txn_dispatch_guard(&m, swap) == CALL_DISPATCH_STALE,
            "queued SWAP cannot cross an unbound New-call cleanup"); }

    { call_model_t m; tinit(&m); call_model_set_now(&m, 1000u);
      call_model_on_event(&m, 1u, true, CALL_LEG_ACTIVE, CALL_DIR_MO);
      call_model_on_ring(&m);
      call_model_on_event(&m, 2u, true, CALL_LEG_WAITING, CALL_DIR_MT);
      uint32_t dial = call_model_request(&m, CALL_TXN_DIAL, 0u, true);
      call_model_txn_dispatched(&m, dial);
      uint32_t answer = call_model_request(&m, CALL_TXN_WAIT_ANSWER, 0u,
                                           false);
      call_model_new_call_abandoned(&m);
      check(call_model_txn_dispatch_guard(&m, answer) == CALL_DISPATCH_STALE,
            "queued WAIT_ANSWER cannot cross an unbound New-call cleanup"); }
}

/* Pressing C can admit HANGUP after ATD has crossed the UART but before the
 * modem has exposed an id-bearing leg. The subsequently revealed MO leg still
 * belongs to that exact dial attempt, so the queued session release must not be
 * suppressed as an empty-baseline no-op. Conversely, a dial admitted only
 * after the hangup is a different session and must remain untouched. */
static void test_hangup_tracks_pre_id_dial_by_token(void) {
    { call_model_t m; tinit(&m); call_model_set_now(&m, 1000u);
      uint32_t dial = call_model_request(&m, CALL_TXN_DIAL, 0u, false);
      uint32_t hangup = call_model_request(&m, CALL_TXN_HANGUP, 0u, false);

      check(call_model_txn_dispatch_guard(&m, dial) == CALL_DISPATCH_STALE,
            "HANGUP cancels a DIAL that never crossed the UART");
      check(call_model_txn_dispatch_guard(&m, hangup) ==
                CALL_DISPATCH_ALREADY_SATISFIED,
            "no physical HANGUP is sent when the queued DIAL was cancelled pre-dispatch"); }

    { call_model_t m; tinit(&m); call_model_set_now(&m, 1000u);
      uint32_t dial = call_model_request(&m, CALL_TXN_DIAL, 0u, false);
      call_model_txn_dispatched(&m, dial);
      uint32_t hangup = call_model_request(&m, CALL_TXN_HANGUP, 0u, false);

      call_model_on_event(&m, 1u, true, CALL_LEG_ACTIVE, CALL_DIR_MO);
      check(call_model_txn_dispatch_guard(&m, hangup) == CALL_DISPATCH_SEND,
            "HANGUP owns a MO leg revealed after admission by its dispatched DIAL token"); }

    { call_model_t m; tinit(&m); call_model_set_now(&m, 1000u);
      uint32_t dial = call_model_request(&m, CALL_TXN_DIAL, 0u, false);
      call_model_txn_dispatched(&m, dial);
      uint32_t hangup = call_model_request(&m, CALL_TXN_HANGUP, 0u, false);
      modem_cmd_result_t rejected = { .status = CMD_ERROR };
      call_model_txn_command_result(&m, dial, rejected);

      check(call_model_txn_dispatch_guard(&m, hangup) ==
                CALL_DISPATCH_ALREADY_SATISFIED,
            "a rejected pre-id DIAL retires its HANGUP obligation"); }

    { call_model_t m; tinit(&m); call_model_set_now(&m, 1000u);
      uint32_t hangup = call_model_request(&m, CALL_TXN_HANGUP, 0u, false);
      uint32_t later_dial = call_model_request(&m, CALL_TXN_DIAL, 0u, false);
      call_model_txn_dispatched(&m, later_dial);
      call_model_on_event(&m, 1u, true, CALL_LEG_ACTIVE, CALL_DIR_MO);

      check(call_model_txn_dispatch_guard(&m, hangup) ==
                CALL_DISPATCH_ALREADY_SATISFIED,
            "an empty-session HANGUP never absorbs a later unrelated DIAL"); }
}

int main(void) {
    test_wait_reject_guard_stale_across_episodes();
    test_wait_reject_guard_live_episode_sends();
    test_hold_guard_rechecks_sole_leg_at_dispatch();
    test_new_call_cleanup_suppresses_role_mutations();
    test_hangup_tracks_pre_id_dial_by_token();
    if (s_failures != 0) { fprintf(stderr, "%d failure(s)\n", s_failures); return 1; }
    printf("test_call_model_dispatch_guard: all assertions passed\n");
    return 0;
}

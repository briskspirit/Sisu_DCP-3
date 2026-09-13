/* Host unit tests for the modem_call_model scheduler, the §6
 * bounded-drain invariant, and the id-scoped release drain.
 *
 * modem_call_model.c is a PURE module (time is injected via call_model_set_now;
 * no HAL calls), so it links directly with no stubs -- same pattern as
 * test_phone_match.
 *
 * COMPILE (sanitizer recipe, standalone):
 *   cc -std=c11 -I include -I tests/stubs -Wall -Wextra \
 *      -fsanitize=address,undefined -fno-sanitize-recover=all -g -O1 \
 *      tests/test_call_model_sched.c src/services/modem_call_model.c -o /tmp/t && /tmp/t
 *
 * ORACLE: expected values are derived from docs/call_model.md (§6 scheduling + bounded
 * drain, §7.1 abandon backstop, §5.3 limbo, §9.4 ring backstop), NOT from
 * observed output. Interval oracles: keepalive 4000 ms, fast confirm 300 ms,
 * backoff cap 64000 ms (4000<<4), limbo 8000 ms, ring timeout 120000 ms.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "services/modem_call_model.h"
#include "call_timing_fixture.h"

/* §16.12: init the model with the stable host timing fixture. */
static void tinit(call_model_t *m) { call_timing_t _t = call_timing_fixture(); call_model_init(m, &_t); }
/* CMD_OK folds in the old txn_accepted (§16.2). */
static void txn_ok(call_model_t *m, uint32_t tok) { modem_cmd_result_t _r = { .status = CMD_OK }; call_model_txn_command_result(m, tok, _r); }

static int s_failures;
static void assert_true(bool c, const char *msg) {
    if (!c) { fprintf(stderr, "FAIL: %s\n", msg); s_failures++; }
}
static call_txn_t *find_txn_sched(call_model_t *m, uint32_t token) {
    for (unsigned i = 0; i < MODEM_MAX_CALL_TRANSACTIONS; i++)
        if (m->txns[i].in_use && m->txns[i].token == token) return &m->txns[i];
    return NULL;
}

/* A single stable ACTIVE leg (id 1) -> a non-empty, consistent model. */
static void model_with_active_leg(call_model_t *m) {
    tinit(m);
    m->legs[0].in_use = true;
    m->legs[0].id = 1u;
    m->legs[0].generation = 0u;
    m->legs[0].state = CALL_LEG_ACTIVE;
    m->legs[0].dir = CALL_DIR_MT;
}

static void test_idle_no_poll(void) {
    call_model_t m;
    tinit(&m);
    call_model_set_now(&m, 1000u);
    call_model_tick(&m);
    assert_true(!call_model_wants_clcc(&m), "empty model never wants CLCC (t=1000)");
    call_model_set_now(&m, 1000000u);
    call_model_tick(&m);
    assert_true(!call_model_wants_clcc(&m), "empty model never wants CLCC (t=1e6)");
}

static void test_service_policy_queries(void) {
    call_model_t m;
    tinit(&m);
    assert_true(!call_model_session_active(&m) &&
                    !call_model_control_pending(&m) &&
                    !call_model_background_work_blocked(&m),
                "an empty model admits sleep and background work");

    uint32_t dial = call_model_request(&m, CALL_TXN_DIAL, 0u, false);
    assert_true(dial != 0u && call_model_session_active(&m) &&
                    call_model_control_pending(&m) &&
                    !call_model_background_work_blocked(&m),
                "a pending dial preempts foreground protocols but not background polling");
    call_model_txn_cancel(&m, dial); /* never dispatched: reclaimed immediately */

    call_model_on_event(&m, 1u, true, CALL_LEG_ACTIVE, CALL_DIR_MO);
    assert_true(call_model_session_active(&m) &&
                    !call_model_control_pending(&m) &&
                    !call_model_background_work_blocked(&m),
                "a stable ACTIVE leg is a live session, not a control transition");

    uint32_t hold = call_model_request(&m, CALL_TXN_HOLD, 0u, false);
    assert_true(hold != 0u && call_model_control_pending(&m) &&
                    !call_model_background_work_blocked(&m),
                "a pending hold preempts foreground protocols without suppressing polls");
    call_model_txn_cancel(&m, hold);

    uint32_t hangup = call_model_request(&m, CALL_TXN_HANGUP, 0u, false);
    assert_true(hangup != 0u && call_model_control_pending(&m) &&
                    call_model_background_work_blocked(&m),
                "a pending session teardown suppresses unrelated background work");

    tinit(&m);
    call_model_on_incoming_diverted(&m);
    assert_true(call_model_session_active(&m) &&
                    !call_model_control_pending(&m) &&
                    !call_model_background_work_blocked(&m),
                "pre-ring redirected-call metadata is live but remains hidden");
    call_model_on_ring(&m);
    assert_true(call_model_session_active(&m) &&
                    call_model_control_pending(&m) &&
                    call_model_background_work_blocked(&m),
                "a presenting incoming episode preempts protocols and background work");

    tinit(&m);
    call_model_set_now(&m, 1000u);
    uint32_t cancelled = call_model_request(&m, CALL_TXN_DIAL, 0u, false);
    call_model_txn_dispatched(&m, cancelled);
    call_model_txn_cancel(&m, cancelled);
    m.next_clcc_ms = m.now_ms;
    assert_true(!call_model_session_active(&m) &&
                    !call_model_control_pending(&m) &&
                    !call_model_background_work_blocked(&m) &&
                    call_model_wants_clcc(&m),
                "an unbound cleanup tombstone polls CLCC without keeping a call session awake");
}

static void test_keepalive_due(void) {
    call_model_t m;
    model_with_active_leg(&m);
    m.next_clcc_ms = 5000u;
    call_model_set_now(&m, 4999u);
    call_model_tick(&m);
    assert_true(!call_model_wants_clcc(&m), "keepalive not due at now < next_clcc_ms");
    call_model_set_now(&m, 5000u);
    call_model_tick(&m);
    assert_true(call_model_wants_clcc(&m), "keepalive due at now == next_clcc_ms");
}

static void test_defer_inflight_txn(void) {
    call_model_t m;
    model_with_active_leg(&m);
    m.next_clcc_ms = 0u;                 /* due */
    call_model_set_now(&m, 10000u);
    m.txns[0].in_use = true;
    m.txns[0].state = TXN_DISPATCHED;    /* a call-control op mid-issue */
    call_model_tick(&m);
    assert_true(!call_model_wants_clcc(&m), "defer keepalive while a txn is DISPATCHED");
    m.txns[0].state = TXN_BOUND;         /* command accepted; bound to a leg */
    call_model_tick(&m);
    assert_true(call_model_wants_clcc(&m), "resume keepalive once the txn is BOUND");
}

static void test_defer_shadow_open(void) {
    call_model_t m;
    model_with_active_leg(&m);
    m.next_clcc_ms = 0u;
    call_model_set_now(&m, 10000u);
    m.shadow_open = true;
    call_model_tick(&m);
    assert_true(!call_model_wants_clcc(&m), "no new CLCC while one is in flight (shadow_open)");
    m.shadow_open = false;
    assert_true(call_model_wants_clcc(&m), "poll allowed once the in-flight CLCC closed");
}

static void test_clcc_begin_bumps_keepalive(void) {
    call_model_t m;
    model_with_active_leg(&m);
    call_model_set_now(&m, 1000u);
    m.clcc_backoff_shift = 0u;
    call_model_tick(&m);                 /* no fast reason -> window closed */
    m.shadow_open = false;               /* isolate the scheduler from CLCC shadow state */
    call_model_clcc_begin(&m, 0u);
    assert_true(m.next_clcc_ms == 1000u + 4000u,
                "clcc_begin bumps next_clcc_ms by the 4000 ms keepalive interval");
}

static void test_fast_window_uncertain_then_backoff(void) {
    call_model_t m;
    model_with_active_leg(&m);
    call_model_set_now(&m, 1000u);
    m.txns[0].in_use = true;
    m.txns[0].state = TXN_UNCERTAIN;
    m.txns[0].policy_deadline_ms = 3000u;  /* fast until 3000 */
    m.txns[0].abandon_deadline_ms = 0u;    /* not armed: isolate the fast-window path */
    call_model_tick(&m);
    assert_true(m.fast_clcc_until_ms == 3000u,
                "fast window ends at the UNCERTAIN txn policy_deadline");
    m.shadow_open = false;
    call_model_clcc_begin(&m, 0u);
    assert_true(m.next_clcc_ms == 1000u + 300u,
                "inside the fast window clcc_begin bumps by the 300 ms confirm interval");
    call_model_set_now(&m, 3500u);         /* past policy_deadline */
    call_model_tick(&m);
    assert_true(m.fast_clcc_until_ms == 3500u,
                "past policy_deadline the fast window is closed (== now)");
    m.shadow_open = false;
    call_model_clcc_begin(&m, 0u);
    assert_true(m.next_clcc_ms == 3500u + 4000u,
                "past the fast window clcc_begin falls back to the keepalive interval");
}

static void test_backoff_exponential_and_cap(void) {
    call_model_t m;
    model_with_active_leg(&m);
    call_model_set_now(&m, 1000u);
    m.clcc_backoff_shift = 2u;             /* 4000 << 2 = 16000 */
    call_model_tick(&m);
    m.shadow_open = false;
    call_model_clcc_begin(&m, 0u);
    assert_true(m.next_clcc_ms == 1000u + 16000u,
                "backoff shift 2 -> 16 s keepalive interval");
    m.clcc_backoff_shift = 250u;           /* clamp to the cap (shift 4 -> 64 s) */
    call_model_set_now(&m, 2000u);
    call_model_tick(&m);
    m.shadow_open = false;
    call_model_clcc_begin(&m, 0u);
    assert_true(m.next_clcc_ms == 2000u + 64000u,
                "backoff clamps at 64 s (4000 << 4)");
}

static void test_abandon_backstop_bound_leg(void) {
    call_model_t m;
    tinit(&m);
    m.legs[0].in_use = true;
    m.legs[0].id = 3u;
    m.legs[0].generation = 0u;
    m.legs[0].state = CALL_LEG_DIALING;    /* a setup leg */
    m.txns[0].in_use = true;
    m.txns[0].kind = CALL_TXN_DIAL;        /* only a DIAL sets bound_id (participant-resolved abandon) */
    m.txns[0].state = TXN_UNCERTAIN;
    m.txns[0].bound_id = 3u;
    m.txns[0].bound_gen = 0u;
    m.txns[0].abandon_deadline_ms = 2000u;
    m.txns[0].result = MODEM_CALL_RESULT_NONE;
    call_model_set_now(&m, 2000u);
    call_model_tick(&m);
    assert_true(m.legs[0].state == CALL_LEG_RELEASING,
                "abandon converts the bound setup leg to RELEASING");
    assert_true(m.legs[0].pending_removal && m.legs[0].first_removal_ms == 2000u,
                "abandon arms the leg limbo clock (pending_removal + first_removal_ms=now)");
    call_cleanup_release_t release = {0};
    assert_true(call_model_pop_release(&m, &release) && release.id == 3u &&
                !release.recover_held_survivor,
                "abandon queues the leg id-scoped release (id 3)");
    assert_true(m.txns[0].state == TXN_FAILED &&
                m.txns[0].result == MODEM_CALL_RESULT_NO_ANSWER,
                "abandon fails the txn with NO_ANSWER (never-witnessed setup)");
}

static void test_abandon_backstop_unbound(void) {
    call_model_t m;
    tinit(&m);
    m.txns[0].in_use = true;
    m.txns[0].state = TXN_UNCERTAIN;
    m.txns[0].bound_id = 0u;               /* never bound a leg */
    m.txns[0].abandon_deadline_ms = 2000u;
    call_model_set_now(&m, 2000u);
    call_model_tick(&m);
    assert_true(m.txns[0].state == TXN_FAILED, "unbound txn fails at abandon_deadline");
    call_cleanup_release_t release = {0};
    assert_true(!call_model_pop_release(&m, &release),
                "an unbound abandon queues no release");
}

static void test_limbo_evict(void) {
    call_model_t m;
    tinit(&m);
    m.legs[0].in_use = true;
    m.legs[0].id = 4u;
    m.legs[0].state = CALL_LEG_RELEASING;
    m.legs[0].pending_removal = true;
    m.legs[0].first_removal_ms = 1000u;    /* limbo expires at 1000 + 8000 = 9000 */
    call_model_set_now(&m, 8999u);
    call_model_tick(&m);
    assert_true(m.legs[0].in_use, "pending_removal leg survives until first_removal_ms + limbo");
    call_model_set_now(&m, 9000u);
    call_model_tick(&m);
    assert_true(!m.legs[0].in_use, "pending_removal leg force-evicted at the limbo deadline");
}

static void test_pending_mt_backstop(void) {
    call_model_t m;
    tinit(&m);
    m.pending_mt.active = true;
    m.pending_mt.bound_valid = false;
    m.pending_mt.last_ring_ms = 1000u;     /* clears at 1000 + 120000 = 121000 */
    call_model_set_now(&m, 120999u);
    call_model_tick(&m);
    assert_true(m.pending_mt.active, "unbound pending-MT survives until the ring timeout");
    call_model_set_now(&m, 121000u);
    call_model_tick(&m);
    assert_true(!m.pending_mt.active, "stuck unbound pending-MT cleared at MODEM_RING_TIMEOUT_MS");

    tinit(&m);                    /* a bound episode is tracked by its leg */
    m.pending_mt.active = true;
    m.pending_mt.bound_valid = true;
    m.pending_mt.last_ring_ms = 1000u;
    call_model_set_now(&m, 500000u);
    call_model_tick(&m);
    assert_true(m.pending_mt.active, "a bound pending-MT is not aged out by the ring backstop");
}

static void test_reason_bits_cleared_when_consistent(void) {
    call_model_t m;
    tinit(&m);
    m.reconcile_reasons =
        (uint32_t)CALL_RC_COARSE_FINAL | (uint32_t)CALL_RC_PENDING_MT |
        (uint32_t)CALL_RC_UNCERTAIN_TXN;
    call_model_set_now(&m, 5000u);
    call_model_tick(&m);                    /* empty model is consistent -> clear */
    assert_true(m.reconcile_reasons == 0u,
                "a consistent (empty) model clears all reconcile reason bits");

    tinit(&m);                     /* a transient leg keeps it inconsistent */
    m.reconcile_reasons = (uint32_t)CALL_RC_COARSE_FINAL;
    m.legs[0].in_use = true;
    m.legs[0].id = 2u;
    m.legs[0].state = CALL_LEG_RELEASING;    /* transient */
    call_model_set_now(&m, 5000u);
    call_model_tick(&m);
    assert_true((m.reconcile_reasons & (uint32_t)CALL_RC_COARSE_FINAL) != 0u,
                "an inconsistent model retains reason bits (tick only clears, never derives)");
}

static void test_pop_release_fifo(void) {
    call_model_t m;
    tinit(&m);
    /* live legs for the queued releases (pop_release validates {id,gen}). */
    for (unsigned i = 0; i < 3u; i++) {
        m.legs[i].in_use = true; m.legs[i].id = (uint8_t)(5u + i); m.legs[i].generation = 1u;
        m.legs[i].state = CALL_LEG_RELEASING;
    }
    m.release_queue[0] = (call_cleanup_release_t){ .id = 5u, .generation = 1u };
    m.release_queue[1] = (call_cleanup_release_t){ .id = 6u, .generation = 1u,
                                                   .recover_held_survivor = true };
    m.release_queue[2] = (call_cleanup_release_t){ .id = 7u, .generation = 1u };
    m.release_count = 3u;
    call_cleanup_release_t release = {0};
    assert_true(call_model_pop_release(&m, &release) && release.id == 5u &&
                !release.recover_held_survivor, "pop 1st queued release (5)");
    assert_true(call_model_pop_release(&m, &release) && release.id == 6u &&
                release.recover_held_survivor,
                "pop 2nd queued release (6) preserves recovery intent");
    assert_true(call_model_pop_release(&m, &release) && release.id == 7u,
                "pop 3rd queued release (7)");
    assert_true(!call_model_pop_release(&m, &release), "drained queue returns false");
    assert_true(m.release_count == 0u, "drained queue has count 0");
}

static void test_release_requeue_after_dispatch_admission_failure(void) {
    call_model_t m;
    tinit(&m);
    call_model_on_event(&m, 3u, true, CALL_LEG_ACTIVE, CALL_DIR_MO);
    m.release_queue[0] = (call_cleanup_release_t){
        .id = 3u,
        .generation = 1u,
        .recover_held_survivor = true,
    };
    m.release_count = 1u;

    call_cleanup_release_t release = {0};
    assert_true(call_model_pop_release(&m, &release) && release.id == 3u,
                "integration may take the generation-validated release");

    for (unsigned i = 1u; i < MODEM_MAX_CALL_TRANSACTIONS; i++) {
        m.txns[i].in_use = true;
        m.txns[i].token = 100u + i;
        m.txns[i].kind = CALL_TXN_DIAL;
        m.txns[i].state = TXN_DISPATCHED;
        m.txns[i].bound_id = 3u;
        m.txns[i].bound_gen = 1u;
    }
    assert_true(call_model_request(&m, CALL_TXN_RELEASE_LEG,
                                   release.id, false) == 0u,
                "a saturated transaction table can reject the guarded release command");
    assert_true(call_model_requeue_release(&m, &release),
                "failed admission restores generation and recovery semantics");
    memset(&m.txns[1], 0, sizeof(m.txns[1]));
    call_cleanup_release_t retried = {0};
    assert_true(call_model_pop_release(&m, &retried) && retried.id == 3u &&
                retried.generation == 1u && retried.recover_held_survivor,
                "restored cleanup retains its complete intent");

    call_model_on_event(&m, 3u, true, CALL_LEG_RELEASING, CALL_DIR_UNKNOWN);
    assert_true(!call_model_requeue_release(&m, &release),
                "a departed leg cannot be requeued against a later id generation");
}

/* A reused id (freed then re-alloc'd at a new generation) is NOT in the
 * gen-qualified baseline, so a DIAL dispatched over the old baseline binds the
 * genuinely-new leg. */
static void test_reused_id_baseline_binds(void) {
    call_model_t m;
    tinit(&m);
    call_model_set_now(&m, 1000u);
    /* leg id 2 gen 1 exists at dispatch -> captured in the DIAL creation baseline. */
    call_model_on_event(&m, 2u, true, CALL_LEG_ACTIVE, CALL_DIR_MO);
    uint32_t tok = call_model_request(&m, CALL_TXN_DIAL, 0u, false);
    call_model_txn_dispatched(&m, tok);
    txn_ok(&m, tok);
    /* id 2 gen 1 is released, then id 2 is REUSED at gen 2 as our new dial's leg. */
    call_model_on_event(&m, 2u, true, CALL_LEG_RELEASING, CALL_DIR_UNKNOWN);
    call_model_on_event(&m, 2u, true, CALL_LEG_DIALING, CALL_DIR_MO);   /* id 2, generation 2 */
    call_txn_t *t = NULL;
    for (unsigned i = 0; i < MODEM_MAX_CALL_TRANSACTIONS; i++)
        if (m.txns[i].in_use && m.txns[i].token == tok) t = &m.txns[i];
    assert_true(t != NULL && t->bound_id == 2u && t->bound_gen == 2u,
                "the DIAL binds the reused id at its NEW generation (baseline is gen-qualified)");
    assert_true(t != NULL && t->state == TXN_BOUND, "bound to the new-generation DIALING leg");
}

/* A queued release for {id=2,gen=1} whose leg vanished and whose id was reused
 * at gen 2 is DROPPED at dequeue -- AT+CHLD=12 must not release the NEW call. */
static void test_stale_release_dropped(void) {
    call_model_t m;
    tinit(&m);
    /* a stale queued release for id 2 generation 1. */
    m.release_queue[0] = (call_cleanup_release_t){ .id = 2u, .generation = 1u };
    m.release_count = 1u;
    /* id 2 now exists at a DIFFERENT generation (a new, unrelated call). */
    m.legs[0].in_use = true; m.legs[0].id = 2u; m.legs[0].generation = 2u;
    m.legs[0].state = CALL_LEG_ACTIVE;
    call_cleanup_release_t release = {0};
    assert_true(!call_model_pop_release(&m, &release),
                "a stale release for a reused id is DROPPED (not applied to the new call)");
    assert_true(m.release_count == 0u, "the stale entry is consumed/dropped");
    assert_true(m.legs[0].in_use && m.legs[0].state == CALL_LEG_ACTIVE,
                "the reused-id call is untouched");
    /* but a release for the leg's ACTUAL generation IS delivered. */
    m.release_queue[0] = (call_cleanup_release_t){ .id = 2u, .generation = 2u };
    m.release_count = 1u;
    assert_true(call_model_pop_release(&m, &release) && release.id == 2u,
                "a release matching the live {id,gen} is delivered");
}

static void test_bounded_drain_invariant(void) {
    call_model_t m;
    tinit(&m);
    /* A model built ONLY from transient elements, each with a hard terminal
     * deadline (spec §6): an unbound UNCERTAIN txn (abandon), a bound setup
     * leg's txn (abandon -> RELEASING -> limbo), an unbound pending-MT (ring). */
    m.txns[0].in_use = true;               /* unbound UNCERTAIN -> abandon at 2000 */
    m.txns[0].state = TXN_UNCERTAIN;
    m.txns[0].bound_id = 0u;
    m.txns[0].abandon_deadline_ms = 2000u;
    m.txns[1].in_use = true;               /* bound setup -> abandon converts its leg */
    m.txns[1].kind = CALL_TXN_DIAL;        /* only a DIAL sets bound_id (participant-resolved abandon) */
    m.txns[1].state = TXN_UNCERTAIN;
    m.txns[1].bound_id = 6u;
    m.txns[1].bound_gen = 0u;
    m.txns[1].abandon_deadline_ms = 2000u;
    m.legs[0].in_use = true;
    m.legs[0].id = 6u;
    m.legs[0].generation = 0u;
    m.legs[0].state = CALL_LEG_ALERTING;
    m.pending_mt.active = true;            /* unbound -> ring backstop at 121000 */
    m.pending_mt.bound_valid = false;
    m.pending_mt.last_ring_ms = 1000u;

    call_model_set_now(&m, 0u);
    call_model_tick(&m);
    assert_true(call_model_wants_clcc(&m),
                "a transient-only model wants CLCC while its elements live");

    call_model_set_now(&m, 130000u);        /* past abandon + ring deadlines */
    call_model_tick(&m);
    assert_true(!m.pending_mt.active, "pending-MT drained by the ring backstop");
    /* leg 6 is now RELEASING + pending_removal, limbo armed at 130000. */
    call_model_set_now(&m, 138000u);        /* past 130000 + 8000 limbo */
    call_model_tick(&m);

    call_cleanup_release_t release = {0};    /* consumer drains queued releases each tick */
    while (call_model_pop_release(&m, &release)) { /* consume */ }

    bool any_leg = false, any_open_txn = false;
    for (unsigned i = 0; i < MODEM_MAX_CALL_LEGS; i++) {
        if (m.legs[i].in_use) { any_leg = true; }
    }
    for (unsigned i = 0; i < MODEM_MAX_CALL_TRANSACTIONS; i++) {
        if (m.txns[i].in_use &&
            (m.txns[i].state == TXN_PENDING || m.txns[i].state == TXN_DISPATCHED ||
             m.txns[i].state == TXN_BOUND   || m.txns[i].state == TXN_UNCERTAIN)) {
            any_open_txn = true;
        }
    }
    assert_true(!any_leg, "bounded-drain: all legs gone after their deadlines");
    assert_true(!any_open_txn, "bounded-drain: no open transactions remain");
    assert_true(!m.pending_mt.active, "bounded-drain: pending-MT cleared");
    assert_true(!call_model_wants_clcc(&m),
                "bounded-drain invariant: a transient-only model reaches wants_clcc==false");
}

/* [T6 Minor-1] a bare final must NOT flip a not-yet-dispatched (PENDING) txn to
 * UNCERTAIN: that would leave it OPEN with unarmed (0) deadlines -> txn_deadlines
 * never abandons it -> a backstop-less open txn breaks the §6 bounded drain. */
static void test_bare_final_undispatched_no_backstopless_open(void) {
    call_model_t m;
    tinit(&m);
    call_model_set_now(&m, 1000u);
    /* A merely-requested (never-dispatched) PENDING DIAL via the public path. */
    uint32_t tok = call_model_request(&m, CALL_TXN_DIAL, 0u, false);
    call_model_on_bare_final(&m, MODEM_CALL_RESULT_NO_CARRIER, false);
    call_txn_t *t = find_txn_sched(&m, tok);
    assert_true(t != NULL && t->state == TXN_PENDING,
                "[T6 Minor-1] a bare final leaves an undispatched PENDING txn PENDING (not UNCERTAIN)");
}

/* [T6 Minor-5a] anti-late-connect recovery: a policy-timeout UNCERTAIN DIAL stays
 * BINDABLE; a matching ACTIVE,MO leg appearing LATER binds AND resolves SUCCEEDED
 * (this is the exact corner the prior optimistic model leaked on). */
static void test_uncertain_late_connect_recovers(void) {
    call_model_t m;
    tinit(&m);
    call_model_set_now(&m, 1000u);
    /* Drive a DIAL to UNCERTAIN via the public path (dispatch, empty baseline,
     * then a reported CMD_TIMEOUT -> UNCERTAIN + still bindable). No hand-poked token. */
    uint32_t tok = call_model_request(&m, CALL_TXN_DIAL, 0u, false);
    call_model_txn_dispatched(&m, tok);
    modem_cmd_result_t to = { .status = CMD_TIMEOUT };
    call_model_txn_command_result(&m, tok, to);
    assert_true(find_txn_sched(&m, tok)->state == TXN_UNCERTAIN, "precondition: DIAL UNCERTAIN, still bindable");
    /* the matching outgoing leg finally appears ACTIVE with dir MO. */
    call_model_on_event(&m, 2u, true, CALL_LEG_ACTIVE, CALL_DIR_MO);
    call_txn_t *t = find_txn_sched(&m, tok);
    assert_true(t != NULL && t->state == TXN_SUCCEEDED,
                "late-connect: an UNCERTAIN DIAL binds+resolves a LATE matching ACTIVE,MO leg");
    assert_true(t != NULL && t->result == MODEM_CALL_RESULT_CONNECTED,
                "late-connect: the recovered DIAL result is CONNECTED (no blind fail)");
    assert_true(t != NULL && t->bound_id == 2u, "late-connect: bound to the late leg");
}

/* [T6 Minor-5b] unbound finiteness: an unbound DIAL (no leg ever appeared) past its
 * abandon deadline -> FAILED, no leg to release. */
static void test_unbound_dial_abandon_failed(void) {
    /* Drive the DIAL through request/dispatch; a short txn_abandon_ms fires
     * the abandon at now=3000 (the original hand-poked deadline). No hand-poked token. */
    call_timing_t t = call_timing_fixture(); t.txn_abandon_ms = 2000u;
    call_model_t m; call_model_init(&m, &t); call_model_set_now(&m, 1000u);
    uint32_t tok = call_model_request(&m, CALL_TXN_DIAL, 0u, false);
    call_model_txn_dispatched(&m, tok);       /* abandon = 1000 + 2000 = 3000; never bound (no leg) */
    call_model_set_now(&m, 3000u);
    call_model_tick(&m);
    call_txn_t *tx = find_txn_sched(&m, tok);
    assert_true(tx != NULL && tx->state == TXN_FAILED, "unbound DIAL past abandon_deadline -> FAILED");
    call_cleanup_release_t release = {0};
    assert_true(!call_model_pop_release(&m, &release),
                "unbound DIAL abandon queues no release");
}

/* Urgency is EDGE-TRIGGERED. A freshly-RAISED urgent reason (via cm_raise_reason,
 * reached here through the public on_bare_final path) pulls next_clcc_ms to now so the
 * poll fires at the next free slot; but the reason bit alone (already set) does NOT keep
 * wants_clcc true -- the predicate is purely schedule-based, so an unresolved reason
 * cannot busy-loop re-polling at the same timestamp, and an ERROR backoff is honored. */
static void test_wants_clcc_edge_triggered(void) {
    call_model_t m;
    model_with_active_leg(&m);              /* stable connected call (non-empty, consistent) */
    call_model_set_now(&m, 1000u);
    m.next_clcc_ms = 100000u;              /* keepalive far in the future (not due) */
    m.fast_clcc_until_ms = 0u;
    m.reconcile_reasons = 0u;
    m.shadow_open = false;
    assert_true(!call_model_wants_clcc(&m), "edge: not due before any urgent reason");

    /* a bare final RAISES COARSE_FINAL via cm_raise_reason -> pulls next_clcc_ms=now. */
    call_model_on_bare_final(&m, MODEM_CALL_RESULT_NO_CARRIER, false);
    assert_true(m.next_clcc_ms == 1000u, "edge: raising an urgent reason pulls next_clcc_ms to now");
    assert_true(call_model_wants_clcc(&m), "edge: a freshly-raised reason polls at the next free slot");

    /* a SECOND raise of the SAME (already-set) bit does NOT re-pull to a later now
     * (edge-trigger only fires on the rising edge). */
    call_model_set_now(&m, 2000u);
    call_model_on_bare_final(&m, MODEM_CALL_RESULT_NO_CARRIER, false);  /* COARSE_FINAL already set */
    assert_true(m.next_clcc_ms == 1000u, "edge: re-raising an already-set bit does not re-pull the schedule");

    /* schedule-based only: with the reason still set but next_clcc_ms in the future and
     * no fast window, wants_clcc is false (no raw urgent-OR term -> no busy-loop). */
    call_model_t s;
    model_with_active_leg(&s);
    call_model_set_now(&s, 1000u);
    s.reconcile_reasons = (uint32_t)CALL_RC_UNCERTAIN_TXN;  /* set WITHOUT a raise/schedule pull */
    s.next_clcc_ms = 100000u;
    s.fast_clcc_until_ms = 0u;
    s.shadow_open = false;
    assert_true(!call_model_wants_clcc(&s),
                "edge: a still-set reason with the schedule in the future does NOT re-poll (no busy-loop)");

    /* the empty-model + defer-in-flight gates still win even at a due schedule. */
    call_model_t e;
    tinit(&e);
    call_model_set_now(&e, 1000u);
    e.reconcile_reasons = (uint32_t)CALL_RC_COARSE_FINAL;
    e.next_clcc_ms = 0u;
    assert_true(!call_model_wants_clcc(&e), "edge: empty model never polls (gate wins)");

    call_model_t f;
    model_with_active_leg(&f);
    call_model_set_now(&f, 1000u);
    f.next_clcc_ms = 0u;                    /* due */
    f.txns[0].in_use = true;
    f.txns[0].state = TXN_DISPATCHED;      /* a call-control op mid-issue */
    assert_true(!call_model_wants_clcc(&f),
                "edge: defer behind an in-flight call-control txn (call-control outranks reconcile)");
}

/* After an ERROR the backoff must be honored -- a still-set UNCERTAIN does not
 * poll again until the backed-off next_clcc_ms deadline (the raw urgent-OR term used to
 * bypass this). */
static void test_error_backoff_honored(void) {
    call_model_t m;
    model_with_active_leg(&m);
    call_model_set_now(&m, 1000u);
    m.reconcile_reasons = (uint32_t)CALL_RC_UNCERTAIN_TXN;   /* an unresolved uncertain op */
    m.fast_clcc_until_ms = 0u;
    m.shadow_open = false;
    /* a CLCC errored twice -> backoff shift advances; the next issue interval is long. */
    call_model_clcc_error(&m);
    call_model_clcc_error(&m);
    assert_true(m.clcc_backoff_shift == 2u, "backoff: two ERRORs advance the shift to 2");
    call_model_clcc_begin(&m, 0u);          /* reschedules next_clcc_ms with the backed-off interval */
    m.shadow_open = false;                  /* isolate the scheduler (a real poll issues only when wants_clcc) */
    /* next_clcc_ms = 1000 + (4000 << 2) = 17000; the UNCERTAIN reason does NOT bypass it. */
    assert_true(m.next_clcc_ms == 1000u + (4000u << 2),
                "backoff: next poll scheduled at the backed-off interval (16 s)");
    call_model_set_now(&m, 5000u);          /* well before the 17000 deadline */
    assert_true(!call_model_wants_clcc(&m),
                "backoff: a still-set UNCERTAIN does NOT re-poll before the backed-off deadline");
    call_model_set_now(&m, 17000u);
    assert_true(call_model_wants_clcc(&m), "backoff: poll resumes at the backed-off deadline");
}

/* An abandon fired at now==0 (the 32-bit ms wrap, or the first ms after
 * boot) must still ARM the limbo clock (never-stamp-zero), so the leg IS
 * tick-limbo-evicted at the horizon -- the §6 bounded-drain backstop must hold WITHOUT
 * trusting CLCC. */
static void test_abandon_at_zero_arms_limbo(void) {
    call_model_t m;
    tinit(&m);
    call_model_set_now(&m, 0u);            /* now == 0 */
    /* A bound 2nd-MO DIAL, hard-abandoned by the app at now==0 (-> cm_abandon_leg),
     * built through the PUBLIC path (§16.7 needs ever_dispatched set at dispatch;
     * No hand-poked m.txns[i].token). */
    uint32_t tok = call_model_request(&m, CALL_TXN_DIAL, 0u, true);
    call_model_txn_dispatched(&m, tok);
    txn_ok(&m, tok);
    call_model_on_event(&m, 3u, true, CALL_LEG_DIALING, CALL_DIR_MO);   /* the ATD leg binds the 2nd-MO DIAL */
    call_model_new_call_abandoned(&m);     /* fires cm_abandon_leg at now==0 */
    assert_true(m.legs[0].id == 3u && m.legs[0].state == CALL_LEG_RELEASING,
                "abandon@0: bound setup leg -> RELEASING");
    assert_true(m.legs[0].pending_removal && m.legs[0].first_removal_ms == 1u,
                "abandon@0: limbo clock armed to 1 (never-stamp-zero), NOT 0");
    /* force-evicted at first_removal_ms(1) + limbo(8000) = 8001 -> backstop holds. */
    call_model_set_now(&m, 8001u);
    call_model_tick(&m);
    assert_true(!m.legs[0].in_use,
                "abandon@0: leg IS tick-limbo-evicted at the horizon (backstop not defeated)");
}

/* The fast-confirm window's raw `now < fast_clcc_until_ms -> return true`
 * branch in wants_clcc used to re-poll EVERY tick for the whole window (a busy
 * loop) and bypassed an ERROR backoff whenever a TXN_UNCERTAIN was live before
 * its policy_deadline. Post-fix: wants_clcc is purely schedule-based
 * (cm_time_reached against next_clcc_ms); the fast window only selects a
 * SHORTER interval, itself scaled by clcc_backoff_shift; and clcc_error
 * immediately re-schedules next_clcc_ms at the fresh backed-off interval (not
 * just at the next clcc_begin). */
static void test_fast_window_respects_backoff(void) {
    call_model_t m;
    model_with_active_leg(&m);
    call_model_set_now(&m, 1000u);
    m.txns[0].in_use = true;
    m.txns[0].state = TXN_UNCERTAIN;
    m.txns[0].policy_deadline_ms = 50000u;   /* far future -> fast window armed */
    m.txns[0].abandon_deadline_ms = 0u;
    m.shadow_open = false;
    call_model_tick(&m);
    assert_true(m.fast_clcc_until_ms > m.now_ms, "fast window armed by the live UNCERTAIN txn");

    /* two CLCC errors: backoff shift climbs to 2. */
    call_model_clcc_error(&m);
    call_model_clcc_error(&m);
    assert_true(m.clcc_backoff_shift == 2u, "two ERRORs advance the shift to 2");

    /* the error pushes next_clcc_ms out using the FRESH backed-off interval
     * right away -- and that interval is the backoff-scaled FAST interval
     * (300<<2 = 1200 ms) since we're still inside the fast window, NOT a flat
     * 300 ms ignoring the backoff. */
    assert_true(m.next_clcc_ms == 1000u + (300u << 2),
                "clcc_error re-schedules next_clcc_ms at the backoff-scaled fast interval");

    /* wants_clcc no longer busy-loops on the raw fast-window return-true --
     * at the SAME timestamp, still inside fast_clcc_until_ms, it must NOT
     * want a re-poll (next_clcc_ms is 2200, not yet reached). */
    assert_true(!call_model_wants_clcc(&m),
                "still inside the fast window at the SAME tick does not re-poll (busy-loop killed)");

    /* it resumes exactly at the backed-off deadline. */
    call_model_set_now(&m, 1000u + (300u << 2));
    assert_true(call_model_wants_clcc(&m), "poll resumes at the backoff-scaled deadline");

    /* a newly-raised reason still polls promptly (edge-pull unaffected by the
     * fast-window branch removal). */
    call_model_t f;
    model_with_active_leg(&f);
    call_model_set_now(&f, 2000u);
    f.next_clcc_ms = 999999u;
    f.shadow_open = false;
    call_model_on_bare_final(&f, MODEM_CALL_RESULT_NO_CARRIER, false);   /* raises COARSE_FINAL -> edge-pull */
    assert_true(call_model_wants_clcc(&f), "a freshly-raised reason still polls promptly");
}

/* With wants_clcc's raw `now < fast_clcc_until_ms` bypass removed, the fast
 * window's ONLY remaining route into next_clcc_ms became
 * clcc_begin/clcc_error -- a poll issued OUTSIDE the fast window (the normal
 * ~4 s keepalive, since a merely-tentative BOUND dial does not itself arm the
 * fast window) that THEN, within the SAME round-trip, has its txn confirmed
 * genuinely UNCERTAIN (a CLCC reveals dir=MT) left next_clcc_ms stranded at
 * that stale ~4 s-away keepalive deadline until the NEXT clcc_begin/error --
 * deferring the fast confirm by seconds instead of ~300 ms. call_model_tick
 * must pull next_clcc_ms IN to the fast interval once the window newly covers
 * `now`, without busy-looping and without bypassing an ERROR backoff. */
static void test_tick_pulls_next_poll_into_fast_window(void) {
    call_model_t m;
    tinit(&m);
    call_model_set_now(&m, 1000u);

    /* A DIAL binds TENTATIVELY to a fresh ACTIVE,dir-unknown leg (§7.4) --
     * still only TXN_BOUND, not yet TXN_UNCERTAIN, so no fast window is armed
     * yet. */
    uint32_t tok = call_model_request(&m, CALL_TXN_DIAL, 0u, false);
    call_model_txn_dispatched(&m, tok);
    call_model_on_event(&m, 2u, true, CALL_LEG_ACTIVE, CALL_DIR_UNKNOWN);

    /* the app's next periodic poll (an ordinary ~4 s keepalive -- issued
     * BEFORE any fast window exists) reveals dir=MT: the DIAL un-binds and its
     * txn goes genuinely TXN_UNCERTAIN, with its far (40 s) policy_deadline
     * arming a LONG fast-confirm window. */
    call_model_set_now(&m, 1050u);
    call_model_clcc_begin(&m, 0u);
    assert_true(m.next_clcc_ms == 1050u + 4000u,
                "precondition: the round-trip that reveals dir=MT was scheduled OUTSIDE any fast window");
    call_model_clcc_row(&m, 2u, CALL_DIR_MT, CALL_MODE_VOICE, 0u, CALL_LEG_INCOMING, "0700900000");
    call_model_clcc_ok(&m, 0u);

    /* a tick must now recompute the fast window AND pull next_clcc_ms into
     * it -- NOT leave it stranded at the stale ~4 s-away keepalive deadline. */
    call_model_tick(&m);
    assert_true(m.fast_clcc_until_ms > m.now_ms, "precondition: the fast window is now armed");
    assert_true(m.next_clcc_ms == 1050u + 300u,
                "the tick clamps next_clcc_ms down to the fast interval (~300 ms), not the stale 4 s keepalive");
    assert_true(!call_model_wants_clcc(&m),
                "does not busy-loop -- still false at the SAME timestamp (interval not yet elapsed)");

    /* idempotent: a second tick at the SAME timestamp does not shrink it further. */
    call_model_tick(&m);
    assert_true(m.next_clcc_ms == 1050u + 300u, "stable across repeated ticks at the same now (no busy-loop)");

    call_model_set_now(&m, 1050u + 300u);
    assert_true(call_model_wants_clcc(&m),
                "wants_clcc becomes true promptly at the fast interval (not deferred to the 4 s keepalive)");

    /* an ERROR backoff is still honored: the fast interval itself scales with
     * clcc_backoff_shift, and the clamp never pulls a poll in EARLIER than
     * that backed-off interval allows. */
    call_model_clcc_begin(&m, 1u);
    call_model_clcc_error(&m);                 /* shift -> 1 */
    assert_true(m.clcc_backoff_shift == 1u, "precondition: one ERROR advances the backoff shift to 1");
    assert_true(m.next_clcc_ms == 1050u + 300u + (300u << 1),
                "precondition: clcc_error reschedules at the backoff-scaled fast interval");
    call_model_tick(&m);
    assert_true(m.next_clcc_ms == 1050u + 300u + (300u << 1),
                "the clamp does NOT shrink the schedule below the backoff-scaled interval (backoff honored)");
    assert_true(!call_model_wants_clcc(&m), "still deferred at the SAME timestamp under backoff");
    call_model_set_now(&m, 1050u + 300u + (300u << 1));
    assert_true(call_model_wants_clcc(&m), "poll resumes exactly at the backed-off fast deadline");
}

/* cm_txn_in_flight used to gate on `state==TXN_PENDING ||
 * state==TXN_DISPATCHED` alone, ignoring at_accepted -- a write-only dead
 * field -- so a call-control op stayed "in flight" (deferring the recovery
 * CLCC) from dispatch all the way to its postcondition or the 40 s policy
 * deadline, long after the AT command's own OK had landed. §6 says defer
 * only while the AT COMMAND itself is in flight, which clears on OK. */
static void test_at_accepted_dispatched_not_inflight(void) {
    call_model_t m;
    tinit(&m);
    call_model_set_now(&m, 1000u);
    call_model_on_event(&m, 1u, true, CALL_LEG_INCOMING, CALL_DIR_MT);   /* a ringing leg */

    uint32_t tok = call_model_request(&m, CALL_TXN_ANSWER, 0u, false);
    call_model_txn_dispatched(&m, tok);        /* ATA written */
    txn_ok(&m, tok);          /* the modem's OK was seen (at_accepted) */

    /* the confirming ID-scoped ACTIVE event is LOST -- the leg stays INCOMING
     * and the txn stays DISPATCHED (never resolves). A recovery poll must NOT
     * be blocked behind it once the AT command itself is no longer in flight. */
    m.next_clcc_ms = m.now_ms;                 /* isolate the scheduler: a poll would be due */
    call_model_tick(&m);
    assert_true(call_model_wants_clcc(&m),
                "an at_accepted DISPATCHED txn (OK seen) no longer blocks the recovery CLCC");

    /* contrast: an UN-accepted DISPATCHED txn still correctly defers (the AT
     * command itself is genuinely in flight). */
    call_model_t m2;
    tinit(&m2);
    call_model_set_now(&m2, 1000u);
    call_model_on_event(&m2, 1u, true, CALL_LEG_INCOMING, CALL_DIR_MT);
    uint32_t tok2 = call_model_request(&m2, CALL_TXN_ANSWER, 0u, false);
    call_model_txn_dispatched(&m2, tok2);      /* NOT yet accepted */
    m2.next_clcc_ms = m2.now_ms;
    call_model_tick(&m2);
    assert_true(!call_model_wants_clcc(&m2),
                "an un-accepted DISPATCHED txn still defers the periodic CLCC");
}

/* §16.10: a CLCC backoff must advance the shift AND recompute next_clcc_ms TOGETHER; a
 * clean recovery resets BOTH. The transport-reject path in clcc_ok_commit was the probed
 * bug (shift climbed but next_clcc_ms stayed at the old, shorter deadline). */
static void test_backoff_deadline_moves_with_shift(void) {
    call_model_t m; tinit(&m); call_model_set_now(&m, 1000u);
    call_model_on_event(&m, 1u, true, CALL_LEG_ACTIVE, CALL_DIR_MT);   /* a live leg -> keepalive schedule */

    /* keepalive poll at shift 0: begin schedules now + 4000. */
    call_model_clcc_begin(&m, 0u);
    assert_true(m.next_clcc_ms == 1000u + 4000u, "begin schedules the shift-0 keepalive (5000)");
    /* a TRANSPORT-REJECT (the counter changed between begin and ok): shift 0->1 AND the
     * deadline must move out to now + (4000<<1) = 9000 together. */
    call_model_clcc_row(&m, 1u, CALL_DIR_MT, CALL_MODE_VOICE, 0u, CALL_LEG_ACTIVE, "5551234");
    call_model_clcc_ok(&m, 1u);
    assert_true(m.clcc_backoff_shift == 1u, "§16.10: a transport-reject advances the backoff shift");
    assert_true(m.next_clcc_ms == 1000u + 8000u,
                "§16.10: the transport-reject moves next_clcc_ms to the NEW backed-off deadline (9000)");

    /* a CLEAN recovery resets the shift AND pulls the deadline back to the shift-0 cadence. */
    call_model_clcc_begin(&m, 0u);
    assert_true(m.next_clcc_ms == 1000u + 8000u, "begin re-schedules at the backed-off shift-1 (9000)");
    call_model_clcc_row(&m, 1u, CALL_DIR_MT, CALL_MODE_VOICE, 0u, CALL_LEG_ACTIVE, "5551234");
    call_model_clcc_ok(&m, 0u);   /* transport-clean */
    assert_true(m.clcc_backoff_shift == 0u, "§16.10: a clean recovery resets the shift");
    assert_true(m.next_clcc_ms == 1000u + 4000u,
                "§16.10: a clean recovery resets next_clcc_ms to the shift-0 keepalive (5000)");
}

/* §16.11: a legitimate ACTIVE/HELD leg lives INDEFINITELY and is NEVER force-drained — a
 * real long call legitimately keeps the CLCC keepalive alive; that is not a drain failure.
 * The bounded-drain invariant is over TRANSIENT obligations only. */
static void test_steady_active_call_keeps_polling(void) {
    call_model_t m; tinit(&m); call_model_set_now(&m, 1000u);
    call_model_on_event(&m, 1u, true, CALL_LEG_ACTIVE, CALL_DIR_MO);   /* a genuine connected call */
    /* tick far into the future (a long call): the ACTIVE leg has NO max-age. */
    for (unsigned k = 0; k < 10u; k++) {
        call_model_set_now(&m, 1000u + (k + 1u) * 300000u);   /* +5 min each -> +50 min */
        call_model_tick(&m);
    }
    assert_true(m.legs[0].in_use && m.legs[0].id == 1u && m.legs[0].state == CALL_LEG_ACTIVE,
                "§16.11: a genuine ACTIVE leg is NEVER force-drained (no max-age)");
    m.next_clcc_ms = m.now_ms; m.shadow_open = false;
    assert_true(call_model_wants_clcc(&m),
                "§16.11: a steady ACTIVE call keeps the CLCC keepalive alive (not stopped prematurely)");
}

/* §16.11: a post-dispatch TOMBSTONE is a transient obligation — it keeps the keepalive
 * alive until a clean CLCC proves its cancelled command's leg absent, then RETIRES; with
 * no legitimate leg left, the model drains to empty and the keepalive STOPS. */
static void test_tombstone_drains_then_keepalive_stops(void) {
    call_model_t m; tinit(&m); call_model_set_now(&m, 1000u);
    uint32_t tok = call_model_request(&m, CALL_TXN_DIAL, 0u, false);
    call_model_txn_dispatched(&m, tok);
    call_model_txn_cancel(&m, tok);                          /* an UNBOUND post-dispatch tombstone */

    m.next_clcc_ms = m.now_ms; m.shadow_open = false;
    assert_true(call_model_wants_clcc(&m),
                "§16.11: an unbound tombstone keeps polling (it needs its clean-CLCC absence proof)");

    /* a clean (empty) CLCC proves the dispatched command created no leg -> cleanup confirmed. */
    call_model_clcc_begin(&m, 0u);
    call_model_clcc_ok(&m, 0u);
    call_model_set_now(&m, 2000u);
    call_model_tick(&m);                                     /* retires the drained tombstone */
    assert_true(find_txn_sched(&m, tok) == NULL, "§16.11: the tombstone retires on clean-CLCC absence");

    m.next_clcc_ms = m.now_ms; m.shadow_open = false;
    assert_true(!call_model_wants_clcc(&m),
                "§16.11: with all transient obligations drained + no legit leg, the keepalive STOPS");
}

int main(void) {
    test_idle_no_poll();
    test_service_policy_queries();
    test_keepalive_due();
    test_defer_inflight_txn();
    test_defer_shadow_open();
    test_clcc_begin_bumps_keepalive();
    test_fast_window_uncertain_then_backoff();
    test_backoff_exponential_and_cap();
    test_abandon_backstop_bound_leg();
    test_abandon_backstop_unbound();
    test_limbo_evict();
    test_pending_mt_backstop();
    test_reason_bits_cleared_when_consistent();
    test_pop_release_fifo();
    test_release_requeue_after_dispatch_admission_failure();
    test_reused_id_baseline_binds();
    test_stale_release_dropped();
    test_bounded_drain_invariant();
    test_bare_final_undispatched_no_backstopless_open();
    test_uncertain_late_connect_recovers();
    test_unbound_dial_abandon_failed();
    test_wants_clcc_edge_triggered();
    test_error_backoff_honored();
    test_abandon_at_zero_arms_limbo();
    test_fast_window_respects_backoff();
    test_tick_pulls_next_poll_into_fast_window();
    test_at_accepted_dispatched_not_inflight();
    test_backoff_deadline_moves_with_shift();
    test_steady_active_call_keeps_polling();
    test_tombstone_drains_then_keepalive_stops();
    if (s_failures == 0) {
        printf("test_call_model_sched: all assertions passed\n");
        return 0;
    }
    fprintf(stderr, "test_call_model_sched: %d assertion(s) failed\n", s_failures);
    return 1;
}

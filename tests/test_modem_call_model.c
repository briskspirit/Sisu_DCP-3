/* Host unit test for the CLCC call-table model skeleton.
 *
 * Single translation unit: we #include the .c under test directly so its
 * file-static helpers (added in later tasks) are reachable and the pure model
 * links with no hardware stubs. Compile (sanitizer recipe, run_tests.sh idiom):
 *   cc -std=c11 -I include -I tests/stubs -Wall -Wextra \
 *      -fsanitize=address,undefined -fno-sanitize-recover=all -g -O1 \
 *      tests/test_modem_call_model.c -o /tmp/t && /tmp/t
 *
 * ORACLE (spec-derived, NOT observed): call_model_init zeroes the model,
 * sets published = the R1 empty projection, next_token = 1.
 * Spec §9 -> last_published_roles initialised to R1 (all ids 0, all flags
 * false). Spec §9.2 R1 -> call_state IDLE, active_call_id 0, every flag false,
 * both result latches NONE. MODEM_CALL_IDLE and MODEM_CALL_RESULT_NONE are the
 * enum-zero constants (modem_service.h:47,56).
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../src/services/modem_call_model.c"
#include "call_timing_fixture.h"

/* §16.12: the model is initialized with a neutral timing profile supplied by
 * the test fixture. tinit wraps the common case. */
static void tinit(call_model_t *m) {
    call_timing_t _t = call_timing_fixture();
    call_model_init(m, &_t);
}
/* CMD_OK folds in the old txn_accepted (§16.2). */
static void txn_ok(call_model_t *m, uint32_t tok) {
    modem_cmd_result_t _r = { .status = CMD_OK, .has_call_result = false,
                              .call_result = MODEM_CALL_RESULT_NONE };
    call_model_txn_command_result(m, tok, _r);
}

static int s_failures;

static void assert_true(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        s_failures++;
    }
}

/* A freshly-inited model must carry the R1 empty projection and an empty
 * table/txn-pool/pending-MT/release state, with next_token seeded to 1. */
static void test_init_projects_r1_empty(void) {
    call_model_t m;
    /* Poison the storage so a missing field-zeroing is caught, not masked by
     * an already-zero stack. */
    memset(&m, 0xA5, sizeof(m));

    tinit(&m);

    /* published == R1 empty projection (§9.2 R1). */
    assert_true(m.published.call_state == MODEM_CALL_IDLE,
                "init: published.call_state == IDLE");
    assert_true(m.published.active_call_id == 0u,
                "init: published.active_call_id == 0");
    assert_true(m.published.call_on_hold == false,
                "init: published.call_on_hold == false");
    assert_true(m.published.second_call_held == false,
                "init: published.second_call_held == false");
    assert_true(m.published.waiting_call == false,
                "init: published.waiting_call == false");
    assert_true(m.published.ring_active == false,
                "init: published.ring_active == false");
    assert_true(m.published.caller_id_withheld == false,
                "init: published.caller_id_withheld == false");
    assert_true(m.published.incoming_diverted == false,
                "init: published.incoming_diverted == false");
    assert_true(m.published.incoming_number[0] == '\0',
                "init: published.incoming_number empty");
    assert_true(m.published.last_call_result == MODEM_CALL_RESULT_NONE,
                "init: published.last_call_result == NONE");
    assert_true(m.published.second_call_result == MODEM_CALL_RESULT_NONE,
                "init: published.second_call_result == NONE");

    /* next_token seeded to 1 (token 0 is the "rejected" sentinel of
     * call_model_request, contract line 150). */
    assert_true(m.next_token == 1u, "init: next_token == 1");

    /* Empty table / txn pool / pending-MT / scheduling. */
    for (unsigned i = 0u; i < MODEM_MAX_CALL_LEGS; i++) {
        assert_true(m.legs[i].in_use == false, "init: no leg in_use");
    }
    for (unsigned i = 0u; i < MODEM_MAX_CALL_TRANSACTIONS; i++) {
        assert_true(m.txns[i].in_use == false, "init: no txn in_use");
    }
    assert_true(m.pending_mt.active == false, "init: pending_mt inactive");
    assert_true(m.semantic_revision == 0u, "init: semantic_revision == 0");
    assert_true(m.overflow == false, "init: overflow == false");
    assert_true(m.reconcile_reasons == 0u, "init: reconcile_reasons == 0");
    assert_true(m.shadow_open == false, "init: shadow_open == false");
    assert_true(m.release_count == 0u, "init: release_count == 0");
    assert_true(m.now_ms == 0u, "init: now_ms == 0");

    /* §8 generation history seeded to 1 for every id (1..7). */
    assert_true(m.gen_next[1] == 1u, "init: gen_next[1] == 1");
    assert_true(m.gen_next[3] == 1u, "init: gen_next[3] == 1");
    assert_true(m.gen_next[MODEM_MAX_CALL_LEGS] == 1u, "init: gen_next[7] == 1");
    /* §9.5/§11 terminal journal + §5.4 shadow-invalidation start clear. */
    assert_true(m.journal_count == 0u, "init: journal_count == 0");
    assert_true(m.journal_incomplete == false, "init: journal_incomplete == false");
    assert_true(m.shadow_invalidated == false, "init: shadow_invalidated == false");
    /* new call_leg_t fields default-zero (poison-proof via the leading memset). */
    assert_true(m.legs[0].mode == CALL_MODE_UNKNOWN, "init: leg mode UNKNOWN");
    assert_true(m.legs[0].was_answered == false, "init: leg was_answered false");
    assert_true(m.legs[0].published_role == CALL_LEG_UNKNOWN, "init: leg published_role UNKNOWN");
}

/* call_model_set_now sets the clock and disturbs nothing else. */
static void test_set_now(void) {
    call_model_t m;
    tinit(&m);

    call_model_set_now(&m, 12345u);
    assert_true(m.now_ms == 12345u, "set_now: now_ms == 12345");
    assert_true(m.published.call_state == MODEM_CALL_IDLE,
                "set_now: projection untouched");
    assert_true(m.next_token == 1u, "set_now: next_token untouched");

    call_model_set_now(&m, 0u);
    assert_true(m.now_ms == 0u, "set_now: now_ms back to 0");
}

/* Defensive: NULL is a no-op, not a crash (ASan/UBSan would flag a deref). */
static void test_null_safe(void) {
    tinit(0);
    call_model_set_now(0, 42u);
    assert_true(true, "null-safe: no crash");
}

/* ==========================================================================
 * Transaction lifecycle tests (merged into this single-TU test; the
 * .c is #included above so the static engine helpers are reachable).
 * ORACLE: spec §7.1/§7.2/§7.3/§7.4 postconditions + locked contract sizes.
 * ========================================================================== */

/* Locate a transaction by token WITHOUT relying on the model's internal
 * txn_by_token (a missing helper fails as an assertion, not a build error). */
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
/* Seed a leg directly (public struct) = "the table after URC handling". */
static void seed_leg(call_model_t *m, unsigned slot, uint8_t id,
                     call_leg_state_t st, call_direction_t dir) {
    call_leg_t *l = &m->legs[slot];
    memset(l, 0, sizeof(*l));
    l->in_use = true; l->id = id; l->generation = 0u; l->state = st; l->dir = dir;
}

static void test_request_alloc(void) {
    call_model_t m;
    tinit(&m);
    call_model_set_now(&m, 1000u);

    uint32_t t1 = call_model_request(&m, CALL_TXN_DIAL, 0u, true);
    assert_true(t1 != 0u, "request(DIAL) returns a nonzero token");
    call_txn_t *tx = find_txn(&m, t1);
    assert_true(tx != NULL, "request created an in_use txn");
    assert_true(tx && tx->kind == CALL_TXN_DIAL, "txn kind = DIAL");
    assert_true(tx && tx->state == TXN_PENDING, "fresh txn state = PENDING (not dispatched)");
    assert_true(tx && tx->second_mo, "second_mo flag stored");
    assert_true(tx && tx->result == MODEM_CALL_RESULT_NONE, "fresh txn result = NONE");
}

static void test_request_pool_and_hangup_reserved(void) {
    call_model_t m;
    tinit(&m);
    call_model_set_now(&m, 0u);

    /* Pool = 12, one slot reserved for HANGUP -> exactly 11 general grants.
     * HOLD is used as a neutral token carrier: same-role DIAL attempts are now
     * deliberately single-owner and an unbound retry supersedes its predecessor. */
    unsigned granted = 0u;
    for (unsigned i = 0; i < 20u; i++) {
        if (call_model_request(&m, CALL_TXN_HOLD, 0u, false) != 0u) granted++;
    }
    assert_true(granted == MODEM_MAX_CALL_TRANSACTIONS - 1u,
                "general pool grants exactly MAX_TXN-1 (HANGUP slot reserved)");
    /* A further non-HANGUP op is rejected... */
    assert_true(call_model_request(&m, CALL_TXN_SWAP, 1u, false) == 0u,
                "non-HANGUP request rejected when general pool is full");
    /* ...but HANGUP is guaranteed its reserved slot. */
    assert_true(call_model_request(&m, CALL_TXN_HANGUP, 0u, false) != 0u,
                "HANGUP always granted the reserved slot");
}

static void test_dispatched_baseline_and_deadlines(void) {
    call_model_t m;
    tinit(&m);
    call_model_set_now(&m, 5000u);
    seed_leg(&m, 0u, 1u, CALL_LEG_ACTIVE, CALL_DIR_MO);   /* one live leg at dispatch */

    uint32_t tok = call_model_request(&m, CALL_TXN_DIAL, 0u, true);
    call_model_txn_dispatched(&m, tok);
    call_txn_t *tx = find_txn(&m, tok);
    assert_true(tx && tx->state == TXN_DISPATCHED, "dispatched -> state DISPATCHED");
    assert_true(tx && tx->baseline_count == 1u, "DIAL baseline captures the 1 live leg");
    assert_true(tx && tx->baseline_ids[0] == 1u && tx->baseline_gens[0] == 0u,
                "DIAL baseline id+generation = the live leg");
    /* Deadlines armed = dispatch time + the model constants (oracle: §7.1). */
    assert_true(tx && tx->policy_deadline_ms == 5000u + m.timing.txn_policy_ms,
                "policy_deadline = now + POLICY");
    assert_true(tx && tx->abandon_deadline_ms == 5000u + m.timing.txn_abandon_ms,
                "abandon_deadline = now + ABANDON");

    assert_true(tx && !tx->at_accepted, "at_accepted false before OK");
    txn_ok(&m, tok);
    assert_true(tx && tx->at_accepted, "accepted sets at_accepted (OK seen)");
}

/* Small helper: dispatch a fresh DIAL against the current table. */
static uint32_t dispatch_dial(call_model_t *m, bool second_mo) {
    uint32_t tok = call_model_request(m, CALL_TXN_DIAL, 0u, second_mo);
    call_model_txn_dispatched(m, tok);
    txn_ok(m, tok);
    return tok;
}

static void test_bind_dialing(void) {
    call_model_t m; tinit(&m); call_model_set_now(&m, 1000u);
    uint32_t tok = dispatch_dial(&m, false);        /* baseline empty */
    seed_leg(&m, 0u, 2u, CALL_LEG_DIALING, CALL_DIR_MO);  /* new leg appears */
    model_reconcile_txns(&m);
    call_txn_t *tx = find_txn(&m, tok);
    assert_true(tx && tx->bound_id == 2u, "DIAL binds to the new-outside-baseline leg");
    assert_true(tx && tx->state == TXN_BOUND, "bound DIALING leg -> state BOUND");
    assert_true(tx && tx->dir_confirmed, "DIALING implies MO -> dir_confirmed");
    assert_true(tx && !txn_is_terminal(tx), "DIALING (not ACTIVE) -> not yet SUCCEEDED");
}

static void test_dial_success_lost_dialing_recovery(void) {
    /* #5: the DIALING URC is lost; the first observation is ACTIVE(dir=MO). It
     * still binds AND resolves SUCCEEDED (no blind fail). */
    call_model_t m; tinit(&m); call_model_set_now(&m, 1000u);
    uint32_t tok = dispatch_dial(&m, false);
    seed_leg(&m, 0u, 2u, CALL_LEG_ACTIVE, CALL_DIR_MO);   /* only ever seen ACTIVE */
    model_reconcile_txns(&m);
    call_txn_t *tx = find_txn(&m, tok);
    assert_true(tx && tx->bound_id == 2u, "lost-DIALING: binds the first-seen ACTIVE MO leg");
    assert_true(tx && tx->state == TXN_SUCCEEDED, "MO-confirmed ACTIVE -> SUCCEEDED");
    assert_true(tx && tx->witnessed_active, "witnessed_active latched");
    assert_true(tx && tx->result == MODEM_CALL_RESULT_CONNECTED, "DIAL success result = CONNECTED");
}

static void test_tentative_dir_unconfirmed_then_confirm(void) {
    /* First seen ACTIVE with NO direction -> tentative bind; the
     * DIAL must NOT resolve SUCCEEDED until +CLCC <dir>==MO. */
    call_model_t m; tinit(&m); call_model_set_now(&m, 1000u);
    uint32_t tok = dispatch_dial(&m, false);
    seed_leg(&m, 0u, 2u, CALL_LEG_ACTIVE, CALL_DIR_UNKNOWN);
    model_reconcile_txns(&m);
    call_txn_t *tx = find_txn(&m, tok);
    assert_true(tx && tx->bound_id == 2u, "tentative: binds the sole new ACTIVE leg");
    assert_true(tx && tx->state == TXN_BOUND && !tx->dir_confirmed,
                "tentative: BOUND but dir NOT confirmed");
    assert_true(tx && !tx->witnessed_active, "tentative ACTIVE does not witness (may be MT)");
    assert_true(tx && tx->result == MODEM_CALL_RESULT_NONE, "tentative: not SUCCEEDED, no result");
    assert_true((m.reconcile_reasons & CALL_RC_UNCERTAIN_TXN) != 0u,
                "tentative sets fast reconcile (schedule CLCC)");

    /* +CLCC confirms dir==MO. */
    m.legs[0].dir = CALL_DIR_MO;
    model_reconcile_txns(&m);
    assert_true(tx && tx->dir_confirmed, "CLCC dir==MO confirms the tentative bind");
    assert_true(tx && tx->state == TXN_SUCCEEDED, "confirmed MO ACTIVE -> SUCCEEDED");
    assert_true(tx && tx->result == MODEM_CALL_RESULT_CONNECTED, "confirmed -> CONNECTED");
}

static void test_tentative_mt_reclassify(void) {
    /* If the confirming <dir> is MT the leg was a coincident incoming, not our
     * dial -> un-bind (DIAL -> UNCERTAIN); CLCC already set the leg's stat. */
    call_model_t m; tinit(&m); call_model_set_now(&m, 1000u);
    uint32_t tok = dispatch_dial(&m, false);
    seed_leg(&m, 0u, 2u, CALL_LEG_ACTIVE, CALL_DIR_UNKNOWN);
    model_reconcile_txns(&m);                       /* tentative bind */
    /* CLCC returns dir==MT + reclassifies the leg to INCOMING. */
    m.legs[0].dir = CALL_DIR_MT; m.legs[0].state = CALL_LEG_INCOMING;
    model_reconcile_txns(&m);
    call_txn_t *tx = find_txn(&m, tok);
    assert_true(tx && tx->bound_id == 0u, "MT: DIAL un-binds (does not claim the incoming leg)");
    assert_true(tx && tx->state == TXN_UNCERTAIN, "un-bound DIAL falls to UNCERTAIN");
    assert_true(tx && !txn_is_terminal(tx), "MT reclassify: DIAL not resolved SUCCEEDED");
    assert_true(m.legs[0].state == CALL_LEG_INCOMING && m.legs[0].dir == CALL_DIR_MT,
                "the incoming leg is left as CLCC classified it");
}

static void test_bind_ambiguity_no_guess(void) {
    call_model_t m; tinit(&m); call_model_set_now(&m, 1000u);
    uint32_t tok = dispatch_dial(&m, false);        /* baseline empty */
    seed_leg(&m, 0u, 2u, CALL_LEG_DIALING, CALL_DIR_MO);
    seed_leg(&m, 1u, 3u, CALL_LEG_DIALING, CALL_DIR_MO);   /* >=2 candidates */
    model_reconcile_txns(&m);
    call_txn_t *tx = find_txn(&m, tok);
    assert_true(tx && tx->bound_id == 0u, "ambiguity (>=2 new legs) never guesses a bind");
    assert_true((m.reconcile_reasons & CALL_RC_UNCERTAIN_TXN) != 0u,
                "ambiguity schedules CLCC");
}

static void test_other_op_postconditions(void) {
    /* ANSWER: incoming -> ACTIVE. */
    { call_model_t m; tinit(&m); call_model_set_now(&m, 0u);
      seed_leg(&m, 0u, 1u, CALL_LEG_INCOMING, CALL_DIR_MT);
      uint32_t tok = call_model_request(&m, CALL_TXN_ANSWER, 1u, false);
      call_model_txn_dispatched(&m, tok);
      m.legs[0].state = CALL_LEG_ACTIVE;
      model_reconcile_txns(&m);
      call_txn_t *tx = find_txn(&m, tok);
      assert_true(tx && tx->state == TXN_SUCCEEDED && tx->result == MODEM_CALL_RESULT_CONNECTED,
                  "ANSWER: incoming->ACTIVE -> SUCCEEDED/CONNECTED"); }

    /* HOLD toggle: ACTIVE -> HELD flips. */
    { call_model_t m; tinit(&m); call_model_set_now(&m, 0u);
      seed_leg(&m, 0u, 1u, CALL_LEG_ACTIVE, CALL_DIR_MO);
      uint32_t tok = call_model_request(&m, CALL_TXN_HOLD, 1u, false);
      call_model_txn_dispatched(&m, tok);          /* baseline hold-state = ACTIVE */
      m.legs[0].state = CALL_LEG_HELD;
      model_reconcile_txns(&m);
      assert_true(find_txn(&m, tok)->state == TXN_SUCCEEDED,
                  "HOLD: hold-state flip ACTIVE->HELD -> SUCCEEDED"); }

    /* SWAP: roles flip (active<->held), disambiguated by captured participants. */
    { call_model_t m; tinit(&m); call_model_set_now(&m, 0u);
      seed_leg(&m, 0u, 1u, CALL_LEG_ACTIVE, CALL_DIR_MO);
      seed_leg(&m, 1u, 2u, CALL_LEG_HELD,   CALL_DIR_MO);
      uint32_t tok = call_model_request(&m, CALL_TXN_SWAP, 1u, false);  /* target=active 1 */
      call_model_txn_dispatched(&m, tok);
      m.legs[0].state = CALL_LEG_HELD; m.legs[1].state = CALL_LEG_ACTIVE;
      model_reconcile_txns(&m);
      assert_true(find_txn(&m, tok)->state == TXN_SUCCEEDED, "SWAP: roles flipped -> SUCCEEDED"); }

    /* WAIT_ANSWER: waiting->ACTIVE, prior active->HELD. */
    { call_model_t m; tinit(&m); call_model_set_now(&m, 0u);
      seed_leg(&m, 0u, 1u, CALL_LEG_ACTIVE,  CALL_DIR_MO);
      seed_leg(&m, 1u, 2u, CALL_LEG_WAITING, CALL_DIR_MT);
      uint32_t tok = call_model_request(&m, CALL_TXN_WAIT_ANSWER, 2u, false); /* held=active 1 */
      call_model_txn_dispatched(&m, tok);
      m.legs[1].state = CALL_LEG_ACTIVE; m.legs[0].state = CALL_LEG_HELD;
      model_reconcile_txns(&m);
      assert_true(find_txn(&m, tok)->state == TXN_SUCCEEDED,
                  "WAIT_ANSWER: waiting->ACTIVE + prior active->HELD -> SUCCEEDED"); }

    /* WAIT_REJECT: waiting leg absent, active untouched. */
    { call_model_t m; tinit(&m); call_model_set_now(&m, 0u);
      seed_leg(&m, 0u, 1u, CALL_LEG_ACTIVE,  CALL_DIR_MO);
      seed_leg(&m, 1u, 2u, CALL_LEG_WAITING, CALL_DIR_MT);
      uint32_t tok = call_model_request(&m, CALL_TXN_WAIT_REJECT, 2u, false);
      call_model_txn_dispatched(&m, tok);
      m.legs[1].in_use = false;                    /* waiting leg gone */
      model_reconcile_txns(&m);
      assert_true(find_txn(&m, tok)->state == TXN_SUCCEEDED, "WAIT_REJECT: waiting absent -> SUCCEEDED"); }

    /* RELEASE_ACTIVE: active absent + held promoted to ACTIVE. */
    { call_model_t m; tinit(&m); call_model_set_now(&m, 0u);
      seed_leg(&m, 0u, 1u, CALL_LEG_ACTIVE, CALL_DIR_MO);
      seed_leg(&m, 1u, 2u, CALL_LEG_HELD,   CALL_DIR_MO);
      uint32_t tok = call_model_request(&m, CALL_TXN_RELEASE_ACTIVE, 1u, false); /* held=2 */
      call_model_txn_dispatched(&m, tok);
      assert_true(call_model_release_active_pending(&m),
                  "RELEASE_ACTIVE owns promotion while unresolved");
      m.legs[0].in_use = false;
      model_reconcile_txns(&m);
      assert_true(call_model_release_active_pending(&m),
                  "RELEASE_ACTIVE stays pending while its survivor is held");
      m.legs[1].state = CALL_LEG_ACTIVE;
      model_reconcile_txns(&m);
      assert_true(find_txn(&m, tok)->state == TXN_SUCCEEDED,
                  "RELEASE_ACTIVE: active gone + held promoted -> SUCCEEDED");
      assert_true(!call_model_release_active_pending(&m),
                  "RELEASE_ACTIVE releases promotion ownership at success"); }

    /* RELEASE_LEG: that leg absent. */
    { call_model_t m; tinit(&m); call_model_set_now(&m, 0u);
      seed_leg(&m, 0u, 3u, CALL_LEG_HELD, CALL_DIR_MO);
      uint32_t tok = call_model_request(&m, CALL_TXN_RELEASE_LEG, 3u, false);
      call_model_txn_dispatched(&m, tok);
      m.legs[0].in_use = false;
      model_reconcile_txns(&m);
      assert_true(find_txn(&m, tok)->state == TXN_SUCCEEDED, "RELEASE_LEG: target absent -> SUCCEEDED"); }

    /* HANGUP: all legs absent (table empty -> IDLE). */
    { call_model_t m; tinit(&m); call_model_set_now(&m, 0u);
      seed_leg(&m, 0u, 1u, CALL_LEG_ACTIVE, CALL_DIR_MO);
      uint32_t tok = call_model_request(&m, CALL_TXN_HANGUP, 0u, false);
      call_model_txn_dispatched(&m, tok);
      m.legs[0].in_use = false;
      model_reconcile_txns(&m);
      assert_true(find_txn(&m, tok)->state == TXN_SUCCEEDED, "HANGUP: all legs absent -> SUCCEEDED"); }
}

/* WAIT_ANSWER has two legitimate network outcomes after B answers: A may be
 * HELD, or the network may release A and leave B as the sole ACTIVE leg. The
 * latter is still success. If the transaction remains live, its abandon timer
 * eventually mistakes B for an abandoned setup leg and tears down the call the
 * user just answered. Drive the complete public event/transaction path so this
 * cannot be hidden by direct state pokes. */
static void test_wait_answer_peer_release_keeps_answered_leg(void) {
    call_model_t m;
    tinit(&m);
    call_model_set_now(&m, 1000u);

    call_model_on_event(&m, 1u, true, CALL_LEG_ACTIVE, CALL_DIR_MO);
    call_model_on_event(&m, 2u, true, CALL_LEG_WAITING, CALL_DIR_MT);
    uint32_t tok = call_model_request(&m, CALL_TXN_WAIT_ANSWER, 2u, false);
    assert_true(tok != 0u, "WAIT_ANSWER peer release: request admitted");
    call_model_txn_dispatched(&m, tok);
    txn_ok(&m, tok);
    call_txn_t *tx = find_txn(&m, tok);
    assert_true(tx != NULL, "WAIT_ANSWER peer release: transaction exists after OK");
    uint32_t abandon_deadline = tx != NULL ? tx->abandon_deadline_ms : 0u;

    call_model_on_event(&m, 2u, true, CALL_LEG_ACTIVE, CALL_DIR_MT);
    call_model_on_event(&m, 1u, true, CALL_LEG_RELEASING, CALL_DIR_MO);

    tx = find_txn(&m, tok);
    assert_true(tx == NULL || tx->state == TXN_SUCCEEDED,
                "WAIT_ANSWER succeeds when answered B is ACTIVE and prior A is released");
    assert_true(find_leg(&m, 2u) != NULL && find_leg(&m, 2u)->state == CALL_LEG_ACTIVE,
                "answered B remains the active leg after A releases");

    call_model_set_now(&m, abandon_deadline + 1u);
    call_model_tick(&m);
    assert_true(m.release_count == 0u,
                "expired WAIT_ANSWER bookkeeping never queues release of answered B");
    assert_true(find_leg(&m, 2u) != NULL && find_leg(&m, 2u)->state == CALL_LEG_ACTIVE,
                "answered B remains active after the old abandon deadline");
}

static void test_policy_deadline_uncertain(void) {
    /* Bound but still in setup past the policy deadline -> UNCERTAIN + fast CLCC. */
    call_model_t m; tinit(&m); call_model_set_now(&m, 1000u);
    uint32_t tok = dispatch_dial(&m, false);
    seed_leg(&m, 0u, 2u, CALL_LEG_DIALING, CALL_DIR_MO);
    model_reconcile_txns(&m);                                /* -> BOUND (DIALING) */
    assert_true(find_txn(&m, tok)->state == TXN_BOUND, "precondition: BOUND DIALING");

    call_model_set_now(&m, 1000u + m.timing.txn_policy_ms);     /* policy hit, abandon NOT */
    model_reconcile_txns(&m);
    call_txn_t *tx = find_txn(&m, tok);
    assert_true(tx && tx->state == TXN_UNCERTAIN, "past policy while in setup -> UNCERTAIN");
    assert_true(tx && !txn_is_terminal(tx), "UNCERTAIN stays bindable (not terminal)");
    assert_true((m.reconcile_reasons & CALL_RC_UNCERTAIN_TXN) != 0u,
                "UNCERTAIN sets fast reconcile reason");
}

static void test_abandon_deadline_not_orphaned(void) {
    /* The abandon_deadline fires while the bound leg is still DIALING ->
     * the leg becomes RELEASING with a queued id-scoped release, its projection RETAINED
     * (no no-row gap); the SINGLE deadline engine resolves the txn FAILED (never
     * UNCERTAIN). The leg is evicted later by its own release URC (or §5.3 limbo). */
    call_model_t m; tinit(&m); call_model_set_now(&m, 1000u);
    uint32_t tok = dispatch_dial(&m, false);
    seed_leg(&m, 0u, 2u, CALL_LEG_DIALING, CALL_DIR_MO);
    model_reconcile_txns(&m);                                /* -> BOUND */

    call_model_set_now(&m, 1000u + m.timing.txn_abandon_ms + 1u);
    model_reconcile_txns(&m);
    call_txn_t *tx = find_txn(&m, tok);
    assert_true(m.legs[0].state == CALL_LEG_RELEASING, "abandon: bound setup leg -> RELEASING");
    assert_true(m.release_count == 1u && m.release_queue[0].id == 2u &&
                !m.release_queue[0].recover_held_survivor,
                "abandon: queues an id-scoped release for the leg");
    assert_true(tx && tx->state == TXN_FAILED, "abandon deadline resolves the txn FAILED (single engine)");
    assert_true(tx && tx->result == MODEM_CALL_RESULT_NO_ANSWER,
                "abandon of a never-witnessed leg latches NO_ANSWER");

    /* The id-scoped RELEASED URC finally evicts the RELEASING leg (positive removal). */
    call_model_on_event(&m, 2u, true, CALL_LEG_RELEASING, CALL_DIR_UNKNOWN);
    assert_true(!m.legs[0].in_use, "release URC evicts the RELEASING leg");
}

/* An UNBOUND txn (no leg EVER appeared) dying at the abandon hard backstop
 * must still stamp a result -- the bound sibling above
 * (test_abandon_deadline_not_orphaned) always gets one via witnessed_active,
 * but an l==NULL branch that skips the stamp leaves t->result NONE
 * (no journal entry if this txn's leg never bound, and no app-visible
 * reason at all here). */
static void test_unbound_abandon_stamps_result(void) {
    call_model_t m; tinit(&m); call_model_set_now(&m, 1000u);
    uint32_t tok = dispatch_dial(&m, false);      /* no leg ever appears -> stays unbound */
    call_model_set_now(&m, 1000u + m.timing.txn_abandon_ms);
    model_reconcile_txns(&m);
    call_txn_t *tx = find_txn(&m, tok);
    assert_true(tx && tx->state == TXN_FAILED, "unbound DIAL past abandon_deadline -> FAILED");
    assert_true(tx && tx->result == MODEM_CALL_RESULT_NO_ANSWER,
                "an unbound txn dying at the abandon backstop still stamps a result (NO_ANSWER)");
}

/* WAIT_ANSWER(target 0) with NO pending-MT and no waiting leg
 * must fail fast -- a genuinely-absent participant, symmetric with ANSWER's
 * target-0 fast-fail (test_answer_zero_no_pending_mt_still_fails). Without a
 * target-0 branch in txn_resolve's WAIT_ANSWER case, the op silently stays
 * open (neither SUCCEEDED nor FAILED) until a policy/abandon deadline,
 * seconds to a minute later, instead of failing immediately. */
static void test_wait_answer_zero_no_pending_mt_fails(void) {
    call_model_t m; tinit(&m); call_model_set_now(&m, 1000u);
    uint32_t tok = call_model_request(&m, CALL_TXN_WAIT_ANSWER, 0u, false);
    call_model_txn_dispatched(&m, tok);
    model_reconcile_txns(&m);
    call_txn_t *tx = find_txn(&m, tok);
    assert_true(tx != NULL && tx->state == TXN_FAILED,
                "WAIT_ANSWER(0) with no pending-MT and no waiting leg fails immediately");
    assert_true(tx != NULL && tx->result == MODEM_CALL_RESULT_NO_ANSWER,
                "WAIT_ANSWER(0) fast-fail stamps NO_ANSWER");
}

/* WAIT_ANSWER(0) mirrors ANSWER's rebind-while-pending-MT
 * half too -- it must NOT fail immediately while a pending-MT episode could
 * still materialize the waiting leg (txn_bind_targets already rebinds
 * WAIT_ANSWER(0), just like ANSWER(0)). */
static void test_wait_answer_zero_pending_mt_holds(void) {
    call_model_t m; tinit(&m); call_model_set_now(&m, 1000u);
    call_model_on_ring(&m);                       /* pending-MT active, no leg id yet */
    uint32_t tok = call_model_request(&m, CALL_TXN_WAIT_ANSWER, 0u, false);
    call_model_txn_dispatched(&m, tok);
    model_reconcile_txns(&m);
    call_txn_t *tx = find_txn(&m, tok);
    assert_true(tx != NULL && !txn_is_terminal(tx),
                "WAIT_ANSWER(0) is held pending (not failed) while pending-MT is active");
}

static void test_new_call_abandoned_bound(void) {
    call_model_t m; tinit(&m); call_model_set_now(&m, 1000u);
    seed_leg(&m, 0u, 1u, CALL_LEG_ACTIVE, CALL_DIR_MO);      /* the surviving original */
    uint32_t tok = dispatch_dial(&m, true);                 /* 2nd-MO New-call */
    seed_leg(&m, 1u, 3u, CALL_LEG_DIALING, CALL_DIR_MO);     /* the 2nd leg */
    model_reconcile_txns(&m);                                /* binds the 2nd-MO DIAL to id 3 */
    assert_true(find_txn(&m, tok)->bound_id == 3u, "precondition: 2nd-MO DIAL bound to id 3");

    call_model_new_call_abandoned(&m);
    call_txn_t *tx = find_txn(&m, tok);
    assert_true(m.legs[1].state == CALL_LEG_RELEASING, "abandon: the 2nd-MO leg -> RELEASING");
    assert_true(m.release_count == 1u && m.release_queue[0].id == 3u &&
                m.release_queue[0].recover_held_survivor,
                "abandon: releases id 3 and recovers the held survivor");
    assert_true(call_model_new_call_cleanup_pending(&m, NULL),
                "bound New-call abandon remains pending until id 3 is gone");
    assert_true(tx && tx->state == TXN_CANCELLED, "app abandon -> txn CANCELLED");
    assert_true(m.published.second_call_result == MODEM_CALL_RESULT_NONE,
                "New-call abandon resets second_call_result to NONE (§9.5 :772)");
    assert_true(m.legs[0].state == CALL_LEG_ACTIVE, "abandon leaves the surviving original untouched");
}

static void test_new_call_abandoned_predispatch(void) {
    call_model_t m; tinit(&m); call_model_set_now(&m, 1000u);
    call_model_on_event(&m, 1u, true, CALL_LEG_ACTIVE, CALL_DIR_MO);
    uint32_t tok = call_model_request(&m, CALL_TXN_DIAL, 0u, true);
    assert_true(tok != 0u && find_txn(&m, tok) != NULL,
                "pre-dispatch abandon: second-MO request is admitted");

    call_model_new_call_abandoned(&m);
    assert_true(find_txn(&m, tok) == NULL,
                "pre-dispatch abandon reclaims the never-written transaction");
    assert_true(!call_model_new_call_cleanup_pending(&m, NULL),
                "pre-dispatch abandon creates no physical cleanup gate");
    assert_true(m.release_count == 0u,
                "pre-dispatch abandon queues no release");
}

static void test_new_call_abandoned_unbound(void) {
    /* No leg has appeared yet: retain a bindable tombstone because ATD crossed
     * the transport and its call can still surface after the app rolls back. */
    call_model_t m; tinit(&m); call_model_set_now(&m, 1000u);
    call_model_on_event(&m, 1u, true, CALL_LEG_ACTIVE, CALL_DIR_MO);
    uint32_t tok = dispatch_dial(&m, true);
    call_model_new_call_abandoned(&m);
    call_txn_t *tx = find_txn(&m, tok);
    assert_true(tx && tx->state == TXN_CANCELLED, "unbound app abandon -> CANCELLED directly");
    assert_true(m.release_count == 0u, "unbound abandon queues no release");
    uint8_t cleanup_id = 0xffu;
    assert_true(call_model_new_call_cleanup_pending(&m, &cleanup_id) &&
                cleanup_id == 0u,
                "unbound dispatched abandon gates mutations without inventing an id");

    call_model_on_event(&m, 3u, true, CALL_LEG_DIALING, CALL_DIR_MO);
    tx = find_txn(&m, tok);
    call_leg_t *late = find_leg(&m, 3u);
    assert_true(tx && tx->bound_id == 3u && late != NULL &&
                late->state == CALL_LEG_RELEASING,
                "a unique late MO setup leg binds to the cancellation tombstone");
    cleanup_id = 0u;
    assert_true(call_model_new_call_cleanup_pending(&m, &cleanup_id) &&
                cleanup_id == 3u,
                "the cleanup gate publishes the bound late-leg id");
    call_cleanup_release_t release = {0};
    assert_true(call_model_pop_release(&m, &release) && release.id == 3u &&
                release.recover_held_survivor,
                "late second-MO cleanup preserves the recover-held semantic");
    assert_true(call_model_new_call_cleanup_pending(&m, &cleanup_id),
                "taking the release intent does not treat command admission as removal");

    call_model_on_event(&m, 3u, true, CALL_LEG_RELEASING,
                        CALL_DIR_UNKNOWN);
    assert_true(!call_model_new_call_cleanup_pending(&m, &cleanup_id),
                "id-scoped removal clears the model-owned cleanup gate");
}

static void test_new_call_abandoned_retires_terminal_result(void) {
    /* Recorded bench ordering (2026-07-24): B's id-scoped release resolves the
     * second-MO transaction before calls_app acknowledges the failure. The
     * acknowledgment must still retire the result latch even though there is
     * no longer an open transaction to cancel. */
    call_model_t m; tinit(&m); call_model_set_now(&m, 1000u);
    call_model_on_event(&m, 1u, true, CALL_LEG_ACTIVE, CALL_DIR_MO);
    (void)dispatch_dial(&m, true);
    call_model_on_event(&m, 2u, true, CALL_LEG_DIALING, CALL_DIR_MO);
    call_model_on_event(&m, 1u, true, CALL_LEG_HELD, CALL_DIR_MO);
    call_model_on_event(&m, 2u, true, CALL_LEG_RELEASING, CALL_DIR_MO);
    assert_true(m.published.second_call_result == MODEM_CALL_RESULT_NO_ANSWER,
                "precondition: terminal second MO latches NO_ANSWER");

    call_model_new_call_abandoned(&m);
    assert_true(m.published.second_call_result == MODEM_CALL_RESULT_NONE,
                "rollback acknowledges a terminal second-MO result");
}

/* §7.4-style unbound wait: ANSWER(target 0) must not fail
 * immediately when only a pending-MT episode exists (RING/CLIP seen, no
 * id-bearing leg yet -- the realistic "user presses Answer before the
 * ID-scoped call event" race). It is HELD PENDING (mirrors DIAL's unbound wait)
 * until the INCOMING leg materializes, then rebinds + resolves SUCCEEDED once
 * it reaches ACTIVE (never FAILED). */
static void test_answer_zero_pending_mt_rebinds(void) {
    call_model_t m; tinit(&m); call_model_set_now(&m, 1000u);
    call_model_on_ring(&m);                      /* RING seen, no leg id yet -- via the public API */
    assert_true(m.pending_mt.active, "precondition: RING starts the pending-MT episode");

    uint32_t tok = call_model_request(&m, CALL_TXN_ANSWER, 0u, false);
    call_txn_t *tx = find_txn(&m, tok);
    assert_true(tx != NULL && tx->target_id == 0u,
                "precondition: no incoming leg yet -> target stays 0 at request time");
    call_model_txn_dispatched(&m, tok);
    model_reconcile_txns(&m);
    tx = find_txn(&m, tok);
    assert_true(tx != NULL && !txn_is_terminal(tx),
                "ANSWER(0) is HELD PENDING (not failed) while pending-MT is active");

    /* the INCOMING leg materializes (its ID-scoped event finally lands) --
     * via the public on_event API, not a hand-poked leg slot. */
    call_model_on_event(&m, 4u, true, CALL_LEG_INCOMING, CALL_DIR_MT);
    tx = find_txn(&m, tok);
    assert_true(tx != NULL && tx->target_id == 4u,
                "the ANSWER rebinds to the INCOMING leg once it materializes");

    /* the user's ANSWER completes the call -- again via on_event, driving the
     * real state transition rather than writing m.legs[...] directly. */
    call_model_on_event(&m, 4u, true, CALL_LEG_ACTIVE, CALL_DIR_MT);
    tx = find_txn(&m, tok);
    assert_true(tx != NULL && tx->state == TXN_SUCCEEDED && tx->result == MODEM_CALL_RESULT_CONNECTED,
                "ANSWER(0) resolves SUCCEEDED once the rebound leg reaches ACTIVE (never FAILED)");
}

/* ANSWER(0) with NO pending-MT and no incoming leg still FAILs
 * immediately -- a genuinely-absent participant, not a materializing one. */
static void test_answer_zero_no_pending_mt_still_fails(void) {
    call_model_t m; tinit(&m); call_model_set_now(&m, 1000u);
    uint32_t tok = call_model_request(&m, CALL_TXN_ANSWER, 0u, false);
    call_model_txn_dispatched(&m, tok);
    model_reconcile_txns(&m);
    call_txn_t *tx = find_txn(&m, tok);
    assert_true(tx != NULL && tx->state == TXN_FAILED,
                "ANSWER(0) with no pending-MT and no incoming leg still fails immediately");
}

/* A target_id of 0 resolves to the op's canonical live participant instead of
 * failing immediately. ANSWER(0) with a ringing leg binds+succeeds on it -> ACTIVE;
 * each target-0 op resolves the right participant; a genuinely-absent one still fails. */
static void test_target_zero_resolves(void) {
    /* ANSWER(0) -> the INCOMING leg; postcondition that leg ACTIVE -> SUCCEEDED. */
    { call_model_t m; tinit(&m); call_model_set_now(&m, 0u);
      seed_leg(&m, 0u, 4u, CALL_LEG_INCOMING, CALL_DIR_MT);
      uint32_t tok = call_model_request(&m, CALL_TXN_ANSWER, 0u, false);   /* target 0 */
      call_txn_t *tx = find_txn(&m, tok);
      assert_true(tx && tx->target_id == 4u, "ANSWER(0) resolves target to the incoming leg");
      call_model_txn_dispatched(&m, tok);
      m.legs[0].state = CALL_LEG_ACTIVE;
      model_reconcile_txns(&m);
      tx = find_txn(&m, tok);
      assert_true(tx && tx->state == TXN_SUCCEEDED && tx->result == MODEM_CALL_RESULT_CONNECTED,
                  "ANSWER(0) binds+succeeds on the ringing leg -> ACTIVE"); }

    /* HOLD(0) -> the sole active leg. */
    { call_model_t m; tinit(&m); call_model_set_now(&m, 0u);
      seed_leg(&m, 0u, 1u, CALL_LEG_ACTIVE, CALL_DIR_MO);
      uint32_t tok = call_model_request(&m, CALL_TXN_HOLD, 0u, false);
      assert_true(find_txn(&m, tok)->target_id == 1u, "HOLD(0) resolves the active leg");
      call_model_txn_dispatched(&m, tok);
      m.legs[0].state = CALL_LEG_HELD;
      model_reconcile_txns(&m);
      assert_true(find_txn(&m, tok)->state == TXN_SUCCEEDED, "HOLD(0) resolves+succeeds"); }

    /* SWAP(0) -> the active leg (held captured separately); roles flip -> SUCCEEDED. */
    { call_model_t m; tinit(&m); call_model_set_now(&m, 0u);
      seed_leg(&m, 0u, 1u, CALL_LEG_ACTIVE, CALL_DIR_MO);
      seed_leg(&m, 1u, 2u, CALL_LEG_HELD,   CALL_DIR_MO);
      uint32_t tok = call_model_request(&m, CALL_TXN_SWAP, 0u, false);
      call_txn_t *tx = find_txn(&m, tok);
      assert_true(tx && tx->target_id == 1u && tx->held_id == 2u,
                  "SWAP(0) resolves active target + held participant");
      call_model_txn_dispatched(&m, tok);
      m.legs[0].state = CALL_LEG_HELD; m.legs[1].state = CALL_LEG_ACTIVE;
      model_reconcile_txns(&m);
      assert_true(find_txn(&m, tok)->state == TXN_SUCCEEDED, "SWAP(0) resolves+succeeds"); }

    /* WAIT_ANSWER(0) -> the waiting leg (prior active captured as held). */
    { call_model_t m; tinit(&m); call_model_set_now(&m, 0u);
      seed_leg(&m, 0u, 1u, CALL_LEG_ACTIVE,  CALL_DIR_MO);
      seed_leg(&m, 1u, 2u, CALL_LEG_WAITING, CALL_DIR_MT);
      uint32_t tok = call_model_request(&m, CALL_TXN_WAIT_ANSWER, 0u, false);
      call_txn_t *tx = find_txn(&m, tok);
      assert_true(tx && tx->target_id == 2u && tx->held_id == 1u,
                  "WAIT_ANSWER(0) resolves the waiting target + prior-active held");
      call_model_txn_dispatched(&m, tok);
      m.legs[1].state = CALL_LEG_ACTIVE; m.legs[0].state = CALL_LEG_HELD;
      model_reconcile_txns(&m);
      assert_true(find_txn(&m, tok)->state == TXN_SUCCEEDED, "WAIT_ANSWER(0) resolves+succeeds"); }

    /* RELEASE_ACTIVE(0) -> the active leg; active gone + held promoted -> SUCCEEDED. */
    { call_model_t m; tinit(&m); call_model_set_now(&m, 0u);
      seed_leg(&m, 0u, 1u, CALL_LEG_ACTIVE, CALL_DIR_MO);
      seed_leg(&m, 1u, 2u, CALL_LEG_HELD,   CALL_DIR_MO);
      uint32_t tok = call_model_request(&m, CALL_TXN_RELEASE_ACTIVE, 0u, false);
      assert_true(find_txn(&m, tok)->target_id == 1u, "RELEASE_ACTIVE(0) resolves the active leg");
      call_model_txn_dispatched(&m, tok);
      m.legs[0].in_use = false; m.legs[1].state = CALL_LEG_ACTIVE;
      model_reconcile_txns(&m);
      assert_true(find_txn(&m, tok)->state == TXN_SUCCEEDED, "RELEASE_ACTIVE(0) resolves+succeeds"); }

    /* a genuinely-absent participant leaves target 0 (ANSWER(0) with NO incoming leg). */
    { call_model_t m; tinit(&m); call_model_set_now(&m, 0u);
      uint32_t tok = call_model_request(&m, CALL_TXN_ANSWER, 0u, false);
      assert_true(find_txn(&m, tok)->target_id == 0u,
                  "ANSWER(0) with no incoming leg leaves target 0 (genuinely absent)"); }
}

/* A fresh outgoing DIAL dispatch clears the stale published result latches +
 * pending_terminal so the new attempt does not surface the previous call's outcome. */
static void test_dial_dispatch_resets_stale(void) {
    call_model_t m; tinit(&m); call_model_set_now(&m, 1000u);
    m.published.last_call_result   = MODEM_CALL_RESULT_NO_CARRIER;   /* stale prior-call results */
    m.published.second_call_result = MODEM_CALL_RESULT_BUSY;
    m.pending_terminal             = MODEM_CALL_RESULT_NO_ANSWER;
    uint32_t tok = call_model_request(&m, CALL_TXN_DIAL, 0u, false);
    call_model_txn_dispatched(&m, tok);
    assert_true(m.published.last_call_result == MODEM_CALL_RESULT_NONE,
                "a fresh DIAL dispatch resets stale last_call_result");
    assert_true(m.published.second_call_result == MODEM_CALL_RESULT_NONE,
                "... and second_call_result");
    assert_true(m.pending_terminal == MODEM_CALL_RESULT_NONE,
                "... and pending_terminal");
}

/* §9.5: a 2nd-MO dial dispatched over a live CONNECTED original
 * must NOT blank last_call_result -- only second_call_result resets
 * ("leaves last_call_result at CONNECTED"). */
static void test_2ndmo_dispatch_keeps_primary_connected(void) {
    call_model_t m; tinit(&m); call_model_set_now(&m, 1000u);
    m.published.last_call_result = MODEM_CALL_RESULT_CONNECTED;   /* A is live+connected */
    m.published.second_call_result = MODEM_CALL_RESULT_BUSY;      /* stale prior 2nd-MO outcome */
    uint32_t tok = call_model_request(&m, CALL_TXN_DIAL, 0u, true);   /* 2nd-MO dial (B) */
    call_model_txn_dispatched(&m, tok);
    assert_true(m.published.last_call_result == MODEM_CALL_RESULT_CONNECTED,
                "a 2nd-MO dial dispatch does NOT blank the primary's last_call_result");
    assert_true(m.published.second_call_result == MODEM_CALL_RESULT_NONE,
                "second_call_result still resets unconditionally for the fresh 2nd-MO attempt");
}

/* §16.7: the model-side dispatch watchdog is REMOVED. A never-dispatched PENDING
 * txn is NOT force-cancelled by a timer — cancellation is queue-owned. So a
 * PENDING txn survives an arbitrarily-late tick unchanged, and the integration
 * reclaims it via call_model_txn_cancel (pre-dispatch: no leg, no release). */
static void test_pending_not_force_cancelled_queue_owns(void) {
    call_model_t m; tinit(&m); call_model_set_now(&m, 1000u);
    uint32_t tok = call_model_request(&m, CALL_TXN_DIAL, 0u, false);
    assert_true(tok != 0u, "precondition: request granted a token");
    call_txn_t *tx = find_txn(&m, tok);
    assert_true(tx && tx->state == TXN_PENDING,
                "precondition: a fresh, never-dispatched txn stays PENDING");

    /* A far-future tick must NOT force-cancel it (no watchdog anymore). */
    call_model_set_now(&m, 1000u + 10u * 60u * 1000u);   /* +10 min */
    call_model_tick(&m);
    tx = find_txn(&m, tok);
    assert_true(tx && tx->state == TXN_PENDING,
                "§16.7: a PENDING txn is NOT force-cancelled by a timer");
    assert_true(call_model_wants_clcc(&m),
                "§16.7: an open PENDING txn keeps the model non-empty (queue owns its cancel)");

    /* The queue reclaims it (§16.7 pre-dispatch: no AT sent, no leg -> the slot is
     * FREED IMMEDIATELY, no tombstone; the token no longer resolves). */
    call_model_txn_cancel(&m, tok);
    tx = find_txn(&m, tok);
    assert_true(tx == NULL,
                "§16.7: a pre-dispatch cancel reclaims the slot immediately (no tombstone)");
    assert_true(m.release_count == 0u, "§16.7: a pre-dispatch cancel queues no release");
    call_model_tick(&m);
    assert_true(!call_model_wants_clcc(&m),
                "§16.7: the model drains once the queue cancels the orphan");
}

/* A not-yet-dispatched (PENDING) txn must never
 * suppress the WHOLE model's CLCC keepalive -- a live leg coexisting with the
 * orphan still needs its own scheduled reconcile poll. If cm_txn_in_flight
 * counted TXN_PENDING as "in flight" unconditionally, wants_clcc would stay
 * false for as long as the orphan sat there even though the live leg had
 * nothing to do with it. */
static void test_orphan_pending_does_not_suppress_clcc(void) {
    call_model_t m; tinit(&m); call_model_set_now(&m, 1000u);
    seed_leg(&m, 0u, 1u, CALL_LEG_ACTIVE, CALL_DIR_MT);   /* a live leg needing reconciliation */
    m.next_clcc_ms = m.now_ms;                             /* isolate the scheduler: a poll is due */

    uint32_t tok = call_model_request(&m, CALL_TXN_DIAL, 0u, false);  /* the orphan: never dispatched */
    call_txn_t *tx = find_txn(&m, tok);
    assert_true(tx != NULL && tx->state == TXN_PENDING, "precondition: the orphan txn is PENDING");

    call_model_tick(&m);
    assert_true(call_model_wants_clcc(&m),
                "a live leg's due CLCC poll is NOT suppressed by a coexisting orphan PENDING txn");
}

/* call_model_txn_cancel(token) on a bound setup leg
 * -- the §7.3 queue-eviction path -- abandons the leg (RELEASING + queued
 * id-scoped release, same as call_model_new_call_abandoned's bound case) and
 * cancels the transaction, leaving an unrelated leg untouched. */
static void test_txn_cancel_bound_leg(void) {
    call_model_t m; tinit(&m); call_model_set_now(&m, 1000u);
    seed_leg(&m, 0u, 1u, CALL_LEG_ACTIVE, CALL_DIR_MO);       /* an unrelated survivor */
    uint32_t tok = dispatch_dial(&m, false);                  /* a primary DIAL setup */
    seed_leg(&m, 1u, 3u, CALL_LEG_DIALING, CALL_DIR_MO);      /* the setup leg materializes */
    model_reconcile_txns(&m);                                 /* binds the DIAL to id 3 */
    call_txn_t *tx = find_txn(&m, tok);
    assert_true(tx && tx->bound_id == 3u, "precondition: the DIAL is bound to leg 3");

    call_model_txn_cancel(&m, tok);
    tx = find_txn(&m, tok);
    assert_true(m.legs[1].state == CALL_LEG_RELEASING, "cancel: the bound setup leg -> RELEASING");
    assert_true(m.release_count == 1u && m.release_queue[0].id == 3u &&
                !m.release_queue[0].recover_held_survivor,
                "cancel: queues an id-scoped release for the bound leg");
    assert_true(tx && tx->state == TXN_CANCELLED, "cancel: the txn -> CANCELLED");
    assert_true(m.legs[0].state == CALL_LEG_ACTIVE, "cancel leaves an unrelated leg untouched");
}

/* §16.12: the timing profile passed to call_model_init takes effect — the model
 * reads cadence/policy/abandon/limbo from m->timing, NOT a baked vendor default.
 * A profile with values distinct from the host fixture proves it. */
static void test_timing_profile_takes_effect(void) {
    call_timing_t custom = { .clcc_keepalive_ms = 7000u, .clcc_confirm_ms = 100u,
                             .leg_limbo_ms = 5000u, .txn_policy_ms = 20000u,
                             .txn_abandon_ms = 60000u };

    /* (a) init copies the profile BY VALUE. */
    { call_model_t m; call_model_init(&m, &custom);
      assert_true(m.timing.clcc_keepalive_ms == 7000u && m.timing.clcc_confirm_ms == 100u &&
                  m.timing.leg_limbo_ms == 5000u && m.timing.txn_policy_ms == 20000u &&
                  m.timing.txn_abandon_ms == 60000u,
                  "timing: init copies the passed-in profile by value"); }

    /* (b) txn deadlines derive from the custom policy/abandon, not fixture values. */
    { call_model_t m; call_model_init(&m, &custom); call_model_set_now(&m, 1000u);
      uint32_t tok = dispatch_dial(&m, false);
      call_txn_t *tx = find_txn(&m, tok);
      assert_true(tx && tx->policy_deadline_ms == 1000u + 20000u,
                  "timing: policy_deadline uses the passed-in txn_policy_ms (20000)");
      assert_true(tx && tx->abandon_deadline_ms == 1000u + 60000u,
                  "timing: abandon_deadline uses the passed-in txn_abandon_ms (60000)"); }

    /* (c) CLCC keepalive cadence uses the custom 7000, not fixture 4000. */
    { call_model_t m; call_model_init(&m, &custom); call_model_set_now(&m, 1000u);
      seed_leg(&m, 0u, 1u, CALL_LEG_ACTIVE, CALL_DIR_MT);
      m.clcc_backoff_shift = 0u;
      call_model_tick(&m);                 /* no fast reason -> window closed */
      m.shadow_open = false;
      call_model_clcc_begin(&m, 0u);
      assert_true(m.next_clcc_ms == 1000u + 7000u,
                  "timing: clcc_begin bumps next_clcc_ms by the passed-in keepalive (7000)"); }

    /* (d) limbo horizon uses the custom 5000, not fixture 8000. */
    { call_model_t m; call_model_init(&m, &custom);
      seed_leg(&m, 0u, 2u, CALL_LEG_RELEASING, CALL_DIR_MT);
      m.legs[0].pending_removal = true; m.legs[0].first_removal_ms = 1000u;
      call_model_set_now(&m, 1000u + 4999u); call_model_tick(&m);
      assert_true(m.legs[0].in_use, "timing: pending_removal leg survives < first_removal+limbo");
      call_model_set_now(&m, 1000u + 5000u); call_model_tick(&m);
      assert_true(!m.legs[0].in_use, "timing: limbo evict uses the passed-in leg_limbo_ms (5000)"); }
}

/* §16.1: tokens are uint32_t, monotonic, collision-avoided among live records,
 * and 0 is reserved "none" — even across a 32-bit wrap. Driven via the public API. */
static void test_token_uint32_collision_avoidance(void) {
    call_model_t m; tinit(&m); call_model_set_now(&m, 0u);
    uint32_t a = call_model_request(&m, CALL_TXN_HOLD, 0u, false);
    uint32_t b = call_model_request(&m, CALL_TXN_HOLD, 0u, false);
    assert_true(a != 0u && b != 0u && a != b, "tokens are nonzero and unique");

    /* Force the allocator to WANT a live token next -> it must step over it. */
    m.next_token = a;
    uint32_t c = call_model_request(&m, CALL_TXN_HOLD, 0u, false);
    assert_true(c != 0u && c != a && c != b,
                "§16.1: allocation avoids collision with a live txn token");

    /* At the wrap boundary the allocator never hands out 0. */
    m.next_token = 0u;
    uint32_t d = call_model_request(&m, CALL_TXN_HOLD, 0u, false);
    assert_true(d != 0u && d != a && d != b && d != c,
                "§16.1: token 0 is reserved even at the 32-bit wrap");

    /* A value well above the uint16 range round-trips (proves the wider type). */
    call_model_t m2; tinit(&m2);
    m2.next_token = 70000u;
    uint32_t big = call_model_request(&m2, CALL_TXN_DIAL, 0u, false);
    assert_true(big == 70000u, "§16.1: tokens span the full 32-bit range (>65535)");
}

/* §16.5: every operation maps to a defined result sink. DIAL/ANSWER
 * own a latch (primary or 2nd-MO); every CHLD supplementary-service op and every
 * teardown op is OUTPUT_FREE (a rejected HOLD/SWAP/WAIT_REJECT never writes a call
 * result). Drives the total descriptor used by production routing. */
static void test_result_sink_enumeration(void) {
    call_txn_t t;
    struct {
        call_txn_kind_t kind;
        call_result_sink_t primary;
        call_result_sink_t second;
    } cases[] = {
        { CALL_TXN_DIAL,           RESULT_SINK_PRIMARY_LATCH, RESULT_SINK_SECOND_MO_LATCH },
        { CALL_TXN_ANSWER,         RESULT_SINK_PRIMARY_LATCH, RESULT_SINK_SECOND_MO_LATCH },
        { CALL_TXN_HOLD,           RESULT_SINK_OUTPUT_FREE,   RESULT_SINK_OUTPUT_FREE },
        { CALL_TXN_SWAP,           RESULT_SINK_OUTPUT_FREE,   RESULT_SINK_OUTPUT_FREE },
        { CALL_TXN_WAIT_ANSWER,    RESULT_SINK_OUTPUT_FREE,   RESULT_SINK_OUTPUT_FREE },
        { CALL_TXN_WAIT_REJECT,    RESULT_SINK_OUTPUT_FREE,   RESULT_SINK_OUTPUT_FREE },
        { CALL_TXN_RELEASE_ACTIVE, RESULT_SINK_OUTPUT_FREE,   RESULT_SINK_OUTPUT_FREE },
        { CALL_TXN_RELEASE_LEG,    RESULT_SINK_OUTPUT_FREE,   RESULT_SINK_OUTPUT_FREE },
        { CALL_TXN_HANGUP,         RESULT_SINK_OUTPUT_FREE,   RESULT_SINK_OUTPUT_FREE },
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        for (unsigned second = 0; second < 2u; second++) {
            memset(&t, 0, sizeof t);
            t.kind = cases[i].kind;
            t.second_mo = (bool)second;
            call_result_sink_t want = second ? cases[i].second : cases[i].primary;
            assert_true(cm_op_descriptor(&t).sink == want,
                        "§16.5: each op maps to its enumerated result sink");
        }
    }
}

/* ========================= §16.7 cancellation model ========================= */

/* §16.7: ever_dispatched is set ONLY when the real AT bytes go out
 * (call_model_txn_dispatched), never at request time, and never clears. */
static void test_ever_dispatched_epoch(void) {
    call_model_t m; tinit(&m); call_model_set_now(&m, 1000u);
    uint32_t tok = call_model_request(&m, CALL_TXN_DIAL, 0u, false);
    call_txn_t *tx = find_txn(&m, tok);
    assert_true(tx && !tx->ever_dispatched, "§16.7: a merely-requested (PENDING) txn is NOT ever_dispatched");
    call_model_txn_dispatched(&m, tok);
    tx = find_txn(&m, tok);
    assert_true(tx && tx->ever_dispatched, "§16.7: dispatch marks ever_dispatched");
    txn_ok(&m, tok);                                  /* a later command result never clears it */
    tx = find_txn(&m, tok);
    assert_true(tx && tx->ever_dispatched, "§16.7: ever_dispatched is immutable (never clears)");
}

/* §16.7: a POST-dispatch cancel of a BOUND setup DIAL leaves a bindable tombstone —
 * its leg -> RELEASING with an id-scoped release queued; the txn -> CANCELLED +
 * is_tombstone; output is trivially resolved (a cancel publishes no user result). */
static void test_cancel_post_dispatch_tombstone(void) {
    call_model_t m; tinit(&m); call_model_set_now(&m, 1000u);
    uint32_t tok = dispatch_dial(&m, false);                        /* dispatched -> ever_dispatched */
    call_model_on_event(&m, 3u, true, CALL_LEG_DIALING, CALL_DIR_MO);   /* the ATD leg materializes + binds */
    assert_true(find_txn(&m, tok) && find_txn(&m, tok)->bound_id == 3u,
                "precondition: the dispatched DIAL bound to leg 3");
    call_model_txn_cancel(&m, tok);
    call_txn_t *tx = find_txn(&m, tok);
    assert_true(tx && tx->state == TXN_CANCELLED, "§16.7 post-dispatch cancel -> CANCELLED");
    assert_true(tx && tx->is_tombstone, "§16.7 post-dispatch cancel -> a BINDABLE tombstone");
    assert_true(tx && tx->output_resolved, "§16.9: a tombstone is output-free (output_resolved)");
    assert_true(tx && tx->tombstone_deadline_ms != 0u, "§16.11: the tombstone's drain backstop is armed");
    call_leg_t *L = find_leg(&m, 3u);
    assert_true(L && L->state == CALL_LEG_RELEASING, "§16.7: the tombstone marks its bound leg RELEASING");
    assert_true(m.release_count == 1u && m.release_queue[0].id == 3u &&
                !m.release_queue[0].recover_held_survivor,
                "§16.7: the tombstone queues the leg's id-scoped release");
}

/* §16.7: a POST-dispatch hard-abandon (New-call) of an UNBOUND 2nd-MO DIAL (its
 * ATD went out but no leg was ever seen) still leaves a tombstone (ever_dispatched),
 * not a clean reclaim — its ATD may have created a leg the model hasn't seen. */
static void test_abandon_post_dispatch_unbound_tombstone(void) {
    call_model_t m; tinit(&m); call_model_set_now(&m, 1000u);
    seed_leg(&m, 0u, 1u, CALL_LEG_ACTIVE, CALL_DIR_MO);   /* the surviving original */
    uint32_t tok = dispatch_dial(&m, true);              /* 2nd-MO, dispatched, never bound */
    call_model_new_call_abandoned(&m);
    call_txn_t *tx = find_txn(&m, tok);
    assert_true(tx && tx->state == TXN_CANCELLED, "§16.7: dispatched-but-unbound abandon -> CANCELLED");
    assert_true(tx && tx->is_tombstone, "§16.7: dispatched abandon -> tombstone (ATD may have created a leg)");
    assert_true(m.release_count == 0u, "§16.7: an unbound tombstone queues no release yet");
    assert_true(m.published.second_call_result == MODEM_CALL_RESULT_NONE,
                "§9.5: New-call abandon resets second_call_result to NONE");
}

/* §16.7/§16.2: CMD_TIMEOUT is UNCERTAIN (still bindable, immediate CLCC) — NOT
 * cancellation: it creates NO tombstone. */
static void test_cmd_timeout_is_not_a_tombstone(void) {
    call_model_t m; tinit(&m); call_model_set_now(&m, 1000u);
    uint32_t tok = call_model_request(&m, CALL_TXN_DIAL, 0u, false);
    call_model_txn_dispatched(&m, tok);
    modem_cmd_result_t r = { .status = CMD_TIMEOUT };
    call_model_txn_command_result(&m, tok, r);
    call_txn_t *tx = find_txn(&m, tok);
    assert_true(tx && tx->state == TXN_UNCERTAIN, "§16.2: CMD_TIMEOUT -> UNCERTAIN");
    assert_true(tx && !tx->is_tombstone, "§16.7: CMD_TIMEOUT creates NO tombstone");
    assert_true(m.next_clcc_ms == m.now_ms, "§16.2: CMD_TIMEOUT edge-pulls an immediate CLCC");
}

/* ================ §16.9 tombstone lifecycle + two-axis retirement ================ */

/* §16.9: a post-dispatch tombstone is output-free (output_resolved) but its cleanup
 * axis is UNRESOLVED (its cancelled leg is not yet proven absent) — so it is NOT
 * retired by ticks; only when its leg's id-scoped release confirms absence does it
 * retire. (Requiring only "output resolved" would retire the very tombstone §16.7
 * needs to keep.) */
static void test_tombstone_held_until_cleanup_then_retires(void) {
    call_model_t m; tinit(&m); call_model_set_now(&m, 1000u);
    uint32_t tok = dispatch_dial(&m, false);                          /* dispatched primary DIAL */
    call_model_on_event(&m, 3u, true, CALL_LEG_DIALING, CALL_DIR_MO); /* binds leg 3 */
    call_model_txn_cancel(&m, tok);                                   /* post-dispatch -> tombstone */
    call_txn_t *tx = find_txn(&m, tok);
    assert_true(tx && tx->is_tombstone && tx->output_resolved,
                "§16.9: a tombstone is output-free (output_resolved true)");

    /* ticks must NOT retire it while its cancelled leg is still present. */
    call_model_set_now(&m, 1100u); call_model_tick(&m); call_model_tick(&m);
    assert_true(find_txn(&m, tok) != NULL,
                "§16.9: a tombstone is NOT retired while its cleanup axis is unresolved");
    assert_true(find_leg(&m, 3u) && find_leg(&m, 3u)->state == CALL_LEG_RELEASING,
                "the cancelled leg is retained RELEASING (projection kept) until authoritative absence");

    /* the leg's ID-scoped RELEASED event proves absence. */
    call_model_on_event(&m, 3u, true, CALL_LEG_RELEASING, CALL_DIR_MO);
    assert_true(find_leg(&m, 3u) == NULL, "the cancelled leg is evicted by its release URC");
    assert_true(m.published.second_call_result == MODEM_CALL_RESULT_NONE &&
                m.published.last_call_result != MODEM_CALL_RESULT_NO_CARRIER,
                "§16.9: a tombstone's leg eviction is output-free (no spurious failure latched)");
    call_model_set_now(&m, 1200u); call_model_tick(&m);
    assert_true(find_txn(&m, tok) == NULL,
                "§16.9: the tombstone retires once BOTH axes resolve (leg proven absent)");
}

/* §16.9: a SUCCEEDED DIAL is RETAINED while its bound leg lives (its ownership is
 * needed for the departure latch), then RETIRES once the leg departs — it must not
 * remain in_use forever after its outcome was projected. */
static void test_succeeded_dial_retires_after_leg_departs(void) {
    call_model_t m; tinit(&m); call_model_set_now(&m, 1000u);
    uint32_t tok = dispatch_dial(&m, false);
    call_model_on_event(&m, 1u, true, CALL_LEG_DIALING, CALL_DIR_MO);
    call_model_on_event(&m, 1u, true, CALL_LEG_ACTIVE, CALL_DIR_MO);   /* connects -> SUCCEEDED */
    call_txn_t *tx = find_txn(&m, tok);
    assert_true(tx && tx->state == TXN_SUCCEEDED, "precondition: the DIAL SUCCEEDED on connect");

    call_model_set_now(&m, 2000u); call_model_tick(&m); call_model_tick(&m);
    assert_true(find_txn(&m, tok) != NULL,
                "§16.9: a SUCCEEDED DIAL is retained while its leg lives (cleanup axis open)");

    call_model_on_event(&m, 1u, true, CALL_LEG_RELEASING, CALL_DIR_MO);   /* the call ends */
    assert_true(find_leg(&m, 1u) == NULL, "the leg departs");
    call_model_set_now(&m, 3000u); call_model_tick(&m);
    assert_true(find_txn(&m, tok) == NULL,
                "§16.9: the SUCCEEDED DIAL retires once its leg departs (no longer lingers in_use)");
}

/* §16.7: a tombstone is BINDABLE — the ATD-created leg that shows up LATE (after the
 * cancel) is recognized against the DIAL baseline and RELEASED (never resurrected). */
static void test_tombstone_releases_late_leg(void) {
    call_model_t m; tinit(&m); call_model_set_now(&m, 1000u);
    uint32_t tok = dispatch_dial(&m, false);        /* dispatched, no leg seen yet */
    call_model_txn_cancel(&m, tok);                 /* post-dispatch -> unbound tombstone */
    call_txn_t *tx = find_txn(&m, tok);
    assert_true(tx && tx->is_tombstone && tx->bound_id == 0u,
                "precondition: an UNBOUND tombstone (its ATD leg not yet seen)");
    assert_true(m.release_count == 0u, "no release queued yet (no leg)");

    /* the ATD leg appears late (a delayed ID-scoped DIALING event). */
    call_model_on_event(&m, 4u, true, CALL_LEG_DIALING, CALL_DIR_MO);
    tx = find_txn(&m, tok);
    assert_true(tx && tx->bound_id == 4u, "§16.7: the tombstone binds the late ATD leg (by baseline)");
    call_leg_t *L = find_leg(&m, 4u);
    assert_true(L && L->state == CALL_LEG_RELEASING,
                "§16.7: the late leg is RELEASED, never resurrected into a call");
    assert_true(m.release_count == 1u && m.release_queue[0].id == 4u &&
                !m.release_queue[0].recover_held_survivor,
                "§16.7: an id-scoped release is queued for the late leg");
}

/* §16.7 "tombstone binds the WRONG leg": a post-dispatch DIAL tombstone
 * must never grab a leg that a DIFFERENT live transaction already owns. Realistic
 * trigger: abandon a New-call (B), then immediately re-dial (C) BEFORE a clean CLCC
 * drains B's tombstone (the window widens to the 120 s abandon deadline under this
 * project's slow/stuck-CLCC-after-a-call regime). txn_bind_dials runs before
 * cm_bind_tombstones, so C's fresh DIAL has already claimed leg 4; the tombstone must
 * respect that claim and defer (n=0), not release the call the user is actively placing.
 * Without the cm_leg_owned_by_other_txn filter, B's tombstone releases C's leg. */
static void test_tombstone_spares_unrelated_redial_leg(void) {
    call_model_t m; tinit(&m); call_model_set_now(&m, 1000u);
    call_model_on_event(&m, 1u, true, CALL_LEG_ACTIVE, CALL_DIR_MO);   /* A: the surviving original */
    uint32_t tokB = dispatch_dial(&m, true);                          /* B: 2nd-MO New-call, dispatched, no leg */
    call_model_txn_cancel(&m, tokB);                                  /* abandon B -> unbound tombstone */
    assert_true(find_txn(&m, tokB) && find_txn(&m, tokB)->is_tombstone &&
                find_txn(&m, tokB)->bound_id == 0u,
                "precondition: B is an UNBOUND post-dispatch tombstone");

    uint32_t tokC = dispatch_dial(&m, true);                          /* C: the user immediately re-dials */
    call_model_on_event(&m, 4u, true, CALL_LEG_DIALING, CALL_DIR_MO); /* C's fresh MO leg appears */
    call_model_tick(&m);

    call_leg_t *L = find_leg(&m, 4u);
    assert_true(L && L->state == CALL_LEG_DIALING,
                "C's fresh dial leg SURVIVES (not RELEASED by B's tombstone)");
    call_txn_t *C = find_txn(&m, tokC);
    assert_true(C && C->bound_id == 4u && !txn_is_terminal(C),
                "C's DIAL owns leg 4 and is still live (not FAILED)");
    assert_true(find_txn(&m, tokB) && find_txn(&m, tokB)->bound_id == 0u,
                "B's tombstone did NOT grab C's leg (stays unbound, defers to CLCC)");
    assert_true(m.release_count == 0u, "no id-scoped release queued for C's leg");
}

/* §16.7: a leg already RELEASING from a post-dispatch cancel/abandon
 * must NOT be resurrected by a late live-state URC (a genuine connect-then-cancel race).
 * The URC path needs the same `!leg_releasing` guard the CLCC merge has -- without it, a
 * late ID-scoped ACTIVE event flips the cancelled leg back to ACTIVE and the cancelled
 * call projects as live for the whole ~8 s limbo window. */
static void test_no_urc_resurrect_cancelled_leg(void) {
    call_model_t m; tinit(&m); call_model_set_now(&m, 1000u);
    uint32_t tok = dispatch_dial(&m, false);
    call_model_on_event(&m, 1u, true, CALL_LEG_DIALING, CALL_DIR_MO);   /* binds leg1 to the DIAL */
    call_model_txn_cancel(&m, tok);                                     /* post-dispatch -> tombstone, leg1 RELEASING */
    call_leg_t *L = find_leg(&m, 1u);
    assert_true(L && L->state == CALL_LEG_RELEASING,
                "precondition: the cancelled leg is RELEASING");
    call_model_on_event(&m, 1u, true, CALL_LEG_ACTIVE, CALL_DIR_MO);    /* late connect URC for the cancelled leg */
    L = find_leg(&m, 1u);
    assert_true(L && L->state == CALL_LEG_RELEASING,
                "§16.7: a late ACTIVE URC does NOT resurrect the RELEASING cancelled leg");
    call_projection_t p; call_model_project(&m, &p);
    assert_true(p.call_state != MODEM_CALL_ACTIVE,
                "§16.7: the cancelled call is NOT projected as a live ACTIVE call");
    assert_true(p.active_call_id != 1u,
                "§16.7: the cancelled leg is not projected as the active call");
}

/* §16.3/§16.4: an UNATTRIBUTABLE later bare final (0/>=2 eligible
 * owners) must NOT clobber a register still owned by a LIVE setup txn. A primary DIAL A
 * gets a bare BUSY (sole owner -> register owned by A); then a concurrent 2nd dial makes
 * a trailing bare NO_CARRIER unattributable. Unconditionally overwriting the register
 * would destroy A's BUSY attribution and later mis-latch the call log (NO_CARRIER over
 * a real BUSY). */
static void test_unattributable_final_preserves_owned_register(void) {
    call_model_t m; tinit(&m); call_model_set_now(&m, 1000u);
    uint32_t tokA = dispatch_dial(&m, false);                          /* primary DIAL A, dispatched, no leg */
    call_model_on_bare_final(&m, MODEM_CALL_RESULT_BUSY, false);        /* sole eligible -> BUSY owned by A */
    assert_true(m.pending_terminal == MODEM_CALL_RESULT_BUSY && m.pending_terminal_owner_token == tokA,
                "precondition: the BUSY register is owned by A's token");
    (void)dispatch_dial(&m, true);                                     /* a concurrent 2nd dial -> 2 eligible owners */
    call_model_on_bare_final(&m, MODEM_CALL_RESULT_NO_CARRIER, false);  /* UNATTRIBUTABLE (>=2 owners) */
    assert_true(m.pending_terminal == MODEM_CALL_RESULT_BUSY,
                "§16.4: an unattributable bare final does NOT clobber A's owned BUSY register");
    assert_true(m.pending_terminal_owner_token == tokA,
                "§16.4: A keeps ownership of the register (its cause is not destroyed)");
}

/* §16.4/§16.5: the UNBOUND hard-abandon path must ROUTE a recorded
 * cause through its sink before retiring — not silently drop it.
 *  (a) a 2nd-MO cause while the original survives -> second_call_result LATCHES it
 *      (the SECOND_MO sink is independent). */
static void test_unbound_abandon_routes_2ndmo_cause(void) {
    call_model_t m; tinit(&m); call_model_set_now(&m, 1000u);
    call_model_on_event(&m, 1u, true, CALL_LEG_ACTIVE, CALL_DIR_MO);   /* the surviving original A */
    uint32_t tok = dispatch_dial(&m, true);                           /* 2nd-MO New-call, dispatched */
    /* the 2nd dial never binds; a bare final records its provisional BUSY (sole open setup owner). */
    call_model_on_bare_final(&m, MODEM_CALL_RESULT_BUSY, false);
    assert_true(find_txn(&m, tok) && find_txn(&m, tok)->result == MODEM_CALL_RESULT_BUSY,
                "precondition: the 2nd-MO DIAL recorded a provisional BUSY");
    /* the ABANDON deadline resolves it (no clean CLCC ever came). */
    call_model_set_now(&m, 1000u + 120000u + 1u);
    call_model_tick(&m);
    assert_true(m.published.second_call_result == MODEM_CALL_RESULT_BUSY,
                "the unbound 2nd-MO abandon ROUTES its BUSY to second_call_result (not dropped)");
    assert_true(m.published.last_call_result != MODEM_CALL_RESULT_BUSY,
                "the surviving original's primary latch is untouched");
}

/* (b) — the surviving-call guard: a PRIMARY provisional cause + a live
 * ACTIVE leg + abandon-deadline-first must NOT route the failed dial's cause onto the
 * primary latch (which would corrupt the live call's projection). A
 * primary (non-2nd-MO) DIAL failing over a live foreground call is OUT-OF-CONTRACT (a
 * primary dial implies no pre-existing call — and its own dispatch already reset the
 * primary latch to NONE, §9.5 fresh-attempt reset); the guard is the correct behavior.
 * The discriminator: WITHOUT the guard,
 * the abandon would route BUSY onto last_call_result; WITH it, the live call's latch
 * (NONE while dialing over it) is left intact. */
static void test_primary_cause_over_live_leg_not_clobbered(void) {
    call_model_t m; tinit(&m); call_model_set_now(&m, 1000u);
    call_model_on_event(&m, 1u, true, CALL_LEG_ACTIVE, CALL_DIR_MT);   /* live call A */
    uint32_t tok = dispatch_dial(&m, false);                          /* a PRIMARY dial (out-of-contract here) */
    call_model_on_bare_final(&m, MODEM_CALL_RESULT_BUSY, false);      /* records the DIAL's provisional BUSY */
    assert_true(find_txn(&m, tok) && find_txn(&m, tok)->result == MODEM_CALL_RESULT_BUSY,
                "precondition: the primary DIAL recorded a provisional BUSY");
    call_model_set_now(&m, 1000u + 120000u + 1u);
    call_model_tick(&m);
    assert_true(m.published.last_call_result != MODEM_CALL_RESULT_BUSY,
                "guard: a PRIMARY cause is NOT routed onto the primary latch over a live call");
    assert_true(find_leg(&m, 1u) != NULL && find_leg(&m, 1u)->state == CALL_LEG_ACTIVE,
                "the surviving call A is untouched by the abandoned primary dial");
}

/* =============== §16.5 total participant descriptor + dispatch guard =============== */

/* §16.5: EVERY op kind has a described (non-UNSET) resolution epoch + a defined result
 * sink — no default fall-through. (The compile guard is -Wswitch on cm_op_descriptor;
 * this is the runtime totality check.) */
static void test_op_descriptor_totality(void) {
    call_txn_kind_t kinds[] = { CALL_TXN_DIAL, CALL_TXN_ANSWER, CALL_TXN_HOLD, CALL_TXN_SWAP,
        CALL_TXN_WAIT_ANSWER, CALL_TXN_WAIT_REJECT, CALL_TXN_RELEASE_ACTIVE,
        CALL_TXN_RELEASE_LEG, CALL_TXN_HANGUP };
    for (unsigned i = 0; i < sizeof(kinds)/sizeof(kinds[0]); i++) {
        for (unsigned sm = 0; sm < 2u; sm++) {
            call_txn_t t; memset(&t, 0, sizeof t); t.kind = kinds[i]; t.second_mo = (bool)sm;
            call_op_descriptor_t d = cm_op_descriptor(&t);
            assert_true(d.epoch != EPOCH_UNSET,
                        "§16.5: every op kind has a described (non-UNSET) resolution epoch");
            assert_true(d.sink >= RESULT_SINK_PRIMARY_LATCH &&
                        d.sink <= RESULT_SINK_OUTPUT_FREE,
                        "§16.5: every op kind has a valid result sink");
        }
    }
    /* the DIAL new-active baseline epoch is AT_DISPATCH; targeted ops are AT_ADMISSION;
     * incoming-call ops are PENDING_MT_EPISODE. */
    call_txn_t d; memset(&d,0,sizeof d); d.kind = CALL_TXN_DIAL;
    assert_true(cm_op_descriptor(&d).epoch == EPOCH_AT_DISPATCH, "DIAL epoch = AT_DISPATCH");
    call_txn_t a; memset(&a,0,sizeof a); a.kind = CALL_TXN_ANSWER;
    assert_true(cm_op_descriptor(&a).epoch == EPOCH_PENDING_MT_EPISODE, "ANSWER epoch = PENDING_MT_EPISODE");
    call_txn_t h; memset(&h,0,sizeof h); h.kind = CALL_TXN_HOLD;
    assert_true(cm_op_descriptor(&h).epoch == EPOCH_AT_ADMISSION, "HOLD epoch = AT_ADMISSION");
    call_txn_t none; memset(&none,0,sizeof none); none.kind = CALL_TXN_NONE;
    assert_true(cm_op_descriptor(&none).epoch == EPOCH_UNSET, "the CALL_TXN_NONE sentinel -> EPOCH_UNSET");

    call_model_t m; tinit(&m);
    assert_true(call_model_request(&m, CALL_TXN_NONE, 0u, false) == 0u,
                "CALL_TXN_NONE is a rejected sentinel, never a live transaction");
    for (unsigned i = 0u; i < MODEM_MAX_CALL_TRANSACTIONS; i++) {
        assert_true(!m.txns[i].in_use,
                    "rejecting CALL_TXN_NONE leaves the transaction pool untouched");
    }
    assert_true(call_model_request(&m, CALL_TXN_RELEASE_LEG,
                                   (uint8_t)(MODEM_MAX_CALL_LEGS + 1u), false) == 0u,
                "an out-of-domain target id is rejected at model admission");
    for (unsigned i = 0u; i < MODEM_MAX_CALL_TRANSACTIONS; i++) {
        assert_true(!m.txns[i].in_use,
                    "rejecting an invalid target leaves the transaction pool untouched");
    }
    assert_true(call_model_request(NULL, CALL_TXN_DIAL, 0u, false) == 0u,
                "a null model rejects request admission safely");
}

/* §16.5 totality: HOLD(0) resolves the SOLE leg whether ACTIVE or HELD (vendor toggle) —
 * a sole HELD leg is RETRIEVED, not left unresolved. */
static void test_hold_zero_resolves_sole_held(void) {
    call_model_t m; tinit(&m); call_model_set_now(&m, 1000u);
    call_model_on_event(&m, 1u, true, CALL_LEG_HELD, CALL_DIR_MT);   /* the SOLE leg is HELD */
    uint32_t tok = call_model_request(&m, CALL_TXN_HOLD, 0u, false);
    call_txn_t *tx = find_txn(&m, tok);
    assert_true(tx && tx->target_id == 1u,
                "§16.5: HOLD(0) resolves the sole HELD leg (not just ACTIVE)");
    assert_true(call_model_txn_dispatch_guard(&m, tok) == CALL_DISPATCH_SEND,
                "§16.5 guard: HOLD SENDs while its target leg is live");
    call_model_txn_dispatched(&m, tok); txn_ok(&m, tok);
    call_model_on_event(&m, 1u, true, CALL_LEG_ACTIVE, CALL_DIR_MT);   /* toggle retrieves it */
    tx = find_txn(&m, tok);
    assert_true(tx && tx->state == TXN_SUCCEEDED,
                "§16.5: the hold toggle retrieves the sole HELD leg (hold-state flip)");
}

/* §16.5 dispatch guard: ANSWER(0) binds the pending-MT episode -> SEND while the episode
 * is live; STALE (write suppressed, txn resolved) once the episode is gone (the caller
 * gave up while the ATA sat behind a long op). */
static void test_answer_zero_guard_episode(void) {
    call_model_t m; tinit(&m); call_model_set_now(&m, 1000u);
    call_model_on_ring(&m);                                   /* incoming episode, no leg id yet */
    uint32_t tok = call_model_request(&m, CALL_TXN_ANSWER, 0u, false);
    assert_true(find_txn(&m, tok) && find_txn(&m, tok)->target_id == 0u,
                "precondition: ANSWER(0) has no leg id yet (episode-bound)");
    assert_true(call_model_txn_dispatch_guard(&m, tok) == CALL_DISPATCH_SEND,
                "§16.5 guard: ANSWER(0) SENDs while the pending-MT episode is live");
    /* the episode times out (caller hangs up) before the write. */
    call_model_set_now(&m, 1000u + 120000u + 1u); call_model_tick(&m);
    assert_true(!m.pending_mt.active, "the ring backstop cleared the stale episode");
    assert_true(call_model_txn_dispatch_guard(&m, tok) == CALL_DISPATCH_STALE,
                "§16.5 guard: ANSWER(0) is STALE once its episode is gone (suppress the write)");
    assert_true(find_txn(&m, tok) && find_txn(&m, tok)->state == TXN_FAILED,
                "§16.5 guard: a STALE constructive op resolves FAILED without sending");
}

/* §16.5 dispatch guard: a pre-ID WAIT_REJECT SENDs while its call-waiting episode is
 * live, and is ALREADY_SATISFIED (write suppressed, resolved SUCCEEDED) once the waiting
 * call is already gone (goal met). */
static void test_wait_reject_preid_guard(void) {
    call_model_t m; tinit(&m); call_model_set_now(&m, 1000u);
    call_model_on_event(&m, 1u, true, CALL_LEG_ACTIVE, CALL_DIR_MO);   /* the foreground call */
    call_model_on_ring(&m);                                            /* pre-ID call-waiting episode */
    uint32_t tok = call_model_request(&m, CALL_TXN_WAIT_REJECT, 0u, false);
    assert_true(find_txn(&m, tok) && find_txn(&m, tok)->target_id == 0u,
                "precondition: WAIT_REJECT(0) has no waiting id yet");
    assert_true(call_model_txn_dispatch_guard(&m, tok) == CALL_DISPATCH_SEND,
                "§16.5 guard: pre-ID WAIT_REJECT SENDs while the waiting episode is live");
    call_model_set_now(&m, 1000u + 120000u + 1u); call_model_tick(&m);  /* the waiting caller gives up */
    assert_true(!m.pending_mt.active && find_leg(&m, 1u) != NULL, "episode cleared; foreground call intact");
    assert_true(call_model_txn_dispatch_guard(&m, tok) == CALL_DISPATCH_ALREADY_SATISFIED,
                "§16.5 guard: WAIT_REJECT is ALREADY_SATISFIED once the waiting call is gone");
    assert_true(find_txn(&m, tok) && find_txn(&m, tok)->state == TXN_SUCCEEDED,
                "§16.5 guard: an ALREADY_SATISFIED op resolves SUCCEEDED without sending");
}

/* §16.5 dispatch guard: HANGUP is session-wide (target-agnostic) — SEND while ANY leg is
 * present (target 0, a known id, or an unknown id all SEND); ALREADY_SATISFIED once the
 * session is already IDLE. */
static void test_hangup_guard_totality(void) {
    /* target 0 (session) + a live leg -> SEND. */
    { call_model_t m; tinit(&m);
      call_model_on_event(&m, 1u, true, CALL_LEG_ACTIVE, CALL_DIR_MO);
      uint32_t tok = call_model_request(&m, CALL_TXN_HANGUP, 0u, false);
      assert_true(call_model_txn_dispatch_guard(&m, tok) == CALL_DISPATCH_SEND,
                  "HANGUP(0) SENDs with a live session leg"); }
    /* an UNKNOWN target id but a live session leg -> still SEND (HANGUP is session-wide). */
    { call_model_t m; tinit(&m);
      call_model_on_event(&m, 1u, true, CALL_LEG_ACTIVE, CALL_DIR_MO);
      uint32_t tok = call_model_request(&m, CALL_TXN_HANGUP, 9u, false);
      assert_true(call_model_txn_dispatch_guard(&m, tok) == CALL_DISPATCH_SEND,
                  "HANGUP(unknown target) still SENDs while a session leg exists (session-wide)"); }
    /* no legs -> already IDLE -> ALREADY_SATISFIED, resolved SUCCEEDED. */
    { call_model_t m; tinit(&m);
      uint32_t tok = call_model_request(&m, CALL_TXN_HANGUP, 0u, false);
      assert_true(call_model_txn_dispatch_guard(&m, tok) == CALL_DISPATCH_ALREADY_SATISFIED,
                  "§16.5 guard: HANGUP over an already-IDLE session is ALREADY_SATISFIED");
      assert_true(find_txn(&m, tok) && find_txn(&m, tok)->state == TXN_SUCCEEDED,
                  "§16.5 guard: it resolves SUCCEEDED without sending CHUP"); }
}

/* =================== §16.6 pending-MT episode generation/token =================== */

/* §16.6: an ANSWER admitted against incoming episode 1 must NEVER bind a LATER episode 2
 * (a new incoming after episode 1 cleared). Episodes carry a monotonic generation; the
 * stale op recorded episode 1's, so txn_bind_targets refuses episode 2's leg and
 * txn_resolve fails the op instead. */
static void test_answer_stale_episode_no_bind(void) {
    /* use txn_abandon_ms > the 120 s ring timeout so the ANSWER OUTLIVES episode 1
     * (isolating the episode gate from the abandon backstop). */
    call_timing_t t = call_timing_fixture(); t.txn_abandon_ms = 300000u;
    call_model_t m; call_model_init(&m, &t); call_model_set_now(&m, 1000u);

    /* episode 1 rings; the user presses Answer before the ID-scoped call event lands. */
    call_model_on_ring(&m);
    uint32_t gen1 = m.pending_mt.generation;
    assert_true(gen1 != 0u, "§16.6: a fresh episode gets a non-zero generation");
    uint32_t tok = call_model_request(&m, CALL_TXN_ANSWER, 0u, false);
    call_model_txn_dispatched(&m, tok); txn_ok(&m, tok);
    assert_true(find_txn(&m, tok) && find_txn(&m, tok)->mt_episode == gen1 &&
                find_txn(&m, tok)->target_id == 0u,
                "precondition: ANSWER admitted against episode 1, still unbound");

    /* episode 1 ends WITHOUT ever materializing a leg (caller gave up) — ring backstop
     * clears it; the ANSWER outlives it (abandon deadline is 300 s out). */
    call_model_set_now(&m, 1000u + 120000u + 1u);
    call_model_tick(&m);
    assert_true(!m.pending_mt.active, "episode 1 cleared (ring backstop)");
    assert_true(find_txn(&m, tok) != NULL, "the ANSWER is still alive (abandon not reached)");

    /* episode 2 rings and DOES materialize an incoming leg. */
    call_model_on_ring(&m);
    uint32_t gen2 = m.pending_mt.generation;
    assert_true(gen2 != gen1, "§16.6: episode 2 has a NEW generation (never reuses episode 1's)");
    call_model_on_event(&m, 5u, true, CALL_LEG_INCOMING, CALL_DIR_MT);

    /* the STALE ANSWER (episode 1) must NOT bind episode 2's leg. */
    call_txn_t *tx = find_txn(&m, tok);
    assert_true(tx == NULL || tx->target_id != 5u,
                "§16.6: a stale ANSWER (episode 1) must NOT bind episode 2's incoming leg");

    /* even after episode 2's leg connects, the stale ANSWER never answered it. */
    call_model_on_event(&m, 5u, true, CALL_LEG_ACTIVE, CALL_DIR_MT);
    tx = find_txn(&m, tok);
    assert_true(tx == NULL || tx->target_id != 5u,
                "§16.6: the stale ANSWER never bound/answered episode 2 (fails on its own dead episode)");
}

int main(void) {
    test_init_projects_r1_empty();
    test_set_now();
    test_timing_profile_takes_effect();
    test_token_uint32_collision_avoidance();
    test_result_sink_enumeration();
    test_null_safe();
    test_target_zero_resolves();
    test_dial_dispatch_resets_stale();
    test_2ndmo_dispatch_keeps_primary_connected();
    test_request_alloc();
    test_request_pool_and_hangup_reserved();
    test_dispatched_baseline_and_deadlines();
    test_bind_dialing();
    test_dial_success_lost_dialing_recovery();
    test_tentative_dir_unconfirmed_then_confirm();
    test_tentative_mt_reclassify();
    test_bind_ambiguity_no_guess();
    test_other_op_postconditions();
    test_wait_answer_peer_release_keeps_answered_leg();
    test_policy_deadline_uncertain();
    test_abandon_deadline_not_orphaned();
    test_unbound_abandon_stamps_result();
    test_wait_answer_zero_no_pending_mt_fails();
    test_wait_answer_zero_pending_mt_holds();
    test_new_call_abandoned_bound();
    test_new_call_abandoned_predispatch();
    test_new_call_abandoned_unbound();
    test_new_call_abandoned_retires_terminal_result();
    test_answer_zero_pending_mt_rebinds();
    test_answer_zero_no_pending_mt_still_fails();
    test_pending_not_force_cancelled_queue_owns();
    test_orphan_pending_does_not_suppress_clcc();
    test_txn_cancel_bound_leg();
    test_ever_dispatched_epoch();
    test_cancel_post_dispatch_tombstone();
    test_abandon_post_dispatch_unbound_tombstone();
    test_cmd_timeout_is_not_a_tombstone();
    test_tombstone_held_until_cleanup_then_retires();
    test_succeeded_dial_retires_after_leg_departs();
    test_tombstone_releases_late_leg();
    test_tombstone_spares_unrelated_redial_leg();
    test_no_urc_resurrect_cancelled_leg();
    test_unattributable_final_preserves_owned_register();
    test_unbound_abandon_routes_2ndmo_cause();
    test_primary_cause_over_live_leg_not_clobbered();
    test_op_descriptor_totality();
    test_hold_zero_resolves_sole_held();
    test_answer_zero_guard_episode();
    test_wait_reject_preid_guard();
    test_hangup_guard_totality();
    test_answer_stale_episode_no_bind();
    if (s_failures != 0) {
        fprintf(stderr, "%d failure(s)\n", s_failures);
        return 1;
    }
    printf("test_modem_call_model: all assertions passed\n");
    return 0;
}

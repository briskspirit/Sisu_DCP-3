/* Host unit test + bug hunt for the URC/event feed of the CLCC call model
 * (src/services/modem_call_model.c). Pure module: linked directly, no HAL stubs.
 *
 * COMPILE (sanitizer recipe):
 *   cc -std=c11 -I include -I tests/stubs -Wall -Wextra \
 *      -fsanitize=address,undefined -fno-sanitize-recover=all -g -O1 \
 *      tests/test_call_model_events.c src/services/modem_call_model.c -o /tmp/t && /tmp/t
 *
 * ORACLE: docs/call_model.md. SETUP_DONE/BUSY (normalized UNKNOWN) are table
 *   no-ops; id-bearing URC updates the live table; §5.3 semantic_revision bumps
 *   ONLY on structural change,
 *   a naming URC clears pending_removal but NOT its limbo clock; §5.5/§16.2 bare finals
 *   never remove a leg (record the attempt into the SOLE eligible open setup/teardown txn,
 *   CHLD-window excluded); §9.5 last_call_result latches CONNECTED on ACTIVE/bare CONNECT and NONE on
 *   any ring indication, second_call_result stays orthogonal; §10 overflow flags, never
 *   drops silently.
 */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "services/modem_call_model.h"
#include "call_timing_fixture.h"

/* §16.12: init the model with the stable host timing fixture. */
static void tinit(call_model_t *m) { call_timing_t _t = call_timing_fixture(); call_model_init(m, &_t); }
/* CMD_OK folds in the old txn_accepted (§16.2). */
static void txn_ok(call_model_t *m, uint32_t tok) { modem_cmd_result_t _r = { .status = CMD_OK }; call_model_txn_command_result(m, tok, _r); }

static int s_failures;

static void assert_true(bool cond, const char *msg) {
    if (!cond) { fprintf(stderr, "FAIL: %s\n", msg); s_failures++; }
}
static void assert_eq_u(unsigned got, unsigned want, const char *msg) {
    if (got != want) { fprintf(stderr, "FAIL: %s (got %u want %u)\n", msg, got, want); s_failures++; }
}

static call_leg_t *find_leg(call_model_t *m, uint8_t id) {
    for (unsigned i = 0; i < MODEM_MAX_CALL_LEGS; i++) {
        if (m->legs[i].in_use && m->legs[i].id == id) { return &m->legs[i]; }
    }
    return NULL;
}
static unsigned count_legs(call_model_t *m) {
    unsigned n = 0;
    for (unsigned i = 0; i < MODEM_MAX_CALL_LEGS; i++) { if (m->legs[i].in_use) { n++; } }
    return n;
}
/* Locate a transaction by token (public struct, no reliance on the model's
 * internal txn_by_token -- this TU links modem_call_model.c as a separate
 * object, so file-static helpers aren't reachable here anyway). */
static call_txn_t *find_txn_by_token(call_model_t *m, uint32_t token) {
    for (unsigned i = 0; i < MODEM_MAX_CALL_TRANSACTIONS; i++) {
        if (m->txns[i].in_use && m->txns[i].token == token) { return &m->txns[i]; }
    }
    return NULL;
}

/* §5.1/§5.3/§8: add-then-update; revision bumps on add + state change, NOT on a
 * metadata-only re-event; a known dir refines but UNKNOWN never clobbers it. */
static void test_on_event_add_update(void) {
    call_model_t m;
    tinit(&m);
    call_model_set_now(&m, 1000u);
    call_model_on_event(&m, 1u, true, CALL_LEG_DIALING, CALL_DIR_MO);
    call_leg_t *L = find_leg(&m, 1u);
    assert_true(L != NULL, "on_event adds a leg for a new id");
    if (L != NULL) {
        assert_eq_u(L->id, 1u, "added leg has id 1");
        assert_eq_u(L->generation, 1u, "fresh leg generation starts at 1");
        assert_eq_u((unsigned)L->state, (unsigned)CALL_LEG_DIALING, "added leg state = DIALING");
        assert_eq_u((unsigned)L->dir, (unsigned)CALL_DIR_MO, "added leg dir = MO");
        assert_eq_u(L->first_seen_ms, 1000u, "first_seen stamped at add");
        assert_eq_u(L->last_seen_ms, 1000u, "last_seen stamped at add");
    }
    assert_eq_u(m.semantic_revision, 1u, "an add bumps semantic_revision once");

    call_model_set_now(&m, 2000u);
    call_model_on_event(&m, 1u, true, CALL_LEG_ALERTING, CALL_DIR_UNKNOWN);
    L = find_leg(&m, 1u);
    assert_eq_u((unsigned)L->state, (unsigned)CALL_LEG_ALERTING, "state transitions to ALERTING");
    assert_eq_u((unsigned)L->dir, (unsigned)CALL_DIR_MO, "UNKNOWN dir does not clobber a known dir");
    assert_eq_u(m.semantic_revision, 2u, "a state transition bumps the revision");
    assert_eq_u(L->last_seen_ms, 2000u, "last_seen refreshed on the update");

    call_model_set_now(&m, 3000u);
    call_model_on_event(&m, 1u, true, CALL_LEG_ALERTING, CALL_DIR_UNKNOWN);
    L = find_leg(&m, 1u);
    assert_eq_u(m.semantic_revision, 2u, "a metadata-only re-event does NOT bump the revision");
    assert_eq_u(L->last_seen_ms, 3000u, "last_seen still refreshed on a metadata-only event");
    assert_eq_u(count_legs(&m), 1u, "a repeated id updates in place, never duplicates a leg");
}

/* §4.1: a normalized-UNKNOWN event (SETUP_DONE / BUSY) is a table no-op. */
static void test_on_event_unknown_noop(void) {
    call_model_t m;
    tinit(&m);
    call_model_set_now(&m, 100u);
    call_model_on_event(&m, 2u, true, CALL_LEG_UNKNOWN, CALL_DIR_UNKNOWN);
    assert_eq_u(count_legs(&m), 0u, "UNKNOWN (SETUP_DONE/BUSY) event adds no leg");
    assert_eq_u(m.semantic_revision, 0u, "a no-op event does not bump the revision");
}

/* §9.5: any leg entering ACTIVE latches last_call_result = CONNECTED. */
static void test_on_event_active_latches_connected(void) {
    call_model_t m;
    tinit(&m);
    call_model_set_now(&m, 500u);
    assert_eq_u((unsigned)m.published.last_call_result, (unsigned)MODEM_CALL_RESULT_NONE,
                "init last_call_result = NONE");
    call_model_on_event(&m, 3u, true, CALL_LEG_ACTIVE, CALL_DIR_MT);
    assert_eq_u((unsigned)m.published.last_call_result, (unsigned)MODEM_CALL_RESULT_CONNECTED,
                "a leg entering ACTIVE latches CONNECTED");
}

/* §5.1/§5.3: an id-scoped RELEASED (stat 6) is a POSITIVE removal — it EVICTS the
 * leg (structural) and arms a coarse-final reconcile. §16.4 CORRECTED: a connected
 * leg that departs with NO recorded cause and NO owning local teardown txn was
 * dropped by the FAR END -> last_call_result = NO_CARRIER (the old model retained
 * CONNECTED — the "remote ACTIVE release retained CONNECTED" probe). A genuine
 * clean LOCAL hangup (with a HANGUP/RELEASE txn) keeps CONNECTED — see
 * test_local_hangup_keeps_connected_remote_drops. */
static void test_on_event_released_removes_leg(void) {
    call_model_t m;
    tinit(&m);
    call_model_set_now(&m, 10u);
    call_model_on_event(&m, 4u, true, CALL_LEG_ACTIVE, CALL_DIR_MT);      /* CONNECTED latched */
    uint32_t rev_before = m.semantic_revision;
    call_model_set_now(&m, 20u);
    call_model_on_event(&m, 4u, true, CALL_LEG_RELEASING, CALL_DIR_UNKNOWN);
    assert_true(find_leg(&m, 4u) == NULL, "a RELEASED EVICTS the leg (positive removal, §5.1)");
    assert_eq_u(count_legs(&m), 0u, "table empty after the sole leg is released");
    assert_true(m.semantic_revision == rev_before + 1u, "a removal is a structural bump");
    assert_true((m.reconcile_reasons & (unsigned)CALL_RC_COARSE_FINAL) != 0u,
                "RELEASED arms a coarse-final reconcile");
    assert_eq_u((unsigned)m.published.last_call_result, (unsigned)MODEM_CALL_RESULT_NO_CARRIER,
                "§16.4: a remote release of a connected leg (no local teardown txn) latches NO_CARRIER");
}

/* Bench-recorded: releasing the foreground leg by id must preserve the WAITING leg
 * and project it as a normal incoming call, with its CLI intact. */
static void test_targeted_active_release_re_presents_waiting(void) {
    call_model_t m;
    tinit(&m);
    call_model_set_now(&m, 100u);
    call_model_on_event(&m, 1u, true, CALL_LEG_ACTIVE, CALL_DIR_MO);
    call_model_on_ring(&m);
    call_model_on_clip(&m, "5552000", 0u);
    call_model_on_event(&m, 2u, true, CALL_LEG_WAITING, CALL_DIR_MT);

    uint32_t tok = call_model_request(&m, CALL_TXN_RELEASE_LEG, 1u, false);
    assert_true(tok != 0u, "targeted release admitted");
    call_model_txn_dispatched(&m, tok);
    txn_ok(&m, tok);
    call_model_set_now(&m, 200u);
    call_model_on_event(&m, 1u, true,
                        CALL_LEG_RELEASING, CALL_DIR_UNKNOWN);

    call_projection_t p;
    call_model_project(&m, &p);
    assert_eq_u((unsigned)p.call_state, (unsigned)MODEM_CALL_RINGING,
                "waiting leg re-presents as normal RINGING");
    assert_true(p.ring_active && !p.waiting_call,
                "sole MT leg rings and is no longer a waiting overlay");
    assert_true(strcmp(p.incoming_number, "5552000") == 0,
                "waiting CLI survives foreground release");
    call_txn_t *tx = find_txn_by_token(&m, tok);
    assert_true(tx != NULL && tx->state == TXN_SUCCEEDED,
                "targeted release resolves after only its target departs");
    assert_true(find_leg(&m, 2u) != NULL,
                "waiting leg remains in the call table");
}

/* Recorded alternate ordering: after CHLD=11 the network can connect B
 * before it reports A released. Legacy clears its one flat incoming-number slot
 * when A then departs, while the call table must retain B's per-leg metadata. */
static void test_targeted_active_release_directly_promotes_waiting(void) {
    call_model_t m;
    tinit(&m);
    call_model_set_now(&m, 100u);
    call_model_on_event(&m, 1u, true, CALL_LEG_ACTIVE, CALL_DIR_MO);
    call_model_on_ring(&m);
    call_model_on_clip(&m, "5552000", 0u);
    call_model_on_event(&m, 2u, true, CALL_LEG_WAITING, CALL_DIR_MT);

    call_projection_t p;
    call_model_project(&m, &p);
    assert_true(p.waiting_call && strcmp(p.incoming_number, "5552000") == 0,
                "direct-promotion precondition: B is waiting with CLI");

    uint32_t tok = call_model_request(&m, CALL_TXN_RELEASE_LEG, 1u, false);
    assert_true(tok != 0u, "direct-promotion targeted release admitted");
    call_model_txn_dispatched(&m, tok);
    txn_ok(&m, tok);

    /* Exact bench order: B ACTIVE, A ACTIVE repeat, A RELEASED, B ACTIVE repeat. */
    call_model_set_now(&m, 200u);
    call_model_on_event(&m, 2u, true, CALL_LEG_ACTIVE, CALL_DIR_MT);
    call_model_on_event(&m, 1u, true, CALL_LEG_ACTIVE, CALL_DIR_UNKNOWN);
    call_model_on_event(&m, 1u, true, CALL_LEG_RELEASING, CALL_DIR_UNKNOWN);
    call_model_on_event(&m, 2u, true, CALL_LEG_ACTIVE, CALL_DIR_UNKNOWN);
    call_model_project(&m, &p);

    assert_eq_u((unsigned)p.call_state, (unsigned)MODEM_CALL_ACTIVE,
                "direct promotion leaves B ACTIVE");
    assert_eq_u(p.active_call_id, 2u,
                "direct promotion publishes B as the foreground leg");
    assert_true(!p.waiting_call && !p.ring_active,
                "direct promotion consumes the waiting overlay");
    assert_true(p.incoming_number[0] == '\0',
                "direct promotion matches legacy's retired flat caller slot");

    call_leg_t *b = find_leg(&m, 2u);
    assert_true(b != NULL && strcmp(b->number, "5552000") == 0,
                "direct promotion retains B's per-leg number for later consumers");
    assert_true(b != NULL && !b->mt_cli_valid,
                "direct promotion retires only B's flat-CLI ownership");
    call_txn_t *tx = find_txn_by_token(&m, tok);
    assert_true(tx != NULL && tx->state == TXN_SUCCEEDED,
                "direct-promotion release resolves after A departs");
}

/* Recorded bench trace A1 corrected the Phase-1 compatibility oracle: legacy
 * publishes NO_CARRIER after the sole leg's id-scoped RELEASED event even when a
 * local HANGUP txn owns the teardown. The Phase-2 journal may still distinguish
 * local from remote; the flat modem_status_t latch must match legacy. */
static void test_sole_connected_release_projects_no_carrier(void) {
    /* LOCAL: the app issued AT+CHUP -> a HANGUP txn accounts for the departure. */
    { call_model_t m; tinit(&m); call_model_set_now(&m, 10u);
      call_model_on_event(&m, 1u, true, CALL_LEG_ACTIVE, CALL_DIR_MO);          /* connected */
      uint32_t tok = call_model_request(&m, CALL_TXN_HANGUP, 0u, false);
      call_model_txn_dispatched(&m, tok);
      call_model_on_event(&m, 1u, true, CALL_LEG_RELEASING, CALL_DIR_UNKNOWN);  /* local teardown */
      assert_eq_u(m.journal_count, 1u,
                  "local teardown still records one Phase-2 terminal entry");
      assert_eq_u((unsigned)m.journal[0].result, (unsigned)MODEM_CALL_RESULT_NONE,
                  "local teardown remains NONE in the Phase-2 journal");
      call_projection_t p; call_model_project(&m, &p);
      assert_eq_u((unsigned)m.published.last_call_result, (unsigned)MODEM_CALL_RESULT_NO_CARRIER,
                  "bench-recorded: a sole LOCAL hangup projects legacy NO_CARRIER"); }

    /* REMOTE: no owning teardown txn, no recorded cause -> the far end dropped it. */
    { call_model_t m; tinit(&m); call_model_set_now(&m, 10u);
      call_model_on_event(&m, 1u, true, CALL_LEG_ACTIVE, CALL_DIR_MO);          /* connected */
      call_model_on_event(&m, 1u, true, CALL_LEG_RELEASING, CALL_DIR_UNKNOWN);  /* far end drops */
      call_projection_t p; call_model_project(&m, &p);
      assert_eq_u((unsigned)m.published.last_call_result, (unsigned)MODEM_CALL_RESULT_NO_CARRIER,
                  "§16.4: a REMOTE release (no owning teardown txn) drops to NO_CARRIER"); }
}

/* Recorded shadow bench: an unanswered primary leg that is locally
 * abandoned/rejected also ends with the legacy flat latch at NO_CARRIER. This is
 * compatibility projection only; its journal record remains an unanswered local
 * teardown rather than being rewritten as a remote failure. */
static void test_sole_unanswered_local_release_projects_no_carrier(void) {
    call_model_t m; tinit(&m); call_model_set_now(&m, 10u);
    uint32_t dial = call_model_request(&m, CALL_TXN_DIAL, 0u, false);
    call_model_txn_dispatched(&m, dial);
    call_model_on_event(&m, 1u, true, CALL_LEG_DIALING, CALL_DIR_MO);
    call_projection_t p;
    call_model_project(&m, &p);
    assert_eq_u((unsigned)p.call_state, (unsigned)MODEM_CALL_DIALING,
                "precondition: locally-abandoned primary is DIALING");

    uint32_t hangup = call_model_request(&m, CALL_TXN_HANGUP, 0u, false);
    call_model_txn_dispatched(&m, hangup);
    call_model_on_event(&m, 1u, true, CALL_LEG_RELEASING, CALL_DIR_UNKNOWN);
    call_model_project(&m, &p);
    assert_eq_u((unsigned)p.call_state, (unsigned)MODEM_CALL_IDLE,
                "bench-recorded: abandoned sole setup returns to IDLE");
    assert_eq_u((unsigned)p.last_call_result, (unsigned)MODEM_CALL_RESULT_NO_CARRIER,
                "bench-recorded: unanswered local release projects legacy NO_CARRIER");
}

/* Recorded shadow-bench ordering: RING, CLIP, id-bearing INCOMING and ACTIVE
 * can all be consumed before the once-per-tick projector runs. Binding must copy
 * the pre-id CLI metadata onto the MT leg before pending-MT is cleared, and the
 * compatibility projection must retain it while that answered leg is active. */
static void test_answered_mt_metadata_survives_preprojection_bind(void) {
    call_model_t m; tinit(&m); call_model_set_now(&m, 100u);
    call_model_on_ring(&m);
    call_model_on_clip(&m, "+12125550123", 0u);
    call_model_on_event(&m, 2u, true, CALL_LEG_INCOMING, CALL_DIR_MT);
    call_model_on_event(&m, 2u, true, CALL_LEG_ACTIVE, CALL_DIR_MT);

    call_projection_t p;
    call_model_project(&m, &p);
    assert_eq_u((unsigned)p.call_state, (unsigned)MODEM_CALL_ACTIVE,
                "bench-recorded: answered MT leg projects ACTIVE");
    assert_true(strcmp(p.incoming_number, "+12125550123") == 0,
                "bench-recorded: incoming number survives bind+answer before first projection");
    assert_true(!p.caller_id_withheld,
                "bench-recorded: valid CLI remains non-withheld while MT call is active");

    uint32_t hangup = call_model_request(&m, CALL_TXN_HANGUP, 0u, false);
    call_model_txn_dispatched(&m, hangup);
    call_model_on_event(&m, 2u, true, CALL_LEG_RELEASING, CALL_DIR_UNKNOWN);
    call_model_project(&m, &p);
    assert_true(p.incoming_number[0] == '\0',
                "legacy compatibility clears incoming number at full teardown");
    assert_eq_u((unsigned)p.last_call_result, (unsigned)MODEM_CALL_RESULT_NO_CARRIER,
                "bench-recorded: answered local MT teardown projects legacy NO_CARRIER");
}

/* Recorded shadow bench: +CLIP validity=2 presents withheld correctly and an
 * id-scoped RELEASED clears the number but does not clear legacy's withheld latch.
 * Preserve that odd lifetime until a fresh incoming CLI overwrites it. */
static void test_withheld_cli_retained_after_id_scoped_release(void) {
    call_model_t m; tinit(&m); call_model_set_now(&m, 100u);
    call_model_on_ring(&m);
    call_model_on_clip(&m, "", 2u);
    call_model_on_event(&m, 3u, true, CALL_LEG_INCOMING, CALL_DIR_MT);

    call_projection_t p;
    call_model_project(&m, &p);
    assert_true(p.caller_id_withheld,
                "bench-recorded precondition: unavailable CLI projects withheld");
    call_model_on_event(&m, 3u, true, CALL_LEG_RELEASING, CALL_DIR_UNKNOWN);
    call_model_project(&m, &p);
    assert_eq_u((unsigned)p.call_state, (unsigned)MODEM_CALL_IDLE,
                "bench-recorded: released withheld call returns to IDLE");
    assert_true(p.incoming_number[0] == '\0',
                "bench-recorded: id-scoped release clears incoming number");
    assert_true(p.caller_id_withheld,
                "bench-recorded: id-scoped release retains legacy withheld latch");
    assert_eq_u((unsigned)p.last_call_result, (unsigned)MODEM_CALL_RESULT_NO_CARRIER,
                "bench-recorded: id-scoped release projects legacy NO_CARRIER");

    call_model_on_ring(&m);
    call_model_on_clip(&m, "+15550000009", 0u);
    call_model_project(&m, &p);
    assert_true(!p.caller_id_withheld,
                "a fresh valid CLI overwrites the retained withheld latch");
}

/* Legacy has one incoming-number slot. Once a newer waiting caller owns it,
 * releasing that caller clears the slot; it must not reveal an older answered
 * MT caller's number from per-leg storage. */
static void test_waiting_cli_owner_release_does_not_restore_older_number(void) {
    call_model_t m; tinit(&m); call_model_set_now(&m, 100u);
    call_model_on_ring(&m);
    call_model_on_clip(&m, "+15550000001", 0u);
    call_model_on_event(&m, 1u, true, CALL_LEG_INCOMING, CALL_DIR_MT);
    call_model_on_event(&m, 1u, true, CALL_LEG_ACTIVE, CALL_DIR_MT);
    call_projection_t p;
    call_model_project(&m, &p);
    assert_true(strcmp(p.incoming_number, "+15550000001") == 0,
                "precondition: answered MT caller owns the flat number");

    call_model_on_ring(&m);
    call_model_on_clip(&m, "+15550000002", 0u);
    call_model_on_event(&m, 2u, true, CALL_LEG_WAITING, CALL_DIR_MT);
    call_model_project(&m, &p);
    assert_true(strcmp(p.incoming_number, "+15550000002") == 0,
                "newer waiting caller replaces the flat number owner");

    call_model_on_event(&m, 2u, true, CALL_LEG_RELEASING, CALL_DIR_UNKNOWN);
    call_model_project(&m, &p);
    assert_eq_u((unsigned)p.call_state, (unsigned)MODEM_CALL_ACTIVE,
                "waiting caller departure preserves the foreground call");
    assert_true(p.incoming_number[0] == '\0',
                "waiting caller departure clears rather than restores the older number");
}

/* §16.4 regression: a FAILED (network-rejected) local teardown
 * txn lingers in_use until lazy reclaim — it must NOT count as an accounted local
 * hangup and so must NOT mask a LATER genuine remote release. The call is still up
 * after the reject; when the far end drops it, last_call_result -> NO_CARRIER. */
static void test_failed_teardown_does_not_mask_remote_drop(void) {
    call_model_t m; tinit(&m); call_model_set_now(&m, 10u);
    call_model_on_event(&m, 1u, true, CALL_LEG_ACTIVE, CALL_DIR_MO);        /* connected */
    /* app requests a local release; the network REJECTS it (CMD_ERROR) -> the txn is
     * TXN_FAILED but stays in_use (lazy reclaim), and the call is genuinely still up. */
    uint32_t tok = call_model_request(&m, CALL_TXN_RELEASE_ACTIVE, 0u, false);
    call_model_txn_dispatched(&m, tok);
    modem_cmd_result_t r = { .status = CMD_ERROR, .has_call_result = false };
    call_model_txn_command_result(&m, tok, r);
    call_txn_t *tx = find_txn_by_token(&m, tok);
    assert_true(tx && tx->state == TXN_FAILED && tx->target_id == 1u,
                "precondition: the rejected RELEASE_ACTIVE is FAILED but still in_use, targeting leg 1");
    assert_true(find_leg(&m, 1u) != NULL, "precondition: the call is still up after the reject");
    m.published.last_call_result = MODEM_CALL_RESULT_CONNECTED;   /* still connected */

    /* later the FAR END genuinely drops the call. */
    call_model_set_now(&m, 5000u);
    call_model_on_event(&m, 1u, true, CALL_LEG_RELEASING, CALL_DIR_UNKNOWN);
    call_projection_t p; call_model_project(&m, &p);
    assert_eq_u((unsigned)m.published.last_call_result, (unsigned)MODEM_CALL_RESULT_NO_CARRIER,
                "§16.4: a FAILED teardown does NOT mask the later remote drop -> NO_CARRIER");
}

/* §9.3 promotion: releasing the ACTIVE leg while a HELD leg remains promotes the
 * held leg to ACTIVE (the projector then emits call_on_hold=false). */
static void test_on_event_released_promotes_held(void) {
    call_model_t m;
    tinit(&m);
    call_model_set_now(&m, 10u);
    call_model_on_event(&m, 1u, true, CALL_LEG_ACTIVE, CALL_DIR_MO);
    call_model_on_event(&m, 2u, true, CALL_LEG_HELD,   CALL_DIR_MT);
    call_model_on_event(&m, 1u, true, CALL_LEG_RELEASING, CALL_DIR_UNKNOWN);   /* active leg gone */
    assert_true(find_leg(&m, 1u) == NULL, "released ACTIVE leg removed");
    call_leg_t *H = find_leg(&m, 2u);
    assert_true(H != NULL && H->state == CALL_LEG_ACTIVE, "held leg PROMOTED to ACTIVE (§9.3)");
}

/* §9.5: a recorded terminal cause (a remote bare final -> m.pending_terminal) is
 * latched to last_call_result when the primary leg is evicted (retained across IDLE). */
static void test_on_event_released_latches_terminal(void) {
    call_model_t m;
    tinit(&m);
    call_model_set_now(&m, 10u);
    call_model_on_event(&m, 1u, true, CALL_LEG_ACTIVE, CALL_DIR_MO);      /* CONNECTED */
    m.pending_terminal = MODEM_CALL_RESULT_NO_CARRIER;             /* a remote bare final recorded this */
    call_model_on_event(&m, 1u, true, CALL_LEG_RELEASING, CALL_DIR_UNKNOWN);
    assert_true(find_leg(&m, 1u) == NULL, "primary leg evicted");
    /* §11: the terminal cause is JOURNALED on eviction and latched into
     * published.last_call_result when the projector DRAINS the journal at publish. */
    call_projection_t p;
    call_model_project(&m, &p);
    assert_eq_u((unsigned)m.published.last_call_result, (unsigned)MODEM_CALL_RESULT_NO_CARRIER,
                "the recorded terminal cause is journaled on eviction, drained to last_call_result at publish (§9.5/§11)");
}

/* §9.4: pending-MT binds to an appearing INCOMING/WAITING leg, then clears when the
 * bound leg is answered (ACTIVE) so waiting/ring drop. */
static void test_on_event_pending_mt_bind_clear(void) {
    call_model_t m;
    tinit(&m);
    call_model_set_now(&m, 100u);
    m.pending_mt.active = true;                                    /* a pre-id RING episode (white-box, on_ring is Cycle B) */
    m.pending_mt.first_ring_ms = m.pending_mt.last_ring_ms = 100u;
    call_model_on_event(&m, 2u, true, CALL_LEG_INCOMING, CALL_DIR_MT);   /* leg appears -> bind */
    assert_true(m.pending_mt.bound_valid && m.pending_mt.bound_id == 2u,
                "pending-MT binds to the appearing INCOMING leg (§9.4)");
    call_model_on_event(&m, 2u, true, CALL_LEG_ACTIVE, CALL_DIR_MT);     /* answered -> clear */
    assert_true(!m.pending_mt.active && !m.pending_mt.bound_valid,
                "pending-MT clears when the bound leg is answered (§9.4)");
}

/* §8 generation reuse: a freed-then-readded id gets an incremented generation
 * (disambiguates a reused id when the free was observed). */
static void test_on_event_generation_reuse(void) {
    call_model_t m;
    tinit(&m);
    call_model_set_now(&m, 10u);
    call_model_on_event(&m, 3u, true, CALL_LEG_DIALING, CALL_DIR_MO);
    call_leg_t *L = find_leg(&m, 3u);
    assert_eq_u(L->generation, 1u, "first use of id 3 -> generation 1");
    call_model_on_event(&m, 3u, true, CALL_LEG_RELEASING, CALL_DIR_UNKNOWN);   /* free it */
    assert_true(find_leg(&m, 3u) == NULL, "id 3 freed");
    call_model_on_event(&m, 3u, true, CALL_LEG_DIALING, CALL_DIR_MO);          /* reuse it */
    L = find_leg(&m, 3u);
    assert_true(L != NULL && L->generation == 2u, "reused id 3 -> generation bumped to 2 (§8)");
}

/* §8 generation history must survive SLOT reuse — the reason the per-id gen_next[]
 * array is required (a scan of the LIVE slots loses history once the freed slot is
 * overwritten by another id). Free id 3, let id 4 REUSE id 3's freed slot, then re-add
 * id 3: it must STILL bump to generation 2 (the old scan-the-slots scheme would have
 * found no id-3 slot and wrongly restarted at generation 1). */
static void test_gen_next_survives_slot_reuse(void) {
    call_model_t m;
    tinit(&m);
    call_model_set_now(&m, 10u);
    call_model_on_event(&m, 3u, true, CALL_LEG_DIALING, CALL_DIR_MO);          /* alloc id 3 */
    call_leg_t *L = find_leg(&m, 3u);
    assert_true(L != NULL && L->generation == 1u, "id 3 first alloc -> generation 1");
    call_model_on_event(&m, 3u, true, CALL_LEG_RELEASING, CALL_DIR_UNKNOWN);   /* cm_remove_leg(id 3) */
    assert_true(find_leg(&m, 3u) == NULL, "id 3 freed");
    call_model_on_event(&m, 4u, true, CALL_LEG_DIALING, CALL_DIR_MO);          /* id 4 reuses id 3's freed slot */
    L = find_leg(&m, 4u);
    assert_true(L != NULL && L->generation == 1u, "id 4 first alloc -> generation 1");
    call_model_on_event(&m, 3u, true, CALL_LEG_DIALING, CALL_DIR_MO);          /* re-alloc id 3 */
    L = find_leg(&m, 3u);
    assert_true(L != NULL && L->generation == 2u,
                "id 3 re-alloc after its slot was reused -> generation 2 (gen_next[], not a slot scan)");
}

/* Generation zero is the absent/wildcard sentinel in transaction references. After
 * the uint8_t generation reaches 255, reuse must wrap to 1 rather than handing a live
 * leg generation 0 and weakening generation-qualified teardown matching. */
static void test_generation_wrap_skips_zero(void) {
    call_model_t m;
    tinit(&m);
    for (unsigned generation = 1u; generation <= 255u; generation++) {
        call_model_set_now(&m, generation);
        call_model_on_event(&m, 1u, true, CALL_LEG_ACTIVE, CALL_DIR_MO);
        call_leg_t *leg = find_leg(&m, 1u);
        assert_true(leg != NULL && leg->generation == (uint8_t)generation,
                    "successive id reuse advances through every non-zero generation");
        call_model_on_event(&m, 1u, true, CALL_LEG_RELEASING, CALL_DIR_UNKNOWN);
        call_projection_t projection;
        call_model_project(&m, &projection); /* drain the bounded Phase-1 terminal journal */
    }
    assert_eq_u(m.gen_next[1], 1u, "generation 255 wraps to 1, never zero");
    call_model_on_event(&m, 1u, true, CALL_LEG_ACTIVE, CALL_DIR_MO);
    call_leg_t *leg = find_leg(&m, 1u);
    assert_true(leg != NULL && leg->generation == 1u,
                "the first leg after generation wrap receives generation 1");
}

/* §5.3: a naming URC clears a pending_removal flag but must NOT reset first_removal_ms
 * (the CLCC-absence/limbo clock); it still requires a CLCC confirm. */
static void test_on_event_clears_pending_removal_not_clock(void) {
    call_model_t m;
    tinit(&m);
    call_model_set_now(&m, 100u);
    call_model_on_event(&m, 5u, true, CALL_LEG_ACTIVE, CALL_DIR_MT);
    call_leg_t *L = find_leg(&m, 5u);
    L->pending_removal = true;
    L->first_removal_ms = 12345u;                 /* stamped by a prior CLCC absence */
    uint32_t rev_before = m.semantic_revision;
    call_model_set_now(&m, 200u);
    call_model_on_event(&m, 5u, true, CALL_LEG_HELD, CALL_DIR_UNKNOWN);   /* naming URC */
    L = find_leg(&m, 5u);
    assert_true(!L->pending_removal, "a naming URC clears pending_removal");
    assert_eq_u(L->first_removal_ms, 12345u, "the limbo clock is NOT reset by a stray URC");
    assert_true((m.reconcile_reasons & (unsigned)CALL_RC_PENDING_REMOVAL) != 0u,
                "an un-masked leg still requests a CLCC confirm");
    assert_true(m.semantic_revision == rev_before + 1u,
                "the coincident state change (ACTIVE->HELD) bumps the revision once");
}

/* §10 / FIX-1: the valid id domain is 1..MODEM_MAX_CALL_LEGS and the table has
 * exactly MODEM_MAX_CALL_LEGS slots, so every valid id fits at once (on_event dedups
 * by id) and cm_alloc_leg's overflow path is unreachable via on_event. An id PAST the
 * domain is rejected at the entry guard (FIX-1) BEFORE indexing gen_next[] — it neither
 * overflows nor overwrites a live leg. (The overflow flag/CALL_RC_OVERFLOW remain in
 * cm_alloc_leg as defense-in-depth for the shared allocator, but on_event can't reach
 * them now that out-of-range ids are rejected.) */
static void test_on_event_overflow(void) {
    call_model_t m;
    tinit(&m);
    call_model_set_now(&m, 1u);
    for (uint8_t id = 1u; id <= (uint8_t)MODEM_MAX_CALL_LEGS; id++) {
        call_model_on_event(&m, id, true, CALL_LEG_DIALING, CALL_DIR_MO);
    }
    assert_eq_u(count_legs(&m), (unsigned)MODEM_MAX_CALL_LEGS, "table fills to MODEM_MAX_CALL_LEGS");
    assert_true(!m.overflow, "no overflow at capacity");
    call_model_on_event(&m, (uint8_t)(MODEM_MAX_CALL_LEGS + 1u), true, CALL_LEG_DIALING, CALL_DIR_MO);
    assert_true(!m.overflow, "an out-of-range id is rejected at the guard, never overflowing (FIX-1)");
    assert_eq_u(m.reconcile_reasons & (unsigned)CALL_RC_OVERFLOW, 0u,
                "a rejected out-of-range id does not arm CALL_RC_OVERFLOW");
    assert_eq_u(count_legs(&m), (unsigned)MODEM_MAX_CALL_LEGS, "a rejected id never overwrites an existing leg");
}

/* §9.4: a RING with no active episode starts a FRESH episode — the ONLY place
 * incoming_number is cleared; RING carries no CLI (default valid). Arms PENDING_MT.
 * §9.5: any ring indication resets last_call_result to NONE. */
static void test_on_ring_fresh_episode(void) {
    call_model_t m;
    tinit(&m);
    call_model_set_now(&m, 400u);
    call_model_on_ring(&m);
    assert_true(m.pending_mt.active, "fresh RING marks pending_mt active");
    assert_true(m.pending_mt.number[0] == '\0', "fresh episode clears incoming_number");
    assert_eq_u(m.pending_mt.cli_validity, 0u, "RING alone defaults CLI validity to valid (not withheld)");
    assert_eq_u(m.pending_mt.first_ring_ms, 400u, "first_ring_ms stamped on fresh episode");
    assert_eq_u(m.pending_mt.last_ring_ms, 400u, "last_ring_ms stamped on fresh episode");
    assert_true(!m.pending_mt.bound_valid, "fresh episode is unbound");
    assert_true((m.reconcile_reasons & (unsigned)CALL_RC_PENDING_MT) != 0u, "fresh RING arms CALL_RC_PENDING_MT");
    assert_eq_u((unsigned)m.published.last_call_result, (unsigned)MODEM_CALL_RESULT_NONE,
                "a ring indication resets last_call_result to NONE");
}

/* §9.4: repeated RING updates the SAME episode — keeps the CLIP-provided number,
 * keeps first_ring_ms, only advances last_ring_ms. */
static void test_on_ring_repeat_keeps_number(void) {
    call_model_t m;
    tinit(&m);
    call_model_set_now(&m, 400u);
    call_model_on_ring(&m);                                  /* fresh, number cleared */
    call_model_set_now(&m, 450u);
    call_model_on_clip(&m, "+358401234567", 0u);             /* names the episode */
    call_model_set_now(&m, 3400u);
    call_model_on_ring(&m);                                  /* repeat */
    assert_true(strcmp(m.pending_mt.number, "+358401234567") == 0,
                "a repeat RING keeps the CLIP-provided number (no clear)");
    assert_eq_u(m.pending_mt.first_ring_ms, 400u, "repeat RING keeps the original first_ring_ms");
    assert_eq_u(m.pending_mt.last_ring_ms, 3400u, "repeat RING advances last_ring_ms");
}

/* §9.5/§9.4: a ring is orthogonal to the connected latch — it resets last_call_result
 * (harmless call-waiting reset) but never touches second_call_result. */
static void test_on_ring_resets_latch_only(void) {
    call_model_t m;
    tinit(&m);
    call_model_set_now(&m, 10u);
    call_model_on_event(&m, 1u, true, CALL_LEG_ACTIVE, CALL_DIR_MT);   /* CONNECTED */
    m.published.second_call_result = MODEM_CALL_RESULT_BUSY;      /* sentinel */
    call_model_set_now(&m, 20u);
    call_model_on_ring(&m);
    assert_eq_u((unsigned)m.published.last_call_result, (unsigned)MODEM_CALL_RESULT_NONE,
                "a call-waiting ring resets last_call_result to NONE (harmless)");
    assert_eq_u((unsigned)m.published.second_call_result, (unsigned)MODEM_CALL_RESULT_BUSY,
                "a ring never touches second_call_result");
}

/* §9.4: +CLIP always refreshes number + CLI validity; it PROVIDES the number so it
 * never clears it; an over-long number is bounded to MODEM_PHONE_MAX. */
static void test_on_clip_refresh(void) {
    call_model_t m;
    tinit(&m);
    call_model_set_now(&m, 100u);
    call_model_on_ring(&m);
    call_model_set_now(&m, 120u);
    call_model_on_clip(&m, "0401112223", 1u);                    /* withheld */
    assert_true(strcmp(m.pending_mt.number, "0401112223") == 0, "CLIP sets the number");
    assert_eq_u(m.pending_mt.cli_validity, 1u, "CLIP sets CLI validity (withheld)");
    assert_eq_u(m.pending_mt.last_ring_ms, 120u, "CLIP advances last_ring_ms");

    /* a CLIP without a prior RING still marks an incoming episode (provides a number). */
    call_model_t m2;
    tinit(&m2);
    call_model_set_now(&m2, 700u);
    call_model_on_clip(&m2, "112", 0u);
    assert_true(m2.pending_mt.active, "a CLIP with no prior RING marks pending_mt active");
    assert_true(strcmp(m2.pending_mt.number, "112") == 0, "CLIP-only episode carries the number");
    assert_true((m2.reconcile_reasons & (unsigned)CALL_RC_PENDING_MT) != 0u, "CLIP-only arms CALL_RC_PENDING_MT");

    /* an over-long number is truncated to MODEM_PHONE_MAX, NUL-terminated (ASan guard). */
    char big[64];
    memset(big, '9', sizeof(big));
    big[sizeof(big) - 1u] = '\0';
    call_model_t m3;
    tinit(&m3);
    call_model_set_now(&m3, 1u);
    call_model_on_clip(&m3, big, 0u);
    assert_eq_u((unsigned)strlen(m3.pending_mt.number), (unsigned)MODEM_PHONE_MAX,
                "an over-long CLIP number is bounded to MODEM_PHONE_MAX");
}

/* A supplementary redirected-call indication may lead RING. It belongs to the
 * pending-MT episode, but must not present a phantom call until an alerting
 * observation arrives. Once bound, it retires with that exact leg. */
static void test_incoming_diverted_before_ring_binds_to_leg(void) {
    call_model_t m;
    tinit(&m);
    call_model_set_now(&m, 100u);
    call_model_on_incoming_diverted(&m);

    call_projection_t p;
    call_model_project(&m, &p);
    assert_true(m.pending_mt.active && !m.pending_mt.alert_observed,
                "redirected CSSU starts a hidden pre-id MT episode");
    assert_eq_u((unsigned)p.call_state, (unsigned)MODEM_CALL_IDLE,
                "CSSU alone does not present a phantom ringing call");
    assert_true(!p.incoming_diverted,
                "hidden redirect metadata is not published before alerting evidence");

    call_model_set_now(&m, 120u);
    call_model_on_ring(&m);
    call_model_on_clip(&m, "+15557654321", 0u);
    call_model_on_event(&m, 2u, true, CALL_LEG_INCOMING, CALL_DIR_MT);
    call_model_project(&m, &p);
    assert_eq_u((unsigned)p.call_state, (unsigned)MODEM_CALL_RINGING,
                "RING presents the redirected incoming episode");
    assert_true(p.incoming_diverted,
                "redirect marker follows the pending episode onto its incoming leg");
    assert_true(strcmp(p.incoming_number, "+15557654321") == 0,
                "redirect binding preserves the episode caller number");

    call_model_on_event(&m, 2u, true, CALL_LEG_ACTIVE, CALL_DIR_MT);
    call_model_project(&m, &p);
    assert_true(!p.incoming_diverted,
                "answering retires redirected incoming wording while retaining the call");
}

/* An id-bearing WAITING indication can precede CSSU. The late supplementary
 * metadata binds to that unique leg and does not leak after its rejection. */
static void test_incoming_diverted_after_waiting_binds_and_retires(void) {
    call_model_t m;
    tinit(&m);
    call_model_set_now(&m, 200u);
    call_model_on_event(&m, 1u, true, CALL_LEG_ACTIVE, CALL_DIR_MO);
    call_model_on_event(&m, 2u, true, CALL_LEG_WAITING, CALL_DIR_MT);
    call_model_on_incoming_diverted(&m);

    call_projection_t p;
    call_model_project(&m, &p);
    call_leg_t *waiting = find_leg(&m, 2u);
    assert_true(waiting != NULL && waiting->incoming_diverted,
                "late CSSU metadata binds to the unique waiting leg");
    assert_true(p.waiting_call && p.incoming_diverted,
                "redirected waiting leg is published beside the foreground call");

    call_model_on_event(&m, 2u, true, CALL_LEG_RELEASING, CALL_DIR_MT);
    call_model_project(&m, &p);
    assert_true(p.call_state == MODEM_CALL_ACTIVE && !p.waiting_call,
                "foreground call survives redirected waiting-leg rejection");
    assert_true(!p.incoming_diverted,
                "redirect marker retires with the rejected waiting leg");
}

/* CSSU carries no CLI. Binding it after an authoritative CLCC row must not
 * overwrite the row's caller number with the pending record's empty buffer. */
static void test_incoming_diverted_does_not_clobber_leg_cli(void) {
    call_model_t m;
    tinit(&m);
    call_model_set_now(&m, 300u);
    call_model_clcc_begin(&m, 0u);
    call_model_clcc_row(&m, 1u, CALL_DIR_MO, CALL_MODE_VOICE, 0u,
                        CALL_LEG_ACTIVE, "+15551230000");
    call_model_clcc_row(&m, 2u, CALL_DIR_MT, CALL_MODE_VOICE, 0u,
                        CALL_LEG_WAITING, "+15557650000");
    call_model_clcc_ok(&m, 0u);
    call_model_on_incoming_diverted(&m);

    call_leg_t *waiting = find_leg(&m, 2u);
    assert_true(waiting != NULL &&
                    strcmp(waiting->number, "+15557650000") == 0,
                "metadata-only CSSU leaves authoritative leg CLI intact");
}

static void test_orphan_incoming_diverted_expires(void) {
    call_model_t m;
    tinit(&m);
    call_model_set_now(&m, 1u);
    call_model_on_incoming_diverted(&m);
    call_model_set_now(&m, 10002u);
    call_model_tick(&m);
    assert_true(!m.pending_mt.active,
                "orphan redirected-call metadata expires before a future episode");
}

/* The real modem may emit the id-bearing waiting state before +CCWA. The later
 * CLI must bind to that unique leg, replace the older active MT compatibility
 * owner, and project the waiting caller immediately. */
static void test_ccwa_after_waiting_event_binds_existing_leg(void) {
    call_model_t m; tinit(&m); call_model_set_now(&m, 100u);
    call_model_on_ring(&m);
    call_model_on_clip(&m, "ACTIVE-A", 0u);
    call_model_on_event(&m, 1u, true, CALL_LEG_INCOMING, CALL_DIR_MT);
    call_model_on_event(&m, 1u, true, CALL_LEG_ACTIVE, CALL_DIR_MT);

    call_model_on_event(&m, 2u, true, CALL_LEG_WAITING, CALL_DIR_MT);
    call_model_on_ring(&m);
    call_model_on_clip(&m, "WAITING-B", 0u);

    call_leg_t *active = find_leg(&m, 1u);
    call_leg_t *waiting = find_leg(&m, 2u);
    assert_true(m.pending_mt.bound_valid && m.pending_mt.bound_id == 2u,
                "late CCWA binds the unique already-present WAITING leg");
    assert_true(waiting != NULL && waiting->mt_cli_valid &&
                strcmp(waiting->number, "WAITING-B") == 0,
                "late CCWA metadata is copied onto the waiting leg");
    assert_true(active != NULL && !active->mt_cli_valid,
                "new waiting episode retires the older active MT flat-CLI owner");

    call_projection_t p;
    call_model_project(&m, &p);
    assert_true(p.waiting_call && strcmp(p.incoming_number, "WAITING-B") == 0,
                "late CCWA projects the actual waiting caller");
}

/* §9.5: a bare CONNECT final latches CONNECTED and does NOT arm a coarse-final
 * reconcile (a connect is not a departure). */
static void test_on_bare_final_connect(void) {
    call_model_t m;
    tinit(&m);
    call_model_on_bare_final(&m, MODEM_CALL_RESULT_CONNECTED, false);
    assert_eq_u((unsigned)m.published.last_call_result, (unsigned)MODEM_CALL_RESULT_CONNECTED,
                "a bare CONNECT latches CONNECTED");
    assert_eq_u(m.reconcile_reasons & (unsigned)CALL_RC_COARSE_FINAL, 0u,
                "a bare CONNECT does not arm a coarse-final reconcile");
}

/* §5.5/§16.2: a bare terminal final records the ATTEMPT result into the SOLE eligible
 * open setup/teardown txn (exactly-one-owner, never "newest by token") as an UNCERTAIN
 * hint, arms COARSE_FINAL, and never writes a terminal last_call_result (the handler is
 * non-authoritative). */
static void test_on_bare_final_records_txn(void) {
    call_model_t m;
    tinit(&m);
    /* A single open DIAL via the public path (no hand-poked m.txns[i].token). */
    uint32_t tok = call_model_request(&m, CALL_TXN_DIAL, 0u, false);
    call_model_txn_dispatched(&m, tok);
    call_model_on_bare_final(&m, MODEM_CALL_RESULT_NO_CARRIER, false);
    call_txn_t *t = find_txn_by_token(&m, tok);
    assert_true(t != NULL && t->result == MODEM_CALL_RESULT_NO_CARRIER,
                "the attempt result is recorded into the sole open DIAL txn");
    assert_true(t != NULL && t->state == TXN_UNCERTAIN,
                "the txn goes UNCERTAIN (CLCC confirms SUCCEEDED/FAILED)");
    assert_true((m.reconcile_reasons & (unsigned)CALL_RC_COARSE_FINAL) != 0u, "a bare final arms COARSE_FINAL");
    assert_eq_u((unsigned)m.published.last_call_result, (unsigned)MODEM_CALL_RESULT_NONE,
                "a bare terminal final never writes a terminal last_call_result");
}

/* §5.5: inside a CHLD window a bare final belongs to the released waiting/held leg —
 * NOT attributed to the primary txn — but still arms COARSE_FINAL for CLCC. */
static void test_on_bare_final_chld_window_excluded(void) {
    call_model_t m;
    tinit(&m);
    /* A single open DIAL via the public path (no hand-poked m.txns[i].token). */
    uint32_t tok = call_model_request(&m, CALL_TXN_DIAL, 0u, false);
    call_model_txn_dispatched(&m, tok);
    call_model_on_bare_final(&m, MODEM_CALL_RESULT_NO_CARRIER, true);
    call_txn_t *t = find_txn_by_token(&m, tok);
    assert_true(t != NULL && t->result == MODEM_CALL_RESULT_NONE,
                "a CHLD-window bare final is NOT attributed to the primary txn");
    assert_true(t != NULL && t->state == TXN_DISPATCHED, "the txn state is untouched in the CHLD window");
    assert_true((m.reconcile_reasons & (unsigned)CALL_RC_COARSE_FINAL) != 0u,
                "a CHLD-window bare final still arms COARSE_FINAL");
}

/* §5.5: a bare final NEVER removes a leg. */
static void test_on_bare_final_never_removes_leg(void) {
    call_model_t m;
    tinit(&m);
    call_model_set_now(&m, 10u);
    call_model_on_event(&m, 1u, true, CALL_LEG_ACTIVE, CALL_DIR_MT);
    call_model_on_bare_final(&m, MODEM_CALL_RESULT_NO_CARRIER, false);
    call_leg_t *L = find_leg(&m, 1u);
    assert_true(L != NULL, "a bare final never removes a leg");
    assert_eq_u((unsigned)L->state, (unsigned)CALL_LEG_ACTIVE, "the leg keeps its state after a bare final");
}

/* §9.5: second_call_result is orthogonal to the primary latch — a single-call
 * lifecycle (ring/clip/connect/bare-final, no 2nd-MO leg) never touches it. (Only a
 * 2nd-MO leg failing while the original survives writes it, via cm_latch_terminal.) */
static void test_latch_independence(void) {
    call_model_t m;
    tinit(&m);
    call_model_set_now(&m, 5u);
    m.published.second_call_result = MODEM_CALL_RESULT_BUSY;      /* sentinel */
    call_model_on_ring(&m);
    call_model_on_clip(&m, "123", 0u);
    call_model_on_event(&m, 1u, true, CALL_LEG_ACTIVE, CALL_DIR_MT);
    call_model_on_bare_final(&m, MODEM_CALL_RESULT_NO_CARRIER, false);
    assert_eq_u((unsigned)m.published.second_call_result, (unsigned)MODEM_CALL_RESULT_BUSY,
                "a single-call lifecycle never writes second_call_result");
}

/* FIX-1 (CRITICAL, OOB gen_next[] write): an id past MODEM_MAX_CALL_LEGS indexes
 * gen_next[MODEM_MAX_CALL_LEGS+1] out of bounds in cm_alloc_leg. The entry guard must
 * reject it as a no-op (UBSan/ASan would abort on the OOB struct write otherwise). */
static void test_on_event_id_out_of_range_noop(void) {
    call_model_t m;
    tinit(&m);
    call_model_set_now(&m, 10u);
    call_model_on_event(&m, 200u, true, CALL_LEG_DIALING, CALL_DIR_MO);   /* id > 7 -> reject */
    assert_eq_u(count_legs(&m), 0u, "an out-of-range id adds no leg (no OOB gen_next[] write)");
    assert_eq_u(m.semantic_revision, 0u, "an out-of-range id is a no-op (no revision bump)");
    assert_true(!m.overflow, "an out-of-range id is rejected before the table, not flagged overflow");
    /* the boundary id (== MODEM_MAX_CALL_LEGS) is still accepted. */
    call_model_on_event(&m, (uint8_t)MODEM_MAX_CALL_LEGS, true, CALL_LEG_DIALING, CALL_DIR_MO);
    assert_true(find_leg(&m, (uint8_t)MODEM_MAX_CALL_LEGS) != NULL,
                "the boundary id MODEM_MAX_CALL_LEGS is accepted");
}

/* FIX-2 (stale pending_terminal): the id-blind bare-final cause register is one-shot.
 * A leg releasing with a recorded NO_CARRIER journals it AND clears the register, so a
 * later UNRELATED clean leg release does NOT inherit the stale NO_CARRIER. */
static void test_pending_terminal_cleared_after_eviction(void) {
    call_model_t m;
    tinit(&m);
    call_model_set_now(&m, 10u);
    /* leg A: connects, then a remote bare NO_CARRIER, then RELEASED -> journals NO_CARRIER. */
    call_model_on_event(&m, 1u, true, CALL_LEG_ACTIVE, CALL_DIR_MT);
    call_model_on_bare_final(&m, MODEM_CALL_RESULT_NO_CARRIER, false);   /* records pending_terminal */
    call_model_on_event(&m, 1u, true, CALL_LEG_RELEASING, CALL_DIR_UNKNOWN);   /* evict, latch NO_CARRIER */
    assert_eq_u((unsigned)m.pending_terminal, (unsigned)MODEM_CALL_RESULT_NONE,
                "pending_terminal is cleared once its eviction consumes it");
    call_projection_t p1; call_model_project(&m, &p1);
    assert_eq_u((unsigned)m.published.last_call_result, (unsigned)MODEM_CALL_RESULT_NO_CARRIER,
                "A's NO_CARRIER drains to last_call_result");
    /* Isolate leg C's contribution (white-box) so the inherit-or-not is observable. */
    m.published.last_call_result = MODEM_CALL_RESULT_NONE;
    /* leg C: a later, unrelated MO dial the user cancels cleanly (DIALING -> RELEASED,
     * never ACTIVE, so it does NOT clear pending_terminal itself). With the stale bug it
     * would inherit A's NO_CARRIER in its terminal journal entry. Its semantic cause
     * must remain NONE even though the Phase-1 flat compatibility latch now translates
     * a final primary teardown to legacy NO_CARRIER (recorded A4). */
    call_model_on_event(&m, 2u, true, CALL_LEG_DIALING, CALL_DIR_MO);
    call_model_on_event(&m, 2u, true, CALL_LEG_RELEASING, CALL_DIR_UNKNOWN);   /* clean cancel */
    assert_eq_u(m.journal_count, 1u,
                "the unrelated clean release records one terminal entry");
    assert_eq_u((unsigned)m.journal[0].result, (unsigned)MODEM_CALL_RESULT_NONE,
                "the unrelated clean release does NOT inherit the stale terminal cause");
    call_projection_t p2; call_model_project(&m, &p2);
    assert_eq_u((unsigned)m.published.last_call_result, (unsigned)MODEM_CALL_RESULT_NO_CARRIER,
                "bench-recorded compatibility: final unanswered primary release projects NO_CARRIER");
}

/* FIX-2b: on_bare_final's CONNECTED branch also clears a stale pending_terminal. */
static void test_bare_connect_clears_pending_terminal(void) {
    call_model_t m;
    tinit(&m);
    m.pending_terminal = MODEM_CALL_RESULT_NO_CARRIER;    /* a stale cause from an earlier attempt */
    call_model_on_bare_final(&m, MODEM_CALL_RESULT_CONNECTED, false);
    assert_eq_u((unsigned)m.pending_terminal, (unsigned)MODEM_CALL_RESULT_NONE,
                "a bare CONNECT clears the stale pending_terminal cause");
    assert_eq_u((unsigned)m.published.last_call_result, (unsigned)MODEM_CALL_RESULT_CONNECTED,
                "a bare CONNECT still latches CONNECTED");
}

/* FIX-3 (missing generation check in the 2nd-MO loop): a leg that reached ACTIVE and
 * departs remotely with no owning txn (a STALE second_mo DIAL txn shares its bound_id
 * but a DIFFERENT generation) must be treated as a PRIMARY departure — the loop must
 * gen-qualify so the stale txn cannot claim it as a 2nd-MO failure. §16.4: the primary
 * remote drop routes NO_CARRIER to last_call_result, and second_call_result is left
 * UNTOUCHED (the stale wrong-gen txn does not route it to the 2nd-MO latch). */
static void test_released_2ndmo_gen_qualified(void) {
    call_model_t m;
    tinit(&m);
    call_model_set_now(&m, 10u);
    /* the genuine leg: id 2, generation 1, reaches ACTIVE (was_answered) then releases. */
    call_model_on_event(&m, 2u, true, CALL_LEG_ACTIVE, CALL_DIR_MO);
    call_leg_t *L = find_leg(&m, 2u);
    assert_true(L != NULL && L->generation == 1u, "leg 2 at generation 1");
    m.published.second_call_result = MODEM_CALL_RESULT_BUSY;   /* sentinel: must stay untouched */
    /* a STALE second_mo DIAL txn shares bound_id 2 but a DIFFERENT (old) generation and
     * was never witnessed_active — the buggy id-only match would claim it as 2nd-MO. */
    m.txns[0].in_use = true;
    m.txns[0].second_mo = true;
    m.txns[0].kind = CALL_TXN_DIAL;
    m.txns[0].bound_id = 2u;
    m.txns[0].bound_gen = 99u;                 /* != leg generation */
    m.txns[0].witnessed_active = false;
    call_model_on_event(&m, 2u, true, CALL_LEG_RELEASING, CALL_DIR_UNKNOWN);   /* remote release */
    call_projection_t p; call_model_project(&m, &p);
    assert_eq_u((unsigned)m.published.last_call_result, (unsigned)MODEM_CALL_RESULT_NO_CARRIER,
                "§16.4: a stale wrong-generation 2nd-MO txn does NOT claim the departure -> primary NO_CARRIER on last");
    assert_eq_u((unsigned)m.published.second_call_result, (unsigned)MODEM_CALL_RESULT_BUSY,
                "the stale wrong-gen 2nd-MO txn does NOT route the result to second_call_result");
}

/* A 2nd-MO leg rejected BUSY publishes second_call_result=BUSY -- the ACTUAL
 * result, not the NO_ANSWER the old survivor/witnessed heuristic produced.
 * Drives the REAL dispatch path
 * (call_model_request/txn_dispatched/txn_accepted) instead of hand-poking
 * m.txns[0] directly: a hand-poke would bypass call_model_txn_dispatched's
 * DIAL-reset code entirely and pass SPURIOUSLY regardless of whether that
 * reset correctly preserves a live primary's last_call_result=CONNECTED
 * across a 2nd-MO dispatch -- it would never exercise
 * call_model_txn_dispatched at all. */
static void test_2ndmo_busy_to_second(void) {
    call_model_t m; tinit(&m); call_model_set_now(&m, 10u);
    call_model_on_event(&m, 1u, true, CALL_LEG_ACTIVE, CALL_DIR_MO);   /* the surviving original */
    uint32_t tok = call_model_request(&m, CALL_TXN_DIAL, 0u, true);    /* 2nd-MO New-call dial (B) */
    call_model_txn_dispatched(&m, tok);
    txn_ok(&m, tok);
    call_model_on_event(&m, 2u, true, CALL_LEG_DIALING, CALL_DIR_MO);  /* the 2nd-MO leg appears -> binds */
    call_model_on_bare_final(&m, MODEM_CALL_RESULT_BUSY, false); /* records BUSY into the 2nd-MO txn */
    call_model_on_event(&m, 2u, true, CALL_LEG_RELEASING, CALL_DIR_MO);/* the 2nd leg drops */
    call_projection_t p; call_model_project(&m, &p);
    assert_eq_u((unsigned)m.published.second_call_result, (unsigned)MODEM_CALL_RESULT_BUSY,
                "2nd-MO BUSY -> second_call_result=BUSY (not NO_ANSWER)");
    assert_eq_u((unsigned)m.published.last_call_result, (unsigned)MODEM_CALL_RESULT_CONNECTED,
                "the surviving original keeps last_call_result CONNECTED through the real dispatch path");
}

/* A 2nd-MO leg lost ONLY via CLCC (no URC, txn resolved after eviction) still
 * publishes a non-NONE second_call_result (a path that pushed no journal entry
 * would publish NONE). Drives the REAL dispatch path
 * (call_model_request/txn_dispatched/txn_accepted), mirroring the
 * sibling test_2ndmo_busy_to_second -- a hand-poke of m.txns[0]
 * (state=TXN_BOUND, second_mo=true, a hand-copied bound_id/bound_gen) would pass
 * SPURIOUSLY regardless of whether txn_bind_dials actually binds a dispatched
 * 2nd-MO DIAL to the newly-appearing leg; it would never exercise that code. */
static void test_2ndmo_clcc_lost_to_second(void) {
    call_model_t m; tinit(&m); call_model_set_now(&m, 10u);
    call_model_on_event(&m, 1u, true, CALL_LEG_ACTIVE, CALL_DIR_MO);   /* survivor */
    uint32_t tok = call_model_request(&m, CALL_TXN_DIAL, 0u, true);    /* 2nd-MO New-call dial (B) */
    call_model_txn_dispatched(&m, tok);
    txn_ok(&m, tok);
    call_model_on_event(&m, 2u, true, CALL_LEG_DIALING, CALL_DIR_MO);  /* the 2nd-MO leg appears -> binds */
    /* two clean CLCC snapshots that list only leg 1 -> leg 2 is the absent set -> evicted. */
    call_model_clcc_begin(&m, 0u);
    call_model_clcc_row(&m, 1u, CALL_DIR_MO, CALL_MODE_VOICE, 0u, CALL_LEG_ACTIVE, "111");
    call_model_clcc_ok(&m, 0u);
    call_model_clcc_begin(&m, 0u);
    call_model_clcc_row(&m, 1u, CALL_DIR_MO, CALL_MODE_VOICE, 0u, CALL_LEG_ACTIVE, "111");
    call_model_clcc_ok(&m, 0u);
    assert_true(find_leg(&m, 2u) == NULL, "the 2nd leg is evicted via CLCC");
    call_projection_t p; call_model_project(&m, &p);
    assert_true(m.published.second_call_result != MODEM_CALL_RESULT_NONE,
                "2nd-MO CLCC-lost publishes a non-NONE second_call_result");
    assert_eq_u((unsigned)m.published.second_call_result, (unsigned)MODEM_CALL_RESULT_NO_ANSWER,
                "2nd-MO CLCC-lost (never answered) -> second_call_result=NO_ANSWER");
}

/* A held/waiting departure while another leg survives goes to last_call_result,
 * NOT second_call_result -- routing is by ownership, never by "a survivor exists". */
static void test_held_departure_to_last(void) {
    call_model_t m; tinit(&m); call_model_set_now(&m, 10u);
    call_model_on_event(&m, 1u, true, CALL_LEG_ACTIVE, CALL_DIR_MO);   /* survivor */
    call_model_on_event(&m, 2u, true, CALL_LEG_HELD, CALL_DIR_MT);     /* a held leg, NOT a 2nd-MO dial */
    m.pending_terminal = MODEM_CALL_RESULT_NO_CARRIER;          /* the held leg drops remotely */
    m.published.second_call_result = MODEM_CALL_RESULT_BUSY;     /* sentinel: must stay untouched */
    call_model_on_event(&m, 2u, true, CALL_LEG_RELEASING, CALL_DIR_MT);
    call_projection_t p; call_model_project(&m, &p);
    assert_eq_u((unsigned)m.published.last_call_result, (unsigned)MODEM_CALL_RESULT_NO_CARRIER,
                "a held departure with a survivor -> last_call_result (not second)");
    assert_eq_u((unsigned)m.published.second_call_result, (unsigned)MODEM_CALL_RESULT_BUSY,
                "a non-2nd-MO departure never touches second_call_result");
}

/* Interface (LTE boundary): id_valid==false is a COARSE observation with no reliable
 * call id -- it must NOT create/mutate a leg, and instead schedules a CLCC (raises
 * COARSE_FINAL + edge-pulls next_clcc_ms). id_valid==true behaves normally. */
static void test_id_valid_coarse_observation(void) {
    call_model_t m;
    tinit(&m);
    call_model_set_now(&m, 1000u);
    m.next_clcc_ms = 100000u;
    call_model_on_event(&m, 3u, false, CALL_LEG_ACTIVE, CALL_DIR_MT);   /* coarse: id unreliable */
    assert_true(count_legs(&m) == 0u, "id_valid=false creates no leg");
    assert_true((m.reconcile_reasons & (unsigned)CALL_RC_COARSE_FINAL) != 0u,
                "id_valid=false schedules a CLCC (COARSE_FINAL raised)");
    assert_true(m.next_clcc_ms == 1000u, "id_valid=false edge-pulls next_clcc_ms to now");
    /* a valid event still creates the leg normally. */
    call_model_on_event(&m, 3u, true, CALL_LEG_ACTIVE, CALL_DIR_MT);
    assert_true(find_leg(&m, 3u) != NULL, "id_valid=true creates the leg normally");
}

/* A bare-final cause recorded (and owner-attributed) for leg A
 * must survive an UNRELATED leg B's later ACTIVE observation (here via a ~4s
 * CLCC keepalive) -- A's cause is consumed ONLY by A's own eviction. */
static void test_pending_terminal_owner_protects_against_unrelated_active(void) {
    call_model_t m; tinit(&m); call_model_set_now(&m, 1000u);
    /* leg A: id 1, reaches ACTIVE via a TENTATIVE (dir-unconfirmed) bind so its owning
     * DIAL txn stays OPEN (not auto-resolved SUCCEEDED in the same on_event call, §7.4)
     * -- on_bare_final's SOLE-eligible-open-txn match (§16.2, never "newest by token")
     * then attributes the cause to A specifically. Drive the real DIAL path
     * (no hand-poked m.txns[i].token). */
    uint32_t tok = call_model_request(&m, CALL_TXN_DIAL, 0u, false);
    call_model_txn_dispatched(&m, tok);
    call_model_on_event(&m, 1u, true, CALL_LEG_ACTIVE, CALL_DIR_UNKNOWN);   /* binds tentatively */
    call_leg_t *a = find_leg(&m, 1u);
    assert_true(a != NULL && a->generation == 1u, "precondition: leg A at generation 1");
    call_txn_t *dialtx = find_txn_by_token(&m, tok);
    assert_true(dialtx != NULL && !dialtx->dir_confirmed && dialtx->state == TXN_BOUND,
                "precondition: the DIAL binds tentatively (open, dir unconfirmed)");

    call_model_set_now(&m, 4000u);
    call_model_on_bare_final(&m, MODEM_CALL_RESULT_NO_CARRIER, false);
    assert_eq_u((unsigned)m.pending_terminal, (unsigned)MODEM_CALL_RESULT_NO_CARRIER,
                "precondition: A's bare-final cause recorded");
    assert_eq_u(m.pending_terminal_owner_id, 1u, "precondition: owner stamped to A");

    /* a ~4s CLCC keepalive reports an UNRELATED leg B (id 2) ACTIVE. */
    call_model_set_now(&m, 8000u);
    call_model_clcc_begin(&m, 0u);
    call_model_clcc_row(&m, 2u, CALL_DIR_MO, CALL_MODE_VOICE, 0u, CALL_LEG_ACTIVE, "5559999");
    call_model_clcc_ok(&m, 0u);

    assert_eq_u((unsigned)m.pending_terminal, (unsigned)MODEM_CALL_RESULT_NO_CARRIER,
                "an unrelated leg B's ACTIVE observation must NOT wipe A's recorded cause");

    /* A's own later eviction still latches its NO_CARRIER cause. */
    call_model_on_event(&m, 1u, true, CALL_LEG_RELEASING, CALL_DIR_UNKNOWN);
    call_projection_t p; call_model_project(&m, &p);
    assert_eq_u((unsigned)m.published.last_call_result, (unsigned)MODEM_CALL_RESULT_NO_CARRIER,
                "A's later eviction still latches NO_CARRIER (the cause survived)");
}

/* The eviction sites (on_event's RELEASING branch and
 * clcc_evict_leg) must not hand a lingering OWNER's pending_terminal cause to
 * a DIFFERENT, non-owning leg's eviction -- that would (a) wrongly latch the
 * non-owner's clean departure to the owner's cause AND (b) STRIP the register
 * so the true owner's own later eviction gets nothing. Build an ATTRIBUTED
 * cause for leg A (owner_id != 0, via the real on_bare_final attribution
 * mechanism), then evict an UNRELATED leg C cleanly (CLCC
 * ACTIVE connect + an id-scoped release) WHILE A's cause is still pending. */
static void test_pending_terminal_not_stolen_by_unrelated_eviction(void) {
    call_model_t m; tinit(&m); call_model_set_now(&m, 1000u);

    /* leg A (id 1): a DIAL binds tentatively (dir unconfirmed) so its owning txn stays
     * OPEN and on_bare_final's SOLE-eligible-open-txn match (§16.2) attributes the cause
     * to A specifically (mirrors the owner-attribution setup above). Drives
     * the real DIAL path (no hand-poked m.txns[i].token). */
    uint32_t tok = call_model_request(&m, CALL_TXN_DIAL, 0u, false);
    call_model_txn_dispatched(&m, tok);
    call_model_on_event(&m, 1u, true, CALL_LEG_ACTIVE, CALL_DIR_UNKNOWN);   /* binds tentatively */

    call_model_set_now(&m, 2000u);
    call_model_on_bare_final(&m, MODEM_CALL_RESULT_BUSY, false);
    assert_eq_u((unsigned)m.pending_terminal, (unsigned)MODEM_CALL_RESULT_BUSY,
                "precondition: A's bare-final cause recorded");
    assert_eq_u(m.pending_terminal_owner_id, 1u, "precondition: owner stamped to A");

    /* leg C (id 2): a completely UNRELATED call connects (via CLCC) then ends
     * CLEANLY (no bare-final cause of its own) WHILE A's BUSY cause is still
     * pending, unconsumed. */
    call_model_set_now(&m, 3000u);
    call_model_clcc_begin(&m, 0u);
    call_model_clcc_row(&m, 2u, CALL_DIR_MT, CALL_MODE_VOICE, 0u, CALL_LEG_ACTIVE, "5551234");
    call_model_clcc_ok(&m, 0u);
    assert_eq_u((unsigned)m.published.last_call_result, (unsigned)MODEM_CALL_RESULT_CONNECTED,
                "precondition: C's connect latches CONNECTED");

    call_model_on_event(&m, 2u, true, CALL_LEG_RELEASING, CALL_DIR_UNKNOWN);   /* C's clean eviction */
    call_projection_t p; call_model_project(&m, &p);
    assert_eq_u((unsigned)m.published.last_call_result, (unsigned)MODEM_CALL_RESULT_CONNECTED,
                "an unrelated leg C's clean eviction must NOT steal A's pending BUSY cause");
    assert_eq_u((unsigned)m.pending_terminal, (unsigned)MODEM_CALL_RESULT_BUSY,
                "A's cause survives an unrelated leg's eviction (neither inherited nor stripped)");

    /* A's OWN later eviction still latches its BUSY cause -- the cause reached
     * its true owner intact. */
    call_model_on_event(&m, 1u, true, CALL_LEG_RELEASING, CALL_DIR_UNKNOWN);
    call_model_project(&m, &p);
    assert_eq_u((unsigned)m.published.last_call_result, (unsigned)MODEM_CALL_RESULT_BUSY,
                "A's own eviction still latches BUSY");
}

/* A 2nd-MO leg that completed setup and later departs is no longer a setup
 * failure. second_call_result stays NONE; the terminal journal owns the later
 * established-call departure.
 * Drives the REAL dispatch path (see the
 * sibling test_2ndmo_clcc_lost_to_second's comment -- a hand-poke
 * of m.txns[0] would bypass txn_bind_dials entirely and pass spuriously). */
static void test_minor_2ndmo_connected_then_departed_not_latched_connected(void) {
    call_model_t m; tinit(&m); call_model_set_now(&m, 10u);
    call_model_on_event(&m, 1u, true, CALL_LEG_ACTIVE, CALL_DIR_MO);   /* survivor A */
    uint32_t tok = call_model_request(&m, CALL_TXN_DIAL, 0u, true);
    call_model_txn_dispatched(&m, tok);
    txn_ok(&m, tok);
    call_model_on_event(&m, 2u, true, CALL_LEG_DIALING, CALL_DIR_MO);  /* the 2nd-MO leg B appears -> binds */

    /* B connects -- the real engine resolves the DIAL txn SUCCEEDED/CONNECTED
     * (it stays in_use, terminal-but-not-reclaimed, per the pool's lazy reclaim). */
    call_model_on_event(&m, 2u, true, CALL_LEG_ACTIVE, CALL_DIR_MO);
    call_txn_t *tx = find_txn_by_token(&m, tok);
    assert_true(tx != NULL && tx->state == TXN_SUCCEEDED && tx->result == MODEM_CALL_RESULT_CONNECTED,
                "precondition: B's DIAL resolves SUCCEEDED/CONNECTED");

    /* B departs cleanly -- no bare-final cause was ever recorded. */
    call_model_on_event(&m, 2u, true, CALL_LEG_RELEASING, CALL_DIR_MO);
    call_projection_t p; call_model_project(&m, &p);
    assert_true(m.published.second_call_result != MODEM_CALL_RESULT_CONNECTED,
                "a connected-then-departed 2nd-MO leg never latches second_call_result=CONNECTED");
    assert_eq_u((unsigned)m.published.second_call_result, (unsigned)MODEM_CALL_RESULT_NONE,
                "an established second leg's later departure is not a New-call setup failure");
}

/* The id-scoped RELEASING branch of on_event must reconcile
 * transactions before it returns -- a New-call (2nd-MO) DIAL txn bound to the
 * JUST-RELEASED leg must be resolved (FAILED, its leg gone) in the SAME step,
 * or its stale "still in setup" state keeps R5 New-call domination alive over
 * the true surviving-leg projection (sole-HELD + call_on_hold, R7).
 * Drives the REAL dispatch path (see the
 * sibling test_2ndmo_clcc_lost_to_second's comment -- a hand-poke
 * of m.txns[0] would bypass txn_bind_dials entirely and pass spuriously). */
static void test_id_scoped_released_reconciles_newcall_txn(void) {
    call_model_t m;
    tinit(&m);
    call_model_set_now(&m, 10u);
    call_model_on_event(&m, 1u, true, CALL_LEG_HELD, CALL_DIR_MO);        /* A: held */
    uint32_t tok = call_model_request(&m, CALL_TXN_DIAL, 0u, true);
    call_model_txn_dispatched(&m, tok);
    txn_ok(&m, tok);
    call_model_on_event(&m, 2u, true, CALL_LEG_DIALING, CALL_DIR_MO);     /* B: New-call setup leg -> binds */

    call_projection_t pre;
    call_model_project(&m, &pre);
    assert_eq_u((unsigned)pre.call_state, (unsigned)MODEM_CALL_DIALING,
                "precondition: the New-call setup dominates -> DIALING (R5)");

    call_model_on_event(&m, 2u, true, CALL_LEG_RELEASING, CALL_DIR_UNKNOWN);  /* id-scoped release of B */
    assert_true(find_leg(&m, 2u) == NULL, "B evicted");

    call_projection_t p;
    call_model_project(&m, &p);
    assert_eq_u((unsigned)p.call_state, (unsigned)MODEM_CALL_ACTIVE,
                "B's id-scoped release reconciles the DIAL txn -> A's true role (sole HELD, R7)");
    assert_true(p.call_on_hold,
                "A projects call_on_hold, no longer masked by a stale New-call DIALING");
    assert_eq_u(p.active_call_id, 1u, "active_call_id = A (the surviving held leg)");
}

/* §16.2/§16.3: an uncorrelated bare BUSY with NO leg records a PROVISIONAL cause
 * against the sole eligible DIAL; a later CLEAN CLCC proving no leg exists LATCHES
 * it to last_call_result — fixes "primary BUSY stays DIALING then IDLE NONE". */
static void test_no_leg_busy_latches_via_clcc(void) {
    call_model_t m; tinit(&m); call_model_set_now(&m, 1000u);
    uint32_t tok = call_model_request(&m, CALL_TXN_DIAL, 0u, false);
    call_model_txn_dispatched(&m, tok);
    txn_ok(&m, tok);                                             /* ATD final OK */
    call_model_on_bare_final(&m, MODEM_CALL_RESULT_BUSY, false); /* uncorrelated BUSY, no leg */
    call_txn_t *tx = find_txn_by_token(&m, tok);
    assert_true(tx && tx->state == TXN_UNCERTAIN && tx->result == MODEM_CALL_RESULT_BUSY,
                "no-leg: the DIAL records a PROVISIONAL BUSY (UNCERTAIN)");
    assert_eq_u((unsigned)m.published.last_call_result, (unsigned)MODEM_CALL_RESULT_NONE,
                "no-leg: BUSY is not yet authoritative before the clean CLCC");

    call_model_set_now(&m, 1400u);
    call_model_clcc_begin(&m, 0u);                               /* zero rows -> proves no leg */
    call_model_clcc_ok(&m, 0u);
    assert_eq_u((unsigned)m.published.last_call_result, (unsigned)MODEM_CALL_RESULT_BUSY,
                "§16.3: a clean CLCC proving no leg latches the provisional BUSY to last_call_result");
    tx = find_txn_by_token(&m, tok);
    assert_true(tx == NULL || tx->state == TXN_FAILED, "§16.3: the no-leg DIAL resolves FAILED");
    assert_true(!call_model_wants_clcc(&m), "§16.3: the model drains after the no-leg failure latches");
}

/* §16.2: a genuinely-unsolicited bare final with >=2 eligible open setup txns is
 * NOT attributed to any of them ("never the newest numeric token") — it stays a
 * provisional unattributed register for CLCC to resolve. */
static void test_bare_final_two_owners_unattributed(void) {
    call_model_t m; tinit(&m); call_model_set_now(&m, 1000u);
    uint32_t t1 = call_model_request(&m, CALL_TXN_DIAL, 0u, false);
    call_model_txn_dispatched(&m, t1);
    /* Distinct latch roles may legitimately overlap; two primary DIAL claimants may
     * not, because a fresh primary retry supersedes the unbound predecessor. */
    uint32_t t2 = call_model_request(&m, CALL_TXN_DIAL, 0u, true);
    call_model_txn_dispatched(&m, t2);
    call_model_on_bare_final(&m, MODEM_CALL_RESULT_NO_CARRIER, false);
    call_txn_t *x1 = find_txn_by_token(&m, t1), *x2 = find_txn_by_token(&m, t2);
    assert_true(x1 && x1->result == MODEM_CALL_RESULT_NONE && x1->state == TXN_DISPATCHED,
                "§16.2: not attributed to the first eligible DIAL");
    assert_true(x2 && x2->result == MODEM_CALL_RESULT_NONE && x2->state == TXN_DISPATCHED,
                "§16.2: not attributed to the second DIAL (no newest-token guess)");
    assert_eq_u((unsigned)m.pending_terminal, (unsigned)MODEM_CALL_RESULT_NO_CARRIER,
                "the cause is held as a provisional register");
    assert_eq_u(m.pending_terminal_owner_id, 0u, "the register is unattributed (owner 0)");
}

/* §16.2 CMD_ERROR: a correlated rejection terminates the txn FAILED immediately,
 * carrying its owned cause routed to the latch — no wait on the policy timer, no
 * leg needed. */
static void test_cmd_error_terminates_with_cause(void) {
    /* explicit cause (BUSY) -> last_call_result=BUSY. */
    { call_model_t m; tinit(&m); call_model_set_now(&m, 1000u);
      uint32_t tok = call_model_request(&m, CALL_TXN_DIAL, 0u, false);
      call_model_txn_dispatched(&m, tok);
      modem_cmd_result_t r = { .status = CMD_ERROR, .has_call_result = true,
                               .call_result = MODEM_CALL_RESULT_BUSY };
      call_model_txn_command_result(&m, tok, r);
      call_txn_t *tx = find_txn_by_token(&m, tok);
      assert_true(tx && tx->state == TXN_FAILED, "CMD_ERROR terminates the txn (FAILED)");
      assert_eq_u((unsigned)tx->result, (unsigned)MODEM_CALL_RESULT_BUSY, "the txn carries its BUSY cause");
      assert_eq_u((unsigned)m.published.last_call_result, (unsigned)MODEM_CALL_RESULT_BUSY,
                  "§16.2: CMD_ERROR routes the cause to last_call_result (no policy-timer wait)");
      assert_true(!call_model_wants_clcc(&m), "CMD_ERROR releases the in-flight gate + drains"); }

    /* no explicit cause -> a sensible default (NO_CARRIER). */
    { call_model_t m; tinit(&m); call_model_set_now(&m, 1000u);
      uint32_t tok = call_model_request(&m, CALL_TXN_DIAL, 0u, false);
      call_model_txn_dispatched(&m, tok);
      modem_cmd_result_t r = { .status = CMD_ERROR, .has_call_result = false };
      call_model_txn_command_result(&m, tok, r);
      assert_eq_u((unsigned)m.published.last_call_result, (unsigned)MODEM_CALL_RESULT_NO_CARRIER,
                  "§16.2: a causeless CMD_ERROR latches a sensible default (NO_CARRIER)"); }
}

/* §16.2 CMD_TIMEOUT: the txn goes UNCERTAIN (still bindable), edge-pulls an
 * immediate CLCC, and does NOT latch a result (not a failure, not cancellation). */
static void test_cmd_timeout_uncertain_immediate_clcc(void) {
    call_model_t m; tinit(&m); call_model_set_now(&m, 5000u);
    uint32_t tok = call_model_request(&m, CALL_TXN_DIAL, 0u, false);
    call_model_txn_dispatched(&m, tok);
    call_model_on_event(&m, 1u, true, CALL_LEG_DIALING, CALL_DIR_MO);   /* binds the DIAL */
    m.next_clcc_ms = 999999u;                                          /* far-future schedule */
    modem_cmd_result_t r = { .status = CMD_TIMEOUT };
    call_model_txn_command_result(&m, tok, r);
    call_txn_t *tx = find_txn_by_token(&m, tok);
    assert_true(tx && tx->state == TXN_UNCERTAIN, "§16.2: CMD_TIMEOUT -> UNCERTAIN (still bindable)");
    assert_true(m.next_clcc_ms == 5000u,
                "§16.2: CMD_TIMEOUT edge-pulls next_clcc_ms to now (immediate CLCC)");
    assert_eq_u((unsigned)m.published.last_call_result, (unsigned)MODEM_CALL_RESULT_NONE,
                "§16.2: CMD_TIMEOUT is not a failure -> no result latched");
    assert_true(find_leg(&m, 1u) != NULL, "CMD_TIMEOUT does not tear down the leg (no cancellation)");
}

/* §16.5: a HOLD rejected by the modem is a supplementary-service failure
 * (OUTPUT_FREE sink) — it terminates FAILED but must NOT write last_call_result,
 * and leaves the active call intact. */
static void test_rejected_hold_no_last_result(void) {
    call_model_t m; tinit(&m); call_model_set_now(&m, 1000u);
    call_model_on_event(&m, 1u, true, CALL_LEG_ACTIVE, CALL_DIR_MO);    /* connected -> CONNECTED */
    assert_eq_u((unsigned)m.published.last_call_result, (unsigned)MODEM_CALL_RESULT_CONNECTED,
                "precondition: the active call latched CONNECTED");
    uint32_t tok = call_model_request(&m, CALL_TXN_HOLD, 0u, false);    /* resolves to the active leg */
    call_model_txn_dispatched(&m, tok);
    modem_cmd_result_t r = { .status = CMD_ERROR, .has_call_result = false };
    call_model_txn_command_result(&m, tok, r);
    call_txn_t *tx = find_txn_by_token(&m, tok);
    assert_true(tx && tx->state == TXN_FAILED, "the rejected HOLD terminates FAILED");
    assert_eq_u((unsigned)m.published.last_call_result, (unsigned)MODEM_CALL_RESULT_CONNECTED,
                "§16.5: a rejected HOLD (OUTPUT_FREE sink) does NOT write last_call_result");
    call_leg_t *L = find_leg(&m, 1u);
    assert_true(L != NULL && L->state == CALL_LEG_ACTIVE,
                "a rejected HOLD leaves the active call intact");
}

int main(void) {
    test_on_event_add_update();
    test_on_event_unknown_noop();
    test_on_event_active_latches_connected();
    test_on_event_released_removes_leg();
    test_targeted_active_release_re_presents_waiting();
    test_targeted_active_release_directly_promotes_waiting();
    test_sole_connected_release_projects_no_carrier();
    test_sole_unanswered_local_release_projects_no_carrier();
    test_answered_mt_metadata_survives_preprojection_bind();
    test_withheld_cli_retained_after_id_scoped_release();
    test_waiting_cli_owner_release_does_not_restore_older_number();
    test_failed_teardown_does_not_mask_remote_drop();
    test_on_event_released_promotes_held();
    test_2ndmo_busy_to_second();
    test_2ndmo_clcc_lost_to_second();
    test_held_departure_to_last();
    test_id_valid_coarse_observation();
    test_on_event_released_latches_terminal();
    test_on_event_pending_mt_bind_clear();
    test_on_event_generation_reuse();
    test_gen_next_survives_slot_reuse();
    test_generation_wrap_skips_zero();
    test_on_event_clears_pending_removal_not_clock();
    test_on_event_overflow();
    test_on_ring_fresh_episode();
    test_on_ring_repeat_keeps_number();
    test_on_ring_resets_latch_only();
    test_on_clip_refresh();
    test_incoming_diverted_before_ring_binds_to_leg();
    test_incoming_diverted_after_waiting_binds_and_retires();
    test_incoming_diverted_does_not_clobber_leg_cli();
    test_orphan_incoming_diverted_expires();
    test_ccwa_after_waiting_event_binds_existing_leg();
    test_on_bare_final_connect();
    test_on_bare_final_records_txn();
    test_on_bare_final_chld_window_excluded();
    test_on_bare_final_never_removes_leg();
    test_latch_independence();
    test_on_event_id_out_of_range_noop();
    test_pending_terminal_cleared_after_eviction();
    test_bare_connect_clears_pending_terminal();
    test_released_2ndmo_gen_qualified();
    test_pending_terminal_owner_protects_against_unrelated_active();
    test_pending_terminal_not_stolen_by_unrelated_eviction();
    test_minor_2ndmo_connected_then_departed_not_latched_connected();
    test_id_scoped_released_reconciles_newcall_txn();
    test_no_leg_busy_latches_via_clcc();
    test_bare_final_two_owners_unattributed();
    test_cmd_error_terminates_with_cause();
    test_cmd_timeout_uncertain_immediate_clcc();
    test_rejected_hold_no_last_result();
    if (s_failures != 0) {
        fprintf(stderr, "%d failure(s)\n", s_failures);
        return 1;
    }
    printf("test_call_model_events: all assertions passed\n");
    return 0;
}

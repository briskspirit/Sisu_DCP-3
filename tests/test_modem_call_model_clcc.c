/* Host unit tests for the AT+CLCC shadow-commit reconcile in
 * src/services/modem_call_model.c. Pure module: the real .c is LINKED (not
 * #included) -- see tests/run_tests.sh. ASan/UBSan catch any OOB/overflow.
 *
 * ORACLE: docs/call_model.md sections 5.1-5.4. Expected values come from the
 * call-table authority and reconciliation invariants, not observed output:
 *   - a dropped/truncated CLCC row moves the transport counter -> the whole
 *     shadow is rejected, the live table is untouched (section 5.3 step 1);
 *   - the FIRST clean snapshot that omits a live leg only ARMS a removal
 *     (marks it pending_removal, retains its role) -- it never removes
 *     (section 5.3 steps 2-3);
 *   - a SECOND clean snapshot reproducing the same absent set commits the
 *     whole shadow atomically: adds + confirmed removals together, so a leg
 *     revealed in the confirmation is present in the SAME publish that removes
 *     the twice-absent leg -> call_state never transits IDLE (section 5.3
 *     step 4);
 *   - an ID-scoped RELEASED event (on_event RELEASING) removes
 *     that leg ALONE and does not disturb an independent removal arm; a
 *     pending_removal leg keeps its published role (section 5.1 / 5.3);
 *   - the CLCC-absence clock (first_removal_ms) is stamped ONCE and is not
 *     reset by an intervening URC, so a leg still CLCC-absent at
 *     first_removal_ms + m.timing.leg_limbo_ms is force-evicted even when the
 *     two-snapshot confirmation is blocked by a changing absent set
 *     (section 5.3 bounded escape). */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "services/modem_call_model.h"
#include "call_timing_fixture.h"

/* §16.12: init the model with the stable host timing fixture. */
static void tinit(call_model_t *m) { call_timing_t _t = call_timing_fixture(); call_model_init(m, &_t); }

static int s_failures;

static void assert_true(bool cond, const char *msg) {
    if (!cond) { fprintf(stderr, "FAIL: %s\n", msg); s_failures++; }
}
static void assert_eq_u(unsigned got, unsigned want, const char *msg) {
    if (got != want) { fprintf(stderr, "FAIL: %s (got %u want %u)\n", msg, got, want); s_failures++; }
}

/* Live-table probe: the returned leg is in_use with this id, else NULL. */
static call_leg_t *find_leg(call_model_t *m, uint8_t id) {
    for (unsigned i = 0; i < MODEM_MAX_CALL_LEGS; i++) {
        if (m->legs[i].in_use && m->legs[i].id == id) return &m->legs[i];
    }
    return NULL;
}

/* A truncated/dropped CLCC row bumps the transport counter between begin and
 * OK -> the whole shadow is rejected; the live leg is untouched (not removed,
 * not marked pending_removal); backoff advances. (Spec 5.3 step 1.) */
static void test_truncated_row_rejected(void) {
    call_model_t m;
    tinit(&m);
    call_model_set_now(&m, 1000u);
    call_model_on_event(&m, 1u, true, CALL_LEG_ACTIVE, CALL_DIR_MT);
    call_projection_t p;
    call_model_project(&m, &p);

    call_model_clcc_begin(&m, 5u);                 /* transport = 5 at issue */
    call_model_clcc_row(&m, 1u, CALL_DIR_MT, CALL_MODE_VOICE, 0u, CALL_LEG_ACTIVE, "0700900000");
    call_model_clcc_ok(&m, 6u);                    /* a row was dropped: 5 -> 6 */

    call_leg_t *a = find_leg(&m, 1u);
    assert_true(a != NULL, "trunc: A survives a rejected shadow");
    if (a != NULL) {
        assert_true(a->state == CALL_LEG_ACTIVE, "trunc: A still ACTIVE");
        assert_true(!a->pending_removal, "trunc: A not marked pending_removal");
    }
    assert_true(m.clcc_backoff_shift > 0u, "trunc: transport reject backs off");
    call_model_project(&m, &p);
    assert_true(p.call_state != MODEM_CALL_IDLE, "trunc: never false-finalizes");
}

/* Two clean snapshots omitting a sole live leg: the first ARMS (retain role),
 * the second COMMITS the removal. (Spec 5.3 steps 2-4.) */
static void test_two_snapshot_removal(void) {
    call_model_t m;
    tinit(&m);
    call_model_set_now(&m, 1000u);
    call_model_on_event(&m, 1u, true, CALL_LEG_ACTIVE, CALL_DIR_MT);
    call_projection_t p;
    call_model_project(&m, &p);                    /* publish A=ACTIVE */
    assert_true(p.call_state == MODEM_CALL_ACTIVE, "2snap: A published ACTIVE");

    /* Snapshot #1: clean, omits A -> ARM only (no removal, role retained). */
    call_model_clcc_begin(&m, 0u);
    call_model_clcc_ok(&m, 0u);
    call_leg_t *a = find_leg(&m, 1u);
    assert_true(a != NULL && a->pending_removal, "2snap: #1 arms pending_removal");
    call_model_project(&m, &p);
    assert_true(p.call_state != MODEM_CALL_IDLE, "2snap: #1 never finalizes a live call");
    assert_eq_u(p.active_call_id, 1u, "2snap: #1 retains active_call_id=A");

    /* Snapshot #2: clean, omits A again -> COMMIT the removal. */
    call_model_clcc_begin(&m, 0u);
    call_model_clcc_ok(&m, 0u);
    assert_true(find_leg(&m, 1u) == NULL, "2snap: #2 commits A removal");
    call_model_project(&m, &p);
    assert_eq_u(p.call_state, MODEM_CALL_IDLE, "2snap: empty table -> IDLE");
    assert_eq_u(p.active_call_id, 0u, "2snap: IDLE clears active_call_id");
}

/* Golden trace: live=A; CLCC #1 omits A (arm); CLCC #2 omits A but reveals
 * a new leg B -> A removed AND B present in the SAME publish, call_state never
 * IDLE (no false finalize). (Spec 5.3 step 4.) */
static void test_confirmation_reveals_b(void) {
    call_model_t m;
    tinit(&m);
    call_model_set_now(&m, 1000u);
    call_model_on_event(&m, 1u, true, CALL_LEG_ACTIVE, CALL_DIR_MT);
    call_projection_t p;
    call_model_project(&m, &p);                    /* publish A=ACTIVE */

    call_model_clcc_begin(&m, 0u);                 /* #1 omits A -> arm */
    call_model_clcc_ok(&m, 0u);
    call_model_project(&m, &p);
    assert_true(p.call_state != MODEM_CALL_IDLE, "revealB: arm never finalizes");

    call_model_clcc_begin(&m, 0u);                 /* #2 omits A, reveals B */
    call_model_clcc_row(&m, 2u, CALL_DIR_MT, CALL_MODE_VOICE, 0u, CALL_LEG_ACTIVE, "0700111222");
    call_model_clcc_ok(&m, 0u);

    assert_true(find_leg(&m, 1u) == NULL, "revealB: A removed on confirmation");
    call_leg_t *b = find_leg(&m, 2u);
    assert_true(b != NULL && b->state == CALL_LEG_ACTIVE, "revealB: B present in same commit");
    call_model_project(&m, &p);
    assert_true(p.call_state != MODEM_CALL_IDLE, "revealB: call_state never IDLE (no finalize)");
    assert_eq_u(p.active_call_id, 2u, "revealB: active role handed to B");
}

/* An id-scoped release removes that leg ALONE and leaves an independent
 * removal arm untouched; the pending_removal leg keeps its published role.
 * (Spec 5.1 id-bearing update / 5.3 retain.) */
static void test_id_scoped_release_alone(void) {
    call_model_t m;
    tinit(&m);
    call_model_set_now(&m, 1000u);
    call_model_on_event(&m, 1u, true, CALL_LEG_ACTIVE, CALL_DIR_MT);
    call_model_on_event(&m, 2u, true, CALL_LEG_HELD, CALL_DIR_MO);
    call_projection_t p;
    call_model_project(&m, &p);                    /* publish A=ACTIVE + B=HELD */

    /* Clean CLCC that lists B but omits A -> arm A pending_removal (phantom
     * absence; A is genuinely live so it must be retained, not removed). */
    call_model_clcc_begin(&m, 0u);
    call_model_clcc_row(&m, 2u, CALL_DIR_MO, CALL_MODE_VOICE, 0u, CALL_LEG_HELD, "0700222333");
    call_model_clcc_ok(&m, 0u);
    call_leg_t *a = find_leg(&m, 1u);
    assert_true(a != NULL && a->pending_removal, "idrel: A armed pending_removal");

    /* ID-scoped RELEASED event for leg 2 -> B released ALONE. */
    call_model_on_event(&m, 2u, true, CALL_LEG_RELEASING, CALL_DIR_MO);
    assert_true(find_leg(&m, 2u) == NULL, "idrel: B removed alone");
    a = find_leg(&m, 1u);
    assert_true(a != NULL && a->pending_removal, "idrel: A's arm untouched by B release");

    call_model_project(&m, &p);
    assert_true(p.call_state == MODEM_CALL_ACTIVE, "idrel: A retains published ACTIVE role");
    assert_eq_u(p.active_call_id, 1u, "idrel: active_call_id stays A");
}

/* The CLCC-absence clock is stamped ONCE and survives an intervening URC; a
 * leg still CLCC-absent at first_removal_ms + m.timing.leg_limbo_ms is
 * force-evicted even when the two-snapshot confirmation is blocked by a
 * CHANGED absent set. A freshly-armed leg (clock not yet expired) is NOT
 * evicted. (Spec 5.3 bounded escape; oracle horizon = 8000 ms.) */
static void test_limbo_force_evict(void) {
    call_model_t m;
    tinit(&m);
    call_model_set_now(&m, 1000u);                /* nonzero base: first_removal_ms==0 is the "not-absent" sentinel */
    call_model_on_event(&m, 1u, true, CALL_LEG_ACTIVE, CALL_DIR_MT);
    call_projection_t p;
    call_model_project(&m, &p);

    /* Snapshot #1 at t=1000: omits A -> arm A, first_removal_ms stamped once = 1000. */
    call_model_clcc_begin(&m, 0u);
    call_model_clcc_ok(&m, 0u);
    call_leg_t *a = find_leg(&m, 1u);
    assert_true(a != NULL && a->pending_removal, "limbo: A armed at t=1000");
    assert_eq_u(a->first_removal_ms, 1000u, "limbo: clock stamped once = 1000");

    /* A new live leg B appears; time advances to exactly A's limbo horizon. */
    call_model_set_now(&m, 1000u + m.timing.leg_limbo_ms);   /* 9000 */
    call_model_on_event(&m, 2u, true, CALL_LEG_ACTIVE, CALL_DIR_MT);

    /* Snapshot #2 omits BOTH A and B -> absent set {A,B} != armed {A}, so the
     * two-snapshot confirmation is BLOCKED (re-arm, not remove). But A's clock
     * is expired -> A is force-evicted; B is freshly armed, not evicted. */
    call_model_clcc_begin(&m, 0u);
    call_model_clcc_ok(&m, 0u);

    assert_true(find_leg(&m, 1u) == NULL, "limbo: A force-evicted at horizon");
    call_leg_t *b = find_leg(&m, 2u);
    assert_true(b != NULL, "limbo: B not evicted (clock not expired)");
    if (b != NULL) {
        assert_true(b->pending_removal, "limbo: B freshly armed");
        assert_eq_u(b->first_removal_ms, 9000u, "limbo: B clock stamped now, once");
    }
}

/* The §5.3 limbo clock is AUTHORITATIVE and independent of the pending_removal
 * role-suppress mask. A lying modem that re-emits a stale non-release event for a
 * dead/lost id clears pending_removal (role un-mask) but must NOT reset the CLCC-absence
 * clock -- else the phantom is immortal (keepalive never stops). After the horizon the
 * still-CLCC-absent leg IS force-evicted despite pending_removal==false. */
static void test_limbo_survives_naming_urc(void) {
    call_model_t m;
    tinit(&m);
    call_model_set_now(&m, 1000u);
    call_model_on_event(&m, 1u, true, CALL_LEG_ACTIVE, CALL_DIR_MT);
    call_projection_t p;
    call_model_project(&m, &p);

    /* Snapshot #1 omits A -> arm, clock stamped once = 1000, pending_removal set. */
    call_model_clcc_begin(&m, 0u);
    call_model_clcc_ok(&m, 0u);
    call_leg_t *a = find_leg(&m, 1u);
    assert_true(a != NULL && a->pending_removal, "naming: A armed");
    assert_eq_u(a->first_removal_ms, 1000u, "naming: clock stamped once = 1000");

    /* A stale ID-scoped non-release event lands (the exact §5.3 adversary): it unmasks
     * the role (clears pending_removal) but must keep the CLCC-absence clock. */
    call_model_set_now(&m, 4000u);
    call_model_on_event(&m, 1u, true, CALL_LEG_HELD, CALL_DIR_UNKNOWN);
    a = find_leg(&m, 1u);
    assert_true(a != NULL && !a->pending_removal, "naming: the URC un-masks (clears pending_removal)");
    assert_eq_u(a->first_removal_ms, 1000u, "naming: the URC does NOT reset the absence clock");

    /* At the horizon, a still-CLCC-absent leg is force-evicted DESPITE pending_removal==false. */
    call_model_set_now(&m, 1000u + m.timing.leg_limbo_ms);   /* 9000 */
    call_model_clcc_begin(&m, 0u);
    call_model_clcc_ok(&m, 0u);
    assert_true(find_leg(&m, 1u) == NULL,
                "naming: phantom is MORTAL -- force-evicted at the horizon (clock, not mask)");
}

/* The other side of the same invariant: a GENUINE clean-CLCC reappearance resets
 * the absence clock, so a leg the modem keeps listing is never limbo-evicted no
 * matter how long it lives. */
static void test_limbo_clock_cleared_on_clean_reappearance(void) {
    call_model_t m;
    tinit(&m);
    call_model_set_now(&m, 1000u);
    call_model_on_event(&m, 1u, true, CALL_LEG_ACTIVE, CALL_DIR_MT);
    call_projection_t p;
    call_model_project(&m, &p);

    call_model_clcc_begin(&m, 0u);                 /* #1 omits A -> arm, clock = 1000 */
    call_model_clcc_ok(&m, 0u);
    call_leg_t *a = find_leg(&m, 1u);
    assert_true(a != NULL && a->first_removal_ms == 1000u, "reappear: armed, clock=1000");

    /* A clean CLCC that LISTS A -> genuine presence clears BOTH the mask and the clock. */
    call_model_set_now(&m, 2000u);
    call_model_clcc_begin(&m, 0u);
    call_model_clcc_row(&m, 1u, CALL_DIR_MT, CALL_MODE_VOICE, 0u, CALL_LEG_ACTIVE, "0700900000");
    call_model_clcc_ok(&m, 0u);
    a = find_leg(&m, 1u);
    assert_true(a != NULL && a->first_removal_ms == 0u, "reappear: clean presence resets the clock");
    assert_true(a != NULL && !a->pending_removal, "reappear: clean presence clears the mask");

    /* Long past the horizon, A still present -> never limbo-evicted. */
    call_model_set_now(&m, 2000u + 5u * m.timing.leg_limbo_ms);
    call_model_clcc_begin(&m, 0u);
    call_model_clcc_row(&m, 1u, CALL_DIR_MT, CALL_MODE_VOICE, 0u, CALL_LEG_ACTIVE, "0700900000");
    call_model_clcc_ok(&m, 0u);
    assert_true(find_leg(&m, 1u) != NULL, "reappear: a genuinely-present leg is never limbo-evicted");
}

/* A pending-MT-bound INCOMING/WAITING leg removed via the CLCC path must clear the
 * pending-MT episode (as cm_remove_leg does) so no phantom RINGING/waiting with a stale
 * incoming_number survives the eviction. */
static void test_clcc_evict_clears_pending_mt(void) {
    call_model_t m;
    tinit(&m);
    call_model_set_now(&m, 1000u);
    call_model_on_ring(&m);                         /* incoming episode */
    call_model_on_clip(&m, "0700555666", 0u);
    call_model_on_event(&m, 3u, true, CALL_LEG_INCOMING, CALL_DIR_MT);   /* leg appears -> binds pending-MT */
    assert_true(m.pending_mt.active && m.pending_mt.bound_valid && m.pending_mt.bound_id == 3u,
                "mt-evict: pending-MT bound to the incoming leg");

    /* The incoming leg vanishes from CLCC across two clean snapshots -> CLCC two-snapshot evict. */
    call_model_clcc_begin(&m, 0u); call_model_clcc_ok(&m, 0u);    /* #1 arm */
    call_model_clcc_begin(&m, 0u); call_model_clcc_ok(&m, 0u);    /* #2 commit -> clcc_evict_leg */
    assert_true(find_leg(&m, 3u) == NULL, "mt-evict: incoming leg removed via CLCC");
    assert_true(!m.pending_mt.active && !m.pending_mt.bound_valid,
                "mt-evict: CLCC evict clears the pending-MT binding (no phantom waiting)");

    call_projection_t p;
    call_model_project(&m, &p);
    assert_true(p.call_state != MODEM_CALL_RINGING, "mt-evict: no phantom RINGING after the CLCC evict");
    assert_true(p.incoming_number[0] == '\0', "mt-evict: no stale incoming_number left behind");
}

/* Spec 5.4: an observation that INTERLEAVES an open CLCC invalidates the shadow — the
 * live table (already updated by the URC) wins, and the raced shadow is DISCARDED whole
 * (no merge/commit) with a fresh CLCC re-scheduled. The shadow says leg 1 is HELD, but a
 * ID-scoped ACTIVE event lands before the OK: leg 1 must end ACTIVE (the event), NOT HELD
 * (a committed shadow), and the model must still want a CLCC.
 * (call_model_wants_clcc is asserted against the final assembled TU.) */
static void test_interleaved_urc_invalidates_shadow(void) {
    call_model_t m;
    tinit(&m);
    call_model_set_now(&m, 1000u);

    call_model_clcc_begin(&m, 0u);                                         /* open a shadow */
    call_model_clcc_row(&m, 1u, CALL_DIR_MT, CALL_MODE_VOICE, 0u, CALL_LEG_HELD, "0700111222"); /* shadow: leg 1 HELD */
    call_model_on_event(&m, 1u, true, CALL_LEG_ACTIVE, CALL_DIR_MT);             /* URC interleaves -> ACTIVE + invalidate */
    call_model_clcc_ok(&m, 0u);                                           /* discard the raced shadow */

    call_leg_t *l = find_leg(&m, 1u);
    assert_true(l != NULL && l->state == CALL_LEG_ACTIVE,
                "5.4: the interleaving ACTIVE URC wins; the HELD shadow is discarded, not committed");
    assert_true(call_model_wants_clcc(&m),
                "5.4: a discarded shadow re-schedules a CLCC (wants_clcc stays true)");
}

/* §5.4: a txn abandon deadline mutating a leg to RELEASING WHILE a CLCC shadow is
 * open invalidates that shadow, so clcc_ok DISCARDS it -- it must not restore the stale
 * DIALING row or clear the leg's limbo clock (which would defeat bounded drain). */
static void test_txn_timeout_invalidates_open_shadow(void) {
    /* Drive the DIAL through the public request/dispatch/on_event path (no
     * hand-poked m.txns[i].token); a short txn_abandon_ms makes the abandon
     * fire at now=2000. */
    call_timing_t t = call_timing_fixture(); t.txn_abandon_ms = 1000u;
    call_model_t m; call_model_init(&m, &t); call_model_set_now(&m, 1000u);
    uint32_t tok = call_model_request(&m, CALL_TXN_DIAL, 0u, false);
    call_model_txn_dispatched(&m, tok);                                 /* abandon = 1000 + 1000 = 2000 */
    call_model_on_event(&m, 5u, true, CALL_LEG_DIALING, CALL_DIR_MO);   /* binds the DIAL to leg 5 */
    call_leg_t *l = find_leg(&m, 5u);
    assert_true(l != NULL && l->state == CALL_LEG_DIALING, "precondition: leg 5 DIALING, DIAL bound");

    /* an open CLCC whose (soon-stale) row still shows leg 5 DIALING. */
    call_model_clcc_begin(&m, 0u);
    call_model_clcc_row(&m, 5u, CALL_DIR_MO, CALL_MODE_VOICE, 0u, CALL_LEG_DIALING, "0700900000");

    /* the abandon deadline matures WHILE the shadow is open. */
    call_model_set_now(&m, 2000u);
    call_model_tick(&m);
    l = find_leg(&m, 5u);
    assert_true(l != NULL && l->state == CALL_LEG_RELEASING,
                "§5.4: abandon deadline -> leg RELEASING while a shadow is open");
    assert_true(m.shadow_invalidated, "§5.4: the leg mutation invalidated the open shadow");
    uint32_t clock = (l != NULL) ? l->first_removal_ms : 0u;
    assert_true(clock != 0u, "§5.4: the limbo clock is armed");

    /* clcc_ok DISCARDS the stale shadow -- leg stays RELEASING, clock preserved. */
    call_model_clcc_ok(&m, 0u);
    l = find_leg(&m, 5u);
    assert_true(l != NULL && l->state == CALL_LEG_RELEASING,
                "§5.4: clcc_ok discards the stale shadow (leg NOT restored to DIALING)");
    assert_true(l != NULL && l->pending_removal && l->first_removal_ms == clock,
                "§5.4: the discarded shadow does NOT clear the limbo clock");
}

/* §16.8: the centralized mutation primitive means a POST-DISPATCH CANCEL that mutates a
 * leg (marks it RELEASING) WHILE a CLCC shadow is open invalidates that shadow — clcc_ok
 * must DISCARD it, not commit a stale DIALING row over the just-cancelled leg
 * ("post-dispatch cancel during an open CLCC commits a stale orphan"). */
static void test_cancel_during_open_shadow_discards(void) {
    call_model_t m; tinit(&m); call_model_set_now(&m, 1000u);
    uint32_t tok = call_model_request(&m, CALL_TXN_DIAL, 0u, false);
    call_model_txn_dispatched(&m, tok);
    call_model_on_event(&m, 6u, true, CALL_LEG_DIALING, CALL_DIR_MO);   /* binds the DIAL to leg 6 */

    /* an open CLCC still showing leg 6 DIALING. */
    call_model_clcc_begin(&m, 0u);
    call_model_clcc_row(&m, 6u, CALL_DIR_MO, CALL_MODE_VOICE, 0u, CALL_LEG_DIALING, "0700900001");

    /* the queue cancels the dispatched op WHILE the shadow is open (post-dispatch tombstone
     * -> leg 6 RELEASING: a call-affecting mutation through the §16.8 primitive). */
    call_model_txn_cancel(&m, tok);
    assert_true(find_leg(&m, 6u) && find_leg(&m, 6u)->state == CALL_LEG_RELEASING,
                "cancel marks the bound leg RELEASING");
    assert_true(m.shadow_invalidated, "§16.8: the cancel mutation invalidated the open shadow");

    /* clcc_ok DISCARDS the stale shadow: leg 6 stays RELEASING (NOT restored to DIALING). */
    call_model_clcc_ok(&m, 0u);
    assert_true(find_leg(&m, 6u) && find_leg(&m, 6u)->state == CALL_LEG_RELEASING,
                "§16.8: clcc_ok discards the stale shadow (leg not un-released)");
}

/* The CLCC merge drives pending-MT the same way a URC does. A RING then a
 * CLCC INCOMING row BINDS the episode; the next CLCC showing that leg ACTIVE CLEARS it
 * (no phantom waiting/ringing). */
static void test_clcc_binds_and_clears_pending_mt(void) {
    call_model_t m;
    tinit(&m);
    call_model_set_now(&m, 1000u);
    call_model_on_ring(&m);
    assert_true(m.pending_mt.active && !m.pending_mt.bound_valid,
                "clcc-mt: RING starts an unbound pending-MT episode");

    call_model_clcc_begin(&m, 0u);
    call_model_clcc_row(&m, 3u, CALL_DIR_MT, CALL_MODE_VOICE, 0u, CALL_LEG_INCOMING, "0700555");
    call_model_clcc_ok(&m, 0u);
    assert_true(m.pending_mt.active && m.pending_mt.bound_valid && m.pending_mt.bound_id == 3u,
                "clcc-mt: a CLCC INCOMING row binds pending-MT to the leg");

    call_model_clcc_begin(&m, 0u);
    call_model_clcc_row(&m, 3u, CALL_DIR_MT, CALL_MODE_VOICE, 0u, CALL_LEG_ACTIVE, "0700555");
    call_model_clcc_ok(&m, 0u);
    assert_true(!m.pending_mt.active && !m.pending_mt.bound_valid,
                "clcc-mt: the bound leg reaching ACTIVE via CLCC clears pending-MT (no phantom waiting)");
    call_projection_t p;
    call_model_project(&m, &p);
    assert_true(p.call_state != MODEM_CALL_RINGING, "clcc-mt: no phantom RINGING after the CLCC answer");
}

/* §5.6: a RING whose call never materializes is cleared by TWO clean CLCC snapshots
 * showing no compatible MT leg -- before the 120 s ring fallback. */
static void test_two_clean_clcc_clears_stale_pending_mt(void) {
    call_model_t m;
    tinit(&m);
    call_model_set_now(&m, 1000u);
    call_model_on_ring(&m);
    assert_true(m.pending_mt.active, "§5.6: episode active after RING");

    call_model_clcc_begin(&m, 0u);         /* clean, no compatible MT leg -> count #1 */
    call_model_clcc_ok(&m, 0u);
    assert_true(m.pending_mt.active,
                "§5.6: ONE clean absent snapshot does not clear (armed only)");

    call_model_clcc_begin(&m, 0u);         /* clean again -> count #2 -> clear */
    call_model_clcc_ok(&m, 0u);
    assert_true(!m.pending_mt.active,
                "§5.6: TWO clean absent snapshots clear the stale episode");
}

/* A leg seen ACTIVE ONLY via CLCC (the ID-scoped ACTIVE event was lost) latches
 * last_call_result=CONNECTED and clears a stale pending_terminal, like the URC path.
 * The pending_terminal cause is built here via the PUBLIC API as
 * an ATTRIBUTED cause (owner_id == this leg), not a raw field write -- an
 * unattributed (owner_id==0) register clears unconditionally regardless of which
 * {id,gen} the merge site passes, so an unattributed setup could not
 * catch a regression that dropped the owner-scoped {leg->id, leg->generation}
 * arguments at this specific CLCC-merge call site (cm_clear_pending_terminal). */
static void test_clcc_active_latches_connected(void) {
    call_model_t m;
    tinit(&m);
    call_model_set_now(&m, 1000u);

    /* leg 1: a DIALING observation binds the open DIAL so on_bare_final's
     * SOLE-eligible-open-txn match (§16.2) attributes the cause to leg 1
     * specifically (owner_id == 1), not left unattributed. */
    uint32_t tok = call_model_request(&m, CALL_TXN_DIAL, 0u, false);
    call_model_txn_dispatched(&m, tok);
    call_model_on_event(&m, 1u, true, CALL_LEG_DIALING, CALL_DIR_MO);
    call_model_set_now(&m, 1500u);
    call_model_on_bare_final(&m, MODEM_CALL_RESULT_NO_CARRIER, false);
    assert_true(m.pending_terminal == MODEM_CALL_RESULT_NO_CARRIER,
                "precondition: an ATTRIBUTED bare-final cause is recorded");
    assert_true(m.pending_terminal_owner_id == 1u,
                "precondition: the cause is owned by leg 1 (not unattributed)");

    /* The same leg then ENTERS ACTIVE only via CLCC (the ID-scoped ACTIVE event
     * was lost), so this authoritative transition must latch CONNECTED and clear
     * its own cause. */
    call_model_set_now(&m, 2000u);
    call_model_clcc_begin(&m, 0u);
    call_model_clcc_row(&m, 1u, CALL_DIR_MO, CALL_MODE_VOICE, 0u, CALL_LEG_ACTIVE, "111");
    call_model_clcc_ok(&m, 0u);
    assert_true(m.published.last_call_result == MODEM_CALL_RESULT_CONNECTED,
                "a CLCC-only ACTIVE latches last_call_result=CONNECTED");
    assert_true(m.pending_terminal == MODEM_CALL_RESULT_NONE,
                "a CLCC-only ACTIVE clears its OWN attributed pending_terminal");
}

/* Recorded shadow trace: call-waiting RING/+CCWA resets the flat
 * last_call_result to NONE. A periodic CLCC row that merely confirms the
 * already-ACTIVE foreground leg must not re-latch CONNECTED; only an actual
 * transition into ACTIVE has that authority. */
static void test_repeat_clcc_active_does_not_reconnect_latch(void) {
    call_model_t m;
    tinit(&m);
    call_model_set_now(&m, 1000u);
    call_model_on_event(&m, 1u, true, CALL_LEG_ACTIVE, CALL_DIR_MO);
    call_model_on_ring(&m);
    call_model_on_clip(&m, "5552000", 0u);
    assert_true(m.published.last_call_result == MODEM_CALL_RESULT_NONE,
                "repeat-ACTIVE precondition: call-waiting indication resets the latch");

    call_model_clcc_begin(&m, 0u);
    call_model_clcc_row(&m, 1u, CALL_DIR_MO, CALL_MODE_VOICE, 0u,
                        CALL_LEG_ACTIVE, "111");
    call_model_clcc_row(&m, 2u, CALL_DIR_MT, CALL_MODE_VOICE, 0u,
                        CALL_LEG_WAITING, "5552000");
    call_model_clcc_ok(&m, 0u);

    assert_true(m.published.last_call_result == MODEM_CALL_RESULT_NONE,
                "repeat CLCC ACTIVE preserves the call-waiting NONE latch");
}

/* CLCC eviction consumes and clears the one-shot pending_terminal cause. */
static void test_clcc_evict_clears_pending_terminal(void) {
    call_model_t m;
    tinit(&m);
    call_model_set_now(&m, 1000u);
    call_model_on_event(&m, 1u, true, CALL_LEG_ACTIVE, CALL_DIR_MT);
    m.pending_terminal = MODEM_CALL_RESULT_NO_CARRIER;   /* a recorded bare-final cause */
    call_model_clcc_begin(&m, 0u); call_model_clcc_ok(&m, 0u);   /* #1 arm leg 1 */
    call_model_clcc_begin(&m, 0u); call_model_clcc_ok(&m, 0u);   /* #2 evict leg 1 */
    assert_true(find_leg(&m, 1u) == NULL, "evict-cause: leg evicted via CLCC");
    assert_true(m.pending_terminal == MODEM_CALL_RESULT_NONE,
                "evict-cause: CLCC eviction clears the consumed pending_terminal");
}

/* CLCC <mode>/<mpty> enter the model via clcc_row and land on the merged leg. */
static void test_clcc_mode_mpty(void) {
    call_model_t m;
    tinit(&m);
    call_model_set_now(&m, 1000u);
    call_model_clcc_begin(&m, 0u);
    call_model_clcc_row(&m, 1u, CALL_DIR_MT, CALL_MODE_DATA, 1u, CALL_LEG_ACTIVE, "111");
    call_model_clcc_ok(&m, 0u);
    call_leg_t *l = find_leg(&m, 1u);
    assert_true(l != NULL && l->mode == CALL_MODE_DATA, "CLCC <mode> sets leg->mode");
    assert_true(l != NULL && l->mpty, "CLCC <mpty> sets leg->mpty");

    call_model_clcc_begin(&m, 0u);
    call_model_clcc_row(&m, 1u, CALL_DIR_MT, CALL_MODE_VOICE, 0u, CALL_LEG_ACTIVE, "111");
    call_model_clcc_ok(&m, 0u);
    l = find_leg(&m, 1u);
    assert_true(l != NULL && l->mode == CALL_MODE_VOICE && !l->mpty,
                "a later CLCC row updates mode/mpty");
}

/* A leg already RELEASING (cm_abandon_leg's hard app/deadline
 * teardown marker) must NOT be un-released by a stale CLCC row that hasn't
 * caught up with the release yet -- the row must not flip the leg's state back
 * (e.g. to a stale DIALING) nor clear its §5.3 limbo clock. RELEASING is
 * retired ONLY by genuine absence (the two-snapshot path) or the limbo timeout. */
static void test_clcc_row_does_not_unrelease(void) {
    call_model_t m;
    tinit(&m);
    call_model_set_now(&m, 1000u);

    /* Reach RELEASING via the PUBLIC API: dispatch a 2nd-MO DIAL, let it bind
     * to a genuinely DIALING leg, then have the app abandon it -- this drives
     * the real cm_abandon_leg postconditions (RELEASING + queued release + the
     * §5.3 limbo clock armed) instead of hand-poking them onto the struct. */
    uint32_t tok = call_model_request(&m, CALL_TXN_DIAL, 0u, true);
    call_model_txn_dispatched(&m, tok);
    call_model_on_event(&m, 5u, true, CALL_LEG_DIALING, CALL_DIR_MO);
    call_model_new_call_abandoned(&m);

    call_leg_t *l = find_leg(&m, 5u);
    assert_true(l != NULL && l->state == CALL_LEG_RELEASING,
                "unrelease precondition: the abandon marks the leg RELEASING");
    assert_true(l != NULL && l->pending_removal,
                "unrelease precondition: the abandon arms the pending_removal mask");
    assert_true(l != NULL && l->first_removal_ms == 1000u,
                "unrelease precondition: the abandon stamps the limbo clock to now (1000)");

    /* a stale in-flight CLCC (issued before the abandon landed, or simply not
     * yet caught up) still reports the leg DIALING. */
    call_model_clcc_begin(&m, 0u);
    call_model_clcc_row(&m, 5u, CALL_DIR_MO, CALL_MODE_VOICE, 0u, CALL_LEG_DIALING, "0700900000");
    call_model_clcc_ok(&m, 0u);

    l = find_leg(&m, 5u);
    assert_true(l != NULL, "unrelease: the leg is still present (it WAS in the shadow, not absent)");
    if (l != NULL) {
        assert_true(l->state == CALL_LEG_RELEASING,
                    "a present CLCC row must NOT flip a RELEASING leg's state back");
        assert_true(l->pending_removal,
                    "a present CLCC row must NOT clear a RELEASING leg's pending_removal mask");
        assert_eq_u(l->first_removal_ms, 1000u,
                    "a present CLCC row must NOT clear a RELEASING leg's limbo clock");
    }
}

/* §5.6: a RING mid-episode is a live sign the call is still
 * ringing and must invalidate an already-armed "two clean absent CLCC
 * snapshots" streak -- else a still-ringing incoming that happens to RING
 * between the two clean snapshots is prematurely dismissed as stale. */
static void test_ring_resets_absent_streak(void) {
    call_model_t m;
    tinit(&m);
    call_model_set_now(&m, 1000u);
    call_model_on_ring(&m);
    assert_true(m.pending_mt.active, "ring-streak: episode active after RING");

    call_model_clcc_begin(&m, 0u);         /* clean, no compatible MT leg -> streak #1 */
    call_model_clcc_ok(&m, 0u);
    assert_eq_u(m.pending_mt.clcc_absent_snapshots, 1u,
                "ring-streak: one clean absent snapshot arms the streak to 1");

    call_model_set_now(&m, 1500u);
    call_model_on_ring(&m);                /* still ringing, no leg yet -- must reset the streak */
    assert_eq_u(m.pending_mt.clcc_absent_snapshots, 0u,
                "a RING mid-episode resets the absent-snapshot streak");

    call_model_clcc_begin(&m, 0u);         /* another clean absent snapshot -> streak #1 again, NOT #2 */
    call_model_clcc_ok(&m, 0u);
    assert_true(m.pending_mt.active,
                "the streak reset means one more clean snapshot does NOT clear pending-MT yet");
}

/* §5.6 CLIP sibling: on_clip resets the same absent-snapshot
 * streak as on_ring -- a still-ringing incoming that happens to refresh
 * via +CLIP (not +CRING) between the two clean snapshots must not be
 * prematurely dismissed as stale either. on_clip has the identical
 * `p->clcc_absent_snapshots = 0u;` reset line and needs its own coverage. */
static void test_clip_resets_absent_streak(void) {
    call_model_t m;
    tinit(&m);
    call_model_set_now(&m, 1000u);
    call_model_on_clip(&m, "0700123456", 0u);
    assert_true(m.pending_mt.active, "clip-streak: episode active after CLIP");

    call_model_clcc_begin(&m, 0u);         /* clean, no compatible MT leg -> streak #1 */
    call_model_clcc_ok(&m, 0u);
    assert_eq_u(m.pending_mt.clcc_absent_snapshots, 1u,
                "clip-streak: one clean absent snapshot arms the streak to 1");

    call_model_set_now(&m, 1500u);
    call_model_on_clip(&m, "0700123456", 0u);   /* still ringing, no leg yet -- must reset the streak */
    assert_eq_u(m.pending_mt.clcc_absent_snapshots, 0u,
                "a CLIP mid-episode resets the absent-snapshot streak");

    call_model_clcc_begin(&m, 0u);         /* another clean absent snapshot -> streak #1 again, NOT #2 */
    call_model_clcc_ok(&m, 0u);
    assert_true(m.pending_mt.active,
                "the CLIP streak reset means one more clean snapshot does NOT clear pending-MT yet");
}

/* The CLCC-confirmed eviction path must retire the same flat CLI owner
 * as an id-scoped RELEASED URC. Otherwise a lost release URC leaves an incoming
 * number latched after the two-snapshot authoritative removal. */
static void test_clcc_evict_retires_flat_cli_owner(void) {
    call_model_t m;
    tinit(&m);
    call_model_set_now(&m, 100u);
    call_model_on_ring(&m);
    call_model_on_clip(&m, "+15550000003", 0u);
    call_model_on_event(&m, 3u, true, CALL_LEG_INCOMING, CALL_DIR_MT);
    call_model_on_event(&m, 3u, true, CALL_LEG_ACTIVE, CALL_DIR_MT);

    call_projection_t p;
    call_model_project(&m, &p);
    assert_true(strcmp(p.incoming_number, "+15550000003") == 0,
                "CLI-owner CLCC precondition: answered number is published");

    call_model_clcc_begin(&m, 0u);
    call_model_clcc_ok(&m, 0u);
    call_model_project(&m, &p);
    assert_true(find_leg(&m, 3u) != NULL,
                "CLI-owner CLCC: first clean absence retains the leg");

    call_model_set_now(&m, 200u);
    call_model_clcc_begin(&m, 0u);
    call_model_clcc_ok(&m, 0u);
    call_model_project(&m, &p);
    assert_true(find_leg(&m, 3u) == NULL,
                "CLI-owner CLCC: second clean absence evicts the leg");
    assert_true(p.incoming_number[0] == '\0',
                "CLI-owner CLCC: authoritative eviction clears the flat number");
}

/* Lost-release fallback: if A's id-scoped RELEASED URC is lost after the network already
 * promoted B, the two-clean-snapshot eviction must perform the same flat-CLI
 * retirement as the direct release path while preserving B's per-leg metadata. */
static void test_clcc_evict_active_retires_promoted_cli(void) {
    call_model_t m;
    tinit(&m);
    call_model_set_now(&m, 100u);
    call_model_on_event(&m, 1u, true, CALL_LEG_ACTIVE, CALL_DIR_MO);
    call_model_on_ring(&m);
    call_model_on_clip(&m, "+15550000004", 0u);
    call_model_on_event(&m, 2u, true, CALL_LEG_WAITING, CALL_DIR_MT);
    call_projection_t p;
    call_model_project(&m, &p);

    call_model_on_event(&m, 2u, true, CALL_LEG_ACTIVE, CALL_DIR_MT);
    call_model_clcc_begin(&m, 0u);
    call_model_clcc_row(&m, 2u, CALL_DIR_MT, CALL_MODE_VOICE, 0u,
                        CALL_LEG_ACTIVE, "+15550000004");
    call_model_clcc_ok(&m, 0u);
    assert_true(find_leg(&m, 1u) != NULL,
                "CLCC fallback: first absence retains A");

    call_model_set_now(&m, 200u);
    call_model_clcc_begin(&m, 0u);
    call_model_clcc_row(&m, 2u, CALL_DIR_MT, CALL_MODE_VOICE, 0u,
                        CALL_LEG_ACTIVE, "+15550000004");
    call_model_clcc_ok(&m, 0u);
    call_model_project(&m, &p);

    assert_true(find_leg(&m, 1u) == NULL,
                "CLCC fallback: second absence evicts A");
    assert_eq_u((unsigned)p.call_state, (unsigned)MODEM_CALL_ACTIVE,
                "CLCC fallback leaves B ACTIVE");
    assert_eq_u(p.active_call_id, 2u,
                "CLCC fallback publishes B as foreground");
    assert_true(p.incoming_number[0] == '\0',
                "CLCC fallback retires the flat incoming-number slot");
    call_leg_t *b = find_leg(&m, 2u);
    assert_true(b != NULL && strcmp(b->number, "+15550000004") == 0,
                "CLCC fallback preserves B's per-leg number");
}

int main(void) {
    test_clcc_row_does_not_unrelease();
    test_ring_resets_absent_streak();
    test_clip_resets_absent_streak();
    test_truncated_row_rejected();
    test_txn_timeout_invalidates_open_shadow();
    test_cancel_during_open_shadow_discards();
    test_clcc_binds_and_clears_pending_mt();
    test_two_clean_clcc_clears_stale_pending_mt();
    test_clcc_active_latches_connected();
    test_repeat_clcc_active_does_not_reconnect_latch();
    test_clcc_evict_clears_pending_terminal();
    test_clcc_mode_mpty();
    test_two_snapshot_removal();
    test_confirmation_reveals_b();
    test_id_scoped_release_alone();
    test_limbo_force_evict();
    test_limbo_survives_naming_urc();
    test_limbo_clock_cleared_on_clean_reappearance();
    test_clcc_evict_clears_pending_mt();
    test_clcc_evict_retires_flat_cli_owner();
    test_clcc_evict_active_retires_promoted_cli();
    test_interleaved_urc_invalidates_shadow();

    if (s_failures != 0) {
        fprintf(stderr, "%d CLCC-reconcile assertion(s) failed\n", s_failures);
        return 1;
    }
    printf("CLCC-reconcile tests passed\n");
    return 0;
}

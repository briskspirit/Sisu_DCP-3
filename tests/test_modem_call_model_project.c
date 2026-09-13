/* §9 projector truth-table unit tests. Oracle = docs/call_model.md §9.2 R1-R10 and
 * the §12 corner traces; expected values are derived from the table,
 * NOT from observed output. The projector is a pure function of model state. */
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
static void assert_eq_s(const char *got, const char *want, const char *msg) {
    if (strcmp(got, want) != 0) { fprintf(stderr, "FAIL: %s (got \"%s\" want \"%s\")\n", msg, got, want); s_failures++; }
}

/* --- model builders --- */
static call_leg_t *add_leg(call_model_t *m, uint8_t id, call_leg_state_t st, call_direction_t dir) {
    for (unsigned i = 0; i < MODEM_MAX_CALL_LEGS; i++) {
        if (!m->legs[i].in_use) {
            call_leg_t *l = &m->legs[i];
            memset(l, 0, sizeof *l);
            l->in_use = true; l->id = id; l->generation = 1u;
            l->state = st; l->dir = dir;
            l->first_seen_ms = l->last_seen_ms = m->now_ms;
            return l;
        }
    }
    assert_true(false, "add_leg: no free slot");
    return &m->legs[0];
}
/* Obtain a REAL allocator token via the public call_model_request (no
 * hand-fabricated m.txns[i].token), then override the state to the synthetic value the
 * projector unit test needs. The projector is a pure function of {kind, state, second_mo,
 * bound_id, ...}; it does not key on the token, so a real token is a strict improvement. */
static call_txn_t *add_txn(call_model_t *m, call_txn_kind_t kind, call_txn_state_t st) {
    uint32_t tok = call_model_request(m, kind, 0u, false);
    assert_true(tok != 0u, "add_txn: call_model_request allocated a txn");
    for (unsigned i = 0; i < MODEM_MAX_CALL_TRANSACTIONS; i++) {
        if (m->txns[i].in_use && m->txns[i].token == tok) {
            m->txns[i].state = st;
            return &m->txns[i];
        }
    }
    assert_true(false, "add_txn: allocated txn not found");
    return &m->txns[0];
}
static void set_pending_mt(call_model_t *m, const char *number, uint8_t cli_validity) {
    m->pending_mt.active = true;
    m->pending_mt.alert_observed = true;
    m->pending_mt.cli_validity = cli_validity;
    size_t n = 0; for (; n < MODEM_PHONE_MAX && number[n]; n++) m->pending_mt.number[n] = number[n];
    m->pending_mt.number[n] = '\0';
}

/* ---------------- stable rows R1..R10 ---------------- */

static void test_R1_idle(void) {
    /* R1: empty model, no txn, no pending-MT -> IDLE, id 0, all flags false. */
    call_model_t m; tinit(&m);
    call_projection_t o; call_model_project(&m, &o);
    assert_eq_u(o.call_state, MODEM_CALL_IDLE, "R1 call_state IDLE");
    assert_eq_u(o.active_call_id, 0, "R1 active_call_id 0");
    assert_true(!o.call_on_hold && !o.second_call_held, "R1 hold flags false");
    assert_true(!o.waiting_call && !o.ring_active, "R1 ring/waiting false");
    assert_true(!o.incoming_diverted, "R1 incoming_diverted false");
    assert_eq_s(o.incoming_number, "", "R1 incoming_number empty");
}

static void test_R2_ringing(void) {
    /* R2: pending-MT, no ACTIVE/HELD -> RINGING, id 0, ring true, waiting false (§9.2). */
    call_model_t m; tinit(&m);
    set_pending_mt(&m, "5551234", 0u);
    call_projection_t o; call_model_project(&m, &o);
    assert_eq_u(o.call_state, MODEM_CALL_RINGING, "R2 call_state RINGING");
    assert_eq_u(o.active_call_id, 0, "R2 active_call_id 0");
    assert_true(!o.waiting_call && o.ring_active, "R2 ring true, waiting false (§9.2 ring-only)");
    assert_true(!o.call_on_hold && !o.second_call_held, "R2 hold flags false");
    assert_eq_s(o.incoming_number, "5551234", "R2 incoming_number from pending-MT");
    assert_true(!o.caller_id_withheld, "R2 cli_validity 0 -> not withheld");
    /* cli_validity != 0 -> withheld true. */
    call_model_t m2; tinit(&m2);
    set_pending_mt(&m2, "0", 1u);
    call_projection_t o2; call_model_project(&m2, &o2);
    assert_true(o2.caller_id_withheld, "R2 cli_validity 1 -> withheld true");
}

static void test_R3_dialing(void) {
    /* R3: DIAL txn in setup, no ACTIVE and no HELD leg -> DIALING, id 0. */
    call_model_t m; tinit(&m);
    add_txn(&m, CALL_TXN_DIAL, TXN_DISPATCHED);
    call_projection_t o; call_model_project(&m, &o);
    assert_eq_u(o.call_state, MODEM_CALL_DIALING, "R3 call_state DIALING");
    assert_eq_u(o.active_call_id, 0, "R3 active_call_id 0");
    assert_true(!o.call_on_hold && !o.second_call_held, "R3 hold flags false");
    assert_true(!o.waiting_call && !o.ring_active, "R3 ring/waiting false");
}

static void test_R4_answering(void) {
    /* R4: ANSWER txn in flight, no ACTIVE leg -> ANSWERING, id 0. */
    call_model_t m; tinit(&m);
    add_txn(&m, CALL_TXN_ANSWER, TXN_DISPATCHED);
    call_projection_t o; call_model_project(&m, &o);
    assert_eq_u(o.call_state, MODEM_CALL_ANSWERING, "R4 call_state ANSWERING");
    assert_eq_u(o.active_call_id, 0, "R4 active_call_id 0");
    assert_true(!o.call_on_hold && !o.second_call_held, "R4 hold flags false");
}

static void test_R5_newcall_domination(void) {
    /* R5: surviving ACTIVE leg + a CONFIRMED 2nd-MO DIAL txn in setup ->
     * call_state DIALING dominates, active_call_id = surviving leg, flags false. */
    call_model_t m; tinit(&m);
    add_leg(&m, 2u, CALL_LEG_ACTIVE, CALL_DIR_MO);
    call_txn_t *t = add_txn(&m, CALL_TXN_DIAL, TXN_BOUND);
    t->second_mo = true; t->dir_confirmed = true;
    call_projection_t o; call_model_project(&m, &o);
    assert_eq_u(o.call_state, MODEM_CALL_DIALING, "R5 call_state DIALING (dominates ACTIVE)");
    assert_eq_u(o.active_call_id, 2u, "R5 active_call_id = surviving leg");
    assert_true(!o.call_on_hold && !o.second_call_held, "R5 hold flags false (app in SETUP)");
    assert_true(!o.waiting_call && !o.ring_active, "R5 ring/waiting false");
    /* R5 fires on a surviving HELD leg too (network holds A during New-call). */
    call_model_t m2; tinit(&m2);
    add_leg(&m2, 3u, CALL_LEG_HELD, CALL_DIR_MO);
    call_txn_t *t2 = add_txn(&m2, CALL_TXN_DIAL, TXN_BOUND);
    t2->second_mo = true; t2->dir_confirmed = true;
    call_projection_t o2; call_model_project(&m2, &o2);
    assert_eq_u(o2.call_state, MODEM_CALL_DIALING, "R5 held-surviving -> DIALING");
    assert_eq_u(o2.active_call_id, 3u, "R5 held-surviving active_call_id = held id");
    /* A merely TENTATIVE (dir-unconfirmed) 2nd DIAL must NOT dominate -> R6. */
    call_model_t m3; tinit(&m3);
    add_leg(&m3, 4u, CALL_LEG_ACTIVE, CALL_DIR_MO);
    call_leg_t *tent = add_leg(&m3, 5u, CALL_LEG_ACTIVE, CALL_DIR_UNKNOWN);
    call_txn_t *t3 = add_txn(&m3, CALL_TXN_DIAL, TXN_BOUND);
    t3->second_mo = true; t3->dir_confirmed = false;
    t3->bound_id = tent->id; t3->bound_gen = tent->generation;
    call_projection_t o3; call_model_project(&m3, &o3);
    assert_eq_u(o3.call_state, MODEM_CALL_ACTIVE, "R5 tentative 2nd DIAL does NOT dominate -> R6");
    assert_eq_u(o3.active_call_id, 4u, "R5 tentative: hold surviving ACTIVE id");
    assert_true(!o3.second_call_held, "R5 tentative: tentative leg not counted");
}

static void test_R6_single_active(void) {
    /* R6: exactly 1 ACTIVE leg, nothing else -> ACTIVE, that id, all flags false.
     * Also asserts the latch fields (§9.5, owned by later tasks) carry through. */
    call_model_t m; tinit(&m);
    m.published.last_call_result = MODEM_CALL_RESULT_CONNECTED;
    add_leg(&m, 4u, CALL_LEG_ACTIVE, CALL_DIR_MT);
    call_projection_t o; call_model_project(&m, &o);
    assert_eq_u(o.call_state, MODEM_CALL_ACTIVE, "R6 call_state ACTIVE");
    assert_eq_u(o.active_call_id, 4u, "R6 active_call_id = the id");
    assert_true(!o.call_on_hold && !o.second_call_held, "R6 hold flags false");
    assert_true(!o.waiting_call && !o.ring_active, "R6 ring/waiting false");
    assert_eq_u(o.last_call_result, MODEM_CALL_RESULT_CONNECTED, "R6 last_call_result latch carried");
    /* published updated to the fresh publish (§9 step 4). */
    assert_eq_u(m.published.active_call_id, 4u, "R6 published updated");
}

static void test_R7_sole_held(void) {
    /* R7: exactly 1 HELD leg, no ACTIVE, no ring -> ACTIVE + call_on_hold,
     * active_call_id = held id (sole-HELD->IDLE would finalize a live call, §9.1). */
    call_model_t m; tinit(&m);
    add_leg(&m, 5u, CALL_LEG_HELD, CALL_DIR_MO);
    call_projection_t o; call_model_project(&m, &o);
    assert_eq_u(o.call_state, MODEM_CALL_ACTIVE, "R7 call_state ACTIVE (never HELD)");
    assert_eq_u(o.active_call_id, 5u, "R7 active_call_id = held id");
    assert_true(o.call_on_hold, "R7 call_on_hold true");
    assert_true(!o.second_call_held, "R7 second_call_held false");
    assert_true(!o.waiting_call && !o.ring_active, "R7 ring/waiting false");
}

static void test_R8_two_call(void) {
    /* R8: 1 ACTIVE + 1 HELD -> ACTIVE, active id, second_call_held, on_hold false. */
    call_model_t m; tinit(&m);
    add_leg(&m, 4u, CALL_LEG_ACTIVE, CALL_DIR_MO);
    add_leg(&m, 5u, CALL_LEG_HELD, CALL_DIR_MT);
    call_projection_t o; call_model_project(&m, &o);
    assert_eq_u(o.call_state, MODEM_CALL_ACTIVE, "R8 call_state ACTIVE");
    assert_eq_u(o.active_call_id, 4u, "R8 active_call_id = active id");
    assert_true(!o.call_on_hold, "R8 call_on_hold false (active present)");
    assert_true(o.second_call_held, "R8 second_call_held true");
    assert_true(!o.waiting_call && !o.ring_active, "R8 ring/waiting false");
}

static void test_R9_active_plus_waiting(void) {
    /* R9: 1 ACTIVE + pending-MT waiting -> ACTIVE, active id, on_hold FALSE
     * (a waiting caller never marks the live call held, §9.1), waiting/ring
     * true, incoming from pending-MT. */
    call_model_t m; tinit(&m);
    add_leg(&m, 4u, CALL_LEG_ACTIVE, CALL_DIR_MO);
    set_pending_mt(&m, "5557777", 0u);
    call_projection_t o; call_model_project(&m, &o);
    assert_eq_u(o.call_state, MODEM_CALL_ACTIVE, "R9 call_state ACTIVE");
    assert_eq_u(o.active_call_id, 4u, "R9 active_call_id = active id");
    assert_true(!o.call_on_hold, "R9 call_on_hold FALSE always");
    assert_true(!o.second_call_held, "R9 no held leg -> second_call_held false");
    assert_true(o.waiting_call && o.ring_active, "R9 waiting+ring true");
    assert_eq_s(o.incoming_number, "5557777", "R9 pre-id waiting number from pending-MT");
}

static void test_R10_held_plus_waiting(void) {
    /* R10: sole HELD + pending-MT -> ACTIVE, held id, call_on_hold true,
     * waiting/ring true. */
    call_model_t m; tinit(&m);
    add_leg(&m, 5u, CALL_LEG_HELD, CALL_DIR_MO);
    set_pending_mt(&m, "5558888", 0u);
    call_projection_t o; call_model_project(&m, &o);
    assert_eq_u(o.call_state, MODEM_CALL_ACTIVE, "R10 call_state ACTIVE");
    assert_eq_u(o.active_call_id, 5u, "R10 active_call_id = held id");
    assert_true(o.call_on_hold, "R10 call_on_hold true");
    assert_true(!o.second_call_held, "R10 second_call_held false");
    assert_true(o.waiting_call && o.ring_active, "R10 waiting+ring true");
}

/* ---------------- consistency gate (§9 step 2) ---------------- */

static void test_gate_two_active(void) {
    /* >1 ACTIVE (lost hold URC) -> publish last_published UNCHANGED + reconcile. */
    call_model_t m; tinit(&m);
    /* seed a stable last-published to prove it is held verbatim. */
    m.published.call_state = MODEM_CALL_ACTIVE;
    m.published.active_call_id = 9u;
    m.published.second_call_held = true;
    add_leg(&m, 4u, CALL_LEG_ACTIVE, CALL_DIR_MO);
    add_leg(&m, 6u, CALL_LEG_ACTIVE, CALL_DIR_MT);
    call_projection_t o; call_model_project(&m, &o);
    assert_eq_u(o.call_state, MODEM_CALL_ACTIVE, "gate: held last_published call_state");
    assert_eq_u(o.active_call_id, 9u, "gate: held last_published active_call_id");
    assert_true(o.second_call_held, "gate: held last_published flags");
    assert_true((m.reconcile_reasons & CALL_RC_UNBINDABLE_URC) != 0u, "gate: reconcile flagged");
    assert_eq_u(m.published.active_call_id, 9u, "gate: published NOT overwritten");
}

static void test_gate_two_held(void) {
    /* >1 HELD (subsumes >=2 HELD & 0 ACTIVE, the split-swap) -> hold + reconcile. */
    call_model_t m; tinit(&m);
    m.published.call_state = MODEM_CALL_ACTIVE;
    m.published.active_call_id = 7u;
    add_leg(&m, 5u, CALL_LEG_HELD, CALL_DIR_MO);
    add_leg(&m, 6u, CALL_LEG_HELD, CALL_DIR_MT);
    call_projection_t o; call_model_project(&m, &o);
    assert_eq_u(o.active_call_id, 7u, "gate(held): held last_published");
    assert_true((m.reconcile_reasons & CALL_RC_UNBINDABLE_URC) != 0u, "gate(held): reconcile flagged");
}

/* ---------------- corner traces (§12) ---------------- */

static void test_corner1_three_leg_hold_false(void) {
    /* Corner 1: active+held+waiting (3-leg) -> R9 with call_on_hold==false
     * throughout, second_call_held true (regression guard for the audio-mute bug). */
    call_model_t m; tinit(&m);
    add_leg(&m, 4u, CALL_LEG_ACTIVE, CALL_DIR_MO);
    add_leg(&m, 5u, CALL_LEG_HELD, CALL_DIR_MT);
    call_leg_t *w = add_leg(&m, 6u, CALL_LEG_WAITING, CALL_DIR_MT);
    w->cli_withheld = false;
    memcpy(w->number, "5559999", sizeof "5559999");
    call_projection_t o; call_model_project(&m, &o);
    assert_eq_u(o.call_state, MODEM_CALL_ACTIVE, "corner1 call_state ACTIVE");
    assert_eq_u(o.active_call_id, 4u, "corner1 active_call_id = active id");
    assert_true(!o.call_on_hold, "corner1 call_on_hold FALSE (3-leg, R9)");
    assert_true(o.second_call_held, "corner1 second_call_held true");
    assert_true(o.waiting_call && o.ring_active, "corner1 waiting+ring true");
    assert_eq_s(o.incoming_number, "5559999", "corner1 waiting number from WAITING leg");
}

/* A 3-leg active+held+waiting whose WAITING leg is tearing down
 * (pending_removal) must retain its OWN published role (WAITING) and NOT be
 * miscounted as a 2nd HELD (which would trip the >1-HELD consistency gate).
 * Oracle: §9.2 R9 + §9 step-1 per-leg retention (proj_retained_role reads
 * leg->published_role), not observed output. */
static void test_pending_removal_waiting_no_gate(void) {
    call_model_t m; tinit(&m);
    add_leg(&m, 1u, CALL_LEG_HELD,    CALL_DIR_MO);
    add_leg(&m, 2u, CALL_LEG_ACTIVE,  CALL_DIR_MT);
    call_leg_t *w = add_leg(&m, 3u, CALL_LEG_WAITING, CALL_DIR_MT);
    memcpy(w->number, "5553333", sizeof "5553333");
    /* first publish records each leg's published_role (HELD / ACTIVE / WAITING). */
    call_projection_t o0; call_model_project(&m, &o0);
    assert_eq_u(o0.call_state, MODEM_CALL_ACTIVE, "waiting-teardown pre-publish ACTIVE");
    assert_true(o0.second_call_held && o0.waiting_call, "waiting-teardown pre-publish 2nd-held + waiting");
    /* the waiting leg starts tearing down (a CLCC absence) -> pending_removal. */
    w->pending_removal = true;
    call_projection_t o; call_model_project(&m, &o);
    assert_eq_u(o.call_state, MODEM_CALL_ACTIVE, "waiting-teardown gate NOT tripped -> still ACTIVE");
    assert_eq_u(o.active_call_id, 2u, "waiting-teardown active_call_id = the ACTIVE leg");
    assert_true(o.second_call_held, "waiting-teardown second_call_held true (HELD retained, NOT a 2nd HELD)");
    assert_true(o.waiting_call && o.ring_active, "waiting-teardown waiting retained (not miscounted as HELD)");
}

static void test_corner2_confirm_reveals_new_leg(void) {
    /* Corner 2: never a false-IDLE across the atomic remove-A/add-B commit.
     * Before commit A is pending_removal (retains its published ACTIVE role) ->
     * R6 A. After commit A gone, B ACTIVE -> R6 B. ACTIVE both times, id flips. */
    call_model_t m; tinit(&m);
    m.published.call_state = MODEM_CALL_ACTIVE;
    m.published.active_call_id = 1u;      /* A was the published ACTIVE */
    call_leg_t *a = add_leg(&m, 1u, CALL_LEG_ACTIVE, CALL_DIR_MO);
    a->pending_removal = true;            /* CLCC #1 omitted A: suspected-gone */
    call_projection_t o1; call_model_project(&m, &o1);
    assert_eq_u(o1.call_state, MODEM_CALL_ACTIVE, "corner2 pre-commit ACTIVE (retain A)");
    assert_eq_u(o1.active_call_id, 1u, "corner2 pre-commit id = A (retained)");
    /* atomic confirmation commit: A removed, B revealed ACTIVE, same snapshot. */
    a->in_use = false;
    add_leg(&m, 2u, CALL_LEG_ACTIVE, CALL_DIR_MO);
    call_projection_t o2; call_model_project(&m, &o2);
    assert_eq_u(o2.call_state, MODEM_CALL_ACTIVE, "corner2 post-commit ACTIVE (never IDLE)");
    assert_eq_u(o2.active_call_id, 2u, "corner2 post-commit id = B");
}

static void test_corner3_sole_tentative(void) {
    /* Corner 3: sole tentative dir-unconfirmed ACTIVE leg (lost DIALING) ->
     * DIALING (never ACTIVE) until +CLCC confirms dir==MO, then ACTIVE. */
    call_model_t m; tinit(&m);
    call_leg_t *l = add_leg(&m, 1u, CALL_LEG_ACTIVE, CALL_DIR_UNKNOWN);
    call_txn_t *t = add_txn(&m, CALL_TXN_DIAL, TXN_BOUND);
    t->dir_confirmed = false; t->bound_id = l->id; t->bound_gen = l->generation;
    call_projection_t o1; call_model_project(&m, &o1);
    assert_eq_u(o1.call_state, MODEM_CALL_DIALING, "corner3 tentative -> DIALING (not ACTIVE)");
    assert_eq_u(o1.active_call_id, 0, "corner3 tentative: active_call_id 0 (no connect)");
    assert_true(o1.call_state != MODEM_CALL_ACTIVE, "corner3 no ACTIVE before dir confirm");
    /* +CLCC confirms dir==MO. */
    t->dir_confirmed = true; l->dir = CALL_DIR_MO;
    call_projection_t o2; call_model_project(&m, &o2);
    assert_eq_u(o2.call_state, MODEM_CALL_ACTIVE, "corner3 after confirm -> ACTIVE");
    assert_eq_u(o2.active_call_id, 1u, "corner3 after confirm active_call_id = leg id");
}

static void test_corner4_abandon_bound_setup_leg(void) {
    /* Corner 4: abandon of a bound setup leg -> the leg is RELEASING with a
     * queued release; its projection is RETAINED (no no-row/IDLE gap) until
     * authoritative absence. Published was DIALING while the leg dialed. */
    call_model_t m; tinit(&m);
    m.published.call_state = MODEM_CALL_DIALING;   /* last stable publish (R3) */
    m.published.active_call_id = 0u;
    add_leg(&m, 1u, CALL_LEG_RELEASING, CALL_DIR_MO);
    /* its DIAL txn is already terminal (cancelled) -> the RETENTION FLOOR, not a
     * live DIAL txn, must hold the projection. */
    add_txn(&m, CALL_TXN_DIAL, TXN_CANCELLED);
    call_projection_t o; call_model_project(&m, &o);
    assert_eq_u(o.call_state, MODEM_CALL_DIALING, "corner4 held DIALING (never IDLE gap)");
    assert_eq_u(o.active_call_id, 0u, "corner4 held active_call_id");
    assert_true(o.call_state != MODEM_CALL_IDLE, "corner4 no false-IDLE for a tearing-down leg");
    assert_eq_u(m.published.call_state, MODEM_CALL_DIALING, "corner4 published NOT overwritten");
}

/* §5.3 + §9 step-1: "a different leg genuinely becoming ACTIVE overrides a
 * retained ACTIVE; at most one leg holds each role." A phantom
 * retained-ACTIVE leg A (pending_removal, tearing down) coexisting with a
 * genuinely-ACTIVE leg B used to tally BOTH into active_count (2), tripping
 * the step-2 consistency gate and FREEZING the projection on the phantom
 * instead of projecting B (the real, current state). */
static void test_genuine_overrides_retained_active(void) {
    call_model_t m; tinit(&m);
    call_leg_t *a = add_leg(&m, 1u, CALL_LEG_ACTIVE, CALL_DIR_MO);
    call_projection_t o0; call_model_project(&m, &o0);   /* publish: a->published_role = ACTIVE */
    assert_eq_u(o0.call_state, MODEM_CALL_ACTIVE, "genuine-override pre-publish ACTIVE (A)");
    assert_eq_u(o0.active_call_id, 1u, "genuine-override pre-publish active id = A");

    a->pending_removal = true;         /* A now tearing down: retains its ACTIVE role */
    add_leg(&m, 2u, CALL_LEG_ACTIVE, CALL_DIR_MT);   /* B genuinely ACTIVE */

    call_projection_t o; call_model_project(&m, &o);
    assert_true((m.reconcile_reasons & CALL_RC_UNBINDABLE_URC) == 0u,
                "a genuine ACTIVE does not trip the gate against a retained-transient ACTIVE");
    assert_eq_u(o.call_state, MODEM_CALL_ACTIVE, "call_state ACTIVE (not frozen on the phantom)");
    assert_eq_u(o.active_call_id, 2u, "the GENUINE leg B wins the ACTIVE role, not the phantom A");
}

/* §9 step-1+2 gate gap: pass 2 must TALLY every retained-transient leg
 * claiming a role, not just fill an empty slot once -- else TWO
 * retained-ACTIVE legs with NO genuine survivor (a lost swap/hold URC)
 * collapse to active_count==1 and the step-2 gate never fires, silently
 * emitting whichever transient leg pass 2 visits first instead of holding
 * last_published. */
static void test_two_retained_active_no_genuine_trips_gate(void) {
    call_model_t m; tinit(&m);
    call_leg_t *a = add_leg(&m, 1u, CALL_LEG_ACTIVE, CALL_DIR_MO);
    call_projection_t o0; call_model_project(&m, &o0);   /* publish: a->published_role = ACTIVE */
    assert_eq_u(o0.active_call_id, 1u, "retained-pair pre-publish active id = A");

    a->pending_removal = true;         /* A now tearing down: retains its ACTIVE role */
    call_leg_t *b = add_leg(&m, 2u, CALL_LEG_ACTIVE, CALL_DIR_MT);   /* B genuinely ACTIVE (takes over) */
    call_projection_t o1; call_model_project(&m, &o1);   /* publish: b->published_role = ACTIVE */
    assert_eq_u(o1.active_call_id, 2u,
                "precondition: B's genuine ACTIVE correctly overrides A's retained ACTIVE");

    b->pending_removal = true;         /* B ALSO now tearing down, still carrying its ACTIVE role */
    /* both A and B are now transient, BOTH published_role==ACTIVE, and NO leg
     * is genuinely ACTIVE any more -- the impossible multiset a lost
     * swap/hold URC would produce. */

    call_projection_t before = m.published;   /* the last GOOD publish (B, id 2) */
    call_projection_t o; call_model_project(&m, &o);

    assert_true((m.reconcile_reasons & (uint32_t)CALL_RC_UNBINDABLE_URC) != 0u,
                "two retained-ACTIVE legs with no genuine survivor trips the consistency gate");
    assert_eq_u(o.call_state, before.call_state, "gate holds the last published call_state");
    assert_eq_u(o.active_call_id, before.active_call_id,
                "gate holds the last published active_call_id (not a silent first-leg emit of A)");
}

int main(void) {
    test_R1_idle();
    test_R2_ringing();
    test_R3_dialing();
    test_R4_answering();
    test_R5_newcall_domination();
    test_R6_single_active();
    test_R7_sole_held();
    test_R8_two_call();
    test_R9_active_plus_waiting();
    test_R10_held_plus_waiting();
    test_gate_two_active();
    test_gate_two_held();
    test_corner1_three_leg_hold_false();
    test_pending_removal_waiting_no_gate();
    test_corner2_confirm_reveals_new_leg();
    test_corner3_sole_tentative();
    test_corner4_abandon_bound_setup_leg();
    test_genuine_overrides_retained_active();
    test_two_retained_active_no_genuine_trips_gate();
    if (s_failures) { fprintf(stderr, "%d assertion(s) failed\n", s_failures); return 1; }
    printf("all projector tests passed\n");
    return 0;
}

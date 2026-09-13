/* CLCC call-table model: pure, host-testable, core0-only.
 *
 * Owns a fixed call-leg table, a transaction pool (local intentions), and a
 * pre-id pending-MT record; consumes normalized URC events + typed +CLCC rows
 * + timer ticks, and emits the flat modem_status_t call projection plus
 * scheduling outputs. See docs/call_model.md. No heap, no locks, no AT I/O
 * here.
 */
#include "services/modem_call_model.h"

#include <stddef.h>
#include <string.h>

/* ===== CLCC scheduling, bounded-drain, id-scoped release ===== */

/* §16.12: the keepalive cadence, the fast-confirm interval, the pending_removal
 * limbo horizon, and the txn policy/abandon deadlines are NO LONGER #defines —
 * they are read from m->timing (the call_timing_t profile copied in at init). The
 * only remaining call-model constants are the backoff cap and incoming-episode
 * correlation/backstop bounds. They are model invariants, not modem command
 * timing values. */
#ifndef MODEM_CLCC_BACKOFF_MAX_SHIFT
#define MODEM_CLCC_BACKOFF_MAX_SHIFT 4u       /* ERROR/overflow backoff cap: keepalive<<4 */
#endif
#ifndef MODEM_RING_TIMEOUT_MS
#define MODEM_RING_TIMEOUT_MS        120000u  /* §9.4 stuck-RINGING pending-MT backstop */
#endif
#ifndef MODEM_MT_METADATA_TIMEOUT_MS
#define MODEM_MT_METADATA_TIMEOUT_MS 10000u   /* id-blind supplementary metadata before RING */
#endif

/* §16.5 per-op PARTICIPANT resolution (defined with the leg-table helpers):
 * the abandon/cleanup paths resolve the leg a txn owns via this — target_id
 * for a target op, bound_id for a DIAL — and the register-ownership
 * predicate uses it to identify a register owner's OWN leg. Forward-declared
 * here so both the top-of-file register-ownership predicate and
 * cm_abandon_leg (all above its definition in file order) can call it. */
static void cm_txn_owner_ref(const call_txn_t *t, uint8_t *id_out, uint8_t *gen_out);
static bool cm_txn_is_open(const call_txn_t *t);
static bool txn_baseline_contains(const call_txn_t *t, uint8_t id, uint8_t gen);
static bool cm_leg_owned_by_other_txn(const call_model_t *m, const call_txn_t *self,
                                      uint8_t id, uint8_t gen);
static bool proj_leg_is_tentative_dial(const call_model_t *m, const call_leg_t *leg);
static bool cm_cancel_txn(call_model_t *m, call_txn_t *t);

/* Forward declaration: is the pending-terminal register OWNED by a
 * still-live setup transaction (by token)? Defined below cm_txn_is_open; cm_take_pending_
 * terminal (above it in file order) consults it so a FOREIGN leg eviction skips it. */
static bool cm_pending_terminal_owned_by_live_txn(const call_model_t *m, uint8_t id, uint8_t gen);

/* clcc_evict_leg is the single terminal-eviction primitive (defined in the
 * CLCC-reconcile section): journals the terminal result, unbinds a pending-MT
 * episode (S2), bumps gen_next (§8), clears the slot. Forward-declared so the
 * tick's limbo backstop reuses it — the tick-driven limbo evict is
 * byte-identical to the
 * CLCC-driven limbo evict (no divergent raw-memset path). */
static void clcc_evict_leg(call_model_t *m, call_leg_t *leg);

/* Wrap-safe modular 32-bit ms compares avoid magic-value deadline sentinels. */
static inline bool cm_time_reached(uint32_t now, uint32_t deadline) {
    return (int32_t)(now - deadline) >= 0;
}
static inline bool cm_time_before(uint32_t a, uint32_t b) {
    return (int32_t)(a - b) < 0;
}

/* §6 edge-triggered urgency: raising an urgent reconcile reason pulls the
 * NEXT poll to the current tick ONCE, on the reason's rising edge only. If the bit
 * is already set (unresolved across an OK), the schedule stands — so a still-set
 * UNCERTAIN does not busy-loop re-polling at the same timestamp, and an ERROR
 * backoff (applied to next_clcc_ms at clcc_begin / clcc_error) is not bypassed.
 * wants_clcc stays purely schedule-based; the raise is what schedules an early poll. */
static void cm_raise_reason(call_model_t *m, uint32_t bit) {
    if ((m->reconcile_reasons & bit) == 0u) {
        m->reconcile_reasons |= bit;
        m->next_clcc_ms = m->now_ms;      /* poll at the next free slot (gates still apply) */
    }
}

/* §16.8 THE single centralized call-affecting-mutation primitive. Every call-affecting
 * transition — leg add/remove/role-change/evict/abandon, txn command-result/timeout/
 * cancel/abandon, latch writes — routes through here so NO future mutation path can
 * forget the §5.4 invalidation: if a CLCC shadow is open, the live table it will be
 * committed against just changed underneath it, so clcc_ok must DISCARD the shadow.
 * Structurally safe during the CLCC commit itself: clcc_ok_commit clears shadow_open
 * FIRST, so the merge's own leg mutations no-op here (never self-invalidate). */
static void cm_touch_shadow(call_model_t *m) {
    if (m->shadow_open) {
        m->shadow_invalidated = true;
    }
}

/* The ONLY way a non-empty pending_terminal cause register gets
 * wiped by an ACTIVE/ring/CONNECT/fresh-DIAL "clearing" observation (as opposed
 * to its owning leg's eviction, which unconditionally CONSUMES it — unchanged,
 * see cm_latch_terminal's call sites). `id`/`gen` identify the observation; an
 * id-blind observation (on_ring/on_clip/on_bare_final CONNECTED/a fresh primary
 * DIAL dispatch) passes id=0. Clears when the register is already empty (no-op),
 * OR unattributed (owner_id==0 — the old best-effort behavior, still the case
 * whenever on_bare_final found no open txn to attribute it to), OR the
 * observation is for the SAME owner {id,gen}. Never wipes a DIFFERENT leg's
 * still-pending cause. */
static void cm_reset_pending_terminal(call_model_t *m) {
    m->pending_terminal = MODEM_CALL_RESULT_NONE;
    m->pending_terminal_owner_id = 0u;
    m->pending_terminal_owner_gen = 0u;
    m->pending_terminal_owner_token = 0u;
}

static void cm_clear_pending_terminal_for_token(call_model_t *m, uint32_t token) {
    if (token != 0u && m->pending_terminal_owner_token == token) {
        cm_reset_pending_terminal(m);
    }
}

static void cm_clear_pending_terminal(call_model_t *m, uint8_t id, uint8_t gen) {
    if (m->pending_terminal == MODEM_CALL_RESULT_NONE) {
        return;                                        /* already empty */
    }
    if (m->pending_terminal_owner_token != 0u &&
        cm_pending_terminal_owned_by_live_txn(m, id, gen)) {
        return;                                        /* a foreign observation cannot clear it */
    }
    if (m->pending_terminal_owner_id == 0u ||
        (id != 0u && m->pending_terminal_owner_id == id && m->pending_terminal_owner_gen == gen)) {
        cm_reset_pending_terminal(m);
    }
}

/* An EVICTION is not a "clearing observation" (cm_clear_
 * pending_terminal above) -- it is the one place a cause gets CONSUMED. The two
 * eviction sites (on_event's RELEASING branch and clcc_evict_leg) used to hand
 * m->pending_terminal to cm_latch_terminal UNCONDITIONALLY, so a NON-owning
 * leg's eviction (a) inherited a still-lingering owner's cause (a clean call C
 * could read last_call_result == the cause that actually belonged to a
 * lingering leg A) and (b) STRIPPED the register out from under the true
 * owner's own later eviction. This is the departing leg's OWN attributed cause
 * ONLY: unattributed (owner_id==0, the old best-effort behavior) or the SAME
 * {id,gen} owner -> take it (and consume/clear the register, since it was
 * actually applied); a different owner -> NONE, and the register is left
 * untouched for its true owner's eviction to consume later. */
static modem_call_result_t cm_take_pending_terminal(call_model_t *m, uint8_t id, uint8_t gen) {
    /* A register owned by a still-LIVE setup txn that has not bound
     * this leg is that txn's own provisional cause — a FOREIGN leg eviction must SKIP it
     * (owner_id==0 for an unbound owner is NOT "truly unattributed"). Only the owning
     * txn's own leg, or its §16.3 no-leg CLCC resolution, may consume it. */
    if (m->pending_terminal_owner_token != 0u) {
        const call_txn_t *owner = NULL;
        for (unsigned i = 0; i < MODEM_MAX_CALL_TRANSACTIONS; i++) {
            if (m->txns[i].in_use &&
                m->txns[i].token == m->pending_terminal_owner_token) {
                owner = &m->txns[i];
                break;
            }
        }
        if (owner == NULL || !cm_txn_is_open(owner)) {
            /* A token-owned cause never degrades into an ownerless cause. Its
             * terminal owner already routed or discarded it. */
            cm_reset_pending_terminal(m);
            return MODEM_CALL_RESULT_NONE;
        }
        uint8_t owner_id = 0u, owner_gen = 0u;
        cm_txn_owner_ref(owner, &owner_id, &owner_gen);
        if (owner_id == 0u || owner_id != id || owner_gen != gen) {
            return MODEM_CALL_RESULT_NONE;
        }
    }
    bool owned = (m->pending_terminal_owner_id == 0u ||
                  (m->pending_terminal_owner_id == id && m->pending_terminal_owner_gen == gen));
    if (!owned) {
        return MODEM_CALL_RESULT_NONE;       /* another leg's cause: neither inherit nor strip it */
    }
    modem_call_result_t cause = m->pending_terminal;
    cm_reset_pending_terminal(m);
    return cause;
}

static bool cm_leg_is_transient(const call_leg_t *l) {
    return l->pending_removal || l->state == CALL_LEG_RELEASING;
}
static bool cm_txn_is_open(const call_txn_t *t) {
    switch (t->state) {
        case TXN_PENDING:
        case TXN_DISPATCHED:
        case TXN_BOUND:
        case TXN_UNCERTAIN:
            return true;
        default:
            return false;
    }
}
static bool cm_pending_mt_unbound(const call_model_t *m) {
    return m->pending_mt.active && !m->pending_mt.bound_valid;
}

/* True iff the pending-terminal register is OWNED by a still-live
 * setup transaction (identified by token) whose OWN current leg is not {id,gen}. Such a
 * register is that transaction's own provisional cause: a foreign leg eviction (and the
 * §16.3 unattributed-latch) must SKIP it. Keyed on the txn's LIVE participant
 * (cm_txn_owner_ref) rather than the stale owner_id snapshot, so once the owner binds a
 * leg its own leg is recognized, and once the owner resolves the register unlocks. */
static bool cm_pending_terminal_owned_by_live_txn(const call_model_t *m, uint8_t id, uint8_t gen) {
    uint32_t tok = m->pending_terminal_owner_token;
    if (tok == 0u) return false;                              /* truly unattributed */
    const call_txn_t *o = NULL;
    for (unsigned i = 0; i < MODEM_MAX_CALL_TRANSACTIONS; i++) {
        if (m->txns[i].in_use && m->txns[i].token == tok) { o = &m->txns[i]; break; }
    }
    if (o == NULL || !cm_txn_is_open(o)) return false;        /* owner retired / resolved -> unlocked */
    uint8_t oid, ogen;
    cm_txn_owner_ref(o, &oid, &ogen);                         /* the owner's OWN current leg */
    if (oid != 0u && oid == id && ogen == gen) return false;  /* the owner's own leg may consume it */
    return true;                                              /* locked to a live setup txn */
}

/* §16.3/§16.4: true iff the pending-terminal register is currently owned by a
 * still-live setup txn (by token), regardless of any leg. An UNATTRIBUTABLE later bare
 * final (0/>=2 eligible owners) must NOT clobber such a register — the owned cause is
 * authoritative and is consumed only by its owner's own leg eviction or §16.3 no-leg CLCC
 * resolution; overwriting it would destroy the attribution and mis-latch the call log. */
static bool cm_pending_terminal_has_live_owner(const call_model_t *m) {
    uint32_t tok = m->pending_terminal_owner_token;
    if (tok == 0u) return false;
    for (unsigned i = 0; i < MODEM_MAX_CALL_TRANSACTIONS; i++) {
        if (m->txns[i].in_use && m->txns[i].token == tok) {
            return cm_txn_is_open(&m->txns[i]);
        }
    }
    return false;
}

/* §6: reconcile reason bits are cleared ONLY when the model is fully settled. */
static bool cm_model_is_consistent(const call_model_t *m) {
    unsigned i;
    if (m->overflow) {
        return false;
    }
    for (i = 0; i < MODEM_MAX_CALL_LEGS; i++) {
        if (m->legs[i].in_use && cm_leg_is_transient(&m->legs[i])) {
            return false;
        }
    }
    for (i = 0; i < MODEM_MAX_CALL_TRANSACTIONS; i++) {
        if (m->txns[i].in_use && cm_txn_is_open(&m->txns[i])) {
            return false;
        }
    }
    if (cm_pending_mt_unbound(m)) {
        return false;
    }
    unsigned genuine_active = 0u, genuine_held = 0u;
    unsigned retained_active = 0u, retained_held = 0u;
    for (i = 0; i < MODEM_MAX_CALL_LEGS; i++) {
        const call_leg_t *l = &m->legs[i];
        if (!l->in_use) continue;
        if (!cm_leg_is_transient(l)) {
            if (l->state == CALL_LEG_ACTIVE && !proj_leg_is_tentative_dial(m, l)) {
                genuine_active++;
            } else if (l->state == CALL_LEG_HELD) {
                genuine_held++;
            }
        } else if (l->published_role == CALL_LEG_ACTIVE) {
            retained_active++;
        } else if (l->published_role == CALL_LEG_HELD) {
            retained_held++;
        }
    }
    if (genuine_active > 1u || genuine_held > 1u ||
        (genuine_active == 0u && retained_active > 1u) ||
        (genuine_held == 0u && retained_held > 1u)) {
        return false;
    }
    return true;
}

/* Any live element keeps the CLCC keepalive alive (§6: empty model -> no poll). */
static bool cm_model_has_activity(const call_model_t *m) {
    if (call_model_session_active(m)) return true;
    for (unsigned i = 0; i < MODEM_MAX_CALL_TRANSACTIONS; i++) {
        const call_txn_t *t = &m->txns[i];
        /* §16.11: an UNBOUND tombstone with unresolved cleanup keeps the keepalive
         * alive so a clean CLCC can prove its dispatched command's leg absent (a bound
         * tombstone's RELEASING leg is already counted above). */
        if (t->in_use && t->is_tombstone && !t->cleanup_confirmed) {
            bool unbound = (t->kind == CALL_TXN_DIAL) ? (t->bound_id == 0u) : (t->target_id == 0u);
            if (unbound) return true;
        }
    }
    return false;
}

bool call_model_control_pending(const call_model_t *m) {
    if (m == NULL) return false;
    if (m->pending_mt.active && m->pending_mt.alert_observed) return true;
    for (unsigned i = 0u; i < MODEM_MAX_CALL_TRANSACTIONS; i++) {
        if (m->txns[i].in_use && cm_txn_is_open(&m->txns[i])) return true;
    }
    for (unsigned i = 0u; i < MODEM_MAX_CALL_LEGS; i++) {
        const call_leg_t *leg = &m->legs[i];
        if (!leg->in_use) continue;
        if (leg->pending_removal ||
            (leg->state != CALL_LEG_ACTIVE && leg->state != CALL_LEG_HELD)) {
            return true;
        }
    }
    return false;
}

bool call_model_release_active_pending(const call_model_t *m) {
    if (m == NULL) return false;
    for (unsigned i = 0u; i < MODEM_MAX_CALL_TRANSACTIONS; i++) {
        const call_txn_t *txn = &m->txns[i];
        if (txn->in_use && txn->kind == CALL_TXN_RELEASE_ACTIVE &&
            cm_txn_is_open(txn)) {
            return true;
        }
    }
    return false;
}

bool call_model_background_work_blocked(const call_model_t *m) {
    if (m == NULL) return false;
    if (m->pending_mt.active && m->pending_mt.alert_observed) return true;

    for (unsigned i = 0u; i < MODEM_MAX_CALL_LEGS; i++) {
        const call_leg_t *leg = &m->legs[i];
        if (!leg->in_use) continue;
        if (leg->pending_removal || leg->state == CALL_LEG_INCOMING ||
            leg->state == CALL_LEG_WAITING ||
            leg->state == CALL_LEG_RELEASING) {
            return true;
        }
    }

    for (unsigned i = 0u; i < MODEM_MAX_CALL_TRANSACTIONS; i++) {
        const call_txn_t *txn = &m->txns[i];
        if (!txn->in_use || !cm_txn_is_open(txn)) continue;
        switch (txn->kind) {
        case CALL_TXN_ANSWER:
        case CALL_TXN_WAIT_ANSWER:
        case CALL_TXN_WAIT_REJECT:
        case CALL_TXN_HANGUP:
            return true;
        default:
            break;
        }
    }
    return false;
}

bool call_model_session_active(const call_model_t *m) {
    if (m == NULL) return false;
    if (m->pending_mt.active) return true;
    for (unsigned i = 0u; i < MODEM_MAX_CALL_LEGS; i++) {
        if (m->legs[i].in_use) return true;
    }
    return call_model_control_pending(m);
}

/* A call-control op still mid-issue (written, OK not yet seen): defer the
 * periodic/confirmation CLCC behind it (§6). "In flight"
 * means the AT COMMAND itself is outstanding — DISPATCHED-and-not-yet-accepted
 * (written, OK not yet seen). Once at_accepted is true the OK has landed; the
 * op may still be mid-issue toward its POSTCONDITION (bound/resolving), but
 * that is exactly what a recovery CLCC is for, so it must not itself be gated
 * behind cm_txn_in_flight (previously DISPATCHED alone deferred all the way to
 * the txn's postcondition or the 40 s policy deadline, ignoring at_accepted
 * entirely — a write-only dead field).
 * TXN_PENDING is DELIBERATELY EXCLUDED — a PENDING
 * txn has not been written to the modem at all (call_model_request allocated
 * it, call_model_txn_dispatched has not yet run), so there is no AT command
 * "in flight" to defer behind. Counting it here used to let a not-yet-
 * dispatched (possibly orphaned, §7.3) txn suppress the WHOLE model's CLCC
 * keepalive — including a live, unrelated leg's own scheduled reconcile —
 * for as long as the txn sat PENDING (up to the dispatch watchdog). The
 * self-healing watchdog (txn_deadlines) bounds how long an orphan can even
 * exist; this function only needs to defer the genuinely-outstanding case. */
static bool cm_txn_in_flight(const call_model_t *m) {
    unsigned i;
    for (i = 0; i < MODEM_MAX_CALL_TRANSACTIONS; i++) {
        if (m->txns[i].in_use &&
            m->txns[i].state == TXN_DISPATCHED && !m->txns[i].at_accepted) {
            return true;
        }
    }
    return false;
}

/* Current CLCC issue interval: fast confirm inside an armed window, else the
 * keepalive shifted by the CLCC error backoff (exponential, capped).
 * The fast-confirm interval is ALSO shifted by the
 * backoff — being inside the fast window no longer flattens to a raw 300 ms
 * regardless of a climbing ERROR backoff (which used to let a live UNCERTAIN
 * txn hammer a failing CLCC channel every 300 ms). The fast interval stays the
 * SHORTER of the two at any given shift (300<<shift is always < 4000<<shift),
 * so it still accelerates confirmation relative to the keepalive — it just no
 * longer bypasses backoff entirely. */
static uint32_t cm_clcc_interval_ms(const call_model_t *m) {
    uint8_t shift = m->clcc_backoff_shift;
    if (shift > (uint8_t)MODEM_CLCC_BACKOFF_MAX_SHIFT) {
        shift = (uint8_t)MODEM_CLCC_BACKOFF_MAX_SHIFT;
    }
    if (cm_time_before(m->now_ms, m->fast_clcc_until_ms)) {
        return m->timing.clcc_confirm_ms << shift;
    }
    return m->timing.clcc_keepalive_ms << shift;
}

/* §5.3 bounded limbo: a pending_removal leg CLCC-absent since first_removal_ms +
 * limbo is force-evicted (terminal) via the shared clcc_evict_leg — feeds the §6
 * drain invariant. Keys on the authoritative absence clock (first_removal_ms != 0,
 * CLCC-reconcile step 1), NOT the mask. */
static void cm_advance_removal_limbo(call_model_t *m) {
    unsigned i;
    for (i = 0; i < MODEM_MAX_CALL_LEGS; i++) {
        call_leg_t *l = &m->legs[i];
        if (l->in_use && l->first_removal_ms != 0u &&
            cm_time_reached(m->now_ms, l->first_removal_ms + m->timing.leg_limbo_ms)) {
            cm_touch_shadow(m);           /* §5.4: a tick eviction races any open CLCC */
            clcc_evict_leg(m, l);
        }
    }
}

/* §9.4 stuck-RINGING backstop: an UNBOUND pending-MT silent for the ring timeout
 * is cleared (missed). A bound episode is tracked by its table leg. */
static void cm_advance_pending_mt(call_model_t *m) {
    if (!cm_pending_mt_unbound(m)) return;

    /* A supplementary redirected-call indication can legally precede RING.
     * Keep it briefly as hidden pre-id metadata, but never let an orphan CSSU
     * contaminate a later unrelated incoming episode. */
    if (!m->pending_mt.alert_observed && m->pending_mt.incoming_diverted) {
        if (cm_time_reached(m->now_ms,
                            m->pending_mt.diverted_observed_ms +
                                MODEM_MT_METADATA_TIMEOUT_MS)) {
            memset(&m->pending_mt, 0, sizeof(m->pending_mt));
        }
        return;
    }
    if (cm_time_reached(m->now_ms,
                        m->pending_mt.last_ring_ms + MODEM_RING_TIMEOUT_MS)) {
        memset(&m->pending_mt, 0, sizeof(m->pending_mt));
    }
}

/* §6 fast-poll window end: fast while an UNCERTAIN txn is inside its
 * policy_deadline, while a removal candidate is armed, or right after a bare
 * final (COARSE_FINAL) -> a short confirm. No fast reason -> window == now. */
static void cm_recompute_fast_window(call_model_t *m) {
    uint32_t until = m->now_ms;
    bool have = false;
    unsigned i;
    for (i = 0; i < MODEM_MAX_CALL_TRANSACTIONS; i++) {
        const call_txn_t *t = &m->txns[i];
        if (t->in_use && t->state == TXN_UNCERTAIN && t->policy_deadline_ms != 0u &&
            cm_time_before(m->now_ms, t->policy_deadline_ms)) {
            if (!have || cm_time_before(until, t->policy_deadline_ms)) {
                until = t->policy_deadline_ms;
                have = true;
            }
        }
    }
    if (m->removal_armed ||
        (m->reconcile_reasons & (uint32_t)CALL_RC_COARSE_FINAL) != 0u) {
        uint32_t confirm = m->now_ms + m->timing.clcc_confirm_ms;
        if (!have || cm_time_before(until, confirm)) {
            until = confirm;
            have = true;
        }
    }
    m->fast_clcc_until_ms = have ? until : m->now_ms;
}

void call_model_init(call_model_t *m, const call_timing_t *timing) {
    if (m == 0) {
        return;
    }
    /* Zero the whole model: empties the leg table, txn pool, pending-MT,
     * shadow/absent scratch, release queue and scheduling. Because
     * MODEM_CALL_IDLE == 0 and MODEM_CALL_RESULT_NONE == 0, the memset already
     * leaves `published` as the R1 empty projection (§9: all ids 0, all flags
     * false, both result latches NONE); the explicit assignments below make
     * that R1 seeding intentional and independent of the enum-zero coincidence. */
    memset(m, 0, sizeof(*m));

    /* §16.12: copy the neutral timing profile BY VALUE. The model bakes no vendor
     * constants — cadence/confirm/limbo/policy/abandon all come from here. A NULL
     * profile is defensively tolerated (timing stays zeroed) but is a caller bug. */
    if (timing != NULL) {
        m->timing = *timing;
    }

    m->published.call_state = MODEM_CALL_IDLE;
    m->published.active_call_id = 0u;
    m->published.call_on_hold = false;
    m->published.second_call_held = false;
    m->published.waiting_call = false;
    m->published.ring_active = false;
    m->published.caller_id_withheld = false;
    m->published.incoming_diverted = false;
    m->published.incoming_number[0] = '\0';
    m->published.last_call_result = MODEM_CALL_RESULT_NONE;
    m->published.second_call_result = MODEM_CALL_RESULT_NONE;
    m->pending_terminal = MODEM_CALL_RESULT_NONE;   /* §9.5 pending primary terminal cause */

    /* Token 0 is the call_model_request "rejected" sentinel, so live tokens
     * start at 1. */
    m->next_token = 1u;
    /* §16.6 episode generations start at 1 (0 = "no episode / episode-agnostic"). */
    m->mt_episode_next = 1u;

    /* §8 generation history: every call id hands out generation 1 first
     * (memset left these 0; the per-id next-generation must start at 1). */
    for (unsigned i = 0u; i <= MODEM_MAX_CALL_LEGS; i++) {
        m->gen_next[i] = 1u;
    }
    /* §9.5/§11 terminal journal + §5.4 shadow-invalidation flag start clear. */
    m->journal_count = 0u;
    m->journal_incomplete = false;
    m->shadow_invalidated = false;
}

void call_model_set_now(call_model_t *m, uint32_t now_ms) {
    if (m == 0) {
        return;
    }
    m->now_ms = now_ms;
}

/* ======================================================================
 * §9 projector — the total role-mapping algorithm.
 * Pure function of model state; the ONLY mutations are m->published
 * (the last_published_roles snapshot) on a normal publish and
 * m->reconcile_reasons on the consistency gate.
 * ====================================================================== */

typedef enum { ROLE_NONE = 0, ROLE_ACTIVE, ROLE_HELD } counted_role_t;

/* Copy up to MODEM_PHONE_MAX chars into a fixed dst[MODEM_PHONE_MAX+1]. */
static void proj_copy_number(char *dst, const char *src) {
    size_t n = 0;
    if (src) { for (; n < MODEM_PHONE_MAX && src[n]; n++) dst[n] = src[n]; }
    dst[n] = '\0';
}

/* Retained (last-published) role of a suspected-gone leg (RELEASING or
 * pending_removal): §5.3 / §9 step 1 -- such a leg is NEVER dropped early, it
 * carries the exact role it was PROJECTED as last publish, recorded per-leg in
 * leg->published_role. Using the leg's own published identity -- not a
 * heuristic over active_call_id/second_call_held -- lets a departing WAITING leg
 * keep a waiting role instead of being miscounted as a 2nd HELD (which used to
 * trip the >1-HELD consistency gate in the 3-leg active+held+waiting case).
 * ACTIVE/HELD map to the counted roles here; a retained WAITING/INCOMING is
 * handled by the caller (it contributes `waiting`, not a counted ACTIVE/HELD);
 * DIALING/UNKNOWN -> NONE, protected by the transient floor (never R1 IDLE). */
static counted_role_t proj_retained_role(const call_model_t *m, const call_leg_t *leg) {
    (void)m;
    if (leg->published_role == CALL_LEG_ACTIVE) return ROLE_ACTIVE;
    if (leg->published_role == CALL_LEG_HELD)   return ROLE_HELD;
    return ROLE_NONE;
}

/* True if this ACTIVE-state leg is only tentatively bound to an unconfirmed-
 * direction DIAL (§7.4): its owning DIAL is still "in setup", so the leg is
 * never counted as a stable ACTIVE/HELD role until +CLCC confirms dir==MO. */
static bool proj_leg_is_tentative_dial(const call_model_t *m, const call_leg_t *leg) {
    for (unsigned i = 0; i < MODEM_MAX_CALL_TRANSACTIONS; i++) {
        const call_txn_t *t = &m->txns[i];
        if (!t->in_use || t->kind != CALL_TXN_DIAL) continue;
        if (t->state == TXN_SUCCEEDED || t->state == TXN_FAILED ||
            t->state == TXN_CANCELLED) continue;
        if (t->bound_id == leg->id && t->bound_gen == leg->generation && !t->dir_confirmed) {
            return true;
        }
    }
    return false;
}

static bool proj_txn_in_setup(const call_txn_t *t) {
    return t->in_use && (t->state == TXN_PENDING || t->state == TXN_DISPATCHED ||
                         t->state == TXN_BOUND   || t->state == TXN_UNCERTAIN);
}

/* R5 New-call domination requires a CONFIRMED 2nd-MO DIAL/ANSWER still in setup
 * (a merely tentative dir-unconfirmed DIAL does NOT dominate, §7.4/§9.2). */
static const call_txn_t *proj_find_newcall_setup(const call_model_t *m) {
    for (unsigned i = 0; i < MODEM_MAX_CALL_TRANSACTIONS; i++) {
        const call_txn_t *t = &m->txns[i];
        if (!proj_txn_in_setup(t) || !t->second_mo) continue;
        if (t->kind == CALL_TXN_ANSWER) return t;
        if (t->kind == CALL_TXN_DIAL && t->dir_confirmed) return t;
    }
    return NULL;
}

static bool proj_find_dial_setup(const call_model_t *m) {
    for (unsigned i = 0; i < MODEM_MAX_CALL_TRANSACTIONS; i++) {
        if (proj_txn_in_setup(&m->txns[i]) && m->txns[i].kind == CALL_TXN_DIAL) return true;
    }
    return false;
}
static bool proj_find_answer_setup(const call_model_t *m) {
    for (unsigned i = 0; i < MODEM_MAX_CALL_TRANSACTIONS; i++) {
        if (proj_txn_in_setup(&m->txns[i]) && m->txns[i].kind == CALL_TXN_ANSWER) return true;
    }
    return false;
}

/* incoming_number / caller_id_withheld: from pending-MT before bind, else the
 * INCOMING/WAITING leg (§9.2), else metadata retained on an answered MT leg.
 * The last case is required when RING/+CLIP/INCOMING/ACTIVE are all consumed
 * before the once-per-tick projector runs. */
static void proj_fill_incoming(const call_model_t *m, call_projection_t *out) {
    if (m->pending_mt.active && m->pending_mt.alert_observed) {
        proj_copy_number(out->incoming_number, m->pending_mt.number);
        out->caller_id_withheld = (m->pending_mt.cli_validity != 0u);
        out->incoming_diverted = m->pending_mt.incoming_diverted;
        return;
    }
    for (unsigned i = 0; i < MODEM_MAX_CALL_LEGS; i++) {
        const call_leg_t *leg = &m->legs[i];
        if (!leg->in_use) continue;
        if (leg->state == CALL_LEG_WAITING || leg->state == CALL_LEG_INCOMING) {
            proj_copy_number(out->incoming_number, leg->number);
            out->caller_id_withheld = leg->cli_withheld;
            out->incoming_diverted = leg->incoming_diverted;
            return;
        }
    }
    for (unsigned i = 0; i < MODEM_MAX_CALL_LEGS; i++) {
        const call_leg_t *leg = &m->legs[i];
        if (!leg->in_use || !leg->mt_cli_valid || leg->dir != CALL_DIR_MT) continue;
        if (leg->state == CALL_LEG_ACTIVE || leg->state == CALL_LEG_HELD) {
            proj_copy_number(out->incoming_number, leg->number);
            out->caller_id_withheld = leg->cli_withheld;
            /* Redirected-call wording belongs only to the presenting incoming
             * leg. Once answered/held, preserve CLI but retire that UI marker. */
            out->incoming_diverted = false;
            return;
        }
    }
}

void call_model_project(call_model_t *m, call_projection_t *out) {
    /* ---- step 0: RETIRE the bounded terminal journal (§16.4 — the projector no
     * longer DERIVES latches from the journal; the owning-transaction result-latch
     * transitions already wrote last/second_call_result at the departure, via
     * cm_route_result). The journal now records actual departures ONLY, for
     * future app-side consumption; today the projector drains (retires) every
     * entry each publish so it cannot grow while calls_app is untouched. */
    if (m->journal_count > 0u) {
        for (unsigned i = 0; i < MODEM_MAX_CALL_LEGS; i++) {
            m->journal[i].in_use = false;
        }
        m->journal_count = 0u;
    }

    /* ---- step 1: assign roles from the legs, with retention ----
     * §5.3 + §9 step 1: "a different leg genuinely becoming ACTIVE
     * overrides a retained ACTIVE; at most one leg holds each role." A single
     * combined pass used to tally a retained-transient role's contribution
     * unconditionally, so a phantom retained-ACTIVE (pending_removal) leg PLUS a
     * genuinely-ACTIVE leg both counted -> active_count==2 -> the step-2 gate
     * froze on the phantom. Fixed with TWO passes: genuine (non-transient) roles
     * are tallied FIRST; a retained-transient role then fills a role slot ONLY
     * when no genuine leg already holds it (a retained WAITING/INCOMING is an
     * OR, not a uniqueness-gated count, so it is unaffected — unchanged). */
    unsigned active_count = 0u, held_count = 0u;
    /* §9 step-1+2 gate gap: pass 2's "fill a role slot ONLY
     * when no genuine leg already holds it" collapses TWO (or more) retained-
     * transient legs both claiming the SAME role to active_count/held_count
     * == 1 — a retained-ONLY impossible multiset (e.g. two RELEASING/
     * pending_removal legs both still carrying published_role ACTIVE, no
     * genuine survivor: a lost swap/hold URC) then silently emits whichever
     * transient leg pass 2 visited first instead of tripping the step-2 gate.
     * genuine_active/genuine_held freeze pass 1's counts; retained_active/
     * retained_held TALLY every matching transient leg (unlike active_count/
     * held_count, which still only fill the slot once) so the gate below can
     * see the true retained-only multiset. */
    unsigned genuine_active = 0u, genuine_held = 0u;
    unsigned retained_active = 0u, retained_held = 0u;
    uint8_t active_id = 0u, held_id = 0u;
    bool waiting = false;       /* WAITING/INCOMING leg or pending-MT */
    bool incoming_leg = false;  /* an INCOMING/WAITING leg is present */
    bool dialing_leg = false;   /* a DIALING/ALERTING setup leg is present */
    bool has_transient = false; /* any RELEASING / pending_removal leg */

    /* Pass 1: genuine (non-transient) roles. */
    for (unsigned i = 0; i < MODEM_MAX_CALL_LEGS; i++) {
        const call_leg_t *leg = &m->legs[i];
        if (!leg->in_use) continue;
        if (leg->state == CALL_LEG_RELEASING || leg->pending_removal) continue;   /* pass 2 */

        switch (leg->state) {
        case CALL_LEG_ACTIVE:
            if (proj_leg_is_tentative_dial(m, leg)) break;   /* §7.4: not counted */
            active_count++; active_id = leg->id; break;
        case CALL_LEG_HELD:
            held_count++; held_id = leg->id; break;
        case CALL_LEG_WAITING:
        case CALL_LEG_INCOMING:
            waiting = true; incoming_leg = true; break;
        case CALL_LEG_DIALING:
        case CALL_LEG_ALERTING:
            dialing_leg = true; break;
        default: break;   /* UNKNOWN: contributes nothing */
        }
    }
    genuine_active = active_count;
    genuine_held = held_count;

    /* Pass 2: retained (transient) roles — fill a role slot ONLY when no
     * genuine leg already holds it (genuine overrides retained); but
     * TALLY every matching transient leg regardless, so >=2 retained
     * claimants of the same role are visible to the step-2 gate even though
     * only the first fills active_count/held_count/active_id/held_id. */
    for (unsigned i = 0; i < MODEM_MAX_CALL_LEGS; i++) {
        const call_leg_t *leg = &m->legs[i];
        if (!leg->in_use) continue;
        if (!(leg->state == CALL_LEG_RELEASING || leg->pending_removal)) continue;   /* pass 1 handled it */

        /* suspected-gone / tearing-down: contribute its RETAINED published
         * role only (§5.3 / §9 step 1, by per-leg identity), never a
         * new role. A retained ACTIVE/HELD counts (unless a genuine leg
         * already holds that role); a retained WAITING/INCOMING keeps
         * the waiting indication so a 3-leg active+held+waiting whose
         * waiting leg is tearing down is NOT miscounted as a 2nd HELD. */
        has_transient = true;
        counted_role_t rr = proj_retained_role(m, leg);
        if (rr == ROLE_ACTIVE) {
            retained_active++;
            if (active_count == 0u) { active_count++; active_id = leg->id; }
        } else if (rr == ROLE_HELD) {
            retained_held++;
            if (held_count == 0u) { held_count++; held_id = leg->id; }
        } else if (leg->published_role == CALL_LEG_WAITING ||
                   leg->published_role == CALL_LEG_INCOMING) {
            waiting = true; incoming_leg = true;
        }
    }
    if (m->pending_mt.active && m->pending_mt.alert_observed) waiting = true;

    /* ---- step 2: consistency gate ---- */
    /* >1 ACTIVE or >1 HELD is structurally impossible (a lost swap/hold URC).
     * >1 HELD already subsumes the ">=2 HELD & 0 ACTIVE" split-swap case.
     * The tally widens the gate to the retained-ONLY case the counts above can't
     * see on their own: no genuine survivor for a role (genuine_active/held
     * == 0), yet more than one transient leg claims it (retained_active/held
     * > 1). Hold the WHOLE last_published_roles snapshot unchanged + flag
     * reconcile; CLCC resolves it. Do NOT touch m->published. */
    if (active_count > 1u || held_count > 1u ||
        (genuine_active == 0u && retained_active > 1u) ||
        (genuine_held == 0u && retained_held > 1u)) {
        cm_raise_reason(m, (uint32_t)CALL_RC_UNBINDABLE_URC);
        *out = m->published;
        return;
    }

    /* ---- step 3: map the counted multiset (<=1 ACTIVE, <=1 HELD) to one row --- */
    call_projection_t r;
    memset(&r, 0, sizeof r);
    /* latches are model-owned (§9.5): the projector DRAINS/CARRIES, never derives. */
    r.last_call_result   = m->published.last_call_result;
    r.second_call_result = m->published.second_call_result;
    r.caller_id_withheld = m->published.caller_id_withheld;
    proj_copy_number(r.incoming_number, m->published.incoming_number);
    proj_fill_incoming(m, &r);
    r.incoming_diverted  = false;

    const bool active = (active_count == 1u);
    const bool held   = (held_count   == 1u);
    const call_txn_t *newcall = proj_find_newcall_setup(m);

    if (active || held) {
        if (newcall != NULL) {
            /* R5: a CONFIRMED 2nd-MO setup dominates call_state; the surviving
             * leg keeps active_call_id; the app is in SETUP and ignores the hold
             * flags, so they stay false. */
            r.call_state = (newcall->kind == CALL_TXN_ANSWER)
                               ? MODEM_CALL_ANSWERING : MODEM_CALL_DIALING;
            r.active_call_id = active ? active_id : held_id;
        } else {
            /* R6..R10: every stable connected row is call_state ACTIVE. */
            r.call_state       = MODEM_CALL_ACTIVE;
            r.active_call_id   = active ? active_id : held_id;
            r.call_on_hold     = (held && !active);   /* sole HELD only (R7/R10); FALSE whenever an ACTIVE leg exists (R8/R9) */
            r.second_call_held = (active && held);    /* two-call (R8, and R9 3-leg) */
            r.waiting_call = r.ring_active = waiting;  /* R9/R10 */
            if (waiting) proj_fill_incoming(m, &r);
        }
    } else {
        /* no counted ACTIVE/HELD role. */
        if (proj_find_answer_setup(m)) {
            /* R4: an in-setup ANSWER txn DOMINATES the R2 incoming-leg branch --
             * during answering the INCOMING leg is still present, so ANSWER must
             * win (spec S9.2 precedence + S9.4 "ring inputs do not re-open the
             * incoming UI while ANSWERING"). Flags stay false (R4 row). */
            r.call_state = MODEM_CALL_ANSWERING;               /* R4 */
        } else if ((m->pending_mt.active &&
                    m->pending_mt.alert_observed) || incoming_leg) {
            /* R2 (and the R2>R3 tiebreak: incoming RINGING beats a fresh DIAL).
             * Spec §9.2 R2 is ring-only: with NO foreground ACTIVE/HELD call this
             * is a plain incoming ring, not call-waiting -- waiting_call stays
             * false (it is TRUE only in R9/R10, where a foreground call coexists). */
            r.call_state   = MODEM_CALL_RINGING;
            r.ring_active  = true;
            r.waiting_call = false;
            proj_fill_incoming(m, &r);
        } else if (proj_find_dial_setup(m) || dialing_leg) {
            r.call_state = MODEM_CALL_DIALING;                 /* R3 */
        } else {
            /* R1 empty -- but NEVER finalize while a leg is still tearing down
             * (transient floor): hold the whole last-published snapshot. */
            if (has_transient) { *out = m->published; return; }
            r.call_state = MODEM_CALL_IDLE;                    /* R1 */
            /* Legacy's id-scoped RELEASED path clears the number at the full
             * session boundary but leaves caller_id_withheld latched until a
             * later incoming CLI overwrites it. Recorded A9 characterization
             * pins this lifetime. */
            r.incoming_number[0] = '\0';
            /* §9.5/§16.9 IDLE / full-teardown reset: R1 is a genuine session
             * boundary (no leg survives retention, no txn in setup, no pending-MT),
             * so the 2nd-MO latch MUST clear here — else a finished session's New-call
             * failure (e.g. BUSY) leaks across a later unrelated incoming. ASYMMETRIC
             * to last_call_result, which is RETAINED across IDLE for the call log
             * (r.last_call_result already carries m->published.last_call_result above).
             * Safe for the app read window: the consumer reads second_call_result
             * during New-call SETUP (before IDLE), never at IDLE. */
            r.second_call_result = MODEM_CALL_RESULT_NONE;
        }
    }

    /* ---- step 4: publish + update last_published_roles + per-leg published_role ---- */
    /* Record, per in_use leg, the exact role it was projected as this publish so a
     * later teardown (RELEASING / pending_removal) retains its own identity
     * (see proj_retained_role). Transient legs keep their prior role. */
    for (unsigned i = 0; i < MODEM_MAX_CALL_LEGS; i++) {
        call_leg_t *leg = &m->legs[i];
        if (!leg->in_use) continue;
        if (leg->state == CALL_LEG_RELEASING || leg->pending_removal) {
            continue;   /* keep the role it last held */
        }
        call_leg_state_t role = CALL_LEG_UNKNOWN;
        if (active_count == 1u && leg->id == active_id &&
            leg->state == CALL_LEG_ACTIVE && !proj_leg_is_tentative_dial(m, leg)) {
            role = CALL_LEG_ACTIVE;
        } else if (held_count == 1u && leg->id == held_id && leg->state == CALL_LEG_HELD) {
            role = CALL_LEG_HELD;
        } else if (leg->state == CALL_LEG_WAITING || leg->state == CALL_LEG_INCOMING) {
            role = leg->state;
        } else if (leg->state == CALL_LEG_DIALING || leg->state == CALL_LEG_ALERTING) {
            role = CALL_LEG_DIALING;
        }
        leg->published_role = role;
    }
    m->published = r;
    *out = r;
}

/* --- leg-table helpers (defined ONCE here; reused verbatim by the
 *     transaction/reconcile/tick sections later in file order — no
 *     redefinition). --- */

/* Forward declaration: the transaction-reconcile engine is defined later in
 * this TU. call_model_on_event's tail hook calls it, so it must be visible
 * here. */
void model_reconcile_txns(call_model_t *m);

/* Forward declaration: §16.9 two-axis retirement eligibility (defined with
 * the transaction engine's retirement pass). call_model_request's lazy
 * slot-reclaim gates on it so it never frees a tombstone (unresolved cleanup)
 * or a live-leg SUCCEEDED txn. */
static bool cm_txn_retirement_eligible(call_model_t *m, const call_txn_t *t);

/* Forward declaration: §16.6 episode-staleness (defined with txn_bind_targets).
 * The dispatch guard uses it so an episode-bound op cannot SEND against a replaced episode. */
static bool cm_txn_episode_stale(const call_model_t *m, const call_txn_t *t);

/* Canonical finder: the live (in_use) leg matching BOTH id and generation, so a
 * stale generation-qualified ref (transaction / CLCC) cannot act on a reused id
 * (§8). */
static call_leg_t *cm_find_leg(call_model_t *m, uint8_t id, uint8_t gen) {
    for (unsigned i = 0; i < MODEM_MAX_CALL_LEGS; i++) {
        if (m->legs[i].in_use && m->legs[i].id == id && m->legs[i].generation == gen) {
            return &m->legs[i];
        }
    }
    return NULL;
}

static bool cm_has_leg(const call_model_t *m, uint8_t id, uint8_t gen) {
    for (unsigned i = 0; i < MODEM_MAX_CALL_LEGS; i++) {
        if (m->legs[i].in_use && m->legs[i].id == id &&
            m->legs[i].generation == gen) {
            return true;
        }
    }
    return false;
}

/* id-only finder (a URC carries an id but no generation). */
static call_leg_t *cm_find_leg_by_id(call_model_t *m, uint8_t id) {
    for (unsigned i = 0; i < MODEM_MAX_CALL_LEGS; i++) {
        if (m->legs[i].in_use && m->legs[i].id == id) {
            return &m->legs[i];
        }
    }
    return NULL;
}

/* §8 generation history: gen_next[id] is the next generation to hand out for a call
 * id. cm_remove_leg / clcc_evict_leg bump it past the departing leg's generation
 * BEFORE the slot is recycled, so a freed-then-reused id is disambiguated even after
 * the slot is reused for a DIFFERENT id (the old scan-the-live-slots scheme LOST that
 * history once the freed slot was overwritten). call_model_init seeds every entry to 1;
 * zero stays reserved for an absent/wildcard generation when the uint8_t counter wraps. */
static uint8_t cm_generation_after(uint8_t generation) {
    uint8_t next = (uint8_t)(generation + 1u);
    return next != 0u ? next : 1u;
}

/* THE single leg allocator — the URC handlers AND the CLCC reconcile both
 * call it (no duplicate allocator).
 * generation = gen_next[id]. On a full table: flag overflow (§10) and return NULL. */
static call_leg_t *cm_alloc_leg(call_model_t *m, uint8_t id) {
    for (unsigned i = 0; i < MODEM_MAX_CALL_LEGS; i++) {
        if (!m->legs[i].in_use) {
            cm_touch_shadow(m);        /* §16.8: a leg-table mutation invalidates an open shadow */
            call_leg_t *L = &m->legs[i];
            memset(L, 0, sizeof(*L));
            L->in_use = true;
            L->id = id;
            L->generation = m->gen_next[id];
            L->state = CALL_LEG_UNKNOWN;
            L->dir = CALL_DIR_UNKNOWN;
            L->mode = CALL_MODE_UNKNOWN;
            L->published_role = CALL_LEG_UNKNOWN;
            L->was_answered = false;
            L->first_seen_ms = m->now_ms;
            L->last_seen_ms = m->now_ms;
            return L;
        }
    }
    m->overflow = true;
    m->reconcile_reasons |= (uint32_t)CALL_RC_OVERFLOW;
    return NULL;
}

/* §9.4: drop a pending-MT episode binding that points at {id,gen}. Shared by EVERY
 * leg-eviction path (cm_remove_leg AND the CLCC clcc_evict_leg) so a removed
 * INCOMING/WAITING leg can never leave a phantom RINGING/waiting with a stale
 * incoming_number behind it. */
static void cm_unbind_pending_mt(call_model_t *m, uint8_t id, uint8_t gen) {
    if (m->pending_mt.bound_valid && m->pending_mt.bound_id == id &&
        m->pending_mt.bound_gen == gen) {
        m->pending_mt.active = false;
        m->pending_mt.bound_valid = false;
    }
}

/* Phase-1 compatibility ownership for the flat incoming_number field. Legacy
 * has one global slot: a newer incoming episode replaces the older caller's
 * display metadata, and releasing that owning leg clears the slot rather than
 * revealing an older caller again. The leg's actual number remains intact for
 * the Phase-2 terminal journal. */
static void cm_retire_compat_cli_owner(call_model_t *m, call_leg_t *leg) {
    if (!leg->mt_cli_valid) return;
    leg->mt_cli_valid = false;
    m->published.incoming_number[0] = '\0';
}

/* Legacy has one flat caller-number slot in addition to the model's per-leg
 * metadata. When the current ACTIVE leg departs after the network has already
 * promoted another MT leg to ACTIVE/HELD, legacy clears that flat slot. Preserve
 * the survivor's number for the terminal journal and any future app-side
 * consumer, but retire its compatibility ownership. If an INCOMING/WAITING
 * episode still exists,
 * its CLI must remain visible so the call can be re-presented as ringing. */
static void cm_retire_promoted_compat_cli(call_model_t *m,
                                          const call_leg_t *departing) {
    if (m->pending_mt.active) return;

    call_leg_t *survivor_owner = NULL;
    for (unsigned i = 0u; i < MODEM_MAX_CALL_LEGS; i++) {
        call_leg_t *leg = &m->legs[i];
        if (!leg->in_use || leg == departing) continue;
        if (leg->state == CALL_LEG_INCOMING || leg->state == CALL_LEG_WAITING) {
            return;
        }
        if (leg->mt_cli_valid &&
            (leg->state == CALL_LEG_ACTIVE || leg->state == CALL_LEG_HELD)) {
            survivor_owner = leg;
        }
    }
    if (survivor_owner != NULL) {
        cm_retire_compat_cli_owner(m, survivor_owner);
    }
}

/* Bind episode-scoped operations before pending-MT tracking consumes an ACTIVE
 * observation. This covers the pre-ID race where ATA/CHLD is sent before the
 * first id-bearing indication and that first indication is already ACTIVE.
 * PENDING operations are included so their write-time guard can later return
 * ALREADY_SATISFIED against the exact episode participant. */
static void cm_bind_episode_observation(call_model_t *m, const call_leg_t *leg) {
    if (!m->pending_mt.active) return;

    bool active_mt = leg->state == CALL_LEG_ACTIVE && leg->dir != CALL_DIR_MO;
    bool consumed_active = false;
    for (unsigned i = 0; i < MODEM_MAX_CALL_TRANSACTIONS; i++) {
        call_txn_t *t = &m->txns[i];
        if (!t->in_use || t->target_id != 0u || t->mt_episode == 0u ||
            t->mt_episode != m->pending_mt.generation) {
            continue;
        }

        bool match = false;
        if (t->is_tombstone) {
            /* Cancelling an unbound answer does not reject a still-ringing call.
             * It does own an ACTIVE leg if the already-written ATA connected it. */
            match = (t->kind == CALL_TXN_ANSWER || t->kind == CALL_TXN_WAIT_ANSWER) &&
                    active_mt && !txn_baseline_contains(t, leg->id, leg->generation);
        } else if (cm_txn_is_open(t)) {
            switch (t->kind) {
            case CALL_TXN_ANSWER:
                match = leg->state == CALL_LEG_INCOMING ||
                        (active_mt && !txn_baseline_contains(t, leg->id, leg->generation));
                break;
            case CALL_TXN_WAIT_ANSWER:
                match = leg->state == CALL_LEG_WAITING ||
                        (active_mt && !txn_baseline_contains(t, leg->id, leg->generation));
                break;
            case CALL_TXN_WAIT_REJECT:
                match = leg->state == CALL_LEG_WAITING;
                break;
            case CALL_TXN_HANGUP:
                match = leg->state == CALL_LEG_INCOMING || leg->state == CALL_LEG_WAITING ||
                        active_mt;
                break;
            default:
                break;
            }
        }
        if (!match) continue;

        t->target_id = leg->id;
        t->target_gen = leg->generation;
        if (leg->state == CALL_LEG_ACTIVE) consumed_active = true;
    }

    if (consumed_active) {
        /* Let cm_pending_mt_track perform the normal bound-ACTIVE clear. */
        m->pending_mt.bound_id = leg->id;
        m->pending_mt.bound_gen = leg->generation;
        m->pending_mt.bound_valid = true;
    }
}

/* §9.4 pending-MT episode tracking for ONE leg's current state — shared by on_event
 * (URC) and the CLCC merge: an INCOMING/WAITING leg binds the pre-id episode;
 * the bound leg reaching ACTIVE (answered) clears it so waiting/ring drop. Removal of
 * a bound leg is handled by cm_unbind_pending_mt in the eviction paths. */
static void cm_pending_mt_copy_metadata(call_model_t *m, call_leg_t *leg) {
    if (!m->pending_mt.active) return;
    /* Keep the episode identity after pending_mt is consumed on ACTIVE. The
     * application snapshot can then prove that a former waiting caller is the
     * same leg that survived a foreground release, even when all state changes
     * were reconciled inside one service tick. */
    leg->mt_episode = m->pending_mt.generation;
    if (m->pending_mt.alert_observed) {
        /* The legacy flat slot belongs to the newest presented MT episode. Do
         * not allow an older answered/held MT leg to reappear after it departs. */
        for (unsigned i = 0; i < MODEM_MAX_CALL_LEGS; i++) {
            if (&m->legs[i] != leg) m->legs[i].mt_cli_valid = false;
        }
        memcpy(leg->number, m->pending_mt.number, sizeof leg->number);
        leg->meta_valid = true;
        leg->cli_withheld = (m->pending_mt.cli_validity != 0u);
        leg->mt_cli_valid = true;
    }
    leg->incoming_diverted = m->pending_mt.incoming_diverted;
    /* A participant bound to a pending-MT episode is authoritatively MT even
     * when its first id-bearing observation was already ACTIVE and therefore
     * carried no direction in the vendor URC. */
    leg->dir = CALL_DIR_MT;
}

static void cm_pending_mt_track(call_model_t *m, call_leg_t *leg) {
    if (leg->state == CALL_LEG_INCOMING || leg->state == CALL_LEG_WAITING) {
        if (m->pending_mt.active) {
            cm_pending_mt_copy_metadata(m, leg);
            m->pending_mt.bound_id = leg->id;
            m->pending_mt.bound_gen = leg->generation;
            m->pending_mt.bound_valid = true;
        }
    } else if (leg->state == CALL_LEG_ACTIVE) {
        if (m->pending_mt.bound_valid && m->pending_mt.bound_id == leg->id &&
            m->pending_mt.bound_gen == leg->generation) {
            cm_pending_mt_copy_metadata(m, leg);
            m->pending_mt.active = false;
            m->pending_mt.bound_valid = false;
        }
    }
}

/* +CCWA and a vendor id-bearing WAITING event are independent URCs, so either
 * wire order is legal. If the id-bearing leg arrived first, attach the later
 * pre-id metadata immediately when exactly one compatible leg exists. */
static void cm_pending_mt_bind_existing(call_model_t *m) {
    if (!m->pending_mt.active || m->pending_mt.bound_valid) return;
    call_leg_t *candidate = NULL;
    for (unsigned i = 0u; i < MODEM_MAX_CALL_LEGS; i++) {
        call_leg_t *leg = &m->legs[i];
        if (!leg->in_use || cm_leg_is_transient(leg) || leg->dir == CALL_DIR_MO ||
            (leg->state != CALL_LEG_INCOMING && leg->state != CALL_LEG_WAITING)) {
            continue;
        }
        if (candidate != NULL) return; /* ambiguous: wait for authoritative CLCC */
        candidate = leg;
    }
    if (candidate == NULL) return;
    cm_pending_mt_copy_metadata(m, candidate);
    m->pending_mt.bound_id = candidate->id;
    m->pending_mt.bound_gen = candidate->generation;
    m->pending_mt.bound_valid = true;
}

/* Evict a leg (positive removal). Bumps gen_next[id] past this leg's generation so a
 * later reuse of the id is disambiguated (§8); clears a pending-MT binding that pointed
 * at it (§9.4); a structural change. */
static void cm_remove_leg(call_model_t *m, call_leg_t *L) {
    cm_touch_shadow(m);                /* §16.8: a leg-table mutation invalidates an open shadow */
    cm_retire_compat_cli_owner(m, L);
    cm_unbind_pending_mt(m, L->id, L->generation);
    /* §8: record the next generation for this id BEFORE the slot is recycled, so a
     * later reuse of the id — even in a slot that gets overwritten for another id —
     * is disambiguated (gen_next survives; the freed slot's fields do not). */
    m->gen_next[L->id] = cm_generation_after(L->generation);
    L->in_use = false;
    L->state = CALL_LEG_UNKNOWN;
    L->pending_removal = false;
    m->semantic_revision++;
}

/* Queue a generation-qualified teardown intent once — the SOLE cleanup-enqueue
 * helper. It selects plain id-scoped release or the neutral release-foreground/
 * recover-held operation; the vendor adapter still owns its wire encoding. */
static void cm_queue_release(call_model_t *m, uint8_t id, uint8_t gen,
                             bool recover_held_survivor) {
    if (id == 0u) { return; }
    for (unsigned i = 0; i < m->release_count; i++) {
        call_cleanup_release_t *queued = &m->release_queue[i];
        if (queued->id == id && queued->generation == gen) {
            /* The stronger semantic wins if two paths converge on one leg. */
            queued->recover_held_survivor |= recover_held_survivor;
            return;
        }
    }
    if (m->release_count < MODEM_MAX_CALL_LEGS) {
        m->release_queue[m->release_count++] = (call_cleanup_release_t){
            .id = id,
            .generation = gen,
            .recover_held_survivor = recover_held_survivor,
        };
    }
}

/* §7.1 hard-abandonment: convert a bound setup leg to a confirmed teardown — the
 * SOLE abandon helper (the app abandon path AND the tick deadline path both call it;
 * the CALLER sets t->state and t->result). Marks the bound leg RELEASING (the
 * projector RETAINS its role until authoritative absence) and queues its
 * generation-qualified cleanup target. RELEASING is reserved for THIS path only. */
static void cm_abandon_leg(call_model_t *m, call_txn_t *t) {
    /* Resolve the leg via the op's PARTICIPANT (cm_txn_owner_ref:
     * target_id for a target op, bound_id for a DIAL), NOT bound_id alone — else an
     * ANSWER / WAIT_REJECT / RELEASE_* abandon (all target_id-bound) finds no leg and
     * releases nothing (§16.5/§16.11). */
    uint8_t oid, ogen;
    cm_txn_owner_ref(t, &oid, &ogen);
    call_leg_t *L = (oid != 0u) ? cm_find_leg(m, oid, ogen) : NULL;
    if (L != NULL) {
        cm_touch_shadow(m);                 /* §5.4: this leg mutation races any open CLCC */
        if (L->state != CALL_LEG_RELEASING) {
            L->state = CALL_LEG_RELEASING;
            m->semantic_revision++;
        }
        if (!L->pending_removal) {          /* arm the §5.3 limbo backstop (stamp once) so a */
            L->pending_removal = true;      /* release URC that never arrives still bounds the */
            /* The limbo-evict gate keys on first_removal_ms != 0, so an
             * abandon at now==0 (32-bit ms wrap, or the
             * first ms after boot) must NOT stamp 0 or the backstop is defeated for
             * this leg. Mirror the CLCC sibling in clcc_ok_commit. */
            L->first_removal_ms = (m->now_ms != 0u) ? m->now_ms : 1u;
        }
        /* §16.5: releasing a pending-MT-bound INCOMING/WAITING leg (a wedged ANSWER /
         * WAIT_REJECT / WAIT_ANSWER) clears the episode so the incoming UI does not stay
         * RINGING/waiting behind a torn-down leg ("never returns to RINGING indefinitely").
         * A no-op for a DIAL leg (pending-MT is never bound to an MO setup leg). */
        cm_unbind_pending_mt(m, L->id, L->generation);
        bool recover_held_survivor =
            t->kind == CALL_TXN_RELEASE_ACTIVE ||
            (t->kind == CALL_TXN_DIAL && t->second_mo);
        cm_queue_release(m, L->id, L->generation,
                         recover_held_survivor);
    }
}

/* §16.4/§16.5 per-operation RESULT SINK — where a transaction's owned terminal
 * result is routed. Making the latch-ownership enumeration structural is what
 * lets a rejected HOLD/SWAP/WAIT_REJECT (a supplementary-service failure) NEVER
 * write last_call_result: only DIAL/ANSWER setup attempts and actual primary-call
 * terminal transitions do. */
typedef enum { RESULT_SINK_PRIMARY_LATCH = 0, RESULT_SINK_SECOND_MO_LATCH,
               RESULT_SINK_JOURNAL_ONLY, RESULT_SINK_OUTPUT_FREE } call_result_sink_t;

/* §16.5 TOTAL per-operation participant descriptor — resolution epoch + result sink,
 * built from explicit strategy enums with NO default fall-through: every op kind has a
 * described epoch, so -Wswitch (compile) AND test_op_descriptor_totality (runtime) both
 * flag any un-described op. This is the single source of both participant timing and
 * §16.4 latch ownership; adding an op cannot silently inherit OUTPUT_FREE routing.
 *   AT_ADMISSION        : existing-leg participants captured when the request is admitted
 *                         (HOLD, SWAP, RELEASE_ACTIVE, RELEASE_LEG, HANGUP).
 *   AT_DISPATCH         : the DIAL new-active creation baseline, captured at the actual
 *                         write (call_model_txn_dispatched), NOT at admission.
 *   PENDING_MT_EPISODE  : bound to the current incoming episode + its 16.6 generation
 *                         (ANSWER, WAIT_ANSWER, WAIT_REJECT). */
typedef enum { EPOCH_UNSET = 0, EPOCH_AT_ADMISSION, EPOCH_AT_DISPATCH,
               EPOCH_PENDING_MT_EPISODE } call_resolution_epoch_t;
typedef struct { call_resolution_epoch_t epoch; call_result_sink_t sink; } call_op_descriptor_t;

static call_op_descriptor_t cm_op_descriptor(const call_txn_t *t) {
    call_op_descriptor_t d = { EPOCH_UNSET, RESULT_SINK_OUTPUT_FREE };
    switch (t->kind) {
        case CALL_TXN_DIAL:
            d.epoch = EPOCH_AT_DISPATCH;
            d.sink = t->second_mo ? RESULT_SINK_SECOND_MO_LATCH
                                  : RESULT_SINK_PRIMARY_LATCH;
            break;
        case CALL_TXN_ANSWER:
            d.epoch = EPOCH_PENDING_MT_EPISODE;
            d.sink = t->second_mo ? RESULT_SINK_SECOND_MO_LATCH
                                  : RESULT_SINK_PRIMARY_LATCH;
            break;
        case CALL_TXN_WAIT_ANSWER:
        case CALL_TXN_WAIT_REJECT:    d.epoch = EPOCH_PENDING_MT_EPISODE; break;
        case CALL_TXN_HOLD:
        case CALL_TXN_SWAP:
        case CALL_TXN_RELEASE_ACTIVE:
        case CALL_TXN_RELEASE_LEG:
        case CALL_TXN_HANGUP:         d.epoch = EPOCH_AT_ADMISSION;       break;
        case CALL_TXN_NONE:           break;   /* sentinel kind: intentionally EPOCH_UNSET */
    }
    return d;
}

static const call_txn_t *cm_hangup_setup_txn(const call_model_t *m,
                                              const call_txn_t *hangup,
                                              unsigned setup_index) {
    if (setup_index >= hangup->hangup_setup_count ||
        setup_index >= MODEM_HANGUP_SETUP_OWNERS) {
        return NULL;
    }
    uint32_t token = hangup->hangup_setup_tokens[setup_index];
    if (token == 0u) return NULL;
    for (unsigned i = 0u; i < MODEM_MAX_CALL_TRANSACTIONS; i++) {
        const call_txn_t *candidate = &m->txns[i];
        if (candidate->in_use && candidate->kind == CALL_TXN_DIAL &&
            candidate->token == token) {
            return candidate;
        }
    }
    return NULL;
}

static bool cm_hangup_owns_leg(const call_model_t *m, const call_txn_t *t,
                               uint8_t id, uint8_t gen) {
    if ((t->target_id == id && t->target_gen == gen) ||
        txn_baseline_contains(t, id, gen)) {
        return true;
    }
    for (unsigned i = 0u; i < t->hangup_setup_count &&
                               i < MODEM_HANGUP_SETUP_OWNERS; i++) {
        const call_txn_t *dial = cm_hangup_setup_txn(m, t, i);
        if (dial != NULL && dial->bound_id == id && dial->bound_gen == gen) {
            return true;
        }
    }
    return false;
}

static bool cm_hangup_has_live_obligation(const call_model_t *m, const call_txn_t *t) {
    for (unsigned i = 0; i < MODEM_MAX_CALL_LEGS; i++) {
        const call_leg_t *l = &m->legs[i];
        if (l->in_use && cm_hangup_owns_leg(m, t, l->id, l->generation)) return true;
    }
    /* ATD crossed the UART before HANGUP admission, but no id-bearing leg has
     * appeared yet. The exact DIAL token is still an owned physical setup
     * obligation; a later unrelated DIAL has a different token and cannot make
     * this predicate true. Once it binds, the leg-owned branch above takes over. */
    for (unsigned i = 0u; i < t->hangup_setup_count &&
                               i < MODEM_HANGUP_SETUP_OWNERS; i++) {
        const call_txn_t *dial = cm_hangup_setup_txn(m, t, i);
        if (dial != NULL && dial->ever_dispatched && cm_txn_is_open(dial) &&
            dial->bound_id == 0u) {
            return true;
        }
    }
    return t->mt_episode != 0u && m->pending_mt.active &&
           m->pending_mt.generation == t->mt_episode;
}

/* §16.4: write a terminal result to its owning latch NOW (explicit transition).
 * NONE is a no-op; JOURNAL_ONLY / OUTPUT_FREE write no latch. */
static void cm_route_result(call_model_t *m, call_result_sink_t sink, modem_call_result_t r) {
    if (r == MODEM_CALL_RESULT_NONE) { return; }
    switch (sink) {
        case RESULT_SINK_PRIMARY_LATCH:   m->published.last_call_result   = r; break;
        case RESULT_SINK_SECOND_MO_LATCH: m->published.second_call_result = r; break;
        case RESULT_SINK_JOURNAL_ONLY:
        case RESULT_SINK_OUTPUT_FREE:     break;
    }
}

/* §16.4: is this departing leg's teardown accounted for by a LOCAL teardown
 * transaction (HANGUP session-wide, or a RELEASE_ACTIVE / RELEASE_LEG / WAIT_REJECT
 * targeting it)? A still-open (in-flight) or SUCCEEDED such txn means the user hung
 * up locally — a CLEAN local hangup in the Phase-2 journal, distinct from a
 * remote failure. The Phase-1 flat compatibility latch is handled separately
 * in cm_latch_terminal. Per "the transaction's own outcome" (§16.4), a
 * CANCELLED **or FAILED** teardown did NOT take effect (e.g. a network-rejected
 * targeted/session release that lingers in_use until lazy reclaim): it must NOT mask a
 * later GENUINE remote release, which stays NO_CARRIER. */
static bool cm_leg_local_teardown(const call_model_t *m, uint8_t id, uint8_t gen) {
    for (unsigned i = 0; i < MODEM_MAX_CALL_TRANSACTIONS; i++) {
        const call_txn_t *t = &m->txns[i];
        if (!t->in_use || t->state == TXN_CANCELLED || t->state == TXN_FAILED) { continue; }
        switch (t->kind) {
            case CALL_TXN_HANGUP:
                if (cm_hangup_owns_leg(m, t, id, gen)) return true;
                break;
            case CALL_TXN_RELEASE_ACTIVE:
            case CALL_TXN_RELEASE_LEG:
            case CALL_TXN_WAIT_REJECT:
                if (t->target_id == id &&
                    (t->target_gen == gen || t->target_gen == 0u)) { return true; }
                break;
            default: break;
        }
    }
    return false;
}

/* §16.4/§9.3: does a genuine ACTIVE/HELD leg OTHER than the departing one survive?
 * If so, the CALL continues (e.g. a held leg about to be promoted), so the
 * departing leg's clean removal must NOT latch a terminal NO_CARRIER. */
static bool cm_any_surviving_active_or_held(const call_model_t *m, uint8_t id, uint8_t gen) {
    for (unsigned i = 0; i < MODEM_MAX_CALL_LEGS; i++) {
        const call_leg_t *l = &m->legs[i];
        if (!l->in_use || (l->id == id && l->generation == gen)) { continue; }
        if (l->state == CALL_LEG_ACTIVE || l->state == CALL_LEG_HELD) { return true; }
    }
    return false;
}

/* Route a setup transaction's terminal result without overwriting the state of a
 * genuinely surviving foreground call. The second-call latch is independent, so
 * it always receives its owning setup result. */
static void cm_route_setup_result(call_model_t *m, const call_txn_t *t,
                                  modem_call_result_t result) {
    call_result_sink_t sink = cm_op_descriptor(t).sink;
    if (sink == RESULT_SINK_PRIMARY_LATCH &&
        cm_any_surviving_active_or_held(m, 0u, 0u)) {
        return;
    }
    cm_route_result(m, sink, result);
}

/* §16.4: the terminal result of a PRIMARY (non-2nd-MO) leg that departed with no
 * explicit cause — distinguishing a clean local hangup / continuing call
 * (NONE in the Phase-2 journal) from a REMOTE release of a connected call
 * (NO_CARRIER).
 * Fixes the probed "remote ACTIVE release retained CONNECTED": a connected leg
 * that vanishes with NO owning local teardown txn and NO surviving call leg was
 * dropped by the far end -> NO_CARRIER. A never-connected leg leaves NONE (its
 * setup DIAL/ANSWER txn owns that outcome, §16.3). */
static modem_call_result_t cm_primary_departure_result(const call_model_t *m,
                                                        const call_leg_t *leg) {
    if (!leg->was_answered) { return MODEM_CALL_RESULT_NONE; }
    if (cm_leg_local_teardown(m, leg->id, leg->generation)) { return MODEM_CALL_RESULT_NONE; }
    if (cm_any_surviving_active_or_held(m, leg->id, leg->generation)) { return MODEM_CALL_RESULT_NONE; }
    return MODEM_CALL_RESULT_NO_CARRIER;              /* remote drop of a connected call */
}

/* §16.4/§16.5 terminal-result transition. Routes the departing leg's owned result
 * to its latch NOW (explicit transition — cm_route_result), THEN records the
 * departure in the bounded journal for Phase-2 side effects (the projector drains
 * the journal but no longer DERIVES latches from it, §16.4). Ownership is EXPLICIT:
 * is_second_mo from generation-qualified 2nd-MO DIAL ownership -> second_call_result,
 * else last_call_result.
 *
 * Result determination stamps the ACTUAL outcome (BUSY stays BUSY):
 *   - the explicit r (a recorded bare-final cause) wins when non-NONE;
 *   - else the owning 2nd-MO txn's resolved result (e.g. a recorded 2nd-MO BUSY);
 *   - else, for a 2nd-MO leg with nothing recorded (lost only via CLCC), synthesize
 *     brief-connected (NO_CARRIER) / never-answered (NO_ANSWER);
 *   - else (primary, no cause) the §16.4 local-vs-remote journal distinguisher
 *     (cm_primary_departure_result): clean local hangup / continuing call -> NONE;
 *     remote release of a connected call -> NO_CARRIER.
 *
 * The Phase-1 flat latch has a separate compatibility rule pinned by the
 * recorded A1/A4/A5/A7/A8/A9 traces: when no foreground leg survives and no specific
 * terminal cause exists, modem_status_t publishes NO_CARRIER for both local and
 * remote teardown. This must not rewrite the richer journal result. */
static void cm_latch_terminal(call_model_t *m, const call_leg_t *leg, modem_call_result_t r) {
    if (leg == NULL) { return; }

    bool is_second = false;
    bool owner_tombstone = false;
    bool owner_output_resolved = false;
    uint32_t token = 0u;                     /* the leg's creating txn token if resolvable */
    modem_call_result_t owner_result = MODEM_CALL_RESULT_NONE;
    for (unsigned i = 0; i < MODEM_MAX_CALL_TRANSACTIONS; i++) {
        const call_txn_t *t = &m->txns[i];
        uint8_t owner_id = 0u, owner_gen = 0u;
        if (t->in_use && (t->kind == CALL_TXN_DIAL || t->kind == CALL_TXN_ANSWER)) {
            cm_txn_owner_ref(t, &owner_id, &owner_gen);
        }
        if (owner_id == leg->id && owner_gen == leg->generation) {
            token = t->token;
            if (t->is_tombstone) owner_tombstone = true;
            owner_output_resolved = t->output_resolved;
            if (t->second_mo) {
                is_second = true;
                owner_result = t->result;
            } else if (t->is_tombstone) {
                owner_result = t->result;
            }
        }
    }

    /* §9.5: second_call_result is only ever NONE or a terminal
     * FAILURE, never CONNECTED. owner_result can be a stale SUCCEEDED DIAL txn's
     * CONNECTED result (the txn resolved when the 2nd-MO leg connected and is
     * lazily-reclaimed, so it is still in_use at this later, clean departure) --
     * exclude it so a connected-then-departed 2nd leg is NOT journaled CONNECTED;
     * fall through to the synthesized brief-connected/never-answered outcome. */
    bool foreground_survives =
        cm_any_surviving_active_or_held(m, leg->id, leg->generation);
    /* A successful New-call leg is setup-owned by second_call_result only while
     * the original foreground call survives. If that original has already
     * departed, the connected second-MO leg is now the sole primary call and
     * its eventual teardown owns last_call_result. Keep the immutable txn
     * history; change only the departure sink. */
    bool second_became_primary =
        is_second && owner_output_resolved && leg->was_answered &&
        !foreground_survives;
    bool local_teardown = !owner_tombstone &&
                          cm_leg_local_teardown(m, leg->id, leg->generation);
    modem_call_result_t result = local_teardown ? MODEM_CALL_RESULT_NONE
                                : (r != MODEM_CALL_RESULT_NONE) ? r
                                : (owner_result != MODEM_CALL_RESULT_NONE &&
                                   owner_result != MODEM_CALL_RESULT_CONNECTED) ? owner_result
                                : is_second ? (leg->was_answered ? MODEM_CALL_RESULT_NO_CARRIER
                                                                 : MODEM_CALL_RESULT_NO_ANSWER)
                                : cm_primary_departure_result(m, leg);   /* §16.4 local-vs-remote */
    /* §16.4: route to the owning latch NOW (explicit transition), independent of
     * the journal. The leg-departure sink is 2nd-MO -> second, else primary.
     * When no foreground leg survives, translate a semantically clean local
     * teardown (journal NONE) to the legacy flat latch's NO_CARRIER. Do not do
     * this for a second-MO leg or a cancelled owner. */
    modem_call_result_t projected_result = result;
    if (projected_result == MODEM_CALL_RESULT_NONE &&
        (!is_second || second_became_primary) && !owner_tombstone &&
        !foreground_survives &&
        (m->published.last_call_result == MODEM_CALL_RESULT_NONE ||
         m->published.last_call_result == MODEM_CALL_RESULT_CONNECTED)) {
        projected_result = MODEM_CALL_RESULT_NO_CARRIER;
    }
    /* second_call_result describes setup only. Once the owning setup already
     * published success/failure, a later physical departure is journal evidence,
     * not another write into a newer setup attempt's latch epoch. */
    if (projected_result != MODEM_CALL_RESULT_NONE && !owner_tombstone &&
        (!(is_second && owner_output_resolved) || second_became_primary)) {
        call_result_sink_t sink =
            (is_second && !second_became_primary) ? RESULT_SINK_SECOND_MO_LATCH
                                                  : RESULT_SINK_PRIMARY_LATCH;
        cm_route_result(m, sink, projected_result);
    }

    if (m->journal_count >= MODEM_MAX_CALL_LEGS) {
        m->journal_incomplete = true;                             /* bounded — drop + flag */
        return;
    }
    call_terminal_t *e = &m->journal[m->journal_count++];
    e->in_use       = true;
    e->id           = leg->id;
    e->generation   = leg->generation;
    e->dir          = leg->dir;
    e->was_answered = leg->was_answered;
    e->is_second_mo = is_second;
    e->result       = result;
    e->token        = token;
    memcpy(e->number, leg->number, sizeof e->number);
}

void call_model_on_event(call_model_t *m, uint8_t id, bool id_valid,
                         call_leg_state_t st, call_direction_t dir) {
    if (m == NULL) {
        return;
    }
    if (!id_valid) {
        /* LTE boundary: a coarse call observation with NO reliable id. Never create or
         * mutate a leg by an unreliable id; instead invalidate any open shadow (§5.4)
         * and schedule a CLCC to recover the true structure (cm_raise_reason edge). */
        if (m->shadow_open) { m->shadow_invalidated = true; }
        cm_raise_reason(m, (uint32_t)CALL_RC_COARSE_FINAL);
        return;
    }
    if (id == 0u || id > MODEM_MAX_CALL_LEGS) {
        return;   /* id indexes gen_next[MODEM_MAX_CALL_LEGS+1]; reject out-of-range ids */
    }
    if (m->shadow_open) { m->shadow_invalidated = true; }  /* §5.4: a URC interleaved an open CLCC */
    /* SETUP_DONE (stat 7) and BUSY normalize to UNKNOWN — table no-ops (§4.1). */
    if (st == CALL_LEG_UNKNOWN) {
        return;
    }

    /* §5.1/§5.3: an ID-scoped RELEASED event is positive removal evidence -
     * evict the leg. Latch the terminal result (§9.5) from the copied snapshot
     * BEFORE eviction; then if the removed leg was ACTIVE and a HELD leg remains,
     * PROMOTE the held leg to ACTIVE (§9.3). A bare final never reaches here (§5.5);
     * a leg the abandon path marked RELEASING is finally evicted here when its own
     * release URC arrives. */
    if (st == CALL_LEG_RELEASING) {
        call_leg_t *L = cm_find_leg_by_id(m, id);
        if (L == NULL) { return; }                 /* already gone */
        call_leg_t snapshot = *L;                  /* readable copy for the latch */
        bool was_active = (L->state == CALL_LEG_ACTIVE);
        /* Pass the recorded bare-final cause (pending_terminal; NONE for a clean
         * local hangup) as the explicit result. cm_latch_terminal owns ownership
         * (is_second_mo) AND the 2nd-MO fallback (BUSY stays BUSY; brief-connected vs
         * NO_ANSWER when nothing was recorded) — no local override heuristic here.
         * Only THIS leg's OWN attributed cause is taken (and
         * consumed) — a non-owning eviction must neither inherit a lingering
         * owner's cause nor strip it out from under that owner's later eviction. */
        modem_call_result_t cause = cm_take_pending_terminal(m, snapshot.id, snapshot.generation);
        cm_latch_terminal(m, &snapshot, cause);
        if (was_active) {
            cm_retire_promoted_compat_cli(m, L);
        }
        cm_remove_leg(m, L);
        if (was_active) {
            for (unsigned i = 0; i < MODEM_MAX_CALL_LEGS; i++) {
                if (m->legs[i].in_use && m->legs[i].state == CALL_LEG_HELD) {
                    m->legs[i].state = CALL_LEG_ACTIVE;         /* promote (§9.3) */
                    m->published.last_call_result = MODEM_CALL_RESULT_CONNECTED;
                    m->semantic_revision++;
                    break;
                }
            }
        }
        cm_raise_reason(m, (uint32_t)CALL_RC_COARSE_FINAL);
        /* Reconcile transactions before returning — a 2nd-MO
         * New-call DIAL/ANSWER txn bound to THIS just-released leg must be
         * resolved (FAILED, its leg gone) in the SAME step, or its stale
         * "still in setup" state keeps dominating call_state (R5 New-call
         * DIALING) over the true surviving-leg projection (e.g. sole-HELD +
         * call_on_hold) until some LATER event happens to run the engine. */
        model_reconcile_txns(m);
        return;
    }

    bool structural = false;
    call_leg_t *L = cm_find_leg_by_id(m, id);
    if (L == NULL) {
        L = cm_alloc_leg(m, id);
        if (L == NULL) {
            return;                 /* overflow already flagged in cm_alloc_leg */
        }
        structural = true;          /* a new leg is a structural change */
    }

    /* §16.7: a leg already RELEASING (cm_abandon_leg's hard-teardown
     * marker from a post-dispatch cancel/abandon) must NEVER be resurrected by a
     * late live-state URC (a genuine connect-then-cancel race). This mirrors the
     * CLCC merge's `!leg_releasing` guard on the URC path — RELEASING is retired
     * ONLY by genuine absence (the id-scoped release) or the §5.3 limbo timeout,
     * never flipped back to a live state. Without it, a late ACTIVE event for a
     * cancelled leg overwrites the state below and the cancelled call projects as
     * live for the whole limbo window (~8 s). */
    bool leg_releasing = (L->state == CALL_LEG_RELEASING);
    if (!leg_releasing) {
        /* A naming URC un-masks a pending_removal leg's role (recompute in the
         * projector) but must NOT reset its CLCC-absence (limbo) clock — only a clean
         * CLCC reappearance does that (§5.3). The leg still needs a CLCC confirm. */
        if (L->pending_removal) {
            L->pending_removal = false;                 /* first_removal_ms kept */
            cm_raise_reason(m, (uint32_t)CALL_RC_PENDING_REMOVAL);
        }

        if (L->state != st) {
            L->state = st;
            structural = true;          /* a state transition is structural */
        }
        if (dir != CALL_DIR_UNKNOWN) {
            L->dir = dir;               /* a known direction refines; UNKNOWN never clobbers */
        }
        L->last_seen_ms = m->now_ms;    /* metadata only — never bumps the revision */

        /* Bind episode operations before ACTIVE clears the episode. */
        cm_bind_episode_observation(m, L);

        /* §9.4 pending-MT binding (shared with the CLCC merge). */
        cm_pending_mt_track(m, L);

        /* §9.5 last_call_result latch: any leg entering ACTIVE (connect, swap-in, or the
         * network retrieving the held original) latches CONNECTED and clears the pending
         * terminal cause. */
        if (st == CALL_LEG_ACTIVE) {
            L->was_answered = true;                          /* journal: this leg connected (§11) */
            m->published.last_call_result = MODEM_CALL_RESULT_CONNECTED;
            cm_clear_pending_terminal(m, L->id, L->generation);   /* owner-scoped pending-terminal clear */
        }
    }

    if (structural) {
        m->semantic_revision++;
    }

    /* An id-bearing URC changed the table -> immediately re-bind / re-resolve any
     * pending transactions (the transaction engine). The pending_terminal
     * clear above is best-effort
     * (§5.5 — CLCC/COARSE_FINAL is the authoritative teardown signal; an exotic
     * two-leg pending-cause race is within that envelope). */
    model_reconcile_txns(m);
}

/* §16.6: hand out the next monotonic pending-MT episode generation. Survives episode
 * clears (never reuses an earlier generation), never 0 (0 = episode-agnostic). */
static uint32_t cm_alloc_episode_gen(call_model_t *m) {
    uint32_t g = m->mt_episode_next++;
    if (m->mt_episode_next == 0u) m->mt_episode_next = 1u;   /* wrap: skip 0 */
    if (g == 0u) { g = m->mt_episode_next++; }               /* never hand out 0 */
    return g;
}

void call_model_on_ring(call_model_t *m) {
    if (m == NULL) {
        return;
    }
    if (m->shadow_open) { m->shadow_invalidated = true; }  /* §5.4: a URC interleaved an open CLCC */
    call_pending_mt_t *p = &m->pending_mt;
    if (!p->active) {
        /* Transition INTO a fresh episode: the ONLY place incoming_number is cleared
         * (§9.4). RING carries no CLI -> default valid; +CLIP refines it. A fresh episode
         * gets a NEW §16.6 generation so a stale op bound to the prior one cannot bind it. */
        memset(p, 0, sizeof(*p));
        p->active = true;
        p->generation = cm_alloc_episode_gen(m);
        p->cli_validity = 0u;                       /* 0 = valid, not "withheld" */
        p->first_ring_ms = m->now_ms;
        m->reconcile_reasons |= (uint32_t)CALL_RC_PENDING_MT;
    }
    p->alert_observed = true;
    p->last_ring_ms = m->now_ms;                     /* repeats update the SAME episode */
    /* §5.6: a RING mid-episode is a live sign the call is still
     * ringing — it must invalidate any already-armed "two clean absent CLCC
     * snapshots" streak against THIS episode, else a still-ringing incoming that
     * happens to RING between the two clean snapshots gets prematurely dismissed
     * as stale. */
    p->clcc_absent_snapshots = 0u;

    /* §9.5: any ring indication resets last_call_result to NONE (idempotent; harmless
     * even as a call-waiting ring over a live call, §9.4) and clears any stale primary
     * terminal cause (a new call attempt). second_call_result is orthogonal and
     * deliberately untouched. */
    m->published.last_call_result = MODEM_CALL_RESULT_NONE;
    cm_clear_pending_terminal(m, 0u, 0u);   /* id-blind pending-terminal observation */
}

void call_model_on_clip(call_model_t *m, const char *number, uint8_t cli_validity) {
    if (m == NULL) {
        return;
    }
    if (m->shadow_open) { m->shadow_invalidated = true; }  /* §5.4: a URC interleaved an open CLCC */
    call_pending_mt_t *p = &m->pending_mt;
    if (!p->active) {
        /* A CLIP with no prior RING marks an incoming episode, but it PROVIDES the
         * number (set below) rather than clearing it, so it never enters through the
         * number-clearing fresh-episode path (§9.4). A fresh episode gets a new §16.6
         * generation. */
        memset(p, 0, sizeof(*p));
        p->active = true;
        p->generation = cm_alloc_episode_gen(m);
        p->first_ring_ms = m->now_ms;
        m->reconcile_reasons |= (uint32_t)CALL_RC_PENDING_MT;
    }
    p->alert_observed = true;
    /* +CLIP always refreshes number + CLI validity, even during ANSWERING (§9.4). */
    if (number != NULL) {
        size_t n = strlen(number);
        if (n > MODEM_PHONE_MAX) {
            n = MODEM_PHONE_MAX;
        }
        memcpy(p->number, number, n);
        p->number[n] = '\0';
    } else {
        p->number[0] = '\0';
    }
    p->cli_validity = cli_validity;
    p->last_ring_ms = m->now_ms;
    p->clcc_absent_snapshots = 0u;   /* §5.6: same rigor as on_ring */

    /* A CLIP is an incoming indication -> same last_call_result/pending_terminal reset
     * as RING (§9.5). */
    m->published.last_call_result = MODEM_CALL_RESULT_NONE;
    cm_clear_pending_terminal(m, 0u, 0u);   /* id-blind pending-terminal observation */
    cm_pending_mt_bind_existing(m);
}

void call_model_on_incoming_diverted(call_model_t *m) {
    if (m == NULL) return;
    if (m->shadow_open) m->shadow_invalidated = true;

    call_pending_mt_t *p = &m->pending_mt;
    if (!p->active) {
        /* CSSU identifies an incoming episode but does not itself authorize the
         * ringing UI. RING/CLIP or an id-bearing INCOMING/WAITING leg does that. */
        memset(p, 0, sizeof(*p));
        p->active = true;
        p->generation = cm_alloc_episode_gen(m);
        p->cli_validity = 0u;
        p->first_ring_ms = m->now_ms;
        p->last_ring_ms = m->now_ms;
        m->reconcile_reasons |= (uint32_t)CALL_RC_PENDING_MT;
    }
    p->incoming_diverted = true;
    p->diverted_observed_ms = m->now_ms;
    p->clcc_absent_snapshots = 0u;
    cm_pending_mt_bind_existing(m);
}

/* The leg reference a txn is "about", for pending_terminal ownership
 * attribution AND the abandon/cleanup leg-resolution. DIAL uses its
 * bound_id/gen (§7.4 binding); every TARGET op (ANSWER / WAIT_ANSWER / WAIT_REJECT /
 * RELEASE_ACTIVE / RELEASE_LEG) references its participant via target_id/gen — so a
 * target_id-bound op's hard-abandon can actually release its INCOMING/WAITING/target
 * leg (§16.5/§16.11), the exact defect the participant-resolution rule
 * prevents. HOLD/SWAP toggle/exchange a LIVE
 * pre-existing leg they do NOT own the lifetime of (an abandon must never tear the live
 * call down), and HANGUP is session-wide — all three own no single leg, so they stay
 * unattributed (0). on_bare_final's cause-attribution only ever passes DIAL/ANSWER/
 * HANGUP/RELEASE_* here (it excludes CHLD swap/wait ops), so widening WAIT_ANSWER/
 * WAIT_REJECT to target_id does not change cause attribution. */
static void cm_txn_owner_ref(const call_txn_t *t, uint8_t *id_out, uint8_t *gen_out) {
    switch (t->kind) {
        case CALL_TXN_DIAL:
            *id_out = t->bound_id; *gen_out = t->bound_gen; return;
        case CALL_TXN_ANSWER:
        case CALL_TXN_WAIT_ANSWER:
        case CALL_TXN_WAIT_REJECT:
        case CALL_TXN_RELEASE_ACTIVE:
        case CALL_TXN_RELEASE_LEG:
            *id_out = t->target_id; *gen_out = t->target_gen; return;
        default:
            *id_out = 0u; *gen_out = 0u; return;   /* HOLD/SWAP/HANGUP: no single owned leg */
    }
}

void call_model_on_bare_final(call_model_t *m, modem_call_result_t r,
                              bool in_supplementary_window) {
    if (m == NULL) {
        return;
    }
    if (m->shadow_open) { m->shadow_invalidated = true; }  /* §5.4: a URC interleaved an open CLCC */
    /* A bare CONNECT is a positive connect signal, not a departure: latch CONNECTED
     * and do NOT arm a coarse-final reconcile (§9.5). A connect also clears any stale
     * primary terminal cause (contract: pending_terminal cleared on connect/dial/ring). */
    if (r == MODEM_CALL_RESULT_CONNECTED) {
        m->published.last_call_result = MODEM_CALL_RESULT_CONNECTED;
        cm_clear_pending_terminal(m, 0u, 0u);   /* id-blind pending-terminal observation */
        return;
    }

    /* A bare terminal final NEVER removes a leg (§5.5) -> force a CLCC to decide which
     * leg (if any) departed. */
    cm_raise_reason(m, (uint32_t)CALL_RC_COARSE_FINAL);

    /* Inside a supplementary-operation window the final belongs to the released
     * waiting/held leg, not the primary call -> do not attribute it to a transaction;
     * CLCC (COARSE_FINAL)
     * resolves it (§5.5). */
    if (in_supplementary_window) {
        return;
    }

    /* §16.2/§16.3: attribute the provisional cause to a transaction ONLY when
     * EXACTLY ONE eligible open setup/teardown owner exists — NEVER "the newest
     * numeric token". With 0 or >=2 eligible owners it stays uncorrelated
     * (unattributed register) and CLCC resolves which leg (if any) departed.
     * (Computed BEFORE writing the register so an unattributable final cannot
     * clobber a live-owned one — see the else-branch below.) */
    call_txn_t *match = NULL;
    unsigned eligible = 0u;
    for (unsigned i = 0; i < MODEM_MAX_CALL_TRANSACTIONS; i++) {
        call_txn_t *t = &m->txns[i];
        /* [T6 Minor-1] skip free / not-yet-dispatched (PENDING) / already-terminal.
         * Requiring state >= TXN_DISPATCHED is essential: flipping a PENDING txn to
         * UNCERTAIN would leave it OPEN with unarmed (0) deadlines -> txn_deadlines
         * never abandons it -> a backstop-less open txn breaks the §6 bounded drain. */
        if (!t->in_use || t->state < TXN_DISPATCHED || t->state >= TXN_SUCCEEDED) {
            continue;
        }
        switch (t->kind) {
            case CALL_TXN_DIAL:
            case CALL_TXN_ANSWER:
            case CALL_TXN_HANGUP:
            case CALL_TXN_RELEASE_ACTIVE:
            case CALL_TXN_RELEASE_LEG:
                break;                                   /* setup/teardown ops absorb it */
            default:
                continue;                                /* CHLD swap/wait ops excluded */
        }
        eligible++;
        match = t;
    }
    if (eligible != 1u) {
        match = NULL;                                    /* §16.2: never guess with 0 or >=2 owners */
    }

    /* §16.3: an id-blind bare final is NON-AUTHORITATIVE — it records a PROVISIONAL
     * cause register (carried until a clean CLCC proves no leg exists (§16.3 no-leg
     * resolution) or a leg eviction consumes it) and triggers CLCC. A remote release of
     * an established call may have NO open transaction, so the register survives
     * independently. */
    if (match != NULL) {
        /* attributable: this cause becomes the register, owned by the matched txn's
         * TOKEN even when it has NOT bound a leg yet (owner_id==0 for an unbound 2nd-MO
         * DIAL), so a foreign leg's later eviction cannot steal it (§16.4). */
        m->pending_terminal = r;
        match->result = r;                               /* provisional; CLCC decides SUCCEEDED/FAILED */
        match->state = TXN_UNCERTAIN;
        cm_txn_owner_ref(match, &m->pending_terminal_owner_id, &m->pending_terminal_owner_gen);
        m->pending_terminal_owner_token = match->token;
    } else if (!cm_pending_terminal_has_live_owner(m)) {
        /* unattributable (0/>=2 eligible owners) AND no live-owned register to protect:
         * §16.3 ownerless provisional register — CLCC resolves which leg (if any) left. */
        m->pending_terminal = r;
        m->pending_terminal_owner_id = 0u;
        m->pending_terminal_owner_gen = 0u;
        m->pending_terminal_owner_token = 0u;   /* truly unattributed (0/≥2 eligible owners) */
    }
    /* else (§16.4): an unattributable final must NOT clobber a register
     * still owned by a LIVE setup txn — that owned cause is authoritative and is consumed
     * only by its owner's own leg eviction / §16.3 no-leg CLCC resolution. Overwriting it
     * here mis-latched the call log (e.g. NO_CARRIER over a real BUSY) when a transient
     * 2nd eligible owner made the second bare final unattributable. Drop this final. */
}

/* ==========================================================================
 * Transaction lifecycle engine — §7. The canonical cm_* helpers
 * are defined above; the abandon/release path reuses cm_abandon_leg /
 * cm_queue_release (no local duplicate). model_reconcile_txns (forward-declared
 * with the leg-table helpers, hooked at the end of call_model_on_event) is the single
 * per-tick/per-event engine; clcc_ok and the tick call it too.
 * ========================================================================== */

/* §16.12: the txn policy/abandon deadlines come from m->timing (POLICY = the
 * fast-CLCC window before an unresolved op backs off; ABANDON = ~2x max GSM call
 * setup, the hard finiteness backstop so a bound setup leg can never outlive its
 * transaction). No baked #define here. */
#define MODEM_TXN_HANGUP_SLOT  0u    /* reserved: HANGUP is guaranteed this slot */
/* §16.7: the model-side dispatch watchdog is REMOVED. A never-dispatched PENDING
 * txn (its allocate-then-enqueue failed after the allocation, §7.3) is NOT
 * force-cancelled by a timer here — cancellation is queue-owned (the integration
 * calls call_model_txn_cancel with the evicted token; Stage 2 wires it fully).
 * A PENDING txn is still excluded from cm_txn_in_flight (it never suppresses the
 * whole-model CLCC keepalive) and does not arm any deadline until dispatch. */

static bool txn_deadline_passed(uint32_t now, uint32_t deadline) {
    return (int32_t)(now - deadline) >= 0;    /* ms-wrap safe (recurring bug class) */
}
static bool txn_is_terminal(const call_txn_t *t) {
    return t->state == TXN_SUCCEEDED || t->state == TXN_FAILED || t->state == TXN_CANCELLED;
}
static call_txn_t *txn_by_token(call_model_t *m, uint32_t token) {
    if (token == 0u) return NULL;
    for (unsigned i = 0; i < MODEM_MAX_CALL_TRANSACTIONS; i++)
        if (m->txns[i].in_use && m->txns[i].token == token) return &m->txns[i];
    return NULL;
}

/* §16.1: is `tok` currently held by a live record? For Stage 1 the only live
 * records are the in_use transactions (Stage 2 adds tombstones — this predicate
 * is where those get folded in). */
static bool cm_token_live(const call_model_t *m, uint32_t tok) {
    for (unsigned i = 0; i < MODEM_MAX_CALL_TRANSACTIONS; i++)
        if (m->txns[i].in_use && m->txns[i].token == tok) return true;
    return false;
}

/* §16.1: allocate the next token monotonically over the full 32-bit space, with
 * COLLISION AVOIDANCE among live records (skip any value already held by a live
 * txn / tombstone). Token 0 is reserved "none". Wrap-safe: after 2^32 allocations
 * the counter wraps through 0 (skipped) and any live collision is stepped over.
 * Bounded: at most MODEM_MAX_CALL_TRANSACTIONS live tokens exist, so a free value
 * is found within that many steps — the caller has already ensured a free SLOT. */
static uint32_t cm_alloc_token(call_model_t *m) {
    for (unsigned guard = 0; guard <= MODEM_MAX_CALL_TRANSACTIONS + 1u; guard++) {
        uint32_t cand = m->next_token++;
        if (m->next_token == 0u) m->next_token = 1u;   /* keep 0 reserved for "none" */
        if (cand == 0u) continue;                      /* never hand out 0 */
        if (!cm_token_live(m, cand)) return cand;      /* collision-free among live records */
    }
    return m->next_token++;    /* unreachable: >MAX_TXN live tokens cannot exist */
}
/* The generation-qualified finder is the canonical `cm_find_leg` (defined once in
 * the leg-table section). The transaction engine reuses it — no local
 * duplicate (was `txn_find_leg`). */
static call_leg_t *txn_find_role(call_model_t *m, call_leg_state_t st) {
    for (unsigned i = 0; i < MODEM_MAX_CALL_LEGS; i++) {
        call_leg_t *l = &m->legs[i];
        if (l->in_use && l->state == st) return l;
    }
    return NULL;
}

bool call_model_hold_toggle_available(const call_model_t *m) {
    if (m == NULL || m->pending_mt.active) return false;

    const call_leg_t *sole = NULL;
    for (unsigned i = 0u; i < MODEM_MAX_CALL_LEGS; i++) {
        const call_leg_t *leg = &m->legs[i];
        if (!leg->in_use) continue;
        if (sole != NULL) return false;
        sole = leg;
    }
    return sole != NULL && !sole->pending_removal &&
           (sole->state == CALL_LEG_ACTIVE || sole->state == CALL_LEG_HELD);
}

static void cm_capture_baseline(const call_model_t *m, call_txn_t *t) {
    t->baseline_count = 0u;
    for (unsigned i = 0; i < MODEM_MAX_CALL_LEGS; i++) {
        if (!m->legs[i].in_use || t->baseline_count >= MODEM_MAX_CALL_LEGS) continue;
        t->baseline_ids[t->baseline_count] = m->legs[i].id;
        t->baseline_gens[t->baseline_count] = m->legs[i].generation;
        t->baseline_count++;
    }
}

static void cm_capture_hangup_setups(call_model_t *m, call_txn_t *hangup) {
    hangup->hangup_setup_count = 0u;
    for (unsigned i = 0u; i < MODEM_MAX_CALL_TRANSACTIONS; i++) {
        call_txn_t *dial = &m->txns[i];
        if (dial == hangup || !dial->in_use || dial->kind != CALL_TXN_DIAL ||
            !cm_txn_is_open(dial)) {
            continue;
        }

        /* A DIAL that has not crossed the UART has no physical setup to hang
         * up. Cancel its model token now; the still-queued/deferred request will
         * fail its mandatory dispatch guard instead of dialing after the user
         * has already returned to standby. */
        if (!dial->ever_dispatched) {
            (void)cm_cancel_txn(m, dial);
            continue;
        }

        /* A currently bound leg is captured by the ordinary admission
         * baseline below. Tokens are needed only for an ATD whose leg can be
         * revealed after HANGUP admission. Admission permits at most one open
         * primary and one open second-MO setup. */
        if (dial->bound_id != 0u &&
            cm_find_leg(m, dial->bound_id, dial->bound_gen) != NULL) {
            continue;
        }
        if (hangup->hangup_setup_count < MODEM_HANGUP_SETUP_OWNERS) {
            hangup->hangup_setup_tokens[hangup->hangup_setup_count++] = dial->token;
        }
    }
}

uint32_t call_model_request(call_model_t *m, call_txn_kind_t kind,
                            uint8_t target_id, bool second_mo) {
    if (m == NULL) return 0u;
    switch (kind) {
        case CALL_TXN_NONE:
            return 0u;
        case CALL_TXN_DIAL:
        case CALL_TXN_ANSWER:
        case CALL_TXN_HOLD:
        case CALL_TXN_SWAP:
        case CALL_TXN_WAIT_ANSWER:
        case CALL_TXN_WAIT_REJECT:
        case CALL_TXN_RELEASE_ACTIVE:
        case CALL_TXN_RELEASE_LEG:
        case CALL_TXN_HANGUP:
            break;
    }
    if (kind == CALL_TXN_DIAL || kind == CALL_TXN_HANGUP) {
        target_id = 0u; /* these operations own a creation baseline / whole session */
    } else if (target_id > MODEM_MAX_CALL_LEGS) {
        return 0u;
    }

    call_txn_t *slot = NULL;

    /* A role has one setup-attempt owner at a time. A new dial can supersede an
     * older UNBOUND attempt (the user redialled after a lost final), but it must
     * never overlap a same-role attempt that already owns a live leg. The old
     * dispatched attempt becomes a tombstone, so its genuinely-late leg remains
     * catchable without claiming the fresh attempt's leg or latch epoch. */
    if (kind == CALL_TXN_DIAL) {
        bool has_old = false;
        bool supersession_frees_slot = false;
        for (unsigned i = 0; i < MODEM_MAX_CALL_TRANSACTIONS; i++) {
            call_txn_t *old = &m->txns[i];
            if (!old->in_use || old->kind != CALL_TXN_DIAL ||
                old->second_mo != second_mo || !cm_txn_is_open(old)) {
                continue;
            }
            has_old = true;
            if (!old->ever_dispatched) supersession_frees_slot = true;
            if (old->bound_id != 0u &&
                cm_find_leg(m, old->bound_id, old->bound_gen) != NULL) {
                return 0u;
            }
        }
        if (has_old && !supersession_frees_slot) {
            bool capacity = false;
            for (unsigned i = MODEM_TXN_HANGUP_SLOT + 1u;
                 i < MODEM_MAX_CALL_TRANSACTIONS; i++) {
                if (!m->txns[i].in_use || cm_txn_retirement_eligible(m, &m->txns[i])) {
                    capacity = true;
                    break;
                }
            }
            if (!capacity) return 0u;  /* admission failure must not cancel the live attempt */
        }
        for (unsigned i = 0; i < MODEM_MAX_CALL_TRANSACTIONS; i++) {
            call_txn_t *old = &m->txns[i];
            if (!old->in_use || old->kind != CALL_TXN_DIAL ||
                old->second_mo != second_mo || !cm_txn_is_open(old)) {
                continue;
            }
            cm_cancel_txn(m, old);
        }
    }
    /* §16.9: lazy slot-reclaim is gated on RETIREMENT eligibility — it may free a
     * terminal txn only when both axes are resolved, so it can never reclaim a
     * tombstone whose cleanup is still outstanding (its late leg unreleased) or a
     * SUCCEEDED DIAL still owning a live leg (2nd-MO ownership for the departure latch). */
    if (kind == CALL_TXN_HANGUP) {
        call_txn_t *r = &m->txns[MODEM_TXN_HANGUP_SLOT];
        if (r->in_use && cm_txn_retirement_eligible(m, r)) memset(r, 0, sizeof(*r));  /* lazy reclaim */
        if (!r->in_use) slot = r;
    } else {
        /* Reclaim retirement-eligible general slots lazily, then take the first free one. */
        for (unsigned i = MODEM_TXN_HANGUP_SLOT + 1u; i < MODEM_MAX_CALL_TRANSACTIONS; i++) {
            if (m->txns[i].in_use && cm_txn_retirement_eligible(m, &m->txns[i]))
                memset(&m->txns[i], 0, sizeof(m->txns[i]));
            if (slot == NULL && !m->txns[i].in_use) slot = &m->txns[i];
        }
    }
    if (slot == NULL) return 0u;    /* rejected: no slot (atomic admission, §7.3) */

    /* Clear the slot FIRST so it holds no live token, THEN allocate collision-free
     * among the remaining live records (§16.1 — slot->token==0 can't self-collide). */
    memset(slot, 0, sizeof(*slot));
    uint32_t token = cm_alloc_token(m);
    slot->in_use   = true;
    slot->token    = token;
    slot->kind     = kind;
    slot->state    = TXN_PENDING;
    slot->second_mo = second_mo;
    slot->result   = MODEM_CALL_RESULT_NONE;
    /* §9.5/§16.9 fresh-dial ACCEPT reset: an accepted DIAL already projects
     * DIALING, even while it is queued behind transport wake or another command.
     * Its owning result latch must therefore start the new episode at NONE at
     * ACCEPT, not only when the bytes eventually dispatch. Otherwise calls_app
     * arms on the pending DIALING projection and mistakes the previous episode's
     * retained terminal result for this attempt's failure.
     *
     * The reset is asymmetric by role. A 2nd-MO (New-call) clears only its
     * second_call_result latch and preserves the live primary's CONNECTED result.
     * A primary DIAL clears both stale result latches and an id-blind provisional
     * terminal cause. Dispatch repeats the same reset idempotently at the UART
     * boundary. */
    if (kind == CALL_TXN_DIAL) {
        m->published.second_call_result = MODEM_CALL_RESULT_NONE;
        if (!second_mo) {
            m->published.last_call_result = MODEM_CALL_RESULT_NONE;
            cm_clear_pending_terminal(m, 0u, 0u);
        }
    }
    /* §16.7: NO dispatch watchdog stamp. A PENDING txn arms no deadline until
     * call_model_txn_dispatched; the integration owns cancelling an un-enqueued
     * txn (call_model_txn_cancel). abandon_deadline_ms stays 0 (unarmed) so the
     * txn_deadlines pass leaves the PENDING txn alone. */

    /* Resolve the target participant, generation-qualified (§7.2). A target_id
     * of 0 means "the op's canonical live participant" (the public request API issues most
     * call-control ops with target 0): ANSWER -> the incoming/ringing leg; HOLD /
     * SWAP / RELEASE_ACTIVE -> the active leg; WAIT_ANSWER / WAIT_REJECT -> the
     * waiting leg. HANGUP is session-wide and DIAL binds via its baseline, so both
     * keep target 0. Only a genuinely-absent participant leaves target 0 (and then
     * the op legitimately fails its postcondition) instead of the old blind
     * cm_find_leg(0,0)==NULL -> immediate FAIL. */
    uint8_t resolved_target = target_id;
    uint8_t resolved_gen = 0u;
    if (target_id != 0u) {
        for (unsigned i = 0; i < MODEM_MAX_CALL_LEGS; i++)
            if (m->legs[i].in_use && m->legs[i].id == target_id) {
                resolved_gen = m->legs[i].generation; break;
            }
    } else if (kind == CALL_TXN_HOLD) {
        /* §16.5 HOLD(0) is a hold-state TOGGLE on the SOLE leg — resolve it whether ACTIVE
         * or HELD (retrieve a sole HELD leg instead of failing when there is no ACTIVE). */
        call_leg_t *cand = txn_find_role(m, CALL_LEG_ACTIVE);
        if (cand == NULL) cand = txn_find_role(m, CALL_LEG_HELD);
        if (cand != NULL) { resolved_target = cand->id; resolved_gen = cand->generation; }
    } else {
        call_leg_state_t want = CALL_LEG_UNKNOWN;
        switch (kind) {
            case CALL_TXN_ANSWER:         want = CALL_LEG_INCOMING; break;
            case CALL_TXN_SWAP:
            case CALL_TXN_RELEASE_ACTIVE: want = CALL_LEG_ACTIVE;   break;
            case CALL_TXN_WAIT_ANSWER:
            case CALL_TXN_WAIT_REJECT:    want = CALL_LEG_WAITING;  break;
            default: break;   /* DIAL / HANGUP / RELEASE_LEG: no canonical target-0 leg */
        }
        if (want != CALL_LEG_UNKNOWN) {
            call_leg_t *cand = txn_find_role(m, want);
            if (cand != NULL) { resolved_target = cand->id; resolved_gen = cand->generation; }
        }
    }
    slot->target_id = resolved_target;
    slot->target_gen = resolved_gen;

    if (kind == CALL_TXN_HANGUP && m->pending_mt.bound_valid) {
        slot->target_id = m->pending_mt.bound_id;
        slot->target_gen = m->pending_mt.bound_gen;
    }

    if (kind == CALL_TXN_SWAP || kind == CALL_TXN_RELEASE_ACTIVE) {
        call_leg_t *h = txn_find_role(m, CALL_LEG_HELD);      /* the held participant */
        if (h) { slot->held_id = h->id; slot->held_gen = h->generation; }
    } else if (kind == CALL_TXN_WAIT_ANSWER) {
        call_leg_t *a = txn_find_role(m, CALL_LEG_ACTIVE);    /* prior active -> held */
        if (a) { slot->held_id = a->id; slot->held_gen = a->generation; }
    }

    /* §16.6: an episode-bound op (ANSWER / WAIT_ANSWER / WAIT_REJECT / HANGUP) records
     * the CURRENT incoming-episode generation at admission so a stale op admitted against
     * episode N can never bind a later episode N+1 (a new incoming after N cleared). 0 =
     * no episode at admission; only an explicitly target-bound op remains valid in that
     * case. An unbound op must never attach itself to a future episode. */
    switch (kind) {
        case CALL_TXN_ANSWER:
        case CALL_TXN_WAIT_ANSWER:
        case CALL_TXN_WAIT_REJECT:
        case CALL_TXN_HANGUP:
            slot->mt_episode = m->pending_mt.active ? m->pending_mt.generation : 0u;
            break;
        default: break;
    }
    if (kind == CALL_TXN_HANGUP) {
        cm_capture_hangup_setups(m, slot);  /* pre-id ATD participants by token */
        cm_capture_baseline(m, slot);       /* id-bearing participants at admission */
    }
    return token;
}

void call_model_txn_dispatched(call_model_t *m, uint32_t token) {
    call_txn_t *t = txn_by_token(m, token);
    if (t == NULL || txn_is_terminal(t)) return;
    /* §16.5 resolution epoch: DIAL is the ONLY op whose participants (its new-active
     * creation baseline) are captured AT_DISPATCH — every other op captured them at
     * admission (AT_ADMISSION) or binds the pending-MT episode (PENDING_MT_EPISODE). */
    if (cm_op_descriptor(t).epoch == EPOCH_AT_DISPATCH) {
        /* creation_baseline = {id,generation} of every leg present at dispatch. */
        cm_capture_baseline(m, t);
        /* §9.5: a fresh outgoing DIAL clears the stale published
         * result latches + bare-final cause so the new attempt does not surface the
         * PREVIOUS call's outcome (mirrors the on_ring incoming-indication reset).
         * second_call_result ALWAYS resets (it is the New-call/2nd-MO owning latch, so
         * every fresh 2nd-MO attempt starts clean) -- but last_call_result/
         * pending_terminal reset ONLY for a PRIMARY dial: a 2nd-MO dial issued over a
         * live CONNECTED original must LEAVE last_call_result at CONNECTED (the
         * corrected §9.5 :1829/:2041 case), not blank the primary's outcome out from
         * under it while its DIAL is merely in setup. */
        m->published.second_call_result = MODEM_CALL_RESULT_NONE;
        if (!t->second_mo) {
            m->published.last_call_result = MODEM_CALL_RESULT_NONE;
            cm_clear_pending_terminal(m, 0u, 0u);   /* id-blind pending-terminal observation */
        }
    } else if (t->kind == CALL_TXN_ANSWER || t->kind == CALL_TXN_WAIT_ANSWER) {
        /* A pre-ID constructive incoming op needs a dispatch baseline too: an
         * ACTIVE leg first seen after the write is bindable, while a pre-existing
         * foreground leg is not. */
        cm_capture_baseline(m, t);
    }
    if (t->kind == CALL_TXN_HOLD || t->kind == CALL_TXN_SWAP) {
        /* Role participants stay admission-bound, but a toggle's postcondition is
         * relative to the state at the real write. This prevents a queued second
         * press from completing on the first press's role transition. */
        call_leg_t *l = cm_find_leg(m, t->target_id, t->target_gen);
        t->baseline_target_held = (l != NULL && l->state == CALL_LEG_HELD);
    }
    /* [T6 Minor-1] never-stamp-zero: 0 is the "unarmed" sentinel that txn_deadlines
     * keys on, so an armed deadline must never BE 0 (a dispatch at the 32-bit ms wrap
     * boundary could compute now+INTERVAL==0 and be silently skipped forever). Stamp 1
     * instead; zero is reserved for "not armed", the same invariant as the
     * CLCC limbo clock. This makes the `!= 0u` armed check robust. */
    t->policy_deadline_ms  = m->now_ms + m->timing.txn_policy_ms;
    if (t->policy_deadline_ms == 0u)  t->policy_deadline_ms = 1u;
    t->abandon_deadline_ms = m->now_ms + m->timing.txn_abandon_ms;
    if (t->abandon_deadline_ms == 0u) t->abandon_deadline_ms = 1u;
    t->ever_dispatched = true;    /* §16.7: the real AT bytes go out HERE (immutable, never clears) */
    t->state = TXN_DISPATCHED;
}

void call_model_txn_command_result(call_model_t *m, uint32_t token, modem_cmd_result_t r) {
    call_txn_t *t = txn_by_token(m, token);
    if (t == NULL || txn_is_terminal(t)) return;
    switch (r.status) {
        case CMD_OK:
            /* §16.2: folds in the old txn_accepted — OK = accepted, NOT
             * outcome-known (§7.1); the §7.2 postcondition still governs SUCCEEDED. */
            t->at_accepted = true;
            break;
        case CMD_ERROR: {
            /* §16.2: a correlated rejection TERMINATES the txn carrying its owned
             * result, releasing the in-flight gate — NOT waiting on the policy timer.
             * The result is routed by the op's §16.5 sink, so a rejected HOLD/SWAP/
             * WAIT_REJECT (OUTPUT_FREE) writes NO latch. */
            cm_touch_shadow(m);                       /* §16.8: a call-affecting transition */
            modem_call_result_t cause = r.has_call_result ? r.call_result
                                                          : MODEM_CALL_RESULT_NO_CARRIER;
            t->result = cause;
            cm_route_result(m, cm_op_descriptor(t).sink, cause);
            t->output_resolved = true;                /* §16.9 axis 1: cause published */
            t->state = TXN_FAILED;
            cm_clear_pending_terminal_for_token(m, t->token);
            model_reconcile_txns(m);
            break;
        }
        case CMD_TIMEOUT:
            /* §16.2: UNCERTAIN, still bindable, edge-pull an immediate CLCC. This is
             * NOT cancellation (§16.7) — no tombstone; the later hard-abandon is the
             * finiteness backstop. */
            cm_touch_shadow(m);                       /* §16.8 */
            t->state = TXN_UNCERTAIN;
            cm_raise_reason(m, (uint32_t)CALL_RC_UNCERTAIN_TXN);
            m->next_clcc_ms = m->now_ms;              /* immediate CLCC */
            model_reconcile_txns(m);
            break;
    }
}

/* §16.5: resolve a txn from the dispatch guard WITHOUT sending — SUCCEEDED for an
 * ALREADY_SATISFIED postcondition, FAILED for a STALE (vanished) target. Both mark the
 * output axis resolved (no user-facing result to publish here — a suppressed write; the
 * projection reflects the live table). Touches the shadow (§16.8: a call-affecting
 * transition). */
static call_dispatch_guard_t cm_guard_resolve(call_model_t *m, call_txn_t *t,
                                              call_dispatch_guard_t verdict) {
    if (verdict == CALL_DISPATCH_ALREADY_SATISFIED) { t->state = TXN_SUCCEEDED; }
    else if (verdict == CALL_DISPATCH_STALE)        { t->state = TXN_FAILED; }
    t->output_resolved = true;
    t->cleanup_confirmed = true;  /* no bytes were written, so no physical cleanup exists */
    cm_clear_pending_terminal_for_token(m, t->token);
    cm_touch_shadow(m);
    return verdict;
}

call_dispatch_guard_t call_model_txn_dispatch_guard(call_model_t *m, uint32_t token) {
    call_txn_t *t = txn_by_token(m, token);
    if (t == NULL || txn_is_terminal(t)) return CALL_DISPATCH_STALE;   /* the op is already gone */

    switch (t->kind) {
    case CALL_TXN_DIAL:
        /* AT_DISPATCH: the baseline is captured at the write itself — nothing to stale. */
        return CALL_DISPATCH_SEND;

    case CALL_TXN_ANSWER: {
        /* Constructive + episode-bound: STALE if the incoming target/episode vanished;
         * ALREADY_SATISFIED if it is already ACTIVE (the call connected first). */
        if (t->target_id != 0u) {
            call_leg_t *l = cm_find_leg(m, t->target_id, t->target_gen);
            if (l == NULL) return cm_guard_resolve(m, t, CALL_DISPATCH_STALE);
            if (l->state == CALL_LEG_ACTIVE) return cm_guard_resolve(m, t, CALL_DISPATCH_ALREADY_SATISFIED);
            return CALL_DISPATCH_SEND;
        }
        /* pre-ID: STALE if the episode is gone OR replaced by a later one (§16.6). */
        if (!m->pending_mt.active || cm_txn_episode_stale(m, t))
            return cm_guard_resolve(m, t, CALL_DISPATCH_STALE);
        return CALL_DISPATCH_SEND;
    }

    case CALL_TXN_HOLD: {
        /* A coarse hold toggle is safe only while the captured leg is still
         * the sole call. Recheck the complete topology at the actual transport
         * boundary: a waiting/held leg may have arrived while this request sat
         * behind another command. There is no ALREADY_SATISFIED result because
         * a toggle has no fixed target state. */
        call_leg_t *target = cm_find_leg(m, t->target_id, t->target_gen);
        if (target == NULL ||
            call_model_new_call_cleanup_pending(m, NULL) ||
            !call_model_hold_toggle_available(m))
            return cm_guard_resolve(m, t, CALL_DISPATCH_STALE);
        return CALL_DISPATCH_SEND;
    }

    case CALL_TXN_SWAP: {
        /* Needs both {active, held} still present; STALE otherwise. */
        if (call_model_new_call_cleanup_pending(m, NULL) ||
            cm_find_leg(m, t->target_id, t->target_gen) == NULL ||
            cm_find_leg(m, t->held_id, t->held_gen) == NULL)
            return cm_guard_resolve(m, t, CALL_DISPATCH_STALE);
        return CALL_DISPATCH_SEND;
    }

    case CALL_TXN_WAIT_ANSWER: {
        /* Constructive: needs the waiting/incoming target still present. */
        if (call_model_new_call_cleanup_pending(m, NULL))
            return cm_guard_resolve(m, t, CALL_DISPATCH_STALE);
        if (t->target_id != 0u) {
            if (cm_find_leg(m, t->target_id, t->target_gen) == NULL)
                return cm_guard_resolve(m, t, CALL_DISPATCH_STALE);
            return CALL_DISPATCH_SEND;
        }
        if (!m->pending_mt.active || cm_txn_episode_stale(m, t))   /* §16.6 replaced/gone episode */
            return cm_guard_resolve(m, t, CALL_DISPATCH_STALE);
        return CALL_DISPATCH_SEND;
    }

    case CALL_TXN_WAIT_REJECT: {
        /* Destructive: ALREADY_SATISFIED if the waiting leg is already gone (goal met). */
        if (t->target_id != 0u) {
            if (cm_find_leg(m, t->target_id, t->target_gen) == NULL)
                return cm_guard_resolve(m, t, CALL_DISPATCH_ALREADY_SATISFIED);
            return CALL_DISPATCH_SEND;
        }
        /* pre-ID: the episode is the target (§16.5 MANDATORY guard —
         * totality): mirror the ANSWER and WAIT_ANSWER dispatch cases + the
         * resolver — the reject's target waiting call is gone when there is
         * no live episode OR the live
         * episode is a REPLACED (later) one this reject was not admitted against (§16.6),
         * so it resolves ALREADY_SATISFIED (goal met, do NOT physically reject a different,
         * live call N+1). The old `!active && no WAITING leg` omitted the episode-stale
         * check, so a WAIT_REJECT queued behind an SMS could reject episode N+1.
         * A normal live/non-stale episode still SENDs. */
        if (!m->pending_mt.active || cm_txn_episode_stale(m, t))
            return cm_guard_resolve(m, t, CALL_DISPATCH_ALREADY_SATISFIED);
        return CALL_DISPATCH_SEND;
    }

    case CALL_TXN_RELEASE_ACTIVE:
    case CALL_TXN_RELEASE_LEG: {
        /* Destructive: ALREADY_SATISFIED if the target leg is already absent. */
        if (t->target_id == 0u || cm_find_leg(m, t->target_id, t->target_gen) == NULL)
            return cm_guard_resolve(m, t, CALL_DISPATCH_ALREADY_SATISFIED);
        return CALL_DISPATCH_SEND;
    }

    case CALL_TXN_HANGUP: {
        /* Session-wide for the participants captured at admission, plus the
         * pending-MT episode that was visible then. A later unrelated call is
         * not silently absorbed by this queued command. */
        if (cm_hangup_has_live_obligation(m, t)) return CALL_DISPATCH_SEND;
        return cm_guard_resolve(m, t, CALL_DISPATCH_ALREADY_SATISFIED);
    }

    case CALL_TXN_NONE:
        break;
    }
    return CALL_DISPATCH_SEND;   /* unreachable for a well-formed txn */
}

/* Generation-qualified baseline membership. Comparing id ONLY would treat a
 * reused id (freed then re-alloc'd at a new generation) as an old baseline leg, so the
 * DIAL would never bind to the genuinely-new leg. */
static bool txn_baseline_contains(const call_txn_t *t, uint8_t id, uint8_t gen) {
    for (unsigned i = 0; i < t->baseline_count; i++)
        if (t->baseline_ids[i] == id && t->baseline_gens[i] == gen) return true;
    return false;
}

/* §7.4: bind a dispatched/uncertain DIAL to a new-outside-baseline leg. */
static void txn_bind_dials(call_model_t *m) {
    for (unsigned ti = 0; ti < MODEM_MAX_CALL_TRANSACTIONS; ti++) {
        call_txn_t *t = &m->txns[ti];
        if (!t->in_use || t->kind != CALL_TXN_DIAL) continue;
        if (t->state != TXN_DISPATCHED && t->state != TXN_UNCERTAIN) continue;
        if (t->bound_id != 0u) continue;

        call_leg_t *cand = NULL; unsigned n = 0u;
        for (unsigned li = 0; li < MODEM_MAX_CALL_LEGS; li++) {
            call_leg_t *l = &m->legs[li];
            if (!l->in_use) continue;
            if (txn_baseline_contains(t, l->id, l->generation)) continue;
            if (cm_leg_owned_by_other_txn(m, t, l->id, l->generation)) continue;
            if (l->dir == CALL_DIR_MT) continue;              /* a coincident incoming, not us */
            if (l->state != CALL_LEG_DIALING && l->state != CALL_LEG_ALERTING &&
                l->state != CALL_LEG_ACTIVE) continue;
            cand = l; n++;
        }
        if (n == 0u) continue;
        if (n >= 2u) { cm_raise_reason(m, (uint32_t)CALL_RC_UNCERTAIN_TXN); continue; } /* never guess */

        t->bound_id = cand->id; t->bound_gen = cand->generation;
        t->state = TXN_BOUND;
        if (cand->state == CALL_LEG_ACTIVE) {
            if (cand->dir == CALL_DIR_MO) {                   /* lost-DIALING recovery (#5) */
                t->dir_confirmed = true; t->witnessed_active = true;
            } else {                                          /* first seen ACTIVE, no dir */
                t->dir_confirmed = false;                     /* tentative: hold until CLCC MO */
                cm_raise_reason(m, (uint32_t)CALL_RC_UNCERTAIN_TXN);
            }
        } else {
            t->dir_confirmed = true;                          /* DIALING/ALERTING imply MO */
        }
    }
}

/* §7.4: once CLCC supplies <dir> for a tentative (dir-unconfirmed) DIAL, confirm
 * (MO) or un-bind + reclassify (MT). */
static void txn_confirm_tentative(call_model_t *m) {
    for (unsigned ti = 0; ti < MODEM_MAX_CALL_TRANSACTIONS; ti++) {
        call_txn_t *t = &m->txns[ti];
        if (!t->in_use || t->kind != CALL_TXN_DIAL) continue;
        if (t->bound_id == 0u || t->dir_confirmed) continue;
        call_leg_t *l = cm_find_leg(m, t->bound_id, t->bound_gen);
        if (l == NULL) continue;                              /* absence handled in resolve */
        if (l->dir == CALL_DIR_MO) {
            t->dir_confirmed = true;
            if (l->state == CALL_LEG_ACTIVE) t->witnessed_active = true;
        } else if (l->dir == CALL_DIR_MT) {                   /* not our dial */
            t->bound_id = 0u; t->bound_gen = 0u;
            t->state = TXN_UNCERTAIN;
            cm_raise_reason(m, (uint32_t)CALL_RC_UNCERTAIN_TXN);
        }
    }
}

/* §7.4-style unbound wait: bind an unresolved (target_id==0)
 * ANSWER/WAIT_ANSWER once its awaited role materializes — mirrors txn_bind_dials
 * for DIAL. call_model_request only resolves target 0 against an ALREADY-
 * materialized leg (§7.2); the realistic "user presses Answer off the RINGING UI
 * before its ID-scoped event lands" race needs a rebind once the leg appears.
 * Meanwhile txn_resolve's ANSWER case holds the op pending (not failed) while a
 * pending-MT episode could still materialize the leg. */
/* §16.6: an episode-bound op is stale if the CURRENT incoming episode is not the one it
 * was admitted against (a new incoming replaced the one it targeted). A zero episode is
 * valid only when admission captured an explicit id-qualified target; an unbound operation
 * admitted with no episode cannot acquire a later caller. */
static bool cm_txn_episode_stale(const call_model_t *m, const call_txn_t *t) {
    if (t->mt_episode == 0u) return t->target_id == 0u;
    return !m->pending_mt.active || m->pending_mt.generation != t->mt_episode;
}

static void txn_bind_targets(call_model_t *m) {
    for (unsigned ti = 0; ti < MODEM_MAX_CALL_TRANSACTIONS; ti++) {
        call_txn_t *t = &m->txns[ti];
        if (!t->in_use || t->target_id != 0u) continue;
        if (t->state != TXN_DISPATCHED && t->state != TXN_UNCERTAIN) continue;
        call_leg_state_t want;
        if (t->kind == CALL_TXN_ANSWER) want = CALL_LEG_INCOMING;
        /* §16.5: WAIT_REJECT rebinds to a late WAITING leg too (mirrors
         * WAIT_ANSWER) so a pre-ID reject is never left orphaned/unrejected — its own
         * reject semantics are preserved by the txn_resolve WAIT_REJECT case; this only
         * gives it a target once the waiting leg materializes. */
        else if (t->kind == CALL_TXN_WAIT_ANSWER || t->kind == CALL_TXN_WAIT_REJECT) want = CALL_LEG_WAITING;
        else continue;
        /* §16.6: never bind a LATER episode's leg — a stale ANSWER/WAIT_ANSWER/WAIT_REJECT
         * admitted against episode N must not act on episode N+1 (a new incoming after N
         * cleared). */
        if (cm_txn_episode_stale(m, t)) continue;
        call_leg_t *cand = txn_find_role(m, want);
        if (cand != NULL) {
            t->target_id = cand->id;
            t->target_gen = cand->generation;
        }
    }
}

/* §7.1/§7.2: resolve every non-terminal, dispatched op against its postcondition. */
static void txn_resolve(call_model_t *m) {
    for (unsigned ti = 0; ti < MODEM_MAX_CALL_TRANSACTIONS; ti++) {
        call_txn_t *t = &m->txns[ti];
        if (!t->in_use || txn_is_terminal(t) || t->state == TXN_PENDING) continue;

        switch (t->kind) {
        case CALL_TXN_DIAL: {
            if (t->bound_id == 0u) break;                     /* unbound: wait / deadline */
            call_leg_t *l = cm_find_leg(m, t->bound_id, t->bound_gen);
            if (l != NULL && l->state == CALL_LEG_ACTIVE) {
                if (t->dir_confirmed) {                        /* MO-confirmed ACTIVE only */
                    t->witnessed_active = true;
                    t->state = TXN_SUCCEEDED;
                    t->output_resolved = true;                /* §16.9: CONNECTED latched at the leg's ACTIVE */
                    if (t->result == MODEM_CALL_RESULT_NONE)
                        t->result = MODEM_CALL_RESULT_CONNECTED;
                }
                /* tentative ACTIVE: hold in BOUND until confirmed (§7.4) */
            } else if (l == NULL || l->state == CALL_LEG_RELEASING) {
                t->state = TXN_FAILED;                        /* bound leg gone (never blind) */
                t->output_resolved = true;                    /* §16.9: cause latched at the leg's departure */
                if (t->result == MODEM_CALL_RESULT_NONE)
                    t->result = t->witnessed_active ? MODEM_CALL_RESULT_NO_CARRIER   /* brief-connected */
                                                    : MODEM_CALL_RESULT_NO_ANSWER;   /* never witnessed */
            }
            break;
        }
        case CALL_TXN_ANSWER: {
            if (t->target_id == 0u) {
                /* Held pending — mirrors DIAL's unbound wait — while a
                 * pending-MT episode might still materialize the INCOMING leg;
                 * txn_bind_targets rebinds it (above, same reconcile pass) once it
                 * appears. A genuinely-absent participant (no pending-MT either) fails
                 * immediately, as before (never blind-waits forever — the abandon
                 * deadline is still the hard backstop either way). §16.6: hold ONLY
                 * while the op's OWN episode is still live — a replaced episode (a new
                 * incoming after this one cleared) is stale, so fail rather than wait to
                 * bind the later episode. */
                if (m->pending_mt.active && !cm_txn_episode_stale(m, t)) break;
                t->state = TXN_FAILED;
                if (t->result == MODEM_CALL_RESULT_NONE) t->result = MODEM_CALL_RESULT_NO_ANSWER;
                /* §16.3: this is a real no-leg setup failure. The episode's clean
                 * disappearance is the authoritative evidence, so route the
                 * synthesized result even though there is no departure to journal. */
                cm_route_setup_result(m, t, t->result);
                t->output_resolved = true;                    /* §16.9 axis 1 */
                break;
            }
            call_leg_t *l = cm_find_leg(m, t->target_id, t->target_gen);
            if (l != NULL && l->state == CALL_LEG_ACTIVE) {
                t->state = TXN_SUCCEEDED;
                t->output_resolved = true;                    /* §16.9: CONNECTED latched at ACTIVE */
                if (t->result == MODEM_CALL_RESULT_NONE) t->result = MODEM_CALL_RESULT_CONNECTED;
            } else if (l == NULL || l->state == CALL_LEG_RELEASING) {
                t->state = TXN_FAILED;
                t->output_resolved = true;                    /* §16.9: cause latched at departure */
                if (t->result == MODEM_CALL_RESULT_NONE) t->result = MODEM_CALL_RESULT_NO_ANSWER;
            }
            break;
        }
        case CALL_TXN_HOLD: {
            call_leg_t *l = cm_find_leg(m, t->target_id, t->target_gen);
            if (l != NULL) {
                bool now_held = (l->state == CALL_LEG_HELD);
                if (now_held != t->baseline_target_held) t->state = TXN_SUCCEEDED; /* flipped */
            } else {
                t->state = TXN_FAILED;
            }
            break;
        }
        case CALL_TXN_SWAP: {
            call_leg_t *a = cm_find_leg(m, t->target_id, t->target_gen);
            call_leg_t *h = cm_find_leg(m, t->held_id, t->held_gen);
            if (a != NULL && h != NULL) {
                bool target_flipped = t->baseline_target_held
                                      ? a->state == CALL_LEG_ACTIVE
                                      : a->state == CALL_LEG_HELD;
                bool peer_flipped = t->baseline_target_held
                                    ? h->state == CALL_LEG_HELD
                                    : h->state == CALL_LEG_ACTIVE;
                if (target_flipped && peer_flipped) t->state = TXN_SUCCEEDED;
            }
            break;
        }
        case CALL_TXN_WAIT_ANSWER: {
            if (t->target_id == 0u) {
                /* Symmetric with ANSWER's target-0 unbound wait
                 * -- a WAIT_ANSWER(0) whose waiting leg had not yet
                 * materialized at request time is held PENDING while a
                 * pending-MT episode could still produce it (txn_bind_targets
                 * rebinds it, same reconcile pass, since it targets
                 * CALL_TXN_WAIT_ANSWER too); a genuinely-absent participant (no
                 * pending-MT either) fails fast instead of silently lingering
                 * open until a policy/abandon deadline, seconds to a minute
                 * later. */
                if (m->pending_mt.active && !cm_txn_episode_stale(m, t)) break;   /* §16.6 */
                t->state = TXN_FAILED;
                t->output_resolved = true;                    /* §16.9 axis 1 */
                if (t->result == MODEM_CALL_RESULT_NONE) t->result = MODEM_CALL_RESULT_NO_ANSWER;
                break;
            }
            call_leg_t *w = cm_find_leg(m, t->target_id, t->target_gen);  /* waiting */
            call_leg_t *a = cm_find_leg(m, t->held_id, t->held_gen);      /* prior active */
            /* A network may satisfy "answer waiting" either by holding the
             * prior active leg or by releasing it. Positive id-scoped absence
             * of A plus the target B being ACTIVE is just as authoritative as
             * A being HELD. Leaving this txn open would let its abandon timer
             * tear down the successfully answered B. */
            if (w != NULL && w->state == CALL_LEG_ACTIVE &&
                (a == NULL || a->state == CALL_LEG_HELD))
                t->state = TXN_SUCCEEDED;
            break;
        }
        case CALL_TXN_WAIT_REJECT: {
            if (t->target_id == 0u) {
                /* §16.5: a pre-ID WAIT_REJECT (target 0) admitted against a
                 * live pending-MT episode must NOT report SUCCEEDED just because
                 * cm_find_leg(0,0)==NULL — §16.5 forbids reporting SUCCEEDED while a
                 * pending-MT is still present-and-projected (no WAITING leg has
                 * materialized yet). HOLD while the op's OWN episode is live (symmetric
                 * with ANSWER/WAIT_ANSWER's unbound wait); txn_bind_targets rebinds it
                 * to the WAITING leg once it appears, in this same reconcile pass. Once
                 * the episode is GONE (ring backstop / cleared) or REPLACED (stale,
                 * §16.6), the waiting call this reject targeted is already absent -> its
                 * goal (no waiting call) is met -> SUCCEEDED. */
                if (m->pending_mt.active && !cm_txn_episode_stale(m, t)) break;
                t->state = TXN_SUCCEEDED;
                break;
            }
            if (cm_find_leg(m, t->target_id, t->target_gen) == NULL) t->state = TXN_SUCCEEDED;
            break;
        }
        case CALL_TXN_RELEASE_ACTIVE: {
            call_leg_t *a = cm_find_leg(m, t->target_id, t->target_gen);
            call_leg_t *h = cm_find_leg(m, t->held_id, t->held_gen);
            if (a == NULL && (t->held_id == 0u || (h != NULL && h->state == CALL_LEG_ACTIVE)))
                t->state = TXN_SUCCEEDED;                     /* active gone + held promoted */
            break;
        }
        case CALL_TXN_RELEASE_LEG: {
            if (cm_find_leg(m, t->target_id, t->target_gen) == NULL) t->state = TXN_SUCCEEDED;
            break;
        }
        case CALL_TXN_HANGUP: {
            if (!cm_hangup_has_live_obligation(m, t)) t->state = TXN_SUCCEEDED;
            break;
        }
        default: break;
        }
        if (txn_is_terminal(t)) {
            cm_clear_pending_terminal_for_token(m, t->token);
        }
    }
}

/* §7.1 hard abandonment backstop — ONE engine, driven by the tick via
 * model_reconcile_txns (call_model_tick calls model_reconcile_txns, which
 * runs txn_deadlines): a bound setup leg must NOT outlive its txn. The deadline path
 * uses the shared cm_abandon_leg — mark the leg RELEASING + queue its
 * id-scoped release — and resolves the txn FAILED (NOT UNCERTAIN — resolving the old
 * contradiction). The leg is evicted later by its own release URC or the §5.3 limbo,
 * so its projection is retained (never a no-row gap). An unbound txn just FAILs and
 * vanishes. cm_queue_release / cm_abandon_leg are the SOLE release/abandon helpers
 * — no duplicate here. */

static bool txn_in_setup(call_model_t *m, const call_txn_t *t) {
    if (t->bound_id == 0u) return true;                      /* still hunting a leg */
    call_leg_t *l = cm_find_leg(m, t->bound_id, t->bound_gen);
    if (l == NULL) return false;
    return l->state == CALL_LEG_DIALING  || l->state == CALL_LEG_ALERTING ||
           l->state == CALL_LEG_INCOMING || l->state == CALL_LEG_WAITING  ||
           (l->state == CALL_LEG_ACTIVE && !t->dir_confirmed);   /* tentative counts as setup */
}

static void txn_deadlines(call_model_t *m) {
    for (unsigned ti = 0; ti < MODEM_MAX_CALL_TRANSACTIONS; ti++) {
        call_txn_t *t = &m->txns[ti];
        if (!t->in_use || txn_is_terminal(t)) continue;

        /* §16.7: a PENDING txn is never force-cancelled by a timer here (the
         * dispatch watchdog is gone). It arms no deadline until dispatch, so the
         * deadline checks below (state-gated implicitly by its 0 deadlines) are
         * no-ops for it; cancellation is queue-owned (call_model_txn_cancel). */
        if (t->state == TXN_PENDING) {
            continue;
        }

        /* [T6 Minor-1] 0 == not armed. This sentinel is now ROBUST because
         * call_model_txn_dispatched never stamps 0 (never-stamp-zero), so within this
         * loop (already state-gated PAST TXN_PENDING to DISPATCHED/BOUND/UNCERTAIN =
         * armed by construction) a 0 deadline can only mean a white-box "unarmed" txn,
         * never a wrapped real deadline. */
        if (t->abandon_deadline_ms != 0u &&
            txn_deadline_passed(m->now_ms, t->abandon_deadline_ms)) {   /* §7.1 hard backstop */
            /* Output and cleanup are independent. Publish a setup failure directly;
             * never fabricate a journal departure while the leg is still live. Then
             * convert the dispatched operation to the same bindable cleanup tombstone
             * as an explicit post-write cancel, so an unbound late leg is still caught. */
            call_result_sink_t sink = cm_op_descriptor(t).sink;
            if (sink == RESULT_SINK_PRIMARY_LATCH || sink == RESULT_SINK_SECOND_MO_LATCH) {
                if (t->result == MODEM_CALL_RESULT_NONE) {
                    t->result = t->witnessed_active ? MODEM_CALL_RESULT_NO_CARRIER
                                                    : MODEM_CALL_RESULT_NO_ANSWER;
                }
                if (!(sink == RESULT_SINK_PRIMARY_LATCH &&
                      cm_any_surviving_active_or_held(m, 0u, 0u))) {
                    cm_route_result(m, sink, t->result);
                }
            }
            t->output_resolved = true;
            cm_clear_pending_terminal_for_token(m, t->token);
            cm_touch_shadow(m);
            /* Reaching an armed dispatch deadline is itself proof that this record
             * crossed the write boundary; keep the invariant explicit even for a
             * defensively reconstructed record. */
            t->ever_dispatched = true;
            cm_cancel_txn(m, t);
            t->state = TXN_FAILED;                 /* failure result + tombstone cleanup */
            cm_raise_reason(m, (uint32_t)CALL_RC_UNCERTAIN_TXN);
            continue;
        }
        if (t->policy_deadline_ms != 0u &&
            txn_deadline_passed(m->now_ms, t->policy_deadline_ms)) {
            if (txn_in_setup(m, t)) {
                t->state = TXN_UNCERTAIN;                    /* stays bindable */
                cm_raise_reason(m, (uint32_t)CALL_RC_UNCERTAIN_TXN);
            }
        }
    }
}

/* ===================== §16.9 two-axis terminal retirement ===================== */

/* Axis 1 — output_resolved: the txn's owned user-facing result has been published to
 * its latch, or there is nothing to publish (an OUTPUT_FREE supplementary/teardown op,
 * or an output-free tombstone). Else the explicit flag, set at each terminal transition
 * once the latch write / accounting happened. Independent of the cleanup axis. */
static bool cm_txn_output_resolved(const call_txn_t *t) {
    if (cm_op_descriptor(t).sink == RESULT_SINK_OUTPUT_FREE) return true;
    if (t->is_tombstone) return true;   /* §16.9: a cancelled/abandoned op is output-free */
    return t->output_resolved;
}

/* Axis 2 — cleanup_resolved: no outstanding physical leg obligation remains. A
 * DIAL/ANSWER holds its slot while its bound leg lives (preserving 2nd-MO ownership for
 * the departure latch), retiring once the leg is gone; a teardown op holds until its
 * target leg(s) depart (so a clean local hangup stays distinguishable from a remote
 * drop, cm_leg_local_teardown); an UNBOUND tombstone holds until a clean CLCC proves no
 * matching leg (cleanup_confirmed) or its §16.11 deadline expires. */
static bool cm_txn_cleanup_resolved(call_model_t *m, const call_txn_t *t) {
    if (t->cleanup_confirmed) return true;
    switch (t->kind) {
    case CALL_TXN_DIAL:
        if (t->bound_id != 0u) return cm_find_leg(m, t->bound_id, t->bound_gen) == NULL;
        if (t->is_tombstone)   return t->cleanup_confirmed;
        return true;                                          /* plain unbound: no leg to clean */
    case CALL_TXN_ANSWER:
        if (t->state == TXN_FAILED && !t->is_tombstone) return true;
        if (t->target_id != 0u) return cm_find_leg(m, t->target_id, t->target_gen) == NULL;
        if (t->is_tombstone)    return t->cleanup_confirmed;
        return true;
    case CALL_TXN_HANGUP: {
        if (t->state == TXN_FAILED) return true;              /* rejected/no-effect: retryable */
        return !cm_hangup_has_live_obligation(m, t);
    }
    case CALL_TXN_RELEASE_ACTIVE:
    case CALL_TXN_RELEASE_LEG:
    case CALL_TXN_WAIT_REJECT:
        if (t->state == TXN_FAILED && !t->is_tombstone) return true;
        return t->target_id == 0u || cm_find_leg(m, t->target_id, t->target_gen) == NULL;
    default:                    /* HOLD / SWAP / WAIT_ANSWER: no lingering leg obligation */
        return true;
    }
}

static bool cm_txn_retirement_eligible(call_model_t *m, const call_txn_t *t) {
    return txn_is_terminal(t) && cm_txn_output_resolved(t) && cm_txn_cleanup_resolved(m, t);
}

/* §16.9: free the slot of every terminal txn whose BOTH axes are resolved. Runs ONLY
 * from call_model_tick (never mid-event) so an in-progress caller/event can still
 * inspect the txn it just resolved; a tombstone survives here until its cleanup axis
 * clears (its cancelled leg is proven absent). */
static void cm_retire_terminal_txns(call_model_t *m) {
    for (unsigned i = 0; i < MODEM_MAX_CALL_TRANSACTIONS; i++) {
        call_txn_t *t = &m->txns[i];
        if (t->in_use && cm_txn_retirement_eligible(m, t)) {
            memset(t, 0, sizeof(*t));
        }
    }
}

/* True if any transaction OTHER than `self` already owns this (id,gen) leg via its
 * bound_id/bound_gen. Covers a live re-dial's freshly-bound leg (txn_bind_dials runs
 * before cm_bind_tombstones, so a concurrent DIAL has already claimed its own leg) AND
 * a terminal-but-not-yet-retired SUCCEEDED DIAL still holding a legitimate ACTIVE leg
 * through the §16.9 one-tick retirement lag. A tombstone must never bind such a leg —
 * it belongs to a different call. The tombstone's OWN genuine late leg is unowned, so
 * it stays bindable; a coincident owner-vs-tombstone contest falls to the n>=2 defer. */
static bool cm_leg_owned_by_other_txn(const call_model_t *m, const call_txn_t *self,
                                      uint8_t id, uint8_t gen) {
    for (unsigned i = 0; i < MODEM_MAX_CALL_TRANSACTIONS; i++) {
        const call_txn_t *o = &m->txns[i];
        if (o == self || !o->in_use) continue;
        if ((o->bound_id == id && o->bound_gen == gen) ||
            (o->target_id == id && o->target_gen == gen)) return true;
    }
    return false;
}

/* §16.7 tombstone binder: recognize a leg the dispatched-then-cancelled command
 * created but the model had not seen at cancel time — identify it by the DIAL's
 * creation baseline (same rule as txn_bind_dials) and RELEASE it (id-scoped, never
 * resurrect the cancelled op). Runs in the reconcile pass so a late call event/CLCC
 * leg is caught the moment it appears. Ambiguity (>=2 candidates) never guesses. */
static void cm_bind_tombstones(call_model_t *m) {
    for (unsigned ti = 0; ti < MODEM_MAX_CALL_TRANSACTIONS; ti++) {
        call_txn_t *t = &m->txns[ti];
        if (!t->in_use || !t->is_tombstone) continue;
        if (t->kind == CALL_TXN_ANSWER || t->kind == CALL_TXN_WAIT_ANSWER) {
            if (t->target_id != 0u) {
                call_leg_t *target = cm_find_leg(m, t->target_id, t->target_gen);
                if (target != NULL && target->state == CALL_LEG_ACTIVE) {
                    cm_abandon_leg(m, t);
                }
            }
            continue;
        }
        if (t->kind != CALL_TXN_DIAL || t->bound_id != 0u) continue;
        call_leg_t *cand = NULL; unsigned n = 0u;
        for (unsigned li = 0; li < MODEM_MAX_CALL_LEGS; li++) {
            call_leg_t *l = &m->legs[li];
            if (!l->in_use || cm_leg_is_transient(l)) continue;        /* skip already-tearing-down legs */
            if (txn_baseline_contains(t, l->id, l->generation)) continue;
            if (l->dir == CALL_DIR_MT) continue;                       /* a coincident incoming, not our dial */
            if (l->state != CALL_LEG_DIALING && l->state != CALL_LEG_ALERTING &&
                l->state != CALL_LEG_ACTIVE) continue;
            /* Destructive late-leg binding requires authoritative MO evidence.
             * A first-seen ACTIVE/UNKNOWN can be the pending-MT answer race. */
            if (l->state == CALL_LEG_ACTIVE && l->dir != CALL_DIR_MO) continue;
            if (cm_leg_owned_by_other_txn(m, t, l->id, l->generation)) continue;  /* another call's leg — never grab it */
            cand = l; n++;
        }
        if (n != 1u) continue;                                         /* 0 -> nothing yet; >=2 -> CLCC decides */
        t->bound_id = cand->id; t->bound_gen = cand->generation;
        cm_abandon_leg(m, t);                                          /* RELEASING + id-scoped release + §5.3 limbo */
    }
}

static unsigned cm_tombstone_candidate_count(const call_model_t *m, const call_txn_t *t) {
    unsigned count = 0u;
    for (unsigned i = 0; i < MODEM_MAX_CALL_LEGS; i++) {
        const call_leg_t *l = &m->legs[i];
        if (!l->in_use || cm_leg_is_transient(l) ||
            txn_baseline_contains(t, l->id, l->generation)) {
            continue;
        }
        if (t->kind == CALL_TXN_DIAL) {
            if (l->dir == CALL_DIR_MT) continue;
            if (l->state == CALL_LEG_DIALING || l->state == CALL_LEG_ALERTING ||
                (l->state == CALL_LEG_ACTIVE && l->dir == CALL_DIR_MO)) {
                count++;
            }
        } else if (t->kind == CALL_TXN_ANSWER || t->kind == CALL_TXN_WAIT_ANSWER) {
            if (l->state == CALL_LEG_ACTIVE && l->dir != CALL_DIR_MO) count++;
        }
    }
    return count;
}

/* §16.11 bounded-drain backstop for an UNBOUND tombstone (no leg to limbo-evict):
 * force cleanup_confirmed at its deadline even if no clean CLCC ever proves absence. */
static void cm_advance_tombstone_deadlines(call_model_t *m) {
    for (unsigned i = 0; i < MODEM_MAX_CALL_TRANSACTIONS; i++) {
        call_txn_t *t = &m->txns[i];
        if (!t->in_use || !t->is_tombstone || t->cleanup_confirmed) continue;
        bool unbound = (t->kind == CALL_TXN_DIAL) ? (t->bound_id == 0u) : (t->target_id == 0u);
        if (unbound && t->tombstone_deadline_ms != 0u &&
            cm_time_reached(m->now_ms, t->tombstone_deadline_ms)) {
            t->cleanup_confirmed = true;
        }
    }
}

/* Single per-tick/per-event transaction engine. on_event, clcc_ok,
 * and the tick call this after they mutate the table. The deadline
 * pass runs LAST so a same-tick postcondition resolution wins over a deadline;
 * cm_bind_tombstones runs after resolution so a late leg is caught + released. */
void model_reconcile_txns(call_model_t *m) {
    txn_bind_dials(m);
    txn_bind_targets(m);      /* rebind a target-0 ANSWER/WAIT_ANSWER once its leg appears */
    txn_confirm_tentative(m);
    txn_resolve(m);
    txn_deadlines(m);
    cm_bind_tombstones(m);    /* §16.7: bind + release a late tombstone leg (never resurrect) */
}

/* §16.7 cancellation split — the SOLE choke point for reclaim-vs-tombstone, keyed
 * on the immutable ever_dispatched epoch. Both the queue-eviction cancel and the
 * hard-abandon (New-call) path route through it.
 *  - PRE-dispatch (!ever_dispatched): nothing was ever written, no leg can result
 *    -> RECLAIM the slot immediately (memset), NO tombstone. Returns true.
 *  - POST-dispatch (ever_dispatched): create a BINDABLE cancellation tombstone —
 *    abandon a bound setup leg (RELEASING + queued id-scoped release), mark the txn
 *    CANCELLED + is_tombstone, arm the §16.11 bounded-drain backstop. output_resolved
 *    is trivially true (§16.9: a cancelled op publishes no user-facing result); the
 *    slot is HELD (not reclaimed) until §16.9 cleanup is resolved. Returns false. */
static bool cm_cancel_txn(call_model_t *m, call_txn_t *t) {
    cm_touch_shadow(m);                 /* §16.8: a call-affecting transition */
    cm_clear_pending_terminal_for_token(m, t->token);
    if (!t->ever_dispatched) {
        memset(t, 0, sizeof(*t));       /* §16.7 pre-dispatch: reclaim immediately, no tombstone */
        return true;
    }
    cm_abandon_leg(m, t);               /* §16.7 post-dispatch: bound leg -> RELEASING + release (no-op if unbound) */
    t->state = TXN_CANCELLED;
    t->is_tombstone = true;
    t->output_resolved = true;          /* §16.9: output-free (a cancel publishes no user result) */
    /* Only constructive operations can surface a late leg after cancellation.
     * Targeted release operations have a concrete participant (cm_abandon_leg
     * just queued it); HOLD/SWAP/HANGUP cannot create a leg and therefore have no
     * bindable cleanup obligation that should block a retry. */
    switch (t->kind) {
    case CALL_TXN_DIAL:
    case CALL_TXN_ANSWER:
    case CALL_TXN_WAIT_ANSWER:
    case CALL_TXN_WAIT_REJECT:
    case CALL_TXN_RELEASE_ACTIVE:
    case CALL_TXN_RELEASE_LEG:
        t->cleanup_confirmed = false;
        break;
    default:
        t->cleanup_confirmed = true;
        break;
    }
    /* §16.11 bounded-drain backstop: never-stamp-zero (the deadline gate keys on != 0). */
    t->tombstone_deadline_ms = m->now_ms + m->timing.txn_abandon_ms;
    if (t->tombstone_deadline_ms == 0u) t->tombstone_deadline_ms = 1u;
    /* §16.11: pull a prompt CLCC so an unbound tombstone gets its clean-absence proof
     * (a bound tombstone's RELEASING leg already keeps the keepalive alive). */
    cm_raise_reason(m, (uint32_t)CALL_RC_COARSE_FINAL);
    return false;
}

void call_model_new_call_abandoned(call_model_t *m) {
    /* This API is also the Phase-1 consumer acknowledgement for the New-call
     * result latch. The second-MO transaction may already be terminal (the
     * common id-scoped RELEASED -> app rollback ordering), so retirement cannot
     * depend on finding an open transaction to cancel. */
    m->published.second_call_result = MODEM_CALL_RESULT_NONE;
    for (unsigned ti = 0; ti < MODEM_MAX_CALL_TRANSACTIONS; ti++) {
        call_txn_t *t = &m->txns[ti];
        if (!t->in_use || txn_is_terminal(t)) continue;
        if (t->kind == CALL_TXN_DIAL && t->second_mo) {
            cm_cancel_txn(m, t);                             /* §16.7 pre/post-dispatch split */
            return;
        }
    }
}

bool call_model_new_call_cleanup_pending(const call_model_t *m,
                                         uint8_t *target_id_out) {
    if (target_id_out != NULL) *target_id_out = 0u;
    if (m == NULL) return false;
    for (unsigned i = 0u; i < MODEM_MAX_CALL_TRANSACTIONS; i++) {
        const call_txn_t *t = &m->txns[i];
        if (!t->in_use || t->kind != CALL_TXN_DIAL || !t->second_mo ||
            !t->is_tombstone) {
            continue;
        }
        if (t->bound_id == 0u) {
            if (!t->cleanup_confirmed) return true;
        } else if (cm_has_leg(m, t->bound_id, t->bound_gen)) {
            if (target_id_out != NULL) *target_id_out = t->bound_id;
            return true;
        }
    }
    return false;
}

/* §16.7 queue-owned cancellation, linked back by TOKEN (generic — any kind). The
 * model picks reclaim-vs-tombstone from the immutable ever_dispatched epoch
 * (cm_cancel_txn): a PRE-dispatch cancel (enqueue failure / eviction before the
 * bytes were sent) reclaims the slot immediately with NO tombstone (no AT sent, no
 * leg can result); a POST-dispatch cancel leaves a BINDABLE tombstone (a bound
 * setup leg -> RELEASING + queued id-scoped release; the record stays so a late
 * URC / CLCC leg can be identified and released, never resurrected). An unknown /
 * already-terminal token is a silent no-op (idempotent / defensive). The
 * public request API drives
 * this on queue eviction (queue_request_front must return the evicted token). */
void call_model_txn_cancel(call_model_t *m, uint32_t token) {
    call_txn_t *t = txn_by_token(m, token);
    if (t == NULL || txn_is_terminal(t)) return;
    cm_cancel_txn(m, t);            /* §16.7: pre-dispatch reclaim vs post-dispatch tombstone */
}

/* ==================== AT+CLCC shadow-commit reconcile ====================
 * Spec sections 5.2/5.3. A dropped CLCC row must never masquerade as an absent
 * leg (transport-integrity gate), a first clean absence only ARMS a removal
 * (legs retain their published role), and a removal COMMITS only when a second
 * clean snapshot reproduces the same absent set with no intervening URC
 * structural mutation -- committing adds + removals together so a revealed leg
 * is never dropped while removing a twice-absent one. A URC-spam-proof limbo
 * clock force-evicts a leg the modem keeps omitting. All helpers are file-local
 * and clcc_-prefixed so they never collide with the URC-feed helpers. */

static void clcc_bump_rev(call_model_t *m) { m->semantic_revision++; }

/* §3-decision: the id-only finder is the single canonical `cm_find_leg_by_id`
 * ; the CLCC reconcile reuses it rather than defining a twin. */

/* Leg allocation uses the SINGLE canonical cm_alloc_leg: generation comes from
 * m->gen_next[id] (bumped by cm_remove_leg / clcc_evict_leg before the slot is recycled),
 * so a reused id is disambiguated even after the slot is overwritten (§8).
 * The CLCC reconcile defines
 * no allocator of its own; the caller bumps the semantic revision (clcc_bump_rev). */

/* Free a leg slot. §8: bump gen_next[id] past this leg's generation BEFORE clearing, so a
 * later reuse of the id is disambiguated (the freed slot's fields no longer carry it).
 * §9.5/§11: route and journal the departing leg's terminal result BEFORE eviction
 * (cm_latch_terminal performs both; the projector later retires the journal entry). Structural remove ->
 * bumps the semantic revision. */
static void clcc_evict_leg(call_model_t *m, call_leg_t *leg) {
    cm_touch_shadow(m);                /* §16.8 (no-op during the commit: shadow_open already cleared) */
    /* Only this leg's OWN attributed cause is taken (and
     * consumed) — see cm_take_pending_terminal; a non-owning CLCC-confirmed
     * eviction must not inherit or strip a different leg's pending cause. */
    modem_call_result_t cause = cm_take_pending_terminal(m, leg->id, leg->generation);
    cm_latch_terminal(m, leg, cause);                 /* canonical latch -> journal; NONE = keep latch */
    if (leg->state == CALL_LEG_ACTIVE) {
        cm_retire_promoted_compat_cli(m, leg);
    }
    cm_retire_compat_cli_owner(m, leg);
    cm_unbind_pending_mt(m, leg->id, leg->generation);/* S2: no phantom RINGING/waiting after a CLCC evict */
    m->gen_next[leg->id] = cm_generation_after(leg->generation);
    leg->in_use = false;
    leg->pending_removal = false;
    leg->state = CALL_LEG_UNKNOWN;
    clcc_bump_rev(m);
}

static bool clcc_id_in_shadow(const call_model_t *m, uint8_t id) {
    for (unsigned i = 0; i < m->shadow_count; i++) {
        if (m->shadow[i].id == id) return true;
    }
    return false;
}

/* Wrap-safe elapsed for a single interval < 2^31 ms. */
static uint32_t clcc_elapsed(uint32_t now, uint32_t then) { return (uint32_t)(now - then); }

void call_model_clcc_begin(call_model_t *m, uint32_t transport_counter) {
    m->shadow_open = true;
    m->shadow_invalidated = false;       /* §5.4: fresh shadow; any interleaving URC re-arms it */
    m->shadow_count = 0u;
    m->shadow_transport_at_begin = transport_counter;
    m->overflow = false;                 /* per-shadow taint sentinel (see clcc_row) */
    memset(m->shadow, 0, sizeof m->shadow);
    /* removal_armed / absent_* / candidate_revision persist across polls -- the
     * two-snapshot machinery spans begin/ok pairs. */

    /* Scheduling: bump the keepalive/confirm cadence at issue-time (mirrors
     * modem_service's s_next_clcc_ms bump). Last statement so it wins over any
     * earlier assignment; interval = fast inside a confirm/uncertain window, else
     * the backed-off keepalive. */
    m->next_clcc_ms = m->now_ms + cm_clcc_interval_ms(m);
}

void call_model_clcc_row(call_model_t *m, uint8_t id, call_direction_t dir,
                         call_mode_t mode, uint8_t mpty,
                         call_leg_state_t st, const char *number) {
    if (!m->shadow_open) return;                                 /* row without begin */
    if (id == 0u || id > MODEM_MAX_CALL_LEGS) { m->overflow = true; return; }  /* malformed */
    if (m->shadow_count >= MODEM_MAX_CALL_LEGS) { m->overflow = true; return; }/* too many */
    if (clcc_id_in_shadow(m, id)) { m->overflow = true; return; }             /* dup id */
    call_leg_t *r = &m->shadow[m->shadow_count++];
    memset(r, 0, sizeof *r);
    r->in_use = true;
    r->id = id;
    r->dir = dir;
    r->mode = mode;                    /* CLCC <mode> (voice/data) */
    r->mpty = (mpty != 0u);            /* CLCC <mpty> multiparty flag */
    r->state = st;
    r->meta_valid = true;
    if (number != NULL) {
        strncpy(r->number, number, MODEM_PHONE_MAX);
        r->number[MODEM_PHONE_MAX] = '\0';
    }
}

void call_model_clcc_error(call_model_t *m) {
    /* ERROR / +CME/+CMS ERROR (spec 5.2): discard the shadow, live table
     * untouched, back off. Removal arms and first_removal_ms clocks are
     * PRESERVED -- a failed CLCC is not a clean reappearance (5.3). */
    m->shadow_open = false;
    m->shadow_count = 0u;
    if (m->clcc_backoff_shift < 8u) m->clcc_backoff_shift++;
    /* Push next_clcc_ms out by the FRESH backed-off
     * interval right now, rather than leaving it at whatever an earlier
     * clcc_begin scheduled (computed with the OLD, lower shift) — otherwise an
     * ERROR would not actually take effect until the NEXT clcc_begin, one
     * cycle late (and with wants_clcc no longer bypassing the schedule via the
     * fast window, that stale next_clcc_ms would otherwise be honored as-is). */
    m->next_clcc_ms = m->now_ms + cm_clcc_interval_ms(m);
}

/* Returns TRUE iff a clean, transport-trusted snapshot was committed (§16.3 no-leg
 * resolution runs only after a clean CLCC). A transport-reject / overflow returns
 * false (live untouched or conservatively merged — proves nothing about absence). */
static bool clcc_ok_commit(call_model_t *m, uint32_t transport_counter) {
    if (!m->shadow_open) return false;
    m->shadow_open = false;

    /* (1) Transport-integrity gate (5.3 step 1) + malformed/overflow taint. A
     * dropped/truncated row bumps no HAL counter except the Phase-1 dropped-line
     * counter folded into transport_counter, so a change here (or a per-shadow
     * taint) means the snapshot is untrustworthy: discard, live untouched. */
    if (transport_counter != m->shadow_transport_at_begin || m->overflow) {
        m->shadow_count = 0u;
        if (m->clcc_backoff_shift < 8u) m->clcc_backoff_shift++;
        /* §16.10: a transport-reject/overflow advances the shift AND recomputes
         * next_clcc_ms to the NEW (longer) backed-off deadline together — otherwise the
         * shift climbs but the next poll still fires at the OLD (shorter) deadline
         * (the probed "shift=1 but next_clcc_ms left at 5000" stale-deadline bug). */
        m->next_clcc_ms = m->now_ms + cm_clcc_interval_ms(m);
        if (m->overflow) m->reconcile_reasons |= CALL_RC_OVERFLOW;
        return false;
    }
    m->clcc_backoff_shift = 0u;                            /* clean snapshot resets backoff */
    /* §16.10 inverse: a clean recovery resets the shift AND the deadline together —
     * pull next_clcc_ms in to the shift-0 keepalive cadence (clcc_begin had scheduled it
     * with the OLD, longer shift), so recovery does not wait the stale backed-off wait. */
    m->next_clcc_ms = m->now_ms + cm_clcc_interval_ms(m);
    m->reconcile_reasons &= ~(uint32_t)CALL_RC_OVERFLOW;   /* this CLCC fit within capacity */

    uint32_t rev_before_merge = m->semantic_revision;

    /* (2) Non-destructive merge (5.3 step 2): additions + state/metadata updates
     * only. A leg PRESENT in a clean shadow clears pending_removal FULLY and
     * resets its CLCC-absence clock (genuine presence). */
    for (unsigned i = 0; i < m->shadow_count; i++) {
        const call_leg_t *row = &m->shadow[i];
        call_leg_t *leg = cm_find_leg_by_id(m, row->id);
        if (leg == NULL) {
            leg = cm_alloc_leg(m, row->id);               /* single canonical allocator (§8) */
            if (leg == NULL) continue;                    /* overflow handled below */
            clcc_bump_rev(m);                             /* structural add -> bumps rev */
        }
        /* §5.3: a leg already RELEASING (cm_abandon_leg's hard
         * teardown marker) must NEVER be un-released by a stale CLCC row the
         * modem hasn't caught up with yet — the row must not flip its state
         * back (e.g. to a stale DIALING) and must not clear its §5.3 limbo
         * clock. RELEASING is retired ONLY by genuine absence (the two-
         * snapshot/abs_ids path below) or the limbo timeout (this is NOT a
         * race — a present-but-stale row is expected while a release URC is
         * still in flight). */
        bool leg_releasing = (leg->state == CALL_LEG_RELEASING);
        if (!leg_releasing) {
            bool entered_active =
                row->state == CALL_LEG_ACTIVE && leg->state != CALL_LEG_ACTIVE;
            if (leg->state != row->state) {                /* structural transition -> bump */
                leg->state = row->state;
                clcc_bump_rev(m);
            }
            if (row->state == CALL_LEG_ACTIVE) {
                /* A CLCC-only transition to ACTIVE latches CONNECTED and clears the
                 * stale bare-final cause, mirroring the on_event ACTIVE branch (§9.5) — the
                 * connect may be seen ONLY via CLCC (the ID-scoped ACTIVE event was lost). */
                leg->was_answered = true;                     /* journal: CLCC revealed connected (§11) */
                if (entered_active) {
                    m->published.last_call_result = MODEM_CALL_RESULT_CONNECTED;
                }
                cm_clear_pending_terminal(m, leg->id, leg->generation);   /* owner-scoped pending-terminal clear */
            }
        }
        if (row->dir != CALL_DIR_UNKNOWN) leg->dir = row->dir;   /* metadata, no bump */
        if (row->mode != CALL_MODE_UNKNOWN) leg->mode = row->mode;   /* CLCC <mode> */
        leg->mpty = row->mpty;                                       /* CLCC <mpty> */
        leg->meta_valid = true;
        if (row->number[0] != '\0') memcpy(leg->number, row->number, sizeof leg->number);
        leg->last_seen_ms = m->now_ms;
        if (!leg_releasing) {
            leg->pending_removal = false;                  /* genuine presence clears the removal mask (not for RELEASING) */
            leg->first_removal_ms = 0u;                    /* ...and resets the removal clock (not for RELEASING) */
        }
        /* §9.4: the CLCC merge drives the same pending-MT episode tracking as a URC
         * — an INCOMING/WAITING row binds the episode; the bound leg reaching ACTIVE
         * clears it (removal is cleared by cm_unbind_pending_mt in the eviction paths).
         * (A RELEASING leg's state never matches INCOMING/WAITING/ACTIVE, so this is a
         * no-op for it — no extra guard needed.) */
        cm_bind_episode_observation(m, leg);
        cm_pending_mt_track(m, leg);
    }
    if (m->overflow) {                                    /* alloc overflowed mid-merge */
        m->reconcile_reasons |= CALL_RC_OVERFLOW;
        return false;                                     /* conservative: no removals, not clean */
    }

    /* §5.6: episode-scoped pending-MT confirm. If the episode is still active but
     * UNBOUND and this clean snapshot shows NO compatible INCOMING/WAITING leg, count it;
     * two consecutive such clean snapshots clear the stale episode (the ring came but no
     * call leg ever materialized) -- the same two-snapshot rigor as leg removal, without
     * waiting the 120 s ring fallback. A compatible leg present resets the count; a bound
     * episode is tracked by its leg, not by this counter. */
    if (m->pending_mt.active && !m->pending_mt.bound_valid) {
        bool compatible = false;
        for (unsigned i = 0; i < MODEM_MAX_CALL_LEGS; i++) {
            if (m->legs[i].in_use &&
                (m->legs[i].state == CALL_LEG_INCOMING || m->legs[i].state == CALL_LEG_WAITING)) {
                compatible = true;
                break;
            }
        }
        if (compatible) {
            m->pending_mt.clcc_absent_snapshots = 0u;
        } else if (++m->pending_mt.clcc_absent_snapshots >= 2u &&
                   (m->pending_mt.alert_observed ||
                    !m->pending_mt.incoming_diverted)) {
            memset(&m->pending_mt, 0, sizeof(m->pending_mt));   /* stale episode -> clear */
        }
    }

    /* Absent set = live legs (by id) not in the shadow, with the URC-spam-proof
     * limbo force-evict applied inline (5.3 bounded escape).
     *
     * S1: first_removal_ms is the AUTHORITATIVE CLCC-absence clock, independent of
     * the pending_removal role-suppress mask. A lying modem that re-emits a stale
     * non-,6 naming URC for a dead id clears pending_removal (role un-mask) but must
     * NOT reset this clock, else the phantom would be immortal (keepalive never
     * stops, §6 bounded-drain broken). So the force-evict gate keys on the CLOCK
     * (first_removal_ms != 0), not the mask; the clock resets to 0 only on a genuine
     * clean-CLCC reappearance (the merge above). */
    uint8_t abs_ids[MODEM_MAX_CALL_LEGS];
    uint8_t abs_gens[MODEM_MAX_CALL_LEGS];
    uint8_t abs_n = 0u;
    for (unsigned i = 0; i < MODEM_MAX_CALL_LEGS; i++) {
        call_leg_t *leg = &m->legs[i];
        if (!leg->in_use) continue;
        if (clcc_id_in_shadow(m, leg->id)) continue;
        if (leg->first_removal_ms != 0u &&
            clcc_elapsed(m->now_ms, leg->first_removal_ms) >= m->timing.leg_limbo_ms) {
            clcc_evict_leg(m, leg);                       /* terminal force-evict (clock, not mask) */
            continue;
        }
        abs_ids[abs_n] = leg->id;
        abs_gens[abs_n] = leg->generation;
        abs_n++;
    }

    if (abs_n == 0u) {                                    /* (5.3 step 3) nothing absent */
        m->removal_armed = false;
        m->absent_count = 0u;
        m->reconcile_reasons &= ~(uint32_t)CALL_RC_PENDING_REMOVAL;
        return true;
    }

    /* Confirmation only fires when a still-armed, byte-identical absent set
     * (id AND generation) is reproduced with no intervening URC structural
     * mutation since arming: candidate_revision captured the post-first-merge
     * revision; rev_before_merge is this snapshot's pre-merge revision, so its
     * own adds never falsely trip the gate. (5.3 step 4.) */
    bool same_set = m->removal_armed && (m->absent_count == abs_n);
    for (unsigned i = 0; same_set && i < abs_n; i++) {
        if (m->absent_ids[i] != abs_ids[i] || m->absent_gens[i] != abs_gens[i]) same_set = false;
    }
    bool no_intervening_urc = (rev_before_merge == m->candidate_revision);

    if (same_set && no_intervening_urc) {                 /* COMMIT the whole shadow */
        for (unsigned i = 0; i < abs_n; i++) {
            call_leg_t *leg = cm_find_leg_by_id(m, abs_ids[i]);
            if (leg != NULL && leg->generation == abs_gens[i]) clcc_evict_leg(m, leg);
        }
        m->removal_armed = false;
        m->absent_count = 0u;
        m->reconcile_reasons &= ~(uint32_t)CALL_RC_PENDING_REMOVAL;
        return true;
    }

    /* (5.3 step 3) FIRST snapshot, or a CHANGED/invalidated set: (re-)arm. Re-assert
     * the pending_removal role-suppress mask (a naming URC may have cleared it) and
     * stamp the CLCC-absence clock first_removal_ms ONCE — keyed on the CLOCK, not the
     * mask, so a stale naming URC that cleared pending_removal can never trigger a
     * re-stamp (S1: the clock is monotone until a genuine clean-CLCC reappearance).
     * Never stamp zero (0 unambiguously means "not CLCC-absent"). */
    for (unsigned i = 0; i < abs_n; i++) {
        m->absent_ids[i] = abs_ids[i];
        m->absent_gens[i] = abs_gens[i];
        call_leg_t *leg = cm_find_leg_by_id(m, abs_ids[i]);
        if (leg != NULL) {
            leg->pending_removal = true;                  /* role-suppress mask (retention) */
            if (leg->first_removal_ms == 0u) {
                leg->first_removal_ms = (m->now_ms != 0u) ? m->now_ms : 1u;   /* stamp once, never 0 */
            }
        }
    }
    m->absent_count = abs_n;
    m->candidate_revision = m->semantic_revision;         /* post-merge revision */
    m->removal_armed = true;
    cm_raise_reason(m, (uint32_t)CALL_RC_PENDING_REMOVAL);
    return true;
}

/* §16.3: after a CLEAN CLCC (transport-trusted) has proven the current leg set,
 * resolve a bare final's provisional cause that has NO candidate leg — the
 * failure now LATCHES without needing a leg eviction (fixes "primary BUSY stays
 * DIALING through two clean CLCCs, then IDLE with last_call_result=NONE" and the
 * no-id 2nd-MO BUSY). Runs AFTER model_reconcile_txns, so any DIAL that DID bind a
 * leg is excluded. */
static void cm_resolve_no_leg_after_clcc(call_model_t *m) {
    /* (1) an open setup attempt (DIAL/ANSWER) that recorded a provisional cause but
     * has NO leg -> route its owned result to its §16.5 sink + FAIL it. */
    for (unsigned i = 0; i < MODEM_MAX_CALL_TRANSACTIONS; i++) {
        call_txn_t *t = &m->txns[i];
        if (!t->in_use) continue;
        if (t->kind != CALL_TXN_DIAL && t->kind != CALL_TXN_ANSWER) continue;
        if (t->state != TXN_DISPATCHED && t->state != TXN_BOUND && t->state != TXN_UNCERTAIN) continue;
        if (t->result == MODEM_CALL_RESULT_NONE) continue;   /* no provisional cause: still setting up */
        call_leg_t *leg = (t->kind == CALL_TXN_DIAL)
                          ? (t->bound_id  != 0u ? cm_find_leg(m, t->bound_id, t->bound_gen)  : NULL)
                          : (t->target_id != 0u ? cm_find_leg(m, t->target_id, t->target_gen) : NULL);
        if (leg != NULL) continue;                           /* a leg exists: not a no-leg failure */
        call_result_sink_t sink = cm_op_descriptor(t).sink;
        /* The PRIMARY latch must yield to a genuinely-live call: if some ACTIVE/HELD
         * leg survives, the primary call is CONNECTED (this failed DIAL/ANSWER did not
         * produce it) — routing its cause would clobber CONNECTED. Leave the txn OPEN
         * in that case so a LATER pass (once no live call survives) can still latch its
         * cause, rather than force-failing it here and losing the result. The SECOND_MO
         * latch is independent (a 2nd-MO BUSY while the original survives is exactly the
         * point), so it always resolves now. */
        if (sink == RESULT_SINK_PRIMARY_LATCH && cm_any_surviving_active_or_held(m, 0u, 0u)) {
            continue;
        }
        cm_route_setup_result(m, t, t->result);              /* §16.3 latch WITHOUT a leg eviction */
        t->output_resolved = true;                           /* §16.9 axis 1 */
        t->state = TXN_FAILED;                               /* no leg + a recorded cause -> done */
        /* consume the unattributed register that carried this txn's cause. */
        cm_clear_pending_terminal_for_token(m, t->token);
    }
    /* (2) a still-set UNATTRIBUTED cause (an unsolicited bare final with NO owning
     * txn) with NO surviving leg -> the primary call ended: latch it + clear.
     * a register still OWNED by a live setup txn (owner_token) is NOT unattributed —
     * its owning txn (block (1) above, or a later pass) routes it to the right latch;
     * block (2) must never grab it into the PRIMARY latch. */
    if (m->pending_terminal != MODEM_CALL_RESULT_NONE &&
        m->pending_terminal_owner_id == 0u && m->pending_terminal_owner_token == 0u) {
        bool any_leg = false;
        for (unsigned i = 0; i < MODEM_MAX_CALL_LEGS; i++) {
            if (m->legs[i].in_use) { any_leg = true; break; }
        }
        if (!any_leg) {
            m->published.last_call_result = m->pending_terminal;
            cm_clear_pending_terminal(m, 0u, 0u);
        }
    }
}

/* §16.9/§16.11: after a clean CLCC (and cm_bind_tombstones, in the reconcile just
 * run, has had its chance to bind any revealed leg to a matching tombstone), any
 * still-UNBOUND tombstone means the dispatched-then-cancelled command's leg genuinely
 * does not exist — its cleanup is confirmed absent and it can retire (§16.9 axis 2). */
static void cm_resolve_tombstones_after_clcc(call_model_t *m) {
    for (unsigned i = 0; i < MODEM_MAX_CALL_TRANSACTIONS; i++) {
        call_txn_t *t = &m->txns[i];
        if (!t->in_use || !t->is_tombstone || t->cleanup_confirmed) continue;
        bool unbound = (t->kind == CALL_TXN_DIAL) ? (t->bound_id == 0u) : (t->target_id == 0u);
        if ((t->kind == CALL_TXN_ANSWER || t->kind == CALL_TXN_WAIT_ANSWER) &&
            t->mt_episode != 0u && m->pending_mt.active &&
            m->pending_mt.generation == t->mt_episode) {
            continue;   /* still ringing: the already-written answer may yet connect */
        }
        if (unbound && cm_tombstone_candidate_count(m, t) == 0u) {
            t->cleanup_confirmed = true;  /* clean CLCC proved absence, not merely ambiguity */
        }
    }
}

/* Public entry: commit the shadow, then re-run the transaction engine so a CLCC that
 * confirmed a leg's direction (§7.4 tentative -> MO, golden P3) or removed a leg
 * re-binds/re-resolves transactions in the SAME step. model_reconcile_txns is defined
 * with the transaction engine (earlier in this TU) so it is visible here. */
void call_model_clcc_ok(call_model_t *m, uint32_t transport_counter) {
    if (m->shadow_invalidated) {
        /* §5.4: an observation (URC/ring/CLIP/bare-final) interleaved this CLCC pass, so
         * the shadow raced the live table and can no longer be trusted. DISCARD it whole
         * — no merge, no commit — and re-arm a CLCC immediately (the live table already
         * reflects the winning URC; a fresh clean two-poll cycle re-derives structure). */
        m->shadow_open = false;
        m->shadow_count = 0u;
        m->shadow_invalidated = false;                             /* consumed */
        cm_raise_reason(m, (uint32_t)CALL_RC_COARSE_FINAL);    /* still needs a CLCC */
        m->next_clcc_ms = m->now_ms;                               /* re-poll ASAP */
        return;
    }
    bool clean = clcc_ok_commit(m, transport_counter);
    model_reconcile_txns(m);
    if (clean) {
        /* §16.3: a clean CLCC that proved no candidate leg resolves a bare final's
         * provisional cause into its owning latch (after reconcile, so a DIAL that
         * bound a leg is excluded). */
        cm_resolve_no_leg_after_clcc(m);
        /* §16.9/§16.11: the same clean CLCC confirms an unbound tombstone's leg absent. */
        cm_resolve_tombstones_after_clcc(m);
    }
}

/* §6 keepalive predicate (the EXPANDED body; the reconcile-stage placeholder was
 * `reconcile_reasons != 0 || removal_armed`, kept only so the §5.4 interleave test
 * would link). Precedence (the first three gates always win):
 *  - empty model (no leg / pending-MT / open txn) -> never poll (standby restored);
 *  - a CLCC already in flight (shadow_open) -> no new poll;
 *  - an in-flight call-control op (PENDING/DISPATCHED) -> defer the periodic/confirmation
 *    CLCC behind it (call-control outranks reconciliation, §6);
 * then a poll is DUE when any of:
 *  - the scheduled next_clcc_ms is reached (the backed-off keepalive cadence), OR
 *  - we are inside the fast-confirm window (cm_recompute_fast_window), OR
 *  - a fresh URGENT reason bit is set (COARSE_FINAL: §5.5 bare final "force a CLCC";
 *    PENDING_REMOVAL: §5.3 removal candidate needs its confirm snapshot; UNCERTAIN_TXN:
 *    a tentative/uncertain op needs the fast reconcile) -- the eager term the
 *    placeholder had and the interval-only schedule cannot deliver until the next
 *    keepalive slot. next_clcc_ms/the fast window only choose the INTERVAL at the next
 *    clcc_begin; they never PULL a poll earlier, so the urgent bits must do it here. */
bool call_model_wants_clcc(const call_model_t *m) {
    if (!cm_model_has_activity(m)) {
        return false;   /* §6: empty model -> never poll (standby restored) */
    }
    if (m->shadow_open) {
        return false;   /* a CLCC is already in flight */
    }
    if (cm_txn_in_flight(m)) {
        return false;   /* §6: defer behind an in-flight call-control op */
    }
    /* PURELY schedule-based — next_clcc_ms is the
     * single source of truth for "is a poll due". A fresh urgent reason already
     * pulled next_clcc_ms to now via cm_raise_reason (edge-triggered, once), so
     * it polls at the next free slot and then obeys the scheduled fast/backoff
     * deadline. There is deliberately NO separate `now < fast_clcc_until_ms`
     * return-true branch here any more: that raw check re-polled every tick for
     * the WHOLE fast window (a busy loop) and bypassed an ERROR backoff whenever
     * a TXN_UNCERTAIN was live before its policy_deadline. The fast window's
     * ONLY remaining job is to select the shorter INTERVAL (cm_clcc_interval_ms,
     * itself backoff-scaled) that clcc_begin/clcc_error use to
     * compute next_clcc_ms — never to bypass it here. */
    return cm_time_reached(m->now_ms, m->next_clcc_ms);
}

void call_model_tick(call_model_t *m) {
    /* 1. Run the single transaction engine: bind/confirm/resolve + the
     * §7.1 policy/abandon deadline pass (txn_deadlines). This is where the tick
     * OWNS deadline advancement (§6) and where a CLCC-confirmed direction re-binds
     * the tentative DIAL (§7.4 / golden P3). Then the timer-driven terminal
     * transitions for legs/pending-MT (bounded-drain: §5.3 / §9.4). */
    /* §16.9 retirement runs at the START of the tick (before reconcile) so a txn that
     * resolves DURING this tick's reconcile/deadline pass stays observable for one tick
     * (retired next tick) — still a bounded drain. It frees every terminal txn whose
     * BOTH axes are resolved; a tombstone survives until its cancelled leg is proven
     * absent (cleanup axis). §16.11 unbound-tombstone deadline is advanced first so an
     * expired one becomes eligible here. */
    cm_advance_tombstone_deadlines(m);
    cm_retire_terminal_txns(m);

    model_reconcile_txns(m);
    cm_advance_removal_limbo(m);
    cm_advance_pending_mt(m);

    /* 2. Reconcile reason bits drop ONLY when the model is fully settled (§6);
     * the setting of bits is owned by the URC/txn/CLCC feeders (Tasks 5-7). */
    if (cm_model_is_consistent(m)) {
        m->reconcile_reasons = 0u;
    }

    /* 3. Refresh the scheduler's fast-poll window for wants_clcc / clcc_begin. */
    cm_recompute_fast_window(m);

    /* wants_clcc is purely schedule-based (no more raw
     * `now < fast_clcc_until_ms` return-true), so the fast window's only
     * remaining route into next_clcc_ms was clcc_begin/clcc_error — a poll
     * issued OUTSIDE the fast window (e.g. the ~4 s keepalive already in
     * flight when a tentative DIAL's CLCC reveal newly arms the window) left
     * next_clcc_ms stranded at that far keepalive deadline until the NEXT
     * clcc_begin/error fires, deferring a fast confirm by seconds instead of
     * ~300 ms. Clamp next_clcc_ms DOWN to the backoff-scaled fast interval
     * whenever we are freshly inside the window: never nearer than
     * now+interval (no busy-loop) and itself backoff-scaled (an ERROR backoff
     * is still honored), so this only ever pulls a stale keepalive-scale
     * deadline IN, never bypasses a nearer one already scheduled. */
    if (cm_time_before(m->now_ms, m->fast_clcc_until_ms)) {
        uint32_t fast_next = m->now_ms + cm_clcc_interval_ms(m);
        if (cm_time_before(fast_next, m->next_clcc_ms)) {
            m->next_clcc_ms = fast_next;
        }
    }
}

bool call_model_pop_release(call_model_t *m, call_cleanup_release_t *out) {
    /* Dequeue FIFO, but DROP any entry whose {id,gen} leg no longer exists
     * (already released by another path, or the id was reused with a different
     * generation) — so a targeted release is never issued against a DIFFERENT call
     * that happens to reuse the id. Keep dropping until a still-live entry or empty. */
    while (m->release_count > 0u) {
        call_cleanup_release_t release = m->release_queue[0];
        for (unsigned i = 1; i < m->release_count; i++) {
            m->release_queue[i - 1u] = m->release_queue[i];
        }
        m->release_count--;
        if (cm_find_leg(m, release.id, release.generation) != NULL) {
            if (out != NULL) *out = release;
            return true;
        }
        /* else stale -> drop and try the next queued release. */
    }
    return false;
}

bool call_model_requeue_release(call_model_t *m,
                                const call_cleanup_release_t *release) {
    if (m == NULL || release == NULL || release->id == 0u) return false;
    call_leg_t *leg = cm_find_leg(m, release->id, release->generation);
    if (leg == NULL) return false;
    uint8_t before = m->release_count;
    cm_queue_release(m, release->id, release->generation,
                     release->recover_held_survivor);
    /* An existing identical entry also means the cleanup remains scheduled. */
    if (m->release_count != before) return true;
    for (unsigned i = 0u; i < m->release_count; i++) {
        if (m->release_queue[i].id == release->id &&
            m->release_queue[i].generation == release->generation) return true;
    }
    return false;
}

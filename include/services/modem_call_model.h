#ifndef MODEM_CALL_MODEL_H
#define MODEM_CALL_MODEL_H

#include <stdbool.h>
#include <stdint.h>

#include "services/call_types.h"      /* Complete vendor/HAL-neutral call contract. */

#define MODEM_MAX_CALL_LEGS         MODEM_CALL_ID_MAX
#define MODEM_MAX_CALL_TRANSACTIONS 12u
#define MODEM_HANGUP_SETUP_OWNERS   2u  /* at most one primary + one second-MO DIAL */
/* The pending_removal force-evict horizon, the CLCC cadence/confirm interval, and
 * the txn policy/abandon deadlines are NO LONGER #defines here — they come from
 * the call_timing_t profile passed to call_model_init and copied into m->timing
 * (§16.12). A backend timing profile supplies the values; the
 * model bakes none. */

typedef struct {
    bool in_use;
    uint8_t id, generation;
    call_leg_state_t state;
    call_direction_t dir;
    call_mode_t mode;                   /* §4.1 per-leg mode (voice/data) */
    bool mpty, meta_valid, cli_withheld;
    bool incoming_diverted;              /* network says this MT leg was redirected */
    bool mt_cli_valid;                    /* owns the Phase-1 flat incoming CLI slot */
    bool pending_removal;
    bool was_answered;                  /* leg ever reached ACTIVE (feeds the §11 terminal journal) */
    call_leg_state_t published_role;    /* §9/§5.3 role this leg was last published as (retained-role identity) */
    uint32_t mt_episode;                 /* pre-id incoming episode bound to this leg; survives ACTIVE */
    uint32_t first_removal_ms, first_seen_ms, last_seen_ms;
    char number[MODEM_PHONE_MAX + 1u];
} call_leg_t;

typedef enum { TXN_PENDING=0, TXN_DISPATCHED, TXN_BOUND, TXN_UNCERTAIN,
               TXN_SUCCEEDED, TXN_FAILED, TXN_CANCELLED } call_txn_state_t;

typedef struct {
    bool in_use;
    uint32_t token;                     /* §16.1 wrap-safe 32-bit correlation key (0 = none) */
    /* A session HANGUP admitted while ATD is already on the wire but before
     * the resulting leg has an id owns that setup by token. Once the DIAL
     * binds, this makes the late leg part of the original session without
     * absorbing a genuinely later, unrelated DIAL. HANGUP-only fields. */
    uint32_t hangup_setup_tokens[MODEM_HANGUP_SETUP_OWNERS];
    call_txn_kind_t kind;
    call_txn_state_t state;
    uint8_t target_id, target_gen;      /* targeted ops: participant ref */
    uint8_t held_id, held_gen;          /* SWAP / RELEASE_ACTIVE: the held participant */
    uint8_t baseline_ids[MODEM_MAX_CALL_LEGS];    /* DIAL creation_baseline (ids) */
    uint8_t baseline_gens[MODEM_MAX_CALL_LEGS];
    uint8_t baseline_count;
    uint8_t hangup_setup_count;
    uint8_t bound_id, bound_gen;
    bool dir_confirmed, witnessed_active, at_accepted, second_mo;
    bool baseline_target_held;          /* HOLD/SWAP role of target at the real AT write */
    /* §16.7 cancellation epoch: ever_dispatched is IMMUTABLE — set the first time
     * the real AT bytes go out (call_model_txn_dispatched), never at request time,
     * and never cleared. call_model_txn_cancel keys pre-dispatch reclaim (no
     * tombstone) vs post-dispatch tombstone off it. */
    bool ever_dispatched;
    /* §16.7/§16.9 a post-dispatch cancel/hard-abandon leaves a BINDABLE tombstone:
     * the op is cancelled but stays bindable so a late id-scoped release / bare
     * final for the leg the dispatched command may have created can be identified
     * and released (never resurrected), then it retires (§16.9 cleanup axis). */
    bool is_tombstone;
    /* §16.9 retirement axis 1: the txn's owned user-facing result has been
     * published to its latch (or the sink is OUTPUT_FREE / a tombstone is
     * output-free). Independent of the cleanup axis. */
    bool output_resolved;
    /* §16.9 retirement axis 2 (an UNBOUND tombstone only): a clean CLCC has proven
     * no matching-baseline leg exists — or the §16.11 tombstone deadline expired. */
    bool cleanup_confirmed;
    uint32_t policy_deadline_ms, abandon_deadline_ms;
    /* §16.11 bounded-drain backstop: a tombstone retires by this deadline even if
     * no clean CLCC / id-scoped release ever confirms its leg absent. */
    uint32_t tombstone_deadline_ms;
    /* §16.6 pending-MT episode generation captured at admission for an
     * episode-bound op (ANSWER / WAIT_ANSWER / WAIT_REJECT / HANGUP) — a stale op
     * admitted against episode N can never bind a later episode N+1. Zero means no
     * episode was captured and is valid only with an explicit id-qualified target. */
    uint32_t mt_episode;
    modem_call_result_t result;
} call_txn_t;

typedef struct {
    bool active;
    bool alert_observed;                 /* RING/CLIP seen; metadata-only CSSU must not ring UI */
    bool incoming_diverted;              /* pre-id redirected-call observation */
    char number[MODEM_PHONE_MAX + 1u];
    uint8_t cli_validity;               /* 0 valid, 1 withheld, 2 unavailable */
    uint8_t bound_id, bound_gen; bool bound_valid;
    uint32_t generation;                /* §16.6 episode generation — bumped on each fresh
                                         * incoming episode (RING/CLIP transition into a new
                                         * episode). Non-zero while an episode is active; an
                                         * episode-bound txn records this and can never bind a
                                         * later (higher-generation) episode. */
    uint8_t clcc_absent_snapshots;      /* §5.6: consecutive clean CLCC snapshots with
                                         * NO compatible MT leg; two -> clear the stale episode.
                                         * Episode-scoped (reset when the episode is (re)started). */
    uint32_t first_ring_ms, last_ring_ms;
    uint32_t diverted_observed_ms;        /* bounds a CSSU-before-RING correlation window */
} call_pending_mt_t;

/* The published projection: the flat modem_status_t call fields + the model-owned latches.
 * This struct IS last_published_roles. */
typedef struct {
    modem_call_state_t call_state;
    uint8_t active_call_id;
    bool call_on_hold, second_call_held, waiting_call, ring_active;
    bool caller_id_withheld, incoming_diverted;
    char incoming_number[MODEM_PHONE_MAX + 1u];
    modem_call_result_t last_call_result, second_call_result;
} call_projection_t;

/* §9.5/§11 one actual departed-leg terminal record. Result latches are written by
 * explicit transaction/leg transitions; the projector currently retires these
 * records at publish, and a future stage may expose them for app
 * acknowledgement. */
typedef struct {
    bool in_use;
    uint8_t id, generation;
    call_direction_t dir;
    bool was_answered;
    bool is_second_mo;                  /* §9.5 owning-latch marker: this departed leg's
                                         * result belongs to second_call_result (a 2nd-MO
                                         * New-call), else last_call_result. The projector
                                         * drains STRICTLY by this flag, never by "a
                                         * survivor exists". Stamped at cm_latch_terminal
                                         * from generation-qualified DIAL-txn ownership. */
    modem_call_result_t result;
    char number[MODEM_PHONE_MAX + 1u];
    uint32_t token;                     /* §16.1 owning-txn token (wrap-safe) */
} call_terminal_t;

/* One generation-qualified teardown obligation. recover_held_survivor selects
 * the neutral "release foreground, then recover held" semantic operation used
 * when abandoning a second outgoing leg; adapters own its wire encoding. */
typedef struct {
    uint8_t id;
    uint8_t generation;
    bool recover_held_survivor;
} call_cleanup_release_t;

typedef struct {
    call_leg_t legs[MODEM_MAX_CALL_LEGS];
    call_txn_t txns[MODEM_MAX_CALL_TRANSACTIONS];
    call_pending_mt_t pending_mt;
    uint32_t semantic_revision;
    bool overflow;
    uint32_t reconcile_reasons;         /* CALL_RC_* bitmask */
    call_projection_t published;        /* last_published_roles + latches */
    modem_call_result_t pending_terminal; /* §5.5 id-blind provisional cause register; explicit resolution/leg-departure transitions route it to the owning latch. */
    /* The {id,generation} of the leg on_bare_final's newest-open-txn
     * match attributed pending_terminal to (0 = unattributed). A clearing site (an
     * ACTIVE/ring/CONNECT/fresh-DIAL observation) may only wipe a NON-empty register
     * when it is unattributed (owner_id==0, the old best-effort behavior) or when the
     * observation is for this SAME leg — an unrelated leg's ACTIVE/ring can never wipe
     * another leg's still-pending cause before its own eviction consumes it (unchanged). */
    uint8_t pending_terminal_owner_id, pending_terminal_owner_gen;
    /* The OWNING TRANSACTION's token (0 = truly unattributed). A
     * bare final attributed to a still-live setup txn that has NOT bound a leg has
     * owner_id==0 (indistinguishable from unattributed) — so the register is OWNED by
     * this token instead. A foreign leg eviction must SKIP a register owned by a live
     * setup txn; only that txn's own leg or its §16.3 no-leg CLCC resolution consumes it. */
    uint32_t pending_terminal_owner_token;
    /* §9.5/§11 bounded actual-departure journal; the projector retires it at publish. */
    call_terminal_t journal[MODEM_MAX_CALL_LEGS];
    uint8_t journal_count; bool journal_incomplete;
    /* §8 next generation to hand out per call id (index 1..7; init all 1). */
    uint8_t gen_next[MODEM_MAX_CALL_LEGS + 1u];
    /* §5.4 an observation interleaved an open CLCC shadow -> discard the shadow on OK. */
    bool shadow_invalidated;
    /* CLCC shadow-commit scratch. */
    call_leg_t shadow[MODEM_MAX_CALL_LEGS];
    uint8_t shadow_count; bool shadow_open; uint32_t shadow_transport_at_begin;
    uint8_t absent_ids[MODEM_MAX_CALL_LEGS]; uint8_t absent_gens[MODEM_MAX_CALL_LEGS];
    uint8_t absent_count; uint32_t candidate_revision; bool removal_armed;
    uint8_t clcc_backoff_shift;
    /* Scheduling. */
    uint32_t next_clcc_ms; uint32_t fast_clcc_until_ms;
    /* Queued teardown intents carry {id,generation} (§16.9): pop_release drops
     * an entry whose leg is gone or whose id was reused, so neither teardown
     * semantic can act on a DIFFERENT call that reused the id. */
    call_cleanup_release_t release_queue[MODEM_MAX_CALL_LEGS];
    uint8_t release_count;
    uint32_t next_token;                /* §16.1 monotonic token allocator (collision-avoided) */
    uint32_t mt_episode_next;           /* §16.6 monotonic pending-MT episode generation allocator
                                         * (survives episode clears so a later episode never reuses
                                         * an earlier generation; starts at 1, 0 = episode-agnostic). */
    /* §16.12 neutral timing profile, copied BY VALUE at call_model_init. The model
     * reads cadence/confirm/limbo/policy/abandon from here — no baked vendor #define. */
    call_timing_t timing;
    uint32_t now_ms;
} call_model_t;

/* Reconcile reason bits. */
enum { CALL_RC_COARSE_FINAL=1u, CALL_RC_UNBINDABLE_URC=2u, CALL_RC_UNCERTAIN_TXN=4u,
       CALL_RC_PENDING_REMOVAL=8u, CALL_RC_OVERFLOW=16u, CALL_RC_PENDING_MT=32u };

/* --- Lifecycle --- */
/* §16.12: `timing` is copied BY VALUE into the model (a per-vendor provider such
 * as the backend table supplies it). Must be non-NULL with non-zero deadlines -
 * a NULL profile leaves the model's timing zeroed (defensive: no crash, but the
 * bounded-drain deadlines would be degenerate). */
void call_model_init(call_model_t *m, const call_timing_t *timing);
void call_model_set_now(call_model_t *m, uint32_t now_ms);

/* --- URC / event inputs --- */
/* id_valid=false = a COARSE observation with no reliable call id (the LTE boundary):
 * do NOT create/mutate a leg by that id; schedule a CLCC to recover structure.
 * Adapters with an id-bearing event pass id_valid=true. */
void call_model_on_event(call_model_t *m, uint8_t id, bool id_valid, call_leg_state_t st, call_direction_t dir);
void call_model_on_ring(call_model_t *m);
void call_model_on_clip(call_model_t *m, const char *number, uint8_t cli_validity);
/* Id-blind supplementary indication that the next/current incoming leg was
 * redirected. It is retained as pre-id MT metadata without presenting a call
 * until RING/CLIP or an INCOMING/WAITING leg supplies alerting evidence. */
void call_model_on_incoming_diverted(call_model_t *m);
void call_model_on_bare_final(call_model_t *m, modem_call_result_t r,
                              bool in_supplementary_window);

/* --- CLCC shadow feed --- */
void call_model_clcc_begin(call_model_t *m, uint32_t transport_counter);
void call_model_clcc_row(call_model_t *m, uint8_t id, call_direction_t dir,
                         call_mode_t mode, uint8_t mpty,
                         call_leg_state_t st, const char *number);
void call_model_clcc_ok(call_model_t *m, uint32_t transport_counter);
void call_model_clcc_error(call_model_t *m);

/* --- Requests / transactions --- */
/* True only when the table contains exactly one non-removing ACTIVE or HELD
 * leg and no pre-id incoming episode. This is the safe topology for a vendor
 * hold toggle whose wire command can otherwise answer/swap another call. */
bool call_model_hold_toggle_available(const call_model_t *m);
uint32_t call_model_request(call_model_t *m, call_txn_kind_t kind,
                            uint8_t target_id, bool second_mo);   /* 0 = rejected (no slot) */
void call_model_txn_dispatched(call_model_t *m, uint32_t token);  /* AT actually written */

/* §16.2 command-result / timeout API (replaces the removed call_model_txn_accepted).
 * The integration reports each AT call-control command's outcome as ONE atomic
 * payload keyed by token — so the CAUSE travels WITH the status and can never be
 * lost to callback ordering.
 *   CMD_OK      -> the AT final was OK (folds in the old txn_accepted: at_accepted);
 *                  the §7.2 postcondition still governs SUCCEEDED.
 *   CMD_ERROR   -> a correlated rejection (incl. +CME/+CMS): TERMINATES the txn
 *                  carrying its owned result (r.call_result if has_call_result, else
 *                  a default), routed by the op's §16.5 result sink — NOT waiting on
 *                  the policy timer. A rejected HOLD/SWAP/WAIT_REJECT (OUTPUT_FREE)
 *                  writes NO latch.
 *   CMD_TIMEOUT -> txn UNCERTAIN, still bindable, edge-pull an immediate CLCC. NOT
 *                  cancellation (Stage 2 owns cancellation). */
typedef enum { CMD_OK = 0, CMD_ERROR, CMD_TIMEOUT } modem_cmd_status_t;
typedef struct {
    modem_cmd_status_t  status;
    bool                has_call_result;  /* true iff the final carried a normalized cause */
    modem_call_result_t call_result;      /* BUSY / NO_ANSWER / NO_DIALTONE / NO_CARRIER / ... */
} modem_cmd_result_t;
void call_model_txn_command_result(call_model_t *m, uint32_t token, modem_cmd_result_t r);

/* §16.5 MANDATORY dispatch guard — the integration calls this immediately BEFORE the
 * real modem write for every call-control op (a stale answer/reject that sat behind a
 * 120 s SMS can otherwise physically affect a DIFFERENT call). It re-checks, at write time,
 * that the op's captured participants (its §16.5 resolution epoch) are still live:
 *   CALL_DISPATCH_SEND              -> the write is valid; proceed (then call
 *                                     call_model_txn_dispatched once the bytes go out).
 *   CALL_DISPATCH_ALREADY_SATISFIED -> the intended postcondition already holds (target
 *                                     already gone for a teardown, already ACTIVE for an
 *                                     answer): DO NOT send; the model resolved the txn
 *                                     SUCCEEDED.
 *   CALL_DISPATCH_STALE            -> the target vanished (a constructive op's leg/episode
 *                                     is gone): DO NOT send; the model resolved the txn.
 * The model resolves the txn on the two non-SEND outcomes; the integration suppresses
 * the write. Op-generic (dispatches on the participant descriptor), not DIAL-shaped. */
typedef enum { CALL_DISPATCH_SEND = 0, CALL_DISPATCH_ALREADY_SATISFIED,
               CALL_DISPATCH_STALE } call_dispatch_guard_t;
call_dispatch_guard_t call_model_txn_dispatch_guard(call_model_t *m, uint32_t token);

void call_model_new_call_abandoned(call_model_t *m);
/* True while a cancelled second-outgoing transaction still has a bound leg to
 * remove or remains an unbound tombstone that can bind a late leg. target_id_out
 * receives the bound cleanup leg, or 0 while the tombstone remains pre-id. */
bool call_model_new_call_cleanup_pending(const call_model_t *m,
                                         uint8_t *target_id_out);
/* §7.3 queue-eviction cancel, linked back by token: call when call_model_request's
 * atomic allocate-then-enqueue contract fails AFTER the allocation (the token-linked
 * AT-command enqueue itself was rejected) — routes a bound setup leg through the same
 * abandon path as call_model_new_call_abandoned's bound case, else cancels directly. */
void call_model_txn_cancel(call_model_t *m, uint32_t token);

/* --- Tick + outputs --- */
void call_model_tick(call_model_t *m);                            /* deadlines, drain, scheduling */
void call_model_project(call_model_t *m, call_projection_t *out); /* §9 total algorithm */
/* Session/control summaries for the transport owner. A live session is a
 * pending incoming episode, any confirmed/retained leg, or an unresolved local
 * transaction. A cleanup-only unbound tombstone is deliberately excluded: it
 * keeps CLCC reconciliation alive, but the transport may sleep between polls.
 * control_pending is narrower: it reports a presenting incoming episode, an
 * unresolved local intention, or a transient leg. Stable ACTIVE/HELD calls and
 * metadata-only pre-ring observations therefore do not preempt foreground
 * protocols. background_work_blocked preserves the service's still-narrower
 * scheduling policy: dialing, hold, and swap may coexist with ordinary polls,
 * while incoming presentation and answer/teardown convergence may not. Both
 * queries inspect the live model, so same-tick observations are visible before
 * the once-per-tick flat projection is published. */
bool call_model_session_active(const call_model_t *m);
bool call_model_control_pending(const call_model_t *m);
bool call_model_background_work_blocked(const call_model_t *m);
/* True while a local RELEASE_ACTIVE intention is unresolved. Its wire command
 * already owns held-leg promotion, so an app-side retrieve fallback must wait
 * rather than race it with a second hold-state toggle. */
bool call_model_release_active_pending(const call_model_t *m);
bool call_model_wants_clcc(const call_model_t *m);                /* scheduling (§6) */
/* Pop a generation-qualified teardown intent. A stale entry whose leg vanished
 * or whose id was reused is dropped before it reaches the integration. */
bool call_model_pop_release(call_model_t *m, call_cleanup_release_t *out);
/* Restore a just-popped intent when command admission cannot complete. The
 * original generation must still be live; a reused id is never requeued. */
bool call_model_requeue_release(call_model_t *m,
                                const call_cleanup_release_t *release);

#endif /* MODEM_CALL_MODEL_H */

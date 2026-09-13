/* Public-API regression matrix for the call-model transaction layer.
 *
 * State is created only through the public API. Model internals are read for
 * precise ownership and retirement assertions; they are never hand-poked.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "../src/services/modem_call_model.c"
#include "call_timing_fixture.h"

static int s_failures;

static void check(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        s_failures++;
    }
}

static void tinit(call_model_t *m) {
    call_timing_t timing = call_timing_fixture();
    call_model_init(m, &timing);
    call_model_set_now(m, 1000u);
}

static call_txn_t *find_txn(call_model_t *m, uint32_t token) {
    for (unsigned i = 0; i < MODEM_MAX_CALL_TRANSACTIONS; i++) {
        if (m->txns[i].in_use && m->txns[i].token == token) return &m->txns[i];
    }
    return NULL;
}

static call_leg_t *find_leg(call_model_t *m, uint8_t id) {
    for (unsigned i = 0; i < MODEM_MAX_CALL_LEGS; i++) {
        if (m->legs[i].in_use && m->legs[i].id == id) return &m->legs[i];
    }
    return NULL;
}

static modem_cmd_result_t cmd_ok(void) {
    modem_cmd_result_t r = { CMD_OK, false, MODEM_CALL_RESULT_NONE };
    return r;
}

static modem_cmd_result_t cmd_error(modem_call_result_t cause) {
    modem_cmd_result_t r = { CMD_ERROR, true, cause };
    return r;
}

static void dispatch_ok(call_model_t *m, uint32_t token) {
    check(call_model_txn_dispatch_guard(m, token) == CALL_DISPATCH_SEND,
          "precondition: transaction dispatch guard returns SEND");
    call_model_txn_dispatched(m, token);
    call_model_txn_command_result(m, token, cmd_ok());
}

static void clean_clcc_begin_ok(call_model_t *m, uint32_t counter) {
    call_model_clcc_begin(m, counter);
    call_model_clcc_ok(m, counter);
}

static void test_redial_has_one_leg_owner(void) {
    call_model_t m;
    tinit(&m);

    uint32_t old = call_model_request(&m, CALL_TXN_DIAL, 0u, false);
    dispatch_ok(&m, old);
    call_model_set_now(&m, 42000u);
    call_model_tick(&m);
    clean_clcc_begin_ok(&m, 1u);

    call_model_set_now(&m, 45000u);
    uint32_t fresh = call_model_request(&m, CALL_TXN_DIAL, 0u, false);
    check(fresh != 0u, "redial: a fresh attempt is admitted");
    dispatch_ok(&m, fresh);
    call_model_on_event(&m, 1u, true, CALL_LEG_DIALING, CALL_DIR_MO);

    call_txn_t *old_t = find_txn(&m, old);
    call_txn_t *fresh_t = find_txn(&m, fresh);
    check(fresh_t != NULL && fresh_t->bound_id == 1u,
          "redial: the new attempt owns the new leg");
    check(old_t == NULL || old_t->bound_id != 1u,
          "redial: the superseded attempt cannot co-own the new leg");

    call_model_on_event(&m, 1u, true, CALL_LEG_ALERTING, CALL_DIR_MO);
    call_model_set_now(&m, 122000u);
    call_model_tick(&m);
    call_cleanup_release_t release = {0};
    check(!call_model_pop_release(&m, &release),
          "redial: the old deadline cannot release the fresh alerting leg");
}

static void test_superseded_busy_cannot_relatch(void) {
    call_model_t m;
    tinit(&m);

    uint32_t old = call_model_request(&m, CALL_TXN_DIAL, 0u, false);
    dispatch_ok(&m, old);
    call_model_on_bare_final(&m, MODEM_CALL_RESULT_BUSY, false);

    uint32_t fresh = call_model_request(&m, CALL_TXN_DIAL, 0u, false);
    dispatch_ok(&m, fresh);
    check(m.published.last_call_result == MODEM_CALL_RESULT_NONE,
          "redial latch: fresh dispatch clears the old BUSY");
    clean_clcc_begin_ok(&m, 2u);
    check(m.published.last_call_result == MODEM_CALL_RESULT_NONE,
          "redial latch: superseded BUSY cannot re-enter the fresh attempt window");
}

static void test_rejected_redial_does_not_cancel_old_attempt(void) {
    call_model_t m;
    tinit(&m);
    uint32_t old = call_model_request(&m, CALL_TXN_DIAL, 0u, false);
    dispatch_ok(&m, old);
    for (unsigned i = 0; i < MODEM_MAX_CALL_TRANSACTIONS - 2u; i++) {
        check(call_model_request(&m, CALL_TXN_HOLD, 0u, false) != 0u,
              "redial admission precondition: general pool fills");
    }

    check(call_model_request(&m, CALL_TXN_DIAL, 0u, false) == 0u,
          "redial admission: no capacity rejects the replacement");
    call_txn_t *old_t = find_txn(&m, old);
    check(old_t != NULL && cm_txn_is_open(old_t) && !old_t->is_tombstone,
          "redial admission: rejection leaves the existing attempt untouched");
}

static void test_terminal_owner_releases_provisional_cause(void) {
    call_model_t m;
    tinit(&m);
    call_model_on_event(&m, 1u, true, CALL_LEG_ACTIVE, CALL_DIR_MO);
    call_model_on_event(&m, 1u, true, CALL_LEG_HELD, CALL_DIR_MO);

    uint32_t b = call_model_request(&m, CALL_TXN_DIAL, 0u, true);
    dispatch_ok(&m, b);
    call_model_on_bare_final(&m, MODEM_CALL_RESULT_BUSY, false);
    call_model_txn_command_result(&m, b, cmd_error(MODEM_CALL_RESULT_BUSY));
    check(m.pending_terminal == MODEM_CALL_RESULT_NONE,
          "cause lifetime: a terminal owner clears its provisional register");

    call_model_on_event(&m, 1u, true, CALL_LEG_RELEASING, CALL_DIR_UNKNOWN);
    check(m.published.last_call_result == MODEM_CALL_RESULT_NO_CARRIER,
          "cause lifetime: a failed second attempt cannot contaminate the primary departure");
}

static void test_departure_does_not_republish_resolved_setup(void) {
    call_model_t m;
    tinit(&m);
    call_model_on_event(&m, 1u, true, CALL_LEG_ACTIVE, CALL_DIR_MO);

    uint32_t b = call_model_request(&m, CALL_TXN_DIAL, 0u, true);
    dispatch_ok(&m, b);
    call_model_on_event(&m, 2u, true, CALL_LEG_DIALING, CALL_DIR_MO);
    call_model_txn_command_result(&m, b, cmd_error(MODEM_CALL_RESULT_BUSY));
    check(m.published.second_call_result == MODEM_CALL_RESULT_BUSY,
          "cause epoch: B publishes BUSY once");

    uint32_t c = call_model_request(&m, CALL_TXN_DIAL, 0u, true);
    check(c != 0u && m.published.second_call_result == MODEM_CALL_RESULT_NONE,
          "cause epoch: C owns a freshly-cleared second-call latch");
    call_model_on_event(&m, 2u, true, CALL_LEG_RELEASING, CALL_DIR_UNKNOWN);
    check(m.published.second_call_result == MODEM_CALL_RESULT_NONE,
          "cause epoch: B's late departure cannot republish BUSY into C's epoch");
}

static void test_preid_hangup_sends_and_failed_hangup_retries(void) {
    call_model_t m;
    tinit(&m);
    call_model_on_ring(&m);

    uint32_t first = call_model_request(&m, CALL_TXN_HANGUP, 0u, false);
    check(call_model_txn_dispatch_guard(&m, first) == CALL_DISPATCH_SEND,
          "pre-ID HANGUP: a live pending-MT episode must be rejected on the modem");
    call_model_txn_dispatched(&m, first);
    call_model_txn_command_result(&m, first, cmd_error(MODEM_CALL_RESULT_NO_CARRIER));
    call_model_tick(&m);
    uint32_t retry = call_model_request(&m, CALL_TXN_HANGUP, 0u, false);
    check(retry != 0u, "HANGUP retry: a rejected command releases the reserved slot");
}

static void test_failed_target_teardowns_do_not_exhaust_pool(void) {
    call_model_t m;
    tinit(&m);
    call_model_on_event(&m, 1u, true, CALL_LEG_ACTIVE, CALL_DIR_MO);

    for (unsigned i = 0; i < MODEM_MAX_CALL_TRANSACTIONS + 2u; i++) {
        uint32_t token = call_model_request(&m, CALL_TXN_RELEASE_ACTIVE, 0u, false);
        check(token != 0u, "teardown retry: rejected RELEASE_ACTIVE does not leak a slot");
        if (token == 0u) break;
        call_model_txn_dispatched(&m, token);
        call_model_txn_command_result(&m, token, cmd_error(MODEM_CALL_RESULT_NO_CARRIER));
        call_model_tick(&m);
    }
}

static void test_rejected_answer_retries_do_not_exhaust_pool(void) {
    call_model_t m;
    tinit(&m);
    call_model_on_event(&m, 1u, true, CALL_LEG_ACTIVE, CALL_DIR_MO);
    call_model_on_ring(&m);
    call_model_on_event(&m, 2u, true, CALL_LEG_INCOMING, CALL_DIR_MT);

    for (unsigned i = 0; i < MODEM_MAX_CALL_TRANSACTIONS + 2u; i++) {
        uint32_t token = call_model_request(&m, CALL_TXN_ANSWER, 0u, true);
        check(token != 0u, "ANSWER retry: a rejected ATA does not leak a slot");
        if (token == 0u) break;
        call_model_txn_dispatched(&m, token);
        call_model_txn_command_result(&m, token, cmd_error(MODEM_CALL_RESULT_NO_CARRIER));
        call_model_tick(&m);
    }
}

static void test_local_release_ignores_late_bare_final(void) {
    call_model_t m;
    tinit(&m);
    call_model_on_event(&m, 1u, true, CALL_LEG_ACTIVE, CALL_DIR_MO);

    uint32_t token = call_model_request(&m, CALL_TXN_RELEASE_ACTIVE, 0u, false);
    dispatch_ok(&m, token);
    call_model_on_bare_final(&m, MODEM_CALL_RESULT_NO_CARRIER, false);
    call_model_on_event(&m, 1u, true, CALL_LEG_RELEASING, CALL_DIR_UNKNOWN);
    check(m.journal_count == 1u &&
          m.journal[0].result == MODEM_CALL_RESULT_NONE,
          "local teardown: a late bare NO CARRIER does not turn the journal into a remote drop");
    check(m.published.last_call_result == MODEM_CALL_RESULT_NO_CARRIER,
          "bench-recorded compatibility: local End projects legacy NO_CARRIER");
}

static void test_swap_rebaselines_at_write(void) {
    call_model_t m;
    tinit(&m);
    call_model_on_event(&m, 1u, true, CALL_LEG_HELD, CALL_DIR_MO);
    call_model_on_event(&m, 2u, true, CALL_LEG_ACTIVE, CALL_DIR_MO);

    uint32_t first = call_model_request(&m, CALL_TXN_SWAP, 0u, false);
    uint32_t second = call_model_request(&m, CALL_TXN_SWAP, 0u, false);
    dispatch_ok(&m, first);
    call_model_on_event(&m, 2u, true, CALL_LEG_HELD, CALL_DIR_MO);
    call_model_on_event(&m, 1u, true, CALL_LEG_ACTIVE, CALL_DIR_MO);
    call_model_tick(&m);

    dispatch_ok(&m, second);
    call_model_tick(&m);
    check(find_txn(&m, second) != NULL && !txn_is_terminal(find_txn(&m, second)),
          "SWAP: the second press waits for its own post-write role flip");
    call_model_on_event(&m, 1u, true, CALL_LEG_HELD, CALL_DIR_MO);
    call_model_on_event(&m, 2u, true, CALL_LEG_ACTIVE, CALL_DIR_MO);
    check(find_txn(&m, second) != NULL && find_txn(&m, second)->state == TXN_SUCCEEDED,
          "SWAP: the second press succeeds after its own role flip");
}

static void test_answer_first_active_binds_episode(void) {
    call_model_t m;
    tinit(&m);
    call_model_on_ring(&m);
    uint32_t token = call_model_request(&m, CALL_TXN_ANSWER, 0u, false);
    dispatch_ok(&m, token);

    call_model_on_event(&m, 3u, true, CALL_LEG_ACTIVE, CALL_DIR_UNKNOWN);
    call_txn_t *t = find_txn(&m, token);
    check(t != NULL && t->target_id == 3u && t->state == TXN_SUCCEEDED,
          "ANSWER: a first ACTIVE observation binds and completes the episode transaction");
    check(!m.pending_mt.active, "ANSWER: the connected episode clears pending-MT");
    call_projection_t p;
    call_model_project(&m, &p);
    check(p.call_state == MODEM_CALL_ACTIVE && !p.waiting_call && !p.ring_active,
          "ANSWER: no phantom waiting/ring flags survive the connect");
}

static void test_dial_tombstone_does_not_take_answered_leg(void) {
    call_model_t m;
    tinit(&m);
    uint32_t abandoned = call_model_request(&m, CALL_TXN_DIAL, 0u, false);
    dispatch_ok(&m, abandoned);
    call_model_txn_cancel(&m, abandoned);

    call_model_on_ring(&m);
    uint32_t answer = call_model_request(&m, CALL_TXN_ANSWER, 0u, false);
    dispatch_ok(&m, answer);
    call_model_on_event(&m, 4u, true, CALL_LEG_ACTIVE, CALL_DIR_UNKNOWN);

    call_txn_t *old = find_txn(&m, abandoned);
    call_txn_t *ans = find_txn(&m, answer);
    check(ans != NULL && ans->target_id == 4u,
          "ownership: ANSWER owns the first-ACTIVE incoming leg");
    check(old == NULL || old->bound_id != 4u,
          "ownership: a DIAL tombstone cannot claim the answered leg");
    check(find_leg(&m, 4u) != NULL && find_leg(&m, 4u)->state == CALL_LEG_ACTIVE,
          "ownership: the answered leg is not torn down by an unrelated tombstone");
}

static void test_cancelled_preid_answer_releases_late_connected_leg(void) {
    call_model_t m;
    tinit(&m);
    call_model_on_ring(&m);
    uint32_t answer = call_model_request(&m, CALL_TXN_ANSWER, 0u, false);
    check(call_model_txn_dispatch_guard(&m, answer) == CALL_DISPATCH_SEND,
          "cancelled ANSWER precondition: pre-ID answer sends");
    call_model_txn_dispatched(&m, answer);
    call_model_txn_cancel(&m, answer);

    call_model_on_event(&m, 7u, true, CALL_LEG_ACTIVE, CALL_DIR_MT);
    call_leg_t *leg = find_leg(&m, 7u);
    check(leg != NULL && leg->state == CALL_LEG_RELEASING,
          "cancelled ANSWER: an ATA-connected late leg is never resurrected");
    call_cleanup_release_t release = {0};
    check(call_model_pop_release(&m, &release) && release.id == 7u &&
              !release.recover_held_survivor,
          "cancelled ANSWER: the late connected leg gets an id-scoped release");
}

static void test_cancelled_answer_survives_clean_still_ringing_clcc(void) {
    call_model_t m;
    tinit(&m);
    call_model_on_ring(&m);
    uint32_t answer = call_model_request(&m, CALL_TXN_ANSWER, 0u, false);
    call_model_txn_dispatched(&m, answer);
    call_model_txn_cancel(&m, answer);

    call_model_clcc_begin(&m, 12u);
    call_model_clcc_row(&m, 7u, CALL_DIR_MT, CALL_MODE_VOICE, 0u,
                        CALL_LEG_INCOMING, "");
    call_model_clcc_ok(&m, 12u);
    call_model_tick(&m);
    check(find_txn(&m, answer) != NULL,
          "cancelled ANSWER: a still-ringing CLCC row is not proof that ATA cannot connect late");

    call_model_on_event(&m, 7u, true, CALL_LEG_ACTIVE, CALL_DIR_MT);
    call_cleanup_release_t release = {0};
    check(call_model_pop_release(&m, &release) && release.id == 7u &&
              !release.recover_held_survivor,
          "cancelled ANSWER: the later connect remains caught after the clean ringing snapshot");
}

static void test_second_answer_failure_uses_second_latch(void) {
    call_model_t m;
    tinit(&m);
    call_model_on_event(&m, 1u, true, CALL_LEG_ACTIVE, CALL_DIR_MO);
    call_model_on_ring(&m);
    call_model_on_event(&m, 2u, true, CALL_LEG_INCOMING, CALL_DIR_MT);
    uint32_t answer = call_model_request(&m, CALL_TXN_ANSWER, 0u, true);
    dispatch_ok(&m, answer);

    call_model_on_event(&m, 2u, true, CALL_LEG_RELEASING, CALL_DIR_UNKNOWN);
    check(m.published.second_call_result == MODEM_CALL_RESULT_NO_ANSWER,
          "ANSWER ownership: a failed second setup routes to second_call_result");
    check(m.published.last_call_result != MODEM_CALL_RESULT_NO_ANSWER,
          "ANSWER ownership: the second setup failure does not enter the primary latch");
    check(m.journal_count == 1u && m.journal[0].token == answer,
          "ANSWER ownership: the departure journal carries the setup token");
}

static void test_preid_answer_no_leg_routes_failure(void) {
    call_model_t m;
    tinit(&m);
    call_model_on_ring(&m);
    uint32_t answer = call_model_request(&m, CALL_TXN_ANSWER, 0u, false);
    dispatch_ok(&m, answer);

    clean_clcc_begin_ok(&m, 20u);
    clean_clcc_begin_ok(&m, 21u);

    call_txn_t *t = find_txn(&m, answer);
    check(t != NULL && t->state == TXN_FAILED && t->output_resolved,
          "pre-ID ANSWER: two clean no-leg snapshots resolve the setup transaction");
    check(m.published.last_call_result == MODEM_CALL_RESULT_NO_ANSWER,
          "pre-ID ANSWER: authoritative no-leg failure reaches its primary latch");
    check(m.journal_count == 0u,
          "pre-ID ANSWER: no synthetic departure is written to the terminal journal");
}

static void test_unowned_episode_ops_cannot_act_on_future_call(void) {
    const call_txn_kind_t kinds[] = {
        CALL_TXN_ANSWER,
        CALL_TXN_WAIT_ANSWER,
        CALL_TXN_WAIT_REJECT,
    };

    for (unsigned i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
        call_model_t m;
        tinit(&m);
        uint32_t token = call_model_request(&m, kinds[i], 0u, false);
        check(token != 0u, "episode epoch: an otherwise-valid operation is admitted");
        call_model_on_ring(&m);  /* a different caller arrives after admission */

        call_dispatch_guard_t verdict = call_model_txn_dispatch_guard(&m, token);
        check(verdict != CALL_DISPATCH_SEND,
              "episode epoch: an op admitted without a participant cannot affect a future call");
        call_txn_t *t = find_txn(&m, token);
        check(t != NULL && t->state != TXN_PENDING,
              "episode epoch: the suppressed operation resolves instead of lingering");
        check(m.pending_mt.active,
              "episode epoch: suppressing the stale operation leaves the new caller untouched");
    }
}

static void test_ambiguous_clcc_does_not_prove_tombstone_absence(void) {
    call_model_t m;
    tinit(&m);
    call_model_on_event(&m, 1u, true, CALL_LEG_ACTIVE, CALL_DIR_MO);
    uint32_t abandoned = call_model_request(&m, CALL_TXN_DIAL, 0u, true);
    dispatch_ok(&m, abandoned);
    call_model_txn_cancel(&m, abandoned);
    uint32_t fresh = call_model_request(&m, CALL_TXN_DIAL, 0u, true);
    dispatch_ok(&m, fresh);

    call_model_clcc_begin(&m, 9u);
    call_model_clcc_row(&m, 1u, CALL_DIR_MO, CALL_MODE_VOICE, 0u,
                        CALL_LEG_ACTIVE, "");
    call_model_clcc_row(&m, 2u, CALL_DIR_MO, CALL_MODE_VOICE, 0u,
                        CALL_LEG_DIALING, "");
    call_model_clcc_row(&m, 3u, CALL_DIR_MO, CALL_MODE_VOICE, 0u,
                        CALL_LEG_DIALING, "");
    call_model_clcc_ok(&m, 9u);

    call_txn_t *old = find_txn(&m, abandoned);
    check(old != NULL && old->is_tombstone && !old->cleanup_confirmed,
          "tombstone: two candidates are ambiguity, not clean proof of absence");
    call_model_tick(&m);
    check(find_txn(&m, abandoned) != NULL,
          "tombstone: an ambiguous cancellation obligation remains live");
}

static void test_unbound_hard_abandon_keeps_late_leg_cleanup(void) {
    call_model_t m;
    tinit(&m);
    uint32_t token = call_model_request(&m, CALL_TXN_DIAL, 0u, false);
    dispatch_ok(&m, token);

    call_model_set_now(&m, 120500u);
    call_model_clcc_begin(&m, 10u);
    call_model_clcc_row(&m, 5u, CALL_DIR_MO, CALL_MODE_VOICE, 0u,
                        CALL_LEG_DIALING, "");
    call_model_set_now(&m, 121100u);
    call_model_tick(&m);
    call_txn_t *t = find_txn(&m, token);
    check(t != NULL && t->is_tombstone,
          "hard abandon: a dispatched unbound setup leaves a bindable tombstone");
    check(m.shadow_invalidated,
          "hard abandon: the in-flight CLCC shadow is invalidated");
    call_model_clcc_ok(&m, 10u);
    check(find_leg(&m, 5u) == NULL,
          "hard abandon: the invalidated shadow cannot commit an orphan leg");

    call_model_on_event(&m, 5u, true, CALL_LEG_DIALING, CALL_DIR_MO);
    call_cleanup_release_t release = {0};
    check(call_model_pop_release(&m, &release) && release.id == 5u &&
              !release.recover_held_survivor,
          "hard abandon: a genuinely late leg is identified and released");
}

static void test_projector_inconsistency_stays_urgent(void) {
    call_model_t m;
    tinit(&m);
    call_model_on_event(&m, 1u, true, CALL_LEG_ACTIVE, CALL_DIR_MO);
    call_model_clcc_begin(&m, 11u);
    call_model_clcc_row(&m, 1u, CALL_DIR_MO, CALL_MODE_VOICE, 0u,
                        CALL_LEG_ACTIVE, "");
    call_model_clcc_ok(&m, 11u);
    call_model_on_event(&m, 2u, true, CALL_LEG_ACTIVE, CALL_DIR_MO);

    call_projection_t p;
    call_model_project(&m, &p);
    check(call_model_wants_clcc(&m),
          "projector gate: an impossible role multiset edge-pulls CLCC immediately");
    call_model_tick(&m);
    check((m.reconcile_reasons & CALL_RC_UNBINDABLE_URC) != 0u,
          "projector gate: tick cannot clear the reason while the multiset stays impossible");
}

static void test_journal_records_departure_not_deadline(void) {
    call_model_t m;
    tinit(&m);
    uint32_t token = call_model_request(&m, CALL_TXN_DIAL, 0u, false);
    dispatch_ok(&m, token);
    call_model_on_event(&m, 6u, true, CALL_LEG_DIALING, CALL_DIR_MO);
    call_model_set_now(&m, 121100u);
    call_model_tick(&m);
    check(m.journal_count == 0u,
          "journal: a hard deadline is not an actual leg departure");
    call_model_on_event(&m, 6u, true, CALL_LEG_RELEASING, CALL_DIR_UNKNOWN);
    check(m.journal_count == 1u,
          "journal: one physical departure creates exactly one terminal record");
}

static void test_established_second_departure_is_not_setup_failure(void) {
    call_model_t m;
    tinit(&m);
    call_model_on_event(&m, 1u, true, CALL_LEG_ACTIVE, CALL_DIR_MO);
    uint32_t token = call_model_request(&m, CALL_TXN_DIAL, 0u, true);
    dispatch_ok(&m, token);
    call_model_on_event(&m, 2u, true, CALL_LEG_DIALING, CALL_DIR_MO);
    call_model_on_event(&m, 2u, true, CALL_LEG_ACTIVE, CALL_DIR_MO);
    check(m.published.second_call_result == MODEM_CALL_RESULT_NONE,
          "second setup: successful connection leaves no setup failure");
    call_model_on_event(&m, 2u, true, CALL_LEG_RELEASING, CALL_DIR_UNKNOWN);
    check(m.published.second_call_result == MODEM_CALL_RESULT_NONE,
          "second setup: a later established-call departure is not a New-call setup failure");
}

static void test_empty_release_is_already_satisfied(void) {
    call_model_t m;
    tinit(&m);
    uint32_t token = call_model_request(&m, CALL_TXN_RELEASE_ACTIVE, 0u, false);
    check(call_model_txn_dispatch_guard(&m, token) == CALL_DISPATCH_ALREADY_SATISFIED,
          "dispatch guard: releasing an absent target suppresses the modem command");
}

int main(void) {
    test_redial_has_one_leg_owner();
    test_superseded_busy_cannot_relatch();
    test_rejected_redial_does_not_cancel_old_attempt();
    test_terminal_owner_releases_provisional_cause();
    test_departure_does_not_republish_resolved_setup();
    test_preid_hangup_sends_and_failed_hangup_retries();
    test_failed_target_teardowns_do_not_exhaust_pool();
    test_rejected_answer_retries_do_not_exhaust_pool();
    test_local_release_ignores_late_bare_final();
    test_swap_rebaselines_at_write();
    test_answer_first_active_binds_episode();
    test_dial_tombstone_does_not_take_answered_leg();
    test_cancelled_preid_answer_releases_late_connected_leg();
    test_cancelled_answer_survives_clean_still_ringing_clcc();
    test_second_answer_failure_uses_second_latch();
    test_preid_answer_no_leg_routes_failure();
    test_unowned_episode_ops_cannot_act_on_future_call();
    test_ambiguous_clcc_does_not_prove_tombstone_absence();
    test_unbound_hard_abandon_keeps_late_leg_cleanup();
    test_projector_inconsistency_stays_urgent();
    test_journal_records_departure_not_deadline();
    test_established_second_departure_is_not_setup_failure();
    test_empty_release_is_already_satisfied();

    if (s_failures != 0) {
        fprintf(stderr, "%d failure(s)\n", s_failures);
        return 1;
    }
    puts("test_call_model_txn_matrix: all assertions passed");
    return 0;
}

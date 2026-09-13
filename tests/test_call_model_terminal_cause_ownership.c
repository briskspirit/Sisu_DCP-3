/* Regression test: a foreign leg eviction must not STEAL an unbound setup
 * txn's terminal cause (§16.3/§16.4).
 *
 * The defect this locks against: on_bare_final attributes an id-blind cause
 * (BUSY) to the sole eligible open setup txn B, but records ownership via
 * cm_txn_owner_ref which returns bound_id==0 for a
 * 2nd-MO DIAL that has not bound a leg yet — so the register is stamped owner_id 0
 * (indistinguishable from "truly unattributed"). cm_take_pending_terminal then lets
 * ANY departing leg with the owner_id==0 clause consume it. So when the primary call
 * A is released remotely BEFORE any CLCC, A's eviction STEALS B's BUSY: A's call-log
 * terminal becomes BUSY (should be NO_CARRIER for a connected call dropped remotely)
 * and B's New-call outcome is stripped from the register.
 *
 * ROOT: own the register by the owning transaction's TOKEN. A cause recorded for a
 * live setup txn that has not bound a leg is consumable ONLY by that txn's own leg or
 * its §16.3 no-leg CLCC resolution; a FOREIGN leg eviction must SKIP a register still
 * owned by a live setup txn.
 *
 * Control: the same delivered as a token-correlated CMD_ERROR already yields
 * NO_CARRIER (it never populates the id-blind register) — isolating the defect to the
 * bare-final path, which §16.3 names as a case it must resolve.
 *
 * TDD discipline: drive the PUBLIC API ONLY; assertions may READ model
 * state but never POKE it to force a scenario. Single TU — #include the .c directly.
 *   cc -std=c11 -I include -I tests/stubs -Wall -Wextra \
 *      -fsanitize=address,undefined -fno-sanitize-recover=all -g -O1 \
 *      tests/test_call_model_terminal_cause_ownership.c -o /tmp/t && /tmp/t
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

/* A's foreign eviction must NOT steal B's still-provisional BUSY. */
static void test_bare_final_2ndmo_cause_not_stolen(void) {
    call_model_t m; tinit(&m); call_model_set_now(&m, 1000u);

    /* the primary call A connects (was_answered). */
    call_model_on_event(&m, 1u, true, CALL_LEG_ACTIVE, CALL_DIR_MO);
    check(m.published.last_call_result == MODEM_CALL_RESULT_CONNECTED,
          "precondition: A latches CONNECTED");

    /* a 2nd-MO New-call B is dispatched + OK'd but has NOT bound a leg yet. */
    uint32_t tokB = call_model_request(&m, CALL_TXN_DIAL, 0u, true);
    call_model_txn_dispatched(&m, tokB);
    modem_cmd_result_t ok = { .status = CMD_OK, .has_call_result = false,
                              .call_result = MODEM_CALL_RESULT_NONE };
    call_model_txn_command_result(&m, tokB, ok);

    /* B's BUSY arrives id-blind (a bare final) -> attributed to B (sole eligible owner). */
    call_model_on_bare_final(&m, MODEM_CALL_RESULT_BUSY, false);
    check(find_txn(&m, tokB) != NULL && find_txn(&m, tokB)->result == MODEM_CALL_RESULT_BUSY,
          "precondition: the bare BUSY is attributed to B");
    check(m.pending_terminal == MODEM_CALL_RESULT_BUSY,
          "precondition: B's BUSY sits in the provisional register");

    /* A is released remotely BEFORE any CLCC. Its foreign eviction must SKIP B's register. */
    call_model_set_now(&m, 2000u);
    call_model_on_event(&m, 1u, true, CALL_LEG_RELEASING, CALL_DIR_UNKNOWN);
    check(m.published.last_call_result == MODEM_CALL_RESULT_NO_CARRIER,
          "A's remote drop latches NO_CARRIER (NOT contaminated by B's BUSY)");
    check(m.pending_terminal == MODEM_CALL_RESULT_BUSY,
          "B's BUSY SURVIVES A's foreign eviction (register still owned by live B)");

    /* a clean CLCC (no legs) resolves B's no-leg failure into second_call_result (§16.3). */
    call_model_clcc_begin(&m, 7u);
    call_model_clcc_ok(&m, 7u);
    check(m.published.second_call_result == MODEM_CALL_RESULT_BUSY,
          "B's BUSY routes to second_call_result via the §16.3 no-leg resolution");
    check(m.published.last_call_result == MODEM_CALL_RESULT_NO_CARRIER,
          "A's last_call_result stays NO_CARRIER (retained across IDLE for the log)");
}

/* Control — the same outcome delivered as a token-correlated CMD_ERROR already yields
 * NO_CARRIER (no id-blind register to steal). Passes on the unfixed model too, proving
 * the defect is specific to the bare-final path. */
static void test_correlated_error_control(void) {
    call_model_t m; tinit(&m); call_model_set_now(&m, 1000u);

    call_model_on_event(&m, 1u, true, CALL_LEG_ACTIVE, CALL_DIR_MO);
    uint32_t tokB = call_model_request(&m, CALL_TXN_DIAL, 0u, true);
    call_model_txn_dispatched(&m, tokB);
    modem_cmd_result_t err = { .status = CMD_ERROR, .has_call_result = true,
                               .call_result = MODEM_CALL_RESULT_BUSY };
    call_model_txn_command_result(&m, tokB, err);
    check(m.published.second_call_result == MODEM_CALL_RESULT_BUSY,
          "control: a correlated CMD_ERROR BUSY routes to second immediately");
    check(m.pending_terminal == MODEM_CALL_RESULT_NONE,
          "control: the correlated path populates NO id-blind register");

    call_model_set_now(&m, 2000u);
    call_model_on_event(&m, 1u, true, CALL_LEG_RELEASING, CALL_DIR_UNKNOWN);
    check(m.published.last_call_result == MODEM_CALL_RESULT_NO_CARRIER,
          "control: the correlated path already yields NO_CARRIER for A");
}

int main(void) {
    test_bare_final_2ndmo_cause_not_stolen();
    test_correlated_error_control();
    if (s_failures != 0) { fprintf(stderr, "%d failure(s)\n", s_failures); return 1; }
    printf("test_call_model_terminal_cause_ownership: all assertions passed\n");
    return 0;
}

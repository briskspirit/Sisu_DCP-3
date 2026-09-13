#ifndef CALL_TIMING_FIXTURE_H
#define CALL_TIMING_FIXTURE_H

#include "services/call_types.h"

/* Stable host-test profile. These values are intentionally distinct from
 * backend ownership: tests exercise model timing semantics, not a modem. */
static inline call_timing_t call_timing_fixture(void) {
    const call_timing_t timing = {
        .clcc_keepalive_ms = 4000u,
        .clcc_confirm_ms = 300u,
        .leg_limbo_ms = 8000u,
        .txn_policy_ms = 40000u,
        .txn_abandon_ms = 120000u,
    };
    return timing;
}

#endif

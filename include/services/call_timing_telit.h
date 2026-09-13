#ifndef CALL_TIMING_TELIT_H
#define CALL_TIMING_TELIT_H

/* Bench-qualified LE910C1 call-model policy and reconciliation timing. */

#include "services/call_types.h"

#define CALL_TIMING_TELIT_INITIALIZER { \
    .clcc_keepalive_ms = 4000u,        \
    .clcc_confirm_ms = 300u,           \
    .leg_limbo_ms = 8000u,             \
    .txn_policy_ms = 40000u,           \
    .txn_abandon_ms = 120000u,         \
}

static inline call_timing_t call_timing_telit(void) {
    call_timing_t timing = CALL_TIMING_TELIT_INITIALIZER;
    return timing;
}

#endif /* CALL_TIMING_TELIT_H */

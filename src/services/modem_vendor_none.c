/* No-backend vendor stub. Not part of any firmware CMake target: host tests
 * link it as a separate translation unit to exercise modem_service with every
 * vendor hook absent. */
#include "services/modem_vendor.h"

static void none_off_sense_begin(uint32_t now_ms) {
    (void)now_ms;
}

static bool none_off_complete(uint32_t now_ms,
                              const modem_power_observation_t *observation) {
    (void)now_ms;
    (void)observation;
    return true;
}

static bool none_parse_call_urc(const char *line, modem_call_event_t *out) {
    (void)line;
    (void)out;
    return false;
}

static bool none_parse_clcc_row(const char *line, modem_clcc_row_t *out) {
    (void)line;
    (void)out;
    return false;
}

static bool none_build_call_command(call_txn_kind_t kind, const char *number,
                                    uint8_t target_id, char *out,
                                    size_t out_cap, uint32_t *timeout_ms) {
    (void)kind;
    (void)number;
    (void)target_id;
    (void)out;
    (void)out_cap;
    if (timeout_ms != NULL) {
        *timeout_ms = 0u;
    }
    return false;
}

const modem_vendor_t g_modem_vendor = {
    .available = false,
    .capabilities = 0u,
    .name = "No modem",
    .power = {0},
    .off_sense_begin = none_off_sense_begin,
    .off_complete = none_off_complete,
    .init_steps = NULL,
    .init_step_count = 0u,
    .parse_sim_observation = NULL,
    .provision_schema_version = 0u,
    .wake = { .strategy = MODEM_WAKE_NONE },
    .call_urc_prefix = NULL,
    .parse_call_urc = none_parse_call_urc,
    .parse_clcc_row = none_parse_clcc_row,
    .call = {
        /* Nonzero defensive values keep the pure model total even though no
         * transaction can be admitted by this backend. */
        .timing = {
            .clcc_keepalive_ms = 4000u,
            .clcc_confirm_ms = 300u,
            .leg_limbo_ms = 8000u,
            .txn_policy_ms = 40000u,
            .txn_abandon_ms = 120000u,
        },
        .capabilities = 0u,
        .clcc_cmd = NULL,
        .clcc_timeout_ms = 0u,
        .progress_finals_may_complete_command = false,
        .build_command = none_build_call_command,
        .build_dtmf_command = NULL,
    },
    .signal_query = {0},
    .aux_urc_prefixes = NULL,
    .aux_urc_prefix_count = 0u,
    .parse_aux_urc = NULL,
    .translate_direct_sms = NULL,
};

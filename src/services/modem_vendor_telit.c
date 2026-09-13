/* Telit LE910C1-WWX backend assembly and measured Rev B2 power policy.
 *
 * Vendor protocol families live in the adjacent modem_vendor_telit_*.c
 * modules; this file binds them to the neutral modem_vendor_t contract. */

#include "services/modem_vendor.h"
#include "services/modem_call_at.h"
#include "services/call_timing_telit.h"
#include "modem_vendor_telit_internal.h"

#include <string.h>


/* ------------------------------------------------------------------------ */
/* Measured Rev B2 power policy and exported backend table.                 */
/* ------------------------------------------------------------------------ */

/* Board-1 distributions at 2.65 V: module off = raw 35..37 (28..30 mV),
 * module on = raw 2213..2215 (1783..1785 mV). Keep a deliberately enormous
 * hysteresis gap. The low threshold was exercised during repeated graceful
 * shutdowns; the high threshold is crossed only by a genuinely driven VAUX /
 * PWRMON level. */
#define TELIT_STATUS_OFF_MAX_RAW 64u
#define TELIT_STATUS_ON_MIN_RAW 1024u
#define TELIT_STATUS_OFF_STABLE_MS 500u

static uint32_t s_telit_off_low_since_ms;
static bool s_telit_off_low_seen;

static bool telit_power_is_on(
    const modem_power_observation_t *observation) {
    return observation != NULL && observation->module_status_valid &&
           observation->module_status_raw >= TELIT_STATUS_ON_MIN_RAW;
}

static void telit_off_sense_begin(uint32_t now_ms) {
    s_telit_off_low_since_ms = now_ms;
    s_telit_off_low_seen = false;
}

static bool telit_off_complete(
    uint32_t now_ms, const modem_power_observation_t *observation) {
    if (observation == NULL || !observation->module_status_valid ||
        observation->module_status_raw > TELIT_STATUS_OFF_MAX_RAW) {
        s_telit_off_low_seen = false;
        return false;
    }
    if (!s_telit_off_low_seen) {
        s_telit_off_low_seen = true;
        s_telit_off_low_since_ms = now_ms;
        return false;
    }
    return (int32_t)(now_ms -
                     (s_telit_off_low_since_ms +
                      TELIT_STATUS_OFF_STABLE_MS)) >= 0;
}

static const char *const TELIT_AUX_URC_PREFIXES[] = {
    "#QSS:",
    "#MWI:",
    "#CFF:",
    "+CSSI:",
    "+CSSU:",
    "#TEMPMEAS:",
};

static bool telit_command_invalidates_sms_wake(const char *cmd) {
    if (cmd == NULL) {
        return false;
    }
    static const char *const set_prefixes[] = {
        "AT+CFUN=",
        "AT+CNMI=",
        "AT#WKIO=",
        "AT#E2SMSRI=",
        "AT#PSMRI=",
        "AT\\R",
    };
    for (size_t i = 0u;
         i < sizeof(set_prefixes) / sizeof(set_prefixes[0]); i++) {
        size_t prefix_len = strlen(set_prefixes[i]);
        if (strncmp(cmd, set_prefixes[i], prefix_len) == 0 &&
            cmd[prefix_len] != '?') {
            return true;
        }
    }
    return strcmp(cmd, "ATZ") == 0 || strncmp(cmd, "AT&F", 4u) == 0;
}

const modem_vendor_t g_modem_vendor = {
    .available = true,
    /* DTR/CTS sleep and the complete DVI/OAP codec loop are bench-qualified
     * on the fitted WWX. Strict provisioning above prevents a fresh module
     * from advertising a usable call path while DVI is still disabled. */
    .capabilities = MODEM_VENDOR_CAP_DTR_SLEEP |
                    MODEM_VENDOR_CAP_VOICE_TRANSPORT,
    .name = "Telit LE910C1-WWX",
    .power = {
        /* PG rose in ~4 ms; PWRMON in ~11.2 s; first reliable AT in ~15 s.
         * These bounds retain generous cold/module-variance margin. */
        .rail_settle_ms = 20u,
        .rail_power_good_timeout_ms = 250u,
        .pwron_pulse_ms = 1200u,
        .ready_budget_ms = 30000u,
        .off_cmd = "AT#SHDN",
        .off_ack_timeout_ms = 3000u,
        /* #SHDN may take up to 25 s after its early OK. If it never completes,
         * retry gracefully through ON_OFF_N (>=2.5 s), allow a deliberately
         * generous minute for the guide's unbounded finalization, then use the
         * emergency-only HW_SHUTDOWN_N input (>=200 ms). All three outcomes
         * still require the same 500 ms qualified-low PWRMON evidence. */
        .off_sense_timeout_ms = 25000u,
        .graceful_off_pulse_ms = 3000u,
        .graceful_off_sense_timeout_ms = 60000u,
        .emergency_off_pulse_ms = 250u,
        .emergency_off_sense_timeout_ms = 5000u,
        .cut_rail_on_off_timeout = false,
    },
    .power_is_on = telit_power_is_on,
    .off_sense_begin = telit_off_sense_begin,
    .off_complete = telit_off_complete,
    .init_steps = TELIT_INIT_STEPS,
    .init_step_count = TELIT_INIT_STEP_COUNT,
    .parse_sim_observation = telit_parse_sim_observation,
    .provision_steps = TELIT_PROVISION_STEPS,
    .provision_step_count = TELIT_PROVISION_STEP_COUNT,
    .provision_schema_version = 9u,
    .provision_reboot_cmd = "AT#REBOOT",
    .provision_reboot_timeout_ms = 10000u,
    .wake = {
        .strategy = MODEM_WAKE_DTR_CTS,
        .awake_window_ms = 1000u,
        .probe_bypass_cts = true,
        /* #PSMRI supports pulses through 1150 ms. Board-1 WWX bench evidence
         * showed +CMTI is released only after RI rises; 1500 ms also bounds a
         * persistent call indication or stuck line without blocking wake. */
        .ri_release_timeout_ms = 1500u,
        /* Bench: CTS asserted 37 ms after DTR wake and deasserted 36 ms after
         * DTR sleep. 500 ms tolerates scheduling and cold-state variation. */
        .dtr_wake_timeout_ms = 500u,
        .persistence = MODEM_SETTING_PROFILE,
    },
    .sms_wake = {
        .arm_cmd = NULL,
        .timeout_ms = 0u,
        .retry_limit = 0u,
        .command_invalidates_arm = telit_command_invalidates_sms_wake,
        .qualified_by_sim_completion = true,
        .sim_activation_window_ms = 10000u,
    },
    .sms_read_status = {
        /* LE910Cx ThreadX AT guide, AT#SMSUCS: mode 1 prevents +CMGL/+CMGR
         * from consuming REC UNREAD; mode 0 restores ordinary read semantics. */
        .preserve_unread_cmd = "AT#SMSUCS=1",
        .consume_unread_cmd = "AT#SMSUCS=0",
        .timeout_ms = 2500u,
    },
    .call_urc_prefix = "#ECAM:",
    .parse_call_urc = telit_parse_ecam,
    .parse_clcc_row = modem_call_at_parse_clcc_row,
    .call = {
        .timing = CALL_TIMING_TELIT_INITIALIZER,
        .capabilities = MODEM_CALL_CAPABILITY_ALL,
        .clcc_cmd = "AT+CLCC",
        .clcc_timeout_ms = 3000u,
        .progress_finals_may_complete_command = true,
        .build_command = telit_build_call_command,
        .build_dtmf_command = telit_build_dtmf_command,
    },
    .supplementary = {
        .supported = true,
        .reason_mask =
            MODEM_CALL_FORWARD_REASON(CALL_FORWARD_REASON_UNCONDITIONAL) |
            MODEM_CALL_FORWARD_REASON(CALL_FORWARD_REASON_BUSY) |
            MODEM_CALL_FORWARD_REASON(CALL_FORWARD_REASON_NO_REPLY) |
            MODEM_CALL_FORWARD_REASON(CALL_FORWARD_REASON_NOT_REACHABLE) |
            MODEM_CALL_FORWARD_REASON(CALL_FORWARD_REASON_ALL) |
            MODEM_CALL_FORWARD_REASON(CALL_FORWARD_REASON_ALL_CONDITIONAL),
        .command_timeout_ms = 15000u,
        .call_forward_response_prefix = "+CCFC:",
        .call_forward_step_count = telit_call_forward_step_count,
        .build_call_forward_step = telit_build_call_forward_step,
        .parse_call_forward_row = telit_parse_call_forward_row,
        .voice_mailbox_query_cmd = "AT#MBN",
        .voice_mailbox_response_prefix = "#MBN:",
        .voice_mailbox_timeout_ms = 5000u,
        .parse_voice_mailbox_row = telit_parse_voice_mailbox_row,
        .message_waiting_query_cmd = "AT#MWI?",
        .message_waiting_response_prefix = "#MWI:",
        .message_waiting_timeout_ms = 5000u,
        .parse_message_waiting_row = telit_parse_message_waiting_row,
    },
    .signal_query = {
        .query_cmd = "AT#RFSTS",
        .response_prefix = "#RFSTS:",
        .timeout_ms = 5000u,
        .parse_response = telit_parse_signal_response,
    },
    .diag_queries = TELIT_DIAG_QUERIES,
    .diag_query_count =
        (uint8_t)(sizeof(TELIT_DIAG_QUERIES) /
                  sizeof(TELIT_DIAG_QUERIES[0])),
    .diag_group_finish = telit_diag_group_finish,
    .maintenance = {
        .supported = true,
        .sim_transition_window_ms = 10000u,
        .scan_timer_query_cmd = "AT#NWSCANTMR?",
        .build_scan_timer_set = telit_maintenance_build_scan_timer,
        .parse_scan_timer = telit_maintenance_parse_scan_timer,
        .band_mode_query_cmd = "AT#SELBNDMODE?",
        .build_band_mode_set = telit_maintenance_build_band_mode,
        .parse_band_mode = telit_maintenance_parse_band_mode,
        .band_nvm_query_cmd = "AT#BND?",
        .band_ram_query_cmd = "AT#BNDRAM?",
        .build_band_ram_set = telit_maintenance_build_band_ram,
        .parse_band_nvm = telit_maintenance_parse_band_nvm,
        .parse_band_ram = telit_maintenance_parse_band_ram,
        .band_preset = telit_maintenance_band_preset,
        .function_query_cmd = "AT+CFUN?",
        .build_function_set = telit_maintenance_build_function,
        .parse_function = telit_maintenance_parse_function,
        .tuner_enabled_query_cmd = "AT#STUNEANT?",
        .build_tuner_enabled_set = telit_maintenance_build_tuner_enabled,
        .parse_tuner_enabled = telit_maintenance_parse_tuner_enabled,
        .tuner_table_query_cmd = "AT#GTUNEANT?",
        .tuner_table_begin = telit_tune_readback_begin,
        .parse_tuner_table_row = telit_maintenance_parse_tuner_row,
        .tuner_table_exact = telit_maintenance_tuner_table_exact,
        .build_tuner_row = telit_maintenance_build_tuner_row,
        .antenna_gpio_a = 2u,
        .antenna_gpio_b = 3u,
        .antenna_gpio_a_alt_direction = 17u,
        .antenna_gpio_b_alt_direction = 18u,
        .antenna_controls = telit_maintenance_antenna_controls,
        .build_gpio_query = telit_maintenance_build_gpio_query,
        .build_gpio_set = telit_maintenance_build_gpio_set,
        .parse_gpio = telit_maintenance_parse_gpio,
    },
    .aux_urc_prefixes = TELIT_AUX_URC_PREFIXES,
    .aux_urc_prefix_count =
        (uint8_t)(sizeof(TELIT_AUX_URC_PREFIXES) /
                  sizeof(TELIT_AUX_URC_PREFIXES[0])),
    .parse_aux_urc = telit_parse_aux_urc,
};

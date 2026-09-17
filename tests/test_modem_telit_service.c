#include <stdio.h>

#include "harness/modem_service_harness.h"
#include "services/modem_supplementary_state.h"
#include "services/sms_identity.h"
#include "services/sms_submit_codec.h"

static int s_failures;
static bool s_rxdiv_configured;
static uint8_t s_sled_mode;
static bool s_sled_saved;
static bool s_cpms_configured;
static bool s_ecamurc_configured;
static bool s_cssn_configured;
static bool s_cff_configured;
static bool s_cff_flags_absent;
static bool s_dviext_configured;
static bool s_dvi_configured;
static bool s_wkio_configured;
static bool s_ring_pulse_mode;
static uint16_t s_e2smsri_ms;
static uint16_t s_psmri_ms;
static uint8_t s_cnmi_mode;
static bool s_psmri_live_armed;
static bool s_late_psmri_invalidated_latch;
static bool s_stune_enabled;
static uint8_t s_gpio2_direction;
static uint8_t s_gpio3_direction;
static bool s_gpio2_state;
static bool s_gpio3_state;
static uint8_t s_cfun;
static uint16_t s_scan_timer_s;
static uint8_t s_auto_profile;
static const char *s_startup_drop_command;
static unsigned s_startup_drops_remaining;
static uint8_t s_band_mode;
static modem_band_config_t s_band_nvm;
static modem_band_config_t s_band_ram;
static uint64_t s_tune_masks[4];
static uint8_t s_tune_zero_rows;
static unsigned s_tune_query_errors;
static bool s_qss_response_enabled;
static bool s_inject_imei_on_qss_set;
static bool s_factory_flow_blocked;
static bool s_diag_rfsts_malformed_once;
static bool s_diag_rfsts_hold_once;
static bool s_diag_moni_hold_once;
static bool s_diag_firmware_hold_once;
static const char *s_rfsts_response;
static const char *s_cops_response;
static const char *s_sim_provider_response;
static bool s_sim_provider_error;
static char s_sim_provider_fixture[64];
static const char *s_clcc_row;
static bool s_sms_keep_unread;
static bool s_sms_test_row_enabled;
static bool s_sms_test_row_unread;
static bool s_sms_test_cmti_interleave;
static const char *s_sms_test_pdu;
typedef enum {
    SMS_FLOW_FAULT_NONE = 0,
    SMS_FLOW_FAULT_CPMS_ERROR,
    SMS_FLOW_FAULT_CPMS_TIMEOUT,
    SMS_FLOW_FAULT_CMGF_PDU_ERROR,
    SMS_FLOW_FAULT_CMGF_PDU_TIMEOUT,
    SMS_FLOW_FAULT_CMGS_PROMPT_ERROR,
    SMS_FLOW_FAULT_CMGS_PROMPT_TIMEOUT,
    SMS_FLOW_FAULT_CMGS_FINAL_ERROR,
    SMS_FLOW_FAULT_CMGS_FINAL_TIMEOUT,
    SMS_FLOW_FAULT_CMGW_PROMPT_ERROR,
    SMS_FLOW_FAULT_CMGW_PROMPT_TIMEOUT,
    SMS_FLOW_FAULT_CMGW_FINAL_ERROR,
    SMS_FLOW_FAULT_CMGW_FINAL_TIMEOUT,
    SMS_FLOW_FAULT_CMGW_FINAL_MEMORY_FULL, /* +CMS ERROR: memory full after the body */
    SMS_FLOW_FAULT_CMGW_PROMPT_MEMORY_FULL, /* refused at AT+CMGW=, no prompt */
    SMS_FLOW_FAULT_CMGF_TEXT_ERROR,
    SMS_FLOW_FAULT_CMGF_TEXT_TIMEOUT,
    SMS_FLOW_FAULT_DELETE_SECOND_ERROR,
} sms_flow_fault_t;
typedef enum {
    SMS_PROMPT_NONE = 0,
    SMS_PROMPT_CMGS_TEXT,
    SMS_PROMPT_CMGS_BINARY,
    SMS_PROMPT_CMGW,
} sms_prompt_t;
static sms_flow_fault_t s_sms_flow_fault;
static sms_prompt_t s_sms_prompt;
static bool s_sms_esc_returns_final;
/* Answer ERROR to AT+CMGF=1 only once the service is READY (init's own
 * AT+CMGF=1 must succeed), to fault the idle-scheduler mode restore. */
static bool s_sms_fail_cmgf_text_when_ready;
/* Answer ERROR to the next N AT+CMGF=1 commands, then OK again. */
static unsigned s_sms_cmgf_text_error_budget;
static modem_service_test_snapshot_t service_probe(void);
/* Exact command whose final is withheld (MH_FINAL_NONE) so a test can observe
 * the service with that command still on the wire. */
static const char *s_hold_final_command;
static bool s_sms_inject_crossed_urcs;
static uint8_t s_sms_delete_command_count;
static modem_message_waiting_status_t s_message_waiting;
static const uint8_t TELIT_MWI_INDICATOR_BY_CATEGORY[
    MODEM_MESSAGE_WAITING_CATEGORY_COUNT] = {1u, 2u, 3u, 4u, 5u};
static bool s_mwi_ambiguous_snapshot;
static bool s_call_forward_active[6];
static char s_call_forward_number[6][MODEM_PHONE_MAX + 1u];
static uint8_t s_call_forward_delay[6];
#define TELIT_PHONEBOOK_FIXTURE_MAX 8u
typedef struct {
    bool used;
    modem_phonebook_entry_t entry;
} telit_phonebook_fixture_entry_t;
static telit_phonebook_fixture_entry_t
    s_phonebook[TELIT_PHONEBOOK_FIXTURE_MAX];
static bool s_phonebook_inject_malformed;
static bool s_phonebook_inject_oversized;
static bool s_phonebook_inject_high_index;
static const uint64_t EXPECTED_TUNE_MASKS[4] = {
    UINT64_C(0x601840A7), UINT64_C(0x00010300),
    UINT64_C(0x1F80BC50), UINT64_C(0x00400008),
};
typedef enum {
    TELIT_FAULT_NONE = 0,
    TELIT_FAULT_RXDIV_READBACK_MALFORMED,
    TELIT_FAULT_RXDIV_SET_ERROR,
    TELIT_FAULT_RXDIV_VERIFY_MISMATCH,
    TELIT_FAULT_RXDIV_SET_TIMEOUT_ONCE,
    TELIT_FAULT_SCAN_READBACK_MALFORMED,
    TELIT_FAULT_SCAN_SET_ERROR,
    TELIT_FAULT_SCAN_VERIFY_MISMATCH,
    TELIT_FAULT_SCAN_SET_TIMEOUT_ONCE,
    TELIT_FAULT_AUTO_PROFILE_READBACK_MALFORMED,
    TELIT_FAULT_AUTO_PROFILE_SET_ERROR,
    TELIT_FAULT_AUTO_PROFILE_VERIFY_MISMATCH,
    TELIT_FAULT_AUTO_PROFILE_SET_TIMEOUT_ONCE,
    TELIT_FAULT_REBOOT_TIMEOUT_ONCE,
    TELIT_FAULT_REBOOT_RI_BEFORE_DROP,
    TELIT_FAULT_REBOOT_RI_LOST_FINAL,
    TELIT_FAULT_CPMS_SET_ERROR,
    TELIT_FAULT_SMS_WAKE_PROFILE_SET_TIMEOUT_ONCE,
    TELIT_FAULT_SMS_WAKE_PROFILE_SET_ERROR,
    TELIT_FAULT_FINAL_CFUN_SET_TIMEOUT_ONCE,
    TELIT_FAULT_CALL_FORWARD_ERROR_ONCE,
    TELIT_FAULT_CALL_FORWARD_TIMEOUT_ONCE,
    TELIT_FAULT_CALL_FORWARD_SECOND_ERROR_ONCE,
    TELIT_FAULT_SHUTDOWN_TIMEOUT,
    TELIT_FAULT_SHUTDOWN_STUCK_ON,
    TELIT_FAULT_MAINT_GPIO2_ALT_READBACK,
    TELIT_FAULT_MAINT_GPIO2_INPUT_HIGH,
    TELIT_FAULT_SMS_STATUS_CONSUME_TIMEOUT_ONCE,
    TELIT_FAULT_VVM_DELETE_ERROR_ONCE,
    TELIT_FAULT_VVM_DELETE_TIMEOUT_ONCE,
    TELIT_FAULT_MWI_URC_DURING_QUERY_ONCE,
    TELIT_FAULT_MWI_URC_THEN_AMBIGUOUS_QUERY_ONCE,
    TELIT_FAULT_MWI_FAX_URC_THEN_AMBIGUOUS_QUERY_ONCE,
    TELIT_FAULT_MBN_ERROR_ALWAYS,
    TELIT_FAULT_MWI_ERROR_ONCE,
    TELIT_FAULT_CFU_FLAGS_ERROR_ONCE,
    TELIT_FAULT_PHONEBOOK_CPBS_ERROR_ONCE,
    TELIT_FAULT_PHONEBOOK_CPBS_TIMEOUT_ONCE,
    TELIT_FAULT_PHONEBOOK_CPBR_ERROR_ONCE,
    TELIT_FAULT_PHONEBOOK_CPBR_TIMEOUT_ONCE,
    TELIT_FAULT_PHONEBOOK_CPBW_ERROR_ONCE,
    TELIT_FAULT_PHONEBOOK_CPBW_TIMEOUT_ONCE,
    TELIT_FAULT_DTMF_ERROR_ONCE,
    TELIT_FAULT_DTMF_TIMEOUT_ONCE,
} telit_fault_t;
static telit_fault_t s_fault;
static bool s_fault_consumed;
static uint8_t s_dtmf_command_count;
static uint8_t s_dtmf_fault_at;
static bool s_dtmf_queue_hangup_on_first;
static bool s_dtmf_hangup_admitted;
static bool s_dtmf_inject_release_on_first;

static int tune_state(uint8_t ctrl1, uint8_t ctrl2) {
    return (int)((ctrl1 << 1u) | ctrl2);
}

static void telit_push_tune_table(void) {
    for (uint8_t i = 0u; i < s_tune_zero_rows; i++) {
        mh_rx_push(i == 0u ? "#GTUNEANT: 0,1,1"
                          : "#GTUNEANT: 0,1,0");
    }
    for (unsigned state = 0u; state < 4u; state++) {
        if (s_tune_masks[state] == 0u) {
            continue;
        }
        char line[64];
        unsigned ctrl1 = state >> 1u;
        unsigned ctrl2 = state & 1u;
        snprintf(line, sizeof(line), "#GTUNEANT: %llX,%u,%u",
                 (unsigned long long)s_tune_masks[state], ctrl1, ctrl2);
        mh_rx_push(line);
    }
}

static bool telit_apply_tune_row(const char *command) {
    unsigned long long raw_mask = 0u;
    unsigned ctrl1 = 0u;
    unsigned ctrl2 = 0u;
    int consumed = 0;
    if (sscanf(command, "AT#STUNEANT=1,%llX,%u,%u%n", &raw_mask,
               &ctrl1, &ctrl2, &consumed) != 3 ||
        command[consumed] != '\0' || raw_mask == 0u || ctrl1 > 1u ||
        ctrl2 > 1u) {
        return false;
    }
    uint64_t mask = (uint64_t)raw_mask;
    if (!s_stune_enabled) {
        memset(s_tune_masks, 0, sizeof(s_tune_masks));
        /* Bench-observed WWX behavior: disabling preserves placeholder slots;
         * the first new row rebuilds the supported-mask complement at 00. */
        s_tune_masks[0] = UINT64_C(0x7FD9FFFF);
    } else if (s_tune_masks[tune_state((uint8_t)ctrl1,
                                       (uint8_t)ctrl2)] == 0u &&
               s_tune_zero_rows > 0u) {
        s_tune_zero_rows--;
    }
    for (unsigned state = 0u; state < 4u; state++) {
        s_tune_masks[state] &= ~mask;
    }
    s_tune_masks[tune_state((uint8_t)ctrl1, (uint8_t)ctrl2)] |= mask;
    s_stune_enabled = true;
    return true;
}

static bool telit_apply_gpio_set(const char *command) {
    unsigned pin = 0u;
    unsigned state = 0u;
    unsigned direction = 0u;
    unsigned save = 0u;
    int consumed = 0;
    if (sscanf(command, "AT#GPIO=%u,%u,%u,%u%n", &pin, &state,
               &direction, &save, &consumed) != 4 ||
        command[consumed] != '\0' || (pin != 2u && pin != 3u) ||
        state > 1u || direction > 20u || save > 1u) {
        return false;
    }
    if (pin == 2u) {
        s_gpio2_direction = (uint8_t)direction;
        s_gpio2_state = state != 0u;
    } else {
        s_gpio3_direction = (uint8_t)direction;
        s_gpio3_state = state != 0u;
    }
    return true;
}

static bool telit_apply_band_ram(const char *command) {
    unsigned gsm = 0u;
    unsigned wcdma = 0u;
    unsigned long long lte = 0u;
    int consumed = 0;
    if (sscanf(command, "AT#BNDRAM=%u,%u,%llX%n", &gsm, &wcdma,
               &lte, &consumed) != 3 || command[consumed] != '\0' ||
        gsm > UINT8_MAX || wcdma > UINT16_MAX || lte == 0u) {
        return false;
    }
    s_band_ram.gsm = (uint8_t)gsm;
    s_band_ram.wcdma = (uint16_t)wcdma;
    s_band_ram.lte = (uint64_t)lte;
    return true;
}

static bool telit_apply_call_forward(const char *command) {
    unsigned reason = 0u;
    unsigned action = 0u;
    int consumed = 0;
    if (sscanf(command, "AT+CCFC=%u,%u%n", &reason, &action, &consumed) != 2 ||
        reason > 5u || action > 4u) {
        return false;
    }

    if (reason == CALL_FORWARD_REASON_ALL &&
        action == CALL_FORWARD_ACTION_ERASE &&
        command[consumed] == '\0') {
        memset(s_call_forward_active, 0, sizeof(s_call_forward_active));
        memset(s_call_forward_number, 0, sizeof(s_call_forward_number));
        memset(s_call_forward_delay, 0, sizeof(s_call_forward_delay));
        return true;
    }

    if (action == CALL_FORWARD_ACTION_QUERY) {
        if (strcmp(command + consumed, ",,,1") != 0) {
            return false;
        }
        char line[96];
        if (!s_call_forward_active[reason]) {
            snprintf(line, sizeof(line), "+CCFC: 0,1");
        } else if (s_call_forward_delay[reason] != 0u) {
            snprintf(line, sizeof(line), "+CCFC: 1,1,\"%s\",%u,,,%u",
                     s_call_forward_number[reason],
                     s_call_forward_number[reason][0] == '+' ? 145u : 129u,
                     (unsigned)s_call_forward_delay[reason]);
        } else {
            snprintf(line, sizeof(line), "+CCFC: 1,1,\"%s\",%u",
                     s_call_forward_number[reason],
                     s_call_forward_number[reason][0] == '+' ? 145u : 129u);
        }
        mh_rx_push(line);
        return true;
    }

    if (action == CALL_FORWARD_ACTION_REGISTER) {
        char number[MODEM_PHONE_MAX + 1u] = {0};
        unsigned number_type = 0u;
        int end = 0;
        int fields = sscanf(command + consumed, ",\"%32[+0-9]\",%u,1%n",
                            number, &number_type, &end);
        if (fields != 2 || command[consumed + end] != '\0' ||
            (number_type != 129u && number_type != 145u) ||
            ((number_type == 145u) != (number[0] == '+'))) {
            return false;
        }
        unsigned first = reason == CALL_FORWARD_REASON_ALL
            ? CALL_FORWARD_REASON_UNCONDITIONAL
            : (reason == CALL_FORWARD_REASON_ALL_CONDITIONAL
                   ? CALL_FORWARD_REASON_BUSY : reason);
        unsigned last = (reason == CALL_FORWARD_REASON_ALL ||
                         reason == CALL_FORWARD_REASON_ALL_CONDITIONAL)
            ? CALL_FORWARD_REASON_NOT_REACHABLE : reason;
        for (unsigned target = first; target <= last; target++) {
            s_call_forward_active[target] = true;
            snprintf(s_call_forward_number[target],
                     sizeof(s_call_forward_number[target]), "%s", number);
            s_call_forward_delay[target] = 0u;
        }
        return true;
    }

    if (action == CALL_FORWARD_ACTION_ENABLE) {
        char number[MODEM_PHONE_MAX + 1u] = {0};
        unsigned number_type = 0u;
        unsigned delay = 0u;
        int end = 0;
        bool has_number = false;
        bool has_delay = false;
        int fields = sscanf(command + consumed,
                            ",\"%32[+0-9]\",%u,1,%u%n",
                            number, &number_type, &delay, &end);
        if (fields == 3 && command[consumed + end] == '\0') {
            has_number = true;
            has_delay = true;
        } else {
            delay = 0u;
            fields = sscanf(command + consumed, ",\"%32[+0-9]\",%u,1%n",
                            number, &number_type, &end);
            if (fields == 2 && command[consumed + end] == '\0') {
                has_number = true;
            } else {
                fields = sscanf(command + consumed, ",,,1,%u%n", &delay,
                                &end);
                if (fields == 1 && command[consumed + end] == '\0') {
                    has_delay = true;
                } else {
                    if (strcmp(command + consumed, ",,,1") != 0) {
                        return false;
                    }
                    delay = 0u;
                }
            }
        }
        if ((has_number &&
             ((number_type != 129u && number_type != 145u) ||
              ((number_type == 145u) != (number[0] == '+')))) ||
            (has_delay &&
             (reason != CALL_FORWARD_REASON_NO_REPLY || delay < 5u ||
              delay > 30u || (delay % 5u) != 0u))) {
            return false;
        }
        unsigned first = reason == CALL_FORWARD_REASON_ALL
            ? CALL_FORWARD_REASON_UNCONDITIONAL
            : (reason == CALL_FORWARD_REASON_ALL_CONDITIONAL
                   ? CALL_FORWARD_REASON_BUSY : reason);
        unsigned last = (reason == CALL_FORWARD_REASON_ALL ||
                         reason == CALL_FORWARD_REASON_ALL_CONDITIONAL)
            ? CALL_FORWARD_REASON_NOT_REACHABLE : reason;
        for (unsigned target = first; target <= last; target++) {
            s_call_forward_active[target] = true;
            if (has_number) {
                snprintf(s_call_forward_number[target],
                         sizeof(s_call_forward_number[target]), "%s", number);
            }
            if (has_delay) {
                s_call_forward_delay[target] = (uint8_t)delay;
            }
        }
        return true;
    }

    if (strcmp(command + consumed, ",,,1") != 0 ||
        (action != CALL_FORWARD_ACTION_DISABLE &&
         action != CALL_FORWARD_ACTION_ERASE)) {
        return false;
    }
    unsigned first = reason == CALL_FORWARD_REASON_ALL
        ? CALL_FORWARD_REASON_UNCONDITIONAL
        : (reason == CALL_FORWARD_REASON_ALL_CONDITIONAL
               ? CALL_FORWARD_REASON_BUSY : reason);
    unsigned last = (reason == CALL_FORWARD_REASON_ALL ||
                     reason == CALL_FORWARD_REASON_ALL_CONDITIONAL)
        ? CALL_FORWARD_REASON_NOT_REACHABLE : reason;
    for (unsigned target = first; target <= last; target++) {
        s_call_forward_active[target] = false;
        if (action == CALL_FORWARD_ACTION_ERASE) {
            s_call_forward_number[target][0] = '\0';
            s_call_forward_delay[target] = 0u;
        }
    }
    return true;
}

static void telit_push_phonebook(void) {
    for (uint16_t i = 0u; i < TELIT_PHONEBOOK_FIXTURE_MAX; i++) {
        if (!s_phonebook[i].used) {
            continue;
        }
        const modem_phonebook_entry_t *entry = &s_phonebook[i].entry;
        char line[128];
        snprintf(line, sizeof(line), "+CPBR: %u,\"%s\",%u,\"%s\"",
                 (unsigned)entry->index, entry->number,
                 entry->number[0] == '+' ? 145u : 129u, entry->name);
        mh_rx_push(line);
    }
    if (s_phonebook_inject_malformed) {
        mh_rx_push("+CPBR: malformed");
    }
    if (s_phonebook_inject_oversized) {
        mh_rx_push(
            "+CPBR: 250,\"1234567890123456789012345678901234567890\",129,"
            "\"ABCDEFGHIJKLMNOPQRSTUVWXYZABCDEFGHIJKLMN\"");
    }
    if (s_phonebook_inject_high_index) {
        mh_rx_push(
            "+CPBR: 500,\"5550500\",129,\"Last contact\"");
    }
}

static bool telit_apply_phonebook_write(const char *command) {
    unsigned index = 0u;
    unsigned toa = 0u;
    int consumed = 0;
    char number[MODEM_PHONE_MAX + 1u] = {0};
    char name[MODEM_PHONEBOOK_NAME_MAX + 1u] = {0};

    if (sscanf(command, "AT+CPBW=%u%n", &index, &consumed) == 1 &&
        command[consumed] == '\0') {
        if (index == 0u || index > TELIT_PHONEBOOK_FIXTURE_MAX) {
            return false;
        }
        memset(&s_phonebook[index - 1u], 0,
               sizeof(s_phonebook[index - 1u]));
        return true;
    }

    bool add = strncmp(command, "AT+CPBW=,", 9u) == 0;
    int fields;
    if (add) {
        fields = sscanf(command, "AT+CPBW=,\"%32[^\"]\",%u,\"%24[^\"]\"%n",
                        number, &toa, name, &consumed);
    } else {
        fields = sscanf(command, "AT+CPBW=%u,\"%32[^\"]\",%u,\"%24[^\"]\"%n",
                        &index, number, &toa, name, &consumed);
    }
    if (fields != (add ? 3 : 4) || command[consumed] != '\0' ||
        (toa != 129u && toa != 145u) ||
        ((toa == 145u) != (number[0] == '+'))) {
        return false;
    }
    if (add) {
        for (index = 1u; index <= TELIT_PHONEBOOK_FIXTURE_MAX; index++) {
            if (!s_phonebook[index - 1u].used) {
                break;
            }
        }
    }
    if (index == 0u || index > TELIT_PHONEBOOK_FIXTURE_MAX) {
        return false;
    }
    telit_phonebook_fixture_entry_t *slot = &s_phonebook[index - 1u];
    memset(slot, 0, sizeof(*slot));
    slot->used = true;
    slot->entry.index = (uint16_t)index;
    snprintf(slot->entry.number, sizeof(slot->entry.number), "%s", number);
    snprintf(slot->entry.name, sizeof(slot->entry.name), "%s", name);
    return true;
}

static void telit_sms_inject_crossed_urcs(void) {
    if (!s_sms_inject_crossed_urcs) {
        return;
    }
    s_sms_inject_crossed_urcs = false;
    mh_rx_push("+CMTI: \"ME\",42");
    mh_rx_push("RING");
}

static bool telit_sms_response(const char *command) {
    if (strcmp(command, "AT+CMGF=0") == 0) {
        if (s_sms_flow_fault == SMS_FLOW_FAULT_CMGF_PDU_ERROR) {
            s_mh_final = MH_FINAL_ERROR;
        } else if (s_sms_flow_fault == SMS_FLOW_FAULT_CMGF_PDU_TIMEOUT) {
            s_mh_final = MH_FINAL_NONE;
        }
        return true;
    }
    if (strcmp(command, "AT+CMGF=1") == 0) {
        if (s_sms_cmgf_text_error_budget != 0u) {
            s_sms_cmgf_text_error_budget--;
            s_mh_final = MH_FINAL_ERROR;
        } else if (s_sms_flow_fault == SMS_FLOW_FAULT_CMGF_TEXT_ERROR ||
            (s_sms_fail_cmgf_text_when_ready &&
             service_probe().state == MODEM_SERVICE_TEST_STATE_READY)) {
            s_mh_final = MH_FINAL_ERROR;
        } else if (s_sms_flow_fault == SMS_FLOW_FAULT_CMGF_TEXT_TIMEOUT) {
            s_mh_final = MH_FINAL_NONE;
        }
        return true;
    }
    if (strncmp(command, "AT+CMGS=", sizeof("AT+CMGS=") - 1u) == 0) {
        telit_sms_inject_crossed_urcs();
        if (s_sms_flow_fault == SMS_FLOW_FAULT_CMGS_PROMPT_ERROR) {
            s_mh_final = MH_FINAL_ERROR;
        } else {
            s_mh_final = MH_FINAL_NONE;
            if (s_sms_flow_fault != SMS_FLOW_FAULT_CMGS_PROMPT_TIMEOUT) {
                static const uint8_t prompt = '>';
                s_sms_prompt = command[sizeof("AT+CMGS=") - 1u] == '"'
                    ? SMS_PROMPT_CMGS_TEXT
                    : SMS_PROMPT_CMGS_BINARY;
                mh_rx_push_raw(&prompt, 1u);
            }
        }
        return true;
    }
    if (strncmp(command, "AT+CMGW", sizeof("AT+CMGW") - 1u) == 0) {
        telit_sms_inject_crossed_urcs();
        if (s_sms_flow_fault == SMS_FLOW_FAULT_CMGW_PROMPT_ERROR) {
            s_mh_final = MH_FINAL_ERROR;
        } else if (s_sms_flow_fault == SMS_FLOW_FAULT_CMGW_PROMPT_MEMORY_FULL) {
            s_mh_final = MH_FINAL_NONE;
            mh_rx_push("+CMS ERROR: 322");
        } else {
            s_mh_final = MH_FINAL_NONE;
            if (s_sms_flow_fault != SMS_FLOW_FAULT_CMGW_PROMPT_TIMEOUT) {
                static const uint8_t prompt = '>';
                s_sms_prompt = SMS_PROMPT_CMGW;
                mh_rx_push_raw(&prompt, 1u);
            }
        }
        return true;
    }
    if (strncmp(command, "AT+CMGD=", sizeof("AT+CMGD=") - 1u) == 0 &&
        (s_sms_flow_fault == SMS_FLOW_FAULT_DELETE_SECOND_ERROR ||
         s_sms_inject_crossed_urcs)) {
        if (s_sms_flow_fault == SMS_FLOW_FAULT_DELETE_SECOND_ERROR) {
            s_sms_delete_command_count++;
        }
        telit_sms_inject_crossed_urcs();
        if (s_sms_flow_fault == SMS_FLOW_FAULT_DELETE_SECOND_ERROR &&
            s_sms_delete_command_count == 2u) {
            s_mh_final = MH_FINAL_ERROR;
        }
        return true;
    }
    return false;
}

static void telit_sms_raw_write(const uint8_t *data, size_t len) {
    if (data == NULL) {
        return;
    }
    if (len == 1u && data[0] == 0x1bu) {
        if (s_sms_esc_returns_final) {
            mh_rx_push("OK");
        }
        return;
    }
    if (len != 1u || data[0] != 0x1au || s_sms_prompt == SMS_PROMPT_NONE) {
        return;
    }

    sms_prompt_t prompt = s_sms_prompt;
    s_sms_prompt = SMS_PROMPT_NONE;
    bool cmgw = prompt == SMS_PROMPT_CMGW;
    sms_flow_fault_t error_fault = cmgw
        ? SMS_FLOW_FAULT_CMGW_FINAL_ERROR
        : SMS_FLOW_FAULT_CMGS_FINAL_ERROR;
    sms_flow_fault_t timeout_fault = cmgw
        ? SMS_FLOW_FAULT_CMGW_FINAL_TIMEOUT
        : SMS_FLOW_FAULT_CMGS_FINAL_TIMEOUT;
    if (s_sms_flow_fault == timeout_fault) {
        return;
    }
    if (s_sms_flow_fault == error_fault) {
        mh_rx_push("ERROR");
        return;
    }
    if (cmgw && s_sms_flow_fault == SMS_FLOW_FAULT_CMGW_FINAL_MEMORY_FULL) {
        mh_rx_push("+CMS ERROR: memory full");
        return;
    }
    mh_rx_push(cmgw ? "+CMGW: 23" : "+CMGS: 23");
    mh_rx_push("OK");
}

static void telit_response(const char *command) {
    if (strcmp(command, "AT") == 0 &&
        (s_fault == TELIT_FAULT_REBOOT_RI_BEFORE_DROP ||
         s_fault == TELIT_FAULT_REBOOT_RI_LOST_FINAL) &&
        !s_mh_status_drop_pending && !s_mh_status_restore_pending) {
        s_mh_dtr_wake_works = true;
        s_mh_cts_asserted = true;
        s_mh_ri_asserted = false;
    }
    if (s_startup_drops_remaining != 0u &&
        strcmp(command, s_startup_drop_command) == 0) {
        s_startup_drops_remaining--;
        s_mh_status_drop_pending = true;
        s_mh_status_drop_ms = s_mh_now;
        s_mh_status_drop_duration_ms = 1500u;
        s_mh_final = MH_FINAL_NONE;
        return;
    }
    if (s_hold_final_command != NULL &&
        strcmp(command, s_hold_final_command) == 0) {
        s_mh_final = MH_FINAL_NONE;
        return;
    }
    if (telit_sms_response(command)) {
        return;
    }
    if (strcmp(command, "AT+CLCC") == 0) {
        if (s_clcc_row != NULL) {
            mh_rx_push(s_clcc_row);
        }
    } else if (strcmp(command, "AT#RFSTS") == 0) {
        if (s_diag_rfsts_hold_once) {
            s_diag_rfsts_hold_once = false;
            s_mh_final = MH_FINAL_NONE;
        } else if (s_diag_rfsts_malformed_once) {
            s_diag_rfsts_malformed_once = false;
            mh_rx_push("#RFSTS: malformed");
        } else {
            mh_rx_push(s_rfsts_response != NULL
                ? s_rfsts_response
                : "#RFSTS: \"310 410\",5230,-101,-82,-11.5,00AF,,32,3,1,"
                  "ABCDEF01,\"310410123456789\",\"AT&T\",2,12,100");
        }
    } else if (strcmp(command, "AT#MONI") == 0) {
        if (s_diag_moni_hold_once) {
            s_diag_moni_hold_once = false;
            s_mh_final = MH_FINAL_NONE;
        } else {
            mh_rx_push(
                "#MONI: AT&T RSRP:-101 RSRQ:-11 TAC:00AF Id:ABCDEF01 "
                "EARFCN:5230 PWR:-82dbm DRX:32 pci:321 QRxLevMin:-130");
        }
    } else if (strcmp(command, "AT+CGATT?") == 0) {
        mh_rx_push("+CGATT: 1");
    } else if (strcmp(command, "AT+CGACT?") == 0) {
        mh_rx_push("+CGACT: 1,1");
        mh_rx_push("+CGACT: 2,1");
        mh_rx_push("+CGACT: 3,0");
        mh_rx_push("+CGACT: 4,0");
    } else if (strcmp(command, "AT+CGCONTRDP") == 0) {
        mh_rx_push(
            "+CGCONTRDP: 1,5,\"apn.example\","
            "\"192.0.2.168.255.255.255.240\" "
            "\"32.1.13.184.0.18.0.52.0.0.0.0.0.0.0.1.255.255.255.255.255.255.255.255.0.0.0.0.0.0.0.0\","
            "\"192.0.2.169\" "
            "\"254.128.0.0.0.0.0.0.0.0.0.0.0.0.1.64\"");
        mh_rx_push(
            "+CGCONTRDP: 2,6,\"ims\","
            "\"32.1.13.184.0.18.0.53.0.0.0.0.0.0.0.2.255.255.255.255.255.255.255.255.0.0.0.0.0.0.0.0\","
            "\"254.128.0.0.0.0.0.0.0.0.0.0.0.0.2.64\"");
    } else if (strcmp(command, "AT+CGMM") == 0) {
        mh_rx_push("LE910C1-WWX");
    } else if (strcmp(command, "AT+CGMR") == 0) {
        if (s_diag_firmware_hold_once) {
            s_diag_firmware_hold_once = false;
            s_mh_final = MH_FINAL_NONE;
        } else {
            mh_rx_push("M0F.660010");
        }
    } else if (strcmp(command, "AT+CREG?") == 0) {
        mh_rx_push("+CREG: 2,1,\"00AF\",\"ABCDEF01\",7");
    } else if (strcmp(command, "AT+CGREG?") == 0) {
        mh_rx_push("+CGREG: 2,1,\"00AF\",\"ABCDEF01\",7");
    } else if (strcmp(command, "AT+CEREG?") == 0) {
        mh_rx_push("+CEREG: 2,1,\"00AF\",\"ABCDEF01\",7");
    } else if (strcmp(command, "AT+COPS?") == 0) {
        if (s_cops_response != NULL) {
            mh_rx_push(s_cops_response);
        }
    } else if (strcmp(command, g_modem_vendor.sim_provider_query.query_cmd) == 0) {
        if (s_sim_provider_response != NULL) {
            mh_rx_push(s_sim_provider_response);
        }
        if (s_sim_provider_error) {
            s_mh_final = MH_FINAL_ERROR;
        }
    } else if (strcmp(command, "AT+CIREG?") == 0) {
        mh_rx_push("+CIREG: 2,1");
    } else if (strcmp(command, "AT#FWAUTOSIM?") == 0) {
        char line[32];
        snprintf(line, sizeof(line), "#FWAUTOSIM: %u",
                 (unsigned)s_auto_profile);
        mh_rx_push(s_fault == TELIT_FAULT_AUTO_PROFILE_READBACK_MALFORMED
                       ? "#FWAUTOSIM: bad" : line);
    } else if (strcmp(command, "AT#FWAUTOSIM=1") == 0) {
        if (s_fault == TELIT_FAULT_AUTO_PROFILE_SET_ERROR) {
            s_mh_final = MH_FINAL_ERROR;
        } else {
            if (s_fault != TELIT_FAULT_AUTO_PROFILE_VERIFY_MISMATCH) {
                s_auto_profile = 1u;
            }
            if (s_fault == TELIT_FAULT_AUTO_PROFILE_SET_TIMEOUT_ONCE &&
                !s_fault_consumed) {
                s_fault_consumed = true;
                s_mh_final = MH_FINAL_NONE;
            }
        }
    } else if (strcmp(command, "AT#STUNEANT=?") == 0) {
        mh_rx_push("#STUNEANT: (0,1),(7FD9FFFF),(0,1),(0,1)");
    } else if (strcmp(command, "AT#STUNEANT?") == 0) {
        mh_rx_push(s_stune_enabled ? "#STUNEANT: 1" : "#STUNEANT: 0");
    } else if (strcmp(command, "AT#GTUNEANT?") == 0) {
        if (s_tune_query_errors > 0u) {
            s_tune_query_errors--;
            mh_rx_push("+CME ERROR: operation not supported");
        } else if (s_stune_enabled) {
            telit_push_tune_table();
        } else {
            s_mh_final = MH_FINAL_ERROR;
        }
    } else if (strcmp(command, "AT#STUNEANT=0") == 0) {
        s_stune_enabled = false;
    } else if (strncmp(command, "AT#STUNEANT=1,", 14u) == 0) {
        if (!telit_apply_tune_row(command)) {
            s_mh_final = MH_FINAL_ERROR;
        }
    } else if (strcmp(command, "AT#GPIO=2,2") == 0) {
        char line[32];
        unsigned direction = s_gpio2_direction;
        bool state = s_gpio2_state;
        if (s_fault == TELIT_FAULT_MAINT_GPIO2_ALT_READBACK &&
            direction == 17u) {
            direction = 0u;
        }
        if (s_fault == TELIT_FAULT_MAINT_GPIO2_INPUT_HIGH &&
            direction == 0u) {
            state = true;
        }
        snprintf(line, sizeof(line), "#GPIO: %u,%u", direction,
                 state ? 1u : 0u);
        mh_rx_push(line);
    } else if (strcmp(command, "AT#GPIO=3,2") == 0) {
        char line[32];
        snprintf(line, sizeof(line), "#GPIO: %u,%u",
                 (unsigned)s_gpio3_direction, s_gpio3_state ? 1u : 0u);
        mh_rx_push(line);
    } else if (strcmp(command, "AT#GPIO=2,0,17") == 0) {
        s_gpio2_direction = 17u;
        s_gpio2_state = false;
    } else if (strcmp(command, "AT#GPIO=3,0,18") == 0) {
        s_gpio3_direction = 18u;
        s_gpio3_state = false;
    } else if (strncmp(command, "AT#GPIO=", 8u) == 0) {
        if (!telit_apply_gpio_set(command)) {
            s_mh_final = MH_FINAL_ERROR;
        }
    } else if (strcmp(command, "AT#NWSCANTMR?") == 0) {
        char line[32];
        snprintf(line, sizeof(line), "#NWSCANTMR: %u",
                 (unsigned)s_scan_timer_s);
        mh_rx_push(s_fault == TELIT_FAULT_SCAN_READBACK_MALFORMED
                       ? "#NWSCANTMR: bad" : line);
    } else if (strncmp(command, "AT#NWSCANTMR=",
                       sizeof("AT#NWSCANTMR=") - 1u) == 0) {
        unsigned seconds = 0u;
        int consumed = 0;
        if (s_fault == TELIT_FAULT_SCAN_SET_ERROR ||
            sscanf(command, "AT#NWSCANTMR=%u%n", &seconds, &consumed) != 1 ||
            command[consumed] != '\0' || seconds < 5u || seconds > 3600u) {
            s_mh_final = MH_FINAL_ERROR;
        } else {
            if (s_fault != TELIT_FAULT_SCAN_VERIFY_MISMATCH) {
                s_scan_timer_s = (uint16_t)seconds;
            }
            if (s_fault == TELIT_FAULT_SCAN_SET_TIMEOUT_ONCE &&
                !s_fault_consumed) {
                s_fault_consumed = true;
                s_mh_final = MH_FINAL_NONE;
            }
        }
    } else if (strcmp(command, "AT#SELBNDMODE?") == 0) {
        char line[32];
        snprintf(line, sizeof(line), "#SELBNDMODE: %u",
                 (unsigned)s_band_mode);
        mh_rx_push(line);
    } else if (strncmp(command, "AT#SELBNDMODE=",
                       sizeof("AT#SELBNDMODE=") - 1u) == 0) {
        unsigned mode = 0u;
        int consumed = 0;
        if (sscanf(command, "AT#SELBNDMODE=%u%n", &mode, &consumed) != 1 ||
            command[consumed] != '\0' || mode > 1u) {
            s_mh_final = MH_FINAL_ERROR;
        } else {
            s_band_mode = (uint8_t)mode;
        }
    } else if (strcmp(command, "AT#BND?") == 0) {
        char line[64];
        snprintf(line, sizeof(line), "#BND: %u,%u,%llX",
                 (unsigned)s_band_nvm.gsm, (unsigned)s_band_nvm.wcdma,
                 (unsigned long long)s_band_nvm.lte);
        mh_rx_push(line);
    } else if (strcmp(command, "AT#BNDRAM?") == 0) {
        char line[64];
        snprintf(line, sizeof(line), "#BNDRAM: %u,%u,%llX",
                 (unsigned)s_band_ram.gsm, (unsigned)s_band_ram.wcdma,
                 (unsigned long long)s_band_ram.lte);
        mh_rx_push(line);
    } else if (strncmp(command, "AT#BNDRAM=",
                       sizeof("AT#BNDRAM=") - 1u) == 0) {
        if (!telit_apply_band_ram(command)) {
            s_mh_final = MH_FINAL_ERROR;
        }
    } else if (strcmp(command, "AT+CFUN?") == 0) {
        char line[24];
        snprintf(line, sizeof(line), "+CFUN: %u", (unsigned)s_cfun);
        mh_rx_push(line);
    } else if (strcmp(command, "AT+CFUN=4") == 0) {
        s_cfun = 4u;
        s_mh_sim_interface_active = false;
        s_psmri_live_armed = false;
    } else if (strcmp(command, "AT+CFUN=1") == 0) {
        s_cfun = 1u;
        s_mh_sim_interface_active = true;
        s_psmri_live_armed = false;
    } else if (strcmp(command, "AT+CFUN=5") == 0) {
        bool crossed_from_full_functionality = s_cfun == 1u;
        s_cfun = 5u;
        s_mh_sim_interface_active = true;
        s_psmri_live_armed = crossed_from_full_functionality &&
            !s_wkio_configured && s_e2smsri_ms == 0u &&
            s_psmri_ms == 1000u && s_cnmi_mode == 2u;
        if (s_qss_response_enabled) {
            if (!s_mh_sim_present) {
                mh_rx_push("#QSS: 2,0");
            } else if (s_mh_sim_ready) {
                mh_rx_push("#QSS: 2,2");
                mh_rx_push("#QSS: 2,3");
            } else {
                mh_rx_push("#QSS: 2,1");
            }
        }
        if (crossed_from_full_functionality &&
            s_fault == TELIT_FAULT_FINAL_CFUN_SET_TIMEOUT_ONCE &&
            !s_fault_consumed) {
            s_fault_consumed = true;
            s_mh_final = MH_FINAL_NONE;
        }
    } else if (strcmp(command, "AT#QSS=2") == 0) {
        if (s_inject_imei_on_qss_set) {
            mh_rx_push("490154203237518");
        }
    } else if (strcmp(command, "AT+CSDH=1") == 0) {
        if (!s_mh_sim_interface_active || !s_mh_sim_present) {
            mh_rx_push("+CMS ERROR: SIM not inserted");
            s_mh_final = MH_FINAL_NONE;
        }
    } else if (strcmp(command, "AT+CNMI=2,2,0,0,0") == 0) {
        if (!s_mh_sim_interface_active || !s_mh_sim_present) {
            mh_rx_push("+CMS ERROR: SIM not inserted");
            s_mh_final = MH_FINAL_NONE;
        } else {
            s_cnmi_mode = 2u;
        }
    } else if (strcmp(command, "AT#QSS?") == 0) {
        if (s_qss_response_enabled) {
            if (!s_mh_sim_interface_active || !s_mh_sim_present) {
                mh_rx_push("#QSS: 2,0");
            } else if (s_mh_sim_ready) {
                mh_rx_push("#QSS: 2,3");
            } else {
                mh_rx_push("#QSS: 2,1");
            }
        }
    } else if (strcmp(command, "AT#RXDIV?") == 0) {
        if (s_fault == TELIT_FAULT_RXDIV_READBACK_MALFORMED) {
            mh_rx_push("#RXDIV: malformed");
        } else {
            mh_rx_push(s_rxdiv_configured
                ? "#RXDIV: 0,1" : "#RXDIV: 1,1");
        }
    } else if (strcmp(command, "AT#SLED?") == 0) {
        char line[32];
        snprintf(line, sizeof(line), "#SLED: %u,10,10",
                 (unsigned)s_sled_mode);
        mh_rx_push(line);
    } else if (strcmp(command, "AT$GPSPSAV?") == 0) {
        mh_rx_push("$GPSPSAV: 0");
    } else if (strcmp(command, "AT$GPSP?") == 0) {
        mh_rx_push("$GPSP: 0");
    } else if (strcmp(command, "AT#DVIEXT?") == 0) {
        mh_rx_push(s_dviext_configured
            ? "#DVIEXT: 1,1,0,0,0" : "#DVIEXT: 0,0,0,0,0");
    } else if (strcmp(command, "AT#DVI?") == 0) {
        mh_rx_push(s_dvi_configured ? "#DVI: 1,2,1" : "#DVI: 0,2,1");
    } else if (strcmp(command, "AT#WKIO?") == 0) {
        mh_rx_push(s_wkio_configured ? "#WKIO: 1,0,2,1"
                                     : "#WKIO: 0,0,2,1");
    } else if (strcmp(command, "AT&V") == 0) {
        mh_rx_push(s_ring_pulse_mode
            ? "RI (C125) OPTIONS : \\R2=follows ring"
            : "RI (C125) OPTIONS : \\R1=follows ring");
    } else if (strcmp(command, "AT#E2SMSRI?") == 0) {
        char line[32];
        snprintf(line, sizeof(line), "#E2SMSRI: %u",
                 (unsigned)s_e2smsri_ms);
        mh_rx_push(line);
    } else if (strcmp(command, "AT#PSMRI?") == 0) {
        char line[32];
        snprintf(line, sizeof(line), "#PSMRI: %u",
                 (unsigned)s_psmri_ms);
        mh_rx_push(line);
    } else if (strcmp(command, "AT+CPBS=\"ME\"") == 0) {
        if (!s_fault_consumed &&
            (s_fault == TELIT_FAULT_PHONEBOOK_CPBS_ERROR_ONCE ||
             s_fault == TELIT_FAULT_PHONEBOOK_CPBS_TIMEOUT_ONCE)) {
            s_fault_consumed = true;
            s_mh_final =
                s_fault == TELIT_FAULT_PHONEBOOK_CPBS_ERROR_ONCE
                    ? MH_FINAL_ERROR : MH_FINAL_NONE;
        }
    } else if (strcmp(command, "AT+CPBR=1,500") == 0) {
        if (!s_fault_consumed &&
            (s_fault == TELIT_FAULT_PHONEBOOK_CPBR_ERROR_ONCE ||
             s_fault == TELIT_FAULT_PHONEBOOK_CPBR_TIMEOUT_ONCE)) {
            s_fault_consumed = true;
            s_mh_final =
                s_fault == TELIT_FAULT_PHONEBOOK_CPBR_ERROR_ONCE
                    ? MH_FINAL_ERROR : MH_FINAL_NONE;
        } else {
            telit_push_phonebook();
        }
    } else if (strncmp(command, "AT+CPBW=", 8u) == 0) {
        if (!s_fault_consumed &&
            (s_fault == TELIT_FAULT_PHONEBOOK_CPBW_ERROR_ONCE ||
             s_fault == TELIT_FAULT_PHONEBOOK_CPBW_TIMEOUT_ONCE)) {
            s_fault_consumed = true;
            s_mh_final =
                s_fault == TELIT_FAULT_PHONEBOOK_CPBW_ERROR_ONCE
                    ? MH_FINAL_ERROR : MH_FINAL_NONE;
        } else if (!telit_apply_phonebook_write(command)) {
            s_mh_final = MH_FINAL_ERROR;
        }
    } else if (strcmp(command, "AT+CPMS?") == 0) {
        mh_rx_push(s_cpms_configured
            ? "+CPMS: \"ME\",0,255,\"ME\",0,255,\"ME\",0,255"
            : "+CPMS: \"SM\",0,20,\"SM\",0,20,\"SM\",0,20");
    } else if (strcmp(command, "AT+CSSN?") == 0) {
        mh_rx_push(s_cssn_configured ? "+CSSN: 1,1" : "+CSSN: 0,0");
    } else if (strcmp(command, "AT+CSSN=1,1") == 0) {
        s_cssn_configured = true;
    } else if (strcmp(command, "AT#CFF?") == 0) {
        if (s_fault == TELIT_FAULT_CFU_FLAGS_ERROR_ONCE && !s_fault_consumed) {
            s_fault_consumed = true;
            s_mh_final = MH_FINAL_ERROR;
            return;
        }
        if (s_cff_flags_absent) {
            mh_rx_push(s_cff_configured ? "#CFF: 1" : "#CFF: 0");
            return;
        }
        char line[64];
        snprintf(line, sizeof(line), "#CFF: %u,%u,%s",
                 s_cff_configured ? 1u : 0u,
                 s_call_forward_active[CALL_FORWARD_REASON_UNCONDITIONAL]
                     ? 1u : 0u,
                 s_call_forward_number[CALL_FORWARD_REASON_UNCONDITIONAL]);
        mh_rx_push(line);
    } else if (strcmp(command, "AT#CFF=1") == 0) {
        s_cff_configured = true;
    } else if (strcmp(command, "AT#MBN") == 0) {
        if (s_fault == TELIT_FAULT_MBN_ERROR_ALWAYS) {
            s_mh_final = MH_FINAL_ERROR;
        } else {
            mh_rx_push("#MBN: 1,\"+18005551212\",145,\"Voice mail\",VOICE");
        }
    } else if (strcmp(command, "AT#MWI?") == 0) {
        char line[32];
        if (s_fault == TELIT_FAULT_MWI_ERROR_ONCE && !s_fault_consumed) {
            s_fault_consumed = true;
            s_mh_final = MH_FINAL_ERROR;
        } else if (s_fault == TELIT_FAULT_MWI_URC_DURING_QUERY_ONCE &&
            !s_fault_consumed) {
            s_fault_consumed = true;
            /* Valid URC, invalid read-row grammar: status=1, voice=1, count=7. */
            snprintf(line, sizeof(line), "#MWI: 1,1,7");
        } else if (s_fault ==
                       TELIT_FAULT_MWI_URC_THEN_AMBIGUOUS_QUERY_ONCE &&
                   !s_fault_consumed) {
            s_fault_consumed = true;
            mh_rx_push("#MWI: 1,1,7");
            snprintf(line, sizeof(line), "#MWI: 1,1");
        } else if (s_fault ==
                       TELIT_FAULT_MWI_FAX_URC_THEN_AMBIGUOUS_QUERY_ONCE &&
                   !s_fault_consumed) {
            s_fault_consumed = true;
            mh_rx_push("#MWI: 1,3,2");
            snprintf(line, sizeof(line), "#MWI: 1,1,3");
        } else if (s_mwi_ambiguous_snapshot) {
            for (uint8_t i = MODEM_MESSAGE_WAITING_VOICE_LINE_2;
                 i < MODEM_MESSAGE_WAITING_CATEGORY_COUNT; i++) {
                const modem_message_waiting_state_t *waiting =
                    &s_message_waiting.category[i];
                if (waiting->active) {
                    snprintf(line, sizeof(line), "#MWI: 1,1,%u,%u",
                             (unsigned)TELIT_MWI_INDICATOR_BY_CATEGORY[i],
                             (unsigned)waiting->count);
                    mh_rx_push(line);
                }
            }
            snprintf(line, sizeof(line), "#MWI: 1,1");
        } else {
            bool any = false;
            for (uint8_t i = 0u; i < MODEM_MESSAGE_WAITING_CATEGORY_COUNT;
                 i++) {
                const modem_message_waiting_state_t *waiting =
                    &s_message_waiting.category[i];
                if (!waiting->active) {
                    continue;
                }
                snprintf(line, sizeof(line), "#MWI: 1,1,%u,%u",
                         (unsigned)TELIT_MWI_INDICATOR_BY_CATEGORY[i],
                         (unsigned)waiting->count);
                mh_rx_push(line);
                any = true;
            }
            if (!any) {
                mh_rx_push("#MWI: 1,0");
            }
            return;
        }
        mh_rx_push(line);
    } else if (strncmp(command, "AT+CCFC=", sizeof("AT+CCFC=") - 1u) == 0) {
        if (s_fault == TELIT_FAULT_CALL_FORWARD_ERROR_ONCE &&
            !s_fault_consumed) {
            s_fault_consumed = true;
            s_mh_final = MH_FINAL_ERROR;
        } else if (s_fault == TELIT_FAULT_CALL_FORWARD_SECOND_ERROR_ONCE &&
                   !s_fault_consumed &&
                   strncmp(command, "AT+CCFC=2,1,,,1,",
                           sizeof("AT+CCFC=2,1,,,1,") - 1u) == 0) {
            s_fault_consumed = true;
            s_mh_final = MH_FINAL_ERROR;
        } else if (!telit_apply_call_forward(command)) {
            s_mh_final = MH_FINAL_ERROR;
        } else if (s_fault == TELIT_FAULT_CALL_FORWARD_TIMEOUT_ONCE &&
                   !s_fault_consumed) {
            s_fault_consumed = true;
            s_mh_final = MH_FINAL_NONE;
        }
    } else if (strcmp(command, "AT#SMSUCS=1") == 0) {
        s_sms_keep_unread = true;
    } else if (strcmp(command, "AT#SMSUCS=0") == 0) {
        s_sms_keep_unread = false;
        if (s_fault == TELIT_FAULT_SMS_STATUS_CONSUME_TIMEOUT_ONCE &&
            !s_fault_consumed) {
            s_fault_consumed = true;
            s_mh_final = MH_FINAL_NONE;
        }
    } else if (strcmp(command, "AT+CMGL=4") == 0 &&
               s_sms_test_row_enabled) {
        mh_rx_push(s_sms_test_row_unread ? "+CMGL: 7,0,\"\",21"
                                         : "+CMGL: 7,1,\"\",21");
        if (s_sms_test_cmti_interleave) {
            mh_rx_push("+CMTI: \"ME\",7");
        }
        mh_rx_push(s_sms_test_pdu);
        if (!s_sms_keep_unread) {
            s_sms_test_row_unread = false;
        }
    } else if (strcmp(command, "AT+CMGR=7") == 0 &&
               s_sms_test_row_enabled) {
        mh_rx_push(s_sms_test_row_unread ? "+CMGR: 0,\"\",21"
                                         : "+CMGR: 1,\"\",21");
        mh_rx_push(s_sms_test_pdu);
        if (!s_sms_keep_unread) {
            s_sms_test_row_unread = false;
        }
    } else if (strcmp(command, "AT+CMGD=7") == 0) {
        if (s_fault == TELIT_FAULT_VVM_DELETE_ERROR_ONCE &&
            !s_fault_consumed) {
            s_fault_consumed = true;
            s_mh_final = MH_FINAL_ERROR;
        } else if (s_fault == TELIT_FAULT_VVM_DELETE_TIMEOUT_ONCE &&
                   !s_fault_consumed) {
            s_fault_consumed = true;
            s_mh_final = MH_FINAL_NONE;
        } else {
            s_sms_test_row_enabled = false;
        }
    } else if (strcmp(command, "AT#ECAMURC?") == 0) {
        mh_rx_push(s_ecamurc_configured
            ? "#ECAMURC: 1" : "#ECAMURC: 0");
    } else if (strcmp(command, "AT#RXDIV=0,1") == 0) {
        if (s_fault == TELIT_FAULT_RXDIV_SET_ERROR) {
            s_mh_final = MH_FINAL_ERROR;
        } else {
            if (s_fault != TELIT_FAULT_RXDIV_VERIFY_MISMATCH) {
                s_rxdiv_configured = true;
            }
            if (s_fault == TELIT_FAULT_RXDIV_SET_TIMEOUT_ONCE &&
                !s_fault_consumed) {
                s_fault_consumed = true;
                s_mh_final = MH_FINAL_NONE;
            }
        }
    } else if (strcmp(command, "AT#SLED=5;#SLEDSAV") == 0) {
        s_sled_mode = 5u;
        s_sled_saved = true;
    } else if (strcmp(command,
                      "AT+CPMS=\"ME\",\"ME\",\"ME\"") == 0) {
        if (s_sms_flow_fault == SMS_FLOW_FAULT_CPMS_ERROR ||
            s_fault == TELIT_FAULT_CPMS_SET_ERROR) {
            s_mh_final = MH_FINAL_ERROR;
        } else if (s_sms_flow_fault == SMS_FLOW_FAULT_CPMS_TIMEOUT) {
            s_mh_final = MH_FINAL_NONE;
        } else {
            s_cpms_configured = true;
        }
    } else if (strcmp(command, "AT#WKIO=0") == 0) {
        s_wkio_configured = false;
    } else if (strcmp(command,
                      "AT#WKIO=0;#E2SMSRI=0;\\R2&W0") == 0) {
        if (s_fault == TELIT_FAULT_SMS_WAKE_PROFILE_SET_ERROR) {
            s_mh_final = MH_FINAL_ERROR;
        } else {
            s_e2smsri_ms = 0u;
            s_ring_pulse_mode = true;
            s_wkio_configured = false;
        }
        if (s_fault == TELIT_FAULT_SMS_WAKE_PROFILE_SET_TIMEOUT_ONCE &&
            !s_fault_consumed) {
            s_fault_consumed = true;
            s_mh_final = MH_FINAL_NONE;
        }
    } else if (strcmp(command, "AT#E2SMSRI=0") == 0) {
        s_e2smsri_ms = 0u;
    } else if (strcmp(command, "AT#PSMRI=1000") == 0) {
        s_psmri_ms = 1000u;
        if (s_cfun == 5u) {
            /* Bench ground truth: reissuing PSMRI after CFUN=5 preserves its
             * readback but disables the SMS RI wake latch. */
            s_late_psmri_invalidated_latch = s_psmri_live_armed;
            s_psmri_live_armed = false;
        }
    } else if (strcmp(command, "AT\\R2") == 0) {
        s_ring_pulse_mode = true;
    } else if (strcmp(command, "AT#CFLO=1") == 0 &&
               s_factory_flow_blocked) {
        s_factory_flow_blocked = false;
        s_mh_dtr_wake_works = true;
        s_mh_cts_asserted = true;
    } else if (strcmp(command, "AT#ECAMURC=1") == 0) {
        s_ecamurc_configured = true;
    } else if (strcmp(command, "AT#DVIEXT=1,1") == 0) {
        s_dviext_configured = true;
    } else if (strcmp(command, "AT#DVI=1,2,1") == 0) {
        s_dvi_configured = true;
    } else if (strncmp(command, "AT+VTS=", 7u) == 0) {
        s_dtmf_command_count++;
        if (s_dtmf_command_count == 1u &&
            s_dtmf_queue_hangup_on_first) {
            s_dtmf_hangup_admitted = modem_service_request_hangup();
        }
        if (s_dtmf_command_count == 1u &&
            s_dtmf_inject_release_on_first) {
            /* The production Telit adapter deliberately treats ECAM's ccid as
             * coarse. Put the topology event before this digit's final. */
            mh_rx_push("#ECAM: 0,0,1,,,");
        }
        if (!s_fault_consumed && s_dtmf_fault_at != 0u &&
            s_dtmf_command_count == s_dtmf_fault_at &&
            (s_fault == TELIT_FAULT_DTMF_ERROR_ONCE ||
             s_fault == TELIT_FAULT_DTMF_TIMEOUT_ONCE)) {
            s_fault_consumed = true;
            s_mh_final = s_fault == TELIT_FAULT_DTMF_ERROR_ONCE
                ? MH_FINAL_ERROR : MH_FINAL_NONE;
        }
    } else if (strcmp(command, "AT#REBOOT") == 0) {
        s_psmri_live_armed = false;
        /* ThreadX documentation does not guarantee #DVI persistence. Model
         * the conservative case so the post-reboot pass must restore it. */
        s_dvi_configured = false;
        /* Telit acknowledges first, then drops PWRMON later. Model both the
         * ordinary final and lost-final paths against that real ordering. */
        s_mh_status_drop_pending = true;
        s_mh_status_drop_ms = s_mh_now + 500u;
        s_mh_status_drop_duration_ms = 1000u;
        if (s_fault == TELIT_FAULT_REBOOT_RI_BEFORE_DROP ||
            s_fault == TELIT_FAULT_REBOOT_RI_LOST_FINAL) {
            /* Bench ordering: RI/CTS change before the PWRMON reset edge,
             * with more than a normal DTR-wake budget until that edge. */
            s_mh_ri_wake_pending = true;
            s_mh_ri_asserted = true;
            s_mh_cts_asserted = false;
            s_mh_dtr_wake_works = false;
            s_mh_status_drop_ms = s_mh_now + 3000u;
            if (s_fault == TELIT_FAULT_REBOOT_RI_LOST_FINAL) {
                s_mh_final = MH_FINAL_NONE;
            }
        }
        if (s_fault == TELIT_FAULT_REBOOT_TIMEOUT_ONCE &&
            !s_fault_consumed) {
            s_fault_consumed = true;
            s_mh_final = MH_FINAL_NONE;
        }
    } else if (strcmp(command, "AT#SHDN") == 0) {
        s_psmri_live_armed = false;
        if (s_fault != TELIT_FAULT_SHUTDOWN_STUCK_ON) {
            s_mh_status_raw = 36u;
            s_mh_status_mv = 29u;
            s_mh_rx_idle_high = false;
            s_mh_cts_asserted = false;
        }
        if (s_fault == TELIT_FAULT_SHUTDOWN_TIMEOUT) {
            s_mh_final = MH_FINAL_NONE;
        }
    }
}

static void begin_telit(bool rxdiv_configured) {
    mh_begin();
    s_rxdiv_configured = rxdiv_configured;
    s_sled_mode = 5u;
    s_sled_saved = true;
    s_cpms_configured = true;
    s_ecamurc_configured = true;
    s_cssn_configured = true;
    s_cff_configured = true;
    s_cff_flags_absent = false;
    s_dviext_configured = true;
    s_dvi_configured = true;
    s_wkio_configured = false;
    s_ring_pulse_mode = true;
    s_e2smsri_ms = 0u;
    s_psmri_ms = 1000u;
    s_cnmi_mode = 0u;
    s_psmri_live_armed = false;
    s_late_psmri_invalidated_latch = false;
    s_stune_enabled = true;
    s_gpio2_direction = 17u;
    s_gpio3_direction = 18u;
    s_gpio2_state = false;
    s_gpio3_state = false;
    s_cfun = 5u;
    s_scan_timer_s = 60u;
    s_auto_profile = 1u;
    s_startup_drop_command = NULL;
    s_startup_drops_remaining = 0u;
    s_band_mode = 0u;
    s_band_nvm = (modem_band_config_t){
        .gsm = 3u,
        .wcdma = 511u,
        .lte = UINT64_C(0x7FD9FFFF),
    };
    s_band_ram = s_band_nvm;
    s_tune_masks[0] = UINT64_C(0x601840A7);
    s_tune_masks[1] = UINT64_C(0x00010300);
    s_tune_masks[2] = UINT64_C(0x1F80BC50);
    s_tune_masks[3] = UINT64_C(0x00400008);
    s_tune_zero_rows = 0u;
    s_tune_query_errors = 0u;
    s_qss_response_enabled = true;
    s_inject_imei_on_qss_set = false;
    s_factory_flow_blocked = false;
    s_diag_rfsts_malformed_once = false;
    s_diag_rfsts_hold_once = false;
    s_diag_moni_hold_once = false;
    s_diag_firmware_hold_once = false;
    s_rfsts_response = NULL;
    s_cops_response = NULL;
    s_sim_provider_response = NULL;
    s_sim_provider_error = false;
    s_clcc_row = NULL;
    s_sms_keep_unread = false;
    s_sms_test_row_enabled = false;
    s_sms_test_row_unread = true;
    s_sms_test_cmti_interleave = false;
    s_sms_test_pdu =
        "07912121550501F0040B912121550521F300006280315142926905C8721E3403";
    s_sms_flow_fault = SMS_FLOW_FAULT_NONE;
    s_sms_prompt = SMS_PROMPT_NONE;
    s_sms_esc_returns_final = false;
    s_hold_final_command = NULL;
    s_sms_fail_cmgf_text_when_ready = false;
    s_sms_cmgf_text_error_budget = 0u;
    s_sms_inject_crossed_urcs = false;
    s_sms_delete_command_count = 0u;
    memset(&s_message_waiting, 0, sizeof(s_message_waiting));
    s_mwi_ambiguous_snapshot = false;
    memset(s_call_forward_active, 0, sizeof(s_call_forward_active));
    memset(s_call_forward_number, 0, sizeof(s_call_forward_number));
    memset(s_call_forward_delay, 0, sizeof(s_call_forward_delay));
    memset(s_phonebook, 0, sizeof(s_phonebook));
    s_phonebook_inject_malformed = false;
    s_phonebook_inject_oversized = false;
    s_phonebook_inject_high_index = false;
    s_fault = TELIT_FAULT_NONE;
    s_fault_consumed = false;
    s_dtmf_command_count = 0u;
    s_dtmf_fault_at = 0u;
    s_dtmf_queue_hangup_on_first = false;
    s_dtmf_hangup_admitted = false;
    s_dtmf_inject_release_on_first = false;
    s_mh_response_hook = telit_response;
    s_mh_raw_write_hook = telit_sms_raw_write;
}

static void check(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        s_failures++;
    }
}

/* Keep each black-box scenario concise while still retaining the exact public
 * owner returned by admission. Result checks below compare against this token;
 * a later operation cannot consume an older request's terminal. */
static uint32_t s_last_sms_request_id;

static bool request_send_sms(const char *number, const char *text) {
    s_last_sms_request_id = 0u;
    return modem_service_request_send_sms(
        number, text, &s_last_sms_request_id);
}

static bool request_send_binary_sms(const char *number,
                                    const uint8_t *payload,
                                    uint16_t payload_len,
                                    uint16_t dest_port,
                                    uint16_t source_port) {
    s_last_sms_request_id = 0u;
    return modem_service_request_send_binary_sms(
        number, payload, payload_len, dest_port, source_port,
        &s_last_sms_request_id);
}

static bool request_save_sms(const char *number, const char *text) {
    s_last_sms_request_id = 0u;
    return modem_service_request_save_sms(
        number, text, &s_last_sms_request_id);
}

static bool request_sms_mailbox(modem_sms_mailbox_t mailbox) {
    s_last_sms_request_id = 0u;
    return modem_service_request_sms_mailbox(
        mailbox, &s_last_sms_request_id);
}

static bool request_sms_read(const uint16_t *indices, uint8_t index_count,
                             bool quarantined,
                             uint32_t expected_identity_hash) {
    s_last_sms_request_id = 0u;
    return modem_service_request_sms_read(
        indices, index_count, quarantined, expected_identity_hash,
        &s_last_sms_request_id);
}

static bool request_delete_sms_indices(const uint16_t *indices,
                                       uint8_t index_count) {
    s_last_sms_request_id = 0u;
    return modem_service_request_delete_sms_indices(
        indices, index_count, &s_last_sms_request_id);
}

#define modem_service_request_send_sms(number, text) \
    request_send_sms((number), (text))
#define modem_service_request_send_binary_sms(number, payload, payload_len, \
                                              dest_port, source_port) \
    request_send_binary_sms((number), (payload), (payload_len), (dest_port), \
                            (source_port))
#define modem_service_request_save_sms(number, text) \
    request_save_sms((number), (text))
#define modem_service_request_sms_mailbox(mailbox) \
    request_sms_mailbox((mailbox))
#define modem_service_request_sms_read(indices, index_count, quarantined, \
                                       expected_identity_hash) \
    request_sms_read((indices), (index_count), (quarantined), \
                     (expected_identity_hash))
#define modem_service_request_delete_sms_indices(indices, index_count) \
    request_delete_sms_indices((indices), (index_count))
#define modem_service_pop_sms_send_result(out) \
    (modem_service_pop_sms_send_result)(s_last_sms_request_id, (out))
#define modem_service_pop_sms_save_result(out) \
    (modem_service_pop_sms_save_result)(s_last_sms_request_id, (out))
#define modem_service_pop_sms_mailbox_result(out) \
    (modem_service_pop_sms_mailbox_result)(s_last_sms_request_id, (out))
#define modem_service_pop_sms_read_result(out) \
    (modem_service_pop_sms_read_result)(s_last_sms_request_id, (out))
#define modem_service_pop_sms_delete_result(out) \
    (modem_service_pop_sms_delete_result)(s_last_sms_request_id, (out))

static modem_service_test_snapshot_t service_probe(void) {
    modem_service_test_snapshot_t snapshot;
    modem_service_test_get_snapshot(&snapshot);
    return snapshot;
}

static size_t active_call_transactions(void) {
    return service_probe().call_transaction_count;
}

static modem_call_snapshot_t call_snapshot(void) {
    modem_call_snapshot_t snapshot;
    modem_service_get_call_snapshot(&snapshot);
    return snapshot;
}

static const modem_call_leg_snapshot_t *snapshot_leg(
    const modem_call_snapshot_t *snapshot, uint8_t id) {
    for (uint8_t i = 0u; i < snapshot->leg_count; i++) {
        if (snapshot->legs[i].id == id) return &snapshot->legs[i];
    }
    return NULL;
}

static bool boot_until_ready(uint32_t budget_ms) {
    modem_service_power_on();
    for (uint32_t elapsed = 0u; elapsed < budget_ms; elapsed += 100u) {
        mh_advance(100u);
        modem_status_t status = mh_status();
        modem_service_test_snapshot_t probe = service_probe();
        if (status.at_ready && !probe.sim_completion_pending &&
            !probe.sim_completion_active &&
            (!status.sim_ready || probe.sms_wake_armed)) {
            return true;
        }
    }
    return false;
}

static size_t tx_first_index(const char *command) {
    for (size_t i = 0u; i < s_mh_tx_history_count; i++) {
        if (strcmp(s_mh_tx_history[i], command) == 0) {
            return i;
        }
    }
    return SIZE_MAX;
}

static size_t tx_last_index(const char *command) {
    for (size_t i = s_mh_tx_history_count; i > 0u; i--) {
        if (strcmp(s_mh_tx_history[i - 1u], command) == 0) {
            return i - 1u;
        }
    }
    return SIZE_MAX;
}

static size_t tx_event_cstr(const char *text, size_t start) {
    return mh_tx_event_find(MH_TX_EVENT_CSTR, text, strlen(text), start);
}

static size_t tx_event_raw(const void *data, size_t len, size_t start) {
    return mh_tx_event_find(MH_TX_EVENT_RAW, data, len, start);
}

static size_t tx_event_raw_byte(uint8_t byte, size_t start) {
    return tx_event_raw(&byte, 1u, start);
}

static size_t tx_event_count_raw_byte(uint8_t byte) {
    size_t count = 0u;
    size_t next = 0u;
    while ((next = tx_event_raw_byte(byte, next)) != SIZE_MAX) {
        count++;
        next++;
    }
    return count;
}

static bool begin_sms_operation_fixture(const char *name) {
    begin_telit(true);
    if (!boot_until_ready(30000u)) {
        check(false, name);
        return false;
    }
    mh_clear_tx_capture();
    return true;
}

static void settle_dtr_sleep(void) {
    /* Drain the initial signal/CEREG/COPS/CPMS backstops, then leave a complete
     * awake window after the last command. */
    for (unsigned i = 0u; i < 16u; i++) mh_advance(250u);
    mh_advance(g_modem_vendor.wake.awake_window_ms + 1u);
}

static void test_runtime_serving_signal_sampling(void) {
    begin_telit(true);
    check(boot_until_ready(30000u), "runtime-signal fixture boots");
    settle_dtr_sleep();

    modem_status_t status = mh_status();
    check(status.network_registered && status.cereg == 1u &&
              status.signal.sequence != 0u &&
              status.signal.rat == MODEM_SIGNAL_RAT_LTE &&
              status.signal.rsrp_dbm == -101 &&
              status.signal.rsrq_db_x2 == -23 &&
              status.signal.sinr_db_x10 == 0 &&
              status.signal.channel == 5230u &&
              status.signal.cell_id == UINT32_C(0xabcdef01),
          "initial registration publishes one typed RFSTS sample");

    mh_feed("+CEREG: malformed");
    status = mh_status();
    check(status.network_registered && status.cereg == 1u,
          "malformed CEREG leaves the published registration unchanged");

    size_t rfsts_before = mh_tx_count_exact("AT#RFSTS");
    uint32_t sequence_before = status.signal.sequence;
    s_rfsts_response =
        "#RFSTS: \"310 410\",5230,-107,-84,-17,00AF,,32,3,1,"
        "ABCDEF01,\"310410123456789\",\"AT&T\",2,12,106";
    modem_service_set_signal_sampling(true, s_mh_now);
    mh_advance(1u); /* request DTR wake */
    mh_advance(1u); /* dispatch after CTS */
    mh_settle();
    status = mh_status();
    check(mh_tx_count_exact("AT#RFSTS") == rfsts_before + 1u &&
              status.signal.sequence == sequence_before + 1u &&
              status.signal.rsrp_dbm == -107 &&
              status.signal.rsrq_db_x2 == -34 &&
              status.signal.sinr_db_x10 == 12,
          "display-active sampling refreshes at the neutral RFSTS boundary");

    modem_signal_sample_t retained = status.signal;
    s_diag_rfsts_malformed_once = true;
    mh_advance(service_probe().signal_active_ms + 1u);
    mh_settle();
    status = mh_status();
    check(memcmp(&status.signal, &retained, sizeof(retained)) == 0,
          "malformed runtime sample retains the last atomic measurement");

    modem_service_set_signal_sampling(false, s_mh_now);
    mh_feed("+CEREG: 0");
    status = mh_status();
    check(!status.network_registered && status.signal.valid_fields == 0u,
          "registration loss immediately invalidates the old serving cell");
}

static void graceful_power_off(void) {
    modem_service_power_off();
    mh_advance(1u);   /* complete a possible DTR wake and dispatch #SHDN */
    mh_advance(1u);   /* consume command final and enter OFF_DISCHARGE */
    mh_advance(500u); /* measured continuous-low PWRMON qualification */
}

static void test_cold_boot_matching_provision(void) {
    begin_telit(true);
    modem_status_t status = mh_status();
    check(status.available && !status.at_ready,
          "production Telit backend starts available but powered off");
    check(modem_service_is_powered_off(),
          "initial Telit service state is logically off");
    check(modem_service_voice_transport_available(),
          "bench-qualified Telit DVI transport is advertised");
    check(s_mh_uart_init_count == 1u && s_mh_uart_park_count == 1u &&
              s_mh_power_monitor_init_count == 1u,
          "service initializes the monitor then parks the off UART");

    check(boot_until_ready(30000u),
          "cold boot reaches READY inside the measured budget");
    status = mh_status();
    check(status.sim_ready, "focused CPIN query publishes SIM ready");
    check(status.sim_checked && status.sim_present,
          "ready SIM publishes checked and physically present");
    check(status.provisioning_verified &&
              status.provisioning_schema_version == 11u,
          "complete readback pass publishes versioned provisioning result");
    check(s_mh_rail_enabled && s_mh_status_raw >= 1024u,
          "READY retains the modem rail with high PWRMON evidence");
    check(mh_tx_count_exact("AT#RXDIV?") == 1u &&
              mh_tx_count_exact("AT#FWAUTOSIM?") == 1u &&
              mh_tx_count_exact("AT#FWAUTOSIM=1") == 0u &&
              mh_tx_count_exact("AT#NWSCANTMR?") == 1u &&
              mh_tx_count_exact("AT#NWSCANTMR=60") == 0u &&
              mh_tx_count_exact("AT#RXDIV=0,1") == 0u &&
              mh_tx_count_exact("AT#SLED?") == 1u &&
              mh_tx_count_exact("AT#SLED=5;#SLEDSAV") == 0u &&
              mh_tx_count_exact("AT#WKIO?") == 1u &&
              mh_tx_count_exact("AT&V") == 1u &&
              mh_tx_count_exact("AT#E2SMSRI?") == 1u &&
              mh_tx_count_exact("AT#PSMRI?") == 2u &&
              mh_tx_count_exact(
                  "AT#WKIO=0;#E2SMSRI=0;\\R2&W0") == 0u &&
              mh_tx_count_exact("AT#PSMRI=1000") == 1u &&
              mh_tx_count_exact("AT#DVIEXT?") == 1u &&
              mh_tx_count_exact("AT#DVI?") == 1u &&
              mh_tx_count_exact("AT#DVIEXT=1,1") == 0u &&
              mh_tx_count_exact("AT#DVI=1,2,1") == 0u &&
              mh_tx_count_exact("AT+CPMS=\"ME\",\"ME\",\"ME\"") == 0u &&
              mh_tx_count_exact("AT#REBOOT") == 0u,
          "matching wake and DVI profiles avoid repair writes but apply PSMRI");
    check(mh_tx_count_exact("AT+IFC=2,2") == 1u &&
              mh_tx_count_exact("AT#CFLO=1") == 1u &&
              mh_tx_count_exact("AT#QSS=2") == 1u &&
              mh_tx_count_exact("AT+CFUN=1") == 1u &&
              mh_tx_count_exact("AT+CFUN=5") == 2u &&
              mh_tx_count_exact("AT+CSDH=1") == 1u &&
              mh_tx_count_exact("AT+CNMI=2,2,0,0,0") == 1u &&
              mh_tx_count_exact("AT+CNMI=1,1,0,0,0") == 0u,
          "RF-safe activation and focused SIM lifecycle run once each");
    check(mh_tx_count_exact("AT+CSSN?") == 1u &&
              mh_tx_count_exact("AT+CSSN=1,1") == 0u &&
              mh_tx_count_exact("AT#CFF?") == 1u &&
              mh_tx_count_exact("AT#CFF=1") == 0u,
          "matching supplementary URC settings are verified without rewrites");
    check(tx_first_index("AT+CFUN=5") < tx_first_index("AT+CFUN=1") &&
              tx_first_index("AT+CFUN=1") <
                  tx_first_index("AT+CSDH=1") &&
              tx_first_index("AT+CSDH=1") <
                  tx_first_index("AT+CNMI=2,2,0,0,0") &&
              tx_first_index("AT+CNMI=2,2,0,0,0") <
                  tx_first_index("AT#WKIO?") &&
              tx_first_index("AT#WKIO?") < tx_first_index("AT&V") &&
              tx_first_index("AT&V") < tx_first_index("AT#PSMRI?") &&
              tx_first_index("AT#PSMRI?") < tx_last_index("AT+CFUN=5"),
          "generic SMS wake profile is verified before the final CFUN=5");

    settle_dtr_sleep();
    check(mh_tx_count_exact("AT#PSMRI=1000") == 1u &&
              service_probe().sms_wake_armed && s_psmri_live_armed &&
              s_ring_pulse_mode && !s_wkio_configured &&
              s_e2smsri_ms == 0u && s_psmri_ms == 1000u,
          "READY proves one pre-CFUN runtime PSMRI apply");
    check(s_mh_dtr_sleep_permitted && !s_mh_cts_asserted,
          "idle READY releases DTR and observes inactive CTS");
    modem_diag_snapshot_t before_sleep;
    modem_diag_snapshot_t after_sleep;
    modem_service_get_diag_snapshot(&before_sleep);
    mh_advance(250u);
    modem_service_get_diag_snapshot(&after_sleep);
    check(after_sleep.transport.ready_ms - before_sleep.transport.ready_ms >=
                  250u &&
              after_sleep.transport.sleep_requested_ms -
                      before_sleep.transport.sleep_requested_ms >=
                  250u &&
              after_sleep.transport.sleep_confirmed_ms -
                      before_sleep.transport.sleep_confirmed_ms >=
                  250u,
          "READY, DTR-requested, and CTS-confirmed residency are counted");
    check(modem_service_request_debug_at("AT"),
          "command is admitted while Telit sleeps");
    mh_settle();
    check(!s_mh_dtr_sleep_permitted,
          "queued command asserts DTR before transmit");
    mh_advance(1u);
    check(s_mh_cts_asserted,
          "CTS confirmation lets the deferred command cross UART");
    modem_diag_snapshot_t diag;
    modem_service_get_diag_snapshot(&diag);
    check(diag.transport.sleep_entries >= 1u &&
              diag.transport.wake_attempts >= 1u &&
              diag.transport.wake_timeouts == 0u &&
              diag.transport.last_wake_latency_ms <=
                  g_modem_vendor.wake.dtr_wake_timeout_ms,
          "typed transport snapshot records the qualified DTR wake cycle");

    graceful_power_off();
    check(modem_service_is_powered_off() && !s_mh_rail_enabled &&
              s_mh_uart_park_count >= 2u,
          "#SHDN plus stable-low PWRMON parks UART before releasing rail");
    check(mh_status().available,
          "powered-off status still advertises the selected backend");
}

static void test_board_imei_capture_is_scoped_and_write_once(void) {
    char imei[STORE_WARRANTY_SERIAL_MAX + 1u];

    begin_telit(true);
    check(store_board_imei_get(imei, sizeof(imei)) == STORE_STATUS_NOT_FOUND,
          "identity fixture starts unprovisioned");
    check(boot_until_ready(30000u), "first identity capture reaches READY");
    check(mh_tx_count_exact("AT+CGSN") == 1u &&
              store_board_imei_get(imei, sizeof(imei)) == STORE_STATUS_OK &&
              strcmp(imei, "490154203237518") == 0,
          "first cold boot captures the complete check-summed IMEI once");

    graceful_power_off();
    check(boot_until_ready(30000u),
          "second boot with persisted identity reaches READY");
    check(mh_tx_count_exact("AT+CGSN") == 1u &&
              store_board_imei_get(imei, sizeof(imei)) == STORE_STATUS_OK &&
              strcmp(imei, "490154203237518") == 0,
          "subsequent modem initialization skips CGSN and retains identity");

    begin_telit(true);
    s_mh_cgsn_response = "490154203237519"; /* invalid check digit */
    s_inject_imei_on_qss_set = true;
    check(boot_until_ready(30000u),
          "bad optional identity response cannot block modem startup");
    check(mh_tx_count_exact("AT+CGSN") == 4u &&
              store_board_imei_get(imei, sizeof(imei)) ==
                  STORE_STATUS_NOT_FOUND,
          "invalid CGSN retries are bounded and a valid line during QSS cannot leak into identity");
}

static void test_raw_rx_capture_preserves_wire_bytes(void) {
    begin_telit(true);
    check(boot_until_ready(30000u), "raw RX fixture boots");
    settle_dtr_sleep();
    modem_rx_trace_status_t trace;
    modem_service_rx_trace_status(&trace);
    check(!trace.enabled && trace.received == 0u && trace.retained == 0u,
          "raw RX capture is disabled during normal startup");
    check(modem_service_rx_trace_start(), "raw RX capture can be armed");
    const uint8_t raw[] = {'$', 'Q', 'C', 'M', 'T', 'I', ':', 0u, 0x80u,
                           'M', 'E', ',', '7', '\r', '\n'};
    mh_rx_push_raw(raw, sizeof(raw));
    mh_settle();
    uint8_t actual[MODEM_RX_TRACE_CAPACITY];
    check(modem_service_rx_trace_read(0u, actual, sizeof(actual)) == 0u,
          "an active capture cannot be read as a stable snapshot");
    modem_service_rx_trace_stop();
    modem_service_rx_trace_status(&trace);
    check(!trace.enabled && trace.received == sizeof(raw) &&
              trace.retained == sizeof(raw) &&
              modem_service_rx_trace_read(0u, actual, sizeof(actual)) == sizeof(raw) &&
              memcmp(actual, raw, sizeof(raw)) == 0,
          "capture preserves NUL, high bytes and line terminators before framing");
    mh_feed("ignored after stop");
    modem_service_rx_trace_status(&trace);
    check(trace.received == sizeof(raw), "stopping freezes the evidence");
    check(modem_service_rx_trace_read(sizeof(raw), actual, 1u) == 0u &&
              modem_service_rx_trace_read(0u, NULL, 1u) == 0u &&
              modem_service_rx_trace_read(SIZE_MAX, actual, sizeof(actual)) == 0u,
          "capture reads reject invalid ranges and destinations");

    modem_service_rx_trace_start();
    uint8_t overflow[MODEM_RX_TRACE_CAPACITY + 37u];
    for (size_t i = 0u; i < sizeof(overflow); i++) {
        overflow[i] = (uint8_t)(0x80u + i % 127u);
    }
    mh_rx_push_raw(overflow, sizeof(overflow));
    mh_settle();
    modem_service_rx_trace_stop();
    modem_service_rx_trace_status(&trace);
    check(trace.received == sizeof(overflow) &&
              trace.retained == MODEM_RX_TRACE_CAPACITY &&
              modem_service_rx_trace_read(0u, actual, sizeof(actual)) == sizeof(actual) &&
              memcmp(actual, overflow + 37u, sizeof(actual)) == 0,
          "full capture retains the newest bytes in chronological order");
    check(modem_service_rx_trace_read(4090u, actual, 16u) == 6u &&
              memcmp(actual, overflow + sizeof(overflow) - 6u, 6u) == 0,
          "partial capture reads stop at the retained end");
    modem_service_rx_trace_start();
    modem_service_rx_trace_status(&trace);
    check(trace.enabled && trace.received == 0u && trace.retained == 0u,
          "rearming explicitly clears only capture metadata");
    modem_service_rx_trace_stop();
}

static void test_background_polling_bench_gate_is_narrow(void) {
    begin_telit(true);
    check(boot_until_ready(30000u), "background-poll fixture boots");
    settle_dtr_sleep();

    size_t signal_before = mh_tx_count_exact("AT#RFSTS");
    size_t cereg_before = mh_tx_count_exact("AT+CEREG?");
    size_t cops_before = mh_tx_count_exact("AT+COPS?");
    size_t cpms_before = mh_tx_count_exact("AT+CPMS?");
    mh_feed("+CMTI: \"ME\",17");
    check(mh_tx_count_exact("AT+CPMS?") == cpms_before + 1u,
          "CMTI schedules exactly one immediate receive-store fullness check");
    cpms_before++;

    check(modem_service_request_debug_background_polling(false),
          "background polling disable request is admitted");
    mh_settle();

    modem_diag_snapshot_t diag;
    modem_service_get_diag_snapshot(&diag);
    check(!diag.scheduler.background_polling_enabled,
          "diagnostics publish disabled background polling");

    mh_advance(300001u);
    check(mh_tx_count_exact("AT#RFSTS") == signal_before &&
              mh_tx_count_exact("AT+CEREG?") == cereg_before &&
              mh_tx_count_exact("AT+COPS?") == cops_before &&
              mh_tx_count_exact("AT+CPMS?") == cpms_before &&
              s_mh_dtr_sleep_permitted && !s_mh_cts_asserted,
          "disabled gate suppresses only due periodic backstops and stays asleep");

    check(modem_service_request_debug_background_polling(true),
          "background polling enable request is admitted");
    mh_settle();
    mh_advance(1u); /* request DTR wake for the first overdue command */
    mh_advance(1u); /* observe CTS, then drain all overdue backstops */
    modem_service_get_diag_snapshot(&diag);
    check(diag.scheduler.background_polling_enabled &&
              mh_tx_count_exact("AT#RFSTS") > signal_before &&
              mh_tx_count_exact("AT+CEREG?") > cereg_before &&
              mh_tx_count_exact("AT+COPS?") == cops_before &&
              mh_tx_count_exact("AT+CPMS?") > cpms_before,
          "re-enable immediately drains overdue production backstops");
}

static void test_transport_sleep_confirmation_is_strict(void) {
    begin_telit(true);
    check(boot_until_ready(30000u), "transport-sleep fixture boots");
    settle_dtr_sleep();
    check(modem_service_transport_sleep_confirmed(),
          "fully idle DTR/CTS state permits RP dormant entry");

    s_mh_cts_asserted = true;
    check(!modem_service_transport_sleep_confirmed(),
          "asserted CTS rejects an unconfirmed modem sleep");
    s_mh_cts_asserted = false;

    s_mh_ri_asserted = true;
    check(!modem_service_transport_sleep_confirmed(),
          "active RI level rejects dormant entry");
    s_mh_ri_asserted = false;

    mh_rx_push("+CEREG: 1");
    check(!modem_service_transport_sleep_confirmed(),
          "an unread UART line rejects dormant entry");
    mh_settle();
    settle_dtr_sleep();
    check(modem_service_transport_sleep_confirmed(),
          "draining the UART restores the transport sleep boundary");

    check(modem_service_request_debug_at("AT"),
          "queued-work sleep fixture admits a debug command");
    check(!modem_service_transport_sleep_confirmed(),
          "queued AT work rejects dormant entry before dispatch");
    mh_settle();
    mh_advance(1u);
    settle_dtr_sleep();
    check(modem_service_transport_sleep_confirmed(),
          "completed AT work returns to confirmed transport sleep");

    s_mh_rx_stuck_readable = true;
    check(!modem_service_transport_sleep_confirmed(),
          "continuously readable UART rejects dormant entry");
    s_mh_rx_stuck_readable = false;
}

static void test_transport_sleep_waits_for_a_raw_cmt_body(void) {
    /* A +CMT body is read raw by <length>, bypassing the line framer, so the
     * framer-pending check alone would let the transport sleep between two
     * body bytes. The collector's pending state must hold the gate. */
    begin_telit(true);
    check(boot_until_ready(30000u), "raw-body sleep fixture boots");
    settle_dtr_sleep();
    check(modem_service_transport_sleep_confirmed(),
          "raw-body sleep fixture starts from confirmed transport sleep");

    static const char BODY[] = "abcdefghijklmnopqrst"; /* 20 GSM chars */
    mh_rx_push("+CMT: \"7866910488\",\"\",\"20260916105524\",129,4098,0,8,20");
    mh_rx_push_raw((const uint8_t *)BODY, 5u);
    mh_settle();
    /* One awake window (1 s), inside the 5 s body deadline. */
    mh_advance(g_modem_vendor.wake.awake_window_ms + 1u);
    check(modem_sms_direct_pending() && !service_probe().command_active &&
              !modem_service_transport_sleep_confirmed(),
          "a partially received raw +CMT body rejects transport sleep");

    modem_status_t before = mh_status();
    mh_rx_push_raw((const uint8_t *)&BODY[5], sizeof(BODY) - 1u - 5u);
    mh_rx_push_raw((const uint8_t *)"\r\n", 2u);
    mh_settle();
    settle_dtr_sleep();
    check(!modem_sms_direct_pending() &&
              mh_status().sms_received_count == before.sms_received_count + 1u &&
              modem_service_transport_sleep_confirmed(),
          "the completed body is stored and transport sleep is permitted again");
}

static void test_cold_boot_repairs_dvi_and_restores_runtime_mode(void) {
    begin_telit(true);
    s_dviext_configured = false;
    s_dvi_configured = false;

    check(boot_until_ready(60000u),
          "fresh DVI profile converges through one controlled reboot");
    modem_status_t status = mh_status();
    check(status.provisioning_verified && status.audio_init_ok &&
              status.provisioning_schema_version == 11u,
          "repaired DVI profile reaches READY with audio qualified");
    check(s_dviext_configured && s_dvi_configured &&
              mh_tx_count_exact("AT#DVIEXT=1,1") == 1u &&
              mh_tx_count_exact("AT#DVI=1,2,1") == 2u &&
              mh_tx_count_exact("AT#REBOOT") == 1u,
          "persistent DVIEXT is written once and volatile DVI is restored after reboot");
    check(mh_tx_count_exact("AT#DVIEXT?") == 3u &&
              mh_tx_count_exact("AT#DVI?") == 4u,
          "writes and the reboot boundary each receive strict DVI readback");
}

static void test_cold_boot_provisions_antenna_before_rf_online(void) {
    begin_telit(true);
    s_stune_enabled = false;
    s_gpio2_direction = 0u;
    s_gpio3_direction = 0u;
    memset(s_tune_masks, 0, sizeof(s_tune_masks));
    s_tune_zero_rows = 2u;

    check(boot_until_ready(30000u),
          "disabled tuner converges before READY");
    check(s_stune_enabled && s_gpio2_direction == 17u &&
              s_gpio3_direction == 18u && s_cfun == 5u,
          "tuner is enabled on ALT16/ALT17 before RF resumes");
    check(s_tune_masks[0] == UINT64_C(0x601840A7) &&
              s_tune_masks[1] == UINT64_C(0x00010300) &&
              s_tune_masks[2] == UINT64_C(0x1F80BC50) &&
              s_tune_masks[3] == UINT64_C(0x00400008) &&
              s_tune_zero_rows == 0u,
          "first provisioning pass leaves the exact WWX RF-state table");
    check(mh_tx_count_exact("AT#GPIO=2,0,17") == 1u &&
              mh_tx_count_exact("AT#GPIO=3,0,18") == 1u &&
              mh_tx_count_exact("AT#STUNEANT=1,10300,0,1") == 1u &&
              mh_tx_count_exact("AT#STUNEANT=1,1F80BC50,1,0") == 1u &&
              mh_tx_count_exact("AT#STUNEANT=1,400008,1,1") == 1u &&
              mh_tx_count_exact("AT#STUNEANT=1,601840A7,0,0") == 0u,
          "only missing tuner rows are written once");

    size_t rf_off = tx_first_index("AT+CFUN=4");
    size_t gpio2 = tx_first_index("AT#GPIO=2,0,17");
    size_t gpio3 = tx_first_index("AT#GPIO=3,0,18");
    size_t final_verify = SIZE_MAX;
    size_t rf_online = tx_first_index("AT+CFUN=5");
    for (size_t i = 0u; i < s_mh_tx_history_count; i++) {
        if (strcmp(s_mh_tx_history[i], "AT#GTUNEANT?") == 0) {
            final_verify = i;
        }
    }
    check(rf_off < gpio2 && gpio2 < gpio3 && gpio3 < final_verify &&
              final_verify < rf_online,
          "RF stays off through GPIO setup and final table verification");
    check(mh_status().provisioning_verified &&
              mh_status().provisioning_schema_version == 11u,
          "antenna provisioning participates in the versioned contract");
}

static void test_carrier_reset_tuner_read_error(void) {
    begin_telit(true);
    s_tune_query_errors = 1u;
    s_gpio2_direction = 0u;
    s_gpio3_direction = 0u;
    memset(s_tune_masks, 0, sizeof(s_tune_masks));

    check(boot_until_ready(30000u),
          "enabled but unreadable reset tuner converges through repair");
    check(mh_status().provisioning_verified && s_stune_enabled &&
              s_gpio2_direction == 17u && s_gpio3_direction == 18u &&
              s_tune_masks[0] == UINT64_C(0x601840A7) &&
              s_tune_masks[1] == UINT64_C(0x00010300) &&
              s_tune_masks[2] == UINT64_C(0x1F80BC50) &&
              s_tune_masks[3] == UINT64_C(0x00400008),
          "reset recovery verifies both GPIOs and every WWX tuner band");
    check(tx_first_index("AT+CFUN=4") <
              tx_first_index("AT#STUNEANT=0") &&
              tx_first_index("AT#STUNEANT=0") <
                  tx_first_index("AT#GPIO=2,0,17") &&
              tx_last_index("AT#GTUNEANT?") <
                  tx_first_index("AT+CFUN=5"),
          "reset recovery keeps RF off until exact final tuner readback");

    begin_telit(true);
    s_tune_query_errors = 1000u;
    check(!boot_until_ready(30000u) && !mh_status().provisioning_verified &&
              s_cfun == 4u && mh_tx_count_exact("AT+CFUN=5") == 0u,
          "persistent tuner read failure cannot enable RF after repair");
}

static void test_scan_timer_provisioning(void) {
    begin_telit(true);
    s_scan_timer_s = 5u;
    check(boot_until_ready(30000u) && s_scan_timer_s == 60u &&
              mh_status().provisioning_verified,
          "factory scan pause is repaired and verified at startup");
    check(mh_tx_count_exact("AT#NWSCANTMR=60") == 1u &&
              mh_tx_count_exact("AT#NWSCANTMR?") == 2u &&
              mh_tx_count_exact("AT#REBOOT") == 0u &&
              tx_first_index("AT#NWSCANTMR?") <
                  tx_first_index("AT#NWSCANTMR=60") &&
              tx_first_index("AT#NWSCANTMR=60") <
                  tx_last_index("AT#NWSCANTMR?") &&
              tx_last_index("AT#NWSCANTMR?") <
                  tx_first_index("AT+CFUN=5"),
          "scan pause follows query/write/verify before RF without reboot");

    graceful_power_off();
    check(modem_service_is_powered_off(),
          "scan fixture powers off before testing the saved setting");
    mh_clear_tx_capture();
    check(boot_until_ready(30000u) &&
              mh_tx_count_exact("AT#NWSCANTMR?") == 1u &&
              mh_tx_count_exact("AT#NWSCANTMR=60") == 0u &&
              mh_status().provisioning_verified,
          "next boot verifies the retained scan pause without NVM wear");

    begin_telit(true);
    s_scan_timer_s = 5u;
    s_mh_sim_present = false;
    s_mh_sim_ready = false;
    check(boot_until_ready(30000u) && s_scan_timer_s == 60u &&
              mh_tx_count_exact("AT#NWSCANTMR=60") == 1u,
          "scan pause provisioning does not depend on an inserted SIM");

    begin_telit(true);
    s_scan_timer_s = 5u;
    s_fault = TELIT_FAULT_SCAN_SET_TIMEOUT_ONCE;
    check(boot_until_ready(45000u) && mh_status().provisioning_verified &&
              s_fault_consumed && s_scan_timer_s == 60u &&
              mh_tx_count_exact("AT#NWSCANTMR=60") == 1u &&
              mh_tx_count_exact("AT#NWSCANTMR?") == 2u &&
              mh_tx_count_exact("AT#REBOOT") == 0u,
          "lost scan-set final is resolved by readback, not a duplicate write");

    static const telit_fault_t faults[] = {
        TELIT_FAULT_SCAN_READBACK_MALFORMED,
        TELIT_FAULT_SCAN_SET_ERROR,
        TELIT_FAULT_SCAN_VERIFY_MISMATCH,
    };
    for (size_t i = 0u; i < sizeof(faults) / sizeof(faults[0]); i++) {
        begin_telit(true);
        s_scan_timer_s = 5u;
        s_fault = faults[i];
        check(boot_until_ready(45000u),
              "scan-setting failure still completes startup");
        settle_dtr_sleep();
        check(!mh_status().provisioning_verified,
              "failed scan-setting verification remains visible after SIM completion");
        check(mh_status().sms_init_ok,
              "scan-setting failure does not disable SMS setup");
        check(mh_status().audio_init_ok,
              "scan-setting failure does not disable audio setup");
        check(mh_tx_count_exact("AT#REBOOT") == 0u,
              "scan-setting failure cannot request a reboot");
        if (faults[i] == TELIT_FAULT_SCAN_READBACK_MALFORMED) {
            check(mh_tx_count_exact("AT#NWSCANTMR?") == 3u &&
                      mh_tx_count_exact("AT#NWSCANTMR=60") == 0u,
                  "malformed scan queries exhaust bounded retries without writing");
        } else {
            check(mh_tx_count_exact("AT#NWSCANTMR=60") == 3u &&
                      mh_tx_count_exact("AT#NWSCANTMR?") ==
                          (faults[i] == TELIT_FAULT_SCAN_SET_ERROR ? 1u : 4u),
                  "failed scan repair is bounded and never accepted without readback");
        }
    }
}

static void test_auto_profile_provisioning(void) {
    for (uint8_t mode = 0u; mode <= 3u; mode++) {
        begin_telit(true);
        s_auto_profile = mode;
        check(boot_until_ready(30000u) && s_auto_profile == 1u &&
                  mh_status().provisioning_verified,
              "disabled and one-shot carrier modes converge to persistent auto");
        check(mh_tx_count_exact("AT#FWAUTOSIM=1") == (mode == 1u ? 0u : 1u) &&
                  mh_tx_count_exact("AT#FWAUTOSIM?") == (mode == 1u ? 1u : 2u) &&
                  mh_tx_count_exact("AT#REBOOT") == 0u &&
                  tx_first_index("AT+CFUN=4") <
                      tx_first_index("AT#FWAUTOSIM?") &&
                  tx_last_index("AT#FWAUTOSIM?") <
                      tx_first_index("AT#STUNEANT?"),
              "auto selection is queried and verified before board provisioning");
    }

    graceful_power_off();
    mh_clear_tx_capture();
    check(boot_until_ready(30000u) &&
              mh_tx_count_exact("AT#FWAUTOSIM?") == 1u &&
              mh_tx_count_exact("AT#FWAUTOSIM=1") == 0u &&
              mh_status().provisioning_verified,
          "saved auto selection is verified without another NVM write");

    begin_telit(true);
    s_auto_profile = 0u;
    s_mh_sim_present = false;
    s_mh_sim_ready = false;
    check(boot_until_ready(30000u) && s_auto_profile == 1u &&
              mh_tx_count_exact("AT#FWAUTOSIM=1") == 1u,
          "persistent auto selection is configured even with no SIM");

    begin_telit(true);
    s_auto_profile = 0u;
    s_fault = TELIT_FAULT_AUTO_PROFILE_SET_TIMEOUT_ONCE;
    check(boot_until_ready(45000u) && mh_status().provisioning_verified &&
              s_fault_consumed && s_auto_profile == 1u &&
              mh_tx_count_exact("AT#FWAUTOSIM=1") == 1u &&
              mh_tx_count_exact("AT#FWAUTOSIM?") == 2u,
          "lost auto-selection final is resolved by readback without duplicate write");

    static const telit_fault_t faults[] = {
        TELIT_FAULT_AUTO_PROFILE_READBACK_MALFORMED,
        TELIT_FAULT_AUTO_PROFILE_SET_ERROR,
        TELIT_FAULT_AUTO_PROFILE_VERIFY_MISMATCH,
    };
    for (size_t i = 0u; i < sizeof(faults) / sizeof(faults[0]); i++) {
        begin_telit(true);
        s_auto_profile = 0u;
        s_fault = faults[i];
        check(boot_until_ready(45000u),
              "auto-selection failure preserves otherwise working service");
        settle_dtr_sleep();
        check(!mh_status().provisioning_verified &&
                  mh_status().sms_init_ok && mh_status().audio_init_ok,
              "failed auto selection remains unverified without disabling SMS or audio");
        if (faults[i] == TELIT_FAULT_AUTO_PROFILE_READBACK_MALFORMED) {
            check(mh_tx_count_exact("AT#FWAUTOSIM?") == 3u &&
                      mh_tx_count_exact("AT#FWAUTOSIM=1") == 0u,
                  "malformed auto-selection readbacks cannot trigger blind writes");
        } else {
            check(mh_tx_count_exact("AT#FWAUTOSIM=1") == 3u &&
                      mh_tx_count_exact("AT#FWAUTOSIM?") ==
                          (faults[i] == TELIT_FAULT_AUTO_PROFILE_SET_ERROR ? 1u : 4u),
                  "auto-selection repair and verification have bounded retries");
        }
    }
}

static void test_pending_power_on_is_not_quiescent_off(void) {
    begin_telit(true);
    modem_service_power_on();
    modem_service_test_snapshot_t probe = service_probe();
    check(probe.state == MODEM_SERVICE_TEST_STATE_OFF &&
              probe.power_on_pending,
          "fresh power-on first enforces the real rail-down dwell");
    check(!modem_service_is_powered_off(),
          "queued power-on cannot let the phone enter dormant during its dwell");
}

static void test_autonomous_startup_restart_reinitializes_once(void) {
    static const char *const commands[] = {"AT", "ATE0", "AT#FWAUTOSIM?"};
    for (size_t i = 0u; i < sizeof(commands) / sizeof(commands[0]); i++) {
        begin_telit(false);
        s_startup_drop_command = commands[i];
        s_startup_drops_remaining = 1u;
        check(boot_until_ready(70000u),
              "autonomous restart during probe/init/provision recovers without user action");
        check(s_startup_drops_remaining == 0u &&
                  mh_status().provisioning_verified &&
                  service_probe().sms_wake_armed,
              "restart discards interrupted exchange and completes full provisioning/SMS wake");
        check(s_mh_rail_transition_count == 1u &&
                  mh_tx_count_exact("AT#REBOOT") == 1u &&
                  mh_tx_count_exact("AT#RXDIV=0,1") == 1u,
              "autonomous restart retains rail and leaves the controlled repair reboot available");
        modem_diag_snapshot_t diag;
        modem_service_get_diag_snapshot(&diag);
        check(diag.runtime.automatic_recoveries == 1u &&
                  diag.runtime.power_failures == 0u,
              "a single startup restart is recorded as recovery, not a power fault");
    }

    begin_telit(true);
    s_startup_drop_command = "AT";
    s_startup_drops_remaining = 2u;
    check(!boot_until_ready(70000u) &&
              service_probe().state == MODEM_SERVICE_TEST_STATE_FAILED &&
              s_startup_drops_remaining == 0u && s_mh_rail_enabled,
          "a second autonomous startup drop is bounded and retains the uncertain rail");
}

static void begin_startup_restart_wait(void) {
    begin_telit(true);
    s_startup_drop_command = "AT";
    s_startup_drops_remaining = 1u;
    modem_service_power_on();
    for (unsigned i = 0u; i < 300u; i++) {
        mh_advance(100u);
        if (s_startup_drops_remaining == 0u &&
            service_probe().state == MODEM_SERVICE_TEST_STATE_MODULE_WAIT) {
            break;
        }
    }
    check(service_probe().state == MODEM_SERVICE_TEST_STATE_MODULE_WAIT &&
              service_probe().uart_parked && s_mh_rail_enabled,
          "observed startup drop enters a parked, powered restart wait");
}

static void test_startup_restart_preserves_wait_and_shutdown_guards(void) {
    begin_startup_restart_wait();
    s_mh_status_restore_pending = false;
    size_t writes_before = s_mh_uart_write_count;
    s_mh_status_raw = 2214u;
    s_mh_status_mv = 1784u;
    s_mh_rx_idle_high = false;
    s_mh_ri_asserted = true;
    s_mh_ri_wake_pending = true;
    mh_advance(2000u);
    check(service_probe().state == MODEM_SERVICE_TEST_STATE_MODULE_WAIT &&
              service_probe().uart_parked &&
              s_mh_uart_write_count == writes_before,
          "returning PWRMON and RI cannot bypass RX-idle or transmit during restart");
    s_mh_status_valid = false;
    mh_advance(g_modem_vendor.power.ready_budget_ms + 1u);
    check(service_probe().state == MODEM_SERVICE_TEST_STATE_FAILED &&
              s_mh_rail_enabled,
          "indeterminate restart has a finite startup budget and cannot authorize a rail cut");

    begin_startup_restart_wait();
    modem_service_power_on();
    modem_service_power_off();
    check(service_probe().power_off_pending,
          "explicit OFF remains pending across the autonomous restart wait");
    for (unsigned i = 0u; i < 400u && !modem_service_is_powered_off(); i++) {
        mh_advance(100u);
    }
    check(modem_service_is_powered_off() && !s_mh_rail_enabled &&
              mh_tx_count_exact("AT#SHDN") == 1u &&
              mh_tx_count_exact("ATE0") == 0u,
          "restart honors OFF through a proven AT channel without starting provisioning");

    begin_startup_restart_wait();
    s_mh_supply_pg = false;
    mh_advance(2000u); /* Include the restart waiter's initial settle gate. */
    check(service_probe().state == MODEM_SERVICE_TEST_STATE_FAILED &&
              modem_service_take_supply_power_failure() && s_mh_rail_enabled,
          "real PG loss during restart remains a supply fault, not another recovery");
}

static void test_controlled_reboot_ignores_sleep_wake_signals(void) {
    static const telit_fault_t faults[] = {
        TELIT_FAULT_REBOOT_RI_BEFORE_DROP,
        TELIT_FAULT_REBOOT_RI_LOST_FINAL,
    };
    for (size_t i = 0u; i < sizeof(faults) / sizeof(faults[0]); i++) {
        begin_telit(false);
        s_fault = faults[i];
        check(boot_until_ready(80000u),
              "RI/CTS reset activity cannot turn a controlled reboot into a wake failure");
        modem_diag_snapshot_t diag;
        modem_service_get_diag_snapshot(&diag);
        check(mh_status().provisioning_verified &&
                  mh_tx_count_exact("AT#REBOOT") == 1u &&
                  diag.runtime.power_failures == 0u &&
                  diag.transport.wake_timeouts == 0u,
              "both acknowledged and lost-final reboot paths converge without a power fault");
        s_mh_ri_asserted = false;
        s_mh_dtr_wake_works = true;
    }
}

static void test_uart_waits_for_module_power_evidence(void) {
    begin_telit(true);
    s_mh_module_boots = false;
    modem_service_test_snapshot_t probe;

    modem_service_power_on();
    mh_advance(service_probe().rail_off_dwell_ms);
    mh_advance(g_modem_vendor.power.rail_settle_ms);
    check(service_probe().state == MODEM_SERVICE_TEST_STATE_POWER_PULSE &&
              s_mh_uart_init_count == 1u && s_mh_uart_park_count == 1u,
          "Telit ON_OFF pulse starts while the off-state UART remains parked");

    mh_advance(g_modem_vendor.power.pwron_pulse_ms);
    check(service_probe().state == MODEM_SERVICE_TEST_STATE_MODULE_WAIT &&
              s_mh_uart_init_count == 1u,
          "low PWRMON cannot unpark RX into a continuous-break interrupt storm");

    s_mh_status_raw = 2214u;
    s_mh_status_mv = 1784u;
    s_mh_cts_asserted = true; /* parked CTS pull-down can look asserted */
    s_mh_now += service_probe().rail_off_poll_ms;
    modem_service_tick(s_mh_now);
    probe = service_probe();
    check(s_mh_uart_init_count == 1u && probe.uart_parked &&
              probe.state == MODEM_SERVICE_TEST_STATE_MODULE_WAIT,
          "parked CTS cannot bypass the RX-idle transport gate");

    modem_service_power_off();
    check(service_probe().power_off_pending &&
              mh_tx_count_exact("AT#SHDN") == 0u,
          "startup power-off cannot transmit through a parked UART");
    modem_service_power_on(); /* latest intent cancels the deferred shutdown */
    check(!service_probe().power_off_pending,
          "power-on intent cancels only the not-yet-dispatched startup shutdown");

    s_mh_rx_idle_high = true;
    s_mh_now += service_probe().rail_off_poll_ms;
    modem_service_tick(s_mh_now);
    probe = service_probe();
    check(s_mh_uart_init_count == 2u && !probe.uart_parked &&
              s_mh_uart_irq_enable_count == 0u &&
              probe.state == MODEM_SERVICE_TEST_STATE_PROBE,
          "high PWRMON unparks UART and advances through CTS to the AT probe");

    mh_settle();
    check(s_mh_uart_irq_enable_count == 1u,
          "only a parsed successful AT probe arms interrupt-driven RX");
}

static void test_continuously_readable_uart_is_tick_bounded(void) {
    begin_telit(true);
    uint32_t before = mh_status().rx_bytes;
    s_mh_rx_stuck_readable = true;
    modem_service_tick(s_mh_now);
    s_mh_rx_stuck_readable = false;
    check(mh_status().rx_bytes - before == service_probe().rx_tick_budget,
          "continuous UART readability consumes only the per-tick RX budget");
}

static void test_factory_flow_profile_bootstraps_without_cts(void) {
    begin_telit(true);
    s_factory_flow_blocked = true;
    s_mh_dtr_wake_works = false;

    check(boot_until_ready(30000u),
          "factory CFLO=0 profile bootstraps without usable CTS");
    check(mh_tx_count_exact("AT#CFLO=1") == 1u &&
              s_mh_uart_no_cts_write_count >= 10u &&
              s_mh_cts_asserted,
          "probe and flow-establishing commands bypass CTS, then restore gating");
}

static void test_clcc_integrity_counts_uart_line_errors(void) {
    begin_telit(true);
    uint32_t before = service_probe().transport_integrity_counter;
    s_mh_rx_line_errors += 3u;
    check(service_probe().transport_integrity_counter == before + 3u,
          "discarded UART line-error words invalidate an open CLCC snapshot");
}

static void test_qualified_voice_transport_starts_only_after_audio_init(void) {
    begin_telit(true);
    modem_service_test_apply_bridge_inputs(MODEM_CALL_ACTIVE, false, true);
    check(service_probe().bridge_active_wanted,
          "qualified Telit ACTIVE call requests the voice bridge");

    modem_service_test_apply_bridge_inputs(MODEM_CALL_IDLE, false, true);
    check(!service_probe().bridge_active_wanted,
          "leaving ACTIVE tears the qualified voice bridge down");

    modem_service_test_apply_bridge_inputs(MODEM_CALL_ACTIVE, false, false);
    check(!service_probe().bridge_active_wanted,
          "failed DVI provisioning suppresses bridge admission");
}

static void test_power_off_during_start_pulse_is_graceful(void) {
    begin_telit(true);
    modem_service_power_on();
    mh_advance(service_probe().rail_off_dwell_ms);
    mh_advance(g_modem_vendor.power.rail_settle_ms);
    check(service_probe().state == MODEM_SERVICE_TEST_STATE_POWER_PULSE &&
              s_mh_power_pin_asserted && s_mh_rail_enabled,
          "fixture reaches the live ON_OFF pulse window");

    modem_service_power_off();
    modem_service_power_off(); /* repeated UI/debug intent remains idempotent */
    check(service_probe().power_off_pending && s_mh_power_pin_asserted &&
              s_mh_rail_enabled,
          "power-off during ON_OFF defers instead of cutting the live rail");

    mh_advance(g_modem_vendor.power.pwron_pulse_ms);
    mh_advance(1u); /* MODULE_WAIT -> PROBE, then run the first AT exchange. */
    modem_service_test_snapshot_t probe = service_probe();
    check(mh_tx_count_exact("AT#SHDN") == 1u &&
              probe.state == MODEM_SERVICE_TEST_STATE_OFF_DISCHARGE &&
              s_mh_rail_enabled && s_mh_uart_park_count >= 2u &&
              probe.uart_parked,
          "graceful shutdown parks PL011 before PWRMON qualification");
    mh_advance(500u);
    check(modem_service_is_powered_off() && !s_mh_rail_enabled &&
              s_mh_uart_park_count >= 3u,
          "stable-low PWRMON gates rail release and fully parks RX afterward");

    begin_telit(true);
    s_mh_module_boots = false;
    modem_service_power_on();
    mh_advance(service_probe().rail_off_dwell_ms);
    mh_advance(g_modem_vendor.power.rail_settle_ms);
    modem_service_power_off();
    mh_advance(g_modem_vendor.power.pwron_pulse_ms);
    check(service_probe().power_off_pending && s_mh_rail_enabled,
          "missing early PWRMON cannot cause an immediate startup rail cut");
    mh_advance(g_modem_vendor.power.ready_budget_ms);
    check(modem_service_is_powered_off() && !s_mh_rail_enabled &&
              mh_tx_count_exact("AT#SHDN") == 0u,
          "full low-PWRMON startup budget proves no live module and releases safely");
}

static void test_power_on_during_inflight_shutdown_restarts_once(void) {
    begin_telit(true);
    check(boot_until_ready(30000u),
          "in-flight shutdown restart fixture boots");
    uint32_t rail_transitions_before = s_mh_rail_transition_count;

    /* Lose the final so the request remains observably on wire long enough for
     * the user to reverse the soft-off intent. PWRMON still drops, exactly as a
     * real #SHDN may do before its final reaches the host. */
    s_fault = TELIT_FAULT_SHUTDOWN_TIMEOUT;
    modem_service_power_off();
    mh_advance(1u);
    check(service_probe().command_active &&
              mh_tx_count_exact("AT#SHDN") == 1u,
          "shutdown command is in flight before the newer power-on intent");

    modem_service_power_on();
    check(service_probe().power_on_pending,
          "power-on during an on-wire shutdown latches one restart intent");
    modem_service_power_off();
    check(!service_probe().power_on_pending &&
              mh_tx_count_exact("AT#SHDN") == 1u,
          "newer power-off cancels restart without duplicating in-flight #SHDN");
    modem_service_power_on();
    modem_service_power_on(); /* duplicate key/event intent remains idempotent */
    check(service_probe().power_on_pending &&
              mh_tx_count_exact("AT#SHDN") == 1u,
          "latest duplicate power-on intents retain one restart and one shutdown");

    bool shutdown_completed = false;
    bool ready = false;
    for (uint32_t elapsed = 0u; elapsed < 40000u; elapsed += 100u) {
        mh_advance(100u);
        if (s_mh_rail_transition_count >= rail_transitions_before + 1u) {
            shutdown_completed = true;
        }
        modem_status_t status = mh_status();
        if (shutdown_completed &&
            s_mh_rail_transition_count >= rail_transitions_before + 2u &&
            status.at_ready &&
            service_probe().state == MODEM_SERVICE_TEST_STATE_READY) {
            ready = true;
            break;
        }
    }
    check(ready,
          "latched quick off/on intent completes shutdown and returns to READY");
    check(mh_tx_count_exact("AT#SHDN") == 1u,
          "duplicate power-on intent never duplicates the shutdown command");
    check(s_mh_rail_transition_count == rail_transitions_before + 2u &&
              s_mh_rail_enabled,
          "quick off/on performs exactly one rail-off and one rail-on transition");
}

static void test_power_on_cancels_shutdown_before_uart_dispatch(void) {
    begin_telit(true);
    check(boot_until_ready(30000u),
          "deferred shutdown cancellation fixture boots");
    settle_dtr_sleep();
    s_mh_dtr_wake_works = false;

    modem_service_power_off();
    check(service_probe().deferred_command_valid &&
              mh_tx_count_exact("AT#SHDN") == 0u,
          "sleeping transport defers shutdown before any UART bytes");
    modem_service_power_on();
    check(!service_probe().deferred_command_valid &&
              !service_probe().power_on_pending &&
              service_probe().state == MODEM_SERVICE_TEST_STATE_READY,
          "newer power-on safely cancels a never-dispatched shutdown");
    mh_advance(g_modem_vendor.wake.dtr_wake_timeout_ms + 100u);
    check(mh_tx_count_exact("AT#SHDN") == 0u && s_mh_rail_enabled &&
              service_probe().state == MODEM_SERVICE_TEST_STATE_READY,
          "cancelled pre-dispatch shutdown neither leaks nor times out later");
}

static void test_power_observation_deadline_rebases_on_restart(void) {
    begin_telit(true);
    check(boot_until_ready(30000u),
          "power-observation rebase fixture boots");
    uint32_t first_session_deadline =
        service_probe().next_power_observation_ms;
    graceful_power_off();

    /* Keep ticking while OFF, as the RTC heartbeat does on hardware, but age
     * the old runtime deadline past the signed 32-bit comparison horizon. */
    mh_advance(UINT32_C(0x48000000));
    mh_advance(UINT32_C(0x48000000));
    check(time_diff_ms(s_mh_now, first_session_deadline) < 0,
          "stale prior-epoch observation deadline now looks falsely future");

    modem_service_power_on();
    modem_service_test_snapshot_t probe = service_probe();
    check(probe.state == MODEM_SERVICE_TEST_STATE_RAIL_WAIT,
          "completed OFF dwell remains latched across the signed-time boundary");
    check(probe.next_power_observation_ms == s_mh_now &&
              probe.next_power_observation_ms != first_session_deadline,
          "each startup rebases runtime power observation to its own epoch");
}

static void test_indeterminate_module_status_never_authorizes_rail_cut(void) {
    begin_telit(true);
    s_mh_module_boots = false;
    modem_service_test_snapshot_t probe;
    modem_service_power_on();
    mh_advance(service_probe().rail_off_dwell_ms);
    mh_advance(g_modem_vendor.power.rail_settle_ms);
    mh_advance(g_modem_vendor.power.pwron_pulse_ms);
    check(service_probe().state == MODEM_SERVICE_TEST_STATE_MODULE_WAIT &&
              s_mh_rail_enabled,
          "invalid-status fixture reaches the bounded module wait");
    s_mh_status_valid = false;
    mh_advance(g_modem_vendor.power.ready_budget_ms);
    probe = service_probe();
    check(probe.state == MODEM_SERVICE_TEST_STATE_FAILED &&
              s_mh_rail_enabled && probe.failed_module_may_be_live,
          "invalid PWRMON retains the rail instead of masquerading as OFF");
    modem_service_power_on();
    probe = service_probe();
    check(probe.state == MODEM_SERVICE_TEST_STATE_MODULE_WAIT &&
              s_mh_rail_enabled && probe.uart_parked,
          "retry requalifies an indeterminate retained module without a rail cut");

    begin_telit(true);
    s_mh_module_boots = false;
    modem_service_power_on();
    mh_advance(service_probe().rail_off_dwell_ms);
    mh_advance(g_modem_vendor.power.rail_settle_ms);
    mh_advance(g_modem_vendor.power.pwron_pulse_ms);
    s_mh_status_raw = 500u; /* deliberately inside the vendor hysteresis gap */
    s_mh_status_mv = 403u;
    mh_advance(g_modem_vendor.power.ready_budget_ms);
    probe = service_probe();
    check(probe.state == MODEM_SERVICE_TEST_STATE_FAILED &&
              s_mh_rail_enabled && probe.failed_module_may_be_live,
          "mid-threshold PWRMON is indeterminate and cannot authorize a rail cut");
}

static void test_sim_absent_locked_and_cme_fallback(void) {
    begin_telit(true);
    s_mh_sim_present = false;
    s_mh_sim_ready = false;
    check(boot_until_ready(30000u),
          "known-absent SIM is nonfatal and does not wait for registration");
    modem_status_t status = mh_status();
    check(status.sim_checked && !status.sim_present && !status.sim_ready,
          "QSS status 0 publishes checked/absent/not-ready");
    check(!status.provisioning_verified,
          "SIM-gated rows prevent a false complete provisioning record");
    check(mh_tx_count_exact("AT#QSS?") == 1u &&
              mh_tx_count_exact("AT+CPIN?") == 0u,
          "first trustworthy QSS absence skips the CPIN retry ladder");

    s_mh_sim_present = true;
    s_mh_sim_ready = true;
    s_clcc_row = "+CLCC: 1,0,0,0,0,\"15551234567\",129";
    modem_service_test_set_call_session_active(true);
    mh_rx_push("#QSS: 3");
    mh_advance(500u);
    check(service_probe().sim_completion_pending &&
              mh_tx_count_exact("AT+CMGF=1") == 0u,
          "hot-SIM setup waits until an established call is gone");
    modem_service_test_set_call_session_active(false);
    bool completion_done = false;
    for (uint32_t elapsed = 0u; elapsed < 20000u; elapsed += 100u) {
        mh_advance(100u);
        modem_service_test_snapshot_t probe = service_probe();
        if (probe.state == MODEM_SERVICE_TEST_STATE_READY &&
            !probe.sim_completion_active && !probe.sim_completion_pending &&
            mh_tx_count_exact("AT+CMGF=1") == 1u) {
            completion_done = true;
            break;
        }
    }
    status = mh_status();
    check(completion_done && status.sim_present && status.sim_ready &&
              status.provisioning_verified,
          "QSS readiness edge completes deferred SIM setup without a cold boot");
    check(mh_tx_count_exact("AT+CPIN?") == 1u &&
              mh_tx_count_exact("AT+CMGF=1") == 1u &&
              mh_tx_count_exact("AT+CSCS=\"GSM\"") == 1u &&
              mh_tx_count_exact("AT+CLIP=1") == 1u &&
              mh_tx_count_exact("AT+CCWA=1") == 1u &&
              mh_tx_count_exact("AT#ECAM=1") == 1u &&
              mh_tx_count_exact("AT#MWI=1") == 1u &&
              mh_tx_count_exact("AT#WKIO?") == 1u &&
              mh_tx_count_exact("AT&V") == 1u &&
              mh_tx_count_exact("AT#E2SMSRI?") == 1u &&
              mh_tx_count_exact("AT#PSMRI?") == 2u &&
              mh_tx_count_exact(
                  "AT#WKIO=0;#E2SMSRI=0;\\R2&W0") == 0u &&
              mh_tx_count_exact("AT#PSMRI=1000") == 1u &&
              mh_tx_count_exact("AT+CFUN=1") == 1u &&
              mh_tx_count_exact("AT+CFUN=5") == 2u,
          "hot-SIM setup verifies the saved sleep-wake profile without rewriting it");
    check(mh_tx_count_exact("ATE0") == 1u &&
              mh_tx_count_exact("AT#RXDIV?") == 1u &&
              mh_tx_count_exact("AT+CPMS?") == 1u &&
              mh_tx_count_exact("AT#ECAMURC?") == 1u,
          "hot-SIM pass leaves non-SIM init alone and verifies SIM provisioning");
    settle_dtr_sleep();
    check(mh_tx_count_exact("AT#PSMRI=1000") == 1u &&
              service_probe().sms_wake_armed && s_psmri_live_armed,
          "hot-SIM completion qualifies SMS wake before DTR sleep");

    begin_telit(true);
    s_mh_sim_present = true;
    s_mh_sim_ready = false;
    check(boot_until_ready(30000u), "PIN-locked SIM reaches limited READY");
    status = mh_status();
    check(status.sim_checked && status.sim_present && !status.sim_ready,
          "PIN-locked card is distinguished from a missing card");
    check(!status.provisioning_verified,
          "PIN-locked boot does not claim SIM-gated provisioning complete");
    check(mh_tx_count_exact("AT+CPIN?") == 0u,
          "QSS present/not-ready state avoids a false CFUN=4 CPIN probe");

    begin_telit(true);
    s_qss_response_enabled = false;
    s_mh_sim_present = false;
    s_mh_sim_ready = false;
    check(boot_until_ready(30000u),
          "CPIN CME fallback remains nonfatal when QSS response is unavailable");
    status = mh_status();
    check(status.sim_checked && !status.sim_present && !status.sim_ready,
          "standardized CME 10/text fallback publishes missing SIM");
    check(mh_tx_count_exact("AT+CPIN?") == 1u,
          "conclusive CPIN missing-SIM final is not retried");
}

static void test_mismatch_writes_verifies_and_reboots_once(void) {
    begin_telit(false);
    s_ring_pulse_mode = false;
    check(boot_until_ready(60000u),
          "partially configured module converges through one reboot");
    check(mh_tx_count_exact("AT#RXDIV=0,1") == 1u,
          "mismatched RXDIV is written exactly once");
    check(mh_tx_count_exact("AT#REBOOT") == 1u,
          "reboot-required changes share one controlled reboot");
    check(s_mh_uart_init_count == 3u && s_mh_uart_park_count == 2u &&
              service_probe().provision_reboot_drop_seen &&
              !service_probe().provision_reboot_cycle_active,
          "reboot waits for PWRMON low, parks UART, then accepts the new boot");
    check(mh_tx_count_exact(
                  "AT#WKIO=0;#E2SMSRI=0;\\R2&W0") == 1u &&
              !s_wkio_configured && s_ring_pulse_mode &&
              s_e2smsri_ms == 0u && s_psmri_ms == 1000u,
          "focused pass repairs the wake profile after the controlled reboot");
    check(mh_tx_count_exact("AT#RXDIV?") >= 3u,
          "RXDIV is queried before write, verified, and checked after reboot");
    settle_dtr_sleep();
    check(service_probe().sms_wake_armed && s_psmri_live_armed,
          "post-reboot focused lifecycle qualifies generic SMS wake");

    graceful_power_off();
    mh_clear_tx_capture();
    check(boot_until_ready(30000u), "second cold boot reaches READY");
    settle_dtr_sleep();
    check(mh_tx_count_exact("AT#RXDIV=0,1") == 0u &&
              mh_tx_count_exact("AT#REBOOT") == 0u &&
              mh_tx_count_exact(
                  "AT#WKIO=0;#E2SMSRI=0;\\R2&W0") == 0u &&
              mh_tx_count_exact("AT#PSMRI=1000") == 1u &&
              s_psmri_live_armed,
          "normal second boot avoids NVM and reapplies runtime PSMRI once");
}

static void test_sled_mismatch_is_saved_without_reboot(void) {
    begin_telit(true);
    s_sled_mode = 2u;
    s_sled_saved = false;

    check(boot_until_ready(30000u),
          "STAT_LED mismatch converges without a module reboot");
    check(s_sled_mode == 5u && s_sled_saved &&
              mh_tx_count_exact("AT#SLED=5;#SLEDSAV") == 1u &&
              mh_tx_count_exact("AT#SLED?") == 2u &&
              mh_tx_count_exact("AT#REBOOT") == 0u &&
              mh_status().provisioning_verified,
          "STAT_LED mode is saved once and proven by strict readback");

    graceful_power_off();
    size_t writes_before = mh_tx_count_exact("AT#SLED=5;#SLEDSAV");
    check(boot_until_ready(30000u), "saved STAT_LED fixture reboots normally");
    check(mh_tx_count_exact("AT#SLED=5;#SLEDSAV") == writes_before,
          "subsequent boot does not rewrite matching STAT_LED NVM");
}

static void test_each_sms_wake_profile_mismatch_converges(void) {
    for (unsigned mismatch = 0u; mismatch < 4u; mismatch++) {
        begin_telit(true);
        if (mismatch == 0u) {
            s_wkio_configured = true;
        } else if (mismatch == 1u) {
            s_ring_pulse_mode = false;
        } else if (mismatch == 2u) {
            s_e2smsri_ms = 1000u;
        } else {
            s_psmri_ms = 0u;
        }

        check(boot_until_ready(60000u) &&
                  mh_status().provisioning_verified,
              "each isolated SMS wake mismatch converges and verifies");
        check(mh_tx_count_exact("AT#REBOOT") == 0u &&
                  mh_tx_count_exact(
                      "AT#WKIO=0;#E2SMSRI=0;\\R2&W0") ==
                      (mismatch < 3u ? 1u : 0u) &&
                  !s_wkio_configured && s_ring_pulse_mode &&
                  s_e2smsri_ms == 0u && s_psmri_ms == 1000u &&
                  service_probe().sms_wake_armed && s_psmri_live_armed,
              "only persistent wake mismatches write the saved profile");
        check(mh_tx_count_exact("AT#PSMRI=1000") == 1u,
              "every lifecycle applies PSMRI once before the final CFUN");
    }
}

static void test_late_psmri_invalidates_and_replays_lifecycle(void) {
    begin_telit(true);
    check(boot_until_ready(30000u), "late-PSMRI recovery fixture boots");
    size_t cfun1_before = mh_tx_count_exact("AT+CFUN=1");
    size_t cfun5_before = mh_tx_count_exact("AT+CFUN=5");

    check(modem_service_request_debug_at("AT#PSMRI=1000"),
          "debug PSMRI mutation is admitted");
    mh_settle();
    check(s_late_psmri_invalidated_latch,
          "post-CFUN PSMRI write demonstrably breaks the hardware proof");

    for (uint32_t elapsed = 0u; elapsed < 20000u; elapsed += 100u) {
        mh_advance(100u);
        modem_service_test_snapshot_t probe = service_probe();
        if (probe.state == MODEM_SERVICE_TEST_STATE_READY &&
            probe.sms_wake_armed && s_psmri_live_armed &&
            !probe.sim_completion_pending) {
            break;
        }
    }
    check(service_probe().sms_wake_armed && s_psmri_live_armed &&
              mh_tx_count_exact("AT+CFUN=1") == cfun1_before + 1u &&
              mh_tx_count_exact("AT+CFUN=5") == cfun5_before + 1u,
          "invalidating maintenance replays the documented lifecycle once");
    check(tx_last_index("AT#PSMRI?") < tx_last_index("AT+CFUN=5"),
          "recovery leaves PSMRI verification before final CFUN=5");
}

static void test_sms_text_send_contract(void) {
    static const char number[] = "+15551234567";
    static const char body[] = "Bench text";
    static const char cmgs[] = "AT+CMGS=\"+15551234567\",145";
    static const char cmgw[] =
        "AT+CMGW=\"+15551234567\",145,\"STO SENT\"";
    static const uint8_t sub = 0x1au;

    if (!begin_sms_operation_fixture("text-send fixture boots")) {
        return;
    }
    uint32_t sent_before = mh_status().sms_sent_count;
    check(modem_service_request_send_sms(number, body),
          "text send is admitted");
    mh_settle();

    size_t cpms_event = tx_event_cstr(
        "AT+CPMS=\"ME\",\"ME\",\"ME\"", 0u);
    size_t cmgs_event = tx_event_cstr(cmgs, cpms_event + 1u);
    size_t body_event = tx_event_raw(body, strlen(body), cmgs_event + 1u);
    size_t first_sub = tx_event_raw(&sub, 1u, body_event + 1u);
    size_t cmgw_event = tx_event_cstr(cmgw, first_sub + 1u);
    size_t copy_body = tx_event_raw(body, strlen(body), cmgw_event + 1u);
    size_t second_sub = tx_event_raw(&sub, 1u, copy_body + 1u);
    modem_sms_send_result_t result;
    check(cpms_event != SIZE_MAX && cmgs_event != SIZE_MAX &&
              body_event != SIZE_MAX && first_sub != SIZE_MAX &&
              cmgw_event != SIZE_MAX && copy_body != SIZE_MAX &&
              second_sub != SIZE_MAX,
          "text send preserves CPMS, CMGS body, and sent-copy ordering");
    check(mh_tx_count_exact(cmgs) == 1u &&
              mh_tx_count_exact(cmgw) == 1u &&
              tx_event_count_raw_byte(0x1au) == 2u,
          "text send emits one network submit and one sent-copy submit");
    check(modem_service_pop_sms_send_result(&result) &&
              result.request_id == s_last_sms_request_id &&
              result.kind == MODEM_SMS_REQUEST_SEND_TEXT &&
              result.outcome == MODEM_SMS_OUTCOME_OK &&
              !modem_service_pop_sms_send_result(&result) &&
              mh_status().sms_sent_count == sent_before + 1u &&
              !mh_status().operation_busy,
          "text send publishes one success and increments its counter once");

    static const struct {
        sms_flow_fault_t fault;
        uint32_t timeout_ms;
        size_t expected_subs;
        size_t expected_escs;
        const char *name;
    } copy_failures[] = {
        {SMS_FLOW_FAULT_CMGW_PROMPT_ERROR, 0u, 1u, 0u,
         "sent-copy prompt error"},
        {SMS_FLOW_FAULT_CMGW_PROMPT_TIMEOUT, 5001u, 1u, 1u,
         "sent-copy prompt timeout"},
        {SMS_FLOW_FAULT_CMGW_FINAL_ERROR, 0u, 2u, 0u,
         "sent-copy final error"},
        {SMS_FLOW_FAULT_CMGW_FINAL_TIMEOUT, 30001u, 2u, 0u,
         "sent-copy final timeout"},
    };
    for (size_t i = 0u;
         i < sizeof(copy_failures) / sizeof(copy_failures[0]); i++) {
        if (!begin_sms_operation_fixture(copy_failures[i].name)) {
            return;
        }
        sent_before = mh_status().sms_sent_count;
        s_sms_flow_fault = copy_failures[i].fault;
        check(modem_service_request_send_sms(number, body),
              "sent-copy failure send is admitted");
        mh_settle();
        if (copy_failures[i].timeout_ms != 0u) {
            mh_advance(copy_failures[i].timeout_ms);
        }
        check(modem_service_pop_sms_send_result(&result) &&
                  result.request_id == s_last_sms_request_id &&
                  result.kind == MODEM_SMS_REQUEST_SEND_TEXT &&
                  result.outcome == MODEM_SMS_OUTCOME_OK &&
                  !modem_service_pop_sms_send_result(&result) &&
                  mh_status().sms_sent_count == sent_before + 1u &&
                  mh_tx_count_exact(cmgw) == 1u &&
                  tx_event_count_raw_byte(0x1au) ==
                      copy_failures[i].expected_subs &&
                  tx_event_count_raw_byte(0x1bu) ==
                      copy_failures[i].expected_escs &&
                  !mh_status().operation_busy,
              "network-accepted text survives every sent-copy failure phase");
    }
}

static void test_sms_text_send_failures_and_prompt_settle(void) {
    static const char cmgs[] = "AT+CMGS=\"5550100\"";
    static const char body[] = "Failure probe";
    static const struct {
        sms_flow_fault_t fault;
        uint32_t timeout_ms;
        bool expect_cmgs;
        bool expect_body;
        const char *name;
    } cases[] = {
        {SMS_FLOW_FAULT_CPMS_ERROR, 0u, false, false, "CPMS error"},
        {SMS_FLOW_FAULT_CPMS_TIMEOUT, 5001u, false, false, "CPMS timeout"},
        {SMS_FLOW_FAULT_CMGS_PROMPT_ERROR, 0u, true, false,
         "CMGS prompt error"},
        {SMS_FLOW_FAULT_CMGS_FINAL_ERROR, 0u, true, true,
         "CMGS final error"},
        {SMS_FLOW_FAULT_CMGS_FINAL_TIMEOUT, 30001u, true, true,
         "CMGS final timeout"},
    };

    for (size_t i = 0u; i < sizeof(cases) / sizeof(cases[0]); i++) {
        if (!begin_sms_operation_fixture(cases[i].name)) {
            return;
        }
        uint32_t sent_before = mh_status().sms_sent_count;
        s_sms_flow_fault = cases[i].fault;
        check(modem_service_request_send_sms("5550100", body),
              "failed text-send case is admitted");
        mh_settle();
        if (cases[i].timeout_ms != 0u) {
            mh_advance(cases[i].timeout_ms);
        }
        modem_sms_send_result_t result;
        modem_sms_outcome_t expected_outcome =
            cases[i].fault == SMS_FLOW_FAULT_CPMS_TIMEOUT ||
                    cases[i].fault == SMS_FLOW_FAULT_CMGS_FINAL_TIMEOUT
                ? (cases[i].fault == SMS_FLOW_FAULT_CMGS_FINAL_TIMEOUT
                       ? MODEM_SMS_OUTCOME_UNCERTAIN
                       : MODEM_SMS_OUTCOME_TIMEOUT)
                : MODEM_SMS_OUTCOME_ERROR;
        check(modem_service_pop_sms_send_result(&result) &&
                  result.request_id == s_last_sms_request_id &&
                  result.kind == MODEM_SMS_REQUEST_SEND_TEXT &&
                  result.outcome == expected_outcome &&
                  !modem_service_pop_sms_send_result(&result) &&
                  !mh_status().operation_busy,
              "failed text-send case publishes one terminal failure");
        check((mh_tx_count_exact(cmgs) != 0u) == cases[i].expect_cmgs &&
                  (tx_event_raw(body, strlen(body), 0u) != SIZE_MAX) ==
                      cases[i].expect_body &&
                  mh_status().sms_sent_count == sent_before &&
                  mh_tx_count_exact(
                      "AT+CMGW=\"5550100\",129,\"STO SENT\"") == 0u,
              "failed text-send case cannot leak into a later phase");
    }

    if (!begin_sms_operation_fixture("lost text-prompt fixture boots")) {
        return;
    }
    s_sms_flow_fault = SMS_FLOW_FAULT_CMGS_PROMPT_TIMEOUT;
    s_sms_esc_returns_final = true;
    check(modem_service_request_send_sms("5550100", body),
          "lost-prompt send is admitted");
    mh_settle();
    check(service_probe().command_active &&
              tx_event_count_raw_byte(0x1bu) == 0u,
          "lost prompt remains active until its command deadline");
    check(modem_service_request_debug_at("AT+CSQ"),
          "following command queues behind the lost prompt");
    mh_advance(5001u);
    size_t esc_event = tx_event_raw_byte(0x1bu, 0u);
    modem_sms_send_result_t result;
    check(esc_event != SIZE_MAX &&
              tx_event_count_raw_byte(0x1bu) == 1u &&
              tx_event_cstr("AT+CSQ", 0u) == SIZE_MAX &&
              modem_service_pop_sms_send_result(&result) &&
              result.request_id == s_last_sms_request_id &&
              result.outcome == MODEM_SMS_OUTCOME_TIMEOUT,
          "prompt timeout writes one ESC and holds the queued command");
    mh_advance(499u);
    check(tx_event_cstr("AT+CSQ", 0u) == SIZE_MAX,
          "queued command remains held for the full ESC settle");
    mh_advance(1u);
    size_t debug_event = tx_event_cstr(
        "AT+CSQ", esc_event == SIZE_MAX ? 0u : esc_event + 1u);
    bool settle_observed = esc_event != SIZE_MAX &&
        debug_event != SIZE_MAX &&
        s_mh_tx_events[debug_event].at_ms -
                s_mh_tx_events[esc_event].at_ms >= 500u;
    modem_debug_result_t debug_result;
    check(settle_observed &&
              modem_service_pop_debug_result(&debug_result) &&
              debug_result.ok && !mh_status().operation_busy,
          "ESC final drains as an orphan before the queued command dispatches");
}

static void test_sms_save_and_delete_contracts(void) {
    static const char body[] = "Saved draft";
    static const char cmgw[] = "AT+CMGW=\"+15550001111\",145";

    if (!begin_sms_operation_fixture("save-SMS fixture boots")) {
        return;
    }
    check(modem_service_request_save_sms("+15550001111", body),
          "save-to-outbox is admitted");
    mh_settle();
    size_t cmgw_event = tx_event_cstr(cmgw, 0u);
    modem_sms_save_result_t save_result;
    check(cmgw_event != SIZE_MAX &&
              tx_event_raw(body, strlen(body), cmgw_event + 1u) != SIZE_MAX &&
              tx_event_count_raw_byte(0x1au) == 1u &&
              modem_service_pop_sms_save_result(&save_result) &&
              save_result.request_id == s_last_sms_request_id &&
              save_result.kind == MODEM_SMS_REQUEST_SAVE &&
              save_result.outcome == MODEM_SMS_OUTCOME_OK &&
              !save_result.sim_not_ready &&
              !modem_service_pop_sms_save_result(&save_result),
          "save-to-outbox publishes one success after its CMGW body");

    static const struct {
        sms_flow_fault_t fault;
        uint32_t timeout_ms;
        size_t expected_subs;
        size_t expected_escs;
        const char *name;
    } save_failures[] = {
        {SMS_FLOW_FAULT_CMGW_PROMPT_ERROR, 0u, 0u, 0u,
         "save prompt error"},
        {SMS_FLOW_FAULT_CMGW_PROMPT_TIMEOUT, 5001u, 0u, 1u,
         "save prompt timeout"},
        {SMS_FLOW_FAULT_CMGW_FINAL_ERROR, 0u, 1u, 0u,
         "save final error"},
        {SMS_FLOW_FAULT_CMGW_FINAL_TIMEOUT, 30001u, 1u, 0u,
         "save final timeout"},
    };
    for (size_t i = 0u;
         i < sizeof(save_failures) / sizeof(save_failures[0]); i++) {
        if (!begin_sms_operation_fixture(save_failures[i].name)) {
            return;
        }
        s_sms_flow_fault = save_failures[i].fault;
        check(modem_service_request_save_sms("+15550001111", body),
              "save failure case is admitted");
        mh_settle();
        if (save_failures[i].timeout_ms != 0u) {
            mh_advance(save_failures[i].timeout_ms);
        }
        modem_sms_outcome_t expected_outcome =
            save_failures[i].fault == SMS_FLOW_FAULT_CMGW_PROMPT_TIMEOUT
                ? MODEM_SMS_OUTCOME_TIMEOUT
                : (save_failures[i].fault == SMS_FLOW_FAULT_CMGW_FINAL_TIMEOUT
                       ? MODEM_SMS_OUTCOME_UNCERTAIN
                       : MODEM_SMS_OUTCOME_ERROR);
        check(modem_service_pop_sms_save_result(&save_result) &&
                  save_result.request_id == s_last_sms_request_id &&
                  save_result.kind == MODEM_SMS_REQUEST_SAVE &&
                  save_result.outcome == expected_outcome &&
                  !save_result.sim_not_ready &&
                  !modem_service_pop_sms_save_result(&save_result) &&
                  mh_tx_count_exact(cmgw) == 1u &&
                  tx_event_count_raw_byte(0x1au) ==
                      save_failures[i].expected_subs &&
                  tx_event_count_raw_byte(0x1bu) ==
                      save_failures[i].expected_escs &&
                  !mh_status().operation_busy,
              "save failure phase publishes one terminal result and unwinds");
    }

    if (!begin_sms_operation_fixture("save missing-SIM fixture boots")) {
        return;
    }
    s_mh_sim_ready = false;
    mh_feed("#QSS: 2,1");
    mh_clear_tx_capture();
    s_sms_flow_fault = SMS_FLOW_FAULT_CPMS_ERROR;
    check(modem_service_request_save_sms("", body),
          "missing-SIM save is admitted for classified failure");
    mh_settle();
    check(modem_service_pop_sms_save_result(&save_result) &&
              save_result.request_id == s_last_sms_request_id &&
              save_result.outcome == MODEM_SMS_OUTCOME_ERROR &&
              save_result.sim_not_ready &&
              mh_tx_count_exact("AT+CMGW") == 0u,
          "CPMS failure reports SIM-not-ready without opening CMGW");

    if (!begin_sms_operation_fixture("single delete fixture boots")) {
        return;
    }
    const uint16_t zero_index = 0u;
    check(modem_service_request_delete_sms_indices(&zero_index, 1u),
          "zero-based SMS index is admitted for deletion");
    mh_settle();
    modem_sms_delete_result_t delete_result;
    check(mh_tx_count_exact("AT+CMGD=0") == 1u &&
              modem_service_pop_sms_delete_result(&delete_result) &&
              delete_result.request_id == s_last_sms_request_id &&
              delete_result.kind == MODEM_SMS_REQUEST_DELETE &&
              delete_result.outcome == MODEM_SMS_OUTCOME_OK &&
              !delete_result.sim_not_ready &&
              !modem_service_pop_sms_delete_result(&delete_result),
          "single delete publishes one success for index zero");

    if (!begin_sms_operation_fixture("multi-delete failure fixture boots")) {
        return;
    }
    const uint16_t indices[] = {3u, 4u, 5u};
    s_sms_flow_fault = SMS_FLOW_FAULT_DELETE_SECOND_ERROR;
    check(modem_service_request_delete_sms_indices(indices, 3u),
          "multi-index delete is admitted");
    mh_settle();
    check(mh_tx_count_exact("AT+CMGD=3") == 1u &&
              mh_tx_count_exact("AT+CMGD=4") == 1u &&
              mh_tx_count_exact("AT+CMGD=5") == 1u &&
              modem_service_pop_sms_delete_result(&delete_result) &&
              delete_result.request_id == s_last_sms_request_id &&
              delete_result.outcome == MODEM_SMS_OUTCOME_ERROR &&
              !delete_result.sim_not_ready &&
              !modem_service_pop_sms_delete_result(&delete_result) &&
              !mh_status().operation_busy,
          "multi-delete continues after one error and publishes one failure");
}

static bool build_expected_binary_segment(sms_submit_pdu_t *submit,
                                          char *pdu,
                                          uint8_t *tpdu_len) {
    return sms_submit_pdu_build(submit, pdu, SMS_SUBMIT_PDU_HEX_MAX,
                                tpdu_len);
}

static void test_sms_binary_send_contract(void) {
    static const char number[] = "+15551230000";
    static const uint8_t sub = 0x1au;
    uint8_t payload[] = {0x01u, 0x23u, 0x45u, 0x67u, 0x89u};

    if (!begin_sms_operation_fixture("binary-send fixture boots")) {
        return;
    }
    sms_submit_pdu_t expected = {
        .number = number,
        .payload = payload,
        .payload_len = sizeof(payload),
        .dest_port = 0x158au,
        .source_port = 0u,
        .mode = MODEM_BINARY_SMS_MODE_DCS04_PORT_FIRST,
        .segment = 1u,
        .segment_total = 1u,
        .reference = 1u,
    };
    char expected_pdu[SMS_SUBMIT_PDU_HEX_MAX];
    uint8_t expected_tpdu = 0u;
    check(build_expected_binary_segment(&expected, expected_pdu,
                                        &expected_tpdu),
          "one-segment binary golden builds independently");
    uint32_t sent_before = mh_status().sms_sent_count;
    check(modem_service_request_send_binary_sms(
              number, payload, sizeof(payload), 0x158au, 0u),
          "one-segment binary SMS is admitted");
    mh_settle();
    char cmgs[24];
    snprintf(cmgs, sizeof(cmgs), "AT+CMGS=%u", (unsigned)expected_tpdu);
    size_t pdu_mode = tx_event_cstr("AT+CMGF=0", 0u);
    size_t cmgs_event = tx_event_cstr(cmgs, pdu_mode + 1u);
    size_t pdu_event = tx_event_raw(expected_pdu, strlen(expected_pdu),
                                    cmgs_event + 1u);
    size_t sub_event = tx_event_raw(&sub, 1u, pdu_event + 1u);
    size_t text_mode = tx_event_cstr("AT+CMGF=1", sub_event + 1u);
    modem_sms_send_result_t result;
    check(pdu_mode != SIZE_MAX && cmgs_event != SIZE_MAX &&
              pdu_event != SIZE_MAX && sub_event != SIZE_MAX &&
              text_mode != SIZE_MAX,
          "one-segment binary send emits exact PDU-mode wire order");
    check(modem_service_pop_sms_send_result(&result) &&
              result.request_id == s_last_sms_request_id &&
              result.kind == MODEM_SMS_REQUEST_SEND_BINARY &&
              result.outcome == MODEM_SMS_OUTCOME_OK &&
              !modem_service_pop_sms_send_result(&result) &&
              mh_status().sms_sent_count == sent_before + 1u &&
              mh_tx_count_exact("AT+CMGF=1") == 1u,
          "one-segment binary send restores text mode and completes once");

    uint8_t multipart[130];
    for (size_t i = 0u; i < sizeof(multipart); i++) {
        multipart[i] = (uint8_t)i;
    }
    mh_clear_tx_capture();
    expected = (sms_submit_pdu_t){
        .number = number,
        .payload = multipart,
        .payload_len = sizeof(multipart),
        .dest_port = 0x158au,
        .source_port = 0u,
        .mode = MODEM_BINARY_SMS_MODE_DCS04_PORT_FIRST,
        .segment = 1u,
        .segment_total = 2u,
        .reference = 2u,
    };
    char pdu1[SMS_SUBMIT_PDU_HEX_MAX];
    char pdu2[SMS_SUBMIT_PDU_HEX_MAX];
    uint8_t tpdu1 = 0u;
    uint8_t tpdu2 = 0u;
    check(build_expected_binary_segment(&expected, pdu1, &tpdu1) &&
              build_expected_binary_segment(&expected, pdu2, &tpdu2) &&
              expected.position == sizeof(multipart) &&
              expected.segment == 3u,
          "multipart binary goldens advance one shared reference monotonically");
    sent_before = mh_status().sms_sent_count;
    check(modem_service_request_send_binary_sms(
              number, multipart, sizeof(multipart), 0x158au, 0u),
          "multipart binary SMS is admitted");
    mh_settle();
    char cmgs1[24];
    char cmgs2[24];
    snprintf(cmgs1, sizeof(cmgs1), "AT+CMGS=%u", (unsigned)tpdu1);
    snprintf(cmgs2, sizeof(cmgs2), "AT+CMGS=%u", (unsigned)tpdu2);
    pdu_mode = tx_event_cstr("AT+CMGF=0", 0u);
    size_t cmgs1_event = tx_event_cstr(cmgs1, pdu_mode + 1u);
    size_t pdu1_event = tx_event_raw(pdu1, strlen(pdu1), cmgs1_event + 1u);
    size_t sub1_event = tx_event_raw(&sub, 1u, pdu1_event + 1u);
    size_t cmgs2_event = tx_event_cstr(cmgs2, sub1_event + 1u);
    size_t pdu2_event = tx_event_raw(pdu2, strlen(pdu2), cmgs2_event + 1u);
    size_t sub2_event = tx_event_raw(&sub, 1u, pdu2_event + 1u);
    text_mode = tx_event_cstr("AT+CMGF=1", sub2_event + 1u);
    check(pdu_mode != SIZE_MAX && cmgs1_event != SIZE_MAX &&
              pdu1_event != SIZE_MAX && sub1_event != SIZE_MAX &&
              cmgs2_event != SIZE_MAX && pdu2_event != SIZE_MAX &&
              sub2_event != SIZE_MAX && text_mode != SIZE_MAX &&
              tx_event_count_raw_byte(0x1au) == 2u,
          "multipart send emits both exact PDUs before one text-mode restore");
    check(modem_service_pop_sms_send_result(&result) &&
              result.request_id == s_last_sms_request_id &&
              result.kind == MODEM_SMS_REQUEST_SEND_BINARY &&
              result.outcome == MODEM_SMS_OUTCOME_OK &&
              !modem_service_pop_sms_send_result(&result) &&
              mh_status().sms_sent_count == sent_before + 1u &&
              mh_tx_count_exact("AT+CMGF=1") == 1u,
          "multipart send publishes and counts one logical SMS, not segments");
}

static void test_sms_binary_failures_and_prompt_settle(void) {
    static const uint8_t payload[] = {0x10u, 0x20u, 0x30u};
    static const struct {
        sms_flow_fault_t fault;
        uint32_t timeout_ms;
        size_t restore_count;
        modem_sms_outcome_t outcome;
        bool counted;
        const char *name;
    } cases[] = {
        {SMS_FLOW_FAULT_CMGF_PDU_ERROR, 0u, 0u, MODEM_SMS_OUTCOME_ERROR, false,
         "binary PDU-mode error"},
        {SMS_FLOW_FAULT_CMGF_PDU_TIMEOUT, 5001u, 1u,
         MODEM_SMS_OUTCOME_TIMEOUT, false,
         "binary PDU-mode timeout"},
        {SMS_FLOW_FAULT_CMGS_PROMPT_ERROR, 0u, 1u,
         MODEM_SMS_OUTCOME_ERROR, false,
         "binary prompt error"},
        {SMS_FLOW_FAULT_CMGS_FINAL_ERROR, 0u, 1u,
         MODEM_SMS_OUTCOME_ERROR, false,
         "binary segment-final error"},
        {SMS_FLOW_FAULT_CMGS_FINAL_TIMEOUT, 120001u, 1u,
         MODEM_SMS_OUTCOME_UNCERTAIN, false,
         "binary segment-final timeout"},
        /* A failed text-mode restore completes the operation (OK, counted)
         * and arms the idle text-mode restore: the persistent ERROR fault
         * then burns its three bounded retries (1 + 3); the timeout fault
         * sees the first idle retry inside the window (1 + 1). */
        {SMS_FLOW_FAULT_CMGF_TEXT_ERROR, 0u, 4u,
         MODEM_SMS_OUTCOME_OK, true,
         "binary text-restore error"},
        {SMS_FLOW_FAULT_CMGF_TEXT_TIMEOUT, 5001u, 2u,
         MODEM_SMS_OUTCOME_OK, true,
         "binary text-restore timeout"},
    };

    for (size_t i = 0u; i < sizeof(cases) / sizeof(cases[0]); i++) {
        if (!begin_sms_operation_fixture(cases[i].name)) {
            return;
        }
        uint32_t sent_before = mh_status().sms_sent_count;
        s_sms_flow_fault = cases[i].fault;
        check(modem_service_request_send_binary_sms(
                  "5550101", payload, sizeof(payload), 0x158au, 0u),
              "binary failure case is admitted");
        mh_settle();
        if (cases[i].timeout_ms != 0u) {
            mh_advance(cases[i].timeout_ms);
        }
        modem_sms_send_result_t result;
        bool have_result = modem_service_pop_sms_send_result(&result);
        bool terminal_ok = have_result &&
                  result.request_id == s_last_sms_request_id &&
                  result.kind == MODEM_SMS_REQUEST_SEND_BINARY &&
                  result.outcome == cases[i].outcome &&
                  !modem_service_pop_sms_send_result(&result) &&
                  !mh_status().operation_busy;
        bool restore_ok =
            mh_tx_count_exact("AT+CMGF=1") == cases[i].restore_count &&
                  mh_status().sms_sent_count ==
                      sent_before + (cases[i].counted ? 1u : 0u);
        if (!terminal_ok || !restore_ok) {
            fprintf(stderr,
                    "SMS matrix case: %s (have=%u outcome=%u restore=%zu count=%lu busy=%u)\n",
                    cases[i].name, have_result ? 1u : 0u,
                    have_result ? (unsigned)result.outcome : 0u,
                    mh_tx_count_exact("AT+CMGF=1"),
                    (unsigned long)(mh_status().sms_sent_count - sent_before),
                    mh_status().operation_busy ? 1u : 0u);
        }
        check(terminal_ok,
              "binary failure case publishes one terminal public result");
        check(restore_ok,
              "binary failure case preserves current restore and count semantics");
    }

    if (!begin_sms_operation_fixture("lost binary-prompt fixture boots")) {
        return;
    }
    s_sms_flow_fault = SMS_FLOW_FAULT_CMGS_PROMPT_TIMEOUT;
    s_sms_esc_returns_final = true;
    check(modem_service_request_send_binary_sms(
              "5550101", payload, sizeof(payload), 0x158au, 0u),
          "lost binary prompt is admitted");
    mh_settle();
    check(modem_service_request_debug_at("AT+CSQ"),
          "debug command queues behind lost binary prompt");
    mh_advance(5001u);
    size_t esc_event = tx_event_raw_byte(0x1bu, 0u);
    modem_sms_send_result_t result;
    check(esc_event != SIZE_MAX && mh_tx_count_exact("AT+CMGF=1") == 0u &&
              !modem_service_pop_sms_send_result(&result) &&
              tx_event_cstr("AT+CSQ", 0u) == SIZE_MAX,
          "binary prompt timeout defers both restore and queued command");
    mh_advance(499u);
    check(mh_tx_count_exact("AT+CMGF=1") == 0u,
          "binary restore remains deferred through the ESC quarantine");
    mh_advance(1u);
    size_t restore_event = tx_event_cstr(
        "AT+CMGF=1", esc_event == SIZE_MAX ? 0u : esc_event + 1u);
    size_t debug_event = tx_event_cstr(
        "AT+CSQ", restore_event == SIZE_MAX ? 0u : restore_event + 1u);
    bool settle_observed = esc_event != SIZE_MAX &&
        restore_event != SIZE_MAX &&
        s_mh_tx_events[restore_event].at_ms -
                s_mh_tx_events[esc_event].at_ms >= 500u;
    modem_debug_result_t debug_result;
    check(settle_observed && debug_event != SIZE_MAX &&
              mh_tx_count_exact("AT+CMGF=1") == 1u &&
              modem_service_pop_sms_send_result(&result) &&
              result.request_id == s_last_sms_request_id &&
              result.kind == MODEM_SMS_REQUEST_SEND_BINARY &&
              result.outcome == MODEM_SMS_OUTCOME_TIMEOUT &&
              modem_service_pop_debug_result(&debug_result) &&
              debug_result.ok && !mh_status().operation_busy,
          "one binary restore completes after settle before later work consumes a final");
}

static bool request_sms_operation_for_transport_case(unsigned operation) {
    static const uint8_t binary[] = {0x41u, 0x42u};
    static const uint16_t index = 3u;
    switch (operation) {
    case 0u:
        return modem_service_request_send_sms("5550102", "CTS text");
    case 1u:
        return modem_service_request_send_binary_sms(
            "5550102", binary, sizeof(binary), 0x158au, 0u);
    case 2u:
        return modem_service_request_save_sms("5550102", "CTS draft");
    case 3u:
        return modem_service_request_sms_mailbox(MODEM_SMS_MAILBOX_INBOX);
    case 4u:
        return modem_service_request_sms_read(&index, 1u, false, 1u);
    case 5u:
        return modem_service_request_delete_sms_indices(&index, 1u);
    default:
        return false;
    }
}

static bool pop_sms_transport_result_for(unsigned operation,
                                         uint32_t request_id,
                                         modem_sms_outcome_t *outcome_out) {
    if (outcome_out == NULL) {
        return false;
    }
    *outcome_out = MODEM_SMS_OUTCOME_NONE;
    switch (operation) {
    case 0u:
    case 1u: {
        modem_sms_send_result_t result;
        bool have = (modem_service_pop_sms_send_result)(request_id, &result);
        *outcome_out = have ? result.outcome : MODEM_SMS_OUTCOME_NONE;
        return have && result.request_id == request_id &&
               result.kind == (operation == 0u
                   ? MODEM_SMS_REQUEST_SEND_TEXT
                   : MODEM_SMS_REQUEST_SEND_BINARY);
    }
    case 2u: {
        modem_sms_save_result_t result;
        bool have = (modem_service_pop_sms_save_result)(request_id, &result);
        *outcome_out = have ? result.outcome : MODEM_SMS_OUTCOME_NONE;
        return have && result.request_id == request_id &&
               result.kind == MODEM_SMS_REQUEST_SAVE;
    }
    case 3u: {
        modem_sms_mailbox_result_t result;
        bool have = (modem_service_pop_sms_mailbox_result)(request_id,
                                                           &result);
        *outcome_out = have ? result.outcome : MODEM_SMS_OUTCOME_NONE;
        return have && result.request_id == request_id &&
               result.kind == MODEM_SMS_REQUEST_MAILBOX;
    }
    case 4u: {
        modem_sms_read_result_t result;
        bool have = (modem_service_pop_sms_read_result)(request_id, &result);
        *outcome_out = have ? result.outcome : MODEM_SMS_OUTCOME_NONE;
        return have && result.request_id == request_id &&
               result.kind == MODEM_SMS_REQUEST_READ;
    }
    case 5u: {
        modem_sms_delete_result_t result;
        bool have = (modem_service_pop_sms_delete_result)(request_id,
                                                           &result);
        *outcome_out = have ? result.outcome : MODEM_SMS_OUTCOME_NONE;
        return have && result.request_id == request_id &&
               result.kind == MODEM_SMS_REQUEST_DELETE;
    }
    default:
        return false;
    }
}

static bool pop_sms_transport_result(unsigned operation,
                                     modem_sms_outcome_t *outcome_out) {
    return pop_sms_transport_result_for(operation, s_last_sms_request_id,
                                        outcome_out);
}

static bool sms_cpms_dispatched_later_phase(unsigned operation) {
    switch (operation) {
    case 0u:
        return mh_tx_count_exact("AT+CMGS=\"5550102\"") != 0u;
    case 2u:
        return mh_tx_count_exact("AT+CMGW=\"5550102\"") != 0u;
    case 3u:
    case 4u:
        return mh_tx_count_exact("AT#SMSUCS=1") != 0u;
    case 5u:
        return mh_tx_count_exact("AT+CMGD=3") != 0u;
    default:
        return true;
    }
}

static void test_sms_cpms_timeout_cancels_every_cpms_operation(void) {
    static const unsigned operations[] = {0u, 2u, 3u, 4u, 5u};
    static const char *const names[] = {
        "CPMS-timeout text send", "CPMS-timeout save",
        "CPMS-timeout mailbox", "CPMS-timeout read",
        "CPMS-timeout delete",
    };

    for (size_t i = 0u; i < sizeof(operations) / sizeof(operations[0]); i++) {
        if (!begin_sms_operation_fixture(names[i])) {
            return;
        }
        unsigned operation = operations[i];
        s_sms_flow_fault = SMS_FLOW_FAULT_CPMS_TIMEOUT;
        check(request_sms_operation_for_transport_case(operation),
              "CPMS-timeout SMS operation is admitted");
        mh_settle();
        check(service_probe().command_active && mh_status().operation_busy,
              "CPMS timeout remains in flight until its command deadline");
        /* Fault only the operation's command. CPMS invalidates Telit's SMS-wake
         * proof at dispatch, so timeout cleanup may legitimately replay CPMS as
         * part of the fresh SIM-completion lifecycle. That replay is recovery,
         * not continuation of the cancelled request. */
        s_sms_flow_fault = SMS_FLOW_FAULT_NONE;
        mh_advance(5001u);

        modem_sms_outcome_t outcome = MODEM_SMS_OUTCOME_NONE;
        bool have_result = pop_sms_transport_result(operation, &outcome);
        modem_service_test_snapshot_t probe = service_probe();
        size_t cpms_count = mh_tx_count_exact(
            "AT+CPMS=\"ME\",\"ME\",\"ME\"");
        bool later_phase = sms_cpms_dispatched_later_phase(operation);
        bool terminal = have_result && outcome == MODEM_SMS_OUTCOME_TIMEOUT &&
            !mh_status().operation_busy && probe.request_queue_depth == 0u &&
            cpms_count >= 1u && !later_phase;
        if (!terminal) {
            fprintf(stderr,
                    "CPMS timeout case %u: have=%u ok=%u busy=%u queue=%u "
                    "active=%u state=%u cpms=%zu later=%u\n",
                    operation, have_result ? 1u : 0u,
                    outcome == MODEM_SMS_OUTCOME_OK ? 1u : 0u,
                    mh_status().operation_busy ? 1u : 0u,
                    (unsigned)probe.request_queue_depth,
                    probe.command_active ? 1u : 0u, (unsigned)probe.state,
                    cpms_count, later_phase ? 1u : 0u);
        }
        check(terminal,
              "CPMS timeout publishes one failure without entering a later phase");
    }
}

static void test_sms_power_recovery_never_replays_transaction(void) {
    static const char number[] = "5550104";
    static const char body[] = "Recovery probe";
    static const char cmgs[] = "AT+CMGS=\"5550104\"";

    if (!begin_sms_operation_fixture("SMS power-recovery fixture boots")) {
        return;
    }
    s_sms_flow_fault = SMS_FLOW_FAULT_CMGS_FINAL_TIMEOUT;
    check(modem_service_request_send_sms(number, body),
          "SMS is admitted before runtime power recovery");
    mh_settle();
    check(service_probe().command_active && mh_status().operation_busy &&
              mh_tx_count_exact(cmgs) == 1u,
          "power fault is injected after the SMS body crossed the transport");

    s_mh_supply_pg = false;
    mh_advance(250u);
    modem_service_test_snapshot_t failed = service_probe();
    modem_sms_send_result_t stale_result;
    check(failed.state == MODEM_SERVICE_TEST_STATE_FAILED &&
              failed.failed_module_may_be_live &&
              failed.request_queue_depth == 0u &&
              !failed.command_active && !mh_status().operation_busy &&
              modem_service_pop_sms_send_result(&stale_result) &&
              stale_result.request_id == s_last_sms_request_id &&
              stale_result.kind == MODEM_SMS_REQUEST_SEND_TEXT &&
              stale_result.outcome == MODEM_SMS_OUTCOME_UNCERTAIN &&
              !modem_service_pop_sms_send_result(&stale_result),
          "runtime power fault publishes one uncertain terminal for the submitted SMS");

    s_sms_flow_fault = SMS_FLOW_FAULT_NONE;
    s_mh_supply_pg = true;
    modem_service_power_on();
    check(boot_until_ready(30000u),
          "retained modem recovers after an in-flight SMS power fault");
    check(mh_tx_count_exact(cmgs) == 1u,
          "recovery never replays the cancelled SMS transaction");

    mh_clear_tx_capture();
    check(modem_service_request_send_sms(number, "Fresh transaction"),
          "fresh SMS is admitted after modem recovery");
    mh_settle();
    check(modem_service_pop_sms_send_result(&stale_result) &&
              stale_result.request_id == s_last_sms_request_id &&
              stale_result.kind == MODEM_SMS_REQUEST_SEND_TEXT &&
              stale_result.outcome == MODEM_SMS_OUTCOME_OK &&
              !mh_status().operation_busy &&
              mh_tx_count_exact(cmgs) == 1u,
          "fresh post-recovery SMS completes without inherited state");
}

static void test_sms_cts_failure_cancels_every_operation(void) {
    static const char *const names[] = {
        "text send", "binary send", "save", "mailbox", "read", "delete",
    };
    /* No request crossed the wire: every reserved owner receives one
     * retry-safe cancellation, including binary mode and selected-read state. */

    for (unsigned operation = 0u;
         operation < sizeof(names) / sizeof(names[0]); operation++) {
        if (!begin_sms_operation_fixture(names[operation])) {
            return;
        }
        settle_dtr_sleep();
        mh_clear_tx_capture();
        s_mh_dtr_wake_works = false;
        s_mh_cts_asserted = false;
        check(request_sms_operation_for_transport_case(operation),
              "SMS operation is admitted before CTS failure");
        mh_settle();
        check(service_probe().deferred_command_valid &&
                  mh_status().operation_busy &&
                  s_mh_tx_history_count == 0u,
              "SMS operation remains deferred without writing through low CTS");
        mh_advance(g_modem_vendor.wake.dtr_wake_timeout_ms + 1u);

        modem_sms_outcome_t outcome = MODEM_SMS_OUTCOME_NONE;
        bool have_result = pop_sms_transport_result(operation, &outcome);
        modem_service_test_snapshot_t probe = service_probe();
        if (!have_result || outcome != MODEM_SMS_OUTCOME_CANCELLED ||
            mh_status().operation_busy || probe.deferred_command_valid ||
            probe.request_queue_depth != 0u ||
            probe.state != MODEM_SERVICE_TEST_STATE_FAILED) {
            fprintf(stderr,
                    "CTS matrix case: %s (have=%u outcome=%u busy=%u deferred=%u queue=%u state=%u)\n",
                    names[operation], have_result ? 1u : 0u,
                    (unsigned)outcome,
                    mh_status().operation_busy ? 1u : 0u,
                    probe.deferred_command_valid ? 1u : 0u,
                    (unsigned)probe.request_queue_depth,
                    (unsigned)probe.state);
        }
        check(have_result && outcome == MODEM_SMS_OUTCOME_CANCELLED &&
                  !mh_status().operation_busy &&
                  !probe.deferred_command_valid &&
                  probe.request_queue_depth == 0u &&
                  probe.state == MODEM_SERVICE_TEST_STATE_FAILED,
              "CTS failure cancels the SMS operation and clears all schedulers");

        /* A fresh service epoch must never inherit a terminal latch from the
         * cancelled request. The fixture reset uses the same public init path
         * as firmware startup; no SMS production state is poked. */
        begin_telit(true);
        modem_sms_outcome_t stale_outcome = MODEM_SMS_OUTCOME_NONE;
        check(!pop_sms_transport_result(operation, &stale_outcome),
              "cancelled SMS result cannot leak into the next service epoch");
    }
}

static void test_sms_queue_eviction_is_exact_for_every_operation(void) {
    static const char *const names[] = {
        "text send", "binary send", "save", "mailbox", "read", "delete",
    };

    for (unsigned operation = 0u;
         operation < sizeof(names) / sizeof(names[0]); operation++) {
        if (!begin_sms_operation_fixture(names[operation])) {
            return;
        }
        s_phonebook[0].used = true;
        s_phonebook[0].entry.index = 1u;
        strcpy(s_phonebook[0].entry.name, "Queue hold");
        strcpy(s_phonebook[0].entry.number, "5550001");
        s_fault = TELIT_FAULT_PHONEBOOK_CPBR_TIMEOUT_ONCE;
        s_fault_consumed = false;
        uint32_t phonebook_id = 0u;
        check(modem_service_request_phonebook_list(&phonebook_id),
              "queue-pressure holder is admitted");
        mh_settle();
        check(service_probe().command_active && mh_status().operation_busy,
              "queue-pressure holder remains on wire");

        bool fillers_admitted = true;
        for (uint8_t i = 0u; i < 5u; i++) {
            fillers_admitted = fillers_admitted &&
                modem_service_request_debug_at("AT+CSQ");
        }
        check(fillers_admitted &&
                  request_sms_operation_for_transport_case(operation),
              "SMS target occupies the newest slot of a full FIFO");
        uint32_t request_id = s_last_sms_request_id;
        check(request_id != 0u && service_probe().request_queue_depth == 6u,
              "queue-pressure SMS owns a token before preemption");

        mh_feed("RING");
        mh_feed("+CLIP: \"+15550000000\",145,,,,0");
        check(modem_service_request_answer(),
              "call control preempts the full SMS FIFO");
        modem_sms_outcome_t outcome = MODEM_SMS_OUTCOME_NONE;
        check(!pop_sms_transport_result_for(operation, request_id + 100u,
                                            &outcome) &&
                  pop_sms_transport_result_for(operation, request_id,
                                               &outcome) &&
                  outcome == MODEM_SMS_OUTCOME_EVICTED &&
                  !pop_sms_transport_result_for(operation, request_id,
                                                &outcome),
              "only the exact evicted SMS owner receives one EVICTED terminal");
    }
}

static void test_sms_recovery_cancels_every_uncommitted_operation(void) {
    static const char *const names[] = {
        "text recovery", "binary recovery", "save recovery",
        "mailbox recovery", "read recovery", "delete recovery",
    };

    for (unsigned operation = 0u;
         operation < sizeof(names) / sizeof(names[0]); operation++) {
        if (!begin_sms_operation_fixture(names[operation])) {
            return;
        }
        s_sms_flow_fault = operation == 1u
            ? SMS_FLOW_FAULT_CMGF_PDU_TIMEOUT
            : SMS_FLOW_FAULT_CPMS_TIMEOUT;
        check(request_sms_operation_for_transport_case(operation),
              "SMS recovery target is admitted");
        uint32_t request_id = s_last_sms_request_id;
        mh_settle();
        check(request_id != 0u && service_probe().command_active &&
                  mh_status().operation_busy,
              "SMS recovery target is unresolved before the power fault");

        s_mh_supply_pg = false;
        mh_advance(250u);
        modem_sms_outcome_t outcome = MODEM_SMS_OUTCOME_NONE;
        check(service_probe().state == MODEM_SERVICE_TEST_STATE_FAILED &&
                  pop_sms_transport_result_for(operation, request_id,
                                               &outcome) &&
                  outcome == MODEM_SMS_OUTCOME_CANCELLED &&
                  !pop_sms_transport_result_for(operation, request_id,
                                                &outcome) &&
                  !mh_status().operation_busy,
              "power recovery publishes one retry-safe cancellation per SMS kind");

        begin_telit(true);
        check(!pop_sms_transport_result_for(operation, request_id, &outcome),
              "a cancelled SMS terminal cannot leak into a fresh service epoch");
    }
}

static void test_sms_crossed_urcs_remain_routed(void) {
    modem_sms_send_result_t send_result;
    modem_sms_save_result_t save_result;
    modem_sms_delete_result_t delete_result;

    if (!begin_sms_operation_fixture("crossed send-URC fixture boots")) {
        return;
    }
    s_sms_inject_crossed_urcs = true;
    check(modem_service_request_send_sms("5550103", "Crossed send"),
          "crossed-URC send is admitted");
    mh_settle();
    check(modem_service_pop_sms_send_result(&send_result) &&
              send_result.request_id == s_last_sms_request_id &&
              send_result.kind == MODEM_SMS_REQUEST_SEND_TEXT &&
              send_result.outcome == MODEM_SMS_OUTCOME_OK &&
              mh_status().sms_received_count == 1u &&
              mh_status().ring_active &&
              mh_status().call_state == MODEM_CALL_RINGING,
          "CMTI and RING crossing CMGS remain URCs while send succeeds");

    if (!begin_sms_operation_fixture("crossed save-URC fixture boots")) {
        return;
    }
    s_sms_inject_crossed_urcs = true;
    check(modem_service_request_save_sms("5550103", "Crossed save"),
          "crossed-URC save is admitted");
    mh_settle();
    check(modem_service_pop_sms_save_result(&save_result) &&
              save_result.request_id == s_last_sms_request_id &&
              save_result.kind == MODEM_SMS_REQUEST_SAVE &&
              save_result.outcome == MODEM_SMS_OUTCOME_OK &&
              mh_status().sms_received_count == 1u &&
              mh_status().ring_active,
          "CMTI and RING crossing CMGW remain URCs while save succeeds");

    if (!begin_sms_operation_fixture("crossed delete-URC fixture boots")) {
        return;
    }
    const uint16_t index = 3u;
    s_sms_inject_crossed_urcs = true;
    check(modem_service_request_delete_sms_indices(&index, 1u),
          "crossed-URC delete is admitted");
    mh_settle();
    check(modem_service_pop_sms_delete_result(&delete_result) &&
              delete_result.request_id == s_last_sms_request_id &&
              delete_result.kind == MODEM_SMS_REQUEST_DELETE &&
              delete_result.outcome == MODEM_SMS_OUTCOME_OK &&
              mh_status().sms_received_count == 1u &&
              mh_status().ring_active && mh_tx_count_exact("AT+CMGD=3") == 1u,
          "CMTI and RING crossing CMGD remain URCs while delete succeeds");

    if (!begin_sms_operation_fixture("post-prompt crossed-URC fixture boots")) {
        return;
    }
    static const char body[] = "Post-prompt crossing";
    s_sms_flow_fault = SMS_FLOW_FAULT_CMGS_FINAL_TIMEOUT;
    check(modem_service_request_send_sms("5550103", body),
          "post-prompt crossed-URC send is admitted");
    mh_settle();
    check(tx_event_raw(body, strlen(body), 0u) != SIZE_MAX &&
              service_probe().command_active && mh_status().operation_busy,
          "post-prompt fixture is waiting for the submit final");
    mh_feed("+CMTI: \"ME\",43");
    mh_feed("RING");
    s_sms_flow_fault = SMS_FLOW_FAULT_NONE;
    mh_feed("+CMGS: 24");
    mh_feed("OK");
    check(modem_service_pop_sms_send_result(&send_result) &&
              send_result.request_id == s_last_sms_request_id &&
              send_result.kind == MODEM_SMS_REQUEST_SEND_TEXT &&
              send_result.outcome == MODEM_SMS_OUTCOME_OK &&
              mh_status().sms_received_count == 1u &&
              mh_status().ring_active && !mh_status().operation_busy,
          "URCs crossing the post-prompt final remain routed and do not steal it");
}

static void test_sms_unread_status_is_bracketed(void) {
    begin_telit(true);
    check(boot_until_ready(30000u),
          "SMS unread-policy fixture boots");
    s_sms_test_row_enabled = true;
    s_sms_test_row_unread = true;
    s_sms_keep_unread = false;

    check(modem_service_request_sms_mailbox(MODEM_SMS_MAILBOX_INBOX),
          "metadata scan is admitted");
    mh_settle();
    check(s_sms_test_row_unread && s_sms_keep_unread,
          "mailbox CMGL leaves the modem's unread bit intact");
    check(tx_last_index("AT+CPMS=\"ME\",\"ME\",\"ME\"") <
              tx_last_index("AT#SMSUCS=1") &&
              tx_last_index("AT#SMSUCS=1") <
                  tx_last_index("AT+CMGF=0") &&
              tx_last_index("AT+CMGF=0") < tx_last_index("AT+CMGL=4") &&
              tx_last_index("AT+CMGL=4") < tx_last_index("AT+CMGF=1") &&
              mh_tx_count_exact("AT+CMGR=7") == 0u,
          "metadata scan streams CMGL rows under preserve mode without per-row CMGR");
    modem_sms_mailbox_result_t mailbox_result;
    check(modem_service_pop_sms_mailbox_result(&mailbox_result),
          "metadata scan publishes a bounded result");

    uint16_t index = 7u;
    check(modem_service_request_sms_read(&index, 1u, false, 1u),
          "selected-body read is admitted");
    mh_settle();
    check(!s_sms_test_row_unread && s_sms_keep_unread,
          "selected read consumes exactly that unread bit then restores preserve mode");
    check(tx_last_index("AT#SMSUCS=0") < tx_last_index("AT+CMGF=0") &&
              tx_last_index("AT+CMGF=0") < tx_last_index("AT+CMGR=7") &&
              tx_last_index("AT+CMGR=7") < tx_last_index("AT+CMGF=1") &&
              tx_last_index("AT+CMGF=1") < tx_last_index("AT#SMSUCS=1"),
          "selected-body read brackets the destructive mode on both sides");
    modem_sms_read_result_t read_result;
    check(modem_service_pop_sms_read_result(&read_result),
          "selected-body read publishes a bounded result");

    begin_telit(true);
    check(boot_until_ready(30000u),
          "SMS unread timeout fixture boots");
    s_sms_test_row_enabled = true;
    s_sms_test_row_unread = true;
    s_fault = TELIT_FAULT_SMS_STATUS_CONSUME_TIMEOUT_ONCE;
    check(modem_service_request_sms_read(&index, 1u, false, 1u),
          "timeout fixture selected read is admitted");
    mh_settle();
    mh_advance(g_modem_vendor.sms_read_status.timeout_ms + 1u);
    check(s_sms_keep_unread && s_sms_test_row_unread &&
              mh_tx_count_exact("AT+CMGR=7") == 0u,
          "uncertain consume-mode write restores preserve mode before any CMGR");
    check(modem_service_pop_sms_read_result(&read_result) &&
              read_result.request_id == s_last_sms_request_id &&
              read_result.kind == MODEM_SMS_REQUEST_READ &&
              read_result.outcome == MODEM_SMS_OUTCOME_TIMEOUT,
          "consume-mode timeout reports failure after restoring the invariant");
}

static void test_vvm_control_sms_is_consumed_silently(void) {
    begin_telit(true);
    check(boot_until_ready(30000u),
          "VVM control-SMS fixture boots");
    s_sms_test_row_enabled = true;
    s_sms_test_row_unread = true;
    /* Synthetic equivalent of a live legacy VVM STATUS datagram: TP-DCS=0
     * (GSM7), TP-UDHI=1, destination port 0x1578. The payload is deliberately
     * non-secret test text. */
    s_sms_test_pdu =
        "00440A8151551000000000000000000000001A06050415780000"
        "536A905AFCCDE9617AB9171CD3D3F632";

    mh_feed("+CMTI: \"ME\",7");
    check(mh_status().sms_received_count == 1u &&
              mh_status().sms_user_received_count == 0u,
          "CMTI is pending classification and cannot alert early");
    check(modem_service_request_sms_mailbox(MODEM_SMS_MAILBOX_INBOX),
          "VVM control mailbox scan is admitted");
    mh_settle();
    modem_sms_mailbox_result_t mailbox_result;
    bool have_mailbox_result = modem_service_pop_sms_mailbox_result(&mailbox_result);
    check(have_mailbox_result &&
              mailbox_result.request_id == s_last_sms_request_id &&
              mailbox_result.kind == MODEM_SMS_REQUEST_MAILBOX &&
              mailbox_result.outcome == MODEM_SMS_OUTCOME_OK &&
              mailbox_result.complete &&
              modem_service_sms_mailbox_count() == 0u,
          "recognized VVM control is absent from the public Inbox");
    check(mh_status().sms_received_count == 1u &&
              mh_status().sms_user_received_count == 0u,
          "recognized VVM control never publishes a user arrival");
    check(mh_tx_count_exact("AT+CMGD=7") == 1u &&
              !s_sms_test_row_enabled,
          "recognized VVM control is removed from modem storage");
}

static void test_legacy_url_vvm_is_consumed_silently(void) {
    begin_telit(true);
    check(boot_until_ready(30000u),
          "legacy URL VVM fixture boots");
    s_sms_test_row_enabled = true;
    s_sms_test_row_unread = true;
    /* GSM7 data SMS to the legacy VVM application port. The hostname and
     * mailbox are deliberately synthetic: classification is by protocol
     * envelope, never carrier or originating number. */
    s_sms_test_pdu =
        "00440A81515510000000000000000000000066060504157B0000"
        "767BBB1576BDE1657998FE96BBCAF8701BCE2EEB70B09FB90733D97B3918"
        "CCD4EEC56AB51A2C068399E03DD3B49734CD7B34DACC94EED562B4D9CB1"
        "6A3CD4CF45E4C17ABD56AB0180CA60BEA84BAA18EA89301";

    mh_feed("+CMTI: \"ME\",7");
    check(modem_service_request_sms_mailbox(MODEM_SMS_MAILBOX_INBOX),
          "legacy URL VVM mailbox scan is admitted");
    mh_settle();

    modem_sms_mailbox_result_t mailbox_result;
    check(modem_service_pop_sms_mailbox_result(&mailbox_result) &&
              mailbox_result.request_id == s_last_sms_request_id &&
              mailbox_result.kind == MODEM_SMS_REQUEST_MAILBOX &&
              mailbox_result.outcome == MODEM_SMS_OUTCOME_OK &&
              mailbox_result.complete &&
              modem_service_sms_mailbox_count() == 0u &&
              mh_status().sms_received_count == 1u &&
              mh_status().sms_user_received_count == 0u &&
              mh_tx_count_exact("AT+CMGD=7") == 1u &&
              !s_sms_test_row_enabled,
          "legacy URL VVM is hidden, silent, and deleted by exact index");
}

static void test_unknown_application_sms_fails_open(void) {
    begin_telit(true);
    check(boot_until_ready(30000u),
          "unknown application-SMS fixture boots");
    s_sms_test_row_enabled = true;
    s_sms_test_row_unread = true;
    /* Same valid legacy-looking body, but on an unrecognized application port.
     * Port addressing by itself must never make a user message disappear. */
    s_sms_test_pdu =
        "00440A8151551000000000000000000000001A060504158B0000"
        "536A905AFCCDE9617AB9171CD3D3F632";

    mh_feed("+CMTI: \"ME\",7");
    mh_feed("+CMTI: \"ME\",7");
    check(modem_service_request_sms_mailbox(MODEM_SMS_MAILBOX_INBOX),
          "unknown application mailbox scan is admitted");
    mh_settle();
    modem_sms_mailbox_result_t mailbox_result;
    modem_sms_record_t record;
    bool have_mailbox_result = modem_service_pop_sms_mailbox_result(&mailbox_result);
    bool have_record = modem_service_sms_mailbox_entry(0u, &record);
    check(have_mailbox_result &&
              mailbox_result.request_id == s_last_sms_request_id &&
              mailbox_result.kind == MODEM_SMS_REQUEST_MAILBOX &&
              mailbox_result.outcome == MODEM_SMS_OUTCOME_OK &&
              mailbox_result.complete &&
              modem_service_sms_mailbox_count() == 1u && have_record,
          "unknown application row remains visible");
    check(mh_status().sms_received_count == 2u &&
              mh_status().sms_user_received_count == 1u &&
              mh_tx_count_exact("AT+CMGD=7") == 0u,
          "duplicate indications for one unknown row alert once and do not delete it");
    check(record.identity_hash == sms_identity_hash(
              "1555010000", "00/00/00,00:00:00", "Data message"),
          "port-addressed GSM7 uses the Nokia Data message identity");

    check(modem_service_request_sms_read(record.indices,
                                         record.index_count,
                                         record.quarantined,
                                         record.identity_hash),
          "GSM7 application detail read is admitted");
    mh_settle();
    modem_sms_read_result_t read_result;
    check(modem_service_pop_sms_read_result(&read_result) &&
              read_result.request_id == s_last_sms_request_id &&
              read_result.kind == MODEM_SMS_REQUEST_READ &&
              read_result.outcome == MODEM_SMS_OUTCOME_OK &&
              read_result.message.has_ports &&
              read_result.message.dest_port == 0x158bu &&
              strcmp(read_result.message.text, "STATE?state=Active") == 0,
          "detail read retains application metadata without identity drift");
}

static void test_interleaved_arrival_waits_for_current_snapshot(void) {
    begin_telit(true);
    check(boot_until_ready(30000u),
          "interleaved application-SMS fixture boots");
    s_sms_test_row_enabled = true;
    s_sms_test_row_unread = true;
    s_sms_test_cmti_interleave = true;
    s_sms_test_pdu =
        "00440A8151551000000000000000000000001A060504158B0000"
        "536A905AFCCDE9617AB9171CD3D3F632";

    check(modem_service_request_sms_mailbox(MODEM_SMS_MAILBOX_INBOX),
          "crossed mailbox scan is admitted");
    mh_settle();
    modem_sms_mailbox_result_t mailbox_result;
    check(modem_service_pop_sms_mailbox_result(&mailbox_result) &&
              mailbox_result.request_id == s_last_sms_request_id &&
              mailbox_result.kind == MODEM_SMS_REQUEST_MAILBOX &&
              mailbox_result.outcome == MODEM_SMS_OUTCOME_OK &&
              mailbox_result.complete &&
              mh_status().sms_received_count == 1u &&
              mh_status().sms_user_received_count == 0u,
          "arrival crossing CMGL cannot publish from the stale snapshot");

    s_sms_test_cmti_interleave = false;
    check(modem_service_request_sms_mailbox(MODEM_SMS_MAILBOX_INBOX),
          "current mailbox retry is admitted");
    mh_settle();
    check(modem_service_pop_sms_mailbox_result(&mailbox_result) &&
              mailbox_result.request_id == s_last_sms_request_id &&
              mailbox_result.outcome == MODEM_SMS_OUTCOME_OK &&
              mailbox_result.complete &&
              mh_status().sms_user_received_count == 1u,
          "next clean snapshot publishes the interleaved user arrival once");
}

static void test_vvm_cleanup_error_retries_without_alerting(void) {
    begin_telit(true);
    check(boot_until_ready(30000u),
          "VVM cleanup-retry fixture boots");
    s_sms_test_row_enabled = true;
    s_sms_test_row_unread = true;
    s_sms_test_pdu =
        "00440A8151551000000000000000000000001A06050415780000"
        "536A905AFCCDE9617AB9171CD3D3F632";
    s_fault = TELIT_FAULT_VVM_DELETE_ERROR_ONCE;
    mh_feed("+CMTI: \"ME\",7");

    check(modem_service_request_sms_mailbox(MODEM_SMS_MAILBOX_INBOX),
          "VVM cleanup-error scan is admitted");
    mh_settle();
    modem_sms_mailbox_result_t mailbox_result;
    check(modem_service_pop_sms_mailbox_result(&mailbox_result) &&
              mailbox_result.request_id == s_last_sms_request_id &&
              mailbox_result.kind == MODEM_SMS_REQUEST_MAILBOX &&
              mailbox_result.outcome == MODEM_SMS_OUTCOME_OK &&
              mailbox_result.complete &&
              modem_service_sms_mailbox_count() == 0u &&
              s_sms_test_row_enabled &&
              mh_status().sms_user_received_count == 0u,
          "definitive cleanup error keeps VVM hidden and silent");

    check(modem_service_request_sms_mailbox(MODEM_SMS_MAILBOX_INBOX),
          "VVM cleanup retry scan is admitted");
    mh_settle();
    check(modem_service_pop_sms_mailbox_result(&mailbox_result) &&
              mailbox_result.request_id == s_last_sms_request_id &&
              mailbox_result.outcome == MODEM_SMS_OUTCOME_OK &&
              mailbox_result.complete &&
              !s_sms_test_row_enabled &&
              mh_tx_count_exact("AT+CMGD=7") == 2u &&
              mh_status().sms_user_received_count == 0u,
          "later scan retries cleanup without inventing an arrival");
}

static void test_vvm_cleanup_timeout_is_bounded_and_silent(void) {
    begin_telit(true);
    check(boot_until_ready(30000u),
          "VVM cleanup-timeout fixture boots");
    s_sms_test_row_enabled = true;
    s_sms_test_row_unread = true;
    s_sms_test_pdu =
        "00440A8151551000000000000000000000001A06050415780000"
        "536A905AFCCDE9617AB9171CD3D3F632";
    s_fault = TELIT_FAULT_VVM_DELETE_TIMEOUT_ONCE;
    mh_feed("+CMTI: \"ME\",7");

    check(modem_service_request_sms_mailbox(MODEM_SMS_MAILBOX_INBOX),
          "VVM cleanup-timeout scan is admitted");
    mh_settle();
    modem_sms_mailbox_result_t mailbox_result;
    modem_service_test_snapshot_t probe = service_probe();
    check(!modem_service_pop_sms_mailbox_result(&mailbox_result) &&
              probe.command_active &&
              probe.active_command == MODEM_SERVICE_TEST_COMMAND_SMS_DELETE,
          "mailbox waits while the cleanup command remains unresolved");
    mh_advance(5001u);
    check(modem_service_pop_sms_mailbox_result(&mailbox_result) &&
              mailbox_result.request_id == s_last_sms_request_id &&
              mailbox_result.kind == MODEM_SMS_REQUEST_MAILBOX &&
              mailbox_result.outcome == MODEM_SMS_OUTCOME_OK &&
              mailbox_result.complete &&
              modem_service_sms_mailbox_count() == 0u &&
              s_sms_test_row_enabled &&
              mh_status().sms_user_received_count == 0u,
          "cleanup timeout is bounded without exposing or alerting on VVM");

    check(modem_service_request_sms_mailbox(MODEM_SMS_MAILBOX_INBOX),
          "post-timeout VVM cleanup retry is admitted");
    mh_settle();
    check(modem_service_pop_sms_mailbox_result(&mailbox_result) &&
              mailbox_result.request_id == s_last_sms_request_id &&
              mailbox_result.outcome == MODEM_SMS_OUTCOME_OK &&
              !s_sms_test_row_enabled &&
              mh_tx_count_exact("AT+CMGD=7") == 2u &&
              mh_status().sms_user_received_count == 0u,
          "later clean scan retries an uncertain delete safely");
}

static void test_unclassifiable_cmti_fails_open(void) {
    begin_telit(true);
    check(boot_until_ready(30000u),
          "unclassifiable-CMTI fixture boots");
    mh_feed("+CMTI: \"SM\",7");
    check(mh_status().sms_received_count == 1u &&
              mh_status().sms_user_received_count == 1u,
          "foreign-storage CMTI fails open instead of suppressing a user SMS");
}

static void test_sms_ri_wakes_sleeping_transport(void) {
    begin_telit(true);
    check(boot_until_ready(30000u), "sleeping-SMS wake fixture boots");
    settle_dtr_sleep();
    check(s_mh_dtr_sleep_permitted && service_probe().sms_wake_armed &&
              s_psmri_live_armed,
          "fixture sleeps only after the pre-CFUN PSMRI lifecycle qualifies");

    size_t profile_writes_before = mh_tx_count_exact(
        "AT#WKIO=0;#E2SMSRI=0;\\R2&W0");
    size_t runtime_psmri_before = mh_tx_count_exact("AT#PSMRI=1000");
    check(modem_service_request_debug_at("AT+CPMS?"),
          "query-only CPMS probe is admitted");
    mh_settle();
    check(mh_tx_count_exact(
                  "AT#WKIO=0;#E2SMSRI=0;\\R2&W0") ==
                  profile_writes_before &&
              mh_tx_count_exact("AT#PSMRI=1000") == runtime_psmri_before &&
              s_psmri_live_armed && s_e2smsri_ms == 0u &&
              s_psmri_ms == 1000u,
          "query-only CPMS leaves the qualified SMS wake lifecycle intact");

    check(modem_service_request_sms_mailbox(MODEM_SMS_MAILBOX_INBOX),
          "mailbox request is admitted before sleeping-SMS wake probe");
    mh_settle();
    size_t cpms = tx_last_index("AT+CPMS=\"ME\",\"ME\",\"ME\"");
    size_t cmgf = tx_last_index("AT+CMGF=0");
    check(mh_tx_count_exact(
                  "AT#WKIO=0;#E2SMSRI=0;\\R2&W0") ==
                  profile_writes_before &&
              mh_tx_count_exact("AT#PSMRI=1000") == runtime_psmri_before &&
              cpms < cmgf && s_psmri_live_armed &&
              s_e2smsri_ms == 0u && s_psmri_ms == 1000u,
          "CPMS-backed mailbox work emits no post-CFUN wake command");

    settle_dtr_sleep();
    check(s_mh_dtr_sleep_permitted && !s_mh_cts_asserted,
          "re-armed SMS path reaches DTR sleep");

    modem_diag_snapshot_t before;
    modem_diag_snapshot_t after;
    modem_service_get_diag_snapshot(&before);
    uint32_t sms_before = mh_status().sms_received_count;
    /* Board-1 WWX releases the buffered +CMTI only after #PSMRI returns high.
     * Latch the edge, but keep DTR asleep until that release is observed. */
    s_mh_ri_wake_pending = true;
    s_mh_ri_asserted = true;
    modem_service_tick(s_mh_now);
    modem_service_get_diag_snapshot(&after);
    check(s_mh_dtr_sleep_permitted && !s_mh_cts_asserted &&
              after.transport.ri_release_pending &&
              after.transport.wake_attempts == before.transport.wake_attempts,
          "active RI pulse is latched without asserting DTR early");
    mh_advance(g_modem_vendor.wake.awake_window_ms + 1u);
    modem_service_get_diag_snapshot(&after);
    check(s_mh_dtr_sleep_permitted && !s_mh_cts_asserted &&
              after.transport.ri_release_pending,
          "ordinary awake-window expiry cannot bypass the RI release gate");
    s_mh_ri_asserted = false;
    modem_service_tick(s_mh_now);
    mh_feed("+CMTI: \"ME\",8");

    modem_service_get_diag_snapshot(&after);
    check(!s_mh_dtr_sleep_permitted && s_mh_cts_asserted &&
              mh_status().sms_received_count == sms_before + 1u,
          "RI release starts DTR wake and consumes the buffered SMS URC");
    check(after.transport.wake_attempts ==
                  before.transport.wake_attempts + 1u &&
              after.transport.wake_timeouts == before.transport.wake_timeouts &&
              after.transport.ri_release_timeouts ==
                  before.transport.ri_release_timeouts,
          "SMS RI uses the qualified DTR wake path without a timeout");

    settle_dtr_sleep();
    modem_service_get_diag_snapshot(&before);
    sms_before = mh_status().sms_received_count;
    /* A pulse that completed before the main loop ran still has to wake from
     * the ISR latch even though the physical level is high again. */
    s_mh_ri_wake_pending = true;
    modem_service_tick(s_mh_now);
    mh_feed("+CMTI: \"ME\",9");
    modem_service_get_diag_snapshot(&after);
    check(!s_mh_dtr_sleep_permitted && s_mh_cts_asserted &&
              mh_status().sms_received_count == sms_before + 1u &&
              after.transport.wake_attempts ==
                  before.transport.wake_attempts + 1u &&
              after.transport.wake_timeouts == before.transport.wake_timeouts,
          "a completed RI pulse remains recoverable from the edge latch");

    settle_dtr_sleep();
    modem_service_get_diag_snapshot(&before);
    s_mh_ri_wake_pending = true;
    s_mh_ri_asserted = true;
    modem_service_tick(s_mh_now);
    mh_advance(g_modem_vendor.wake.ri_release_timeout_ms + 1u);
    modem_service_get_diag_snapshot(&after);
    check(!s_mh_dtr_sleep_permitted && s_mh_cts_asserted &&
              !after.transport.ri_release_pending &&
              after.transport.wake_attempts ==
                  before.transport.wake_attempts + 1u &&
              after.transport.ri_release_timeouts ==
                  before.transport.ri_release_timeouts + 1u,
          "stuck-low RI forces one bounded wake instead of blocking forever");
    s_mh_ri_asserted = false;
}

static void test_sms_wake_profile_failure_fails_closed(void) {
    begin_telit(true);
    s_e2smsri_ms = 1000u;
    s_fault = TELIT_FAULT_SMS_WAKE_PROFILE_SET_ERROR;
    check(!boot_until_ready(50000u) &&
              service_probe().state == MODEM_SERVICE_TEST_STATE_FAILED,
          "rejected pre-CFUN wake profile cannot reach qualified READY");
    check(mh_tx_count_exact(
                  "AT#WKIO=0;#E2SMSRI=0;\\R2&W0") == 3u &&
              mh_tx_count_exact("AT+CFUN=5") == 1u &&
              !service_probe().sms_wake_armed && !s_psmri_live_armed &&
              !s_mh_dtr_sleep_permitted,
          "bounded profile failure stops before the qualifying CFUN=5");
}

static void test_missing_sim_uses_bounded_activation_window(void) {
    begin_telit(true);
    s_mh_sim_present = false;
    s_mh_sim_ready = false;
    check(boot_until_ready(30000u), "missing-SIM activation fixture boots");
    uint32_t deadline = service_probe().sms_wake_activation_deadline_ms;
    check(deadline != 0u && !service_probe().sms_wake_armed &&
              mh_tx_count_exact("AT+CFUN=1") == 0u,
          "missing SIM starts one bounded wait without a false completion");

    while (time_diff_ms(deadline, s_mh_now) > 100) {
        mh_advance(100u);
        check(!s_mh_dtr_sleep_permitted,
              "DTR stays awake throughout the SIM activation window");
    }
    if (time_diff_ms(deadline, s_mh_now) > 0) {
        mh_advance((uint32_t)time_diff_ms(deadline, s_mh_now));
    }
    for (uint32_t elapsed = 0u;
         elapsed < g_modem_vendor.wake.awake_window_ms + 3000u;
         elapsed += 100u) {
        mh_advance(100u);
        if (s_mh_dtr_sleep_permitted) {
            break;
        }
    }
    check(service_probe().sms_wake_activation_deadline_ms == 0u &&
              s_mh_dtr_sleep_permitted && !s_mh_cts_asserted,
          "missing SIM is released to DTR sleep after the bounded window");
}

static void test_hot_sim_never_spends_a_second_automatic_reboot(void) {
    begin_telit(false);
    s_mh_sim_present = false;
    s_mh_sim_ready = false;
    s_ecamurc_configured = false;
    check(boot_until_ready(60000u),
          "absent-SIM fixture completes its non-SIM provisioning reboot");
    check(mh_tx_count_exact("AT#REBOOT") == 1u &&
              !mh_status().provisioning_verified,
          "initial RXDIV correction consumes the one automatic reboot budget");

    s_mh_sim_present = true;
    s_mh_sim_ready = true;
    mh_rx_push("#QSS: 3");
    bool deferred = false;
    for (uint32_t elapsed = 0u; elapsed < 25000u; elapsed += 100u) {
        mh_advance(100u);
        modem_service_test_snapshot_t probe = service_probe();
        if (probe.state == MODEM_SERVICE_TEST_STATE_READY &&
            !probe.sim_completion_active &&
            mh_tx_count_exact("AT#ECAMURC=1") == 1u) {
            deferred = true;
            break;
        }
    }
    check(deferred && mh_tx_count_exact("AT#REBOOT") == 1u &&
              service_probe().sim_completion_needed &&
              !mh_status().provisioning_verified,
          "hot-SIM NVM change is verified but its second reboot is deferred");

    graceful_power_off();
    check(boot_until_ready(30000u),
          "next ordinary module power cycle applies deferred SIM provisioning");
    check(mh_tx_count_exact("AT#REBOOT") == 1u &&
              mh_status().provisioning_verified,
          "deferred setting verifies without another automatic provisioning reboot");
}

static void test_pg_and_cts_faults_fail_closed(void) {
    begin_telit(true);
    modem_service_power_on();
    mh_advance(service_probe().rail_off_dwell_ms);
    s_mh_supply_pg = false;
    mh_advance(g_modem_vendor.power.rail_power_good_timeout_ms + 1u);
    check(service_probe().state == MODEM_SERVICE_TEST_STATE_FAILED &&
              !s_mh_rail_enabled &&
              !s_mh_power_pin_asserted,
          "missing supply PG is bounded before ON_OFF and releases a dead rail");
    check(modem_service_take_supply_power_failure(),
          "rail-start PG timeout publishes one physical supply failure");
    check(!modem_service_take_supply_power_failure(),
          "supply failure evidence is an acknowledged one-shot");

    begin_telit(true);
    modem_service_power_on();
    mh_advance(service_probe().rail_off_dwell_ms);
    s_mh_supply_pg = false;
    mh_advance(g_modem_vendor.power.rail_power_good_timeout_ms + 1u);
    check(service_probe().state == MODEM_SERVICE_TEST_STATE_FAILED,
          "stale-event fixture reaches the same pre-pulse PG failure");
    s_mh_supply_pg = true;
    check(boot_until_ready(30000u),
          "a new healthy rail epoch recovers after the old PG fault");
    check(!modem_service_take_supply_power_failure(),
          "a fresh power epoch discards unconsumed old PG evidence");

    begin_telit(true);
    modem_service_power_on();
    mh_advance(service_probe().rail_off_dwell_ms);
    mh_advance(g_modem_vendor.power.rail_settle_ms);
    check(service_probe().state == MODEM_SERVICE_TEST_STATE_POWER_PULSE,
          "startup-drop fixture reaches the ON_OFF pulse");
    s_mh_supply_pg = false;
    mh_advance(g_modem_vendor.power.pwron_pulse_ms);
    check(service_probe().state == MODEM_SERVICE_TEST_STATE_FAILED &&
              modem_service_take_supply_power_failure(),
          "PG loss during module startup publishes the same neutral event");
    size_t shutdown_commands = mh_tx_count_exact("AT#SHDN");
    modem_service_power_off();
    check(service_probe().state == MODEM_SERVICE_TEST_STATE_OFF_DISCHARGE &&
              s_mh_graceful_shutdown_pulse_count == 1u &&
              s_mh_power_pin_asserted &&
              mh_tx_count_exact("AT#SHDN") == shutdown_commands,
          "startup PG collapse bypasses parked UART and starts hardware shutdown");
    mh_advance(g_modem_vendor.power.graceful_off_pulse_ms + 2u);
    mh_advance(1u); /* first qualified-low PWRMON observation after pulse release */
    mh_advance(500u);
    check(modem_service_is_powered_off() && !s_mh_rail_enabled,
          "qualified PWRMON low after supply-fault shutdown releases 3V8");

    begin_telit(true);
    check(boot_until_ready(30000u), "runtime-PG fixture boots first");
    s_mh_supply_pg = false;
    mh_advance(250u);
    modem_service_test_snapshot_t probe = service_probe();
    check(probe.state == MODEM_SERVICE_TEST_STATE_FAILED &&
              s_mh_rail_enabled && probe.failed_module_may_be_live,
          "runtime PG loss retains a previously proven-live module rail");
    check(modem_service_take_supply_power_failure(),
          "runtime PG loss remains visible to battery safety policy");

    begin_telit(true);
    check(boot_until_ready(30000u), "runtime-PWRMON fixture boots first");
    s_mh_status_raw = 36u;
    s_mh_status_mv = 29u;
    mh_advance(250u);
    probe = service_probe();
    check(probe.state == MODEM_SERVICE_TEST_STATE_FAILED &&
              probe.failed_module_may_be_live,
          "unexpected runtime PWRMON loss remains a modem power fault");
    check(!modem_service_take_supply_power_failure(),
          "PWRMON loss cannot masquerade as supply-PG collapse");

    begin_telit(true);
    check(boot_until_ready(30000u), "CTS-fault fixture boots first");
    settle_dtr_sleep();
    s_mh_dtr_wake_works = false;
    check(modem_service_request_dial("15551234567"),
          "CTS-fault call command is admitted");
    mh_settle();
    mh_advance(g_modem_vendor.wake.dtr_wake_timeout_ms + 1u);
    probe = service_probe();
    check(probe.state == MODEM_SERVICE_TEST_STATE_FAILED &&
              s_mh_rail_enabled && probe.failed_module_may_be_live,
          "stuck CTS is bounded and retains a possibly live module rail");
    check(!modem_service_take_supply_power_failure(),
          "a CTS transport fault cannot masquerade as supply collapse");
    check(probe.request_queue_depth == 0u &&
              !probe.deferred_command_valid &&
              active_call_transactions() == 0u,
          "transport failure clears queued work and the prior call epoch");

    s_mh_dtr_wake_works = true;
    modem_service_power_on();
    check(boot_until_ready(30000u),
          "retained live module recovers through PWRMON/CTS and fresh init");
    mh_advance(2000u);
    check(mh_tx_count_exact("ATD15551234567;") == 0u,
          "recovery never dispatches a stale pre-failure dial");
}

static void test_expected_shutdown_status_drop_is_not_a_fault(void) {
    begin_telit(true);
    modem_service_test_snapshot_t probe;
    check(boot_until_ready(30000u), "shutdown-race fixture boots");
    s_fault = TELIT_FAULT_SHUTDOWN_TIMEOUT;
    modem_service_power_off();
    mh_advance(1u); /* dispatch #SHDN; fixture drops PWRMON but loses its final */
    mh_advance(250u);
    check(service_probe().state != MODEM_SERVICE_TEST_STATE_FAILED &&
              s_mh_rail_enabled,
          "expected PWRMON fall cannot preempt an in-flight shutdown command");
    mh_advance(g_modem_vendor.power.off_ack_timeout_ms + 1u);
    probe = service_probe();
    check(probe.state == MODEM_SERVICE_TEST_STATE_OFF_DISCHARGE &&
              s_mh_rail_enabled && s_mh_uart_park_count >= 2u &&
              probe.uart_parked,
          "lost shutdown final still removes PL011 before low qualification");
    mh_advance(500u);
    check(modem_service_is_powered_off() && !s_mh_rail_enabled,
          "qualified low PWRMON releases the rail after a lost shutdown final");
}

static void test_stuck_software_shutdown_uses_graceful_hardware_fallback(void) {
    begin_telit(true);
    check(boot_until_ready(30000u), "graceful fallback fixture boots");
    s_fault = TELIT_FAULT_SHUTDOWN_STUCK_ON;
    modem_service_power_off();
    mh_advance(1u); /* dispatch #SHDN and receive OK while PWRMON stays high */
    mh_advance(1u); /* consume final and enter OFF_DISCHARGE */
    check(service_probe().state == MODEM_SERVICE_TEST_STATE_OFF_DISCHARGE &&
              s_mh_rail_enabled,
          "graceful shutdown holds VCC while PWRMON remains high");

    mh_advance(g_modem_vendor.power.off_sense_timeout_ms + 1u);
    modem_diag_snapshot_t diag;
    modem_service_get_diag_snapshot(&diag);
    check(service_probe().state == MODEM_SERVICE_TEST_STATE_OFF_DISCHARGE &&
              s_mh_rail_enabled && s_mh_power_pin_asserted &&
              s_mh_graceful_shutdown_pulse_count == 1u &&
              s_mh_last_shutdown_pulse_width_ms ==
                  g_modem_vendor.power.graceful_off_pulse_ms &&
              diag.runtime.shutdown_stage == 2u &&
              diag.runtime.graceful_shutdown_pulses == 1u &&
              diag.runtime.emergency_shutdown_pulses == 0u,
          "stuck #SHDN starts exactly one bounded graceful hardware-off pulse");

    mh_advance(g_modem_vendor.power.graceful_off_pulse_ms - 1u);
    check(s_mh_power_pin_asserted && s_mh_rail_enabled,
          "graceful hardware-off remains asserted for its complete minimum width");
    mh_advance(1u);
    check(!s_mh_power_pin_asserted && s_mh_rail_enabled,
          "hardware alarm boundary deasserts graceful control before qualification");
    mh_advance(1u);   /* conservative service sequencing gate */
    mh_advance(1u);   /* first vendor-qualified low observation */
    mh_advance(500u); /* complete Telit's sustained-low qualification */
    check(modem_service_is_powered_off() && !s_mh_rail_enabled,
          "qualified PWRMON low after graceful fallback releases the rail");
}

static void test_failed_graceful_fallback_uses_unconditional_shutdown(void) {
    begin_telit(true);
    check(boot_until_ready(30000u), "unconditional fallback fixture boots");
    s_fault = TELIT_FAULT_SHUTDOWN_STUCK_ON;
    s_mh_graceful_shutdown_works = false;
    modem_service_power_off();
    mh_advance(2u); /* dispatch, final, OFF_DISCHARGE */

    mh_advance(g_modem_vendor.power.off_sense_timeout_ms + 1u);
    check(s_mh_graceful_shutdown_pulse_count == 1u &&
              s_mh_power_pin_asserted,
          "software timeout first retries through graceful hardware control");
    mh_advance(g_modem_vendor.power.graceful_off_pulse_ms + 1u);
    mh_advance(g_modem_vendor.power.graceful_off_sense_timeout_ms + 1u);
    modem_diag_snapshot_t diag;
    modem_service_get_diag_snapshot(&diag);
    check(s_mh_emergency_shutdown_pulse_count == 1u &&
              s_mh_emergency_pin_asserted && s_mh_rail_enabled &&
              s_mh_last_shutdown_pulse_width_ms ==
                  g_modem_vendor.power.emergency_off_pulse_ms &&
              diag.runtime.shutdown_stage == 3u &&
              diag.runtime.graceful_shutdown_pulses == 1u &&
              diag.runtime.emergency_shutdown_pulses == 1u,
          "failed graceful retry starts one bounded unconditional shutdown pulse");

    mh_advance(g_modem_vendor.power.emergency_off_pulse_ms);
    check(!s_mh_emergency_pin_asserted && s_mh_rail_enabled,
          "unconditional control is released before PWRMON qualification");
    mh_advance(1u); /* conservative service sequencing gate */
    mh_advance(1u); /* first vendor-qualified low observation */
    mh_advance(500u);
    check(modem_service_is_powered_off() && !s_mh_rail_enabled,
          "unconditional shutdown still requires qualified-low PWRMON before rail release");
}

static void test_exhausted_shutdown_ladder_is_visible_and_fail_closed(void) {
    begin_telit(true);
    check(boot_until_ready(30000u), "terminal shutdown fixture boots");
    s_fault = TELIT_FAULT_SHUTDOWN_STUCK_ON;
    s_mh_graceful_shutdown_works = false;
    s_mh_emergency_shutdown_works = false;
    modem_service_power_off();
    mh_advance(2u);
    mh_advance(g_modem_vendor.power.off_sense_timeout_ms + 1u);
    mh_advance(g_modem_vendor.power.graceful_off_pulse_ms + 1u);
    mh_advance(g_modem_vendor.power.graceful_off_sense_timeout_ms + 1u);
    mh_advance(g_modem_vendor.power.emergency_off_pulse_ms + 1u);
    mh_advance(g_modem_vendor.power.emergency_off_sense_timeout_ms + 1u);

    modem_service_test_snapshot_t probe = service_probe();
    modem_diag_snapshot_t diag;
    modem_service_get_diag_snapshot(&diag);
    check(probe.state == MODEM_SERVICE_TEST_STATE_FAILED &&
              probe.failed_module_may_be_live && s_mh_rail_enabled &&
              !s_mh_power_pin_asserted && !s_mh_emergency_pin_asserted,
          "exhausted ladder reaches a bounded terminal state with live rail retained");
    check(diag.runtime.shutdown_stage == 4u &&
              diag.runtime.shutdown_terminal_fault &&
              diag.runtime.graceful_shutdown_pulses == 1u &&
              diag.runtime.emergency_shutdown_pulses == 1u &&
              diag.runtime.terminal_shutdown_failures == 1u,
          "terminal shutdown state and every attempted stage are published");
    check(modem_service_take_power_off_failure() &&
              !modem_service_take_power_off_failure(),
          "terminal failure produces exactly one app-visible indication");
    mh_advance(30000u);
    check(service_probe().state == MODEM_SERVICE_TEST_STATE_FAILED &&
              s_mh_rail_enabled,
          "terminal state neither loops pulses nor cuts a high-PWRMON rail");

    s_mh_status_raw = 36u;
    s_mh_status_mv = 29u;
    s_mh_rx_idle_high = false;
    s_mh_cts_asserted = false;
    mh_advance(250u); /* reach the next failed-state observation slot */
    mh_advance(500u);
    check(modem_service_is_powered_off() && !s_mh_rail_enabled,
          "late qualified PWRMON low can still complete a terminal shutdown");
    check(!modem_service_take_power_off_failure(),
          "late successful completion retires any unconsumed failure indication");
}

static void test_shutdown_pulse_arm_failure_is_bounded(void) {
    begin_telit(true);
    check(boot_until_ready(30000u), "pulse-arm failure fixture boots");
    s_fault = TELIT_FAULT_SHUTDOWN_STUCK_ON;
    s_mh_shutdown_pulse_arm_works = false;
    modem_service_power_off();
    mh_advance(2u);
    mh_advance(g_modem_vendor.power.off_sense_timeout_ms + 1u);
    check(service_probe().state == MODEM_SERVICE_TEST_STATE_FAILED &&
              s_mh_rail_enabled &&
              s_mh_graceful_shutdown_pulse_count == 0u &&
              s_mh_emergency_shutdown_pulse_count == 0u &&
              modem_service_take_power_off_failure(),
          "local pulse-arm failure stops safely without escalating blindly");
}

static void test_power_on_intent_survives_exhausted_shutdown_ladder(void) {
    begin_telit(true);
    check(boot_until_ready(30000u), "off-on terminal fixture boots");
    s_fault = TELIT_FAULT_SHUTDOWN_STUCK_ON;
    s_mh_graceful_shutdown_works = false;
    s_mh_emergency_shutdown_works = false;
    modem_service_power_off();
    mh_advance(2u);
    modem_service_power_on();
    check(service_probe().power_on_pending,
          "newer power-on intent queues behind irreversible shutdown");

    mh_advance(g_modem_vendor.power.off_sense_timeout_ms + 1u);
    mh_advance(g_modem_vendor.power.graceful_off_pulse_ms + 1u);
    mh_advance(g_modem_vendor.power.graceful_off_sense_timeout_ms + 1u);
    mh_advance(g_modem_vendor.power.emergency_off_pulse_ms + 1u);
    mh_advance(g_modem_vendor.power.emergency_off_sense_timeout_ms + 1u);
    check(boot_until_ready(30000u) && s_mh_rail_enabled &&
              !service_probe().power_on_pending,
          "exhausted OFF ladder recovers the retained module for the newer ON intent");
    check(!modem_service_take_power_off_failure(),
          "successful latest-intent recovery suppresses a stale power-off warning");
}

static void test_unexpected_runtime_status_drop_retains_rail(void) {
    begin_telit(true);
    modem_service_test_snapshot_t probe;
    check(boot_until_ready(30000u), "runtime-status-drop fixture boots");
    s_mh_status_raw = 36u;
    s_mh_status_mv = 29u;
    s_mh_cts_asserted = false;
    mh_advance(250u);
    probe = service_probe();
    check(probe.state == MODEM_SERVICE_TEST_STATE_FAILED &&
              s_mh_rail_enabled && probe.failed_module_may_be_live,
          "one unexpected low PWRMON sample faults but cannot cut a live rail");

    mh_advance(1000u);
    check(service_probe().state == MODEM_SERVICE_TEST_STATE_FAILED &&
              s_mh_rail_enabled,
          "uncommanded low PWRMON never inherits shutdown authorization");

    modem_service_power_off();
    mh_advance(g_modem_vendor.power.off_ack_timeout_ms + 1u);
    mh_advance(500u);
    check(modem_service_is_powered_off() && !s_mh_rail_enabled,
          "explicit shutdown obtains sustained-low proof before releasing rail");
}

static void test_provision_fault_outcomes(void) {
    begin_telit(true);
    s_fault = TELIT_FAULT_RXDIV_READBACK_MALFORMED;
    check(!boot_until_ready(45000u) &&
              service_probe().state == MODEM_SERVICE_TEST_STATE_FAILED,
          "malformed persistent readback exhausts a bounded retry budget");
    check(s_mh_rail_enabled && service_probe().failed_module_may_be_live,
          "readback failure retains the rail while PWRMON says module live");
    check(mh_tx_count_exact("AT#RXDIV?") == 3u,
          "malformed readback performs only the initial query plus two retries");

    begin_telit(false);
    s_fault = TELIT_FAULT_RXDIV_SET_ERROR;
    check(!boot_until_ready(45000u) &&
              service_probe().state == MODEM_SERVICE_TEST_STATE_FAILED,
          "rejected nonrecoverable persistent write fails closed");
    check(mh_tx_count_exact("AT#RXDIV=0,1") == 3u,
          "rejected write performs only the descriptor's bounded attempts");

    begin_telit(false);
    s_fault = TELIT_FAULT_RXDIV_VERIFY_MISMATCH;
    check(!boot_until_ready(45000u) &&
              service_probe().state == MODEM_SERVICE_TEST_STATE_FAILED,
          "acknowledged write with stale readback exhausts a bounded retry budget");
    check(mh_tx_count_exact("AT#RXDIV=0,1") == 3u &&
              mh_tx_count_exact("AT#RXDIV?") == 4u,
          "write/verify mismatch cannot reset its retry budget indefinitely");

    begin_telit(false);
    s_fault = TELIT_FAULT_RXDIV_SET_TIMEOUT_ONCE;
    check(boot_until_ready(70000u),
          "uncertain persistent write converges through readback verification");
    check(mh_tx_count_exact("AT#RXDIV=0,1") == 1u &&
              mh_tx_count_exact("AT#REBOOT") == 1u &&
              mh_status().provisioning_verified,
          "lost set final is never rewritten after matching verification");

    begin_telit(false);
    s_fault = TELIT_FAULT_REBOOT_TIMEOUT_ONCE;
    check(boot_until_ready(80000u),
          "lost reboot final recovers through module-status probing and re-init");
    check(mh_tx_count_exact("AT#REBOOT") == 1u &&
              mh_status().provisioning_verified,
          "uncertain reboot consumes the one-reboot budget and still verifies");

    begin_telit(true);
    s_cpms_configured = false;
    s_fault = TELIT_FAULT_CPMS_SET_ERROR;
    check(boot_until_ready(60000u),
          "recoverable SMS-storage provisioning failure reaches degraded READY");
    modem_status_t status = mh_status();
    check(!status.sms_init_ok && !status.provisioning_verified,
          "skipped SMS row is visible and never recorded as fully verified");
    check(mh_tx_count_exact("AT+CPMS=\"ME\",\"ME\",\"ME\"") == 3u &&
              mh_tx_count_exact("AT#REBOOT") == 0u,
          "failed recoverable write is bounded and cannot trigger a false reboot");

    begin_telit(true);
    s_e2smsri_ms = 1000u;
    s_fault = TELIT_FAULT_SMS_WAKE_PROFILE_SET_TIMEOUT_ONCE;
    check(boot_until_ready(50000u) && mh_status().provisioning_verified,
          "lost wake-profile final converges through strict readback");
    check(mh_tx_count_exact(
                  "AT#WKIO=0;#E2SMSRI=0;\\R2&W0") == 1u &&
              s_fault_consumed && service_probe().sms_wake_armed &&
              s_psmri_live_armed,
          "uncertain profile write is verified without duplicate NVM write");

    begin_telit(true);
    s_fault = TELIT_FAULT_FINAL_CFUN_SET_TIMEOUT_ONCE;
    check(boot_until_ready(50000u) && mh_status().provisioning_verified,
          "lost final-CFUN response converges through functional-level readback");
    check(s_fault_consumed && mh_tx_count_exact("AT+CFUN=5") == 2u &&
              mh_tx_count_exact("AT#PSMRI=1000") == 1u &&
              service_probe().sms_wake_armed && s_psmri_live_armed,
          "verified final CFUN qualifies wake without replaying the transition");
}

static void test_quiet_registration_indicator_profile(void) {
    begin_telit(true);
    check(boot_until_ready(40000u),
          "quiet registration/indicator profile reaches READY");
    check(mh_tx_count_exact("AT+CEREG=1") > 0u &&
              mh_tx_count_exact("AT+CMER=2,0,0,0,0") > 0u,
          "boot applies registration-only URCs and disables generic indicators");
    check(mh_tx_count_exact("AT+CEREG=2") == 0u &&
              mh_tx_count_exact("AT+CMER=2,0,0,2,0") == 0u &&
              mh_tx_count_exact("AT+CIND=?") == 0u,
          "boot never restores the noisy indicator/cell-change profile");
}

static void test_diag_scheduler_atomicity_and_preemption(void) {
    begin_telit(true);
    check(boot_until_ready(30000u), "diagnostic scheduler fixture boots");
    settle_dtr_sleep();
    size_t rfsts_before = mh_tx_count_exact("AT#RFSTS");

    check(modem_service_diag_select(MODEM_DIAG_GROUP_SERVING, 1u, true),
          "serving group is admitted");
    mh_advance(1u); /* request DTR wake */
    mh_advance(1u); /* observe CTS and dispatch the deferred command */
    modem_diag_snapshot_t snapshot;
    modem_service_get_diag_snapshot(&snapshot);
    check(snapshot.group[MODEM_DIAG_GROUP_SERVING].state ==
              MODEM_DIAG_STATE_FRESH &&
              snapshot.group[MODEM_DIAG_GROUP_SERVING].sequence == 1u &&
              snapshot.group[MODEM_DIAG_GROUP_SERVING].consecutive_failures == 0u &&
              snapshot.serving.rat == MODEM_DIAG_RAT_LTE &&
              snapshot.serving.band == 12u && snapshot.serving.pci == 321u,
          "successful two-command group publishes one atomic serving sample");
    check(mh_tx_count_exact("AT#RFSTS") == rfsts_before + 1u &&
              mh_tx_count_exact("AT#MONI") == 1u,
          "page-scoped serving request emits only its two commands");

    modem_diag_serving_t first = snapshot.serving;
    s_diag_rfsts_malformed_once = true;
    check(modem_service_diag_select(MODEM_DIAG_GROUP_SERVING, 1u, true),
          "same subscription can request a later refresh");
    mh_settle();
    modem_service_get_diag_snapshot(&snapshot);
    check(snapshot.group[MODEM_DIAG_GROUP_SERVING].state ==
              MODEM_DIAG_STATE_STALE &&
              snapshot.group[MODEM_DIAG_GROUP_SERVING].last_error ==
                  MODEM_DIAG_ERROR_MALFORMED &&
              snapshot.group[MODEM_DIAG_GROUP_SERVING].consecutive_failures == 1u &&
              snapshot.group[MODEM_DIAG_GROUP_SERVING].sequence == 1u &&
              memcmp(&snapshot.serving, &first, sizeof(first)) == 0,
          "malformed refresh keeps the last good payload and marks it stale");
    check(mh_tx_count_exact("AT#MONI") == 1u,
          "failed first row cannot continue into a hybrid group");

    s_diag_rfsts_hold_once = true;
    check(modem_service_diag_select(MODEM_DIAG_GROUP_SERVING, 2u, true),
          "new generation starts a fresh serving request");
    modem_service_get_diag_snapshot(&snapshot);
    check(snapshot.group[MODEM_DIAG_GROUP_SERVING].state ==
              MODEM_DIAG_STATE_PENDING &&
              snapshot.group[MODEM_DIAG_GROUP_SERVING].last_error ==
                  MODEM_DIAG_ERROR_MALFORMED &&
              snapshot.group[MODEM_DIAG_GROUP_SERVING].consecutive_failures == 1u,
          "pending retry preserves the completed failure cause and streak");
    mh_advance(1u);
    modem_service_test_snapshot_t probe = service_probe();
    check(probe.command_active &&
              probe.active_command == MODEM_SERVICE_TEST_COMMAND_DIAG_QUERY,
          "held transcript leaves exactly one diagnostic command active");
    check(modem_service_diag_select(MODEM_DIAG_GROUP_SERVING, 2u, true),
          "duplicate active request coalesces");
    modem_service_get_diag_snapshot(&snapshot);
    check(snapshot.scheduler.coalesced == 1u &&
              snapshot.scheduler.active_group == MODEM_DIAG_GROUP_SERVING,
          "scheduler exposes active-group coalescing");
    mh_feed(
        "#RFSTS: \"310 410\",5230,-101,-82,-11.5,00AF,,32,3,1,"
        "ABCDEF01,\"310410123456789\",\"AT&T\",2,12,100");
    mh_feed("OK");
    modem_service_get_diag_snapshot(&snapshot);
    check(snapshot.group[MODEM_DIAG_GROUP_SERVING].state ==
              MODEM_DIAG_STATE_FRESH &&
              snapshot.group[MODEM_DIAG_GROUP_SERVING].sequence == 2u &&
              snapshot.group[MODEM_DIAG_GROUP_SERVING].consecutive_failures == 0u &&
              mh_tx_count_exact("AT#RFSTS") == rfsts_before + 3u &&
              mh_tx_count_exact("AT#MONI") == 2u,
          "one coalesced request produces one additional atomic commit");

    s_diag_rfsts_hold_once = true;
    check(modem_service_diag_select(MODEM_DIAG_GROUP_SERVING, 3u, true),
          "old generation command starts for cancellation test");
    mh_advance(1u);
    check(modem_service_diag_select(MODEM_DIAG_GROUP_REGISTRATION, 4u, true),
          "page change replaces the selected generation");
    mh_feed("OK");
    modem_service_get_diag_snapshot(&snapshot);
    check(snapshot.scheduler.generation == 4u &&
              snapshot.scheduler.selected_group ==
                  MODEM_DIAG_GROUP_REGISTRATION &&
              snapshot.scheduler.cancelled >= 1u &&
              snapshot.group[MODEM_DIAG_GROUP_REGISTRATION].state ==
                  MODEM_DIAG_STATE_FRESH &&
              snapshot.group[MODEM_DIAG_GROUP_SERVING].sequence == 2u &&
              snapshot.group[MODEM_DIAG_GROUP_SERVING].consecutive_failures == 0u,
          "late final cannot publish or count cancellation as a refresh failure");

    begin_telit(true);
    check(boot_until_ready(30000u), "call-preemption fixture boots");
    settle_dtr_sleep();
    s_diag_rfsts_hold_once = true;
    check(modem_service_diag_select(MODEM_DIAG_GROUP_SERVING, 9u, true),
          "serving group starts before call admission");
    mh_advance(1u);
    mh_advance(1u);
    check(modem_service_request_dial("15551234567"),
          "dial is admitted while a diagnostic command is in flight");
    modem_service_get_diag_snapshot(&snapshot);
    check(snapshot.scheduler.cancelled == 1u,
          "call admission cancels the remaining diagnostic group");
    mh_feed("OK");
    check(mh_tx_count_exact("ATD15551234567;") == 1u &&
              mh_tx_count_exact("AT#MONI") == 0u,
          "dial runs at the first command boundary before any next diag row");
    modem_service_get_diag_snapshot(&snapshot);
    check(snapshot.group[MODEM_DIAG_GROUP_SERVING].sequence == 0u,
          "cancelled in-flight shadow never becomes a published sample");
}

static void test_plain_diag_response_cannot_swallow_call_progress(void) {
    begin_telit(true);
    check(boot_until_ready(30000u), "plain diagnostic interleave fixture boots");
    settle_dtr_sleep();

    s_diag_firmware_hold_once = true;
    check(modem_service_diag_select(MODEM_DIAG_GROUP_IDENTITY, 10u, true),
          "identity group is admitted");
    mh_advance(1u);
    mh_advance(1u);
    modem_service_test_snapshot_t probe = service_probe();
    check(probe.command_active &&
              probe.active_command == MODEM_SERVICE_TEST_COMMAND_DIAG_QUERY &&
              strcmp(mh_status().debug_last_command, "AT+CGMR") == 0,
          "firmware plain-text query is held in flight");

    mh_feed("BUSY");
    mh_feed("OK");

    modem_diag_snapshot_t snapshot;
    modem_service_get_diag_snapshot(&snapshot);
    check(snapshot.scheduler.cancelled == 1u &&
              snapshot.group[MODEM_DIAG_GROUP_IDENTITY].sequence == 0u,
          "call progress cancels the identity shadow instead of becoming firmware text");
    check(mh_tx_count_exact("AT+CGMM") == 1u &&
              mh_tx_count_exact("AT+CGMR") == 1u &&
              mh_tx_count_exact("AT+CGSN") == 1u,
          "cancelled plain response cannot advance to the final identity row");
}

static void test_packet_diag_accepts_wwx_dynamic_context_shape(void) {
    begin_telit(true);
    check(boot_until_ready(30000u), "packet diagnostic fixture boots");
    settle_dtr_sleep();

    check(modem_service_diag_select(MODEM_DIAG_GROUP_PACKET, 11u, true),
          "packet group is admitted");
    mh_advance(1u);
    mh_advance(1u);

    modem_diag_snapshot_t snapshot;
    modem_service_get_diag_snapshot(&snapshot);
    check(snapshot.group[MODEM_DIAG_GROUP_PACKET].state ==
              MODEM_DIAG_STATE_FRESH &&
              snapshot.group[MODEM_DIAG_GROUP_PACKET].sequence == 1u,
          "real WWX packet transcript commits atomically");
    check(snapshot.packet.attached == 1u &&
              snapshot.packet.context_count == 4u &&
              snapshot.packet.active_context_count == 2u &&
              snapshot.packet.first_cid == 1u &&
              snapshot.packet.first_status == 1u &&
              strcmp(snapshot.packet.apn, "apn.example") == 0 &&
              strcmp(snapshot.packet.address,
                     "192.0.2.168.255.255.255.240") == 0,
          "packet group publishes the active default context");
    check(mh_tx_count_exact("AT+CGATT?") == 1u &&
              mh_tx_count_exact("AT+CGACT?") == 1u &&
              mh_tx_count_exact("AT+CGCONTRDP") == 1u &&
              mh_tx_count_exact("AT+CGPADDR") == 0u,
          "packet group uses the WWX-supported three-command sequence");
}

static void test_optional_diag_timeout_is_not_unsupported(void) {
    begin_telit(true);
    check(boot_until_ready(30000u), "optional-timeout fixture boots");
    settle_dtr_sleep();

    check(modem_service_diag_select(MODEM_DIAG_GROUP_SERVING, 20u, true),
          "baseline serving group is admitted");
    mh_advance(1u);
    mh_advance(1u);
    modem_diag_snapshot_t snapshot;
    modem_service_get_diag_snapshot(&snapshot);
    check(snapshot.group[MODEM_DIAG_GROUP_SERVING].state ==
              MODEM_DIAG_STATE_FRESH &&
              snapshot.group[MODEM_DIAG_GROUP_SERVING].sequence == 1u,
          "baseline serving sample commits");
    modem_diag_serving_t baseline = snapshot.serving;

    s_diag_moni_hold_once = true;
    check(modem_service_diag_select(MODEM_DIAG_GROUP_SERVING, 20u, true),
          "refresh with optional row is admitted");
    mh_advance(1u);
    mh_advance(1u);
    modem_service_test_snapshot_t probe = service_probe();
    check(probe.command_active &&
              probe.active_command == MODEM_SERVICE_TEST_COMMAND_DIAG_QUERY &&
              strcmp(mh_status().debug_last_command, "AT#MONI") == 0,
          "optional MONI row is held in flight");
    mh_advance(5001u);
    modem_service_get_diag_snapshot(&snapshot);
    check(snapshot.group[MODEM_DIAG_GROUP_SERVING].state ==
              MODEM_DIAG_STATE_STALE &&
              snapshot.group[MODEM_DIAG_GROUP_SERVING].last_error ==
                  MODEM_DIAG_ERROR_TIMEOUT &&
              snapshot.group[MODEM_DIAG_GROUP_SERVING].consecutive_failures == 1u &&
              snapshot.group[MODEM_DIAG_GROUP_SERVING].sequence == 1u &&
              snapshot.scheduler.command_timeouts == 1u &&
              memcmp(&snapshot.serving, &baseline, sizeof(baseline)) == 0,
          "optional timeout fails atomically and preserves the last sample");
}

static modem_maintenance_snapshot_t maintenance_snapshot(void) {
    modem_maintenance_snapshot_t snapshot;
    modem_service_get_maintenance_snapshot(&snapshot);
    return snapshot;
}

static void test_guarded_scan_and_band_maintenance(void) {
    begin_telit(true);
    check(boot_until_ready(30000u), "maintenance fixture boots");
    check(modem_service_maintenance_supported(),
          "Telit advertises guarded maintenance through the neutral service");
    mh_clear_tx_capture();

    check(modem_service_maintenance_request_scan_timer(300u),
          "scan timer write is admitted while idle");
    mh_settle();
    modem_maintenance_snapshot_t snapshot = maintenance_snapshot();
    check(snapshot.state == MODEM_MAINTENANCE_DONE &&
              snapshot.scan_timer_s == 300u && s_scan_timer_s == 300u &&
              mh_tx_count_exact("AT#NWSCANTMR=300") == 1u &&
              mh_tx_count_exact("AT#NWSCANTMR?") == 1u,
          "scan timer write is accepted only after exact readback");

    modem_band_config_t original = s_band_nvm;
    check(modem_service_maintenance_start_band_test(2u),
          "B4 RAM-only band test is admitted");
    mh_settle();
    snapshot = maintenance_snapshot();
    size_t first_mutation = tx_first_index("AT#SELBNDMODE=1");
    check(snapshot.state == MODEM_MAINTENANCE_ACTIVE &&
              snapshot.band_test_active && snapshot.band_preset == 2u &&
              s_band_mode == 1u && s_band_ram.gsm == original.gsm &&
              s_band_ram.wcdma == original.wcdma &&
              s_band_ram.lte == UINT64_C(0x8),
          "band test preserves GSM/WCDMA and constrains only the requested LTE band");
    check(s_mh_radio_recovery[0] != '\0' &&
              first_mutation != SIZE_MAX &&
              s_mh_recovery_store_tx_count <= first_mutation,
          "persistent recovery marker precedes the first band mutation");
    check(modem_service_maintenance_restore_band(),
          "explicit band restore is admitted");
    mh_settle();
    snapshot = maintenance_snapshot();
    check(snapshot.state == MODEM_MAINTENANCE_DONE &&
              !snapshot.band_test_active && snapshot.band_preset == 0u &&
              s_band_mode == 0u &&
              s_band_ram.gsm == original.gsm &&
              s_band_ram.wcdma == original.wcdma &&
              s_band_ram.lte == original.lte &&
              s_mh_radio_recovery[0] == '\0',
          "band restore verifies the full original tuple and retires the preset before clearing recovery");
}

static void test_scan_maintenance_yields_to_call(void) {
    begin_telit(true);
    check(boot_until_ready(30000u), "scan-preemption fixture boots");
    mh_clear_tx_capture();
    check(modem_service_maintenance_request_scan_timer(900u),
          "scan write is admitted before a call");

    modem_service_tick(s_mh_now);
    modem_service_test_snapshot_t probe = service_probe();
    check(probe.command_active &&
              probe.active_command == MODEM_SERVICE_TEST_COMMAND_MAINTENANCE &&
              strcmp(mh_status().debug_last_command,
                     "AT#NWSCANTMR=900") == 0,
          "scan mutation is in flight before call admission");
    check(modem_service_request_dial("15551234567"),
          "dial queues behind the in-flight scan final");
    check(mh_respond_to_tx(), "scan command receives its final");
    mh_settle();

    check(mh_tx_count_exact("AT#NWSCANTMR?") == 0u &&
              tx_first_index("ATD15551234567;") != SIZE_MAX &&
              maintenance_snapshot().error ==
                  MODEM_MAINTENANCE_ERROR_CANCELLED,
          "call skips scan readback immediately after the current final");
}

static void test_telit_coarse_call_events_reconcile_live_projection(void) {
    begin_telit(true);
    check(boot_until_ready(30000u), "Telit call-projection fixture boots");

    s_clcc_row = "+CLCC: 1,0,0,0,0,\"15551234567\",129";
    check(modem_service_request_dial("15551234567"),
          "outgoing Telit call is admitted");
    mh_settle();
    modem_status_t status = mh_status();
    check(status.call_state == MODEM_CALL_ACTIVE &&
              status.active_call_id == 1u &&
              status.last_call_result == MODEM_CALL_RESULT_CONNECTED,
          "CLCC-confirmed Telit outgoing call leaves setup and publishes ACTIVE");

    mh_rx_push("+CCWA: \"5552000\",129,1,\"Doe, Jane\",0");
    modem_service_tick(s_mh_now);
    status = mh_status();
    check(status.waiting_call && status.ring_active &&
              strcmp(status.incoming_number, "5552000") == 0 &&
              !status.caller_id_withheld,
          "typed CCWA parsing reaches the live waiting-call projection");

    begin_telit(true);
    check(boot_until_ready(30000u), "Telit incoming-call fixture boots");
    s_clcc_row = "+CLCC: 1,1,4,0,0,\"15557654321\",145";
    mh_feed("RING");
    mh_feed("+CLIP: \"+15557654321\",145,,,,0");
    mh_feed("#ECAM: 0,6,1,,,");
    status = mh_status();
    check(status.call_state == MODEM_CALL_RINGING && status.ring_active &&
              strcmp(status.incoming_number, "+15557654321") == 0 &&
              !status.caller_id_withheld,
          "typed CLIP plus Telit call evidence publishes the incoming identity");

    s_clcc_row = NULL;
    uint32_t released_at = s_mh_now;
    mh_feed("#ECAM: 0,0,1,,,");
    mh_advance(g_modem_vendor.call.timing.clcc_confirm_ms + 1u);
    status = mh_status();
    check(status.call_state == MODEM_CALL_IDLE && !status.ring_active &&
              status.last_call_result == MODEM_CALL_RESULT_NO_CARRIER,
          "remote-cancelled Telit call clears after two clean empty CLCC snapshots");
    check((uint32_t)(s_mh_now - released_at) < 2000u,
          "remote cancellation does not wait for the 120-second ringing watchdog");
    check(!modem_service_battery_high_load_active(),
          "model-confirmed IDLE immediately releases the service high-load gate");
}

static void test_remote_foreground_release_preserves_waiting_identity(void) {
    begin_telit(true);
    check(boot_until_ready(30000u),
          "remote foreground-release ringing fixture boots");
    s_clcc_row = "+CLCC: 1,0,0,0,0,\"15551234567\",129";
    check(modem_service_request_dial("15551234567"),
          "foreground A is admitted before waiting B");
    mh_settle();

    mh_feed("+CCWA: \"15557654321\",145,1,\"Waiting B\",0");
    s_clcc_row =
        "+CLCC: 1,0,0,0,0,\"15551234567\",129\r\n"
        "+CLCC: 2,1,5,0,0,\"15557654321\",145";
    mh_feed("#ECAM: 0,5,1,,,");
    mh_advance(g_modem_vendor.call.timing.clcc_confirm_ms + 1u);
    modem_call_snapshot_t before = call_snapshot();
    const modem_call_leg_snapshot_t *waiting = snapshot_leg(&before, 2u);
    check(mh_status().waiting_call && waiting != NULL &&
              waiting->state == CALL_LEG_WAITING &&
              waiting->incoming_episode != 0u,
          "waiting B has a generation-qualified incoming episode");
    uint8_t waiting_generation = waiting != NULL ? waiting->generation : 0u;
    uint32_t waiting_episode =
        waiting != NULL ? waiting->incoming_episode : 0u;

    /* Ordering 1: A disappears while B remains network-waiting. The second
     * clean snapshot evicts A and the projector re-presents B as ordinary
     * RINGING; B's {id,generation,episode} must not change. */
    s_clcc_row = "+CLCC: 2,1,5,0,0,\"15557654321\",145";
    mh_feed("#ECAM: 0,0,1,,,");
    mh_advance(g_modem_vendor.call.timing.clcc_confirm_ms + 1u);
    mh_advance(g_modem_vendor.call.timing.clcc_confirm_ms + 1u);
    modem_status_t status = mh_status();
    modem_call_snapshot_t after = call_snapshot();
    waiting = snapshot_leg(&after, 2u);
    check(status.call_state == MODEM_CALL_RINGING &&
              !status.waiting_call && status.ring_active,
          "sole surviving waiting B is re-presented as an incoming ring");
    check(waiting != NULL && waiting->generation == waiting_generation &&
              waiting->incoming_episode == waiting_episode &&
              waiting->state == CALL_LEG_WAITING,
          "ringing handoff preserves B's exact model identity");

    begin_telit(true);
    check(boot_until_ready(30000u),
          "remote foreground-release promotion fixture boots");
    s_clcc_row = "+CLCC: 1,0,0,0,0,\"15551234567\",129";
    check(modem_service_request_dial("15551234567"),
          "promotion fixture admits foreground A");
    mh_settle();
    mh_feed("+CCWA: \"15557654321\",145,1,\"Waiting B\",0");
    s_clcc_row =
        "+CLCC: 1,0,0,0,0,\"15551234567\",129\r\n"
        "+CLCC: 2,1,5,0,0,\"15557654321\",145";
    mh_feed("#ECAM: 0,5,1,,,");
    mh_advance(g_modem_vendor.call.timing.clcc_confirm_ms + 1u);
    before = call_snapshot();
    waiting = snapshot_leg(&before, 2u);
    waiting_generation = waiting != NULL ? waiting->generation : 0u;
    waiting_episode = waiting != NULL ? waiting->incoming_episode : 0u;

    /* Ordering 2: B is already ACTIVE in the same snapshot that first marks A
     * absent. Genuine ACTIVE B overrides retained A immediately; the app can
     * hand off by identity without waiting for A's second absence snapshot. */
    s_clcc_row = "+CLCC: 2,1,0,0,0,\"15557654321\",145";
    mh_feed("#ECAM: 0,3,1,,,");
    mh_advance(g_modem_vendor.call.timing.clcc_confirm_ms + 1u);
    status = mh_status();
    after = call_snapshot();
    waiting = snapshot_leg(&after, 2u);
    check(status.call_state == MODEM_CALL_ACTIVE &&
              status.active_call_id == 2u && !status.waiting_call,
          "network-promoted B becomes the foreground active call");
    check(waiting != NULL && waiting->generation == waiting_generation &&
              waiting->incoming_episode == waiting_episode &&
              waiting->state == CALL_LEG_ACTIVE,
          "active promotion preserves B's exact model identity");
}

static void test_long_active_call_outlives_legacy_setup_horizon(void) {
    begin_telit(true);
    check(boot_until_ready(30000u), "long-call authority fixture boots");

    s_clcc_row = "+CLCC: 1,0,0,0,0,\"15551234567\",129";
    check(modem_service_request_dial("15551234567"),
          "long-call fixture admits its outgoing call");
    mh_settle();
    check(mh_status().call_state == MODEM_CALL_ACTIVE &&
              modem_service_battery_high_load_active(),
          "CLCC-confirmed active call owns the service high-load gate");

    /* Telit's coarse ECAM does not carry a trustworthy call id. The confirmed
     * CLCC leg remains authoritative after the old compatibility watchdog's
     * 120-second horizon; neither load policy nor sleep admission may regress
     * to the stale pre-CLCC DIALING state. */
    mh_advance(120001u);
    check(mh_status().call_state == MODEM_CALL_ACTIVE &&
              mh_status().active_call_id == 1u,
          "a confirmed active leg remains published beyond 120 seconds");
    check(modem_service_battery_high_load_active(),
          "a long active call keeps the service high-load gate asserted");

    begin_telit(true);
    check(boot_until_ready(30000u), "long incoming-call authority fixture boots");
    s_clcc_row = "+CLCC: 1,1,4,0,0,\"15557654321\",145";
    mh_feed("RING");
    mh_feed("+CLIP: \"+15557654321\",145,,,,0");
    check(mh_status().call_state == MODEM_CALL_RINGING,
          "long incoming-call fixture presents the network call");

    s_clcc_row = "+CLCC: 1,1,0,0,0,\"15557654321\",145";
    check(modem_service_request_answer(),
          "long incoming-call fixture admits Answer");
    mh_settle();
    mh_feed("#ECAM: 0,3,1,,,");
    mh_advance(g_modem_vendor.call.timing.clcc_confirm_ms + 1u);
    check(mh_status().call_state == MODEM_CALL_ACTIVE &&
              mh_status().active_call_id == 1u &&
              modem_service_battery_high_load_active(),
          "CLCC-confirmed answered call owns the service high-load gate");
    mh_advance(120001u);
    check(mh_status().call_state == MODEM_CALL_ACTIVE &&
              mh_status().active_call_id == 1u &&
              modem_service_battery_high_load_active(),
          "a long answered call survives the retired answer/ring watchdog horizon");
}

static void test_telit_hold_toggle_is_safely_gated(void) {
    begin_telit(true);
    check(boot_until_ready(30000u), "Telit hold-gate fixture boots");

    s_clcc_row = "+CLCC: 1,0,0,0,0,\"15551234567\",129";
    check(modem_service_request_dial("15551234567"),
          "hold-gate fixture admits its outgoing call");
    mh_settle();
    check(modem_service_call_hold_available(),
          "a sole confirmed active call exposes Hold");

    mh_clear_tx_capture();
    s_clcc_row = "+CLCC: 1,0,1,0,0,\"15551234567\",129";
    check(modem_service_request_call_hold(),
          "sole active Hold is admitted");
    mh_settle();
    mh_feed("#ECAM: 0,4,1,,,");
    mh_advance(g_modem_vendor.call.timing.clcc_confirm_ms + 1u);
    modem_status_t status = mh_status();
    check(mh_tx_count_exact("AT+CHLD=2") == 1u,
          "Hold emits exactly one toggle");
    check(status.call_state == MODEM_CALL_ACTIVE && status.call_on_hold,
          "CLCC publishes the sole leg as held after Hold");
    check(modem_service_call_hold_available(),
          "the sole held leg remains retrievable");

    s_clcc_row = "+CLCC: 1,0,0,0,0,\"15551234567\",129";
    check(modem_service_request_call_hold(),
          "sole held Unhold is admitted");
    mh_settle();
    mh_feed("#ECAM: 0,3,1,,,");
    mh_advance(g_modem_vendor.call.timing.clcc_confirm_ms + 1u);
    status = mh_status();
    check(mh_tx_count_exact("AT+CHLD=2") == 2u &&
              status.call_state == MODEM_CALL_ACTIVE &&
              !status.call_on_hold,
          "Unhold emits one further toggle and restores the active leg");

    mh_feed("+CCWA: \"5552000\",129,1,\"Doe, Jane\",0");
    status = mh_status();
    size_t toggles_before = mh_tx_count_exact("AT+CHLD=2");
    check(status.waiting_call && !modem_service_call_hold_available(),
          "a waiting caller removes Hold availability");
    check(!modem_service_request_call_hold(),
          "service admission rejects Hold while a caller waits");
    mh_settle();
    check(mh_tx_count_exact("AT+CHLD=2") == toggles_before,
          "rejected waiting-call Hold writes no CHLD command");
}

static void test_telit_new_call_cleanup_is_model_owned(void) {
    begin_telit(true);
    check(boot_until_ready(30000u),
          "Telit New-call cleanup fixture boots");

    s_clcc_row = "+CLCC: 1,0,0,0,0,\"15551234567\",129";
    check(modem_service_request_dial("15551234567"),
          "New-call cleanup fixture admits the original call");
    mh_settle();
    check(mh_status().call_state == MODEM_CALL_ACTIVE &&
              mh_status().active_call_id == 1u,
          "the original call is CLCC-confirmed active");

    s_clcc_row =
        "+CLCC: 1,0,1,0,0,\"15551234567\",129\r\n"
        "+CLCC: 2,0,2,0,0,\"15557654321\",129";
    check(modem_service_request_dial("15557654321"),
          "a second outgoing call is admitted");
    mh_settle();
    mh_feed("#ECAM: 0,1,1,,,");
    check(mh_status().call_state == MODEM_CALL_DIALING,
          "CLCC binds the second outgoing leg while ECAM remains id-less");

    mh_clear_tx_capture();
    modem_service_new_call_abandoned();
    check(modem_service_new_call_cleanup_pending(),
          "the model exposes New-call cleanup without a legacy ECAM id");
    check(!modem_service_call_hold_available() &&
              !modem_service_request_call_hold(),
          "Hold admission stays closed until the abandoned leg is gone");
    check(!modem_service_request_dial("15550001111"),
          "another outgoing call cannot overtake New-call cleanup");
    mh_settle();
    check(mh_tx_count_exact("AT+CHLD=1") == 1u &&
              mh_tx_count_exact("AT+CHLD=12") == 0u &&
              mh_tx_count_exact("AT+CHLD=2") == 0u,
          "second-MO cleanup emits release-active, never id-scoped release or toggle");
    check(modem_service_new_call_cleanup_pending(),
          "CHLD command acceptance alone does not retire cleanup");

    s_clcc_row = "+CLCC: 1,0,0,0,0,\"15551234567\",129";
    mh_feed("#ECAM: 0,0,1,,,");
    mh_advance(g_modem_vendor.call.timing.clcc_confirm_ms + 1u);
    check(!modem_service_new_call_cleanup_pending(),
          "two clean CLCC snapshots retire the coarse-event cleanup gate");
    check(mh_status().call_state == MODEM_CALL_ACTIVE &&
              mh_status().active_call_id == 1u &&
              modem_service_call_hold_available(),
          "the held survivor is recovered and Hold becomes available again");
}

static bool begin_active_dtmf_fixture(const char *message) {
    begin_telit(true);
    if (!boot_until_ready(30000u)) {
        check(false, message);
        return false;
    }
    s_clcc_row = "+CLCC: 1,0,0,0,0,\"15551234567\",129";
    if (!modem_service_request_dial("15551234567")) {
        check(false, message);
        return false;
    }
    mh_settle();
    if (mh_status().call_state != MODEM_CALL_ACTIVE ||
        mh_status().active_call_id != 1u) {
        check(false, message);
        return false;
    }
    mh_clear_tx_capture();
    s_dtmf_command_count = 0u;
    return true;
}

static bool dtmf_history_matches(const char *symbols) {
    size_t position = 0u;
    size_t expected = strlen(symbols);
    for (size_t i = 0u; i < s_mh_tx_history_count; i++) {
        const char *command = s_mh_tx_history[i];
        if (strncmp(command, "AT+VTS=", 7u) != 0) {
            continue;
        }
        if (position >= expected || command[7] != symbols[position] ||
            command[8] != '\0') {
            return false;
        }
        position++;
    }
    return position == expected;
}

static void test_dtmf_sequence_is_atomic_and_ordered(void) {
    static const char sequence[] = "1234567890*#1234567890*#12345678";
    _Static_assert(sizeof(sequence) - 1u == MODEM_DTMF_SEQUENCE_MAX,
                   "fixture must cover the public DTMF bound");

    begin_telit(true);
    check(boot_until_ready(30000u), "DTMF no-call fixture boots");
    mh_clear_tx_capture();
    check(!modem_service_request_dtmf_sequence("123") &&
              !modem_service_request_dtmf('1'),
          "DTMF admission requires a confirmed active leg");
    mh_settle();
    check(s_dtmf_command_count == 0u,
          "rejected no-call DTMF writes nothing");

    if (!begin_active_dtmf_fixture("DTMF sequence fixture reaches ACTIVE")) {
        return;
    }
    check(modem_service_request_dtmf_sequence(sequence),
          "a 32-digit DTMF sequence is admitted as one request");
    mh_settle();
    check(s_dtmf_command_count == MODEM_DTMF_SEQUENCE_MAX &&
              dtmf_history_matches(sequence) &&
              !mh_status().operation_busy,
          "all 32 digits cross the UART once, in order, beyond FIFO depth six");

    char overlength[MODEM_DTMF_SEQUENCE_MAX + 2u];
    memset(overlength, '1', sizeof(overlength) - 1u);
    overlength[sizeof(overlength) - 1u] = '\0';
    size_t before = s_dtmf_command_count;
    check(!modem_service_request_dtmf_sequence(NULL) &&
              !modem_service_request_dtmf_sequence("") &&
              !modem_service_request_dtmf_sequence("12A3") &&
              !modem_service_request_dtmf_sequence(overlength),
          "invalid and overlength DTMF strings are rejected atomically");
    mh_settle();
    check(s_dtmf_command_count == before,
          "invalid DTMF admission never emits a valid prefix");
}

static void test_dtmf_sequence_stops_on_failure_and_call_change(void) {
    if (!begin_active_dtmf_fixture("DTMF ERROR fixture reaches ACTIVE")) {
        return;
    }
    uint32_t errors_before = mh_status().command_errors;
    s_fault = TELIT_FAULT_DTMF_ERROR_ONCE;
    s_dtmf_fault_at = 3u;
    check(modem_service_request_dtmf_sequence("123456"),
          "DTMF ERROR fixture admits its sequence");
    mh_settle();
    check(s_fault_consumed && s_dtmf_command_count == 3u &&
              dtmf_history_matches("123") &&
              mh_status().command_errors == errors_before + 1u &&
              !mh_status().operation_busy,
          "a digit ERROR retires the unsent suffix exactly once");

    if (!begin_active_dtmf_fixture("DTMF timeout fixture reaches ACTIVE")) {
        return;
    }
    s_fault = TELIT_FAULT_DTMF_TIMEOUT_ONCE;
    s_dtmf_fault_at = 2u;
    check(modem_service_request_dtmf_sequence("123456"),
          "DTMF timeout fixture admits its sequence");
    mh_settle();
    check(s_dtmf_command_count == 2u && service_probe().command_active,
          "the second DTMF digit remains unresolved before its deadline");
    mh_advance(1501u);
    check(s_fault_consumed && s_dtmf_command_count == 2u &&
              dtmf_history_matches("12") &&
              !mh_status().operation_busy,
          "a digit timeout retires the unsent suffix without replay");

    if (!begin_active_dtmf_fixture("DTMF release fixture reaches ACTIVE")) {
        return;
    }
    s_dtmf_inject_release_on_first = true;
    check(modem_service_request_dtmf_sequence("123456"),
          "DTMF release fixture admits its sequence");
    mh_settle();
    check(s_dtmf_command_count == 1u && dtmf_history_matches("1") &&
              !mh_status().operation_busy,
          "a coarse release event cancels the suffix before CLCC catches up");

    if (!begin_active_dtmf_fixture("DTMF hangup fixture reaches ACTIVE")) {
        return;
    }
    s_dtmf_queue_hangup_on_first = true;
    check(modem_service_request_dtmf_sequence("123456"),
          "DTMF hangup fixture admits its sequence");
    mh_settle();
    check(s_dtmf_hangup_admitted && s_dtmf_command_count == 1u &&
              mh_tx_count_exact("AT+CHUP") == 1u,
          "queued Hangup preempts the suffix after the current digit final");
}

static void test_dtmf_sequence_does_not_survive_recovery(void) {
    if (!begin_active_dtmf_fixture("DTMF recovery fixture reaches ACTIVE")) {
        return;
    }
    s_fault = TELIT_FAULT_DTMF_TIMEOUT_ONCE;
    s_dtmf_fault_at = 1u;
    check(modem_service_request_dtmf_sequence("123456"),
          "DTMF recovery fixture admits its sequence");
    mh_settle();
    check(s_dtmf_command_count == 1u && service_probe().command_active,
          "DTMF recovery fault lands with one digit on wire");

    s_mh_supply_pg = false;
    mh_advance(250u);
    check(service_probe().state == MODEM_SERVICE_TEST_STATE_FAILED &&
              service_probe().request_queue_depth == 0u &&
              !mh_status().operation_busy,
          "runtime power recovery cancels current and queued DTMF ownership");

    s_fault = TELIT_FAULT_NONE;
    s_dtmf_fault_at = 0u;
    s_mh_supply_pg = true;
    modem_service_power_on();
    check(boot_until_ready(30000u),
          "modem recovers after an in-flight DTMF power fault");
    check(s_dtmf_command_count == 1u,
          "recovery never replays the cancelled DTMF suffix");

    s_clcc_row = "+CLCC: 1,0,0,0,0,\"15551234567\",129";
    check(modem_service_request_dial("15551234567"),
          "a fresh call is admitted after DTMF recovery");
    mh_settle();
    mh_clear_tx_capture();
    s_dtmf_command_count = 0u;
    check(modem_service_request_dtmf_sequence("9#"),
          "a fresh DTMF sequence is admitted after recovery");
    mh_settle();
    check(s_dtmf_command_count == 2u && dtmf_history_matches("9#") &&
              !mh_status().operation_busy,
          "fresh post-recovery DTMF has no inherited sequence state");
}

static void test_band_marker_requires_durable_clean_unit(void) {
    begin_telit(true);
    check(boot_until_ready(30000u), "marker-durability fixture boots");
    s_mh_store_flush_leaves_dirty = true;

    check(modem_service_maintenance_start_band_test(1u),
          "band test starts while the store still accepts writes");
    mh_settle();
    modem_maintenance_snapshot_t snapshot = maintenance_snapshot();
    check(snapshot.state == MODEM_MAINTENANCE_ERROR &&
              snapshot.error == MODEM_MAINTENANCE_ERROR_STORAGE &&
              snapshot.recovery_pending &&
              mh_tx_count_exact("AT#SELBNDMODE=1") == 0u,
          "a still-dirty recovery marker blocks the first modem mutation");
    check(!modem_service_maintenance_read_scan_timer(),
          "a pending recovery marker blocks unrelated maintenance");

    s_mh_store_flush_leaves_dirty = false;
    check(modem_service_maintenance_restore_band(),
          "conservative restore retries marker cleanup");
    mh_settle();
    snapshot = maintenance_snapshot();
    check(snapshot.state == MODEM_MAINTENANCE_DONE &&
              !snapshot.recovery_pending &&
              s_mh_radio_recovery[0] == '\0',
          "verified restore clears the durable marker before re-admission");
}

static void test_band_recovery_and_call_preemption(void) {
    begin_telit(true);
    check(boot_until_ready(30000u), "band recovery fixture boots");
    modem_band_config_t original = s_band_nvm;
    check(modem_service_maintenance_start_band_test(3u),
          "B5 test starts");
    mh_settle();
    check(maintenance_snapshot().band_test_active &&
              s_band_ram.lte == UINT64_C(0x10),
          "B5 RAM mask becomes active");

    size_t before_call = s_mh_tx_history_count;
    check(modem_service_request_dial("15551234567"),
          "dial is retained while a band test is active");
    mh_settle();
    size_t dial_index = tx_first_index("ATD15551234567;");
    size_t restore_index = tx_first_index("AT#SELBNDMODE=0");
    check(dial_index != SIZE_MAX && dial_index >= before_call &&
              restore_index != SIZE_MAX && restore_index < dial_index &&
              s_band_mode == 0u && s_band_ram.lte == original.lte &&
              s_mh_radio_recovery[0] == '\0',
          "call dispatch waits for verified band restoration");

    begin_telit(true);
    check(boot_until_ready(30000u), "interrupted-band fixture boots");
    original = s_band_nvm;
    check(modem_service_maintenance_start_band_test(1u),
          "interrupted B2 test starts");
    mh_settle();
    check(s_mh_radio_recovery[0] != '\0',
          "interrupted test has a durable recovery record");
    graceful_power_off();
    check(s_mh_radio_recovery[0] != '\0',
          "ordinary shutdown preserves an un-restored recovery record");
    check(boot_until_ready(30000u), "next boot reaches READY");
    mh_settle();
    check(s_band_mode == 0u && s_band_ram.lte == original.lte &&
              s_mh_radio_recovery[0] == '\0' &&
              maintenance_snapshot().state == MODEM_MAINTENANCE_DONE,
          "next READY automatically restores an interrupted band test");
}

static void test_deferred_band_mutation_keeps_its_phase(void) {
    begin_telit(true);
    check(boot_until_ready(30000u), "deferred-band fixture boots");

    check(modem_service_maintenance_start_band_test(3u),
          "deferred B5 test starts");
    modem_service_tick(s_mh_now);
    check(mh_respond_to_tx(), "band mode query receives its response");
    modem_service_tick(s_mh_now);
    check(mh_respond_to_tx(), "band baseline query receives its response");
    modem_uart_hal_set_dtr_sleep_permitted(true);
    s_mh_dtr_wake_works = false;
    modem_service_tick(s_mh_now);
    modem_maintenance_snapshot_t deferred = maintenance_snapshot();
    modem_service_test_snapshot_t probe = service_probe();
    check(deferred.state == MODEM_MAINTENANCE_RUNNING &&
              deferred.recovery_pending && probe.deferred_command_valid &&
              probe.deferred_command ==
                  MODEM_SERVICE_TEST_COMMAND_MAINTENANCE,
          "durable marker precedes a RAM-mode mutation deferred by DTR wake");
    modem_service_maintenance_cancel();
    modem_maintenance_snapshot_t cancelled = maintenance_snapshot();
    check(cancelled.state == deferred.state &&
              cancelled.step == deferred.step,
          "page exit cannot advance the deferred command's phase");
    check(modem_service_request_dial("15551234567"),
          "call queues while the maintenance command is deferred");

    s_mh_dtr_wake_works = true;
    s_mh_cts_asserted = true;
    mh_settle();
    size_t mutation = tx_first_index("AT#SELBNDMODE=1");
    size_t restore = tx_first_index("AT#SELBNDMODE=0");
    size_t dial = tx_first_index("ATD15551234567;");
    check(mutation != SIZE_MAX && restore != SIZE_MAX && dial != SIZE_MAX &&
              mutation < restore && restore < dial &&
              s_mh_radio_recovery[0] == '\0',
          "deferred mutation finishes, restores exactly, then releases the call");
}

static void test_guarded_antenna_maintenance(void) {
    begin_telit(true);
    check(boot_until_ready(30000u), "antenna maintenance fixture boots");
    bool provisioning_before_antenna = mh_status().provisioning_verified;
    check(modem_service_maintenance_enter_antenna(),
          "manual antenna ownership is admitted while idle");
    mh_settle();
    modem_maintenance_snapshot_t snapshot = maintenance_snapshot();
    check(snapshot.state == MODEM_MAINTENANCE_ACTIVE &&
              snapshot.antenna_active && snapshot.antenna_rf == 1u &&
              s_cfun == 4u && !s_stune_enabled &&
              s_gpio2_direction == 1u && !s_gpio2_state &&
              s_gpio3_direction == 1u && !s_gpio3_state,
          "entry verifies production policy before taking both GPIOs at CFUN4");
    modem_status_t sim_before_cfun4_qss = mh_status();
    mh_rx_push("#QSS: 0");
    modem_service_tick(s_mh_now);
    modem_status_t sim_during_cfun4_qss = mh_status();
    check(sim_before_cfun4_qss.sim_ready &&
              sim_during_cfun4_qss.sim_checked &&
              sim_during_cfun4_qss.sim_present &&
              sim_during_cfun4_qss.sim_ready,
          "CFUN4 maintenance cannot turn transient QSS 0 into physical SIM removal");
    check(modem_service_maintenance_select_antenna(4u),
          "RF4 selection is admitted only after ownership is active");
    mh_settle();
    check(s_gpio2_state && s_gpio3_state &&
              maintenance_snapshot().antenna_rf == 4u,
          "RF4 drives the documented 1/1 control state with readback");
    s_qss_response_enabled = false; /* exercise lifecycle URCs after CFUN verify */
    check(modem_service_maintenance_exit_antenna(),
          "manual antenna exit is admitted");
    mh_settle();
    snapshot = maintenance_snapshot();
    check(snapshot.state == MODEM_MAINTENANCE_DONE &&
              !snapshot.antenna_active && s_cfun == 5u &&
              s_stune_enabled && s_gpio2_direction == 17u &&
              s_gpio3_direction == 18u &&
              s_tune_masks[0] == EXPECTED_TUNE_MASKS[0] &&
              s_tune_masks[1] == EXPECTED_TUNE_MASKS[1] &&
              s_tune_masks[2] == EXPECTED_TUNE_MASKS[2] &&
              s_tune_masks[3] == EXPECTED_TUNE_MASKS[3],
          "exit restores ALT ownership, exact policy table, then CFUN5");
    mh_rx_push("#QSS: 2");
    modem_service_tick(s_mh_now);
    modem_status_t sim_during_cfun5_qss = mh_status();
    check(sim_during_cfun5_qss.sim_checked &&
              sim_during_cfun5_qss.sim_present &&
              sim_during_cfun5_qss.sim_ready &&
              service_probe().maintenance_sim_guard_deadline_ms != 0u &&
              service_probe().maintenance_sim_deferred ==
                  MODEM_SERVICE_TEST_SIM_PRESENT,
          "intermediate post-CFUN5 QSS 2 preserves the confirmed SIM state");
    mh_rx_push("#QSS: 3");
    modem_service_tick(s_mh_now);
    check(mh_status().sim_ready &&
              service_probe().maintenance_sim_guard_deadline_ms != 0u &&
              service_probe().maintenance_sim_deferred ==
                  MODEM_SERVICE_TEST_SIM_NONE,
          "post-CFUN5 QSS READY clears downgrade evidence but keeps the lifecycle guarded");
    mh_rx_push("#QSS: 2");
    modem_service_tick(s_mh_now);
    check(mh_status().sim_ready &&
              service_probe().maintenance_sim_deferred ==
                  MODEM_SERVICE_TEST_SIM_PRESENT,
          "late QSS 2 after READY remains guarded until lifecycle convergence");
    mh_rx_push("#QSS: 3");
    modem_service_tick(s_mh_now);
    check(mh_status().sim_ready &&
              service_probe().maintenance_sim_guard_deadline_ms != 0u &&
              service_probe().maintenance_sim_deferred ==
                  MODEM_SERVICE_TEST_SIM_NONE,
          "final READY clears the late downgrade without ending the bounded guard");
    s_qss_response_enabled = true;
    for (uint32_t elapsed = 0u;
         elapsed < 30000u &&
         (service_probe().sim_completion_pending ||
          service_probe().sim_completion_active ||
          mh_status().provisioning_verified != provisioning_before_antenna);
         elapsed += 100u) {
        mh_advance(100u);
    }
    check(mh_status().provisioning_verified == provisioning_before_antenna &&
              !service_probe().sim_completion_pending &&
              !service_probe().sim_completion_active &&
              service_probe().sms_wake_armed,
          "maintenance CFUN cycle completes required SMS-wake requalification");
    mh_advance(g_modem_vendor.maintenance.sim_transition_window_ms + 1u);
    check(service_probe().maintenance_sim_guard_deadline_ms == 0u,
          "successful post-CFUN5 lifecycle guard expires at the vendor bound");
    mh_rx_push("#QSS: 0");
    modem_service_tick(s_mh_now);
    modem_status_t sim_after_ready_qss = mh_status();
    check(sim_after_ready_qss.sim_checked &&
              !sim_after_ready_qss.sim_present &&
              !sim_after_ready_qss.sim_ready,
          "QSS 0 is authoritative after the guarded lifecycle settles");

    begin_telit(true);
    check(boot_until_ready(30000u),
          "antenna SIM-guard timeout fixture boots");
    check(modem_service_maintenance_enter_antenna(),
          "SIM-guard timeout fixture enters manual mode");
    mh_settle();
    s_qss_response_enabled = false; /* no READY: prove bounded fallback */
    check(modem_service_maintenance_exit_antenna(),
          "SIM-guard timeout fixture starts restore");
    mh_settle();
    mh_rx_push("#QSS: 0");
    modem_service_tick(s_mh_now);
    check(mh_status().sim_ready,
          "post-CFUN5 downgrade remains deferred inside the vendor window");
    mh_advance(g_modem_vendor.maintenance.sim_transition_window_ms - 1u);
    check(mh_status().sim_ready,
          "deferred SIM downgrade remains guarded through the full window");
    mh_advance(2u);
    check(!mh_status().sim_present && !mh_status().sim_ready,
          "latest deferred SIM state becomes authoritative at guard timeout");

    begin_telit(true);
    check(boot_until_ready(30000u),
          "antenna input-level fixture boots");
    s_fault = TELIT_FAULT_MAINT_GPIO2_INPUT_HIGH;
    check(modem_service_maintenance_enter_antenna(),
          "manual antenna entry tolerates a high released input");
    mh_settle();
    snapshot = maintenance_snapshot();
    check(snapshot.state == MODEM_MAINTENANCE_ACTIVE &&
              snapshot.antenna_active && s_cfun == 4u,
          "input readback verifies ownership by direction, not pin voltage");
    check(modem_service_maintenance_exit_antenna(),
          "manual antenna exit starts after high-input entry");
    mh_settle();
    snapshot = maintenance_snapshot();
    check(snapshot.state == MODEM_MAINTENANCE_DONE && s_cfun == 5u &&
              s_gpio2_direction == 17u && s_gpio3_direction == 18u,
          "high released input does not prevent exact production restore");

    begin_telit(true);
    check(boot_until_ready(30000u),
          "antenna call-preemption fixture boots");
    check(modem_service_maintenance_enter_antenna(),
          "call-preemption fixture enters manual mode");
    mh_settle();
    check(maintenance_snapshot().state == MODEM_MAINTENANCE_ACTIVE &&
              s_cfun == 4u && !s_stune_enabled,
          "call-preemption starts from verified manual ownership");
    size_t before_call = s_mh_tx_history_count;
    check(modem_service_request_dial("15551234567"),
          "dial is retained while manual antenna ownership is active");
    mh_settle();
    size_t restore_cfun = tx_last_index("AT+CFUN=5");
    size_t dial = tx_last_index("ATD15551234567;");
    snapshot = maintenance_snapshot();
    check(restore_cfun != SIZE_MAX && restore_cfun >= before_call &&
              dial != SIZE_MAX && restore_cfun < dial &&
              snapshot.state == MODEM_MAINTENANCE_DONE &&
              !snapshot.antenna_active && s_cfun == 5u &&
              s_stune_enabled && s_gpio2_direction == 17u &&
              s_gpio3_direction == 18u &&
              s_tune_masks[0] == EXPECTED_TUNE_MASKS[0] &&
              s_tune_masks[1] == EXPECTED_TUNE_MASKS[1] &&
              s_tune_masks[2] == EXPECTED_TUNE_MASKS[2] &&
              s_tune_masks[3] == EXPECTED_TUNE_MASKS[3],
          "call dispatch waits for exact antenna-policy restoration");

    begin_telit(true);
    check(boot_until_ready(30000u), "antenna fail-closed fixture boots");
    check(modem_service_maintenance_enter_antenna(),
          "fail-closed fixture enters manual mode");
    mh_settle();
    s_fault = TELIT_FAULT_MAINT_GPIO2_ALT_READBACK;
    check(modem_service_maintenance_exit_antenna(),
          "restore starts with injected ALT readback mismatch");
    mh_settle();
    snapshot = maintenance_snapshot();
    check(snapshot.state == MODEM_MAINTENANCE_LOCKED && s_cfun == 4u &&
              mh_tx_count_exact("AT#GPIO=2,2") >= 4u,
          "unprovable tuner ownership exhausts retries and leaves RF disabled");
}

static void test_call_forwarding_mailbox_and_indicators(void) {
    begin_telit(true);
    s_message_waiting.category[MODEM_MESSAGE_WAITING_VOICE_LINE_1] =
        (modem_message_waiting_state_t){.active = true, .count = 4u};
    s_message_waiting.category[MODEM_MESSAGE_WAITING_FAX] =
        (modem_message_waiting_state_t){.active = true, .count = 2u};
    s_message_waiting.category[MODEM_MESSAGE_WAITING_EMAIL] =
        (modem_message_waiting_state_t){.active = true, .count = 5u};
    check(boot_until_ready(30000u),
          "supplementary-services fixture boots");
    mh_feed("+CEREG: 1");
    mh_settle();

    char mailbox[MODEM_PHONE_MAX + 1u];
    modem_status_t status = mh_status();
    check(mh_tx_count_exact("AT#MBN") == 1u &&
              modem_service_get_voice_mailbox_number(mailbox,
                                                     sizeof(mailbox)) &&
              strcmp(mailbox, "+18005551212") == 0,
          "SIM voice mailbox is discovered once after readiness");
    check(status.call_forward_unconditional_known &&
              !status.call_forward_unconditional_active &&
              mh_tx_count_exact("AT+CCFC=0,2,,,1") == 0u,
          "startup reads local divert flags without a network transaction");
    check(mh_tx_count_exact("AT#MWI?") == 1u &&
              status.message_waiting
                  .category[MODEM_MESSAGE_WAITING_VOICE_LINE_1].active &&
              status.message_waiting
                      .category[MODEM_MESSAGE_WAITING_VOICE_LINE_1].count ==
                  4u &&
              status.message_waiting
                  .category[MODEM_MESSAGE_WAITING_FAX].active &&
              status.message_waiting
                      .category[MODEM_MESSAGE_WAITING_FAX].count == 2u &&
              status.message_waiting
                  .category[MODEM_MESSAGE_WAITING_EMAIL].active &&
              status.message_waiting
                      .category[MODEM_MESSAGE_WAITING_EMAIL].count == 5u,
          "startup query atomically recovers all stored MWI categories");

    call_forward_request_t request;
    memset(&request, 0, sizeof(request));
    request.reason = CALL_FORWARD_REASON_UNCONDITIONAL;
    request.action = CALL_FORWARD_ACTION_REGISTER;
    request.has_number = true;
    snprintf(request.number, sizeof(request.number), "+15551234567");

    uint32_t request_id = 0u;
    check(modem_service_request_call_forward(&request, &request_id) &&
              request_id != 0u,
          "unconditional registration is admitted with an identity");
    mh_settle();
    call_forward_result_t result;
    check(modem_service_pop_call_forward_result(&result) &&
              result.request_id == request_id &&
              result.outcome == CALL_FORWARD_OUTCOME_SUCCESS &&
              mh_tx_count_exact(
                  "AT+CCFC=0,3,\"+15551234567\",145,1") == 1u,
          "registration publishes success for the exact Telit command");
    status = mh_status();
    check(status.call_forward_unconditional_known &&
              status.call_forward_unconditional_active,
          "post-mutation query refreshes the standby divert authority");

    memset(&request, 0, sizeof(request));
    request.reason = CALL_FORWARD_REASON_UNCONDITIONAL;
    request.action = CALL_FORWARD_ACTION_QUERY;
    check(modem_service_request_call_forward(&request, &request_id),
          "unconditional status query is admitted");
    mh_settle();
    check(modem_service_pop_call_forward_result(&result) &&
              result.request_id == request_id && result.status_known &&
              result.active &&
              strcmp(result.number, "+15551234567") == 0,
          "query returns the network row, number, and active state atomically");

    request.action = CALL_FORWARD_ACTION_DISABLE;
    check(modem_service_request_call_forward(&request, &request_id),
          "unconditional disable is admitted");
    mh_settle();
    check(modem_service_pop_call_forward_result(&result) &&
              result.outcome == CALL_FORWARD_OUTCOME_SUCCESS &&
              mh_tx_count_exact("AT+CCFC=0,0,,,1") == 1u &&
              !mh_status().call_forward_unconditional_active,
          "disable succeeds and the authoritative icon state is refreshed");

    memset(&request, 0, sizeof(request));
    request.reason = CALL_FORWARD_REASON_NO_REPLY;
    request.action = CALL_FORWARD_ACTION_REGISTER;
    request.has_number = true;
    snprintf(request.number, sizeof(request.number), "+15550000061");
    request.has_delay = true;
    request.delay_seconds = 15u;
    check(modem_service_request_call_forward(&request, &request_id),
          "Nokia no-reply registration with delay is admitted");
    mh_settle();
    check(modem_service_pop_call_forward_result(&result) &&
              result.outcome == CALL_FORWARD_OUTCOME_SUCCESS &&
              mh_tx_count_exact(
                  "AT+CCFC=2,3,\"+15550000061\",145,1") == 1u &&
              mh_tx_count_exact("AT+CCFC=2,1,,,1,15") == 1u,
          "no-reply delay executes the documented two-step Telit sequence");
    request.action = CALL_FORWARD_ACTION_QUERY;
    request.has_number = false;
    request.number[0] = '\0';
    request.has_delay = false;
    request.delay_seconds = 0u;
    check(modem_service_request_call_forward(&request, &request_id),
          "no-reply status after composite registration is admitted");
    mh_settle();
    check(modem_service_pop_call_forward_result(&result) &&
              result.status_known && result.active && result.has_delay &&
              result.delay_seconds == 15u &&
              strcmp(result.number, "+15550000061") == 0,
          "composite registration leaves the number and delay queryable");

    request.action = CALL_FORWARD_ACTION_ENABLE;
    request.has_delay = true;
    request.delay_seconds = 20u;
    check(modem_service_request_call_forward(&request, &request_id),
          "timer-only no-reply activation is admitted through the service");
    mh_settle();
    check(modem_service_pop_call_forward_result(&result) &&
              result.outcome == CALL_FORWARD_OUTCOME_SUCCESS &&
              mh_tx_count_exact("AT+CCFC=2,1,,,1,20") == 1u,
          "timer-only activation executes without inventing a destination");
    request.action = CALL_FORWARD_ACTION_QUERY;
    request.has_delay = false;
    request.delay_seconds = 0u;
    check(modem_service_request_call_forward(&request, &request_id),
          "timer-only no-reply result can be queried");
    mh_settle();
    check(modem_service_pop_call_forward_result(&result) &&
              result.status_known && result.active && result.has_delay &&
              result.delay_seconds == 20u &&
              strcmp(result.number, "+15550000061") == 0,
          "timer-only activation preserves the registered destination");

    s_fault = TELIT_FAULT_CALL_FORWARD_SECOND_ERROR_ONCE;
    s_fault_consumed = false;
    request.action = CALL_FORWARD_ACTION_REGISTER;
    request.has_number = true;
    snprintf(request.number, sizeof(request.number), "+15550000062");
    request.has_delay = true;
    request.delay_seconds = 25u;
    check(modem_service_request_call_forward(&request, &request_id),
          "partial-success fixture admits a composite request");
    mh_settle();
    check(modem_service_pop_call_forward_result(&result) &&
              result.outcome == CALL_FORWARD_OUTCOME_RESULT_UNKNOWN &&
              mh_tx_count_exact(
                  "AT+CCFC=2,3,\"+15550000062\",145,1") == 1u &&
              mh_tx_count_exact("AT+CCFC=2,1,,,1,25") == 1u,
          "second-step ERROR reports Result unknown after network mutation");

    mh_feed("#MWI: 1,1,3");
    status = mh_status();
    check(status.message_waiting
              .category[MODEM_MESSAGE_WAITING_VOICE_LINE_1].active &&
              status.message_waiting
                      .category[MODEM_MESSAGE_WAITING_VOICE_LINE_1].count ==
                  3u,
          "voice-message indication publishes its count");
    mh_feed("#MWI: 0,1");
    status = mh_status();
    check(!status.message_waiting
               .category[MODEM_MESSAGE_WAITING_VOICE_LINE_1].active &&
              status.message_waiting
                  .category[MODEM_MESSAGE_WAITING_FAX].active &&
              status.message_waiting
                  .category[MODEM_MESSAGE_WAITING_EMAIL].active,
          "category clear removes only voice and preserves fax/e-mail");
    mh_feed("#MWI: 1,4,6");
    status = mh_status();
    check(status.message_waiting
              .category[MODEM_MESSAGE_WAITING_EMAIL].active &&
              status.message_waiting
                      .category[MODEM_MESSAGE_WAITING_EMAIL].count == 6u,
          "e-mail URC updates its independent category");
    mh_feed("#MWI: 0");
    status = mh_status();
    check(!status.message_waiting
               .category[MODEM_MESSAGE_WAITING_VOICE_LINE_1].active &&
              !status.message_waiting
                   .category[MODEM_MESSAGE_WAITING_FAX].active &&
              !status.message_waiting
                   .category[MODEM_MESSAGE_WAITING_EMAIL].active,
          "unqualified MWI clear removes every category");
    mh_feed("#CFF: 1,1,+15550001111");
    check(mh_status().call_forward_unconditional_active,
          "CFF URC updates unconditional divert without polling");
    mh_feed("#CFF: 1,0,");
    check(!mh_status().call_forward_unconditional_active,
          "CFF clear URC removes unconditional divert without polling");

    memset(&request, 0, sizeof(request));
    s_fault = TELIT_FAULT_CALL_FORWARD_ERROR_ONCE;
    s_fault_consumed = false;
    request.reason = CALL_FORWARD_REASON_BUSY;
    request.action = CALL_FORWARD_ACTION_QUERY;
    check(modem_service_request_call_forward(&request, &request_id),
          "command-error query is admitted");
    mh_settle();
    check(modem_service_pop_call_forward_result(&result) &&
              result.outcome == CALL_FORWARD_OUTCOME_NOT_DONE,
          "registered command error maps to Nokia Not done");

    s_fault = TELIT_FAULT_CALL_FORWARD_TIMEOUT_ONCE;
    s_fault_consumed = false;
    request.reason = CALL_FORWARD_REASON_NO_REPLY;
    check(modem_service_request_call_forward(&request, &request_id),
          "timeout query is admitted");
    mh_settle();
    mh_advance(g_modem_vendor.supplementary.command_timeout_ms + 1u);
    check(modem_service_pop_call_forward_result(&result) &&
              result.outcome == CALL_FORWARD_OUTCOME_RESULT_UNKNOWN,
          "uncertain command timeout maps to Nokia Result unknown");

    memset(&request, 0, sizeof(request));
    request.reason = CALL_FORWARD_REASON_UNCONDITIONAL;
    request.action = CALL_FORWARD_ACTION_REGISTER;
    request.has_number = true;
    snprintf(request.number, sizeof(request.number), "+15550000020");
    size_t cfu_queries_before = mh_tx_count_exact("AT#CFF?");
    s_fault = TELIT_FAULT_CALL_FORWARD_TIMEOUT_ONCE;
    s_fault_consumed = false;
    check(modem_service_request_call_forward(&request, &request_id),
          "timed-out mutation fixture is admitted");
    mh_settle();
    mh_advance(g_modem_vendor.supplementary.command_timeout_ms + 1u);
    check(modem_service_pop_call_forward_result(&result) &&
              result.request_id == request_id &&
              result.outcome == CALL_FORWARD_OUTCOME_RESULT_UNKNOWN,
          "on-wire mutation timeout reports Result unknown");
    check(mh_tx_count_exact("AT#CFF?") ==
                  cfu_queries_before + 1u &&
              mh_status().call_forward_unconditional_known &&
              mh_status().call_forward_unconditional_active,
          "timed-out CFU mutation refreshes local flags without another network query");

    begin_telit(true);
    check(boot_until_ready(30000u),
          "supplementary cancellation fixture boots");
    mh_feed("+CEREG: 1");
    s_fault = TELIT_FAULT_CALL_FORWARD_TIMEOUT_ONCE;
    memset(&request, 0, sizeof(request));
    request.reason = CALL_FORWARD_REASON_BUSY;
    request.action = CALL_FORWARD_ACTION_QUERY;
    check(modem_service_request_call_forward(&request, &request_id),
          "cancellation fixture admits a request");
    mh_settle();
    modem_service_power_off();
    check(modem_service_pop_call_forward_result(&result) &&
              result.request_id == request_id &&
              result.outcome == CALL_FORWARD_OUTCOME_CANCELLED,
          "power-off explicitly cancels the in-flight UI request");

    begin_telit(true);
    check(boot_until_ready(30000u),
          "on-wire supplementary cancellation fixture boots");
    mh_feed("+CEREG: 1");
    mh_settle();
    memset(&request, 0, sizeof(request));
    request.reason = CALL_FORWARD_REASON_UNCONDITIONAL;
    request.action = CALL_FORWARD_ACTION_REGISTER;
    request.has_number = true;
    snprintf(request.number, sizeof(request.number), "+15550000021");
    s_fault = TELIT_FAULT_CALL_FORWARD_TIMEOUT_ONCE;
    s_fault_consumed = false;
    check(modem_service_request_call_forward(&request, &request_id),
          "on-wire mutation fixture admits a request");
    mh_settle();
    modem_service_test_snapshot_t probe = service_probe();
    check(probe.command_active &&
              probe.active_command ==
                  MODEM_SERVICE_TEST_COMMAND_CALL_FORWARD,
          "mutation command is on wire while its final is withheld");
    modem_service_power_off();
    check(modem_service_pop_call_forward_result(&result) &&
              result.request_id == request_id &&
              result.outcome == CALL_FORWARD_OUTCOME_RESULT_UNKNOWN,
          "power-off cannot call an on-wire mutation cleanly cancelled");

    begin_telit(true);
    check(boot_until_ready(30000u),
          "pre-dispatch supplementary failure fixture boots");
    mh_feed("+CEREG: 1");
    mh_settle();
    settle_dtr_sleep();
    s_mh_dtr_wake_works = false;
    memset(&request, 0, sizeof(request));
    request.reason = CALL_FORWARD_REASON_UNCONDITIONAL;
    request.action = CALL_FORWARD_ACTION_REGISTER;
    request.has_number = true;
    snprintf(request.number, sizeof(request.number), "+15550000022");
    check(modem_service_request_call_forward(&request, &request_id),
          "pre-dispatch failure fixture admits a request");
    mh_settle();
    check(service_probe().deferred_command_valid &&
              mh_tx_count_exact(
                  "AT+CCFC=0,3,\"+15550000022\",145,1") == 0u,
          "failed DTR wake keeps the mutation entirely off UART");
    mh_advance(g_modem_vendor.wake.dtr_wake_timeout_ms + 1u);
    check(modem_service_pop_call_forward_result(&result) &&
              result.request_id == request_id &&
              result.outcome == CALL_FORWARD_OUTCOME_NOT_DONE,
          "never-dispatched mutation is not mislabeled Result unknown");

    begin_telit(true);
    check(boot_until_ready(30000u),
          "incoming-divert indication fixture boots");
    mh_feed("+CSSU: 0");
    probe = service_probe();
    check(probe.pending_mt_active && !probe.pending_mt_alert_observed &&
              probe.pending_mt_incoming_diverted,
          "early CSSU is retained as hidden pre-id metadata");
    check(!mh_status().ring_active,
          "CSSU alone does not invent an incoming-call presentation");
    s_clcc_row = "+CLCC: 1,1,4,0,0,\"15557654321\",145";
    mh_feed("RING");
    mh_feed("#ECAM: 0,6,1,,,");
    check(mh_status().incoming_diverted,
          "redirect evidence binds to the incoming call for Nokia UI text");

    begin_telit(true);
    check(boot_until_ready(30000u),
          "redirected waiting-leg lifetime fixture boots");
    s_clcc_row = "+CLCC: 1,0,0,0,0,\"15551234567\",129";
    check(modem_service_request_dial("15551234567"),
          "foreground call is admitted before redirected waiting leg");
    mh_settle();
    check(mh_status().call_state == MODEM_CALL_ACTIVE,
          "foreground call is active before redirected waiting leg");

    mh_feed("+CSSU: 10");
    s_clcc_row =
        "+CLCC: 1,0,0,0,0,\"15551234567\",129\r\n"
        "+CLCC: 2,1,5,0,0,\"15557654321\",145";
    mh_feed("#ECAM: 0,5,1,,,");
    check(mh_status().waiting_call && mh_status().incoming_diverted,
          "additional-redirect marker binds to a waiting leg beside a foreground call");

    s_clcc_row = "+CLCC: 1,0,0,0,0,\"15551234567\",129";
    check(modem_service_request_call_waiting_reject(),
          "redirected waiting leg can be rejected");
    mh_settle();
    mh_feed("#ECAM: 0,0,1,,,");
    mh_advance(g_modem_vendor.call.timing.clcc_confirm_ms + 1u);
    modem_status_t surviving = mh_status();
    check(surviving.call_state == MODEM_CALL_ACTIVE,
          "foreground call survives redirected waiting-leg rejection");
    check(!surviving.waiting_call,
          "redirected waiting leg is authoritatively absent after rejection");
    check(!surviving.incoming_diverted,
          "redirect marker retires with its waiting leg while foreground survives");
}

static void test_mwi_urc_survives_query_interleave(void) {
    begin_telit(true);
    s_fault = TELIT_FAULT_MWI_URC_DURING_QUERY_ONCE;
    check(boot_until_ready(30000u),
          "MWI interleave fixture boots despite a missing solicited row");
    mh_settle();
    modem_status_t status = mh_status();
    check(s_fault_consumed &&
              status.message_waiting
                  .category[MODEM_MESSAGE_WAITING_VOICE_LINE_1].active &&
              status.message_waiting
                      .category[MODEM_MESSAGE_WAITING_VOICE_LINE_1].count ==
                  7u,
          "an unambiguous MWI URC inside AT#MWI? is not swallowed as a bad row");

    begin_telit(true);
    s_fault = TELIT_FAULT_MWI_URC_THEN_AMBIGUOUS_QUERY_ONCE;
    check(boot_until_ready(30000u),
          "MWI URC plus ambiguous snapshot fixture boots");
    mh_settle();
    status = mh_status();
    check(s_fault_consumed &&
              status.message_waiting
                  .category[MODEM_MESSAGE_WAITING_VOICE_LINE_1].active &&
              status.message_waiting
                      .category[MODEM_MESSAGE_WAITING_VOICE_LINE_1].count ==
                  7u &&
              status.command_errors == 0u,
          "ambiguous solicited row cannot overwrite a newer explicit voice URC");

    begin_telit(true);
    s_fault = TELIT_FAULT_MWI_FAX_URC_THEN_AMBIGUOUS_QUERY_ONCE;
    check(boot_until_ready(30000u),
          "fax URC plus three-field ambiguous snapshot fixture boots");
    mh_settle();
    status = mh_status();
    check(s_fault_consumed &&
              status.message_waiting
                  .category[MODEM_MESSAGE_WAITING_FAX].active &&
              status.message_waiting
                      .category[MODEM_MESSAGE_WAITING_FAX].count == 2u &&
              status.command_errors == 0u,
          "three-field ambiguity cannot erase its query-side fax category");
}

static void test_mwi_ambiguous_query_never_invents_voice_mail(void) {
    begin_telit(true);
    s_mwi_ambiguous_snapshot = true;
    s_message_waiting.category[MODEM_MESSAGE_WAITING_FAX] =
        (modem_message_waiting_state_t){.active = true, .count = 2u};
    s_message_waiting.category[MODEM_MESSAGE_WAITING_EMAIL] =
        (modem_message_waiting_state_t){.active = true, .count = 3u};
    check(boot_until_ready(30000u),
          "ambiguous MWI readback fixture boots");
    mh_settle();
    modem_status_t status = mh_status();
    check(mh_tx_count_exact("AT#MWI?") == 1u &&
              !status.message_waiting
                   .category[MODEM_MESSAGE_WAITING_VOICE_LINE_1].active &&
              status.message_waiting
                      .category[MODEM_MESSAGE_WAITING_VOICE_LINE_1].count ==
                  0u &&
              status.message_waiting
                  .category[MODEM_MESSAGE_WAITING_FAX].active &&
              status.message_waiting
                      .category[MODEM_MESSAGE_WAITING_FAX].count == 2u &&
              status.message_waiting
                  .category[MODEM_MESSAGE_WAITING_EMAIL].active &&
              status.message_waiting
                      .category[MODEM_MESSAGE_WAITING_EMAIL].count == 3u &&
              status.command_errors == 0u,
          "ambiguous voice row preserves voice while clean fax/e-mail rows commit");

    check(modem_service_request_debug_at("AT#MWI?"),
          "raw MWI diagnostic query is admitted");
    mh_settle();
    modem_debug_result_t result;
    check(modem_service_pop_debug_result(&result) && result.ok,
          "raw MWI diagnostic query completes");
    status = mh_status();
    check(!status.message_waiting
               .category[MODEM_MESSAGE_WAITING_VOICE_LINE_1].active &&
              status.message_waiting
                      .category[MODEM_MESSAGE_WAITING_VOICE_LINE_1].count ==
                  0u &&
              status.message_waiting
                  .category[MODEM_MESSAGE_WAITING_FAX].active &&
              status.message_waiting
                  .category[MODEM_MESSAGE_WAITING_EMAIL].active,
          "solicited raw MWI response cannot masquerade as a production URC");
}

static const char *UNKNOWN_PLMN_RFSTS =
    "#RFSTS: \"001 01\",5230,-101,-82,-11.5,00AF,,32,3,1,"
    "ABCDEF01,\"310410123456789\",\"Network Alias\",2,12,100";

static void set_sim_provider_fixture(const char *name) {
    static const char HEX[] = "0123456789ABCDEF";
    size_t length = strlen(name);
    check(length <= 16u, "SIM provider fixture fits EF-SPN");
    strcpy(s_sim_provider_fixture, "+CRSM: 144,0,\"00");
    size_t offset = strlen(s_sim_provider_fixture);
    for (size_t i = 0u; i < 16u; i++) {
        uint8_t byte = i < length ? (uint8_t)name[i] : 0xffu;
        s_sim_provider_fixture[offset++] = HEX[byte >> 4u];
        s_sim_provider_fixture[offset++] = HEX[byte & 15u];
    }
    s_sim_provider_fixture[offset++] = '"';
    s_sim_provider_fixture[offset] = '\0';
    s_sim_provider_response = s_sim_provider_fixture;
}

static void test_carrier_refresh_priority(void) {
    const char *held_commands[] = {"AT#MBN"};
    for (size_t i = 0u; i < sizeof(held_commands) / sizeof(held_commands[0]); i++) {
        begin_telit(true);
        s_cops_response = "+COPS: 0,0,\"Misleading Brand\",7";
        s_hold_final_command = held_commands[i];
        check(boot_until_ready(30000u), "carrier-priority fixture boots");
        mh_feed("+CEREG: 1");
        mh_advance(1000u);
        modem_status_t status = mh_status();
        check(status.network_registered &&
                  strcmp(status.operator_name, "AT&T") == 0 &&
                  status.operator_name_source == MODEM_OPERATOR_NAME_DATABASE,
              "carrier is available while a supplementary query is stalled");
        check(tx_first_index("AT#RFSTS") < tx_first_index(held_commands[i]) &&
                  mh_tx_count_exact("AT+COPS?") == 0u &&
                  mh_tx_count_exact(g_modem_vendor.sim_provider_query.query_cmd) == 0u,
              "boot carrier lookup precedes slow supplementary work");
    }

    begin_telit(true);
    check(boot_until_ready(30000u), "registration-priority fixture boots");
    settle_dtr_sleep();
    s_hold_final_command = "AT+CGMI";
    check(modem_service_request_debug_at("AT+CGMI"),
          "carrier-priority fixture holds one existing command");
    mh_advance(1u);
    mh_advance(1u);
    check(service_probe().command_active, "existing command remains active");
    mh_clear_tx_capture();
    check(modem_service_request_debug_at("AT+CGMM"),
          "ordinary request queues behind the active command");
    mh_feed("+CEREG: 0");
    mh_feed("+CEREG: 1");
    check(mh_tx_count_exact("AT#RFSTS") == 0u,
          "registration refresh never interrupts an on-wire command");
    modem_supplementary_refresh_arm(
        MODEM_SUPPLEMENTARY_REFRESH_VOICE_MAILBOX, s_mh_now);
    modem_supplementary_refresh_arm(
        MODEM_SUPPLEMENTARY_REFRESH_MESSAGE_WAITING, s_mh_now);
    s_hold_final_command = NULL;
    mh_feed("OK");
    mh_settle();
    check(strcmp(mh_status().operator_name, "AT&T") == 0 &&
              tx_first_index("AT#RFSTS") < tx_first_index("AT+CGMM") &&
              mh_tx_count_exact("AT+COPS?") == 0u,
          "registration-edge serving PLMN refresh precedes ordinary queued work");
    check(tx_first_index("AT#MBN") < tx_first_index("AT#MWI?") &&
              tx_first_index("AT#MWI?") < tx_first_index("AT#CFF?") &&
              mh_tx_count_exact("AT+CCFC=0,2,,,1") == 0u,
          "re-registration keeps voicemail ahead of local forwarding flags");
}

static void test_operator_name_fallbacks(void) {
    const char *sim_names[] = {"US Mobile", "", "US Mobile", "US Mobile", "US Mobile", "US Mobile"};
    const char *expected[] = {"US Mobile", "00101", "00101", "00101", "00101", "00101"};
    char duplicate[128];
    for (unsigned i = 0u; i < 6u; i++) {
        begin_telit(true);
        s_rfsts_response = UNKNOWN_PLMN_RFSTS;
        set_sim_provider_fixture(sim_names[i]);
        if (i == 2u) s_sim_provider_response = "+CRSM: 144,0,\"0055\"";
        if (i == 3u) s_sim_provider_error = true;
        if (i == 4u) s_sim_provider_response = NULL;
        if (i == 5u) {
            snprintf(duplicate, sizeof(duplicate), "%s\r\n%s",
                     s_sim_provider_fixture, s_sim_provider_fixture);
            s_sim_provider_response = duplicate;
        }
        check(boot_until_ready(30000u), "SIM-name fallback fixture boots");
        mh_feed("+CEREG: 1");
        mh_advance(1000u);
        modem_status_t status = mh_status();
        check(strcmp(status.operator_name, expected[i]) == 0 &&
                  status.operator_name_source == (i == 0u
                      ? MODEM_OPERATOR_NAME_SIM : MODEM_OPERATOR_NAME_PLMN) &&
                  strcmp(status.signal.mcc, "001") == 0 &&
                  strcmp(status.signal.mnc, "01") == 0 &&
                  mh_tx_count_exact("AT+COPS?") == 0u &&
                  mh_tx_count_exact(g_modem_vendor.sim_provider_query.query_cmd) == 1u,
              "unknown PLMN uses only validated SIM name or numeric digits");

        modem_service_set_signal_sampling(true, s_mh_now);
        mh_advance(1001u);
        mh_advance(1001u);
        check(mh_tx_count_exact(g_modem_vendor.sim_provider_query.query_cmd) == 1u,
              "foreground signal sampling cannot repeatedly read the SIM name");
        if (i >= 2u) {
            mh_advance(service_probe().sim_provider_retry_ms);
            mh_advance(100u);
            check(mh_tx_count_exact(g_modem_vendor.sim_provider_query.query_cmd) == 2u,
                  "failed SIM name gets one delayed retry");
            mh_advance(service_probe().sim_provider_retry_ms * 2u);
            check(mh_tx_count_exact(g_modem_vendor.sim_provider_query.query_cmd) == 2u &&
                      strcmp(mh_status().operator_name, "00101") == 0,
                  "persistent SIM read failure keeps numeric name without a polling loop");
        } else {
            mh_advance(service_probe().sim_provider_retry_ms * 2u);
            check(mh_tx_count_exact(g_modem_vendor.sim_provider_query.query_cmd) == 1u &&
                      strcmp(mh_status().operator_name, expected[i]) == 0,
                  "valid SIM name or empty field stays cached across background refreshes");
        }
    }

    begin_telit(true);
    s_rfsts_response =
        "#RFSTS: \"001 001\",5230,-101,-82,-11.5,00AF,,32,3,1,"
        "ABCDEF01,\"310410123456789\",\"Network Alias\",2,12,100";
    set_sim_provider_fixture("");
    check(boot_until_ready(30000u), "three-digit MNC fallback fixture boots");
    mh_feed("+CEREG: 1");
    mh_advance(1000u);
    check(strcmp(mh_status().operator_name, "001001") == 0,
          "numeric fallback preserves a three-digit MNC and every leading zero");
}

static void test_operator_name_registration_and_sim_lifetimes(void) {
    begin_telit(true);
    s_rfsts_response = UNKNOWN_PLMN_RFSTS;
    set_sim_provider_fixture("SIM Brand");
    check(boot_until_ready(30000u), "operator lifetime fixture boots");
    mh_feed("+CEREG: 1");
    mh_advance(1000u);
    check(strcmp(mh_status().operator_name, "SIM Brand") == 0,
          "unknown network initially uses cached SIM provider");
    s_rfsts_response = NULL;
    modem_service_set_signal_sampling(true, s_mh_now);
    mh_advance(1001u);
    check(strcmp(mh_status().operator_name, "AT&T") == 0 &&
              mh_status().operator_name_source == MODEM_OPERATOR_NAME_DATABASE &&
              mh_tx_count_exact(g_modem_vendor.sim_provider_query.query_cmd) == 1u,
          "a new known serving PLMN immediately outranks the cached SIM brand");
    mh_feed("+CEREG: 0");
    check(mh_status().operator_name[0] == '\0' &&
              mh_status().operator_name_source == MODEM_OPERATOR_NAME_NONE,
          "loss of registration immediately clears the old operator name");

    s_hold_final_command = "AT#RFSTS";
    mh_feed("+CEREG: 1");
    mh_advance(2u);
    check(service_probe().command_active,
          "serving-cell query can be held across a roaming edge");
    mh_feed(UNKNOWN_PLMN_RFSTS);
    mh_feed("+CEREG: 5");
    mh_feed("OK");
    check(mh_status().operator_name[0] == '\0',
          "a late serving-cell final cannot resurrect a pre-roaming identity");
    mh_feed(UNKNOWN_PLMN_RFSTS);
    s_hold_final_command = NULL;
    mh_feed("OK");
    mh_advance(100u);
    check(strcmp(mh_status().operator_name, "SIM Brand") == 0,
          "the new registration can reuse this SIM provider after a fresh PLMN sample");

    begin_telit(true);
    s_rfsts_response = UNKNOWN_PLMN_RFSTS;
    s_hold_final_command = g_modem_vendor.sim_provider_query.query_cmd;
    check(boot_until_ready(30000u), "in-flight SIM replacement fixture boots");
    mh_feed("+CEREG: 1");
    mh_advance(100u);
    check(mh_tx_count_exact(g_modem_vendor.sim_provider_query.query_cmd) == 1u,
          "unknown network has an in-flight SIM provider read");
    mh_feed("#QSS: 0,0");
    check(mh_status().operator_name[0] == '\0',
          "removing the SIM clears both the name and registered identity");
    set_sim_provider_fixture("Old SIM");
    mh_feed(s_sim_provider_response);
    mh_feed("OK");
    check(mh_status().operator_name[0] == '\0',
          "the removed SIM's delayed name response is discarded");
    set_sim_provider_fixture("New SIM");
    s_hold_final_command = NULL;
    mh_feed("#QSS: 0,3");
    for (unsigned i = 0u; i < 300u &&
         strcmp(mh_status().operator_name, "New SIM") != 0; i++) {
        mh_advance(100u);
    }
    check(strcmp(mh_status().operator_name, "New SIM") == 0 &&
              mh_tx_count_exact(g_modem_vendor.sim_provider_query.query_cmd) == 2u,
          "SIM completion reads the new provider rather than retaining the old cache");

    mh_clear_tx_capture();
    s_mh_supply_pg = false;
    mh_advance(250u);
    check(service_probe().state == MODEM_SERVICE_TEST_STATE_FAILED &&
              mh_status().operator_name[0] == '\0',
          "a retained-module fault immediately retires the old network identity");
    s_mh_supply_pg = true;
    set_sim_provider_fixture("Recovered SIM");
    check(boot_until_ready(30000u), "operator cache fixture recovers retained modem");
    mh_feed("+CEREG: 1");
    mh_advance(1000u);
    check(strcmp(mh_status().operator_name, "Recovered SIM") == 0 &&
              mh_tx_count_exact(g_modem_vendor.sim_provider_query.query_cmd) == 1u,
          "recovery rereads the SIM name even without a not-ready SIM indication");
}

static void test_local_forwarding_flags_without_network_fallback(void) {
    begin_telit(true);
    s_cff_flags_absent = true;
    check(boot_until_ready(30000u), "missing local-flags fixture boots");
    settle_dtr_sleep();
    check(!mh_status().call_forward_unconditional_known &&
              mh_tx_count_exact("AT+CCFC=0,2,,,1") == 0u,
          "absent SIM flags stay unknown without an automatic network query");
    mh_advance(300000u);
    check(mh_tx_count_exact("AT+CCFC=0,2,,,1") == 0u,
          "background refresh never falls back to a network SS transaction");

    mh_feed("#CFF: 1,1,+15551234567");
    check(mh_status().call_forward_unconditional_known &&
              mh_status().call_forward_unconditional_active,
          "a real forwarding indication still updates standby");
    mh_feed("+CEREG: 0");
    mh_feed("+CEREG: 1");
    mh_settle();
    check(!mh_status().call_forward_unconditional_known,
          "a later valid flags-absent snapshot does not claim a stale state");

    s_hold_final_command = "AT#CFF?";
    modem_supplementary_refresh_arm(MODEM_SUPPLEMENTARY_REFRESH_CFU, s_mh_now);
    mh_settle();
    mh_feed("#CFF: 1,1,+15551234567");
    mh_feed("#CFF: 1");
    s_hold_final_command = NULL;
    mh_feed("OK");
    check(mh_status().call_forward_unconditional_known &&
              mh_status().call_forward_unconditional_active,
          "newer CFU evidence survives a flags-absent query final");
}

static void test_supplementary_refresh_recovery(void) {
    begin_telit(true);
    s_fault = TELIT_FAULT_MBN_ERROR_ALWAYS;
    check(boot_until_ready(30000u),
          "mailbox retry fixture boots through an unavailable SIM row");
    mh_settle();
    char mailbox[MODEM_PHONE_MAX + 1u];
    check(mh_tx_count_exact("AT#MBN") == 1u &&
              !modem_service_get_voice_mailbox_number(mailbox,
                                                       sizeof(mailbox)),
          "first failed mailbox read stays unknown");
    mh_advance(MODEM_SUPPLEMENTARY_REFRESH_RETRY_MS + 1u);
    check(mh_tx_count_exact("AT#MBN") == 2u,
          "mailbox read performs exactly one timed retry");
    s_fault = TELIT_FAULT_NONE;
    mh_feed("#QSS: 3");
    mh_settle();
    check(mh_tx_count_exact("AT#MBN") == 3u &&
              modem_service_get_voice_mailbox_number(mailbox,
                                                      sizeof(mailbox)) &&
              strcmp(mailbox, "+18005551212") == 0,
          "later QSS READY repairs a mailbox read after retry exhaustion");

    begin_telit(true);
    s_message_waiting.category[MODEM_MESSAGE_WAITING_VOICE_LINE_1] =
        (modem_message_waiting_state_t){.active = true, .count = 6u};
    s_fault = TELIT_FAULT_MWI_ERROR_ONCE;
    check(boot_until_ready(30000u), "MWI retry fixture boots");
    mh_settle();
    check(mh_tx_count_exact("AT#MWI?") == 1u &&
              !mh_status().message_waiting
                   .category[MODEM_MESSAGE_WAITING_VOICE_LINE_1].active,
          "failed MWI snapshot is not treated as an authoritative clear");
    mh_advance(MODEM_SUPPLEMENTARY_REFRESH_RETRY_MS + 1u);
    check(mh_tx_count_exact("AT#MWI?") == 2u &&
              mh_status().message_waiting
                  .category[MODEM_MESSAGE_WAITING_VOICE_LINE_1].active &&
              mh_status().message_waiting
                      .category[MODEM_MESSAGE_WAITING_VOICE_LINE_1].count ==
                  6u,
          "bounded MWI retry recovers the SIM-stored indicator");

    begin_telit(true);
    check(boot_until_ready(30000u), "CFU retry fixture boots");
    s_call_forward_active[CALL_FORWARD_REASON_UNCONDITIONAL] = true;
    snprintf(s_call_forward_number[CALL_FORWARD_REASON_UNCONDITIONAL],
             sizeof(s_call_forward_number[0]), "+15550000021");
    size_t flags_before = mh_tx_count_exact("AT#CFF?");
    s_fault = TELIT_FAULT_CFU_FLAGS_ERROR_ONCE;
    mh_feed("+CEREG: 1");
    mh_settle();
    check(mh_tx_count_exact("AT#CFF?") == flags_before + 1u &&
              !mh_status().call_forward_unconditional_known,
          "failed registration-edge flags read remains unknown");
    mh_advance(MODEM_SUPPLEMENTARY_REFRESH_RETRY_MS + 1u);
    modem_status_t status = mh_status();
    check(mh_tx_count_exact("AT#CFF?") == flags_before + 2u &&
              status.call_forward_unconditional_known &&
              status.call_forward_unconditional_active,
          "bounded local-flags retry restores the indicator");

    call_forward_request_t request;
    memset(&request, 0, sizeof(request));
    request.reason = CALL_FORWARD_REASON_UNCONDITIONAL;
    request.action = CALL_FORWARD_ACTION_DISABLE;
    uint32_t request_id = 0u;
    size_t queries_before = mh_tx_count_exact("AT#CFF?");
    s_fault = TELIT_FAULT_CFU_FLAGS_ERROR_ONCE;
    s_fault_consumed = false;
    check(modem_service_request_call_forward(&request, &request_id),
          "CFU mutation with failed verification is admitted");
    mh_settle();
    call_forward_result_t result;
    check(modem_service_pop_call_forward_result(&result) &&
              result.request_id == request_id &&
              result.outcome == CALL_FORWARD_OUTCOME_SUCCESS &&
              !mh_status().call_forward_unconditional_known,
          "mutation success invalidates the old icon until local flags refresh");
    mh_advance(MODEM_SUPPLEMENTARY_REFRESH_RETRY_MS + 1u);
    status = mh_status();
    check(mh_tx_count_exact("AT#CFF?") == queries_before + 2u &&
              status.call_forward_unconditional_known &&
              !status.call_forward_unconditional_active,
          "failed post-mutation verification retries and clears the icon");
}

static void test_call_forward_queue_cancellation(void) {
    begin_telit(true);
    check(boot_until_ready(30000u), "queued cancellation fixture boots");
    mh_feed("+CEREG: 1");
    mh_settle();

    call_forward_request_t request;
    memset(&request, 0, sizeof(request));
    request.reason = CALL_FORWARD_REASON_BUSY;
    request.action = CALL_FORWARD_ACTION_QUERY;
    uint32_t request_id = 0u;
    size_t commands_before = mh_tx_count_exact("AT+CCFC=1,2,,,1");
    check(modem_service_request_debug_at("AT") &&
              modem_service_request_call_forward(&request, &request_id) &&
              request_id != 0u &&
              modem_service_cancel_queued_call_forward(request_id),
          "Quit can remove a supplementary request still inside the FIFO");
    mh_settle();
    call_forward_result_t result;
    check(mh_tx_count_exact("AT+CCFC=1,2,,,1") == commands_before &&
              !modem_service_pop_call_forward_result(&result),
          "cancelled queued request never reaches UART or publishes a result");

    s_fault = TELIT_FAULT_CALL_FORWARD_TIMEOUT_ONCE;
    s_fault_consumed = false;
    check(modem_service_request_call_forward(&request, &request_id),
          "in-flight cancellation fixture is admitted");
    mh_settle();
    modem_service_test_snapshot_t probe = service_probe();
    check(probe.command_active &&
              probe.active_command ==
                  MODEM_SERVICE_TEST_COMMAND_CALL_FORWARD &&
              !modem_service_cancel_queued_call_forward(request_id),
          "queue cancellation cannot mislabel an on-wire request as aborted");
    modem_service_power_off();
    check(modem_service_pop_call_forward_result(&result) &&
              result.request_id == request_id &&
              result.outcome == CALL_FORWARD_OUTCOME_CANCELLED,
          "ordinary session cancellation still owns an in-flight query");
}

static void test_newer_call_forward_result_survives_old_final(void) {
    begin_telit(true);
    check(boot_until_ready(30000u),
          "call-forward result-order fixture boots");
    mh_feed("+CEREG: 1");
    mh_settle();

    call_forward_request_t old_request;
    memset(&old_request, 0, sizeof(old_request));
    old_request.reason = CALL_FORWARD_REASON_BUSY;
    old_request.action = CALL_FORWARD_ACTION_QUERY;
    uint32_t old_id = 0u;
    s_fault = TELIT_FAULT_CALL_FORWARD_TIMEOUT_ONCE;
    s_fault_consumed = false;
    check(modem_service_request_call_forward(&old_request, &old_id),
          "detached old status request is admitted");
    mh_settle();
    modem_service_test_snapshot_t probe = service_probe();
    check(probe.command_active &&
              probe.active_command ==
                  MODEM_SERVICE_TEST_COMMAND_CALL_FORWARD,
          "old status request remains on wire without a final");

    /* Model the UI having Quit the old request before service is lost and a
     * newer request receives an immediate local result. */
    mh_feed("+CEREG: 0");
    call_forward_request_t new_request;
    memset(&new_request, 0, sizeof(new_request));
    new_request.reason = CALL_FORWARD_REASON_NOT_REACHABLE;
    new_request.action = CALL_FORWARD_ACTION_QUERY;
    uint32_t new_id = 0u;
    check(modem_service_request_call_forward(&new_request, &new_id) &&
              new_id != old_id,
          "new no-service request receives a distinct identity");

    mh_advance(g_modem_vendor.supplementary.command_timeout_ms + 1u);
    call_forward_result_t result;
    check(modem_service_pop_call_forward_result(&result) &&
              result.request_id == new_id &&
              result.outcome == CALL_FORWARD_OUTCOME_NO_NETWORK,
          "older late final cannot overwrite the visible newer result");
    check(!modem_service_pop_call_forward_result(&result),
          "stale old completion is discarded instead of leaking later");
}

static void test_phonebook_list_and_crud_contract(void) {
    begin_telit(true);
    check(boot_until_ready(30000u), "phonebook fixture boots");
    check(!modem_service_request_phonebook_list(NULL),
          "phonebook admission requires a request-id owner");

    s_phonebook[0].used = true;
    s_phonebook[0].entry.index = 1u;
    strcpy(s_phonebook[0].entry.name, "Ada");
    strcpy(s_phonebook[0].entry.number, "+15550000001");
    s_phonebook[2].used = true;
    s_phonebook[2].entry.index = 3u;
    strcpy(s_phonebook[2].entry.name, "Bob");
    strcpy(s_phonebook[2].entry.number, "5550003");
    s_phonebook_inject_malformed = true;

    uint32_t request_id = 0u;
    check(modem_service_request_phonebook_list(&request_id) &&
              request_id != 0u,
          "phonebook list request is admitted");
    mh_settle();
    modem_phonebook_result_t result;
    modem_phonebook_entry_t entry;
    check(modem_service_pop_phonebook_result(&result) &&
              result.request_id == request_id &&
              result.kind == MODEM_PHONEBOOK_OP_LIST &&
              result.outcome == MODEM_PHONEBOOK_OUTCOME_ERROR &&
              !result.sim_not_ready &&
              modem_service_phonebook_count() == 0u,
          "malformed CPBR row fails closed without publishing partial contacts");

    s_phonebook_inject_malformed = false;
    s_phonebook_inject_oversized = true;
    s_phonebook_inject_high_index = true;
    request_id = 0u;
    check(modem_service_request_phonebook_list(&request_id) &&
              request_id != 0u,
          "clean full-domain phonebook list is admitted");
    mh_settle();
    check(modem_service_pop_phonebook_result(&result) &&
              result.request_id == request_id &&
              result.kind == MODEM_PHONEBOOK_OP_LIST &&
              result.outcome == MODEM_PHONEBOOK_OUTCOME_OK &&
              !result.sim_not_ready &&
              modem_service_phonebook_count() == 4u &&
              modem_service_phonebook_entry(0u, &entry) &&
              entry.index == 1u && strcmp(entry.name, "Ada") == 0 &&
              strcmp(entry.number, "+15550000001") == 0,
          "clean CPBR final atomically publishes normalized modem order");
    check(modem_service_phonebook_entry(2u, &entry) &&
              entry.index == 250u &&
              strlen(entry.number) == MODEM_PHONE_MAX &&
              strlen(entry.name) == MODEM_PHONEBOOK_NAME_MAX,
          "oversized fields retain the established bounded previews");
    check(modem_service_phonebook_entry(3u, &entry) &&
              entry.index == MODEM_PHONEBOOK_LAST_INDEX &&
              strcmp(entry.name, "Last contact") == 0 &&
              !modem_service_phonebook_entry(4u, &entry),
          "the complete 1..500 ME index domain survives service publication");

    s_phonebook_inject_oversized = false;
    s_phonebook_inject_high_index = false;
    request_id = 0u;
    check(modem_service_request_phonebook_add(
              "Alice", "+15550000002", &request_id) &&
              request_id != 0u,
          "international add is admitted");
    mh_settle();
    check(mh_tx_count_exact(
              "AT+CPBW=,\"+15550000002\",145,\"Alice\"") == 1u &&
              modem_service_pop_phonebook_result(&result) &&
              result.request_id == request_id &&
              result.kind == MODEM_PHONEBOOK_OP_ADD &&
              result.outcome == MODEM_PHONEBOOK_OUTCOME_OK &&
              modem_service_phonebook_count() == 3u,
          "add uses international TOA then refreshes the complete cache");
    check(modem_service_phonebook_entry(1u, &entry) && entry.index == 2u &&
              strcmp(entry.name, "Alice") == 0,
          "post-add readback publishes the modem-assigned index");

    request_id = 0u;
    check(modem_service_request_phonebook_update(
              3u, "Robert", "5550103", &request_id) &&
              request_id != 0u,
          "national update is admitted");
    mh_settle();
    check(mh_tx_count_exact(
              "AT+CPBW=3,\"5550103\",129,\"Robert\"") == 1u &&
              modem_service_pop_phonebook_result(&result) &&
              result.request_id == request_id &&
              result.kind == MODEM_PHONEBOOK_OP_UPDATE &&
              result.outcome == MODEM_PHONEBOOK_OUTCOME_OK &&
              modem_service_phonebook_entry(2u, &entry) &&
              entry.index == 3u && strcmp(entry.name, "Robert") == 0 &&
              strcmp(entry.number, "5550103") == 0,
          "update uses national TOA and returns refreshed contents");

    request_id = 0u;
    check(modem_service_request_phonebook_delete(1u, &request_id) &&
              request_id != 0u,
          "delete is admitted");
    mh_settle();
    check(mh_tx_count_exact("AT+CPBW=1") == 1u &&
              modem_service_pop_phonebook_result(&result) &&
              result.request_id == request_id &&
              result.kind == MODEM_PHONEBOOK_OP_DELETE &&
              result.outcome == MODEM_PHONEBOOK_OUTCOME_OK &&
              modem_service_phonebook_count() == 2u &&
              modem_service_phonebook_entry(0u, &entry) && entry.index == 2u,
          "delete command removes the row and refreshes cache order");

    check(modem_service_phonebook_count() != 0u &&
              modem_service_phonebook_cache_valid(),
          "phonebook fixture has a published cache before SIM removal");
    mh_feed("#QSS: 2,0");
    s_fault = TELIT_FAULT_PHONEBOOK_CPBS_ERROR_ONCE;
    s_fault_consumed = false;
    request_id = 0u;
    check(modem_service_request_phonebook_list(&request_id) &&
              request_id != 0u,
          "missing-SIM list still reaches the modem error path");
    mh_settle();
    check(modem_service_pop_phonebook_result(&result) &&
              result.request_id == request_id &&
              result.kind == MODEM_PHONEBOOK_OP_LIST &&
              result.outcome == MODEM_PHONEBOOK_OUTCOME_ERROR &&
              result.sim_not_ready &&
              modem_service_phonebook_count() == 0u &&
              !modem_service_phonebook_cache_valid(),
          "CPBS failure reports missing SIM and retires its stale cache");
}

static bool request_phonebook_operation(modem_phonebook_op_t operation,
                                        uint32_t *request_id_out) {
    switch (operation) {
    case MODEM_PHONEBOOK_OP_LIST:
        return modem_service_request_phonebook_list(request_id_out);
    case MODEM_PHONEBOOK_OP_ADD:
        return modem_service_request_phonebook_add(
            "Fault add", "+15550000999", request_id_out);
    case MODEM_PHONEBOOK_OP_UPDATE:
        return modem_service_request_phonebook_update(
            1u, "Fault update", "5550999", request_id_out);
    case MODEM_PHONEBOOK_OP_DELETE:
        return modem_service_request_phonebook_delete(1u, request_id_out);
    case MODEM_PHONEBOOK_OP_NONE:
    default:
        return false;
    }
}

static void run_phonebook_fault_case(
    modem_phonebook_op_t operation, telit_fault_t fault,
    uint32_t timeout_advance_ms, modem_phonebook_outcome_t expected_outcome,
    const char *message) {
    begin_telit(true);
    if (!boot_until_ready(30000u)) {
        check(false, message);
        return;
    }
    s_phonebook[0].used = true;
    s_phonebook[0].entry.index = 1u;
    strcpy(s_phonebook[0].entry.name, "Seed");
    strcpy(s_phonebook[0].entry.number, "5550001");
    s_fault = fault;
    s_fault_consumed = false;

    uint32_t request_id = 0u;
    bool admitted = request_phonebook_operation(operation, &request_id);
    mh_settle();
    if (timeout_advance_ms != 0u) {
        mh_advance(timeout_advance_ms);
    }
    modem_phonebook_result_t result;
    bool terminal = admitted && request_id != 0u &&
        modem_service_pop_phonebook_result(&result) &&
        result.request_id == request_id && result.kind == operation &&
        result.outcome == expected_outcome && !result.sim_not_ready &&
        !mh_status().operation_busy &&
        !modem_service_pop_phonebook_result(&result);
    check(terminal, message);
}

static void test_phonebook_operation_fault_matrix(void) {
    static const modem_phonebook_op_t operations[] = {
        MODEM_PHONEBOOK_OP_LIST,
        MODEM_PHONEBOOK_OP_ADD,
        MODEM_PHONEBOOK_OP_UPDATE,
        MODEM_PHONEBOOK_OP_DELETE,
    };
    for (size_t i = 0u; i < sizeof(operations) / sizeof(operations[0]); i++) {
        run_phonebook_fault_case(
            operations[i], TELIT_FAULT_PHONEBOOK_CPBS_ERROR_ONCE, 0u,
            MODEM_PHONEBOOK_OUTCOME_ERROR,
            "every operation terminates once on CPBS ERROR");
        run_phonebook_fault_case(
            operations[i], TELIT_FAULT_PHONEBOOK_CPBS_TIMEOUT_ONCE, 5001u,
            MODEM_PHONEBOOK_OUTCOME_TIMEOUT,
            "every operation terminates once on CPBS timeout");
        run_phonebook_fault_case(
            operations[i], TELIT_FAULT_PHONEBOOK_CPBR_ERROR_ONCE, 0u,
            MODEM_PHONEBOOK_OUTCOME_ERROR,
            "every operation terminates once on readback ERROR");
        run_phonebook_fault_case(
            operations[i], TELIT_FAULT_PHONEBOOK_CPBR_TIMEOUT_ONCE, 15001u,
            MODEM_PHONEBOOK_OUTCOME_TIMEOUT,
            "every operation terminates once on readback timeout");
    }

    static const modem_phonebook_op_t writes[] = {
        MODEM_PHONEBOOK_OP_ADD,
        MODEM_PHONEBOOK_OP_UPDATE,
        MODEM_PHONEBOOK_OP_DELETE,
    };
    for (size_t i = 0u; i < sizeof(writes) / sizeof(writes[0]); i++) {
        run_phonebook_fault_case(
            writes[i], TELIT_FAULT_PHONEBOOK_CPBW_ERROR_ONCE, 0u,
            MODEM_PHONEBOOK_OUTCOME_ERROR,
            "every write terminates once on CPBW ERROR");
        run_phonebook_fault_case(
            writes[i], TELIT_FAULT_PHONEBOOK_CPBW_TIMEOUT_ONCE, 10001u,
            MODEM_PHONEBOOK_OUTCOME_TIMEOUT,
            "every write terminates once on CPBW timeout");
    }
}

static void test_phonebook_queue_eviction_and_recovery_are_terminal(void) {
    begin_telit(true);
    check(boot_until_ready(30000u),
          "phonebook eviction/recovery fixture boots");
    s_fault = TELIT_FAULT_PHONEBOOK_CPBR_TIMEOUT_ONCE;
    s_fault_consumed = false;

    uint32_t request_ids[7];
    memset(request_ids, 0, sizeof(request_ids));
    check(modem_service_request_phonebook_list(&request_ids[0]),
          "phonebook current request is admitted");
    mh_settle();
    check(service_probe().command_active && mh_status().operation_busy,
          "phonebook readback remains on wire for queue-pressure fixture");
    bool queue_filled = true;
    for (uint8_t i = 1u; i < 7u; i++) {
        queue_filled = queue_filled &&
            modem_service_request_phonebook_list(&request_ids[i]);
    }
    uint32_t rejected_id = 99u;
    check(queue_filled &&
              !modem_service_request_phonebook_list(&rejected_id) &&
              rejected_id == 0u &&
              service_probe().request_queue_depth == 6u,
          "full FIFO rejects an unaccepted phonebook request without a token");

    mh_feed("RING");
    mh_feed("+CLIP: \"+15550000000\",145,,,,0");
    check(modem_service_request_answer(),
          "incoming call control preempts a full normal FIFO");
    modem_phonebook_result_t result;
    check(modem_service_pop_phonebook_result(&result) &&
              result.request_id == request_ids[6] &&
              result.kind == MODEM_PHONEBOOK_OP_LIST &&
              result.outcome == MODEM_PHONEBOOK_OUTCOME_EVICTED,
          "the exact displaced phonebook request terminates as EVICTED");

    s_mh_supply_pg = false;
    mh_advance(250u);
    bool cancelled_once[6] = {false};
    uint8_t cancelled_count = 0u;
    while (modem_service_pop_phonebook_result(&result)) {
        for (uint8_t i = 0u; i < 6u; i++) {
            if (result.request_id == request_ids[i] &&
                result.kind == MODEM_PHONEBOOK_OP_LIST &&
                result.outcome == MODEM_PHONEBOOK_OUTCOME_CANCELLED &&
                !cancelled_once[i]) {
                cancelled_once[i] = true;
                cancelled_count++;
                break;
            }
        }
    }
    check(service_probe().state == MODEM_SERVICE_TEST_STATE_FAILED &&
              cancelled_count == 6u &&
              modem_service_phonebook_count() == 0u &&
              !mh_status().operation_busy,
          "runtime recovery cancels current and queued phonebook requests exactly once");
}


/* ---- direct delivery (+CMT) -------------------------------------------- */

static const char DIRECT_3GPP2_HEADER[] =
    "+CMT: \"7866910488\",\"\",\"20260916105524\",129,4098,0,8,9";
static const char DIRECT_3GPP2_BODY[] = "Dhdjdjdjs";
static const char DIRECT_CPMS_SET[] = "AT+CPMS=\"ME\",\"ME\",\"ME\"";

/* The module emits a +CMT as one atomic header+payload unit: nothing can land
 * between the two lines on the wire, so the fixture queues both before the
 * service runs (a settle between them would let the auto-responder interleave
 * a background command's row as the payload, which hardware cannot do). */
static void feed_direct(const char *header, const char *payload) {
    mh_rx_push(header);
    mh_rx_push(payload);
    mh_settle();
}

/* Golden: the same vendor hook + codec the service uses, run independently. */
static bool build_expected_direct_pdu(const char *header, const char *payload,
                                      char *pdu, uint8_t *tpdu_len) {
    sms_deliver_t deliver;
    return g_modem_vendor.translate_direct_sms != NULL &&
           g_modem_vendor.translate_direct_sms(header, (const uint8_t *)payload,
                                               strlen(payload), &deliver) ==
               MODEM_SMS_DIRECT_ACCEPTED &&
           sms_deliver_build(&deliver, pdu, SMS_DELIVER_HEX_MAX, tpdu_len);
}

/* Wire bytes exactly as the module emits them: header CRLF body CRLF, queued
 * as one unit before the service runs. The body is a byte range so it can
 * carry CR, LF and 0x00. */
static void feed_direct_wire(const char *header, const uint8_t *body,
                             size_t body_len) {
    mh_rx_push(header);
    mh_rx_push_raw(body, body_len);
    mh_rx_push_raw((const uint8_t *)"\r\n", 2u);
    mh_settle();
}

/* The PDU hex the service wrote after its last AT+CMGW= (the raw event that
 * follows the command; the ^Z is a separate one-byte event). */
static bool captured_stored_pdu(char *out, size_t cap) {
    size_t cmgw = SIZE_MAX;
    for (size_t i = 0u; i < s_mh_tx_event_count; i++) {
        const mh_tx_event_t *event = &s_mh_tx_events[i];
        if (event->kind == MH_TX_EVENT_CSTR && event->len >= 8u &&
            memcmp(event->data, "AT+CMGW=", 8u) == 0) {
            cmgw = i;
        }
    }
    if (cmgw == SIZE_MAX) {
        return false;
    }
    for (size_t i = cmgw + 1u; i < s_mh_tx_event_count; i++) {
        const mh_tx_event_t *event = &s_mh_tx_events[i];
        if (event->kind == MH_TX_EVENT_RAW && event->len > 1u &&
            !event->truncated && event->len < cap) {
            memcpy(out, event->data, event->len);
            out[event->len] = '\0';
            return true;
        }
    }
    return false;
}

static void test_direct_delivery_is_restored_as_a_pdu(void) {
    static const uint8_t sub = 0x1au;
    if (!begin_sms_operation_fixture("direct-delivery fixture boots")) {
        return;
    }
    char expected_pdu[SMS_DELIVER_HEX_MAX];
    uint8_t expected_tpdu = 0u;
    check(build_expected_direct_pdu(DIRECT_3GPP2_HEADER, DIRECT_3GPP2_BODY,
                                    expected_pdu, &expected_tpdu),
          "direct-delivery golden builds independently");
    modem_status_t before = mh_status();
    size_t cpms_poll_before = mh_tx_count_exact("AT+CPMS?");

    feed_direct(DIRECT_3GPP2_HEADER, DIRECT_3GPP2_BODY);

    char cmgw[24];
    snprintf(cmgw, sizeof(cmgw), "AT+CMGW=%u,0", (unsigned)expected_tpdu);
    size_t cpms_event = tx_event_cstr(DIRECT_CPMS_SET, 0u);
    size_t pdu_mode = tx_event_cstr("AT+CMGF=0", cpms_event + 1u);
    size_t cmgw_event = tx_event_cstr(cmgw, pdu_mode + 1u);
    size_t pdu_event = tx_event_raw(expected_pdu, strlen(expected_pdu),
                                    cmgw_event + 1u);
    size_t sub_event = tx_event_raw(&sub, 1u, pdu_event + 1u);
    size_t text_mode = tx_event_cstr("AT+CMGF=1", sub_event + 1u);
    check(cpms_event != SIZE_MAX && pdu_mode != SIZE_MAX &&
              cmgw_event != SIZE_MAX && pdu_event != SIZE_MAX &&
              sub_event != SIZE_MAX && text_mode != SIZE_MAX &&
              mh_tx_count_exact("AT+CMGF=1") == 1u,
          "a +CMT is re-stored as an SMS-DELIVER PDU in exact wire order");
    modem_status_t after = mh_status();
    check(after.sms_received_count == before.sms_received_count + 1u &&
              after.sms_user_received_count == before.sms_user_received_count &&
              after.command_errors == before.command_errors,
          "a stored delivery counts like a +CMTI (classified by the scan)");
    check(mh_tx_count_exact("AT+CPMS?") == cpms_poll_before + 1u,
          "a stored delivery schedules the +CMTI receive-store check");
    check(!after.operation_busy && !service_probe().command_active &&
              service_probe().request_queue_depth == 0u,
          "the internal store leaves no request or operation behind");

    /* The ring slot was released: a second delivery stores again. */
    mh_clear_tx_capture();
    feed_direct(DIRECT_3GPP2_HEADER, DIRECT_3GPP2_BODY);
    check(mh_tx_count_exact(cmgw) == 1u &&
              mh_status().sms_received_count == before.sms_received_count + 2u,
          "a released ring slot stores the next delivery");
}

static void test_slow_call_forwarding_keeps_receiving(void) {
    for (unsigned reject = 0u; reject < 2u; reject++) {
        begin_telit(true);
        check(boot_until_ready(30000u), "slow forwarding fixture boots");
        settle_dtr_sleep();
        mh_clear_tx_capture();
        modem_status_t before = mh_status();
        call_forward_request_t request = {0};
        request.reason = CALL_FORWARD_REASON_BUSY;
        request.action = CALL_FORWARD_ACTION_QUERY;
        uint32_t request_id = 0u;
        s_hold_final_command = "AT+CCFC=1,2,,,1";
        check(modem_service_request_call_forward(&request, &request_id),
              "explicit network forwarding query is admitted");
        mh_advance(1u);
        mh_advance(1u);
        check(modem_service_request_debug_at("AT+CGMM"),
              "ordinary work queues behind the network request");
        mh_advance(16000u);
        check(service_probe().command_active &&
                  service_probe().active_command == MODEM_SERVICE_TEST_COMMAND_CALL_FORWARD &&
                  mh_tx_count_exact("AT+CGMM") == 0u,
              "network request still owns UART beyond the old 15-second deadline");

        feed_direct("+CMT: \"15551230000\",\"\",\"20260916210000\",129,4098,0,8,2", "OK");
        feed_direct("+CMT: \"15551230000\",\"\",\"20260916210001\",129,4098,0,8,7", "Another");
        mh_feed("RING");
        mh_feed("+CLIP: \"+15557654321\",145,,,,0");
        mh_feed("#ECAM: 0,6,1,,,");
        check(mh_status().ring_active &&
                  strcmp(mh_status().incoming_number, "+15557654321") == 0 &&
                  service_probe().active_command == MODEM_SERVICE_TEST_COMMAND_CALL_FORWARD &&
                  mh_tx_count_exact(DIRECT_CPMS_SET) == 0u,
              "call indication and SMS bodies are received without completing the pending query");
        mh_advance(14000u);
        check(service_probe().active_command == MODEM_SERVICE_TEST_COMMAND_CALL_FORWARD &&
                  mh_status().ring_active && mh_tx_count_exact("AT+CGMM") == 0u,
              "30-second network wait preserves the incoming call and queued work");
        check(modem_service_request_answer(), "answer queues for the incoming call");
        s_clcc_row = "+CLCC: 1,1,0,0,0,\"15557654321\",145";
        s_hold_final_command = NULL;
        if (!reject) {
            mh_feed("+CCFC: 0,1");
        }
        mh_feed(reject ? "+CME ERROR: network rejected request" : "OK");
        for (unsigned i = 0u; i < 5u; i++) mh_advance(100u);
        call_forward_result_t result;
        check(modem_service_pop_call_forward_result(&result) &&
                  result.request_id == request_id &&
                  result.outcome == (reject ? CALL_FORWARD_OUTCOME_NOT_DONE :
                                             CALL_FORWARD_OUTCOME_SUCCESS),
              "late network final completes only its original forwarding request");
        check(tx_first_index("ATA") < tx_first_index("AT+CGMM") &&
                  mh_status().call_state == MODEM_CALL_ACTIVE,
              "queued answer takes priority as soon as the network request finishes");
        check(mh_status().sms_received_count == before.sms_received_count + 2u,
              "both SMS received during the wait are stored after the answer");
        char expected[SMS_DELIVER_HEX_MAX];
        char stored[SMS_DELIVER_HEX_MAX];
        uint8_t tpdu = 0u;
        check(build_expected_direct_pdu(
                  "+CMT: \"15551230000\",\"\",\"20260916210001\",129,4098,0,8,7",
                  "Another", expected, &tpdu) &&
                  captured_stored_pdu(stored, sizeof(stored)) &&
                  strcmp(stored, expected) == 0,
              "delayed SMS storage preserves the exact received payload");
        modem_debug_result_t debug;
        check(modem_service_pop_debug_result(&debug) && debug.ok,
              "the next AT transaction is not poisoned by the delayed forwarding final");
    }
}

static void test_direct_delivery_mid_command_and_qcmti(void) {
    if (!begin_sms_operation_fixture(
            "direct-delivery mid-command fixture boots")) {
        return;
    }
    modem_status_t before = mh_status();

    s_hold_final_command = "AT+CGMI";
    check(modem_service_request_debug_at("AT+CGMI"),
          "debug command is admitted");
    mh_settle();
    check(service_probe().command_active && mh_status().operation_busy,
          "debug command is on the wire without a final");

    /* A payload that reads like a final must not finish AT+CGMI. The body
     * is read raw by <length> (2), so the header says exactly "OK". */
    feed_direct("+CMT: \"7866910488\",\"\",\"20260916105524\",129,4098,0,8,2",
                "OK");
    check(service_probe().command_active && mh_status().operation_busy &&
              mh_tx_count_exact(DIRECT_CPMS_SET) == 0u,
          "a +CMT payload spelled OK does not complete the active command");

    s_hold_final_command = NULL;
    mh_feed("OK");
    check(!service_probe().command_active &&
              mh_tx_count_exact(DIRECT_CPMS_SET) == 1u &&
              mh_tx_count_exact("AT+CMGF=1") == 1u &&
              mh_status().sms_received_count == before.sms_received_count + 1u,
          "the store runs after the command completes");

    /* A zero-length body never reaches the service as a line (the framer
     * drops empty lines): the header alone must complete the delivery, so the
     * OK that follows still finishes the command it belongs to. */
    s_hold_final_command = "AT+CGMI";
    check(modem_service_request_debug_at("AT+CGMI"),
          "second debug command is admitted");
    mh_settle();
    mh_clear_tx_capture();
    s_hold_final_command = NULL;
    feed_direct("+CMT: \"7866910488\",\"\",\"20260916105524\",129,4098,0,8,0",
                "OK");
    check(!service_probe().command_active && !mh_status().operation_busy &&
              mh_tx_count_exact(DIRECT_CPMS_SET) == 1u &&
              mh_tx_count_exact("AT+CMGW=18,0") == 1u &&
              mh_tx_count_exact("AT+CMGF=1") == 1u &&
              mh_status().sms_received_count == before.sms_received_count + 2u,
          "a length-0 +CMT completes on its header: the OK after it finishes "
          "the command and one empty message is stored");
    before.sms_received_count++;

    /* A header whose body never arrives (wire corruption; the module emits
     * header and body as one unit): the raw reader takes the next line as
     * the body candidate, it does not parse and is far past the plain-body
     * bound for <length> 9, so the delivery is rejected at once; the stray
     * line after it is ignored and the next real delivery stores normally.
     * Nothing is misparsed. */
    modem_status_t lost_before = mh_status();
    mh_clear_tx_capture();
    mh_rx_push(DIRECT_3GPP2_HEADER);
    feed_direct(DIRECT_3GPP2_HEADER, DIRECT_3GPP2_BODY);
    check(mh_tx_count_exact(DIRECT_CPMS_SET) == 0u && !modem_sms_direct_pending() &&
              mh_status().command_errors == lost_before.command_errors + 1u &&
              mh_status().sms_received_count == lost_before.sms_received_count,
          "a header without its body is rejected at the next line (past the plain-body bound)");
    mh_clear_tx_capture();
    feed_direct(DIRECT_3GPP2_HEADER, DIRECT_3GPP2_BODY);
    for (unsigned i = 0u; i < 5u; i++) {
        mh_advance(100u); /* the store may first need the DTR wake */
    }
    check(mh_tx_count_exact("AT+CMGF=0") == 1u &&
              mh_tx_count_exact("AT+CMGW=26,0") == 1u &&
              mh_status().sms_received_count == before.sms_received_count + 2u,
          "the delivery after a corrupted one stores exactly one message");

    /* An unsupported encoding is rejected at its own terminator, without
     * waiting for the body deadline or consuming the next notification. */
    modem_status_t rejected_before = mh_status();
    mh_clear_tx_capture();
    feed_direct("+CMT: \"7866910488\",\"\",\"20260916105524\",129,4098,0,7,9",
                DIRECT_3GPP2_BODY);
    check(!modem_sms_direct_pending() &&
              mh_status().command_errors == rejected_before.command_errors + 1u &&
              mh_status().sms_received_count ==
                  rejected_before.sms_received_count &&
              mh_tx_count_exact("AT+CMGF=0") == 0u,
          "an unparseable +CMT is counted as an error and not stored");

    /* $QCMTI: a store this backend cannot read. Counted, never read. */
    modem_status_t qcmti_before = mh_status();
    mh_feed("$QCMTI: \"ME\",24");
    modem_status_t qcmti_after = mh_status();
    check(qcmti_after.command_errors == qcmti_before.command_errors + 1u &&
              qcmti_after.urc_count == qcmti_before.urc_count + 1u &&
              qcmti_after.sms_received_count ==
                  qcmti_before.sms_received_count &&
              mh_tx_count_exact("AT+CMGR=24") == 0u &&
              mh_tx_count_exact("AT+CMGF=0") == 0u,
          "$QCMTI is recognised, counted as an error, and never read");
}

static void test_direct_delivery_retries_after_store_failure(void) {
    if (!begin_sms_operation_fixture("direct-delivery retry fixture boots")) {
        return;
    }
    modem_status_t before = mh_status();

    s_sms_flow_fault = SMS_FLOW_FAULT_CPMS_ERROR;
    feed_direct(DIRECT_3GPP2_HEADER, DIRECT_3GPP2_BODY);
    for (unsigned i = 0u; i < 8u; i++) {
        mh_advance(100u);
    }
    check(mh_tx_count_exact(DIRECT_CPMS_SET) == 3u &&
              mh_tx_count_exact("AT+CMGF=0") == 0u,
          "a failed store is retried from the ring, bounded to three attempts");
    modem_status_t after = mh_status();
    /* Three failed operations count one command error each (finish_operation
     * accounting shared by every failed SMS command) plus exactly one for the
     * drop itself. */
    check(after.sms_received_count == before.sms_received_count &&
              after.command_errors == before.command_errors + 3u + 1u &&
              !after.operation_busy &&
              service_probe().request_queue_depth == 0u,
          "a dropped delivery costs exactly one error beyond its three failed stores");
    /* A failed CPMS set also re-arms the deferred SMS-setup chain (CSMP then
     * CPMS), so count the store itself: no PDU mode is ever entered again. */
    mh_advance(1000u);
    check(mh_tx_count_exact("AT+CMGF=0") == 0u && !mh_status().operation_busy &&
              service_probe().request_queue_depth == 0u,
          "a dropped slot is never retried again");

    /* The slot was dropped after the third attempt; a healthy modem stores
     * the next delivery in one attempt. */
    s_sms_flow_fault = SMS_FLOW_FAULT_NONE;
    mh_clear_tx_capture();
    feed_direct(DIRECT_3GPP2_HEADER, DIRECT_3GPP2_BODY);
    check(mh_tx_count_exact(DIRECT_CPMS_SET) == 1u &&
              mh_tx_count_exact("AT+CMGF=1") == 1u &&
              mh_status().sms_received_count == before.sms_received_count + 1u,
          "a dropped slot does not block later deliveries");

    /* Ring full: the ninth pending delivery while eight are held is dropped. */
    s_sms_flow_fault = SMS_FLOW_FAULT_CMGF_PDU_TIMEOUT; /* no CMGF=0 final */
    mh_clear_tx_capture();
    feed_direct(DIRECT_3GPP2_HEADER, DIRECT_3GPP2_BODY); /* slot 0: parks */
    check(service_probe().command_active &&
              mh_tx_count_exact("AT+CMGF=0") == 1u,
          "ring-full fixture parks the first store at AT+CMGF=0");
    modem_status_t held_before = mh_status();
    for (unsigned i = 1u; i < 8u; i++) {
        feed_direct(DIRECT_3GPP2_HEADER, DIRECT_3GPP2_BODY); /* slots 1..7 */
    }
    check(mh_status().command_errors == held_before.command_errors,
          "eight deliveries are held without loss");
    modem_status_t full_before = mh_status();
    feed_direct(DIRECT_3GPP2_HEADER, DIRECT_3GPP2_BODY); /* no slot */
    check(mh_status().command_errors == full_before.command_errors + 1u,
          "the ninth delivery on a full ring is counted as an error");
    s_sms_flow_fault = SMS_FLOW_FAULT_NONE;
    for (unsigned i = 0u; i < 24u; i++) {
        mh_advance(1000u); /* CMGF=0 times out; retry, then drain the rest */
    }
    check(mh_status().sms_received_count == before.sms_received_count + 1u + 8u &&
              !mh_status().operation_busy &&
              service_probe().request_queue_depth == 0u,
          "all eight held slots drain once the modem answers again");
}

/* Bodies of distinct lengths give distinct AT+CMGW=<tpdu>,0 commands, so
 * the store order is observable on the wire. */
static void feed_direct_len(unsigned chars) {
    static const char LETTERS[] = "abcdefghijkl";
    char header[96];
    char body[16];
    snprintf(header, sizeof(header),
             "+CMT: \"7866910488\",\"\",\"20260916105524\",129,4098,0,8,%u", chars);
    memcpy(body, LETTERS, chars);
    body[chars] = '\0';
    feed_direct(header, body);
}

static void test_direct_delivery_dropped_line_resets_the_collector(void) {
    if (!begin_sms_operation_fixture("direct-delivery dropped-line fixture boots")) {
        return;
    }
    modem_status_t before = mh_status();
    s_hold_final_command = "AT+CGMI";
    check(modem_service_request_debug_at("AT+CGMI"), "debug command is admitted");
    mh_settle();
    check(service_probe().command_active, "debug command is on the wire without a final");

    /* A header without a <length> field pends in line mode (the next framed
     * line is its body). If that line overflows the framer it is DROPPED and
     * never delivered: the collector must be reset so the command's real
     * final is not eaten as the body. */
    mh_clear_tx_capture();
    mh_feed("+CMT: \"+1555\",,\"26/09/16,10:59:04-16\"");
    static char garbage[600];
    memset(garbage, 'X', sizeof(garbage) - 1u);
    garbage[sizeof(garbage) - 1u] = '\0';
    mh_feed(garbage);
    s_hold_final_command = NULL;
    mh_feed("OK");
    check(!service_probe().command_active && !mh_status().operation_busy &&
              mh_tx_count_exact(DIRECT_CPMS_SET) == 0u &&
              mh_status().sms_received_count == before.sms_received_count,
          "a dropped overlong line resets a pending header: the final completes "
          "the command and nothing is stored");
    /* The collector is clean afterwards: a real delivery stores normally. */
    feed_direct(DIRECT_3GPP2_HEADER, DIRECT_3GPP2_BODY);
    check(mh_tx_count_exact(DIRECT_CPMS_SET) == 1u &&
              mh_status().sms_received_count == before.sms_received_count + 1u,
          "the next delivery after a dropped line stores normally");
}

/* Index of the n-th (1-based) CSTR event equal to text, or SIZE_MAX. */
static size_t tx_event_cstr_nth(const char *text, unsigned n) {
    size_t at = 0u;
    for (unsigned i = 0u; i < n; i++) {
        at = tx_event_cstr(text, at);
        if (at == SIZE_MAX) {
            return SIZE_MAX;
        }
        at++;
    }
    return at - 1u;
}

static void test_direct_delivery_restores_text_mode_after_a_cancelled_scan(void) {
    /* A MAILBOX scan parked after AT+CMGF=0 succeeded, then cancelled by a
     * runtime power fault (the live cancel path: call requests wait behind
     * a running SMS operation, and the multipart-read yield already restores
     * text mode itself). After recovery the idle scheduler must issue
     * AT+CMGF=1 before any +CMT store. */
    if (!begin_sms_operation_fixture("mode-restore fixture boots")) {
        return;
    }
    s_hold_final_command = "AT+CMGL=4";
    check(modem_service_request_sms_mailbox(MODEM_SMS_MAILBOX_INBOX),
          "mailbox scan is admitted");
    mh_settle();
    check(service_probe().command_active && mh_tx_count_exact("AT+CMGF=0") == 1u &&
              mh_tx_count_exact("AT+CMGF=1") == 0u,
          "the scan is parked at AT+CMGL=4 with PDU mode entered");

    s_mh_supply_pg = false;
    mh_advance(250u);
    modem_sms_mailbox_result_t mailbox_result;
    check(service_probe().state == MODEM_SERVICE_TEST_STATE_FAILED &&
              modem_service_pop_sms_mailbox_result(&mailbox_result) &&
              mailbox_result.outcome == MODEM_SMS_OUTCOME_CANCELLED &&
              service_probe().sms_mode_restore_pending,
          "the cancelled scan leaves a text-mode restore pending");

    s_hold_final_command = NULL;
    s_mh_supply_pg = true;
    mh_clear_tx_capture();
    modem_service_power_on();
    check(boot_until_ready(30000u), "retained modem recovers");
    modem_status_t before = mh_status();
    feed_direct(DIRECT_3GPP2_HEADER, DIRECT_3GPP2_BODY);
    size_t cnmi = tx_event_cstr("AT+CNMI=2,2,0,0,0", 0u);
    size_t init_text = tx_event_cstr_nth("AT+CMGF=1", 1u);
    size_t restore_text = tx_event_cstr_nth("AT+CMGF=1", 2u);
    size_t store_pdu = tx_event_cstr("AT+CMGF=0", 0u);
    check(cnmi != SIZE_MAX && init_text != SIZE_MAX && restore_text != SIZE_MAX &&
              store_pdu != SIZE_MAX && init_text < cnmi && cnmi < restore_text &&
              restore_text < store_pdu && !service_probe().sms_mode_restore_pending &&
              mh_status().sms_received_count == before.sms_received_count + 1u,
          "after recovery the idle restore issues AT+CMGF=1 before the +CMT store");

    /* Bounded: a restore the module keeps rejecting is retried from the idle
     * scheduler at most three times, then given up. */
    if (!begin_sms_operation_fixture("mode-restore retry fixture boots")) {
        return;
    }
    s_hold_final_command = "AT+CMGL=4";
    check(modem_service_request_sms_mailbox(MODEM_SMS_MAILBOX_INBOX), "retry scan admitted");
    mh_settle();
    s_mh_supply_pg = false;
    mh_advance(250u);
    check(service_probe().state == MODEM_SERVICE_TEST_STATE_FAILED &&
              service_probe().sms_mode_restore_pending,
          "retry fixture cancelled with a restore pending");
    s_hold_final_command = NULL;
    s_mh_supply_pg = true;
    s_sms_fail_cmgf_text_when_ready = true;
    mh_clear_tx_capture();
    modem_service_power_on();
    check(boot_until_ready(30000u), "retained modem recovers for the retry fixture");
    before = mh_status();
    for (unsigned i = 0u; i < 6u; i++) {
        mh_advance(1000u);
    }
    size_t rejected = mh_tx_count_exact("AT+CMGF=1") - 1u; /* minus init's */
    check(rejected == 3u && !service_probe().sms_mode_restore_pending &&
              !mh_status().operation_busy,
          "a rejected restore is retried three times, then given up");
    s_sms_fail_cmgf_text_when_ready = false;
    mh_advance(1000u);
    check(mh_tx_count_exact("AT+CMGF=1") == 4u,
          "no further restore after giving up");
}

static void test_direct_delivery_failed_restore_after_store_is_recovered(void) {
    /* A store whose AT+CMGF=1 answers ERROR still published its row; the
     * module may rest in PDU mode, so the idle restore must run. */
    if (!begin_sms_operation_fixture("failed-restore-after-store fixture boots")) {
        return;
    }
    modem_status_t before = mh_status();
    s_sms_cmgf_text_error_budget = 1u; /* only the store's own AT+CMGF=1 fails */
    mh_clear_tx_capture();
    feed_direct(DIRECT_3GPP2_HEADER, DIRECT_3GPP2_BODY);
    mh_advance(100u);
    check(mh_tx_count_exact("AT+CMGW=26,0") == 1u &&
              mh_status().sms_received_count == before.sms_received_count + 1u &&
              !mh_status().operation_busy,
          "the stored row is published OK despite the failed AT+CMGF=1");
    check(mh_tx_count_exact("AT+CMGF=1") == 2u &&
              tx_event_cstr_nth("AT+CMGF=1", 2u) > tx_event_cstr("AT+CMGW=26,0", 0u) &&
              !service_probe().sms_mode_restore_pending,
          "the idle scheduler re-issues AT+CMGF=1 after the store's failed restore");

    /* Same for a MAILBOX scan whose own restore fails. */
    mh_clear_tx_capture();
    s_sms_cmgf_text_error_budget = 1u;
    check(modem_service_request_sms_mailbox(MODEM_SMS_MAILBOX_INBOX), "scan admitted");
    mh_settle();
    mh_advance(100u);
    modem_sms_mailbox_result_t mailbox_result;
    check(modem_service_pop_sms_mailbox_result(&mailbox_result) &&
              !mh_status().operation_busy &&
              mh_tx_count_exact("AT+CMGF=1") == 2u && !service_probe().sms_mode_restore_pending,
          "the idle scheduler restores text mode after the failed scan restore");
}

static void test_direct_delivery_interrupted_body_times_out(void) {
    /* A header followed by a truncated body must not keep the collector (and
     * the transport) pending forever: the service bounds the wait. */
    if (!begin_sms_operation_fixture("interrupted-body fixture boots")) {
        return;
    }
    s_hold_final_command = "AT+CGMI";
    check(modem_service_request_debug_at("AT+CGMI"), "debug command is admitted");
    mh_settle();
    modem_status_t before = mh_status();
    mh_clear_tx_capture();
    static const char BODY[] = "abcdefghijklmnopqrst";
    mh_rx_push("+CMT: \"7866910488\",\"\",\"20260916105524\",129,4098,0,8,20");
    mh_rx_push_raw((const uint8_t *)BODY, 5u);
    mh_settle();
    check(modem_sms_direct_pending(), "the truncated body leaves the collector pending");
    mh_advance(5100u);
    check(!modem_sms_direct_pending() &&
              mh_status().command_errors == before.command_errors + 1u &&
              mh_status().sms_received_count == before.sms_received_count,
          "5 s without the rest of the body resets the collector and counts one error");
    s_hold_final_command = NULL;
    mh_feed("OK");
    check(!service_probe().command_active && mh_tx_count_exact("AT+CMGF=0") == 0u,
          "the next line after the timeout completes the command instead of being eaten");
    settle_dtr_sleep();
    check(modem_service_transport_sleep_confirmed(),
          "transport sleep is permitted again after the body timeout");

    /* A body completed within the deadline stores as before. */
    before = mh_status();
    mh_clear_tx_capture();
    mh_rx_push("+CMT: \"7866910488\",\"\",\"20260916105524\",129,4098,0,8,20");
    mh_rx_push_raw((const uint8_t *)BODY, 5u);
    mh_settle();
    mh_advance(2000u);
    mh_rx_push_raw((const uint8_t *)&BODY[5], sizeof(BODY) - 1u - 5u);
    mh_rx_push_raw((const uint8_t *)"\r\n", 2u);
    mh_settle();
    for (unsigned i = 0u; i < 4u; i++) {
        mh_advance(100u); /* the store may first need the DTR wake */
    }
    check(mh_tx_count_exact("AT+CMGF=0") == 1u &&
              mh_status().sms_received_count == before.sms_received_count + 1u &&
              mh_status().command_errors == before.command_errors,
          "a body completed within the deadline is stored");
}

static void test_direct_delivery_storage_full_waits_for_room(void) {
    /* A full ME store must not burn the bounded attempts: the module has
     * already acknowledged the network, so the entry waits for room. */
    if (!begin_sms_operation_fixture("storage-full fixture boots")) {
        return;
    }
    settle_dtr_sleep(); /* drain the boot backstops, incl. the first AT+CPMS? */
    modem_status_t before = mh_status();
    s_sms_flow_fault = SMS_FLOW_FAULT_CMGW_FINAL_MEMORY_FULL;
    mh_clear_tx_capture();
    feed_direct(DIRECT_3GPP2_HEADER, DIRECT_3GPP2_BODY);
    for (unsigned i = 0u; i < 5u; i++) {
        mh_advance(1000u);
    }
    check(mh_tx_count_exact("AT+CMGW=26,0") == 1u &&
              mh_status().sms_received_count == before.sms_received_count &&
              !mh_status().operation_busy && service_probe().request_queue_depth == 0u,
          "a memory-full store is not retried while the store stays full");

    /* (a) a DELETE that completes OK makes room: the store is retried. */
    s_sms_flow_fault = SMS_FLOW_FAULT_NONE;
    static const uint16_t victim = 7u;
    check(modem_service_request_delete_sms_indices(&victim, 1u), "delete is admitted");
    for (unsigned i = 0u; i < 5u; i++) {
        mh_advance(100u); /* the delete first needs the DTR wake */
    }
    modem_sms_delete_result_t delete_result;
    check(modem_service_pop_sms_delete_result(&delete_result) &&
              delete_result.outcome == MODEM_SMS_OUTCOME_OK,
          "the delete completes OK");
    for (unsigned i = 0u; i < 3u; i++) {
        mh_advance(100u);
    }
    check(mh_tx_count_exact("AT+CMGW=26,0") == 2u &&
              mh_status().sms_received_count == before.sms_received_count + 1u,
          "after a successful delete the held store is retried and succeeds");

    /* (b) a +CPMS? poll reporting used < total unblocks as well. */
    before = mh_status();
    s_sms_flow_fault = SMS_FLOW_FAULT_CMGW_FINAL_MEMORY_FULL;
    mh_clear_tx_capture();
    feed_direct(DIRECT_3GPP2_HEADER, DIRECT_3GPP2_BODY);
    mh_advance(1000u);
    check(mh_tx_count_exact("AT+CMGW=26,0") == 1u, "second memory-full store is held");
    s_sms_flow_fault = SMS_FLOW_FAULT_NONE;
    /* The +CMTI is only the harness's trigger for an AT+CPMS? poll (store
     * mode never runs alongside direct delivery); the poll's answer unblocks. */
    mh_feed("+CMTI: \"ME\",30"); /* schedules AT+CPMS?; the harness answers 0/255 */
    for (unsigned i = 0u; i < 3u; i++) {
        mh_advance(100u);
    }
    check(mh_tx_count_exact("AT+CPMS?") >= 1u && mh_tx_count_exact("AT+CMGW=26,0") == 2u &&
              mh_status().sms_received_count == before.sms_received_count + 2u,
          "a receive-store poll with room unblocks the held store");

    /* (c) without a delete or a poll, one retry after 60 s. */
    before = mh_status();
    s_sms_flow_fault = SMS_FLOW_FAULT_CMGW_FINAL_MEMORY_FULL;
    mh_clear_tx_capture();
    feed_direct(DIRECT_3GPP2_HEADER, DIRECT_3GPP2_BODY);
    for (unsigned i = 0u; i < 30u; i++) {
        mh_advance(1000u);
    }
    check(mh_tx_count_exact("AT+CMGW=26,0") == 1u, "still held at 30 s");
    s_sms_flow_fault = SMS_FLOW_FAULT_NONE;
    /* 31 s reach the 60 s retry; the rest lets the retried store's first
     * command dispatch through the DTR wake after the long idle. */
    for (unsigned i = 0u; i < 45u; i++) {
        mh_advance(1000u);
    }
    check(mh_tx_count_exact("AT+CMGW=26,0") == 2u &&
              mh_status().sms_received_count == before.sms_received_count + 1u,
          "after 60 s the held store is retried once");

    /* Refused at the prompt stage (AT+CMGW= answered +CMS ERROR: 322, no
     * prompt): the same hold, retried after a delete. */
    before = mh_status();
    s_sms_flow_fault = SMS_FLOW_FAULT_CMGW_PROMPT_MEMORY_FULL;
    mh_clear_tx_capture();
    feed_direct(DIRECT_3GPP2_HEADER, DIRECT_3GPP2_BODY);
    for (unsigned i = 0u; i < 5u; i++) {
        mh_advance(1000u);
    }
    check(mh_tx_count_exact("AT+CMGW=26,0") == 1u &&
              mh_status().sms_received_count == before.sms_received_count &&
              !mh_status().operation_busy,
          "a prompt-stage memory-full refusal holds the slot without retry");
    s_sms_flow_fault = SMS_FLOW_FAULT_NONE;
    check(modem_service_request_delete_sms_indices(&victim, 1u), "second delete is admitted");
    for (unsigned i = 0u; i < 8u; i++) {
        mh_advance(100u);
    }
    check(modem_service_pop_sms_delete_result(&delete_result) &&
              delete_result.outcome == MODEM_SMS_OUTCOME_OK &&
              mh_tx_count_exact("AT+CMGW=26,0") == 2u &&
              mh_status().sms_received_count == before.sms_received_count + 1u,
          "after the delete the prompt-refused store is retried and succeeds");
}

static void test_direct_delivery_bursts_store_in_arrival_order(void) {
    if (!begin_sms_operation_fixture("direct-delivery FIFO fixture boots")) {
        return;
    }
    modem_status_t before = mh_status();

    /* Three back-to-back +CMT pairs queued before the service runs. */
    mh_clear_tx_capture();
    static const char LETTERS[] = "abcdefghijkl";
    for (unsigned chars = 5u; chars <= 7u; chars++) {
        char header[96];
        char body[16];
        snprintf(header, sizeof(header),
                 "+CMT: \"7866910488\",\"\",\"20260916105524\",129,4098,0,8,%u", chars);
        memcpy(body, LETTERS, chars);
        body[chars] = '\0';
        mh_rx_push(header);
        mh_rx_push(body);
    }
    mh_settle();
    for (unsigned i = 0u; i < 4u; i++) {
        mh_advance(100u);
    }
    size_t first = tx_event_cstr("AT+CMGW=23,0", 0u);
    size_t second = tx_event_cstr("AT+CMGW=24,0", 0u);
    size_t third = tx_event_cstr("AT+CMGW=25,0", 0u);
    check(first != SIZE_MAX && second != SIZE_MAX && third != SIZE_MAX &&
              first < second && second < third &&
              mh_status().sms_received_count == before.sms_received_count + 3u &&
              mh_status().command_errors == before.command_errors,
          "three back-to-back deliveries are stored in arrival order");

    /* A slot freed and refilled while older entries wait must not jump the
     * queue: A stores, B and C wait, D lands in A's slot, order stays B C D. */
    mh_clear_tx_capture();
    before = mh_status();
    s_hold_final_command = "AT+CMGF=1";
    feed_direct_len(5u); /* A: parks at its AT+CMGF=1 final */
    check(service_probe().command_active && mh_tx_count_exact("AT+CMGW=23,0") == 1u,
          "FIFO fixture parks A at its text-mode restore");
    feed_direct_len(6u); /* B waits */
    feed_direct_len(7u); /* C waits */
    mh_feed("OK");       /* A completes; B starts and parks likewise */
    check(mh_tx_count_exact("AT+CMGW=24,0") == 1u && service_probe().command_active,
          "B starts after A and parks");
    /* D is 9 chars: 8 septets pack into 7 octets, the same TPDU length as C. */
    feed_direct_len(9u); /* D takes A's freed slot while C still waits */
    s_hold_final_command = NULL;
    mh_feed("OK");       /* B completes; C then D drain */
    for (unsigned i = 0u; i < 4u; i++) {
        mh_advance(100u);
    }
    size_t c_event = tx_event_cstr("AT+CMGW=25,0", 0u);
    size_t d_event = tx_event_cstr("AT+CMGW=26,0", 0u);
    check(c_event != SIZE_MAX && d_event != SIZE_MAX && c_event < d_event &&
              mh_status().sms_received_count == before.sms_received_count + 4u &&
              !mh_status().operation_busy &&
              service_probe().request_queue_depth == 0u,
          "a refilled slot does not overtake older held deliveries (FIFO)");
}

typedef struct {
    const char *header;
    const uint8_t *body;
    size_t body_len;
    const char *expected_text;
    const char *name;
} direct_wire_case_t;

static void test_direct_delivery_plain_bodies_are_read_raw(void) {
    static const char MULTI[] = "Your code is 123456\r\nDo not share";
    static const uint8_t AT_BODY[3] = {'a', 0x00u, 'b'};
    static const uint8_t ESC_BODY[3] = {'a', 0x1Bu, 0x3Cu};
    static const char TELIT_MULTI[] = "Hi\r\nthere";
    static const char WEMT_HEX[] = "050003620202D46435599D9EABE7EAB99AAC26ABC9";
    static const char EMOJI_HEX[] = "D83EDD2A";
    static const direct_wire_case_t cases[] = {
        {"+CMT: \"+18132936877\",,\"26/09/16,10:59:04-16\",145,4,0,0,\"+19037029920\",145,33",
         (const uint8_t *)MULTI, sizeof(MULTI) - 1u, MULTI,
         "3GPP text body with an embedded CRLF"},
        {"+CMT: \"+18132936877\",,\"26/09/16,10:59:04-16\",145,4,0,0,\"+19037029920\",145,6",
         (const uint8_t *)"Hello ", 6u, "Hello ", "3GPP text body with a trailing space"},
        {"+CMT: \"+18132936877\",,\"26/09/16,10:59:04-16\",145,4,0,0,\"+19037029920\",145,3",
         AT_BODY, sizeof(AT_BODY), "a@b", "3GPP text body with GSM 0x00 (@)"},
        {"+CMT: \"+18132936877\",,\"26/09/16,10:59:04-16\",145,4,0,0,\"+19037029920\",145,3",
         ESC_BODY, sizeof(ESC_BODY), "a[", "3GPP text body with ESC 0x3C ([)"},
        {"+CMT: \"7866910488\",\"\",\"20260916105524\",129,4098,0,8,9",
         (const uint8_t *)TELIT_MULTI, sizeof(TELIT_MULTI) - 1u, TELIT_MULTI,
         "Telit enc 8 body with a line break"},
        {DIRECT_3GPP2_HEADER, (const uint8_t *)DIRECT_3GPP2_BODY,
         sizeof(DIRECT_3GPP2_BODY) - 1u, DIRECT_3GPP2_BODY, "Verizon enc 8 plain body"},
        {"+CMT: \"7866910488\",\"\",\"20260916105553\",129,4101,0,9,23",
         (const uint8_t *)WEMT_HEX, sizeof(WEMT_HEX) - 1u, NULL,
         "Verizon WEMT GSM-7+UDH hex body"},
        {"+CMT: \"7866910488\",\"\",\"20260916105559\",129,4098,0,4,2",
         (const uint8_t *)EMOJI_HEX, sizeof(EMOJI_HEX) - 1u, "??",
         "Verizon Unicode hex body"},
    };
    if (!begin_sms_operation_fixture("raw-body fixture boots")) {
        return;
    }
    for (size_t i = 0u; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const direct_wire_case_t *c = &cases[i];
        modem_status_t before = mh_status();
        mh_clear_tx_capture();
        feed_direct_wire(c->header, c->body, c->body_len);
        char pdu[SMS_DELIVER_HEX_MAX];
        sms_codec_message_t decoded;
        bool stored = captured_stored_pdu(pdu, sizeof(pdu)) &&
                      sms_pdu_decode(pdu, &decoded) && !decoded.submit;
        bool text_ok = stored &&
                       (c->expected_text == NULL
                            ? (decoded.has_concat && decoded.concat_seq == 2u)
                            : strcmp(decoded.text, c->expected_text) == 0);
        if (!text_ok) {
            fprintf(stderr, "raw body case '%s': stored=%u text='%s'\n", c->name,
                    stored ? 1u : 0u, stored ? decoded.text : "");
        }
        check(text_ok && mh_tx_count_exact("AT+CMGF=1") == 1u &&
                  mh_status().sms_received_count == before.sms_received_count + 1u &&
                  mh_status().command_errors == before.command_errors &&
                  !mh_status().operation_busy,
              c->name);
    }

    /* IS-637 7-bit ASCII (enc 2): <length> is the packed octet count (14 for
     * 15 characters), so the RAW reader terminates at the body's own CRLF
     * one byte past <length>. Bench self-loopback vector, 2026-09-15. */
    {
        static const char ASCII_HEADER[] =
            "+CMT: \"8132936877\",\"\",\"20260915193325\",129,4098,1,2,14";
        static const char ASCII_BODY[] = "Warp loopback 2";
        char expected[SMS_DELIVER_HEX_MAX];
        uint8_t expected_tpdu = 0u;
        check(build_expected_direct_pdu(ASCII_HEADER, ASCII_BODY, expected, &expected_tpdu),
              "enc 2 golden builds independently");
        modem_status_t ascii_before = mh_status();
        mh_clear_tx_capture();
        feed_direct_wire(ASCII_HEADER, (const uint8_t *)ASCII_BODY, sizeof(ASCII_BODY) - 1u);
        char cmgw[24];
        snprintf(cmgw, sizeof(cmgw), "AT+CMGW=%u,0", (unsigned)expected_tpdu);
        char ascii_pdu[SMS_DELIVER_HEX_MAX];
        sms_codec_message_t ascii;
        check(mh_tx_count_exact(cmgw) == 1u && captured_stored_pdu(ascii_pdu, sizeof(ascii_pdu)) &&
                  strcmp(ascii_pdu, expected) == 0 && sms_pdu_decode(ascii_pdu, &ascii) &&
                  strcmp(ascii.text, "Warp loopback 2") == 0 &&
                  mh_status().sms_received_count == ascii_before.sms_received_count + 1u,
              "a CDMA 7-bit ASCII text-form message is stored through the RAW reader");
    }

    /* Encoding 2 bodies are up to 8/7 longer than <length> and may contain a
     * line break past it: the RAW reader must not stop at the first line
     * break at/after <length> unless the body up to it parses (owner audit P2
     * repro: "AAAAAAAAAAAAAA\nB", <length> 14, was truncated to 14 chars). */
    {
        static const char ENC2_HEADER[] =
            "+CMT: \"8132936877\",\"\",\"20260915193325\",129,4098,1,2,14";
        static const struct { const char *body; const char *name; } enc2[] = {
            {"AAAAAAAAAAAAAA\nB", "enc 2 body with a line break past <length> is stored whole"},
            {"AAAAAAAAAAAAAA\n", "enc 2 body ending in a line break keeps it"},
            {"AAAAAAAAAAAAAAA\n", "enc 2 preserves a trailing LF at the ambiguous packed boundary"},
            {"AAAAAAAAAAAAAA\r\n", "enc 2 preserves a trailing CRLF"},
            {"AAAAAAAAAAAAAAA\r", "enc 2 preserves a trailing CR before the terminator"},
            {"AAAAAAAAAAAAAAB", "enc 2 body without a line break is stored as before"},
        };
        for (size_t i = 0u; i < sizeof(enc2) / sizeof(enc2[0]); i++) {
            modem_status_t e_before = mh_status();
            s_hold_final_command = "AT+CGMI";
            check(modem_service_request_debug_at("AT+CGMI"), "enc 2 fixture admits a debug command");
            mh_settle();
            mh_clear_tx_capture();
            feed_direct_wire(ENC2_HEADER, (const uint8_t *)enc2[i].body, strlen(enc2[i].body));
            bool held = service_probe().command_active &&
                        mh_status().urc_count == e_before.urc_count + 1u;
            s_hold_final_command = NULL;
            mh_feed("OK");
            char e_pdu[SMS_DELIVER_HEX_MAX];
            sms_codec_message_t e_msg;
            bool stored = captured_stored_pdu(e_pdu, sizeof(e_pdu)) &&
                          sms_pdu_decode(e_pdu, &e_msg) && strcmp(e_msg.text, enc2[i].body) == 0;
            if (!stored || !held) {
                fprintf(stderr, "enc2 case '%s': held=%u stored=%u text='%s'\n", enc2[i].name,
                        held ? 1u : 0u, stored ? 1u : 0u, stored ? e_msg.text : "");
            }
            check(held && !service_probe().command_active && stored &&
                      mh_status().sms_received_count == e_before.sms_received_count + 1u &&
                      mh_status().command_errors == e_before.command_errors,
                  enc2[i].name);
        }
    }

    /* A garbage enc 2 body that never parses is rejected once it exceeds the
     * plain-body bound (ceil(8*14/7) + 1 = 17), not held to the deadline:
     * LFs at 14 and 18, the held command survives and completes on its OK. */
    {
        static const char GARBAGE[] = "XXXXXXXXXXXXXX\nXXX\nXX";
        modem_status_t g_before = mh_status();
        s_hold_final_command = "AT+CGMI";
        check(modem_service_request_debug_at("AT+CGMI"), "bound fixture admits a debug command");
        mh_settle();
        mh_clear_tx_capture();
        feed_direct_wire("+CMT: \"8132936877\",\"\",\"20260915193325\",129,4098,1,2,14",
                         (const uint8_t *)GARBAGE, sizeof(GARBAGE) - 1u);
        bool held = service_probe().command_active && !modem_sms_direct_pending() &&
                    mh_status().command_errors == g_before.command_errors + 1u;
        s_hold_final_command = NULL;
        mh_feed("OK");
        check(held && !service_probe().command_active && mh_tx_count_exact("AT+CMGF=0") == 0u &&
                  mh_status().sms_received_count == g_before.sms_received_count,
              "a never-parsing plain body is rejected at the bound and the next line reaches the command");
    }

    /* The byte exceeding a continued body's bound can start the waiting
     * command's final: it must be replayed, not dropped with the body. */
    {
        modem_status_t b_before = mh_status();
        s_hold_final_command = "AT+CGMI";
        check(modem_service_request_debug_at("AT+CGMI"), "byte-replay fixture admits a command");
        mh_settle();
        mh_clear_tx_capture();
        mh_rx_push("+CMT: \"123\",\"\",\"20260915193325\",129,4098,1,2,14");
        mh_rx_push_raw((const uint8_t *)"XXXXXXXXXXXXXX\nXX", 17u);
        mh_settle();
        check(modem_sms_direct_pending(), "incomplete body reaches its bound");
        s_hold_final_command = NULL;
        mh_feed("OK");
        check(!service_probe().command_active && !modem_sms_direct_pending() &&
                  mh_status().command_errors == b_before.command_errors + 1u &&
                  mh_status().sms_received_count == b_before.sms_received_count &&
                  mh_tx_count_exact("AT+CMGF=0") == 0u,
              "the first byte beyond the body bound is retained in the command final");
    }

    /* Reviewer's probe: an unparseable plain body (final rejection) followed
     * by a valid +CMT inside the window must not eat the second header. */
    {
        static const struct { const char *header; size_t len; const char *name; } probes[] = {
            {"+CMT: \"+18132936877\",,\"26/09/16,10:59:04-16\",145,4,0,0,\"+19037029920\",145,15",
             15u, "an unparseable 3GPP plain body (L 15) is rejected at its CRLF"},
            {"+CMT: \"+18132936877\",,\"26/09/16,10:59:04-16\",145,4,0,0,\"+19037029920\",145,140",
             140u, "an unparseable 3GPP plain body (L 140) is rejected at its CRLF"},
            {"+CMT: \"7866910488\",\"\",\"20260916105559\",129,4099,0,8,15",
             15u, "a rejected-by-design VMN text delivery ends at its CRLF"},
            {"+CMT: \"123\",\"\",\"20260915193325\",129,4098,0,2,14",
             15u, "an invalid ASCII body ends at its CRLF without consuming the next SMS"},
        };
        for (size_t i = 0u; i < sizeof(probes) / sizeof(probes[0]); i++) {
            static uint8_t body[160];
            memset(body, 'A', probes[i].len);
            body[probes[i].len / 2u] = 0xE9u; /* >= 0x80: no plain parser accepts it */
            modem_status_t p_before = mh_status();
            mh_clear_tx_capture();
            mh_rx_push(probes[i].header);
            mh_rx_push_raw(body, probes[i].len);
            mh_rx_push_raw((const uint8_t *)"\r\n", 2u);
            feed_direct(DIRECT_3GPP2_HEADER, DIRECT_3GPP2_BODY); /* within the window */
            for (unsigned t = 0u; t < 4u; t++) {
                mh_advance(100u);
            }
            check(mh_status().command_errors == p_before.command_errors + 1u &&
                      mh_tx_count_exact("AT+CMGW=26,0") == 1u &&
                      mh_status().sms_received_count == p_before.sms_received_count + 1u &&
                      !modem_sms_direct_pending(),
                  probes[i].name);
        }
    }

    /* Telit enc 8 exact-length body with an embedded LF before <length>. */
    {
        modem_status_t l_before = mh_status();
        mh_clear_tx_capture();
        feed_direct_wire("+CMT: \"7866910488\",\"\",\"20260916105524\",129,4098,0,8,5",
                         (const uint8_t *)"ab\ncd", 5u);
        char l_pdu[SMS_DELIVER_HEX_MAX];
        sms_codec_message_t l_msg;
        check(captured_stored_pdu(l_pdu, sizeof(l_pdu)) && sms_pdu_decode(l_pdu, &l_msg) &&
                  strcmp(l_msg.text, "ab\ncd") == 0 &&
                  mh_status().sms_received_count == l_before.sms_received_count + 1u,
              "a Telit enc 8 exact-length body keeps an embedded LF");
    }

    /* An A2P message from an alphanumeric sender (TON 5) is stored with its
     * sender intact. */
    {
        static const char A2P_HEADER[] =
            "+CMT: \"AMAZON\",,\"26/09/16,10:59:04-16\",208,4,0,0,\"+19037029920\",145,7";
        modem_status_t a2p_before = mh_status();
        mh_clear_tx_capture();
        feed_direct_wire(A2P_HEADER, (const uint8_t *)"Djssjjs", 7u);
        char a2p_pdu[SMS_DELIVER_HEX_MAX];
        sms_codec_message_t a2p;
        check(captured_stored_pdu(a2p_pdu, sizeof(a2p_pdu)) && sms_pdu_decode(a2p_pdu, &a2p) &&
                  strcmp(a2p.address, "AMAZON") == 0 && strcmp(a2p.text, "Djssjjs") == 0 &&
                  mh_status().sms_received_count == a2p_before.sms_received_count + 1u,
              "an alphanumeric sender is stored and decodes as its name");
    }

    /* No fragment of a multi-line body reaches route_urc or an active
     * command: with AT+CGMI held on the wire, only the +CMT itself counts as a
     * URC and the command still waits for its real final. */
    modem_status_t before = mh_status();
    s_hold_final_command = "AT+CGMI";
    check(modem_service_request_debug_at("AT+CGMI"), "debug command is admitted");
    mh_settle();
    mh_clear_tx_capture();
    feed_direct_wire(cases[0].header, cases[0].body, cases[0].body_len);
    check(service_probe().command_active && mh_status().operation_busy &&
              mh_status().urc_count == before.urc_count + 1u &&
              mh_status().command_errors == before.command_errors &&
              mh_tx_count_exact(DIRECT_CPMS_SET) == 0u,
          "a multi-line body raises exactly one URC and never completes the command");
    s_hold_final_command = NULL;
    mh_feed("OK");
    char pdu[SMS_DELIVER_HEX_MAX];
    sms_codec_message_t decoded;
    check(!service_probe().command_active &&
              mh_tx_count_exact(DIRECT_CPMS_SET) == 1u &&
              captured_stored_pdu(pdu, sizeof(pdu)) && sms_pdu_decode(pdu, &decoded) &&
              strcmp(decoded.text, MULTI) == 0 &&
              mh_status().sms_received_count == before.sms_received_count + 1u,
          "the real final completes the command and the whole body is stored");

    /* A body that would overflow the raw cap is dropped as unreadable and the
     * bytes after it flow through the framer again. */
    modem_status_t cap_before = mh_status();
    mh_clear_tx_capture();
    static uint8_t huge[MODEM_SMS_DIRECT_LINE_MAX + 16u];
    memset(huge, 'A', sizeof(huge));
    feed_direct_wire(cases[0].header, huge, sizeof(huge));
    mh_feed("+CEREG: 1,1");
    check(mh_status().command_errors == cap_before.command_errors + 1u &&
              mh_status().sms_received_count == cap_before.sms_received_count &&
              mh_status().urc_count == cap_before.urc_count + 2u &&
              mh_tx_count_exact(DIRECT_CPMS_SET) == 0u,
          "a body past the raw cap is dropped and the line path resumes");
}

static void test_sim_provider_query_keeps_receiving(bool timeout) {
    begin_telit(true);
    s_rfsts_response = UNKNOWN_PLMN_RFSTS;
    set_sim_provider_fixture("US Mobile");
    s_hold_final_command = g_modem_vendor.sim_provider_query.query_cmd;
    check(boot_until_ready(30000u), "SIM-name receive fixture boots");
    mh_feed("+CEREG: 1");
    mh_advance(100u);
    check(service_probe().command_active &&
              strcmp(mh_status().operator_name, "00101") == 0,
          "pending SIM read leaves an honest numeric operator visible");
    modem_status_t before = mh_status();
    check(modem_service_request_debug_at("AT+CGMM"),
          "ordinary request queues behind the SIM read");
    feed_direct("+CMT: \"15551230000\",\"\",\"20260916210000\",129,4098,0,8,2", "OK");
    mh_feed("RING");
    mh_feed("+CLIP: \"+15557654321\",145,,,,0");
    mh_feed("#ECAM: 0,6,1,,,");
    check(mh_status().ring_active && service_probe().command_active &&
              strcmp(mh_status().incoming_number, "+15557654321") == 0,
          "SIM name collection continues routing incoming call and SMS indications");
    check(modem_service_request_answer(), "incoming answer queues during the SIM read");
    s_clcc_row = "+CLCC: 1,1,0,0,0,\"15557654321\",145";
    if (timeout) {
        mh_advance(g_modem_vendor.sim_provider_query.timeout_ms + 1u);
        check(service_probe().command_active &&
                  mh_tx_count_exact("ATA") == 0u &&
                  mh_tx_count_exact("AT+CGMM") == 0u,
              "expired SIM read retains final ownership before queued answer or SMS storage");
        feed_direct("+CMT: \"15551230000\",\"\",\"20260916210001\",129,4098,0,8,2", "OK");
        check(service_probe().command_active && mh_tx_count_exact("ATA") == 0u,
              "an SMS body reading OK cannot release the late-response drain");
    }
    s_hold_final_command = NULL;
    mh_feed(s_sim_provider_response);
    mh_feed("OK");
    for (unsigned i = 0u; i < 5u; i++) mh_advance(100u);
    check(tx_first_index("ATA") < tx_first_index("AT+CGMM") &&
              mh_status().call_state == MODEM_CALL_ACTIVE &&
              mh_status().sms_received_count == before.sms_received_count + (timeout ? 2u : 1u) &&
              strcmp(mh_status().operator_name, timeout ? "00101" : "US Mobile") == 0,
          "answer takes priority after the SIM final and the incoming SMS is preserved");
}

static void test_sim_provider_unreleased_channel_and_shutdown(void) {
    begin_telit(true);
    s_rfsts_response = UNKNOWN_PLMN_RFSTS;
    s_hold_final_command = g_modem_vendor.sim_provider_query.query_cmd;
    check(boot_until_ready(30000u), "SIM drain failure fixture boots");
    mh_feed("+CEREG: 1");
    mh_advance(100u);
    check(modem_service_request_debug_at("AT+CGMM"), "request queues before SIM timeout");
    mh_advance(g_modem_vendor.sim_provider_query.timeout_ms + 1u);
    mh_advance(5001u);
    check(service_probe().state == MODEM_SERVICE_TEST_STATE_FAILED &&
              service_probe().failed_module_may_be_live && s_mh_rail_enabled &&
              mh_tx_count_exact("AT+CGMM") == 0u &&
              service_probe().request_queue_depth == 0u,
          "an unreleased SIM command fails the channel without dispatching queued work or cutting power");
    mh_feed("OK");
    check(service_probe().state == MODEM_SERVICE_TEST_STATE_FAILED &&
              mh_tx_count_exact("AT+CGMM") == 0u,
          "a final after drain exhaustion cannot revive cancelled requests");

    for (unsigned timeout = 0u; timeout < 2u; timeout++) {
        begin_telit(true);
        s_rfsts_response = UNKNOWN_PLMN_RFSTS;
        s_hold_final_command = g_modem_vendor.sim_provider_query.query_cmd;
        check(boot_until_ready(30000u), "SIM read shutdown fixture boots");
        mh_feed("+CEREG: 1");
        mh_advance(100u);
        if (timeout) mh_advance(g_modem_vendor.sim_provider_query.timeout_ms + 1u);
        modem_service_power_off();
        check(service_probe().power_off_pending && service_probe().command_active &&
                  mh_tx_count_exact("AT#SHDN") == 0u,
              "shutdown waits for the non-abortable SIM read to release the AT channel");
        s_hold_final_command = NULL;
        mh_feed("ERROR");
        mh_advance(10u);
        check(mh_tx_count_exact("AT#SHDN") == 1u,
              "the SIM final releases exactly one deferred shutdown command");
    }
}

int main(void) {
    test_operator_name_fallbacks();
    test_operator_name_registration_and_sim_lifetimes();
    test_sim_provider_query_keeps_receiving(false);
    test_sim_provider_query_keeps_receiving(true);
    test_sim_provider_unreleased_channel_and_shutdown();
    test_autonomous_startup_restart_reinitializes_once();
    test_startup_restart_preserves_wait_and_shutdown_guards();
    test_controlled_reboot_ignores_sleep_wake_signals();
    test_pending_power_on_is_not_quiescent_off();
    test_uart_waits_for_module_power_evidence();
    test_continuously_readable_uart_is_tick_bounded();
    test_factory_flow_profile_bootstraps_without_cts();
    test_clcc_integrity_counts_uart_line_errors();
    test_qualified_voice_transport_starts_only_after_audio_init();
    test_power_off_during_start_pulse_is_graceful();
    test_power_on_during_inflight_shutdown_restarts_once();
    test_power_on_cancels_shutdown_before_uart_dispatch();
    test_power_observation_deadline_rebases_on_restart();
    test_indeterminate_module_status_never_authorizes_rail_cut();
    test_cold_boot_matching_provision();
    test_scan_timer_provisioning();
    test_auto_profile_provisioning();
    test_board_imei_capture_is_scoped_and_write_once();
    test_runtime_serving_signal_sampling();
    test_raw_rx_capture_preserves_wire_bytes();
    test_background_polling_bench_gate_is_narrow();
    test_transport_sleep_confirmation_is_strict();
    test_transport_sleep_waits_for_a_raw_cmt_body();
    test_cold_boot_repairs_dvi_and_restores_runtime_mode();
    test_cold_boot_provisions_antenna_before_rf_online();
    test_carrier_reset_tuner_read_error();
    test_sim_absent_locked_and_cme_fallback();
    test_mismatch_writes_verifies_and_reboots_once();
    test_sled_mismatch_is_saved_without_reboot();
    test_each_sms_wake_profile_mismatch_converges();
    test_late_psmri_invalidates_and_replays_lifecycle();
    test_sms_text_send_contract();
    test_sms_text_send_failures_and_prompt_settle();
    test_sms_save_and_delete_contracts();
    test_sms_binary_send_contract();
    test_direct_delivery_is_restored_as_a_pdu();
    test_direct_delivery_mid_command_and_qcmti();
    test_slow_call_forwarding_keeps_receiving();
    test_direct_delivery_retries_after_store_failure();
    test_direct_delivery_plain_bodies_are_read_raw();
    test_direct_delivery_bursts_store_in_arrival_order();
    test_direct_delivery_dropped_line_resets_the_collector();
    test_direct_delivery_restores_text_mode_after_a_cancelled_scan();
    test_direct_delivery_failed_restore_after_store_is_recovered();
    test_direct_delivery_interrupted_body_times_out();
    test_direct_delivery_storage_full_waits_for_room();
    test_sms_binary_failures_and_prompt_settle();
    test_sms_cpms_timeout_cancels_every_cpms_operation();
    test_sms_power_recovery_never_replays_transaction();
    test_sms_cts_failure_cancels_every_operation();
    test_sms_queue_eviction_is_exact_for_every_operation();
    test_sms_recovery_cancels_every_uncommitted_operation();
    test_sms_crossed_urcs_remain_routed();
    test_sms_unread_status_is_bracketed();
    test_vvm_control_sms_is_consumed_silently();
    test_legacy_url_vvm_is_consumed_silently();
    test_unknown_application_sms_fails_open();
    test_interleaved_arrival_waits_for_current_snapshot();
    test_vvm_cleanup_error_retries_without_alerting();
    test_vvm_cleanup_timeout_is_bounded_and_silent();
    test_unclassifiable_cmti_fails_open();
    test_sms_ri_wakes_sleeping_transport();
    test_sms_wake_profile_failure_fails_closed();
    test_missing_sim_uses_bounded_activation_window();
    test_hot_sim_never_spends_a_second_automatic_reboot();
    test_pg_and_cts_faults_fail_closed();
    test_expected_shutdown_status_drop_is_not_a_fault();
    test_stuck_software_shutdown_uses_graceful_hardware_fallback();
    test_failed_graceful_fallback_uses_unconditional_shutdown();
    test_exhausted_shutdown_ladder_is_visible_and_fail_closed();
    test_shutdown_pulse_arm_failure_is_bounded();
    test_power_on_intent_survives_exhausted_shutdown_ladder();
    test_unexpected_runtime_status_drop_retains_rail();
    test_provision_fault_outcomes();
    test_quiet_registration_indicator_profile();
    test_diag_scheduler_atomicity_and_preemption();
    test_plain_diag_response_cannot_swallow_call_progress();
    test_packet_diag_accepts_wwx_dynamic_context_shape();
    test_optional_diag_timeout_is_not_unsupported();
    test_guarded_scan_and_band_maintenance();
    test_scan_maintenance_yields_to_call();
    test_telit_coarse_call_events_reconcile_live_projection();
    test_remote_foreground_release_preserves_waiting_identity();
    test_long_active_call_outlives_legacy_setup_horizon();
    test_telit_hold_toggle_is_safely_gated();
    test_telit_new_call_cleanup_is_model_owned();
    test_dtmf_sequence_is_atomic_and_ordered();
    test_dtmf_sequence_stops_on_failure_and_call_change();
    test_dtmf_sequence_does_not_survive_recovery();
    test_band_marker_requires_durable_clean_unit();
    test_band_recovery_and_call_preemption();
    test_deferred_band_mutation_keeps_its_phase();
    test_guarded_antenna_maintenance();
    test_call_forwarding_mailbox_and_indicators();
    test_mwi_urc_survives_query_interleave();
    test_mwi_ambiguous_query_never_invents_voice_mail();
    test_carrier_refresh_priority();
    test_local_forwarding_flags_without_network_fallback();
    test_supplementary_refresh_recovery();
    test_call_forward_queue_cancellation();
    test_newer_call_forward_result_survives_old_final();
    test_phonebook_list_and_crud_contract();
    test_phonebook_operation_fault_matrix();
    test_phonebook_queue_eviction_and_recovery_are_terminal();

    if (s_failures == 0) {
        printf("test_modem_telit_service: OK\n");
    }
    return s_failures != 0;
}

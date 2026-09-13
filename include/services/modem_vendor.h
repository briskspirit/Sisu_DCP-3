#ifndef MODEM_VENDOR_H
#define MODEM_VENDOR_H

/* Modem-backend abstraction. A concrete backend owns power timing, AT setup,
 * call syntax/parsing, transport wake policy, and proprietary diagnostics.
 * Each supported hardware/backend pair selects exactly one implementation at
 * link time; the generic service never selects behavior by modem model.
 *
 * Plan-level refinements over the original design doc:
 *  - vendor parsers are pure protocol boundaries; they must not lock or send AT;
 *  - off_sense_begin() resets a vendor-qualified OFF observation window used
 *    both during startup ambiguity and graceful off-discharge. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "services/call_forward_types.h"
#include "services/call_types.h"
#include "services/modem_diag.h"
#include "services/modem_maintenance.h"
#include "services/modem_signal.h"

typedef enum {
    MODEM_DEGRADE_NONE = 0, MODEM_DEGRADE_AUDIO, MODEM_DEGRADE_SMS_SETUP,
    MODEM_DEGRADE_SIM_GATE,
} modem_degrade_t;

typedef enum {
    MODEM_INIT_PREREQ_NONE = 0u,
    MODEM_INIT_PREREQ_SIM_READY = 1u << 0,
    /* Run only in the focused pass started by a trustworthy SIM-ready edge.
     * This lets a vendor bracket SIM-owned setup with functional-level changes
     * without repeating unrelated cold-boot initialization. */
    MODEM_INIT_PREREQ_SIM_COMPLETION = 1u << 1,
} modem_init_prereq_t;

typedef enum {
    MODEM_SETTING_RUNTIME = 0,
    MODEM_SETTING_PROFILE,
    MODEM_SETTING_NVM,
    MODEM_SETTING_NVM_REBOOT,
} modem_setting_persistence_t;

/* Neutral one-time data captured by a backend's ordinary init ladder. The
 * generic service owns persistence and skip-on-subsequent-boot policy; the
 * backend supplies only the command that exposes the value. */
typedef enum {
    MODEM_INIT_CAPTURE_NONE = 0,
    MODEM_INIT_CAPTURE_BOARD_IMEI,
} modem_init_capture_t;

typedef struct {
    const char *cmd;
    uint32_t timeout_ms;
    uint8_t retry_limit;       /* retries after the first attempt */
    bool recoverable;          /* NAK tolerated: degrade + continue */
    modem_degrade_t degrade;
    uint8_t prerequisites;     /* modem_init_prereq_t bit set */
    modem_setting_persistence_t persistence;
    /* Optional response validator/parser. It may update vendor-private
     * provisioning state, but must not lock or transmit. */
    bool (*parse)(const char *line);
    modem_init_capture_t capture;
    /* Ignore inbound CTS only for bootstrap commands needed to establish the
     * modem's own command-mode flow-control policy. Hardware RTS remains
     * active, and all later traffic returns to bounded CTS-gated writes. */
    bool bypass_cts;
} modem_init_step_t;

typedef struct {
    bool supply_power_good;
    bool ri_asserted;
    bool module_status_valid;
    uint16_t module_status_raw;
    uint16_t module_status_mv;
} modem_power_observation_t;

typedef struct {
    uint32_t rail_settle_ms;
    /* Zero keeps the legacy behavior. A nonzero bound requires supply PG
     * before the module control pulse or any UART probe is attempted. */
    uint32_t rail_power_good_timeout_ms;
    uint32_t pwron_pulse_ms;
    uint32_t ready_budget_ms;
    const char *off_cmd;
    uint32_t off_ack_timeout_ms;
    uint32_t off_sense_timeout_ms;
    /* Optional bounded fallbacks after a graceful command was acknowledged (or
     * timed out) but module-status never qualified OFF. The first pulse reuses
     * the ordinary ON/OFF control as a hardware-requested graceful shutdown;
     * the second uses the module's unconditional emergency input. A zero pulse
     * width disables that stage. Every nonzero stage must also provide a
     * post-pulse status-observation budget; the core still releases the rail
     * only through off_complete(). */
    uint32_t graceful_off_pulse_ms;
    uint32_t graceful_off_sense_timeout_ms;
    uint32_t emergency_off_pulse_ms;
    uint32_t emergency_off_sense_timeout_ms;
    /* Some modules must never have their supply cut while their status output
     * still says they are alive. False leaves the rail owned and reports a
     * failed shutdown instead of using the timeout as power-off evidence. */
    bool cut_rail_on_off_timeout;
} modem_power_cfg_t;

typedef enum {
    MODEM_PROVISION_LINE_IGNORE = 0,
    MODEM_PROVISION_LINE_MATCH,
    MODEM_PROVISION_LINE_MISMATCH,
    MODEM_PROVISION_LINE_INVALID,
} modem_provision_line_t;

#define MODEM_PROVISION_COMMAND_MAX 96u

/* Vendor-specific SIM indications are normalized before reaching the core.
 * NONE means the line is unrelated or malformed. PRESENT is deliberately
 * distinct from READY: a PIN-locked card is present but not service-ready. */
typedef enum {
    MODEM_SIM_OBSERVATION_NONE = 0,
    MODEM_SIM_OBSERVATION_ABSENT,
    MODEM_SIM_OBSERVATION_PRESENT,
    MODEM_SIM_OBSERVATION_READY,
} modem_sim_observation_t;

/* Query/compare/write/verify descriptor for settings whose current value can
 * be read back. Persistent settings must use this path rather than blind init
 * writes. The generic service owns sequencing and the one-reboot budget; the
 * vendor owns every command string and response grammar.
 *
 * The optional callbacks keep compound vendor policy behind this boundary:
 * - applicable() can omit a row after an earlier discovery query;
 * - build_set_cmd() produces a capability-dependent write;
 * - readback_begin()/readback_finish() aggregate multi-line readback. When a
 *   finish callback is present, its result is authoritative for the query
 *   final, including a documented ERROR response.
 * - set_each_pass executes and re-verifies a runtime/profile SET even when the
 *   initial readback matches, for commands with a transient hardware effect.
 * Query-only discovery/verification rows set query_only and have no writer. */
typedef struct {
    const char *query_cmd;
    const char *set_cmd;
    uint32_t timeout_ms;
    uint8_t retry_limit;
    bool recoverable;
    modem_degrade_t degrade;
    uint8_t prerequisites;     /* modem_init_prereq_t bit set */
    modem_setting_persistence_t persistence;
    modem_provision_line_t (*parse_readback)(const char *line);
    bool query_only;
    bool set_each_pass;
    /* A matching readback after this row proves that the vendor's complete
     * pre-sleep SMS-wake sequence has crossed its final apply boundary. */
    bool qualifies_sms_wake;
    bool (*applicable)(void);
    bool (*build_set_cmd)(char *out, size_t out_cap);
    void (*readback_begin)(void);
    modem_provision_line_t (*readback_finish)(bool command_ok,
                                               bool timed_out);
} modem_provision_step_t;

typedef enum {
    MODEM_WAKE_NONE = 0,
    MODEM_WAKE_DTR_CTS,
} modem_wake_strategy_t;

typedef struct {
    modem_wake_strategy_t strategy;
    uint32_t awake_window_ms;
    bool probe_bypass_cts;
    /* Some modules emit the buffered URC only after their active-low RI pulse
     * has completed. Nonzero waits for RI release before asserting DTR, with
     * this value as the persistent/stuck-low fail-awake bound. */
    uint32_t ri_release_timeout_ms;
    uint32_t dtr_wake_timeout_ms;
    modem_setting_persistence_t persistence;
} modem_wake_cfg_t;

/* Vendor policy needed to wake a sleeping transport for incoming SMS. A
 * backend may expose either:
 * - a runtime arm command, whose successful final qualifies the current power
 *   session and whose invalidating commands require another arm; or
 * - a SIM-completion lifecycle, whose final verified provisioning row
 *   qualifies the session before DTR sleep. An invalidating command schedules
 *   that lifecycle again instead of issuing a post-transition arm.
 *
 * These strategies are deliberately distinct. Some modems sample their URC
 * wake policy while entering a functional level, so issuing an arm command
 * afterward is not equivalent and can invalidate an otherwise working setup. */
typedef struct {
    const char *arm_cmd;
    uint32_t timeout_ms;
    uint8_t retry_limit;
    /* Identifies commands that invalidate the current qualification. */
    bool (*command_invalidates_arm)(const char *cmd);
    /* Some modems sample a saved URC/RI profile only while entering their sleep
     * functional level. For those, qualification comes from a provisioning row
     * instead of a command sent after the transition. */
    bool qualified_by_sim_completion;
    /* Keep DTR awake after the first radio/SIM activation long enough for the
     * readiness URC that starts the focused completion pass. */
    uint32_t sim_activation_window_ms;
} modem_sms_wake_cfg_t;

/* Some backends mutate REC UNREAD to REC READ merely by listing or reading a
 * stored message. The generic SMS service brackets metadata scans with the
 * preserve command and temporarily selects consume mode only for a body the
 * user explicitly opens. NULL commands retain the backend's native behavior. */
typedef struct {
    const char *preserve_unread_cmd;
    const char *consume_unread_cmd;
    uint32_t timeout_ms;
} modem_sms_read_status_cfg_t;

/* modem_call_event_kind_t / modem_call_event_t are the §16.12 normalized call-event
 * kinds — moved to the neutral services/call_types.h (included above) so the vendor
 * adapter shares them with the model without a vendor->model dependency. */

#define MODEM_CALL_CAPABILITY(kind) (1u << (unsigned)(kind))
#define MODEM_CALL_CAPABILITY_ALL \
    ((1u << ((unsigned)CALL_TXN_HANGUP + 1u)) - 2u)

/* Vendor-owned call-control syntax, capabilities, transport deadlines, and
 * model timing. Builders return false for an unsupported/invalid operation or
 * when the caller's output buffer is too small. DTMF remains outside the leg
 * transaction model, but its wire command and deadline are vendor-owned here. */
typedef struct {
    call_timing_t timing;
    uint32_t capabilities;       /* MODEM_CALL_CAPABILITY(CALL_TXN_*) */
    const char *clcc_cmd;
    uint32_t clcc_timeout_ms;
    bool progress_finals_may_complete_command;
    bool (*build_command)(call_txn_kind_t kind, const char *number, uint8_t target_id,
                          char *out, size_t out_cap, uint32_t *timeout_ms);
    bool (*build_dtmf_command)(char symbol, char *out, size_t out_cap,
                               uint32_t *timeout_ms);
} modem_call_control_t;

#define MODEM_CALL_FORWARD_REASON(reason) (1u << (unsigned)(reason))
#define MODEM_CALL_FORWARD_MAX_STEPS 2u

/* Supplementary-service protocol boundary. Call-forwarding, SIM-mailbox, and
 * message-waiting syntax/parsing are vendor-owned; the service receives only
 * normalized 3GPP semantics. A logical request may require more than one AT
 * command on a particular backend. step_count returns zero for an invalid or
 * unsupported request; every returned step must build successfully before the
 * service admits the transaction. */
typedef struct {
    bool supported;
    uint32_t reason_mask;
    uint32_t command_timeout_ms;
    const char *call_forward_response_prefix;
    uint8_t (*call_forward_step_count)(
        const call_forward_request_t *request);
    bool (*build_call_forward_step)(const call_forward_request_t *request,
                                    uint8_t step_index,
                                    char *out, size_t out_cap);
    bool (*parse_call_forward_row)(const char *line,
                                   call_forward_row_t *out);
    const char *voice_mailbox_query_cmd;
    const char *voice_mailbox_response_prefix;
    uint32_t voice_mailbox_timeout_ms;
    bool (*parse_voice_mailbox_row)(const char *line,
                                    modem_voice_mailbox_row_t *out);
    const char *message_waiting_query_cmd;
    const char *message_waiting_response_prefix;
    uint32_t message_waiting_timeout_ms;
    modem_message_waiting_row_result_t (*parse_message_waiting_row)(
        const char *line, modem_aux_event_t *out);
} modem_supplementary_vendor_t;

typedef enum {
    MODEM_VENDOR_CAP_VOICE_TRANSPORT = 1u << 0,
    MODEM_VENDOR_CAP_DTR_SLEEP = 1u << 1,
} modem_vendor_capability_t;

typedef struct {
    const char *query_cmd;
    const char *response_prefix;
    uint32_t timeout_ms;
    bool (*parse_response)(const char *line, modem_signal_sample_t *out);
} modem_signal_query_t;

typedef struct modem_vendor {
    bool available;
    uint32_t capabilities;       /* modem_vendor_capability_t */
    const char *name;
    modem_power_cfg_t power;
    /* Optional immediate module-on classifier. GPIO/ADC sampling stays in the
     * HAL and arrives only through modem_power_observation_t. */
    bool (*power_is_on)(const modem_power_observation_t *observation);
    void (*off_sense_begin)(uint32_t now_ms);      /* begin qualified-OFF window */
    bool (*off_complete)(uint32_t now_ms,
                         const modem_power_observation_t *observation);
    const modem_init_step_t *init_steps;
    uint8_t init_step_count;
    modem_sim_observation_t (*parse_sim_observation)(const char *line);
    const modem_provision_step_t *provision_steps;
    uint8_t provision_step_count;
    uint16_t provision_schema_version;
    const char *provision_reboot_cmd;
    uint32_t provision_reboot_timeout_ms;
    modem_wake_cfg_t wake;
    modem_sms_wake_cfg_t sms_wake;
    modem_sms_read_status_cfg_t sms_read_status;
    const char *call_urc_prefix;
    bool (*parse_call_urc)(const char *line, modem_call_event_t *out);
    bool (*parse_clcc_row)(const char *line, modem_clcc_row_t *out);
    modem_call_control_t call;
    modem_supplementary_vendor_t supplementary;
    modem_signal_query_t signal_query;
    /* Diagnostic parse/finalize callbacks run while the generic service holds
     * its status-publication lock. They must remain pure protocol functions:
     * no modem_service_* calls, lock acquisition, or AT transmission. */
    const modem_diag_query_t *diag_queries;
    uint8_t diag_query_count;
    /* Final validation/projection after every command in a group succeeded.
     * May derive backend policy such as the expected RF throw. */
    modem_diag_group_finish_fn diag_group_finish;
    modem_maintenance_backend_t maintenance;
    const char *const *aux_urc_prefixes;
    uint8_t aux_urc_prefix_count;
    /* Validates and, when semantically relevant, normalizes a line matching
     * aux_urc_prefixes. A valid informational line returns kind NONE. */
    bool (*parse_aux_urc)(const char *line, modem_aux_event_t *out);
} modem_vendor_t;

extern const modem_vendor_t g_modem_vendor;

#endif

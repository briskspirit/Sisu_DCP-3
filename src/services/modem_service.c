#include "services/modem_service.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "services/log.h"
#include "hal/board.h"
#include "hal/modem_power_monitor_hal.h"
#include "hal/modem_uart_hal.h"
#include "hal/accessory_hal.h"
#include "services/timebase.h"
#include "audio/nau88c22_codec.h"
#include "services/core1_services.h"
#include "services/modem_at_util.h"
#include "services/modem_call_model.h"
#include "services/modem_diag_engine.h"
#include "services/modem_line_framer.h"
#include "services/modem_line_parser.h"
#include "services/modem_phonebook_state.h"
#include "services/modem_sms_direct.h"
#include "services/operator_name_db.h"
#include "modem_phonebook_protocol_internal.h"
#include "modem_sms_protocol_internal.h"
#include "modem_sms_state_internal.h"
#include "services/modem_supplementary_state.h"
#include "services/shared_3v8_service.h"
#include "storage/store_service.h"
#include "services/modem_vendor.h"
#include "pico/sync.h"

#if defined(SISU_MODEM_SERVICE_TEST)
#include "modem_service_test.h"
#endif

/* Keep the pre-extraction short call-site names; the shared AT-parsing helpers
 * now live (static inline) in modem_at_util.h. Aliases stay in the .c, never in
 * a header. */
#define starts_with        modem_at_starts_with
#define copy_bounded       modem_at_copy_bounded

#define MODEM_RX_CHUNK 128u
#define MODEM_RX_TICK_BUDGET 1024u
#define MODEM_PING_TIMEOUT_MS 800u
#define MODEM_PING_RETRY_MS 1500u
/* Rail-off dwell before a pending restart. An immediate re-enable is only a
 * supply blip; 2 s guarantees the module and its bulk capacitance discharge. */
#define MODEM_RAIL_OFF_DWELL_MS 2000u
#define MODEM_RAIL_OFF_POLL_MS 20u
#define MODEM_POWER_OBSERVATION_MS 250u
/* Power-sequence timings (rail settle, PWR pulse width, ready budget) + the
 * off-command/timeouts live in g_modem_vendor.power. */
#define MODEM_POST_READY_SETTLE_MS 500u
/* Init command timeout/retry policy is per-step vendor data. In particular,
 * Some non-abortable SMS commands need a longer first-attempt budget, while
 * profile/NVM steps need different policy and prerequisites. */
/* Standby cadences: +CEREG=1 reports only registration transitions, while
 * typed #RFSTS samples provide signal quality. The periodic queries below are
 * slow backstops; foreground display sampling remains intentionally faster. */
#define MODEM_SIGNAL_ACTIVE_MS 1000u
#define MODEM_SIGNAL_BACKSTOP_MS 60000u
#define MODEM_CEREG_BACKSTOP_MS 60000u

/* The selected backend owns its transport wake strategy: either a tracked TX
 * poke after command-driven idle or a physical DTR/CTS handshake. */
#define MODEM_SIM_PROVIDER_RETRY_MS 60000u
#define MODEM_SIM_PROVIDER_ATTEMPTS 2u
#define MODEM_SIM_PROVIDER_DRAIN_MS 5000u
#define MODEM_CPMS_POLL_PERIOD_MS 300000u /* receive-store fullness backstop; +CMTI re-checks now */
#define MODEM_SMS_SETUP_RETRY_MS 3000u   /* deferred CSMP/CPMS retry cadence until they stick */
#define MODEM_SMS_WAKE_RETRY_DELAY_MS 250u
#define MODEM_SMS_WAKE_BACKGROUND_RETRY_MS 3000u
#define MODEM_REQUEST_QUEUE_LEN 6u
#define MODEM_SMS_SUB_CHAR 0x1au
#define MODEM_SMS_ESC_CHAR 0x1bu /* ESC: cancels an open '>' body prompt */
/* After an ESC prompt-abort, hold off the next scheduled command this long so
 * ESC's own abort response (the module returns to command mode, possibly with an
 * OK/ERROR) drains as a harmless orphan before any real command is in flight. */
#define MODEM_SMS_ESC_SETTLE_MS 500u
_Static_assert(MODEM_SMS_RECORD_MAX <= UINT8_MAX,
               "SMS mailbox counts use uint8_t");
_Static_assert(MODEM_SMS_SEGMENT_MAX <= 8u,
               "multipart completion masks use one byte");
_Static_assert(MODEM_PHONEBOOK_RESULT_CAPACITY >=
                   MODEM_REQUEST_QUEUE_LEN + 1u,
               "phonebook journal must hold the current request plus the FIFO");

typedef enum {
    MODEM_STATE_PROBE = 0,
    MODEM_STATE_OFF,
    /* Shutdown acknowledged (or timed out): VCC stays up until Telit PWRMON
     * qualifies internal switch-off. A timeout follows the backend's explicit
     * retain-or-cut policy and never invents an OFF observation. */
    MODEM_STATE_OFF_DISCHARGE,
    MODEM_STATE_RAIL_WAIT,
    MODEM_STATE_POWER_PULSE,
    MODEM_STATE_MODULE_WAIT,
    MODEM_STATE_INIT,
    MODEM_STATE_PROVISION,
    MODEM_STATE_READY,
    MODEM_STATE_FAILED,
} modem_state_t;

typedef enum {
    MODEM_SHUTDOWN_STAGE_NONE = 0,
    MODEM_SHUTDOWN_STAGE_SOFTWARE,
    MODEM_SHUTDOWN_STAGE_GRACEFUL_CONTROL,
    MODEM_SHUTDOWN_STAGE_UNCONDITIONAL,
    MODEM_SHUTDOWN_STAGE_TERMINAL_FAILURE,
} modem_shutdown_stage_t;

typedef enum {
    MODEM_SHUTDOWN_START_UNSUPPORTED = 0,
    MODEM_SHUTDOWN_START_OK,
    MODEM_SHUTDOWN_START_FAILED,
} modem_shutdown_start_t;

typedef enum {
    MODEM_AT_NONE = 0,
    MODEM_AT_PING,
    MODEM_AT_INIT,
    MODEM_AT_PROVISION_QUERY,
    MODEM_AT_PROVISION_SET,
    MODEM_AT_PROVISION_REBOOT,
    MODEM_AT_CSQ,
    MODEM_AT_SIGNAL,
    MODEM_AT_SMS_CPMS_POLL, /* periodic AT+CPMS? to detect a full receive store */
    MODEM_AT_SMS_MODE_RESTORE, /* idle AT+CMGF=1 after a cancelled op may have left PDU mode */
    MODEM_AT_SMS_SETUP_CSMP, /* deferred SMS setup retry (CSMP), init timing self-heal */
    MODEM_AT_SMS_SETUP_CPMS, /* deferred SMS setup retry (CPMS -> ME storage) */
    MODEM_AT_SMS_WAKE_ARM, /* transient vendor command proving SMS can wake DTR sleep via RI */
    MODEM_AT_CEREG,
    MODEM_AT_COPS, /* reserved diagnostic kind; standby uses serving PLMN */
    MODEM_AT_CLCC_MODEL,     /* typed all-leg reconcile for the id-authoritative model */
    MODEM_AT_CALL_DIAL,
    MODEM_AT_CALL_ANSWER,
    MODEM_AT_CALL_HANGUP,
    MODEM_AT_CALL_DTMF,
    MODEM_AT_CALL_SUPPLEMENTARY,
    MODEM_AT_CALL_FORWARD,
    MODEM_AT_VOICE_MAILBOX,
    MODEM_AT_MESSAGE_WAITING,
    MODEM_AT_SMS_CPMS,
    MODEM_AT_SMS_STATUS_PRESERVE,
    MODEM_AT_SMS_STATUS_CONSUME,
    MODEM_AT_SMS_CMGL,
    MODEM_AT_SMS_CMGR,
    MODEM_AT_SMS_CMGD,
    MODEM_AT_SMS_CMGF_PDU,
    MODEM_AT_SMS_CMGF_TEXT,
    MODEM_AT_SMS_CMGS_PROMPT,
    MODEM_AT_SMS_CMGS_FINAL,
    MODEM_AT_SMS_CMGW_PROMPT,
    MODEM_AT_SMS_CMGW_FINAL,
    MODEM_AT_PHONEBOOK_CPBS,
    MODEM_AT_PHONEBOOK_CPBR,
    MODEM_AT_PHONEBOOK_CPBW,
    MODEM_AT_DIAG_QUERY,        /* one Net Monitor v2 query; always yields on final */
    MODEM_AT_MAINTENANCE,
    MODEM_AT_POWER_OFF,
    MODEM_AT_DEBUG,
    MODEM_AT_CALL_FORWARD_FLAGS,
    MODEM_AT_SIM_PROVIDER,
    MODEM_AT_SIM_PROVIDER_DRAIN,
} modem_at_kind_t;

typedef enum {
    MODEM_PROVISION_QUERY = 0,
    MODEM_PROVISION_SET,
    MODEM_PROVISION_VERIFY,
} modem_provision_phase_t;

typedef enum {
    MODEM_OP_NONE = 0,
    MODEM_OP_DIAL,
    MODEM_OP_ANSWER,
    MODEM_OP_HANGUP,
    MODEM_OP_DTMF,
    MODEM_OP_CALL_SUPPLEMENTARY,
    MODEM_OP_CALL_FORWARD,
    MODEM_OP_VOICE_MAILBOX,
    MODEM_OP_MESSAGE_WAITING,
    MODEM_OP_SEND_SMS,
    MODEM_OP_SEND_BINARY_SMS,
    MODEM_OP_SAVE_SMS,
    MODEM_OP_SMS_MAILBOX,
    MODEM_OP_SMS_READ,
    MODEM_OP_DELETE_SMS,
    MODEM_OP_STORE_DELIVERED_SMS, /* internal: re-store a +CMT as a PDU */
    MODEM_OP_PHONEBOOK_LIST,
    MODEM_OP_PHONEBOOK_ADD,
    MODEM_OP_PHONEBOOK_UPDATE,
    MODEM_OP_PHONEBOOK_DELETE,
    MODEM_OP_DEBUG_AT,
} modem_operation_t;

typedef enum {
    MODEM_REQ_DIAL = 0,
    MODEM_REQ_ANSWER,
    MODEM_REQ_HANGUP,
    MODEM_REQ_DTMF,
    MODEM_REQ_WAITING_ANSWER,
    MODEM_REQ_WAITING_REJECT,
    MODEM_REQ_CALL_SWAP,
    MODEM_REQ_CALL_HOLD,
    MODEM_REQ_RELEASE_ACTIVE,
    MODEM_REQ_RELEASE_LEG, /* release a specific call id; id in .index */
    MODEM_REQ_CALL_FORWARD,
    MODEM_REQ_VOICE_MAILBOX,
    MODEM_REQ_MESSAGE_WAITING,
    MODEM_REQ_SEND_SMS,
    MODEM_REQ_SEND_BINARY_SMS,
    MODEM_REQ_SAVE_SMS,
    MODEM_REQ_SMS_MAILBOX,
    MODEM_REQ_SMS_READ,
    MODEM_REQ_DELETE_SMS,
    MODEM_REQ_STORE_DELIVERED_SMS, /* internal: queued by the direct ring */
    MODEM_REQ_PHONEBOOK_LIST,
    MODEM_REQ_PHONEBOOK_ADD,
    MODEM_REQ_PHONEBOOK_UPDATE,
    MODEM_REQ_PHONEBOOK_DELETE,
    MODEM_REQ_DEBUG_AT,
    MODEM_REQ_DEBUG_BACKGROUND_POLLING,
} modem_request_type_t;

typedef struct {
    modem_request_type_t type;
    uint32_t call_token;     /* 0 for non-call requests; model correlation otherwise */
    uint32_t request_id;     /* async-result correlation; 0 = internal/no owner */
    call_forward_request_t call_forward;
    uint32_t sms_identity_hash;
    bool sms_quarantined;
    uint16_t index;
    char number[MODEM_PHONE_MAX + 1u];
    char name[MODEM_PHONEBOOK_NAME_MAX + 1u];
    char text[MODEM_SMS_TEXT_MAX + 1u];
    uint8_t binary[MODEM_SMS_BINARY_MAX];
    uint16_t binary_len;
    uint16_t dest_port;
    uint16_t source_port;
    uint8_t binary_mode;
    uint8_t index_count;
    uint16_t indices[MODEM_SMS_SEGMENT_MAX];
} modem_request_t;

typedef enum {
    SMS_WAKE_RESUME_NONE = 0,
    SMS_WAKE_RESUME_OPERATION_CPMS,
    SMS_WAKE_RESUME_DEFERRED_SETUP,
} sms_wake_resume_t;

/* modem_degrade_t + modem_init_step_t are declared in modem_vendor.h; the AT
 * init ladder is supplied by g_modem_vendor.init_steps / .init_step_count. */

static void drain_rx(void);
static void feed_byte(uint8_t byte);
static void process_line(char *line);
static void process_prompt(void);
static void route_urc(const char *line);
static void process_timeout(uint32_t now_ms);
static void advance_state(uint32_t now_ms);
static void start_next_request(uint32_t now_ms);
static void send_command(modem_at_kind_t kind, const char *cmd, uint32_t timeout_ms, uint32_t now_ms);
static void send_raw_command(modem_at_kind_t kind, const char *cmd, uint32_t timeout_ms, uint32_t now_ms);
static bool send_model_command(modem_at_kind_t kind, const char *cmd, uint32_t timeout_ms,
                               uint32_t token, uint32_t now_ms);
static bool send_command_with_token(modem_at_kind_t kind, const char *cmd,
                                    uint32_t timeout_ms, uint32_t token, uint32_t now_ms);
static bool send_raw_command_with_token(modem_at_kind_t kind, const char *cmd,
                                        uint32_t timeout_ms, uint32_t token, uint32_t now_ms);
static bool command_bypasses_cts(modem_at_kind_t kind);
static void send_deferred(uint32_t now_ms);
static void finish_command_result(bool ok, const char *line);
static void handle_ping_failed(uint32_t now_ms);
static void modem_enter_ready(uint32_t now_ms);
static void modem_begin_provision(uint32_t now_ms);
static void provision_advance(uint32_t now_ms);
static void provision_handle_final(bool ok, bool timeout, uint32_t now_ms);
static void provision_begin_reboot_wait(uint32_t now_ms);
static void provision_note_reboot_drop(uint32_t now_ms);
static void modem_begin_reinit_wait(uint32_t now_ms);
static void modem_recover_startup_restart(uint32_t now_ms);
static uint8_t init_retry_limit(void);
static bool init_step_is_sim_dependent(const modem_init_step_t *step);
static bool init_step_capture_satisfied(const modem_init_step_t *step);
static bool init_step_requires_response(const modem_init_step_t *step);
static bool init_step_capture_line(const modem_init_step_t *step,
                                   const char *line);
static bool provision_step_is_sim_dependent(
    const modem_provision_step_t *step);
static bool init_step_prerequisites_met(void);
static bool init_step_recoverable(void);
static void note_init_step_skipped(void);
static void modem_begin_sim_completion(uint32_t now_ms);
static bool at_kind_is_operation(modem_at_kind_t kind);
static void diag_cancel_active_group(void);
static bool diag_start_next_command(uint32_t now_ms);
static bool diag_parse_expected_line(const char *line);
static void diag_finish_query(bool ok, bool timed_out, uint32_t now_ms);
static void diag_invalidate_all(uint32_t now_ms);
static bool parse_expected_line(const char *line);
static bool parse_final_result(const char *line, bool *ok);
static bool is_call_progress_final(const char *line);
static bool is_known_urc_line(const char *line);
static bool is_vendor_call_urc(const char *line);
static bool is_vendor_aux_urc(const char *line);
static bool observe_vendor_aux_line(const char *line);
static void apply_aux_event(const modem_aux_event_t *event);
static void finish_operation(bool ok);
static void set_operation_busy(bool busy);
static void modem_bridge_follow_call_state(void);
static bool dtmf_char_valid(char symbol);
static bool dtmf_call_active(void);
static bool dtmf_send_current(uint32_t now_ms);
static void dtmf_finish_digit(bool ok, uint32_t now_ms);
static bool queue_request(const modem_request_t *request);
static bool pop_request(modem_request_t *request);
static void cancel_request(const modem_request_t *request,
                           modem_phonebook_outcome_t phonebook_outcome);
static void cancel_request_session(void);
static bool model_queue_call_request(modem_request_t *request, bool front);
static bool model_start_pending_release(uint32_t now_ms);
static void model_project_publish(void);
static uint32_t model_transport_counter(void);
static void model_cancel_deferred_command(void);
static bool call_control_pending(void);
static bool call_session_idle_snapshot(void);
static bool sms_protocol_request_view(modem_sms_protocol_request_t *out);
static bool sms_protocol_command_from_at(modem_at_kind_t at_kind,
                                         modem_sms_command_kind_t *out);
static modem_at_kind_t sms_protocol_command_to_at(
    modem_sms_command_kind_t kind);
static bool sms_protocol_emit(const modem_sms_protocol_action_t *action);
static bool sms_protocol_call_preempt_pending(void);
static bool sms_start_current_request(uint32_t now_ms);
static const modem_sms_protocol_hooks_t s_sms_protocol_hooks;
static bool sms_request_kind_from_request_type(
    modem_request_type_t type, modem_sms_request_kind_t *out);
static bool queue_sms_request(modem_request_t *request,
                              modem_sms_request_kind_t kind,
                              uint32_t *request_id_out);
static bool sms_publish_terminal(
    uint32_t request_id, modem_sms_request_kind_t kind,
    modem_sms_outcome_t outcome, bool sim_not_ready,
    modem_sms_mailbox_t mailbox, uint32_t expected_identity_hash);
static bool sms_publish_terminal_for_request(
    const modem_request_t *request, modem_sms_outcome_t outcome);
static void sms_cancel_protocol(void);
static void direct_feed_line(const char *line);
static void direct_feed_raw(uint8_t byte);
static bool direct_ring_start(uint32_t now_ms);
static void direct_ring_reset(void);
static void direct_store_unblock(const char *why);
static void direct_body_deadline_tick(uint32_t now_ms);
static bool sms_mode_restore_start(uint32_t now_ms);
static bool phonebook_protocol_request_view(
    modem_phonebook_protocol_request_t *out);
static bool phonebook_protocol_command_from_at(
    modem_at_kind_t at_kind, modem_phonebook_command_kind_t *out);
static modem_at_kind_t phonebook_protocol_command_to_at(
    modem_phonebook_command_kind_t kind);
static bool phonebook_protocol_emit(
    const modem_phonebook_protocol_action_t *action);
static bool phonebook_start_current_request(uint32_t now_ms);
static void phonebook_clear_cache(void);
static const modem_phonebook_protocol_hooks_t s_phonebook_protocol_hooks;
static bool phonebook_operation_from_request_type(
    modem_request_type_t type, modem_phonebook_op_t *out);
static bool queue_phonebook_request(modem_request_t *request,
                                    uint32_t *request_id_out);
static void phonebook_cancel_protocol(void);
static void sms_continue_after_cpms(uint32_t now_ms);
static void sms_complete_deferred_setup(void);
static bool sms_wake_required(void);
static bool sms_wake_runtime_arm_available(void);
static void sms_wake_reset_session(void);
static void sms_wake_command_dispatched(const char *cmd, uint32_t now_ms);
static void sms_request_wake_qualified_continuation(
    sms_wake_resume_t resume, uint32_t now_ms);
static void sms_wake_handle_final(bool ok, uint32_t now_ms);
static bool sms_wake_retry_due(uint32_t now_ms);
static void sms_wake_send_arm(uint32_t now_ms);
static void sms_begin_prompt_abort_settle(uint32_t now_ms);
static void push_debug_result(bool ok);
static void phonebook_finish_operation(uint32_t request_id,
                                       modem_phonebook_op_t kind,
                                       modem_phonebook_outcome_t outcome);
static void phonebook_push_result(uint32_t request_id,
                                  modem_phonebook_op_t kind,
                                  modem_phonebook_outcome_t outcome);
static bool is_final_text(const char *line);
static bool is_call_progress_final(const char *line);
static bool is_call_at_kind(modem_at_kind_t kind);
static void parse_csq(const char *line);
static bool signal_query_supported(void);
static bool signal_query_due(uint32_t now_ms);
static bool signal_start_query(uint32_t now_ms);
static void signal_finish_query(bool command_ok, uint32_t now_ms);
static void signal_invalidate_locked(uint32_t now_ms);
static void parse_sms_cpms(const char *line);
static void parse_cereg(const char *line);
static void operator_name_publish_locked(void);
static void sim_provider_reset(void);
static bool sim_provider_start_query(uint32_t now_ms);
static void sim_provider_finish_query(bool ok);
static bool parse_cpin(const char *line);
static bool is_sim_absent_error(const char *line);
static void apply_sim_observation(modem_sim_observation_t observation);
static void sim_maintenance_guard_hold_cfun4(void);
static void sim_maintenance_guard_begin_online(uint32_t now_ms);
static void sim_maintenance_guard_tick(uint32_t now_ms);
static bool parse_cmti_index(const char *line, uint16_t *index_out);
static bool is_ccwa_urc(const char *line);
static bool status_sim_ready_snapshot(void);
static bool status_sim_missing_snapshot(void);
static void set_status_ready(bool ready);
static void modem_enter_off(void);
static void modem_enter_module_wait(uint32_t now_ms);
static void modem_recover_retained_module(uint32_t now_ms);
static void modem_begin_power_on(uint32_t now_ms);
static void modem_begin_power_off(uint32_t now_ms);
static void modem_begin_off_discharge(uint32_t now_ms);
static void modem_start_power_pulse(uint32_t now_ms);
static void modem_start_supply_fault_shutdown(uint32_t now_ms);
static modem_shutdown_start_t modem_start_shutdown_fallback(
    modem_shutdown_stage_t stage, uint32_t now_ms);
static void modem_terminal_shutdown_failure(const char *reason,
                                            uint32_t now_ms);
static void modem_reset_shutdown_escalation(void);
static modem_power_observation_t modem_power_observation(void);
static void modem_transport_start_dtr_wake(uint32_t now_ms);
static void modem_transport_tick(uint32_t now_ms);
static void modem_transport_idle(uint32_t now_ms);
static void modem_fail_before_dispatch(uint32_t now_ms);
static void modem_fail_power_state(const char *reason, bool module_may_be_live);
static void modem_fail_supply_power_state(const char *reason,
                                          bool module_may_be_live);
static void modem_runtime_power_tick(uint32_t now_ms);
static bool maintenance_start_next_command(uint32_t now_ms);
static bool maintenance_parse_expected_line(const char *line);
static void maintenance_finish_command(bool ok, bool timed_out,
                                       uint32_t now_ms);
static void maintenance_cancel_for_call(void);
static void maintenance_check_recovery_record(void);
static void maintenance_reset_session(void);
static bool maintenance_write_recovery_record(const char *record);
static bool maintenance_flush_recovery_record(void);
static bool call_forward_request_valid(const call_forward_request_t *request,
                                       uint8_t *step_count_out);
static void call_forward_parse_row(const char *line);
static void call_forward_finish_step(bool ok, bool timed_out);
static void call_forward_finish(bool ok, bool timed_out);
static void call_forward_push_result(uint32_t request_id,
                                     const call_forward_request_t *request,
                                     call_forward_outcome_t outcome,
                                     bool status_known, bool active,
                                     const char *number, bool has_delay,
                                     uint8_t delay_seconds);
static void voice_mailbox_parse_row(const char *line);
static void voice_mailbox_finish(bool ok);
static void message_waiting_parse_row(const char *line);
static void message_waiting_finish(bool ok);
static bool supplementary_start_background(uint32_t now_ms);
static void supplementary_project_status_locked(void);

/* Switch-off sense (the vendor decides "module is done, cut the rail") lives in
 * g_modem_vendor.off_complete / .off_sense_begin. The core owns the finite
 * command -> graceful-control -> unconditional-control escalation sequence,
 * using only the neutral timing/capability data in g_modem_vendor.power. */

static critical_section_t s_status_lock;
static critical_section_t s_request_lock;
static critical_section_t s_sms_lock;
static critical_section_t s_phonebook_lock;
static modem_status_t s_status;
static modem_diag_engine_t s_diag_engine;
_Static_assert(sizeof(modem_diag_snapshot_t) <= 1616u,
               "modem diagnostic snapshot exceeded its copy budget");
_Static_assert(sizeof(modem_call_snapshot_t) <= 160u,
               "call snapshot exceeded the app/service copy budget");
static call_model_t s_call_model;
static call_projection_t s_call_projection;
static modem_call_snapshot_t s_call_snapshot;
static bool s_bridge_active_wanted;
static uint32_t s_active_call_token;
static uint32_t s_deferred_call_token;
static modem_line_framer_t s_line_framer;
static uint32_t s_line_drop_count; /* service integrity counter, not framing */
static uint32_t s_model_malformed_clcc;
static uint8_t s_request_high_water;
static uint32_t s_request_admission_failures;
static uint32_t s_request_evictions;
static uint32_t s_diag_transport_sleep_entries;
static uint32_t s_diag_transport_wake_attempts;
static uint32_t s_diag_transport_wake_timeouts;
static uint32_t s_diag_transport_ri_release_timeouts;
static uint32_t s_diag_transport_wake_started_ms;
static uint32_t s_diag_transport_last_wake_latency_ms;
static uint32_t s_diag_transport_max_wake_latency_ms;
static uint32_t s_diag_transport_last_account_ms;
static uint32_t s_diag_transport_ready_ms;
static uint32_t s_diag_transport_sleep_requested_ms;
static uint32_t s_diag_transport_sleep_confirmed_ms;
static bool s_debug_background_polling_enabled;
static uint32_t s_diag_active_started_ms;
static uint32_t s_diag_power_on_requests;
static uint32_t s_diag_power_on_starts;
static uint32_t s_diag_ready_entries;
static uint32_t s_diag_power_off_requests;
static uint32_t s_diag_shutdown_completions;
static uint32_t s_diag_graceful_shutdown_pulses;
static uint32_t s_diag_emergency_shutdown_pulses;
static uint32_t s_diag_terminal_shutdown_failures;
static uint32_t s_diag_automatic_recoveries;
static uint32_t s_diag_controlled_restarts;
static uint32_t s_diag_power_failures;
static uint32_t s_diag_last_transition_ms;
static modem_diag_recovery_reason_t s_diag_last_recovery_reason;
static uint32_t s_maintenance_sim_guard_deadline_ms;
static modem_sim_observation_t s_maintenance_sim_deferred;
static volatile bool s_status_lock_ready;
static volatile bool s_request_lock_ready;
static volatile bool s_sms_lock_ready;
static volatile bool s_phonebook_lock_ready;
static modem_state_t s_state;
static modem_at_kind_t s_active_kind;
static modem_operation_t s_operation;
static bool s_active;
static uint32_t s_now_ms;
static uint32_t s_command_deadline_ms;
static uint32_t s_boot_deadline_ms;
static uint32_t s_power_release_ms;
static uint32_t s_rail_power_good_deadline_ms;
static uint32_t s_next_power_observation_ms;
static uint32_t s_next_action_ms;
static uint32_t s_next_signal_ms;
static uint32_t s_next_cereg_ms;
static bool s_operator_refresh_needed;
static uint32_t s_registration_generation;
static uint32_t s_signal_registration_generation;
static uint32_t s_signal_transport_counter;
static uint32_t s_sim_generation;
static uint32_t s_sim_provider_generation;
static uint32_t s_sim_provider_transport_counter;
static uint32_t s_next_sim_provider_ms;
static uint8_t s_sim_provider_attempts;
static bool s_sim_provider_cached;
static bool s_sim_provider_line_seen;
static bool s_sim_provider_line_invalid;
static char s_sim_provider_name[MODEM_OPERATOR_NAME_CAPACITY];
static char s_sim_provider_shadow[MODEM_OPERATOR_NAME_CAPACITY];
static bool s_cfu_flags_row_seen;
static bool s_signal_foreground_sampling;
static bool s_signal_refresh_needed;
static bool s_signal_line_seen;
static bool s_signal_line_invalid;
static modem_signal_sample_t s_signal_shadow;
static bool s_uart_parked;
static bool s_dtr_sleep_permitted;
static bool s_dtr_wake_pending;
static uint32_t s_dtr_wake_deadline_ms;
static bool s_ri_release_pending;
static uint32_t s_ri_release_deadline_ms;
static bool s_failed_module_may_be_live;
static bool s_failed_shutdown_qualification_pending;
static modem_shutdown_stage_t s_shutdown_stage;
static bool s_shutdown_pulse_active;
static uint32_t s_shutdown_pulse_release_ms;
static bool s_power_off_failure_pending;
static bool s_supply_power_failure_pending;
static bool s_failed_supply_power;
static uint32_t s_last_tx_ms;      /* last transport activity: drives the idle-window check */
static bool s_deferred_valid;      /* one-slot stash while DTR wake is in progress */
static modem_at_kind_t s_deferred_kind;
static uint32_t s_deferred_timeout_ms;
/* Sized to the largest command send_command can carry (== request.text) so the
 * DTR-wake stash never truncates a deferred command. */
static char s_deferred_cmd[MODEM_SMS_TEXT_MAX + 1u];
static bool s_sms_storage_full_latched; /* edge detector: notify once per full episode */
/* Core-owned SMS transport continuations. These deliberately remain outside
 * modem_sms_state: they are scheduler deadlines, DTR/RI qualification, and
 * prompt-abort recovery rather than message state. Keep them as individual
 * symbols: grouping them in a struct costs four target BSS bytes because of
 * the neighboring eight-byte linker alignment. */
static uint32_t s_next_cpms_ms;
static uint32_t s_next_sms_setup_ms;
static uint32_t s_sms_wake_retry_ms;
static uint32_t s_sms_wake_activation_deadline_ms;
static sms_wake_resume_t s_sms_wake_resume;
static uint8_t s_sms_wake_retries;
static bool s_cpms_check_needed;
/* A cancelled SMS operation had issued AT+CMGF=0 without a completed
 * AT+CMGF=1: the idle scheduler restores text mode, bounded like a store. */
static bool s_sms_mode_restore_pending;
static uint8_t s_sms_mode_restore_attempts;
#define MODEM_SMS_MODE_RESTORE_ATTEMPTS 3u
static bool s_sms_setup_needed;
static bool s_sms_wake_armed;
static bool s_active_sms_wake_arm;
static bool s_binary_restore_pending;
static bool s_sms_terminal_published;
static uint8_t s_init_index;
static uint8_t s_init_retries;
static bool s_init_parse_seen;
static uint8_t s_provision_index;
static uint8_t s_provision_retries;
static modem_provision_phase_t s_provision_phase;
static bool s_provision_line_seen;
static modem_provision_line_t s_provision_line_result;
static bool s_provision_step_written;
static bool s_provision_reboot_required;
static bool s_provision_rebooted;
static bool s_provision_reboot_cycle_active;
static bool s_provision_reboot_drop_seen;
static bool s_startup_restart_recovered;
static bool s_startup_complete;
static bool s_provision_all_verified;
static bool s_provision_non_sim_verified;
static bool s_sim_completion_needed;
static bool s_sim_completion_pending;
static bool s_sim_completion_active;
static bool s_sim_completion_failed;
static bool s_sim_steps_skipped;
static bool s_power_pulsed;
/* A cold-start probe gets one automatic recovery before the service parks in
 * FAILED and waits for an explicit retry. */
static bool s_startup_recycled;
static uint32_t s_off_discharge_deadline_ms;
static uint32_t s_rail_off_ready_ms;
/* True only after the PHYSICAL shared rail has been observed down and the
 * discharge deadline has been started. A buzzer owner can legitimately keep
 * +3V8 high after the modem releases it; that interval must never count toward
 * a recovery rail-cycle dwell. */
static bool s_rail_off_dwell_started;
static bool s_rail_off_dwell_complete;
static uint32_t s_rail_off_transition_snapshot;
static bool s_power_on_pending; /* power-on requested mid-OFF_DISCHARGE; honored at OFF */
/* Power-off requested after the module start pulse began but before its AT
 * channel became usable. Startup proceeds only far enough to issue the normal
 * vendor shutdown command; the rail is never cut on missing early PWRMON. */
static bool s_power_off_pending;
static modem_request_t s_request_queue[MODEM_REQUEST_QUEUE_LEN];
static uint8_t s_request_head;
static uint8_t s_request_tail;
static uint8_t s_request_count;
static modem_request_t s_current_request;
/* A call-topology URC can arrive while one AT+VTS command is awaiting its
 * final. Let that in-flight digit finish, then suppress the unsent suffix. */
static bool s_dtmf_cancel_requested;
static modem_debug_result_t s_debug_result;
static bool s_debug_result_pending;
static char s_debug_last_command[48];
static char s_debug_last_line[48];
static uint8_t s_rx_trace[MODEM_RX_TRACE_CAPACITY];
static modem_rx_trace_status_t s_rx_trace_status;
static uint16_t s_rx_trace_write;

static bool model_request_kind(modem_request_type_t type, call_txn_kind_t *kind) {
    switch (type) {
    case MODEM_REQ_DIAL:             *kind = CALL_TXN_DIAL;           return true;
    case MODEM_REQ_ANSWER:           *kind = CALL_TXN_ANSWER;         return true;
    case MODEM_REQ_HANGUP:           *kind = CALL_TXN_HANGUP;         return true;
    case MODEM_REQ_WAITING_ANSWER:   *kind = CALL_TXN_WAIT_ANSWER;    return true;
    case MODEM_REQ_WAITING_REJECT:   *kind = CALL_TXN_WAIT_REJECT;    return true;
    case MODEM_REQ_CALL_SWAP:        *kind = CALL_TXN_SWAP;           return true;
    case MODEM_REQ_CALL_HOLD:        *kind = CALL_TXN_HOLD;           return true;
    case MODEM_REQ_RELEASE_ACTIVE:   *kind = CALL_TXN_RELEASE_ACTIVE; return true;
    case MODEM_REQ_RELEASE_LEG:      *kind = CALL_TXN_RELEASE_LEG;    return true;
    default:                         *kind = CALL_TXN_NONE;           return false;
    }
}

/* These operations can create or mutate the foreground call while an
 * abandoned second outgoing leg is still bindable. Defer them until the
 * model-owned cleanup proves that leg absent. Session-wide Hangup and explicit
 * release operations remain available as escape paths. */
static bool model_new_call_cleanup_blocks(call_txn_kind_t kind) {
    return kind == CALL_TXN_DIAL || kind == CALL_TXN_HOLD ||
           kind == CALL_TXN_SWAP || kind == CALL_TXN_WAIT_ANSWER;
}

static const call_leg_t *model_leg_by_id(uint8_t id) {
    for (unsigned i = 0u; i < MODEM_MAX_CALL_LEGS; i++) {
        if (s_call_model.legs[i].in_use && s_call_model.legs[i].id == id) {
            return &s_call_model.legs[i];
        }
    }
    return NULL;
}

static call_txn_t *model_txn_by_token(uint32_t token) {
    if (token == 0u) return NULL;
    for (unsigned i = 0u; i < MODEM_MAX_CALL_TRANSACTIONS; i++) {
        if (s_call_model.txns[i].in_use && s_call_model.txns[i].token == token) {
            return &s_call_model.txns[i];
        }
    }
    return NULL;
}

static uint8_t model_request_target(const modem_request_t *request,
                                    call_txn_kind_t kind) {
    if (request == NULL) return 0u;
    return (kind == CALL_TXN_RELEASE_ACTIVE || kind == CALL_TXN_RELEASE_LEG)
         ? (uint8_t)request->index : 0u;
}

static bool model_build_call_request(const modem_request_t *request,
                                     call_txn_kind_t kind,
                                     char *command, size_t command_cap,
                                     uint32_t *timeout_ms) {
    if (kind <= CALL_TXN_NONE || kind > CALL_TXN_HANGUP ||
        (g_modem_vendor.call.capabilities &
         MODEM_CALL_CAPABILITY(kind)) == 0u) {
        return false;
    }
    const char *number = kind == CALL_TXN_DIAL ? request->number : NULL;
    uint8_t target = model_request_target(request, kind);
    return g_modem_vendor.call.build_command != NULL &&
           g_modem_vendor.call.build_command(kind, number, target,
                                             command, command_cap, timeout_ms) &&
           *timeout_ms != 0u;
}

static bool model_has_published_foreground(void) {
    /* The projector, not a raw leg state, decides whether a foreground call is
     * established. In particular, a dir-unconfirmed tentative ACTIVE leg still
     * projects DIALING/id=0, while a RELEASING leg can retain its published
     * ACTIVE role until authoritative absence. */
    return s_call_model.published.active_call_id != 0u;
}

static void model_cancel_token(uint32_t token) {
    if (token != 0u) {
        call_txn_t *txn = model_txn_by_token(token);
        const call_leg_t *leg = txn != NULL ? model_leg_by_id(txn->target_id) : NULL;
        bool restore_release = txn != NULL && !txn->ever_dispatched &&
                               (txn->kind == CALL_TXN_RELEASE_ACTIVE ||
                                txn->kind == CALL_TXN_RELEASE_LEG) &&
                               leg != NULL &&
                               leg->generation == txn->target_gen &&
                               (leg->state == CALL_LEG_RELEASING || leg->pending_removal);
        call_cleanup_release_t release = {0};
        if (restore_release) {
            release.id = txn->target_id;
            release.generation = txn->target_gen;
            release.recover_held_survivor =
                txn->kind == CALL_TXN_RELEASE_ACTIVE;
        }
        call_model_txn_cancel(&s_call_model, token);
        /* A model cleanup release may have transferred its FIFO obligation to
         * this queued transaction. If that request is evicted before any bytes
         * are written, put the same still-live generation back on the FIFO. */
        if (restore_release &&
            !call_model_requeue_release(&s_call_model, &release)) {
            LOGE("modem", "lost cancelled release retry for call id %u",
                 (unsigned)release.id);
        }
    }
}

static void model_cancel_request(const modem_request_t *request) {
    if (request != NULL) {
        model_cancel_token(request->call_token);
    }
}

static void model_cancel_deferred_command(void) {
    uint32_t token = s_deferred_call_token;
    s_deferred_valid = false;
    s_deferred_call_token = 0u;
    if (token != 0u) {
        modem_request_t request;
        memset(&request, 0, sizeof(request));
        request.call_token = token;
        model_cancel_request(&request);
    }
}

static bool model_queue_call_request(modem_request_t *request, bool front) {
    if (!g_modem_vendor.available || request == NULL || !s_request_lock_ready) {
        return false;
    }

    call_txn_kind_t kind;
    if (!model_request_kind(request->type, &kind)) return false;
    char capability_command[MODEM_PHONE_MAX + 32u];
    uint32_t capability_timeout_ms = 0u;
    if (!model_build_call_request(request, kind, capability_command,
                                  sizeof(capability_command), &capability_timeout_ms)) {
        return false; /* unsupported by this modem: reject before model/UI admission */
    }
    call_model_set_now(&s_call_model, time_ms());
    uint8_t target = model_request_target(request, kind);
    bool second_mo = kind == CALL_TXN_DIAL && model_has_published_foreground();
    modem_request_t evicted;
    memset(&evicted, 0, sizeof(evicted));
    bool dropped = false;

    /* Allocation + queue admission is one critical section. A normal-priority
     * request observes a full queue before call_model_request can mutate model
     * ownership/latches; a front request atomically captures the exact evicted
     * token. Thus there is no "allocated but failed to enqueue" rollback window. */
    critical_section_enter_blocking(&s_request_lock);
    if (!front && s_request_count >= MODEM_REQUEST_QUEUE_LEN) {
        s_request_admission_failures++;
        critical_section_exit(&s_request_lock);
        return false;
    }
    if (model_new_call_cleanup_blocks(kind) &&
        call_model_new_call_cleanup_pending(&s_call_model, NULL)) {
        critical_section_exit(&s_request_lock);
        return false;
    }
    if (kind == CALL_TXN_HOLD &&
        !call_model_hold_toggle_available(&s_call_model)) {
        critical_section_exit(&s_request_lock);
        return false;
    }
    uint32_t token = call_model_request(&s_call_model, kind, target, second_mo);
    if (token == 0u) {
        critical_section_exit(&s_request_lock);
        return false;
    }
    request->call_token = token;
    if (front) {
        if (s_request_count >= MODEM_REQUEST_QUEUE_LEN) {
            s_request_tail = (uint8_t)((s_request_tail + MODEM_REQUEST_QUEUE_LEN - 1u) %
                                       MODEM_REQUEST_QUEUE_LEN);
            evicted = s_request_queue[s_request_tail];
            s_request_count--;
            dropped = true;
            s_request_evictions++;
        }
        s_request_head = (uint8_t)((s_request_head + MODEM_REQUEST_QUEUE_LEN - 1u) %
                                   MODEM_REQUEST_QUEUE_LEN);
        s_request_queue[s_request_head] = *request;
        s_request_count++;
    } else {
        s_request_queue[s_request_tail] = *request;
        s_request_tail = (uint8_t)((s_request_tail + 1u) % MODEM_REQUEST_QUEUE_LEN);
        s_request_count++;
    }
    if (s_request_count > s_request_high_water) {
        s_request_high_water = s_request_count;
    }
    critical_section_exit(&s_request_lock);

    if (dropped) {
        LOGW("modem", "request queue full; dropped newest for call control");
        cancel_request(&evicted, MODEM_PHONEBOOK_OUTCOME_EVICTED);
    }
    /* A call-control admission invalidates the remainder of a diagnostic
     * group immediately. The one command already on the wire is allowed to
     * finish, but its shadow can no longer publish. */
    diag_cancel_active_group();
    return true;
}

static bool model_release_already_scheduled(uint8_t id) {
    const call_leg_t *leg = model_leg_by_id(id);
    if (leg == NULL) return false;

    /* A generation-qualified entry still waiting in the model cleanup FIFO
     * already owns this release. This matters at the abandon boundary: queuing
     * another queued request beside that FIFO entry sends the same targeted
     * release twice if the first command's transaction retires on OK before
     * the id-scoped release URC arrives. */
    for (unsigned i = 0u; i < s_call_model.release_count; i++) {
        if (s_call_model.release_queue[i].id == id &&
            s_call_model.release_queue[i].generation == leg->generation) {
            return true;
        }
    }

    /* A transaction is the ownership record once the FIFO entry has moved into
     * queued, deferred, in-flight, or accepted-but-not-yet-observed command
     * state. Match its generation, not just the reusable CLCC id: an old
     * command must never consume a new generation's cleanup obligation.
     * FAILED/CANCELLED commands do not own a physical release and therefore
     * cannot suppress a retry. */
    for (unsigned i = 0u; i < MODEM_MAX_CALL_TRANSACTIONS; i++) {
        const call_txn_t *txn = &s_call_model.txns[i];
        if (txn->in_use &&
            (txn->kind == CALL_TXN_RELEASE_ACTIVE ||
             txn->kind == CALL_TXN_RELEASE_LEG) &&
            txn->target_id == id && txn->target_gen == leg->generation &&
            txn->state != TXN_FAILED && txn->state != TXN_CANCELLED) {
            return true;
        }
    }
    return false;
}

static bool model_call_request_waiting(call_txn_kind_t *kind_out) {
    bool is_call = false;
    if (kind_out != NULL) *kind_out = CALL_TXN_NONE;
    critical_section_enter_blocking(&s_request_lock);
    if (s_request_count != 0u) {
        call_txn_kind_t kind = CALL_TXN_NONE;
        is_call = model_request_kind(s_request_queue[s_request_head].type,
                                     &kind);
        if (is_call && kind_out != NULL) *kind_out = kind;
    }
    critical_section_exit(&s_request_lock);
    return is_call;
}

static bool model_release_active_cleanup_ready(uint8_t target_id) {
    bool found_other = false;
    bool found_held = false;
    bool found_nonheld = false;

    for (unsigned i = 0u; i < MODEM_MAX_CALL_LEGS; i++) {
        const call_leg_t *leg = &s_call_model.legs[i];
        if (!leg->in_use || leg->id == target_id) continue;
        found_other = true;
        if (leg->state == CALL_LEG_HELD) found_held = true;
        else found_nonheld = true;
    }

    /* With no survivor, release-active is a plain foreground teardown. With a
     * survivor, wait until the table proves exactly the topology required by
     * this semantic operation: the other leg is HELD and can be recovered.
     * This avoids targeting an earlier ACTIVE leg during the split
     * DIALING->HELD setup sequence, and avoids accepting an unrelated waiting
     * third leg. The cancelled tombstone keeps CLCC polling while we wait. */
    return !found_other || (found_held && !found_nonheld);
}

static bool model_start_pending_release(uint32_t now_ms) {
    call_cleanup_release_t release;
    memset(&release, 0, sizeof(release));
    while (call_model_pop_release(&s_call_model, &release)) {
        uint8_t id = release.id;
        if (model_release_already_scheduled(id)) continue;

        bool recover_held_survivor = release.recover_held_survivor;
        if (recover_held_survivor &&
            !model_release_active_cleanup_ready(id)) {
            if (!call_model_requeue_release(&s_call_model, &release)) {
                LOGE("modem", "lost deferred New-call cleanup for call id %u",
                     (unsigned)id);
            }
            return false;
        }

        modem_request_t request;
        memset(&request, 0, sizeof(request));
        request.type = recover_held_survivor
                     ? MODEM_REQ_RELEASE_ACTIVE : MODEM_REQ_RELEASE_LEG;
        request.index = id;
        call_model_set_now(&s_call_model, now_ms);
        call_txn_kind_t kind = recover_held_survivor
                            ? CALL_TXN_RELEASE_ACTIVE : CALL_TXN_RELEASE_LEG;
        request.call_token = call_model_request(&s_call_model, kind, id, false);
        if (request.call_token == 0u) {
            if (!call_model_requeue_release(&s_call_model, &release)) {
                LOGE("modem", "lost model release retry for call id %u", (unsigned)id);
            }
            return false;
        }
        s_current_request = request;
        start_next_request(now_ms);
        return true;
    }
    return false;
}

static uint32_t model_transport_counter(void) {
    return modem_uart_hal_rx_dropped() + modem_uart_hal_rx_overruns() +
           modem_uart_hal_rx_line_errors() + s_line_drop_count +
           s_model_malformed_clcc;
}

static void model_note_coarse_call_observation(void) {
    /* A recognizable call indication whose identity/state could not be trusted
     * is still an observation: invalidate an interleaved CLCC shadow and pull a
     * fresh snapshot, but never invent or mutate a leg by a guessed id. */
    call_model_on_event(&s_call_model, 0u, false,
                        CALL_LEG_UNKNOWN, CALL_DIR_UNKNOWN);
}

static void model_apply_projection(modem_status_t *status,
                                   const call_projection_t *projection) {
    status->call_state = projection->call_state;
    status->active_call_id = projection->active_call_id;
    status->call_on_hold = projection->call_on_hold;
    status->second_call_held = projection->second_call_held;
    status->waiting_call = projection->waiting_call;
    status->ring_active = projection->ring_active;
    status->caller_id_withheld = projection->caller_id_withheld;
    status->incoming_diverted = projection->incoming_diverted;
    copy_bounded(status->incoming_number, sizeof(status->incoming_number),
                 projection->incoming_number);
    status->last_call_result = projection->last_call_result;
    status->second_call_result = projection->second_call_result;
}

static void model_copy_live_snapshot(modem_call_snapshot_t *out) {
    memset(out, 0, sizeof(*out));
    out->semantic_revision = s_call_model.semantic_revision;
    out->pending_incoming_active = s_call_model.pending_mt.active;
    out->pending_incoming_alert_observed =
        s_call_model.pending_mt.alert_observed;
    out->pending_incoming_episode = s_call_model.pending_mt.generation;
    out->overflow = s_call_model.overflow;

    for (uint8_t i = 0u; i < MODEM_MAX_CALL_LEGS; i++) {
        const call_leg_t *src = &s_call_model.legs[i];
        if (!src->in_use || out->leg_count >= MODEM_CALL_ID_MAX) continue;
        modem_call_leg_snapshot_t *dst = &out->legs[out->leg_count++];
        dst->id = src->id;
        dst->generation = src->generation;
        dst->state = src->state;
        dst->direction = src->dir;
        dst->pending_removal = src->pending_removal;
        dst->incoming_episode = src->mt_episode;
    }
}

static void model_project_publish(void) {
    /* Stateful projection may retire terminal journal records, so it is called
     * exactly once per service tick. Readers consume this saved value. */
    call_projection_t projection;
    call_model_project(&s_call_model, &projection);
    modem_call_snapshot_t snapshot;
    model_copy_live_snapshot(&snapshot);

    critical_section_enter_blocking(&s_status_lock);
    s_call_projection = projection;
    s_call_snapshot = snapshot;
    critical_section_exit(&s_status_lock);
}

static void model_reset_call_session(uint32_t now_ms) {
    call_model_init(&s_call_model, &g_modem_vendor.call.timing);
    call_model_set_now(&s_call_model, now_ms);
    modem_call_snapshot_t snapshot;
    model_copy_live_snapshot(&snapshot);
    if (s_status_lock_ready) {
        critical_section_enter_blocking(&s_status_lock);
    }
    s_call_projection = s_call_model.published;
    s_call_snapshot = snapshot;
    if (s_status_lock_ready) {
        critical_section_exit(&s_status_lock);
    }
    s_active_call_token = 0u;
    s_deferred_call_token = 0u;
}

void modem_service_init(void) {
    critical_section_init(&s_status_lock);
    critical_section_init(&s_request_lock);
    critical_section_init(&s_sms_lock);
    critical_section_init(&s_phonebook_lock);
    memset(&s_status, 0, sizeof(s_status));
    memset(&s_rx_trace_status, 0, sizeof(s_rx_trace_status));
    s_rx_trace_write = 0u;
    s_status.available = g_modem_vendor.available;
    s_status.rssi = 99u;
    s_status.ber = 99u;
    s_status.cereg = 0xffu;
    s_status.audio_init_ok = true; /* cleared only if a recoverable init step is skipped */
    s_status.sms_init_ok = true;
    s_signal_foreground_sampling = false;
    s_signal_refresh_needed = false;
    s_signal_line_seen = false;
    s_signal_line_invalid = false;
    memset(&s_signal_shadow, 0, sizeof(s_signal_shadow));
    s_operator_refresh_needed = false;
    s_registration_generation = 0u;
    s_signal_registration_generation = 0u;
    s_sim_generation = 0u;
    sim_provider_reset();
    modem_diag_engine_init(&s_diag_engine, g_modem_vendor.diag_queries,
                           g_modem_vendor.diag_query_count,
                           g_modem_vendor.diag_group_finish,
                           g_modem_vendor.available);
    call_model_init(&s_call_model, &g_modem_vendor.call.timing);
    memset(&s_call_projection, 0, sizeof(s_call_projection));
    memset(&s_call_snapshot, 0, sizeof(s_call_snapshot));
    s_bridge_active_wanted = false;
    s_active_call_token = 0u;
    s_deferred_call_token = 0u;
    modem_line_framer_reset(&s_line_framer);
    s_line_drop_count = 0u;
    s_model_malformed_clcc = 0u;

    if (g_modem_vendor.available) {
        modem_power_monitor_hal_init();
        modem_uart_hal_init();
        modem_uart_hal_set_dtr_sleep_permitted(false);
        modem_uart_hal_park();
        shared_3v8_service_set_required(SHARED_3V8_OWNER_MODEM, false);
    }

    s_state = MODEM_STATE_OFF;
    s_active_kind = MODEM_AT_NONE;
    s_operation = MODEM_OP_NONE;
    s_binary_restore_pending = false;
    s_sms_terminal_published = false;
    s_active = false;
    s_init_index = 0;
    s_init_retries = 0;
    s_init_parse_seen = false;
    s_provision_index = 0u;
    s_provision_retries = 0u;
    s_provision_phase = MODEM_PROVISION_QUERY;
    s_provision_line_seen = false;
    s_provision_line_result = MODEM_PROVISION_LINE_IGNORE;
    s_provision_step_written = false;
    s_provision_reboot_required = false;
    s_provision_rebooted = false;
    s_provision_reboot_cycle_active = false;
    s_provision_reboot_drop_seen = false;
    s_startup_restart_recovered = false;
    s_startup_complete = false;
    s_provision_all_verified = false;
    s_provision_non_sim_verified = false;
    s_sim_completion_needed = false;
    s_sim_completion_pending = false;
    s_sim_completion_active = false;
    s_sim_completion_failed = false;
    s_sim_steps_skipped = false;
    s_power_pulsed = false;
    s_request_head = 0;
    s_request_tail = 0;
    s_request_count = 0;
    s_request_high_water = 0u;
    s_request_admission_failures = 0u;
    s_request_evictions = 0u;
    memset(&s_current_request, 0, sizeof(s_current_request));
    s_dtmf_cancel_requested = false;
    modem_supplementary_init();
    s_diag_transport_sleep_entries = 0u;
    s_diag_transport_wake_attempts = 0u;
    s_diag_transport_wake_timeouts = 0u;
    s_diag_transport_ri_release_timeouts = 0u;
    s_diag_transport_wake_started_ms = 0u;
    s_diag_transport_last_wake_latency_ms = 0u;
    s_diag_transport_max_wake_latency_ms = 0u;
    s_diag_active_started_ms = 0u;
    s_diag_power_on_requests = 0u;
    s_diag_power_on_starts = 0u;
    s_diag_ready_entries = 0u;
    s_diag_power_off_requests = 0u;
    s_diag_shutdown_completions = 0u;
    s_diag_graceful_shutdown_pulses = 0u;
    s_diag_emergency_shutdown_pulses = 0u;
    s_diag_terminal_shutdown_failures = 0u;
    s_diag_automatic_recoveries = 0u;
    s_diag_controlled_restarts = 0u;
    s_diag_power_failures = 0u;
    s_diag_last_transition_ms = 0u;
    s_diag_last_recovery_reason = MODEM_DIAG_RECOVERY_NONE;
    const modem_maintenance_hooks_t maintenance_hooks = {
        .write_recovery_record = maintenance_write_recovery_record,
        .flush_recovery_record = maintenance_flush_recovery_record,
        .hold_sim_offline = sim_maintenance_guard_hold_cfun4,
        .begin_sim_online = sim_maintenance_guard_begin_online,
    };
    modem_maintenance_init(&g_modem_vendor.maintenance, &maintenance_hooks);
    s_maintenance_sim_guard_deadline_ms = 0u;
    s_maintenance_sim_deferred = MODEM_SIM_OBSERVATION_NONE;
    s_deferred_valid = false;
    modem_sms_state_init();
    modem_sms_protocol_init();
    memset(&s_debug_result, 0, sizeof(s_debug_result));
    s_debug_result_pending = false;
    modem_sms_protocol_reset_pending_arrivals();
    direct_ring_reset();
    s_sms_mode_restore_pending = false;
    s_sms_mode_restore_attempts = 0u;
    modem_phonebook_state_init();
    s_now_ms = time_ms();
    s_diag_transport_last_account_ms = s_now_ms;
    s_diag_transport_ready_ms = 0u;
    s_diag_transport_sleep_requested_ms = 0u;
    s_diag_transport_sleep_confirmed_ms = 0u;
    s_debug_background_polling_enabled = true;
    s_diag_last_transition_ms = s_now_ms;
    s_boot_deadline_ms = 0u;
    s_rail_power_good_deadline_ms = 0u;
    s_next_power_observation_ms = 0u;
    s_next_signal_ms = 0u;
    /* Enforce the same proven discharge dwell at boot as every later recycle.
     * A power-key request arriving sooner is queued by power_on(). */
    s_rail_off_ready_ms = s_now_ms + MODEM_RAIL_OFF_DWELL_MS;
    s_rail_off_dwell_started = true;
    s_rail_off_dwell_complete = false;
    s_rail_off_transition_snapshot = shared_3v8_service_transition_count();
    s_next_action_ms = 0u;
    s_power_on_pending = false;
    s_power_off_pending = false;
    s_uart_parked = g_modem_vendor.available;
    s_dtr_sleep_permitted = false;
    s_dtr_wake_pending = false;
    s_dtr_wake_deadline_ms = 0u;
    s_ri_release_pending = false;
    s_ri_release_deadline_ms = 0u;
    sms_wake_reset_session();
    s_failed_module_may_be_live = false;
    s_failed_shutdown_qualification_pending = false;
    s_shutdown_stage = MODEM_SHUTDOWN_STAGE_NONE;
    s_shutdown_pulse_active = false;
    s_shutdown_pulse_release_ms = 0u;
    s_power_off_failure_pending = false;
    s_supply_power_failure_pending = false;
    s_failed_supply_power = false;
    modem_uart_hal_cancel_shutdown_pulse();
    s_status_lock_ready = true;
    s_request_lock_ready = true;
    s_sms_lock_ready = true;
    s_phonebook_lock_ready = true;

    s_startup_recycled = false;

    if (g_modem_vendor.available) {
        LOGI("modem", "transport ready; modem rail off until power key");
    } else {
        LOGI("modem", "backend unavailable; transport and controls inert");
    }
}

static void modem_transport_account_residency(uint32_t now_ms) {
    uint32_t elapsed_ms = now_ms - s_diag_transport_last_account_ms;
    s_diag_transport_last_account_ms = now_ms;

    if (s_state != MODEM_STATE_READY || s_uart_parked) {
        return;
    }
    s_diag_transport_ready_ms += elapsed_ms;
    if (!s_dtr_sleep_permitted) {
        return;
    }
    s_diag_transport_sleep_requested_ms += elapsed_ms;
    if (!modem_uart_hal_cts_asserted()) {
        s_diag_transport_sleep_confirmed_ms += elapsed_ms;
    }
}

void modem_service_tick(uint32_t now_ms) {
    s_now_ms = now_ms;
    if (!g_modem_vendor.available) {
        return;
    }
    /* Charge the elapsed interval to the transport state that was in force
     * since the previous tick, before this tick can change DTR or CTS state. */
    modem_transport_account_residency(now_ms);
    call_model_set_now(&s_call_model, now_ms);
    modem_runtime_power_tick(now_ms);
    modem_transport_tick(now_ms);
    drain_rx();
    sim_maintenance_guard_tick(now_ms);
    process_timeout(now_ms);
    direct_body_deadline_tick(now_ms);
    call_model_tick(&s_call_model);
    advance_state(now_ms);
    modem_transport_idle(now_ms);
    model_project_publish();
    modem_bridge_follow_call_state();
}

bool modem_service_transport_sleep_confirmed(void) {
    if (!g_modem_vendor.available) {
        return true;
    }
    if (g_modem_vendor.wake.strategy != MODEM_WAKE_DTR_CTS ||
        s_state != MODEM_STATE_READY || s_uart_parked ||
        !s_dtr_sleep_permitted || modem_uart_hal_cts_asserted() ||
        modem_uart_hal_ri_asserted() || s_dtr_wake_pending ||
        s_ri_release_pending || s_deferred_valid || s_active ||
        s_operation != MODEM_OP_NONE || s_binary_restore_pending ||
        s_active_sms_wake_arm ||
        s_sms_wake_resume != SMS_WAKE_RESUME_NONE ||
        s_power_on_pending || s_power_off_pending ||
        modem_diag_engine_has_work(&s_diag_engine) ||
        modem_line_framer_pending(&s_line_framer) ||
        modem_sms_direct_pending() || /* +CMT body in flight (raw or line mode) */
        !call_session_idle_snapshot() ||
        call_model_wants_clcc(&s_call_model) ||
        s_call_model.release_count != 0u ||
        !modem_uart_hal_tx_idle() || !modem_uart_hal_rx_idle()) {
        return false;
    }
    if (modem_maintenance_blocks_sleep()) {
        return false;
    }
    if (sms_wake_required() && status_sim_ready_snapshot() &&
        !s_sms_wake_armed) {
        return false;
    }

    bool queue_empty;
    critical_section_enter_blocking(&s_request_lock);
    queue_empty = s_request_count == 0u;
    critical_section_exit(&s_request_lock);
    return queue_empty;
}

bool modem_service_battery_high_load_active(void) {
    if (!g_modem_vendor.available) {
        return false;
    }
    switch (s_state) {
    case MODEM_STATE_RAIL_WAIT:
    case MODEM_STATE_POWER_PULSE:
    case MODEM_STATE_MODULE_WAIT:
    case MODEM_STATE_PROBE:
    case MODEM_STATE_INIT:
    case MODEM_STATE_PROVISION:
    case MODEM_STATE_OFF_DISCHARGE:
        return true;
    case MODEM_STATE_FAILED:
        return s_failed_module_may_be_live;
    case MODEM_STATE_READY:
        return !call_session_idle_snapshot();
    case MODEM_STATE_OFF:
    default:
        return false;
    }
}

bool modem_service_take_power_off_failure(void) {
    bool pending = s_power_off_failure_pending;
    s_power_off_failure_pending = false;
    return pending;
}

bool modem_service_take_supply_power_failure(void) {
    bool pending = s_supply_power_failure_pending;
    s_supply_power_failure_pending = false;
    return pending;
}

static void modem_start_rail_off_dwell(uint32_t now_ms) {
    s_rail_off_dwell_started = true;
    s_rail_off_dwell_complete = false;
    s_rail_off_ready_ms = now_ms + MODEM_RAIL_OFF_DWELL_MS;
    s_rail_off_transition_snapshot = shared_3v8_service_transition_count();
}

static void modem_recover_retained_module(uint32_t now_ms) {
    if (g_modem_vendor.power_is_on != NULL) {
        modem_power_observation_t observation = modem_power_observation();
        if (!g_modem_vendor.power_is_on(&observation) && !s_uart_parked) {
            modem_uart_hal_park();
            s_uart_parked = true;
        }
    }
    modem_uart_hal_set_dtr_sleep_permitted(false);
    s_dtr_sleep_permitted = false;
    s_dtr_wake_pending = false;
    s_ri_release_pending = false;
    s_ri_release_deadline_ms = 0u;
    sms_wake_reset_session();
    s_power_on_pending = false;
    s_failed_module_may_be_live = false;
    s_failed_supply_power = false;
    s_power_pulsed = true;
    s_boot_deadline_ms = now_ms + g_modem_vendor.power.ready_budget_ms;
    s_next_action_ms = now_ms;
    modem_enter_module_wait(now_ms);
    LOGW("modem", "recovering retained or indeterminate module after power fault");
}

void modem_service_power_on(void) {
    if (!g_modem_vendor.available) {
        return;
    }
    uint32_t now = time_ms();
    s_diag_power_on_requests++;
    if (s_deferred_valid && s_deferred_kind == MODEM_AT_POWER_OFF) {
        /* No shutdown byte crossed UART yet. A newer ON intent may safely
         * cancel the DTR-wake stash and keep the live session. Retire the wake
         * deadline with it; otherwise a CTS line that remains asleep would
         * later turn this already-cancelled request into a modem power fault. */
        model_cancel_deferred_command();
        s_dtr_wake_pending = false;
        s_dtr_wake_deadline_ms = 0u;
        LOGI("modem", "cancelled never-dispatched shutdown");
        return;
    }
    if (s_power_off_pending) {
        /* The module has not received a shutdown command yet; a newer power-on
         * intent can therefore cancel the deferred startup-time shutdown
         * without creating an uncertain command race. */
        s_power_off_pending = false;
        LOGI("modem", "cancelled deferred startup shutdown");
        return;
    }
    if ((s_active && s_active_kind == MODEM_AT_POWER_OFF) ||
        s_state == MODEM_STATE_OFF_DISCHARGE) {
        /* Once shutdown is on wire its outcome is uncertain and cannot be
         * cancelled. Finish the vendor-safe off qualification, then restart
         * once from a real rail-down boundary. */
        s_power_on_pending = true;
        LOGI("modem", "restart queued behind active shutdown");
        return;
    }
    /* Reset one-session budgets only for genuinely fresh OFF/FAILED intent.
     * A duplicate power-on event during provisioning must not silently restore
     * the already-consumed one-reboot allowance. */
    if (s_state == MODEM_STATE_OFF || s_state == MODEM_STATE_FAILED) {
        s_startup_recycled = false;
        s_startup_restart_recovered = false;
        s_startup_complete = false;
        s_provision_rebooted = false;
        s_provision_reboot_required = false;
        s_provision_reboot_cycle_active = false;
        s_provision_reboot_drop_seen = false;
    }
    if (s_state == MODEM_STATE_FAILED) {
        s_power_off_failure_pending = false;
        if (s_failed_module_may_be_live && g_modem_vendor.power_is_on != NULL) {
            /* A failure that retained the rail has no trustworthy OFF boundary.
             * Recover through the measured module-status gate even if this one
             * ADC sample is low, in the hysteresis gap, or invalid. Treating
             * "not confirmed on" as OFF would turn an observation fault into an
             * unsafe rail cut. */
            modem_recover_retained_module(now);
            return;
        }
        /* Other failures have no live-module evidence. Establish a real OFF
         * boundary and discharge dwell before retrying. */
        s_power_on_pending = true;
        modem_enter_off();
        return;
    }
    if (s_state == MODEM_STATE_OFF) {
        bool rail_enabled = shared_3v8_service_enabled();
        bool dwell_invalid = !s_rail_off_dwell_started ||
            s_rail_off_transition_snapshot !=
                shared_3v8_service_transition_count();
        if (!rail_enabled && dwell_invalid) {
            modem_start_rail_off_dwell(now);
        }
        if (rail_enabled || dwell_invalid ||
            (!s_rail_off_dwell_complete &&
             time_diff_ms(now, s_rail_off_ready_ms) < 0)) {
            s_power_on_pending = true;
            s_next_action_ms = rail_enabled
                ? now + MODEM_RAIL_OFF_POLL_MS
                : s_rail_off_ready_ms;
            LOGI("modem", "power-on queued for rail discharge dwell");
            return;
        }
        s_rail_off_dwell_complete = true;
    }
    modem_begin_power_on(now);
}

void modem_service_power_off(void) {
    if (!g_modem_vendor.available) {
        return;
    }
    s_diag_power_off_requests++;
    modem_begin_power_off(time_ms());
}

bool modem_service_voice_transport_available(void) {
    return g_modem_vendor.available &&
           (g_modem_vendor.capabilities &
            MODEM_VENDOR_CAP_VOICE_TRANSPORT) != 0u;
}

void modem_service_get_status(modem_status_t *out) {
    if (out == 0) {
        return;
    }
    if (!s_status_lock_ready) {
        memset(out, 0, sizeof(*out));
        out->available = g_modem_vendor.available;
        out->rssi = 99u;
        out->ber = 99u;
        out->cereg = 0xffu;
        return;
    }
    critical_section_enter_blocking(&s_status_lock);
    *out = s_status;
    model_apply_projection(out, &s_call_projection);
    out->last_update_ms = s_now_ms;
    critical_section_exit(&s_status_lock);
    /* UART RX health counters live in the HAL; surface them fresh on read,
     * like the debug_* fields below. */
    if (g_modem_vendor.available) {
        out->rx_overruns = modem_uart_hal_rx_overruns();
        out->rx_dropped = modem_uart_hal_rx_dropped();
        out->rx_line_errors = modem_uart_hal_rx_line_errors();
        out->tx_stall_drops = modem_uart_hal_tx_stall_drops();
    }
    out->debug_state = (uint8_t)s_state;
    out->debug_active_kind = (uint8_t)s_active_kind;
    out->debug_init_index = s_init_index;
    out->debug_init_retries = s_init_retries;
    copy_bounded(out->debug_last_command, sizeof(out->debug_last_command), s_debug_last_command);
    copy_bounded(out->debug_last_line, sizeof(out->debug_last_line), s_debug_last_line);
}

void modem_service_get_call_snapshot(modem_call_snapshot_t *out) {
    if (out == NULL) return;
    if (!s_status_lock_ready) {
        memset(out, 0, sizeof(*out));
        return;
    }
    critical_section_enter_blocking(&s_status_lock);
    *out = s_call_snapshot;
    critical_section_exit(&s_status_lock);
}

static bool signal_query_supported(void) {
    return g_modem_vendor.signal_query.query_cmd != NULL &&
           g_modem_vendor.signal_query.response_prefix != NULL &&
           g_modem_vendor.signal_query.timeout_ms != 0u &&
           g_modem_vendor.signal_query.parse_response != NULL;
}

static bool signal_query_due(uint32_t now_ms) {
    if (!signal_query_supported()) {
        return false;
    }
    critical_section_enter_blocking(&s_status_lock);
    bool registered = s_status.network_registered;
    critical_section_exit(&s_status_lock);
    return registered &&
        (s_signal_refresh_needed ||
         (s_signal_foreground_sampling &&
          time_diff_ms(now_ms, s_next_signal_ms) >= 0) ||
         (s_debug_background_polling_enabled &&
          time_diff_ms(now_ms, s_next_signal_ms) >= 0));
}

void modem_service_set_signal_sampling(bool display_active, uint32_t now_ms) {
    if (s_signal_foreground_sampling == display_active) {
        return;
    }
    s_signal_foreground_sampling = display_active;
    if (display_active) {
        s_signal_refresh_needed = true;
        s_next_signal_ms = now_ms;
    } else {
        /* A display timeout must not leave one final 1 Hz poll queued. A
         * registration edge can still request an immediate typed sample. */
        s_next_signal_ms = now_ms + MODEM_SIGNAL_BACKSTOP_MS;
    }
}

static uint32_t signal_next_sequence(uint32_t current) {
    current++;
    return current == 0u ? 1u : current;
}

static void signal_invalidate_locked(uint32_t now_ms) {
    s_registration_generation++;
    uint32_t sequence = signal_next_sequence(s_status.signal.sequence);
    memset(&s_status.signal, 0, sizeof(s_status.signal));
    s_status.signal.sequence = sequence;
    s_status.signal.updated_ms = now_ms;
    s_status.operator_name[0] = '\0';
    s_status.operator_name_source = MODEM_OPERATOR_NAME_NONE;
}

static void operator_name_publish_locked(void) {
    s_status.operator_name[0] = '\0';
    s_status.operator_name_source = MODEM_OPERATOR_NAME_NONE;
    if (!s_status.network_registered || !s_status.sim_ready ||
        (s_status.signal.valid_fields & MODEM_SIGNAL_VALID_PLMN) == 0u) {
        return;
    }
    const char *name = operator_name_db_lookup(s_status.signal.mcc,
                                               s_status.signal.mnc);
    if (name != NULL) {
        copy_bounded(s_status.operator_name, sizeof(s_status.operator_name), name);
        s_status.operator_name_source = MODEM_OPERATOR_NAME_DATABASE;
    } else if (s_sim_provider_cached && s_sim_provider_name[0] != '\0') {
        copy_bounded(s_status.operator_name, sizeof(s_status.operator_name),
                      s_sim_provider_name);
        s_status.operator_name_source = MODEM_OPERATOR_NAME_SIM;
    } else {
        snprintf(s_status.operator_name, sizeof(s_status.operator_name), "%s%s",
                 s_status.signal.mcc, s_status.signal.mnc);
        s_status.operator_name_source = MODEM_OPERATOR_NAME_PLMN;
    }
}

static void sim_provider_reset(void) {
    s_sim_generation++;
    s_sim_provider_cached = false;
    s_sim_provider_attempts = 0u;
    s_next_sim_provider_ms = 0u;
    s_sim_provider_name[0] = '\0';
}

static bool sim_provider_start_query(uint32_t now_ms) {
    const modem_sim_provider_query_t *query = &g_modem_vendor.sim_provider_query;
    if (s_operation != MODEM_OP_NONE ||
        query->query_cmd == NULL || query->response_prefix == NULL ||
        query->parse_response == NULL || query->timeout_ms == 0u ||
        s_sim_provider_cached ||
        s_sim_provider_attempts >= MODEM_SIM_PROVIDER_ATTEMPTS ||
        (s_sim_provider_attempts != 0u &&
         time_diff_ms(now_ms, s_next_sim_provider_ms) < 0)) {
        return false;
    }
    critical_section_enter_blocking(&s_status_lock);
    bool needed = s_status.sim_ready && s_status.network_registered &&
        (s_status.signal.valid_fields & MODEM_SIGNAL_VALID_PLMN) != 0u &&
        operator_name_db_lookup(s_status.signal.mcc, s_status.signal.mnc) == NULL;
    critical_section_exit(&s_status_lock);
    if (!needed) {
        return false;
    }
    s_sim_provider_generation = s_sim_generation;
    s_sim_provider_transport_counter = model_transport_counter();
    s_sim_provider_line_seen = false;
    s_sim_provider_line_invalid = false;
    s_sim_provider_shadow[0] = '\0';
    if (!send_command_with_token(MODEM_AT_SIM_PROVIDER, query->query_cmd,
                                 query->timeout_ms, 0u, now_ms)) {
        return false;
    }
    s_sim_provider_attempts++;
    s_next_sim_provider_ms = now_ms + MODEM_SIM_PROVIDER_RETRY_MS;
    return true;
}

static void sim_provider_finish_query(bool ok) {
    /* A name belongs to the SIM, not to whichever PLMN happened to be serving
     * when the read began. Never accept a result across a SIM/reset boundary. */
    if (s_sim_provider_generation != s_sim_generation || !ok ||
        !s_sim_provider_line_seen ||
        s_sim_provider_line_invalid ||
        s_sim_provider_transport_counter != model_transport_counter()) {
        return;
    }
    s_sim_provider_cached = true;
    copy_bounded(s_sim_provider_name, sizeof(s_sim_provider_name),
                  s_sim_provider_shadow);
    critical_section_enter_blocking(&s_status_lock);
    operator_name_publish_locked();
    s_status.last_update_ms = s_now_ms;
    critical_section_exit(&s_status_lock);
}

static bool signal_start_query(uint32_t now_ms) {
    if (!signal_query_supported()) {
        return false;
    }
    critical_section_enter_blocking(&s_status_lock);
    bool registered = s_status.network_registered;
    s_signal_registration_generation = s_registration_generation;
    critical_section_exit(&s_status_lock);
    if (!registered) {
        return false;
    }

    s_signal_line_seen = false;
    s_signal_line_invalid = false;
    memset(&s_signal_shadow, 0, sizeof(s_signal_shadow));
    s_signal_transport_counter = model_transport_counter();
    if (!send_command_with_token(MODEM_AT_SIGNAL,
                                 g_modem_vendor.signal_query.query_cmd,
                                 g_modem_vendor.signal_query.timeout_ms,
                                 0u, now_ms)) {
        return false;
    }
    /* Clear only at admission. A registration edge racing the in-flight query
     * can set it again and therefore request a follow-up typed sample. */
    s_signal_refresh_needed = false;
    s_operator_refresh_needed = false;
    s_next_signal_ms = now_ms +
        (s_signal_foreground_sampling
             ? MODEM_SIGNAL_ACTIVE_MS
             : MODEM_SIGNAL_BACKSTOP_MS);
    return true;
}

static void signal_finish_query(bool command_ok, uint32_t now_ms) {
    if (!command_ok || !s_signal_line_seen || s_signal_line_invalid ||
        s_signal_transport_counter != model_transport_counter()) {
        return;
    }
    critical_section_enter_blocking(&s_status_lock);
    if (s_status.network_registered &&
        s_signal_registration_generation == s_registration_generation) {
        s_signal_shadow.sequence =
            signal_next_sequence(s_status.signal.sequence);
        s_signal_shadow.updated_ms = now_ms;
        s_status.signal = s_signal_shadow;
        operator_name_publish_locked();
        s_status.last_update_ms = now_ms;
    }
    critical_section_exit(&s_status_lock);
}

/* ------------------------------------------------------------------------ */
/* Guarded Net Monitor maintenance.                                         */
/* ------------------------------------------------------------------------ */

static bool maintenance_command_in_flight(void) {
    return (s_active && s_active_kind == MODEM_AT_MAINTENANCE) ||
           (s_deferred_valid &&
            s_deferred_kind == MODEM_AT_MAINTENANCE);
}

static bool maintenance_write_recovery_record(const char *record) {
    return store_setting_set_text(
               STORE_SETTING_SYSTEM_NETMON_RADIO_RECOVERY, record) ==
           STORE_STATUS_OK;
}

static bool maintenance_flush_recovery_record(void) {
    if (!store_service_flush_all()) {
        return false;
    }
    store_diag_snapshot_t diag;
    store_service_get_diag(&diag);
    return diag.ready &&
           (diag.dirty_mask &
            (uint16_t)(1u << STORE_UNIT_SETTINGS_SYSTEM)) == 0u;
}

static bool maintenance_request_admissible(void) {
    if (!g_modem_vendor.available || !modem_maintenance_supported() ||
        s_state != MODEM_STATE_READY || s_operation != MODEM_OP_NONE ||
        s_active || s_dtr_wake_pending || s_ri_release_pending ||
        s_deferred_valid || !call_session_idle_snapshot() ||
        !modem_maintenance_can_admit()) {
        return false;
    }
    bool queue_empty;
    critical_section_enter_blocking(&s_request_lock);
    queue_empty = s_request_count == 0u;
    critical_section_exit(&s_request_lock);
    return queue_empty;
}

bool modem_service_maintenance_supported(void) {
    return g_modem_vendor.available && modem_maintenance_supported();
}

bool modem_service_maintenance_request_scan_timer(uint16_t seconds) {
    if (!maintenance_request_admissible() ||
        !modem_maintenance_request_scan_timer(seconds)) {
        return false;
    }
    diag_cancel_active_group();
    return true;
}

bool modem_service_maintenance_read_scan_timer(void) {
    if (!maintenance_request_admissible() ||
        !modem_maintenance_read_scan_timer()) {
        return false;
    }
    diag_cancel_active_group();
    return true;
}

bool modem_service_maintenance_start_band_test(uint8_t preset) {
    if (!maintenance_request_admissible() ||
        !modem_maintenance_start_band_test(preset)) {
        return false;
    }
    diag_cancel_active_group();
    return true;
}

bool modem_service_maintenance_restore_band(void) {
    return modem_maintenance_restore_band(maintenance_command_in_flight());
}

bool modem_service_maintenance_enter_antenna(void) {
    if (!maintenance_request_admissible() ||
        !modem_maintenance_enter_antenna()) {
        return false;
    }
    diag_cancel_active_group();
    return true;
}

bool modem_service_maintenance_select_antenna(uint8_t rf_state) {
    return modem_maintenance_select_antenna(rf_state);
}

bool modem_service_maintenance_exit_antenna(void) {
    return modem_maintenance_exit_antenna(maintenance_command_in_flight());
}

void modem_service_maintenance_cancel(void) {
    modem_maintenance_cancel(maintenance_command_in_flight());
}

void modem_service_get_maintenance_snapshot(
    modem_maintenance_snapshot_t *out) {
    if (out == NULL) {
        return;
    }
    if (!s_status_lock_ready) {
        memset(out, 0, sizeof(*out));
        out->state = MODEM_MAINTENANCE_IDLE;
        return;
    }
    critical_section_enter_blocking(&s_status_lock);
    modem_maintenance_get_snapshot(out);
    critical_section_exit(&s_status_lock);
}

static void maintenance_reset_session(void) {
    modem_maintenance_reset_session();
    s_maintenance_sim_guard_deadline_ms = 0u;
    s_maintenance_sim_deferred = MODEM_SIM_OBSERVATION_NONE;
}

static void maintenance_check_recovery_record(void) {
    if (modem_maintenance_recovery_checked() || !store_service_ready() ||
        s_state != MODEM_STATE_READY) {
        return;
    }
    char record[STORE_TEXT_MAX + 1u];
    store_status_t status = store_setting_get_text(
        STORE_SETTING_SYSTEM_NETMON_RADIO_RECOVERY, record,
        sizeof(record));
    if (status != STORE_STATUS_OK) {
        return;
    }
    modem_maintenance_recovery_result_t result =
        modem_maintenance_recover_record(record,
                                         maintenance_command_in_flight());
    if (result == MODEM_MAINTENANCE_RECOVERY_FAIL_CLOSED) {
        LOGE("modem", "invalid Net Monitor radio recovery record; disabling RF");
    } else if (result == MODEM_MAINTENANCE_RECOVERY_RESTORE_STARTED) {
        LOGW("modem", "restoring interrupted Net Monitor band test");
    }
}
static void diag_cancel_active_group(void) {
    bool external_command =
        (s_deferred_valid && s_deferred_kind == MODEM_AT_DIAG_QUERY) ||
        (s_active && s_active_kind == MODEM_AT_DIAG_QUERY);
    critical_section_enter_blocking(&s_status_lock);
    modem_diag_engine_cancel_active(&s_diag_engine, external_command, s_now_ms);
    critical_section_exit(&s_status_lock);
    if (s_deferred_valid && s_deferred_kind == MODEM_AT_DIAG_QUERY) {
        s_deferred_valid = false;
        s_deferred_call_token = 0u;
    }
}

bool modem_service_diag_select(modem_diag_group_t group, uint32_t generation,
                               bool request_now) {
    if (!g_modem_vendor.available || generation == 0u ||
        group >= MODEM_DIAG_GROUP_COUNT) {
        return false;
    }
    if (group == MODEM_DIAG_GROUP_NONE) {
        diag_cancel_active_group();
        critical_section_enter_blocking(&s_status_lock);
        modem_diag_engine_set_subscription(&s_diag_engine,
                                           MODEM_DIAG_GROUP_NONE,
                                           generation);
        critical_section_exit(&s_status_lock);
        return true;
    }

    critical_section_enter_blocking(&s_status_lock);
    bool supported = modem_diag_engine_group_supported(&s_diag_engine, group);
    if (!supported) {
        modem_diag_engine_mark_unsupported(&s_diag_engine, group);
    }
    bool same_subscription = supported &&
        modem_diag_engine_same_subscription(&s_diag_engine, group, generation);
    critical_section_exit(&s_status_lock);
    if (!supported) {
        return false;
    }

    if (!same_subscription) {
        diag_cancel_active_group();
        critical_section_enter_blocking(&s_status_lock);
        modem_diag_engine_set_subscription(&s_diag_engine, group, generation);
        critical_section_exit(&s_status_lock);
    }
    if (!request_now) {
        return true;
    }
    critical_section_enter_blocking(&s_status_lock);
    bool admitted = modem_diag_engine_request(&s_diag_engine, s_now_ms);
    critical_section_exit(&s_status_lock);
    return admitted;
}

void modem_service_diag_cancel(uint32_t generation) {
    critical_section_enter_blocking(&s_status_lock);
    uint32_t selected_generation =
        modem_diag_engine_selected_generation(&s_diag_engine);
    critical_section_exit(&s_status_lock);
    if (generation != 0u && generation != selected_generation) {
        return;
    }
    diag_cancel_active_group();
    critical_section_enter_blocking(&s_status_lock);
    modem_diag_engine_set_subscription(&s_diag_engine,
                                       MODEM_DIAG_GROUP_NONE,
                                       selected_generation);
    critical_section_exit(&s_status_lock);
}

void modem_service_get_diag_snapshot(modem_diag_snapshot_t *out) {
    if (out == NULL) {
        return;
    }
    if (!s_status_lock_ready) {
        memset(out, 0, sizeof(*out));
        out->backend_available = g_modem_vendor.available;
        return;
    }

    critical_section_enter_blocking(&s_status_lock);
    modem_diag_engine_copy_snapshot(&s_diag_engine, out);
    out->backend_available = g_modem_vendor.available;
    out->at_ready = s_status.at_ready;
    out->sim_checked = s_status.sim_checked;
    out->sim_present = s_status.sim_present;
    out->sim_ready = s_status.sim_ready;
    out->network_registered = s_status.network_registered;
    out->urc_count = s_status.urc_count;
    out->command_errors = s_status.command_errors;
    out->sms_received_count = s_status.sms_received_count;
    out->sms_sent_count = s_status.sms_sent_count;
    out->sms_storage_full_events = s_status.sms_storage_full_events;
    out->transport.rx_bytes = s_status.rx_bytes;
    out->runtime.provisioning_verified = s_status.provisioning_verified;
    out->runtime.provisioning_schema = s_status.provisioning_schema_version;
    out->runtime.audio_init_ok = s_status.audio_init_ok;
    out->runtime.sms_init_ok = s_status.sms_init_ok;
    out->calls.projected_state = s_call_projection.call_state;
    out->calls.active_call_id = s_call_projection.active_call_id;
    out->calls.ringing = s_call_projection.ring_active;
    out->calls.waiting = s_call_projection.waiting_call;
    out->calls.on_hold = s_call_projection.call_on_hold;
    out->calls.second_held = s_call_projection.second_call_held;
    out->calls.last_result = s_call_projection.last_call_result;
    out->calls.second_result = s_call_projection.second_call_result;
    critical_section_exit(&s_status_lock);

    if (g_modem_vendor.available) {
        out->transport.rx_overruns = modem_uart_hal_rx_overruns();
        out->transport.rx_dropped = modem_uart_hal_rx_dropped();
        out->transport.rx_line_errors = modem_uart_hal_rx_line_errors();
        out->transport.tx_stall_drops = modem_uart_hal_tx_stall_drops();
        out->transport.cts_asserted = modem_uart_hal_cts_asserted();
        out->transport.ri_asserted = modem_uart_hal_ri_asserted();
    }
    out->transport.dtr_sleep_permitted = s_dtr_sleep_permitted;
    out->transport.dtr_wake_pending = s_dtr_wake_pending;
    out->transport.ri_release_pending = s_ri_release_pending;
    out->transport.sleep_entries = s_diag_transport_sleep_entries;
    out->transport.wake_attempts = s_diag_transport_wake_attempts;
    out->transport.wake_timeouts = s_diag_transport_wake_timeouts;
    out->transport.ri_release_timeouts =
        s_diag_transport_ri_release_timeouts;
    out->transport.last_wake_latency_ms =
        s_diag_transport_last_wake_latency_ms;
    out->transport.max_wake_latency_ms =
        s_diag_transport_max_wake_latency_ms;
    out->transport.ready_ms = s_diag_transport_ready_ms;
    out->transport.sleep_requested_ms =
        s_diag_transport_sleep_requested_ms;
    out->transport.sleep_confirmed_ms =
        s_diag_transport_sleep_confirmed_ms;

    out->runtime.state = (uint8_t)s_state;
    out->runtime.active_kind = (uint8_t)s_active_kind;
    out->runtime.operation = (uint8_t)s_operation;
    out->runtime.init_index = s_init_index;
    out->runtime.init_retries = s_init_retries;
    out->runtime.provision_index = s_provision_index;
    out->runtime.provision_phase = (uint8_t)s_provision_phase;
    out->runtime.provision_retries = s_provision_retries;
    out->runtime.active_since_ms = s_active ? s_diag_active_started_ms : 0u;
    out->runtime.power_on_requests = s_diag_power_on_requests;
    out->runtime.power_on_starts = s_diag_power_on_starts;
    out->runtime.ready_entries = s_diag_ready_entries;
    out->runtime.power_off_requests = s_diag_power_off_requests;
    out->runtime.shutdown_completions = s_diag_shutdown_completions;
    out->runtime.graceful_shutdown_pulses =
        s_diag_graceful_shutdown_pulses;
    out->runtime.emergency_shutdown_pulses =
        s_diag_emergency_shutdown_pulses;
    out->runtime.terminal_shutdown_failures =
        s_diag_terminal_shutdown_failures;
    out->runtime.automatic_recoveries = s_diag_automatic_recoveries;
    out->runtime.controlled_restarts = s_diag_controlled_restarts;
    out->runtime.power_failures = s_diag_power_failures;
    out->runtime.last_transition_ms = s_diag_last_transition_ms;
    out->runtime.last_recovery_reason =
        (uint8_t)s_diag_last_recovery_reason;
    out->runtime.shutdown_stage = (uint8_t)s_shutdown_stage;
    out->runtime.shutdown_terminal_fault =
        s_state == MODEM_STATE_FAILED &&
        s_shutdown_stage == MODEM_SHUTDOWN_STAGE_TERMINAL_FAILURE;
    copy_bounded(out->runtime.last_command,
                 sizeof(out->runtime.last_command), s_debug_last_command);
    copy_bounded(out->runtime.last_line,
                 sizeof(out->runtime.last_line), s_debug_last_line);

    out->scheduler.background_polling_enabled =
        s_debug_background_polling_enabled;
    critical_section_enter_blocking(&s_request_lock);
    out->scheduler.normal_queue_depth = s_request_count;
    critical_section_exit(&s_request_lock);
    out->scheduler.normal_queue_high_water = s_request_high_water;
    out->scheduler.normal_queue_admission_failures = s_request_admission_failures;
    out->scheduler.normal_queue_evictions = s_request_evictions;

    out->calls.wants_clcc = call_model_wants_clcc(&s_call_model);
    out->calls.leg_count = 0u;
    for (uint8_t i = 0u; i < MODEM_MAX_CALL_LEGS; i++) {
        const call_leg_t *leg = &s_call_model.legs[i];
        if (!leg->in_use || out->calls.leg_count >= MODEM_DIAG_CALL_LEGS_MAX) {
            continue;
        }
        modem_diag_call_leg_t *dst =
            &out->calls.legs[out->calls.leg_count++];
        dst->id = leg->id;
        dst->generation = leg->generation;
        dst->direction = (uint8_t)leg->dir;
        dst->state = (uint8_t)leg->state;
        dst->role = (uint8_t)leg->published_role;
    }
    out->calls.txn_count = 0u;
    for (uint8_t i = 0u; i < MODEM_MAX_CALL_TRANSACTIONS; i++) {
        const call_txn_t *txn = &s_call_model.txns[i];
        if (!txn->in_use || out->calls.txn_count >= MODEM_DIAG_CALL_TXNS_MAX) {
            continue;
        }
        modem_diag_call_txn_t *dst =
            &out->calls.txns[out->calls.txn_count++];
        dst->token = txn->token;
        dst->kind = (uint8_t)txn->kind;
        dst->state = (uint8_t)txn->state;
        dst->target_id = txn->target_id;
        dst->target_generation = txn->target_gen;
    }
    out->calls.terminal_count = s_call_model.journal_count;
    out->calls.terminal_sequence = s_call_model.semantic_revision;
}

static bool diag_start_next_command(uint32_t now_ms) {
    critical_section_enter_blocking(&s_status_lock);
    bool pending = modem_diag_engine_has_pending(&s_diag_engine);
    critical_section_exit(&s_status_lock);
    if (!pending || s_operation != MODEM_OP_NONE || s_active ||
        s_dtr_wake_pending || !call_session_idle_snapshot()) {
        return false;
    }

    modem_diag_dispatch_t dispatch = {0};
    critical_section_enter_blocking(&s_status_lock);
    bool prepared = modem_diag_engine_prepare_next(&s_diag_engine, now_ms,
                                                   &dispatch);
    critical_section_exit(&s_status_lock);
    if (!prepared) {
        return false;
    }
    send_command(MODEM_AT_DIAG_QUERY, dispatch.command,
                 dispatch.timeout_ms, now_ms);
    return true;
}

static bool diag_parse_expected_line(const char *line) {
    if (line == NULL) {
        return false;
    }
    /* CGMM/CGMR/CGSN return unprefixed payloads. Never let their permissive
     * parsers consume an asynchronous call/registration line or a command
     * final before the ordinary router sees it. The engine isolates
     * prefix-bearing responses against the active query's exact prefix. */
    bool protected_unprefixed_line =
        is_known_urc_line(line) || is_call_progress_final(line) ||
        is_final_text(line);
    critical_section_enter_blocking(&s_status_lock);
    bool consumed = modem_diag_engine_parse_line(
        &s_diag_engine, line, protected_unprefixed_line);
    critical_section_exit(&s_status_lock);
    return consumed;
}

static void diag_finish_query(bool ok, bool timed_out, uint32_t now_ms) {
    critical_section_enter_blocking(&s_status_lock);
    modem_diag_engine_finish_command(&s_diag_engine, ok, timed_out, now_ms);
    critical_section_exit(&s_status_lock);
}

static bool maintenance_start_next_command(uint32_t now_ms) {
    if (!modem_maintenance_has_sequence() ||
        s_operation != MODEM_OP_NONE || s_active || s_dtr_wake_pending) {
        return false;
    }
    modem_maintenance_dispatch_t dispatch;
    if (!modem_maintenance_prepare_next(now_ms, &dispatch)) {
        return false;
    }
    send_command(MODEM_AT_MAINTENANCE, dispatch.command,
                 dispatch.timeout_ms, now_ms);
    return true;
}

static bool maintenance_parse_expected_line(const char *line) {
    return modem_maintenance_parse_expected_line(line);
}

static void maintenance_finish_command(bool ok, bool timed_out,
                                       uint32_t now_ms) {
    modem_maintenance_finish_command(ok, timed_out, now_ms);
}

static void maintenance_cancel_for_call(void) {
    modem_maintenance_cancel_for_call(maintenance_command_in_flight());
}
static void diag_invalidate_all(uint32_t now_ms) {
    diag_cancel_active_group();
    critical_section_enter_blocking(&s_status_lock);
    modem_diag_engine_invalidate_all(&s_diag_engine, now_ms);
    critical_section_exit(&s_status_lock);
}

bool modem_service_request_dial(const char *number) {
    modem_request_t request;
    memset(&request, 0, sizeof(request));
    request.type = MODEM_REQ_DIAL;
    copy_bounded(request.number, sizeof(request.number), number);
    if (request.number[0] == '\0') {
        return false;
    }
    /* #2: clear the latched 2nd-MO failure at ACCEPT time -- not just when this request
     * is later dispatched (start_next_request). Otherwise, while the dial sits queued
     * behind an in-flight AT command, the app could still observe a PRIOR New-call's
     * stale second_call_result and immediately (falsely) roll back this fresh attempt,
     * with the real ATD then connecting B invisibly. Clearing here closes that window;
     * the app does one New-call at a time, so no attempt-generation counter is needed. */
    if (!model_queue_call_request(&request, false)) {
        return false;
    }
    return true;
}

void modem_service_new_call_abandoned(void) {
    if (!g_modem_vendor.available) {
        return;
    }
    call_model_set_now(&s_call_model, time_ms());
    call_model_new_call_abandoned(&s_call_model);
    /* The model converts a dispatched second-outgoing transaction into a
     * generation-qualified cleanup tombstone and emits the appropriate neutral
     * release intent. No service-side call-id reconstruction is permitted. */
}

bool modem_service_new_call_cleanup_pending(void) {
    if (!g_modem_vendor.available) {
        return false;
    }
    return call_model_new_call_cleanup_pending(&s_call_model, NULL);
}

bool modem_service_call_release_active_pending(void) {
    if (!g_modem_vendor.available) {
        return false;
    }
    return call_model_release_active_pending(&s_call_model);
}

bool modem_service_request_answer(void) {
    modem_request_t request;
    memset(&request, 0, sizeof(request));
    request.type = MODEM_REQ_ANSWER;
    bool ok = model_queue_call_request(&request, true);
    LOGI("modem", "answer request queued=%u", ok ? 1u : 0u);
    return ok;
}

bool modem_service_request_hangup(void) {
    modem_request_t request;
    memset(&request, 0, sizeof(request));
    request.type = MODEM_REQ_HANGUP;
    bool ok = model_queue_call_request(&request, true);
    LOGI("modem", "hangup request queued=%u", ok ? 1u : 0u);
    return ok;
}

bool modem_service_request_dtmf(char symbol) {
    char sequence[2] = {symbol, '\0'};
    return modem_service_request_dtmf_sequence(sequence);
}

bool modem_service_request_dtmf_sequence(const char *symbols) {
    if (symbols == NULL || !g_modem_vendor.available ||
        g_modem_vendor.call.build_dtmf_command == NULL ||
        !dtmf_call_active()) {
        return false;
    }

    size_t length = 0u;
    while (length <= MODEM_DTMF_SEQUENCE_MAX &&
           symbols[length] != '\0') {
        if (!dtmf_char_valid(symbols[length])) {
            return false;
        }
        length++;
    }
    if (length == 0u || length > MODEM_DTMF_SEQUENCE_MAX) {
        return false;
    }

    _Static_assert(MODEM_DTMF_SEQUENCE_MAX <= MODEM_SMS_TEXT_MAX,
                   "DTMF sequence must fit the request payload");
    modem_request_t request;
    memset(&request, 0, sizeof(request));
    request.type = MODEM_REQ_DTMF;
    memcpy(request.text, symbols, length + 1u);
    return queue_request(&request);
}

bool modem_service_request_call_waiting_answer(void) {
    modem_request_t request;
    memset(&request, 0, sizeof(request));
    request.type = MODEM_REQ_WAITING_ANSWER;
    return model_queue_call_request(&request, true);
}

bool modem_service_request_call_waiting_reject(void) {
    modem_request_t request;
    memset(&request, 0, sizeof(request));
    request.type = MODEM_REQ_WAITING_REJECT;
    return model_queue_call_request(&request, true);
}

bool modem_service_request_call_swap(void) {
    modem_request_t request;
    memset(&request, 0, sizeof(request));
    request.type = MODEM_REQ_CALL_SWAP;
    return model_queue_call_request(&request, true);
}

bool modem_service_call_hold_available(void) {
    return g_modem_vendor.available &&
           (g_modem_vendor.call.capabilities &
            MODEM_CALL_CAPABILITY(CALL_TXN_HOLD)) != 0u &&
           !call_model_new_call_cleanup_pending(&s_call_model, NULL) &&
           call_model_hold_toggle_available(&s_call_model);
}

bool modem_service_request_call_hold(void) {
    modem_request_t request;
    memset(&request, 0, sizeof(request));
    request.type = MODEM_REQ_CALL_HOLD;
    return model_queue_call_request(&request, true);
}

bool modem_service_request_call_release_active(void) {
    modem_request_t request;
    memset(&request, 0, sizeof(request));
    request.type = MODEM_REQ_RELEASE_ACTIVE;
    return model_queue_call_request(&request, true);
}

bool modem_service_request_call_release_leg(uint8_t call_id) {
    if (call_id == 0u || call_id > MODEM_MAX_CALL_LEGS) {
        return false;
    }
    modem_request_t request;
    memset(&request, 0, sizeof(request));
    request.type = MODEM_REQ_RELEASE_LEG;
    request.index = call_id;
    bool ok = model_queue_call_request(&request, true);
    LOGI("modem", "release leg request id=%u queued=%u",
         (unsigned)call_id, ok ? 1u : 0u);
    return ok;
}

bool modem_service_request_call_forward(const call_forward_request_t *request,
                                        uint32_t *request_id_out) {
    if (request_id_out != NULL) {
        *request_id_out = 0u;
    }
    if (!s_request_lock_ready || !s_status_lock_ready ||
        !call_forward_request_valid(request, NULL)) {
        return false;
    }

    modem_request_t queued;
    memset(&queued, 0, sizeof(queued));
    queued.type = MODEM_REQ_CALL_FORWARD;
    queued.call_forward = *request;

    critical_section_enter_blocking(&s_request_lock);
    queued.request_id =
        modem_supplementary_call_forward_next_request_id();
    critical_section_exit(&s_request_lock);

    modem_status_t status;
    modem_service_get_status(&status);
    if (!status.sim_ready || !status.network_registered) {
        call_forward_push_result(
            queued.request_id, &queued.call_forward,
            status.sim_ready ? CALL_FORWARD_OUTCOME_NO_NETWORK
                             : CALL_FORWARD_OUTCOME_NOT_DONE,
            false, false, NULL, false, 0u);
        if (request_id_out != NULL) {
            *request_id_out = queued.request_id;
        }
        return true;
    }
    if (!queue_request(&queued)) {
        return false;
    }
    if (request_id_out != NULL) {
        *request_id_out = queued.request_id;
    }
    return true;
}

bool modem_service_cancel_queued_call_forward(uint32_t request_id) {
    if (request_id == 0u || !s_request_lock_ready) {
        return false;
    }

    bool removed = false;
    critical_section_enter_blocking(&s_request_lock);
    for (uint8_t offset = 0u; offset < s_request_count; offset++) {
        uint8_t index =
            (uint8_t)((s_request_head + offset) % MODEM_REQUEST_QUEUE_LEN);
        modem_request_t *candidate = &s_request_queue[index];
        if (candidate->type != MODEM_REQ_CALL_FORWARD ||
            candidate->request_id != request_id) {
            continue;
        }

        /* Compact the bounded ring in FIFO order. The request cannot become
         * s_current_request while this lock is held because dequeue uses the
         * same boundary; false therefore means it is already in flight/gone. */
        for (uint8_t move = offset; move + 1u < s_request_count; move++) {
            uint8_t to =
                (uint8_t)((s_request_head + move) % MODEM_REQUEST_QUEUE_LEN);
            uint8_t from = (uint8_t)((to + 1u) % MODEM_REQUEST_QUEUE_LEN);
            s_request_queue[to] = s_request_queue[from];
        }
        s_request_tail = (uint8_t)((s_request_tail +
                                    MODEM_REQUEST_QUEUE_LEN - 1u) %
                                   MODEM_REQUEST_QUEUE_LEN);
        memset(&s_request_queue[s_request_tail], 0,
               sizeof(s_request_queue[s_request_tail]));
        s_request_count--;
        removed = true;
        break;
    }
    critical_section_exit(&s_request_lock);
    return removed;
}

bool modem_service_pop_call_forward_result(call_forward_result_t *out) {
    if (out == NULL || !s_status_lock_ready) {
        return false;
    }
    bool available = false;
    critical_section_enter_blocking(&s_status_lock);
    available = modem_supplementary_call_forward_pop_result(out);
    critical_section_exit(&s_status_lock);
    return available;
}

bool modem_service_get_voice_mailbox_number(char *out, size_t out_cap) {
    if (out == NULL || out_cap == 0u || !s_status_lock_ready) {
        return false;
    }
    bool available;
    critical_section_enter_blocking(&s_status_lock);
    available = modem_supplementary_voice_mailbox_get(out, out_cap);
    critical_section_exit(&s_status_lock);
    return available;
}

static bool sms_request_kind_from_request_type(
    modem_request_type_t type, modem_sms_request_kind_t *out) {
    if (out == NULL) {
        return false;
    }
    switch (type) {
    case MODEM_REQ_SEND_SMS:
        *out = MODEM_SMS_REQUEST_SEND_TEXT;
        return true;
    case MODEM_REQ_SEND_BINARY_SMS:
        *out = MODEM_SMS_REQUEST_SEND_BINARY;
        return true;
    case MODEM_REQ_SAVE_SMS:
        *out = MODEM_SMS_REQUEST_SAVE;
        return true;
    case MODEM_REQ_SMS_MAILBOX:
        *out = MODEM_SMS_REQUEST_MAILBOX;
        return true;
    case MODEM_REQ_SMS_READ:
        *out = MODEM_SMS_REQUEST_READ;
        return true;
    case MODEM_REQ_DELETE_SMS:
        *out = MODEM_SMS_REQUEST_DELETE;
        return true;
    case MODEM_REQ_STORE_DELIVERED_SMS:
        *out = MODEM_SMS_REQUEST_DELIVERED;
        return true;
    default:
        *out = MODEM_SMS_REQUEST_NONE;
        return false;
    }
}

static bool queue_sms_request(modem_request_t *request,
                              modem_sms_request_kind_t kind,
                              uint32_t *request_id_out) {
    if (request_id_out == NULL) {
        return false;
    }
    *request_id_out = 0u;
    modem_sms_request_kind_t expected_kind;
    if (request == NULL || !s_sms_lock_ready ||
        !sms_request_kind_from_request_type(request->type, &expected_kind) ||
        expected_kind != kind) {
        return false;
    }

    uint32_t request_id = 0u;
    critical_section_enter_blocking(&s_sms_lock);
    bool reserved = modem_sms_state_reserve_request(kind, &request_id);
    critical_section_exit(&s_sms_lock);
    if (!reserved) {
        s_request_admission_failures++;
        return false;
    }

    request->request_id = request_id;
    if (!queue_request(request)) {
        critical_section_enter_blocking(&s_sms_lock);
        bool released = modem_sms_state_release_request(request_id, kind);
        critical_section_exit(&s_sms_lock);
        if (!released) {
            LOGE("modem", "SMS reservation %lu disappeared before admission",
                 (unsigned long)request_id);
        }
        request->request_id = 0u;
        return false;
    }
    *request_id_out = request_id;
    return true;
}

bool modem_service_request_send_sms(const char *number, const char *text,
                                    uint32_t *request_id_out) {
    if (request_id_out != NULL) {
        *request_id_out = 0u;
    }
    modem_request_t request;
    memset(&request, 0, sizeof(request));
    request.type = MODEM_REQ_SEND_SMS;
    copy_bounded(request.number, sizeof(request.number), number);
    copy_bounded(request.text, sizeof(request.text), text);
    return request.number[0] != '\0' &&
           queue_sms_request(&request, MODEM_SMS_REQUEST_SEND_TEXT,
                             request_id_out);
}

bool modem_service_request_send_binary_sms(const char *number,
                                           const uint8_t *payload,
                                           uint16_t payload_len,
                                           uint16_t dest_port,
                                           uint16_t source_port,
                                           uint32_t *request_id_out) {
    return modem_service_request_send_binary_sms_mode(number,
                                                      payload,
                                                      payload_len,
                                                      dest_port,
                                                      source_port,
                                                      MODEM_BINARY_SMS_MODE_DCS04_PORT_FIRST,
                                                      request_id_out);
}

bool modem_service_request_send_binary_sms_mode(const char *number,
                                                const uint8_t *payload,
                                                uint16_t payload_len,
                                                uint16_t dest_port,
                                                uint16_t source_port,
                                                modem_binary_sms_mode_t mode,
                                                uint32_t *request_id_out) {
    if (request_id_out != NULL) {
        *request_id_out = 0u;
    }
    if (payload == 0 || payload_len == 0u || payload_len > MODEM_SMS_BINARY_MAX) {
        return false;
    }
    modem_request_t request;
    memset(&request, 0, sizeof(request));
    request.type = MODEM_REQ_SEND_BINARY_SMS;
    copy_bounded(request.number, sizeof(request.number), number);
    memcpy(request.binary, payload, payload_len);
    request.binary_len = payload_len;
    request.dest_port = dest_port;
    request.source_port = source_port;
    request.binary_mode = (uint8_t)mode;
    return request.number[0] != '\0' &&
           queue_sms_request(&request, MODEM_SMS_REQUEST_SEND_BINARY,
                             request_id_out);
}

bool modem_service_request_save_sms(const char *number, const char *text,
                                    uint32_t *request_id_out) {
    if (request_id_out != NULL) {
        *request_id_out = 0u;
    }
    modem_request_t request;
    memset(&request, 0, sizeof(request));
    request.type = MODEM_REQ_SAVE_SMS;
    copy_bounded(request.number, sizeof(request.number), number);
    copy_bounded(request.text, sizeof(request.text), text);
    return request.text[0] != '\0' &&
           queue_sms_request(&request, MODEM_SMS_REQUEST_SAVE,
                             request_id_out);
}

bool modem_service_request_debug_at(const char *command) {
    modem_request_t request;
    memset(&request, 0, sizeof(request));
    request.type = MODEM_REQ_DEBUG_AT;
    copy_bounded(request.text, sizeof(request.text), command);
    return request.text[0] != '\0' && queue_request(&request);
}

bool modem_service_request_debug_background_polling(bool enabled) {
    modem_request_t request;
    memset(&request, 0, sizeof(request));
    request.type = MODEM_REQ_DEBUG_BACKGROUND_POLLING;
    request.index = enabled ? 1u : 0u;
    return queue_request(&request);
}

bool modem_service_request_sms_mailbox(modem_sms_mailbox_t mailbox,
                                       uint32_t *request_id_out) {
    if (request_id_out != NULL) {
        *request_id_out = 0u;
    }
    if (mailbox != MODEM_SMS_MAILBOX_INBOX &&
        mailbox != MODEM_SMS_MAILBOX_OUTBOX) {
        return false;
    }
    modem_request_t request;
    memset(&request, 0, sizeof(request));
    request.type = MODEM_REQ_SMS_MAILBOX;
    request.index = (uint16_t)mailbox;
    return queue_sms_request(&request, MODEM_SMS_REQUEST_MAILBOX,
                             request_id_out);
}

bool modem_service_request_sms_read(const uint16_t *indices,
                                    uint8_t index_count,
                                    bool quarantined,
                                    uint32_t expected_identity_hash,
                                    uint32_t *request_id_out) {
    if (request_id_out != NULL) {
        *request_id_out = 0u;
    }
    if (indices == NULL || index_count == 0u ||
        index_count > MODEM_SMS_SEGMENT_MAX) {
        return false;
    }
    modem_request_t request;
    memset(&request, 0, sizeof(request));
    request.type = MODEM_REQ_SMS_READ;
    request.sms_identity_hash = expected_identity_hash;
    request.sms_quarantined = quarantined;
    request.index_count = index_count;
    memcpy(request.indices, indices, index_count * sizeof(indices[0]));
    return queue_sms_request(&request, MODEM_SMS_REQUEST_READ,
                             request_id_out);
}

bool modem_service_pop_sms_mailbox_result(uint32_t request_id,
                                          modem_sms_mailbox_result_t *out) {
    if (request_id == 0u || out == 0 || !s_sms_lock_ready) {
        return false;
    }
    bool ok = false;
    critical_section_enter_blocking(&s_sms_lock);
    ok = modem_sms_state_pop_mailbox_result(request_id, out);
    critical_section_exit(&s_sms_lock);
    return ok;
}

bool modem_service_pop_sms_read_result(uint32_t request_id,
                                       modem_sms_read_result_t *out) {
    if (request_id == 0u || out == NULL || !s_sms_lock_ready) {
        return false;
    }
    bool ok = false;
    critical_section_enter_blocking(&s_sms_lock);
    ok = modem_sms_state_pop_read_result(request_id, out);
    critical_section_exit(&s_sms_lock);
    return ok;
}

bool modem_service_pop_sms_send_result(uint32_t request_id,
                                       modem_sms_send_result_t *out) {
    if (request_id == 0u || out == 0 || !s_sms_lock_ready) {
        return false;
    }
    bool ok = false;
    critical_section_enter_blocking(&s_sms_lock);
    ok = modem_sms_state_pop_send_result(request_id, out);
    critical_section_exit(&s_sms_lock);
    return ok;
}

bool modem_service_pop_debug_result(modem_debug_result_t *out) {
    if (out == 0 || !s_sms_lock_ready) {
        return false;
    }
    bool ok = false;
    critical_section_enter_blocking(&s_sms_lock);
    if (s_debug_result_pending) {
        *out = s_debug_result;
        s_debug_result_pending = false;
        ok = true;
    }
    critical_section_exit(&s_sms_lock);
    return ok;
}

bool modem_service_pop_sms_save_result(uint32_t request_id,
                                       modem_sms_save_result_t *out) {
    if (request_id == 0u || out == 0 || !s_sms_lock_ready) {
        return false;
    }
    bool ok = false;
    critical_section_enter_blocking(&s_sms_lock);
    ok = modem_sms_state_pop_save_result(request_id, out);
    critical_section_exit(&s_sms_lock);
    return ok;
}

bool modem_service_request_delete_sms_indices(const uint16_t *indices,
                                              uint8_t index_count,
                                              uint32_t *request_id_out) {
    if (request_id_out != NULL) {
        *request_id_out = 0u;
    }
    if (indices == NULL || index_count == 0u ||
        index_count > MODEM_SMS_SEGMENT_MAX) {
        return false;
    }
    modem_request_t request;
    memset(&request, 0, sizeof(request));
    request.type = MODEM_REQ_DELETE_SMS;
    request.index_count = index_count;
    memcpy(request.indices, indices, index_count * sizeof(indices[0]));
    /* SMS storage indices are 0-based, so index 0 is a real slot. */
    return queue_sms_request(&request, MODEM_SMS_REQUEST_DELETE,
                             request_id_out);
}

bool modem_service_pop_sms_delete_result(uint32_t request_id,
                                         modem_sms_delete_result_t *out) {
    if (request_id == 0u || out == 0 || !s_sms_lock_ready) {
        return false;
    }
    bool ok = false;
    critical_section_enter_blocking(&s_sms_lock);
    ok = modem_sms_state_pop_delete_result(request_id, out);
    critical_section_exit(&s_sms_lock);
    return ok;
}

uint8_t modem_service_sms_mailbox_count(void) {
    if (!s_sms_lock_ready) {
        return 0;
    }
    critical_section_enter_blocking(&s_sms_lock);
    uint8_t count = modem_sms_state_mailbox_count();
    critical_section_exit(&s_sms_lock);
    return count;
}

bool modem_service_sms_mailbox_entry(uint8_t position,
                                     modem_sms_record_t *out) {
    if (out == 0 || !s_sms_lock_ready) {
        return false;
    }
    bool ok = false;
    critical_section_enter_blocking(&s_sms_lock);
    ok = modem_sms_state_mailbox_record(position, out);
    critical_section_exit(&s_sms_lock);
    return ok;
}

static bool queue_phonebook_request(modem_request_t *request,
                                    uint32_t *request_id_out) {
    if (request_id_out == NULL) {
        return false;
    }
    *request_id_out = 0u;
    if (request == NULL || !s_phonebook_lock_ready) {
        return false;
    }

    uint32_t request_id = 0u;
    critical_section_enter_blocking(&s_phonebook_lock);
    bool reserved =
        modem_phonebook_state_reserve_request(&request_id);
    critical_section_exit(&s_phonebook_lock);
    if (!reserved) {
        s_request_admission_failures++;
        return false;
    }

    request->request_id = request_id;
    if (!queue_request(request)) {
        critical_section_enter_blocking(&s_phonebook_lock);
        bool released =
            modem_phonebook_state_release_request(request_id);
        critical_section_exit(&s_phonebook_lock);
        if (!released) {
            LOGE("modem", "phonebook reservation %lu disappeared before admission",
                 (unsigned long)request_id);
        }
        request->request_id = 0u;
        return false;
    }
    *request_id_out = request_id;
    return true;
}

bool modem_service_request_phonebook_list(uint32_t *request_id_out) {
    modem_request_t request;
    memset(&request, 0, sizeof(request));
    request.type = MODEM_REQ_PHONEBOOK_LIST;
    return queue_phonebook_request(&request, request_id_out);
}

bool modem_service_request_phonebook_add(const char *name, const char *number,
                                         uint32_t *request_id_out) {
    if (request_id_out != NULL) {
        *request_id_out = 0u;
    }
    modem_request_t request;
    memset(&request, 0, sizeof(request));
    request.type = MODEM_REQ_PHONEBOOK_ADD;
    copy_bounded(request.name, sizeof(request.name), name);
    copy_bounded(request.number, sizeof(request.number), number);
    return request.number[0] != '\0' &&
           queue_phonebook_request(&request, request_id_out);
}

bool modem_service_request_phonebook_update(uint16_t index, const char *name,
                                            const char *number,
                                            uint32_t *request_id_out) {
    if (request_id_out != NULL) {
        *request_id_out = 0u;
    }
    modem_request_t request;
    memset(&request, 0, sizeof(request));
    request.type = MODEM_REQ_PHONEBOOK_UPDATE;
    request.index = index;
    copy_bounded(request.name, sizeof(request.name), name);
    copy_bounded(request.number, sizeof(request.number), number);
    return request.index >= MODEM_PHONEBOOK_FIRST_INDEX &&
           request.index <= MODEM_PHONEBOOK_LAST_INDEX &&
           request.number[0] != '\0' &&
           queue_phonebook_request(&request, request_id_out);
}

bool modem_service_request_phonebook_delete(uint16_t index,
                                            uint32_t *request_id_out) {
    if (request_id_out != NULL) {
        *request_id_out = 0u;
    }
    modem_request_t request;
    memset(&request, 0, sizeof(request));
    request.type = MODEM_REQ_PHONEBOOK_DELETE;
    request.index = index;
    return request.index >= MODEM_PHONEBOOK_FIRST_INDEX &&
           request.index <= MODEM_PHONEBOOK_LAST_INDEX &&
           queue_phonebook_request(&request, request_id_out);
}

bool modem_service_pop_phonebook_result(modem_phonebook_result_t *out) {
    if (out == 0 || !s_phonebook_lock_ready) {
        return false;
    }
    bool ok = false;
    critical_section_enter_blocking(&s_phonebook_lock);
    ok = modem_phonebook_state_pop_result(out);
    critical_section_exit(&s_phonebook_lock);
    return ok;
}

bool modem_service_phonebook_cache_valid(void) {
    if (!s_phonebook_lock_ready) {
        return false;
    }
    critical_section_enter_blocking(&s_phonebook_lock);
    bool valid = modem_phonebook_state_cache_valid();
    critical_section_exit(&s_phonebook_lock);
    return valid;
}

uint16_t modem_service_phonebook_count(void) {
    if (!s_phonebook_lock_ready) {
        return 0;
    }
    critical_section_enter_blocking(&s_phonebook_lock);
    uint16_t count = modem_phonebook_state_count();
    critical_section_exit(&s_phonebook_lock);
    return count;
}

bool modem_service_phonebook_entry(uint16_t position, modem_phonebook_entry_t *out) {
    if (out == 0 || !s_phonebook_lock_ready) {
        return false;
    }
    bool ok = false;
    critical_section_enter_blocking(&s_phonebook_lock);
    ok = modem_phonebook_state_entry(position, out);
    critical_section_exit(&s_phonebook_lock);
    return ok;
}

bool modem_service_rx_trace_start(void) {
    if (!s_status_lock_ready) return false;
    critical_section_enter_blocking(&s_status_lock);
    memset(&s_rx_trace_status, 0, sizeof(s_rx_trace_status));
    s_rx_trace_write = 0u;
    s_rx_trace_status.enabled = true;
    critical_section_exit(&s_status_lock);
    return true;
}

void modem_service_rx_trace_stop(void) {
    if (!s_status_lock_ready) return;
    critical_section_enter_blocking(&s_status_lock);
    s_rx_trace_status.enabled = false;
    critical_section_exit(&s_status_lock);
}

void modem_service_rx_trace_status(modem_rx_trace_status_t *out) {
    if (out == NULL) return;
    memset(out, 0, sizeof(*out));
    if (!s_status_lock_ready) return;
    critical_section_enter_blocking(&s_status_lock);
    *out = s_rx_trace_status;
    critical_section_exit(&s_status_lock);
}

size_t modem_service_rx_trace_read(size_t offset, uint8_t *out, size_t capacity) {
    if (!s_status_lock_ready || out == NULL || capacity == 0u) return 0u;
    critical_section_enter_blocking(&s_status_lock);
    size_t count = 0u;
    if (!s_rx_trace_status.enabled && offset < s_rx_trace_status.retained) {
        count = s_rx_trace_status.retained - offset;
        if (count > capacity) count = capacity;
        size_t start = (s_rx_trace_write + MODEM_RX_TRACE_CAPACITY -
                        s_rx_trace_status.retained + offset) %
                       MODEM_RX_TRACE_CAPACITY;
        for (size_t i = 0u; i < count; i++) {
            out[i] = s_rx_trace[(start + i) % MODEM_RX_TRACE_CAPACITY];
        }
    }
    critical_section_exit(&s_status_lock);
    return count;
}

static void drain_rx(void) {
    uint8_t buf[MODEM_RX_CHUNK];
    uint32_t remaining = MODEM_RX_TICK_BUDGET;
    while (remaining != 0u) {
        uint32_t request = remaining < MODEM_RX_CHUNK
            ? remaining : MODEM_RX_CHUNK;
        uint32_t count = modem_uart_hal_read_available(buf, request);
        if (count == 0) {
            break;
        }
        remaining -= count;
        critical_section_enter_blocking(&s_status_lock);
        s_status.rx_bytes += count;
        if (s_rx_trace_status.enabled) {
            for (uint32_t i = 0u; i < count; i++) {
                s_rx_trace[s_rx_trace_write] = buf[i];
                s_rx_trace_write = (s_rx_trace_write + 1u) % MODEM_RX_TRACE_CAPACITY;
            }
            uint32_t retained = s_rx_trace_status.retained + count;
            s_rx_trace_status.retained = retained < MODEM_RX_TRACE_CAPACITY
                ? (uint16_t)retained : MODEM_RX_TRACE_CAPACITY;
            if (UINT32_MAX - s_rx_trace_status.received < count) {
                s_rx_trace_status.received = UINT32_MAX;
            } else {
                s_rx_trace_status.received += count;
            }
        }
        critical_section_exit(&s_status_lock);
        for (uint32_t i = 0; i < count; i++) {
            feed_byte(buf[i]);
        }
        if (count < request) {
            break;
        }
    }
}

static void feed_byte(uint8_t byte) {
    /* A +CMT body is read raw by <length>: the framer would drop CR, split
     * on LF and end the C string at 0x00 (GSM '@'). The module emits header
     * and body as one unit, so no prompt or line can interleave here. */
    if (modem_sms_direct_raw_active()) {
        direct_feed_raw(byte);
        if (!modem_sms_direct_raw_take_unconsumed()) {
            return;
        }
    }
    /* '>' is the SMS-send prompt only while we are actually waiting for it
     * (AT+CMGS / AT+CMGW). Otherwise it is an ordinary character -- intercepting
     * it globally silently strips '>' from echoes, URCs and message bodies.
     * The bare '>' must still be matched mid-line because the "> "
     * prompt carries no trailing newline. */
    if (byte == '>' && s_active &&
        (s_active_kind == MODEM_AT_SMS_CMGS_PROMPT || s_active_kind == MODEM_AT_SMS_CMGW_PROMPT)) {
        process_prompt();
        return;
    }
    modem_line_framer_event_t event =
        modem_line_framer_feed(&s_line_framer, byte);
    if (event == MODEM_LINE_FRAMER_LINE) {
        process_line(modem_line_framer_line(&s_line_framer));
    } else if (event == MODEM_LINE_FRAMER_DROPPED) {
        /* A line we were depending on is gone; abort any in-progress +CMGR
         * body collection so the next line is not mistaken for the body.
         * A lost body then degrades to a graceful empty read. The same goes
         * for a +CMT header waiting for its body line: the dropped line was
         * that body, so the collector must not eat the next line instead.
         * (A header with a <length> reads its body raw and is normally not
         * pending here; the reset is unconditional either way.) */
        modem_sms_protocol_line_dropped();
        modem_phonebook_protocol_line_dropped();
        modem_sms_direct_reset();
        s_line_drop_count++;
        LOGW("modem", "dropped overlong AT line");
    }
}

/* --- direct delivery (+CMT) ring --------------------------------------------
 * A +CMT arrives as header + payload; the collector (modem_sms_direct) turns
 * the pair into an SMS-DELIVER PDU which is held here until the
 * STORE_DELIVERED operation has written it back into ME as REC UNREAD. The
 * stored row then follows the +CMTI bookkeeping (pending arrival, counters,
 * receive-store check) so the mailbox scan, multipart and VVM paths are
 * unchanged. Entries survive a failed or cancelled store and are retried from
 * the idle scheduler, bounded to MODEM_DIRECT_STORE_ATTEMPTS. Stores run in
 * arrival order (FIFO by sequence number), so a slot freed and refilled
 * while older entries wait never overtakes them. */
#define MODEM_DIRECT_RING_DEPTH 8u
#define MODEM_DIRECT_STORE_ATTEMPTS 3u
/* Internal request id: never handed to an app, never reserved in
 * modem_sms_state (which owns only app-facing result channels). */
#define MODEM_DIRECT_INTERNAL_REQUEST_ID 0xFFFFFFF0u

typedef struct {
    char pdu_hex[SMS_DELIVER_HEX_MAX];
    uint32_t seq;     /* arrival order; monotonic, wrap-safe comparison */
    uint8_t tpdu_len;
    uint8_t attempts;
    bool used;
} modem_direct_entry_t;

static modem_direct_entry_t s_direct_ring[MODEM_DIRECT_RING_DEPTH];
static uint8_t s_direct_active; /* entry being stored, or UINT8_MAX */
static uint32_t s_direct_seq;   /* last sequence number handed out */
/* A header whose body stops arriving must not keep the collector (and the
 * transport, which stays awake while a body is pending) waiting forever: the
 * collector has no time source, so the service bounds the wait. Armed on the
 * header, refreshed on every accepted body byte, cleared on completion. */
#define MODEM_DIRECT_BODY_TIMEOUT_MS 5000u
static bool s_direct_body_deadline_armed;
static uint32_t s_direct_body_deadline_ms;
/* A full ME store: the module already acknowledged the network, so a held
 * entry must not burn its attempts on "memory full". The ring is blocked
 * until a DELETE completes OK, a +CPMS? poll reports room in the receive
 * store, or MODEM_DIRECT_STORAGE_FULL_RETRY_MS elapse, whichever first. */
#define MODEM_DIRECT_STORAGE_FULL_RETRY_MS 60000u
static bool s_direct_store_blocked;
static uint32_t s_direct_store_blocked_ms;
/* Build target while the ring is full: a real-size sink, so a valid body
 * still parses (the RAW reader terminates on a parsed candidate) and is
 * dropped as "ring full" at once instead of being held until the deadline. */
static char s_direct_sink[SMS_DELIVER_HEX_MAX];

static void direct_ring_reset(void) {
    memset(s_direct_ring, 0, sizeof(s_direct_ring));
    s_direct_active = UINT8_MAX;
    s_direct_body_deadline_armed = false;
    s_direct_store_blocked = false;
    modem_sms_direct_reset();
}

static void direct_store_unblock(const char *why) {
    if (s_direct_store_blocked) {
        s_direct_store_blocked = false;
        LOGI("modem", "direct SMS store unblocked: %s", why);
    }
}

static void direct_body_deadline_tick(uint32_t now_ms) {
    if (!s_direct_body_deadline_armed) {
        return;
    }
    if (!modem_sms_direct_pending()) {
        s_direct_body_deadline_armed = false;
        return;
    }
    if (time_diff_ms(now_ms, s_direct_body_deadline_ms) < 0) {
        return;
    }
    /* The body never completed: the delivery is lost (the module already
     * acknowledged the network); later bytes go back to the line framer. */
    s_direct_body_deadline_armed = false;
    modem_sms_direct_reset();
    critical_section_enter_blocking(&s_status_lock);
    s_status.command_errors++;
    critical_section_exit(&s_status_lock);
    LOGW("modem", "direct SMS body did not complete; message lost");
}

static void direct_count_error_locked_free(void) {
    critical_section_enter_blocking(&s_status_lock);
    s_status.command_errors++;
    critical_section_exit(&s_status_lock);
}

static uint8_t direct_free_slot(void) {
    for (uint8_t i = 0u; i < MODEM_DIRECT_RING_DEPTH; i++) {
        if (!s_direct_ring[i].used) {
            return i;
        }
    }
    return UINT8_MAX;
}

/* Apply one collector step. The PDU was built straight into the free ring
 * slot (or the static sink when the ring is full): the collector's frames
 * beneath this one already carry the core0 stack budget, so no hex buffer
 * lives here. */
static void direct_apply_step(modem_sms_direct_step_t step, uint8_t slot,
                              uint8_t tpdu_len) {
    switch (step) {
    case MODEM_SMS_DIRECT_STEP_HEADER:
        s_direct_body_deadline_armed = true;
        s_direct_body_deadline_ms = s_now_ms + MODEM_DIRECT_BODY_TIMEOUT_MS;
        break;
    case MODEM_SMS_DIRECT_STEP_READY:
        s_direct_body_deadline_armed = false;
        if (slot >= MODEM_DIRECT_RING_DEPTH) {
            /* Built into the sink: nowhere to hold it. */
            LOGW("modem", "direct SMS dropped: ring full");
            direct_count_error_locked_free();
            break;
        }
        s_direct_ring[slot].tpdu_len = tpdu_len;
        s_direct_ring[slot].attempts = 0u;
        s_direct_ring[slot].seq = ++s_direct_seq;
        s_direct_ring[slot].used = true;
        LOGI("modem", "direct SMS rebuilt tpdu=%u slot=%u",
             (unsigned)tpdu_len, (unsigned)slot);
        break;
    case MODEM_SMS_DIRECT_STEP_REJECTED:
        s_direct_body_deadline_armed = false;
        /* The module already acknowledged the network: the message is lost.
         * Same loss class as a quarantined stored row. */
        if (slot >= MODEM_DIRECT_RING_DEPTH) {
            LOGW("modem", "direct SMS dropped: ring full");
        } else {
            LOGW("modem", "direct SMS not understood; message lost");
        }
        direct_count_error_locked_free();
        break;
    case MODEM_SMS_DIRECT_STEP_IGNORED:
    default:
        if (s_direct_body_deadline_armed) {
            /* A body byte was accepted: the body is still arriving. */
            s_direct_body_deadline_ms = s_now_ms + MODEM_DIRECT_BODY_TIMEOUT_MS;
        }
        break;
    }
}

/* Feed one line to the collector: the +CMT header from route_urc, or (for a
 * header without a <length> field) the payload line from process_line, which
 * owns the line after such a header unconditionally. A header with <length>
 * > 0 switches the collector to RAW mode (see feed_byte). */
static void direct_feed_line(const char *line) {
    uint8_t slot = direct_free_slot();
    char *pdu_hex = slot < MODEM_DIRECT_RING_DEPTH
        ? s_direct_ring[slot].pdu_hex : s_direct_sink;
    size_t pdu_hex_cap = slot < MODEM_DIRECT_RING_DEPTH
        ? sizeof(s_direct_ring[slot].pdu_hex) : sizeof(s_direct_sink);
    uint8_t tpdu_len = 0u;
    modem_sms_direct_step_t step = modem_sms_direct_feed(
        line, g_modem_vendor.translate_direct_sms, pdu_hex, pdu_hex_cap,
        &tpdu_len);
    if (step == MODEM_SMS_DIRECT_STEP_HEADER) {
        LOGI("modem", "%s", line);
    }
    direct_apply_step(step, slot, tpdu_len);
}

/* RAW mode: one wire byte of the +CMT body, bypassing the line framer. */
static void direct_feed_raw(uint8_t byte) {
    uint8_t slot = direct_free_slot();
    char *pdu_hex = slot < MODEM_DIRECT_RING_DEPTH
        ? s_direct_ring[slot].pdu_hex : s_direct_sink;
    size_t pdu_hex_cap = slot < MODEM_DIRECT_RING_DEPTH
        ? sizeof(s_direct_ring[slot].pdu_hex) : sizeof(s_direct_sink);
    uint8_t tpdu_len = 0u;
    /* Two statements: the call writes tpdu_len and the apply reads it, and C
     * leaves the evaluation order of function arguments unspecified. */
    modem_sms_direct_step_t step = modem_sms_direct_feed_raw(
        byte, g_modem_vendor.translate_direct_sms, pdu_hex, pdu_hex_cap,
        &tpdu_len);
    direct_apply_step(step, slot, tpdu_len);
}

/* The oldest held entry (smallest sequence number), or UINT8_MAX. */
static uint8_t direct_oldest_slot(void) {
    uint8_t oldest = UINT8_MAX;
    for (uint8_t i = 0u; i < MODEM_DIRECT_RING_DEPTH; i++) {
        if (s_direct_ring[i].used &&
            (oldest == UINT8_MAX ||
             (int32_t)(s_direct_ring[i].seq - s_direct_ring[oldest].seq) < 0)) {
            oldest = i;
        }
    }
    return oldest;
}

/* Idle "SMS mode restore": AT+CMGF=1 when a cancelled SMS operation may have
 * left the module in PDU mode (see sms_cancel_protocol). Same gate as the
 * ring drain; cleared on OK, retried next idle tick on ERROR/timeout up to
 * MODEM_SMS_MODE_RESTORE_ATTEMPTS. Returns true only when it sent the command. */
static bool sms_mode_restore_start(uint32_t now_ms) {
    if (!s_sms_mode_restore_pending || s_operation != MODEM_OP_NONE || s_active ||
        s_dtr_wake_pending) {
        return false;
    }
    if (s_sms_mode_restore_attempts >= MODEM_SMS_MODE_RESTORE_ATTEMPTS) {
        LOGW("modem", "SMS text-mode restore gave up after %u attempts",
             (unsigned)s_sms_mode_restore_attempts);
        s_sms_mode_restore_pending = false;
        return false;
    }
    send_command(MODEM_AT_SMS_MODE_RESTORE, "AT+CMGF=1", 5000u, now_ms);
    return true;
}

/* Idle-scheduler drain: queue one STORE_DELIVERED request for the oldest
 * held entry. Same gate as the neighbouring background branches; never
 * touches DTR/RI. Returns true only when a request was queued. */
static bool direct_ring_start(uint32_t now_ms) {
    if (s_operation != MODEM_OP_NONE || s_active || s_dtr_wake_pending ||
        !status_sim_ready_snapshot()) {
        return false;
    }
    if (s_direct_store_blocked) {
        if (time_diff_ms(now_ms, s_direct_store_blocked_ms +
                                     MODEM_DIRECT_STORAGE_FULL_RETRY_MS) < 0) {
            return false;
        }
        direct_store_unblock("retry interval elapsed");
    }
    for (;;) {
        uint8_t i = direct_oldest_slot();
        if (i == UINT8_MAX) {
            return false;
        }
        if (s_direct_ring[i].attempts >= MODEM_DIRECT_STORE_ATTEMPTS) {
            LOGW("modem", "direct SMS store gave up after %u attempts",
                 (unsigned)s_direct_ring[i].attempts);
            s_direct_ring[i].used = false;
            direct_count_error_locked_free();
            continue;
        }
        modem_request_t request;
        memset(&request, 0, sizeof(request));
        request.type = MODEM_REQ_STORE_DELIVERED_SMS;
        request.request_id = MODEM_DIRECT_INTERNAL_REQUEST_ID;
        if (!queue_request(&request)) {
            return false;
        }
        /* attempts counts stores the modem answered (PUBLISH_DELIVERED),
         * not queue admissions: an evicted request never asked the modem. */
        s_direct_active = i;
        return true;
    }
}

static void process_line(char *line) {
    while (*line == ' ') {
        line++;
    }
    size_t len = strlen(line);
    while (len > 0 && line[len - 1u] == ' ') {
        line[--len] = '\0';
    }
    if (len == 0) {
        return;
    }
    copy_bounded(s_debug_last_line, sizeof(s_debug_last_line), line);

    if (modem_sms_direct_pending()) {
        /* The line after a +CMT header without a <length> field is its
         * payload by protocol, even when it happens to read like a final or
         * a URC. It must never complete an active command. (A header with a
         * length reads its body raw; no line reaches here in that mode.) */
        direct_feed_line(line);
        return;
    }

    if (g_modem_vendor.parse_sim_observation != NULL) {
        apply_sim_observation(g_modem_vendor.parse_sim_observation(line));
    }
    if (s_active && s_active_kind == MODEM_AT_INIT &&
        s_init_index < g_modem_vendor.init_step_count &&
        g_modem_vendor.init_steps[s_init_index].degrade ==
            MODEM_DEGRADE_SIM_GATE &&
        is_sim_absent_error(line)) {
        /* +CME 10 is standardized by 3GPP; textual CMEE=2 wording is accepted
         * too. This fallback keeps the path correct if a vendor presence query
         * was unavailable while CPIN still conclusively reports no card. */
        apply_sim_observation(MODEM_SIM_OBSERVATION_ABSENT);
    }

    bool ok = false;
    if (s_active && parse_expected_line(line)) {
        return;
    }
    if (parse_final_result(line, &ok)) {
        /* Complete the active command on a call-progress token ONLY when a call
         * command is active; otherwise route it as the URC it is. Without this,
         * a remote hangup (unsolicited NO CARRIER) during an in-call AT+CMGR /
         * Net Monitor poll would finish that command as failed -- and on a
         * CMGS_FINAL it would report an already-accepted SMS send as failed,
         * causing a duplicate resend. OK / ERROR / +CME / +CMS stay universal
         * finals for any active command. */
        if (is_call_progress_final(line) &&
            !(s_active && is_call_at_kind(s_active_kind) &&
              g_modem_vendor.call.progress_finals_may_complete_command)) {
            route_urc(line);
            return;
        }
        if (s_active && s_operation == MODEM_OP_SEND_BINARY_SMS &&
            (s_active_kind == MODEM_AT_SMS_CMGF_PDU ||
             s_active_kind == MODEM_AT_SMS_CMGS_PROMPT ||
             s_active_kind == MODEM_AT_SMS_CMGS_FINAL ||
             s_active_kind == MODEM_AT_SMS_CMGF_TEXT)) {
            LOGI("modem", "SMS final kind=%u ok=%u line=%s",
                 (unsigned)s_active_kind,
                 ok ? 1u : 0u,
                 line);
        }
        if (s_active) {
            finish_command_result(ok, line);
        } else {
            route_urc(line);
        }
        return;
    }
    route_urc(line);
}

static void process_prompt(void) {
    if (!s_active ||
        (s_active_kind != MODEM_AT_SMS_CMGS_PROMPT && s_active_kind != MODEM_AT_SMS_CMGW_PROMPT)) {
        LOGD("modem", "prompt");
        return;
    }
    modem_sms_command_kind_t kind;
    modem_sms_protocol_request_t request;
    if (!sms_protocol_command_from_at(s_active_kind, &kind) ||
        !sms_protocol_request_view(&request) ||
        !modem_sms_protocol_on_prompt(
            kind, &request, &s_sms_protocol_hooks, s_now_ms)) {
        LOGE("modem", "SMS prompt has no valid protocol owner");
    }
}

static void apply_call_event(const modem_call_event_t *ev) {
    call_leg_state_t model_state = CALL_LEG_UNKNOWN;
    call_direction_t model_dir = CALL_DIR_UNKNOWN;
    switch (ev->event) {
    case MODEM_CALL_EV_ACTIVE:       model_state = CALL_LEG_ACTIVE; break;
    case MODEM_CALL_EV_HELD:         model_state = CALL_LEG_HELD; break;
    case MODEM_CALL_EV_DIALING:      model_state = CALL_LEG_DIALING; model_dir = CALL_DIR_MO; break;
    case MODEM_CALL_EV_ALERTING_MO:  model_state = CALL_LEG_ALERTING; model_dir = CALL_DIR_MO; break;
    case MODEM_CALL_EV_RINGING_MT:   model_state = CALL_LEG_INCOMING; model_dir = CALL_DIR_MT; break;
    case MODEM_CALL_EV_WAITING_MT:   model_state = CALL_LEG_WAITING; model_dir = CALL_DIR_MT; break;
    case MODEM_CALL_EV_RELEASED:     model_state = CALL_LEG_RELEASING; break;
    case MODEM_CALL_EV_SETUP_DONE:
    case MODEM_CALL_EV_BUSY:         break;
    }
    call_model_on_event(&s_call_model, ev->call_id, ev->id_valid,
                        model_state, model_dir);
}

static void route_urc(const char *line) {
    if (strcmp(line, "RING") == 0 || starts_with(line, "+CLIP:") ||
        is_ccwa_urc(line) || is_vendor_call_urc(line) ||
        is_call_progress_final(line)) {
        diag_cancel_active_group();
    }
    if (s_operation == MODEM_OP_DTMF &&
        (strcmp(line, "RING") == 0 || is_ccwa_urc(line) ||
         is_vendor_call_urc(line) || is_call_progress_final(line))) {
        /* Telit ECAM ids are deliberately treated as coarse. Even without a
         * trustworthy leg id, a topology event means an IVR suffix must not
         * race a release, hold, waiting call, or newly connected leg. */
        s_dtmf_cancel_requested = true;
    }
    if (strcmp(line, "RING") == 0) {
        call_model_on_ring(&s_call_model);
        critical_section_enter_blocking(&s_status_lock);
        s_status.urc_count++;
        critical_section_exit(&s_status_lock);
        LOGI("modem", "URC RING");
    } else if (starts_with(line, "+CLIP:")) {
        modem_cli_line_t parsed;
        if (modem_line_parse_clip(line, &parsed)) {
            call_model_on_clip(&s_call_model, parsed.number, parsed.validity);
        } else {
            model_note_coarse_call_observation();
        }
        critical_section_enter_blocking(&s_status_lock);
        s_status.urc_count++;
        critical_section_exit(&s_status_lock);
        LOGI("modem", "%s", line);
    } else if (starts_with(line, "+CMTI:")) {
        uint16_t index = 0u;
        bool tracked = parse_cmti_index(line, &index) &&
                       modem_sms_protocol_track_pending_arrival(index);
        critical_section_enter_blocking(&s_status_lock);
        s_status.sms_received_count++;
        if (!tracked) {
            /* A malformed, foreign-storage, or overflowed indication cannot be
             * safely classified. Fail open so a real user SMS is never silent. */
            s_status.sms_user_received_count++;
        }
        s_status.urc_count++;
        critical_section_exit(&s_status_lock);
        s_cpms_check_needed = true; /* arrival may have filled the receive store */
        LOGI("modem", "%s", line);
    } else if (starts_with(line, "+CMT:")) {
        critical_section_enter_blocking(&s_status_lock);
        s_status.urc_count++;
        critical_section_exit(&s_status_lock);
        direct_feed_line(line);
    } else if (starts_with(line, "+CPIN:")) {
        (void)parse_cpin(line);
        critical_section_enter_blocking(&s_status_lock);
        s_status.urc_count++;
        critical_section_exit(&s_status_lock);
    } else if (starts_with(line, "+PACSP") || strcmp(line, "Call Ready") == 0) {
        critical_section_enter_blocking(&s_status_lock);
        s_status.urc_count++;
        critical_section_exit(&s_status_lock);
        LOGI("modem", "%s", line);
    } else if (starts_with(line, "+CEREG:")) {
        parse_cereg(line);
        critical_section_enter_blocking(&s_status_lock);
        s_status.urc_count++;
        critical_section_exit(&s_status_lock);
    } else if (starts_with(line, "+CREG:")) {
        parse_cereg(line);
        critical_section_enter_blocking(&s_status_lock);
        s_status.urc_count++;
        critical_section_exit(&s_status_lock);
    } else if (starts_with(line, "+CIREGU:")) {
        critical_section_enter_blocking(&s_status_lock);
        s_status.urc_count++;
        critical_section_exit(&s_status_lock);
    } else if (starts_with(line, "+CIEV:")) {
        /* Production explicitly disables generic indicator URCs with CMER.
         * Keep an unexpected/stale-profile line isolated from any in-flight
         * command, but never use its coarse vendor value for signal bars. */
        critical_section_enter_blocking(&s_status_lock);
        s_status.urc_count++;
        critical_section_exit(&s_status_lock);
    } else if (starts_with(line, "+CEREGU:")) {
        critical_section_enter_blocking(&s_status_lock);
        s_status.urc_count++;
        critical_section_exit(&s_status_lock);
    } else if (strcmp(line, "CONNECT") == 0) {
        call_model_on_bare_final(&s_call_model, MODEM_CALL_RESULT_CONNECTED, false);
        critical_section_enter_blocking(&s_status_lock);
        s_status.urc_count++;
        critical_section_exit(&s_status_lock);
        LOGI("modem", "%s", line);
    } else if (is_ccwa_urc(line)) {
        /* Standard pre-id call-waiting indication. The per-id vendor URC/CLCC
         * will bind it later; meanwhile the pending-MT episode projects waiting
         * alongside an established foreground call. +CCWA reporting must be
         * enabled by the vendor init table; unlike the ID-scoped call event it
         * carries CLI. */
        modem_cli_line_t parsed;
        if (modem_line_parse_ccwa(line, &parsed)) {
            call_model_on_ring(&s_call_model);
            call_model_on_clip(&s_call_model, parsed.number, parsed.validity);
        } else {
            /* It had the quoted URC shape but was malformed. Preserve transport
             * uncertainty without inventing a caller or mutating call topology. */
            model_note_coarse_call_observation();
            LOGW("modem", "discarded malformed call-waiting URC");
        }
        critical_section_enter_blocking(&s_status_lock);
        s_status.urc_count++;
        critical_section_exit(&s_status_lock);
        LOGI("modem", "%s", line);
    } else if (is_vendor_call_urc(line)) {
        modem_call_event_t ev;
        if (g_modem_vendor.parse_call_urc != NULL &&
            g_modem_vendor.parse_call_urc(line, &ev)) {
            apply_call_event(&ev);
        } else {
            model_note_coarse_call_observation();
            LOGW("modem", "discarded malformed call-state URC");
        }
        critical_section_enter_blocking(&s_status_lock);
        s_status.urc_count++;
        critical_section_exit(&s_status_lock);
        LOGI("modem", "%s", line);
    } else if (is_vendor_aux_urc(line)) {
        bool parsed = observe_vendor_aux_line(line);
        critical_section_enter_blocking(&s_status_lock);
        s_status.urc_count++;
        critical_section_exit(&s_status_lock);
        if (!parsed) {
            LOGW("modem", "discarded malformed vendor URC");
        } else {
            LOGI("modem", "%s", line);
        }
    } else if (strcmp(line, "NO CARRIER") == 0 || strcmp(line, "BUSY") == 0 ||
               strcmp(line, "NO ANSWER") == 0 || strcmp(line, "NO DIALTONE") == 0 ||
               strcmp(line, "NO DIAL TONE") == 0) {
        modem_call_result_t result;
        if (strcmp(line, "BUSY") == 0) {
            result = MODEM_CALL_RESULT_BUSY;
        } else if (strcmp(line, "NO ANSWER") == 0) {
            result = MODEM_CALL_RESULT_NO_ANSWER;
        } else if (strcmp(line, "NO DIALTONE") == 0 || strcmp(line, "NO DIAL TONE") == 0) {
            result = MODEM_CALL_RESULT_NO_DIALTONE;
        } else {
            result = MODEM_CALL_RESULT_NO_CARRIER;
        }
        call_model_on_bare_final(&s_call_model, result, false);
        critical_section_enter_blocking(&s_status_lock);
        s_status.urc_count++;
        critical_section_exit(&s_status_lock);
        LOGI("modem", "%s", line);
    } else {
        LOGD("modem", "line: %s", line);
    }
}

static void process_timeout(uint32_t now_ms) {
    if (s_state == MODEM_STATE_POWER_PULSE && time_diff_ms(now_ms, s_power_release_ms) >= 0) {
        modem_uart_hal_set_power_pin(false);
        if (g_modem_vendor.power_is_on != NULL) {
            modem_enter_module_wait(now_ms);
        } else {
            s_state = MODEM_STATE_PROBE;
        }
        s_next_action_ms = now_ms;
        LOGI("modem", "PWR pulse released");
    }

    if (!s_active || time_diff_ms(now_ms, s_command_deadline_ms) < 0) {
        return;
    }

    modem_at_kind_t kind = s_active_kind;
    uint32_t call_token = s_active_call_token;
    bool sms_wake_arm_command = s_active_sms_wake_arm;
    s_active = false;
    s_active_kind = MODEM_AT_NONE;
    s_active_call_token = 0u;
    s_active_sms_wake_arm = false;
    if (sms_wake_arm_command) {
        s_sms_wake_armed = false;
    }
    if (call_token != 0u) {
        modem_cmd_result_t result = { .status = CMD_TIMEOUT };
        call_model_set_now(&s_call_model, now_ms);
        call_model_txn_command_result(&s_call_model, call_token, result);
    }
    if (!at_kind_is_operation(kind) && kind != MODEM_AT_DIAG_QUERY) {
        critical_section_enter_blocking(&s_status_lock);
        s_status.command_errors++;
        critical_section_exit(&s_status_lock);
    }
    LOGW("modem", "AT timeout kind=%u cmd=%s", (unsigned)kind,
         s_debug_last_command);

    if (kind == MODEM_AT_CLCC_MODEL) {
        call_model_clcc_error(&s_call_model);
        return;
    }

    if (kind == MODEM_AT_DIAG_QUERY) {
        diag_finish_query(false, true, now_ms);
        return;
    }

    if (kind == MODEM_AT_SIGNAL) {
        signal_finish_query(false, now_ms);
        return;
    }

    if (kind == MODEM_AT_SIM_PROVIDER) {
        sim_provider_finish_query(false);
        /* CRSM is non-abortable. Retain its final-result ownership after the
         * name deadline so a late OK cannot complete a queued call or SMS. */
        s_active = true;
        s_active_kind = MODEM_AT_SIM_PROVIDER_DRAIN;
        s_command_deadline_ms = now_ms + MODEM_SIM_PROVIDER_DRAIN_MS;
        return;
    }

    if (kind == MODEM_AT_SIM_PROVIDER_DRAIN) {
        s_power_off_failure_pending = s_power_off_pending;
        modem_fail_power_state("SIM read did not release the AT channel", true);
        return;
    }

    if (kind == MODEM_AT_MAINTENANCE) {
        maintenance_finish_command(false, true, now_ms);
        return;
    }

    if (kind == MODEM_AT_SMS_WAKE_ARM) {
        sms_wake_handle_final(false, now_ms);
        return;
    }

    if (kind == MODEM_AT_SMS_MODE_RESTORE) {
        s_sms_mode_restore_attempts++; /* retried from the next idle tick */
        return;
    }

    modem_sms_command_kind_t sms_kind;
    if (sms_protocol_command_from_at(kind, &sms_kind)) {
        modem_sms_protocol_request_t request;
        if (sms_protocol_request_view(&request)) {
            modem_sms_protocol_on_timeout(
                sms_kind, &request, &s_sms_protocol_hooks, now_ms);
        } else {
            (void)sms_publish_terminal_for_request(
                &s_current_request, MODEM_SMS_OUTCOME_TIMEOUT);
            finish_operation(false);
        }
        return;
    }

    modem_phonebook_command_kind_t phonebook_kind;
    if (phonebook_protocol_command_from_at(kind, &phonebook_kind)) {
        modem_phonebook_protocol_request_t request;
        if (phonebook_protocol_request_view(&request)) {
            modem_phonebook_protocol_on_timeout(
                phonebook_kind, &request, &s_phonebook_protocol_hooks);
        } else {
            modem_phonebook_op_t operation;
            if (phonebook_operation_from_request_type(
                    s_current_request.type, &operation)) {
                phonebook_finish_operation(
                    s_current_request.request_id, operation,
                    MODEM_PHONEBOOK_OUTCOME_TIMEOUT);
            } else {
                finish_operation(false);
            }
        }
        return;
    }

    if (kind == MODEM_AT_CALL_FORWARD) {
        call_forward_finish_step(false, true);
        return;
    }

    if (kind == MODEM_AT_CALL_FORWARD_FLAGS) {
        modem_supplementary_call_forward_flags_finish(false, now_ms);
        return;
    }

    if (kind == MODEM_AT_VOICE_MAILBOX) {
        voice_mailbox_finish(false);
        return;
    }

    if (kind == MODEM_AT_MESSAGE_WAITING) {
        message_waiting_finish(false);
        return;
    }

    if (kind == MODEM_AT_PING) {
        handle_ping_failed(now_ms);
    } else if (kind == MODEM_AT_INIT) {
        if (++s_init_retries <= init_retry_limit()) {
            s_next_action_ms = now_ms + MODEM_POST_READY_SETTLE_MS;
        } else if (init_step_recoverable()) {
            LOGW("modem", "skipping cold-start init cmd index=%u cmd=%s",
                 (unsigned)s_init_index,
                 g_modem_vendor.init_steps[s_init_index].cmd);
            note_init_step_skipped();
            s_init_index++;
            s_init_retries = 0;
            s_next_action_ms = now_ms + 50u;
        } else {
            LOGE("modem", "init timeout index=%u cmd=%s",
                 (unsigned)s_init_index,
                 g_modem_vendor.init_steps[s_init_index].cmd);
            modem_fail_power_state("required modem init command timed out", true);
        }
    } else if (kind == MODEM_AT_PROVISION_QUERY ||
               kind == MODEM_AT_PROVISION_SET) {
        provision_handle_final(false, true, now_ms);
    } else if (kind == MODEM_AT_PROVISION_REBOOT) {
        /* A reboot command can stop the UART before its final reaches us. The
         * outcome is uncertain, so consume the one-reboot budget and recover
         * through PWRMON/CTS plus a fresh AT/init/provision pass. */
        provision_begin_reboot_wait(now_ms);
    } else {
        if (kind == MODEM_AT_POWER_OFF) {
            /* No OK, but CPWROFF may still be executing -- hold the rail and let
             * the break sense / timeout decide when it is safe to cut. */
            modem_begin_off_discharge(now_ms);
            return;
        }
        if (kind == MODEM_AT_DEBUG) {
            push_debug_result(false);
        }
        finish_operation(false);
    }
}

static void advance_state(uint32_t now_ms) {
    if (s_active || s_dtr_wake_pending || s_ri_release_pending ||
        s_deferred_valid ||
        time_diff_ms(now_ms, s_next_action_ms) < 0) {
        return;
    }
    /* Re-base the scheduling deadline every time the gate passes so it can
     * never sit >2^31 ms stale. The READY steady-state poll branches don't
     * write s_next_action_ms (they use their own signal/CEREG deadlines), so
     * without this it kept its last transition-time value; after ~24.85 days
     * of uptime the signed compare above inverted and froze the ENTIRE modem
     * service (no polls, no dial/answer/hangup/SMS) until ~49.7 days or a
     * reboot. Any state that needs to defer overwrites this below; the OFF
     * dwell already elapsed by the time the gate passes, so it is unaffected. */
    s_next_action_ms = now_ms;

    if (s_binary_restore_pending && s_state == MODEM_STATE_READY) {
        /* The ESC settle has elapsed (this gate was held by s_next_action_ms):
         * ESC's abort answer has drained, so it is now safe to send the binary
         * text-mode restore. Gated on READY: the restore is only ever relevant in
         * the SMS steady state, and firing it in an OFF/OFF_DISCHARGE/re-init
         * state would hijack that state's own advance_state handling (e.g. the
         * power-off break-sense poll). modem_begin_power_off clears the flag on
         * the power-off path; this guard covers any other READY->non-READY exit. */
        s_binary_restore_pending = false;
        modem_sms_protocol_request_t request;
        if (sms_protocol_request_view(&request)) {
            modem_sms_protocol_resume_after_prompt_abort(
                &request, &s_sms_protocol_hooks, now_ms);
        } else {
            finish_operation(false);
        }
        return;
    }

    if (s_state == MODEM_STATE_OFF) {
        bool rail_enabled = shared_3v8_service_enabled();
        bool rail_changed = s_rail_off_transition_snapshot !=
            shared_3v8_service_transition_count();
        if (rail_enabled) {
            /* Another owner (normally buzzer audio) still holds the physical
             * rail. Poll cheaply; no part of this interval is discharge time. */
            s_rail_off_dwell_started = false;
            s_rail_off_dwell_complete = false;
            if (s_power_on_pending) {
                s_next_action_ms = now_ms + MODEM_RAIL_OFF_POLL_MS;
            }
            return;
        }
        if (!s_rail_off_dwell_started || rail_changed) {
            modem_start_rail_off_dwell(now_ms);
            if (s_power_on_pending) {
                s_next_action_ms = s_rail_off_ready_ms;
            }
            return;
        }
        if (!s_rail_off_dwell_complete &&
            time_diff_ms(now_ms, s_rail_off_ready_ms) < 0) {
            if (s_power_on_pending) {
                s_next_action_ms = s_rail_off_ready_ms;
            }
            return;
        }
        /* Latch completion while OFF. A timestamp older than INT32_MAX ms is
         * ambiguous to wrap-safe deadline arithmetic; the minute heartbeat
         * reaches this branch long before then, so a later power key need not
         * compare against an ancient discharge deadline. */
        s_rail_off_dwell_complete = true;
        if (s_power_on_pending) {
            s_power_on_pending = false; /* dwell elapsed (head deadline gate) */
            modem_begin_power_on(now_ms);
        }
        return;
    }

    if (s_state == MODEM_STATE_OFF_DISCHARGE) {
        /* A hardware alarm owns exact control deassertion. The service deadline
         * is the sequencing gate: do not treat a transient PWRMON fall during an
         * asserted control pulse as completed shutdown evidence. */
        if (s_shutdown_pulse_active) {
            if (time_diff_ms(now_ms, s_shutdown_pulse_release_ms) < 0) {
                s_next_action_ms = s_shutdown_pulse_release_ms;
                return;
            }
            modem_uart_hal_cancel_shutdown_pulse();
            s_shutdown_pulse_active = false;
            s_shutdown_pulse_release_ms = 0u;
            uint32_t sense_timeout_ms =
                s_shutdown_stage == MODEM_SHUTDOWN_STAGE_GRACEFUL_CONTROL
                    ? g_modem_vendor.power.graceful_off_sense_timeout_ms
                    : g_modem_vendor.power.emergency_off_sense_timeout_ms;
            if (sense_timeout_ms == 0u) {
                modem_terminal_shutdown_failure(
                    "shutdown fallback has no status-observation budget", now_ms);
                return;
            }
            g_modem_vendor.off_sense_begin(now_ms);
            s_off_discharge_deadline_ms = now_ms + sense_timeout_ms;
            s_next_action_ms = now_ms;
            LOGW("modem", "shutdown fallback pulse released; stage=%u sense=%ums",
                 (unsigned)s_shutdown_stage, (unsigned)sense_timeout_ms);
            return;
        }

        /* Hold VCC until PWRMON qualifies the module's internal switch-off, or
         * this stage's timeout backstop fires. */
        modem_power_observation_t observation = modem_power_observation();
        if (g_modem_vendor.off_complete(now_ms, &observation)) {
            LOGI("modem", "switch-off completion observed");
            modem_enter_off();
            return;
        }
        if (time_diff_ms(now_ms, s_off_discharge_deadline_ms) >= 0) {
            if (g_modem_vendor.power.cut_rail_on_off_timeout) {
                LOGW("modem", "switch-off sense timeout; cutting 3V8 by vendor policy");
                modem_enter_off();
            } else {
                modem_shutdown_start_t start = MODEM_SHUTDOWN_START_UNSUPPORTED;
                if (s_shutdown_stage == MODEM_SHUTDOWN_STAGE_SOFTWARE) {
                    start = modem_start_shutdown_fallback(
                        MODEM_SHUTDOWN_STAGE_GRACEFUL_CONTROL, now_ms);
                    if (start == MODEM_SHUTDOWN_START_UNSUPPORTED) {
                        start = modem_start_shutdown_fallback(
                            MODEM_SHUTDOWN_STAGE_UNCONDITIONAL, now_ms);
                    }
                } else if (s_shutdown_stage ==
                           MODEM_SHUTDOWN_STAGE_GRACEFUL_CONTROL) {
                    start = modem_start_shutdown_fallback(
                        MODEM_SHUTDOWN_STAGE_UNCONDITIONAL, now_ms);
                }
                if (start == MODEM_SHUTDOWN_START_OK) {
                    return;
                }
                modem_terminal_shutdown_failure(
                    start == MODEM_SHUTDOWN_START_FAILED
                        ? "shutdown fallback pulse could not be armed"
                        : "all shutdown stages exhausted while module may be live",
                    now_ms);
            }
        }
        return;
    }

    if (s_state == MODEM_STATE_RAIL_WAIT) {
        modem_power_observation_t observation = modem_power_observation();
        if (!observation.supply_power_good) {
            if (time_diff_ms(now_ms, s_rail_power_good_deadline_ms) >= 0) {
                modem_fail_supply_power_state("modem supply PG timeout", false);
            } else {
                s_next_action_ms = now_ms + MODEM_RAIL_OFF_POLL_MS;
            }
            return;
        }
        if (g_modem_vendor.power_is_on != NULL &&
            g_modem_vendor.power_is_on(&observation)) {
            s_power_pulsed = true;
            modem_enter_module_wait(now_ms);
            s_next_action_ms = now_ms;
            LOGI("modem", "supply PG; module already reports on");
        } else {
            modem_start_power_pulse(now_ms);
        }
        return;
    }

    if (s_state == MODEM_STATE_MODULE_WAIT) {
        modem_power_observation_t observation = modem_power_observation();
        if (g_modem_vendor.power.rail_power_good_timeout_ms != 0u &&
            !observation.supply_power_good) {
            /* The start pulse has already happened (or PWRMON previously said
             * ON). A single PG-low sample is a fault, never qualified module-
             * off evidence, so retain ownership even if the simultaneous ADC
             * sample is invalid or low. */
            modem_fail_supply_power_state(
                "modem supply PG dropped during startup", true);
            return;
        }
        bool module_on = g_modem_vendor.power_is_on == NULL ||
            g_modem_vendor.power_is_on(&observation);
        bool module_off_qualified = g_modem_vendor.power_is_on != NULL &&
            g_modem_vendor.off_complete(now_ms, &observation);
        if (s_provision_reboot_cycle_active &&
            !s_provision_reboot_drop_seen) {
            if (g_modem_vendor.power_is_on == NULL) {
                s_provision_reboot_drop_seen = true;
            } else if (observation.module_status_valid && !module_on) {
                provision_note_reboot_drop(now_ms);
            }
            if (!s_provision_reboot_drop_seen) {
                /* AT#REBOOT acknowledges before Telit actually resets. Do not
                 * mistake the still-running pre-reset UART for the new boot. */
                if (time_diff_ms(now_ms, s_boot_deadline_ms) >= 0) {
                    modem_fail_power_state(
                        "provisioning reboot never asserted module reset", true);
                } else {
                    s_next_action_ms = now_ms + MODEM_RAIL_OFF_POLL_MS;
                }
                return;
            }
        }
        if (module_on && s_uart_parked && modem_uart_hal_rx_idle_high()) {
            /* The module's I/O domain is now proven alive, so RX/CTS can be
             * handed back to PL011 only after its TX actually reaches idle-high.
             * Telit PWRMON precedes UART readiness by several seconds; treating
             * PWRMON alone as transport-ready turns the off-state pull-down into
             * a continuous UART-break interrupt storm during that gap. */
            modem_uart_hal_init();
            modem_uart_hal_set_dtr_sleep_permitted(false);
            s_uart_parked = false;
        }
        bool transport_ready = !s_uart_parked &&
            (g_modem_vendor.wake.strategy != MODEM_WAKE_DTR_CTS ||
             g_modem_vendor.wake.probe_bypass_cts ||
             modem_uart_hal_cts_asserted());
        if (module_on && transport_ready) {
            s_provision_reboot_cycle_active = false;
            s_state = MODEM_STATE_PROBE;
            s_next_action_ms = now_ms;
            LOGI("modem", "module and UART flow control ready");
            return;
        }
        if (time_diff_ms(now_ms, s_boot_deadline_ms) >= 0) {
            if (s_power_off_pending && module_off_qualified) {
                /* The full startup budget elapsed without any module-on
                 * evidence, and the vendor's sustained-low qualification proves
                 * no live module exists to shut down. */
                s_power_off_pending = false;
                LOGW("modem", "startup cancelled before module status asserted");
                modem_enter_off();
            } else if (!s_startup_recycled && module_off_qualified) {
                s_startup_recycled = true;
                s_diag_automatic_recoveries++;
                s_diag_last_recovery_reason =
                    MODEM_DIAG_RECOVERY_STARTUP_STATUS;
                s_power_on_pending = true;
                LOGE("modem", "qualified-off module startup timeout; automatic rail-cycle");
                modem_enter_off();
            } else {
                modem_fail_power_state(
                    module_on
                        ? "CTS did not assert during startup"
                        : (module_off_qualified
                               ? "module status did not assert during startup"
                               : "module status remained indeterminate during startup"),
                    !module_off_qualified);
            }
        } else {
            s_next_action_ms = now_ms + MODEM_RAIL_OFF_POLL_MS;
        }
        return;
    }

    if (s_state == MODEM_STATE_PROBE) {
        send_command(MODEM_AT_PING, "AT", MODEM_PING_TIMEOUT_MS, now_ms);
    } else if (s_state == MODEM_STATE_INIT) {
        uint32_t count = g_modem_vendor.init_step_count;
        if (s_init_index < count) {
            const modem_init_step_t *step =
                &g_modem_vendor.init_steps[s_init_index];
            if (!s_sim_completion_active &&
                (step->prerequisites &
                 MODEM_INIT_PREREQ_SIM_COMPLETION) != 0u) {
                /* This is a deliberate lifecycle bracket, not an unmet SIM
                 * capability. The focused completion pass will execute it. */
                s_init_index++;
                s_init_retries = 0u;
                s_next_action_ms = now_ms + 50u;
                return;
            }
            if (s_sim_completion_active &&
                !init_step_is_sim_dependent(step)) {
                s_init_index++;
                s_init_retries = 0u;
                s_next_action_ms = now_ms + 50u;
                return;
            }
            if (init_step_capture_satisfied(step)) {
                LOGI("modem", "skipping persisted init capture index=%u",
                     (unsigned)s_init_index);
                s_init_index++;
                s_init_retries = 0u;
                s_next_action_ms = now_ms + 50u;
                return;
            }
            if (step->degrade == MODEM_DEGRADE_SIM_GATE &&
                status_sim_missing_snapshot()) {
                LOGI("modem", "SIM absent; skipping known-inapplicable SIM gate");
                s_sim_completion_needed = true;
                s_sim_steps_skipped = true;
                if (s_sim_completion_active) {
                    s_sim_completion_failed = true;
                }
                s_init_index++;
                s_init_retries = 0u;
                s_next_action_ms = now_ms + 50u;
                return;
            }
            if (!init_step_prerequisites_met()) {
                LOGI("modem", "skipping inapplicable init cmd index=%u",
                     (unsigned)s_init_index);
                if (init_step_is_sim_dependent(step)) {
                    s_sim_completion_needed = true;
                    s_sim_steps_skipped = true;
                    if (s_sim_completion_active) {
                        s_sim_completion_failed = true;
                    }
                }
                note_init_step_skipped();
                s_init_index++;
                s_init_retries = 0u;
                s_next_action_ms = now_ms + 50u;
                return;
            }
            s_init_parse_seen = false;
            send_command(MODEM_AT_INIT, step->cmd, step->timeout_ms, now_ms);
        } else {
            modem_begin_provision(now_ms);
        }
    } else if (s_state == MODEM_STATE_PROVISION) {
        provision_advance(now_ms);
    } else if (s_state == MODEM_STATE_READY) {
        maintenance_check_recovery_record();

        /* A CPMS-backed SMS operation can be deliberately paused between the
         * storage SET and its next command while the vendor RI latch is
         * re-armed. Resume/retry that chain before any unrelated poll. */
        if (s_sms_wake_resume != SMS_WAKE_RESUME_NONE) {
            if (sms_wake_retry_due(now_ms)) {
                sms_wake_send_arm(now_ms);
            }
            return;
        }

        /* A call always cancels a diagnostic radio experiment. If the
         * experiment has already changed modem state, finish its verified
         * restoration before dispatching the call command: an antenna test
         * leaves the modem in CFUN=4 with vendor GPIO ownership removed, where
         * answering or dialing cannot work. The call request remains queued
         * while this bounded restore runs. */
        if (!call_session_idle_snapshot() || model_call_request_waiting(NULL)) {
            maintenance_cancel_for_call();
        }
        if (modem_maintenance_has_sequence()) {
            (void)maintenance_start_next_command(now_ms);
            return;
        }
        if (modem_maintenance_blocks_other_work()) {
            /* While the antenna page owns the pins, no unrelated AT work may
             * race it. LOCKED is deliberately fail-closed in CFUN=4; the
             * ordinary power-off/restart path remains available. */
            return;
        }

        if (s_sim_completion_pending && s_operation == MODEM_OP_NONE &&
            call_session_idle_snapshot() &&
            !call_model_wants_clcc(&s_call_model)) {
            /* A #QSS/+CPIN readiness edge after an absent/PIN-locked boot must
             * finish the setup rows that were deliberately skipped. Keep an
             * established call authoritative; the completion pass starts as
             * soon as the call table is quiescent. */
            modem_begin_sim_completion(now_ms);
            return;
        }
        call_txn_kind_t queued_call_kind = CALL_TXN_NONE;
        bool queued_call = model_call_request_waiting(&queued_call_kind);
        bool cleanup_blocks_queued_call =
            queued_call && model_new_call_cleanup_blocks(queued_call_kind) &&
            call_model_new_call_cleanup_pending(&s_call_model, NULL);

        /* Do not let queued SMS/debug work starve call convergence. A user call
         * normally remains first, but an incompatible hold/swap/new-call waits
         * behind model-owned New-call cleanup. Hangup and explicit releases
         * retain their escape-path priority. */
        if (s_operation == MODEM_OP_NONE && cleanup_blocks_queued_call &&
            model_start_pending_release(now_ms)) {
            /* Resolve the abandoned foreground before mutating call roles. */
        } else if (s_operation == MODEM_OP_NONE && queued_call &&
                   !cleanup_blocks_queued_call &&
                   pop_request(&s_current_request)) {
            start_next_request(now_ms);
        } else if (s_operation == MODEM_OP_NONE &&
                   !cleanup_blocks_queued_call &&
                   model_start_pending_release(now_ms)) {
            /* The model emitted a generation-qualified cleanup command. */
        } else if (s_operation == MODEM_OP_NONE &&
                   call_model_wants_clcc(&s_call_model)) {
            /* The id-authoritative model owns both routine keepalive and
             * two-snapshot removal confirmation. */
            send_command(MODEM_AT_CLCC_MODEL, g_modem_vendor.call.clcc_cmd,
                         g_modem_vendor.call.clcc_timeout_ms, now_ms);
        } else if (s_operation == MODEM_OP_NONE &&
                   !call_model_background_work_blocked(&s_call_model) &&
                   s_operator_refresh_needed && signal_query_due(now_ms)) {
            /* The first serving-cell sample resolves both network identity and
             * signal before mailbox/supplementary work. No extra COPS query. */
            (void)signal_start_query(now_ms);
        } else if (s_operation == MODEM_OP_NONE && pop_request(&s_current_request)) {
            start_next_request(now_ms);
        } else if (call_model_background_work_blocked(&s_call_model)) {
            s_next_action_ms = now_ms + 50u;
        } else if (sim_provider_start_query(now_ms)) {
            /* Only unknown PLMNs need a local SIM read. Calls, SMS requests,
             * and other foreground operations retain their normal priority. */
        } else if (diag_start_next_command(now_ms)) {
            /* A diagnostic group yields after this one command. */
        } else if (signal_query_due(now_ms)) {
            (void)signal_start_query(now_ms);
        } else if (s_debug_background_polling_enabled &&
                   time_diff_ms(now_ms, s_next_cereg_ms) >= 0) {
            s_next_cereg_ms = now_ms + MODEM_CEREG_BACKSTOP_MS;
            send_command(MODEM_AT_CEREG, "AT+CEREG?", 2000u, now_ms);
        } else if (s_sms_setup_needed && time_diff_ms(now_ms, s_next_sms_setup_ms) >= 0) {
            /* Deferred SMS setup: CSMP + CPMS failed at cold init (SIM SMS
             * subsystem not ready) and were skipped, leaving storage at the SM
             * default. Retry now that we are READY; CSMP first, then CPMS chains
             * on its OK (see finish_command). Cleared when CPMS sticks. */
            s_next_sms_setup_ms = now_ms + MODEM_SMS_SETUP_RETRY_MS;
            send_command(MODEM_AT_SMS_SETUP_CSMP, "AT+CSMP=17,167,0,0", 5000u, now_ms);
        } else if (sms_wake_retry_due(now_ms)) {
            /* No operation continuation owns this attempt. A failure leaves
             * DTR asserted and schedules another bounded background cycle. */
            sms_wake_send_arm(now_ms);
        } else if (sms_mode_restore_start(now_ms)) {
            /* AT+CMGF=1 after a cancelled operation left PDU mode possible. */
        } else if (direct_ring_start(now_ms)) {
            /* Queued a STORE_DELIVERED operation for a held +CMT. */
        } else if (s_cpms_check_needed ||
                   (s_debug_background_polling_enabled &&
                    time_diff_ms(now_ms, s_next_cpms_ms) >= 0)) {
            s_cpms_check_needed = false;
            s_next_cpms_ms = now_ms + MODEM_CPMS_POLL_PERIOD_MS;
            send_command(MODEM_AT_SMS_CPMS_POLL, "AT+CPMS?", 5000u, now_ms);
        } else if (supplementary_start_background(now_ms)) {
            /* Lowest-priority background work: voicemail number, message
             * waiting, then local call-forwarding flags. */
        }
    }
}

static void send_command(modem_at_kind_t kind, const char *cmd, uint32_t timeout_ms, uint32_t now_ms) {
    (void)send_command_with_token(kind, cmd, timeout_ms, 0u, now_ms);
}

static bool send_model_command(modem_at_kind_t kind, const char *cmd, uint32_t timeout_ms,
                               uint32_t token, uint32_t now_ms) {
    return send_command_with_token(kind, cmd, timeout_ms, token, now_ms);
}

static bool send_command_with_token(modem_at_kind_t kind, const char *cmd,
                                    uint32_t timeout_ms, uint32_t token, uint32_t now_ms) {
    if (g_modem_vendor.wake.strategy == MODEM_WAKE_DTR_CTS &&
        !command_bypasses_cts(kind) &&
        (s_dtr_sleep_permitted || !modem_uart_hal_cts_asserted())) {
        if (s_deferred_valid) {
            LOGE("modem", "DTR wake stash already occupied");
            if (token != 0u) {
                call_model_txn_cancel(&s_call_model, token);
            }
            return false;
        }
        s_deferred_valid = true;
        s_deferred_kind = kind;
        s_deferred_timeout_ms = timeout_ms;
        s_deferred_call_token = token;
        copy_bounded(s_deferred_cmd, sizeof(s_deferred_cmd), cmd);
        if (!s_ri_release_pending) {
            modem_transport_start_dtr_wake(now_ms);
            LOGD("modem", "DTR wake requested before kind=%u", (unsigned)kind);
        } else {
            LOGD("modem", "kind=%u deferred until RI release", (unsigned)kind);
        }
        return true;
    }
    if (!send_raw_command_with_token(kind, cmd, timeout_ms, token, now_ms)) {
        return false;
    }
    if (command_bypasses_cts(kind)) {
        modem_uart_hal_write_cstr_no_cts("\r\n");
    } else {
        modem_uart_hal_write_cstr("\r\n");
    }
    if (token != 0u) {
        call_model_set_now(&s_call_model, now_ms);
        call_model_txn_dispatched(&s_call_model, token);
    }
    return true;
}

static bool start_model_call_request(uint32_t now_ms) {
    call_txn_kind_t txn_kind;
    if (!model_request_kind(s_current_request.type, &txn_kind) ||
        s_current_request.call_token == 0u) {
        return false;
    }

    modem_at_kind_t at_kind = MODEM_AT_CALL_SUPPLEMENTARY;
    switch (txn_kind) {
    case CALL_TXN_DIAL:
        s_operation = MODEM_OP_DIAL;
        at_kind = MODEM_AT_CALL_DIAL;
        break;
    case CALL_TXN_ANSWER:
        s_operation = MODEM_OP_ANSWER;
        at_kind = MODEM_AT_CALL_ANSWER;
        LOGI("modem", "answering incoming call");
        break;
    case CALL_TXN_HANGUP:
        s_operation = MODEM_OP_HANGUP;
        at_kind = MODEM_AT_CALL_HANGUP;
        LOGI("modem", "hanging up call");
        break;
    case CALL_TXN_WAIT_ANSWER:    LOGI("modem", "answering waiting call"); break;
    case CALL_TXN_WAIT_REJECT:    LOGI("modem", "rejecting waiting call"); break;
    case CALL_TXN_SWAP:           LOGI("modem", "swapping calls"); break;
    case CALL_TXN_HOLD:           LOGI("modem", "toggling call hold"); break;
    case CALL_TXN_RELEASE_ACTIVE: LOGI("modem", "releasing active call"); break;
    case CALL_TXN_RELEASE_LEG:
        LOGI("modem", "releasing call leg %u", (unsigned)s_current_request.index);
        break;
    case CALL_TXN_NONE:
        return false;
    }
    if (at_kind == MODEM_AT_CALL_SUPPLEMENTARY) {
        s_operation = MODEM_OP_CALL_SUPPLEMENTARY;
    }
    set_operation_busy(true);

    char cmd[MODEM_PHONE_MAX + 32u];
    uint32_t timeout_ms = 0u;
    if (!model_build_call_request(&s_current_request, txn_kind, cmd, sizeof(cmd),
                                  &timeout_ms)) {
        modem_cmd_result_t result = { .status = CMD_ERROR };
        call_model_txn_command_result(&s_call_model, s_current_request.call_token, result);
        LOGE("modem", "vendor cannot build call operation %u", (unsigned)txn_kind);
        finish_operation(false);
        return true;
    }
    (void)send_model_command(at_kind, cmd, timeout_ms, s_current_request.call_token, now_ms);
    return true;
}

static void start_next_request(uint32_t now_ms) {
    if (start_model_call_request(now_ms)) return;

    switch (s_current_request.type) {
    case MODEM_REQ_DTMF: {
        s_operation = MODEM_OP_DTMF;
        s_dtmf_cancel_requested = false;
        set_operation_busy(true);
        if (!dtmf_call_active() || call_control_pending()) {
            /* The call changed while this atomically-admitted request waited
             * in the normal FIFO. Nothing crossed UART, so this is a clean
             * cancellation rather than a command error. */
            finish_operation(true);
        } else if (!dtmf_send_current(now_ms)) {
            finish_operation(false);
        }
        break;
    }
    case MODEM_REQ_CALL_FORWARD: {
        char command[MODEM_PROVISION_COMMAND_MAX];
        uint8_t step_count = 0u;
        if (!call_forward_request_valid(&s_current_request.call_forward,
                                        &step_count) ||
            !g_modem_vendor.supplementary.build_call_forward_step(
                &s_current_request.call_forward, 0u, command,
                sizeof(command))) {
            call_forward_push_result(
                s_current_request.request_id,
                &s_current_request.call_forward,
                CALL_FORWARD_OUTCOME_NOT_DONE,
                false, false, NULL, false, 0u);
            finish_operation(false);
            break;
        }
        s_operation = MODEM_OP_CALL_FORWARD;
        set_operation_busy(true);
        modem_supplementary_call_forward_begin(step_count);
        send_command(MODEM_AT_CALL_FORWARD, command,
                     g_modem_vendor.supplementary.command_timeout_ms,
                     now_ms);
        break;
    }
    case MODEM_REQ_VOICE_MAILBOX:
        if (g_modem_vendor.supplementary.voice_mailbox_query_cmd == NULL ||
            g_modem_vendor.supplementary.parse_voice_mailbox_row == NULL ||
            g_modem_vendor.supplementary.voice_mailbox_timeout_ms == 0u) {
            finish_operation(false);
            break;
        }
        s_operation = MODEM_OP_VOICE_MAILBOX;
        set_operation_busy(true);
        modem_supplementary_voice_mailbox_begin();
        send_command(MODEM_AT_VOICE_MAILBOX,
                     g_modem_vendor.supplementary.voice_mailbox_query_cmd,
                     g_modem_vendor.supplementary.voice_mailbox_timeout_ms,
                     now_ms);
        break;
    case MODEM_REQ_MESSAGE_WAITING:
        if (g_modem_vendor.supplementary.message_waiting_query_cmd == NULL ||
            g_modem_vendor.supplementary.parse_message_waiting_row == NULL ||
            g_modem_vendor.supplementary.message_waiting_timeout_ms == 0u) {
            finish_operation(false);
            break;
        }
        s_operation = MODEM_OP_MESSAGE_WAITING;
        set_operation_busy(true);
        modem_supplementary_message_waiting_begin();
        send_command(
            MODEM_AT_MESSAGE_WAITING,
            g_modem_vendor.supplementary.message_waiting_query_cmd,
            g_modem_vendor.supplementary.message_waiting_timeout_ms, now_ms);
        break;
    case MODEM_REQ_SEND_SMS:
    case MODEM_REQ_SEND_BINARY_SMS:
    case MODEM_REQ_SAVE_SMS:
    case MODEM_REQ_SMS_MAILBOX:
    case MODEM_REQ_SMS_READ:
    case MODEM_REQ_DELETE_SMS:
    case MODEM_REQ_STORE_DELIVERED_SMS:
        (void)sms_start_current_request(now_ms);
        break;
    case MODEM_REQ_PHONEBOOK_LIST:
    case MODEM_REQ_PHONEBOOK_ADD:
    case MODEM_REQ_PHONEBOOK_UPDATE:
    case MODEM_REQ_PHONEBOOK_DELETE:
        (void)phonebook_start_current_request(now_ms);
        break;
    case MODEM_REQ_DEBUG_AT:
        s_operation = MODEM_OP_DEBUG_AT;
        set_operation_busy(true);
        LOGI("modem", "debug AT request: %s", s_current_request.text);
        send_command(MODEM_AT_DEBUG, s_current_request.text, 15000u, now_ms);
        break;
    case MODEM_REQ_DEBUG_BACKGROUND_POLLING:
        s_debug_background_polling_enabled = s_current_request.index != 0u;
        LOGI("modem", "periodic background polling %s",
             s_debug_background_polling_enabled ? "enabled" : "disabled");
        break;
    default:
        finish_operation(false);
        break;
    }
}

static void supplementary_project_status_locked(void) {
    modem_supplementary_cache_t cache;
    modem_supplementary_get_cache(&cache);
    s_status.call_forward_unconditional_known =
        cache.call_forward_unconditional_known;
    s_status.call_forward_unconditional_active =
        cache.call_forward_unconditional_active;
    s_status.message_waiting = cache.message_waiting;
}

static bool supplementary_start_background(uint32_t now_ms) {
    if (s_operation != MODEM_OP_NONE ||
        !g_modem_vendor.supplementary.supported ||
        (!modem_supplementary_refresh_due(
             MODEM_SUPPLEMENTARY_REFRESH_VOICE_MAILBOX, now_ms) &&
         !modem_supplementary_refresh_due(
             MODEM_SUPPLEMENTARY_REFRESH_MESSAGE_WAITING, now_ms) &&
         !modem_supplementary_refresh_due(
             MODEM_SUPPLEMENTARY_REFRESH_CFU, now_ms))) {
        return false;
    }
    bool sim_ready;
    bool registered;
    critical_section_enter_blocking(&s_status_lock);
    sim_ready = s_status.sim_ready;
    registered = s_status.network_registered;
    critical_section_exit(&s_status_lock);

    if (modem_supplementary_refresh_due(
            MODEM_SUPPLEMENTARY_REFRESH_VOICE_MAILBOX, now_ms) &&
        sim_ready &&
        g_modem_vendor.supplementary.voice_mailbox_query_cmd != NULL) {
        modem_request_t request;
        memset(&request, 0, sizeof(request));
        modem_supplementary_refresh_begin(
            MODEM_SUPPLEMENTARY_REFRESH_VOICE_MAILBOX);
        request.type = MODEM_REQ_VOICE_MAILBOX;
        s_current_request = request;
        start_next_request(now_ms);
        return true;
    }
    if (modem_supplementary_refresh_due(
            MODEM_SUPPLEMENTARY_REFRESH_MESSAGE_WAITING, now_ms) &&
        sim_ready &&
        g_modem_vendor.supplementary.message_waiting_query_cmd != NULL) {
        modem_request_t request;
        memset(&request, 0, sizeof(request));
        modem_supplementary_refresh_begin(
            MODEM_SUPPLEMENTARY_REFRESH_MESSAGE_WAITING);
        request.type = MODEM_REQ_MESSAGE_WAITING;
        s_current_request = request;
        start_next_request(now_ms);
        return true;
    }
    if (modem_supplementary_refresh_due(
            MODEM_SUPPLEMENTARY_REFRESH_CFU, now_ms) && sim_ready &&
        registered &&
        g_modem_vendor.supplementary.call_forward_flags_query_cmd != NULL) {
        modem_supplementary_refresh_begin(MODEM_SUPPLEMENTARY_REFRESH_CFU);
        s_cfu_flags_row_seen = false;
        send_command(MODEM_AT_CALL_FORWARD_FLAGS,
                     g_modem_vendor.supplementary.call_forward_flags_query_cmd,
                     g_modem_vendor.supplementary.call_forward_flags_timeout_ms,
                     now_ms);
        return true;
    }
    return false;
}

static void send_deferred(uint32_t now_ms) {
    if (!s_deferred_valid) {
        return;
    }
    s_deferred_valid = false;
    uint32_t token = s_deferred_call_token;
    s_deferred_call_token = 0u;
    /* Re-enter the common command path after CTS confirms the DTR wake. */
    (void)send_command_with_token(s_deferred_kind, s_deferred_cmd,
                                  s_deferred_timeout_ms, token, now_ms);
}

static void modem_fail_before_dispatch(uint32_t now_ms) {
    if (!s_deferred_valid) {
        modem_fail_power_state("CTS wake timeout", true);
        return;
    }

    modem_at_kind_t kind = s_deferred_kind;
    uint32_t token = s_deferred_call_token;
    s_deferred_valid = false;
    s_deferred_call_token = 0u;
    if (token != 0u) {
        /* The command never crossed the UART boundary. Cancellation, not a
         * command timeout, is the transaction evidence required by the call
         * model's queue-ownership contract. */
        call_model_txn_cancel(&s_call_model, token);
    }
    if (kind == MODEM_AT_PROVISION_REBOOT) {
        modem_fail_power_state(
            "CTS wake timeout before provisioning reboot dispatch", true);
        return;
    }
    if (kind == MODEM_AT_CALL_FORWARD) {
        /* No bytes crossed the UART. Preserve the stronger evidence instead
         * of feeding this through the ordinary on-wire timeout path. If this
         * was a deferred second step, the first step's mutation still makes
         * the overall result unknown. */
        call_forward_finish(false, false);
        if (s_state != MODEM_STATE_OFF &&
            s_state != MODEM_STATE_OFF_DISCHARGE) {
            modem_fail_power_state(
                "CTS wake timeout before command dispatch", true);
        }
        return;
    }

    modem_sms_command_kind_t sms_kind;
    if (sms_protocol_command_from_at(kind, &sms_kind)) {
        modem_sms_protocol_request_t request;
        modem_sms_outcome_t outcome =
            sms_protocol_request_view(&request)
                ? modem_sms_protocol_cancel_outcome(&request)
                : MODEM_SMS_OUTCOME_CANCELLED;
        (void)sms_publish_terminal_for_request(&s_current_request, outcome);
        finish_operation(false);
        if (s_state != MODEM_STATE_OFF &&
            s_state != MODEM_STATE_OFF_DISCHARGE) {
            modem_fail_power_state(
                "CTS wake timeout before command dispatch", true);
        }
        return;
    }

    /* Reuse the mature per-kind timeout cleanup after stripping the
     * never-dispatched call token. This completes debug/UI operations and takes
     * the conservative shutdown path without falsely reporting an AT outcome
     * to the call model. SMS requests use their evidence-aware cancellation
     * path above so a command that never crossed UART is retry-safe. */
    s_active = true;
    s_active_kind = kind;
    s_active_call_token = 0u;
    s_command_deadline_ms = now_ms;
    process_timeout(now_ms);
    if (kind != MODEM_AT_POWER_OFF && s_state != MODEM_STATE_OFF &&
        s_state != MODEM_STATE_OFF_DISCHARGE) {
        modem_fail_power_state("CTS wake timeout before command dispatch", true);
    }
}

static void modem_transport_start_dtr_wake(uint32_t now_ms) {
    if (s_dtr_wake_pending) {
        return;
    }
    modem_uart_hal_set_dtr_sleep_permitted(false);
    s_dtr_sleep_permitted = false;
    s_dtr_wake_pending = true;
    s_diag_transport_wake_attempts++;
    s_diag_transport_wake_started_ms = now_ms;
    s_dtr_wake_deadline_ms = now_ms +
        g_modem_vendor.wake.dtr_wake_timeout_ms;
}

static void modem_transport_tick(uint32_t now_ms) {
    /* Consume the edge even while awake so it cannot become stale. The level
     * closes the complementary race where RI is already low as sleep is armed. */
    bool ri_wake = modem_uart_hal_take_ri_wake();
    if (g_modem_vendor.wake.strategy != MODEM_WAKE_DTR_CTS ||
        s_uart_parked || s_state == MODEM_STATE_OFF) {
        return;
    }

    bool ri_asserted = modem_uart_hal_ri_asserted();
    /* Boot/reset can toggle RI and CTS before the module accepts commands.
     * Only READY may initiate an unsolicited sleep wake; an explicit command's
     * pending DTR wake still runs during init/provisioning below. */
    bool ri_evidence = s_state == MODEM_STATE_READY &&
        (ri_wake || ri_asserted);
    bool ri_ready_to_wake = false;

    if (ri_evidence) {
        s_last_tx_ms = now_ms;
    }

    if (s_ri_release_pending) {
        if (!ri_asserted) {
            s_ri_release_pending = false;
            s_ri_release_deadline_ms = 0u;
            ri_ready_to_wake = true;
            LOGD("modem", "RI released; requesting DTR wake");
        } else if (time_diff_ms(now_ms, s_ri_release_deadline_ms) >= 0) {
            s_ri_release_pending = false;
            s_ri_release_deadline_ms = 0u;
            s_diag_transport_ri_release_timeouts++;
            ri_ready_to_wake = true;
            LOGI("modem", "RI held through release window; requesting DTR wake");
        }
    } else if (ri_evidence && !s_dtr_wake_pending &&
               (s_dtr_sleep_permitted ||
                !modem_uart_hal_cts_asserted())) {
        if (ri_asserted && s_dtr_sleep_permitted &&
            g_modem_vendor.wake.ri_release_timeout_ms != 0u) {
            /* Board-1 LE910C1-WWX suppresses the buffered +CMTI when DTR is
             * asserted during #PSMRI's low pulse. Wait for the pulse to finish;
             * a completed pulse (edge latched, line high) skips this state. */
            s_ri_release_pending = true;
            s_ri_release_deadline_ms = now_ms +
                g_modem_vendor.wake.ri_release_timeout_ms;
            LOGD("modem", "RI wake latched; waiting for release");
        } else {
            ri_ready_to_wake = true;
        }
    }

    if (ri_ready_to_wake && !s_dtr_wake_pending &&
        (s_dtr_sleep_permitted || !modem_uart_hal_cts_asserted())) {
        modem_transport_start_dtr_wake(now_ms);
    }
    if (!s_dtr_wake_pending) {
        return;
    }
    if (modem_uart_hal_cts_asserted()) {
        s_dtr_wake_pending = false;
        uint32_t latency = now_ms - s_diag_transport_wake_started_ms;
        s_diag_transport_last_wake_latency_ms = latency;
        if (latency > s_diag_transport_max_wake_latency_ms) {
            s_diag_transport_max_wake_latency_ms = latency;
        }
        s_last_tx_ms = now_ms;
        send_deferred(now_ms);
        return;
    }
    if (time_diff_ms(now_ms, s_dtr_wake_deadline_ms) >= 0) {
        s_dtr_wake_pending = false;
        s_diag_transport_wake_timeouts++;
        LOGE("modem", "DTR wake timed out waiting for CTS");
        modem_fail_before_dispatch(now_ms);
    }
}

static void modem_transport_idle(uint32_t now_ms) {
    if (g_modem_vendor.wake.strategy != MODEM_WAKE_DTR_CTS ||
        s_state != MODEM_STATE_READY || s_uart_parked ||
        s_dtr_sleep_permitted || s_dtr_wake_pending ||
        s_ri_release_pending || s_deferred_valid ||
        s_active || s_operation != MODEM_OP_NONE || s_binary_restore_pending ||
        (sms_wake_required() && status_sim_ready_snapshot() &&
         !s_sms_wake_armed) ||
        time_diff_ms(now_ms,
                     s_last_tx_ms + g_modem_vendor.wake.awake_window_ms) < 0) {
        return;
    }

    if (s_sms_wake_activation_deadline_ms != 0u) {
        if (time_diff_ms(now_ms, s_sms_wake_activation_deadline_ms) < 0) {
            return;
        }
        s_sms_wake_activation_deadline_ms = 0u;
        LOGW("modem", "SIM activation window expired before SMS wake setup");
    }

    if (!call_session_idle_snapshot() ||
        call_model_wants_clcc(&s_call_model)) {
        return;
    }

    critical_section_enter_blocking(&s_request_lock);
    bool queue_empty = s_request_count == 0u;
    critical_section_exit(&s_request_lock);
    if (!queue_empty) {
        return;
    }

    modem_uart_hal_set_dtr_sleep_permitted(true);
    s_dtr_sleep_permitted = true;
    s_diag_transport_sleep_entries++;
    LOGD("modem", "DTR released for modem sleep");
}

static void modem_runtime_power_tick(uint32_t now_ms) {
    if (s_state == MODEM_STATE_FAILED &&
        s_failed_shutdown_qualification_pending) {
        if (time_diff_ms(now_ms, s_next_power_observation_ms) < 0) {
            return;
        }
        s_next_power_observation_ms = now_ms + MODEM_POWER_OBSERVATION_MS;
        modem_power_observation_t observation = modem_power_observation();
        if (g_modem_vendor.off_complete != NULL &&
            g_modem_vendor.off_complete(now_ms, &observation)) {
            LOGI("modem", "late switch-off completion observed after timeout");
            modem_enter_off();
        }
        return;
    }
    if (g_modem_vendor.power.rail_power_good_timeout_ms == 0u ||
        (s_state != MODEM_STATE_PROBE && s_state != MODEM_STATE_INIT &&
         s_state != MODEM_STATE_PROVISION && s_state != MODEM_STATE_READY) ||
        time_diff_ms(now_ms, s_next_power_observation_ms) < 0) {
        return;
    }
    s_next_power_observation_ms = now_ms + MODEM_POWER_OBSERVATION_MS;
    modem_power_observation_t observation = modem_power_observation();
    bool module_on = g_modem_vendor.power_is_on == NULL ||
        g_modem_vendor.power_is_on(&observation);
    bool module_drop_expected = s_active &&
        (s_active_kind == MODEM_AT_POWER_OFF ||
         s_active_kind == MODEM_AT_PROVISION_REBOOT);
    if (s_provision_reboot_cycle_active && module_drop_expected &&
        observation.module_status_valid && !module_on) {
        /* A lost reboot final can leave us in PROVISION until its command
         * timeout. Preserve the reset edge now so the later waiter may accept
         * an already-recovered module, and park PL011 while its I/O is down. */
        provision_note_reboot_drop(now_ms);
    }
    if (!observation.supply_power_good) {
        /* This state was reached only after a proven AT channel. PG-low can be
         * a regulator/sense transient; do not turn it into an immediate cut of
         * a module that has not completed the vendor OFF qualification. */
        modem_fail_supply_power_state("modem supply PG dropped", true);
    } else if (g_modem_vendor.power_is_on != NULL &&
               observation.module_status_valid && !module_on &&
               !module_drop_expected) {
        if (!s_startup_complete && !s_startup_restart_recovered &&
            !s_provision_reboot_cycle_active) {
            modem_recover_startup_restart(now_ms);
            return;
        }
        /* One low sample is fault evidence, not safe rail-cut evidence. The
         * module may be rebooting; retain VCC until an explicit shutdown path
         * obtains the vendor's sustained-low qualification. */
        modem_fail_power_state("module status dropped while service was live",
                               true);
    }
}

static void send_raw_command(modem_at_kind_t kind, const char *cmd, uint32_t timeout_ms, uint32_t now_ms) {
    (void)send_raw_command_with_token(kind, cmd, timeout_ms, 0u, now_ms);
}

static bool send_raw_command_with_token(modem_at_kind_t kind, const char *cmd,
                                        uint32_t timeout_ms, uint32_t token, uint32_t now_ms) {
    if (token != 0u) {
        call_model_set_now(&s_call_model, now_ms);
        call_dispatch_guard_t guard = call_model_txn_dispatch_guard(&s_call_model, token);
        if (guard != CALL_DISPATCH_SEND) {
            LOGI("modem", "suppressed stale call command token=%lu verdict=%u",
                 (unsigned long)token, (unsigned)guard);
            finish_operation(true);
            return false;
        }
    }

    if (kind == MODEM_AT_CLCC_MODEL) {
        call_model_clcc_begin(&s_call_model, model_transport_counter());
    }

    sms_wake_command_dispatched(cmd, now_ms);

    modem_sms_command_kind_t sms_kind;
    if (sms_protocol_command_from_at(kind, &sms_kind)) {
        /* This is the common immediate/deferred UART boundary. Protocol emit
         * alone is not evidence: DTR/CTS may have stashed the command first. */
        modem_sms_protocol_command_dispatched(sms_kind);
    }

    s_active_kind = kind;
    s_active = true;
    s_diag_active_started_ms = now_ms;
    s_active_call_token = token;
    s_command_deadline_ms = now_ms + timeout_ms;
    s_last_tx_ms = now_ms;
    copy_bounded(s_debug_last_command, sizeof(s_debug_last_command), cmd);
    if (command_bypasses_cts(kind)) {
        /* A persisted sleep/flow setting may leave the module unable to accept
         * a CTS-gated probe or the commands that establish command-mode flow
         * control. The backend explicitly opts in for only those commands. */
        modem_uart_hal_write_cstr_no_cts(cmd);
    } else {
        modem_uart_hal_write_cstr(cmd);
    }
    LOGD("modem", "> %s", cmd);
    return true;
}

static bool command_bypasses_cts(modem_at_kind_t kind) {
    if (kind == MODEM_AT_PING) {
        return g_modem_vendor.wake.probe_bypass_cts;
    }
    return kind == MODEM_AT_INIT &&
           s_init_index < g_modem_vendor.init_step_count &&
           g_modem_vendor.init_steps[s_init_index].bypass_cts;
}

static modem_cmd_result_t model_command_result(bool ok, const char *line) {
    modem_cmd_result_t result;
    memset(&result, 0, sizeof(result));
    result.status = ok ? CMD_OK : CMD_ERROR;
    if (line == NULL) return result;
    if (strcmp(line, "BUSY") == 0) {
        result.has_call_result = true;
        result.call_result = MODEM_CALL_RESULT_BUSY;
    } else if (strcmp(line, "NO ANSWER") == 0) {
        result.has_call_result = true;
        result.call_result = MODEM_CALL_RESULT_NO_ANSWER;
    } else if (strcmp(line, "NO DIALTONE") == 0 || strcmp(line, "NO DIAL TONE") == 0) {
        result.has_call_result = true;
        result.call_result = MODEM_CALL_RESULT_NO_DIALTONE;
    } else if (strcmp(line, "NO CARRIER") == 0) {
        result.has_call_result = true;
        result.call_result = MODEM_CALL_RESULT_NO_CARRIER;
    }
    return result;
}

static void finish_command_result(bool ok, const char *line) {
    modem_at_kind_t kind = s_active_kind;
    uint32_t call_token = s_active_call_token;
    bool sms_wake_arm_command = s_active_sms_wake_arm;
    modem_cmd_result_t command_result = model_command_result(ok, line);
    s_active = false;
    s_active_kind = MODEM_AT_NONE;
    s_active_call_token = 0u;
    s_active_sms_wake_arm = false;
    if (sms_wake_arm_command) {
        s_sms_wake_armed = ok;
    }

    if (line != NULL && strcmp(line, "CONNECT") == 0 && is_call_at_kind(kind)) {
        /* CONNECT is both the command final and positive call evidence. Token
         * acceptance alone cannot publish that evidence because CMD_OK only
         * means the command was accepted; keep the id-blind connect latch while
         * CLCC/URC remains responsible for confirming a leg id. */
        call_model_on_bare_final(&s_call_model, MODEM_CALL_RESULT_CONNECTED, false);
    }
    if (kind == MODEM_AT_CALL_SUPPLEMENTARY && command_result.has_call_result) {
        /* A V.250 progress final inside a supplementary-service command is
         * id-blind: it may describe the waiting/held leg that CHLD affected.
         * Keep the command's correlated outcome, but also force authoritative
         * CLCC recovery without attributing the cause to the primary leg. */
        call_model_on_bare_final(&s_call_model, command_result.call_result, true);
    }
    if (call_token != 0u) {
        call_model_txn_command_result(&s_call_model, call_token, command_result);
    }

    if (kind == MODEM_AT_CLCC_MODEL) {
        if (ok) call_model_clcc_ok(&s_call_model, model_transport_counter());
        else call_model_clcc_error(&s_call_model);
        return;
    }

    if (!ok && !at_kind_is_operation(kind) &&
        kind != MODEM_AT_DIAG_QUERY) {
        critical_section_enter_blocking(&s_status_lock);
        s_status.command_errors++;
        critical_section_exit(&s_status_lock);
    }

    if (kind == MODEM_AT_DIAG_QUERY) {
        diag_finish_query(ok, false, s_now_ms);
        return;
    }

    if (kind == MODEM_AT_SIGNAL) {
        signal_finish_query(ok, s_now_ms);
        return;
    }

    if (kind == MODEM_AT_SIM_PROVIDER || kind == MODEM_AT_SIM_PROVIDER_DRAIN) {
        sim_provider_finish_query(ok && kind == MODEM_AT_SIM_PROVIDER);
        if (s_power_off_pending) {
            s_power_off_pending = false;
            modem_begin_power_off(s_now_ms);
        }
        return;
    }

    if (kind == MODEM_AT_MAINTENANCE) {
        maintenance_finish_command(ok, false, s_now_ms);
        return;
    }

    if (kind == MODEM_AT_SMS_WAKE_ARM) {
        sms_wake_handle_final(ok, s_now_ms);
        return;
    }

    modem_sms_command_kind_t sms_kind;
    if (sms_protocol_command_from_at(kind, &sms_kind)) {
        modem_sms_protocol_request_t request;
        if (sms_protocol_request_view(&request)) {
            modem_sms_protocol_note_final_line(line);
            modem_sms_protocol_on_final(
                sms_kind, ok, &request, &s_sms_protocol_hooks, s_now_ms);
        } else {
            finish_operation(false);
        }
        return;
    }

    modem_phonebook_command_kind_t phonebook_kind;
    if (phonebook_protocol_command_from_at(kind, &phonebook_kind)) {
        modem_phonebook_protocol_request_t request;
        if (phonebook_protocol_request_view(&request)) {
            modem_phonebook_protocol_on_final(
                phonebook_kind, ok, &request, &s_phonebook_protocol_hooks,
                s_now_ms);
        } else {
            finish_operation(false);
        }
        return;
    }

    if (kind == MODEM_AT_PING) {
        if (ok) {
            /* Startup receives the tiny probe final by FIFO polling. Only a
             * successful parsed exchange proves the UART is stable enough to
             * hand RX to the interrupt-driven ring. */
            modem_uart_hal_enable_rx_irq();
            if (s_power_off_pending) {
                /* A shutdown requested while ON_OFF/PWRMON was still in flight
                 * waits for this first proven command exchange. Shut down now;
                 * do not enter ordinary initialization first. */
                s_power_off_pending = false;
                send_command(MODEM_AT_POWER_OFF,
                             g_modem_vendor.power.off_cmd,
                             g_modem_vendor.power.off_ack_timeout_ms,
                             s_now_ms);
                LOGI("modem", "AT ready; deferred startup shutdown sent");
            } else {
                s_state = MODEM_STATE_INIT;
                s_init_index = 0;
                s_init_retries = 0;
                s_sim_completion_needed = false;
                s_sim_completion_pending = false;
                s_sim_completion_active = false;
                s_sim_completion_failed = false;
                s_sim_steps_skipped = false;
                s_status.audio_init_ok = true; /* fresh init pass: clear prior degraded flags */
                s_status.sms_init_ok = true;
                /* Normally no deferred command can coexist with a probe PING. If a
                 * prior failed-link episode left one, cancel it rather than silently
                 * dropping its never-dispatched model token on re-init. */
                model_cancel_deferred_command();
                s_next_action_ms = s_now_ms + MODEM_POST_READY_SETTLE_MS;
                LOGI("modem", "AT ready");
            }
        } else {
            handle_ping_failed(s_now_ms);
        }
    } else if (kind == MODEM_AT_INIT) {
        bool init_ok = ok;
        if (init_ok && s_init_index < g_modem_vendor.init_step_count &&
            init_step_requires_response(
                &g_modem_vendor.init_steps[s_init_index]) &&
            !s_init_parse_seen) {
            init_ok = false;
            LOGW("modem", "init response missing/invalid index=%u cmd=%s",
                 (unsigned)s_init_index,
                 g_modem_vendor.init_steps[s_init_index].cmd);
        }
        if (init_ok) {
            s_init_index++;
            s_init_retries = 0;
        } else if (g_modem_vendor.init_steps[s_init_index].degrade ==
                       MODEM_DEGRADE_SIM_GATE &&
                   status_sim_missing_snapshot()) {
            /* A prior trustworthy vendor SIM-presence observation makes CPIN's
             * expected CME final conclusive. Missing SIM is a
             * usable phone state, not a long retry ladder or fatal init. */
            LOGI("modem", "SIM absent; skipping SIM gate without retry");
            s_sim_completion_needed = true;
            s_sim_steps_skipped = true;
            if (s_sim_completion_active) {
                s_sim_completion_failed = true;
            }
            s_init_index++;
            s_init_retries = 0u;
        } else {
            if (++s_init_retries > init_retry_limit()) {
                if (init_step_recoverable()) {
                    LOGW("modem", "skipping failed init cmd index=%u cmd=%s",
                         (unsigned)s_init_index,
                         g_modem_vendor.init_steps[s_init_index].cmd);
                    note_init_step_skipped();
                    s_init_index++;
                    s_init_retries = 0;
                } else {
                    LOGE("modem", "init rejected index=%u cmd=%s line=%s",
                         (unsigned)s_init_index,
                         g_modem_vendor.init_steps[s_init_index].cmd,
                         s_debug_last_line);
                    modem_fail_power_state("required modem init command rejected", true);
                }
            }
        }
        if (s_state != MODEM_STATE_FAILED) {
            s_next_action_ms = s_now_ms + 50u;
        }
    } else if (kind == MODEM_AT_PROVISION_QUERY ||
               kind == MODEM_AT_PROVISION_SET) {
        provision_handle_final(ok, false, s_now_ms);
    } else if (kind == MODEM_AT_PROVISION_REBOOT) {
        if (ok) {
            provision_begin_reboot_wait(s_now_ms);
        } else {
            modem_fail_power_state("provisioning reboot command rejected", true);
        }
    } else if (kind == MODEM_AT_CALL_FORWARD) {
        call_forward_finish_step(ok, false);
    } else if (kind == MODEM_AT_CALL_FORWARD_FLAGS) {
        critical_section_enter_blocking(&s_status_lock);
        if (modem_supplementary_call_forward_flags_finish(
                ok && s_cfu_flags_row_seen, s_now_ms)) {
            supplementary_project_status_locked();
            s_status.last_update_ms = s_now_ms;
        }
        critical_section_exit(&s_status_lock);
    } else if (kind == MODEM_AT_VOICE_MAILBOX) {
        voice_mailbox_finish(ok);
    } else if (kind == MODEM_AT_MESSAGE_WAITING) {
        message_waiting_finish(ok);
    } else if (kind == MODEM_AT_CALL_DIAL) {
        finish_operation(ok);
    } else if (kind == MODEM_AT_CALL_ANSWER) {
        finish_operation(ok);
    } else if (kind == MODEM_AT_CALL_HANGUP) {
        finish_operation(ok);
    } else if (kind == MODEM_AT_CALL_DTMF) {
        dtmf_finish_digit(ok, s_now_ms);
    } else if (kind == MODEM_AT_CALL_SUPPLEMENTARY) {
        finish_operation(ok);
    } else if (kind == MODEM_AT_POWER_OFF) {
        /* OK means the switch-off routine STARTED (SIM 1.6.2 figure 13); the
         * rail must stay up until it completes -- sense the UART break. */
        modem_begin_off_discharge(s_now_ms);
    } else if (kind == MODEM_AT_SMS_SETUP_CSMP) {
        if (ok) {
            /* CSMP stuck; now set storage. The retry timer re-runs from CSMP if
             * this CPMS fails, so a transient failure here just retries later. */
            send_command(MODEM_AT_SMS_SETUP_CPMS,
                         "AT+CPMS=\"" MODEM_SMS_STORAGE "\",\"" MODEM_SMS_STORAGE "\",\"" MODEM_SMS_STORAGE "\"",
                         5000u, s_now_ms);
        }
    } else if (kind == MODEM_AT_SMS_SETUP_CPMS) {
        if (ok) {
            sms_request_wake_qualified_continuation(
                SMS_WAKE_RESUME_DEFERRED_SETUP, s_now_ms);
        }
    } else if (kind == MODEM_AT_SMS_MODE_RESTORE) {
        if (ok) {
            s_sms_mode_restore_pending = false;
            s_sms_mode_restore_attempts = 0u;
            LOGI("modem", "SMS text mode restored after a cancelled operation");
        } else {
            s_sms_mode_restore_attempts++; /* retried from the next idle tick */
        }
    } else if (kind == MODEM_AT_DEBUG) {
        LOGI("modem", "debug AT final ok=%u", ok ? 1u : 0u);
        push_debug_result(ok);
        finish_operation(ok);
    }
}

static void modem_enter_ready(uint32_t now_ms) {
    s_startup_complete = true;
    bool completed_sim_pass = s_sim_completion_active;
    bool sim_ready = status_sim_ready_snapshot();
    if (s_sim_completion_active) {
        s_sim_completion_needed = !sim_ready || s_sim_completion_failed;
        s_sim_completion_pending = false;
        s_sim_completion_active = false;
        s_sim_completion_failed = false;
    } else if (sim_ready) {
        /* A readiness edge can arrive midway through the ordinary init pass.
         * If any earlier SIM row was skipped, immediately schedule the focused
         * completion traversal; otherwise this full pass already covered it. */
        s_sim_completion_needed = s_sim_steps_skipped;
        s_sim_completion_pending = s_sim_steps_skipped;
    } else {
        s_sim_completion_needed = true;
        s_sim_completion_pending = false;
    }
    if (g_modem_vendor.sms_wake.qualified_by_sim_completion) {
        if (!completed_sim_pass && !sim_ready && s_sim_completion_needed &&
            g_modem_vendor.sms_wake.sim_activation_window_ms != 0u) {
            /* CFUN=5 has just activated the SIM after the RF-safe hardware
             * pass. Keep UART/DTR awake for its readiness edge; a missing or
             * locked SIM is released after this one bounded boot window. */
            s_sms_wake_activation_deadline_ms = now_ms +
                g_modem_vendor.sms_wake.sim_activation_window_ms;
        } else if (completed_sim_pass && sim_ready &&
                   !s_sms_wake_armed) {
            LOGE("modem", "SIM completion ended without SMS wake qualification");
        }
    }
    s_sim_steps_skipped = false;
    s_state = MODEM_STATE_READY;
    s_diag_ready_entries++;
    s_diag_last_transition_ms = now_ms;
    critical_section_enter_blocking(&s_status_lock);
    s_status.at_ready = true;
    s_status.provisioning_schema_version =
        g_modem_vendor.provision_schema_version;
    s_status.provisioning_verified =
        g_modem_vendor.provision_schema_version != 0u &&
        s_provision_all_verified;
    s_status.last_update_ms = now_ms;
    critical_section_exit(&s_status_lock);
    /* DTR stays asserted through this transition and is released by the idle
     * gate after a genuine quiescent window. */
    s_last_tx_ms = now_ms;
    s_next_signal_ms = now_ms;
    s_next_cereg_ms = now_ms + 300u;
    s_operator_refresh_needed = true;
    s_next_cpms_ms = now_ms + 900u;
    s_next_sms_setup_ms = now_ms + 1200u;
    s_sms_storage_full_latched = false;
    LOGI("modem", "AT init and provisioning complete");
}

static bool provision_prerequisites_met(
    const modem_provision_step_t *step) {
    return step != NULL &&
        (((step->prerequisites & MODEM_INIT_PREREQ_SIM_READY) == 0u) ||
         status_sim_ready_snapshot()) &&
        (((step->prerequisites & MODEM_INIT_PREREQ_SIM_COMPLETION) == 0u) ||
         s_sim_completion_active);
}

static bool provision_step_is_sim_dependent(
    const modem_provision_step_t *step) {
    return step != NULL &&
        ((step->prerequisites &
          (MODEM_INIT_PREREQ_SIM_READY |
           MODEM_INIT_PREREQ_SIM_COMPLETION)) != 0u);
}

static bool provision_has_writer(const modem_provision_step_t *step) {
    return step != NULL &&
           (step->set_cmd != NULL || step->build_set_cmd != NULL);
}

static void provision_note_degrade(modem_degrade_t degrade) {
    critical_section_enter_blocking(&s_status_lock);
    switch (degrade) {
    case MODEM_DEGRADE_AUDIO:
        s_status.audio_init_ok = false;
        break;
    case MODEM_DEGRADE_SMS_SETUP:
        s_status.sms_init_ok = false;
        s_sms_setup_needed = true;
        break;
    case MODEM_DEGRADE_NONE:
    case MODEM_DEGRADE_SIM_GATE:
    default:
        break;
    }
    critical_section_exit(&s_status_lock);
}

static void provision_reset_step_state(void) {
    s_provision_retries = 0u;
    s_provision_phase = MODEM_PROVISION_QUERY;
    s_provision_line_seen = false;
    s_provision_line_result = MODEM_PROVISION_LINE_IGNORE;
    s_provision_step_written = false;
}

static bool provision_qualify_sms_wake(
    const modem_provision_step_t *step) {
    if (step == NULL || !step->qualifies_sms_wake) {
        return true;
    }
    if (!g_modem_vendor.sms_wake.qualified_by_sim_completion ||
        !s_sim_completion_active || !status_sim_ready_snapshot()) {
        modem_fail_power_state(
            "SMS wake qualified outside SIM completion", true);
        return false;
    }
    s_sms_wake_armed = true;
    s_sms_wake_activation_deadline_ms = 0u;
    LOGI("modem", "SMS RI wake qualified before DTR sleep");
    return true;
}

static void provision_complete_step(uint32_t now_ms) {
    const modem_provision_step_t *step =
        &g_modem_vendor.provision_steps[s_provision_index];
    if (s_provision_step_written &&
        step->persistence == MODEM_SETTING_NVM_REBOOT) {
        s_provision_reboot_required = true;
    }
    s_provision_index++;
    provision_reset_step_state();
    s_next_action_ms = now_ms + 50u;
}

static void provision_retry_or_fail(const char *reason,
                                    modem_provision_phase_t retry_phase,
                                    uint32_t now_ms) {
    const modem_provision_step_t *step =
        &g_modem_vendor.provision_steps[s_provision_index];
    if (s_provision_retries < step->retry_limit) {
        s_provision_retries++;
        s_provision_phase = retry_phase;
        s_provision_line_seen = false;
        s_provision_line_result = MODEM_PROVISION_LINE_IGNORE;
        s_next_action_ms = now_ms + 100u;
        LOGW("modem", "provision retry index=%u attempt=%u: %s",
             (unsigned)s_provision_index,
             (unsigned)s_provision_retries,
             reason);
        return;
    }
    if (step->recoverable) {
        LOGW("modem", "skipping recoverable provision index=%u: %s",
             (unsigned)s_provision_index, reason);
        provision_note_degrade(step->degrade);
        s_provision_all_verified = false;
        if ((step->prerequisites & MODEM_INIT_PREREQ_SIM_READY) == 0u) {
            s_provision_non_sim_verified = false;
        } else if (s_sim_completion_active) {
            s_sim_completion_needed = true;
            s_sim_completion_failed = true;
        }
        s_provision_index++;
        provision_reset_step_state();
        s_next_action_ms = now_ms + 50u;
        return;
    }
    modem_fail_power_state(reason, true);
}

static void modem_begin_provision(uint32_t now_ms) {
    s_provision_index = 0u;
    s_provision_reboot_required = false;
    if (!s_sim_completion_active) {
        s_provision_non_sim_verified = true;
    }
    s_provision_all_verified = s_provision_non_sim_verified;
    provision_reset_step_state();
    if (g_modem_vendor.provision_step_count == 0u) {
        modem_enter_ready(now_ms);
        return;
    }
    s_state = MODEM_STATE_PROVISION;
    s_next_action_ms = now_ms + 50u;
    LOGI("modem", "AT init complete; checking provisioning");
}

static void provision_advance(uint32_t now_ms) {
    if (s_provision_index >= g_modem_vendor.provision_step_count) {
        if (!s_provision_reboot_required) {
            modem_enter_ready(now_ms);
            return;
        }
        if (s_sim_completion_active && s_provision_rebooted) {
            /* One automatic provisioning reboot is the per-phone-boot limit.
             * A SIM inserted after that reboot may expose another SIM-gated NVM
             * mismatch. It has already been written and verified; defer its
             * apply boundary to the next ordinary module power cycle instead
             * of surprising the user with a second automatic restart. */
            s_provision_reboot_required = false;
            s_provision_all_verified = false;
            s_sim_completion_needed = true;
            s_sim_completion_failed = true;
            LOGW("modem", "SIM provisioning reboot deferred to next power cycle");
            modem_enter_ready(now_ms);
            return;
        }
        if (s_provision_rebooted || g_modem_vendor.provision_reboot_cmd == NULL ||
            g_modem_vendor.provision_reboot_timeout_ms == 0u) {
            modem_fail_power_state(
                "provisioning requires an unavailable second reboot", true);
            return;
        }
        send_command(MODEM_AT_PROVISION_REBOOT,
                     g_modem_vendor.provision_reboot_cmd,
                     g_modem_vendor.provision_reboot_timeout_ms,
                     now_ms);
        s_provision_reboot_cycle_active = true;
        s_provision_reboot_drop_seen = false;
        LOGI("modem", "provisioning verified; one controlled reboot requested");
        return;
    }

    const modem_provision_step_t *step =
        &g_modem_vendor.provision_steps[s_provision_index];
    if (!s_sim_completion_active &&
        (step->prerequisites & MODEM_INIT_PREREQ_SIM_COMPLETION) != 0u) {
        /* Like its init counterpart, this row closes a focused SIM lifecycle
         * and is intentionally absent from the ordinary hardware pass. */
        s_provision_index++;
        provision_reset_step_state();
        s_next_action_ms = now_ms + 50u;
        return;
    }
    if (s_sim_completion_active &&
        !provision_step_is_sim_dependent(step)) {
        s_provision_index++;
        provision_reset_step_state();
        s_next_action_ms = now_ms + 50u;
        return;
    }
    if (step->applicable != NULL && !step->applicable()) {
        provision_complete_step(now_ms);
        return;
    }
    if (!provision_prerequisites_met(step)) {
        LOGI("modem", "skipping inapplicable provision index=%u",
             (unsigned)s_provision_index);
        s_provision_all_verified = false;
        if ((step->prerequisites & MODEM_INIT_PREREQ_SIM_READY) != 0u) {
            s_sim_completion_needed = true;
            s_sim_steps_skipped = true;
            if (s_sim_completion_active) {
                s_sim_completion_failed = true;
            }
        } else {
            s_provision_non_sim_verified = false;
        }
        provision_complete_step(now_ms);
        return;
    }
    if (step->query_cmd == NULL || step->parse_readback == NULL ||
        step->timeout_ms == 0u ||
        (step->query_only && provision_has_writer(step)) ||
        (!step->query_only && !provision_has_writer(step)) ||
        (step->set_each_pass &&
         (step->query_only || !provision_has_writer(step) ||
          (step->persistence != MODEM_SETTING_RUNTIME &&
           step->persistence != MODEM_SETTING_PROFILE))) ||
        (step->qualifies_sms_wake &&
         (!g_modem_vendor.sms_wake.qualified_by_sim_completion ||
          (step->prerequisites &
           (MODEM_INIT_PREREQ_SIM_READY |
            MODEM_INIT_PREREQ_SIM_COMPLETION)) !=
              (MODEM_INIT_PREREQ_SIM_READY |
               MODEM_INIT_PREREQ_SIM_COMPLETION))) ||
        (step->set_cmd != NULL && step->build_set_cmd != NULL)) {
        modem_fail_power_state("invalid vendor provisioning descriptor", true);
        return;
    }

    if (s_provision_phase == MODEM_PROVISION_SET) {
        const char *set_cmd = step->set_cmd;
        char built_cmd[MODEM_PROVISION_COMMAND_MAX];
        if (step->build_set_cmd != NULL) {
            if (!step->build_set_cmd(built_cmd, sizeof(built_cmd))) {
                modem_fail_power_state(
                    "vendor provisioning command build failed", true);
                return;
            }
            set_cmd = built_cmd;
        }
        LOGI("modem", "provision write index=%u cmd=%s",
             (unsigned)s_provision_index, set_cmd);
        send_command(MODEM_AT_PROVISION_SET, set_cmd, step->timeout_ms,
                     now_ms);
    } else {
        s_provision_line_seen = false;
        s_provision_line_result = MODEM_PROVISION_LINE_IGNORE;
        if (step->readback_begin != NULL) {
            step->readback_begin();
        }
        send_command(MODEM_AT_PROVISION_QUERY, step->query_cmd,
                     step->timeout_ms, now_ms);
    }
}

static void provision_handle_final(bool ok, bool timeout, uint32_t now_ms) {
    if (s_state != MODEM_STATE_PROVISION ||
        s_provision_index >= g_modem_vendor.provision_step_count) {
        modem_fail_power_state("provisioning result outside active step", true);
        return;
    }
    const modem_provision_step_t *step =
        &g_modem_vendor.provision_steps[s_provision_index];

    if (s_provision_phase == MODEM_PROVISION_SET) {
        if (ok) {
            s_provision_step_written = true;
            s_provision_phase = MODEM_PROVISION_VERIFY;
            s_next_action_ms = now_ms + 50u;
        } else if (timeout) {
            if (step->set_each_pass) {
                /* Matching readback predated this transient side effect, so it
                 * cannot prove the lost-final SET armed the hardware. Retrying
                 * is safe because this mode is restricted to runtime/profile
                 * commands. */
                provision_retry_or_fail("provision runtime set timed out",
                                        MODEM_PROVISION_SET, now_ms);
            } else {
                /* The write may have reached NVM before the final disappeared.
                 * Query first; never blindly repeat a potentially successful
                 * persistent write. */
                s_provision_step_written = true;
                s_provision_phase = MODEM_PROVISION_VERIFY;
                s_next_action_ms = now_ms + 100u;
            }
        } else {
            provision_retry_or_fail("provision set command rejected",
                                    MODEM_PROVISION_SET, now_ms);
        }
        return;
    }

    bool final_owned_by_vendor = false;
    if (step->readback_finish != NULL) {
        modem_provision_line_t result =
            step->readback_finish(ok, timeout);
        s_provision_line_seen = result != MODEM_PROVISION_LINE_IGNORE;
        s_provision_line_result = result;
        final_owned_by_vendor = true;
    }

    if ((!ok && !final_owned_by_vendor) || !s_provision_line_seen ||
        s_provision_line_result == MODEM_PROVISION_LINE_INVALID) {
        provision_retry_or_fail(timeout
                                    ? "provision readback timed out"
                                    : "provision readback missing or invalid",
                                s_provision_phase, now_ms);
        return;
    }
    if (s_provision_line_result == MODEM_PROVISION_LINE_MATCH) {
        if (s_provision_phase == MODEM_PROVISION_QUERY &&
            step->set_each_pass) {
            s_provision_phase = MODEM_PROVISION_SET;
            s_provision_retries = 0u;
            s_next_action_ms = now_ms + 50u;
            return;
        }
        /* Only a successful query/verify final can qualify a lifecycle-based
         * SMS wake path. Skipped/inapplicable rows never reach this point. */
        if (!provision_qualify_sms_wake(step)) {
            return;
        }
        provision_complete_step(now_ms);
        return;
    }
    if (s_provision_line_result != MODEM_PROVISION_LINE_MISMATCH) {
        provision_retry_or_fail("unexpected provision readback result",
                                s_provision_phase, now_ms);
        return;
    }

    if (s_provision_phase == MODEM_PROVISION_QUERY) {
        if (step->query_only) {
            provision_retry_or_fail("query-only provision did not match",
                                    MODEM_PROVISION_QUERY, now_ms);
            return;
        }
        if (s_provision_rebooted && !s_sim_completion_active &&
            step->persistence == MODEM_SETTING_NVM_REBOOT) {
            provision_retry_or_fail(
                "reboot-required setting still mismatched after reboot",
                MODEM_PROVISION_QUERY, now_ms);
            return;
        }
        s_provision_phase = MODEM_PROVISION_SET;
        s_provision_retries = 0u;
        s_next_action_ms = now_ms + 50u;
    } else {
        provision_retry_or_fail("provision write did not verify",
                                MODEM_PROVISION_SET, now_ms);
    }
}

static void provision_note_reboot_drop(uint32_t now_ms) {
    if (s_provision_reboot_drop_seen) {
        return;
    }
    s_provision_reboot_drop_seen = true;
    if (!s_uart_parked) {
        modem_uart_hal_park();
        s_uart_parked = true;
    }
    modem_uart_hal_set_dtr_sleep_permitted(false);
    s_dtr_sleep_permitted = false;
    s_dtr_wake_pending = false;
    s_ri_release_pending = false;
    s_ri_release_deadline_ms = 0u;
    LOGI("modem", "provisioning reboot reset edge observed at %lums",
         (unsigned long)now_ms);
}

static void provision_begin_reboot_wait(uint32_t now_ms) {
    s_diag_controlled_restarts++;
    s_diag_last_recovery_reason = MODEM_DIAG_RECOVERY_PROVISION_REBOOT;
    s_provision_rebooted = true;
    s_provision_reboot_required = false;
    s_provision_reboot_cycle_active = true;
    if (g_modem_vendor.power_is_on == NULL) {
        s_provision_reboot_drop_seen = true;
    }
    modem_begin_reinit_wait(now_ms);
    LOGI("modem", "waiting for module after controlled provisioning reboot");
}

static void modem_recover_startup_restart(uint32_t now_ms) {
    /* FWAUTOSIM may restart Telit during the first AT exchanges. Wait once for
     * the observed drop with PG still good; do not pulse ON_OFF, cut VCC, or
     * spend the separate provisioning-reboot allowance. */
    s_startup_restart_recovered = true;
    s_diag_automatic_recoveries++;
    s_diag_last_recovery_reason = MODEM_DIAG_RECOVERY_STARTUP_RESTART;
    s_diag_last_transition_ms = now_ms;
    model_cancel_deferred_command();
    cancel_request_session();
    s_active = false;
    s_active_kind = MODEM_AT_NONE;
    s_active_call_token = 0u;
    s_active_sms_wake_arm = false;
    s_operation = MODEM_OP_NONE;
    s_provision_reboot_required = false;
    if (!s_uart_parked) {
        modem_uart_hal_park();
        s_uart_parked = true;
    }
    modem_begin_reinit_wait(now_ms);
    LOGW("modem", "startup module status dropped; waiting once for restart");
}

static void modem_begin_reinit_wait(uint32_t now_ms) {
    /* A reboot invalidates the interrupted pass, even if it was SIM-only. */
    s_sim_completion_active = false;
    s_sim_completion_pending = false;
    s_sim_completion_needed = false;
    s_sim_completion_failed = false;
    s_sim_steps_skipped = false;
    s_provision_index = 0u;
    provision_reset_step_state();
    s_init_index = 0u;
    s_init_retries = 0u;
    modem_uart_hal_set_dtr_sleep_permitted(false);
    s_dtr_sleep_permitted = false;
    s_dtr_wake_pending = false;
    s_ri_release_pending = false;
    s_ri_release_deadline_ms = 0u;
    sms_wake_reset_session();
    phonebook_clear_cache();
    modem_line_framer_reset(&s_line_framer);
    s_power_pulsed = true;
    s_boot_deadline_ms = now_ms + g_modem_vendor.power.ready_budget_ms;
    modem_enter_module_wait(now_ms);
    s_next_action_ms = now_ms + MODEM_PING_RETRY_MS;

    critical_section_enter_blocking(&s_status_lock);
    s_status.at_ready = false;
    s_status.provisioning_verified = false;
    s_status.sim_checked = false;
    s_status.sim_present = false;
    s_status.sim_ready = false;
    s_status.network_registered = false;
    s_status.cereg = 0xffu;
    signal_invalidate_locked(now_ms);
    sim_provider_reset();
    s_status.last_update_ms = now_ms;
    critical_section_exit(&s_status_lock);
}

static void handle_ping_failed(uint32_t now_ms) {
    if (s_power_off_pending) {
        if (time_diff_ms(now_ms, s_boot_deadline_ms) < 0) {
            s_state = MODEM_STATE_PROBE;
            s_next_action_ms = now_ms + MODEM_PING_RETRY_MS;
        } else {
            /* PWRMON/CTS were present, so a module may be alive even though no
             * AT exchange succeeded. Retain its rail; lack of an AT final is
             * never evidence that a hard cut is safe. */
            s_power_off_pending = false;
            modem_fail_power_state(
                "startup shutdown could not establish an AT channel", true);
        }
        return;
    }
    if (!s_power_pulsed) {
        modem_start_power_pulse(now_ms);
    } else if (time_diff_ms(now_ms, s_boot_deadline_ms) < 0) {
        s_state = MODEM_STATE_PROBE;
        s_next_action_ms = now_ms + MODEM_PING_RETRY_MS;
    } else if (!s_startup_recycled) {
        /* No AT ever arrived. Retire the uncertain module state through the
         * vendor off path, enforce a real rail-down dwell, and try once more.
         * The old behavior parked forever in FAILED until a manual rail cycle;
         * that exact failure was reproduced after a bench reflash. */
        s_startup_recycled = true;
        s_diag_automatic_recoveries++;
        s_diag_last_recovery_reason = MODEM_DIAG_RECOVERY_STARTUP_AT;
        LOGE("modem", "startup AT probe failed; automatic rail-cycle");
        modem_begin_power_off(now_ms);
        s_power_on_pending = true;
    } else {
        modem_fail_power_state("modem did not answer AT", true);
    }
}

static void modem_begin_sim_completion(uint32_t now_ms) {
    s_sim_completion_pending = false;
    s_sim_completion_active = true;
    s_sim_completion_failed = false;
    s_sim_steps_skipped = false;
    s_init_index = 0u;
    s_init_retries = 0u;
    s_state = MODEM_STATE_INIT;
    s_next_action_ms = now_ms;
    critical_section_enter_blocking(&s_status_lock);
    s_status.provisioning_verified = false;
    s_status.last_update_ms = now_ms;
    critical_section_exit(&s_status_lock);
    LOGI("modem", "SIM ready; completing deferred SIM-dependent setup");
}

static uint8_t init_retry_limit(void) {
    return g_modem_vendor.init_steps[s_init_index].retry_limit;
}

static bool init_step_is_sim_dependent(const modem_init_step_t *step) {
    return step != NULL &&
        (((step->prerequisites &
           (MODEM_INIT_PREREQ_SIM_READY |
            MODEM_INIT_PREREQ_SIM_COMPLETION)) != 0u) ||
         step->degrade == MODEM_DEGRADE_SIM_GATE);
}

static bool init_step_capture_satisfied(const modem_init_step_t *step) {
    if (step == NULL || step->capture == MODEM_INIT_CAPTURE_NONE) {
        return false;
    }
    if (step->capture == MODEM_INIT_CAPTURE_BOARD_IMEI) {
        char imei[STORE_WARRANTY_SERIAL_MAX + 1u];
        return store_board_imei_get(imei, sizeof(imei)) == STORE_STATUS_OK;
    }
    return false;
}

static bool init_step_requires_response(const modem_init_step_t *step) {
    return step != NULL &&
           (step->parse != NULL || step->capture != MODEM_INIT_CAPTURE_NONE);
}

static bool init_step_capture_line(const modem_init_step_t *step,
                                   const char *line) {
    if (step == NULL || line == NULL ||
        step->capture != MODEM_INIT_CAPTURE_BOARD_IMEI ||
        strlen(line) != STORE_WARRANTY_SERIAL_MAX) {
        return false;
    }
    for (uint8_t i = 0u; i < STORE_WARRANTY_SERIAL_MAX; i++) {
        if (line[i] < '0' || line[i] > '9') {
            return false;
        }
    }

    /* A 15-digit line is shaped like this command's payload and must not leak
     * into the URC router. Only a valid check-summed value satisfies the row;
     * malformed values force the bounded retry/recoverable-skip path. */
    if (!store_board_imei_valid(line)) {
        LOGW("modem", "rejected invalid IMEI response");
        return true;
    }

    store_status_t status = store_board_imei_provision(line);
    if (status == STORE_STATUS_OK || status == STORE_STATUS_NOT_READY) {
        s_init_parse_seen = true;
        LOGI("modem", "captured board IMEI");
    } else if (status == STORE_STATUS_CONFLICT) {
        /* Never replace the board identity. A conflict can only arise if the
         * slot changed after this row was admitted; consume it and continue
         * while leaving the durable value authoritative. */
        s_init_parse_seen = true;
        LOGE("modem", "modem IMEI conflicts with provisioned board identity");
    } else {
        LOGW("modem", "failed to store board IMEI: %u", (unsigned)status);
    }
    return true;
}

static bool init_step_prerequisites_met(void) {
    uint8_t prerequisites =
        g_modem_vendor.init_steps[s_init_index].prerequisites;
    if ((prerequisites & MODEM_INIT_PREREQ_SIM_READY) != 0u &&
        !status_sim_ready_snapshot()) {
        return false;
    }
    if ((prerequisites & MODEM_INIT_PREREQ_SIM_COMPLETION) != 0u &&
        !s_sim_completion_active) {
        return false;
    }
    return true;
}

static bool init_step_recoverable(void) {
    return g_modem_vendor.init_steps[s_init_index].recoverable;
}

/* Record which capability a skipped recoverable init step degrades, so the modem
 * can reach READY while flagging that voice or SMS may be broken. Call with
 * s_init_index still pointing at the step being skipped. */
static void note_init_step_skipped(void) {
    if (s_sim_completion_active &&
        init_step_is_sim_dependent(&g_modem_vendor.init_steps[s_init_index])) {
        s_sim_completion_needed = true;
        s_sim_completion_failed = true;
    }
    switch (g_modem_vendor.init_steps[s_init_index].degrade) {
    case MODEM_DEGRADE_AUDIO:
        s_status.audio_init_ok = false;
        break;
    case MODEM_DEGRADE_SMS_SETUP:
        s_status.sms_init_ok = false;
        /* These fail at cold init because the SIM SMS subsystem is not ready yet
         * (both succeed a few seconds later) -- retry them in the background once
         * READY instead of leaving storage at the modem default (SM), which is
         * tiny and makes incoming SMS die when it fills. */
        s_sms_setup_needed = true;
        break;
    default:
        break;
    }
}

static bool at_kind_is_operation(modem_at_kind_t kind) {
    return kind == MODEM_AT_CALL_DIAL || kind == MODEM_AT_CALL_ANSWER ||
           kind == MODEM_AT_CALL_HANGUP || kind == MODEM_AT_CALL_DTMF ||
           kind == MODEM_AT_CALL_SUPPLEMENTARY ||
           kind == MODEM_AT_CALL_FORWARD || kind == MODEM_AT_VOICE_MAILBOX ||
           kind == MODEM_AT_MESSAGE_WAITING ||
           kind == MODEM_AT_SMS_CPMS ||
           kind == MODEM_AT_SMS_STATUS_PRESERVE ||
           kind == MODEM_AT_SMS_STATUS_CONSUME ||
           kind == MODEM_AT_SMS_CMGL || kind == MODEM_AT_SMS_CMGR ||
           kind == MODEM_AT_SMS_CMGD || kind == MODEM_AT_SMS_CMGF_PDU ||
           kind == MODEM_AT_SMS_CMGF_TEXT || kind == MODEM_AT_SMS_CMGS_PROMPT ||
           kind == MODEM_AT_SMS_CMGS_FINAL || kind == MODEM_AT_SMS_CMGW_PROMPT ||
           kind == MODEM_AT_SMS_CMGW_FINAL || kind == MODEM_AT_PHONEBOOK_CPBS ||
           kind == MODEM_AT_PHONEBOOK_CPBR || kind == MODEM_AT_PHONEBOOK_CPBW ||
           kind == MODEM_AT_POWER_OFF || kind == MODEM_AT_DEBUG ||
           kind == MODEM_AT_MAINTENANCE;
}

static bool parse_expected_line(const char *line) {
    modem_sms_command_kind_t sms_kind;
    if (sms_protocol_command_from_at(s_active_kind, &sms_kind)) {
        modem_sms_protocol_request_t request;
        return sms_protocol_request_view(&request) &&
            modem_sms_protocol_parse_line(
                sms_kind, line, is_known_urc_line(line), is_final_text(line),
                &request, &s_sms_protocol_hooks);
    }
    modem_phonebook_command_kind_t phonebook_kind;
    if (phonebook_protocol_command_from_at(s_active_kind, &phonebook_kind)) {
        return modem_phonebook_protocol_parse_line(
            phonebook_kind, line, &s_phonebook_protocol_hooks);
    }
    switch (s_active_kind) {
    case MODEM_AT_INIT:
        if (s_init_index < g_modem_vendor.init_step_count) {
            const modem_init_step_t *step =
                &g_modem_vendor.init_steps[s_init_index];
            bool consumed = step->parse != NULL && step->parse(line);
            if (consumed) {
                s_init_parse_seen = true;
                return true;
            }
            if (init_step_capture_line(step, line)) {
                return true;
            }
        }
        if (starts_with(line, "+CPIN:")) {
            (void)parse_cpin(line);
            return true;
        }
        if (starts_with(line, "+PACSP") || strcmp(line, "Call Ready") == 0) {
            return true;
        }
        if (starts_with(line, "+CPMS:")) {
            return true;
        }
        break;
    case MODEM_AT_PROVISION_QUERY:
        if (s_provision_index < g_modem_vendor.provision_step_count &&
            g_modem_vendor.provision_steps[s_provision_index].parse_readback != NULL) {
            modem_provision_line_t result =
                g_modem_vendor.provision_steps[s_provision_index].parse_readback(line);
            if (result != MODEM_PROVISION_LINE_IGNORE) {
                if (is_vendor_aux_urc(line)) {
                    (void)observe_vendor_aux_line(line);
                }
                if (result == MODEM_PROVISION_LINE_INVALID) {
                    LOGW("modem", "invalid provision line index=%u line=%s",
                         (unsigned)s_provision_index, line);
                }
                if (!s_provision_line_seen) {
                    s_provision_line_seen = true;
                    s_provision_line_result = result;
                } else if (s_provision_line_result != result) {
                    s_provision_line_result = MODEM_PROVISION_LINE_INVALID;
                }
                return true;
            }
        }
        break;
    case MODEM_AT_CSQ:
        if (starts_with(line, "+CSQ:")) {
            parse_csq(line);
            return true;
        }
        break;
    case MODEM_AT_SIGNAL:
        if (signal_query_supported() &&
            starts_with(line,
                        g_modem_vendor.signal_query.response_prefix)) {
            if (s_signal_line_seen) {
                s_signal_line_invalid = true;
            } else {
                modem_signal_sample_t parsed;
                memset(&parsed, 0, sizeof(parsed));
                s_signal_line_seen = true;
                if (g_modem_vendor.signal_query.parse_response(line, &parsed)) {
                    s_signal_shadow = parsed;
                } else {
                    s_signal_line_invalid = true;
                }
            }
            return true;
        }
        break;
    case MODEM_AT_CLCC_MODEL:
        if (starts_with(line, "+CLCC:")) {
            modem_clcc_row_t row;
            if (g_modem_vendor.parse_clcc_row != NULL &&
                g_modem_vendor.parse_clcc_row(line, &row)) {
                call_model_clcc_row(&s_call_model, row.id, row.dir, row.mode,
                                    row.mpty ? 1u : 0u, row.state, row.number);
            } else {
                /* A syntactically present but corrupt row is transport loss, not
                 * authoritative evidence that a leg is absent. */
                s_model_malformed_clcc++;
                LOGW("modem", "discarded malformed CLCC row");
            }
            return true;
        }
        break;
    case MODEM_AT_SMS_CPMS_POLL:
        if (starts_with(line, "+CPMS:")) {
            parse_sms_cpms(line);
            return true;
        }
        break;
    case MODEM_AT_SMS_SETUP_CPMS:
        if (starts_with(line, "+CPMS:")) {
            return true; /* used/total echo; nothing to parse here */
        }
        break;
    case MODEM_AT_CEREG:
        if (starts_with(line, "+CEREG:")) {
            parse_cereg(line);
            return true;
        }
        break;
    case MODEM_AT_SIM_PROVIDER:
    case MODEM_AT_SIM_PROVIDER_DRAIN:
        if (g_modem_vendor.sim_provider_query.response_prefix != NULL &&
            starts_with(line, g_modem_vendor.sim_provider_query.response_prefix)) {
            if (s_active_kind == MODEM_AT_SIM_PROVIDER_DRAIN) {
                return true;
            }
            if (s_sim_provider_line_seen ||
                !g_modem_vendor.sim_provider_query.parse_response(
                    line, s_sim_provider_shadow, sizeof(s_sim_provider_shadow))) {
                s_sim_provider_line_invalid = true;
            }
            s_sim_provider_line_seen = true;
            return true;
        }
        break;
    case MODEM_AT_CALL_FORWARD:
        if (g_modem_vendor.supplementary.call_forward_response_prefix != NULL &&
            starts_with(
                line,
                g_modem_vendor.supplementary.call_forward_response_prefix)) {
            call_forward_parse_row(line);
            return true;
        }
        break;
    case MODEM_AT_CALL_FORWARD_FLAGS:
        if (g_modem_vendor.supplementary.call_forward_flags_response_prefix != NULL &&
            starts_with(line,
                g_modem_vendor.supplementary.call_forward_flags_response_prefix)) {
            modem_aux_event_t event = {0};
            if (g_modem_vendor.parse_aux_urc != NULL &&
                g_modem_vendor.parse_aux_urc(line, &event) &&
                (event.kind == MODEM_AUX_EVENT_NONE ||
                 event.kind == MODEM_AUX_EVENT_CFU_STATE)) {
                s_cfu_flags_row_seen = true;
                apply_aux_event(&event);
            }
            return true;
        }
        break;
    case MODEM_AT_VOICE_MAILBOX:
        if (g_modem_vendor.supplementary.voice_mailbox_response_prefix != NULL &&
            starts_with(
                line,
                g_modem_vendor.supplementary.voice_mailbox_response_prefix)) {
            voice_mailbox_parse_row(line);
            return true;
        }
        break;
    case MODEM_AT_MESSAGE_WAITING:
        if (g_modem_vendor.supplementary.message_waiting_response_prefix !=
                NULL &&
            starts_with(
                line,
                g_modem_vendor.supplementary.message_waiting_response_prefix)) {
            message_waiting_parse_row(line);
            return true;
        }
        break;
    case MODEM_AT_DEBUG:
        if (g_modem_vendor.supplementary.message_waiting_query_cmd != NULL &&
            g_modem_vendor.supplementary.message_waiting_response_prefix !=
                NULL &&
            g_modem_vendor.supplementary.parse_message_waiting_row != NULL &&
            strcmp(s_current_request.text,
                   g_modem_vendor.supplementary.message_waiting_query_cmd) ==
                0 &&
            starts_with(
                line,
                g_modem_vendor.supplementary.message_waiting_response_prefix)) {
            /* A solicited message-waiting read can be byte-for-byte identical
             * to an unsolicited update. Raw diagnostics must not mutate
             * production icon state by routing their own response through the
             * unsolicited parser. A row that is invalid as a read remains
             * eligible below as an unambiguous interleaved event. */
            modem_aux_event_t ignored;
            modem_message_waiting_row_result_t row_result =
                g_modem_vendor.supplementary.parse_message_waiting_row(
                    line, &ignored);
            if (row_result != MODEM_MESSAGE_WAITING_ROW_INVALID) {
                LOGI("modem", "debug AT line: %s", line);
                return true;
            }
        }
        if (is_known_urc_line(line)) {
            return false; /* let route_urc handle an async URC (+CMTI/RING/+CLIP/
                           * ID-scoped call event) that lands during a debug command instead
                           * of the catch-all swallowing it (SMS/call state lost) */
        }
        if (!is_final_text(line)) {
            LOGI("modem", "debug AT line: %s", line);
            return true;
        }
        break;
    case MODEM_AT_DIAG_QUERY:
        if (diag_parse_expected_line(line)) {
            return true;
        }
        break;
    case MODEM_AT_MAINTENANCE:
        if (maintenance_parse_expected_line(line)) {
            return true;
        }
        break;
    default:
        break;
    }
    return false;
}

/* Standard V.250 call-progress result codes. They are a command final only for
 * an active call command; elsewhere they are asynchronous call observations. */
static bool is_call_progress_final(const char *line) {
    return strcmp(line, "CONNECT") == 0 || strcmp(line, "NO CARRIER") == 0 ||
           strcmp(line, "BUSY") == 0 || strcmp(line, "NO ANSWER") == 0 ||
           strcmp(line, "NO DIALTONE") == 0 || strcmp(line, "NO DIAL TONE") == 0;
}

static bool is_call_at_kind(modem_at_kind_t kind) {
    return kind == MODEM_AT_CALL_DIAL || kind == MODEM_AT_CALL_ANSWER ||
           kind == MODEM_AT_CALL_HANGUP || kind == MODEM_AT_CALL_SUPPLEMENTARY;
}

static bool parse_final_result(const char *line, bool *ok) {
    if (strcmp(line, "OK") == 0) {
        *ok = true;
        return true;
    }
    if (strcmp(line, "CONNECT") == 0) {
        *ok = true;
        return true;
    }
    if (strcmp(line, "ERROR") == 0 || starts_with(line, "+CME ERROR:") ||
        starts_with(line, "+CMS ERROR:") || strcmp(line, "NO CARRIER") == 0 ||
        strcmp(line, "BUSY") == 0 || strcmp(line, "NO ANSWER") == 0 ||
        strcmp(line, "NO DIALTONE") == 0 || strcmp(line, "NO DIAL TONE") == 0) {
        *ok = false;
        return true;
    }
    return false;
}

static bool is_known_urc_line(const char *line) {
    return strcmp(line, "RING") == 0 ||
           strcmp(line, "CONNECT") == 0 ||
           strcmp(line, "NO CARRIER") == 0 ||
           starts_with(line, "+CLIP:") ||
           starts_with(line, "+CMTI:") ||
           starts_with(line, "+CMT:") ||
           starts_with(line, "+CDSI:") ||
           starts_with(line, "+CPIN:") ||
           starts_with(line, "+PACSP") ||
           starts_with(line, "+CREG:") ||
           starts_with(line, "+CEREG:") ||
           starts_with(line, "+CIREG:") ||
           starts_with(line, "+CIREGU:") ||
           starts_with(line, "+CIEV:") ||
           is_ccwa_urc(line) ||
           is_vendor_call_urc(line) ||
           is_vendor_aux_urc(line) ||
           strcmp(line, "Call Ready") == 0 ||
           strcmp(line, "NO DIAL TONE") == 0;
}

static bool is_ccwa_urc(const char *line) {
    return modem_line_is_ccwa_urc(line);
}

static bool is_vendor_call_urc(const char *line) {
    const char *prefix = g_modem_vendor.call_urc_prefix;
    return prefix != NULL && prefix[0] != '\0' && starts_with(line, prefix);
}

static bool is_vendor_aux_urc(const char *line) {
    if (line == NULL || g_modem_vendor.aux_urc_prefixes == NULL) {
        return false;
    }
    for (uint8_t i = 0u; i < g_modem_vendor.aux_urc_prefix_count; i++) {
        const char *prefix = g_modem_vendor.aux_urc_prefixes[i];
        if (prefix != NULL && prefix[0] != '\0' && starts_with(line, prefix)) {
            return true;
        }
    }
    return false;
}

static void apply_aux_event(const modem_aux_event_t *event) {
    if (event == NULL || event->kind == MODEM_AUX_EVENT_NONE) {
        return;
    }
    if (event->kind == MODEM_AUX_EVENT_INCOMING_DIVERTED) {
        call_model_on_incoming_diverted(&s_call_model);
        return;
    }
    if (event->kind == MODEM_AUX_EVENT_MESSAGE_STORED_UNREADABLE) {
        /* The module filed a message where this backend cannot read it: the
         * message is lost to us. Same error class as an unparseable +CMT. */
        critical_section_enter_blocking(&s_status_lock);
        s_status.command_errors++;
        critical_section_exit(&s_status_lock);
        LOGW("modem", "message stored in an unreadable store; not retrievable");
        return;
    }
    critical_section_enter_blocking(&s_status_lock);
    bool applied = modem_supplementary_apply_aux_event(event);
    if (applied) {
        supplementary_project_status_locked();
        s_status.last_update_ms = s_now_ms;
    }
    critical_section_exit(&s_status_lock);
}

static bool observe_vendor_aux_line(const char *line) {
    if (g_modem_vendor.parse_aux_urc == NULL) {
        return false;
    }
    modem_aux_event_t event;
    if (!g_modem_vendor.parse_aux_urc(line, &event)) {
        return false;
    }
    apply_aux_event(&event);
    return true;
}

static void finish_operation(bool ok) {
    bool was_call_forward = s_operation == MODEM_OP_CALL_FORWARD;
    bool was_dtmf = s_operation == MODEM_OP_DTMF;
    if (!ok) {
        critical_section_enter_blocking(&s_status_lock);
        s_status.command_errors++;
        critical_section_exit(&s_status_lock);
    }
    s_operation = MODEM_OP_NONE;
    memset(&s_current_request, 0, sizeof(s_current_request));
    if (was_call_forward) {
        modem_supplementary_call_forward_reset();
    }
    if (was_dtmf) {
        s_dtmf_cancel_requested = false;
    }
    modem_sms_protocol_operation_finished();
    set_operation_busy(false);
}

static void set_operation_busy(bool busy) {
    critical_section_enter_blocking(&s_status_lock);
    s_status.operation_busy = busy;
    s_status.last_update_ms = s_now_ms;
    critical_section_exit(&s_status_lock);
}

/* Bring the modem<->codec voice bridge up on entry to ACTIVE and down on leaving
 * it. This observes the model's final once-per-tick projection, so every event,
 * CLCC merge, and transaction transition follows the same bridge path. It also
 * correctly ignores held/waiting sub-states that keep ACTIVE. Runs on core0:
 * codec route I2C happens here, ring activation is posted to core1. */
/* Tracks whether the voice bridge SHOULD be running: a call is active AND not on
 * network hold. Keying on this (not call_state alone) makes a single-call hold /
 * retrieve (call_on_hold toggles while call_state stays ACTIVE) a deterministic
 * bridge stop / restart -- otherwise, if the modem drops its I2S BCLK during the
 * hold, the core1 BCLK-loss fallback tears the bridge down and nothing restarts
 * it on retrieve (no call_state edge for the follower to see). */
static bool model_leg_is_confirmed_active(uint8_t id) {
    if (id == 0u) return false;
    for (unsigned i = 0u; i < MODEM_MAX_CALL_LEGS; i++) {
        const call_leg_t *leg = &s_call_model.legs[i];
        if (leg->in_use && leg->id == id) {
            return leg->state == CALL_LEG_ACTIVE && !leg->pending_removal;
        }
    }
    return false;
}

void modem_service_accessory_changed(bool headset_inserted) {
    if (!s_bridge_active_wanted) {
        /* No live call: switch the codec route NOW, not just at call start —
         * real-3210 bench check 2026-07-10: with a headset inserted, ALL
         * earpiece-path sounds (keypad clicks, tones) play on the headset even
         * at standby. set_route also republishes the core1 sample format and,
         * with the playback path idle (A2), stays muted until the next
         * set_playback_idle(false). MICBIAS hold tracks the accessory (the
         * A1: the hook sense needs bias while inserted). */
        nau88c22_codec_set_route(headset_inserted ? NAU_ROUTE_HEADSET
                                                  : NAU_ROUTE_HANDSET);
        nau88c22_codec_set_mic_power(false, headset_inserted);
        return;
    }
    /* Mid-call accessory change: swap the codec route (output + mic) live. The
     * bridge keeps running; set_route republishes the sample format to core1 and
     * unmutes to the cached per-output call-volume gains, so the new transducer
     * comes up at the user's current volume. Without this, removing the headset
     * mid-call left the audio on a disconnected route until hang-up. */
    nau88c22_codec_set_mic_power(true, headset_inserted);
    nau_route_t route = headset_inserted ? NAU_ROUTE_HEADSET : NAU_ROUTE_HANDSET;
    nau88c22_codec_set_route(route);
    LOGI("modem", "mid-call accessory reroute route=%d", (int)route);
}

static void modem_bridge_follow_call_state(void) {
    uint8_t model_cleanup_id = 0u;
    bool model_cleanup_pending =
        call_model_new_call_cleanup_pending(&s_call_model,
                                            &model_cleanup_id);
    critical_section_enter_blocking(&s_status_lock);
    modem_call_state_t state = s_call_projection.call_state;
    bool on_hold = s_call_projection.call_on_hold;
    uint8_t active_id = s_call_projection.active_call_id;
    bool audio_init_ok = s_status.audio_init_ok;
    critical_section_exit(&s_status_lock);
    uint8_t cleanup_id = model_cleanup_pending ? model_cleanup_id : 0u;

    /* Bridge runs while the call is active AND not held. call_on_hold is set only
     * for the sole active call (a two-call swap leaves it false), so a swap keeps
     * the active leg's bridge up. A survivor's id-scoped ACTIVE event is positive
     * media evidence even if a cancelled setup leg has not emitted RELEASED yet:
     * keep that leg's cleanup identity for final attribution, but do not mute the
     * confirmed survivor behind it. The audio service independently requires a
     * fresh stable BCLK and a safe DMA resync before publishing the bridge.
     *
     * During that cleanup window the flat projection can retain an ACTIVE-looking
     * foreground while its underlying survivor leg is still HELD. Require the
     * id-authoritative leg itself to be ACTIVE, and never accept the cleanup target
     * as the survivor. This keeps model publication silent on mere command
     * acceptance while allowing prompt media on the survivor's ACTIVE URC. */
    bool cleanup_media_confirmed =
        cleanup_id == 0u ||
        (active_id != cleanup_id && model_leg_is_confirmed_active(active_id));
    bool wanted = modem_service_voice_transport_available() && audio_init_ok &&
                  (state == MODEM_CALL_ACTIVE) && !on_hold &&
                  cleanup_media_confirmed;
    /* A codec outage can keep wanted=false for an entire short call. In that
     * case no bridge edge exists to run the ordinary teardown block below, but
     * calls_app may already have latched the user's semantic volume while the
     * call was ACTIVE. Clear that intent once the model is no longer ACTIVE so
     * a later codec recovery cannot apply stale call attenuation to idle tones.
     * Do not clear it for a held ACTIVE call: retrieve must preserve volume. */
    if (!wanted && !s_bridge_active_wanted && state != MODEM_CALL_ACTIVE) {
        core1_services_codec_reset_call_volume();
    }
    if (wanted == s_bridge_active_wanted) {
        return;
    }
    /* ACTIVE on both sides of a stop => a HOLD (call continues); otherwise the
     * call actually ended. */
    bool hold_transition = (state == MODEM_CALL_ACTIVE);
    s_bridge_active_wanted = wanted;

    if (wanted) {
        /* Call connect OR retrieve-from-hold. Route to the headset when one is
         * inserted (1:1: route 8 = headset), else the handset receiver. The codec
         * route API also swaps the mic (internal left -> headset right). */
        bool headset = accessory_hal_headset_inserted();
        /* Mic-chain gate: mic chain up for the call (kept off at idle/hold). Before
         * set_route so the route's POWER2 write already carries the enables. */
        nau88c22_codec_set_mic_power(true, headset);
        nau_route_t route = headset ? NAU_ROUTE_HEADSET : NAU_ROUTE_HANDSET;
        nau88c22_codec_set_route(route);
        core1_post_command(CORE1_CMD_AUDIO_BRIDGE_START, (uint16_t)route);
        LOGI("modem", "voice bridge start route=%d%s", (int)route,
             hold_transition ? " (retrieve)" : "");
    } else {
        core1_post_command(CORE1_CMD_AUDIO_BRIDGE_STOP, 0u);
        bool headset = accessory_hal_headset_inserted();
        /* Mic-chain gate: mic chain back down (MICBIAS stays if a headset is in) --
         * also saves the ~3.8 mA mic chain during a hold. */
        nau88c22_codec_set_mic_power(false, headset);
        if (hold_transition) {
            /* HOLD: the call is still connected and will resume on the SAME route
             * and volume. Do NOT restore the idle route or reset the output gains
             * -- keep them so retrieve comes up at the user's call volume (there is
             * no connect_call volume re-seed on retrieve). set_route on retrieve
             * re-unmutes to the preserved cached gains. */
            LOGI("modem", "voice bridge hold");
        } else {
            /* Call ENDED: restore the idle codec route. The call may have switched
             * it to the headset route; without restoring, the codec stays there ->
             * earpiece SPK off -> tones/keytones silent until reboot. */
            nau_route_t idle_route = headset ? NAU_ROUTE_HEADSET : NAU_ROUTE_HANDSET;
            nau88c22_codec_set_route(idle_route);
            /* Call volume is per-call plumbing on the SAME analog stages the tone
             * paths use (their digital level tables assume the default gains); the
             * next call re-seeds them from call_volume_level at ACTIVE. Without
             * this, every ring/keytone after a quiet call plays quieter than
             * intended. */
            core1_services_codec_reset_call_volume();
            LOGI("modem", "voice bridge stop route=%d", (int)idle_route);
        }
    }
}

static bool dtmf_char_valid(char symbol) {
    /* 1:1 with v6.00's DTMF acceptor: 0-9 * # only. The tone engine can synthesise
     * A-D (the 1633 Hz column) but the original exposes no input path for them, so
     * we reject A-D too rather than send a digit the handset never could. */
    return (symbol >= '0' && symbol <= '9') || symbol == '*' || symbol == '#';
}

static bool dtmf_call_active(void) {
    /* The leg table is the call authority. A sole held call and a leg already
     * marked for removal are not valid DTMF destinations. */
    for (unsigned i = 0u; i < MODEM_MAX_CALL_LEGS; i++) {
        const call_leg_t *leg = &s_call_model.legs[i];
        if (leg->in_use && !leg->pending_removal &&
            leg->state == CALL_LEG_ACTIVE) {
            return true;
        }
    }
    return false;
}

static bool dtmf_send_current(uint32_t now_ms) {
    if (s_current_request.type != MODEM_REQ_DTMF ||
        s_current_request.index >= MODEM_DTMF_SEQUENCE_MAX) {
        return false;
    }
    char symbol = s_current_request.text[s_current_request.index];
    char command[24];
    uint32_t timeout_ms = 0u;
    if (!dtmf_char_valid(symbol) ||
        g_modem_vendor.call.build_dtmf_command == NULL ||
        !g_modem_vendor.call.build_dtmf_command(
            symbol, command, sizeof(command), &timeout_ms) ||
        timeout_ms == 0u) {
        return false;
    }
    return send_command_with_token(MODEM_AT_CALL_DTMF, command,
                                   timeout_ms, 0u, now_ms);
}

static void dtmf_finish_digit(bool ok, uint32_t now_ms) {
    if (!ok) {
        finish_operation(false);
        return;
    }

    s_current_request.index++;
    bool complete = s_current_request.index >= MODEM_DTMF_SEQUENCE_MAX ||
                    s_current_request.text[s_current_request.index] == '\0';
    bool preempted = !complete &&
        (s_dtmf_cancel_requested || !dtmf_call_active() ||
         call_control_pending() ||
         model_call_request_waiting(NULL));
    if (complete || preempted) {
        if (preempted) {
            LOGI("modem", "DTMF suffix cancelled for call control");
        }
        /* A preemption is deliberate cancellation, not a modem command error;
         * the digit that was on wire completed successfully. */
        finish_operation(true);
        return;
    }
    if (!dtmf_send_current(now_ms)) {
        finish_operation(false);
    }
}

static bool call_forward_request_valid(const call_forward_request_t *request,
                                       uint8_t *step_count_out) {
    if (!g_modem_vendor.available || !g_modem_vendor.supplementary.supported ||
        request == NULL ||
        (unsigned)request->reason >
            (unsigned)CALL_FORWARD_REASON_ALL_CONDITIONAL ||
        (unsigned)request->action >
            (unsigned)CALL_FORWARD_ACTION_ERASE ||
        (g_modem_vendor.supplementary.reason_mask &
         MODEM_CALL_FORWARD_REASON(request->reason)) == 0u ||
        g_modem_vendor.supplementary.call_forward_step_count == NULL ||
        g_modem_vendor.supplementary.build_call_forward_step == NULL) {
        return false;
    }
    uint8_t step_count =
        g_modem_vendor.supplementary.call_forward_step_count(request);
    if (step_count == 0u || step_count > MODEM_CALL_FORWARD_MAX_STEPS) {
        return false;
    }
    for (uint8_t step = 0u; step < step_count; step++) {
        char command[MODEM_PROVISION_COMMAND_MAX];
        if (!g_modem_vendor.supplementary.build_call_forward_step(
                request, step, command, sizeof(command))) {
            return false;
        }
    }
    if (step_count_out != NULL) {
        *step_count_out = step_count;
    }
    return true;
}

static void call_forward_parse_row(const char *line) {
    call_forward_row_t row;
    if (g_modem_vendor.supplementary.parse_call_forward_row == NULL ||
        !g_modem_vendor.supplementary.parse_call_forward_row(line, &row)) {
        modem_supplementary_call_forward_collect_invalid();
        return;
    }
    modem_supplementary_call_forward_collect_row(&row);
}

static void call_forward_push_result(uint32_t request_id,
                                     const call_forward_request_t *request,
                                     call_forward_outcome_t outcome,
                                     bool status_known, bool active,
                                     const char *number, bool has_delay,
                                     uint8_t delay_seconds) {
    critical_section_enter_blocking(&s_status_lock);
    modem_supplementary_call_forward_publish_result(
        request_id, request, outcome, status_known, active, number, has_delay,
        delay_seconds);
    critical_section_exit(&s_status_lock);
}

static void call_forward_finish_step(bool ok, bool timed_out) {
    uint8_t next_step = 0u;
    bool query = s_current_request.call_forward.action ==
                 CALL_FORWARD_ACTION_QUERY;
    if (modem_supplementary_call_forward_step_complete(
            ok, timed_out, query, &next_step)) {
        char command[MODEM_PROVISION_COMMAND_MAX];
        if (!g_modem_vendor.supplementary.build_call_forward_step(
                &s_current_request.call_forward, next_step, command,
                sizeof(command))) {
            /* Every step was preflighted before admission, so this can only
             * be an internal inconsistency. An earlier step may already have
             * changed network state. */
            LOGE("modem", "call-forward step %u no longer builds",
                 (unsigned)next_step);
            call_forward_finish(false, false);
            return;
        }
        send_command(MODEM_AT_CALL_FORWARD, command,
                     g_modem_vendor.supplementary.command_timeout_ms,
                     s_now_ms);
        return;
    }
    call_forward_finish(ok, timed_out);
}

static void call_forward_finish(bool ok, bool timed_out) {
    uint32_t request_id = s_current_request.request_id;
    call_forward_request_t request = s_current_request.call_forward;
    bool status_updated = false;
    critical_section_enter_blocking(&s_status_lock);
    bool semantic_ok = modem_supplementary_call_forward_finish(
        request_id, &request, ok, timed_out, s_status.network_registered,
        s_now_ms, &status_updated);
    supplementary_project_status_locked();
    if (status_updated) {
        s_status.last_update_ms = s_now_ms;
    }
    critical_section_exit(&s_status_lock);
    finish_operation(semantic_ok);
}

static void voice_mailbox_parse_row(const char *line) {
    modem_voice_mailbox_row_t row;
    if (g_modem_vendor.supplementary.parse_voice_mailbox_row == NULL ||
        !g_modem_vendor.supplementary.parse_voice_mailbox_row(line, &row)) {
        modem_supplementary_voice_mailbox_collect_invalid();
        return;
    }
    modem_supplementary_voice_mailbox_collect_row(&row);
}

static void voice_mailbox_finish(bool ok) {
    critical_section_enter_blocking(&s_status_lock);
    bool result_ok = modem_supplementary_voice_mailbox_finish(ok, s_now_ms);
    critical_section_exit(&s_status_lock);
    finish_operation(result_ok);
}

static void message_waiting_parse_row(const char *line) {
    modem_aux_event_t event;
    if (g_modem_vendor.supplementary.parse_message_waiting_row == NULL) {
        modem_supplementary_message_waiting_collect_row(
            MODEM_MESSAGE_WAITING_ROW_INVALID, NULL);
        return;
    }
    modem_message_waiting_row_result_t row_result =
        g_modem_vendor.supplementary.parse_message_waiting_row(line, &event);
    if (row_result != MODEM_MESSAGE_WAITING_ROW_VALID) {
        if (row_result == MODEM_MESSAGE_WAITING_ROW_AMBIGUOUS) {
            modem_supplementary_message_waiting_collect_row(row_result,
                                                             &event);
            return;
        }
        /* A backend may use the same prefix for read rows and unsolicited
         * updates. If an unambiguously URC-shaped line lands inside a query,
         * publish it as an async event and keep waiting for the real read row.
         * Ambiguous forms remain owned by the active query and its final
         * authoritative snapshot. */
        if (observe_vendor_aux_line(line)) {
            return;
        }
        modem_supplementary_message_waiting_collect_row(row_result, &event);
        return;
    }
    modem_supplementary_message_waiting_collect_row(row_result, &event);
}

static void message_waiting_finish(bool ok) {
    critical_section_enter_blocking(&s_status_lock);
    bool result_ok =
        modem_supplementary_message_waiting_finish(ok, s_now_ms);
    if (result_ok) {
        supplementary_project_status_locked();
        s_status.last_update_ms = s_now_ms;
    }
    critical_section_exit(&s_status_lock);
    finish_operation(result_ok);
}

static void cancel_request(const modem_request_t *request,
                           modem_phonebook_outcome_t phonebook_outcome) {
    if (request == NULL) {
        return;
    }
    model_cancel_request(request);
    if (request->type == MODEM_REQ_CALL_FORWARD) {
        call_forward_push_result(request->request_id,
                                 &request->call_forward,
                                 CALL_FORWARD_OUTCOME_CANCELLED,
                                 false, false, NULL, false, 0u);
    }
    modem_phonebook_op_t operation;
    if (phonebook_operation_from_request_type(request->type, &operation)) {
        phonebook_push_result(request->request_id, operation,
                              phonebook_outcome);
    }
    modem_sms_request_kind_t sms_kind;
    if (sms_request_kind_from_request_type(request->type, &sms_kind)) {
        modem_sms_outcome_t outcome =
            phonebook_outcome == MODEM_PHONEBOOK_OUTCOME_EVICTED
                ? MODEM_SMS_OUTCOME_EVICTED
                : MODEM_SMS_OUTCOME_CANCELLED;
        (void)sms_publish_terminal_for_request(request, outcome);
    }
}

static void cancel_request_session(void) {
    typedef struct {
        uint32_t call_token;
        uint32_t request_id;
        call_forward_request_t call_forward;
        bool is_call_forward;
        bool call_forward_uncertain;
        modem_phonebook_op_t phonebook_operation;
        bool is_phonebook;
        modem_sms_request_kind_t sms_kind;
        modem_sms_outcome_t sms_outcome;
        modem_sms_mailbox_t sms_mailbox;
        uint32_t sms_identity_hash;
        bool is_sms;
    } cancelled_request_t;

    cancelled_request_t cancelled[MODEM_REQUEST_QUEUE_LEN + 1u];
    uint8_t cancelled_count = 0u;

    if (!s_request_lock_ready) {
        memset(&s_current_request, 0, sizeof(s_current_request));
        s_dtmf_cancel_requested = false;
        s_request_head = 0u;
        s_request_tail = 0u;
        s_request_count = 0u;
        modem_supplementary_call_forward_reset();
        return;
    }

    critical_section_enter_blocking(&s_request_lock);
    if (s_operation != MODEM_OP_NONE || s_current_request.call_token != 0u ||
        s_current_request.request_id != 0u) {
        cancelled[cancelled_count++] = (cancelled_request_t){
            .call_token = s_current_request.call_token,
            .request_id = s_current_request.request_id,
            .call_forward = s_current_request.call_forward,
            .is_call_forward =
                s_current_request.type == MODEM_REQ_CALL_FORWARD,
            .call_forward_uncertain =
                s_operation == MODEM_OP_CALL_FORWARD &&
                modem_supplementary_call_forward_cancel_uncertain(
                    s_active && s_active_kind == MODEM_AT_CALL_FORWARD,
                    s_current_request.call_forward.action ==
                        CALL_FORWARD_ACTION_QUERY),
        };
        (void)phonebook_operation_from_request_type(
            s_current_request.type,
            &cancelled[cancelled_count - 1u].phonebook_operation);
        cancelled[cancelled_count - 1u].is_phonebook =
            cancelled[cancelled_count - 1u].phonebook_operation !=
                MODEM_PHONEBOOK_OP_NONE;
        cancelled[cancelled_count - 1u].is_sms =
            sms_request_kind_from_request_type(
                s_current_request.type,
                &cancelled[cancelled_count - 1u].sms_kind);
        if (cancelled[cancelled_count - 1u].is_sms) {
            modem_sms_protocol_request_t sms_request;
            cancelled[cancelled_count - 1u].sms_outcome =
                sms_protocol_request_view(&sms_request)
                    ? modem_sms_protocol_cancel_outcome(&sms_request)
                    : MODEM_SMS_OUTCOME_CANCELLED;
            cancelled[cancelled_count - 1u].sms_mailbox =
                s_current_request.index ==
                        (uint16_t)MODEM_SMS_MAILBOX_OUTBOX
                    ? MODEM_SMS_MAILBOX_OUTBOX
                    : MODEM_SMS_MAILBOX_INBOX;
            cancelled[cancelled_count - 1u].sms_identity_hash =
                s_current_request.sms_identity_hash;
        }
    }
    while (s_request_count > 0u &&
           cancelled_count < sizeof(cancelled) / sizeof(cancelled[0])) {
        const modem_request_t *request = &s_request_queue[s_request_head];
        cancelled[cancelled_count++] = (cancelled_request_t){
            .call_token = request->call_token,
            .request_id = request->request_id,
            .call_forward = request->call_forward,
            .is_call_forward = request->type == MODEM_REQ_CALL_FORWARD,
        };
        (void)phonebook_operation_from_request_type(
            request->type,
            &cancelled[cancelled_count - 1u].phonebook_operation);
        cancelled[cancelled_count - 1u].is_phonebook =
            cancelled[cancelled_count - 1u].phonebook_operation !=
                MODEM_PHONEBOOK_OP_NONE;
        cancelled[cancelled_count - 1u].is_sms =
            sms_request_kind_from_request_type(
                request->type,
                &cancelled[cancelled_count - 1u].sms_kind);
        if (cancelled[cancelled_count - 1u].is_sms) {
            cancelled[cancelled_count - 1u].sms_outcome =
                MODEM_SMS_OUTCOME_CANCELLED;
            cancelled[cancelled_count - 1u].sms_mailbox =
                request->index == (uint16_t)MODEM_SMS_MAILBOX_OUTBOX
                    ? MODEM_SMS_MAILBOX_OUTBOX
                    : MODEM_SMS_MAILBOX_INBOX;
            cancelled[cancelled_count - 1u].sms_identity_hash =
                request->sms_identity_hash;
        }
        s_request_head =
            (uint8_t)((s_request_head + 1u) % MODEM_REQUEST_QUEUE_LEN);
        s_request_count--;
    }
    s_request_head = 0u;
    s_request_tail = 0u;
    s_request_count = 0u;
    memset(s_request_queue, 0, sizeof(s_request_queue));
    memset(&s_current_request, 0, sizeof(s_current_request));
    s_dtmf_cancel_requested = false;
    critical_section_exit(&s_request_lock);

    phonebook_cancel_protocol();
    sms_cancel_protocol();

    for (uint8_t i = 0u; i < cancelled_count; i++) {
        model_cancel_token(cancelled[i].call_token);
        if (cancelled[i].is_call_forward) {
            call_forward_push_result(cancelled[i].request_id,
                                     &cancelled[i].call_forward,
                                     cancelled[i].call_forward_uncertain
                                         ? CALL_FORWARD_OUTCOME_RESULT_UNKNOWN
                                         : CALL_FORWARD_OUTCOME_CANCELLED,
                                     false, false, NULL, false, 0u);
        }
        if (cancelled[i].is_phonebook) {
            phonebook_push_result(cancelled[i].request_id,
                                  cancelled[i].phonebook_operation,
                                  MODEM_PHONEBOOK_OUTCOME_CANCELLED);
        }
        if (cancelled[i].is_sms) {
            (void)sms_publish_terminal(
                cancelled[i].request_id, cancelled[i].sms_kind,
                cancelled[i].sms_outcome, !status_sim_ready_snapshot(),
                cancelled[i].sms_mailbox,
                cancelled[i].sms_identity_hash);
        }
    }
    modem_supplementary_call_forward_reset();
}

static bool queue_request(const modem_request_t *request) {
    if (!g_modem_vendor.available || request == 0 || !s_request_lock_ready) {
        s_request_admission_failures++;
        return false;
    }
    bool ok = false;
    critical_section_enter_blocking(&s_request_lock);
    if (s_request_count < MODEM_REQUEST_QUEUE_LEN) {
        s_request_queue[s_request_tail] = *request;
        s_request_tail = (uint8_t)((s_request_tail + 1u) % MODEM_REQUEST_QUEUE_LEN);
        s_request_count++;
        if (s_request_count > s_request_high_water) {
            s_request_high_water = s_request_count;
        }
        ok = true;
    } else {
        s_request_admission_failures++;
    }
    critical_section_exit(&s_request_lock);
    return ok;
}

static bool pop_request(modem_request_t *request) {
    if (request == 0 || !s_request_lock_ready) {
        return false;
    }
    bool ok = false;
    critical_section_enter_blocking(&s_request_lock);
    if (s_request_count > 0) {
        *request = s_request_queue[s_request_head];
        s_request_head = (uint8_t)((s_request_head + 1u) % MODEM_REQUEST_QUEUE_LEN);
        s_request_count--;
        ok = true;
    }
    critical_section_exit(&s_request_lock);
    return ok;
}

static bool call_control_pending(void) {
    /* Read the live model rather than the once-per-tick projection: a RING or
     * CLCC observation handled earlier in this tick must preempt background AT
     * work immediately. Stable ACTIVE/HELD legs deliberately return false. */
    return call_model_control_pending(&s_call_model);
}

static bool call_session_idle_snapshot(void) {
    if (!s_status_lock_ready) {
        return true;
    }
    bool operation_busy;
    critical_section_enter_blocking(&s_status_lock);
    operation_busy = s_status.operation_busy;
    critical_section_exit(&s_status_lock);
    return !operation_busy && !call_model_session_active(&s_call_model);
}

static bool sms_protocol_command_from_at(modem_at_kind_t at_kind,
                                         modem_sms_command_kind_t *out) {
    if (out == NULL) {
        return false;
    }
    switch (at_kind) {
    case MODEM_AT_SMS_CPMS:            *out = MODEM_SMS_COMMAND_CPMS; return true;
    case MODEM_AT_SMS_STATUS_PRESERVE: *out = MODEM_SMS_COMMAND_STATUS_PRESERVE; return true;
    case MODEM_AT_SMS_STATUS_CONSUME:  *out = MODEM_SMS_COMMAND_STATUS_CONSUME; return true;
    case MODEM_AT_SMS_CMGL:            *out = MODEM_SMS_COMMAND_CMGL; return true;
    case MODEM_AT_SMS_CMGR:            *out = MODEM_SMS_COMMAND_CMGR; return true;
    case MODEM_AT_SMS_CMGD:            *out = MODEM_SMS_COMMAND_CMGD; return true;
    case MODEM_AT_SMS_CMGF_PDU:        *out = MODEM_SMS_COMMAND_CMGF_PDU; return true;
    case MODEM_AT_SMS_CMGF_TEXT:       *out = MODEM_SMS_COMMAND_CMGF_TEXT; return true;
    case MODEM_AT_SMS_CMGS_PROMPT:     *out = MODEM_SMS_COMMAND_CMGS_PROMPT; return true;
    case MODEM_AT_SMS_CMGS_FINAL:      *out = MODEM_SMS_COMMAND_CMGS_FINAL; return true;
    case MODEM_AT_SMS_CMGW_PROMPT:     *out = MODEM_SMS_COMMAND_CMGW_PROMPT; return true;
    case MODEM_AT_SMS_CMGW_FINAL:      *out = MODEM_SMS_COMMAND_CMGW_FINAL; return true;
    default: return false;
    }
}

static modem_at_kind_t sms_protocol_command_to_at(
    modem_sms_command_kind_t kind) {
    switch (kind) {
    case MODEM_SMS_COMMAND_CPMS:            return MODEM_AT_SMS_CPMS;
    case MODEM_SMS_COMMAND_STATUS_PRESERVE: return MODEM_AT_SMS_STATUS_PRESERVE;
    case MODEM_SMS_COMMAND_STATUS_CONSUME:  return MODEM_AT_SMS_STATUS_CONSUME;
    case MODEM_SMS_COMMAND_CMGL:            return MODEM_AT_SMS_CMGL;
    case MODEM_SMS_COMMAND_CMGR:            return MODEM_AT_SMS_CMGR;
    case MODEM_SMS_COMMAND_CMGD:            return MODEM_AT_SMS_CMGD;
    case MODEM_SMS_COMMAND_CMGF_PDU:        return MODEM_AT_SMS_CMGF_PDU;
    case MODEM_SMS_COMMAND_CMGF_TEXT:       return MODEM_AT_SMS_CMGF_TEXT;
    case MODEM_SMS_COMMAND_CMGS_PROMPT:     return MODEM_AT_SMS_CMGS_PROMPT;
    case MODEM_SMS_COMMAND_CMGS_FINAL:      return MODEM_AT_SMS_CMGS_FINAL;
    case MODEM_SMS_COMMAND_CMGW_PROMPT:     return MODEM_AT_SMS_CMGW_PROMPT;
    case MODEM_SMS_COMMAND_CMGW_FINAL:      return MODEM_AT_SMS_CMGW_FINAL;
    default:                                return MODEM_AT_NONE;
    }
}

static bool sms_protocol_request_view(modem_sms_protocol_request_t *out) {
    if (out == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    switch (s_operation) {
    case MODEM_OP_SEND_SMS:
        out->operation = MODEM_SMS_PROTOCOL_SEND_TEXT;
        break;
    case MODEM_OP_SEND_BINARY_SMS:
        out->operation = MODEM_SMS_PROTOCOL_SEND_BINARY;
        break;
    case MODEM_OP_SAVE_SMS:
        out->operation = MODEM_SMS_PROTOCOL_SAVE;
        break;
    case MODEM_OP_SMS_MAILBOX:
        out->operation = MODEM_SMS_PROTOCOL_MAILBOX;
        break;
    case MODEM_OP_SMS_READ:
        out->operation = MODEM_SMS_PROTOCOL_READ;
        break;
    case MODEM_OP_DELETE_SMS:
        out->operation = MODEM_SMS_PROTOCOL_DELETE;
        break;
    case MODEM_OP_STORE_DELIVERED_SMS:
        out->operation = MODEM_SMS_PROTOCOL_STORE_DELIVERED;
        break;
    default:
        return false;
    }
    out->request_id = s_current_request.request_id;
    out->number = s_current_request.number;
    out->text = s_current_request.text;
    out->binary = s_current_request.binary;
    out->binary_len = s_current_request.binary_len;
    out->dest_port = s_current_request.dest_port;
    out->source_port = s_current_request.source_port;
    out->binary_mode = (modem_binary_sms_mode_t)s_current_request.binary_mode;
    out->mailbox = s_current_request.index ==
            (uint16_t)MODEM_SMS_MAILBOX_OUTBOX
        ? MODEM_SMS_MAILBOX_OUTBOX : MODEM_SMS_MAILBOX_INBOX;
    out->indices = s_current_request.indices;
    out->index_count = s_current_request.index_count;
    out->quarantined = s_current_request.sms_quarantined;
    out->expected_identity_hash = s_current_request.sms_identity_hash;
    out->read_status = (modem_sms_read_status_policy_t){
        .preserve_unread_cmd =
            g_modem_vendor.sms_read_status.preserve_unread_cmd,
        .consume_unread_cmd =
            g_modem_vendor.sms_read_status.consume_unread_cmd,
        .timeout_ms = g_modem_vendor.sms_read_status.timeout_ms,
    };
    if (s_operation == MODEM_OP_STORE_DELIVERED_SMS &&
        s_direct_active < MODEM_DIRECT_RING_DEPTH) {
        out->pdu_hex = s_direct_ring[s_direct_active].pdu_hex;
        out->tpdu_len = s_direct_ring[s_direct_active].tpdu_len;
    }
    return true;
}

static bool sms_protocol_call_preempt_pending(void) {
    return call_control_pending() || model_call_request_waiting(NULL);
}

static bool sms_current_matches(uint32_t request_id,
                                modem_sms_request_kind_t kind) {
    modem_sms_request_kind_t current_kind;
    return request_id != 0u && request_id == s_current_request.request_id &&
           sms_request_kind_from_request_type(s_current_request.type,
                                              &current_kind) &&
           current_kind == kind;
}

static bool sms_publish_terminal(
    uint32_t request_id, modem_sms_request_kind_t kind,
    modem_sms_outcome_t outcome, bool sim_not_ready,
    modem_sms_mailbox_t mailbox, uint32_t expected_identity_hash) {
    if (!s_sms_lock_ready || request_id == 0u ||
        outcome == MODEM_SMS_OUTCOME_NONE) {
        return false;
    }
    bool published = false;
    critical_section_enter_blocking(&s_sms_lock);
    switch (kind) {
    case MODEM_SMS_REQUEST_SEND_TEXT:
    case MODEM_SMS_REQUEST_SEND_BINARY:
        published = modem_sms_state_publish_send_result(
            request_id, kind, outcome);
        break;
    case MODEM_SMS_REQUEST_SAVE:
        published = modem_sms_state_publish_save_result(
            request_id, kind, outcome, sim_not_ready);
        break;
    case MODEM_SMS_REQUEST_MAILBOX:
        published = modem_sms_state_publish_mailbox_result(
            request_id, kind, outcome, sim_not_ready, false, mailbox);
        break;
    case MODEM_SMS_REQUEST_READ:
        published = modem_sms_state_publish_read_result(
            request_id, kind, outcome, sim_not_ready,
            expected_identity_hash, 0u, NULL);
        break;
    case MODEM_SMS_REQUEST_DELETE:
        published = modem_sms_state_publish_delete_result(
            request_id, kind, outcome, sim_not_ready);
        break;
    case MODEM_SMS_REQUEST_DELIVERED:
        published = true; /* internal operation: no app-facing result */
        break;
    case MODEM_SMS_REQUEST_NONE:
    default:
        break;
    }
    critical_section_exit(&s_sms_lock);
    if (!published) {
        LOGE("modem", "SMS terminal rejected id=%lu kind=%u outcome=%u",
             (unsigned long)request_id, (unsigned)kind,
             (unsigned)outcome);
    }
    return published;
}

static bool sms_publish_terminal_for_request(
    const modem_request_t *request, modem_sms_outcome_t outcome) {
    modem_sms_request_kind_t kind;
    if (request == NULL ||
        !sms_request_kind_from_request_type(request->type, &kind)) {
        return false;
    }
    modem_sms_mailbox_t mailbox =
        request->index == (uint16_t)MODEM_SMS_MAILBOX_OUTBOX
            ? MODEM_SMS_MAILBOX_OUTBOX : MODEM_SMS_MAILBOX_INBOX;
    return sms_publish_terminal(
        request->request_id, kind, outcome, !status_sim_ready_snapshot(),
        mailbox, request->sms_identity_hash);
}

static void sms_cancel_protocol(void) {
    if (modem_sms_protocol_pdu_mode_possible()) {
        /* AT+CMGF=0 crossed the wire and no AT+CMGF=1 completed: the module
         * may rest in PDU mode. Restore text mode from the idle scheduler. */
        s_sms_mode_restore_pending = true;
        s_sms_mode_restore_attempts = 0u;
    }
    modem_sms_protocol_cancel();
    s_sms_terminal_published = false;
    /* The ring entry keeps used=true: a cancelled store is retried once the
     * session is back. A half-collected +CMT (header without payload) is
     * dead with the transport; it must not eat the next session's line. */
    s_direct_active = UINT8_MAX;
    modem_sms_direct_reset();
}

static bool sms_protocol_emit(const modem_sms_protocol_action_t *action) {
    if (action == NULL) {
        return false;
    }
    switch (action->type) {
    case MODEM_SMS_ACTION_COMMAND: {
        modem_at_kind_t kind = sms_protocol_command_to_at(
            action->data.command.kind);
        if (kind == MODEM_AT_NONE || action->data.command.command == NULL) {
            return false;
        }
        if (action->data.command.raw) {
            send_raw_command(kind, action->data.command.command,
                             action->data.command.timeout_ms,
                             action->data.command.now_ms);
        } else {
            send_command(kind, action->data.command.command,
                         action->data.command.timeout_ms,
                         action->data.command.now_ms);
        }
        if (action->data.command.append_cr) {
            modem_uart_hal_write_cstr("\r");
        }
        return true;
    }
    case MODEM_SMS_ACTION_BODY: {
        if (action->data.body.bytes == NULL) {
            return false;
        }
        modem_uart_hal_write(action->data.body.bytes,
                             action->data.body.length);
        uint8_t sub = MODEM_SMS_SUB_CHAR;
        modem_uart_hal_write(&sub, 1u);
        s_active_kind = sms_protocol_command_to_at(
            action->data.body.final_kind);
        s_command_deadline_ms = action->data.body.now_ms +
            action->data.body.timeout_ms;
        if (action->data.body.binary) {
            LOGI("modem", "binary SMS PDU sent segment=%u/%u tpdu=%u hex=%u",
                 (unsigned)action->data.body.segment,
                 (unsigned)action->data.body.segment_total,
                 (unsigned)action->data.body.tpdu_len,
                 (unsigned)action->data.body.length);
        }
        LOGD("modem", "SMS body sent");
        return s_active_kind != MODEM_AT_NONE;
    }
    case MODEM_SMS_ACTION_ABORT_PROMPT:
        sms_begin_prompt_abort_settle(action->data.abort_prompt.now_ms);
        if (action->data.abort_prompt.restore_after_settle) {
            s_binary_restore_pending = true;
        }
        return true;
    case MODEM_SMS_ACTION_REQUEST_WAKE_CONTINUATION:
        sms_request_wake_qualified_continuation(
            SMS_WAKE_RESUME_OPERATION_CPMS,
            action->data.wake_continuation.now_ms);
        return true;
    case MODEM_SMS_ACTION_CLEAR_MAILBOX:
        critical_section_enter_blocking(&s_sms_lock);
        modem_sms_state_mailbox_clear();
        critical_section_exit(&s_sms_lock);
        modem_sms_state_multipart_groups_reset();
        return true;
    case MODEM_SMS_ACTION_SELECTED_BEGIN:
        critical_section_enter_blocking(&s_sms_lock);
        modem_sms_state_selected_begin(
            action->data.selected_begin.first_index,
            action->data.selected_begin.index_count,
            action->data.selected_begin.quarantined);
        critical_section_exit(&s_sms_lock);
        return true;
    case MODEM_SMS_ACTION_PUBLISH_MAILBOX: {
        if (!sms_current_matches(action->data.mailbox_result.request_id,
                                 action->data.mailbox_result.kind)) {
            return false;
        }
        critical_section_enter_blocking(&s_sms_lock);
        bool mailbox_published = modem_sms_state_publish_mailbox_result(
            action->data.mailbox_result.request_id,
            action->data.mailbox_result.kind,
            action->data.mailbox_result.outcome,
            action->data.mailbox_result.sim_not_ready,
            action->data.mailbox_result.complete,
            action->data.mailbox_result.mailbox);
        critical_section_exit(&s_sms_lock);
        s_sms_terminal_published = mailbox_published;
        return mailbox_published;
    }
    case MODEM_SMS_ACTION_PUBLISH_READ: {
        if (!sms_current_matches(action->data.read_result.request_id,
                                 action->data.read_result.kind)) {
            return false;
        }
        bool identity_mismatch = false;
        critical_section_enter_blocking(&s_sms_lock);
        bool read_published = modem_sms_state_selected_publish_result(
            action->data.read_result.request_id,
            action->data.read_result.kind,
            action->data.read_result.outcome,
            action->data.read_result.sim_not_ready,
            action->data.read_result.expected_identity_hash,
            &identity_mismatch);
        critical_section_exit(&s_sms_lock);
        s_sms_terminal_published = read_published;
        if (identity_mismatch) {
            LOGW("sms", "selected record changed before read completed");
        }
        return read_published;
    }
    case MODEM_SMS_ACTION_PUBLISH_SEND: {
        if (!sms_current_matches(action->data.result.request_id,
                                 action->data.result.kind)) {
            return false;
        }
        critical_section_enter_blocking(&s_sms_lock);
        bool send_published = modem_sms_state_publish_send_result(
            action->data.result.request_id, action->data.result.kind,
            action->data.result.outcome);
        critical_section_exit(&s_sms_lock);
        s_sms_terminal_published = send_published;
        return send_published;
    }
    case MODEM_SMS_ACTION_PUBLISH_SAVE: {
        if (!sms_current_matches(action->data.result.request_id,
                                 action->data.result.kind)) {
            return false;
        }
        critical_section_enter_blocking(&s_sms_lock);
        bool save_published = modem_sms_state_publish_save_result(
            action->data.result.request_id, action->data.result.kind,
            action->data.result.outcome,
            action->data.result.sim_not_ready);
        critical_section_exit(&s_sms_lock);
        s_sms_terminal_published = save_published;
        return save_published;
    }
    case MODEM_SMS_ACTION_PUBLISH_DELETE: {
        if (!sms_current_matches(action->data.result.request_id,
                                 action->data.result.kind)) {
            return false;
        }
        critical_section_enter_blocking(&s_sms_lock);
        bool delete_published = modem_sms_state_publish_delete_result(
            action->data.result.request_id, action->data.result.kind,
            action->data.result.outcome,
            action->data.result.sim_not_ready);
        critical_section_exit(&s_sms_lock);
        s_sms_terminal_published = delete_published;
        return delete_published;
    }
    case MODEM_SMS_ACTION_APPEND_MAILBOX: {
        bool appended;
        critical_section_enter_blocking(&s_sms_lock);
        appended = modem_sms_state_mailbox_append(
            action->data.append_mailbox.record);
        critical_section_exit(&s_sms_lock);
        return appended;
    }
    case MODEM_SMS_ACTION_ARRIVAL_SCAN_BEGIN:
        critical_section_enter_blocking(&s_status_lock);
        modem_sms_state_arrival_scan_begin(s_status.sms_received_count);
        critical_section_exit(&s_status_lock);
        return true;
    case MODEM_SMS_ACTION_ARRIVAL_SCAN_COMMIT: {
        uint32_t user_arrivals = 0u;
        critical_section_enter_blocking(&s_status_lock);
        bool committed = modem_sms_state_arrival_scan_commit(
            action->data.arrival_commit.complete,
            action->data.arrival_commit.inbox,
            s_status.sms_received_count, &user_arrivals);
        if (committed) {
            s_status.sms_user_received_count += user_arrivals;
        }
        critical_section_exit(&s_status_lock);
        return true;
    }
    case MODEM_SMS_ACTION_INCREMENT_SENT:
        critical_section_enter_blocking(&s_status_lock);
        s_status.sms_sent_count++;
        critical_section_exit(&s_status_lock);
        return true;
    case MODEM_SMS_ACTION_INCREMENT_COMMAND_ERRORS:
        critical_section_enter_blocking(&s_status_lock);
        s_status.command_errors++;
        critical_section_exit(&s_status_lock);
        return true;
    case MODEM_SMS_ACTION_PUBLISH_DELIVERED: {
        /* A pre-prompt final can latch OK with no +CMGW row: index 0 is a
         * failure regardless of the outcome. */
        bool ok = action->data.delivered.outcome == MODEM_SMS_OUTCOME_OK &&
                  action->data.delivered.index != 0u;
        if (ok) {
            uint16_t index = action->data.delivered.index;
            bool tracked = modem_sms_protocol_track_pending_arrival(index);
            critical_section_enter_blocking(&s_status_lock);
            s_status.sms_received_count++;
            if (!tracked) {
                /* Fail open like +CMTI: never leave a real SMS silent. */
                s_status.sms_user_received_count++;
            }
            critical_section_exit(&s_status_lock);
            s_cpms_check_needed = true;
            if (s_direct_active < MODEM_DIRECT_RING_DEPTH) {
                s_direct_ring[s_direct_active].used = false;
            }
            LOGI("modem", "direct SMS stored at index %u", (unsigned)index);
        } else if (action->data.delivered.outcome ==
                   MODEM_SMS_OUTCOME_STORAGE_FULL) {
            /* No attempt burned: the entry waits for room in the store. */
            if (!s_direct_store_blocked) {
                LOGW("modem", "direct SMS store blocked: message store full");
            }
            s_direct_store_blocked = true;
            s_direct_store_blocked_ms = s_now_ms;
        } else {
            if (s_direct_active < MODEM_DIRECT_RING_DEPTH) {
                s_direct_ring[s_direct_active].attempts++;
            }
            LOGW("modem", "direct SMS store failed outcome=%u",
                 (unsigned)action->data.delivered.outcome);
        }
        s_direct_active = UINT8_MAX;
        s_sms_terminal_published = true; /* internal op: nothing for the app */
        return true;
    }
    case MODEM_SMS_ACTION_COMPLETE: {
        bool owner_matches = sms_current_matches(
            action->data.complete.request_id,
            action->data.complete.kind);
        if (!owner_matches) {
            LOGE("modem", "stale SMS completion id=%lu kind=%u",
                 (unsigned long)action->data.complete.request_id,
                 (unsigned)action->data.complete.kind);
            return false;
        }
        if (!s_sms_terminal_published) {
            modem_sms_outcome_t fallback = action->data.complete.outcome;
            if (fallback == MODEM_SMS_OUTCOME_NONE ||
                fallback == MODEM_SMS_OUTCOME_OK) {
                fallback = MODEM_SMS_OUTCOME_ERROR;
            }
            (void)sms_publish_terminal_for_request(&s_current_request,
                                                   fallback);
        }
        s_sms_terminal_published = false;
        if (action->data.complete.kind == MODEM_SMS_REQUEST_DELETE &&
            action->data.complete.outcome == MODEM_SMS_OUTCOME_OK) {
            direct_store_unblock("a delete completed");
        }
        if (modem_sms_protocol_pdu_mode_possible()) {
            /* The operation completed (its result is already published) but
             * its AT+CMGF=1 never succeeded: the module may rest in PDU mode.
             * Every SMS operation, not only STORE_DELIVERED. */
            s_sms_mode_restore_pending = true;
            s_sms_mode_restore_attempts = 0u;
        }
        finish_operation(action->data.complete.command_ok);
        return true;
    }
    default:
        return false;
    }
}

static const modem_sms_protocol_hooks_t s_sms_protocol_hooks = {
    .emit = sms_protocol_emit,
    .sim_ready = status_sim_ready_snapshot,
    .call_preempt_pending = sms_protocol_call_preempt_pending,
};

static bool sms_start_current_request(uint32_t now_ms) {
    switch (s_current_request.type) {
    case MODEM_REQ_SEND_SMS:        s_operation = MODEM_OP_SEND_SMS; break;
    case MODEM_REQ_SEND_BINARY_SMS: s_operation = MODEM_OP_SEND_BINARY_SMS; break;
    case MODEM_REQ_SAVE_SMS:        s_operation = MODEM_OP_SAVE_SMS; break;
    case MODEM_REQ_SMS_MAILBOX:     s_operation = MODEM_OP_SMS_MAILBOX; break;
    case MODEM_REQ_SMS_READ:        s_operation = MODEM_OP_SMS_READ; break;
    case MODEM_REQ_DELETE_SMS:      s_operation = MODEM_OP_DELETE_SMS; break;
    case MODEM_REQ_STORE_DELIVERED_SMS:
        s_operation = MODEM_OP_STORE_DELIVERED_SMS;
        break;
    default: return false;
    }

    s_sms_terminal_published = false;
    set_operation_busy(true);
    modem_sms_protocol_request_t request;
    if (!sms_protocol_request_view(&request) ||
        !modem_sms_protocol_begin(&request, &s_sms_protocol_hooks, now_ms)) {
        (void)sms_publish_terminal_for_request(
            &s_current_request, MODEM_SMS_OUTCOME_ERROR);
        finish_operation(false);
    }
    return true;
}

static void sms_begin_prompt_abort_settle(uint32_t now_ms) {
    /* A lost '>' leaves the module consuming command text as message body.
     * ESC exits that editor, but its own final must drain while no command is
     * active; otherwise the next command can consume the abort's OK/ERROR. */
    uint8_t esc = MODEM_SMS_ESC_CHAR;
    modem_uart_hal_write(&esc, 1u);
    s_next_action_ms = now_ms + MODEM_SMS_ESC_SETTLE_MS;
}

static bool sms_wake_required(void) {
    return sms_wake_runtime_arm_available() ||
           g_modem_vendor.sms_wake.qualified_by_sim_completion;
}

static bool sms_wake_runtime_arm_available(void) {
    return g_modem_vendor.sms_wake.arm_cmd != NULL &&
           g_modem_vendor.sms_wake.arm_cmd[0] != '\0';
}

static void sms_wake_reset_session(void) {
    s_sms_wake_armed = !sms_wake_required();
    s_active_sms_wake_arm = false;
    s_sms_wake_resume = SMS_WAKE_RESUME_NONE;
    s_sms_wake_retries = 0u;
    s_sms_wake_retry_ms = 0u;
    s_sms_wake_activation_deadline_ms = 0u;
}

static void sms_wake_command_dispatched(const char *cmd, uint32_t now_ms) {
    s_active_sms_wake_arm = sms_wake_runtime_arm_available() && cmd != NULL &&
        strcmp(cmd, g_modem_vendor.sms_wake.arm_cmd) == 0;
    if (!s_active_sms_wake_arm && sms_wake_required() &&
        g_modem_vendor.sms_wake.command_invalidates_arm != NULL &&
        g_modem_vendor.sms_wake.command_invalidates_arm(cmd)) {
        /* Invalidate at the UART boundary, not on the final: ERROR/timeout can
         * still mean the module applied the command and lost its response. */
        s_sms_wake_armed = false;
        s_sms_wake_retries = 0u;
        if (g_modem_vendor.sms_wake.qualified_by_sim_completion) {
            s_sms_wake_retry_ms = 0u;
            s_sim_completion_needed = true;
            bool sim_ready = status_sim_ready_snapshot();
            s_sim_completion_pending = sim_ready;
            if (!sim_ready &&
                g_modem_vendor.sms_wake.sim_activation_window_ms != 0u) {
                s_sms_wake_activation_deadline_ms = now_ms +
                    g_modem_vendor.sms_wake.sim_activation_window_ms;
            }
            LOGD("modem", "SMS RI wake lifecycle invalidated by command");
        } else {
            s_sms_wake_retry_ms = now_ms;
            LOGD("modem", "SMS RI wake arm invalidated by command");
        }
    }
}

static void sms_complete_deferred_setup(void) {
    s_sms_setup_needed = false;
    critical_section_enter_blocking(&s_status_lock);
    s_status.sms_init_ok = true;
    critical_section_exit(&s_status_lock);
    LOGI("modem", "deferred SMS setup complete (storage=%s, RI wake=%s)",
         MODEM_SMS_STORAGE, s_sms_wake_armed ? "armed" : "retrying");
}

static void sms_continue_after_cpms(uint32_t now_ms) {
    modem_sms_protocol_request_t request;
    if (!sms_protocol_request_view(&request)) {
        finish_operation(false);
        return;
    }
    modem_sms_protocol_resume_after_wake(
        &request, &s_sms_protocol_hooks, now_ms);
}

static void sms_wake_resume_after_arm(sms_wake_resume_t resume,
                                      uint32_t now_ms) {
    if (resume == SMS_WAKE_RESUME_OPERATION_CPMS) {
        sms_continue_after_cpms(now_ms);
    } else if (resume == SMS_WAKE_RESUME_DEFERRED_SETUP) {
        sms_complete_deferred_setup();
    }
}

static void sms_wake_send_arm(uint32_t now_ms) {
    if (!sms_wake_runtime_arm_available() || s_active) {
        return;
    }
    send_command(MODEM_AT_SMS_WAKE_ARM, g_modem_vendor.sms_wake.arm_cmd,
                 g_modem_vendor.sms_wake.timeout_ms, now_ms);
}

static void sms_request_wake_qualified_continuation(
    sms_wake_resume_t resume, uint32_t now_ms) {
    /* Phase 5E handoff: CPMS has completed, but SMS work cannot resume until
     * the configured RI wake lifecycle is qualified. The continuation either
     * runs synchronously when already safe or is consumed once by the arm
     * command's terminal path. */
    if (!sms_wake_required() || s_sms_wake_armed ||
        !sms_wake_runtime_arm_available()) {
        sms_wake_resume_after_arm(resume, now_ms);
        return;
    }
    s_sms_wake_resume = resume;
    s_sms_wake_retries = 0u;
    s_sms_wake_retry_ms = now_ms;
    sms_wake_send_arm(now_ms);
}

static bool sms_wake_retry_due(uint32_t now_ms) {
    return sms_wake_runtime_arm_available() && !s_sms_wake_armed && !s_active &&
           (s_sms_wake_resume != SMS_WAKE_RESUME_NONE ||
            status_sim_ready_snapshot()) &&
           time_diff_ms(now_ms, s_sms_wake_retry_ms) >= 0;
}

static void sms_wake_handle_final(bool ok, uint32_t now_ms) {
    if (ok && s_sms_wake_armed) {
        sms_wake_resume_t resume = s_sms_wake_resume;
        s_sms_wake_resume = SMS_WAKE_RESUME_NONE;
        s_sms_wake_retries = 0u;
        s_sms_wake_retry_ms = 0u;
        LOGI("modem", "SMS RI wake armed for DTR sleep");
        sms_wake_resume_after_arm(resume, now_ms);
        return;
    }

    if (s_sms_wake_retries < g_modem_vendor.sms_wake.retry_limit) {
        s_sms_wake_retries++;
        s_sms_wake_retry_ms = now_ms + MODEM_SMS_WAKE_RETRY_DELAY_MS;
        s_next_action_ms = s_sms_wake_retry_ms;
        LOGW("modem", "SMS RI wake arm failed; retry %u/%u",
             (unsigned)s_sms_wake_retries,
             (unsigned)g_modem_vendor.sms_wake.retry_limit);
        return;
    }

    /* Do not strand the user operation forever if a vendor command is
     * unavailable. Continue fail-awake, and keep retrying in the background;
     * modem_transport_idle will refuse DTR sleep while armed is false. */
    sms_wake_resume_t resume = s_sms_wake_resume;
    s_sms_wake_resume = SMS_WAKE_RESUME_NONE;
    s_sms_wake_retries = 0u;
    s_sms_wake_retry_ms = now_ms + MODEM_SMS_WAKE_BACKGROUND_RETRY_MS;
    LOGE("modem", "SMS RI wake unarmed; DTR sleep inhibited");
    sms_wake_resume_after_arm(resume, now_ms);
}

static void push_debug_result(bool ok) {
    if (!s_sms_lock_ready) {
        return;
    }
    critical_section_enter_blocking(&s_sms_lock);
    s_debug_result.pending = true;
    s_debug_result.ok = ok;
    s_debug_result_pending = true;
    critical_section_exit(&s_sms_lock);
}

static bool phonebook_protocol_command_from_at(
    modem_at_kind_t at_kind, modem_phonebook_command_kind_t *out) {
    _Static_assert(MODEM_AT_PHONEBOOK_CPBR == MODEM_AT_PHONEBOOK_CPBS + 1,
                   "phonebook AT-kind map requires a contiguous range");
    _Static_assert(MODEM_AT_PHONEBOOK_CPBW == MODEM_AT_PHONEBOOK_CPBS + 2,
                   "phonebook AT-kind map requires a contiguous range");
    if (out == NULL || at_kind < MODEM_AT_PHONEBOOK_CPBS ||
        at_kind > MODEM_AT_PHONEBOOK_CPBW) {
        return false;
    }
    *out = (modem_phonebook_command_kind_t)(
        at_kind - MODEM_AT_PHONEBOOK_CPBS);
    return true;
}

static modem_at_kind_t phonebook_protocol_command_to_at(
    modem_phonebook_command_kind_t kind) {
    if ((unsigned)kind > (unsigned)MODEM_PHONEBOOK_COMMAND_CPBW) {
        return MODEM_AT_NONE;
    }
    return (modem_at_kind_t)(MODEM_AT_PHONEBOOK_CPBS + kind);
}

static bool phonebook_operation_from_request_type(
    modem_request_type_t type, modem_phonebook_op_t *out) {
    _Static_assert(MODEM_REQ_PHONEBOOK_ADD == MODEM_REQ_PHONEBOOK_LIST + 1,
                   "phonebook request map requires a contiguous range");
    _Static_assert(MODEM_REQ_PHONEBOOK_UPDATE == MODEM_REQ_PHONEBOOK_LIST + 2,
                   "phonebook request map requires a contiguous range");
    _Static_assert(MODEM_REQ_PHONEBOOK_DELETE == MODEM_REQ_PHONEBOOK_LIST + 3,
                   "phonebook request map requires a contiguous range");
    if (out == NULL || type < MODEM_REQ_PHONEBOOK_LIST ||
        type > MODEM_REQ_PHONEBOOK_DELETE) {
        return false;
    }
    *out = (modem_phonebook_op_t)(
        MODEM_PHONEBOOK_OP_LIST + (type - MODEM_REQ_PHONEBOOK_LIST));
    return true;
}

static bool phonebook_protocol_request_view(
    modem_phonebook_protocol_request_t *out) {
    modem_phonebook_op_t operation;
    if (out == NULL || !phonebook_operation_from_request_type(
                           s_current_request.type, &operation)) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->request_id = s_current_request.request_id;
    out->operation = operation;
    out->index = s_current_request.index;
    out->name = s_current_request.name;
    out->number = s_current_request.number;
    return true;
}

static void phonebook_finish_operation(uint32_t request_id,
                                       modem_phonebook_op_t kind,
                                       modem_phonebook_outcome_t outcome) {
    phonebook_push_result(request_id, kind, outcome);
    s_operation = MODEM_OP_NONE;
    memset(&s_current_request, 0, sizeof(s_current_request));
    set_operation_busy(false);
}

static void phonebook_clear_cache(void) {
    if (!s_phonebook_lock_ready) {
        return;
    }
    critical_section_enter_blocking(&s_phonebook_lock);
    modem_phonebook_state_clear();
    critical_section_exit(&s_phonebook_lock);
}

static void phonebook_push_result(uint32_t request_id,
                                  modem_phonebook_op_t kind,
                                  modem_phonebook_outcome_t outcome) {
    if (!s_phonebook_lock_ready) {
        return;
    }
    bool sim_not_ready = !status_sim_ready_snapshot();
    critical_section_enter_blocking(&s_phonebook_lock);
    bool published = modem_phonebook_state_publish_result(
        request_id, kind, outcome, sim_not_ready);
    critical_section_exit(&s_phonebook_lock);
    if (!published) {
        LOGE("modem", "phonebook terminal rejected id=%lu op=%u outcome=%u",
             (unsigned long)request_id, (unsigned)kind, (unsigned)outcome);
    }
}

static bool phonebook_protocol_emit(
    const modem_phonebook_protocol_action_t *action) {
    if (action == NULL) {
        return false;
    }
    switch (action->type) {
    case MODEM_PHONEBOOK_ACTION_COMMAND: {
        modem_at_kind_t kind = phonebook_protocol_command_to_at(
            action->data.command.kind);
        if (kind == MODEM_AT_NONE || action->data.command.command == NULL) {
            return false;
        }
        send_command(kind, action->data.command.command,
                     action->data.command.timeout_ms,
                     action->data.command.now_ms);
        return true;
    }
    case MODEM_PHONEBOOK_ACTION_BEGIN_REFRESH:
        if (!s_phonebook_lock_ready) {
            return false;
        }
        critical_section_enter_blocking(&s_phonebook_lock);
        modem_phonebook_state_refresh_begin();
        critical_section_exit(&s_phonebook_lock);
        return true;
    case MODEM_PHONEBOOK_ACTION_APPEND_ENTRY: {
        if (!s_phonebook_lock_ready) {
            return false;
        }
        bool appended;
        critical_section_enter_blocking(&s_phonebook_lock);
        appended = modem_phonebook_state_append(&action->data.entry);
        critical_section_exit(&s_phonebook_lock);
        return appended;
    }
    case MODEM_PHONEBOOK_ACTION_FINISH_REFRESH: {
        if (!s_phonebook_lock_ready) {
            return false;
        }
        bool finished;
        critical_section_enter_blocking(&s_phonebook_lock);
        finished = modem_phonebook_state_refresh_finish(
            action->data.refresh.publish);
        critical_section_exit(&s_phonebook_lock);
        return finished;
    }
    case MODEM_PHONEBOOK_ACTION_COMPLETE:
        phonebook_finish_operation(action->data.complete.request_id,
                                   action->data.complete.operation,
                                   action->data.complete.outcome);
        return true;
    default:
        return false;
    }
}

static const modem_phonebook_protocol_hooks_t s_phonebook_protocol_hooks = {
    .emit = phonebook_protocol_emit,
    .transport_counter = model_transport_counter,
};

static void phonebook_cancel_protocol(void) {
    modem_phonebook_protocol_cancel(&s_phonebook_protocol_hooks);
}

static bool phonebook_start_current_request(uint32_t now_ms) {
    _Static_assert(MODEM_OP_PHONEBOOK_ADD == MODEM_OP_PHONEBOOK_LIST + 1,
                   "phonebook operation map requires a contiguous range");
    _Static_assert(MODEM_OP_PHONEBOOK_UPDATE == MODEM_OP_PHONEBOOK_LIST + 2,
                   "phonebook operation map requires a contiguous range");
    _Static_assert(MODEM_OP_PHONEBOOK_DELETE == MODEM_OP_PHONEBOOK_LIST + 3,
                   "phonebook operation map requires a contiguous range");
    modem_phonebook_protocol_request_t request;
    if (!phonebook_protocol_request_view(&request)) {
        modem_phonebook_op_t operation;
        if (phonebook_operation_from_request_type(
                s_current_request.type, &operation)) {
            phonebook_finish_operation(
                s_current_request.request_id, operation,
                MODEM_PHONEBOOK_OUTCOME_ERROR);
        } else {
            finish_operation(false);
        }
        return false;
    }
    s_operation = (modem_operation_t)(
        MODEM_OP_PHONEBOOK_LIST +
        (request.operation - MODEM_PHONEBOOK_OP_LIST));
    set_operation_busy(true);

    if (!modem_phonebook_protocol_begin(
            &request, &s_phonebook_protocol_hooks, now_ms)) {
        phonebook_finish_operation(request.request_id, request.operation,
                                   MODEM_PHONEBOOK_OUTCOME_ERROR);
        return false;
    }
    return true;
}

static bool is_final_text(const char *line) {
    bool ok = false;
    return parse_final_result(line, &ok);
}

static void parse_csq(const char *line) {
    modem_csq_line_t parsed;
    if (modem_line_parse_csq(line, &parsed)) {
        critical_section_enter_blocking(&s_status_lock);
        s_status.rssi = parsed.rssi;
        s_status.ber = parsed.ber;
        s_status.last_update_ms = s_now_ms;
        critical_section_exit(&s_status_lock);
    }
}

/* AT+CPMS? read form: +CPMS: <m1>,<u1>,<t1>,<m2>,<u2>,<t2>,<m3>,<u3>,<t3>.
 * mem3 (fields 6..8) is the RECEIVE store -- the one that blocks incoming when
 * full. Track its occupancy and raise a rising-edge event (empty/partial ->
 * full) so the app can show the 1:1 "No space for new messages" notice once per
 * full episode (re-arms when the user deletes and frees a slot). */
static void parse_sms_cpms(const char *line) {
    uint16_t used = 0u;
    uint16_t total = 0u;
    if (!modem_line_parse_cpms(line, &used, &total)) {
        return; /* need the mem3 triple */
    }
    bool full = total != 0u && used >= total;
    if (!full) {
        direct_store_unblock("receive store has room");
    }
    critical_section_enter_blocking(&s_status_lock);
    s_status.sms_storage_used = used;
    s_status.sms_storage_total = total;
    if (full && !s_sms_storage_full_latched) {
        s_status.sms_storage_full_events++;
    }
    s_sms_storage_full_latched = full;
    s_status.last_update_ms = s_now_ms;
    critical_section_exit(&s_status_lock);
}

static void parse_cereg(const char *line) {
    unsigned stat = 0u;
    if (!modem_line_parse_cereg(line, &stat)) {
        return;
    }
    critical_section_enter_blocking(&s_status_lock);
    bool was_registered = s_status.network_registered;
    uint8_t previous_cereg = s_status.cereg;
    s_status.cereg = (uint8_t)stat;
    s_status.network_registered = (stat == 1u || stat == 5u);
    bool registration_changed = s_status.network_registered != was_registered ||
        (s_status.network_registered && previous_cereg != s_status.cereg);
    bool became_registered =
        registration_changed && s_status.network_registered;
    if (registration_changed) {
        /* Retire both identity and metrics before re-registration/roaming can
         * display a name from the previous registration generation. */
        s_operator_refresh_needed = s_status.network_registered;
        signal_invalidate_locked(s_now_ms);
    }
    s_status.last_update_ms = s_now_ms;
    critical_section_exit(&s_status_lock);
    if (became_registered) {
        s_signal_refresh_needed = true;
        s_next_signal_ms = s_now_ms;
        modem_supplementary_refresh_arm(MODEM_SUPPLEMENTARY_REFRESH_CFU,
                                        s_now_ms);
    } else if (registration_changed) {
        s_signal_refresh_needed = false;
        s_next_signal_ms = s_now_ms + MODEM_SIGNAL_BACKSTOP_MS;
    }
}

static bool parse_cpin(const char *line) {
    bool ready = strstr(line, "READY") != 0;
    apply_sim_observation(ready ? MODEM_SIM_OBSERVATION_READY
                                : MODEM_SIM_OBSERVATION_PRESENT);
    return ready;
}

static bool is_sim_absent_error(const char *line) {
    return modem_line_is_sim_absent_error(line);
}

static bool maintenance_holds_cfun4(void) {
    return modem_maintenance_holds_cfun4();
}

static void sim_maintenance_guard_hold_cfun4(void) {
    /* A new RF-off interval invalidates any deferred observation from an older
     * transition. While maintenance remains mutated, the guard has no expiry. */
    s_maintenance_sim_guard_deadline_ms = 0u;
    s_maintenance_sim_deferred = MODEM_SIM_OBSERVATION_NONE;
}

static void sim_maintenance_guard_begin_online(uint32_t now_ms) {
    /* Drop observations made while SIM access was intentionally unavailable;
     * only lifecycle states emitted after this CFUN=5 request may become the
     * fallback state if READY never arrives. */
    s_maintenance_sim_deferred = MODEM_SIM_OBSERVATION_NONE;
    uint32_t window_ms =
        g_modem_vendor.maintenance.sim_transition_window_ms;
    if (window_ms == 0u || !status_sim_ready_snapshot()) {
        s_maintenance_sim_guard_deadline_ms = 0u;
        return;
    }
    s_maintenance_sim_guard_deadline_ms = now_ms + window_ms;
    if (s_maintenance_sim_guard_deadline_ms == 0u) {
        s_maintenance_sim_guard_deadline_ms = UINT32_MAX;
    }
}

static void sim_maintenance_guard_tick(uint32_t now_ms) {
    if (s_maintenance_sim_guard_deadline_ms == 0u ||
        maintenance_holds_cfun4() ||
        time_diff_ms(now_ms, s_maintenance_sim_guard_deadline_ms) < 0) {
        return;
    }
    modem_sim_observation_t deferred = s_maintenance_sim_deferred;
    s_maintenance_sim_guard_deadline_ms = 0u;
    s_maintenance_sim_deferred = MODEM_SIM_OBSERVATION_NONE;
    if (deferred != MODEM_SIM_OBSERVATION_NONE) {
        apply_sim_observation(deferred);
    }
}

static void apply_sim_observation(modem_sim_observation_t observation) {
    if (observation == MODEM_SIM_OBSERVATION_NONE) {
        return;
    }
    bool was_ready;
    bool mailbox_known;
    bool message_waiting_known;
    bool cfu_known;
    bool registered;
    critical_section_enter_blocking(&s_status_lock);
    bool known_present = s_status.sim_checked && s_status.sim_present;
    bool known_ready = known_present && s_status.sim_ready;
    bool guard_active = maintenance_holds_cfun4() ||
                        s_maintenance_sim_guard_deadline_ms != 0u;
    bool maintenance_downgrade = guard_active &&
        ((known_present && observation == MODEM_SIM_OBSERVATION_ABSENT) ||
         (known_ready && observation != MODEM_SIM_OBSERVATION_READY));
    if (maintenance_downgrade) {
        /* Telit can publish #QSS: 0 in CFUN=4, followed by intermediate
         * inserted/unlocked states while CFUN=5 activates the SIM. Preserve
         * the last confirmed READY state through that vendor-bounded sequence.
         * If READY never arrives, the latest deferred state becomes
         * authoritative when the transition window expires. */
        s_maintenance_sim_deferred = observation;
        critical_section_exit(&s_status_lock);
        return;
    }
    if (guard_active && observation == MODEM_SIM_OBSERVATION_READY) {
        /* READY is positive evidence, but it is not the end of Telit's CFUN
         * lifecycle: the WWX can report CPIN READY, then a late #QSS: 2, then
         * #QSS: 3. Keep the bounded guard alive so that intermediate state
         * cannot make the focused SIM-completion pass skip required rows. */
        s_maintenance_sim_deferred = MODEM_SIM_OBSERVATION_NONE;
    }
    was_ready = s_status.sim_ready;
    s_status.sim_checked = true;
    s_status.sim_present = observation != MODEM_SIM_OBSERVATION_ABSENT;
    s_status.sim_ready = observation == MODEM_SIM_OBSERVATION_READY;
    if (!s_status.sim_ready) {
        s_status.network_registered = false;
        s_status.cereg = 0xffu;
        signal_invalidate_locked(s_now_ms);
        sim_provider_reset();
        s_operator_refresh_needed = false;
    }
    modem_supplementary_cache_t supplementary_cache;
    modem_supplementary_get_cache(&supplementary_cache);
    mailbox_known = supplementary_cache.voice_mailbox_known;
    message_waiting_known = supplementary_cache.message_waiting_known;
    cfu_known = supplementary_cache.call_forward_unconditional_known;
    registered = s_status.network_registered;
    s_status.last_update_ms = s_now_ms;
    critical_section_exit(&s_status_lock);

    if (observation != MODEM_SIM_OBSERVATION_READY) {
        phonebook_clear_cache();
        /* SIM removal/PIN lock invalidates runtime settings owned by the SIM
         * subsystem. Wait for the next trustworthy readiness edge before
         * attempting their bounded completion pass. */
        s_sim_completion_needed = true;
        s_sim_completion_pending = false;
        modem_supplementary_refresh_reset_all();
        critical_section_enter_blocking(&s_status_lock);
        modem_supplementary_invalidate_cache();
        supplementary_project_status_locked();
        critical_section_exit(&s_status_lock);
    } else if (!was_ready && s_sim_completion_needed) {
        s_sim_completion_pending = true;
    }
    if (observation == MODEM_SIM_OBSERVATION_READY) {
        /* +CPIN READY may precede Telit's final #QSS: 3. Re-arm only unknown
         * authorities on every positive readiness observation, so that later
         * #QSS evidence repairs an early failed read without creating polling. */
        if (!mailbox_known) {
            modem_supplementary_refresh_arm(
                MODEM_SUPPLEMENTARY_REFRESH_VOICE_MAILBOX, s_now_ms);
        }
        if (!message_waiting_known) {
            modem_supplementary_refresh_arm(
                MODEM_SUPPLEMENTARY_REFRESH_MESSAGE_WAITING, s_now_ms);
        }
        if (registered && !cfu_known) {
            modem_supplementary_refresh_arm(
                MODEM_SUPPLEMENTARY_REFRESH_CFU, s_now_ms);
        }
    }
}

static bool parse_cmti_index(const char *line, uint16_t *index_out) {
    return modem_line_parse_cmti(line, MODEM_SMS_STORAGE, index_out);
}

static bool status_sim_ready_snapshot(void) {
    bool ready = false;
    if (!s_status_lock_ready) {
        return false;
    }
    critical_section_enter_blocking(&s_status_lock);
    ready = s_status.sim_ready;
    critical_section_exit(&s_status_lock);
    return ready;
}

static bool status_sim_missing_snapshot(void) {
    if (!s_status_lock_ready) {
        return false;
    }
    bool missing;
    critical_section_enter_blocking(&s_status_lock);
    missing = s_status.sim_checked && !s_status.sim_present;
    critical_section_exit(&s_status_lock);
    return missing;
}

static void set_status_ready(bool ready) {
    critical_section_enter_blocking(&s_status_lock);
    s_status.at_ready = ready;
    s_status.last_update_ms = s_now_ms;
    critical_section_exit(&s_status_lock);
}

bool modem_service_is_powered_off(void) {
    /* MODEM_STATE_OFF only: OFF_DISCHARGE still has the 3V8 rail energized
     * (holding VCC through the module's internal switch-off, UBX-21010011
     * 1.6.2), so the dormant gate must keep the cores up until the cut. An OFF
     * state with power_on_pending is likewise not quiescent: it is enforcing
     * the rail-down dwell before a queued restart, and dormant entry would
     * prevent that deadline from ever dispatching. */
    return !g_modem_vendor.available ||
        (s_state == MODEM_STATE_OFF && !s_power_on_pending);
}

static void modem_enter_off(void) {
    bool was_off = s_state == MODEM_STATE_OFF;
    diag_invalidate_all(s_now_ms);
    cancel_request_session();
    s_active = false;
    s_active_kind = MODEM_AT_NONE;
    s_operation = MODEM_OP_NONE;
    maintenance_reset_session();
    s_binary_restore_pending = false;
    s_state = MODEM_STATE_OFF;
    if (!was_off) {
        s_diag_shutdown_completions++;
    }
    s_diag_last_transition_ms = s_now_ms;
    s_next_action_ms = 0u;
    s_next_signal_ms = 0u;
    s_next_cereg_ms = 0u;
    s_sms_setup_needed = false; /* re-evaluated by the next session's init */
    s_operator_refresh_needed = false;
    sim_provider_reset();
    s_signal_refresh_needed = false;
    s_signal_line_seen = false;
    s_signal_line_invalid = false;
    s_cpms_check_needed = false;
    s_dtr_wake_pending = false;
    s_dtr_sleep_permitted = false;
    s_ri_release_pending = false;
    s_ri_release_deadline_ms = 0u;
    sms_wake_reset_session();
    s_provision_reboot_cycle_active = false;
    s_provision_reboot_drop_seen = false;
    s_deferred_valid = false;
    s_deferred_call_token = 0u;
    s_failed_module_may_be_live = false;
    s_failed_supply_power = false;
    s_failed_shutdown_qualification_pending = false;
    modem_reset_shutdown_escalation();
    s_power_off_failure_pending = false;
    s_power_off_pending = false;
    s_sim_completion_needed = false;
    s_sim_completion_pending = false;
    s_sim_completion_active = false;
    s_sim_completion_failed = false;
    s_sim_steps_skipped = false;
    model_reset_call_session(s_now_ms);
    modem_supplementary_reset_transient();
    modem_sms_protocol_reset_pending_arrivals();
    direct_ring_reset();
    modem_sms_protocol_operation_finished();
    phonebook_clear_cache();
    modem_uart_hal_set_power_pin(false);
    modem_uart_hal_set_dtr_sleep_permitted(false);
    /* OFF_DISCHARGE may have left RX enabled as a SIO break sensor. Re-park
     * unconditionally so the fully-off input buffer cannot leak current. */
    modem_uart_hal_park();
    s_uart_parked = true;
    /* Real modem off: release this client's +3V8 ownership. The buzzer may keep
     * the shared rail up, but modem ON/OFF remains deasserted. Reached only on an
     * actual power-off (CPWROFF completion or a direct off) -- modem failures go
     * to STATE_FAILED, so a transient fault never drops the modem's ownership. */
    shared_3v8_service_set_required(SHARED_3V8_OWNER_MODEM, false);
    if (shared_3v8_service_enabled()) {
        s_rail_off_dwell_started = false;
        s_rail_off_dwell_complete = false;
        s_rail_off_ready_ms = 0u;
    } else {
        modem_start_rail_off_dwell(s_now_ms);
    }

    critical_section_enter_blocking(&s_status_lock);
    memset(&s_status, 0, sizeof(s_status));
    modem_supplementary_invalidate_cache();
    s_status.available = g_modem_vendor.available;
    s_status.rssi = 99u;
    s_status.ber = 99u;
    s_status.cereg = 0xffu;
    s_status.last_update_ms = s_now_ms;
    critical_section_exit(&s_status_lock);
    LOGI("modem", "powered off (+3V8 modem owner released)");

    if (s_power_on_pending) {
        /* Hold the rail DOWN for a genuine discharge dwell before the queued
         * restart (advance_state fires it from MODEM_STATE_OFF): an immediate
         * re-enable is only a supply blip and may not reset a wedged module. */
        s_next_action_ms = s_rail_off_dwell_started
            ? s_rail_off_ready_ms
            : s_now_ms + MODEM_RAIL_OFF_POLL_MS;
        LOGI("modem", "restart queued in %u ms (rail discharge dwell)",
             (unsigned)MODEM_RAIL_OFF_DWELL_MS);
    }
}

static void modem_begin_off_discharge(uint32_t now_ms) {
    modem_reset_shutdown_escalation();
    s_power_off_failure_pending = false;
    s_shutdown_stage = MODEM_SHUTDOWN_STAGE_SOFTWARE;
    s_state = MODEM_STATE_OFF_DISCHARGE;
    /* No command traffic remains after the shutdown final. Park PL011 while
     * the Telit PWRMON observation qualifies module shutdown. */
    modem_uart_hal_park();
    s_uart_parked = true;
    g_modem_vendor.off_sense_begin(now_ms);
    s_off_discharge_deadline_ms = now_ms + g_modem_vendor.power.off_sense_timeout_ms;
    s_next_action_ms = now_ms;
    LOGI("modem", "power-off command done; holding rail for module shutdown");
}

static void modem_reset_shutdown_escalation(void) {
    modem_uart_hal_cancel_shutdown_pulse();
    s_shutdown_stage = MODEM_SHUTDOWN_STAGE_NONE;
    s_shutdown_pulse_active = false;
    s_shutdown_pulse_release_ms = 0u;
}

static void modem_start_supply_fault_shutdown(uint32_t now_ms) {
    /* PG has already made the AT channel untrustworthy. Do not loop through
     * MODULE_WAIT trying to recover UART before honoring an explicit OFF
     * request: a persistently low supply would fail that wait and erase the
     * request, leaving the shared rail owned behind a dark soft-off UI. Start
     * the existing documented control-pin ladder directly, then retain VCC
     * until PWRMON supplies the same qualified-low evidence as every shutdown. */
    modem_reset_shutdown_escalation();
    s_power_off_failure_pending = false;
    s_failed_shutdown_qualification_pending = false;
    s_failed_module_may_be_live = false;
    s_state = MODEM_STATE_OFF_DISCHARGE;
    if (!s_uart_parked) {
        modem_uart_hal_park();
        s_uart_parked = true;
    }

    modem_shutdown_start_t start = modem_start_shutdown_fallback(
        MODEM_SHUTDOWN_STAGE_GRACEFUL_CONTROL, now_ms);
    if (start == MODEM_SHUTDOWN_START_UNSUPPORTED) {
        start = modem_start_shutdown_fallback(
            MODEM_SHUTDOWN_STAGE_UNCONDITIONAL, now_ms);
    }
    s_failed_supply_power = false;
    if (start == MODEM_SHUTDOWN_START_OK) {
        return;
    }
    modem_terminal_shutdown_failure(
        start == MODEM_SHUTDOWN_START_FAILED
            ? "supply-fault shutdown pulse could not be armed"
            : "supply-fault shutdown has no supported control pulse",
        now_ms);
}

static modem_shutdown_start_t modem_start_shutdown_fallback(
    modem_shutdown_stage_t stage, uint32_t now_ms) {
    modem_shutdown_pulse_t pulse;
    uint32_t width_ms;
    if (stage == MODEM_SHUTDOWN_STAGE_GRACEFUL_CONTROL) {
        pulse = MODEM_SHUTDOWN_PULSE_GRACEFUL;
        width_ms = g_modem_vendor.power.graceful_off_pulse_ms;
    } else if (stage == MODEM_SHUTDOWN_STAGE_UNCONDITIONAL) {
        pulse = MODEM_SHUTDOWN_PULSE_UNCONDITIONAL;
        width_ms = g_modem_vendor.power.emergency_off_pulse_ms;
    } else {
        return MODEM_SHUTDOWN_START_UNSUPPORTED;
    }
    if (width_ms == 0u) {
        return MODEM_SHUTDOWN_START_UNSUPPORTED;
    }
    if (!modem_uart_hal_start_shutdown_pulse(pulse, width_ms)) {
        LOGE("modem", "could not arm shutdown fallback pulse stage=%u",
             (unsigned)stage);
        return MODEM_SHUTDOWN_START_FAILED;
    }

    s_shutdown_stage = stage;
    s_shutdown_pulse_active = true;
    /* add_alarm_in_ms() starts after this coarse millisecond timestamp was
     * sampled. Keep the service sequencing gate one millisecond later so its
     * synchronous cleanup can never beat the hardware alarm and shorten the
     * requested physical pulse through timestamp truncation. */
    s_shutdown_pulse_release_ms = now_ms + width_ms + 1u;
    s_next_action_ms = s_shutdown_pulse_release_ms;
    if (stage == MODEM_SHUTDOWN_STAGE_GRACEFUL_CONTROL) {
        s_diag_graceful_shutdown_pulses++;
        if (s_failed_supply_power) {
            LOGW("modem", "supply fault; bypassing AT and asserting graceful hardware-off for %ums",
                 (unsigned)width_ms);
        } else {
            LOGW("modem", "#SHDN did not complete; asserting graceful hardware-off for %ums",
                 (unsigned)width_ms);
        }
    } else {
        s_diag_emergency_shutdown_pulses++;
        if (s_failed_supply_power) {
            LOGE("modem", "supply fault has no graceful control; asserting unconditional shutdown for %ums",
                 (unsigned)width_ms);
        } else {
            LOGE("modem", "graceful shutdown did not complete; asserting unconditional shutdown for %ums",
                 (unsigned)width_ms);
        }
    }
    return MODEM_SHUTDOWN_START_OK;
}

static void modem_terminal_shutdown_failure(const char *reason,
                                            uint32_t now_ms) {
    modem_uart_hal_cancel_shutdown_pulse();
    s_shutdown_pulse_active = false;
    s_shutdown_pulse_release_ms = 0u;
    s_shutdown_stage = MODEM_SHUTDOWN_STAGE_TERMINAL_FAILURE;
    s_diag_terminal_shutdown_failures++;
    s_power_off_failure_pending = true;
    modem_fail_power_state(reason, true);
    /* This failure is still descended from an explicit authorized shutdown.
     * Keep polling the same vendor qualification so a late, sustained PWRMON
     * fall can release VCC without another unsafe command or blind rail cut. */
    s_failed_shutdown_qualification_pending = true;
    s_next_power_observation_ms = now_ms;
    if (s_power_on_pending) {
        /* A newer ON intent was accepted while shutdown was irreversible. If
         * every OFF stage fails, honor that latest intent by recovering the
         * still-powered module instead of stranding the UI in a queued state.
         * No rail cut occurs and the ordinary PWRMON/RX/CTS startup gates still
         * decide whether the retained module is usable. */
        s_power_off_failure_pending = false;
        modem_recover_retained_module(now_ms);
    }
}

static modem_power_observation_t modem_power_observation(void) {
    modem_power_observation_t observation;
    memset(&observation, 0, sizeof(observation));
    observation.supply_power_good = modem_uart_hal_status_pin();
    observation.ri_asserted = modem_uart_hal_ri_asserted();
    observation.module_status_valid = modem_power_monitor_hal_read(
        &observation.module_status_raw, &observation.module_status_mv);
    return observation;
}

static void modem_fail_power_state(const char *reason,
                                   bool module_may_be_live) {
    s_failed_supply_power = false;
    s_diag_power_failures++;
    s_diag_last_recovery_reason = MODEM_DIAG_RECOVERY_POWER_FAULT;
    s_diag_last_transition_ms = s_now_ms;
    if (s_active_call_token != 0u) {
        call_model_txn_cancel(&s_call_model, s_active_call_token);
        s_active_call_token = 0u;
    }
    model_cancel_deferred_command();
    /* Failure is a call/request session boundary even when vendor safety policy
     * retains the physical rail. Otherwise a later recovery can dispatch work
     * admitted against stale legs or an earlier transport epoch. Serialize the
     * reset with admission, which allocates model tokens under this same lock. */
    cancel_request_session();
    critical_section_enter_blocking(&s_request_lock);
    model_reset_call_session(s_now_ms);
    critical_section_exit(&s_request_lock);
    critical_section_enter_blocking(&s_status_lock);
    s_status.provisioning_verified = false;
    s_status.network_registered = false;
    s_status.cereg = 0xffu;
    signal_invalidate_locked(s_now_ms);
    sim_provider_reset();
    modem_supplementary_invalidate_cache();
    supplementary_project_status_locked();
    critical_section_exit(&s_status_lock);
    modem_supplementary_reset_transient();
    s_active = false;
    s_active_kind = MODEM_AT_NONE;
    s_operation = MODEM_OP_NONE;
    maintenance_reset_session();
    s_binary_restore_pending = false;
    set_operation_busy(false);
    set_status_ready(false);
    s_state = MODEM_STATE_FAILED;
    s_failed_module_may_be_live = module_may_be_live;
    s_failed_shutdown_qualification_pending = false;
    modem_uart_hal_cancel_shutdown_pulse();
    s_shutdown_pulse_active = false;
    s_shutdown_pulse_release_ms = 0u;
    if (s_shutdown_stage != MODEM_SHUTDOWN_STAGE_TERMINAL_FAILURE) {
        s_shutdown_stage = MODEM_SHUTDOWN_STAGE_NONE;
    }
    modem_uart_hal_set_power_pin(false);
    modem_uart_hal_set_dtr_sleep_permitted(false);
    s_dtr_sleep_permitted = false;
    s_dtr_wake_pending = false;
    s_ri_release_pending = false;
    s_ri_release_deadline_ms = 0u;
    sms_wake_reset_session();
    s_provision_reboot_cycle_active = false;
    s_provision_reboot_drop_seen = false;
    s_power_off_pending = false;
    s_sim_completion_needed = false;
    s_sim_completion_pending = false;
    s_sim_completion_active = false;
    s_sim_completion_failed = false;
    s_sim_steps_skipped = false;

    if (!module_may_be_live) {
        if (!s_uart_parked) {
            modem_uart_hal_park();
            s_uart_parked = true;
        }
        shared_3v8_service_set_required(SHARED_3V8_OWNER_MODEM, false);
    }
    LOGE("modem", "%s", reason);
}

static void modem_fail_supply_power_state(const char *reason,
                                          bool module_may_be_live) {
    /* Keep the physical PG fact separate from modem failure classification. The
     * app may call this a depleted pack only when its independent LTC/charger
     * evidence agrees; a healthy-voltage regulator or board fault must remain a
     * modem fault. The latch survives FAILED long enough for a settling LTC
     * sample, but a new power epoch below clears it. */
    modem_fail_power_state(reason, module_may_be_live);
    s_failed_supply_power = true;
    s_supply_power_failure_pending = true;
}

static void modem_start_power_pulse(uint32_t now_ms) {
    if (s_power_pulsed) {
        return;
    }
    s_power_pulsed = true;
    s_state = MODEM_STATE_POWER_PULSE;
    s_power_release_ms = now_ms + g_modem_vendor.power.pwron_pulse_ms;
    modem_uart_hal_set_power_pin(true);
    LOGI("modem", "PWR pulse start");
}

static void modem_enter_module_wait(uint32_t now_ms) {
    s_state = MODEM_STATE_MODULE_WAIT;
    s_failed_shutdown_qualification_pending = false;
    modem_reset_shutdown_escalation();
    /* A sensed backend must distinguish three states at the startup deadline:
     * confirmed ON, vendor-qualified OFF, and indeterminate. Reset the
     * qualified-OFF timer on every entry so only observations from this exact
     * wait episode can authorize a rail release or recycle. */
    if (g_modem_vendor.power_is_on != NULL) {
        g_modem_vendor.off_sense_begin(now_ms);
    }
}

static void modem_begin_power_on(uint32_t now_ms) {
    /* Keep the rail inert even if a future internal caller bypasses the public
     * service/tick gates while the Rev B2 NONE backend is selected. */
    if (!g_modem_vendor.available) {
        return;
    }
    if (s_state == MODEM_STATE_OFF_DISCHARGE) {
        /* Mid-shutdown power-on (user flipped their mind): finish the graceful
         * switch-off first, then restart from a clean rail cycle. */
        s_power_on_pending = true;
        return;
    }
    if (s_state != MODEM_STATE_OFF && s_state != MODEM_STATE_FAILED) {
        return;
    }
    s_diag_power_on_starts++;
    s_startup_complete = false;
    s_diag_last_transition_ms = now_ms;
    s_failed_supply_power = false;
    s_supply_power_failure_pending = false;
    modem_reset_shutdown_escalation();
    s_power_off_failure_pending = false;
    /* Every power epoch owns a fresh runtime-monitor deadline. Retaining a
     * prior epoch's value across a long OFF interval can invert the signed
     * wrap comparison and suppress PG/PWRMON checks for hours or days. */
    s_next_power_observation_ms = now_ms;
    if (s_uart_parked && g_modem_vendor.power_is_on == NULL) {
        /* A modem-free test backend has no authoritative module-on signal. The
         * Telit backend keeps UART parked until PWRMON is true and RX reaches
         * idle-high; enabling PL011 earlier would interpret the pull-down as a
         * continuous break and service it as an interrupt storm. */
        modem_uart_hal_init();
        s_uart_parked = false;
    }
    modem_uart_hal_set_dtr_sleep_permitted(false);
    s_dtr_sleep_permitted = false;
    s_dtr_wake_pending = false;
    s_ri_release_pending = false;
    s_ri_release_deadline_ms = 0u;
    sms_wake_reset_session();
    s_power_off_pending = false;
    s_failed_module_may_be_live = false;
    s_failed_shutdown_qualification_pending = false;
    /* Acquire the modem's share of +3V8, then hold off the first probe/PWR pulse
     * for the bulk-cap charge time (non-blocking -- the state machine just waits).
     * This does not disturb an overlapping buzzer owner. */
    shared_3v8_service_set_required(SHARED_3V8_OWNER_MODEM, true);
    s_rail_off_ready_ms = 0u;
    s_rail_off_dwell_started = false;
    s_rail_off_dwell_complete = false;
    s_rail_off_transition_snapshot = shared_3v8_service_transition_count();
    s_active = false;
    s_active_kind = MODEM_AT_NONE;
    s_operation = MODEM_OP_NONE;
    s_power_pulsed = false;
    s_state = g_modem_vendor.power.rail_power_good_timeout_ms != 0u
        ? MODEM_STATE_RAIL_WAIT
        : MODEM_STATE_PROBE;
    s_boot_deadline_ms = now_ms + g_modem_vendor.power.rail_settle_ms + g_modem_vendor.power.ready_budget_ms;
    s_rail_power_good_deadline_ms = now_ms +
        g_modem_vendor.power.rail_power_good_timeout_ms;
    s_next_action_ms = now_ms + g_modem_vendor.power.rail_settle_ms;
    s_now_ms = now_ms;
    LOGI("modem", "3V8 rail on; startup gate in %ums",
         (unsigned)g_modem_vendor.power.rail_settle_ms);
}

static void modem_begin_power_off(uint32_t now_ms) {
    s_now_ms = now_ms;
    s_power_on_pending = false; /* latest intent wins: cancel a queued restart */
    s_power_off_failure_pending = false;
    if (s_power_off_pending) {
        return; /* already waiting for the startup AT channel */
    }
    if (s_state == MODEM_STATE_OFF_DISCHARGE) {
        return; /* already on the way down */
    }
    if (s_active && s_active_kind == MODEM_AT_POWER_OFF) {
        /* The desired shutdown is already on wire. In particular, do not
         * clear/reissue it after an intervening ON intent changed its mind. */
        return;
    }
    if (s_state == MODEM_STATE_OFF) {
        modem_enter_off();
        return;
    }
    cancel_request_session();
    if (s_active && (s_active_kind == MODEM_AT_SIM_PROVIDER ||
                     s_active_kind == MODEM_AT_SIM_PROVIDER_DRAIN)) {
        s_power_off_pending = true;
        LOGI("modem", "power-off waiting for the non-abortable SIM read");
        return;
    }
    if (s_active) {
        if (s_active_kind == MODEM_AT_SMS_CMGS_PROMPT || s_active_kind == MODEM_AT_SMS_CMGW_PROMPT) {
            /* An SMS '>' body prompt is open: cancel it with ESC BEFORE we send
             * AT+CPWROFF, else the module eats "AT+CPWROFF" as message body and
             * the graceful power-off never happens -- it degrades to the hard 3V8
             * cut on a live module. Same abort the timeout path uses; harmless if
             * the module already left prompt mode (ESC on a command line is
             * ignored). The off_discharge break-sense + timeout backstop absorb any
             * OK the ESC itself answers. */
            uint8_t esc = MODEM_SMS_ESC_CHAR;
            modem_uart_hal_write(&esc, 1u);
        }
        if (s_active_call_token != 0u) {
            call_model_txn_cancel(&s_call_model, s_active_call_token);
            s_active_call_token = 0u;
        }
        s_active = false;
        s_active_kind = MODEM_AT_NONE;
    }
    /* A wake poke may have a periodic poll stashed: power-off supersedes it
     * (idempotent polls only; modem_enter_off would clear it later anyway). */
    model_cancel_deferred_command();
    /* Cancel a deferred binary-SMS text-mode restore too: if we leave READY for
     * the power-off path with it still armed, advance_state would otherwise fire
     * AT+CMGF=1 on the first OFF_DISCHARGE tick -- stalling the break-sense poll
     * and talking to a module that is already shutting down. */
    s_binary_restore_pending = false;
    s_operation = MODEM_OP_NONE;
    set_operation_busy(false);
    modem_power_observation_t observation = modem_power_observation();
    bool module_on = g_modem_vendor.power_is_on != NULL &&
        g_modem_vendor.power_is_on(&observation);
    if (s_state == MODEM_STATE_FAILED && s_failed_supply_power &&
        s_failed_module_may_be_live) {
        modem_start_supply_fault_shutdown(now_ms);
        return;
    }
    if (s_state == MODEM_STATE_POWER_PULSE ||
        s_state == MODEM_STATE_MODULE_WAIT ||
        (s_state == MODEM_STATE_RAIL_WAIT && module_on)) {
        /* Once the vendor start pulse has begun, neither low nor high early
         * PWRMON proves an AT channel is usable. Finish the bounded startup
         * probe, then issue the normal shutdown command after its first parsed
         * response. An already-on module discovered in RAIL_WAIT follows the
         * same path; an unpulsed, qualified prior-OFF rail may stop directly. */
        s_power_off_pending = true;
        LOGI("modem", "power-off deferred until startup AT channel is ready");
        return;
    }
    if (s_state == MODEM_STATE_FAILED && s_failed_module_may_be_live &&
        s_uart_parked) {
        /* OFF_DISCHARGE faults deliberately leave PL011 parked while retaining
         * VCC. Re-establish the transport through the same status/RX/CTS gates;
         * never send a shutdown command through a deinitialized UART. */
        s_power_off_pending = true;
        s_failed_module_may_be_live = false;
        s_power_pulsed = true;
        s_boot_deadline_ms = now_ms + g_modem_vendor.power.ready_budget_ms;
        s_next_action_ms = now_ms;
        modem_enter_module_wait(now_ms);
        LOGI("modem", "power-off waiting to recover retained module transport");
        return;
    }
    if (s_state == MODEM_STATE_READY || s_state == MODEM_STATE_INIT ||
        s_state == MODEM_STATE_PROVISION || s_state == MODEM_STATE_PROBE ||
        (s_state == MODEM_STATE_FAILED && s_failed_module_may_be_live) ||
        module_on) {
        send_command(MODEM_AT_POWER_OFF, g_modem_vendor.power.off_cmd,
                     g_modem_vendor.power.off_ack_timeout_ms, now_ms);
        LOGI("modem", "power-off command sent");
    } else {
        modem_enter_off();
    }
}

#if defined(SISU_MODEM_SERVICE_TEST)
static modem_service_test_state_t modem_service_test_state(
    modem_state_t state) {
    switch (state) {
        case MODEM_STATE_PROBE:
            return MODEM_SERVICE_TEST_STATE_PROBE;
        case MODEM_STATE_OFF:
            return MODEM_SERVICE_TEST_STATE_OFF;
        case MODEM_STATE_OFF_DISCHARGE:
            return MODEM_SERVICE_TEST_STATE_OFF_DISCHARGE;
        case MODEM_STATE_RAIL_WAIT:
            return MODEM_SERVICE_TEST_STATE_RAIL_WAIT;
        case MODEM_STATE_POWER_PULSE:
            return MODEM_SERVICE_TEST_STATE_POWER_PULSE;
        case MODEM_STATE_MODULE_WAIT:
            return MODEM_SERVICE_TEST_STATE_MODULE_WAIT;
        case MODEM_STATE_INIT:
            return MODEM_SERVICE_TEST_STATE_INIT;
        case MODEM_STATE_PROVISION:
            return MODEM_SERVICE_TEST_STATE_PROVISION;
        case MODEM_STATE_READY:
            return MODEM_SERVICE_TEST_STATE_READY;
        case MODEM_STATE_FAILED:
        default:
            return MODEM_SERVICE_TEST_STATE_FAILED;
    }
}

static modem_service_test_command_t modem_service_test_command_kind(
    modem_at_kind_t kind) {
    switch (kind) {
        case MODEM_AT_NONE:
            return MODEM_SERVICE_TEST_COMMAND_NONE;
        case MODEM_AT_SMS_CMGD:
            return MODEM_SERVICE_TEST_COMMAND_SMS_DELETE;
        case MODEM_AT_DIAG_QUERY:
            return MODEM_SERVICE_TEST_COMMAND_DIAG_QUERY;
        case MODEM_AT_MAINTENANCE:
            return MODEM_SERVICE_TEST_COMMAND_MAINTENANCE;
        case MODEM_AT_CALL_FORWARD:
            return MODEM_SERVICE_TEST_COMMAND_CALL_FORWARD;
        default:
            return MODEM_SERVICE_TEST_COMMAND_OTHER;
    }
}

static modem_service_test_sim_observation_t
modem_service_test_sim_observation(modem_sim_observation_t observation) {
    switch (observation) {
        case MODEM_SIM_OBSERVATION_ABSENT:
            return MODEM_SERVICE_TEST_SIM_ABSENT;
        case MODEM_SIM_OBSERVATION_PRESENT:
            return MODEM_SERVICE_TEST_SIM_PRESENT;
        case MODEM_SIM_OBSERVATION_READY:
            return MODEM_SERVICE_TEST_SIM_READY;
        case MODEM_SIM_OBSERVATION_NONE:
        default:
            return MODEM_SERVICE_TEST_SIM_NONE;
    }
}

void modem_service_test_get_snapshot(modem_service_test_snapshot_t *out) {
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->state = modem_service_test_state(s_state);
    out->active_command = modem_service_test_command_kind(s_active_kind);
    out->deferred_command = modem_service_test_command_kind(s_deferred_kind);
    out->maintenance_sim_deferred =
        modem_service_test_sim_observation(s_maintenance_sim_deferred);
    out->command_active = s_active;
    out->deferred_command_valid = s_deferred_valid;
    out->uart_parked = s_uart_parked;
    out->failed_module_may_be_live = s_failed_module_may_be_live;
    out->power_on_pending = s_power_on_pending;
    out->power_off_pending = s_power_off_pending;
    out->bridge_active_wanted = s_bridge_active_wanted;
    out->sms_wake_armed = s_sms_wake_armed;
    out->sms_mode_restore_pending = s_sms_mode_restore_pending;
    out->provision_reboot_cycle_active = s_provision_reboot_cycle_active;
    out->provision_reboot_drop_seen = s_provision_reboot_drop_seen;
    out->sim_completion_needed = s_sim_completion_needed;
    out->sim_completion_pending = s_sim_completion_pending;
    out->sim_completion_active = s_sim_completion_active;
    out->sms_wake_activation_deadline_ms =
        s_sms_wake_activation_deadline_ms;
    out->maintenance_sim_guard_deadline_ms =
        s_maintenance_sim_guard_deadline_ms;
    out->next_power_observation_ms = s_next_power_observation_ms;
    out->transport_integrity_counter = model_transport_counter();
    out->rail_off_dwell_ms = MODEM_RAIL_OFF_DWELL_MS;
    out->rail_off_poll_ms = MODEM_RAIL_OFF_POLL_MS;
    out->rx_tick_budget = MODEM_RX_TICK_BUDGET;
    out->signal_active_ms = MODEM_SIGNAL_ACTIVE_MS;
    out->sim_provider_retry_ms = MODEM_SIM_PROVIDER_RETRY_MS;

    critical_section_enter_blocking(&s_request_lock);
    out->request_queue_depth = s_request_count;
    critical_section_exit(&s_request_lock);

    critical_section_enter_blocking(&s_status_lock);
    out->pending_mt_active = s_call_model.pending_mt.active;
    out->pending_mt_alert_observed =
        s_call_model.pending_mt.alert_observed;
    out->pending_mt_incoming_diverted =
        s_call_model.pending_mt.incoming_diverted;
    for (uint8_t i = 0u; i < MODEM_MAX_CALL_TRANSACTIONS; i++) {
        if (s_call_model.txns[i].in_use) {
            out->call_transaction_count++;
        }
    }
    critical_section_exit(&s_status_lock);
}

void modem_service_test_set_call_session_active(bool active) {
    model_reset_call_session(s_now_ms);
    if (active) {
        call_model_on_event(&s_call_model, 1u, true,
                            CALL_LEG_ACTIVE, CALL_DIR_MO);
    }
    model_project_publish();
}

void modem_service_test_apply_bridge_inputs(modem_call_state_t state,
                                            bool on_hold,
                                            bool audio_init_ok) {
    critical_section_enter_blocking(&s_status_lock);
    s_call_projection.call_state = state;
    s_call_projection.call_on_hold = on_hold;
    s_status.audio_init_ok = audio_init_ok;
    critical_section_exit(&s_status_lock);
    modem_bridge_follow_call_state();
}
#endif

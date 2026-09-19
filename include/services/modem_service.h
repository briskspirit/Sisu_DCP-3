#ifndef MODEM_SERVICE_H
#define MODEM_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "services/call_types.h"
#include "services/call_forward_types.h"
#include "services/modem_diag.h"
#include "services/modem_maintenance.h"
#include "services/modem_signal.h"
#include "services/sms_types.h"

/* EF-SPN has up to 16 characters; allow their UTF-8 representation. */
#define MODEM_OPERATOR_NAME_CAPACITY 49u

typedef enum {
    MODEM_OPERATOR_NAME_NONE = 0,
    MODEM_OPERATOR_NAME_DATABASE,
    MODEM_OPERATOR_NAME_SIM,
    MODEM_OPERATOR_NAME_PLMN,
} modem_operator_name_source_t;

typedef struct {
    bool available;
    bool at_ready;
    /* SIM discovery is independent of full AT initialization/registration.
     * checked=false means no trustworthy SIM observation has arrived yet;
     * present distinguishes a physically/electrically absent card from a
     * present card that is still PIN-locked or otherwise not ready. */
    bool sim_checked;
    bool sim_present;
    bool sim_ready;
    /* Nonzero only for a backend with a versioned provisioning schema.
     * verified means every applicable row completed query/compare/verify in
     * this boot; the stored value is diagnostic and never skips readback. */
    bool provisioning_verified;
    uint16_t provisioning_schema_version;
    /* False if a recoverable init step was skipped after repeated failure, so the
     * modem reports READY while a capability is silently degraded: audio = the
     * in-call I2S voice path (AT+UI2S/+USPM), sms = SMS submit params/storage
     * (AT+CSMP/+CPMS). Lets the UI/diagnostics show why voice/SMS may not work. */
    bool audio_init_ok;
    bool sms_init_ok;
    bool network_registered;
    bool ring_active;
    bool waiting_call;
    /* The sole active call is on NETWORK hold: uplink is muted by the
     * network and the UI shows "On hold". Driven by ID-scoped HELD/ACTIVE
     * events so it reflects the real network state, not an optimistic UI
     * guess. Distinct from the two-call waiting/held-leg state
     * (waiting_call + the app swap model). */
    bool call_on_hold;
    /* A two-call HELD leg exists on the network (id tracked internally). Lets the
     * app reconcile its optimistic two-call model: when this goes false while the
     * app still shows a held second call, the held leg was released -> drop the
     * phantom instead of letting a later Swap/End act on a dead leg. */
    bool second_call_held;
    /* Current active call id (0 = none). Lets the app tell a HELD-leg-dropped
     * (active id unchanged) from an ACTIVE-leg-released-and-held-promoted (active
     * id changed) when second_call_held goes false, so it swaps to the surviving
     * party instead of dropping it. */
    uint8_t active_call_id;
    /* +CLIP CLI-validity field != 0 (withheld/unavailable) -> "Private number". */
    bool caller_id_withheld;
    /* Network indicated the incoming call was diverted to us -> "Diverted call". */
    bool incoming_diverted;
    /* Network-owned supplementary-service indicators. CFU is tri-state so a
     * missing/failed status query cannot masquerade as a disabled divert. */
    bool call_forward_unconditional_known;
    bool call_forward_unconditional_active;
    modem_message_waiting_status_t message_waiting;
    bool operation_busy;
    uint8_t rssi;
    uint8_t ber;
    uint8_t cereg;
    modem_signal_sample_t signal;
    modem_call_state_t call_state;
    modem_call_result_t last_call_result;
    /* Latched outcome of a failed 2nd MO (New-call) leg, kept SEPARATE from
     * last_call_result because retrieving the held original leg emits an ID-scoped
     * ACTIVE event that overwrites last_call_result with CONNECTED. The app's
     * New-call gate reads this to revert to the surviving single call. NONE unless a
     * New-call's 2nd leg just failed; reset at the next dial / when it connects. */
    modem_call_result_t second_call_result;
    char operator_name[MODEM_OPERATOR_NAME_CAPACITY];
    modem_operator_name_source_t operator_name_source;
    char incoming_number[MODEM_PHONE_MAX + 1u];
    uint32_t rx_bytes;
    /* UART health (from modem_uart_hal): bytes dropped on a full receive ring,
     * PL011 hardware FIFO overruns, framing/parity/break-marked words, and bytes
     * dropped when a TX write hit the 100 ms CTS-flow-control stall bound. */
    uint32_t rx_overruns;
    uint32_t rx_dropped;
    uint32_t rx_line_errors;
    uint32_t tx_stall_drops;
    uint32_t urc_count;
    uint32_t command_errors;
    uint32_t sms_received_count; /* normalized incoming transport segments */
    uint32_t sms_recovered;
    uint32_t sms_recovery_errors;
    uint8_t sms_recovery_step;
    uint32_t sms_sent_count;
    uint32_t sms_filtered_type0;
    uint32_t sms_filtered_vvm;
    uint32_t sms_filtered_oma_dm;
    uint32_t picture_parts_received;
    uint32_t picture_receive_errors;
    uint32_t last_update_ms;
    uint8_t debug_state;
    uint8_t debug_active_kind;
    uint8_t debug_init_index;
    uint8_t debug_init_retries;
    char debug_last_command[48];
    char debug_last_line[48];
} modem_status_t;

/* Generation-qualified live call-table view for identity-sensitive UI
 * transitions. This is deliberately neutral: raw vendor states stop at the
 * adapter, and callers cannot mutate model ownership. Flat modem_status_t
 * remains the rendering compatibility view; this snapshot answers only which
 * exact legs still exist when a role changes between polls. */
typedef struct {
    uint8_t id;
    uint8_t generation;
    call_leg_state_t state;
    call_direction_t direction;
    bool pending_removal;
    uint32_t incoming_episode;
} modem_call_leg_snapshot_t;

typedef struct {
    uint32_t semantic_revision;
    uint32_t pending_incoming_episode;
    uint8_t leg_count;
    bool pending_incoming_active;
    bool pending_incoming_alert_observed;
    bool overflow;
    modem_call_leg_snapshot_t legs[MODEM_CALL_ID_MAX];
} modem_call_snapshot_t;

typedef struct {
    bool pending;
    bool ok;
} modem_debug_result_t;

void modem_service_init(void);
void modem_service_tick(uint32_t now_ms);
void modem_service_power_on(void);
void modem_service_power_off(void);
/* True once the modem is fully OFF: CPWROFF/discharge complete and its shared
 * +3V8 ownership released -- not merely "power-off requested" (the bounded
 * command/hardware fallback sequence can exceed 90 s under a fault). The
 * physical rail can remain up for the buzzer; the dormant-sleep gate checks
 * shared ownership separately. */
bool modem_service_is_powered_off(void);
/* Consume the one-shot indication that every configured safe shutdown stage
 * was exhausted while PWRMON still failed to qualify OFF. The rail remains
 * owned; the app uses this to make the fail-closed state visible. */
bool modem_service_take_power_off_failure(void);
/* Consume one modem-supply PG failure from the current power epoch. This is a
 * vendor-neutral physical-supply observation, not by itself a battery verdict:
 * the battery owner must combine it with fresh terminal/charger evidence. Core
 * 0 only. A fresh modem power-on epoch clears an unconsumed stale indication. */
bool modem_service_take_supply_power_failure(void);
/* Backend capability, independent of current registration/call state. False
 * for an unavailable backend, keeping modem-side voice PIO/DMA unclaimed. */
bool modem_service_voice_transport_available(void);
/* True only at a fully quiescent DTR/CTS boundary suitable for stopping all RP
 * clocks. This is stricter than READY: no AT, call, diagnostic, queue, recovery,
 * UART byte, or unresolved maintenance work may remain. */
bool modem_service_transport_sleep_confirmed(void);
/* True during modem rail/startup/shutdown transitions and non-idle call work.
 * This is a vendor-neutral battery-sampling hint, not a sleep veto or timer. */
bool modem_service_battery_high_load_active(void);
void modem_service_get_status(modem_status_t *out);
void modem_service_get_call_snapshot(modem_call_snapshot_t *out);
/* Enables the 1 Hz serving-signal cadence while the display is active. The
 * idle path retains only event-driven refreshes and the existing slow
 * backstop, so this does not add a second periodic modem wake source. */
void modem_service_set_signal_sampling(bool display_active, uint32_t now_ms);
/* Headset inserted/removed. If a call is up, re-route the codec live (audio +
 * mic follow the accessory mid-call, matching the original's accessory
 * subsystem); otherwise a no-op -- call start picks the route itself. Core 0
 * only (codec I2C). */
void modem_service_accessory_changed(bool headset_inserted);
bool modem_service_request_dial(const char *number);
/* The app has GIVEN UP on a New-call (a 2nd outgoing call dialled while already in a
 * call) and reverted its UI to the original call. The service authoritatively RESOLVES
 * that abandoned 2nd leg. If it is still tracked, it queues a generation-qualified
 * cleanup which releases the foreground setup and recovers the held survivor; the
 * vendor adapter owns the wire command. A delayed connect therefore cannot resurrect
 * the cancelled leg under a single-call UI. Idempotent once the leg has resolved. */
void modem_service_new_call_abandoned(void);
bool modem_service_request_answer(void);
bool modem_service_request_hangup(void);
#define MODEM_DTMF_SEQUENCE_MAX 32u
bool modem_service_request_dtmf(char symbol);
/* Atomically admit one in-call DTMF string. The service owns sequencing: each
 * digit is sent only after the previous command succeeds, and any command
 * failure, call loss, recovery, or higher-priority call control retires the
 * unsent suffix. Empty, invalid, or overlength strings are rejected. */
bool modem_service_request_dtmf_sequence(const char *symbols);
bool modem_service_request_call_waiting_answer(void);
bool modem_service_request_call_waiting_reject(void);
bool modem_service_request_call_swap(void);
/* True only when the selected backend can safely toggle NETWORK hold on one
 * sole confirmed leg. False while another held/waiting/ringing leg exists. */
bool modem_service_call_hold_available(void);
/* Toggle NETWORK hold on the sole active/held call. Admission and the final
 * UART dispatch both recheck that no second/waiting leg exists. The UI's "On
 * hold" state follows normalized modem evidence, not this request. */
bool modem_service_request_call_hold(void);
bool modem_service_request_call_release_active(void);
/* True while a locally requested release-active operation still owns the
 * network's held-survivor promotion. This is operation authority, not a vendor
 * command detail. */
bool modem_service_call_release_active_pending(void);
/* Release exactly one confirmed call-table leg. Unlike release-active, this
 * does not accept/promote another leg as part of the operation. */
bool modem_service_request_call_release_leg(uint8_t call_id);
/* True after abandoning a second outgoing setup until that setup leg is
 * authoritatively confirmed gone. A command OK is not completion. */
bool modem_service_new_call_cleanup_pending(void);
bool modem_service_request_call_forward(const call_forward_request_t *request,
                                        uint32_t *request_id_out);
/* Cancel only a call-forward request which has not crossed the queue-to-wire
 * boundary. An in-flight network request is intentionally left alone and its
 * eventual result remains request-id scoped. */
bool modem_service_cancel_queued_call_forward(uint32_t request_id);
bool modem_service_pop_call_forward_result(call_forward_result_t *out);
/* SIM-provided number discovered through the selected backend. Manual phone
 * settings remain an app-level override and are intentionally not mixed into
 * this cache. */
bool modem_service_get_voice_mailbox_number(char *out, size_t out_cap);
/* SMS result channels are single-flight. Successful admission returns a
 * nonzero request id and guarantees one matching terminal; rejection leaves
 * the output at zero and publishes nothing. */
bool modem_service_request_send_sms(const char *number, const char *text,
                                    uint32_t *request_id_out);
bool modem_service_request_send_binary_sms(const char *number,
                                           const uint8_t *payload,
                                           uint16_t payload_len,
                                           uint16_t dest_port,
                                           uint16_t source_port,
                                           uint32_t *request_id_out);
bool modem_service_request_send_binary_sms_mode(const char *number,
                                                const uint8_t *payload,
                                                uint16_t payload_len,
                                                uint16_t dest_port,
                                                uint16_t source_port,
                                                modem_binary_sms_mode_t mode,
                                                uint32_t *request_id_out);
bool modem_service_request_debug_at(const char *command);
/* RAM-only bench control. Disabling suppresses periodic radio
 * backstops, but never event-driven work, RI wake, calls, or SMS handling. */
bool modem_service_request_debug_background_polling(bool enabled);
/* Opt-in RAM-only capture before AT line framing. Reading requires a stopped
 * capture; bytes are returned oldest first, including NUL and non-ASCII. */
#define MODEM_RX_TRACE_CAPACITY 4096u
typedef struct {
    bool enabled;
    uint32_t received;
    uint16_t retained;
} modem_rx_trace_status_t;
bool modem_service_rx_trace_start(void);
void modem_service_rx_trace_stop(void);
void modem_service_rx_trace_status(modem_rx_trace_status_t *out);
size_t modem_service_rx_trace_read(size_t offset, uint8_t *out, size_t capacity);
/* Guarded Net Monitor maintenance. These APIs are semantic: modem command
 * syntax and readback grammar remain owned by the selected vendor adapter.
 * Every mutation is verified, and cancel converges toward production state. */
bool modem_service_maintenance_supported(void);
bool modem_service_maintenance_request_scan_timer(uint16_t seconds);
bool modem_service_maintenance_read_scan_timer(void);
bool modem_service_maintenance_start_band_test(uint8_t preset);
bool modem_service_maintenance_restore_band(void);
bool modem_service_maintenance_enter_antenna(void);
bool modem_service_maintenance_select_antenna(uint8_t rf_state);
bool modem_service_maintenance_exit_antenna(void);
void modem_service_maintenance_cancel(void);
void modem_service_get_maintenance_snapshot(
    modem_maintenance_snapshot_t *out);
/* RevB2/Telit Net Monitor v2 diagnostics. Selecting a group replaces the
 * previous subscription generation; duplicate requests coalesce while queued
 * or active. The service yields after every AT command and never polls while a
 * call is present. GROUP_NONE cancels the subscription. */
bool modem_service_diag_select(modem_diag_group_t group, uint32_t generation,
                               bool request_now);
void modem_service_diag_cancel(uint32_t generation);
void modem_service_get_diag_snapshot(modem_diag_snapshot_t *out);
/* Terminals are consumed only by their exact request token. */
bool modem_service_pop_sms_send_result(uint32_t request_id,
                                       modem_sms_send_result_t *out);
bool modem_service_pop_debug_result(modem_debug_result_t *out);

#endif

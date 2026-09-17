#ifndef MODEM_SERVICE_TEST_H
#define MODEM_SERVICE_TEST_H

#if !defined(SISU_MODEM_SERVICE_TEST)
#error "modem_service_test.h is available only to the host service harness"
#endif

#include <stdbool.h>
#include <stdint.h>

#include "services/call_types.h"

typedef enum {
    MODEM_SERVICE_TEST_STATE_PROBE = 0,
    MODEM_SERVICE_TEST_STATE_OFF,
    MODEM_SERVICE_TEST_STATE_OFF_DISCHARGE,
    MODEM_SERVICE_TEST_STATE_RAIL_WAIT,
    MODEM_SERVICE_TEST_STATE_POWER_PULSE,
    MODEM_SERVICE_TEST_STATE_MODULE_WAIT,
    MODEM_SERVICE_TEST_STATE_INIT,
    MODEM_SERVICE_TEST_STATE_PROVISION,
    MODEM_SERVICE_TEST_STATE_READY,
    MODEM_SERVICE_TEST_STATE_FAILED,
} modem_service_test_state_t;

typedef enum {
    MODEM_SERVICE_TEST_COMMAND_NONE = 0,
    MODEM_SERVICE_TEST_COMMAND_OTHER,
    MODEM_SERVICE_TEST_COMMAND_SMS_DELETE,
    MODEM_SERVICE_TEST_COMMAND_DIAG_QUERY,
    MODEM_SERVICE_TEST_COMMAND_MAINTENANCE,
    MODEM_SERVICE_TEST_COMMAND_CALL_FORWARD,
} modem_service_test_command_t;

typedef enum {
    MODEM_SERVICE_TEST_SIM_NONE = 0,
    MODEM_SERVICE_TEST_SIM_ABSENT,
    MODEM_SERVICE_TEST_SIM_PRESENT,
    MODEM_SERVICE_TEST_SIM_READY,
} modem_service_test_sim_observation_t;

typedef struct {
    modem_service_test_state_t state;
    modem_service_test_command_t active_command;
    modem_service_test_command_t deferred_command;
    modem_service_test_sim_observation_t maintenance_sim_deferred;
    bool command_active;
    bool deferred_command_valid;
    bool uart_parked;
    bool failed_module_may_be_live;
    bool power_on_pending;
    bool power_off_pending;
    bool bridge_active_wanted;
    bool sms_wake_armed;
    bool sms_mode_restore_pending;
    bool provision_reboot_cycle_active;
    bool provision_reboot_drop_seen;
    bool sim_completion_needed;
    bool sim_completion_pending;
    bool sim_completion_active;
    bool pending_mt_active;
    bool pending_mt_alert_observed;
    bool pending_mt_incoming_diverted;
    uint8_t request_queue_depth;
    uint8_t call_transaction_count;
    uint32_t sms_wake_activation_deadline_ms;
    uint32_t maintenance_sim_guard_deadline_ms;
    uint32_t next_power_observation_ms;
    uint32_t transport_integrity_counter;
    uint32_t rail_off_dwell_ms;
    uint32_t rail_off_poll_ms;
    uint32_t rx_tick_budget;
    uint32_t signal_active_ms;
    uint32_t sim_provider_retry_ms;
} modem_service_test_snapshot_t;

/* Host-only visibility for invariants that intentionally do not belong to the
 * runtime diagnostic ABI. Firmware builds neither declare nor emit this seam. */
void modem_service_test_get_snapshot(modem_service_test_snapshot_t *out);

/* Synthetic controls are limited to tests whose subject is the bridge/SIM
 * gate itself. Session activity is still constructed through the call model. */
void modem_service_test_set_call_session_active(bool active);
void modem_service_test_apply_bridge_inputs(modem_call_state_t state,
                                            bool on_hold,
                                            bool audio_init_ok);

#endif

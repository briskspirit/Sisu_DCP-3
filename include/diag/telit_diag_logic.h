#ifndef TELIT_DIAG_LOGIC_H
#define TELIT_DIAG_LOGIC_H

#include <stdbool.h>
#include <stdint.h>

/* Characterization-only safety bounds from the LE910Cx hardware guide.
 * They are not production timing choices; bench evidence will select those. */
#define TELIT_DIAG_BOOT_PULSE_MIN_MS 1000u
#define TELIT_DIAG_BOOT_PULSE_MAX_MS 2000u
#define TELIT_DIAG_HW_OFF_PULSE_MIN_MS 2500u
#define TELIT_DIAG_HW_OFF_PULSE_MAX_MS 5000u
#define TELIT_DIAG_EMERGENCY_PULSE_MS 250u
#define TELIT_DIAG_EMERGENCY_ARM_MS 5000u
#define TELIT_DIAG_OFF_STABLE_MS 500u
#define TELIT_DIAG_STATUS_SAMPLE_MAX_GAP_MS 50u

/* An off threshold is operator-supplied from measured GP40 data. Restrict it
 * to the lower ADC quarter so an on-level cannot accidentally be accepted as
 * proof that PWRMON has fallen. This is a guardrail, not a calibrated value. */
#define TELIT_DIAG_OFF_THRESHOLD_MAX_RAW 1024u

typedef enum {
    TELIT_DIAG_PULSE_NONE = 0,
    TELIT_DIAG_PULSE_BOOT,
    TELIT_DIAG_PULSE_HW_OFF,
    TELIT_DIAG_PULSE_EMERGENCY,
} telit_diag_pulse_t;

typedef enum {
    TELIT_DIAG_OK = 0,
    TELIT_DIAG_ERR_STATE,
    TELIT_DIAG_ERR_RANGE,
    TELIT_DIAG_ERR_NEEDS_PG,
    TELIT_DIAG_ERR_NEEDS_STATUS,
    TELIT_DIAG_ERR_NEEDS_THRESHOLD,
    TELIT_DIAG_ERR_STATUS_HIGH,
    TELIT_DIAG_ERR_STATUS_UNSTABLE,
    TELIT_DIAG_ERR_NOT_ARMED,
} telit_diag_result_t;

typedef struct {
    bool power_good;
    bool status_valid;
    bool status_fresh;
    uint16_t status_raw;
} telit_diag_observation_t;

typedef struct {
    bool rail_enabled;
    bool force_pwm;
    bool dtr_sleep_permitted;
    bool boot_attempted;
    bool off_threshold_set;
    uint16_t off_threshold_raw;
    bool status_low_tracking;
    bool status_low_stable;
    uint32_t status_low_since_ms;
    uint32_t status_last_sample_ms;
    telit_diag_pulse_t pulse;
    uint32_t pulse_width_ms;
    uint32_t pulse_deadline_ms;
    uint32_t pulse_sequence;
    bool emergency_armed;
    uint32_t emergency_arm_deadline_ms;
} telit_diag_logic_t;

void telit_diag_logic_init(telit_diag_logic_t *logic);
void telit_diag_logic_tick(telit_diag_logic_t *logic, uint32_t now_ms,
                           telit_diag_observation_t observation);

telit_diag_result_t telit_diag_request_rail_on(telit_diag_logic_t *logic,
                                                bool force_pwm);
telit_diag_result_t telit_diag_request_rail_off(
    telit_diag_logic_t *logic, telit_diag_observation_t observation);
telit_diag_result_t telit_diag_set_off_threshold(
    telit_diag_logic_t *logic, uint16_t raw);
telit_diag_result_t telit_diag_request_boot_pulse(
    telit_diag_logic_t *logic, uint32_t now_ms,
    telit_diag_observation_t observation, uint32_t width_ms);
telit_diag_result_t telit_diag_request_hw_off_pulse(
    telit_diag_logic_t *logic, uint32_t now_ms,
    telit_diag_observation_t observation, uint32_t width_ms);
telit_diag_result_t telit_diag_request_emergency_arm(
    telit_diag_logic_t *logic, uint32_t now_ms,
    telit_diag_observation_t observation);
telit_diag_result_t telit_diag_request_emergency_fire(
    telit_diag_logic_t *logic, uint32_t now_ms,
    telit_diag_observation_t observation);
telit_diag_result_t telit_diag_request_dtr(
    telit_diag_logic_t *logic, bool sleep_permitted,
    telit_diag_observation_t observation);

void telit_diag_abort_pulse(telit_diag_logic_t *logic);
bool telit_diag_uart_tx_allowed(const telit_diag_logic_t *logic,
                                telit_diag_observation_t observation);
bool telit_diag_on_off_asserted(const telit_diag_logic_t *logic);
bool telit_diag_emergency_asserted(const telit_diag_logic_t *logic);
const char *telit_diag_result_text(telit_diag_result_t result);
const char *telit_diag_pulse_text(telit_diag_pulse_t pulse);

#endif

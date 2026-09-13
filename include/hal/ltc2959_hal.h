#ifndef LTC2959_HAL_H
#define LTC2959_HAL_H

#include <stdbool.h>
#include <stdint.h>

#include "hal/ltc2959_regs.h"

#define LTC2959_SAMPLE_DEFAULT_PERIOD_MS 5000u

/* Rev B2 bench-qualified sign contract: registered standby moved the ACR in
 * the discharge direction, and charger tests moved it positive. Public current
 * and ACR delta are +charge / -discharge. Absolute ACR calibration is separate;
 * see ltc2959_regs.h and docs/ltc2959_acr_calibration.md. */
#define LTC2959_BOARD_CURRENT_POLARITY \
    LTC2959_RAW_POSITIVE_IS_CHARGING

typedef struct {
    bool present;
    bool configured;
    bool sample_valid;
    bool continuity_valid;
    bool current_polarity_verified; /* true for the qualified Rev B2 routing */
    bool ara_last_transport_ok;
    bool ara_last_valid;
    uint8_t ara_last_response;
    int32_t ara_last_read_result;
    uint16_t voltage_mv;
    int32_t current_ua;
    uint32_t acr_raw;
    int64_t session_delta_nah;
    int32_t temperature_mdegc;
    uint8_t status_latched;
    uint32_t conversion_sequence;
    bool last_conversion_sample_valid;
    uint32_t sample_sequence;
    uint32_t i2c_error_count;
    uint32_t ara_error_count;
    uint32_t gauge_session;
} ltc2959_snapshot_t;

typedef struct {
    bool transport_ok;
    bool did_work;
    bool more_work;
    bool ara_released;
    uint8_t status;
} ltc2959_irq_result_t;

bool ltc2959_hal_init(uint32_t now_ms);
/* sample_period_ms is an opportunistic awake cadence. The HAL does not own a
 * wake timer; callers must invoke poll after the deadline. A shorter requested
 * period advances a pending in-RAM deadline, while a longer one never postpones
 * an already-scheduled sample. */
void ltc2959_hal_poll(uint32_t now_ms, uint32_t sample_period_ms);
void ltc2959_hal_get_snapshot(ltc2959_snapshot_t *out);

/* Start a new battery session by atomically restoring the hardware ACR to its
 * POR midpoint. This is intended for provisioning/migration or an explicitly
 * confirmed battery replacement; ordinary RP resets must never call it. */
bool ltc2959_hal_start_new_session(void);

/* Put the ADC into 52-second smart-sleep while leaving the coulomb counter and
 * open-drain alert active. A full RP wake re-runs init and resumes shots. */
bool ltc2959_hal_prepare_dormant(void);
/* Restore active single-shot acquisition after a dormant-entry rollback. */
bool ltc2959_hal_resume_active(uint32_t now_ms);

/* Service one possible LTC source on the shared GP42 line. STATUS is captured
 * before its clear-on-read side effect; a nonzero alert is then released with
 * SMBus ARA and the responder's seven address bits are validated. */
void ltc2959_hal_service_alert(ltc2959_irq_result_t *out);

/* Bench-only primitive used by diagnostics to force the next voltage sample
 * above its high threshold. The normal all-range thresholds are restored by
 * the following init. */
bool ltc2959_hal_debug_force_voltage_alert(void);

#endif

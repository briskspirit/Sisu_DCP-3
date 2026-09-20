#ifndef BOARD_DIAG_SERVICE_H
#define BOARD_DIAG_SERVICE_H

#include <stdbool.h>
#include <stdint.h>

#include "services/battery_charge_logic.h"
#include "services/battery_learning_logic.h"

typedef enum {
    BOARD_DIAG_CHEM_LI_ION = 0,
    BOARD_DIAG_CHEM_NI_MH,
    BOARD_DIAG_CHEM_ALK,
    BOARD_DIAG_CHEM_COUNT,
} board_diag_chemistry_t;

typedef enum {
    BOARD_DIAG_CHARGE_DETACHED = 0,
    BOARD_DIAG_CHARGE_ACTIVE,
    BOARD_DIAG_CHARGE_FULL,
    BOARD_DIAG_CHARGE_FAULT,
    BOARD_DIAG_CHARGE_DISABLED,
} board_diag_charge_state_t;

typedef enum {
    BOARD_DIAG_POWER_ON_REFUSED = 0,
    BOARD_DIAG_POWER_ON_PENDING,
    BOARD_DIAG_POWER_ON_ALLOWED,
} board_diag_power_on_status_t;

typedef struct {
    uint16_t battery_mv; /* Nokia 65 mA reference-load domain */
    bool battery_valid;
    uint8_t battery_bars;
    bool battery_forced;
    bool battery_low;
    bool battery_empty;
    int16_t battery_correction_mv;
    int32_t battery_window_current_ua;
    bool battery_window_current_valid;
    uint8_t battery_power_on_samples;
    uint8_t battery_power_on_attempts;
    uint16_t battery_power_on_average_mv;
    bool ltc_present;
    bool ltc_configured;
    bool ltc_sample_valid;
    bool ltc_continuity_valid;
    bool ltc_current_polarity_verified;
    uint16_t ltc_voltage_mv;
    int32_t ltc_current_ua;
    uint32_t ltc_acr_raw;
    int64_t ltc_session_delta_nah;
    int32_t ltc_temperature_mdegc;
    uint8_t ltc_status;
    uint32_t ltc_i2c_errors;
    uint32_t ltc_ara_errors;
    uint32_t ltc_gauge_session;
    bool charger_connected;
    uint16_t charger_adc_raw;
    uint16_t charger_pin_mv;
    uint32_t charger_input_mv;
    bool charge_status_valid;
    bool charger_forced;
    bool charger_enabled_requested;
    bool charger_enabled;
    bool charger_enable_valid;
    uint8_t charger_inhibit_owner_mask;
    uint32_t charger_control_attempts;
    uint32_t charger_control_failures;
    uint32_t charger_control_mismatches;
    uint8_t chr_stat1;
    uint8_t chr_stat2;
    board_diag_charge_state_t charge_state;
    board_diag_chemistry_t chemistry;
    bool simulated;
    bool vbus_present;
    bool vbus_forced;
    bool headset_inserted;
    bool headset_forced;
    bool headset_hook_pressed;
    uint16_t headset_hook_mv;
    bool sys_int_asserted;
    bool tca_present;
    bool tps63020_pwm_mode;
    bool rail_3v8_power_good;
    bool rail_3v8_enabled;
    bool modem_status_monitor_available;
    uint16_t modem_status_adc_raw;
    uint16_t modem_status_pin_mv;
    bool modem_dtr_level;
    bool modem_ri_level;
    bool modem_pwr_control_asserted;
    bool modem_hw_shutdown_asserted;
    uint32_t modem_ri_edges;
    uint32_t shared_irq_edges;
    uint32_t service_vbus_edges;
    uint32_t shared_irq_drains;
    uint32_t shared_irq_stuck;
    uint32_t shared_irq_tca_events;
    uint32_t shared_irq_rtc_events;
    uint32_t shared_irq_ltc_events;
    uint32_t shared_irq_source_errors[3];
    uint8_t shared_irq_last_serviced;
    uint8_t shared_irq_last_errors;
    uint8_t shared_irq_last_tca_status;
    uint8_t shared_irq_last_rtc_flags;
    uint8_t shared_irq_last_ltc_status;
    bool backlight_on;
    bool backlight_override;
    bool backlight_override_on;
    bool vibra_pin_configured;
    bool vibra_test_enabled;
    uint8_t vibra_strength;
    bool buzzer_pin_configured;
    bool codec_present;
    bool rtc_present;
} board_diag_snapshot_t;

void board_diag_service_init(uint32_t now_ms,
                             bool supervisor_inhibit_at_boot);
void board_diag_service_poll(uint32_t now_ms);

uint16_t board_diag_battery_millivolts(void);
uint16_t board_diag_battery_millivolts_raw(void);
uint16_t board_diag_battery_millivolts_fresh(void);
uint32_t board_diag_battery_sample_sequence(void);
bool board_diag_battery_valid(void);
uint8_t board_diag_battery_level_bars(void);
bool board_diag_battery_low(void);
bool board_diag_battery_empty(void);
/* True only when a modem-supply failure may be attributed to a marginal pack:
 * fresh valid terminal evidence is below Nokia's 2320 mV low threshold. The
 * app independently decides whether verified positive charging is recovering
 * the pack; physical charger presence alone is not a veto. */
bool board_diag_battery_supply_failure_indicates_empty(void);
/* Power-on admission uses the rolling bounded LTC qualification even if the
 * newest conversion failed. A refusal gets one fresh charger-input sample, but
 * bypasses the battery floor only when /CE readback and BQ status prove active
 * recovery. Pending is not permission to start: an explicit power-on request
 * may wait for qualification while awake, with its own bounded deadline. */
board_diag_power_on_status_t board_diag_battery_power_on_status(void);
/* Fail-closed convenience for callers that cannot defer their action. */
bool board_diag_battery_power_on_allowed(void);
bool board_diag_charger_connected(void);
/* Effective headset insert state (physical OR the bench force override),
 * as sampled on the last poll. */
bool board_diag_headset_inserted(void);
bool board_diag_charge_status_valid(void);
battery_charger_state_t board_diag_charger_state(void);
bool board_diag_charger_enable_readback(bool *enabled);

/* One neutral, bounded observation for the battery-health supervisor. This
 * reads the already-maintained LTC/BQ state and never starts a conversion or
 * creates a wake deadline. Debug-forced battery/charger inputs are marked
 * non-authoritative so bench controls cannot contaminate per-pack learning. */
void board_diag_get_battery_learning_observation(
    uint32_t now_ms, battery_learning_observation_t *out);

void board_diag_get_snapshot(board_diag_snapshot_t *out);

/* Debug: hold the battery reading at a forced mv (to exercise the low-battery
 * warning / empty-shutdown path without draining the pack). Cleared by the clear
 * call or a reboot. */
void board_diag_debug_force_battery_mv(uint16_t mv);
void board_diag_debug_clear_battery_force(void);
void board_diag_debug_force_charger_connected(bool connected, uint32_t now_ms);
void board_diag_debug_clear_charger_force(void);
/* Returns true only when the requested COL5 expander state was read back. */
bool board_diag_debug_set_charger_enabled(bool enabled);
/* Release only the debug inhibit owner. Supervisor/fault owners remain latched,
 * so a power-off or Net Monitor exit cannot restart a terminated charge. */
bool board_diag_restore_charger_default(void);
bool board_diag_debug_set_tps63020_pwm_mode(bool enabled);
void board_diag_debug_set_backlight_override(bool active, bool on);
void board_diag_debug_set_vibra_test(bool enabled);

bool board_diag_backlight_override_active(void);
bool board_diag_backlight_override_on(void);
uint8_t board_diag_vibra_strength(void);

const char *board_diag_chemistry_text(board_diag_chemistry_t chemistry);
const char *board_diag_charge_state_text(board_diag_charge_state_t state);

#endif

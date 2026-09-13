#ifndef NETMON_DIAG_SERVICE_H
#define NETMON_DIAG_SERVICE_H

#include <stdbool.h>
#include <stdint.h>

#include "services/battery_learning_service.h"
#include "services/battery_charge_supervisor_service.h"
#include "services/core1_services.h"
#include "services/board_diag_service.h"
#include "services/power_sleep.h"
#include "services/stack_monitor.h"
#include "storage/store_service.h"

typedef struct {
    bool codec_ready;
    bool codec_recover_pending;
    uint8_t codec_route;
    bool mic_in_call;
    bool mic_bias_hold;
    bool playback_idle;
    uint8_t speaker_gain;
    bool speaker_muted;
    uint8_t headphone_gain;
    bool headphone_muted;
    bool service_active;
    bool audio_gate_enabled;
    bool audio_gated;
    bool bridge_active;
    uint8_t bridge_route;
    uint8_t uplink_mic_channel;
    bool right_invert;
    uint16_t downlink_depth;
    uint16_t uplink_depth;
    uint32_t downlink_underflow;
    uint32_t downlink_overflow;
    uint32_t uplink_underflow;
    uint32_t uplink_overflow;
    uint32_t codec_tx_irq;
    uint32_t codec_rx_irq;
    uint32_t codec_tx_rate;
    uint32_t codec_rx_rate;
    int16_t codec_last_left;
    int16_t codec_last_right;
    int16_t codec_peak_left;
    int16_t codec_peak_right;
    uint32_t codec_abort_timeouts;
    uint32_t modem_rx_irq;
    uint32_t modem_rx_rate;
    int16_t modem_last_wa_low;
    int16_t modem_last_wa_high;
    uint32_t modem_slot_mismatch;
    uint32_t modem_relock_attempts;
    uint32_t modem_relock_total;
    uint32_t modem_abort_timeouts;
    uint32_t modem_tx_irq;
    uint32_t modem_tx_rate;
    uint32_t modem_tx_fifo_level;
    uint32_t modem_tx_pc_min;
    uint32_t modem_tx_pc_max;
    uint32_t modem_dma0_ctrl;
    uint32_t modem_pio_fdebug;
    bool modem_tx_sm_enabled;
    bool modem_dma0_busy;
    bool modem_dma1_busy;
    bool bridge_start_pending;
    bool bclk_qualifying;
    uint32_t bridge_start_requests;
    uint32_t bridge_activations;
    uint32_t bridge_stop_requests;
    uint32_t bridge_bclk_losses;
    uint32_t bridge_resync_failures;
    uint32_t bridge_last_acquire_ms;
    uint32_t bridge_max_acquire_ms;
    uint32_t bridge_last_request_ms;
    uint32_t bridge_last_active_ms;
    uint32_t bridge_last_stop_ms;
} netmon_audio_diag_t;

typedef struct {
    uint32_t chip_reset;
    uint32_t scratch_marker;
    uint32_t pwrup[4];
    uint32_t last_swcore_pwrup;
    uint32_t current_pwrup_req;
    uint32_t prev_entry_stamp;
    uint32_t prev_entry_info;
    uint8_t wake_cause;
    uint16_t abort_counts[14];
    uint8_t abort_count;
    uint8_t last_abort_cause;
    uint32_t last_abort_ms;
    power_sleep_runtime_diag_t runtime;
} netmon_sleep_diag_t;

typedef struct {
    bool chip_available;
    bool time_valid;
    bool alarm_enabled;
    bool alarm_config_committed;
    bool alarm_event_pending;
    bool snooze_active;
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
} netmon_rtc_diag_t;

/* Deliberately compact projection. The full supervisor snapshot is 200+ bytes
 * and Net Monitor double-buffers this aggregate; keep only page-48 fields. */
typedef struct {
    int64_t net_input_nah;
    uint64_t target_input_nah;
    uint32_t charge_generation;
    uint32_t elapsed_ms;
    uint32_t blockers;
    uint32_t shadow_matches;
    uint32_t shadow_mismatches;
    uint32_t minute_count;
    uint32_t persistence_failures;
    int32_t current_ua;
    uint16_t deficit_mah;
    uint16_t terminal_mv;
    uint16_t compensated_mv;
    uint16_t curve_peak_mv;
    uint16_t curve_drop_mv;
    uint16_t frozen_capacity_mah;
    uint16_t frozen_remaining_mah;
    uint16_t charge_factor_permille;
    int16_t curve_slope_mv_per_min;
    uint8_t phase;
    uint8_t policy;
    uint8_t terminal_reason;
    uint8_t candidate;
    uint8_t frozen_capacity_confidence;
    uint8_t frozen_soc_provenance;
    uint8_t frozen_soc_confidence;
    uint8_t trace_count;
    bool deficit_valid;
    bool target_valid;
    bool target_full_capacity;
    bool frozen_capacity_valid;
    bool frozen_remaining_valid;
    bool attached;
    bool admitted;
    bool restore_pending;
    bool restored_after_reset;
    bool persistence_pending;
    bool charge_factor_confident;
    bool stop_requested;
    bool supervisor_inhibit_latched;
    bool release_inhibit_pending;
} netmon_charge_supervisor_diag_t;

/* Compact page-47 projection. Net Monitor double-buffers its local aggregate;
 * do not embed the full learner service/model state here. */
typedef struct {
    uint32_t pack_generation;
    uint32_t persistence_failures;
    uint16_t nominal_capacity_mah;
    uint16_t learned_capacity_mah;
    uint16_t remaining_capacity_mah;
    uint16_t last_capacity_mah;
    uint16_t capacity_spread_mah;
    uint64_t capacity_overrun_nah;
    uint16_t soc_bootstrap_reference_mv;
    uint16_t accepted_capacity_cycles;
    uint16_t rejected_capacity_cycles;
    uint16_t resistance_mohm[BATTERY_LEARNING_RESISTANCE_BIN_COUNT];
    uint16_t resistance_sample_count[
        BATTERY_LEARNING_RESISTANCE_BIN_COUNT];
    uint8_t state_of_charge_percent;
    uint8_t state_of_health_percent;
    uint8_t capacity_confidence;
    uint8_t soc_provenance;
    uint8_t soc_confidence;
    uint8_t soc_bars;
    uint8_t current_resistance_bin;
    bool learned_capacity_valid;
    bool remaining_capacity_valid;
    bool soc_bars_valid;
    bool capacity_prediction_exhausted;
    bool capacity_voltage_disagreement;
    bool soc_charge_segment;
    bool full_anchor_valid;
    bool capacity_cycle_qualified;
    bool current_resistance_bin_valid;
    bool persistence_pending;
} netmon_battery_learning_diag_t;

typedef struct {
    uint32_t updated_ms;
    uint32_t sequence;
    board_diag_snapshot_t board;
    netmon_audio_diag_t audio;
    netmon_sleep_diag_t sleep;
    netmon_rtc_diag_t rtc;
    uint8_t rail_owner_mask;
    uint32_t rail_transitions;
    uint8_t event_queue_depth;
    uint8_t event_queue_high_water;
    uint32_t event_queue_drops;
    uint32_t main_loop_last_us;
    uint32_t main_loop_max_us;
    uint32_t main_loop_over_budget;
    uint32_t main_loop_budget_us;
    store_diag_snapshot_t storage;
    netmon_battery_learning_diag_t battery_learning;
    netmon_charge_supervisor_diag_t battery_charge_supervisor;
    core1_services_diag_t core1;
    stack_monitor_snapshot_t stack;
    bool power_button_pressed;
    bool lcd_powered_down;
    uint8_t lcd_vop;
    uint8_t lcd_stored_vop;
    bool lcd_stored_vop_valid;
    uint8_t lcd_temperature_coefficient;
    uint8_t lcd_bias_system;
    uint8_t lcd_stored_temperature_coefficient;
    uint8_t lcd_stored_bias_system;
    uint32_t measurement_started_ms;
    uint32_t measurement_elapsed_ms;
    int64_t measurement_delta_nah;
    int32_t measurement_average_ua;
    uint32_t measurement_generation;
    char build_hash[17];
    char build_date[11];
    char modem_backend[9];
    uint32_t clk_sys_hz;
    uint32_t clk_peri_hz;
    uint32_t clk_usb_hz;
    uint32_t clk_adc_hz;
} netmon_local_diag_snapshot_t;

void netmon_diag_service_init(uint32_t now_ms);
void netmon_diag_service_poll(uint32_t now_ms);
void netmon_diag_service_get_snapshot(netmon_local_diag_snapshot_t *out);
uint32_t netmon_diag_service_measurement_generation(void);
void netmon_diag_service_reset_measurement_window(uint32_t now_ms);
void netmon_diag_service_note_main_loop(uint32_t duration_us,
                                       uint32_t budget_us);

#endif

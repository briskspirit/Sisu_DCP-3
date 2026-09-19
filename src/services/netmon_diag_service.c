#include "services/netmon_diag_service.h"

#include <limits.h>
#include <string.h>

#include "audio/audio_bridge.h"
#include "audio/audio_i2s_hal.h"
#include "audio/audio_service.h"
#include "audio/modem_i2s_hal.h"
#include "audio/nau88c22_codec.h"
#include "build_info.h"
#include "hal/lcd_pcd8544.h"
#include "hal/power_button_hal.h"
#include "hal/rtc_alarm_hal.h"
#include "hardware/clocks.h"
#include "services/core1_services.h"
#include "services/battery_learning_service.h"
#include "services/battery_charge_supervisor_service.h"
#include "services/event_queue.h"
#include "services/lcd_calibration.h"
#include "services/power_sleep.h"
#include "services/shared_3v8_service.h"
#include "services/stack_monitor.h"
#include "services/message_service.h"
#include "storage/storage_partitions.h"

#define NETMON_DIAG_SAMPLE_MS 250u

typedef struct {
    bool valid;
    uint32_t at_ms;
    uint32_t codec_tx;
    uint32_t codec_rx;
    uint32_t modem_rx;
    uint32_t modem_tx;
} netmon_rate_baseline_t;

static netmon_local_diag_snapshot_t s_snapshot;
/* Polling and publication are serialized on core 0. A separate working copy
 * preserves the existing publish-at-end behavior without spending roughly
 * 1 KiB of the 4 KiB core-0 stack on every diagnostic sample. */
static netmon_local_diag_snapshot_t s_next_snapshot;
static battery_learning_service_snapshot_t s_learning_scratch;
static battery_charge_supervisor_service_snapshot_t s_charge_scratch;
static netmon_rate_baseline_t s_rate_baseline;
static uint32_t s_next_sample_ms;
static uint32_t s_last_storage_sample_ms;
static bool s_storage_sampled;
static bool s_measurement_baseline_valid;
static uint32_t s_measurement_gauge_session;
static int64_t s_measurement_baseline_nah;
static uint32_t s_measurement_started_ms;
static uint32_t s_measurement_generation;

_Static_assert(POWER_SLEEP_ABORT_CAUSE_COUNT <= 14u,
               "Net Monitor sleep-abort snapshot is too small");

static uint32_t counter_rate(uint32_t current, uint32_t previous,
                             uint32_t elapsed_ms) {
    if (elapsed_ms == 0u) {
        return 0u;
    }
    uint32_t delta = current - previous;
    uint64_t scaled = (uint64_t)delta * 1000u;
    uint64_t rate = scaled / elapsed_ms;
    return rate > UINT32_MAX ? UINT32_MAX : (uint32_t)rate;
}

void netmon_diag_service_init(uint32_t now_ms) {
    memset(&s_snapshot, 0, sizeof(s_snapshot));
    memset(&s_rate_baseline, 0, sizeof(s_rate_baseline));
    s_measurement_baseline_valid = false;
    s_measurement_gauge_session = 0u;
    s_measurement_baseline_nah = 0;
    s_measurement_started_ms = now_ms;
    s_measurement_generation = 1u;
    s_next_sample_ms = now_ms;
    s_storage_sampled = false;
    netmon_diag_service_poll(now_ms);
}

void netmon_diag_service_poll_storage(uint32_t now_ms) {
    if (s_storage_sampled && (uint32_t)(now_ms - s_last_storage_sample_ms) < 5000u) return;
    s_last_storage_sample_ms = now_ms;
    s_storage_sampled = true;
    netmon_storage_diag_t *out = &s_snapshot.partitions;
    memset(out, 0, sizeof(*out));
    out->sampled_ms = now_ms;
    message_status_t messages;
    message_service_get_status(&messages);
    out->inbox = messages.inbox; out->outbox = messages.outbox;
    out->pending = messages.pending; out->queued = messages.queued;
    out->messages_ready = messages.ready; out->messages_full = messages.full;
    out->retention_clock_valid = messages.retention_clock_valid;
    out->expired = messages.expired_incomplete;
    out->filtered = messages.filtered_controls; out->lost = messages.receive_errors;
    storage_partition_diag_t partitions;
    storage_user_usage_t usage;
    storage_partitions_get_diag(&partitions);
    if (!partitions.ready || partitions.system_used < 0 || partitions.user_used < 0 ||
        storage_user_get_usage(&usage) != STORAGE_RECORD_OK) return;
    out->valid = true;
    out->system_used_kib = (uint16_t)(partitions.system_used * 4);
    out->system_total_kib = (uint16_t)(partitions.system_blocks * 4);
    out->user_used_kib = (uint16_t)(usage.allocated_bytes / 1024u);
    out->user_total_kib = (uint16_t)(usage.capacity_bytes / 1024u);
    out->reserve_kib = (uint16_t)(usage.recovery_reserve_bytes / 1024u);
    for (unsigned i = 0; i < STORAGE_USER_POOL_COUNT; i++) {
        out->pools[i].used_kib = (uint16_t)(usage.pools[i].allocated_bytes / 1024u);
        out->pools[i].limit_kib = (uint16_t)(usage.pools[i].limit_bytes / 1024u);
        out->pools[i].files = (uint16_t)usage.pools[i].contents.files;
        out->pools[i].data_kib = (uint16_t)((usage.pools[i].contents.file_bytes + 1023u) / 1024u);
    }
}

void netmon_diag_service_poll(uint32_t now_ms) {
    if ((int32_t)(now_ms - s_next_sample_ms) < 0) {
        return;
    }
    s_next_sample_ms = now_ms + NETMON_DIAG_SAMPLE_MS;

    s_next_snapshot = s_snapshot;
    netmon_local_diag_snapshot_t *next = &s_next_snapshot;
    next->updated_ms = now_ms;
    next->sequence++;
    board_diag_get_snapshot(&next->board);
    if (!s_measurement_baseline_valid ||
        s_measurement_gauge_session != next->board.ltc_gauge_session) {
        s_measurement_baseline_valid = true;
        s_measurement_gauge_session = next->board.ltc_gauge_session;
        s_measurement_baseline_nah = next->board.ltc_session_delta_nah;
        s_measurement_started_ms = now_ms;
        s_measurement_generation++;
        if (s_measurement_generation == 0u) {
            s_measurement_generation = 1u;
        }
    }
    next->measurement_started_ms = s_measurement_started_ms;
    next->measurement_elapsed_ms = now_ms - s_measurement_started_ms;
    next->measurement_delta_nah =
        next->board.ltc_session_delta_nah - s_measurement_baseline_nah;
    next->measurement_average_ua = 0;
    if (next->measurement_elapsed_ms != 0u) {
        int64_t average =
            (next->measurement_delta_nah * 3600) /
            (int64_t)next->measurement_elapsed_ms;
        if (average > INT32_MAX) {
            average = INT32_MAX;
        } else if (average < INT32_MIN) {
            average = INT32_MIN;
        }
        next->measurement_average_ua = (int32_t)average;
    }
    next->measurement_generation = s_measurement_generation;

    nau88c22_codec_diag_t codec;
    nau88c22_codec_get_diag(&codec);
    next->audio.codec_ready = codec.ready;
    next->audio.codec_recover_pending = codec.recover_pending;
    next->audio.codec_route = (uint8_t)codec.route;
    next->audio.mic_in_call = codec.mic_in_call;
    next->audio.mic_bias_hold = codec.mic_bias_hold;
    next->audio.playback_idle = codec.playback_idle;
    next->audio.speaker_gain = codec.speaker_gain;
    next->audio.speaker_muted = codec.speaker_muted;
    next->audio.headphone_gain = codec.headphone_gain;
    next->audio.headphone_muted = codec.headphone_muted;
    next->audio.service_active = audio_service_is_active();
    next->audio.audio_gate_enabled = core1_services_audio_gate_enabled();
    next->audio.audio_gated = core1_services_audio_gated();

    audio_bridge_stats_t bridge;
    audio_bridge_get_stats(&bridge);
    next->audio.bridge_active = bridge.active;
    next->audio.bridge_route = audio_bridge_route();
    next->audio.uplink_mic_channel = audio_bridge_uplink_mic_channel();
    next->audio.right_invert = audio_bridge_right_invert();
    next->audio.downlink_depth = bridge.downlink_depth;
    next->audio.uplink_depth = bridge.uplink_depth;
    next->audio.downlink_underflow = bridge.downlink_underflow;
    next->audio.downlink_overflow = bridge.downlink_overflow;
    next->audio.uplink_underflow = bridge.uplink_underflow;
    next->audio.uplink_overflow = bridge.uplink_overflow;

    audio_i2s_stats_t codec_i2s;
    audio_i2s_hal_get_stats(&codec_i2s);
    next->audio.codec_tx_irq = codec_i2s.tx_irq_count;
    next->audio.codec_rx_irq = codec_i2s.rx_irq_count;
    next->audio.codec_last_left = codec_i2s.last_left;
    next->audio.codec_last_right = codec_i2s.last_right;
    next->audio.codec_peak_left = codec_i2s.peak_left;
    next->audio.codec_peak_right = codec_i2s.peak_right;
    next->audio.codec_abort_timeouts = codec_i2s.abort_timeouts;

    modem_i2s_stats_t modem_i2s;
    modem_i2s_hal_get_stats(&modem_i2s);
    next->audio.modem_rx_irq = modem_i2s.rx_irq_count;
    next->audio.modem_last_wa_low = modem_i2s.last_wa_low;
    next->audio.modem_last_wa_high = modem_i2s.last_wa_high;
    next->audio.modem_slot_mismatch = modem_i2s.last_slot_mismatch;
    next->audio.modem_relock_attempts = modem_i2s.relock_attempts;
    next->audio.modem_relock_total = modem_i2s.relock_total;
    next->audio.modem_abort_timeouts = modem_i2s.abort_timeouts;

    modem_i2s_tx_probe_t tx;
    modem_i2s_hal_tx_probe(&tx);
    next->audio.modem_tx_irq = tx.tx_irq_count;
    next->audio.modem_tx_fifo_level = tx.tx_fifo_level;
    next->audio.modem_tx_pc_min = tx.pc_min;
    next->audio.modem_tx_pc_max = tx.pc_max;
    next->audio.modem_dma0_ctrl = tx.dma0_ctrl;
    next->audio.modem_pio_fdebug = tx.pio_fdebug;
    next->audio.modem_tx_sm_enabled = tx.tx_sm_enabled;
    next->audio.modem_dma0_busy = tx.dma0_busy;
    next->audio.modem_dma1_busy = tx.dma1_busy;

    audio_service_diag_t audio_service;
    audio_service_get_diag(&audio_service);
    next->audio.bridge_start_pending = audio_service.bridge_start_pending;
    next->audio.bclk_qualifying = audio_service.bclk_qualifying;
    next->audio.bridge_start_requests = audio_service.bridge_start_requests;
    next->audio.bridge_activations = audio_service.bridge_activations;
    next->audio.bridge_stop_requests = audio_service.bridge_stop_requests;
    next->audio.bridge_bclk_losses = audio_service.bridge_bclk_losses;
    next->audio.bridge_resync_failures =
        audio_service.bridge_resync_failures;
    next->audio.bridge_last_acquire_ms =
        audio_service.bridge_last_acquire_ms;
    next->audio.bridge_max_acquire_ms =
        audio_service.bridge_max_acquire_ms;
    next->audio.bridge_last_request_ms =
        audio_service.bridge_last_request_ms;
    next->audio.bridge_last_active_ms =
        audio_service.bridge_last_active_ms;
    next->audio.bridge_last_stop_ms = audio_service.bridge_last_stop_ms;

    if (s_rate_baseline.valid) {
        uint32_t elapsed = now_ms - s_rate_baseline.at_ms;
        next->audio.codec_tx_rate = counter_rate(
            codec_i2s.tx_irq_count, s_rate_baseline.codec_tx, elapsed);
        next->audio.codec_rx_rate = counter_rate(
            codec_i2s.rx_irq_count, s_rate_baseline.codec_rx, elapsed);
        next->audio.modem_rx_rate = counter_rate(
            modem_i2s.rx_irq_count, s_rate_baseline.modem_rx, elapsed);
        next->audio.modem_tx_rate = counter_rate(
            tx.tx_irq_count, s_rate_baseline.modem_tx, elapsed);
    }
    s_rate_baseline.valid = true;
    s_rate_baseline.at_ms = now_ms;
    s_rate_baseline.codec_tx = codec_i2s.tx_irq_count;
    s_rate_baseline.codec_rx = codec_i2s.rx_irq_count;
    s_rate_baseline.modem_rx = modem_i2s.rx_irq_count;
    s_rate_baseline.modem_tx = tx.tx_irq_count;

    power_sleep_evidence_t evidence;
    power_sleep_get_evidence(&evidence);
    next->sleep.chip_reset = evidence.chip_reset;
    next->sleep.scratch_marker = evidence.scratch_marker;
    next->sleep.pwrup[0] = evidence.pwrup0;
    next->sleep.pwrup[1] = evidence.pwrup1;
    next->sleep.pwrup[2] = evidence.pwrup2;
    next->sleep.pwrup[3] = evidence.pwrup3;
    next->sleep.last_swcore_pwrup = evidence.last_swcore_pwrup;
    next->sleep.current_pwrup_req = evidence.current_pwrup_req;
    next->sleep.prev_entry_stamp = evidence.prev_entry_stamp;
    next->sleep.prev_entry_info = evidence.prev_entry_info;
    next->sleep.wake_cause = (uint8_t)evidence.cause;

    power_sleep_abort_stats_t aborts;
    power_sleep_get_abort_stats(&aborts);
    next->sleep.abort_count = (uint8_t)POWER_SLEEP_ABORT_CAUSE_COUNT;
    for (uint8_t i = 0u; i < next->sleep.abort_count; i++) {
        next->sleep.abort_counts[i] = aborts.counts[i];
    }
    next->sleep.last_abort_cause = aborts.last_cause;
    next->sleep.last_abort_ms = aborts.last_ms;
    power_sleep_get_runtime_diag(&next->sleep.runtime);

    next->rtc.chip_available = rtc_alarm_hal_chip_available();
    next->rtc.time_valid = rtc_alarm_hal_time_valid();
    next->rtc.alarm_enabled = rtc_alarm_hal_alarm_enabled();
    next->rtc.alarm_config_committed =
        rtc_alarm_hal_alarm_config_committed();
    next->rtc.alarm_event_pending = rtc_alarm_hal_alarm_event_pending();
    next->rtc.snooze_active = rtc_alarm_hal_snooze_active();
    rtc_datetime_t datetime;
    rtc_alarm_hal_get_datetime(&datetime);
    next->rtc.year = datetime.year;
    next->rtc.month = datetime.month;
    next->rtc.day = datetime.day;
    next->rtc.hour = datetime.hour;
    next->rtc.minute = datetime.minute;
    next->rtc.second = datetime.second;

    next->rail_owner_mask = shared_3v8_service_owner_mask();
    next->rail_transitions = shared_3v8_service_transition_count();
    event_queue_stats_t queue;
    event_queue_get_stats(&queue);
    next->event_queue_depth = queue.depth;
    next->event_queue_high_water = queue.high_water;
    next->event_queue_drops = queue.dropped;
    store_service_get_diag(&next->storage);
    battery_learning_service_get_snapshot(&s_learning_scratch);
    const battery_learning_snapshot_t *learning = &s_learning_scratch.model;
    next->battery_learning = (netmon_battery_learning_diag_t){
        .pack_generation = learning->pack_generation,
        .persistence_failures = s_learning_scratch.persistence_failures,
        .nominal_capacity_mah = learning->nominal_capacity_mah,
        .learned_capacity_mah = learning->learned_capacity_mah,
        .remaining_capacity_mah = learning->remaining_capacity_mah,
        .last_capacity_mah = learning->last_capacity_mah,
        .capacity_spread_mah = learning->capacity_spread_mah,
        .capacity_overrun_nah = learning->capacity_overrun_nah,
        .soc_bootstrap_reference_mv =
            learning->soc_bootstrap_reference_mv,
        .accepted_capacity_cycles = learning->accepted_capacity_cycles,
        .rejected_capacity_cycles = learning->rejected_capacity_cycles,
        .state_of_charge_percent = learning->state_of_charge_percent,
        .state_of_health_percent = learning->state_of_health_percent,
        .capacity_confidence = (uint8_t)learning->capacity_confidence,
        .soc_provenance = (uint8_t)learning->soc_provenance,
        .soc_confidence = (uint8_t)learning->soc_confidence,
        .soc_bars = learning->soc_bars,
        .current_resistance_bin =
            (uint8_t)learning->current_resistance_bin,
        .learned_capacity_valid = learning->learned_capacity_valid,
        .remaining_capacity_valid = learning->remaining_capacity_valid,
        .soc_bars_valid = learning->soc_bars_valid,
        .capacity_prediction_exhausted =
            learning->capacity_prediction_exhausted,
        .capacity_voltage_disagreement =
            learning->capacity_prediction_exhausted &&
            next->board.battery_valid && !next->board.battery_low &&
            !next->board.battery_empty,
        .soc_charge_segment = learning->soc_charge_segment,
        .full_anchor_valid = learning->full_anchor_valid,
        .capacity_cycle_qualified = learning->capacity_cycle_qualified,
        .current_resistance_bin_valid =
            learning->current_resistance_bin_valid,
        .persistence_pending = s_learning_scratch.persistence_pending,
    };
    memcpy(next->battery_learning.resistance_mohm,
           learning->resistance_mohm,
           sizeof(next->battery_learning.resistance_mohm));
    memcpy(next->battery_learning.resistance_sample_count,
           learning->resistance_sample_count,
           sizeof(next->battery_learning.resistance_sample_count));
    battery_charge_supervisor_service_get_snapshot(&s_charge_scratch);
    const battery_charge_supervisor_snapshot_t *charge =
        &s_charge_scratch.model;
    next->battery_charge_supervisor = (netmon_charge_supervisor_diag_t){
        .net_input_nah = charge->net_input_nah,
        .target_input_nah = charge->target_input_nah,
        .charge_generation = charge->charge_generation,
        .elapsed_ms = charge->elapsed_ms,
        .blockers = charge->blockers,
        .shadow_matches = s_charge_scratch.shadow_matches,
        .shadow_mismatches = s_charge_scratch.shadow_mismatches,
        .minute_count = charge->minute_count,
        .persistence_failures = s_charge_scratch.persistence_failures,
        .current_ua = charge->current_ua,
        .deficit_mah = charge->deficit_mah,
        .terminal_mv = charge->terminal_mv,
        .compensated_mv = charge->compensated_mv,
        .curve_peak_mv = charge->curve_peak_mv,
        .curve_drop_mv = charge->curve_drop_mv,
        .frozen_capacity_mah = charge->frozen_capacity_mah,
        .frozen_remaining_mah = charge->frozen_remaining_mah,
        .charge_factor_permille = charge->charge_factor_permille,
        .curve_slope_mv_per_min = charge->curve_slope_mv_per_min,
        .phase = (uint8_t)charge->phase,
        .policy = (uint8_t)charge->policy,
        .terminal_reason = (uint8_t)charge->terminal_reason,
        .candidate = (uint8_t)charge->candidate,
        .frozen_capacity_confidence =
            (uint8_t)charge->frozen_capacity_confidence,
        .frozen_soc_provenance =
            (uint8_t)charge->frozen_soc_provenance,
        .frozen_soc_confidence =
            (uint8_t)charge->frozen_soc_confidence,
        .trace_count = s_charge_scratch.trace_count,
        .deficit_valid = charge->deficit_valid,
        .target_valid = charge->target_valid,
        .target_full_capacity = charge->target_full_capacity,
        .frozen_capacity_valid = charge->frozen_capacity_valid,
        .frozen_remaining_valid = charge->frozen_remaining_valid,
        .attached = charge->attached,
        .admitted = charge->admitted,
        .restore_pending = s_charge_scratch.restore_pending,
        .restored_after_reset = s_charge_scratch.restored_after_reset,
        .persistence_pending = s_charge_scratch.persistence_pending,
        .charge_factor_confident = charge->charge_factor_confident,
        .stop_requested = charge->stop_requested,
        .supervisor_inhibit_latched =
            s_charge_scratch.persisted.supervisor_inhibit_latched,
        .release_inhibit_pending =
            s_charge_scratch.release_inhibit_pending,
    };
    core1_services_get_diag(&next->core1);
    stack_monitor_get_snapshot(&next->stack);
    next->power_button_pressed = power_button_scan_raw();

    lcd_calibration_status_t lcd;
    lcd_calibration_get_status(&lcd);
    next->lcd_powered_down = lcd_is_powered_down();
    next->lcd_vop = lcd.vop;
    next->lcd_stored_vop = lcd.stored_vop;
    next->lcd_stored_vop_valid = lcd.stored_vop_valid;
    next->lcd_temperature_coefficient = lcd.temperature_coefficient;
    next->lcd_bias_system = lcd.bias_system;
    next->lcd_stored_temperature_coefficient =
        lcd.stored_temperature_coefficient;
    next->lcd_stored_bias_system = lcd.stored_bias_system;

    strncpy(next->build_hash, SISU_BUILD_HASH, sizeof(next->build_hash) - 1u);
    next->build_hash[sizeof(next->build_hash) - 1u] = '\0';
    strncpy(next->build_date, SISU_BUILD_DATE, sizeof(next->build_date) - 1u);
    next->build_date[sizeof(next->build_date) - 1u] = '\0';
    strcpy(next->modem_backend, "Telit");

    next->clk_sys_hz = clock_get_hz(clk_sys);
    next->clk_peri_hz = clock_get_hz(clk_peri);
    next->clk_usb_hz = clock_get_hz(clk_usb);
    next->clk_adc_hz = clock_get_hz(clk_adc);
    s_snapshot = *next;
}

void netmon_diag_service_get_snapshot(netmon_local_diag_snapshot_t *out) {
    if (out != NULL) {
        *out = s_snapshot;
    }
}

uint32_t netmon_diag_service_measurement_generation(void) {
    return s_snapshot.measurement_generation;
}

void netmon_diag_service_reset_measurement_window(uint32_t now_ms) {
    board_diag_snapshot_t board;
    board_diag_get_snapshot(&board);
    s_measurement_baseline_valid = true;
    s_measurement_gauge_session = board.ltc_gauge_session;
    s_measurement_baseline_nah = board.ltc_session_delta_nah;
    s_measurement_started_ms = now_ms;
    s_measurement_generation++;
    if (s_measurement_generation == 0u) {
        s_measurement_generation = 1u;
    }
    memset(&s_rate_baseline, 0, sizeof(s_rate_baseline));
    s_next_sample_ms = now_ms;
}

void netmon_diag_service_note_main_loop(uint32_t duration_us,
                                       uint32_t budget_us) {
    s_snapshot.main_loop_last_us = duration_us;
    if (duration_us > s_snapshot.main_loop_max_us) {
        s_snapshot.main_loop_max_us = duration_us;
    }
    s_snapshot.main_loop_budget_us = budget_us;
    if (duration_us > budget_us && s_snapshot.main_loop_over_budget != UINT32_MAX) {
        s_snapshot.main_loop_over_budget++;
    }
}

_Static_assert(sizeof(netmon_storage_diag_t) <= 96u,
               "Net Monitor partition projection unexpectedly large");
_Static_assert(sizeof(netmon_local_diag_snapshot_t) <= 1024u + 96u,
               "Net Monitor local snapshot unexpectedly large");

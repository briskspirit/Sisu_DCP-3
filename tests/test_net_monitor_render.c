#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "apps/net_monitor_logic.h"
#include "apps/net_monitor_render.h"
#include "ui/assets.h"
#include "../src/apps/net_monitor/render_internal.h"

static int s_failures;

static void check(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        s_failures++;
    }
}

static uint32_t supported_caps(void) {
    return NETMON_CAP_REVB2 | NETMON_CAP_TELIT | NETMON_CAP_MODEM |
           NETMON_CAP_TUNER | NETMON_CAP_VOICE |
           NETMON_CAP_LOCAL_CONTROLS | NETMON_CAP_RUNTIME_EXT |
           NETMON_CAP_RADIO_CONTROL | NETMON_CAP_RF_MAINTENANCE;
}

static void populate_snapshots(netmon_local_diag_snapshot_t *local,
                               modem_diag_snapshot_t *modem,
                               netmon_control_snapshot_t *control) {
    memset(local, 0, sizeof(*local));
    memset(modem, 0, sizeof(*modem));
    memset(control, 0, sizeof(*control));

    local->sequence = UINT32_MAX;
    local->board.battery_valid = true;
    local->board.battery_mv = UINT16_MAX;
    local->board.ltc_sample_valid = true;
    local->board.ltc_current_ua = INT32_MIN;
    local->board.ltc_session_delta_nah = INT64_MIN / 2;
    local->board.ltc_temperature_mdegc = INT32_MIN;
    local->battery_learning.nominal_capacity_mah = 1225u;
    local->battery_learning.learned_capacity_mah = 1050u;
    local->battery_learning.learned_capacity_valid = true;
    local->battery_learning.remaining_capacity_mah = 725u;
    local->battery_learning.remaining_capacity_valid = true;
    local->battery_learning.state_of_charge_percent = 69u;
    local->battery_learning.state_of_health_percent = 86u;
    local->battery_learning.soc_provenance =
        BATTERY_SOC_PROVENANCE_TRACKED;
    local->battery_learning.soc_confidence =
        BATTERY_SOC_CONFIDENCE_ANCHORED;
    local->battery_learning.soc_bars = 3u;
    local->battery_learning.soc_bars_valid = true;
    local->battery_learning.capacity_prediction_exhausted = false;
    local->battery_learning.capacity_voltage_disagreement = false;
    local->battery_learning.capacity_overrun_nah = 0u;
    local->battery_learning.soc_charge_segment = false;
    local->battery_learning.capacity_confidence =
        BATTERY_CAPACITY_CONFIDENCE_LEARNED;
    local->battery_learning.full_anchor_valid = true;
    local->battery_learning.capacity_cycle_qualified = true;
    local->battery_learning.accepted_capacity_cycles = 3u;
    local->battery_learning.rejected_capacity_cycles = 1u;
    local->battery_learning.last_capacity_mah = 1000u;
    local->battery_learning.capacity_spread_mah = 100u;
    local->battery_learning.resistance_mohm[0] = 190u;
    local->battery_learning.resistance_mohm[1] = 220u;
    local->battery_learning.resistance_mohm[2] = 300u;
    local->battery_learning.resistance_sample_count[0] = 11u;
    local->battery_learning.resistance_sample_count[1] = 22u;
    local->battery_learning.resistance_sample_count[2] = 33u;
    local->battery_learning.current_resistance_bin =
        BATTERY_RESISTANCE_BIN_MID;
    local->battery_learning.current_resistance_bin_valid = true;
    local->battery_charge_supervisor.phase =
        BATTERY_CHARGE_PHASE_CHARGING;
    local->battery_charge_supervisor.policy =
        BATTERY_CHARGE_POLICY_ENFORCE_BOOTSTRAP;
    local->battery_charge_supervisor.charge_generation = 1u;
    local->battery_charge_supervisor.elapsed_ms = 123000u;
    local->battery_charge_supervisor.candidate =
        BATTERY_CHARGE_CANDIDATE_SOFTWARE_COULOMB_FULL;
    local->battery_charge_supervisor.net_input_nah = 123400000;
    local->battery_charge_supervisor.deficit_valid = true;
    local->battery_charge_supervisor.deficit_mah = 800u;
    local->battery_charge_supervisor.target_valid = true;
    local->battery_charge_supervisor.target_full_capacity = true;
    local->battery_charge_supervisor.target_input_nah = 1120000000u;
    local->battery_charge_supervisor.blockers = 0x00001234u;
    local->battery_charge_supervisor.terminal_mv = 2700u;
    local->battery_charge_supervisor.compensated_mv = 2680u;
    local->battery_charge_supervisor.curve_peak_mv = 2690u;
    local->battery_charge_supervisor.curve_drop_mv = 10u;
    local->battery_charge_supervisor.curve_slope_mv_per_min = -2;
    local->battery_charge_supervisor.current_ua = 100000;
    local->battery_charge_supervisor.frozen_capacity_valid = true;
    local->battery_charge_supervisor.frozen_capacity_mah = 1000u;
    local->battery_charge_supervisor.frozen_capacity_confidence =
        BATTERY_CAPACITY_CONFIDENCE_LEARNED;
    local->battery_charge_supervisor.frozen_remaining_valid = true;
    local->battery_charge_supervisor.frozen_remaining_mah = 200u;
    local->battery_charge_supervisor.frozen_soc_provenance =
        BATTERY_SOC_PROVENANCE_BOOTSTRAP_VOLTAGE;
    local->battery_charge_supervisor.frozen_soc_confidence =
        BATTERY_SOC_CONFIDENCE_PROVISIONAL;
    local->battery_charge_supervisor.charge_factor_permille = 1224u;
    local->battery_charge_supervisor.charge_factor_confident = true;
    local->battery_charge_supervisor.attached = true;
    local->battery_charge_supervisor.admitted = true;
    local->battery_charge_supervisor.minute_count = 5u;
    local->battery_charge_supervisor.shadow_matches = 12u;
    local->battery_charge_supervisor.shadow_mismatches = 1u;
    local->battery_charge_supervisor.trace_count = 5u;
    local->battery_charge_supervisor.restored_after_reset = true;
    local->board.charger_input_mv = UINT32_MAX;
    local->board.headset_hook_mv = UINT16_MAX;
    local->sleep.abort_count = 14u;
    for (uint8_t i = 0u; i < local->sleep.abort_count; i++) {
        local->sleep.abort_counts[i] = UINT16_MAX;
    }
    local->measurement_delta_nah = INT64_MIN / 2;
    local->measurement_average_ua = INT32_MIN;
    strcpy(local->build_hash, "0123456789abcdef");
    strcpy(local->build_date, "2026-08-13");
    strcpy(local->modem_backend, "Telit");
    local->stack.core0.initialized = true;
    local->stack.core0.canary_intact = true;
    local->stack.core0.size_bytes = 4096u;
    local->stack.core0.peak_used_bytes = 2368u;
    local->stack.core0.minimum_margin_bytes = 1728u;
    local->stack.core1.initialized = true;
    local->stack.core1.canary_intact = true;
    local->stack.core1.size_bytes = 4096u;
    local->stack.core1.peak_used_bytes = 880u;
    local->stack.core1.minimum_margin_bytes = 3216u;
    local->partitions = (netmon_storage_diag_t){.valid=true, .sampled_ms=1200u,
        .system_used_kib=12u, .system_total_kib=64u,
        .user_used_kib=120u, .user_total_kib=384u, .reserve_kib=32u,
        .inbox=500u, .outbox=500u, .pending=64u, .queued=8u,
        .messages_ready=true, .retention_clock_valid=true,
        .expired=123u, .filtered=456u, .lost=7u};
    for (unsigned i = 0u; i < STORAGE_USER_POOL_COUNT; i++) {
        local->partitions.pools[i].used_kib = 32u;
        local->partitions.pools[i].limit_kib = 64u;
        local->partitions.pools[i].files = 500u;
        local->partitions.pools[i].data_kib = 25u;
    }

    modem->backend_available = true;
    modem->at_ready = true;
    modem->sim_checked = true;
    modem->sim_present = true;
    modem->sim_ready = true;
    modem->network_registered = true;
    for (uint8_t i = 1u; i < (uint8_t)MODEM_DIAG_GROUP_COUNT; i++) {
        modem->group[i].state = MODEM_DIAG_STATE_FRESH;
        modem->group[i].sequence = UINT32_MAX;
        modem->group[i].last_success_ms = 1000u;
        modem->group[i].present_fields = UINT64_MAX;
    }
    modem->serving.rat = MODEM_DIAG_RAT_LTE;
    modem->serving.band = UINT16_MAX;
    modem->serving.channel = UINT32_MAX;
    modem->serving.rssi_dbm = INT16_MIN;
    modem->serving.rsrp_dbm = INT16_MIN;
    modem->serving.rsrq_db_x2 = INT16_MIN;
    modem->serving.sinr_db_x10 = INT16_MIN;
    modem->serving.tx_power_dbm_x10 = INT16_MIN;
    modem->serving.inferred_rf_state = 4u;
    modem->serving.inferred_rf_tuned = true;
    strcpy(modem->serving.mcc, "310");
    strcpy(modem->serving.mnc, "410");
    strcpy(modem->serving.operator_name, "ABCDEFGHIJKLMNO");
    strcpy(modem->serving.area_code, "FFFF");
    strcpy(modem->serving.cell_id, "FFFFFFFF");
    modem->registration.cs_act = UINT8_MAX;
    modem->registration.ps_act = UINT8_MAX;
    modem->registration.eps_act = UINT8_MAX;
    strcpy(modem->radio_policy.cops_operator, "ABCDEFGHIJKLMNO");
    strcpy(modem->radio_policy.bnd_lte, "FFFFFFFFFFFFFFFF");
    strcpy(modem->radio_policy.bndram_lte, "FFFFFFFFFFFFFFFF");
    modem->tuner.row_count = MODEM_DIAG_TUNER_ROWS_MAX;
    modem->tuner.supported_mask = UINT64_MAX;
    modem->tuner.enabled = true;
    modem->tuner.table_exact = true;
    modem->tuner.table_complete = true;
    for (uint8_t i = 0u; i < MODEM_DIAG_TUNER_ROWS_MAX; i++) {
        modem->tuner.rows[i].band_mask = UINT64_MAX;
        modem->tuner.rows[i].ctrl1 = 1u;
        modem->tuner.rows[i].ctrl2 = 1u;
    }
    strcpy(modem->packet.apn, "abcdefghijklmnopqrstuvwx");
    strcpy(modem->packet.address, "ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff");
    strcpy(modem->sim.cpin, "READY");
    strcpy(modem->identity.model, "LE910C1-WWX-EXTRA-LONG");
    strcpy(modem->identity.firmware, "ABCDEFGHIJKLMNOPQRSTUVWXYZ123456");
    strcpy(modem->identity.imei, "123456789012345");
    strcpy(modem->runtime.last_command, "AT+A-LONG-DIAGNOSTIC-COMMAND");
    strcpy(modem->runtime.last_line, "A-LONG-MODEM-RESPONSE-LINE");
    strcpy(modem->call_cause.ceer, "A long terminal cause from the network");
    strcpy(modem->sms.service_center, "+1234567890123456789012345678901");
    strcpy(modem->storage.sms_read_store, "ME");
    strcpy(modem->storage.phonebook_store, "ME");
    modem->calls.leg_count = MODEM_DIAG_CALL_LEGS_MAX;
    for (uint8_t i = 0u; i < modem->calls.leg_count; i++) {
        modem->calls.legs[i].id = i;
        modem->calls.legs[i].generation = UINT16_MAX;
        modem->calls.legs[i].direction = UINT8_MAX;
        modem->calls.legs[i].state = UINT8_MAX;
        modem->calls.legs[i].role = UINT8_MAX;
    }
    modem->calls.txn_count = MODEM_DIAG_CALL_TXNS_MAX;
    for (uint8_t i = 0u; i < modem->calls.txn_count; i++) {
        modem->calls.txns[i].token = UINT32_MAX;
        modem->calls.txns[i].kind = UINT8_MAX;
        modem->calls.txns[i].state = UINT8_MAX;
        modem->calls.txns[i].target_id = UINT8_MAX;
        modem->calls.txns[i].target_generation = UINT16_MAX;
    }

    control->sequence = UINT32_MAX;
    control->backlight_percent = 73u;
    control->backlight_stored_percent = 65u;
    control->backlight_stored_valid = true;
    control->lcd_vop = 127u;
    control->lcd_temperature_coefficient = 3u;
    control->lcd_bias_system = 7u;
    control->measurement_generation = UINT32_MAX;
    control->status[0] = '\0';
}

static netmon_render_owner_t expected_owner(uint8_t id) {
    if (id >= 1u && id <= 17u) {
        return NETMON_RENDER_OWNER_RADIO;
    }
    if (id >= 20u && id <= 29u) {
        return NETMON_RENDER_OWNER_MODEM;
    }
    if (id >= 30u && id <= 38u) {
        return NETMON_RENDER_OWNER_TELEPHONY;
    }
    if ((id >= 40u && id <= 48u) ||
        (id >= 50u && id <= 58u) ||
        (id >= 60u && id <= 68u) ||
        (id >= 70u && id <= 76u) ||
        (id >= 80u && id <= 88u)) {
        return NETMON_RENDER_OWNER_LOCAL;
    }
    if (id >= 89u && id <= 99u) {
        return NETMON_RENDER_OWNER_CONTROL;
    }
    return NETMON_RENDER_OWNER_NONE;
}

static void test_page_ownership(void) {
    size_t owned = 0u;
    check(netmon_registry_count() == 89u,
          "render ownership contract covers the complete page registry");

    for (size_t i = 0u; i < netmon_registry_count(); i++) {
        const netmon_page_descriptor_t *page = netmon_registry_at(i);
        netmon_render_owner_t expected = expected_owner(page->id);
        netmon_render_owner_t actual = netmon_render_owner_for_page(page);
        check(expected != NETMON_RENDER_OWNER_NONE,
              "every registered page has an expected render owner");
        check(actual == expected,
              "every registered page maps to exactly its expected render owner");
        if (actual != NETMON_RENDER_OWNER_NONE) {
            owned++;
        }
    }
    check(owned == netmon_registry_count(),
          "no registered page is left without a renderer");
    check(netmon_render_owner_for_page(NULL) == NETMON_RENDER_OWNER_NONE,
          "null page has no render owner");

    netmon_page_descriptor_t future_telephony = {
        .id = 38u,
        .behavior_flags = NETMON_PAGE_READ_ONLY,
    };
    check(netmon_render_owner_for_page(&future_telephony) ==
              NETMON_RENDER_OWNER_TELEPHONY,
          "unregistered page 38 remains in the telephony ownership range");
}

static void check_lines(const netmon_frame_t *frame,
                        const char *line0, const char *line1,
                        const char *line2, const char *line3,
                        const char *message) {
    check(strcmp(frame->lines[0], line0) == 0 &&
              strcmp(frame->lines[1], line1) == 0 &&
              strcmp(frame->lines[2], line2) == 0 &&
              strcmp(frame->lines[3], line3) == 0,
          message);
}

static void test_representative_exact_frames(void) {
    netmon_local_diag_snapshot_t local;
    modem_diag_snapshot_t modem;
    netmon_control_snapshot_t control;
    netmon_frame_t frame;
    populate_snapshots(&local, &modem, &control);
    uint32_t caps = supported_caps();

    netmon_format_frame(netmon_registry_find(17u), 0u, caps, 1200u,
                        &local, &modem, &control, &frame);
    check_lines(&frame, "TEMP 0C", "LEVEL 0", "", "",
                "radio renderer exact frame stays stable");

    netmon_format_frame(netmon_registry_find(14u), 0u, caps, 1200u,
                        &local, &modem, &control, &frame);
    check_lines(&frame, "ATT 0", "CTX 0/0", "CID 0 S0", "abcdefghijk>",
                "packet APN truncation is explicit on its one-row frame");
    strcpy(modem.packet.address,
           "AAAAAAAAAAAABBBBBBBBBBBBCCCCCCCCCCCCDDDD");
    netmon_format_frame(netmon_registry_find(14u), 1u, caps, 1200u,
                        &local, &modem, &control, &frame);
    check_lines(&frame, "AAAAAAAAAAAA", "BBBBBBBBBBBB", "CCCCCCCCCCCC",
                "DDDD>",
                "packet address truncation is explicit on its final row");

    netmon_format_frame(netmon_registry_find(20u), 0u, caps, 1200u,
                        &local, &modem, &control, &frame);
    check_lines(&frame, "STATE 0", "AT ready", "OP 0 K0", "REG yes",
                "modem renderer exact frame stays stable");

    netmon_format_frame(netmon_registry_find(30u), 0u, caps, 1200u,
                        &local, &modem, &control, &frame);
    check_lines(&frame, "STATE 0 ID0", "R0 W0 H0", "2H 0", "RES 0/0",
                "telephony renderer exact frame stays stable");

    local.board.ltc_voltage_mv = 2518u;
    local.board.battery_mv = 2555u;
    local.board.battery_correction_mv = 37;
    local.board.battery_bars = 3u;
    local.board.battery_low = false;
    local.board.battery_empty = false;
    local.board.battery_window_current_valid = true;
    local.board.battery_window_current_ua = -12000;
    local.board.battery_power_on_samples = 5u;
    local.board.battery_power_on_attempts = 6u;
    netmon_format_frame(netmon_registry_find(40u), 0u, caps, 1200u,
                        &local, &modem, &control, &frame);
    check_lines(&frame, "T2518 R2555", "C37 B3 W-", "I -12.0mA", "BOOT 5/6",
                "battery page distinguishes terminal and reference-load evidence");

    netmon_format_frame(netmon_registry_find(47u), 0u, caps, 1200u,
                        &local, &modem, &control, &frame);
    check_lines(&frame, "NOM 1225mAh", "CAP 1050mAh", "REM 725mAh",
                "S69 B3 H86", "learned-capacity summary is exact");
    netmon_format_frame(netmon_registry_find(47u), 1u, caps, 1200u,
                        &local, &modem, &control, &frame);
    check_lines(&frame, "CONF LEARN", "ANCH Q P0 V0", "OK 3 O0", "BAD 1",
                "capacity confidence evidence is exact");
    local.battery_learning.capacity_prediction_exhausted = true;
    local.battery_learning.capacity_voltage_disagreement = true;
    local.battery_learning.capacity_overrun_nah = UINT64_C(50000000);
    netmon_format_frame(netmon_registry_find(47u), 1u, caps, 1200u,
                        &local, &modem, &control, &frame);
    check_lines(&frame, "CONF LEARN", "ANCH Q P1 V1", "OK 3 O50", "BAD 1",
                "capacity overrun disagreement remains visible");
    netmon_format_frame(netmon_registry_find(47u), 2u, caps, 1200u,
                        &local, &modem, &control, &frame);
    check_lines(&frame, "SOC ANCH", "SRC TRACK", "SEG DIS", "BOOT --",
                "SOC provenance and confidence are exact");
    netmon_format_frame(netmon_registry_find(47u), 3u, caps, 1200u,
                        &local, &modem, &control, &frame);
    check_lines(&frame, "LAST 1000", "SPREAD 100", "GEN 00000000",
                "PF00000000P0", "capacity persistence evidence is exact");
    netmon_format_frame(netmon_registry_find(47u), 4u, caps, 1200u,
                        &local, &modem, &control, &frame);
    check_lines(&frame, "H190 N000B", "M220 N0016", "L300 N0021",
                "BIN M", "resistance evidence is exact");

    netmon_format_frame(netmon_registry_find(48u), 0u, caps, 1200u,
                        &local, &modem, &control, &frame);
    check_lines(&frame, "B P2 G0001", "T 123s", "TERM 0 C2",
                "SH 000C/0001", "charge supervisor identity is exact");
    netmon_format_frame(netmon_registry_find(48u), 1u, caps, 1200u,
                        &local, &modem, &control, &frame);
    check_lines(&frame, "dQ 123.4mAh", "DEF 800mAh", "TGT1120mAh F",
                "BLK 00001234", "charge supervisor accounting is exact");
    netmon_format_frame(netmon_registry_find(48u), 2u, caps, 1200u,
                        &local, &modem, &control, &frame);
    check_lines(&frame, "V2700 C2680", "PK2690 D10", "SL -2",
                "I 100.0mA", "charge supervisor curve evidence is exact");
    netmon_format_frame(netmon_registry_find(48u), 3u, caps, 1200u,
                        &local, &modem, &control, &frame);
    check_lines(&frame, "CAP 1000 C2", "REM 200mAh", "A1 Q1 M5",
                "N+ R1 F0000", "charge supervisor persistence is exact");
    netmon_format_frame(netmon_registry_find(48u), 4u, caps, 1200u,
                        &local, &modem, &control, &frame);
    check_lines(&frame, "POL BOOT", "FAC 1224 T", "STOP 0 L0", "REL 0",
                "charge enforcement configuration is exact");

    memset(&local.battery_learning, 0, sizeof(local.battery_learning));
    local.battery_learning.nominal_capacity_mah = 1225u;
    local.battery_learning.remaining_capacity_valid = true;
    local.battery_learning.remaining_capacity_mah = 1225u;
    local.battery_learning.state_of_charge_percent = 100u;
    local.battery_learning.soc_bars_valid = false;
    netmon_format_frame(netmon_registry_find(47u), 0u, caps, 1200u,
                        &local, &modem, &control, &frame);
    check_lines(&frame, "NOM 1225mAh", "CAP --", "REM 1225mAh",
                "S100 B- H--", "capacity prior never presents unknown SOH as zero");
    netmon_format_frame(netmon_registry_find(47u), 3u, caps, 1200u,
                        &local, &modem, &control, &frame);
    check_lines(&frame, "LAST --", "SPREAD --", "GEN 00000000",
                "PF00000000P0", "virgin cycle history stays explicitly unknown");
    netmon_format_frame(netmon_registry_find(47u), 4u, caps, 1200u,
                        &local, &modem, &control, &frame);
    check_lines(&frame, "H-- N0000", "M-- N0000", "L-- N0000",
                "BIN -", "unlearned resistance stays explicitly unknown");

    netmon_format_frame(netmon_registry_find(50u), 0u, caps, 1200u,
                        &local, &modem, &control, &frame);
    check_lines(&frame, "RevB2 Telit", "0123456789ab", "cdef", "UP 0s",
                "local renderer exact frame stays stable");

    netmon_format_frame(netmon_registry_find(90u), 0u, caps, 1200u,
                        &local, &modem, &control, &frame);
    check_lines(&frame, "1 LIGHT", "2 VIBRA", "3 BUZZER", "0 STOP",
                "control renderer exact frame stays stable");
}

static void test_modem_gates_and_control_status(void) {
    netmon_local_diag_snapshot_t local;
    modem_diag_snapshot_t modem;
    netmon_control_snapshot_t control;
    netmon_frame_t frame;
    populate_snapshots(&local, &modem, &control);
    uint32_t caps = supported_caps();

    modem.backend_available = false;
    netmon_format_frame(netmon_registry_find(1u), 0u, caps, 1200u,
                        &local, &modem, &control, &frame);
    check_lines(&frame, "", "NO MODEM", "", "",
                "query page reports an absent modem exactly");

    modem.backend_available = true;
    modem.at_ready = false;
    netmon_format_frame(netmon_registry_find(1u), 0u, caps, 1200u,
                        &local, &modem, &control, &frame);
    check_lines(&frame, "", "AT STARTUP", "", "",
                "query page reports modem startup exactly");

    modem.backend_available = false;
    modem.at_ready = true;
    netmon_format_frame(netmon_registry_find(99u), 0u, caps, 1200u,
                        &local, &modem, &control, &frame);
    check_lines(&frame, "", "NO MODEM", "", "",
                "editable modem page reports an absent modem exactly");

    modem.backend_available = true;
    modem.at_ready = false;
    netmon_format_frame(netmon_registry_find(99u), 0u, caps, 1200u,
                        &local, &modem, &control, &frame);
    check_lines(&frame, "", "AT STARTUP", "", "",
                "editable modem page reports modem startup exactly");

    modem.at_ready = true;
    strcpy(control.status, "SAVED");
    netmon_format_frame(netmon_registry_find(90u), 0u, caps, 1200u,
                        &local, &modem, &control, &frame);
    check_lines(&frame, "1 LIGHT", "2 VIBRA", "3 BUZZER", "SAVED",
                "editable status replaces the final control row exactly");
}

static void test_every_frame_fits(void) {
    netmon_local_diag_snapshot_t local;
    modem_diag_snapshot_t modem;
    netmon_control_snapshot_t control;
    populate_snapshots(&local, &modem, &control);
    const font_t *font = asset_font(FONT_FS4);
    uint32_t caps = supported_caps();

    for (size_t i = 0u; i < netmon_registry_count(); i++) {
        const netmon_page_descriptor_t *page = netmon_registry_at(i);
        if (!netmon_page_supported(page, caps)) {
            continue;
        }
        uint8_t count = netmon_frame_count(page, &modem);
        check(count != 0u, "implemented page has at least one frame");
        for (uint8_t frame_index = 0u; frame_index < count; frame_index++) {
            netmon_frame_t frame;
            netmon_format_frame(page, frame_index, caps, 1200u,
                                &local, &modem, &control, &frame);
            check(strcmp(frame.lines[1], "UNSUP") != 0,
                  "every supported registry page has an implemented formatter");
            for (uint8_t row = 0u; row < NETMON_FRAME_LINE_COUNT; row++) {
                check(memchr(frame.lines[row], '\0', NETMON_FRAME_LINE_CAP) != NULL,
                      "rendered line is terminated");
                check(asset_text_width(font, frame.lines[row]) <= 72,
                      "rendered FS4 line fits the 72-pixel overlay");
            }
        }
    }

    strcpy(control.status, "123456789012");
    for (size_t i = 0u; i < netmon_registry_count(); i++) {
        const netmon_page_descriptor_t *page = netmon_registry_at(i);
        if (page == NULL ||
            (page->behavior_flags & NETMON_PAGE_EDITABLE) == 0u ||
            !netmon_page_supported(page, caps)) {
            continue;
        }
        netmon_frame_t frame;
        netmon_format_frame(page, 0u, caps, 1200u, &local, &modem,
                            &control, &frame);
        check(asset_text_width(font, frame.lines[3]) <= 72,
              "control status overlay fits the 72-pixel overlay");
    }
}

static void test_state_and_absence_rendering(void) {
    netmon_local_diag_snapshot_t local;
    modem_diag_snapshot_t modem;
    netmon_control_snapshot_t control;
    netmon_frame_t frame;
    populate_snapshots(&local, &modem, &control);
    uint32_t caps = supported_caps();
    modem_diag_group_meta_t *serving =
        &modem.group[MODEM_DIAG_GROUP_SERVING];

    serving->sequence = 0u;
    serving->state = MODEM_DIAG_STATE_PENDING;
    netmon_format_frame(netmon_registry_find(1u), 0u, caps, 1200u,
                        &local, &modem, &control, &frame);
    check(strcmp(frame.lines[1], "READING...") == 0 &&
              frame.freshness_marker == ' ',
          "pending group with no sample reports READING");

    serving->state = MODEM_DIAG_STATE_ERROR;
    serving->last_error = MODEM_DIAG_ERROR_MALFORMED;
    serving->consecutive_failures = 1u;
    netmon_format_frame(netmon_registry_find(1u), 0u, caps, 1200u,
                        &local, &modem, &control, &frame);
    check(strcmp(frame.lines[1], "READING...") == 0,
          "first periodic failure without a sample remains READING");

    serving->consecutive_failures =
        NETMON_QUERY_FAILURE_GRACE_ATTEMPTS - 1u;
    netmon_format_frame(netmon_registry_find(1u), 0u, caps, 1200u,
                        &local, &modem, &control, &frame);
    check(strcmp(frame.lines[1], "READING...") == 0,
          "fourth periodic failure without a sample remains READING");

    serving->consecutive_failures = NETMON_QUERY_FAILURE_GRACE_ATTEMPTS;
    netmon_format_frame(netmon_registry_find(1u), 0u, caps, 1200u,
                        &local, &modem, &control, &frame);
    check(strcmp(frame.lines[1], "BAD DATA") == 0,
          "fifth periodic failure without a sample names its cause");

    modem.group[MODEM_DIAG_GROUP_SMS_CONFIG].sequence = 0u;
    modem.group[MODEM_DIAG_GROUP_SMS_CONFIG].state = MODEM_DIAG_STATE_ERROR;
    modem.group[MODEM_DIAG_GROUP_SMS_CONFIG].last_error =
        MODEM_DIAG_ERROR_TIMEOUT;
    modem.group[MODEM_DIAG_GROUP_SMS_CONFIG].consecutive_failures = 1u;
    netmon_format_frame(netmon_registry_find(35u), 0u, caps, 1200u,
                        &local, &modem, &control, &frame);
    check(strcmp(frame.lines[1], "TIMEOUT") == 0,
          "one-shot query reports its first failure immediately");
    modem.group[MODEM_DIAG_GROUP_SMS_CONFIG].sequence = UINT32_MAX;
    modem.group[MODEM_DIAG_GROUP_SMS_CONFIG].state = MODEM_DIAG_STATE_FRESH;
    modem.group[MODEM_DIAG_GROUP_SMS_CONFIG].last_error =
        MODEM_DIAG_ERROR_NONE;
    modem.group[MODEM_DIAG_GROUP_SMS_CONFIG].consecutive_failures = 0u;

    modem_diag_group_meta_t *policy =
        &modem.group[MODEM_DIAG_GROUP_RADIO_POLICY];
    policy->sequence = 1u;
    policy->state = MODEM_DIAG_STATE_STALE;
    policy->last_error = MODEM_DIAG_ERROR_TIMEOUT;
    policy->consecutive_failures = 1u;
    policy->last_success_ms = 1000u;
    netmon_format_frame(netmon_registry_find(8u), 0u, caps, 62000u,
                        &local, &modem, &control, &frame);
    check(frame.freshness_marker == '!' &&
              strcmp(frame.lines[1], "TIMEOUT") != 0,
          "periodic policy refresh keeps its cache marked during retry grace");
    policy->consecutive_failures = NETMON_QUERY_FAILURE_GRACE_ATTEMPTS;
    netmon_format_frame(netmon_registry_find(8u), 0u, caps, 62000u,
                        &local, &modem, &control, &frame);
    check(strcmp(frame.lines[1], "TIMEOUT") == 0 &&
              frame.freshness_marker == ' ',
          "fifth policy failure replaces cached values with the cause");
    policy->state = MODEM_DIAG_STATE_FRESH;
    policy->last_error = MODEM_DIAG_ERROR_NONE;
    policy->consecutive_failures = 0u;
    policy->last_success_ms = 1000u;

    local.updated_ms = 1000u;
    local.core1.heartbeat_ms = 1001u;
    netmon_format_frame(netmon_registry_find(75u), 0u, caps, 1200u,
                        &local, &modem, &control, &frame);
    check(strcmp(frame.lines[1], "HB 0ms") == 0,
          "cross-core heartbeat skew cannot wrap into a false huge age");

    const netmon_page_descriptor_t *storage = netmon_registry_find(76u);
    check(netmon_frame_count(storage, &modem) == 8u, "partition page has eight frames");
    netmon_format_frame(storage, 0u, caps, 1200u, &local, &modem, &control, &frame);
    check_lines(&frame, "SYS 12/64K", "USR 120/384K", "FREE 264K", "RSV 32K",
                "partition totals and physical free space");
    netmon_format_frame(storage, 1u, caps, 1200u, &local, &modem, &control, &frame);
    check_lines(&frame, "CONTACTS KiB", "USE 32/64", "FILES 500", "DATA 25K",
                "category budget, files and data are distinct");
    netmon_format_frame(storage, 6u, caps, 1200u, &local, &modem, &control, &frame);
    check_lines(&frame, "SMS OK", "IN500 OUT500", "PART 64 Q8", "CLOCK OK",
                "message queue and retention health");
    netmon_format_frame(storage, 7u, caps, 1200u, &local, &modem, &control, &frame);
    check_lines(&frame, "SMS CLEANUP", "E 123", "F 456", "L 7",
                "local cleanup counters");
    local.partitions.valid = false;
    netmon_format_frame(storage, 0u, caps, 1200u, &local, &modem, &control, &frame);
    check(strcmp(frame.lines[0], "FS NOT READY") == 0, "failed sample never displays zero usage");
    local.partitions.valid = true;
    local.updated_ms = 18000u;
    netmon_format_frame(storage, 0u, caps, 18000u, &local, &modem, &control, &frame);
    check(strcmp(frame.lines[0], "FS STALE") == 0, "deferred sampling is visibly stale");
    local.updated_ms = 1000u;

    serving->sequence = 1u;
    serving->state = MODEM_DIAG_STATE_PENDING;
    serving->last_error = MODEM_DIAG_ERROR_NONE;
    serving->consecutive_failures = 0u;
    serving->last_success_ms = 1000u;
    netmon_format_frame(netmon_registry_find(1u), 0u, caps, 1200u,
                        &local, &modem, &control, &frame);
    check(strcmp(frame.lines[0], "LTE B65535") == 0 &&
              frame.freshness_marker == '?',
          "pending refresh keeps an existing payload with a question marker");

    serving->last_error = MODEM_DIAG_ERROR_TIMEOUT;
    serving->consecutive_failures = 2u;
    netmon_format_frame(netmon_registry_find(1u), 0u, caps, 1200u,
                        &local, &modem, &control, &frame);
    check(strcmp(frame.lines[0], "LTE B65535") == 0 &&
              frame.freshness_marker == '!',
          "retry in flight keeps the prior failure marker stable");

    serving->state = MODEM_DIAG_STATE_STALE;
    serving->consecutive_failures = 1u;
    netmon_format_frame(netmon_registry_find(1u), 0u, caps, 1200u,
                        &local, &modem, &control, &frame);
    check(strcmp(frame.lines[0], "LTE B65535") == 0 &&
              frame.freshness_marker == '!',
          "first failed refresh keeps the cached payload with a warning");

    serving->consecutive_failures =
        NETMON_QUERY_FAILURE_GRACE_ATTEMPTS - 1u;
    netmon_format_frame(netmon_registry_find(1u), 0u, caps, 1200u,
                        &local, &modem, &control, &frame);
    check(strcmp(frame.lines[0], "LTE B65535") == 0 &&
              frame.freshness_marker == '!',
          "fourth failed refresh remains inside the display grace");

    serving->consecutive_failures = NETMON_QUERY_FAILURE_GRACE_ATTEMPTS;
    netmon_format_frame(netmon_registry_find(1u), 0u, caps, 1200u,
                        &local, &modem, &control, &frame);
    check(strcmp(frame.lines[1], "TIMEOUT") == 0 &&
              frame.lines[0][0] == '\0' &&
              frame.freshness_marker == ' ',
          "fifth failed refresh hides the cache and names its cause");

    serving->state = MODEM_DIAG_STATE_PENDING;
    netmon_format_frame(netmon_registry_find(1u), 0u, caps, 1200u,
                        &local, &modem, &control, &frame);
    check(strcmp(frame.lines[1], "TIMEOUT") == 0,
          "post-threshold retry preserves the last cause instead of blinking");

    serving->state = MODEM_DIAG_STATE_STALE;
    serving->last_error = MODEM_DIAG_ERROR_CANCELLED;
    serving->consecutive_failures = 0u;
    netmon_format_frame(netmon_registry_find(1u), 0u, caps, 1200u,
                        &local, &modem, &control, &frame);
    check(strcmp(frame.lines[1], "CANCELLED") == 0 &&
              frame.lines[0][0] == '\0' &&
              frame.freshness_marker == ' ',
          "cancellation never presents cached values as a failed refresh");

    serving->state = MODEM_DIAG_STATE_FRESH;
    serving->last_error = MODEM_DIAG_ERROR_NONE;
    serving->consecutive_failures = 0u;
    netmon_format_frame(netmon_registry_find(1u), 0u, caps, 11001u,
                        &local, &modem, &control, &frame);
    check(strcmp(frame.lines[1], "STALE DATA") == 0,
          "age cap eventually hides a payload independently of retries");

    modem.calls.projected_state = MODEM_CALL_ACTIVE;
    netmon_format_frame(netmon_registry_find(1u), 0u, caps, 1200u,
                        &local, &modem, &control, &frame);
    check(strcmp(frame.lines[1], "QUERY PAUSED") == 0,
          "call-time query suppression cannot masquerade as live data");
    modem.calls.projected_state = MODEM_CALL_IDLE;

    netmon_format_frame(netmon_registry_find(1u), 0u,
                        NETMON_CAP_REVB2, 1200u,
                        &local, &modem, &control, &frame);
    check(strcmp(frame.lines[1], "UNSUP") == 0,
          "known page without capability reports UNSUP");

    modem.group[MODEM_DIAG_GROUP_SERVING].state = MODEM_DIAG_STATE_FRESH;
    modem.group[MODEM_DIAG_GROUP_SERVING].present_fields =
        MODEM_DIAG_SERVING_RAT | MODEM_DIAG_SERVING_CHANNEL |
        MODEM_DIAG_SERVING_BAND;
    netmon_format_frame(netmon_registry_find(2u), 0u, caps, 1200u,
                        &local, &modem, &control, &frame);
    check(strcmp(frame.lines[0], "RSSI --") == 0,
          "absent RSSI is not rendered as zero");
    check(strcmp(frame.lines[2], "RSRQ --") == 0,
          "absent RSRQ is not rendered as zero");

    modem.group[MODEM_DIAG_GROUP_RADIO_POLICY].present_fields =
        MODEM_DIAG_POLICY_CFUN | MODEM_DIAG_POLICY_COPS;
    netmon_format_frame(netmon_registry_find(7u), 0u, caps, 1200u,
                        &local, &modem, &control, &frame);
    check(strcmp(frame.lines[0], "ENS --") == 0,
          "absent optional ENS is not rendered as zero");

    modem.sms.mwi_enabled = 1u;
    modem.sms.message_waiting
        .category[MODEM_MESSAGE_WAITING_VOICE_LINE_1] =
        (modem_message_waiting_state_t){.active = true, .count = 7u};
    modem.sms.message_waiting.category[MODEM_MESSAGE_WAITING_FAX] =
        (modem_message_waiting_state_t){.active = true, .count = 2u};
    modem.sms.message_waiting.category[MODEM_MESSAGE_WAITING_EMAIL] =
        (modem_message_waiting_state_t){.active = true, .count = 3u};
    netmon_format_frame(netmon_registry_find(36u), 0u, caps, 1200u,
                        &local, &modem, &control, &frame);
    check(strcmp(frame.lines[0], "MWI EN1") == 0 &&
              strcmp(frame.lines[1], "V 1/7") == 0 &&
              strcmp(frame.lines[2], "FAX 1/2") == 0 &&
              strcmp(frame.lines[3], "MAIL 1/3") == 0,
          "MWI page separates voice, fax, and e-mail state");

    modem.group[MODEM_DIAG_GROUP_SMS_CONFIG].present_fields =
        MODEM_DIAG_SMS_CSMS | MODEM_DIAG_SMS_CNMI |
        MODEM_DIAG_SMS_CSMP | MODEM_DIAG_SMS_CSCA |
        MODEM_DIAG_SMS_CSDH;
    netmon_format_frame(netmon_registry_find(36u), 0u, caps, 1200u,
                        &local, &modem, &control, &frame);
    check(strcmp(frame.lines[0], "MWI --") == 0,
          "absent optional MWI is not rendered as inactive");

    modem.group[MODEM_DIAG_GROUP_RADIO_POLICY].present_fields = UINT64_MAX;
    netmon_format_frame(netmon_registry_find(8u), 0u, caps, 1200u,
                        &local, &modem, &control, &frame);
    check(strncmp(frame.lines[0], "WS46", 4u) == 0,
          "band-policy frame zero remains intact");
    netmon_format_frame(netmon_registry_find(8u), 1u, caps, 1200u,
                        &local, &modem, &control, &frame);
    check(strcmp(frame.lines[0], "BND LTE") == 0,
          "whole-frame rotation selects the next logical frame");
}

static void test_context_labels_and_local_units(void) {
    netmon_local_diag_snapshot_t local;
    modem_diag_snapshot_t modem;
    netmon_control_snapshot_t control;
    netmon_frame_t frame;
    populate_snapshots(&local, &modem, &control);
    uint32_t caps = supported_caps();

    strcpy(modem.radio_policy.cops_operator, "Dark Star Mobile");
    netmon_format_frame(netmon_registry_find(7u), 0u, caps, 1200u,
                        &local, &modem, &control, &frame);
    check(strcmp(frame.lines[3], "COPS NAME") == 0 &&
              netmon_frame_count(netmon_registry_find(7u), &modem) == 2u,
          "policy page gives the selected operator its own frame");
    netmon_format_frame(netmon_registry_find(7u), 1u, caps, 1200u,
                        &local, &modem, &control, &frame);
    check(strcmp(frame.lines[0], "COPS NAME") == 0 &&
              strcmp(frame.lines[1], "Dark Star Mo") == 0 &&
              strcmp(frame.lines[2], "bile") == 0,
          "policy operator renders in full across bounded lines");

    local.measurement_delta_nah = -1234567;
    netmon_format_frame(netmon_registry_find(41u), 0u, caps, 1200u,
                        &local, &modem, &control, &frame);
    check(strcmp(frame.lines[1], "dQ -1.234mAh") == 0,
          "coulomb delta uses battery-scale mAh without losing uAh resolution");

    modem.transport.rx_bytes = 1000u;
    const netmon_page_descriptor_t *uart = netmon_registry_find(22u);
    netmon_format_frame(uart, 0u, caps, 1200u, &local, &modem,
                        &control, &frame);
    check(netmon_frame_count(uart, &modem) == 1u &&
              strcmp(frame.lines[0], "RX 1000") == 0 &&
              strncmp(frame.lines[1], "OVR ", 4u) == 0 &&
              strncmp(frame.lines[3], "CTSdrop ", 8u) == 0,
          "UART page is one transport-only frame");

    local.event_queue_depth = 2u;
    local.event_queue_high_water = 4u;
    local.event_queue_drops = 3u;
    const netmon_page_descriptor_t *queue = netmon_registry_find(54u);
    netmon_format_frame(queue, 0u, caps, 1200u, &local, &modem,
                        &control, &frame);
    check(strcmp(frame.lines[0], "Q 2 HI4") == 0 &&
              strcmp(frame.lines[1], "DROP 3") == 0 &&
              frame.lines[2][0] == '\0' && frame.lines[3][0] == '\0',
          "input queue page no longer duplicates headset evidence");

    local.main_loop_last_us = 100u;
    local.main_loop_max_us = 200u;
    local.main_loop_budget_us = 500u;
    local.main_loop_over_budget = 1u;
    const netmon_page_descriptor_t *loop = netmon_registry_find(74u);
    netmon_format_frame(loop, 0u, caps, 1200u, &local, &modem,
                        &control, &frame);
    check(netmon_frame_count(loop, &modem) == 1u &&
              strncmp(frame.lines[0], "LOOP ", 5u) == 0 &&
              strncmp(frame.lines[3], "OVER ", 5u) == 0,
          "main-loop page is one focused timing frame");

    const netmon_page_descriptor_t *core = netmon_registry_find(75u);
    netmon_format_frame(core, 3u, caps, 1200u, &local, &modem,
                        &control, &frame);
    check(netmon_frame_count(core, &modem) == 4u &&
              strcmp(frame.lines[0], "C0 2368/1728") == 0 &&
              strcmp(frame.lines[1], "C1 880/3216") == 0 &&
              strcmp(frame.lines[2], "G0 OK G1 OK") == 0 &&
              strcmp(frame.lines[3], "used/margin") == 0,
          "core runtime page exposes both stack high-water guards");

    modem.runtime.shutdown_stage = 4u;
    modem.runtime.shutdown_terminal_fault = true;
    modem.runtime.graceful_shutdown_pulses = 2u;
    modem.runtime.emergency_shutdown_pulses = 1u;
    modem.runtime.terminal_shutdown_failures = 1u;
    const netmon_page_descriptor_t *runtime = netmon_registry_find(28u);
    netmon_format_frame(runtime, 2u, caps, 1200u, &local, &modem,
                        &control, &frame);
    check(netmon_frame_count(runtime, &modem) == 3u &&
              strcmp(frame.lines[0], "SHDN 4 F1") == 0 &&
              strcmp(frame.lines[1], "HW 2") == 0 &&
              strcmp(frame.lines[2], "EMERG 1") == 0 &&
              strcmp(frame.lines[3], "TERM 1") == 0,
          "modem runtime page preserves the bounded shutdown postmortem");
}

static void test_control_frames_expose_every_action(void) {
    netmon_local_diag_snapshot_t local;
    modem_diag_snapshot_t modem;
    netmon_control_snapshot_t control;
    netmon_frame_t frame;
    populate_snapshots(&local, &modem, &control);
    control.status[0] = '\0';
    uint32_t caps = supported_caps();

    const netmon_page_descriptor_t *backlight = netmon_registry_find(89u);
    check(netmon_frame_count(backlight, &modem) == 2u,
          "backlight calibration uses two complete frames");
    netmon_format_frame(backlight, 0u, caps, 1200u, &local, &modem,
                        &control, &frame);
    check(strcmp(frame.lines[0], "LIGHT 73%") == 0 &&
              strcmp(frame.lines[1], "SAVED 65%") == 0 &&
              strcmp(frame.lines[2], "1/3 5%") == 0 &&
              strcmp(frame.lines[3], "4/6 1%") == 0,
          "page 89 renders live/stored duty and both adjustment scales");
    netmon_format_frame(backlight, 1u, caps, 1200u, &local, &modem,
                        &control, &frame);
    check(strcmp(frame.lines[0], "0 REVERT") == 0 &&
              strcmp(frame.lines[1], "* SAVE") == 0 &&
              strcmp(frame.lines[2], "# DEFAULT") == 0,
          "backlight persistence controls are explicit");

    const netmon_page_descriptor_t *charger = netmon_registry_find(92u);
    check(netmon_frame_count(charger, &modem) == 2u,
          "charger controls use two whole frames");
    netmon_format_frame(charger, 0u, caps, 1200u, &local, &modem,
                        &control, &frame);
    check(strcmp(frame.lines[3], "# AUTO") == 0,
          "charger force cleanup is visible");
    netmon_format_frame(charger, 1u, caps, 1200u, &local, &modem,
                        &control, &frame);
    check(strcmp(frame.lines[1], "3 ENABLE") == 0 &&
              strcmp(frame.lines[2], "4 DISABLE") == 0,
          "charger enable controls are visible");

    const netmon_page_descriptor_t *lcd = netmon_registry_find(95u);
    check(netmon_frame_count(lcd, &modem) == 2u,
          "LCD controls use two whole frames");
    netmon_format_frame(lcd, 1u, caps, 1200u, &local, &modem,
                        &control, &frame);
    check(strcmp(frame.lines[1], "0 REVERT") == 0 &&
              strcmp(frame.lines[2], "* SAVE") == 0 &&
              strcmp(frame.lines[3], "# STOCK") == 0,
          "LCD revert, save, and stock controls are visible");
    control.lcd_save_armed = true;
    netmon_format_frame(lcd, 1u, caps, 1200u, &local, &modem,
                        &control, &frame);
    check(strcmp(frame.lines[2], "* CONFIRM") == 0,
          "armed LCD persistence renders an explicit second-press confirmation");
    control.lcd_save_armed = false;
    netmon_format_frame(lcd, 0u, caps, 1200u, &local, &modem,
                        &control, &frame);
    check(strcmp(frame.lines[1], "SAVED --") == 0,
          "missing stored LCD tuple is not rendered as zero");

    control.maintenance.action = MODEM_MAINTENANCE_SCAN_TIMER;
    control.maintenance.state = MODEM_MAINTENANCE_DONE;
    control.maintenance.scan_timer_s = 300u;
    const netmon_page_descriptor_t *scan = netmon_registry_find(97u);
    netmon_format_frame(scan, 0u, caps, 1200u, &local, &modem,
                        &control, &frame);
    check(strcmp(frame.lines[0], "SCAN 300s") == 0 &&
              strcmp(frame.lines[2], "* ARM # READ") == 0,
          "scan control shows verified value and explicit arm/read keys");

    control.maintenance.action = MODEM_MAINTENANCE_BAND_TEST;
    control.maintenance.state = MODEM_MAINTENANCE_ACTIVE;
    control.maintenance.band_preset = 4u;
    control.maintenance.band_test_active = true;
    const netmon_page_descriptor_t *band = netmon_registry_find(98u);
    netmon_format_frame(band, 0u, caps, 1200u, &local, &modem,
                        &control, &frame);
    check(strcmp(frame.lines[0], "BAND B12") == 0 &&
              strcmp(frame.lines[3], "0/# RESTORE") == 0,
          "temporary band page names the active preset and both restore keys");
    control.maintenance.state = MODEM_MAINTENANCE_RESTORING;
    netmon_format_frame(band, 0u, caps, 1200u, &local, &modem,
                        &control, &frame);
    check(strcmp(frame.lines[0], "RESTORE B12") == 0,
          "temporary band page distinguishes restoration from active policy");
    control.maintenance.state = MODEM_MAINTENANCE_DONE;
    control.maintenance.band_preset = 0u;
    control.maintenance.band_test_active = false;
    netmon_format_frame(band, 0u, caps, 1200u, &local, &modem,
                        &control, &frame);
    check(strcmp(frame.lines[0], "BAND BASE") == 0,
          "completed restoration cannot display the retired test band");

    control.maintenance.action = MODEM_MAINTENANCE_ANTENNA;
    control.maintenance.state = MODEM_MAINTENANCE_ACTIVE;
    control.maintenance.antenna_active = true;
    control.maintenance.antenna_rf = 4u;
    modem.group[MODEM_DIAG_GROUP_TUNER].state = MODEM_DIAG_STATE_ERROR;
    modem.group[MODEM_DIAG_GROUP_TUNER].last_error =
        MODEM_DIAG_ERROR_TIMEOUT;
    const netmon_page_descriptor_t *antenna = netmon_registry_find(99u);
    netmon_format_frame(antenna, 0u, caps, 1200u, &local, &modem,
                        &control, &frame);
    check(strcmp(frame.lines[0], "ANT RF4") == 0 &&
              strcmp(frame.lines[1], "ACTIVE") == 0,
          "active maintenance remains operable when its background query is stale");
}

int main(void) {
    test_page_ownership();
    test_representative_exact_frames();
    test_modem_gates_and_control_status();
    test_every_frame_fits();
    test_state_and_absence_rendering();
    test_context_labels_and_local_units();
    test_control_frames_expose_every_action();
    if (s_failures != 0) {
        fprintf(stderr, "%d Net Monitor render test(s) failed\n", s_failures);
        return 1;
    }
    puts("Net Monitor render tests passed");
    return 0;
}

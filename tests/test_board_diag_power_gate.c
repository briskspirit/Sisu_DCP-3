/* Power-key battery admission must fail closed while LTC voltage is unknown.
 * Fresh VIN is necessary but not sufficient: /CE readback and BQ ACTIVE must
 * also prove that the attached charger can recover the pack. */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "hal/battery_hal.h"
#include "hal/ltc2959_hal.h"
#include "services/board_diag_service.h"
#include "services/charger_control_service.h"
#include "services/timebase.h"

static int s_failures;
static bool s_fresh_charger_present;
static unsigned s_fresh_charger_reads;
static battery_power_on_gate_t s_production_gate;
static unsigned s_production_gate_reads;
static bool s_production_gate_charger;
static ltc2959_snapshot_t s_ltc;
static bool s_cached_charger_connected = true;
static bool s_charge_status_valid = true;
static battery_charge_status_t s_charge_status = BATTERY_CHARGE_ACTIVE;
static bool s_charger_enabled;

static void check(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        s_failures++;
    }
}

uint32_t time_ms(void) {
    return 1234u;
}

void ltc2959_hal_get_snapshot(ltc2959_snapshot_t *out) {
    *out = s_ltc;
}

bool battery_hal_charge_status_valid(void) {
    return s_charge_status_valid;
}

battery_charge_status_t battery_hal_charge_status(void) {
    return s_charge_status;
}

bool tca8418_hal_set_charger_enabled(bool enabled) {
    s_charger_enabled = enabled;
    return true;
}

bool tca8418_hal_get_charger_enabled(bool *enabled) {
    *enabled = s_charger_enabled;
    return true;
}

battery_charger_state_t battery_hal_charger_state_from_inputs(
    bool charger_present, battery_charge_status_t status) {
    if (!charger_present) {
        return BATTERY_CHARGER_DETACHED;
    }
    if (status == BATTERY_CHARGE_ACTIVE) {
        return BATTERY_CHARGER_ACTIVE;
    }
    if (status == BATTERY_CHARGE_FAULT) {
        return BATTERY_CHARGER_FAULT;
    }
    return BATTERY_CHARGER_FULL;
}

bool battery_hal_charger_input_present_now(uint32_t now_ms) {
    check(now_ms == 1234u, "fresh charger observation uses the current time");
    s_fresh_charger_reads++;
    return s_fresh_charger_present;
}

bool battery_hal_charger_connected(void) {
    return s_cached_charger_connected;
}

bool battery_hal_valid(void) {
    return false;
}

uint16_t battery_hal_millivolts(void) {
    return 0u;
}

uint16_t battery_hal_millivolts_raw(void) {
    return 0u;
}

uint16_t battery_hal_millivolts_fresh(void) {
    return 0u;
}

battery_power_on_gate_t battery_hal_power_on_gate_from_mv(uint16_t mv,
                                                          bool charger) {
    return charger || mv >= VBAT_POWERON_MIN_MV
        ? BATTERY_POWER_ON_OK
        : BATTERY_POWER_ON_REFUSE;
}

battery_power_on_gate_t battery_hal_power_on_gate(bool charger) {
    s_production_gate_reads++;
    s_production_gate_charger = charger;
    return charger ? BATTERY_POWER_ON_OK : s_production_gate;
}

static void test_unknown_battery_requires_fresh_charger_evidence(void) {
    s_production_gate = BATTERY_POWER_ON_REFUSE;
    s_production_gate_reads = 0u;
    s_fresh_charger_present = false;
    s_fresh_charger_reads = 0u;
    check(!board_diag_battery_power_on_allowed(),
          "unknown battery without charger evidence refuses modem startup");
    check(s_fresh_charger_reads == 1u,
          "unknown battery takes one bounded fresh charger observation");

    s_fresh_charger_present = true;
    s_fresh_charger_reads = 0u;
    check(board_diag_battery_power_on_allowed(),
          "fresh charger evidence permits recovery before LTC is ready");
    check(s_fresh_charger_reads == 1u,
          "charger-assisted recovery also uses exactly one observation");
    check(s_production_gate_reads == 2u && !s_production_gate_charger,
          "unknown battery never trusts cached charger presence");

    s_charge_status_valid = false;
    check(!board_diag_battery_power_on_allowed(),
          "fresh VIN without valid BQ ACTIVE evidence cannot bypass");
    s_charge_status_valid = true;
}

static void test_qualified_history_survives_invalid_latest_sample(void) {
    s_production_gate = BATTERY_POWER_ON_OK;
    s_production_gate_reads = 0u;
    s_fresh_charger_present = false;
    s_fresh_charger_reads = 0u;
    check(board_diag_battery_power_on_allowed(),
          "qualified rolling history admits despite an invalid latest sample");
    check(s_production_gate_reads == 1u && !s_production_gate_charger,
          "history admission does not disguise cached charger as evidence");
    check(s_fresh_charger_reads == 0u,
          "qualified history needs no synchronous ADC work");
}

static void test_valid_battery_uses_voltage_gate_without_adc_work(void) {
    s_production_gate = BATTERY_POWER_ON_REFUSE;
    s_fresh_charger_present = false;
    s_fresh_charger_reads = 0u;
    board_diag_debug_force_battery_mv(VBAT_POWERON_MIN_MV - 1u);
    check(!board_diag_battery_power_on_allowed(),
          "known battery below the Nokia floor is refused");
    check(s_fresh_charger_reads == 0u,
          "known battery admission does not resample charger ADC");

    board_diag_debug_force_battery_mv(VBAT_POWERON_MIN_MV);
    check(board_diag_battery_power_on_allowed(),
          "known battery at the Nokia floor is admitted");

    board_diag_debug_force_battery_mv(VBAT_POWERON_MIN_MV - 1u);
    board_diag_debug_force_charger_connected(true, 1234u);
    check(board_diag_battery_power_on_allowed(),
          "a known low battery is admitted when charging");
}

static void test_unknown_battery_does_not_trust_stale_cached_charger(void) {
    /* Leave a cached connected value behind while the underlying battery
     * source becomes invalid, then prove the fresh ADC result wins. */
    board_diag_debug_clear_battery_force();
    board_diag_debug_clear_charger_force();
    s_production_gate = BATTERY_POWER_ON_REFUSE;
    s_fresh_charger_present = false;
    s_fresh_charger_reads = 0u;
    check(!board_diag_battery_power_on_allowed(),
          "unknown battery ignores stale cached charger presence");
    check(s_fresh_charger_reads == 1u,
          "stale cached presence is replaced by one fresh ADC observation");
}

static void test_supply_failure_needs_fresh_marginal_pack_evidence(void) {
    board_diag_debug_force_charger_connected(false, 1234u);
    board_diag_debug_force_battery_mv(VBAT_LOW_MV - 1u);
    check(board_diag_battery_supply_failure_indicates_empty(),
          "fresh terminal evidence below 2320 mV qualifies a supply collapse");

    board_diag_debug_force_battery_mv(VBAT_LOW_MV);
    check(!board_diag_battery_supply_failure_indicates_empty(),
          "the inclusive healthy boundary keeps a PG fault modem-specific");

    board_diag_debug_force_battery_mv(VBAT_POWERON_MIN_MV);
    board_diag_debug_force_charger_connected(true, 1234u);
    check(board_diag_battery_supply_failure_indicates_empty(),
          "physical charger presence cannot hide a marginal pack collapse");

    board_diag_debug_clear_battery_force();
    board_diag_debug_clear_charger_force();
    check(!board_diag_battery_supply_failure_indicates_empty(),
          "unknown battery evidence cannot diagnose a supply failure as empty");
}

static void test_learning_observation_is_raw_and_override_safe(void) {
    s_cached_charger_connected = false;
    board_diag_debug_force_charger_connected(false, 1234u);
    board_diag_debug_clear_charger_force();
    memset(&s_ltc, 0, sizeof(s_ltc));
    s_ltc.sample_valid = true;
    s_ltc.continuity_valid = true;
    s_ltc.current_polarity_verified = true;
    s_ltc.sample_sequence = UINT32_MAX;
    s_ltc.gauge_session = 7u;
    s_ltc.acr_raw = UINT32_C(0x89abcdef);
    s_ltc.session_delta_nah = -INT64_C(123456789);
    s_ltc.voltage_mv = 2512u;
    s_ltc.current_ua = -345678;
    s_ltc.temperature_mdegc = 23456;

    battery_learning_observation_t observation;
    board_diag_get_battery_learning_observation(4321u, &observation);
    check(observation.now_ms == 4321u &&
              observation.sample_sequence == UINT32_MAX &&
              observation.gauge_session == 7u &&
              observation.acr_raw == UINT32_C(0x89abcdef) &&
              observation.session_delta_nah == -INT64_C(123456789) &&
              observation.terminal_mv == 2512u &&
              observation.current_ua == -345678 &&
              observation.temperature_mdegc == 23456,
          "learning observation preserves raw LTC evidence");
    check(observation.sample_valid && observation.current_valid &&
              observation.continuity_valid && observation.authoritative &&
              !observation.charger_connected && !observation.charge_active,
          "physical learning observation publishes honest qualification");

    board_diag_debug_force_battery_mv(2400u);
    board_diag_get_battery_learning_observation(4322u, &observation);
    check(!observation.authoritative,
          "debug-forced battery evidence cannot contaminate learning");
    board_diag_debug_clear_battery_force();
}

int main(void) {
    charger_control_service_init(0u, 0u);
    test_unknown_battery_requires_fresh_charger_evidence();
    test_qualified_history_survives_invalid_latest_sample();
    test_valid_battery_uses_voltage_gate_without_adc_work();
    test_unknown_battery_does_not_trust_stale_cached_charger();
    test_supply_failure_needs_fresh_marginal_pack_evidence();
    test_learning_observation_is_raw_and_override_safe();
    if (s_failures != 0) {
        fprintf(stderr, "%d board power-gate test(s) failed\n", s_failures);
        return 1;
    }
    puts("board power-gate tests passed");
    return 0;
}

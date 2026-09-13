#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "hal/accessory_hal.h"
#include "services/board_diag_service.h"
#include "services/backlight_calibration.h"
#include "services/backlight_service.h"
#include "services/core1_services.h"
#include "services/lcd_calibration.h"
#include "services/netmon_control_service.h"
#include "services/netmon_diag_service.h"

static int s_failures;
static board_diag_snapshot_t s_board;
static accessory_debug_override_t s_headset_override;
static backlight_calibration_status_t s_backlight;
static lcd_calibration_status_t s_lcd;
static netmon_local_diag_snapshot_t s_local;
static core1_cmd_t s_last_command;
static uint32_t s_command_count;
static uint32_t s_measurement_resets;
static bool s_backlight_override;
static bool s_vibra_test;
static modem_maintenance_snapshot_t s_maintenance;
static bool s_maintenance_supported;
static bool s_maintenance_admit;
static uint32_t s_maintenance_cancel_count;
static bool s_charger_control_ok;

static void check(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        s_failures++;
    }
}

int32_t time_diff_ms(uint32_t a, uint32_t b) {
    return (int32_t)(a - b);
}

void core1_post_command(core1_cmd_t cmd, uint16_t arg) {
    (void)arg;
    s_last_command = cmd;
    s_command_count++;
}

void board_diag_get_snapshot(board_diag_snapshot_t *out) {
    *out = s_board;
}

void board_diag_debug_force_battery_mv(uint16_t mv) {
    s_board.battery_forced = true;
    s_board.battery_mv = mv;
}

void board_diag_debug_clear_battery_force(void) {
    s_board.battery_forced = false;
}

void board_diag_debug_force_charger_connected(bool connected,
                                               uint32_t now_ms) {
    (void)now_ms;
    s_board.charger_forced = true;
    s_board.charger_connected = connected;
}

void board_diag_debug_clear_charger_force(void) {
    s_board.charger_forced = false;
}

bool board_diag_debug_set_charger_enabled(bool enabled) {
    if (!s_charger_control_ok) {
        s_board.charger_enable_valid = false;
        return false;
    }
    if (enabled) {
        s_board.charger_inhibit_owner_mask &= (uint8_t)~(1u << 1);
    } else {
        s_board.charger_inhibit_owner_mask |= (1u << 1);
    }
    s_board.charger_enabled_requested =
        s_board.charger_inhibit_owner_mask == 0u;
    s_board.charger_enabled = s_board.charger_enabled_requested;
    s_board.charger_enable_valid = true;
    return true;
}

bool board_diag_restore_charger_default(void) {
    return board_diag_debug_set_charger_enabled(true);
}

bool board_diag_debug_set_tps63020_pwm_mode(bool enabled) {
    s_board.tps63020_pwm_mode = enabled;
    return enabled;
}

uint8_t backlight_service_level_percent(void) {
    return s_backlight.active_percent;
}

bool backlight_calibration_preview(uint8_t level_percent) {
    if (level_percent < BACKLIGHT_LEVEL_MIN_PERCENT ||
        level_percent > BACKLIGHT_LEVEL_MAX_PERCENT) {
        return false;
    }
    s_backlight.active_percent = level_percent;
    return true;
}

bool backlight_calibration_preview_default(void) {
    return backlight_calibration_preview(BACKLIGHT_LEVEL_DEFAULT_PERCENT);
}

bool backlight_calibration_revert_stored(void) {
    return backlight_calibration_preview(
        s_backlight.stored_valid ? s_backlight.stored_percent
                                 : BACKLIGHT_LEVEL_DEFAULT_PERCENT);
}

bool backlight_calibration_save_active(void) {
    s_backlight.stored_valid = true;
    s_backlight.stored_percent = s_backlight.active_percent;
    return true;
}

void backlight_calibration_get_status(
    backlight_calibration_status_t *out_status) {
    *out_status = s_backlight;
}

void board_diag_debug_set_backlight_override(bool active, bool on) {
    s_backlight_override = active && on;
}

void board_diag_debug_set_vibra_test(bool enabled) {
    s_vibra_test = enabled;
}

uint8_t board_diag_vibra_strength(void) {
    return 5u;
}

void accessory_hal_debug_set_override(accessory_debug_override_t override) {
    s_headset_override = override;
}

accessory_debug_override_t accessory_hal_debug_get_override(void) {
    return s_headset_override;
}

bool lcd_calibration_preview_tuning(uint8_t vop,
                                    uint8_t temperature_coefficient,
                                    uint8_t bias_system) {
    s_lcd.vop = vop;
    s_lcd.temperature_coefficient = temperature_coefficient;
    s_lcd.bias_system = bias_system;
    return true;
}

bool lcd_calibration_preview_stock(void) {
    return lcd_calibration_preview_tuning(
        LCD_CALIBRATION_STOCK_VOP,
        LCD_CALIBRATION_STOCK_TEMPERATURE_COEFFICIENT,
        LCD_CALIBRATION_STOCK_BIAS_SYSTEM);
}

bool lcd_calibration_revert_stored(void) {
    return lcd_calibration_preview_tuning(
        s_lcd.stored_vop,
        s_lcd.stored_temperature_coefficient,
        s_lcd.stored_bias_system);
}

bool lcd_calibration_save_active(void) {
    s_lcd.stored_vop_valid = true;
    s_lcd.stored_vop = s_lcd.vop;
    s_lcd.stored_temperature_coefficient =
        s_lcd.temperature_coefficient;
    s_lcd.stored_bias_system = s_lcd.bias_system;
    return true;
}

void lcd_calibration_get_status(lcd_calibration_status_t *out_status) {
    *out_status = s_lcd;
}

void netmon_diag_service_reset_measurement_window(uint32_t now_ms) {
    (void)now_ms;
    s_measurement_resets++;
    s_local.measurement_generation++;
}

void netmon_diag_service_get_snapshot(netmon_local_diag_snapshot_t *out) {
    *out = s_local;
}

uint32_t netmon_diag_service_measurement_generation(void) {
    return s_local.measurement_generation;
}

bool modem_service_maintenance_supported(void) {
    return s_maintenance_supported;
}

bool modem_service_maintenance_request_scan_timer(uint16_t seconds) {
    if (!s_maintenance_admit) return false;
    s_maintenance.action = MODEM_MAINTENANCE_SCAN_TIMER;
    s_maintenance.state = MODEM_MAINTENANCE_PENDING;
    s_maintenance.scan_timer_s = seconds;
    s_maintenance.sequence++;
    return true;
}

bool modem_service_maintenance_read_scan_timer(void) {
    return modem_service_maintenance_request_scan_timer(
        s_maintenance.scan_timer_s);
}

bool modem_service_maintenance_start_band_test(uint8_t preset) {
    if (!s_maintenance_admit) return false;
    s_maintenance.action = MODEM_MAINTENANCE_BAND_TEST;
    s_maintenance.state = MODEM_MAINTENANCE_PENDING;
    s_maintenance.band_preset = preset;
    s_maintenance.sequence++;
    return true;
}

bool modem_service_maintenance_restore_band(void) {
    if (!s_maintenance_admit ||
        s_maintenance.action != MODEM_MAINTENANCE_BAND_TEST) return false;
    s_maintenance.state = MODEM_MAINTENANCE_RESTORING;
    s_maintenance.sequence++;
    return true;
}

bool modem_service_maintenance_enter_antenna(void) {
    if (!s_maintenance_admit) return false;
    s_maintenance.action = MODEM_MAINTENANCE_ANTENNA;
    s_maintenance.state = MODEM_MAINTENANCE_PENDING;
    s_maintenance.sequence++;
    return true;
}

bool modem_service_maintenance_select_antenna(uint8_t rf_state) {
    if (!s_maintenance_admit ||
        s_maintenance.action != MODEM_MAINTENANCE_ANTENNA ||
        s_maintenance.state != MODEM_MAINTENANCE_ACTIVE) return false;
    s_maintenance.antenna_rf = rf_state;
    s_maintenance.sequence++;
    return true;
}

bool modem_service_maintenance_exit_antenna(void) {
    if (!s_maintenance_admit ||
        s_maintenance.action != MODEM_MAINTENANCE_ANTENNA) return false;
    s_maintenance.state = MODEM_MAINTENANCE_RESTORING;
    s_maintenance.sequence++;
    return true;
}

void modem_service_maintenance_cancel(void) {
    s_maintenance_cancel_count++;
}

void modem_service_get_maintenance_snapshot(
    modem_maintenance_snapshot_t *out) {
    *out = s_maintenance;
}

static void reset_fixture(void) {
    memset(&s_board, 0, sizeof(s_board));
    memset(&s_backlight, 0, sizeof(s_backlight));
    memset(&s_lcd, 0, sizeof(s_lcd));
    memset(&s_local, 0, sizeof(s_local));
    s_board.battery_mv = 2650u;
    s_board.charger_enabled_requested = true;
    s_board.charger_enabled = true;
    s_board.charger_enable_valid = true;
    s_backlight.active_percent = 70u;
    s_backlight.stored_valid = true;
    s_backlight.stored_percent = 70u;
    s_lcd.stored_vop_valid = true;
    s_lcd.stored_vop = 54u;
    s_lcd.stored_temperature_coefficient = 1u;
    s_lcd.stored_bias_system = 4u;
    s_lcd.vop = s_lcd.stored_vop;
    s_lcd.temperature_coefficient = s_lcd.stored_temperature_coefficient;
    s_lcd.bias_system = s_lcd.stored_bias_system;
    s_headset_override = ACCESSORY_DEBUG_OVERRIDE_AUTO;
    s_last_command = CORE1_CMD_NONE;
    s_command_count = 0u;
    s_measurement_resets = 0u;
    s_backlight_override = false;
    s_vibra_test = false;
    memset(&s_maintenance, 0, sizeof(s_maintenance));
    s_maintenance.sequence = 1u;
    s_maintenance.state = MODEM_MAINTENANCE_IDLE;
    s_maintenance_supported = true;
    s_maintenance_admit = true;
    s_maintenance_cancel_count = 0u;
    s_charger_control_ok = true;
    netmon_control_service_init(0u);
}

static netmon_control_snapshot_t snapshot(void) {
    netmon_control_snapshot_t value;
    netmon_control_service_get_snapshot(&value);
    return value;
}

static void test_bounded_outputs(void) {
    reset_fixture();
    netmon_control_service_set_active(NETMON_ACTION_OUTPUT_TEST, 100u);
    check(netmon_control_service_execute(NETMON_ACTION_OUTPUT_TEST,
                                         NETMON_CONTROL_KEY_1, 100u),
          "backlight test admitted");
    check(s_backlight_override && snapshot().output == NETMON_OUTPUT_BACKLIGHT,
          "backlight output is owned by the bounded action");
    netmon_control_service_poll(5099u);
    check(s_backlight_override, "output remains active before its deadline");
    netmon_control_service_poll(5100u);
    check(!s_backlight_override && snapshot().output == NETMON_OUTPUT_NONE,
          "output stops exactly at its deadline");

    check(netmon_control_service_execute(NETMON_ACTION_OUTPUT_TEST,
                                         NETMON_CONTROL_KEY_2, 6000u),
          "vibra test admitted");
    check(s_vibra_test &&
              s_last_command == CORE1_CMD_AUDIO_DEBUG_VIBRA_TEST,
          "vibra uses the owner service command");
    netmon_control_service_set_active(NETMON_ACTION_NONE, 6001u);
    check(!s_vibra_test && s_last_command == CORE1_CMD_AUDIO_DEBUG_STOP,
          "leaving the page stops a live vibra test");

    netmon_control_service_set_active(NETMON_ACTION_OUTPUT_TEST,
                                      UINT32_MAX - 1000u);
    check(netmon_control_service_execute(NETMON_ACTION_OUTPUT_TEST,
                                         NETMON_CONTROL_KEY_1,
                                         UINT32_MAX - 1000u),
          "wrapped-deadline output admitted");
    netmon_control_service_poll(3998u);
    check(s_backlight_override,
          "wrapped output deadline does not expire one millisecond early");
    netmon_control_service_poll(3999u);
    check(!s_backlight_override,
          "wrapped output deadline expires at the intended instant");
}

static void test_override_cleanup(void) {
    reset_fixture();
    netmon_control_service_set_active(NETMON_ACTION_BATTERY_SIM, 1u);
    check(netmon_control_service_execute(NETMON_ACTION_BATTERY_SIM,
                                         NETMON_CONTROL_KEY_4, 1u) &&
              s_board.battery_forced && s_board.battery_mv == 2650u,
          "battery preset is exact");
    netmon_control_service_set_active(NETMON_ACTION_NONE, 2u);
    check(!s_board.battery_forced, "battery override clears on page leave");

    netmon_control_service_set_active(NETMON_ACTION_CHARGER, 3u);
    check(netmon_control_service_execute(NETMON_ACTION_CHARGER,
                                         NETMON_CONTROL_KEY_2, 3u) &&
              s_board.charger_forced && !s_board.charger_connected,
          "charger-out simulation is explicit");
    check(netmon_control_service_execute(NETMON_ACTION_CHARGER,
                                         NETMON_CONTROL_KEY_4, 4u) &&
              !s_board.charger_enabled,
          "charger disable is read back");
    netmon_control_service_set_active(NETMON_ACTION_NONE, 5u);
    check(!s_board.charger_forced && s_board.charger_enabled,
          "charger force clears and the debug inhibit is released");

    netmon_control_service_set_active(NETMON_ACTION_HEADSET_FORCE, 6u);
    check(netmon_control_service_execute(NETMON_ACTION_HEADSET_FORCE,
                                         NETMON_CONTROL_KEY_1, 6u) &&
              s_headset_override == ACCESSORY_DEBUG_OVERRIDE_INSERTED,
          "headset force uses accessory ownership");
    netmon_control_service_set_active(NETMON_ACTION_NONE, 7u);
    check(s_headset_override == ACCESSORY_DEBUG_OVERRIDE_AUTO,
          "headset force returns to automatic sensing");

    netmon_control_service_set_active(NETMON_ACTION_CONVERTER_MODE, 8u);
    check(netmon_control_service_execute(NETMON_ACTION_CONVERTER_MODE,
                                         NETMON_CONTROL_KEY_1, 8u) &&
              s_board.tps63020_pwm_mode,
          "converter PWM action applies");
    netmon_control_service_set_active(NETMON_ACTION_NONE, 9u);
    check(!s_board.tps63020_pwm_mode,
          "converter mode restores its entry baseline");
}

static void test_charger_control_requires_readback(void) {
    reset_fixture();
    netmon_control_service_set_active(NETMON_ACTION_CHARGER, 1u);
    s_charger_control_ok = false;
    check(netmon_control_service_execute(NETMON_ACTION_CHARGER,
                                         NETMON_CONTROL_KEY_4, 2u),
          "charger command key is consumed on a transport failure");
    netmon_control_snapshot_t failed = snapshot();
    check(failed.last_result == NETMON_CONTROL_RESULT_ERROR &&
              !failed.charger_enable_valid,
          "charger command reports failed hardware readback");

    reset_fixture();
    s_board.charger_inhibit_owner_mask = 1u;
    s_board.charger_enabled_requested = false;
    s_board.charger_enabled = false;
    netmon_control_service_set_active(NETMON_ACTION_CHARGER, 3u);
    check(netmon_control_service_execute(NETMON_ACTION_CHARGER,
                                         NETMON_CONTROL_KEY_4, 3u),
          "debug inhibit can coexist with a supervisor inhibit");
    netmon_control_service_set_active(NETMON_ACTION_NONE, 4u);
    check(s_board.charger_enable_valid && !s_board.charger_enabled &&
              s_board.charger_inhibit_owner_mask == 1u,
          "leaving Net Monitor preserves a supervisor inhibit");
}

static void test_lcd_and_measurement_actions(void) {
    reset_fixture();
    netmon_control_service_set_active(NETMON_ACTION_LCD_CALIBRATION, 10u);
    check(netmon_control_service_execute(NETMON_ACTION_LCD_CALIBRATION,
                                         NETMON_CONTROL_KEY_3, 10u) &&
              s_lcd.vop == 55u,
          "LCD Vop preview increments once");
    netmon_control_service_set_active(NETMON_ACTION_NONE, 11u);
    check(s_lcd.vop == 54u, "unsaved LCD preview reverts on page leave");

    netmon_control_service_set_active(NETMON_ACTION_LCD_CALIBRATION, 12u);
    check(netmon_control_service_execute(NETMON_ACTION_LCD_CALIBRATION,
                                         NETMON_CONTROL_KEY_6, 12u) &&
              s_lcd.temperature_coefficient == 2u,
          "LCD TC preview increments");
    check(netmon_control_service_execute(NETMON_ACTION_LCD_CALIBRATION,
                                         NETMON_CONTROL_KEY_STAR, 13u),
          "first LCD save press is consumed");
    check(s_lcd.vop == LCD_CALIBRATION_STOCK_VOP &&
              s_lcd.temperature_coefficient ==
                  LCD_CALIBRATION_STOCK_TEMPERATURE_COEFFICIENT &&
              s_lcd.stored_temperature_coefficient == 1u &&
              snapshot().lcd_save_armed &&
              strcmp(snapshot().status, "* confirm") == 0,
          "first save press shows a visible stock-profile confirmation without persisting");
    check(netmon_control_service_execute(NETMON_ACTION_LCD_CALIBRATION,
                                         NETMON_CONTROL_KEY_STAR, 14u),
          "second LCD save press is consumed");
    netmon_control_service_set_active(NETMON_ACTION_NONE, 15u);
    check(s_lcd.temperature_coefficient == 2u &&
              s_lcd.stored_temperature_coefficient == 2u &&
              !snapshot().lcd_save_armed,
          "confirmed LCD tuple is reapplied, saved, and survives page leave");

    reset_fixture();
    s_lcd.vop = 1u;
    netmon_control_service_set_active(NETMON_ACTION_LCD_CALIBRATION, 100u);
    check(netmon_control_service_execute(NETMON_ACTION_LCD_CALIBRATION,
                                         NETMON_CONTROL_KEY_1, 100u) &&
              s_lcd.vop == 0u,
          "the complete raw Vop range remains previewable");
    netmon_control_service_poll(10099u);
    check(s_lcd.vop == 0u,
          "a raw LCD preview remains active until its exact deadline");
    netmon_control_service_poll(10100u);
    check(s_lcd.vop == 54u &&
              s_lcd.stored_vop == 54u &&
              strcmp(snapshot().status, "LCD reverted") == 0,
          "an unsaved blank preview restores the stored tuple at ten seconds");

    reset_fixture();
    netmon_control_service_set_active(NETMON_ACTION_LCD_CALIBRATION, 200u);
    check(netmon_control_service_execute(NETMON_ACTION_LCD_CALIBRATION,
                                         NETMON_CONTROL_KEY_3, 200u) &&
              s_lcd.vop == 55u,
          "confirmation-timeout fixture previews its target");
    check(netmon_control_service_execute(NETMON_ACTION_LCD_CALIBRATION,
                                         NETMON_CONTROL_KEY_STAR, 201u) &&
              s_lcd.vop == LCD_CALIBRATION_STOCK_VOP,
          "save confirmation temporarily restores a visible profile");
    netmon_control_service_poll(5200u);
    check(s_lcd.vop == LCD_CALIBRATION_STOCK_VOP,
          "save confirmation remains open one millisecond before expiry");
    netmon_control_service_poll(5201u);
    check(s_lcd.vop == 54u && s_lcd.stored_vop == 54u,
          "an unconfirmed save expires without changing persistence");

    reset_fixture();
    netmon_control_service_set_active(NETMON_ACTION_LCD_CALIBRATION, 6000u);
    check(netmon_control_service_execute(NETMON_ACTION_LCD_CALIBRATION,
                                         NETMON_CONTROL_KEY_3, 6000u) &&
              s_lcd.vop == 55u,
          "continued-edit fixture previews Vop 55");
    check(netmon_control_service_execute(NETMON_ACTION_LCD_CALIBRATION,
                                         NETMON_CONTROL_KEY_STAR, 6001u) &&
              s_lcd.vop == LCD_CALIBRATION_STOCK_VOP,
          "continued-edit fixture enters visible confirmation");
    check(netmon_control_service_execute(NETMON_ACTION_LCD_CALIBRATION,
                                         NETMON_CONTROL_KEY_3, 6002u) &&
              s_lcd.vop == 56u && !snapshot().lcd_save_armed,
          "another edit cancels confirmation and continues from the captured target");
    netmon_control_service_set_active(NETMON_ACTION_NONE, 6003u);
    check(s_lcd.vop == 54u && s_lcd.stored_vop == 54u,
          "cancelled confirmation remains an unsaved preview");

    netmon_control_service_set_active(NETMON_ACTION_MEASUREMENT_RESET, 7000u);
    check(netmon_control_service_execute(NETMON_ACTION_MEASUREMENT_RESET,
                                         NETMON_CONTROL_KEY_0, 7000u) &&
              s_measurement_resets == 1u,
          "measurement reset is one explicit action");
    check(snapshot().measurement_generation == 1u,
          "control snapshot publishes the reset measurement generation");
    check(!netmon_control_service_execute(NETMON_ACTION_BATTERY_SIM,
                                          NETMON_CONTROL_KEY_1, 7001u),
          "a non-active action cannot mutate another page's resource");
}

static void test_backlight_level_action(void) {
    reset_fixture();
    netmon_control_service_set_active(
        NETMON_ACTION_BACKLIGHT_LEVEL, 20u);
    check(netmon_control_service_execute(
              NETMON_ACTION_BACKLIGHT_LEVEL, NETMON_CONTROL_KEY_1, 20u) &&
              s_backlight.active_percent == 65u,
          "backlight coarse preview decrements by five percent");
    check(snapshot().backlight_percent == 65u &&
              snapshot().backlight_stored_percent == 70u,
          "control snapshot separates preview and stored brightness");
    netmon_control_service_set_active(NETMON_ACTION_NONE, 21u);
    check(s_backlight.active_percent == 70u,
          "unsaved backlight preview reverts on page leave");

    netmon_control_service_set_active(
        NETMON_ACTION_BACKLIGHT_LEVEL, 22u);
    check(netmon_control_service_execute(
              NETMON_ACTION_BACKLIGHT_LEVEL, NETMON_CONTROL_KEY_4, 22u) &&
              s_backlight.active_percent == 69u,
          "backlight fine preview decrements by one percent");
    check(netmon_control_service_execute(
              NETMON_ACTION_BACKLIGHT_LEVEL, NETMON_CONTROL_KEY_STAR, 23u) &&
              s_backlight.stored_percent == 69u,
          "Star persists the live backlight value");
    netmon_control_service_set_active(NETMON_ACTION_NONE, 24u);
    check(s_backlight.active_percent == 69u,
          "saved backlight value survives page leave");

    netmon_control_service_set_active(
        NETMON_ACTION_BACKLIGHT_LEVEL, 25u);
    check(netmon_control_service_execute(
              NETMON_ACTION_BACKLIGHT_LEVEL, NETMON_CONTROL_KEY_HASH, 25u) &&
              s_backlight.active_percent == BACKLIGHT_LEVEL_DEFAULT_PERCENT,
          "Hash previews the 100% compatibility default");
    check(netmon_control_service_execute(
              NETMON_ACTION_BACKLIGHT_LEVEL, NETMON_CONTROL_KEY_0, 26u) &&
              s_backlight.active_percent == 69u,
          "zero explicitly reverts the factory preview");
}

static void test_guarded_radio_actions(void) {
    reset_fixture();
    netmon_control_service_set_active(NETMON_ACTION_NO_SERVICE_TIMER, 100u);
    check(netmon_control_service_execute(NETMON_ACTION_NO_SERVICE_TIMER,
                                         NETMON_CONTROL_KEY_1, 100u) &&
              s_maintenance.action == MODEM_MAINTENANCE_NONE,
          "scan mutation without Star is consumed but rejected");
    check(netmon_control_service_execute(NETMON_ACTION_NO_SERVICE_TIMER,
                                         NETMON_CONTROL_KEY_STAR, 101u) &&
              snapshot().radio_armed,
          "Star arms scan mutation for a bounded window");
    check(netmon_control_service_execute(NETMON_ACTION_NO_SERVICE_TIMER,
                                         NETMON_CONTROL_KEY_1, 102u) &&
              s_maintenance.action == MODEM_MAINTENANCE_SCAN_TIMER &&
              s_maintenance.scan_timer_s == 5u &&
              !snapshot().radio_armed,
          "armed preset submits the exact scan timer and disarms");

    s_maintenance.action = MODEM_MAINTENANCE_NONE;
    s_maintenance.state = MODEM_MAINTENANCE_IDLE;
    netmon_control_service_execute(NETMON_ACTION_NO_SERVICE_TIMER,
                                   NETMON_CONTROL_KEY_STAR, 200u);
    netmon_control_service_poll(10200u);
    check(!snapshot().radio_armed &&
              strcmp(snapshot().status, "Arm expired") == 0,
          "radio arm expires exactly at ten seconds");

    netmon_control_service_set_active(NETMON_ACTION_BAND_TEST, 11000u);
    netmon_control_service_execute(NETMON_ACTION_BAND_TEST,
                                   NETMON_CONTROL_KEY_STAR, 11001u);
    check(netmon_control_service_execute(NETMON_ACTION_BAND_TEST,
                                         NETMON_CONTROL_KEY_3, 11002u) &&
              s_maintenance.action == MODEM_MAINTENANCE_BAND_TEST &&
              s_maintenance.band_preset == 3u,
          "armed band key selects the B5 preset");
    netmon_control_service_set_active(NETMON_ACTION_NONE, 11003u);
    check(s_maintenance_cancel_count == 1u,
          "leaving a band page requests verified restoration");

    s_maintenance.action = MODEM_MAINTENANCE_NONE;
    s_maintenance.state = MODEM_MAINTENANCE_IDLE;
    netmon_control_service_set_active(
        NETMON_ACTION_ANTENNA_MAINTENANCE, 12000u);
    netmon_control_service_execute(NETMON_ACTION_ANTENNA_MAINTENANCE,
                                   NETMON_CONTROL_KEY_STAR, 12001u);
    check(netmon_control_service_execute(
              NETMON_ACTION_ANTENNA_MAINTENANCE,
              NETMON_CONTROL_KEY_HASH, 12002u) &&
              s_maintenance.action == MODEM_MAINTENANCE_ANTENNA,
          "Star then Hash enters guarded antenna ownership");
    s_maintenance.state = MODEM_MAINTENANCE_ACTIVE;
    s_maintenance.antenna_active = true;
    check(netmon_control_service_execute(
              NETMON_ACTION_ANTENNA_MAINTENANCE,
              NETMON_CONTROL_KEY_4, 12003u) &&
              s_maintenance.antenna_rf == 4u,
          "manual antenna mode accepts only an explicit RF selection");
    netmon_control_service_set_active(NETMON_ACTION_NONE, 12004u);
    check(s_maintenance_cancel_count == 2u,
          "leaving antenna maintenance requests automatic ownership restore");
}

int main(void) {
    test_bounded_outputs();
    test_override_cleanup();
    test_charger_control_requires_readback();
    test_lcd_and_measurement_actions();
    test_backlight_level_action();
    test_guarded_radio_actions();
    if (s_failures != 0) {
        fprintf(stderr, "%d Net Monitor control test(s) failed\n", s_failures);
        return 1;
    }
    puts("Net Monitor control tests passed");
    return 0;
}

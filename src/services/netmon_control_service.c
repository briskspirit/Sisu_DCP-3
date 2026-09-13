#include "services/netmon_control_service.h"

#include <stdio.h>
#include <string.h>

#include "audio/audio_levels.h"
#include "hal/accessory_hal.h"
#include "services/backlight_calibration.h"
#include "services/backlight_service.h"
#include "services/board_diag_service.h"
#include "services/core1_services.h"
#include "services/lcd_calibration.h"
#include "services/modem_service.h"
#include "services/netmon_diag_service.h"
#include "services/timebase.h"

#define NETMON_OUTPUT_TIMEOUT_MS 5000u
#define NETMON_STATUS_TIMEOUT_MS 1600u
#define NETMON_VIBRA_TEST_TICKS 48u
#define NETMON_RADIO_ARM_MS 10000u
#define NETMON_LCD_PREVIEW_TIMEOUT_MS 10000u
#define NETMON_LCD_SAVE_CONFIRM_MS 5000u

static netmon_control_snapshot_t s_snapshot;
static bool s_converter_baseline_valid;
static bool s_converter_pwm_baseline;
static bool s_backlight_preview_dirty;
static bool s_lcd_preview_dirty;
static uint32_t s_lcd_preview_until_ms;
static uint8_t s_lcd_pending_vop;
static uint8_t s_lcd_pending_tc;
static uint8_t s_lcd_pending_bias;
static uint32_t s_last_maintenance_sequence;

static bool is_radio_action(netmon_action_t action) {
    return action == NETMON_ACTION_NO_SERVICE_TIMER ||
           action == NETMON_ACTION_BAND_TEST ||
           action == NETMON_ACTION_ANTENNA_MAINTENANCE;
}

static void disarm_radio(void) {
    s_snapshot.radio_armed = false;
    s_snapshot.radio_arm_until_ms = 0u;
}

static void clear_lcd_save_arm(void) {
    s_lcd_pending_vop = 0u;
    s_lcd_pending_tc = 0u;
    s_lcd_pending_bias = 0u;
    s_snapshot.lcd_save_armed = false;
}

static void clear_lcd_preview_state(void) {
    s_lcd_preview_dirty = false;
    s_lcd_preview_until_ms = 0u;
    clear_lcd_save_arm();
}

static void arm_lcd_preview_timeout(uint32_t now_ms, uint32_t timeout_ms) {
    s_lcd_preview_dirty = true;
    s_lcd_preview_until_ms = now_ms + timeout_ms;
}

static bool revert_lcd_preview(void) {
    bool ok = lcd_calibration_revert_stored();
    clear_lcd_preview_state();
    return ok;
}

static void publish_status(netmon_control_result_t result, const char *text,
                           uint32_t now_ms) {
    s_snapshot.last_result = result;
    strncpy(s_snapshot.status, text != NULL ? text : "",
            sizeof(s_snapshot.status) - 1u);
    s_snapshot.status[sizeof(s_snapshot.status) - 1u] = '\0';
    s_snapshot.status_until_ms = now_ms + NETMON_STATUS_TIMEOUT_MS;
    s_snapshot.sequence++;
}

static void stop_output(void) {
    bool stop_audio = s_snapshot.output == NETMON_OUTPUT_VIBRA ||
                      s_snapshot.output == NETMON_OUTPUT_BUZZER;
    board_diag_debug_set_backlight_override(false, false);
    board_diag_debug_set_vibra_test(false);
    if (stop_audio) {
        core1_post_command(CORE1_CMD_AUDIO_DEBUG_STOP, 0u);
    }
    s_snapshot.output = NETMON_OUTPUT_NONE;
    s_snapshot.output_until_ms = 0u;
}

static void leave_action(netmon_action_t action, uint32_t now_ms) {
    switch (action) {
    case NETMON_ACTION_OUTPUT_TEST:
        stop_output();
        break;
    case NETMON_ACTION_BATTERY_SIM:
        board_diag_debug_clear_battery_force();
        break;
    case NETMON_ACTION_CHARGER:
        board_diag_debug_clear_charger_force();
        /* Page 92 owns only the DEBUG inhibit. Never replay an entry-state
         * boolean: that could clear a supervisor or fault stop which arrived
         * while the page was open. */
        (void)board_diag_restore_charger_default();
        break;
    case NETMON_ACTION_HEADSET_FORCE:
        accessory_hal_debug_set_override(ACCESSORY_DEBUG_OVERRIDE_AUTO);
        break;
    case NETMON_ACTION_CONVERTER_MODE:
        if (s_converter_baseline_valid) {
            (void)board_diag_debug_set_tps63020_pwm_mode(
                s_converter_pwm_baseline);
        }
        s_converter_baseline_valid = false;
        break;
    case NETMON_ACTION_BACKLIGHT_LEVEL:
        if (s_backlight_preview_dirty) {
            (void)backlight_calibration_revert_stored();
        }
        s_backlight_preview_dirty = false;
        break;
    case NETMON_ACTION_LCD_CALIBRATION:
        if (s_lcd_preview_dirty) {
            (void)revert_lcd_preview();
        } else {
            clear_lcd_preview_state();
        }
        break;
    case NETMON_ACTION_MEASUREMENT_RESET:
    case NETMON_ACTION_NO_SERVICE_TIMER:
        disarm_radio();
        break;
    case NETMON_ACTION_BAND_TEST:
    case NETMON_ACTION_ANTENNA_MAINTENANCE:
        modem_service_maintenance_cancel();
        disarm_radio();
        break;
    case NETMON_ACTION_NONE:
        break;
    }
    (void)now_ms;
}

static void enter_action(netmon_action_t action) {
    board_diag_snapshot_t board;
    board_diag_get_snapshot(&board);
    if (action == NETMON_ACTION_CONVERTER_MODE) {
        s_converter_baseline_valid = true;
        s_converter_pwm_baseline = board.tps63020_pwm_mode;
    }
}

void netmon_control_service_init(uint32_t now_ms) {
    memset(&s_snapshot, 0, sizeof(s_snapshot));
    s_snapshot.active_action = NETMON_ACTION_NONE;
    s_snapshot.last_result = NETMON_CONTROL_RESULT_IDLE;
    s_snapshot.sequence = 1u;
    s_converter_baseline_valid = false;
    s_backlight_preview_dirty = false;
    clear_lcd_preview_state();
    s_last_maintenance_sequence = 0u;
    (void)now_ms;
}

void netmon_control_service_set_active(netmon_action_t action,
                                       uint32_t now_ms) {
    if (action == s_snapshot.active_action) {
        return;
    }
    leave_action(s_snapshot.active_action, now_ms);
    s_snapshot.active_action = action;
    s_snapshot.last_result = NETMON_CONTROL_RESULT_IDLE;
    s_snapshot.status[0] = '\0';
    s_snapshot.status_until_ms = 0u;
    enter_action(action);
    s_snapshot.sequence++;
}

void netmon_control_service_poll(uint32_t now_ms) {
    if (s_lcd_preview_dirty && s_lcd_preview_until_ms != 0u &&
        time_diff_ms(now_ms, s_lcd_preview_until_ms) >= 0) {
        bool ok = revert_lcd_preview();
        publish_status(ok ? NETMON_CONTROL_RESULT_OK
                          : NETMON_CONTROL_RESULT_ERROR,
                       ok ? "LCD reverted" : "LCD error", now_ms);
    }

    if (s_snapshot.radio_armed &&
        time_diff_ms(now_ms, s_snapshot.radio_arm_until_ms) >= 0) {
        disarm_radio();
        publish_status(NETMON_CONTROL_RESULT_ERROR, "Arm expired", now_ms);
    }

    modem_maintenance_snapshot_t maintenance;
    modem_service_get_maintenance_snapshot(&maintenance);
    if (maintenance.sequence != s_last_maintenance_sequence) {
        s_last_maintenance_sequence = maintenance.sequence;
        s_snapshot.maintenance = maintenance;
        s_snapshot.sequence++;
        if (maintenance.state == MODEM_MAINTENANCE_ACTIVE) {
            publish_status(NETMON_CONTROL_RESULT_OK,
                           maintenance.action == MODEM_MAINTENANCE_BAND_TEST
                               ? "Band active" : "RF manual",
                           now_ms);
        } else if (maintenance.state == MODEM_MAINTENANCE_DONE) {
            publish_status(NETMON_CONTROL_RESULT_OK, "Done", now_ms);
        } else if (maintenance.state == MODEM_MAINTENANCE_ERROR) {
            publish_status(NETMON_CONTROL_RESULT_ERROR, "Radio error",
                           now_ms);
        } else if (maintenance.state == MODEM_MAINTENANCE_LOCKED) {
            publish_status(NETMON_CONTROL_RESULT_ERROR, "RF locked",
                           now_ms);
        }
    }

    if (s_snapshot.output != NETMON_OUTPUT_NONE &&
        time_diff_ms(now_ms, s_snapshot.output_until_ms) >= 0) {
        stop_output();
        publish_status(NETMON_CONTROL_RESULT_OK, "Test stopped", now_ms);
    }
    if (s_snapshot.status_until_ms != 0u &&
        time_diff_ms(now_ms, s_snapshot.status_until_ms) >= 0) {
        s_snapshot.status[0] = '\0';
        s_snapshot.status_until_ms = 0u;
        s_snapshot.last_result = NETMON_CONTROL_RESULT_IDLE;
        s_snapshot.sequence++;
    }
}

static bool execute_output(netmon_control_key_t key, uint32_t now_ms) {
    stop_output();
    if (key == NETMON_CONTROL_KEY_0) {
        publish_status(NETMON_CONTROL_RESULT_OK, "Outputs off", now_ms);
        return true;
    }
    if (key == NETMON_CONTROL_KEY_1) {
        board_diag_debug_set_backlight_override(true, true);
        s_snapshot.output = NETMON_OUTPUT_BACKLIGHT;
        publish_status(NETMON_CONTROL_RESULT_OK, "Light test", now_ms);
    } else if (key == NETMON_CONTROL_KEY_2) {
        board_diag_debug_set_vibra_test(true);
        uint16_t arg = (uint16_t)(NETMON_VIBRA_TEST_TICKS |
            ((uint16_t)board_diag_vibra_strength() << 8u));
        core1_post_command(CORE1_CMD_AUDIO_DEBUG_VIBRA_TEST, arg);
        s_snapshot.output = NETMON_OUTPUT_VIBRA;
        publish_status(NETMON_CONTROL_RESULT_OK, "Vibra test", now_ms);
    } else if (key == NETMON_CONTROL_KEY_3) {
        core1_post_command(CORE1_CMD_AUDIO_DEBUG_BUZZER_TEST,
                           audio_arg(0u, AUDIO_LEVEL_MAX));
        s_snapshot.output = NETMON_OUTPUT_BUZZER;
        publish_status(NETMON_CONTROL_RESULT_OK, "Buzzer test", now_ms);
    } else {
        return false;
    }
    s_snapshot.output_until_ms = now_ms + NETMON_OUTPUT_TIMEOUT_MS;
    return true;
}

static bool execute_battery(netmon_control_key_t key, uint32_t now_ms) {
    static const uint16_t PRESETS[] = {2200u, 2320u, 2500u, 2650u, 2900u};
    if (key == NETMON_CONTROL_KEY_HASH) {
        board_diag_debug_clear_battery_force();
        publish_status(NETMON_CONTROL_RESULT_OK, "Battery auto", now_ms);
        return true;
    }
    if (key < NETMON_CONTROL_KEY_1 || key > NETMON_CONTROL_KEY_5) {
        return false;
    }
    uint16_t mv = PRESETS[(unsigned)key - (unsigned)NETMON_CONTROL_KEY_1];
    board_diag_debug_force_battery_mv(mv);
    char status[13];
    snprintf(status, sizeof(status), "Batt %u", (unsigned)mv);
    publish_status(NETMON_CONTROL_RESULT_OK, status, now_ms);
    return true;
}

static bool execute_charger(netmon_control_key_t key, uint32_t now_ms) {
    board_diag_snapshot_t board;
    if (key == NETMON_CONTROL_KEY_1 || key == NETMON_CONTROL_KEY_2) {
        bool connected = key == NETMON_CONTROL_KEY_1;
        board_diag_debug_force_charger_connected(connected, now_ms);
        publish_status(NETMON_CONTROL_RESULT_OK,
                       connected ? "Charger in" : "Charger out", now_ms);
        return true;
    }
    if (key == NETMON_CONTROL_KEY_HASH) {
        board_diag_debug_clear_charger_force();
        publish_status(NETMON_CONTROL_RESULT_OK, "Charger auto", now_ms);
        return true;
    }
    if (key == NETMON_CONTROL_KEY_3 || key == NETMON_CONTROL_KEY_4) {
        bool enabled = key == NETMON_CONTROL_KEY_3;
        bool applied = board_diag_debug_set_charger_enabled(enabled);
        board_diag_get_snapshot(&board);
        if (!applied || !board.charger_enable_valid) {
            publish_status(NETMON_CONTROL_RESULT_ERROR, "Readback err", now_ms);
        } else {
            publish_status(NETMON_CONTROL_RESULT_OK,
                           enabled ? "Debug clear" : "Charge disab", now_ms);
        }
        return true;
    }
    return false;
}

static bool execute_headset(netmon_control_key_t key, uint32_t now_ms) {
    accessory_debug_override_t override;
    const char *status;
    if (key == NETMON_CONTROL_KEY_0) {
        override = ACCESSORY_DEBUG_OVERRIDE_AUTO;
        status = "Headset auto";
    } else if (key == NETMON_CONTROL_KEY_1) {
        override = ACCESSORY_DEBUG_OVERRIDE_INSERTED;
        status = "Force insert";
    } else if (key == NETMON_CONTROL_KEY_2) {
        override = ACCESSORY_DEBUG_OVERRIDE_REMOVED;
        status = "Force remove";
    } else {
        return false;
    }
    accessory_hal_debug_set_override(override);
    if (accessory_hal_debug_get_override() != override) {
        publish_status(NETMON_CONTROL_RESULT_ERROR, "Readback err", now_ms);
    } else {
        publish_status(NETMON_CONTROL_RESULT_OK, status, now_ms);
    }
    return true;
}

static bool execute_converter(netmon_control_key_t key, uint32_t now_ms) {
    if (key != NETMON_CONTROL_KEY_0 && key != NETMON_CONTROL_KEY_1) {
        return false;
    }
    bool pwm = key == NETMON_CONTROL_KEY_1;
    bool actual = board_diag_debug_set_tps63020_pwm_mode(pwm);
    publish_status(actual == pwm ? NETMON_CONTROL_RESULT_OK
                                 : NETMON_CONTROL_RESULT_ERROR,
                   actual == pwm ? (pwm ? "TPS PWM" : "TPS save")
                                 : "Readback err",
                   now_ms);
    return true;
}

static bool execute_backlight_level(netmon_control_key_t key,
                                    uint32_t now_ms) {
    uint8_t level = backlight_service_level_percent();
    if (key == NETMON_CONTROL_KEY_0) {
        bool ok = backlight_calibration_revert_stored();
        s_backlight_preview_dirty = false;
        publish_status(ok ? NETMON_CONTROL_RESULT_OK
                          : NETMON_CONTROL_RESULT_ERROR,
                       ok ? "Light revert" : "Light error", now_ms);
        return true;
    }
    if (key == NETMON_CONTROL_KEY_STAR) {
        bool ok = backlight_calibration_save_active();
        if (ok) {
            s_backlight_preview_dirty = false;
        }
        publish_status(ok ? NETMON_CONTROL_RESULT_OK
                          : NETMON_CONTROL_RESULT_ERROR,
                       ok ? "Light saved" : "Save failed", now_ms);
        return true;
    }

    uint8_t next = level;
    if (key == NETMON_CONTROL_KEY_HASH) {
        next = BACKLIGHT_LEVEL_DEFAULT_PERCENT;
    } else if (key == NETMON_CONTROL_KEY_1 &&
               level > BACKLIGHT_LEVEL_MIN_PERCENT) {
        uint8_t room = (uint8_t)(level - BACKLIGHT_LEVEL_MIN_PERCENT);
        next = (uint8_t)(level - (room < 5u ? room : 5u));
    } else if (key == NETMON_CONTROL_KEY_3 &&
               level < BACKLIGHT_LEVEL_MAX_PERCENT) {
        uint8_t room = (uint8_t)(BACKLIGHT_LEVEL_MAX_PERCENT - level);
        next = (uint8_t)(level + (room < 5u ? room : 5u));
    } else if (key == NETMON_CONTROL_KEY_4 &&
               level > BACKLIGHT_LEVEL_MIN_PERCENT) {
        next = (uint8_t)(level - 1u);
    } else if (key == NETMON_CONTROL_KEY_6 &&
               level < BACKLIGHT_LEVEL_MAX_PERCENT) {
        next = (uint8_t)(level + 1u);
    } else {
        publish_status(NETMON_CONTROL_RESULT_ERROR, "Limit", now_ms);
        return true;
    }

    bool ok = backlight_calibration_preview(next);
    if (ok) {
        s_backlight_preview_dirty = true;
    }
    char status[13];
    snprintf(status, sizeof(status), "Light %u%%", (unsigned)next);
    publish_status(ok ? NETMON_CONTROL_RESULT_OK
                      : NETMON_CONTROL_RESULT_ERROR,
                   ok ? status : "Light error", now_ms);
    return true;
}

static bool execute_lcd(netmon_control_key_t key, uint32_t now_ms) {
    lcd_calibration_status_t lcd;
    lcd_calibration_get_status(&lcd);
    uint8_t vop = lcd.vop;
    uint8_t tc = lcd.temperature_coefficient;
    uint8_t bias = lcd.bias_system;
    bool apply = true;
    if (key == NETMON_CONTROL_KEY_0) {
        bool ok = revert_lcd_preview();
        publish_status(ok ? NETMON_CONTROL_RESULT_OK
                          : NETMON_CONTROL_RESULT_ERROR,
                       ok ? "LCD reverted" : "LCD error", now_ms);
        return true;
    }
    if (key == NETMON_CONTROL_KEY_STAR) {
        if (s_snapshot.lcd_save_armed &&
            time_diff_ms(now_ms, s_lcd_preview_until_ms) >= 0) {
            if (!revert_lcd_preview()) {
                publish_status(NETMON_CONTROL_RESULT_ERROR, "LCD error",
                               now_ms);
                return true;
            }
            lcd_calibration_get_status(&lcd);
        }
        if (!s_snapshot.lcd_save_armed) {
            s_lcd_pending_vop = lcd.vop;
            s_lcd_pending_tc = lcd.temperature_coefficient;
            s_lcd_pending_bias = lcd.bias_system;
            bool ok = lcd_calibration_preview_stock();
            if (ok) {
                s_snapshot.lcd_save_armed = true;
                arm_lcd_preview_timeout(now_ms, NETMON_LCD_SAVE_CONFIRM_MS);
            } else {
                (void)revert_lcd_preview();
            }
            publish_status(ok ? NETMON_CONTROL_RESULT_OK
                              : NETMON_CONTROL_RESULT_ERROR,
                           ok ? "* confirm" : "LCD error", now_ms);
            return true;
        }

        bool applied = lcd_calibration_preview_tuning(
            s_lcd_pending_vop, s_lcd_pending_tc, s_lcd_pending_bias);
        bool ok = applied && lcd_calibration_save_active();
        if (ok) {
            clear_lcd_preview_state();
        } else if (applied) {
            clear_lcd_save_arm();
            arm_lcd_preview_timeout(now_ms, NETMON_LCD_PREVIEW_TIMEOUT_MS);
        } else {
            (void)revert_lcd_preview();
        }
        publish_status(ok ? NETMON_CONTROL_RESULT_OK
                          : NETMON_CONTROL_RESULT_ERROR,
                       ok ? "LCD saved" : "Save failed", now_ms);
        return true;
    }

    if (s_snapshot.lcd_save_armed) {
        bool restored = lcd_calibration_preview_tuning(
            s_lcd_pending_vop, s_lcd_pending_tc, s_lcd_pending_bias);
        clear_lcd_save_arm();
        if (!restored) {
            (void)revert_lcd_preview();
            publish_status(NETMON_CONTROL_RESULT_ERROR, "LCD error", now_ms);
            return true;
        }
        arm_lcd_preview_timeout(now_ms, NETMON_LCD_PREVIEW_TIMEOUT_MS);
        lcd_calibration_get_status(&lcd);
        vop = lcd.vop;
        tc = lcd.temperature_coefficient;
        bias = lcd.bias_system;
    }

    if (key == NETMON_CONTROL_KEY_HASH) {
        apply = lcd_calibration_preview_stock();
    } else if (key == NETMON_CONTROL_KEY_1 && vop > 0u) {
        vop--;
    } else if (key == NETMON_CONTROL_KEY_3 && vop < LCD_CALIBRATION_VOP_MAX) {
        vop++;
    } else if (key == NETMON_CONTROL_KEY_4 && tc > 0u) {
        tc--;
    } else if (key == NETMON_CONTROL_KEY_6 &&
               tc < LCD_CALIBRATION_TEMPERATURE_COEFFICIENT_MAX) {
        tc++;
    } else if (key == NETMON_CONTROL_KEY_7 && bias > 0u) {
        bias--;
    } else if (key == NETMON_CONTROL_KEY_9 &&
               bias < LCD_CALIBRATION_BIAS_SYSTEM_MAX) {
        bias++;
    } else {
        publish_status(NETMON_CONTROL_RESULT_ERROR, "Limit", now_ms);
        return true;
    }
    if (key != NETMON_CONTROL_KEY_HASH) {
        apply = lcd_calibration_preview_tuning(vop, tc, bias);
    }
    if (apply) {
        arm_lcd_preview_timeout(now_ms, NETMON_LCD_PREVIEW_TIMEOUT_MS);
        lcd_calibration_get_status(&lcd);
        char status[13];
        unsigned shown_vop = (unsigned)(lcd.vop & 0x7fu);
        unsigned shown_tc =
            (unsigned)(lcd.temperature_coefficient & 0x03u);
        unsigned shown_bias = (unsigned)(lcd.bias_system & 0x07u);
        snprintf(status, sizeof(status), "V%u T%u B%u",
                 shown_vop, shown_tc, shown_bias);
        publish_status(NETMON_CONTROL_RESULT_OK, status, now_ms);
    } else {
        publish_status(NETMON_CONTROL_RESULT_ERROR, "LCD error", now_ms);
    }
    return true;
}

static bool arm_radio(netmon_action_t action, uint32_t now_ms) {
    if (!is_radio_action(action)) {
        return false;
    }
    if (!modem_service_maintenance_supported()) {
        publish_status(NETMON_CONTROL_RESULT_ERROR, "Unsupported", now_ms);
        return true;
    }
    s_snapshot.radio_armed = true;
    s_snapshot.radio_arm_until_ms = now_ms + NETMON_RADIO_ARM_MS;
    publish_status(NETMON_CONTROL_RESULT_OK, "Armed 10s", now_ms);
    return true;
}

static bool radio_requires_arm(uint32_t now_ms) {
    if (s_snapshot.radio_armed &&
        time_diff_ms(now_ms, s_snapshot.radio_arm_until_ms) < 0) {
        return true;
    }
    disarm_radio();
    publish_status(NETMON_CONTROL_RESULT_ERROR, "Press *", now_ms);
    return false;
}

static void publish_radio_request(bool admitted, uint32_t now_ms) {
    publish_status(admitted ? NETMON_CONTROL_RESULT_PENDING
                            : NETMON_CONTROL_RESULT_ERROR,
                   admitted ? "Working..." : "Busy", now_ms);
}

static bool execute_scan_timer(netmon_control_key_t key, uint32_t now_ms) {
    static const uint16_t PRESETS[] = {
        5u, 30u, 60u, 300u, 900u, 1800u, 3600u,
    };
    if (key == NETMON_CONTROL_KEY_STAR) {
        return arm_radio(NETMON_ACTION_NO_SERVICE_TIMER, now_ms);
    }
    if (key == NETMON_CONTROL_KEY_HASH) {
        bool admitted = modem_service_maintenance_read_scan_timer();
        publish_radio_request(admitted, now_ms);
        return true;
    }
    if (key < NETMON_CONTROL_KEY_1 || key > NETMON_CONTROL_KEY_7) {
        return false;
    }
    if (!radio_requires_arm(now_ms)) {
        return true;
    }
    uint16_t seconds = PRESETS[(unsigned)key -
                               (unsigned)NETMON_CONTROL_KEY_1];
    bool admitted = modem_service_maintenance_request_scan_timer(seconds);
    if (admitted) {
        disarm_radio();
    }
    publish_radio_request(admitted, now_ms);
    return true;
}

static bool execute_band_test(netmon_control_key_t key, uint32_t now_ms) {
    if (key == NETMON_CONTROL_KEY_STAR) {
        return arm_radio(NETMON_ACTION_BAND_TEST, now_ms);
    }
    if (key == NETMON_CONTROL_KEY_0 || key == NETMON_CONTROL_KEY_HASH) {
        disarm_radio();
        bool admitted = modem_service_maintenance_restore_band();
        publish_status(admitted ? NETMON_CONTROL_RESULT_PENDING
                                : NETMON_CONTROL_RESULT_ERROR,
                       admitted ? "Restoring..." : "No band test", now_ms);
        return true;
    }
    if (key < NETMON_CONTROL_KEY_1 || key > NETMON_CONTROL_KEY_5) {
        return false;
    }
    if (!radio_requires_arm(now_ms)) {
        return true;
    }
    bool admitted = modem_service_maintenance_start_band_test(
        (uint8_t)((unsigned)key - (unsigned)NETMON_CONTROL_KEY_1 + 1u));
    if (admitted) {
        disarm_radio();
    }
    publish_radio_request(admitted, now_ms);
    return true;
}

static bool execute_antenna(netmon_control_key_t key, uint32_t now_ms) {
    if (key == NETMON_CONTROL_KEY_STAR) {
        return arm_radio(NETMON_ACTION_ANTENNA_MAINTENANCE, now_ms);
    }
    if (key == NETMON_CONTROL_KEY_0) {
        disarm_radio();
        bool admitted = modem_service_maintenance_exit_antenna();
        publish_status(admitted ? NETMON_CONTROL_RESULT_PENDING
                                : NETMON_CONTROL_RESULT_ERROR,
                       admitted ? "Restoring..." : "Not manual", now_ms);
        return true;
    }
    if (key == NETMON_CONTROL_KEY_HASH) {
        if (!radio_requires_arm(now_ms)) {
            return true;
        }
        bool admitted = modem_service_maintenance_enter_antenna();
        if (admitted) {
            disarm_radio();
        }
        publish_radio_request(admitted, now_ms);
        return true;
    }
    if (key < NETMON_CONTROL_KEY_1 || key > NETMON_CONTROL_KEY_4) {
        return false;
    }
    bool admitted = modem_service_maintenance_select_antenna(
        (uint8_t)((unsigned)key - (unsigned)NETMON_CONTROL_KEY_1 + 1u));
    publish_radio_request(admitted, now_ms);
    return true;
}

bool netmon_control_service_execute(netmon_action_t action,
                                    netmon_control_key_t key,
                                    uint32_t now_ms) {
    if (action == NETMON_ACTION_NONE || action != s_snapshot.active_action ||
        key == NETMON_CONTROL_KEY_INVALID) {
        return false;
    }
    switch (action) {
    case NETMON_ACTION_OUTPUT_TEST:
        return execute_output(key, now_ms);
    case NETMON_ACTION_BATTERY_SIM:
        return execute_battery(key, now_ms);
    case NETMON_ACTION_CHARGER:
        return execute_charger(key, now_ms);
    case NETMON_ACTION_HEADSET_FORCE:
        return execute_headset(key, now_ms);
    case NETMON_ACTION_CONVERTER_MODE:
        return execute_converter(key, now_ms);
    case NETMON_ACTION_BACKLIGHT_LEVEL:
        return execute_backlight_level(key, now_ms);
    case NETMON_ACTION_LCD_CALIBRATION:
        return execute_lcd(key, now_ms);
    case NETMON_ACTION_MEASUREMENT_RESET:
        if (key != NETMON_CONTROL_KEY_0) {
            return false;
        }
        netmon_diag_service_reset_measurement_window(now_ms);
        publish_status(NETMON_CONTROL_RESULT_OK, "Window reset", now_ms);
        return true;
    case NETMON_ACTION_NO_SERVICE_TIMER:
        return execute_scan_timer(key, now_ms);
    case NETMON_ACTION_BAND_TEST:
        return execute_band_test(key, now_ms);
    case NETMON_ACTION_ANTENNA_MAINTENANCE:
        return execute_antenna(key, now_ms);
    case NETMON_ACTION_NONE:
        return false;
    }
    return false;
}

void netmon_control_service_get_snapshot(netmon_control_snapshot_t *out) {
    if (out == NULL) {
        return;
    }
    netmon_control_snapshot_t next = s_snapshot;
    board_diag_snapshot_t board;
    board_diag_get_snapshot(&board);
    next.battery_forced = board.battery_forced;
    next.battery_mv = board.battery_mv;
    next.charger_forced = board.charger_forced;
    next.charger_connected = board.charger_connected;
    next.charger_enabled_requested = board.charger_enabled_requested;
    next.charger_enabled = board.charger_enabled;
    next.charger_enable_valid = board.charger_enable_valid;
    next.charger_inhibit_owner_mask = board.charger_inhibit_owner_mask;
    next.converter_pwm = board.tps63020_pwm_mode;
    backlight_calibration_status_t backlight;
    backlight_calibration_get_status(&backlight);
    next.backlight_percent = backlight.active_percent;
    next.backlight_stored_percent = backlight.stored_percent;
    next.backlight_stored_valid = backlight.stored_valid;
    accessory_debug_override_t override =
        accessory_hal_debug_get_override();
    next.headset_override = override == ACCESSORY_DEBUG_OVERRIDE_INSERTED
        ? NETMON_HEADSET_INSERTED
        : override == ACCESSORY_DEBUG_OVERRIDE_REMOVED
            ? NETMON_HEADSET_REMOVED : NETMON_HEADSET_AUTO;
    lcd_calibration_status_t lcd;
    lcd_calibration_get_status(&lcd);
    next.lcd_vop = lcd.vop;
    next.lcd_temperature_coefficient = lcd.temperature_coefficient;
    next.lcd_bias_system = lcd.bias_system;
    next.lcd_stored_vop = lcd.stored_vop;
    next.lcd_stored_temperature_coefficient =
        lcd.stored_temperature_coefficient;
    next.lcd_stored_bias_system = lcd.stored_bias_system;
    next.lcd_stored_valid = lcd.stored_vop_valid;
    next.measurement_generation =
        netmon_diag_service_measurement_generation();
    modem_service_get_maintenance_snapshot(&next.maintenance);
    *out = next;
}

_Static_assert(sizeof(netmon_control_snapshot_t) <= 128u,
               "Net Monitor control snapshot unexpectedly large");

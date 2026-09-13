#ifndef NETMON_CONTROL_SERVICE_H
#define NETMON_CONTROL_SERVICE_H

#include <stdbool.h>
#include <stdint.h>

#include "services/modem_service.h"
#include "services/netmon_control_types.h"

typedef enum {
    NETMON_CONTROL_RESULT_IDLE = 0,
    NETMON_CONTROL_RESULT_PENDING,
    NETMON_CONTROL_RESULT_OK,
    NETMON_CONTROL_RESULT_ERROR,
} netmon_control_result_t;

typedef enum {
    NETMON_OUTPUT_NONE = 0,
    NETMON_OUTPUT_BACKLIGHT,
    NETMON_OUTPUT_VIBRA,
    NETMON_OUTPUT_BUZZER,
} netmon_output_t;

typedef enum {
    NETMON_HEADSET_AUTO = 0,
    NETMON_HEADSET_INSERTED,
    NETMON_HEADSET_REMOVED,
} netmon_headset_override_t;

typedef struct {
    uint32_t sequence;
    netmon_action_t active_action;
    netmon_control_result_t last_result;
    char status[13];
    uint32_t status_until_ms;
    netmon_output_t output;
    uint32_t output_until_ms;
    bool battery_forced;
    uint16_t battery_mv;
    bool charger_forced;
    bool charger_connected;
    bool charger_enabled_requested;
    bool charger_enabled;
    bool charger_enable_valid;
    uint8_t charger_inhibit_owner_mask;
    netmon_headset_override_t headset_override;
    bool converter_pwm;
    uint8_t backlight_percent;
    uint8_t backlight_stored_percent;
    bool backlight_stored_valid;
    uint8_t lcd_vop;
    uint8_t lcd_temperature_coefficient;
    uint8_t lcd_bias_system;
    uint8_t lcd_stored_vop;
    uint8_t lcd_stored_temperature_coefficient;
    uint8_t lcd_stored_bias_system;
    bool lcd_stored_valid;
    bool lcd_save_armed;
    uint32_t measurement_generation;
    bool radio_armed;
    uint32_t radio_arm_until_ms;
    modem_maintenance_snapshot_t maintenance;
} netmon_control_snapshot_t;

void netmon_control_service_init(uint32_t now_ms);
void netmon_control_service_set_active(netmon_action_t action,
                                       uint32_t now_ms);
void netmon_control_service_poll(uint32_t now_ms);
bool netmon_control_service_execute(netmon_action_t action,
                                    netmon_control_key_t key,
                                    uint32_t now_ms);
void netmon_control_service_get_snapshot(netmon_control_snapshot_t *out);

#endif

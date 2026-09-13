#include "services/power_sleep_logic.h"

power_wake_cause_t power_sleep_decode_wake(bool had_swcore_pd,
                                           bool marker_present,
                                           bool button_latched,
                                           bool sys_int_latched,
                                           bool service_vbus_latched) {
    if (!had_swcore_pd || !marker_present) {
        return POWER_WAKE_COLD;
    }
    if (button_latched) {
        return POWER_WAKE_BUTTON;
    }
    if (sys_int_latched) {
        return POWER_WAKE_SYS_INT;
    }
    if (service_vbus_latched) {
        return POWER_WAKE_SERVICE_VBUS;
    }
    /* Source latches can release before boot capture, but HAD_SWCORE_PD plus
     * our one-shot marker proves this is not a cold reset. */
    return POWER_WAKE_DORMANT_UNKNOWN;
}

power_sleep_arm_result_t power_sleep_check_armed_levels(
    bool button_low,
    bool sys_int_low,
    bool external_power_present,
    bool usb_live,
    bool charger_status_read_ok,
    bool charger_stat2_high,
    bool allow_stuck_stat2) {
    if (button_low) {
        return POWER_SLEEP_ARM_BUTTON_LOW;
    }
    if (sys_int_low) {
        return POWER_SLEEP_ARM_SYS_INT_LOW;
    }
    if (!charger_status_read_ok) {
        return POWER_SLEEP_ARM_LEVEL_READ_FAILED;
    }
    if (external_power_present || usb_live) {
        return POWER_SLEEP_ARM_EXTERNAL_POWER;
    }
    if (!charger_stat2_high && !allow_stuck_stat2) {
        return POWER_SLEEP_ARM_CHARGER_STATUS_LOW;
    }
    return POWER_SLEEP_ARM_CLEAR;
}

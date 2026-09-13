#ifndef POWER_SLEEP_LOGIC_H
#define POWER_SLEEP_LOGIC_H

#include <stdbool.h>

/* Pure wake-cause decode for the POWMAN dormant off state (no SDK deps; unit
 * tested on host in test_power_sleep). Inputs come from the POWMAN always-on
 * registers, which survive the switched-core power cycle:
 *  - had_swcore_pd:  CHIP_RESET.HAD_SWCORE_PD (last reset was a power-down wake)
 *  - marker_present: our scratch-register magic written just before power-off
 *  - button/sys_int/vbus: latched sources for GP7 / GP42 / GP28
 * A reset lacking either half of the dormant signature decodes as COLD
 * (battery insert, watchdog, flash reboot, aborted entry). If both halves are
 * present but the transient source evidence is gone, preserve the fact that
 * this was a dormant wake as UNKNOWN so boot still runs the RTC alarm probe
 * and skips cold-only initialization. Button outranks SYS_INT, which
 * outranks service VBUS; all coincident sources are polled after boot. */
typedef enum {
    POWER_WAKE_COLD = 0,
    POWER_WAKE_BUTTON,
    POWER_WAKE_SYS_INT,
    POWER_WAKE_SERVICE_VBUS,
    POWER_WAKE_DORMANT_UNKNOWN,
} power_wake_cause_t;

typedef enum {
    POWER_SLEEP_ARM_CLEAR = 0,
    POWER_SLEEP_ARM_BUTTON_LOW,
    POWER_SLEEP_ARM_SYS_INT_LOW,
    POWER_SLEEP_ARM_LEVEL_READ_FAILED,
    POWER_SLEEP_ARM_EXTERNAL_POWER,
    POWER_SLEEP_ARM_CHARGER_STATUS_LOW,
} power_sleep_arm_result_t;

power_wake_cause_t power_sleep_decode_wake(bool had_swcore_pd,
                                           bool marker_present,
                                           bool button_latched,
                                           bool sys_int_latched,
                                           bool service_vbus_latched);

/* Final arm-then-read race closure. Inputs are sampled only after all POWMAN
 * slots and device wake modes are live. Anything except CLEAR aborts entry. */
power_sleep_arm_result_t power_sleep_check_armed_levels(
    bool button_low,
    bool sys_int_low,
    bool external_power_present,
    bool usb_live,
    bool charger_status_read_ok,
    bool charger_stat2_high,
    bool allow_stuck_stat2);

#endif

#include <stdbool.h>
#include <stdio.h>

#include "hal/power_sleep_hal.h"
#include "services/power_sleep_logic.h"

static int s_failures;

static void assert_true(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        s_failures++;
    }
}

/* Oracle: the design spec (power_sleep_logic.h). A wake is trusted as dormant
 * only when BOTH HAD_SWCORE_PD and our marker agree. A missing transient source
 * remains dormant-unknown; it must not fall into cold-only boot behavior. */
static void test_decode_wake(void) {
    /* Without the power-down flag or the marker, every latch combination must
     * decode COLD (battery insert, watchdog, flash reboot, aborted entry). */
    for (int button = 0; button <= 1; button++) {
        for (int sys_int = 0; sys_int <= 1; sys_int++) {
            assert_true(power_sleep_decode_wake(false, false, button, sys_int, true) ==
                            POWER_WAKE_COLD,
                        "no evidence at all -> COLD");
            assert_true(power_sleep_decode_wake(true, false, button, sys_int, true) ==
                            POWER_WAKE_COLD,
                        "power-down without our marker -> COLD");
            assert_true(power_sleep_decode_wake(false, true, button, sys_int, true) ==
                            POWER_WAKE_COLD,
                        "marker without a power-down -> COLD");
        }
    }

    assert_true(power_sleep_decode_wake(true, true, true, false, false) == POWER_WAKE_BUTTON,
                "button latch -> BUTTON");
    assert_true(power_sleep_decode_wake(true, true, true, true, true) == POWER_WAKE_BUTTON,
                "button outranks a coincident SYS_INT");
    assert_true(power_sleep_decode_wake(true, true, false, true, true) == POWER_WAKE_SYS_INT,
                "SYS_INT latch alone -> SYS_INT");
    assert_true(power_sleep_decode_wake(true, true, false, false, true) ==
                    POWER_WAKE_SERVICE_VBUS,
                "direct service VBUS has its own wake cause");
    assert_true(power_sleep_decode_wake(true, true, false, false, false) ==
                    POWER_WAKE_DORMANT_UNKNOWN,
                "dormant evidence but no latched source -> DORMANT_UNKNOWN");
}

static void test_last_swcore_pwrup_bit_decode(void) {
    power_sleep_hal_wake_evidence_t evidence = {0};
    power_sleep_hal_wake_sources_t sources;

    power_sleep_hal_decode_wake_sources(&evidence, 0x02u, &sources);
    assert_true(sources.button && !sources.shared_irq && !sources.service_vbus,
                "LAST 0x02 -> PWRUP0/button");

    power_sleep_hal_decode_wake_sources(&evidence, 0x04u, &sources);
    assert_true(!sources.button && sources.shared_irq && !sources.service_vbus,
                "LAST 0x04 -> PWRUP1/shared IRQ");

    power_sleep_hal_decode_wake_sources(&evidence, 0x08u, &sources);
    assert_true(!sources.button && !sources.shared_irq && sources.service_vbus,
                "LAST 0x08 -> PWRUP2/service VBUS");

    power_sleep_hal_decode_wake_sources(&evidence, 0x06u, &sources);
    assert_true(sources.button && sources.shared_irq && !sources.service_vbus,
                "LAST 0x06 preserves coincident PWRUP0+PWRUP1");

    evidence.shared_irq = 1u << 9;
    power_sleep_hal_decode_wake_sources(&evidence, 0u, &sources);
    assert_true(!sources.button && sources.shared_irq && !sources.service_vbus,
                "transient PWRUP STATUS remains valid evidence");
}

static void test_armed_level_race_closure(void) {
    assert_true(
        power_sleep_check_armed_levels(
            false, false, false, false, true, true, false) ==
            POWER_SLEEP_ARM_CLEAR,
        "all armed wake levels inactive -> entry is clear");
    assert_true(
        power_sleep_check_armed_levels(
            true, false, false, false, true, true, false) ==
            POWER_SLEEP_ARM_BUTTON_LOW,
        "button asserted after arm aborts");
    assert_true(
        power_sleep_check_armed_levels(
            false, true, false, false, true, true, false) ==
            POWER_SLEEP_ARM_SYS_INT_LOW,
        "shared IRQ asserted after arm aborts");
    assert_true(
        power_sleep_check_armed_levels(
            false, false, false, false, false, true, false) ==
            POWER_SLEEP_ARM_LEVEL_READ_FAILED,
        "unreadable charger status aborts");
    assert_true(
        power_sleep_check_armed_levels(
            false, false, true, false, true, true, false) ==
            POWER_SLEEP_ARM_EXTERNAL_POWER,
        "service VBUS asserted after arm aborts");
    assert_true(
        power_sleep_check_armed_levels(
            false, false, false, true, true, true, false) ==
            POWER_SLEEP_ARM_EXTERNAL_POWER,
        "live USB asserted after arm aborts");
    assert_true(
        power_sleep_check_armed_levels(
            false, false, false, false, true, false, false) ==
            POWER_SLEEP_ARM_CHARGER_STATUS_LOW,
        "charger status asserted after arm aborts");
    assert_true(
        power_sleep_check_armed_levels(
            false, false, false, false, true, false, true) ==
            POWER_SLEEP_ARM_CLEAR,
        "explicit stuck-STAT2 policy permits only that source");
    assert_true(
        power_sleep_check_armed_levels(
            true, true, true, true, false, false, true) ==
            POWER_SLEEP_ARM_BUTTON_LOW,
        "button has deterministic priority among simultaneous aborts");
}

int main(void) {
    test_decode_wake();
    test_last_swcore_pwrup_bit_decode();
    test_armed_level_race_closure();
    if (s_failures != 0) {
        fprintf(stderr, "%d failures\n", s_failures);
        return 1;
    }
    printf("power_sleep tests passed\n");
    return 0;
}

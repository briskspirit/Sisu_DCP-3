#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>

#include "services/standby_sleep_logic.h"

static standby_sleep_readiness_t all_ready(void) {
    return (standby_sleep_readiness_t){
        .standby_route = true,
        .backlight_off = true,
        .app_idle = true,
        .audio_idle = true,
        .core1_idle = true,
        .storage_idle = true,
        .modem_asleep = true,
        .uart_idle = true,
        .usb_absent = true,
        .wake_levels_clear = true,
        .wake_latches_clear = true,
        .quiet_clock = true,
        .accessory_idle = true,
        .battery_idle = true,
    };
}

static void test_every_blocker_is_independent(void) {
    standby_sleep_readiness_t ready = all_ready();
    assert(standby_sleep_blockers(&ready) == 0u);

#define CHECK_BLOCK(field, bit) do {            \
        standby_sleep_readiness_t one = ready;  \
        one.field = false;                      \
        assert(standby_sleep_blockers(&one) == (bit)); \
    } while (0)
    CHECK_BLOCK(standby_route, STANDBY_SLEEP_BLOCK_ROUTE);
    CHECK_BLOCK(backlight_off, STANDBY_SLEEP_BLOCK_BACKLIGHT);
    CHECK_BLOCK(app_idle, STANDBY_SLEEP_BLOCK_APP_WORK);
    CHECK_BLOCK(audio_idle, STANDBY_SLEEP_BLOCK_AUDIO);
    CHECK_BLOCK(core1_idle, STANDBY_SLEEP_BLOCK_CORE1);
    CHECK_BLOCK(storage_idle, STANDBY_SLEEP_BLOCK_STORAGE);
    CHECK_BLOCK(modem_asleep, STANDBY_SLEEP_BLOCK_MODEM);
    CHECK_BLOCK(uart_idle, STANDBY_SLEEP_BLOCK_UART);
    CHECK_BLOCK(usb_absent, STANDBY_SLEEP_BLOCK_USB);
    CHECK_BLOCK(wake_levels_clear, STANDBY_SLEEP_BLOCK_WAKE_LEVEL);
    CHECK_BLOCK(wake_latches_clear, STANDBY_SLEEP_BLOCK_WAKE_LATCH);
    CHECK_BLOCK(quiet_clock, STANDBY_SLEEP_BLOCK_QUIET_CLOCK);
    CHECK_BLOCK(accessory_idle, STANDBY_SLEEP_BLOCK_ACCESSORY);
    CHECK_BLOCK(battery_idle, STANDBY_SLEEP_BLOCK_BATTERY);
#undef CHECK_BLOCK

    ready.audio_idle = false;
    ready.modem_asleep = false;
    ready.usb_absent = false;
    assert(standby_sleep_blockers(&ready) ==
           (STANDBY_SLEEP_BLOCK_AUDIO | STANDBY_SLEEP_BLOCK_MODEM |
            STANDBY_SLEEP_BLOCK_USB));
    assert(standby_sleep_blockers(NULL) == UINT32_MAX);
}

static void test_stability_and_periodic_deadline(void) {
    assert(!standby_sleep_stable_for(1031u, 1000u, 32u));
    assert(standby_sleep_stable_for(1032u, 1000u, 32u));
    assert(!standby_sleep_stable_for(999u, 1000u, 0u));

    assert(standby_sleep_advance_deadline(0u, 1000u, 60000u) == 61000u);
    assert(standby_sleep_advance_deadline(61000u, 60999u, 60000u) ==
           61000u);
    assert(standby_sleep_advance_deadline(61000u, 61000u, 60000u) ==
           121000u);
    assert(standby_sleep_advance_deadline(61000u, 181001u, 60000u) ==
           241000u);
    assert(standby_sleep_advance_deadline(1u, UINT64_MAX, 1u) == UINT64_MAX);
    assert(standby_sleep_advance_deadline(1u, 2u, 0u) == UINT64_MAX);

    assert(standby_sleep_maintenance_deadline(
               61000u, 2000u, 60000u, false, 12345u) == 61000u);
    assert(standby_sleep_maintenance_deadline(
               61000u, 2000u, 60000u, true, 40000u) == 42000u);
    assert(standby_sleep_maintenance_deadline(
               61000u, 41999u, 60000u, true, 1u) == 42000u);
    assert(standby_sleep_maintenance_deadline(
               61000u, 42000u, 60000u, true, 60000u) == 102000u);
    assert(standby_sleep_maintenance_deadline(
               61000u, 42000u, 60000u, true, 0u) == 102000u);
    assert(standby_sleep_maintenance_deadline(
               61000u, UINT64_MAX - 10u, 60000u, true, 20u) == UINT64_MAX);

    assert(standby_sleep_deadline_remaining(61000u, 1000u, 10u) == 60000u);
    assert(standby_sleep_deadline_remaining(1005u, 1000u, 10u) == 10u);
    assert(standby_sleep_deadline_remaining(1000u, 1000u, 10u) == 10u);
    assert(standby_sleep_deadline_remaining(UINT64_MAX, 0u, 10u) ==
           UINT32_MAX);

    assert(standby_sleep_select_duration(60000u, UINT32_MAX, 10u) ==
           60000u);
    assert(standby_sleep_select_duration(60000u, 512u, 10u) == 512u);
    assert(standby_sleep_select_duration(60000u, 1u, 10u) == 10u);
    assert(standby_sleep_select_duration(5u, UINT32_MAX, 10u) == 10u);
}

int main(void) {
    test_every_blocker_is_independent();
    test_stability_and_periodic_deadline();
    puts("standby sleep logic tests passed");
    return 0;
}

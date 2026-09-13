#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "hardware/structs/watchdog.h"
#include "services/runtime_watchdog.h"

#ifndef TEST_WATCHDOG_BOOT_CASE
#define TEST_WATCHDOG_BOOT_CASE 0
#endif

static int s_failures;
static watchdog_hw_t s_watchdog_hw;
watchdog_hw_t *watchdog_hw = &s_watchdog_hw;

static bool s_stub_caused_reboot;
static uint32_t s_enable_calls;
static uint32_t s_disable_calls;
static uint32_t s_update_calls;
static uint32_t s_last_delay_ms;
static bool s_last_pause_on_debug;

bool watchdog_enable_caused_reboot(void) {
    return s_stub_caused_reboot;
}

void watchdog_disable(void) {
    s_disable_calls++;
}

void watchdog_enable(uint32_t delay_ms, bool pause_on_debug) {
    s_enable_calls++;
    s_last_delay_ms = delay_ms;
    s_last_pause_on_debug = pause_on_debug;
}

void watchdog_update(void) {
    s_update_calls++;
}

static void check(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        s_failures++;
    }
}

static void check_stamp(runtime_watchdog_phase_t expected,
                        const char *message) {
    /* A fresh process is used for every boot case, so the live scratch value
     * can be decoded by temporarily treating it as previous-boot evidence only
     * through its documented magic/phase wire contract. */
    uint32_t stamp = s_watchdog_hw.scratch[5];
    check((stamp & 0xffffff00u) == 0x53495300u &&
              (stamp & 0xffu) == (uint32_t)expected,
          message);
}

int main(void) {
    memset(&s_watchdog_hw, 0, sizeof(s_watchdog_hw));
#if TEST_WATCHDOG_BOOT_CASE == 1
    s_stub_caused_reboot = true;
    s_watchdog_hw.scratch[5] =
        0x53495300u | (uint32_t)RUNTIME_WATCHDOG_PHASE_STANDBY;
#elif TEST_WATCHDOG_BOOT_CASE == 2
    s_stub_caused_reboot = true;
    s_watchdog_hw.scratch[5] = 0xdeadbeefu;
#else
    s_stub_caused_reboot = false;
    s_watchdog_hw.scratch[5] = 0x53495300u |
        (uint32_t)RUNTIME_WATCHDOG_PHASE_FLASH;
#endif

    runtime_watchdog_boot_capture();
    runtime_watchdog_boot_capture();
    check(s_disable_calls == 1u, "boot capture is idempotent");
    check(!runtime_watchdog_started(), "boot capture leaves runtime lease off");

    runtime_watchdog_boot_evidence_t evidence;
    runtime_watchdog_get_boot_evidence(&evidence);
#if TEST_WATCHDOG_BOOT_CASE == 1
    check(evidence.timeout_reset, "runtime timeout is retained");
    check(evidence.phase_valid &&
              evidence.phase == RUNTIME_WATCHDOG_PHASE_STANDBY,
          "valid timeout phase is decoded");
#elif TEST_WATCHDOG_BOOT_CASE == 2
    check(evidence.timeout_reset, "malformed timeout still records reset kind");
    check(!evidence.phase_valid &&
              evidence.phase == RUNTIME_WATCHDOG_PHASE_NONE,
          "malformed timeout phase fails closed");
#else
    check(!evidence.timeout_reset && !evidence.phase_valid,
          "non-watchdog boot ignores stale phase scratch");
#endif

    runtime_watchdog_flash_begin();
    check(s_enable_calls == 1u &&
              s_last_delay_ms == RUNTIME_WATCHDOG_FLASH_TIMEOUT_MS,
          "standalone flash window arms the short lease");
    check_stamp(RUNTIME_WATCHDOG_PHASE_FLASH,
                "standalone flash window publishes its phase");
    runtime_watchdog_flash_checkpoint();
    check(s_update_calls == 1u, "flash checkpoint refreshes the short lease");
    runtime_watchdog_flash_end();
    check(s_disable_calls == 2u,
          "early standalone flash window disarms on completion");

    runtime_watchdog_start();
    runtime_watchdog_start();
    check(runtime_watchdog_started(), "runtime lease starts");
    check(s_enable_calls == 2u &&
              s_last_delay_ms == RUNTIME_WATCHDOG_MAIN_TIMEOUT_MS &&
              s_last_pause_on_debug,
          "runtime lease is armed once with debugger pause");
    check_stamp(RUNTIME_WATCHDOG_PHASE_BOOT,
                "runtime start publishes boot phase");

    runtime_watchdog_note_phase(RUNTIME_WATCHDOG_PHASE_POWER_OFF);
    check_stamp(RUNTIME_WATCHDOG_PHASE_POWER_OFF,
                "phase note changes provenance without feeding");
    check(s_update_calls == 1u, "phase note does not refresh the deadline");
    runtime_watchdog_feed(RUNTIME_WATCHDOG_PHASE_MAIN_LOOP);
    check_stamp(RUNTIME_WATCHDOG_PHASE_MAIN_LOOP,
                "healthy-turn feed publishes main phase");
    check(s_update_calls == 2u, "healthy-turn feed refreshes runtime lease");

    runtime_watchdog_flash_begin();
    check(s_enable_calls == 3u &&
              s_last_delay_ms == RUNTIME_WATCHDOG_FLASH_TIMEOUT_MS,
          "runtime flash window replaces main lease with short lease");
    runtime_watchdog_flash_begin();
    check(s_enable_calls == 3u && s_update_calls == 3u,
          "nested flash begin refreshes rather than replacing the short lease");
    runtime_watchdog_flash_checkpoint();
    check(s_update_calls == 4u, "runtime flash checkpoint refreshes short lease");
    runtime_watchdog_flash_end();
    check(s_enable_calls == 3u && s_update_calls == 5u,
          "inner flash completion keeps and refreshes the short lease");
    check_stamp(RUNTIME_WATCHDOG_PHASE_FLASH,
                "inner flash completion retains flash provenance");
    runtime_watchdog_flash_end();
    check(s_enable_calls == 4u &&
              s_last_delay_ms == RUNTIME_WATCHDOG_MAIN_TIMEOUT_MS,
          "outer flash completion restores the runtime lease");
    check_stamp(RUNTIME_WATCHDOG_PHASE_MAIN_LOOP,
                "flash completion restores main provenance");
    runtime_watchdog_flash_end();
    check(s_enable_calls == 4u,
          "duplicate flash completion cannot alter watchdog ownership");

    check(strcmp(runtime_watchdog_phase_name(RUNTIME_WATCHDOG_PHASE_FLASH),
                 "flash") == 0,
          "phase name identifies flash");
    check(strcmp(runtime_watchdog_phase_name((runtime_watchdog_phase_t)99),
                 "unknown") == 0,
          "invalid phase name fails closed");
    check(strcmp(runtime_watchdog_phase_name(
                     RUNTIME_WATCHDOG_PHASE_DEBUG_HANG),
                 "debug-hang") == 0,
          "phase name identifies the intentional bench hang");

    if (s_failures == 0) {
        puts("PASS: runtime watchdog");
    }
    return s_failures == 0 ? 0 : 1;
}

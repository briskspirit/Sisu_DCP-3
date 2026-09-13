# Runtime Watchdog

Status: code-complete, host/ARM verified, and all six Rev B2 hardware gates
passed as of 2026-08-30.

## Ownership

`runtime_watchdog` is the sole owner of continuous watchdog enable, feed, and
disable operations. Immediate reboot calls remain local to the recovery paths
that intentionally never return.

- The normal runtime lease is 12 seconds and pauses under the debugger.
- The main loop feeds once, near its end, only after the complete service,
  application, render, storage, and power scheduling turn returned normally.
- Phase writes do not feed. A loop that repeatedly reaches only an early phase
  cannot extend its deadline accidentally.
- Internal-flash erase/program temporarily replaces the main lease with a
  depth-balanced 3-second lease. The final flash exit restores the 12-second
  lease; an early-boot flash operation instead disarms its standalone lease.

At the first instruction in `main`, the service captures
`watchdog_enable_caused_reboot()` and watchdog scratch 5, then disarms any
deadline inherited from ROM, picotool, or the previous application. Runtime
leases stamp `SIS` plus a phase byte into scratch 5. The captured result remains
available for the boot log and the CDC `wake` command throughout that boot.

## Dormant Contract

Powered standby switches `clk_ref` to LPOSC and sets `SLEEP_EN0` to POWMAN
only before entering deep sleep. RP2350's TICKS block is therefore gated, so
the watchdog countdown pauses during the up-to-60-second dormant interval. It
resumes after `runtime_init_clocks()` restores the tick generators. The lease is
fed immediately before the standby attempt, leaving the full 12-second bound
for arm and clock restoration.

P1.7 power-off removes the switched core domain. Before that point the active
watchdog remains useful: a half-shutdown deadlock reboots instead of holding the
phone awake indefinitely. Existing warm-boot handling owns modem/rail recovery.

## Verification

Host tests cover lease ownership, nested flash operations, reset provenance,
phase stamps, debugger behavior, and the dormant pause/resume contract. The
intentional `wdhang main confirm` and `wdhang flash confirm` commands are
exact-confirmation development hooks; malformed forms only print usage.

All six Rev B2 hardware checks passed:

- normal boot reported a live watchdog without false reset provenance;
- a main-loop hang reset after approximately 11.9 seconds with the debug-hang
  phase preserved;
- a flash-lease hang reset after approximately 2.9 seconds with the flash phase
  preserved;
- an ordinary setting survived normal reboot and battery-removal cold boot;
- USB-absent dormant standby survived repeated maintenance and physical wakes,
  including sleeping SMS after the UART RX hold fix, without UART errors or a
  false timeout; and
- P1.7 power-off woke by both Power and RTC alarm without a false runtime
  timeout or shutdown-entry abort.

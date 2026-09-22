# Power-Off Design

Status: implemented and bench-qualified on Rev B2. With the battery input at
2.65 V, service USB removed, and the CDC-enabled image's three GPIO wake paths
armed, the measured POWMAN P1.7 floor is 0.84 mA. The release profile retains
GP7/GP42 but omits GP28. Its USB isolation, cable-attached dormant behavior,
ordinary Power wake, and cable-plus-Power BOOTSEL recovery are bench-qualified;
its floor has not been measured as a separate long current window.

## Two Off States

The handset presents one power-off UX backed by two firmware states:

- Soft off keeps the application alive while shutdown work is incomplete,
  charging animation or an alarm needs the UI, or a peripheral still owns
  power. Attached service USB is an additional veto only in the CDC-enabled
  image.
- Dormant off enters RP2350 POWMAN P1.7. The switched core, XIP, and SRAM are
  lost, so every wake is a bootrom-to-flash restart.

The transition is intentionally conservative. A failed I2C operation, pending
flash write, asserted wake source, live modem rail, or attached charger leaves
the handset in functional soft off instead of crossing a partially completed
power boundary.

## Eligibility

`power_sleep_tick()` requires all of the following continuously for the
2-second settle window:

- the application route is `APP_ROUTE_POWER_OFF`
- the modem service reports fully off
- the shared 3.8 V rail has no owner and is physically disabled
- no charger is present
- GP28 service VBUS and TinyUSB are inactive (CDC-enabled image only)
- the power button and shared interrupt are at their inactive levels
- the retry backoff from any previous aborted entry has elapsed

The ordinary powered-on clock-gating mask remains enabled during this fallible
period. It is cleared only after every abortable check passes; carrying that
mask into P1.7 was the root cause of the repeatable 2.40 mA off-state result.

A qualified Power hold can arrive before the fresh LTC startup window has
enough valid samples. That explicit power-on request waits up to three seconds
in soft off for battery admission; it is not discarded or treated as permission
to start the modem. A completed refusal or timeout clears the request. Releasing
after the qualified hold does not cancel it, but a short tap never creates one.
Deep-sleep entry is deferred only while this bounded request remains pending.

Wake initialization keeps the LCD in power-down while applying calibration and
clearing its RAM. Key activity in the off route cannot enable the backlight;
accepted power-on and alarm paths own that transition. A short Power tap or a
pending/refused battery qualification therefore remains dark. Charging can
still wake the LCD for its existing off-state animation without lighting it.
The controller's serial interface remains usable in power-down
([PCD8544 datasheet](https://cdn-shop.adafruit.com/datasheets/pcd8544.pdf),
sections 8.2-8.3).

## Entry Sequence

The production sequence is fallible-first:

1. Flush every dirty storage journal while both cores and flash coordination
   are still available.
2. Commit the requested RTC alarm state, drain all GP42 owners, and re-read the
   RTC alarm flag.
3. Put the LTC2959 into smart sleep and arm the TCA8418 charger-status wake.
4. Reset all four POWMAN wake slots and arm falling edges on GP7 and GP42. The
   CDC-enabled image also arms a rising edge on GP28.
5. Re-read the button, shared IRQ, charger STAT input, and a fresh un-debounced
   GP43 charger-voltage sample. The CDC-enabled image additionally rechecks
   VBUS and USB state. Any active source rolls back.
6. Clear normal core-WFI sleep gating, force the shared 3.8 V rail off, stop
   core 1, quiesce both I2S domains, place the codec in standby, stop MCLK,
   disable backlight, and power down the LCD.
7. Deinitialize USB, power down and isolate its PHY, then park every non-wake
   pad at its reviewed dormant level.
8. Configure POWMAN for P1.7 using LPOSC, retain wake telemetry in scratch, and
   request the all-domains-off state.

If POWMAN refuses the request or a wake arrives while the sequencer is waiting,
firmware records the condition and performs a clean watchdog reboot. It does
not continue running after destructive teardown.

## Wake Sources

| Source | Electrical path | Boot behavior |
|---|---|---|
| Power button | GP7 falling edge | The held press is credited to the normal 1.2-second power-on gesture. A short press returns to off. |
| RTC alarm | RV-8803 AF holds GP42 low | AF is captured before alarm initialization clears it, then reinjected after app initialization so the alarm rings normally. |
| Charger status | TCA8418 latches a BQ25171 status edge onto GP42 | The phone enters charging UI and remains awake while charging work is active. |
| Service USB | GP28 rising edge, CDC-enabled image only | Soft off stays awake, USB enumerates, and CDC diagnostics become available. Release ignores this edge. |
| LTC2959 alert | Open-drain shared GP42 path | The shared IRQ service identifies and clears the source before normal policy resumes. |

Wake decoding requires switched-core power-down evidence plus the firmware
scratch marker. Power button outranks shared IRQ, which outranks service VBUS.
A proven dormant wake with no surviving source latch remains an explicit
unknown-dormant cause and is never misclassified as a cold battery insertion.

## Charger Discovery Policy

GP43 is an ADC input and cannot directly wake P1.7. Ordinary charger insertion
changes BQ25171 STAT and therefore wakes through the TCA8418. The exceptional
case is attaching a charger to an already-full pack while STAT remains idle.

A periodic RV-8803 heartbeat was implemented and measured:

- dormant floor: 0.84 mA
- optimized wake: about 33 mA for 325 ms
- 60-second cadence average: about 1.01 mA

That cost still provided feedback much later than a user expects. Production
therefore has no charger-discovery heartbeat. It performs a fresh GP43 check
during any normal awake maintenance and at the final dormant boundary. Closing
the already-full insertion corner immediately requires a low-power comparator
on a wake-capable digital input.

## Bootloader Recovery

BOOTSEL requires service VBUS and a continuous 5-second Power hold. Both
profiles support this at cold boot. The release image also supports it from
soft off and after a GP7 P1.7 wake, so removing CDC does not require opening the
phone or reflashing through a running console. Releasing after the ordinary
1.2-second threshold keeps normal power-on behavior. The cold/wake gate observes
VBUS for a bounded 12-second window so the slow battery-insertion rail ramp
cannot hide the recovery path.

## Bench Qualification

The following paths were repeated on Rev B2:

- initial battery-powered dormant entry at 0.84 mA
- short power-button wake and return to 0.84 mA
- normal power-on, UI power-off, and return to 0.84 mA
- service-USB wake, CDC enumeration, unplug, and return to dormant
  (CDC-enabled image)
- charger insertion/removal and powered-off charging UI
- scheduled RTC alarm wake, snooze, re-ring, dismissal, and return to dormant
- all required GPIO wake slots armed together without the former 2.40 mA state
- release-profile no-enumeration/no-USB-wake behavior with the cable attached
- release-profile ordinary Power wake and cable-first 5-second Power BOOTSEL

For a 1200-1250 mAh pack, the 0.84 mA floor corresponds to an idealized
59-62 days before accounting for NiMH self-discharge, temperature, aging, and
capacity variation.

## Retaining The Modem While Off

This is an estimate, not an implemented alternative to P1.7. It was examined
because [Phone security](phone_security.md#startup-delay) waits for the modem
to provide a verified SIM identity at startup.

Telit's LE910Cx Hardware Design Guide, 1VV0301298 Rev.46, Table 20 (pages
49-50), specifies typical module currents of 19 uA in PSM or supplied-but-off,
1.8-2.1 mA for ThreadX LTE DRX in `CFUN=5`, and 14 mA in radio-off `CFUN=4`.
These are module-rail figures, not battery currents or measured Sisu values.
The older PSM application note quotes 7.7 uA; the estimate below uses the
newer hardware guide's 19 uA instead. Vendor documents are listed in the
[Telit ThreadX download zone](https://www.telit.com/le910cx-tx-download-zone/).

For an illustrative 2.65 V battery, 3.8 V modem rail, and assumed 85 percent
conversion efficiency:

```text
additional battery mA = modem mA * 3.8 / (2.65 * 0.85)
total battery mA = 0.84 + additional battery mA + additional rail overhead
ideal days = 1250 / (total battery mA * 24)
```

The table omits additional rail overhead, wake/re-registration energy and
battery self-discharge. It assumes the rest of the board can retain its
existing 0.84 mA floor; current firmware cannot do this with a live modem.

| Mode | Additional battery current | Total before rail overhead | Drain per day | Ideal 1250 mAh duration |
|---|---:|---:|---:|---:|
| Current rail-off design | 0 mA | 0.840 mA | 20.2 mAh | 62.0 days |
| PSM or supplied-but-off, static floor only | 0.032 mA | 0.872 mA | 20.9 mAh | 59.7 days |
| Registered `CFUN=5` DRX | 3.04-3.54 mA | 3.88-4.38 mA | 93-105 mAh | 11.9-13.4 days |
| Radio-off `CFUN=4` | 23.62 mA | 24.46 mA | 587 mAh | 2.1 days |

The TPS63020 itself has a typical 25 uA operating quiescent current
([TI datasheet/product page](https://www.ti.com/product/TPS63020)); its enabled
rail, feedback divider and level translators also need a measured budget.
Efficiency at microamp loads cannot be assumed to match the illustrative
85 percent above. A lower battery voltage increases the converted current.

PSM is not retained instant resume. Telit's PSM Application Note,
80502NT11758A Rev.3, pages 9-15, describes network-controlled entry/timers,
SIM power-down, and boot/camp/TAU on exit. Host-requested wake requires at
least a five-second ON_OFF_N assertion. Its periodic wake energy is absent
from the static-floor row. Supplied-but-off likewise still needs startup.

`CFUN=5` retains service, so calls/SMS and network activity can occur while
the handset appears off. `CFUN=0` is not a documented radio-off microamp
alternative: it remains responsive to network events, uses RTS rather than
DTR for host wake, and the hardware guide warns it can consume 300 uA more
than `CFUN=5`. `CFUN=4` disables RF and deactivates the SIM, so even retaining
the baseband would not eliminate SIM activation or a carrier-profile change.

Retaining any mode would require new rail ownership, dormant pad/wake policy,
and recovery after the RP loses RAM in P1.7. Current off entry explicitly
requires the modem rail off and parks DTR low. The preferred next investigation
is earlier verified SIM identification and better startup feedback, before
accepting a permanent off-state power cost. The current rail-off policy is
unchanged.

## Known Limits

- A charger attached to an already-full powered-off pack may not create a STAT
  edge; this is the accepted GP43/comparator limitation above.
- Ordinary keypad keys do not wake a powered-off phone. Only the power button
  does, matching the intended product behavior.
- Long-cycle POWMAN soak remains useful when upgrading the Pico SDK because
  wake and resume fixes can change silicon-facing behavior.
- The 0.84 mA figure is a complete board measurement, not an RP-only current.
  Further reduction requires a rail-by-rail hardware budget as well as firmware
  changes.

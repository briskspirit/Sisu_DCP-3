# Powered-On Standby

Status: implemented and bench-qualified on Rev B2. The final one-hour,
registered, service-USB-disconnected soak has a retrospectively calibrated
ACR average of 16.710 mA, with Telit CTS-confirmed sleep residency above
98 percent. The originally reported 11.642 mA used the LTC2959 data-sheet ACR
scale that later bench comparison found low on this hardware.

This document is the maintained powered-on standby contract. Historical
implementation diaries and intermediate failures are intentionally excluded
from the active documentation set.

## Operating States

The powered-on phone uses three execution levels:

1. Full-speed foreground at 153.6 MHz for rendering, audio, flash operations,
   USB work, modem transitions, and other latency-sensitive activity.
2. A 6 MHz quiet-awake path for short maintenance work when no foreground
   subsystem needs the full clock.
3. POWMAN clock-dormant intervals while the standby screen is idle. SRAM and
   application state remain powered; clocks stop until a wake source fires.

This is distinct from powered-off P1.7, where switched-core state and SRAM are
lost and wake starts again through bootrom. See [Power-off design](power_off_design.md).

## Dormant Eligibility

Entry requires one complete, stable readiness census:

- standby route with no pending event, render, dialog, or backlight work;
- audio playback and bridge idle, codec/I2S gated, and core1 command state
  quiescent;
- no flash journal work or flash/core1 coordination in flight;
- Telit READY with DTR sleep requested, CTS confirming sleep, RI inactive, and
  UART TX/RX plus the PL011 FIFO empty;
- no pending accessory, charger, RTC, battery, shared-IRQ, or input work;
- service VBUS absent and TinyUSB inactive (CDC-enabled image only);
- every wake input at its inactive level; and
- the 6 MHz quiet clock already established.

Core1 is parked before clocks stop. Readiness is checked again after that park
and once more at the interrupt-off HAL boundary. An assertion crossing the
entry window either fails a level recheck or is retained by POWMAN/TCA wake
evidence; it cannot be consumed as ordinary work and then lost to sleep.

## Wake Sources

| Source | Purpose |
|---|---|
| GP37 Telit RI | Incoming call, SMS, and qualified modem-originated activity |
| GP42 shared interrupt | Keypad, headset, charger STAT, RTC alarm, and LTC alert |
| GP7 power button | Immediate user wake |
| GP28 service VBUS | USB service attach and CDC recovery (CDC-enabled image only) |
| POWMAN AON alarm | Absolute 60-second maintenance deadline or an earlier app deadline |

The TCA8418 re-arms headset and charger inputs for the opposite accepted level.
Keypad PRESS evidence survives one scan so a quick press/release already queued
in hardware cannot collapse into no key. Wake cleanup acknowledges only the
sources actually captured, preserving concurrent events.

## Time And Clock Restoration

TIMER0 does not advance while its clock is stopped. The standby HAL therefore
uses the POWMAN AON counter as wall-time authority across dormant and rebases
the firmware timebase before normal scheduling resumes.

Pico SDK 2.2.0 stops POWMAN while changing tick sources and may restart from
the last explicitly written value. The HAL checkpoints the live counter before
each XOSC/LPOSC handoff and uses the RP2350 OTP LPOSC calibration when valid.
This local workaround is part of the qualified clock contract.

The project stays pinned to SDK 2.2.0 until a newer SDK is reviewed for RP2350
timer, POWMAN, USB, multicore, flash, DMA, and PIO changes and passes the full
standby regression gate. SDK 2.3.0 is specifically unqualified because its
RP2350 `sleep_until()` regression affects the 125 Hz main loop. An SDK version
bump is not a routine build-only change.

## Maintenance Policy

The absolute 60-second maintenance wake updates the software clock mirror,
battery/LTC state, registration and signal backstops, and the Rev B2 GP43
charger-presence fallback. It returns to dormant as soon as work drains.

Stable charging does not keep the CPU continuously awake. The charging
animation publishes its next 512 ms frame deadline, and the standby controller
uses the earlier of that deadline and normal maintenance. User interaction
keeps foreground scheduling active through the backlight interval to avoid
sleep churn between key presses.

Battery sampling cadences are opportunities while the CPU is already running;
they are not independent wake deadlines. Modem-side PIO/DMA exists only for a
voice session. In the CDC-enabled image, USB clocks and PHY state follow
physical service VBUS. The release image keeps them parked regardless of VBUS.

## Measured Result

The maintained qualification point is the post-indicator-cleanup soak:

| Measurement | Result |
|---|---:|
| Elapsed LTC2959 window | 3600.255 s |
| Original ACR delta at data-sheet scale | -11.643 mAh |
| Calibration-revision-2 ACR delta | -16.711 mAh |
| Calibrated average battery current | 16.710 mA |
| Telit READY residency | 3600.243 s |
| DTR sleep requested | 3538.897 s (98.30%) |
| CTS-confirmed sleep | 3537.829 s (98.27%) |
| Modem sleep entries/wakes | 52 / 52 |

The phone was registered to the serving LTE network, untouched at standby,
with the Nokia charger and service USB disconnected. UART drops, overruns, line
errors, TX stalls, modem wake timeouts, unknown wakes, core1 park failures, and
LTC I2C/ARA errors were all zero.

The representative pre-dormant baseline was approximately 24 mA. For the
fitted 1200-1250 mAh pack, simple division at 16.710 mA gives 71.8-74.8 hours;
allowing 80-90 percent usable capacity gives a planning range of roughly
57.5-67.3 hours. These are bench budgets, not a product guarantee: coverage,
network search, calls, messages, display/audio use, temperature, pack aging,
self-discharge, and converter efficiency all matter.

The scale correction and its evidence are maintained in
[`ltc2959_acr_calibration.md`](ltc2959_acr_calibration.md). No new one-hour soak
was run merely to perform this arithmetic correction. Replacing the maintained
headline requires a comparable calibrated soak rather than arithmetic alone.

In the CDC-enabled image, service USB prevents dormant and therefore produces a
materially higher diagnostic current. USB-connected service-image snapshots
must not be compared directly with the qualified 16.710 mA result. The release
image deliberately ignores VBUS for this policy.

### Battery-Gauge Regression (2026-09-01)

The Nokia-derived load-compensated estimator was followed by a fresh
USB-absent regression window. The completed LTC interval was 658.246 seconds.
Its original data-sheet-scale delta was -1.804 mAh / 9.867 mA; calibration
revision 2 maps that to approximately -2.589 mAh / 14.16 mA. Telit
CTS-confirmed sleep occupied 646.781 seconds (98.26 percent), with 9 sleep
entries and 9 wakes. The dormant controller reported no pre-arm, final-check,
or core1 abort and no battery-owned blocker; LTC I2C and ARA errors stayed zero.

This is a passing regression sample, not a new headline current. The one-hour
16.710 mA run above remains the maintained qualification because it averages
more network and maintenance variation.

## Regression Gate

After any change to clocks, POWMAN, USB, modem sleep/RI, shared IRQ, TCA8418,
RTC, battery cadence, flash coordination, codec, PIO, DMA, or Pico SDK:

1. Build and identify both profiles. For the CDC-enabled image, boot once with
   service USB absent and once present. For release, verify that attachment does
   not enumerate or disturb the phone.
2. Reach registered standby and confirm clock-dormant entry. Repeat release
   once with USB attached.
3. Let an untouched 60-second maintenance wake complete and verify monotonic
   time plus immediate return to dormant.
4. Wake independently with keypad, power button, SMS RI, incoming-call RI,
   headset insertion/removal, charger attach/removal, and RTC alarm. Test USB
   wake only in the CDC-enabled image. Repeat the physical sources for at least
   ten consecutive cycles, then run a mixed-source sequence.
5. Verify service-image USB detach/re-enumeration; verify release-image
   cable-first 5-second Power BOOTSEL. In both profiles verify SMS indication,
   bidirectional call audio, alarm snooze/re-ring, and no duplicate or lost
   input after every wake.
6. Inspect dormant blockers/aborts, modem UART counters, shared-IRQ evidence,
   core1 park/audio recovery, RTC, and LTC errors.
7. Run a service-USB-disconnected LTC window under comparable battery and
   serving-cell conditions. Record current, dormant residency, and Telit sleep
   residency rather than inferring savings from an instantaneous reading.

Any missed wake or URC, duplicate input, audio alignment failure, timer jump,
USB recovery failure, stuck rail owner, or unexplained unknown wake is a stop
condition.

## Accepted Limits

- A charger attached to an already-full pack may produce no STAT edge. GP43 is
  checked during normal maintenance, but immediate interrupt-driven discovery
  would require a comparator on a wake-capable input in a later board revision.
- The 60-second AON cadence is deliberate; the RV-8803 countdown timer is not
  also run as a redundant heartbeat.
- Further current reduction needs a new measured rail budget. The current
  architecture is considered qualified, not an open-ended optimization task.

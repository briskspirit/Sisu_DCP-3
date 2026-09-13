# Rev B2 USB VBUS Backfeed Erratum

Status: mandatory Board-1 bench rework applied and verified on 2026-08-14.

## Affected circuit

The service connector exposes two USB pairs:

- RP USB D+/D- through U6 (`USBLC6-2SC6`)
- Telit USB D+/D- through U14 (`USBLC6-2SC6`)

Both protection devices have pin 5 connected to `EXT_VBUS`. `EXT_VBUS` also
feeds Telit `USB_VBUS` A13 directly and drives RP GP28 `VBUS_DET` through the
R18/R15 100k:100k divider.

## Failure mechanism

With the service cable absent, the active USB D+ idle bias from either USB
device can feed the corresponding USBLC6 steering diode and raise the common
pin-5 rail. Because both pin-5 rails are tied to `EXT_VBUS`, the circuit
self-biases Telit `USB_VBUS` even though no external VBUS source is present.

This violates the condition required for Telit low-power operation. It also
makes GP28 correctly report a voltage that the board generated internally,
rather than a connected service cable.

The RP was ruled out as a GPIO-output source. Live pad state was:

```text
GP28 function=SIO, direction=input, output latch=0,
input enabled, pull-up=off, pull-down=off, isolation=off
```

RP2354B/RP2350B GP28 is not an ADC pin. All firmware handling of this signal
must remain digital; the ADC bank is GP40-GP47.

## Board-1 evidence

Measurements were taken at the service connector with external USB removed:

| Condition | EXT_VBUS | RP D+ | RP D- | Telit D+ |
|---|---:|---:|---:|---:|
| Both USB devices active | 2.04 V | not recorded | not recorded | not recorded |
| RP TinyUSB D+ pull-up disabled | 1.76 V | 0 V | 0 V | 2.52 V |
| U6 pin 5 and U14 pin 5 lifted | 0 V | not required | not required | not required |

The intermediate measurement isolates two independent contributors: removing
the RP D+ bias reduced but did not eliminate `EXT_VBUS`; Telit D+ remained at
2.52 V and continued to feed it through U14.

The current result confirms the functional consequence:

| Firmware/state | Original data-sheet-scale ACR average | Calibration-revision-2 equivalent |
|---|---:|---:|
| Diagnostic, `CFUN=0`, before rework | 62.121 mA | 89.161 mA |
| Diagnostic, `CFUN=0`, after rework | 25.903 mA | 37.178 mA |
| Production, registered `CFUN=5`, after rework | 24.880 mA | 35.710 mA |

The registered production window lasted 599.993 s. The service was READY for
599.994 s, requested DTR sleep for 563.317 s, and observed CTS-confirmed sleep
for 562.424 s (93.74%). It completed 34 wake cycles with no DTR wake timeout,
UART drop, overrun, line error, or TX stall.

These are whole-board measurements from different sessions. The later LTC2959
ACR calibration changes their absolute scale but not their experimental ratio
or conclusion; the inferred backfeed loss changes from the originally reported
36.2 mA to approximately 52.0 mA. They prove the
backfeed loss and successful registered sleep, but they
must not be subtracted from an older rail-off baseline to infer a precise
standalone modem current.

## Rev B2 disposition

Lift pin 5 on both U6 and U14. This leaves each protected data path connected
through pins 1/6 and 3/4 while breaking the D+-to-`EXT_VBUS` backfeed path.
Board-1 measured `EXT_VBUS=0 V` after this rework.

This is a bench rework, not approval of the resulting protection network for
production. Reworked boards must still pass USB enumeration and ESD/system
qualification.

## Rev C requirement

Redesign both USB protection networks so an active data-line bias cannot
source the service-VBUS detection net or Telit `USB_VBUS`. Use a protection
topology and parts whose rail/reference connection remains valid when the
protected USB device is locally powered and the cable VBUS is absent. Keep
`EXT_VBUS` as the sole source of cable-presence truth and retain GP28 as a
digital input through the divider.

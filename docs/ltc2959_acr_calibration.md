# LTC2959 ACR Calibration

Status: calibration revision 2 is implemented, host-tested, and qualified for
the assembled Rev B2 DUT. High- and low-current same-chip cross-checks and
subsequent full/empty-cycle continuity are complete. No external-reference or
second-board measurement supports treating 3825 nAh/count as a universal
LTC2959 calibration.

## Why This Exists

The LTC2959 data sheet specifies an accumulated-charge-register (ACR) LSB of
533 nAh with a 50 mohm sense resistor:

```text
Q_LSB = 533 nAh * (50 mohm / R_sense)
```

Rev B2 uses a 10 mohm, 1 percent, four-terminal R50, so the direct data-sheet
conversion is 2665 nAh/count. During a stable charge on 2026-09-03, however,
the ACR advanced by only about 69.7 percent of the charge obtained by
integrating the LTC2959's own current ADC over the same intervals.

This is not a sense-resistor calibration: the instantaneous-current and ACR
measurements share R50, so its absolute tolerance cancels from their ratio.
The firmware therefore carries both the data-sheet value and a separate Rev B2
effective value.

Primary reference:

- Analog Devices, LTC2959 Rev. A data sheet, especially QLSB/Note 5, current
  ADC scaling, and the CFP/CFN application circuit:
  https://www.analog.com/media/en/technical-documentation/data-sheets/ltc2959.pdf

## Controlled DUT Evidence

The battery was charging steadily with the phone in soft-off and service USB
attached. The current ADC varied by only 0.32 percent RMS in the dedicated
three-minute capture, ruling out charger duty-cycle aliasing as an explanation.

| Capture | ACR counts | Data-sheet ACR | Integrated current | Ratio |
|---|---:|---:|---:|---:|
| Extended in-charge interval | 44,991 | 119,901,015 nAh | 172,056,653 nAh | 0.696869 |
| 179.651 s, 180 samples | 3,136 | 8,357,440 nAh | 11,954,108 nAh | 0.699127 |
| 600.010 s normal-radio discharge, 1 Hz | 3,983 | 10,614,695 nAh | 14,881,486 nAh | 0.713277 |
| 600.001 s rail-off discharge, 1 Hz | 1,680 | 4,477,200 nAh | 6,275,933 nAh | 0.713403 |

The second capture's LTC current was tightly clustered:

```text
minimum 234430 uA
mean    239548.9 uA
maximum 240975 uA
sigma   764.1 uA
```

The independently inferred effective scales are:

```text
extended interval: 3824.246 nAh/count
179.651 s interval: 3811.897 nAh/count
```

Calibration revision 2 uses 3825 nAh/count. Applied retrospectively, it gives:

| Capture | Calibrated ACR | Difference from current integral |
|---|---:|---:|
| Extended in-charge interval | 172,090,575 nAh | +0.020 percent |
| 179.651 s interval | 11,995,200 nAh | +0.344 percent |
| Normal-radio discharge | 15,234,975 nAh | +2.375 percent |
| Rail-off discharge | 6,426,000 nAh | +2.391 percent |

The longer in-charge interval receives more weight because fixed
sample/timestamp edge errors are a smaller fraction of its total and the
approximately 240 mA current makes current-ADC offset a small relative error.
The normal-radio discharge was bursty, but the rail-off repeat was controlled:
601 of 601 samples succeeded, current averaged -37.655 mA with 0.791 mA RMS
spread, and terminal voltage moved only 2577 to 2571 mV. Both discharge traces
independently inferred approximately 3736 nAh/count.

That 2.39 percent low-current difference does not establish a direction- or
current-dependent ACR scale. At 3825 nAh/count the rail-off ACR implies
38.556 mA, only 0.900 mA above the sampled-current mean. Across the 10 mohm
shunt this is 9.00 uV, or 3.03 current-ADC LSBs. The LTC2959 data sheet permits
up to +/-8 LSB current offset in addition to +/-1 percent gain error, so the
observed difference is inside the independent current measurement's specified
uncertainty. The high-current capture remains the stronger scale estimate.
The rounded 3825 value is an empirical board calibration, not a correction
published by Analog Devices.

Combining the dedicated positive and negative captures as an affine two-point
fit removes any constant *difference* between the ACR and current-ADC offsets:

```text
+239.547 mA at +62,841.84 ACR counts/hour
 -37.656 mA at -10,079.98 ACR counts/hour
slope       = 3801.36 nAh/count
intercept   = +0.662 mA
```

The fitted slope is only 0.62 percent below 3825. It still includes the current
ADC's allowed gain error, and the intercept combines two physically distinct
offsets, so it is not a sound reason to revise the active coefficient. It does
show that one gain-only coefficient cannot make every low-current comparison
exact.

## Deadband And Very Low Current

The current configuration uses the 20 uV coulomb-counter deadband. Across the
10 mohm shunt this is approximately 2 mA: charge from any 0.5-second interval
whose average magnitude is below that threshold is deliberately not added to
the ACR. This does not affect the 38 mA and 240 mA calibration captures, but it
can omit the measured 0.84 mA soft-off baseline entirely. A continuously
omitted 0.84 mA is 20.2 mAh/day, before considering NiMH self-discharge.

Changing Q_LSB cannot repair this because the loss depends on operating-state
residency, not ACR count. Setting the deadband to zero is not automatically
more accurate either: the specified coulomb-counter offset is typically 1 uV
and up to +/-10 uV, equivalent to 0.1 mA typical and +/-1 mA worst case on this
board, which then integrates continuously. Production precision therefore
needs either a characterized zero-deadband offset correction or a separate
low-power-residency correction while retaining the deadband.

## Independent Corroboration

An Analog Devices EngineerZone report dated 2026-03-19 describes a separate
LTC2959 implementation whose ACR measured about 68.9 to 69.3 percent of the
expected charge at three stable currents, while its current ADC and external
instrument agreed. That design used the data-sheet 50 mohm shunt, `ADC=0xA0`,
`CC=0x10`, a midpoint ACR, and 470 nF between CFP and CFN. The report had no
vendor resolution when checked.

This is corroboration only, not an erratum or an explanation:

- https://ez.analog.com/power/battery-management-system/f/qa/603498/ltc2959-coloumb-counter-measuring-68-5-lower-than-expected-value

## Hardware Audit

The Rev B2 design source was checked against the data-sheet application
requirements:

- R50 is a 10 mohm, 1 percent, four-terminal Kelvin resistor;
- C21 is 470 nF (`GRM155R61A474KE15D`) and connects only CFP to CFN;
- the DFN exposed pad is intentionally unsoldered, matching the data sheet's
  explicit instruction not to connect or solder pin 11; and
- firmware leaves counting enabled and uses the 20 uV deadband, equivalent to
  only about 2 mA through R50. The calibration current produced roughly
  2.34-2.41 mV across R50, more than 100 times that threshold. The independent
  report used no deadband and observed the same class of deficit.

No schematic, BOM, footprint, or obvious register-setting cause for a
30-percent deficit was found. The numerical resemblance to any mathematical
constant or possible capacitor behavior is not evidence and must not be turned
into a hardware change without a controlled reproduction.

## Firmware And Persistence Contract

`LTC2959_ACR_DATASHEET_LSB_NAH` preserves the published 2665 nAh/count value.
`LTC2959_ACR_LSB_NAH` is the active 3825 nAh/count conversion, and
`LTC2959_ACR_SCALE_REVISION` is 2. All session deltas, ACR-window currents,
remaining charge, learned capacity, and charge-supervisor input use the active
conversion.

Stored values containing converted charge cannot cross this boundary:

- battery-learning profile 1 used the data-sheet conversion; profile 2 owns
  calibration revision 2;
- charge-supervisor hardware profile 2 represented the fitted 273 mA/8-hour
  Rev B2 rework with the old conversion; profile 3 represents the same hardware
  with calibration revision 2; and
- active charge baselines and terminal summaries from another profile are
  rejected and repaired instead of being relabeled.

The previously accepted 865 mAh cycle becomes 1241.5 mAh when its raw charge is
rescaled by `3825 / 2665`. This is a one-cycle estimate, not proof of the pack's
marked capacity or of a universal board calibration. It agrees with the pack's
stated 1200-to-1250 mAh range, but firmware deliberately does not migrate it
silently.

Later profile-3 discharge cycles retained gauge continuity and produced recent
accepted capacity samples of 1272, 1301, and 1239 mAh. Their coherence supports
the use of calibration revision 2 on this DUT, but it is not an independent ACR
scale measurement. The 1.224 charge factor is a separate battery-terminal input
efficiency used by the charge supervisor; it does not alter QLSB.

## Qualification Scope

- The active 3825 nAh/count value is board-specific empirical calibration. An
  external instrument and a second Rev B2 board have not independently measured
  its gain and offset.
- The 20 uV deadband can omit the measured 0.84 mA soft-off state. Coulomb SOC
  across long powered-off storage therefore excludes that sub-deadband load and
  NiMH self-discharge.
- A multi-board product calibration requires an end-to-end calibration identity
  or provisioned per-board QLSB/offset with versioning and CRC. Raw anchors and
  derived history must remain tied to that identity.
- Integer nAh/count already provides 0.026 percent resolution at 3825; finer
  numeric formatting would add digits without improving the measurement.

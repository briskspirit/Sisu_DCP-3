# Battery Gauge

Status: the Rev B2 two-cell NiMH estimator and battery health/SOC supervisor are
implemented and host-tested. Loaded-call bars, charge/unplug behavior, natural
idle EMPTY, shutdown, and durable endpoint recovery are qualified on the
installed Rev B2 pack. Multi-pack resistance calibration and the controlled
in-call 1800 mV emergency boundary remain outside that evidence.

The deeper reverse-engineering record, addresses, and evidence grades are in
[`battery_gauge_nokia_v600_research.md`](battery_gauge_nokia_v600_research.md).
This document describes the code that now ships.

## Ownership

`battery_hal` owns the production estimate:

- the LTC2959 supplies coherent terminal voltage, current, and ACR evidence;
- `battery_gauge_logic` is a pure estimator with no Pico SDK dependency;
- `board_diag_service` publishes the estimate and preserves deterministic
  voltage overrides for Net Monitor and CDC tests;
- `app_status_runtime` consumes only the estimator's LOW/EMPTY state and owns
  dialog/repetition timing;
- `battery_status_logic` independently confirms physical terminal collapse.

These boundaries are deliberate. Load compensation may stabilize a user-facing
bar, but it can never hide fresh terminal-voltage collapse, TPS63020 PG loss, or
Telit PWRMON loss.

LTC voltage is authoritative only when `SISU_LTC2959_VOLTAGE_AUTHORITATIVE` is
enabled and the current snapshot passed settling and plausibility checks. A
numeric zero never means an empty pack; callers must honor validity.

## Sampling And Sleep

LTC single-shot opportunities are:

| Existing awake context | Requested cadence |
|---|---:|
| Telit startup, call, or active audio | 250 ms |
| Active UI or charging | 1 s |
| Quiet/soft-off diagnostic work | 5 s |

The estimator itself advances at most once per second. Faster conversions still
refresh terminal voltage for the independent collapse supervisor, but cannot
accelerate the display filter.

These periods are not wake deadlines. P1.7 dormant residency remains governed
by the existing phone-maintenance schedule and interrupt sources. The gauge does
not wake the RP merely to obtain another sample. After a real dormant gap, the
slow fixed-point state advances in elapsed-time domain, capped at 60 one-second
steps, instead of turning a 15-second algorithm into a 15-minute one.

At initialization or after an LTC gauge-session change, acquisition temporarily
requests 250 ms opportunities until it has five valid conversions or has seen
ten conversion attempts. It does this only while core 0 is already awake.

## LTC Session And Sign Contract

The hardware ACR midpoint (`0x80000000`) is the battery-session zero. The LTC
continues counting while the RP sleeps and retains ACR across an RP reset, so
`session_delta_nah` is reconstructed from hardware after every boot. UVLO/POR
evidence starts a new session and causes one verified four-byte midpoint write.
A voltage jump alone only re-settles voltage; modem load release must not erase
charge history.

Rev B2 current polarity is bench-qualified:

- charging current and ACR movement are positive;
- discharge current and ACR movement are negative;
- the registered one-hour standby soak moved the ACR monotonically in the
  discharge direction; its data-sheet-scale `-11.643 mAh` result recalibrates
  to `-16.711 mAh`, or `16.710 mA` average;
- charger tests moved current in the positive direction.

Absolute ACR conversion uses the assembled-board calibration of 3825
nAh/count rather than the direct 2665 nAh/count data-sheet scaling for the
10 mohm shunt. The two controlled charge captures, independent corroboration,
hardware audit, persistence migration, and qualification boundary are in
[`ltc2959_acr_calibration.md`](ltc2959_acr_calibration.md).

`current_polarity_verified` is therefore true for this board routing. A board
profile without verified current polarity still accepts voltage, but uses a
neutral 65 mA correction and rejects ACR-window current.

## Nokia Calibration Domain

The active NSE-8 PM record `0x205` contains:

```text
empty      1900 mV
low        2320 mV
terminal   2230 mV
resistance 190 mohm
reference  65 mA
bars       2575, 2530, 2425, 2425, 2425, 2425, 2425, 2425 mV
```

The display map is `100, 75, 50, 25, 25, 25, 25, 25`, producing:

| Icon | Reference-load floor |
|---:|---:|
| 4 bars | 2575 mV |
| 3 bars | 2530 mV |
| 2 bars | 2425 mV |
| 1 bar | 1900 mV |
| 0 bars | below 1900 mV |

The wide one-bar region is authentic.

The 65 mA value is not an estimate of Sisu's standby draw. It is the coordinate
system in which Nokia calibrated this table. Rev B2 measured roughly 16.7 mA in
registered standby, and that real current is normalized to the Nokia domain.
Changing the 65 mA reference would require recalibrating the complete voltage
table; the hardware-specific parameter to bench-fit first is effective
resistance.

## Reference-Load Compensation

For ordinary discharge:

```text
I_now_mA    = magnitude of the coherent LTC discharge current
I_window_mA = ACR delta over the nearest valid 4..7 second window
I_used_mA   = min(I_now_mA, I_window_mA) when the window is valid

correction_mV = (I_used_mA - 65 mA) * 190 mohm / 1000
V65_mV        = V_fast_terminal_mV + correction_mV
```

The lower-of-two rule follows Nokia's conservative immediate-versus-five-second
load choice. It prevents a single high current sample from over-correcting a
terminal sag. The ACR calculation is based on conversion timestamps, not on a
nominal polling rate.

While BQ status says charging is active, 85 percent of measured charging current
is subtracted before applying the same reference transformation. Compensation
is clamped to +/-150 mV as a Rev B2 bench-policy guard. Arithmetic is saturating
at the public `int16_t` boundary.

This is a 65 mA reference-load voltage, not open-circuit voltage. Examples at
the initial 190 mohm profile are approximately:

| Real current | Correction |
|---:|---:|
| 12 mA discharge | -10 mV |
| 65 mA discharge | 0 mV |
| 300 mA discharge | +45 mV |
| 100 mA active charge | -29 mV |
| 200 mA active charge | -45 mV |

The charging current is always the coherent LTC measurement. No nominal
charger-current limit is compiled into the voltage estimator; changing charger
hardware therefore does not require changing this calculation. Charge-session
evidence still uses a separate immutable hardware profile ID so curves and
efficiency factors cannot cross an ISET or timer change.

## Robust Voltage Pipeline

Each accepted one-second model step does the following:

1. The primary fast terminal track snaps upward and moves downward by one tenth
   of its error.
2. The symmetric fast terminal track moves by one tenth in either direction.
3. Current compensation converts the primary fast track to the 65 mA domain.
4. A time-pruned 15-entry median window rejects short radio-load outliers.
5. A slow fixed-point state moves toward that median by one tenth per elapsed
   one-second step.

On a new session all 15 median entries and the slow state seed at corrected
voltage plus the recovered 23 mV startup term. Invalid samples mark the live
estimate unavailable but do not fabricate an empty pack or blank the last
qualified standby icon. Net Monitor still reports `NO SAMPLE`, so the cached
icon is never presented as fresh diagnostic evidence. A later valid sample
restores validity without double-stepping a sub-second model interval.

## Bar Projection

Falling bars follow the robust reference-load estimate directly. An adjacent
rise uses Nokia's midpoint rule:

- 1 to 2 bars: 2477 mV, midpoint of 2425 and 2530;
- 2 to 3 bars: 2552 mV, midpoint of 2530 and 2575;
- 3 to 4 bars: 2597 mV, the top-level extrapolation of 2575 mV plus half
  the 2575-to-2530 spacing.

The threshold scan must first reach the candidate level. The midpoint test is
then applied only to an adjacent rise; it cannot promote a candidate which the
scan did not produce. Falling boundaries remain the direct 2425, 2530, and
2575 mV floors. This ordering is important: promoting before the scan caused a
live 3/4-bar oscillation while charging near 2552 mV.

Discharge-time upward movement is additionally blocked until ACR consumption
since the last committed bar reaches 150 mAh. Active charging bypasses this
rearm gate. A new session seeds immediately and never crawls upward from a
fabricated zero.

The old implementation's +/-40 mV and 10/30-second dwell has been removed. ROM
routine `0x27ce7c` owns transient/drop state, not displayed-bar hysteresis; using
it as icon dwell was a reconstruction error.

## LOW And EMPTY

Warning classification is independent of bars. Healthy requires both:

- terminal or symmetric-fast evidence at or above 2230 mV; and
- reference-load or primary-fast evidence at or above 2320 mV.

If that conjunction fails, primary-fast terminal voltage at or above 1900 mV
is LOW; below 1900 mV is EMPTY. A healthy-to-warning transition must remain
failed for 20 seconds. LOW-to-EMPTY and recovery to healthy are immediate.

The health supervisor then fuses that committed warning with its independent
ACR ledger. Projected capacity exhaustion alone remains non-terminal while the
voltage classifier is healthy, so unexpected extra capacity continues to be
measured. Outside a call, projected exhaustion plus committed LOW is a natural
EMPTY endpoint: the independent domains agree, and waiting for 1900 mV or the
raw operating floor would leave too little loaded-rail margin to journal the
cycle. During a call this particular fusion waits for load release, while the
existing remaining-charge and emergency-floor rules retain authority.

The app then applies the recovered user-facing cadence:

- LOW opens immediately on accepted entry, repeats after about 61 seconds,
  then about every 614 seconds;
- EMPTY opens once and powers off after about 2.056 seconds;
- verified positive charging recovery and already-powered-off routes suppress
  low/empty notices. Physical charger presence alone does not.

Separately, Rev B2 applies a chemistry/product operating floor to fresh LTC
samples. For the current 2S NiMH hardware, two distinct samples below 2100 mV
enter EMPTY while no call transaction is live. This deliberately matches the
power-on admission floor: the September 2026 controlled discharge reached a
last valid 2054 mV sample and then reset by RP2350 BOR before the 1900 mV Nokia
EMPTY classifier or the old 1800 mV guard could act. During a live call the
threshold remains 1800 mV, preserving the rule that LOW does not voluntarily
end a call. Gauge EMPTY or marginal modem-supply PG collapse is classified as
load sag while an anchored coulomb ledger still shows charge remaining.
Changing between those policies resets the two-sample streak, and re-polling a
cached sample cannot manufacture confirmation. The lower in-call boundary still
needs converter-dropout bench characterization.

On the first EMPTY action, the battery-health supervisor consumes the endpoint and an
immediate store flush is attempted before the empty tone/dialog is posted. Each
poll during the bounded countdown retries while anything remains dirty. This
gives the capacity record and natural-empty marker priority over user-facing
work at the battery knee; the ordinary soft-off path remains a final retry
backstop for transient flash-park failures.

## Power-On Qualification

Every completed LTC conversion increments `conversion_sequence`, including a
transport or decode failure. A separate accepted-voltage `sample_sequence`
continues to identify fresh terminal evidence.

The power-on qualifier:

1. observes at most ten initial conversion attempts at the accelerated cadence;
2. averages the first five valid terminal samples when available;
3. thereafter keeps a rolling five-valid-sample window fresh;
4. admits a no-charger power-on only when qualification is ready and its average
   is at least 2100 mV;
5. retains that bounded decision across an isolated failed latest conversion;
6. fails closed when no valid voltage exists;
7. after a voltage refusal, allows one fresh GP43 charger observation to begin
   a recovery check, but admits power-on only when valid BQ ACTIVE status and
   requested/read-back `/CE` enabled also prove that charger recovery is live.

Debug-forced battery voltage remains a deterministic single-value gate and does
not pretend to have five physical LTC samples.

After admission, the existing 250 ms TPS63020 PG and Telit PWRMON supervision
continues through modem startup and live service. Each intended-supply PG
failure publishes one vendor-neutral event. The battery owner calls it EMPTY
only when fresh valid terminal evidence is below 2320 mV and the pack is not in
verified positive charging recovery. During a call, anchored positive remaining
charge instead classifies that marginal event as load sag and leaves the 1800 mV
floor as the hardware backstop. A healthy-voltage regulator or modem fault
remains a modem fault. An accepted natural-EMPTY verdict is latched through the
existing 2.056-second shutdown even though terminal voltage may rebound when the
modem load vanishes. An isolated failed LTC conversion cannot cancel or restart
that countdown, or re-arm an existing LOW warning cadence. Charger VIN with an
idle, disabled, faulted, or net-discharging charge path likewise cannot hide or
cancel EMPTY. Cancellation requires valid BQ ACTIVE state, enabled `/CE`
readback, continuous polarity-qualified LTC current, and current strictly into
the battery. Completion of soft-off clears the latch. The event remains pending
while the LTC is settling, and a new modem power epoch clears stale unconsumed
evidence.

If supply PG failed after Telit may already be live, the resulting battery
shutdown does not try to recover a parked UART. It enters the existing bounded
hardware-control ladder directly: graceful ON/OFF control first, GP39 only if
needed, then qualified-low PWRMON before +3V8 ownership is released. This avoids
both unsafe blind rail cuts and a dark soft-off UI retaining the modem rail.

This closes the intended marginal-pack UX in code without letting the estimator
overrule physical evidence. The idle 2S NiMH path has passed a natural endpoint;
the lower in-call 1800 mV boundary remains outside controlled-source evidence.
Any other chemistry requires a separately selected and calibrated profile.

## Charger Observation

BQ25171 STAT1/STAT2 are sampled every second and after TCA8418 charger-status
interrupts. GP43 distinguishes physical charger presence from the ambiguous
STAT=(1,1) idle/full state. A real edge requests a bounded series of 80 ms GP43
confirmation samples; quiet analog maintenance remains on the existing
60-second wake.

Detach qualification suppresses transient STAT=(1,1), preventing an incomplete
unplug from showing `Battery full`. Before powered-off dormant entry, firmware
takes one fresh un-debounced GP43 sample as a conservative sleep veto.

## Diagnostics

Net Monitor page 40 now presents:

- `T`: fresh LTC terminal millivolts;
- `R`: robust 65 mA reference-load millivolts;
- `C`: current load correction in millivolts;
- `B`: committed bars;
- `W`: `-`, `L`, or `E` warning state;
- `I`: ACR-derived approximately five-second current when available;
- `BOOT`: valid samples versus conversion attempts.

Pages 41 and 42 retain raw ACR/session measurement and LTC transport/status
evidence. Page 47 exposes learned capacity, confidence, full-anchor,
remaining-mAh, and resistance-bin evidence. This is enough to compare idle,
Telit startup, ringing, active call, load release, charging, and dormant wake
without adding temporary debug code.

## Battery Health Authority Boundary

The production UI uses health-supervisor SOC bars when observed or learned,
non-conflicted capacity and coherent SOC are available; otherwise it falls back
to the Nokia reference-load voltage ladder. The supervisor uses a 1225 mAh
product prior, qualified FULL/EMPTY anchors, coherent ACR movement, explicit
confidence, and rare checksummed persistence. It also collects median effective
resistance estimates in high/mid/low terminal-voltage bins.

The health supervisor treats known delivered capacity as a prediction, not an
EMPTY source. It keeps measuring any overrun until fused physical evidence
closes the cycle. During a call, anchored positive remaining charge downgrades
gauge EMPTY or a marginal supply-PG event to load sag; only the 1800 mV
emergency floor may still force shutdown, and that action does not teach a
capacity endpoint. Outside a call, the qualified Nokia classifier and 2100 mV
distinct-sample floor can close an earlier aging cycle. Power-on admission and
wake scheduling remain separate. LTC UVLO/new-session evidence clears all pack-
specific learning while retaining the selected chemistry profile's prior.
The persisted profile ID prevents a different chemistry profile from accepting
a NiMH record. The complete algorithm, evidence, and qualification scope are in
[`battery_learning.md`](battery_learning.md).

## Verification

Host coverage pins:

- PM threshold boundaries and midpoint rises;
- 65 mA correction, charging term, conservative current pair, and clamps;
- ACR-derived five-second current;
- one-sample modem-sag rejection and sparse elapsed-time behavior;
- warning entry/recovery and session invalidation;
- distinct-attempt five-of-ten power-on qualification;
- LTC conversion-attempt versus accepted-sample sequencing;
- exact Net Monitor page-40 rendering;
- app/HAL closure and the independent raw-collapse path;
- warning/countdown and standby-icon retention across an unavailable LTC sample;
- exhausted capacity plus committed LOW closing an idle endpoint while active
  calls and healthy-voltage overruns remain non-terminal;
- direct, bounded hardware shutdown after a supply-PG fault with parked UART.

## Rev B2 Bench Evidence

- A sustained voice call produced terminal-current peaks near 949 mA while the
  icon made one bounded four-to-three transition, never the former
  four-to-two-to-four collapse; TPS63020 PG remained asserted.
- Active charging held four bars after the corrected threshold ordering. A
  deliberate early unplug stopped the animation without a false `Battery full`
  notice even though BQ status changed 158 ms before GP43 qualified detach.
- Natural idle EMPTY was observed from distinct raw samples of 2074 and 2096 mV
  with the filtered estimate at 2119 mV. The dialog appeared, the approximately
  two-second shutdown armed, and the endpoint marker was flushed before power
  down. The next runtime loaded the anchored EMPTY record cleanly.
- That endpoint run began without a qualified FULL anchor, so it correctly
  rejected a new delivered-capacity sample. It qualifies the physical floor,
  UI, shutdown, and persistence path rather than a complete learning cycle.
- A USB-absent post-feature regression window averaged 14.16 mA with 98.26
  percent CTS-confirmed modem sleep and no battery-owned dormant blocker. The
  longer 16.710 mA run remains the standby qualification baseline.

## Calibration Scope

The ROM-grounded 190 mohm resistance remains the product default until several
packs have comparable high-, middle-, and low-SOC load traces. The in-call
1800 mV emergency floor remains a non-learning safety path; controlled-source
coverage would characterize its margin but is not required for ordinary idle
EMPTY authority.

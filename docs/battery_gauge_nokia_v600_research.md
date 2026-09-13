# Nokia 3210 v6.00 Battery-Gauge Reconstruction

Status: reconstruction complete. The Rev B2 load-normalized gauge, persistent
capacity/SOC supervisor, and fused endpoint policy are implemented and tested.
Loaded-call bars, charge/unplug behavior, and durable natural EMPTY have passed
on the installed pack. Multi-pack resistance data remains outside the evidence
used by production compensation. The exact shipped contracts are in
[`battery_gauge_design.md`](battery_gauge_design.md).

This note reconstructs the active Nokia 3210 NSE-8 two-cell NiMH battery
monitor from the v6.00 ROM and its factory EEPROM record, compares it with the
current Rev B2 implementation, and defines the safest way to reuse the design
with the LTC2959. The important distinction is that Nokia estimated load in
software, while Rev B2 can measure battery current and accumulated charge.

## Confidence Convention

- **Confirmed**: directly visible in executable control flow, constants, or a
  checksummed factory record.
- **Derived**: arithmetic units or intent reconstructed from multiple callers,
  tables, and timing constants. The mechanism is well supported, but an
  original Nokia symbol name is unavailable.
- **Adaptation**: a Sisu Rev B2 design choice, not a claim about Nokia behavior.
  `[BP]` marks an adaptation value whose hardware qualification is still
  bounded as described in the maintained implementation documents.

## Evidence

Ground-truth binaries:

- Nokia 3210 v6.00 MCU image `3210600c.fls`, SHA-256
  `0ab99ed809232d2c14c6587c9fc904323358f223e978b6215d15475a62362f6c`.
- NSE-8 repaired external EEPROM image `nse-8.bin`, SHA-256
  `e16598cc54ca0dc1c27216a23bc966bcb7c998225297358672eb1f1cbd87b484`.
- Ghidra program `3210600c.mcu.fls`, ARM big-endian v4t. Addresses below are
  firmware addresses in that program.

Nokia documentation (provenance and hashes in
[the reference index](nokia_3210_reference_index.md)):

- System Module Technical Documentation
  (`Nokia_3210_NSE-8_NSE-9_System_Module_Technical_Documentation.pdf`)
- Service Software Instructions
  (`Nokia_3210_NSE-8_NSE-9_Service_Software_Instructions.pdf`)

Independent primary references used only to validate the Rev B2 adaptation:

- [Analog Devices LTC2959 data sheet](https://www.analog.com/media/en/technical-documentation/data-sheets/ltc2959.pdf)
- [Energizer NiMH handbook](https://data.energizer.com/pdfs/nickelmetalhydride_appman.pdf)
- [Microchip AN1384, Ni-MH Battery Charger Application Library](https://ww1.microchip.com/downloads/en/Appnotes/01384A.pdf)

The manufacturer material agrees on a key limitation: the broad, flat NiMH
discharge plateau makes voltage alone a poor mid-state-of-charge estimator.
Coulometry is the useful backbone, while voltage, load, temperature, and
qualified endpoint observations constrain drift and protect the pack. The
LTC2959 continuously integrates current in hardware, so this does not require
periodic RP2350 wakeups.

## Original Hardware Context

The Nokia technical documentation confirms the NSE-8 product uses a semi-fixed
two-cell NiMH pack:

- nominal pack voltage: 2.4 V;
- documented operating range: 1.9 V to 3.6 V;
- software discharge cutoff: 1.9 V;
- 10-bit baseband ADC, one-point calibrated near 2.7 V;
- battery measurement routed through the CCONT RSSI ADC on NSE-8;
- analog filtering around the battery input to reject TDMA and general noise;
- a board NTC near battery ground, nominally 47 kohm at 25 C, B approximately
  4050.

The service document says 2.15 V for the power-on software limit. The shipped
v6.00 executable compares against 2100 mV. For behavior cloning, the ROM wins;
the service value is retained as a documentation discrepancy.

## Factory Battery Record

**Confirmed.** PM record `0x205` consists of three checksummed 32-byte records
at EEPROM offsets `0x334`, `0x354`, and `0x374`. The active NSE-8 NiMH path
loads record 0:

```text
1900, 2320, 2230, 160, 190, 65,
2575, 2530, 2425, 2425, 2425, 2425, 2425, 2425,
0, checksum
```

The fields established by their consumers are:

| Offset | Value | Meaning |
|---:|---:|---|
| `+0x00` | 1900 mV | empty/cutoff floor |
| `+0x02` | 2320 mV | low-battery floor |
| `+0x04` | 2230 mV | secondary terminal-voltage qualification floor |
| `+0x06` | 160 mV | paired-sample/drop initialization term; exact Nokia name unresolved |
| `+0x08` | 190 mohm | nominal effective battery resistance |
| `+0x0a` | 65 mA | reference load current |
| `+0x0c..0x1a` | 2575, 2530, 2425... mV | eight descending gauge breakpoints |

The percentage map at ROM `0x2e2054` is:

```text
100, 75, 50, 25, 25, 25, 25, 25
```

Together these produce the familiar four-level icon ladder: 2575 mV, 2530 mV,
2425 mV, and 1900 mV. The unusually broad one-bar region is intentional.

Records 1 and 2 contain 3.0-to-3.9 V thresholds and feed a generic dual-record
battery mode. That code is present in the ROM, but it is not the active NSE-8
two-cell NiMH algorithm. It must not be cited as stock 3210 NiMH behavior.

## Reconstructed Active Pipeline

The active battery task at `0x27dc54` runs at approximately one-second cadence
while the phone is awake. Its high-level order is:

```text
ADC acquisition
  -> two fast voltage tracks
  -> software load estimate
  -> normalize terminal voltage to the 65 mA reference load
  -> median-of-15 outlier rejection
  -> slow one-tenth IIR
  -> transient/drop-state update
  -> bar/percentage projection
  -> independent low/empty classifier
```

### ADC And Fast Tracks

**Confirmed.** `0x27cc74` reads ADC channel 7, applies factory calibration, and
produces millivolts. The conversion is approximately `raw * 1500 / 313`. Values
below about 1200 mV and values above the valid upper range are rejected.

**Confirmed.** `0x27cdbe` maintains two separate fixed-point voltage tracks:

- the primary track snaps immediately upward, but moves downward by one tenth
  of the error;
- the secondary track moves by one tenth in both directions;
- the newest raw readings are retained separately.

The earlier Sisu gauge copied only the first behavior. The estimator now
preserves both roles. The second track is used by Nokia's load/drop and warning
logic; it is not redundant display state. The exact electrical interpretation
of the paired ADC outputs is not fully named, so the implementation deliberately
uses role-based names rather than invented hardware labels.

### Software Current Model

**Confirmed.** Nokia has no direct discharge-current sensor feeding this gauge.
Routines around `0x2a6f1c`, `0x2a7180`, and `0x2b0ea0` track enabled subsystems,
radio states, and elapsed time against ROM power tables. PM record `0x204`
supplies a one-byte scale whose factory value is 100.

`0x2a7154` returns the current modeled load and `0x2a730a` returns an
approximately five-second average. `0x27d1c0` uses the lower of the two. This is
a conservative guard against over-correcting a transient or an uncertain load
model, not a generic averaging rule for every current sensor.

Rev B2 does not reproduce the subsystem ledger. A coherent LTC voltage and
current observation, plus ACR delta over a time window, supplies better evidence
at the HAL boundary.

### Reference-Load Compensation

**Confirmed mechanism; derived physical units.** `0x27d1c0` subtracts the
65 mA record value from the selected load, multiplies the signed difference by
the active 190 mohm resistance, and divides by 1000. In charge-related state it
also subtracts 85 percent of the modeled charging current before applying the
resistance term.

For ordinary discharge, the useful interpretation is:

```text
I_used_mA = min(I_modeled_now_mA, I_modeled_approximately_5s_mA)
V_65_mV = V_terminal_mV +
          (I_used_mA - 65 mA) * R_effective_mohm / 1000
```

This is not an open-circuit-voltage estimate. It normalizes every reading to a
65 mA reference load, which is the domain in which the factory voltage ladder
was calibrated. At an idle load below 65 mA the correction is slightly
negative; that is intentional. Treating Nokia's ladder as an OCV ladder would
systematically bias it upward.

`0x27d2ec` combines this correction with conservative retained-state rules. A
separate special path uses paired readings and bounds a measured drop to 300 mV.
The first-order equation above is well supported; some surrounding state names
remain unresolved and are not part of the Rev B2 adaptation.

### Robust Slow Estimate

**Confirmed.** Nokia does substantially more than one asymmetric EMA:

- `0x27d368` maintains a value/index ordered set;
- `0x27d420` replaces one member of a 15-entry ring while preserving order;
- `0x27d4ac` returns the median;
- `0x27d500` feeds that median into a one-tenth IIR;
- `0x27d3b4` seeds all 15 entries at corrected voltage plus 23 mV, then seeds
  the fixed-point slow state.

At Nokia's roughly 1 Hz update rate, the median window rejects short GSM burst
sags over an approximately 15-second horizon. Copying "15 samples" into Sisu's
one-minute dormant cadence would instead create a 15-minute filter and would
not be faithful. The time horizon, not the literal count, is the portable part.

### The 40 mV / 10 s / 30 s Routine

**Confirmed correction to the earlier Sisu documentation.** `0x27ce7c` uses
40 mV and 23 mV comparisons with timers that decode to approximately 10 and
30 seconds. Its outputs are transient/drop state at offsets `+0x14`, `+0x18`,
`+0x26`, and `+0x32` in the battery state object.

It is not the displayed-bar Schmitt trigger or a generic bar dwell. The old
`battery_gauge_bar_step()` and comments incorrectly attributed those values to
icon projection; the load-normalized estimator removed that reconstruction
error.

### Bar Projection And Rise Restraint

**Confirmed.** `0x27db40` scans the eight descending voltage thresholds and maps
the selected index through the 100/75/50/25 table. A falling or equal result can
be accepted directly after the upstream filtering. An adjacent rise also uses
a midpoint boundary between levels, but only after the threshold scan has
reached that candidate. For the visible four-bar projection this gives 2477 mV
for 1-to-2, 2552 mV for 2-to-3, and a special top-level extrapolation of
2597 mV for 3-to-4. The top value is 2575 mV plus half of the 2575-to-2530 mV
spacing; there is no higher table row to average with.

The load-bearing rise restraint is charge-throughput based, not a 10-second
timer. In the normal path, a rise can remain blocked until Nokia's software
consumption accumulator advances by `0xd2f0`, or 54,000 units. The current
ledger's arithmetic establishes one unit as 10 mA-seconds, so:

```text
54,000 * 10 mA*s = 540,000 mA*s = 150 mAh
```

Charge/special-state flags can bypass that gate. The fixed 150 mAh value should
not be described as a percentage of the original pack because the active ROM
record contains no confirmed nominal-capacity field.

The design intent is clear: a short load release must not make the icon climb.
Rev B2 uses measured coulombs and stores the 150 mAh rule in the active chemistry
profile rather than embedding it in the projection machinery.

### Low And Empty Classification

**Confirmed.** `0x27cbec` classifies warnings independently of the displayed
percentage. In the normal NiMH path, healthy requires both:

- the raw/fast terminal evidence to satisfy the 2230 mV qualification; and
- either the slow compensated estimate or the fast terminal estimate to satisfy
  the 2320 mV low threshold.

If that conjunction fails, a fast terminal value at or above 1900 mV is LOW;
below it is EMPTY. When the previous class was healthy, a transition into a
warning class is held until the failure persists for about 20 seconds. A later
LOW-to-EMPTY transition does not use that healthy-state delay. The caller then
forces the published percentage to 25 for LOW and 0 for EMPTY.

This dual-domain test is important. Compensation may keep the user-facing gauge
stable under load, but fresh terminal evidence still participates in the safety
decision.

### Power-On Qualification

**Confirmed.** `0x27d5fc` attempts at most ten battery conversions, stops once
five valid samples have been acquired, averages those valid samples, and refuses
power-on below 2100 mV. If no valid sample is obtained, it returns without making
a low-voltage cutoff decision.

The earlier Sisu gate checked one already-filtered value. The current code uses
a bounded five-valid-sample, ten-attempt qualifier before starting the Telit
load.

## Generic ROM Features Not Active For NSE-8 NiMH

These mechanisms are useful design references, but calling them stock 3210
NiMH behavior would be wrong.

### Temperature-Generated Tables

`0x2a7d90` treats temperatures outside -40 to 80 C as invalid and substitutes
25 C; it clamps the useful output to -15 through 75 C. The table at `0x2e2e90`
contains:

```text
(45 C, 87%), (40 C, 100%), (30 C, 115%), (20 C, 125%),
(10 C, 135%), (0 C, 165%), (-10 C, 244%)
```

The generic path interpolates these factors into resistance/voltage tables.
For example, 190 mohm would become about 228 mohm at 25 C and 314 mohm at
0 C. However, the relevant routines execute only in normalized battery mode 2,
not the active NSE-8 NiMH mode.

The LTC2959 reports its own die temperature, not pack temperature. It must not
be fed into this table as if it were Nokia's pack-adjacent NTC.

### Impedance Learning

The generic mode captures 12 load-transition records and qualifies resistance
updates using repeated same-mode observations, a 20-to-55 C window, and current
steps above roughly 229 mA. It derives delta-V/delta-I, applies temperature
correction, maps resistance into an aging index, filters it by one fifth, and
persists it.

That is evidence that Nokia understood impedance as a load-, age-, and
temperature-dependent quantity. It is not evidence that the shipped NSE-8
NiMH gauge learned its 190 mohm value; the active path uses the fixed record.

## Earlier Rev B2 Gap Analysis

This table records the gap which motivated the load-normalized adaptation; it
is historical, not a description of the current tree.

| Area | Earlier Rev B2 | Nokia v6.00 | Consequence |
|---|---|---|---|
| Measurement | LTC voltage/current/ACR | calibrated ADC + software load ledger | Rev B2 has better raw evidence |
| Load compensation | none | normalized to 65 mA using 190 mohm | calls can produce 4-to-2-to-4 bar bounce |
| Fast filter | snap up, one-tenth down | two distinct fast tracks | warning/drop evidence is collapsed |
| Robust filter | no median stage | median-of-15 then one-tenth IIR | burst/outlier rejection is incomplete |
| Bar transition | +/-40 mV plus 10/30 s dwell | midpoint + 150 mAh rise restraint | current code cites the wrong ROM mechanism |
| Warning | filtered voltage plus custom 60 mV hysteresis | dual fast/slow classifier plus 20 s entry hold | warning behavior is only approximate |
| Boot gate | one filtered/fresh value | average five valid of up to ten attempts | marginal startup is less robust |
| Capacity | raw session ACR only | coarse voltage gauge plus software energy ledger | no persistent SOC or learned capacity |
| Temperature | LTC die telemetry only | pack NTC; active NiMH compensation fixed | no trustworthy pack-temperature correction |

The implementation keeps physical shutdown evidence separate from the display
filter. A controlled September 2026 discharge showed why the domains cannot
share the Nokia 1900 mV cutoff: the last valid sample was 2054 mV, followed by
an RP2350 BOR before either the classifier or the former 1800 mV guard fired.
Rev B2 therefore uses two distinct samples below 2100 mV as its ordinary 2S
NiMH operating floor, matching power-on admission. During a live call it keeps
the 1800 mV `[BP]` emergency floor so LOW alone does not end the call. Threshold
changes reset the evidence streak. The lower in-call floor still needs bench
qualification against the 3.3 V converter and Telit rail behavior.

## Rev B2 Adaptation Rationale

### 1. Preserve Three Ownership Layers

**Adaptation.** Hardware acquisition, chemistry estimation, and shutdown safety
remain separate:

1. `battery_hal` publishes a coherent raw snapshot: terminal voltage, signed
   current, ACR, sample/session identity, charger state, and relevant PG state.
2. A pure estimator consumes snapshots plus an externally selected chemistry
   profile and publishes reference-load voltage, bars, SOC confidence, and
   warning evidence.
3. A safety supervisor owns raw startup qualification, PG collapse, true empty
   shutdown, and charger-fault policy. Compensation can never veto raw physical
   brownout evidence.

The estimator does not infer chemistry from voltage. It receives a
product-owned chemistry selection and uses a distinct profile.

The research used this neutral input/output shape to define the boundary:

```c
typedef struct {
    uint32_t now_ms;
    uint32_t sample_sequence;
    uint32_t gauge_session;
    uint16_t terminal_mv;
    int32_t current_ua;       /* positive charge, negative discharge */
    int64_t session_delta_nah;
    bool sample_valid;
    bool charger_present;
    bool charge_active;
    bool modem_pg;
} battery_observation_t;

typedef struct {
    uint16_t reference_mv;
    uint8_t bars;
    uint8_t soc_percent;
    uint8_t confidence;
    bool low;
    bool empty;
} battery_estimate_t;
```

This is a conceptual shape; the maintained headers define the shipped API.

### 2. Normalize To The Nokia Reference Load

For the initial 2S NiMH profile:

```text
I_discharge_mA = -current_ua / 1000
I_window_mA = ACR-derived average discharge current over about 5 s
I_used_mA = conservative_pair(I_discharge_mA, I_window_mA)
V_65_mV = V_terminal_mV +
          (I_used_mA - 65 mA) * R_effective_mohm / 1000
```

The voltage and instantaneous current must belong to the same completed LTC
conversion epoch. The ACR-derived average is valuable because it includes
energy between ADC snapshots. The implementation uses Nokia's lower-of-two
behavior and requires coherent epochs so an asynchronous low-current sample
cannot be paired with a burst-sag voltage.

Production uses `R_effective = 190 mohm` as the ROM-grounded default and clamps
the correction to +/-150 mV. Effective resistance includes the pack, contacts,
and the path represented by the voltage sample; learned bins remain diagnostic
until multi-pack evidence supports replacing the default.

Charging uses its own policy. NiMH polarization and less-than-unity charge
storage make a simple signed Ohmic correction unsuitable as the sole SOC
source. Charger state remains explicit and only a qualified full indication
creates an anchor.

### 3. Port The Time Behavior, Not Sparse Sample Counts

**Adaptation.** While already awake:

- 250 ms observations during Telit startup, calls, and other high-load work;
- 1 s observations during ordinary active UI/charging;
- the existing maintenance wake while dormant; no new battery wake deadline.

The implementation uses a 15-entry time-pruned median followed by a time-domain
IIR. It does not interpret Nokia's 15 samples as 15 dormant wakeups, and it
re-seeds the window when the gauge session changes. The old single asymmetric
filter was removed so load recovery is not counted twice.

### 4. Keep Bars And Safety Independent

The implementation retains Nokia's coarse ladder but feeds it `V_65_mV`, not
raw or OCV-estimated voltage. It reproduces the midpoint transition rule and
prevents discharge-time upward bounce using measured coulombs rather than the
mistaken 10-second dwell.

The current policy is:

- falling bars follow the robust reference-load estimate;
- rising bars require qualified charging throughput or a deliberate
  session/rest re-seed; discharge-time load release alone cannot raise them;
- charging may raise bars under a separate charge-state policy;
- loss of valid gauge data freezes the last icon briefly, then marks it unknown
  internally rather than fabricating an empty pack.

LOW and EMPTY remain separate from bars. Ordinary warning uses the Nokia
fast/slow conjunction. Raw terminal voltage and regulator/modem PG remain the
hard safety evidence.

### 5. Use A Hybrid Capacity Estimator

NiMH voltage is informative near full and near the discharge knee, but weak in
the middle. The implemented passive sidecar therefore uses:

- nominal initial capacity: 1225 mAh, the midpoint of the current 1200-to-1250
  mAh product packs;
- a qualified full anchor: charger present and BQ termination/full stable;
- discharged coulombs from the LTC ACR as the primary movement away from full;
- a qualified natural empty endpoint to learn usable capacity without asking
  the user to perform a special calibration cycle;
- no mid-cycle voltage reconciliation: remaining mAh stays unavailable without
  a trustworthy full anchor;
- explicit confidence that resets after an LTC UVLO/new battery session and
  reports inconsistent endpoint evidence.

The LTC counts current but does not know nominal capacity, charge efficiency,
self-discharge, battery replacement, or aging. ACR continuity across an RP reset
is useful; an LTC UVLO/POR starts a new battery epoch. Without a pack identity,
that epoch must discard pack-specific SOC because the battery may have been
replaced. The implemented new-session path discards exact pack history and
retains only the selected profile's bounded product prior.

Persist only rare, qualified anchors and learned capacity. Never write EEPROM
on each sample or each coulomb increment. A full/empty learning record needs a
version, chemistry id, epoch/continuity evidence, capacity bounds, and checksum.

### 6. Learn Resistance Carefully

The fixed 190 mohm Nokia value may be poor for aged or inexpensive modern packs.
The implemented sidecar first exposes diagnostic estimates:

```text
R_sample_mohm = 1000 * (V_before_mV - V_after_mV) /
                       (I_after_mA - I_before_mA)
```

Accept a sample only when charger is absent, voltage/current and ACR continuity
are valid, temperature is 10 to 35 C, adjacent samples are 100 to 1500 ms apart,
discharge load rises by at least 200 mA, terminal voltage falls by 5 to 300 mV,
and the result is 50 to 1000 mohm. Debug-forced evidence is ineligible.

It takes a median of nine qualified steps in high, middle, and low terminal-
voltage bins and persists only the first estimate, a change of at least 25
mohm, or 128 additional accepted samples. The values remain diagnostic and do
not replace the production gauge's 190 mohm correction until several packs are
bench-qualified. Resistance learning is a Sisu enhancement, not active Nokia
NiMH behavior.

### 7. Harden Startup Without Hurting Standby

At power-on:

1. acquire five valid LTC samples within a bounded ten-attempt window;
2. average terminal voltage for the initial gate;
3. publish initial bars only after a qualified estimator seed;
4. enable the 3.8 V rail and wait for stable PG/capacitor charge;
5. start Telit and retain 250 ms voltage/current/PG monitoring through READY;
6. if a marginal 2.1-to-2.3 V pack collapses PG during Telit startup, show
   `Battery empty` and enter soft-off.

The current code implements item 6 as a neutral modem-supply event combined
with fresh terminal voltage below 2320 mV and charger absence. The verdict stays
latched through the empty shutdown so post-collapse voltage rebound cannot
cancel it.
The later fused endpoint policy adds one necessary distinction: during a call,
an anchored ledger that still shows positive charge classifies this combination
as load sag, not learned EMPTY. The absolute 1800 mV floor remains able to stop
unsafe hardware without writing a capacity endpoint.
Once any valid EMPTY path arms shutdown, a later unavailable LTC sample also
preserves the countdown; missing evidence is not treated as voltage recovery.
If PG made UART untrustworthy after Telit may be live, the battery shutdown
enters the existing graceful-control/GP39 ladder directly and still waits for
qualified-low PWRMON before releasing the rail.
The 2100 mV idle path has since passed a natural-pack EMPTY, shutdown, and
persistence cycle. The lower 1800 mV in-call boundary remains outside
controlled-source evidence.

During a call, warn and beep as the original does, but do not voluntarily end
the call merely because compensated SOC is low or a loaded PG event conflicts
with anchored remaining charge. If the pack reaches the absolute emergency
floor, shut down safely without treating that event as learned capacity.

None of these checks requires keeping the RP awake after the existing work is
finished. The LTC accumulator continues independently in dormant mode.

## Implemented Adaptation

### Load-Normalized Voltage Gauge

- Documentation and tests identify `0x27ce7c` as transient/drop state, not bar
  dwell.
- A pure estimator consumes replayable coherent observations.
- Current polarity and coherent sample/ACR-window behavior are explicit.
- The 65 mA bounded-resistance reference-load correction feeds a short robust
  filter and slow time-domain estimate.
- Nokia midpoint bars and the dual-domain warning classifier are reproduced.
- Startup uses bounded five-valid-sample qualification.
- Raw-collapse and PG safety paths remain independent.

The first sustained-call trace confirmed that this design prevents the visible
four-to-two-to-four load collapse. Percentage authority comes from the separate
capacity/SOC supervisor.

### Capacity And Pack Characterization

- Full/empty anchors and explicit confidence are implemented.
- ACR drives a persistent SOC ledger from qualified FULL/EMPTY
  anchors or a separately labeled five-sample provisional voltage bootstrap.
- A versioned, profile-tagged journal persists rare milestones and invalidates
  exact pack state on a new LTC hardware session.
- Qualified load steps feed median high/mid/low resistance bins; their use in
  production compensation remains deliberately disabled because multi-pack
  traces do not support replacing the ROM-grounded default.
- Net Monitor page 47 exposes SOC, SOH, confidence, resistance, and endpoint
  state. See [`battery_learning.md`](battery_learning.md).

## Verification Principles

Regression evidence spans quiet registered standby, UI and audio loads, Telit
startup and radio bursts, calls, charging and unplug, powered-off charging, and
natural discharge endpoints. Host replay and randomized tests keep observations
coherent across sample/session changes and invalid data.

The maintained invariants are:

- ordinary calls do not cause the former four-to-two-to-four bar collapse;
- load compensation cannot hide raw voltage or supply-PG collapse;
- the gauge adds no dormant wake source or measurable standby-current
  regression;
- charging UI, alarm/SMS wake, and call audio remain independent; and
- a new gauge session cannot reuse stale SOC as trusted pack evidence.

## Known Limits

- The installed pack supports the 190 mohm model functionally, but the
  high/mid/low resistance distribution across representative packs is unknown.
  Learned resistance therefore remains diagnostic.
- The 2100 mV idle endpoint is hardware-qualified. The 1800 mV in-call
  emergency floor and weakest-pack converter sequence have not been mapped with
  a controlled source across several packs.
- Charge efficiency and long-storage self-discharge are not inferred by this
  voltage gauge. The separate charge supervisor's 1.224 factor is scoped to its
  own Rev B2/pack evidence, and the LTC deadband omits sub-2 mA load.
- Rev B2 has neither independent pack identity nor a firmware-readable pack
  temperature. LTC die temperature remains board telemetry only.
- Provisional voltage-bootstrap SOC is explicitly lower confidence;
  `ENFORCE_ANCHORED` does not depend on it.

## Decision Summary

The Nokia design is adapted rather than copied mechanically. Rev B2 retains the
reference-load voltage domain, separate fast and slow evidence, robust burst
rejection, independent warning/safety classification, and charge-throughput
restraint against rebound, while replacing Nokia's software power ledger with
coherent LTC current and ACR evidence.

The shipped design combines a trace-replayable 65 mA estimator with a persistent
capacity/SOC supervisor. Qualified capacity owns bars, but it remains a
prediction at exhaustion; fused physical evidence owns EMPTY. Resistance
learning cannot alter production compensation without a separately justified
calibration change.

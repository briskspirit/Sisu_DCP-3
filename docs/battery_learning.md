# Battery Health And SOC Supervisor

Status: implemented and host-tested for the Rev B2 two-cell NiMH profile. On
the installed pack, qualified discharge history, SOC bars, natural EMPTY, and
durable endpoint recovery have been exercised on hardware. An estimate reaching
zero is not allowed to declare EMPTY by itself. Resistance bins still need
multi-pack evidence before they can influence the voltage compensation model.

The supervisor works beside the Nokia-derived voltage gauge in
[`battery_gauge_design.md`](battery_gauge_design.md). Its authority is narrow
and explicit:

- observed or learned, non-conflicted capacity plus valid SOC owns the four
  battery bars;
- a qualified FULL anchor plus coherent discharge measures delivered capacity
  until a separate physical EMPTY endpoint closes the cycle;
- the fused endpoint policy combines the Nokia voltage classifier, distinct-
  sample operating floor, modem-supply collapse, call state, and anchored
  remaining charge;
- in-call load sag with anchored charge left is LOW, not EMPTY; and
- power-on admission and sleep/wake scheduling remain outside this service.

Capacity evidence can therefore prevent a call-induced sag from corrupting a
capacity sample, while the 1800 mV in-call emergency floor can still protect
the hardware without being misreported to the learner as natural EMPTY.

## Ownership

- `battery_learning_logic`: pure profile, state machine, qualification,
  capacity/resistance estimates, and confidence projection.
- `board_diag_service`: translates the already-cached LTC2959 and charger state
  into one neutral observation. Reading it starts no conversion.
- `battery_learning_service`: owns live state, loads the journal, consumes app
  endpoint events, publishes authoritative SOC bars plus capacity prediction
  and overrun diagnostics, and retries a pending store handoff.
- `app_status_runtime`: supplies confirmed charger completion, fuses physical
  endpoint/load evidence with anchored remaining charge, and owns the existing
  notification and shutdown sequence.
- `store_battery_learning`: fixed versioned wire codec and independently
  replaced persistence record.
- Net Monitor page 47: read-only evidence.

The sidecar is called from the existing battery poll. It adds no timer and no
wake deadline; P1.7 residency and LTC conversion cadence are unchanged.

## NiMH Profile

The active profile is identified persistently as `NIMH_2S`, profile 2. Profile
1 used the direct LTC2959 data-sheet ACR scale and is rejected because its
capacity and anchor values are unit-bearing. Any chemistry or ACR-scale profile
must have a different ID, its own constants, and an explicitly selected runtime
path. A record from another profile fails validation and resets to the selected
profile's product prior instead of crossing evidence boundaries. See
[`ltc2959_acr_calibration.md`](ltc2959_acr_calibration.md).

Current Rev B2 constants are:

| Parameter | Value |
|---|---:|
| Product prior | 1225 mAh |
| Accepted learned capacity | 300 to 2000 mAh |
| Capacity temperature window | 10 to 40 C |
| Resistance temperature window | 10 to 35 C |
| Minimum resistance load step | 200 mA |
| Accepted effective resistance | 50 to 1000 mohm |
| Recharge invalidation | more than 5 mAh above the full anchor |
| Learned-cycle consistency | spread no greater than 20% of nominal |

The 1225 mAh prior is the midpoint of the current 1200-to-1250 mAh product
packs. The qualification limits are conservative bench policy, not recovered
Nokia values. Nokia v6.00 provides the load-normalized voltage model; persistent
capacity learning is a Sisu extension.

## Capacity Cycle

A capacity cycle begins only when the charge supervisor publishes a qualified
FULL event. That event may follow a corroborated BQ terminal or a software
coulomb stop whose durable inhibit and disabled `/CE` readback are confirmed.
Legacy observe-only BQ completion may retain the Nokia-compatible presentation,
but it cannot anchor the learner. The model also requires:

- physical charger presence;
- a valid LTC sample;
- the bench-qualified ACR/current polarity;
- uninterrupted ACR continuity; and
- an authoritative observation, not a Net Monitor/CDC force override.

The journaled anchor contains the raw ACR value and signed session-relative
charge. Temperature at the anchor seeds the cycle range. The cycle remains
qualified only while every later valid observation stays in the 10-to-40 C
window.

Any resumed charging invalidates the cycle. The same happens if ACR rises more
than 5 mAh above the full anchor, even if charger-state evidence missed the
charge interval. This intentionally rejects partial top-ups rather than trying
to reconstruct them.

The endpoint is the first physically supported EMPTY action that arms the
product's existing two-second shutdown countdown. It may originate from the
Nokia warning classifier, the two-sample Rev B2 idle operating floor, or a
marginal-pack modem-supply collapse. Coherent discharge through the current
capacity estimate is explicitly not an endpoint: it publishes an unconfirmed
prediction and keeps the FULL anchor live so a larger actual capacity remains
measurable.

Physical charger presence is not recovery and does not veto that endpoint. A
phone left attached after charge termination may still be discharging because
`/CE` is inhibited, BQ is idle/faulted, or system load exceeds delivered charge.
LOW/EMPTY suppression and cancellation require coherent positive current into
the pack together with BQ ACTIVE and enabled `/CE`. Consequently a physically
attached but noncharging pack can still close and persist a natural EMPTY cycle.

For current 2S NiMH hardware the idle floor is 2100 mV, while a live call uses
the lower 1800 mV emergency floor. Anchored positive remaining charge suppresses
in-call gauge EMPTY and marginal modem-supply collapse as load sag. The stale
gauge state remains suppressed after the call until it recovers. If the pack
nevertheless reaches 1800 mV during the call, firmware may shut down for hardware
safety, but it does not send that emergency action to the learner as EMPTY. An
idle physical endpoint may occur earlier than the estimate and therefore teach
lower usable capacity as the pack ages. Learned capacity is consequently
*usable delivered capacity in this phone*, not laboratory chemical capacity.

The EMPTY update is handed to the learner and an immediate journal flush is
attempted before the empty alert is posted. Every poll during the shutdown
countdown retries while data remains dirty, and soft-off retries again before
dormant entry. The earlier operating floor is what provides voltage margin for
those commit attempts.

The same accepted endpoint journals a `natural empty` marker independently of
whether a full-to-empty capacity sample was available. The marker contains no
ACR or inferred SOC: it says only that this pack generation reached the
firmware's real EMPTY action. It survives soft-off and RP reset, is offered to
the charge supervisor at the next admission, and is consumed on the first
authoritative positive-current sample. Merely attaching VIN does not consume it.
A gauge-session change clears it with the rest of the per-pack evidence.

For a qualified cycle:

```text
discharged_nAh = full_anchor_nAh - empty_sample_nAh
capacity_mAh   = round(discharged_nAh / 1,000,000)
```

Only 300-to-2000 mAh is accepted. Every EMPTY event consumes the anchor. A
failed cycle increments the rejected count but cannot replace accepted history.

## Capacity And Confidence

The last three accepted capacities form a ring:

- one sample: that sample;
- two samples: rounded mean;
- three samples: median.

Confidence is explicit:

| State | Meaning |
|---|---|
| `PRIOR` | no qualified completed cycle; 1225 mAh product prior only |
| `OBS` | one or two accepted cycles |
| `LEARN` | three accepted cycles with spread at most 245 mAh |
| `CONFL` | three accepted cycles whose spread exceeds 245 mAh |

The sidecar reports learned capacity even in `CONFL`, so the disagreement is
visible. SOC projection trusts learned capacity only in `OBS` or `LEARN`;
`PRIOR` and `CONFL` use the 1225 mAh nominal prior.

## Persistent SOC Ledger

SOC has separate provenance and confidence. A numeric value without those tags
must never authorize a charge decision.

| Provenance | Meaning |
|---|---|
| `UNKNOWN` | no defensible starting point |
| `BOOTSTRAP_VOLTAGE` | coarse initial estimate from five reference-load samples |
| `TRACKED` | coulomb projection has moved away from its stored origin |
| `ANCHORED_FULL` | a qualified charger completion established 100 percent |
| `ANCHORED_EMPTY` | the product EMPTY action established zero percent |

`BOOTSTRAP_VOLTAGE` is `PROVISIONAL`; both endpoint anchors are `ANCHORED`.
Tracking preserves the confidence of the origin. The bootstrap requires five
distinct, authoritative, noncharging samples with valid current, continuity,
and 65 mA reference-load voltage. Their average selects the conservative lower
bucket:

| Reference voltage | Initial SOC |
|---|---:|
| below 1900 mV | 0% |
| 1900 to 2424 mV | 25% |
| 2425 to 2529 mV | 50% |
| 2530 to 2574 mV | 75% |
| at least 2575 mV | 100% |

This is not voltage termination. It is a bounded starting estimate for an
unknown partially charged pack. If a charger is already attached before five
clean samples exist, SOC remains unknown and the BQ25171 remains the fallback.

The ledger projects from a persisted ACR-relative origin:

```text
discharge: remaining += delta_nAh
charge:    remaining += positive_delta_nAh * 1000 / effective_charge_factor
SOC:       remaining / frozen_capacity
```

The effective charge factor comes from the charge supervisor's persisted
configuration, or from the active hardware profile's 1.224 prior when no
configuration exists. The learner stores the factor with each charge-segment
origin. A detached configuration change therefore takes effect at the next
direction transition without reinterpreting coulombs accumulated under the old
factor.

Remaining charge is clamped to zero through capacity. Once it reaches zero on a
qualified discharge, `capacity_prediction_exhausted` becomes true and
`capacity_overrun_nah` continues increasing from the live FULL anchor. Neither
condition alone consumes the anchor, writes a capacity sample, or requests
shutdown. If voltage remains healthy, diagnostics additionally expose a
capacity/voltage disagreement and the phone continues measuring overrun. In
idle operation, a separately debounced gauge LOW corroborates exhausted
capacity and closes the natural endpoint before the loaded rail reaches the
hard floor. During a call that fused path waits for load release; trusted
remaining charge still suppresses sag-derived EMPTY. The resulting endpoint
records the complete delivered charge, allowing later cycles to raise the
estimate; a physical endpoint before zero records a smaller sample and lets
repeated aging evidence lower it.

A transition between
charge and discharge closes the old segment and persists a new origin, so an RP
reset does not reinterpret old coulombs using the other direction's efficiency.
The transition boundary is quantized to one LTC sample; no finer split is
observable. A lost current-polarity or continuity contract invalidates the SOC
ledger instead of bridging the gap.

A qualified FULL event reanchors at capacity and EMPTY reanchors at zero. When
capacity confidence is `OBS` or `LEARN` and SOC remains valid, the ledger maps
remaining exact nAh into four ceiling-rounded quartiles and those bars replace
the voltage ladder. At predicted zero the bars conservatively remain at zero,
but endpoint and learning authority still require physical evidence. If charging
starts while that prediction is unconfirmed, charge admission marks the frozen
capacity conflicted and the zero remaining value unknown; the estimate-derived
target cannot stop the charger, so BQ completion and the independent safety
ceiling retain authority. The Nokia voltage ladder remains the fallback whenever
capacity or SOC is unavailable. A long cycle can also under-report chemical
capacity because internal NiMH self-discharge does not cross the LTC shunt; that
is one reason learned capacity is labeled delivered capacity.

## Effective Resistance

Resistance learning observes naturally occurring load increases while the RP
is already awake. A pair is eligible only when:

- both samples are authoritative and have valid voltage/current continuity;
- the charger is absent and inactive;
- temperature is 10 to 35 C;
- samples are 100 to 1500 ms apart;
- discharge load increases by at least 200 mA;
- terminal voltage falls by 5 to 300 mV; and
- the derived value is 50 to 1000 mohm.

```text
R_mohm = 1000 * voltage_drop_mV / load_increase_mA
```

Nine accepted samples produce one median estimate. Samples are grouped by the
midpoint terminal voltage of the pair:

| Bin | Midpoint terminal voltage |
|---|---:|
| High | at least 2530 mV |
| Mid | 2320 to 2529 mV |
| Low | below 2320 mV |

This is effective pack-plus-contact resistance, which is the useful quantity
for this phone's load behavior. It is not yet fed back into the production
gauge's ROM-grounded 190 mohm correction. Multi-pack traces must first show that
the bins are stable and beneficial.

## Battery Session And Replacement

An ordinary RP reset preserves ACR continuity and therefore a journaled full
anchor. LTC UVLO/POR or an explicit hardware gauge-session change means the
pack may have been replaced. On the first such observation the model:

- discards capacity history, SOC anchor, and resistance estimates;
- increments `pack_generation`;
- retains only the selected profile and its nominal product prior; and
- journals the reset.

Rev B2 has no independent battery identity. A replacement that somehow avoids
LTC reset evidence cannot be distinguished in software; the under-battery pack
arrangement makes that unlikely. Stronger identity or removal evidence would
require different hardware.

The public learner snapshot carries both `gauge_session_bound` and the bound
`gauge_session`. Before the learner's first observation, persisted evidence is
considered current only when the HAL still reports session zero, matching the
LTC same-powered-gauge convention. A nonzero or changed session must first pass
through the learner's pack-reset path. This prevents the charge supervisor,
which deliberately polls first to freeze pre-charge SOC, from admitting a new
pack with the previous pack's cached SOC or EMPTY marker.

## Development Provisioning

The CDC development console can carry measured health evidence across an
intentional battery removal or gauge reset, for example while changing charger
resistors during board development:

```text
battlearn
battlearn provision <capacity_mAh> [<high_mOhm> <mid_mOhm> <low_mOhm>] [empty] confirm
battlearn recover-cycle <capacity_mAh> <expected_ok_count> confirm
battlearn recover-full <acr_raw> <session_delta_nAh> <expected_ok_count> confirm
```

`battlearn` prints the current model, journal state, and a copyable restore
command whenever an observed capacity exists. A zero resistance value means
unknown. Provisioning replaces the capacity history with one observed sample
and optionally restores the three resistance estimates; it does not claim
three independently learned cycles. The optional `empty` qualifier restores a
separately observed natural-EMPTY endpoint, for example after a development
flash or hardware rework. It is an explicit assertion, never inferred from a
single NiMH voltage reading.

The command never imports ACR, a numeric remaining-charge estimate, or a full
anchor. Those are tied to one LTC session and would become unsafe after battery
removal. It is refused while the charger is connected/active, while a
full-to-empty cycle is
anchored, when current gauge evidence is invalid, discontinuous, or not
authoritative, or when the store cannot accept the record. It also preserves
the gauge-session identity seen at command time, so the next ordinary poll
cannot immediately erase a valid restore.

The normal store engine commits the record with its usual audio-aware pacing;
the console does not invoke the power-off-only forced flush. Do not remove the
battery until a later `battlearn` status reports `store dirty=0` and
`degraded=0`. The command is absent from `RELEASE=ON` builds with the rest of
the USB service console.

`recover-cycle` is the narrower postmortem path for a full-to-empty experiment
whose endpoint was captured externally but whose EMPTY event was lost before
the learner handled it. It appends one capacity to the existing three-sample
ring; it does not replace the earlier samples, resistance bins, accepted or
rejected counts, pack generation, full anchor, or endpoint state. If a current
SOC anchor exists, the ledger is rebased to the updated capacity estimate while
preserving the same physical endpoint or remaining-charge fraction.

The expected accepted-cycle count is an optimistic concurrency guard. The
command succeeds only when it still equals the live `ok=` value, then increments
that value once. Repeating the same command after a lost console response is
therefore refused instead of duplicating the sample. Recovery also requires a
detached charger, coherent current gauge evidence, a ready store, and an
explicit `confirm`; it is intended to be run only after the active charge has
finished and the charger has been removed.

`recover-full` restores a missed FULL endpoint from a captured raw ACR and
session delta without inventing capacity or incrementing cycle history. It is
accepted only while detached on the same live gauge session, with authoritative
nonpositive-current evidence, observed non-conflicted capacity, the expected
accepted-cycle count, and an exact 3825 nAh-per-raw-count match between the
captured endpoint and the current sample. It anchors 100 percent at the captured
point and projects the present remaining charge forward from that historical
anchor. Repeating the identical recovery is idempotent; a different endpoint is
refused.

## Persistence

`STORE_UNIT_BATTERY_LEARNING` remains unit 14 and the charge supervisor unit 15.
Both are opaque records on littlefs, with unchanged payload schemas. The old
two-slot units are read once during migration and then left untouched. See
[Storage engine](storage_engine.md) for the layout and migration authority.

The current `BTL2` payload is exactly 96 bytes, little-endian, and contains
magic, version, profile ID, flags, capacity history/counters, three resistance
bins/counters, pack generation, full-anchor evidence, the natural-EMPTY marker,
and the SOC origin/capacity/factor/provenance/confidence fields. The decoder
still accepts the deployed 64-byte `BTL1` record. A qualified BTL1 full anchor
migrates exactly; no provisional SOC is invented during migration. The generic
record layer adds ID, version, length, and CRC, syncs a replacement file and
atomically renames it over the previous record.

Writes are requested only for rare semantic changes:

- a confirmed or explicitly recovered full anchor;
- the first temperature disqualification;
- recharge invalidation;
- an accepted or rejected physically qualified empty endpoint;
- consumption of a natural-EMPTY marker by positive charge current;
- first five-sample SOC bootstrap, FULL/EMPTY SOC reanchor, direction change,
  or SOC invalidation;
- a new battery session; or
- the first resistance estimate, a change of at least 25 mohm, or 128 further
  accepted resistance samples.

No coulomb increment or ordinary sample dirties flash. Store commit pacing,
audio deferral, failure isolation, and power-off flush remain owned by the
existing storage engine.

## Net Monitor Page 47

Page 47 has five frames:

1. nominal, learned, and remaining mAh; SOC, authoritative bar value, and SOH;
2. confidence, anchor qualification, predicted exhaustion, voltage
   disagreement, capacity overrun, and accepted/rejected cycle counts;
3. SOC confidence, provenance, charge/discharge segment, and bootstrap voltage;
4. last capacity, three-cycle spread, pack generation, persistence pending and
   failure count;
5. high/mid/low resistance estimates and hexadecimal accepted-sample counts,
   plus the current voltage bin.

`CAP --`, `REM --`, `SOC --`, `H--`, `LAST --`, a `--` resistance, or `BIN -`
means evidence is not yet available; zero is never substituted for unknown.
`B-` means the health supervisor is not authoritative for bars and the app uses
the voltage fallback. `P1` means the projected capacity has been exhausted but
is still awaiting corroborating LOW/EMPTY evidence; `V1` means current voltage
still looks healthy, and `O` is the continuing overrun in mAh. `ANCH Q` means a
temperature-qualified full anchor, `ANCH X` an anchor already disqualified for
learning, and `ANCH -` no live anchor.

## Verification

Host tests cover:

- exact threshold inclusion/exclusion;
- full/recharge/empty and temperature invalidation;
- median history and confidence transitions;
- restart continuity and new-session reset;
- five-sample voltage bootstrap, exact nAh tracking, direction segmentation,
  endpoint reanchoring, and SOC invalidation;
- resistance median/outlier/bin behavior;
- debug-force isolation and sample/time wrap;
- 250,000 randomized public-API transitions under ASan/UBSan;
- fixed wire bytes, every truncated payload, semantic corruption, profile
  mismatch, atomic apply, dirty-unit ownership, and restart persistence;
- service write retry behavior; and
- exact five-frame Net Monitor output and worst-value line fit.

## Rev B2 Bench Evidence

- Recent accepted capacity samples were 1272, 1301, and 1239 mAh. Their median
  is 1272 mAh; the persisted history reported four accepted and two rejected
  cycles, so rejected or partial runs did not silently become authority.
- Qualified FULL anchors, exact remaining-charge tracking, SOC-owned bars, and
  restart continuity were exercised across long discharge and ordinary-use
  intervals.
- Natural idle EMPTY occurred on distinct 2074 and 2096 mV raw samples. The
  endpoint marker was stored before shutdown and loaded cleanly afterward.
- That particular endpoint began without a qualified FULL anchor and correctly
  rejected a capacity sample, while still validating endpoint, UI, shutdown,
  and persistence authority.
- Host coverage proves that healthy-voltage capacity overrun remains diagnostic
  and that anchored remaining charge suppresses a call-sag endpoint. The
  separate 1800 mV in-call emergency path never teaches capacity.

## Calibration Scope

Capacity continues adapting from naturally completed, qualified discharge
cycles, including an earlier physical EMPTY as a pack ages. Effective
resistance remains diagnostic and the production voltage gauge stays at
190 mohm until several packs show stable, beneficial high-, middle-, and
low-SOC bins.

No special user-performed calibration cycle is required for normal operation.
If a user never completes a clean full-to-empty cycle, the phone simply remains
on the honest 1225 mAh prior and its existing voltage-based UI.

# Battery Charge Supervisor Design

Status: persistent SOC admission, charge accounting, durable `/CE` enforcement,
and attached maintenance are implemented and host-tested. Explicitly
provisioned `ENFORCE_ANCHORED` has completed Rev B2 target-stop and maintenance
cycles. The source-tree default remains `OBSERVE`, and the 1.224 factor remains
specific to the installed hardware/pack evidence rather than a universal NiMH
constant.

## Decision

A chemistry-aware `battery_charge_supervisor` runs beside, not inside,
`battery_learning`.

The two services have deliberately separate authority:

- the battery-health supervisor owns capacity-backed bars and an unconfirmed
  exhaustion prediction, but neither physical EMPTY nor charger `/CE`;
- the supervisor observes charge sessions, classifies their termination,
  may inhibit BQ25171 `/CE` under an explicit policy, and publishes qualified
  FULL evidence;
- the app owns Nokia-compatible presentation only: charging animation,
  `Battery full`, and powered-off display behavior;
- `board_diag` remains a cache of physical evidence, not a charge-policy owner.

The implementation exposes every input and decision over CDC and Net Monitor.
It contains two opt-in enforcement policies, but neither can become active by
accident: policy and charge factor are journaled, the factor must be explicitly
marked trusted, and the default record is observe-only.

## Why A Separate Sidecar

Putting termination into `battery_learning` would create a circular authority
loop: a learned capacity would stop a charge, and the discharge resulting from
that stop would then be allowed to validate and alter the same estimate without
preserving which estimate made the decision. It would also violate the
learner's current invariant that its output cannot affect charging, bars,
warnings, admission, or shutdown.

The supervisor instead snapshots the *previously committed* capacity evidence
at charge admission. That immutable basis is attached to the charge generation
and used again when a later natural EMPTY endpoint grades the termination.
Capacity learned from that later discharge can influence only a subsequent
charge.

## Hardware Boundary

Rev B2 currently provides:

- BQ25171 constant-current NiMH charging;
- an 8-hour timer selected by 24 kOhm on `CHM_TMR`;
- about 273 mA total output selected by 1.10 kOhm on `ISET` on the
  bench-reworked Rev B2;
- BQ STAT1/STAT2 and `/CE` through TCA8418;
- physical charger detection on GP43;
- battery-terminal voltage, current, ACR, and board/LTC temperature from
  LTC2959;
- a populated 10 kOhm `TH1` board NTC near the battery contacts, connected
  directly to BQ `TS` but not exposed to the RP2354B.

The source-tree supervisor identity is hardware profile 3: approximately
273 mA total with the fitted 8-hour backup timer and LTC2959 ACR calibration
revision 2. Profile 2 represented this same fitted ISET/timer hardware under
the old data-sheet ACR conversion; its unit-bearing baselines and terminal
summaries must not cross into profile 3. The 1.10 kOhm and 24 kOhm parts were
already fitted before profile-3 evidence collection began, so correcting the
earlier 14-hour documentation does not create a new hardware identity. The
measured battery-terminal current is lower while the phone is awake because
the system load shares the BQ output. The present rework keeps the worst-case
BQ current near 300 mA including its tolerance, below the ACP-7's 320 mA
minimum specification, but still requires thermal qualification at both a
stiff 5 V input and an original/current-limited Nokia charger.

BQ25171 NiMH charging is constant-current and timer-terminated. Its thermal
regulation and voltage/fault protections remain valuable backstops, but it does
not infer pack state of charge. The system load shares the programmed output
current with the battery, which is why the LTC's battery-terminal coulomb count
is the authoritative delivered-charge measurement.

`TH1` gives the BQ an autonomous local battery-contact/PCB temperature guard,
but it is not embedded in the pack and firmware cannot read it. LTC die
temperature is a second, nearby board-temperature proxy. The current model
records it but does not enforce on it, and it must not be presented as exact
battery temperature or used as the primary full detector. Firmware-controlled
pack-temperature policy would require a dedicated measurement that does not
disturb the BQ `TS` network.

## Modules And Layering

### Pure Model

`include/services/battery_charge_supervisor_logic.h`

`src/services/battery_charge_supervisor_logic.c`

Owns the deterministic state machine, evidence qualification, ACR arithmetic,
minute-window curve features, candidate decisions, terminal classification,
and validation math. It includes no HAL, app, storage, Net Monitor, or debug
headers.

### Runtime Sidecar

`include/services/battery_charge_supervisor_service.h`

`src/services/battery_charge_supervisor_service.c`

Collects already-cached board evidence, snapshots the battery-health supervisor at charge
admission, calls the pure model once per existing battery poll, applies the
supervisor's charger-inhibit owner only under an explicitly provisioned policy,
publishes transition results, and owns persistence pacing. It creates no timer
and no wake deadline.

### Charger Control Arbiter

A neutral charger-control service replaces `board_diag`'s former single
`s_charger_enabled_target`. Owners request an inhibit; no caller directly
writes the production `/CE` target.

Initial owners:

- `CHARGER_INHIBIT_SUPERVISOR`;
- `CHARGER_INHIBIT_DEBUG`;
- `CHARGER_INHIBIT_FAULT`, reserved for an independent hardware-fault policy
  and not asserted by the current supervisor.

Effective charging is enabled only when no inhibit owner is present. Every
transition is read back from TCA8418 and exposes requested, actual, valid,
retry, and failure evidence. Debug override makes the supervisor session
non-authoritative.

`power_off()` must clear only the debug owner. It must not erase a supervisor
termination latch and restart charging while the charger remains attached.
Physical detach releases the supervisor latch. A qualified FULL latch also has
the narrowly defined attached-maintenance transition below; safety and fault
latches never use it.

### App Integration

`app_status_runtime` keeps the legacy BQ dwell as a characterized observation
and presentation compatibility path:

- `ui_charge_active` for the existing animation;
- in `OBSERVE`, stable legacy BQ completion may stop the animation and show the
  original `Battery full` note, but it cannot teach the learner;
- under an enforcing policy, unqualified BQ completion stops the animation but
  cannot show `Battery full`; the supervisor owns that decision;
- one-shot supervisor `full_qualified` may show the same note and is the only
  charge-side event that can create the learner's FULL anchor;
- terminal provenance for diagnostics only.

The app normally suppresses duplicate FULL notices until physical detach. A
successful attached-maintenance restart re-arms only that presentation latch,
so the next qualified generation can show its own `Battery full` record. The
restart event takes precedence over any stale same-poll completion evidence.

The existing stable-BQ-FULL filter remains as a characterized compatibility
path. It becomes a qualified FULL only when the frozen coulomb target was also
reached with an authoritative session. A software FULL is separate: it is
emitted only after the stop latch is durable and `/CE` request plus readback
both confirm disabled. A BQ fault after a corroborated full-capacity coulomb
target is also qualified as FULL; a BQ FULL/fault before its target and every
safety stop remain non-FULL.

## Input Observation

One neutral observation contains:

- monotonic time and LTC sample sequence;
- LTC gauge session, raw ACR, signed session delta, terminal voltage, current,
  and board temperature;
- sample, polarity, and continuity validity;
- physical charger presence and input voltage;
- decoded and raw BQ status pins;
- `/CE` requested state, readback state, and validity;
- selected battery chemistry;
- debug/simulation authority;
- the learner snapshot captured before that poll can invalidate its discharge
  anchor on seeing charge current; and
- whether that learner snapshot is explicitly bound to the same LTC gauge
  session. A nonzero session that the learner has not processed is stale pack
  evidence and cannot authorize admission.

The service uses the existing one-second awake battery poll. Sixty-second
aggregates provide curve data. It does not increase dormant cadence; an active
charger already keeps the powered-off charging UI awake.

The supervisor intentionally runs before the learner on each app poll so it
can freeze the last pre-charge deficit. Session identity makes that ordering
safe on the first poll after boot or battery replacement: the supervisor may
observe physical attach, but admission waits until the learner has consumed the
new gauge session and published current-pack evidence.

## State Machine

1. `DETACHED`: no physical charger. No charge session or supervisor inhibit.
2. `QUALIFYING`: charger appeared; wait for coherent LTC, `/CE`, and BQ
   evidence. Bounce and debug-forced evidence cannot start an authoritative
   generation.
3. `CHARGING`: BQ was observed ACTIVE with positive net battery current. The
   generation owns immutable admission evidence and accumulates charge.
4. `STOPPING`: a software decision is frozen while its stop latch is journaled,
   committed, applied through the charger arbiter, and read back.
5. `COMPLETE_BQ`: stable BQ completion after a real ACTIVE phase.
6. `COMPLETE_SOFTWARE`: an enforced software decision has raised `/CE` and its
   readback is confirmed. The inhibit remains latched until detach or one
   qualified attached-maintenance rearm.
7. `STOPPED_SAFETY`: a hard software ceiling stopped charging. This is not
   automatically called FULL.
8. `FAULT`: BQ fault, control/readback failure, invalid configuration, or other
   terminal error.
9. `DEGRADED`: charging may continue under BQ protection, but lost gauge
   continuity or insufficient evidence forbids an intelligent FULL decision.

A new physical attach creates a new monotonically increasing charge generation.
Input bounce within one attach does not. A changed LTC gauge session invalidates
all ACR-derived evidence rather than silently rebasing it.

## Terminal Provenance

Never collapse all high/high STAT observations into `Battery full`. Persist and
publish an explicit reason:

- `BQ_COMPLETE_AFTER_ACTIVE`;
- `BQ_ALREADY_FULL_AT_ATTACH`;
- `SOFTWARE_COULOMB_FULL`;
- `SOFTWARE_CURVE_FULL` (reserved and not emitted);
- `SOFTWARE_COULOMB_AND_CURVE_FULL`;
- `SAFETY_CHARGE_LIMIT`;
- `SAFETY_TIME_LIMIT`;
- `BOARD_TEMPERATURE_LIMIT`;
- `BQ_FAULT`;
- `BQ_FAULT_AFTER_COULOMB_FULL`;
- `CONTROL_READBACK_FAILED`;
- `GAUGE_CONTINUITY_LOST`;
- `MANUAL_DEBUG_STOP`;
- `DETACHED`.

`BQ_COMPLETE_AFTER_ACTIVE`, the implemented software FULL reasons, and
`BQ_FAULT_AFTER_COULOMB_FULL` are candidates for a qualified FULL anchor, but
all still require their independent evidence gates. A manual `/CE` disable,
physical detach, safety stop, or uncorroborated fault must never seed capacity
learning. The legacy presentation path may still show `Battery full` for a
stable BQ completion without granting it learner authority.

The STAT interface cannot distinguish every internal BQ reason. In particular,
the firmware must call a transition after a genuine ACTIVE phase simply
`BQ_COMPLETE_AFTER_ACTIVE`, not claim that it proved timer expiration.

## Charge Accounting

At admission capture:

- start ACR/session delta;
- capacity estimate and confidence available *before* this charge;
- exact remaining nAh and its SOC provenance/confidence, when available;
- whether the immediately preceding accepted endpoint was natural EMPTY;
- effective-resistance estimate and confidence;
- initial voltage/current/temperature;
- charge-current hardware profile ID.

During charging:

```text
net_charge_in = current_session_delta - start_session_delta
```

This automatically subtracts phone consumption and negative current bursts.
The model also tracks time-integrated positive and negative samples as
diagnostic cross-checks, but ACR remains authoritative.
The active ACR conversion and its evidence-domain migration are specified in
[`ltc2959_acr_calibration.md`](ltc2959_acr_calibration.md).

Missing capacity is known when the learner has a valid persistent SOC ledger at
attach. That origin may be an anchored FULL/EMPTY endpoint or the deliberately
coarse five-sample voltage bootstrap described in
[`battery_learning.md`](battery_learning.md). The capacity side of the equation
uses an accepted learned value when available and otherwise the 1225 mAh
product prior; a conflicted learned history cannot authorize early stopping.

The learner journals that natural-EMPTY marker independently of its full
anchor, because consuming the full-to-empty anchor must not erase the evidence
needed by the following charge. The supervisor reads the marker before the
learner consumes it on the first authoritative positive-current sample.

When starting SOC is unknown but observed or learned non-conflicted capacity is
available, the supervisor freezes one complete capacity as the replacement
deficit. This target cannot underfill an unknown partially charged pack and
prevents the BQ timer from accepting substantially more than one known pack
capacity. A product prior alone cannot request a software FULL stop. Reaching
its complete-capacity target may corroborate an independent stable BQ terminal,
which supports first-pack bootstrap without turning the prior into a standalone
completion oracle.
The voltage bootstrap never pretends to be precise: it is tagged `PROVISIONAL`,
and only the `ENFORCE_BOOTSTRAP` policy may consume its smaller exact deficit.

For a known deficit:

```text
target_input = missing_deliverable_capacity * charge_factor
```

`charge_factor` is input coulombs divided by later deliverable coulombs. The
profile-3 source prior is 1.224. It was first derived when 1520.5 mAh input
divided by the provisioned 1242 mAh capacity coincided with the first
compensated-voltage plateau and 5 mV drop; that single trace was only a
candidate, not a completed-cycle efficiency measurement. Later Rev B2
target-stop and discharge evidence supports explicitly provisioned use on the
installed hardware and pack, but does not make the value universal. Firmware
therefore keeps the source default untrusted and observe-only. A CDC
configuration command must explicitly promote a reviewed value to `trusted`
before it can authorize an early stop.

The supervisor's effective configured factor is also passed into the learner's
charge-side SOC projection. This is one physical calibration, not two knobs:
changing it while detached affects both the next target calculation and the
next charge segment, while an already-journaled segment retains its historical
factor.

Production enforcement uses a reviewed battery-family factor scoped to one
chemistry and hardware profile. A user does not need to perform a controlled
cycle before safe charging works. The firmware does not auto-learn a per-pack
factor; changes require bounded multi-cycle evidence and explicit detached
configuration rather than promotion of one cycle.

## Voltage-Curve Evidence

For charging current `I` and learned effective resistance `R`, the diagnostic
electrochemical estimate is:

```text
V_compensated = V_terminal - I_charge * R
```

The model keeps bounded one-minute medians, a rolling peak, adjacent-minute
slope, and drop from peak. A gap over ten seconds restarts the current minute;
a new charge generation or rejected restore resets the curve state. The
hardware-current profile is immutable within a generation.

Panasonic's general rapid-charge guidance cites approximately 5-10 mV/cell
negative-delta-V and 1-2 degrees C/min dT/dt, while explicitly requiring
pack-specific matching. The programmed approximately 0.22 C total rate is below
their 0.5-1 C rapid-charge range, and we lack pack temperature. Therefore:

- voltage peak/drop is recorded from the first build;
- it may corroborate a coulomb decision after bench calibration;
- it is not an enforcing standalone detector in version 1;
- board-temperature rate is diagnostic or a conservative safety veto only.

No final delta-V, plateau, or temperature threshold is hard-coded before traces
from the actual cells, case, current, and charger source exist.

## Enforcement Rules

### Policies

- `OBSERVE` computes candidates and blockers but never requests the supervisor
  inhibit. This is the persistent factory default.
- `ENFORCE_ANCHORED` accepts only SOC descended from a qualified FULL or EMPTY
  endpoint.
- `ENFORCE_BOOTSTRAP` additionally accepts the five-sample provisional voltage
  bootstrap. It is useful for an unknown partially charged first-use pack, but
  deliberately carries more uncertainty.

Software may declare FULL only when all currently implemented gates hold:

1. an enforcement policy is selected and the session was admitted after real
   BQ ACTIVE current;
2. chemistry and hardware-current profile match;
3. an exact SOC deficit or the observed/learned full-capacity replacement target
   was frozen before charging advanced the learner;
4. LTC sample, current polarity, ACR continuity, and gauge session remain valid;
5. no debug force, foreign inhibit owner, or manual charger control contaminated
   the generation;
6. capacity is observed/learned and not conflicted; SOC confidence satisfies the
   selected policy only when the target is smaller than full capacity;
7. the configured 1000-to-2000 permille charge factor is explicitly trusted;
8. net battery-terminal input reaches the frozen exact-nAh target; and
9. the stop latch reaches flash before `/CE` is disabled, after which request
   and readback both confirm the disabled state.

Failure of an evidence gate means continue under the BQ timer, not guess FULL.

### Independent Ceilings

The implemented independent ceiling is battery-terminal net input equal to
1.75 times the frozen capacity. It is available even when SOC is unknown, but
only in an explicitly selected enforcement policy. Crossing it requests a
safety inhibit and never claims FULL or anchors the learner. The BQ's 8-hour
timer, autonomous TS input, and voltage/fault handling remain hardware
backstops. No software elapsed-time, pack-temperature, or voltage ceiling is
implemented; those enum values reserve provenance, not behavior.

After a safety stop, `/CE` remains inhibited until physical charger detach. A
qualified FULL stop may use the attached-maintenance policy below. `/CE` is not
pulsed for rest measurements; its timer-reset effect is used only for an actual
maintenance-charge restart.

### Attached Maintenance Charge

After a qualified BQ or software FULL, a phone left indefinitely on external
power is allowed to operate from the battery while the supervisor keeps `/CE`
inhibited. Physical VIN is not charging evidence and does not suppress LOW or
EMPTY. The ordinary battery-health SOC ledger, bars, voltage policy, and
endpoint detection continue to run.

One maintenance restart becomes eligible only when all of these are true at the
same poll:

1. the durable terminal record still proves a qualified FULL;
2. policy is `ENFORCE_ANCHORED` and the configured charge factor is trusted;
3. capacity is observed or learned, non-conflicted, bound to the current gauge
   session, and remaining SOC is anchored;
4. exact remaining capacity is at or below 80 percent of capacity;
5. the filtered Nokia 65 mA reference voltage is strictly below 2600 mV;
6. physical charger presence, valid charger control, matching chemistry/profile,
   a clean supervisor journal, and absence of foreign inhibit owners all hold.

The SOC condition may become true first while voltage remains above 2600 mV; the
supervisor keeps waiting. Falling farther below 80 percent does not disarm it.
The raw terminal sample and modem-load sag cannot satisfy the voltage gate.

The restart is a crash-safe four-step transaction: journal a
`MAINTENANCE_REARM` inhibit intent, confirm `/CE` disabled, hold it disabled for
at least one second, then queue removal of the inhibit and re-enable `/CE`
immediately. Stop/inhibit is durability-first; release is deliberately
fail-open so a flash fault cannot strand a connected pack. The cleared record
continues retrying to durability before model re-admission. Readback
confirmation then starts a fresh charge generation whose exact target is the
now-current SOC deficit. The prior qualified FULL authorizes one attempt only.
If BQ remains FULL, refuses to charge, or reports a fault after the pulse, the
supervisor records that generation without entering an endless `/CE` retry
loop. A later qualified FULL is required to earn another maintenance restart.

### Unqualified BQ Completion Recovery

A stable BQ completion that fails coulomb qualification is not accepted as
FULL. When exact anchored SOC is nevertheless at or below 80 percent, the
enforcing supervisor may use the same durable one-second `/CE` transaction once
to recover a BQ that stopped early or remained FULL across attach. This path
requires trusted charge-factor configuration, current gauge continuity,
healthy `/CE` request/readback, matching chemistry/profile, physical charger
presence, and no inhibit owner. Unlike ordinary qualified-FULL maintenance, it
does not wait for the 2600 mV threshold: that voltage gate prevents needless
maintenance recharge after a real FULL, while this path is correcting a proven
unqualified completion.

Every automatic `/CE` transaction sets a durable completion-rearm guard. A
second unqualified completion cannot pulse again while that guard is set. Real
charger detach or a later independently qualified FULL clears the guard; the
latter allows subsequent 80-percent/2600 mV maintenance cycles during an
indefinitely attached deployment.

Independently, app-side LOW or EMPTY evidence releases the supervisor's own
inhibit whenever a charger is physically attached but positive recovery is not
actually observed. This floor escape applies to FULL, safety, and stale restored
latches; it does not clear a foreign/debug inhibit and does not suppress the
LOW/EMPTY UX merely because VIN exists.

## Factor Validation Boundary

The current wire format intentionally stores neither validation tickets nor a
history of charge-factor samples. It freezes the admitted learner snapshot so a
capacity result from the following discharge cannot feed back into the charge
generation that produced it. Charge input and the later qualified delivered
capacity can be compared externally for reviewed calibration, but firmware does
not promote or demote the factor automatically.

A factor is changed only through explicit detached configuration after that
evidence is reviewed. A known-empty-to-full input ratio is relevant, while
successful discharge capacity alone cannot prove the pack was not overcharged.

## Persistence

The dedicated `STORE_UNIT_BATTERY_CHARGE_SUPERVISOR` retains unit ID 15 and is
an independently replaced littlefs record. The old journal is read only for
the one-time import described in [Storage engine](storage_engine.md).
The payload is versioned and explicitly encoded; no C struct is an on-flash ABI.

The supervisor persists only transitions and configuration:

- authoritative session admission, including ACR baseline;
- exact frozen remaining/deficit/target and their confidence tags;
- configured policy, factor, and explicit factor-trust bit;
- stop reason, durable inhibit latch, maintenance-rearm intent, automatic-rearm
  loop guard, and whether the latest terminal record is qualified FULL;
- terminal reason and one latest terminal summary; and
- pack/gauge-session invalidation.

`CGS4` is a fixed 128-byte little-endian payload. The decoder accepts deployed
128-byte `CGS3` and `CGS2` records plus the 96-byte `CGS1` observe-only record.
A pending CGS3 maintenance transaction migrates with its one-shot loop guard
consumed. A CGS2 record recovers one-shot maintenance authority only when its
software/coulomb terminal reason intrinsically proves qualified FULL. Legacy
`BQ_COMPLETE_AFTER_ACTIVE` alone cannot reconstruct the newer coulomb
corroboration and therefore migrates unqualified; maintenance-in-progress
defaults false.
CGS1 remains unqualified, and representable whole-mAh fields migrate without
inventing enforcement trust. The wire format intentionally omits validation
tickets and charge-factor history. It never persists each sample, minute
window, or coulomb increment; the RAM trace is diagnostic only.

An RP reset while the charger remains attached restores an active session only
when physical attach, pack generation, LTC gauge session, and ACR continuity all
match. If evidence does not match, fail degraded and rely on BQ rather than
reusing stale charge accounting. A durable software-stop or maintenance latch
is inspected after store initialization before the charger arbiter permits
`/CE`. The TCA HAL itself starts fail-disabled and retains the arbiter target
across expander recovery, so an RP reset or TCA-only reset cannot briefly
restart a stopped charge. Inhibit requests remain durability-first. Detach,
maintenance, and battery-floor releases take effect immediately in hardware
while their cleared records retry to durability.

The record deliberately does not write once per minute. After an RP
reset, the ACR baseline and immutable admission evidence survive, while elapsed
time, integration cross-checks, and voltage-curve windows restart at zero. That
is sufficient for exact coulomb and input-ceiling decisions because both use
the persistent ACR baseline. No elapsed-time enforcement exists, so reset-time
loss of elapsed milliseconds cannot weaken an implemented safety deadline.

### Rev B2 Reset-Continuity Bench Evidence

An admitted charge survived a deliberate BOOTSEL round trip while the charger
remained attached. Charge, pack, and gauge generations, the start ACR, session
baseline, and immutable target survived unchanged; net input resumed from the
original baseline while elapsed time and diagnostic curve windows restarted.

That run used profile 2 and the old data-sheet ACR scale, so it qualifies the
restore mechanism only. Profile 3 rejects those unit-bearing calibration values
rather than silently relabeling them.

## CDC Contract

The CDC build provides:

```text
charge
charge trace [off|on]
charge csv
charge history
charge configure <observe|anchored|bootstrap> <1000-2000> <candidate|trusted> confirm
```

`charge` prints a bounded current snapshot including:

- exact configured policy, phase, charge/pack/gauge generations;
- `/CE` request/readback and inhibit owners;
- elapsed time, ACR baseline, net input, positive/negative cross-checks;
- exact-nAh remaining, deficit, target, safety ceiling, SOC provenance and
  confidence, factor, and factor trust;
- raw/compensated voltage, rolling peak, drop, slope, and curve validity;
- current, temperature/rate, hardware-current profile;
- current candidate, blocker bitmask, terminal reason, persistence health;
- active persisted admission and the latest terminal summary through
  `charge history`.

`charge trace on` emits one concise CSV row per completed minute, never per
one-second sample. `charge csv` dumps a bounded RAM ring without enabling live
spam. `charge history` dumps the active persisted admission (when present) and
the most recent terminal summary.

`charge configure` is refused while a charger, active/restoring session,
durable stop latch, pending latch release, or supervisor inhibit is present.
The final `confirm` token is mandatory. `candidate` records a value for
observation but leaves the factor blocker set; only `trusted` can authorize an
early stop. A successful command updates the store's journaled RAM owner; wait
for the reported dirty bit to clear before removing power. No factor-history
journal exists.
The ordinary `hw` command gains one compact supervisor summary line.

Other commands remain read-only observability. Raw charger debug control marks
the generation non-authoritative and cannot be mistaken for supervisor FULL.

## Net Monitor

Power page 48 has five read-only frames:

1. policy, phase, generation, elapsed time, terminal reason, candidate, and
   shadow counters;
2. net input, deficit, target, full-capacity/deficit basis, and blocker mask;
3. terminal/compensated voltage, peak/drop/slope, and current;
4. frozen capacity/remaining evidence, attach/admission state, minute count,
   restore state, and persistence health;
5. full policy name, live factor/trust, stop request/latch, and pending release.

Page 47 remains exclusively the battery-health/SOC supervisor. Repeated
capacity fields on page 48 are frozen charge-decision inputs and must be labeled
as such, not presented as another live learner page.

## Rev B2 Bench Evidence

- Explicit `ENFORCE_ANCHORED` with a trusted 1.224 factor completed multiple
  exact coulomb-target stops. Generation 23 stopped at 719.773200 mAh against a
  719.706186 mAh target, a 0.067014 mAh overshoot, with durable `/CE=0/0`,
  qualified FULL, and clean persistence. An earlier run overshot by only
  0.032589 mAh.
- A powered-on software FULL displayed the Nokia-compatible `Battery full`
  notice. Powered-off completion stored the same qualified state without
  opening a dialog, as intended.
- During attached maintenance, filtered reference voltage crossed below 2600 mV
  while SOC was still 89 percent and correctly did not restart. At the exact
  80-percent SOC boundary, both conditions held and the durable one-second rearm
  created a fresh deficit-based generation; later attached generations completed
  normally.
- One BQ generation stopped after only 914.0067 mAh, 58.71 percent of its
  target, without a taper. The supervisor rejected FULL and performed its one
  durable recovery rearm. The BQ did not resume until physical detach/reattach,
  and the loop guard prevented repeated pulses.
- BQ terminal evidence that arrives only after the corroborated coulomb target
  is eligible for qualified FULL; early BQ completion or fault remains
  non-FULL. Charger detach, manual control, and failed `/CE` readback did not
  create learner anchors in the exercised paths.

The 1.224 factor is qualified only for the installed Rev B2 profile and current
pack evidence. Unknown, conflicted, discontinuous, or policy-ineligible evidence
continues under BQ control. Any ISET, timer, chemistry, or ACR-scale change
receives a new hardware/profile identity before its evidence can be used.

## Verification

Current host gates cover:

- the implemented transition/evidence matrix for every supervisor phase;
- attach bounce and STAT interleavings;
- manual disable never becoming FULL;
- detach during the two-second completion dwell;
- ACR/time wrap, gauge-session change, polarity loss, stale/repeated sample,
  and debug-force contamination;
- partial, empty, and unknown-SOC admission;
- first-poll rejection of previous-pack SOC/EMPTY evidence after an unprocessed
  nonzero gauge-session change, followed by admission from the new pack only;
- phone-load bursts producing negative current during charging;
- reset during charge and reset after software termination;
- `/CE` write/readback failure and retry without oscillation;
- BQ completion, safety stop, and software stop provenance;
- exact target arithmetic, provisional-versus-anchored policy gates, explicit
  factor trust, one shared supervisor/learner factor, overflow-safe SOC
  projection, and nominal-capacity safety operation with unknown SOC;
- the recovered `[1242, 1301] -> 1272 mAh` path end to end: no request at
  `1,556,927,999 nAh`, a request at `1,556,928,000 nAh`, durable latch before
  `/CE`, and qualified software FULL only after disabled readback;
- unconfirmed capacity exhaustion maps to conflicted admission and cannot stop
  charge at the old estimate-derived target;
- in-call gauge/supply sag with anchored remaining charge cannot learn EMPTY,
  while an emergency-floor shutdown remains non-learning;
- terminal-history rejection of invalid or non-authoritative current evidence;
- durable stop ordering, boot-time inhibit, control retry/readback failure,
  reset before/after terminal persistence, fail-open detach/floor release, and
  durable maintenance rearm across reset;
- strict maintenance boundaries (`SOC <= 80%`, filtered reference `< 2600 mV`),
  foreign-owner blocking, one-second `/CE` hold, durable inhibit before control,
  immediate release with persistence retry, exact-deficit readmission, and no
  repeated pulse after a failed BQ restart;
- no same-generation learner feedback and hardware-profile separation;
- fixed wire bytes, truncation, corruption, semantic validation, and all 16
  storage bindings/layout assertions;
- exact Net Monitor output fit and production compilation of the CDC surface;
- ASan/UBSan randomized public-API transitions;
- static layering guards proving no HAL or app headers enter the pure model.

Hardware regression after relevant battery, charger, storage, or power changes
covers attach/detach, already-full attach, partial charge, natural-empty
recharge, reset during charge, power-off charging, call/SMS load, BQ terminal
handling, `/CE` stop/readback, post-termination detach/reconnect, and an
indefinite-attached maintenance cycle.

## Success Criteria

- No false `Battery full` from detach, fault, manual control, or failed `/CE`.
- No charge restart after software termination except the one persisted,
  qualified attached-maintenance transaction.
- No new wake deadline or measurable dormant-current regression.
- Net charge is battery-terminal ACR, independent of phone load.
- Every terminal decision is explainable live and after reboot over CDC.
- Frozen admission inputs permit external comparison with a following qualified
  natural discharge without same-generation feedback.
- Battery health owns bars and capacity prediction but cannot directly control
  `/CE`; fused physical evidence owns EMPTY.
- Unknown evidence degrades to the BQ hardware path instead of guessing.

## Primary References

- Texas Instruments, BQ25171-Q1 data sheet:
  https://www.ti.com/lit/ds/symlink/bq25171-q1.pdf
- Panasonic, Nickel-Metal Hydride Batteries Handbook:
  https://eu.industrial.panasonic.com/sites/default/pidseu/files/downloads/files/id_ni-mh_1104_e.pdf
- Nokia NSE-8/9 Non Serviceable Accessories
  (`Nokia_3210_NSE-8_NSE-9_Accessories.pdf`, see
  [the reference index](nokia_3210_reference_index.md))
- Nokia NSE-8/9 System Module Technical Documentation
  (`Nokia_3210_NSE-8_NSE-9_System_Module_Technical_Documentation.pdf`, see
  [the reference index](nokia_3210_reference_index.md))

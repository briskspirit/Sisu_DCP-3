# BL-5C support on Rev B2

This is initial Li-ion support for the insertion contract below, not unrestricted
pack identification or a completed hardware qualification.
The test pack is labelled **1020 mAh, 4.2 V maximum charge voltage**. Nominal
capacity is a starting estimate; qualified discharge cycles supply learned
capacity. The voltage table below is provisional, not a measured BL-5C OCV curve.

## Insertion contract

- Disconnect the Nokia charger before every battery swap.
- Externally precharge a newly inserted BL-5C to at least 3.6 V.
- Identification requires five distinct, stable, valid LTC samples while the
  charger is disconnected. Samples are at least 200 ms apart and span no more
  than 50 mV.
- 3.6-4.3 V identifies Li-ion. 1.0-3.2 V permits the two-cell NiMH assumption
  only when empty RP retention and an unambiguous RP power-on reset coincide
  with gauge reset evidence during its initial acquisition. The intervening
  voltage range remains unknown.
- These rules do **not** distinguish a depleted Li-ion cell from NiMH. Inserting
  depleted Li-ion violates this initial contract; recovery charging is deferred.

Once identified, voltage alone cannot change chemistry. POWMAN scratch 6 retains
the latch across switched-core watchdog reset and dormant sleep, but not resets
of POWMAN itself, including POR, brownout, or external RUN. LTC POR/UVLO evidence
invalidates it. On boot, a retained identity is kept but exposed as UNKNOWN until
the gauge has been read, configured and produced a fresh valid conversion. An
initial I2C outage cannot enable charging from retention alone. If the eventual
read shows no gauge power reset, the retained chemistry returns even at depleted
voltage; no new voltage classification or charger unplug is needed for that
restoration. Ordinary runtime I2C failures and an explicit ACR re-zero do not
discard already-checked identity. Gauge reset during an established RP session,
RP brownout/RUN/debug/watchdog reset with lost retention, and corrupt retention
leave identity uncertain; none grants the low-voltage NiMH assumption. An
uncertain marker survives subsequent warm resets. High-voltage Li-ion evidence
can identify the pack again using the ordinary charger-disconnected qualification.
During initial detection, any fresh valid reading above the NiMH insertion range
permanently removes that insertion's NiMH permission, even with VIN present. A
pack first observed at 3.4 V, or partway through high-voltage Li-ion qualification,
cannot become NiMH just because it subsequently discharges below 3.2 V.
Charger-connected readings cannot identify either chemistry and cannot be reused
as identification samples merely by re-polling them after unplugging.

The cold-start permission is consumed by the first successful gauge boot-register
read, even if a subsequent configuration write fails; a later UVLO cannot reuse
the RP's historical POR flag. An initial I2C failure before that read does not
consume it. A completely unpowered RP and gauge
cannot distinguish battery removal from full supply collapse. This remains a
limitation of two-terminal identification, not proof of a physical insertion.
After such a collapse, a Li-ion pack must be externally precharged before use,
just like a newly inserted depleted pack. No flash history is treated as pack ID.

Unknown chemistry inhibits charging and normal battery-only power-on. Following
an initial flash onto an already powered NiMH system, disconnect the charger and
reseat the battery to supply insertion evidence. A reset that clears RP retention
while leaving the LTC powered can require the same recovery. Keeping the charger
connected through a battery swap is not supported: it can keep these domains alive.

## Charger control

The Rev B2 schematic selects BQ25171-Q1 configuration through TMUX1574, driven by
TCA8418 COL6. These are schematic-derived settings, not measurements of this pack:

| Mode | VSET | CHM_TMR | ISET | Configuration |
| --- | --- | --- | --- | --- |
| NiMH | 36 kohm | 24 kohm | 1.10 kohm | Two cells, 8-hour timer, about 273 mA |
| Li-ion | 18 kohm | 82 kohm | 1.10 kohm | 4.20 V, 10-hour timer, about 273 mA |

Firmware verifies /CE disabled before changing the selector and holds it disabled
for one second before allowing re-enable. TCA recovery preserves the desired
chemistry and reacquires outputs with charging disabled if selector ownership was
lost. A separate chemistry inhibit cannot be released by a debug charging request.
The older `diag_charger_adc` bench firmware remains NiMH-only and refuses its
manual enable command unless the battery has been identified as NiMH.

BQ regulates Li-ion CC/CV charging and retains its hardware termination and safety
timer. Its output-current termination includes the board load, so the supervisor
also qualifies taper using LTC net battery current. The NiMH 1.224 charge factor,
input-charge FULL inference, and 80%/2.6 V maintenance conditions do not apply.
Neither nominal nor learned capacity is a Li-ion charge-stop target.

Software taper requires three consecutive 60-second windows with battery voltage
at 4.17-4.25 V. Each window must have nonnegative net ACR current below 27 mA and
sampled positive-only average current below 27 mA. The positive cross-check prevents
discharge pulses from cancelling substantial charge input into a false taper.
BQ must report ACTIVE, /CE must be confirmed enabled without an inhibit, and input
voltage must exceed battery voltage by at least 500 mV. Gauge continuity, polarity,
pack identity, and chemistry must remain valid. Invalid evidence or sample gaps
over ten seconds restart qualification; cached samples cannot advance it. These
voltage and dwell settings passed the first USB-awake software-termination test;
broader load/temperature qualification remains open. The host has no direct
indication of BQ thermal regulation.

The supervisor persists its stop latch before inhibiting /CE and reports software
FULL only after disabled readback with valid gauge/pack evidence. It also accepts
qualified hardware BQ FULL. That hardware endpoint requires prior active charging, coherent
gauge/pack evidence, verified enable state, the normal completion dwell, terminal
voltage 4.10-4.25 V, current no greater than 35 mA, and charger input at least
500 mV above the terminal voltage. A BQ fault is not Li-ion FULL.

Both qualified termination paths retain a durable /CE hold. With the charger left
connected, recharge requires raw and reference battery voltage below 4.00 V for
60 seconds of fresh, valid discharge samples. The existing persisted maintenance
sequence then resets /CE and starts a new accounting generation. Foreign inhibits,
faults, stale evidence, and unresolved persistence prevent that restart. This
supersedes BQ's approximately 4.1 V automatic recharge threshold. No SOC condition
is applied to Li-ion. An unqualified BQ completion cannot authorize a host hold;
if BQ resumes on its own, accounting starts a new generation.

Disconnecting the charger clears the completed-session hold. Reconnecting starts
a fresh charge admission, so it can top up above 4.00 V; the maintenance voltage
gate applies only while the charger remains connected. Chemistry checks, BQ
regulation, and full qualification still apply to the new session.

A recharge candidate found during an existing standby wake keeps the RP awake
through the 60-second qualification and /CE rearm sequence. Otherwise the normal
minute-spaced standby samples would repeatedly expire the ten-second gap limit.
Invalid evidence or a voltage recovery cancels the candidate and releases this
sleep veto; healthy FULL hold adds no periodic wake source.

Reset retains the hold and charge origin but discards partial taper/recharge dwell.
Restoring a completed session does not re-anchor the learner at a later discharged
counter value. A missing FULL event can be replayed only while the live counter
still equals the saved endpoint; later loss of an anchor remains unqualified.
Restoration waits for the learner to initialize with the identified chemistry;
that startup ordering gap is not a pack mismatch. Failed LTC conversions invalidate
the supervisor's observation even while the HAL retains the last good values.
They restart taper/BQ-FULL qualification, or leave a software stop pending with
/CE held off until valid endpoint evidence returns. A confirmed gauge/pack change
or fault still rejects the endpoint.

The learner uses a 1.000 coulomb-ledger prior,
not a claimed measured charging efficiency. Chemistry-specific profile IDs prevent
using NiMH learning or charge-session evidence for Li-ion. Storage currently keeps
one active learner record; switching chemistry replaces incompatible evidence,
rather than maintaining a library of individual packs.

## Initial voltage policy

| Condition | Li-ion threshold |
| --- | --- |
| Battery-only power-on | 3.40 V |
| Early LOW notice | Below 3.60 V for 10 s, at least 3 fresh samples |
| Early LOW notice recovery | At least 3.65 V for 10 s, at least 3 fresh samples |
| Endpoint voltage LOW reference / fast terminal | 3.50 / 3.40 V |
| Gauge EMPTY | 3.20 V |
| Operating shutdown outside a call | 3.30 V |
| Emergency floor during a call | 3.00 V |
| Voltage-only bar floors 1 / 2 / 3 / 4 | 3.20 / 3.70 / 3.85 / 4.00 V |

Existing sample qualification, load-sag handling, and capacity-based display rules
still apply. Early LOW is notification-only: it cannot corroborate counted-zero
EMPTY, teach capacity, force bars, or release the charge supervisor's physical
battery-floor hold. It bypasses the slow bar filters but rejects brief sags;
invalid conversions or gaps over 10 s restart its entry/recovery qualification.
The active notice survives invalid data and clears on qualified voltage recovery
or verified positive charging. Pending qualification briefly vetoes powered-on
standby after an existing wake; healthy standby gains no extra periodic wakeup.
An abrupt collapse can still reach the operating floor before LOW, and shutdown
must not wait for a notice. The original dialog, localization, tone, screen
deferral and repeat cadence are unchanged.

For both chemistries, measured capacity with a valid anchored SOC ledger adds a
LOW notice at remaining charge <=5%. It stays active until remaining charge is
above 7%; comparisons use exact nAh, not the rounded display percentage. Nominal
capacity, voltage-bootstrap SOC, conflicting capacity history, and unavailable
remaining-charge evidence cannot drive this notice. Losing that authority clears
its hysteresis latch; the independent voltage warning remains available.
Verified positive charging suppresses the shared warning schedule, but a merely
connected charger does not. The percentage notice never supplies EMPTY evidence,
teaches capacity, or releases the charger's physical battery-floor hold. It uses
the same dialog and repeat timer as voltage LOW, without a second notification.

`hw` reports capacity, early-voltage and endpoint LOW and the last eligibility,
shown/deferred and EMPTY timestamps/voltages. This diagnostic history survives soft-off while
RAM is retained, not a reset or deep sleep; it is not a new persistent record.

No NiMH resistance compensation or startup voltage bias is applied
to the Li-ion voltage estimate. Gauge telemetry accepts up to 4.5 V so it can
report a 4.2 V cell; this acquisition bound is **not** a charge-voltage limit.

## Qualification And Remaining Checks

The first bench cycle demonstrated Li-ion selector operation, hardware BQ FULL,
and a learned FULL-to-EMPTY discharge of 812 mAh. The subsequent charge ended by
software taper at 4.196 V after 803.231 mAh input, with three qualifying windows,
verified /CE inhibit and a clean persisted FULL anchor. Active charge origin and
learner data also survived a firmware update without gauge/session reset.
The latest image restored the software FULL hold after flashing without moving
the learner's original FULL anchor. Stored user/system data compared byte-for-byte
equal before the updated firmware first ran.

One automatic maintenance restart and subsequent hardware BQ FULL are confirmed
by the retained charge history. That recharge delivered 283.566 mAh over 86 min
51 s and ended at 4.186 V. No host capture exists at the precise restart trigger,
and overnight USB presence was not independently established. This verifies an
automatic recharge, not a separate USB-disconnected standby timing test. A later
manual charger replug started a fresh session at about 92% estimated SOC,
consistent with the documented top-up behavior.

Early LOW presentation on the latest hardware image, recharge with USB disconnected
throughout qualification, and broader load conditions still need live checks.
Host tests cover early warning ramp/sag/gap behavior, exact 5%/7% capacity notice
boundaries and confidence gates for both chemistries, app dialog deferral/cadence,
unchanged EMPTY protection, and chemistry reset/retention cases including delayed
boot reads, configuration failures and charger-interrupted detection. Recharge
tests also exercise minute-spaced sleeping samples followed by the one- and
five-second awake cadences, including cancellation and release of the sleep veto.
These do not
qualify all pack conditions. Pack temperature behavior and depleted-cell recovery
also remain unqualified. LTC die temperature is not cell temperature.

In particular, the pre-firmware selector state needs qualification. TCA COL6 starts
as an input with a typical 105 kohm pull-up. Against the schematic's 100 kohm
pull-down and TMUX input pull-down, the estimated level is about 1.6 V at a 3.3 V
rail, above TMUX's 1.2 V high threshold. Thus the pull-down alone does not establish
a guaranteed NiMH default before firmware runs. This is a circuit analysis concern,
not an observed failure or a software-resolved hardware guarantee. The DUT also
requires its existing /CE pull-down rework for unpowered recovery behavior.

References: [BQ25171-Q1 datasheet](https://www.ti.com/lit/ds/symlink/bq25171-q1.pdf),
[TCA8418 datasheet](https://www.ti.com/lit/ds/symlink/tca8418.pdf),
[TMUX1574 datasheet](https://www.ti.com/lit/ds/symlink/tmux1574.pdf), and
[RP2350 datasheet](https://pip.raspberrypi.com/documents/RP-008373-DS).

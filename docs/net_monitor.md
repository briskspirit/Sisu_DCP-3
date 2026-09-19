# Net Monitor v2

Status: implemented for Rev B2 with Telit LE910C1. The read-only core, local
controls, and guarded radio-maintenance paths are bench-qualified. Long-soak
and induced-failure checks are retained below as regression procedures.

Net Monitor v2 is a development instrument, not a clone of the original GSM
Field Test payloads. It keeps the Nokia 3210 v6.00 overlay geometry while
showing typed LTE/modem and Rev B2 firmware evidence that is useful on this
hardware.

## Scope

- Supported target: Rev B2 with the Telit backend.
- The modem-free Rev B2 diagnostic build exposes local pages only.
- There is no compatibility layer for discontinued boards or modem backends.

## UI Contract

Open `Menu -> Net monitor`, enter a two-digit selector, and press Navi/OK.

- `00` disables the overlay.
- A known but unavailable selector displays `UNSUP`.
- An unknown selector displays `NO TEST` and keeps the previous selection.
- Up/Down browse only pages supported by the current build, wrapping at the
  ends of the registry.
- The selector and schema version are persisted. Schema v2 deliberately resets
  an old v1 selector to `00`, so a retired read-only ID cannot become an edit
  page after an upgrade.

The overlay remains the traced `x=6, y=7, w=72, h=30` FS4 window. It displays
four rows at a seven-pixel pitch. Multi-page content rotates as one complete
frame every 2.4 seconds; individual rows never rotate independently.

Freshness is explicit:

| Text | Meaning |
|---|---|
| `?` | A refresh is in progress; the last valid sample remains visible. |
| `!` | One to four consecutive refreshes have failed; the last valid sample remains visible, including while the next retry is in progress. |
| `READING...` | No valid sample exists and fewer than five automatic refreshes have failed. |
| `AT STARTUP` | The modem transport is not ready. |
| `NO DATA` | No complete sample exists. |
| `AT REJECT`, `TIMEOUT`, `BAD DATA` | The latest refresh failed and the five-attempt grace has expired; cached values are hidden. One-shot pages report these immediately. |
| `CANCELLED` | The query was cancelled and no grace-eligible sample can be shown. |
| `STALE DATA` | The last sample exceeded the page's age budget and is hidden. |
| `QUERY PAUSED` | Call handling has suspended diagnostic AT queries; cached values are hidden. |
| `--` | This field was absent, not numeric zero. |
| `UNSUP` | The page or modem command is unsupported. |

### Key Ownership

- Read-only pages consume only Up/Down on standby.
- Digits, Star, Hash, C, Navi, and Power pass to normal phone behavior on a
  read-only page.
- Editable pages consume only the exact digit/Star/Hash keys listed for that
  page, and only on standby.
- An owned key acts once on key-down. Its hold event is swallowed without
  repeating the action or leaking into speed dial/number entry.
- Navi is never intercepted by an active overlay. It continues to open the
  normal Menu/Options path.
- C and Power are never Net Monitor edit keys.
- During every call surface, Net Monitor consumes no keys. Up/Down remain call
  volume, digits/Star/Hash remain DTMF, C remains End, and Navi remains Options.

### Route Policy

Read-only pages render over standby, outgoing call setup, and a connected call.
Modem query pages show `QUERY PAUSED` during calls and emit no diagnostic AT
traffic. Local audio pages remain live.

The overlay is suppressed for incoming and waiting prompts, call-volume popups,
Options, menus, editors, dialogs, alarms, keyguard, number entry, missed-call/SMS
notices, SIM-missing UI, and other higher-priority surfaces. Editable pages are
standby-only; leaving standby immediately releases their temporary action.

## Architecture

The implementation has four ownership layers:

1. `net_monitor_app` owns selection, route policy, frame timing, and normalized
   key dispatch.
2. `net_monitor_logic` owns the single declarative page registry, capabilities,
   browse order, and exact edit-key masks.
3. `net_monitor_render` formats immutable service snapshots. It performs no
   UART, I2C, GPIO, flash, codec, or core1 operation.
4. `netmon_diag_service`, `netmon_control_service`, and `modem_service` own
   local sampling, typed actions, and Telit query scheduling respectively.

The app shell includes no HAL, codec, core1, accessory, or modem-vendor header.
Telit syntax and parsers stay in the `modem_vendor_telit*.c` backend modules;
the UI consumes the
neutral `modem_diag_snapshot_t` and `modem_maintenance_snapshot_t` models.
The maintenance state machine owns sequencing, readback, rollback, and call
preemption. Net Monitor never submits an arbitrary AT command.

Both local and modem snapshots have compile-time size ceilings. There is no heap
allocation. Local rates are calculated by the provider at a 250 ms cadence,
not by rendering code.

The larger radio-policy and antenna-tuner groups refresh every 30 seconds only
while one of their pages is selected, before their 60-second cache age limit.
This preserves the last sample with `!` through four failed retries and replaces
it with the explicit failure cause on the fifth.

## Telit Query Scheduler

A selected modem page subscribes to exactly one typed group. Admission marks
the group pending immediately, duplicate requests coalesce, and changing pages
invalidates the old generation. Parsers write a private shadow. The service
publishes the group atomically only after every required row and final validate.
A malformed line, rejection, cancellation, or timeout preserves the last good
payload internally for atomic recovery and changes only its metadata. To avoid
normal refresh flicker without hiding a persistent fault, the renderer shows
that payload with `?` during an ordinary refresh and with `!` after the first
four consecutive failed refreshes. The fifth failure replaces it with the named
cause, which remains stable while later retries are in flight. A success resets
the streak immediately. One-shot pages report their first failure immediately.
An age cap independently prevents an abandoned query from leaving cached data
visible forever.

The service sends one AT command, yields to normal modem scheduling, and then
continues the group. Call-model CLCC and queued call/ordinary operations are
checked before every next diagnostic command. An in-flight command is allowed
to finish so its final cannot bind to another operation. A call-control
admission cancels the rest of the group.

An optional command may be skipped only after an explicit response/final that
shows it is unsupported. Silence is a transport timeout and fails the atomic
group.

| Group | Pages | Refresh | Telit commands |
|---|---:|---:|---|
| Serving | 01, 02, 03, 05 | 2 s or 5 s | `#RFSTS`, optional `#MONI` |
| Registration | 04 | 5 s | `CREG?`, `CGREG?`, `CEREG?`, optional `CIREG?` |
| Radio policy | 06-08 | on entry | `CFUN?`, `COPS?`, ENS/profile/RAT/band/search readbacks |
| Antenna tuner | 09-13 | on entry | `#STUNEANT?`, capability, `#GTUNEANT?` |
| Packet | 14 | 10 s | `CGATT?`, `CGACT?`, and `CGCONTRDP` |
| SIM | 15 | 10 s | `#QSS?`, optional `CPIN?` |
| Voice/IMS | 16 | 10 s | `CEVDP?`, optional `CIREG?` |
| Temperature | 17 | 5 s | `#TEMPMON=1` |
| Identity | 26 | on entry | `CGMM`, `CGMR`, `CGSN` |
| Digital voice | 27 | on entry | `#DVI?`, `#DVIEXT?` |
| Call cause | 33 | on entry | `CEER` |
| SMS configuration | 35-36 | on entry | CSMS/CNMI/CSMP/CSCA/CSDH and optional IMS plus voice/fax/e-mail MWI readbacks |
| Storage capability | 37 | on entry | `CPMS?`, `CPBS?` |

Local pages never wake the modem. Cache pages never launch their own AT query.

## Page Registry

### Radio and Network

| ID | Page | Main evidence |
|---:|---|---|
| 01 | Serving RF | RAT, band, channel, PCI, calculated RF1-RF4 throw |
| 02 | Signal | RSSI, RSRP/RSCP, RSRQ/EcN0, SINR, TX power |
| 03 | Cell identity | PLMN/operator, area, cell ID, PCI |
| 04 | Registration | CS, PS, EPS, and IMS registration |
| 05 | Radio state | AT/registration, MM/RRC/service/DRX |
| 06 | Search policy | `NWSCANTMR` configuration and remaining pause |
| 07 | Operator profile | COPS, ENS, firmware-switch/profile evidence |
| 08 | RAT/band policy | WS46, selection mode, persistent and RAM masks |
| 09 | Antenna tuner | enable/capability/table integrity/current calculated throw |
| 10-13 | Tuner RF1-RF4 | control bits and assigned band masks |
| 14 | Packet data | attach, contexts, APN, address |
| 15 | SIM | QSS/CPIN plus normalized present/ready state |
| 16 | Voice/IMS | voice domain, IMS registration, VoLTE evidence |
| 17 | Modem temperature | Telit die-temperature measurement |

The active RF throw is a policy inference from typed RAT/band plus the verified
tuner table. It is never labelled as a measured GPIO level.

Some fields intentionally appear on more than one page so a page remains
self-describing during a bench action: page 1 summarizes RAT/band/PCI/throw,
page 2 repeats RAT/band beside signal and TX power, page 3 repeats PCI beside
cell identity, page 4 is the registration matrix, and page 16 repeats IMS and
registration in the voice-domain context. Page 7 labels its operator as `COPS`
to distinguish selected policy from page 3's serving-network identity.

### Modem and Telephony

| IDs | Pages |
|---:|---|
| 20-24 | modem FSM, scheduler, UART health, UART sleep, init/provisioning |
| 25 | Rev B2 modem rail and control-pin electrical state |
| 26-27 | identity and Telit digital-voice configuration |
| 28 | modem power-cycle and automatic-recovery counters |
| 29 | last AT command/line and error counters |
| 30-32 | call projection, all call legs, and all call transactions |
| 33 | call result and CEER terminal cause |
| 34-36 | SMS traffic, configuration, and multi-category message waiting |
| 37 | SMS and phonebook storage capabilities |

### Rev B2 Local Diagnostics

| IDs | Pages |
|---:|---|
| 40-48 | battery/current, mAh coulomb window, LTC health, charger, shared 3V8, service USB, battery health/SOC/resistance learning, charge supervisor |
| 50-58 | build, devices, shared IRQ, input queue, headset/hook, wake pins, RTC/alarm, outputs |
| 60-66 | audio route/codec, codec I2S, modem RX/TX, bridge FIFO and bridge-start evidence |
| 67-68 | live mic meter and audio fault/relock counters |
| 70-71 | LCD calibration/sleep and backlight state |
| 72-73 | storage health and per-unit dirty/degraded/commit evidence |
| 74-75 | main-loop timing plus core1 queue/heartbeat/flash/audio-recovery and both-core stack high-water evidence |
| 76 | littlefs partitions, category budgets/file counts and local SMS cleanup |
| 80-81 | soft-off subsystem state and retained wake evidence |
| 82-88 | sleep readiness, entry, aborts, POWMAN evidence, clocks, blockers, and boot/wake timing |

Page 76 has eight frames. The first shows system/user allocated versus total
KiB, physical user free space, and the recovery reserve. The next five show
contacts, inbox, outbox, incomplete SMS and OTHER: allocated/budget KiB, file
count, and rounded-up file-data KiB (including record envelopes). Filesystem
metadata makes allocated space larger than file data. Free space includes the
reserve and does not override category budgets. OTHER includes the existing
call logs, pictures, tones, dictionary and divert records.

The last two frames show mailbox/pending/queue counts, SMS readiness/full state,
retention-clock validity, and boot-local expiry/filter/loss counters (`E/F/L`). `F`
counts controls consumed by the local message store; modem-side filters retain
their separate counters. Sampling is at most once per five seconds while page
76 is selected and the phone is on, only in the shared storage-safe window.
Failed samples show `FS NOT READY`; samples deferred
for more than 15 seconds show `FS STALE`. Neither case reports unknown
usage as zero. Page selection and rendering issue no modem commands or writes.

Power page 47 is the battery-health/SOC view. Its five frames show:

1. nominal/learned/remaining mAh, SOC, authoritative bar value, and SOH;
2. confidence, full-anchor qualification, predicted exhaustion, healthy-voltage
   disagreement, capacity overrun, and accepted/rejected cycles;
3. SOC confidence/provenance, charge/discharge segment, and bootstrap voltage;
4. last cycle, three-cycle spread, pack generation, and persistence health;
5. high/mid/low effective-resistance medians and accepted-sample counts.

`--` and `-` mean unavailable, never numeric zero. `PRIOR`, `OBS`, `LEARN`,
and `CONFL` expose confidence explicitly. `P1` is unconfirmed capacity
exhaustion, `V1` is a simultaneous healthy-voltage disagreement, and `O` is
overrun mAh. The page itself is read-only; the health supervisor owns bars,
while fused physical evidence owns EMPTY.

Power page 48 is the charge-generation view. Its five frames show frozen
decision inputs, exact target accounting and target basis, voltage-curve evidence, persistence,
the configured observe/anchored/bootstrap policy, charge-factor trust, and the
durable stop-latch/release state. Repeated capacity fields are admission-time
snapshots, not a second live learner view.

The remaining repeated fields are deliberate context, not duplicate pages:
page 50 is the sole build identity; page 54 owns the input queue while page 55
owns headset/hook state; page 70 owns LCD state while page 95 owns calibration;
page 71 owns backlight state while page 89 owns its persistent level and page 90
owns output tests; page 80 shows actual soft-off subsystem state while page 88
interprets entry blockers.

## Editable Pages

| ID | Keys | Action and cleanup |
|---:|---|---|
| 89 | `1/3`, `4/6`, `0`, `*`, `#` | Backlight -/+ 5%, -/+ 1%, restore stored, save active, preview the 100% default. Unsaved previews revert on leave. |
| 90 | `1`, `2`, `3`, `0` | Backlight, vibra, buzzer, stop. Tests expire after 5 s and stop on route/page leave. Audio cleanup is ownership-qualified and cannot silence a later production ring/vibra. |
| 91 | `1`..`5`, `#` | Force 2200/2320/2500/2650/2900 mV; `#` returns to measured battery. Force clears on leave. |
| 92 | `1`, `2`, `3`, `4`, `#` | Simulate charger in/out, enable/disable charger, return input to automatic. Force clears and the entry enable state is restored on leave. |
| 93 | `0`, `1`, `2` | Headset auto/forced inserted/forced removed. Returns to auto on leave. |
| 94 | `0`, `1` | TPS63020 power-save/forced-PWM preview. Entry state is restored on leave. |
| 95 | `1/3`, `4/6`, `7/9`, `0`, `*`, `#` | Decrease/increase VOP, TC, and bias; revert stored; preview stock tuple. Previews auto-revert after 10 s. First `*` shows stock contrast and arms for 5 s; second `*` reapplies and saves the captured tuple. |
| 96 | `0` | Reset the local coulomb/rate measurement window. |
| 97 | `*`, `1`..`7`, `#` | `*` arms for 10 s; presets set `NWSCANTMR` to 5/30/60/300/900/1800/3600 s; `#` performs a read-only query without arming. Every write is read back. |
| 98 | `*`, `1`..`5`, `0`, `#` | `*` arms for 10 s; presets temporarily test B2/B4/B5/B12/B14 in Telit RAM only. `0` or `#`, page leave, a call, error, or timeout restores the exact entry mode and masks. |
| 99 | `*`, `#`, `1`..`4`, `0` | `*` then `#` enters RF-safe manual mode (`CFUN=4`) after production tuner/GPIO preflight. `1`..`4` selects RF1-RF4; `0`, page leave, a call, error, or timeout restores automatic tuner ownership and `CFUN=5`. |

LCD VOP/TC/bias is stored atomically in a versioned tuple. Existing VOP-only
calibration is migrated using stock TC1/bias4 and retained as a rollback mirror.
An intentionally unreadable saved tuple can be recovered without the display
over CDC using `lcd stock` followed by `lcd save`.

The production SIM-missing screen intentionally applies a three-second
ready-state dwell. LE910C1-WWX can report `#QSS: 0` while startup deliberately
holds RF at `CFUN=4`; a positive presence observation cancels the candidate
immediately. Net Monitor page 15 continues to expose the raw modem observation.

Pages 97-99 use a typed maintenance service and do not subscribe to ordinary
background diagnostic groups. This avoids query contention and display flicker:
the control's own verified readback is its only displayed authority. A band test
writes a recovery record before the first mutation and clears it only after an
exact restore. Boot consumes a surviving record before normal operation. A
restore/readback failure forces `CFUN=4`, reports `RF LOCKED`, and keeps the
record for the next boot rather than returning to transmit-capable service with
unknown RF state.

Startup provisioning verifies a 60-second no-coverage scan pause and repairs
`NWSCANTMR` only on mismatch. Page 97 presets last until changed or the next
modem startup; they do not replace that startup default.

Startup also verifies persistent SIM-based carrier selection (`FWAUTOSIM=1`).
Page 07 reports the module's selected profile; firmware does not force a
particular carrier with `FWSWITCH`.

Page 98 labels the lifecycle explicitly: `TEST Bx` while applying, `BAND Bx`
only while that verified preset is active, `RESTORE Bx` during rollback, and
`BAND BASE` after the exact entry policy has been restored. Leaving the page is
itself a restore request, so serving-cell pages always observe baseline policy.

Page 99 retains the last confirmed physical SIM-ready state while maintenance
owns `CFUN=4` and for Telit's bounded 10-second lifecycle after restoring
`CFUN=5`. A READY observation clears deferred downgrade evidence but does not
end that window: the fitted WWX was observed reporting READY, then `#QSS: 2`,
then `#QSS: 3`. If READY never converges, the latest deferred observation
becomes authoritative at the deadline. This prevents a maintenance-owned
transition from opening the production Insert-SIM surface or skipping the
SIM-dependent pass that requalifies SMS RI wake.

## Verification

Host and build gates:

```sh
./tests/run_tests.sh
cmake --build build -j4
```

The default build compiles the Telit production firmware; configure with
`-DSISU_BUILD_DIAGNOSTICS=ON` to also build the none-vendor diagnostic
images so both vendor gates compile in one pass.

The host suite covers registry uniqueness/order, capability filtering, route
visibility, exact key ownership, held-key isolation, all-frame FS4 width, absent
field rendering, atomic query commit/failure/cancellation, call preemption,
unprefixed response/URC interleaving, optional-query timeout semantics, bounded
controls, output ownership, LCD persistence, and app boundary audits.

Bench-qualified on the fitted LE910C1-WWX:

- live radio, modem, call, SMS, power, input, sleep, and audio pages render and
  refresh without the former startup `ERR`, stale-cache, or clipped-field bugs;
- pages 89-96 perform their local actions and restore unsaved/temporary state;
- page 97 writes and reads back the 30-second `NWSCANTMR` setting;
- page 98 applies temporary band presets and restores the exact entry policy;
- page 99 selects all four RF throws in `CFUN=4`, restores through both `0` and
  page leave, returns automatic tuner ownership and `CFUN=5`, and regains
  registration; and
- normal SMS RI wake and incoming-call audio work after radio maintenance, with
  zero UART error counters. Host tests cover call-preemption ordering and the
  missing-READY timeout.

Regression protocol after relevant modem, UI, or scheduler changes:

1. Compare every query page with direct CDC readback after changing Telit
   firmware or its AT guide revision.
2. Run at least 20 call cycles while watching modem, call-model, I2S, bridge,
   and relock pages; counters must remain bounded.
3. Leave each polling group selected for ten minutes and confirm bounded queue,
   power, and call-admission behavior.
4. Induce one query timeout and registration loss to recheck freshness/error
   markers and recovery without deceptive cached data.
5. Interrupt page 98 by RP reset after mutation; boot must restore the stored
   policy before normal service becomes transmit-capable.
6. Repeat output-test cleanup against a real incoming ring/vibra, then run a
   mixed long-cycle control and call/SMS soak.

The production modem-status path now carries typed voicemail-waiting state from
startup readback and unsolicited updates to the standby cassette icon. Page 36
remains a diagnostic readback rather than the owner of that state.

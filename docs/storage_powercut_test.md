# Storage Power-Cut Bench

This bench is retained for the historical stage-1 layout only: 128 KiB littlefs
plus the untouched 128 KiB journal. **Do not flash it after the partition split.**
The expanded user filesystem no longer fits its bounded 128 KiB view.

This opt-in service build replaces the phone runtime with a storage test.
It does not run normal apps, modem startup, charging policy, or store-service
commits. Use a disconnected Nokia charger. It keeps production core1 codec DMA
running and uses the unchanged littlefs adapter and flash park/IRQ/watchdog HAL.
Do not leave this image installed for normal phone use.

## Build and Arm

```sh
cmake -S . -B tmp/build-powercut -DSISU_STORAGE_POWERCUT_BENCH=ON -DRELEASE=OFF
cmake --build tmp/build-powercut --target sisu_dcp3_revb2_telit -j 8
```

The artifact is `sisu_dcp3_revb2_telit_powercut.uf2`. Normal builds default the
option off; release builds reject it. Flash only after capturing the current
flash image. Do not erase storage when loading the diagnostic.

CDC commands:

- `status`: report verification, generations, counts, legacy CRC, and park errors.
- `arm confirm`: initialize a new run, or resume a paused run without resetting
  its baseline/counters. Initialization requires the existing migration marker.
- `runusb confirm`: exercise writes while USB is connected for preflight only.
- `pause`: reconcile any pending operation and durably disable automatic writes.
- `bootsel confirm`: enter the ROM for readout or firmware restoration.

With an armed run, disconnecting USB starts writes. Reconnecting USB pauses
them; no normal phone UI is available. Every boot validates the prior records
before writing again. After 512 completed replacements in a boot, the test
pauses to bound wear. A new boot gets another bounded run; `arm` does not bypass
that per-boot limit.

## Manual Procedure

1. Complete a USB-connected preflight and check the counters. Arm the run and
   confirm the LCD says `USB - PAUSED` with no error.
2. Leave the Nokia charger disconnected and unplug service USB. Wait for
   `CUT POWER` and an increasing write counter.
3. Remove the battery while the counter is advancing. Confirm the LCD/backlight
   goes completely off; leave the battery out for at least five seconds.
4. Reinsert the battery. The diagnostic starts by itself; no Power-button press
   is needed. Confirm `CUT POWER` returns and writes advance.
5. Repeat with varied timing, initially 20 cycles, usually 1-5 seconds after
   writes begin. Stop immediately on `FAIL - STOP`, unexpected `NOT ARMED`, a
   failure to boot, or a counter that does not advance. Do not reflash or reset
   evidence after a failure; connect USB for inspection instead.
6. Reconnect USB with the battery installed. Capture `status`, then `pause`.
   Save the flash image and compare protected records and the complete legacy
   bank with the preflight capture before restoring normal firmware.

## Oracle and Scope

Only records `fff7` (control), `fff8`, and `fff9` (test data) are modified. Data
sizes cycle through 96, 128, 4064, 240, 241, 1024, and zero bytes. This covers
inline records, the inline/external boundary, and maximum record payloads.
The two test records use deterministic, generation-specific byte patterns.

Each update first commits an intent naming the exact old and next generation,
then replaces its data, verifies readback, and commits acknowledgment. Following
power loss, only that intent's old or new value is acceptable; all other data
must match exactly. A successful write followed by old data fails immediately.
The control record and data record use independent littlefs atomic replacements,
not an assumed multi-file transaction.

The initial lengths and CRCs of all 16 semantic records, the import marker, and
the earlier `fffe` scratch record are protected. The whole legacy bank is also
hashed on every boot and is never written. Missing/corrupt control or data does
not auto-initialize a new run. A fully erased filesystem is rejected before the
adapter can format it. Mount recovery can still finish an interrupted littlefs
metadata transaction, as it does in production.

`boots` counts durably recorded, verified diagnostic starts, not necessarily
physical battery removals. `recovered_old/new` count pending intents observed
at startup; they do not prove power failed inside a particular flash instruction.
All counters are lower bounds if another cut interrupts their own commit.
The failure state halts further test writes. It is not forcibly persisted into a
possibly damaged filesystem: observe the LCD and stop on the first failure.
Host-side baseline captures remain necessary for independent evidence.

Battery removal tests abrupt supply loss and uncontrolled contact transients,
not a specified slow voltage ramp. Controlled brownout qualification needs a
programmable supply and rail measurements. Finite manual cuts complement, but
do not replace, exhaustive simulated interruptions. This bench does not test
incoming calls, modem UART load, or active voice quality.

## Qualified Run

The operator reported 50 boots with no errors. Inspection recorded 50 verified
starts, 834 completed replacements, and 35 recovered pending intents: 19 kept
the old generation and 16 recovered the new generation. The handoff had already
recorded two starts; this counter is not a count of 50 additional battery cuts.
Independent flash readout confirmed all 18 protected records and all 128 KiB of
the journal were byte-identical to their pre-test baseline. The final observed
boot had no flash pause/resume timeout or watchdog-failure indication.

That result passed the gate for the system/user split. The partition migration
and both volumes' full/error behavior are tested separately on the host; this
physical run does not qualify the new migration sequence against every possible
electrical interruption. A future power-cut bench for the split layout must
protect both volumes and use a new baseline, not reuse this run's oracle.

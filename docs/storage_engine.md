# Storage Engine

## Flash Layout and Transactions

The 128 KiB immediately below the deployed journal (`0x101c0000` through
`0x101dffff`) holds the littlefs record store. The legacy journal stays
at `0x101e0000` through `0x101fffff`. The build guard protects both regions.
All sixteen persistent units now use littlefs; the legacy region is read-only
input for a one-time import. Phonebook contacts and ordinary SMS bodies remain
modem-backed in this stage.

littlefs v2.11.3 is vendored unchanged with its BSD-3-Clause license. Its adapter
uses static buffers, 4 KiB erase blocks, 256-byte program/cache units, and the
same HAL core1/DMA park, IRQ masking, and watchdog leases as journal commits.
Programming stages each page in RAM; no caller's XIP buffer is read with XIP
disabled. Core0 is the sole filesystem owner; no filesystem calls run in IRQs.

`storage_backend.h` exposes opaque semantic records, not filenames or littlefs
types. Replacements write and close a temporary record before atomic rename.
Each record has a versioned ID/length/CRC envelope. Every mount completes pending
littlefs consistency work before exposing reads: an interrupted cross-pair
rename must not be mistaken for a missing record. The adapter remounts after
I/O failures before retrying. Only wholly erased media can be autoformatted;
a corrupt filesystem is never silently replaced with defaults. An interrupted
initial format that cannot mount requires explicit recovery, not autoformat.

The service-console command `storetest confirm` writes only reserved scratch
record `fffe`, performs eight maximum-sized replacements and remount/readbacks,
and reports elapsed time, allocated blocks, and flash-park diagnostics.
It must not be used during ordinary phone use: it intentionally bypasses the
normal audio deferral to exercise the flash/DMA handshake.

Host tests interrupt each program/erase boundary before, halfway through, and
after the media change. They cover inline records, empty records, maximum-sized
records, grow/shrink replacements, cuts during recovery, and all sixteen legacy
units during import. Other cases cover full media, repeated remounts, allocation
churn, transient busy/error recovery, corruption handling, HAL bounds and
critical-section ordering. Physical power removal remains a separate bench
test; host interruption tests do not reproduce electrical brownout behavior.

## Migration

On the first boot, the engine loads each legacy unit using its existing codec,
applies the existing defaults/schema migrations, and writes all sixteen units
to littlefs. Record `ffff` is a migration-complete marker, published last. A
reset before that marker repeats the import from the unchanged source. A valid
marker makes littlefs authoritative permanently: missing or damaged new data
does not resurrect stale charge latches, SOC anchors, or user settings from
the old journal. An unreadable/invalid marker fails initialization.

An unused flash range may still contain bytes from an older, larger firmware.
After installing firmware whose build guard protects this region, provision
the new region explicitly if needed: erase **only** `0x101c0000..0x101e0000`
(exclusive end) with the device in BOOTSEL, then boot to format/import. Never
erase the legacy region as part of this step. Do not boot older firmware after
migration expecting it to see new settings: it only understands the old store.

The C firmware exposes a typed local NVM service backed by littlefs.
Phonebook and ordinary SMS contents remain modem-backed, but preferences, profile state,
clock/alarm preferences, speed dials, call-register lists, T9 learned words,
saved and pending picture messages, own-tone/composer drafts, call-divert editing
history, and per-pack battery health/SOC evidence are stored through this layer.

## Layers

- `nvm_hal`: raw media operations and geometry, with the RP2354 flash safety
  handshake implemented in `nvm_flash_hal.c`.
- `storage_lfs`: filesystem/block-device adapter and atomic opaque records.
- `storage_backend`: record read/write contract and board composition point.
  It contains no domain schemas. A future FRAM implementation can use this
  contract without exposing FRAM or filesystem calls to applications/codecs.
- `storage_journal`: legacy reader used only during import in production.
- `store_service.c`: record orchestration, immutable unit registry, commit
  scheduling, failure isolation, and diagnostics. It owns the single shared
  payload buffer but no domain state.
- `store_settings.c` and `store_calls.c`: the keyed settings/default table and
  call-accounting records respectively. The call owner also contains the sole
  storage-side RTC dependency and the non-resettable Life timer policy.
- `store_t9.c`, `store_pictures.c`, `store_tones.c`, `store_divert.c`,
  `store_warranty.c`, `store_battery_learning.c`, and
  `store_battery_charge_supervisor.c`: one state owner and wire codec per
  semantic persistence unit.
  All domains publish the same typed application API from `store_service.h` and
  register private reset/serialize/apply operations with the engine.

## Storage Units

Each unit is one independently replaced record:

- settings: phonebook
- settings: SMS
- settings: calls
- settings: profiles
- settings: clock
- settings: system
- call list: missed
- call list: received
- call list: dialled
- T9 user dictionary: up to 16 learned words, newest/promoted first
- picture messages: seven shared saved slots and two pending receptions, with
  bitmap, caption, and sender metadata; see [Picture messages](picture_messages.md)
- own tones: two composer slots for user-composed melodies
- call divert: delay setting and per-condition number history; live activation
  state is always network-owned and is never restored from flash
- service/warranty: write-once board IMEI, manufacture/repair/purchase records,
  and flags
- battery learning: profile-tagged capacity history, confidence evidence,
  effective-resistance bins, full/empty evidence, and a persistent
  provenance-tagged SOC origin
- charge supervisor: frozen charge-generation accounting, policy/factor trust,
  latest terminal summary, qualified-FULL authority, and durable `/CE` stop and
  attached-maintenance latches

The non-resettable Life timer lives in the settings/calls unit beside the four
ordinary duration counters. This matches v6.00's call-accounting ownership and
lets a completed call persist all five values in one record update.

The flash budget guard protects the combined 256 KiB reservation: 128 KiB
littlefs plus 128 KiB legacy import source. Expanding/reclaiming these regions
is a separate migration, not part of the current change. The unit-count guard
preserves the legacy ID/order contract and 16-bit diagnostics mask. Small
records share filesystem metadata blocks instead of owning 8 KiB sector pairs.

The removed SMS-status journal was unit 14. Removing it did not change the
index, unit ID, or flash offset of any older live setting, call log, or semantic
blob. Battery learning later reused that pair with a distinct payload magic, so
an upgraded phone rejects stale SMS-status bytes before its first learner write.
The charge supervisor retains legacy unit ID 15. Its stop
latch is read before the first charger-enable output is driven after boot, so a
reset cannot silently restart a software-terminated charge. Its fixed 128-byte
`CGS4` payload persists maintenance rearm state, the automatic-rearm loop guard,
and the latest qualified-FULL authorization. Deployed `CGS3` records migrate
with an in-progress rearm guard consumed. `CGS2` records recover maintenance
authority only from an intrinsically qualified terminal reason, while `CGS1`
records remain unqualified.

## Board IMEI provisioning

The warranty record's 15-byte serial field is the phone's board identity. On
erased flash it is blank. The first successful Telit initialization reads
`AT+CGSN`, accepts only a 15-digit Luhn-valid IMEI, and persists it through the
dedicated write-once API. Later boots skip that init query and both `*#06#` and
the `*#92702689#` Serial No. page read the persisted value; ordinary warranty
updates cannot replace it. Net Monitor may still query the module identity as
live diagnostic data, but that path cannot mutate the board identity.

Loading a malformed serial clears only the serial field and re-arms first-run
provisioning; Made, Repaired, Purchasing date, and flags are retained.

Firmware predating the calls-domain Life timer stored a donor value at offset
8 of the warranty payload and deferred its flash write until 30 accumulated
call minutes. On upgrade, the store seeds the new lifetime counter from the
largest of the new value, the legacy donor, and the surviving All-calls total.
The donor slot remains read-only for migration safety because the two records
cannot be committed atomically. Clear timers resets Last/All/Received/Dialled
only; it never resets the service Life timer.

The v6.00 `*#06#` presentation is preserved: PPM record index 337 (runtime SID
`0x18b`) is rendered by display record `0x1f` in FS0 over the full 84-pixel
window. The raw 15 digits hard-wrap at glyph boundaries (10 digits, then 5);
the warranty preview uses FS1, where all 15 digits fit its 78-pixel value field.

## SMS read/unread ownership

The 3210 ordinary SMS mailbox uses the SIM and reads the GSM status octet for its four
inbox icons (read 42 / unread 43 / sent 44 / unsent 45). On the Telit WWX,
`AT#SMSUCS=1` prevents `CMGL` and `CMGR` from changing `REC UNREAD` to
`REC READ`; this behavior was verified on the live module. The modem status is
therefore the sole read/unread authority, and no SMS status is persisted in RP
flash for ordinary SMS. Picture messages use the separate host-owned workflow.

- Metadata scans first issue `AT#SMSUCS=1`, then list/read the mailbox with
  status preservation enabled.
- An explicit user body-open issues `AT#SMSUCS=0`, reads the selected logical
  message, and restores `AT#SMSUCS=1` on success, error, timeout, or preemption.
  Only that selected message becomes read.
- A complete protected inbox scan replaces the RAM unread count directly from
  `REC UNREAD` rows. Incomplete or malformed scans cannot replace a known count.
- `+CMTI` increments the envelope immediately and leaves a status sync pending.
  The scan records the arrival counter at admission; if another `+CMTI` crosses
  the scan, its result is not published and a clean scan is retried.
- Compact row metadata and a content identity remain in RAM for lazy body reads.
  The identity rejects a read if a reusable modem slot changed between listing
  and selection; it is not a read-state database.

Row order (inbox): unread first, then newest-first — `strncmp` on the fixed-width
`YY/MM/DD,HH:MM:SS` timestamp is chronological, so a descending compare floats the
newest to the top within each read/unread group.

## Call Records

Call lists are capped at 20 records each, like the original phone behavior we
want to clone. Each record stores:

- number
- contact/display name
- duration in seconds
- date and time from `rtc_alarm_hal`
- call result/reason

The list API inserts newest records at index 0.

## Firmware EEPROM Semantics

The C firmware deliberately stores semantic records instead of a byte-for-byte
copy of the NSE-8 EEPROM table. The mapping follows the reverse-engineered
meaning of the important fields:

- T9 user dictionary mirrors the behavior traced around `0x0751/0x0752`: Spell
  and Insert word promote learned words to the front and persist them.
- Picture messages follow key `0x0757`'s shared template/saved-picture semantics,
  expanded from four to seven slots. The same record also holds two pending
  receptions. SMS transport encoding remains outside the storage layer; the
  picture store uses the pure decoder to validate/reassemble received content.
- Own tones mirror key `0x0748`: the composer saves a melody (ASCII notes + the
  packed Smart-Messaging byte stream) into slot 0, which is also played as the
  ringing tone when "Own tone" is selected.
- Call-divert numbers cover the visible runtime state traced in the simulator;
  firmware support fields `0x073e/0x073f` remain intentionally abstract because
  their semantic ownership is not established.

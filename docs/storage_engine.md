# Storage Engine

## Flash Layout and Transactions

Rev B2 reserves the last 448 KiB of internal flash for two littlefs volumes:

| Volume | Address range (inclusive) | Size | Current records |
| --- | --- | --- | --- |
| System | `0x10190000..0x1019ffff` | 64 KiB | Settings, board identity, battery and charge state |
| User | `0x101a0000..0x101fffff` | 384 KiB | Call lists, T9, pictures, tones, divert history; future contacts/SMS |

The build guard protects both regions. All sixteen existing persistent units
use littlefs. The old 128 KiB journal overlaps the upper third of the user
volume and is reclaimed only after migration establishes the new authority.
Phonebook contacts and ordinary SMS bodies remain modem-backed in this stage.

littlefs v2.11.3 is vendored with its BSD-3-Clause license and a documented
[local changes](../third_party/littlefs/README.sisu.md). Its adapter
uses static buffers, 4 KiB erase blocks, 256-byte physical program units,
256-byte system caches and 512-byte user caches, and the
same HAL core1/DMA park, IRQ masking, and watchdog leases as journal commits.
Programming stages each page in RAM; no caller's XIP buffer is read with XIP
disabled. Core0 is the sole filesystem owner; no filesystem calls run in IRQs.

`storage_backend.h` exposes opaque semantic records, not filenames or littlefs
types. Replacements write and close a temporary record before atomic rename.
Each record has a versioned ID/length/CRC envelope. Every mount completes pending
littlefs consistency work before exposing reads: an interrupted cross-pair
rename must not be mistaken for a missing record. The adapter remounts after
I/O failures before retrying. Each volume has independent caches and allocator
state; `STORAGE_RECORD_FULL` is distinct from a transient busy or I/O failure.
A full user volume cannot consume the system volume's space. Only wholly erased
initial format extents can be autoformatted;
a corrupt filesystem is never silently replaced with defaults. An interrupted
initial format that cannot mount requires explicit recovery, not autoformat.

The service-console command `storeinfo` reports each volume's allocated blocks,
user-category budgets and usage, and file-byte totals for the existing user
record groups. File bytes include envelopes; allocated bytes also include
metadata pairs and temporary files. Physical free space is not the amount an
individual category can consume.
`storetest confirm` writes only reserved scratch
record `fffe`, performs eight maximum-sized replacements and remount/readbacks,
and reports elapsed time, allocated blocks, and flash-park diagnostics.
It must not be used during ordinary phone use: it intentionally bypasses the
normal audio deferral to exercise the flash/DMA handshake.

Host tests interrupt each program/erase boundary before, halfway through, and
after the media change. They cover inline records, empty records, maximum-sized
records, grow/shrink replacements, cuts during recovery, and all sixteen legacy
units during import. Other cases cover full media, repeated remounts, allocation
churn, transient busy/error recovery, corruption handling, HAL bounds and
critical-section ordering. Split-volume tests also interrupt migration/growth,
verify that updates leave the other volume byte-identical, and fill both volumes
to test failed replacements, remounts, and recovery after deleting filler records.
Growth tests force a root relocation beyond the old extent, repeatedly remount
128-to-256, 128-to-384 and 256-to-384 KiB volumes, and inject torn writes and I/O
failures during growth. Mount discovery retains physical HAL bounds while
waiting for the authoritative superblock's size; all visited metadata pairs
must fit the final durable size before recovery can write anything.
The [physical power-cut run](storage_powercut_test.md#qualified-run) passed on
the stage-1 layout; that is not a controlled voltage-ramp brownout qualification
or a hardware qualification of the newer layout and category policy.

## User Space Budgets

The 384 KiB user volume has these starting budgets:

| Pool | Budget |
| --- | ---: |
| Contacts | 64 KiB |
| Inbox | 144 KiB |
| Outbox | 48 KiB |
| Incomplete-message staging | 32 KiB |
| Existing user records and shared filesystem overhead | 64 KiB |
| Recovery reserve | 32 KiB |

The first four pools own their directory metadata pairs and file data blocks.
The shared pool contains call lists, pictures, tones, T9 and divert history,
the durable object-ID allocator, root metadata and any unclassified filesystem
overhead. Its constituent records also have separate file-count/byte diagnostics;
they do not each have a separate physical quota.

After the system/user migration finishes, object initialization creates the
directories and enables admission checks for ordinary user-record writes too.
Usage is reconstructed from live filesystem blocks, deduplicated, and cached
until a filesystem mutation. There is no persisted counter/index to become
inconsistent with the files. The single core0 storage owner performs admission
and the complete write synchronously; callers do not reserve space separately
and later race another caller to use it.

An allocation guard admits every new block, including both halves of metadata
pairs. A normal write cannot borrow another pool's unused budget or the recovery
reserve. The guard bounds the entire allocation peak, including the temporary
copy, by the caller's remaining budget and physical headroom. Object writes
are also conservatively bounded by shared-pool headroom because littlefs may
update shared metadata during relocation. Thus a write may report FULL before
every byte of its nominal budget is occupied. No fixed message/contact count
is promised, and replacement at a completely filled quota is not guaranteed.

Deletion and abandoned-temporary cleanup may borrow up to 16 KiB of the recovery
reserve per operation, while preserving other pools' unused reservations. This
allows metadata relocation while reclaiming space; ordinary growth still cannot
use that reserve. littlefs consistency recovery completes before accounting is
rebuilt. Interrupted replacements leave either the previous or complete new
record, never a missing/partial record; an I/O error can be returned after the
commit reached media. Temporary files are removed on object-store recovery and are
never returned by collection scans.

Host coverage fills all four collections in sequence, checks every allocation
against its budget, then saves all seven existing user records. It verifies
readback and reuse after deletion, reconstructed usage after remount, the
512-byte inline boundary, and interrupted writes while the inbox is full.

### Message File Policy

The chosen policy is **one file per logical SMS**, including multipart SMS.
There will not be a separate file for each segment or a shared message database.
Incomplete reception belongs in one per-message staging file, atomically replaced
as parts arrive. The message codec, assembly and SMS/contact migration remain
the next stage; ordinary SMS/contact contents are still modem-backed today.

The user filesystem retains its 512-byte inline cutoff and 4 KiB erase blocks.
Small files share directory metadata blocks; 512 bytes is not a minimum file
allocation. A larger file requires out-of-line data blocks as well as metadata.
The storage envelope is 20 bytes, leaving 492 bytes of inline object payload.
The current object API accepts up to 4060 payload bytes per atomic file.

## Migration

An upgrade from the A/B journal first loads each legacy unit using its existing codec,
applies the existing defaults/schema migrations, and writes all sixteen units
to littlefs. Record `ffff` is a migration-complete marker, published last. A
reset before that marker repeats the import from the unchanged source. A valid
marker makes littlefs authoritative permanently: missing or damaged new data
does not resurrect stale charge latches, SOC anchors, or user settings from
the old journal. An unreadable/invalid marker fails initialization.

The partition split then copies the nine system units and the import marker
into the new system filesystem, verifies each payload, and publishes `SYS2`
in record `fff0`. Only then may the existing user filesystem grow from 128 to
384 KiB over the old journal. It removes the stage-1 bench records, publishes
`USR2` in its own `fff0`, and deletes the now-redundant system copies. Interrupted
migration resumes from these markers without recopying stale system state.
A grown user volume with missing system authority, or a missing/corrupt import
marker after authority was established, fails closed instead of importing from
the reclaimed journal. These are ordered per-file commits, not a cross-volume
atomic transaction.

### Moving from the 256 KiB user layout

This revision moves the volume bases; it is **not** an in-place size-only
firmware upgrade. Do not boot the new firmware over an unconverted old layout.
There is no automatic overlapping relocation in the handset firmware.

Back up and verify the old flash while the DUT is stopped in BOOTSEL. Assemble
the replacement storage image off-device: copy the old 64 KiB system volume
from `0x101b0000` to `0x10190000`, copy the complete old 256 KiB user volume from
`0x101c0000` to `0x101a0000`, and initialize its added 128 KiB extent to erased
bytes. Addresses overlap, so retain a complete verified off-device source;
do not copy forward directly on the live device. Program and read back the
complete converted layout and a firmware image that passes the new flash
budget guard before allowing a boot. Keep the backup until record readback
and growth to 96 blocks have been verified. Interrupted offline programming
must be retried in BOOTSEL, not resumed by booting a partly relocated image.

An older stage-1 filesystem likewise needs relocation from `0x101c0000` to
`0x101a0000` before the existing import/split protocol can run. Preserve its
legacy journal at `0x101e0000` until system authority commits. An unused range
may contain old firmware bytes; only explicitly provision new, unused extents
after taking a backup. A journal-only device needs the new system range and
initial user extent `0x101a0000..0x101c0000` erased, without erasing the journal.
Never erase a live filesystem merely because mount fails.

Do not boot older firmware after conversion. Its storage addresses no longer
describe the active volumes. The historical power-cut bench retains its old
`0x101c0000` address and must not be used after either partition migration.

The C firmware exposes a typed local NVM service backed by littlefs.
Phonebook and ordinary SMS contents remain modem-backed, but preferences, profile state,
clock/alarm preferences, speed dials, call-register lists, T9 learned words,
saved and pending picture messages, own-tone/composer drafts, call-divert editing
history, and per-pack battery health/SOC evidence are stored through this layer.

## Layers

- `nvm_hal`: raw media operations and geometry, with the RP2354 flash safety
  handshake implemented in `nvm_flash_hal.c`.
- `storage_lfs`: filesystem/block-device adapter and atomic opaque records.
- `storage_objects`: independent user-record files with durable IDs, CRCs,
  atomic replacement and directory iteration. It is a storage foundation;
  ordinary SMS/contact migration and multipart assembly are not implemented.
- `storage_user_space`: category usage and budgets, enforced inside the adapter;
  application code never handles littlefs block numbers or allocation callbacks.
- `storage_backend`: record read/write contract and board composition point.
  It contains no domain schemas. A future FRAM implementation can use this
  contract without exposing FRAM or filesystem calls to applications/codecs.
- `storage_partitions`: record affinity, migration authority, and independent
  system/user filesystem instances. Applications do not choose physical media.
- `storage_journal`: legacy reader used only during initial import. Keep this
  code until the RevC storage review; its flash allocation is no longer reserved.
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

The flash budget guard protects the combined 448 KiB reservation, leaving
1600 KiB for firmware. The unit-count guard
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

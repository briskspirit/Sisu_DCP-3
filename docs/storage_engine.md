# Storage Engine

## Flash Layout and Transactions

Rev B2 reserves the last 448 KiB of internal flash for two littlefs volumes:

| Volume | Address range (inclusive) | Size | Current records |
| --- | --- | --- | --- |
| System | `0x10190000..0x1019ffff` | 64 KiB | Settings, board identity, battery and charge state |
| User | `0x101a0000..0x101fffff` | 384 KiB | Call lists, T9, pictures, tones, divert history, contacts, SMS |

The build guard protects both regions. All seventeen persistent units
use littlefs. The old journal's flash allocation has been reclaimed. Its code
is retained for a possible FRAM backend, but is not linked into normal firmware.
Contacts and logical SMS are separate local files, independent of SIM or
carrier profile. The modem supplies transport only.

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
records, grow/shrink replacements, cuts during recovery, and initialization
of all seventeen default records. Other cases cover full media, repeated remounts, allocation
churn, transient busy/error recovery, corruption handling, HAL bounds and
critical-section ordering. Split-volume tests interrupt initial formatting,
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

### Integrated DUT Check (2026-09-19)

The 64/384 KiB build was programmed after an explicit erase of the storage
region. Both volumes formatted, all sixteen defaults initialized, and the
eight-replacement/remount console test passed in 711 ms with no flash-park or
resume timeout. Subsequent flashes and reboot checks did not erase storage.

The normal UI created a contact, resolved it in the inbox, read a received SMS,
and persisted its read flag. A successful send created a separate outbox file.
A three-part picture loopback reassembled and saved into the fifth picture
slot. A dialled call updated the call list, and an LCD calibration save updated
the system volume. A readback of both volumes validated all sixteen semantic
record envelopes and the contact/inbox/outbox objects. After reboot, these
records loaded again and the inbox was usable with the modem powered off.
With the modem still off, the UI deleted the sent copy and saved a new
recipient-less draft; occupancy reflected each change without touching the inbox.

The full host suite passed 133 tests with ASan/UBSan, including multipart SMS,
category-full isolation, and interrupted storage operations. Service and release
builds passed the flash and stack guards. Physical power cuts were not repeated
on this integrated layout. Ordinary multipart/Unicode SMS storage is covered by
host tests; additional external live reception was not qualified in this run.
The loopback also exposed a separate, pre-existing outgoing character-encoding
bug, subsequently fixed in the Telit adapter. See
[outgoing character encoding](sms_direct_delivery_design.md#outgoing-character-encoding).

## Startup Failure Handling

Both volumes are qualified independently before normal phone startup. A failed
mount, structural collection error, unreadable semantic record, or rejected
record payload latches a service fault. The bytes remain intact; unavailable
units cannot be committed over with their RAM defaults. Loading continues for
other units, so a user-volume fault cannot hide the system volume's charge-stop
latch. An unreadable charge-supervisor record keeps charging inhibited. Missing
records use defaults; an ordinary FULL result when saving them stays a pending
write, not a corruption diagnosis.

An individually malformed contact or SMS is excluded from the RAM index, not
deleted or rewritten. Healthy neighbours remain available. Corrupt pending SMS
are also excluded from expiry cleanup. Contact-binding pruning is suppressed
when damaged contacts exist. Filesystem/I/O errors are not treated as individual
bad records: an incomplete scan cannot masquerade as a healthy empty list.

System record `3220` holds four saturating 32-bit corruption-detection counters:
contacts, inbox, outbox and incomplete SMS. A given collection/object ID counts
once per boot, including repeated scans; a later boot detecting the same damage
counts another observation. This is not a count of unique lifetime failures.
Deduplication uses 10 KiB of bounded RAM. Exhausting that diagnostic bound also
requires service. Counters use the normal atomic record writer; NetMonitor 76
and `storeinfo` distinguish durable, pending, failed and unavailable evidence.
Power loss before a counter commit can lose that observation, but the preserved
file is detected again next boot.

On a power-on attempt with a latched fault, the UI takes the dedicated
`CONTACT SERVICE` startup route instead of starting the modem or showing the
success logo. It cannot be dismissed by normal keys or replaced by messages,
calls, alarms, or clock setup. Power-off and battery protection remain live.
Battery insertion or USB attachment alone does not turn on the screen.

The v6.00 ROM's failing self-test branch at `0x23491c..0x234924` arms timer
`0x19` with `0x80` ticks. We use the existing 8 ms UI timer model: a 1,024 ms
blank interval after the failed power-on gate, then the persistent screen.
The draw path at `0x237b62..0x237b76` uses the embedded FS4 font at `0x2e0a24`,
x=13 and baselines 15/32 (top rows 10/27 in our framebuffer). It draws uppercase
`CONTACT` and `SERVICE`, with no icon, softkey, status bars or dismissal timer.
These ROM literals are outside PPM; our string service exposes them centrally.
All 4,032 rendered pixels were compared with glyphs read directly from the ROM.
The host tests pin the delay boundary and uptime wrap. This reproduces the UI
timer model and startup phase, not the original hardware's self-test execution
time from battery insertion.

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

At startup, object initialization creates the
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

Deletion, fixed-size object-state updates, and abandoned-temporary cleanup may borrow up to 16 KiB of the recovery
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
as parts arrive. Direct modem reception queues normalized DELIVER segments into
the local service. Listing, reading, saving and deleting never send ME commands.
Successful network sends queue a separate local sent copy; failure to save that
copy cannot turn an accepted transmission into a retryable send failure.

The RAM index holds at most 500 inbox and 500 outbox entries (12 bytes each),
plus 64 pending identities. One shared UI metadata buffer holds 500 rows
(34,000 bytes); only the selected body is decoded. List requests borrow that
buffer until their exact completion token returns. The service fills it inside
the storage window, and an incomplete list is never published. Together with the
500-contact cache, these caches and codec scratch remain below the agreed
roughly 100 KiB allowance.

Files retain up to eight normal segments; conflicting fragments are preserved
in a quarantined file rather than silently overwritten. Plain bodies decode
into up to 2560 UTF-8 bytes. Compose/forward retains the current 160-byte draft
limit: a larger received body can be read but is explicitly refused by edit/send
instead of being truncated. Binary and quarantined content uses the translated
Data message label and remains visible/deletable. Recognized type-0, voicemail
and qualified OMA-DM controls are rejected before queue/storage admission, with
the same policy applied again after unambiguous multipart reassembly. Filtering
never guesses WDP transport provenance from payload bytes. Unknown service data
is not silently discarded. Picture receptions take their separate bounded record
path; complete pictures await user action, while abandoned partial slots are
reclaimed on later picture reception.

The eight-entry receive/sent-copy RAM queue retries BUSY, I/O and FULL outcomes.
FULL defers that entry for one minute; three I/O failures without successful
work defer the service for one minute, including reload, publication and expiry
failures. A successful index reload alone does not reset that retry bound.
Queued data remains in RAM. Quota-deferred entries do not block another category,
and neither backoff vetoes RAM-retaining standby. Deletes immediately rearm
queued writes; fresh queued work or a user request can interrupt I/O backoff.
Fresh work and transient BUSY results still block sleep. Deep power-off, including
EMPTY shutdown, may proceed with deferred entries after flushing system state;
it logs the number of RAM-only messages that cannot survive that boundary. This
deliberately avoids draining an empty battery while retrying unavailable storage.
A full outbox cannot block an incoming message behind it. Once staged, fragments
survive reboot. Incomplete SMS expire after seven days of local wall-clock time;
the 32 KiB staging budget and 64-entry bound still apply. The pending object's
attribute stores its first trusted local receipt time (seconds since 2000), not
the sender/SMSC timestamp. Extra parts and duplicates do not renew it. Old files
without a receipt time get a fresh window. An invalid/unset RTC pauses expiry;
a backward clock correction rebases affected files conservatively. A forward
clock adjustment counts toward age. Complete messages waiting for inbox space
never expire. One cleanup mutation runs per storage tick, through the same safe
write window and recovery reserve as user deletes. A full inbox cannot prevent
incomplete expiry or filtered-control cleanup. `storeinfo` reports clock validity
and boot-local expired/filtered counters. Queue overflow and malformed
deliveries are reported as receive losses. Because the modem acknowledges the
network before local commit, sudden power loss can still lose RAM-only arrivals;
this is not end-to-end exactly-once delivery.

Message bodies retain the encoded DELIVER segments in one versioned file.
Read/unread state uses a fixed four-byte littlefs attribute, atomically updated
without rewriting the body and preserved across body replacements. A completed
staging file is published under the same durable ID in the inbox before staging
is removed. Recovery verifies identical bodies when both copies survive a cut.
The RAM index is rebuilt from files; it is not another persisted transaction.
Host tests cover out-of-order eight-part reception, exact retransmissions,
conflicting segments, reboot persistence, torn stage/publish/cleanup writes,
and read/delete operations with the inbox quota full.

The user filesystem retains its 512-byte inline cutoff and 4 KiB erase blocks.
Small files share directory metadata blocks; 512 bytes is not a minimum file
allocation. A larger file requires out-of-line data blocks as well as metadata.
The storage envelope is 20 bytes, leaving 492 bytes of inline object payload.
The current object API accepts up to 4060 payload bytes per atomic file.

## Fresh Initialization

This revision deliberately does not import or relocate previous flash contents.
Before deploying from an older layout, stop the DUT in BOOTSEL and explicitly
erase `0x10190000..0x101fffff`, then program firmware that passes the 1600 KiB
build guard. This discards old settings and records. Do not boot a partially
programmed image or boot older firmware against the new layout.

The firmware formats wholly erased volumes, creates the user collections and
durable ID allocator, and writes defaults for missing semantic records. A reset
during default creation resumes the missing records without consulting old
journal bytes. Existing corrupt volumes are not erased automatically. Smaller
historical filesystem geometries are rejected rather than grown or imported.
There are no import markers or fallback reads from the old journal.

The historical stage-1 power-cut bench uses obsolete addresses and must not be
flashed onto this layout. Its retained results describe that earlier test only.

The C firmware exposes a typed local NVM service backed by littlefs.
Contacts and SMS are local objects. Preferences, profile state,
clock/alarm preferences, speed dials, call-register lists, T9 learned words,
saved and pending picture messages, own-tone/composer drafts, call-divert editing
history, and per-pack battery health/SOC evidence are stored through this layer.

## Layers

- `nvm_hal`: raw media operations and geometry, with the RP2354 flash safety
  handshake implemented in `nvm_flash_hal.c`.
- `storage_lfs`: filesystem/block-device adapter and atomic opaque records.
- `storage_objects`: independent user-record files with durable IDs, CRCs,
  atomic replacement and directory iteration. Contacts, inbox, outbox and
  incomplete SMS use this interface.
- `storage_user_space`: category usage and budgets, enforced inside the adapter;
  application code never handles littlefs block numbers or allocation callbacks.
- `storage_backend`: record read/write contract and board composition point.
  It contains no domain schemas. A future FRAM implementation can use this
  contract without exposing FRAM or filesystem calls to applications/codecs.
- `storage_partitions`: record affinity, fresh initialization, and independent
  system/user filesystem instances. Applications do not choose physical media.
- `storage_journal`: disconnected A/B implementation retained for the Rev C
  FRAM review, with standalone tests. It is not linked into normal firmware.
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
- storage health: corruption-detection counters for contacts and three SMS collections

The non-resettable Life timer lives in the settings/calls unit beside the four
ordinary duration counters. This matches v6.00's call-accounting ownership and
lets a completed call persist all five values in one record update.

The flash budget guard protects the combined 448 KiB reservation, leaving
1600 KiB for firmware. The unit-count guard
preserves the legacy ID/order contract and uses a 32-bit diagnostics mask. Small
records share filesystem metadata blocks instead of owning 8 KiB sector pairs.

Battery learning occupies unit 14 and the charge supervisor unit 15. The charge
stop latch is read before the first charger-enable output is driven after boot,
so a reset cannot silently restart a software-terminated charge. The 128-byte
`CGS4` payload persists maintenance rearm state, its automatic-rearm loop guard,
and qualified-FULL authorization. Obsolete call, picture, learner and charge
payloads are rejected; deployment starts from erased volumes, not legacy imports.

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

The Life timer belongs only to call accounting. Clear timers resets
Last/All/Received/Dialled, never the service Life timer. No warranty donor or
All-calls migration runs during initialization.

The v6.00 `*#06#` presentation is preserved: PPM record index 337 (runtime SID
`0x18b`) is rendered by display record `0x1f` in FS0 over the full 84-pixel
window. The raw 15 digits hard-wrap at glyph boundaries (10 digits, then 5);
the warranty preview uses FS1, where all 15 digits fit its 78-pixel value field.

## Contacts

Contacts live in the `contacts` collection, one atomic file per contact. The
versioned payload stores byte-counted name and number fields; the object
filename supplies a durable 32-bit ID. The ID allocator never reuses deleted
IDs. Speed dials and contact tones also store full-width IDs. A complete valid
scan prunes orphan bindings, including a deletion interrupted before its
settings cleanup. Corrupt or incomplete scans never prune bindings.

`phonebook_service` owns a 500-entry RAM cache rebuilt during boot. A bounded
request/result queue preserves UI operation tokens without involving the modem.
Results are published after the file commit. BUSY writes stay queued, and media
failures invalidate the cache before retry. Normal writes honor the shared audio
and key-activity defer window; outstanding writes block dormant entry. Names
resolve from this cache across Inbox, call screens and call lists even before
the modem starts or with no SIM inserted.

The 500-entry limit is a RAM bound, not a promise that every contact fits in the
64 KiB category budget. The Memory status page reports actual category headroom
in KiB and the number of saved contacts. A full category reports the localized
Memory full dialog; files remain intact and deletion can reclaim space.

Host tests cover add/update/delete at every media boundary, I/O failures,
BUSY retry, request backpressure, reboot persistence, quota exhaustion, malformed
records, and IDs above 65535. The old modem phonebook protocol is removed.

## SMS read/unread ownership

The local service owns read/unread state. Listing leaves attributes unchanged;
an explicit successful body read commits the selected message's read flag.
The boot scan rebuilds the unread count from durable attributes. Reception
increments the arrival counter only after a complete logical message has been
published, not for every transport fragment.

Inbox order is unread first, then newest timestamp and durable ID. Selection,
read, delete and completion tokens use durable IDs, never reusable modem slot
numbers. SIM removal, carrier-profile switches and modem reboots do not clear
mailboxes or their read state. The original read/unread/sent/unsent icons remain.

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

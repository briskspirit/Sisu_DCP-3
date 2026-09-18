# Storage Engine

The C firmware uses a Nokia-like local NVM service instead of a filesystem.
Phonebook and ordinary SMS contents remain modem-backed, but preferences, profile state,
clock/alarm preferences, speed dials, call-register lists, T9 learned words,
saved and pending picture messages, own-tone/composer drafts, call-divert editing
history, and per-pack battery health/SOC evidence are stored through this layer.

## Layers

- `nvm_hal`: raw byte backend. The backend is internal RP2354 flash. The HAL
  interface is generic, but this firmware standardizes on reserved internal
  flash.
- `storage_journal`: two-slot journal per storage unit. Writes go to the
  inactive slot, with CRC validation, so a failed write keeps the previous slot
  readable.
- `store_service.c`: journal orchestration, immutable unit registry, commit
  scheduling, failure isolation, and diagnostics. It owns the single shared
  payload buffer but no domain state.
- `store_settings.c` and `store_calls.c`: the keyed settings/default table and
  call-accounting records respectively. The call owner also contains the sole
  storage-side RTC dependency and the non-resettable Life timer policy.
- `store_t9.c`, `store_pictures.c`, `store_tones.c`, `store_divert.c`,
  `store_warranty.c`, `store_battery_learning.c`, and
  `store_battery_charge_supervisor.c`: one state owner and wire codec per
  semantic journal unit.
  All domains publish the same typed application API from `store_service.h` and
  register private reset/serialize/apply operations with the engine.

## Storage Units

Each unit owns two 4 KB journal slots:

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
lets a completed call persist all five values in one journal update.

The internal flash backend reserves 128 KiB at the end of flash. The flash
layout uses the complete region (16 units x two 4 KiB slots); no unassigned
journal pair remains. **Build-time guards protect the region:** a post-build check
(`cmake/check_flash_budget.cmake`) fails the build if code/rodata grows into
the storage region (last 128 KB), while `_Static_assert`s in `store_service.c`
pin the reviewed unit count, the 128 KiB requirement and ceiling, and the
16-bit diagnostic-mask capacity.

The removed SMS-status journal was unit 14. Removing it did not change the
index, unit ID, or flash offset of any older live setting, call log, or semantic
blob. Battery learning later reused that pair with a distinct payload magic, so
an upgraded phone rejects stale SMS-status bytes before its first learner write.
The charge supervisor occupies the formerly unassigned unit 15 pair. Its stop
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
`AT+CGSN`, accepts only a 15-digit Luhn-valid IMEI, and journals it through the
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
The donor slot remains read-only for migration safety because the two journals
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
  expanded from four to seven slots. The same journal also holds two pending
  receptions. SMS transport encoding remains outside the storage layer; the
  picture store uses the pure decoder to validate/reassemble received content.
- Own tones mirror key `0x0748`: the composer saves a melody (ASCII notes + the
  packed Smart-Messaging byte stream) into slot 0, which is also played as the
  ringing tone when "Own tone" is selected.
- Call-divert numbers cover the visible runtime state traced in the simulator;
  firmware support fields `0x073e/0x073f` remain intentionally abstract because
  their semantic ownership is not established.

# Phone Security

Phone security is the local handset security-code gate. It is separate from
SIM PIN/PUK and does not change modem provisioning or send SIM-security AT
commands. It is a UI access restriction, not encryption or protection against
physical flash access or the service console.

## Original Behavior

The 1999 Nokia 3210 user guide, printed pages 47, 48 and 54, describes a memory
of five SIM cards. With Phone security On, an unfamiliar card requires the
security code. The code is also required before entering the On/Off selector.
An unfamiliar card or a changed Phone security setting clears missed,
received and dialled call lists, not duration counters.

The v6.00 ROM refines those rules:

- `0x28f3f8` enters the common code challenge before the selector.
- `0x2df048` selects Off (`0x11`) or On (`0x39`).
- `0x27c434` recognizes any of five stored identities; `0x27c504` inserts a
  new identity at the front and shifts the previous four. Recognizing a
  stored identity does not promote it. This is a five-distinct-card FIFO,
  rather than an LRU cache.
- `0x28f484` uses SID `0x29a` for the prompt and `0x145` for Code error.
  Startup object `0x111ec0` and menu object `0x111ed8`, initialized at
  `0x2b6fa0`/`0x2b6fc0`, share window 50 and its FS2 text geometry.
- `0x2ae6b0` counts failures up to five and restarts timer `0x32` with
  `0x969a` ticks on each failed or blocked attempt. At the project's traced
  8 ms timer scale this is 308432 ms. A correct attempt before the fifth
  failure clears the counter; after five, even a correct attempt restarts
  the quiet period. `0x2ae584` restarts that period after boot when a failure
  count is present.

Source: [Nokia 3210 user guide](https://www.instructionsmanuals.com/sites/default/files/2019-05/Nokia-3210-en.pdf).
ROM addresses refer to the hash-qualified v6.00 build input documented in
[the reference index](nokia_3210_reference_index.md).

## Ownership And Persistence

`phone_security` owns admission, retry timing and one-use successful-code
grants. Apps own screens and input; `store_phone_security` owns the wire
record. The modem exposes its existing ICCID only after a clean command
final. SIM loss, refresh and modem-session invalidation revoke that identity.
No extra AT transaction is added to the startup queue.

Sisu deliberately uses the full verified ICCID to identify a physical SIM.
The ROM's protected identity encoding is not reproduced. ICCIDs are validated
as 19 or 20 decimal digits. Carrier names and serving PLMNs never establish
trust. An eSIM profile with a different ICCID is a different card to this
policy.

System record `0x3222` contains 113 payload bytes: a versioned header,
enabled flag, pending-call-clear flag, failed-attempt count, and five bounded
ICCID strings. It uses the existing CRC-protected atomic record backend.
The code remains in the existing system-settings record, with default
`12345`; newly chosen codes must contain exactly five digits. Previously
stored digit-only codes up to ten digits remain readable. The ROM master-code
fallback is not implemented.

Initial installation defaults to Off and an empty card history. The first
card seeds that history without deleting pre-existing call lists. There is
no import from the formerly unsupported menu's saved placeholder value.

Admission of a later unfamiliar card is recoverable across multiple files:

1. Persist the new identity together with a pending-call-clear flag.
2. Clear and commit all three recent-call lists.
3. Commit removal of the pending flag, then release the startup gate.

Boot resumes an interrupted clear. A pending or failed write cannot authorize
access. Confirmed storage degradation in a required unit enters the existing
Contact service path, rather than silently disabling security. A transient
commit failure remains retryable. Call-duration settings are not cleared.

Code changes, factory settings restore and phonebook Erase all use the same
retry counter as the Phone security prompt. A new SIM identity revokes
outstanding grants. Factory restore disables Phone security but preserves
the code and remembered identities; if this changes On to Off, recent call
lists are cleared as for the setting change.

## Runtime Gate

After the normal power-up display, an enabled handset waits for the verified
card identity. A recognized card proceeds; an unfamiliar card opens the
masked security-code prompt before clock setup or standby. The native prompt,
Code error, Done and Not done strings use the translation library.

Menus, delayed numeric shortcuts, incoming-call presentation, auto-answer,
headset answer and SMS/clock/interactive-alarm presentation cannot replace the
startup gate. Modem reception and local message storage continue underneath
it. Power-off and battery protection remain active. Emergency-call behavior
is not added by this feature and remains a separate qualification task.

Host tests cover five-card eviction, identity invalidation, shared retries,
timer wrap, reboot lockout, interrupted admission, corrupt records, masked
rendering and runtime interruption guards. Physical unfamiliar-SIM behavior
and startup rendering still require comparison on the handset; host coverage
does not substitute for that bench check.

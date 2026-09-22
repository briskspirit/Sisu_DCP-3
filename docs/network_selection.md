# Network Selection

Settings > Phone settings > Network selection controls the modem's actual
Automatic or Manual selection mode. The old scalar setting remains readable for
storage compatibility; it is not the authority for the menu preview.

## Ownership And Persistence

The modem service owns scheduling, cancellation, and SIM/session isolation.
`modem_network` owns the request state and scan list; the Telit adapter owns AT
commands and response parsing. Apps see only typed requests and snapshots.

The system settings store the confirmed choice as one atomic record: ICCID for
Automatic, or ICCID:PLMN for Manual. Startup reads the identity and live mode
without scanning, restores the same SIM's saved choice when necessary, and
verifies the result. A different or unbound SIM defaults to Automatic. Factory
settings clear this binding and trigger the same reconciliation. Storage failures
block selection changes; a new choice is saved only after matching readback.

Telit's guide says manual selection survives reboot and SIM changes. On the
bench, however, the WWX AT&T firmware returned Automatic after ordinary modem
shutdown/startup, before our first selection readback. The host therefore keeps
the confirmed choice instead of relying on modem persistence. A rejected startup
restore preserves that choice without retrying in a loop or silently selecting
Automatic; the menu still allows a different selection.

The UI never reports a successful selection from an `OK` alone: the following
numeric mode/PLMN readback must match. PLMNs retain all five or six digits.

## Lists And Display

Scans publish at most 24 distinct PLMNs. Available home networks come first,
then SIM user preferences (or the legacy SIM list), then operator preferences.
Unlisted networks retain scan order, with the current network first among them.
Names use the project PLMN database, then a printable scan name, then digits.
A truncated, malformed, overflowing, or transport-damaged response is rejected
as a whole. List generations prevent selection from a previous SIM or scan.

The Nokia v6.00 handlers at `0x289e7c`, `0x28a094`, and `0x28a054` provide the
manual list flow and progress records. Searching and Requesting use record
`0x25`; the populated list uses the existing three-row FS2 renderer. Empty,
denied, failed, and successful requests use original translated strings and
dialog records. Automatic returns to the parent menu.

The ROM list builder at `0x289c5c` puts the **home**, not merely current, PLMN
ahead of SIM preferences and remaining scan results. A verified manual selection
of home uses SID `0x18a`, record `0x1a` (`0x28a01c`); other selections use
Selected network (SID `0x3b9`, record 3). Both use the existing translated strings.

SIM metadata is read only after an explicit scan, never during startup or
background polling. Telit `#CIMI` supplies the IMSI prefix and READ BINARY of
EF-AD supplies the two/three-digit MNC length; neither display names nor an MCC
heuristic decide home identity. A nonempty EF-EHPLMN replaces IMSI home identity,
as on the tested AT&T SIM. Verified absent/empty EF-EHPLMN permits IMSI fallback;
an unreadable or malformed home list leaves home classification unknown.

GET RESPONSE and chunked READ BINARY read EF-EHPLMN (`6FD9`), EF-PLMNwAcT
(`6F60`), EF-OPLMNwAcT (`6F61`), and, when the user list is unavailable,
EF-PLMNsel (`6F30`). No CPOL/CPLS writes, SIM updates, or modem automatic-selection
policy changes are made. The UI orders distinct PLMNs, not separate radio-access
technology rows. It does not edit the preference lists.

Only ranks of scanned networks are retained in RAM. Each file is limited to
4095 bytes and read in at most 120-byte chunks. Classic SIM and USIM FCP
metadata are validated, including file identity, transparent structure, length,
BCD digits, and complete final responses. A failed file contributes no partial
priorities; other valid files can still be used. Missing optional metadata does
not fail a successful scan. Unverified home selections retain the ordinary
Selected network confirmation. Each new scan rereads the files, and session/SIM
invalidation retires the complete list and its home flags.

References: Nokia v6.00 addresses above; Telit ThreadX AT Guide r4, CPOL/CPLS and
CIMI/CRSM; [3GPP TS 31.102](https://www.etsi.org/deliver/etsi_TS/131100_131199/131102/18.07.00_60/ts_131102v180700p.pdf)
for SIM file layouts and [TS 23.122](https://www.etsi.org/deliver/etsi_TS/123100_123199/123122/18.07.01_60/ts_123122v180701p.pdf)
for equivalent-home identity and SIM preference ordering.

## Cancellation And Channel Safety

The scan deadline is 185 seconds, including a margin over the Telit Software
User Guide r17's 180-second test-command limit. Selection uses the same bounded
deadline. Short identity/readback commands have five-second deadlines.

Quit, incoming calls, shutdown, and SIM changes cancel a request's result. A transmitted
command retains ownership of the AT channel until its final is drained. An
unreleased channel enters the existing failed-modem recovery path rather than
dispatching another command into the unfinished response. No cancellation
cuts modem power. UI preemption detaches the request so late results cannot
replace a call, shutdown, or unrelated dialog.

ESC and bare CR both failed to interrupt a live WWX scan on the AT&T profile
(`M0F.103008`). There is no qualified abort sequence, so the adapter sends none.
Quit immediately leaves the progress
screen, but the scan can still occupy the AT channel until its original
deadline. A command timeout allows a final five-second drain before failure.
Calls get priority after that command ends; prompt call answering or shutdown
during a scan is not guaranteed. Direct SMS parsing remains active, but modem
radio availability during scanning is outside the host's control. Scans run
only on an explicit Manual selection, never as background polling.

## Regression Coverage

Host tests cover SIM ownership, durable-write failures, strict scan parsing,
verified selection, cancellation and timeout draining, missing-final failure,
SIM loss, call/shutdown preemption, UART errors, and late UI completion.
Original and synthetic asset suites exercise the translated UI paths.
SIM tests include two/three-digit MNC distinction, equivalent-home replacement,
user/operator priority and legacy fallback, duplicate entries, multi-chunk
files, malformed/truncated/oversized files, transport loss, and cancellation or
SIM loss during metadata reads. The full service harness checks incoming-call
preemption while a SIM read still owns its final response.

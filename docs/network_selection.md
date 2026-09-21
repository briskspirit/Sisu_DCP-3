# Network Selection

Settings > Phone settings > Network selection controls the modem's actual
Automatic or Manual selection mode. The old scalar setting remains readable for
storage compatibility; it is not the authority for the menu preview.

## Ownership And Persistence

The modem service owns scheduling, cancellation, and SIM/session isolation.
`modem_network` owns the request state and scan list; the Telit adapter owns AT
commands and response parsing. Apps see only typed requests and snapshots.

Telit's manual selection survives a SIM change. To match the Nokia policy, the
system settings store the ICCID that owns the selection. Startup reads that
identity and the live mode without scanning. A different or unbound SIM returns
to Automatic, verified by readback, before its identity is saved. The same SIM
keeps its live selection. Factory settings clear this binding and trigger the
same reconciliation. Storage failures block selection changes.

The UI never reports a successful selection from an `OK` alone: the following
numeric mode/PLMN readback must match. PLMNs retain all five or six digits.

## Lists And Display

Scans publish at most 24 distinct PLMNs, with the current network first.
Names use the project PLMN database, then a printable scan name, then digits.
A truncated, malformed, overflowing, or transport-damaged response is rejected
as a whole. List generations prevent selection from a previous SIM or scan.

The Nokia v6.00 handlers at `0x289e7c`, `0x28a094`, and `0x28a054` provide the
manual list flow and progress records. Searching and Requesting use record
`0x25`; the populated list uses the existing three-row FS2 renderer. Empty,
denied, failed, and successful requests use original translated strings and
dialog records. Automatic returns to the parent menu.

Two original refinements are not reproduced: SIM-preferred ordering after the
current network, and the special Home network selected confirmation. Without
verified home-PLMN identity, success uses Selected network with the actual
network name, not a guessed home/roaming classification.

## Cancellation And Channel Safety

The scan deadline is 185 seconds, including a margin over the Telit Software
User Guide r17's 180-second test-command limit. Selection uses the same bounded
deadline. Short identity/readback commands have five-second deadlines.

Quit, incoming calls, shutdown, and SIM changes cancel a request. A transmitted
command retains ownership of the AT channel until its final is drained. An
unreleased channel enters the existing failed-modem recovery path rather than
dispatching another command into the unfinished response. No cancellation
cuts modem power. UI preemption detaches the request so late results cannot
replace a call, shutdown, or unrelated dialog.

[BP] The adapter sends CR to abort a scan/selection. ESC did not release the
live module's scan. Cancellation retains the original command deadline, with
at least five seconds to drain a final: an ignored abort must not declare a
healthy, unfinished scan a failed modem. The CR behavior still needs live
qualification. Asynchronous host processing does not guarantee radio service
during a modem network scan.

## Regression Coverage

Host tests cover SIM ownership, durable-write failures, strict scan parsing,
verified selection, cancellation and timeout draining, missing-final failure,
SIM loss, call/shutdown preemption, UART errors, and late UI completion.
Original and synthetic asset suites exercise the translated UI paths.

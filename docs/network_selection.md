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

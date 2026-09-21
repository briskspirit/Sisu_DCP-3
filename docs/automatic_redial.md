# Automatic Redial

Settings > Call settings > Automatic redial now controls outgoing voice-call
retries. Off remains the default. With On selected, an ordinary single-call
attempt that ends with an explicit Busy or No answer result can be retried up
to ten times. Each retry uses the existing dial command builder and caller-ID
preference. The call register keeps one entry for the whole sequence.

## Original Behavior

The Nokia 3210 v6.00 redial state handler is at `0x002ac022`, state-table entry
`0x16`. The [1999 Nokia user guide, page 50](https://ringtones.specialtyansweringservice.net/wp-content/uploads/2014/08/manuals/nokia-3210-userguide.pdf)
also describes a maximum of ten attempts after an unsuccessful call.

The ordinary ROM schedule at `0x002ac102` uses timer `0x30`, with `0x202`
eight-millisecond ticks per beat:

| Additional Attempt | Wait After Previous Failure |
| --- | ---: |
| 1 | 8.224 seconds (2 beats) |
| 2-4 | 65.792 seconds (16 beats) |
| 5-10 | 189.152 seconds (46 beats) |

The waiting screen uses translated SID `0x005c` (Redialling), windows 70/71,
and animation 2: the solid 80x4 bitmap 165. Handler `0x002b0010` supplies an
8-bit progress fraction; renderer `0x002304c8` scales the bitmap width by 255.
The layout follows the existing handset-qualified framed progress surface.
The stock softkey action is blank; hard C cancels. The retry cue at
`0x002ac296` selects system tone 11. Its short earpiece stream precedes the
ordinary call-progress tone and respects the profile's warning-tone setting.

These timings and resource identities are ROM-derived, not new side-by-side
timing measurements on an original handset.

## Modem Boundary

The original cause classifier, event `0x138c` and expressions `0x02c3..0x02dd`,
has more information than the current modem-neutral call-result contract.
Causes 17, 18 and 19 correspond to Busy and No answer. Other ROM classes cover
network failures and a separate one-retry policy for certain permanent errors.

This implementation does not guess those classes from Telit's generic
`NO CARRIER`. That result can describe several different failures, including
rejection, and retains the normal failure dialog without automatic retries.
Likewise, short codes (fewer than five digits), service strings, incoming calls,
and second-call setup are never automatic retry targets. Emergency-call work
is separate and no emergency test calls are made.

Unlike Nokia's additional no-network wait loop, loss of registration ends this
sequence. A new manual call is required after recovery. Any observed connection,
even before the UI's connection-confirmation delay, disarms retries.

## Cancellation and Ownership

Waiting is nonblocking: modem polling, incoming SMS and call indications keep
running. C or Power cancels the wait without sending a hangup command. Another
UI surface, SIM/registration change, incoming caller or uncertain call table
also ends the sequence. Returning from a dialog cannot re-arm it.

An automatic dial needs a fully idle call model with no pending incoming
episode, live leg, unresolved transaction or cleanup obligation. The service
checks this both at admission and immediately before UART transmission,
including after a DTR/CTS wake delay. The queued request also retains its
registration and incoming-episode generations, preventing a recovered network
or an intervening incoming call from reviving stale work. Pre-dispatch C
cancellation names the retry token, not the whole call session.

The app owns timing, rendering and the finite retry count. The call model owns
call/transaction identity. The modem service owns queue and transport checks;
the vendor adapter still owns AT command formatting. No new persistent schema,
modem provisioning setting, storage backend or startup network query is needed.

## Verification

- `test_call_waiting_handoff` drives the real app runtime through success,
  cancellation, incoming preemption, queue rejection, ambiguous failures,
  provisional results, all ten attempts and millisecond counter wrap.
- `test_call_model_dispatch_guard` checks idle eligibility and unresolved-call
  exclusion, including pre-ID incoming calls and abandoned outgoing setup.
- `test_modem_telit_service` checks queued and wake-deferred retries, caller-ID
  snapshots, incoming SMS, SIM/registration loss, shutdown, and token cancellation.
- `test_call_redial_render` checks the real renderer across compiled languages,
  with original bitmap geometry when original assets are available.
- `test_clock_alarm_app` checks that a redial wait cannot suppress an alarm,
  while actual outgoing setup and connected calls retain their audio ownership.

Live carrier qualification must establish which explicit terminal result a
particular carrier returns for busy and unanswered calls. A generic rejection
or a successful voicemail connection is not evidence of either retry case.

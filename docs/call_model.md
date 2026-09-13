# CLCC Call Model

Status: implemented, host-tested, and bench-qualified on Rev B2. This document
is the behavioral contract for `modem_call_model` and preserves the section
numbers cited by its source and regression tests.

## 1. Scope

The model owns call identity and call-control intent. It consumes normalized
call events, `RING`/`CLIP` observations, typed `+CLCC` rows, command
outcomes, and time. It publishes the legacy flat call fields consumed by the
application.

It does not parse AT text, write UART bytes, touch hardware, allocate memory, or
depend on a concrete modem backend.

## 2. Core Invariants

- A confirmed leg is identified by `{id, generation}`, never by list position.
- A local request is an intention until network evidence proves its
  postcondition.
- An id-bearing release event may remove one leg immediately.
- A bare final result never identifies or removes a leg.
- One clean CLCC snapshot may add or update legs, but absence requires stronger
  confirmation.
- Every transient obligation either resolves or reaches a bounded cleanup
  deadline.
- Projection runs once per service tick from the model state; it does not infer
  new model state from previously published flat fields.

## 3. Integration

`modem_service` owns the model instance and calls it from one core. The modem
backend normalizes vendor events and CLCC rows before they cross this boundary.
The service preserves each transaction token with its queued command and
reports dispatch, final result, timeout, enqueue failure, or eviction back to
the model. The saved model projection is the sole source for public flat call
status and audio-bridge decisions. The legacy-vs-model migration comparator and
its runtime publication switch were retired after Rev B2 bench qualification.
This is distinct from the CLCC shadow in Section 5.2: that isolated reconcile
snapshot remains a required atomicity mechanism inside the production model.

The application still uses `call_projection_t` as its rendering compatibility
contract. A narrow read-only `modem_call_snapshot_t` also exposes live
generation-qualified leg identity for transitions that cannot be resolved
safely from flat field deltas, currently the waiting-caller handoff after a
foreground release. It is published atomically with the flat projection and
contains only neutral model types. The terminal journal and a complete
per-leg/event view remain internal model details.

## 4. Three-Bucket State

### 4.1 Normalized types

Raw direction, state, and mode numbers stop at the modem backend. The model
stores only `call_direction_t`, `call_leg_state_t`, `call_mode_t`, and the
neutral call operation enum from `call_types.h`.

### 4.2 Local transactions

`call_txn_t` records one local operation, its wrap-safe token, participant
references, baseline leg set, postcondition evidence, deadlines, command
outcome, and cancellation state. Transactions represent intention; they are not
legs.

### 4.3 Pre-id incoming observation

`call_pending_mt_t` holds a `RING`/`CLIP` episode before a trustworthy call
id exists. CLIP supplies number and presentation data but is not id-bearing.

### 4.4 Confirmed leg table

`call_leg_t` stores id, generation, state, direction, mode, multiparty flag,
number, timestamps, and the last published role. The fixed table covers the
complete 3GPP call-id domain 1 through 7.

## 5. Authority And Reconciliation

### 5.1 Id-bearing events

A trustworthy id-bearing event may add or update that leg immediately. An
id-scoped RELEASED event is positive removal evidence for only that
`{id, generation}`. A coarse event with `id_valid=false` never mutates a leg;
it schedules CLCC instead.

### 5.2 CLCC shadow

`call_model_clcc_begin()` opens an isolated shadow. Rows populate the shadow;
the live table is untouched until a clean `call_model_clcc_ok()`. ERROR,
overflow, a changed transport-drop counter, or an interleaved observation
rejects the complete shadow.

### 5.3 Asymmetric removal

A clean shadow is applied in this order:

1. Reject the shadow if transport integrity or revision checks fail.
2. Add and update rows immediately.
3. On the first clean omission of a live leg, mark that exact generation
   `pending_removal` and retain its previously published role.
4. Remove omitted legs only when a second clean snapshot against the same model
   revision reproduces the same absent set. Additions and confirmed removals
   commit atomically.

The atomic commit prevents a false IDLE frame when one leg disappears in the
same observation that reveals its replacement. A pending-removal leg also has a
bounded limbo deadline so transport silence cannot retain it forever.

### 5.4 Observation invalidation

Every call-affecting mutation passes through the same semantic-revision and
shadow-invalidation path. An event arriving during CLCC wins; the older shadow
is discarded and reconciliation is rescheduled.

### 5.5 Bare finals

`BUSY`, `NO CARRIER`, `NO ANSWER`, and similar bare finals are provisional
causes. They may be attributed to exactly one eligible transaction, but they
never remove a leg. CLCC or an id-scoped release determines structure.

### 5.6 Pending incoming absence

A pre-id incoming episode is not cleared by one clean CLCC with no compatible
incoming leg. Two consecutive clean snapshots for the same episode are
required, with the ring timeout as the final bounded backstop.

## 6. Scheduling And Bounded Drain

`reconcile_reasons` records coarse finals, unbindable events, uncertain
transactions, pending removals, overflow, and pending incoming observations.
Urgent evidence pulls the next CLCC deadline to the current tick. Clean success
resets backoff; errors apply a capped exponential backoff.

`call_model_wants_clcc()` becomes false only when the model has no live leg,
pending incoming episode, open or tombstoned transaction, pending removal,
queued release, or unresolved reconciliation reason. A transient-only model is
required to drain to that empty state.

## 7. Transaction Lifecycle

### 7.1 States and deadlines

Transactions move through PENDING, DISPATCHED, BOUND, UNCERTAIN, SUCCEEDED,
FAILED, or CANCELLED. The policy deadline requests stronger evidence. The hard
abandon deadline releases a bound participant or confirms cleanup without
leaving a permanent slot.

### 7.2 Postconditions

Success is operation-specific:

- DIAL: a new compatible outgoing leg appears and progresses.
- ANSWER or WAIT_ANSWER: the captured incoming participant becomes active.
- HOLD: the captured active participant becomes held.
- SWAP: the captured active/held roles exchange.
- WAIT_REJECT or RELEASE_LEG: the captured participant disappears.
- RELEASE_ACTIVE: the captured active participant disappears and any held
  survivor may become active.
- HANGUP: every captured participant intended by the operation disappears.

An AT `OK` acknowledges command acceptance; it does not replace the network
postcondition.

### 7.3 Capacity and reclamation

Admission is atomic with queueing at the integration boundary. A request
rejected before dispatch is reclaimed immediately. Terminal records retire only
after both their user-visible result and cleanup obligations are resolved.

### 7.4 Binding

DIAL binds only to a compatible leg not present in its admission baseline.
Targeted operations bind to generation-qualified participants captured at
admission or dispatch. A direction-unknown tentative DIAL may keep the UI in
DIALING, but it cannot displace a confirmed foreground leg until direction is
known.

## 8. Generation Semantics

Each call id has an independent next-generation counter. Removing id 3,
reusing its table slot for id 4, and then observing id 3 again produces a new
generation for id 3. Stale references, queued releases, and transactions cannot
bind to the reused id.

Generation cannot recover two complete unseen lifetimes of the same id between
all observations. The model records this transport-information limit rather
than pretending otherwise.

## 9. Total Projection

Projection is retention, consistency gate, row map, then publish:

1. Pending-removal legs retain their own last published role.
2. Impossible multisets, including more than one effective ACTIVE or HELD leg,
   hold the previous projection and request reconciliation.
3. A stable multiset maps through the rows below.
4. The result and each leg's published role commit together.

There is no flat HELD call state. A sole held leg publishes ACTIVE plus
`call_on_hold=true`.

### 9.1 Stable meaning

`call_on_hold` means the foreground call itself is held because no active leg
exists. `second_call_held` means a distinct held leg exists alongside the
foreground active leg. A waiting leg never changes `call_on_hold` while an
active leg exists.

### 9.2 Stable rows

| Row | Model state | Projection |
|---|---|---|
| R1 | empty | IDLE; id 0; all flags clear |
| R2 | pending incoming only | RINGING; ring true; waiting false |
| R3 | outgoing setup only | DIALING; id 0 |
| R4 | answer setup only | ANSWERING; id 0 |
| R5 | foreground leg plus confirmed second outgoing setup | DIALING; foreground id retained |
| R6 | one ACTIVE leg | ACTIVE; that id; hold flags clear |
| R7 | one HELD leg | ACTIVE; held id; call_on_hold true |
| R8 | one ACTIVE plus one HELD | ACTIVE; active id; second_call_held true |
| R9 | ACTIVE plus incoming/waiting | ACTIVE; active id; waiting and ring true; call_on_hold false |
| R10 | sole HELD plus incoming/waiting | ACTIVE; held id; waiting, ring, and call_on_hold true |

A three-leg ACTIVE/HELD/WAITING set is R9 with
`second_call_held=true` and `call_on_hold=false`.

### 9.3 Promotion

When an active leg is positively released and a held leg survives, the model
promotes the survivor to ACTIVE. This is structural evidence, not an inference
from a changed flat active id.

### 9.4 Incoming binding

A compatible incoming or waiting leg binds the current pre-id episode. Number
and presentation data follow that generation. Repeated RING updates the same
episode; a genuinely new episode receives a new episode generation.

### 9.5 Result latches

`last_call_result` and `second_call_result` are explicit model-owned
latches. The projector carries or drains them; it does not derive them from
which legs happen to survive. A fresh ring or dial clears only the latch it
owns. Second-call failure cannot overwrite the primary result.

## 10. Capacity And Overflow

The leg table is sized to all seven valid call ids and the transaction pool has
12 slots. Invalid ids and overflow are explicit reconciliation conditions.
Nothing is silently dropped or written out of bounds.

## 11. Terminal Journal

Every actual departed leg can create a bounded terminal record containing id,
generation, direction, answer history, result, number, token, and second-call
ownership. The current flat projector drains this journal into its two result
latches once per tick. The current application does not expose persistent
per-leg acknowledgement semantics.

## 12. Verification Contract

Host tests cover the public API under ASan/UBSan and include:

- all R1-R10 projector rows and impossible-state retention
- three-leg ACTIVE/HELD/WAITING with `call_on_hold=false`
- atomic remove-old/add-new reconciliation without false IDLE
- interleaved event invalidation of an open CLCC shadow
- generation reuse across an intervening different id
- no-leg command failure and provisional bare-final ownership
- every operation across target-id zero, pre-id incoming, timeout, error, and
  cancellation points
- tombstone and pending-removal bounded drain
- stale dispatch suppression after a queued command's participant changes

Tests must drive mutations through the public model API unless a projector-only
test is intentionally constructing a stable input multiset.

## 13. Bench Qualification

The production integration has been exercised with outgoing and incoming calls,
remote and local release, hold/unhold, call waiting, answer/reject, swap,
release-this, release-all, held-leg promotion, and call-audio resynchronization.
The Telit timing profile currently uses 4 s keepalive, 300 ms confirmation,
8 s leg limbo, 40 s transaction policy, and 120 s hard abandon.

## 14. Naming And Ownership

The call model is the only owner of confirmed call topology. The modem service
may cache the published flat projection for application compatibility, but it
must not rebuild a second optimistic topology beside the table.

## 15. Application API Boundary

`modem_call_snapshot_t` is a deliberately small identity-sensitive API. It lets
code follow one live `{id, generation}` (with the pre-id incoming episode as a
binding aid) without copying topology ownership into the application. It is not
a second projector and does not expose mutable model state.

The flat projection remains the rendering compatibility contract and must stay
behaviorally stable. Exposing the complete per-leg snapshot or acknowledged
terminal events would require a separately reviewed public contract; the model
does not promise either today.

## 16. Transaction And Cancellation Details

### 16.1 Tokens

Every admitted operation receives a nonzero, collision-avoided 32-bit token.
The token follows the request through queue, dispatch, final result, timeout,
eviction, and terminal attribution.

### 16.2 Command result

`call_model_txn_command_result()` accepts one atomic outcome:

- `CMD_OK`: command accepted; wait for the operation postcondition
- `CMD_ERROR`: terminate with the supplied normalized cause or the operation's
  default; output-free supplementary operations do not write call-result latches
- `CMD_TIMEOUT`: mark UNCERTAIN and request immediate CLCC

### 16.3 No-leg failure

A bare failure owned by one setup transaction remains provisional until clean
CLCC proves that no compatible leg exists. It then resolves that transaction's
proper result latch without inventing or evicting a leg.

### 16.4 Latch ownership

Terminal causes belong to a transaction or a departed leg generation. A foreign
leg cannot steal or clear another live setup transaction's provisional cause.
Remote primary departure defaults to NO_CARRIER when no local teardown owns it.

### 16.5 Participant descriptors and dispatch guard

Each operation defines target resolution, postcondition, bindability, result
sink, and deadline behavior. Immediately before writing AT bytes, the service
calls `call_model_txn_dispatch_guard()`. It returns SEND,
ALREADY_SATISFIED, or STALE, preventing an old queued answer/reject/hangup from
affecting a later call.

### 16.6 Incoming episode generation

Pre-id operations capture the current incoming episode generation. An operation
admitted for episode N cannot bind episode N+1, even when both have no call id
at admission.

### 16.7 Cancellation

`call_model_txn_cancel()` decides from immutable `ever_dispatched`.
Pre-dispatch cancellation reclaims the transaction. Post-dispatch cancellation
leaves a bindable, output-free tombstone because the command may already have
changed network state.

### 16.8 Centralized mutation

Every leg, pending-incoming, transaction, deadline, and cancellation mutation
uses the same shadow-invalidation path. This makes CLCC interleave safety a
structural property rather than a list of event-specific exceptions.

### 16.9 Retirement

Output resolution and cleanup confirmation are independent axes. A transaction
retires only when both are satisfied. A tombstone never produces a user-visible
success or failure after cancellation.

### 16.10 Backoff

Urgent new evidence may shorten a scheduled reconcile deadline. Repeated noise
must not restart limbo or extend hard-abandon deadlines.

### 16.11 Bounded cleanup

Pending removals, orphaned incoming episodes, uncertain operations, hard
abandons, and tombstones all have explicit terminal paths. No dropped command,
missing final, or silent network can consume a model slot forever.

### 16.12 Vendor-neutral boundary

`call_types.h` contains the complete shared contract and no hardware or vendor
dependency. The selected backend supplies a `call_timing_t` value at
initialization. AT command timeout remains transport-owned; the model consumes
only the reported `CMD_TIMEOUT`.

### 16.13 Probe matrix

Regression work crosses every operation with representative topology,
id-bearing versus pre-id evidence, command OK/error/timeout, and cancellation
before versus after dispatch. Public-API probes are the authority; tests that
hand-edit internals cannot claim transaction-path coverage.

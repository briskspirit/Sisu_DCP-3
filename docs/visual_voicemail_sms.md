# Visual voicemail control SMS

## Scope

Visual voicemail (VVM) signaling is an application protocol carried over
port-addressed SMS. It is not an LTE radio feature and must not be inferred
from LTE registration, the sender alone, or a destination port alone.

The phone does not implement a VVM data/IMAP client. It uses the traditional
voice-mailbox number and a vendor-neutral message-waiting model populated from
Telit `#MWI`. Voice line 1/2, fax, e-mail, and other are retained independently;
voice drives mailbox dialing/the standby tape icon, while fax and e-mail drive
their original v6.00 passive icons and notices. VVM provisioning credentials
and mailbox metadata therefore have no consumer and are intentionally not
retained.

References:

- [GSMA OMTP Visual Voicemail Interface Specification](https://www.gsma.com/newsroom/wp-content/uploads/2012/07/OMTP_VVM_Specification_1_3.pdf)
- [Android visual voicemail integration](https://source.android.com/docs/core/permissions/voicemail)
- [AOSP VisualVoicemailSmsFilter](https://android.googlesource.com/platform/prebuilts/fullsdk/sources/+/refs/heads/androidx-constraintlayout-release/android-35/com/android/internal/telephony/VisualVoicemailSmsFilter.java)
- [AOSP VisualVoicemailSmsParser](https://android.googlesource.com/platform/prebuilts/fullsdk/sources/+/refs/heads/androidx-constraintlayout-release/android-35/com/android/internal/telephony/VisualVoicemailSmsParser.java)

## Classification

Classification is strict and fail-open. A row is consumed only when it has an
application-port UDH and one of these payloads parses completely:

| Family | Required shape |
| --- | --- |
| OMTP | `//...VVM:STATUS:` with `st` or `rc` |
| OMTP | `//...VVM:SYNC:` with `ev` |
| Legacy VVM | `STATE?` with `state=` on live-qualified port `0x1578` or `0x157B` |
| Legacy VVM | `MBOXUPDATE?` with `m=` on live-qualified port `0x1578` or `0x157B` |
| Legacy VVM | `UNRECOGNIZED?`/`UNRECOGNISED?` with `cmd=` on those ports |
| Legacy URL VVM | `host:port?f=...&v=...&m=...&p=...&s=...&t=...` on application port `0x157B` (`5499`); `p=` may be empty |

OMTP application ports are client/carrier configured, so valid OMTP syntax is
accepted on any destination port. The legacy dialect has no local carrier
configuration source; the event dialect is restricted to its observed VVM
ports and the URL dialect to the legacy VVM endpoint `5499`. URL notification
classification validates the complete protocol envelope and does not inspect
the originating number, operator, MCC/MNC, or server domain. This keeps it
usable with different operators without embedding a carrier database.

Unknown ports, malformed controls, ordinary binary SMS, and unsupported
application protocols remain visible as the Nokia `Data message` and retain the
normal user notification. Port matching by itself is never sufficient.

The URL recognizer is grounded in a 2026-08-16 live capture of seven retained
records on port `5499`. They used three different server hosts but the same
protocol envelope. Captured mailbox credentials are not stored in the source
tree; tests use synthetic operators, hosts, mailbox IDs, and tokens.

## Receive transaction

Complete single-part direct deliveries are now recognized before `CMGW` and
consumed without using an ME slot or generating a user arrival. The filter
preserves unsupported UDH and message-waiting/class-specific codings; see
[direct-delivery controls](sms_direct_delivery_design.md#controls-before-storage).
The reconciliation path below remains for already-stored controls and
multipart deliveries.

1. `+CMTI` increments the raw mailbox revision and records its storage index.
2. The app requests its existing protected Inbox reconciliation. No tone,
   unread bump, or backlight notification is emitted yet.
3. A complete `AT+CMGL=4` snapshot classifies the decoded row for each pending
   index. A `+CMTI` crossing the scan invalidates publication and forces the
   existing retry path.
4. Recognized VVM controls are omitted from the public mailbox cache. After
   text mode is restored, their exact indexes are deleted with `AT+CMGD`.
5. Only user-visible rows advance the classified-arrival counter that drives
   the Nokia notification path.

A definitive delete error or uncertain timeout does not expose or alert on a
recognized control. Cleanup stops and a later Inbox scan retries the still
stored row. The mailbox snapshot itself remains usable. The pending CPMS
backstop runs only after the operation finishes, so receive-store occupancy is
sampled after successful cleanup.

## Message-waiting authority and latency

The tape icon follows explicit voice `#MWI` set/clear indications. Fax and
e-mail indications follow their own categories: a category clear cannot erase
another category, while an unqualified clear erases the complete snapshot.
An inactive-to-active edge or an increased count queues the matching v6.00
standby notice, with fax selected before e-mail when both are pending. These
notices are silent and use the original `Exit` action. Dismissing the notice
does not clear its status icon; only authoritative network state does that.
Ending a mailbox call is not evidence that every message was heard or deleted,
so the phone must not clear the icon optimistically on hang-up. The carrier
voicemail platform may take time to propagate the clear to the mobile network. A live
AT&T bench observation on 2026-08-17 saw the icon clear about 3-5 minutes after
deletion, but CDC was not attached during that interval, so the arrival time of
the actual clear URC was not captured. There is no firmware delay timer in this
path: once a clear URC reaches the modem service, the standby UI applies it on
the same runtime update.

LE910C1-WWX firmware `M0F.103008` was also observed returning the two-field
line `#MWI: 1,1` to `AT#MWI?` after the icon had cleared. That line is
ambiguous: in read syntax it is an enabled profile plus a set status missing
its required indicator, while in URC syntax it means set/voice. A solicited
readback with that shape is non-authoritative and preserves the last explicit
voice state. Three-field rows can overlap too: `#MWI: 1,1,<1..5>` is either an
active read row with an omitted count or a voice URC whose count happens to be
a category number. It is likewise non-authoritative in a solicited read, so both
the voice category and the query-side category named by that third field are
retained unless a clean row in the same snapshot resolves one of them. Other
clean fax/e-mail rows still commit. Ambiguous rows must never be re-routed as
URCs or used to resurrect an icon.

## Hardware Regression Procedure

1. Leave a voicemail for the DUT.
2. Confirm the tape icon follows `#MWI` as before.
3. Confirm no message tone/dialog is emitted for VVM control SMS.
4. Open Inbox and confirm no new `Data message` rows were created.
5. Send an ordinary text SMS and confirm immediate notification, unread state,
   persistence, and normal Inbox reading.
6. Listen to/delete the voicemail and confirm the tape icon clears without a
   user-visible VVM control message.

# SIM Number Lists

Phone book's **Service Nos.** reads EF_SDN (`6F49`, with EXT3). **Info numbers**
reads the CPHS information-number tree (`6F19`, or legacy `EA01` if absent),
subject to the CPHS service table and CSP `D5/FF` permission. Neither list uses
the local contacts store or changes the modem's phonebook/SMS selectors.

## Original Menu Visibility

Both rows are conditional in Nokia 3210 v6.00, despite existing in the static
Phone book descriptor at `002D0990`. Its mask at `0011225C` is a **big-endian
16-bit word**:

- `002AD894` calls the service-number availability helper `0026EF68`. Failure
  clears `02` in the mask's **second byte**, hiding item 2, Service Nos.
- `002AE840..002AE852` tests runtime byte `0011FCFF == 2`. Otherwise it clears
  `02` in the mask's **first byte**, hiding item 10, Info numbers. The Info
  numbers opener at `002A6E14` checks the same byte.

Earlier research incorrectly attributed the first-byte clear to Service Nos.
Static descriptor presence alone does not prove visibility. These gates express
SIM/runtime availability; they do not establish that every allocated record is
populated. Hidden rows retain their original menu ordinals.

## Implementation

After ordinary modem startup work, a bounded, read-only capability probe checks
the two lists. Rows remain hidden until probing finishes, and only supported,
active files with nonzero record capacity are exposed. CPHS also requires its
service and CSP permissions. Missing data hides the corresponding row;
malformed/failed capability reads get at most three attempts per modem/SIM
session. SIM loss or lock clears the gates and list cache immediately.

Opening a visible row loads its complete list into a bounded RAM cache. Empty
records are skipped; an entirely empty list returns to Phone book and hides
that row. Nested Info folders, inherited premium/home-only flags, GSM/UCS2 names,
and number extensions are decoded before publishing anything. Home-only entries
are hidden while roaming or registration is unknown. Dialling uses the normal
call transaction, not an app-owned AT command.

Limits are 64 populated entries, 120 bytes per SIM record, and 32 dial characters
including a possible `+`. Unsupported bearer/dial-control encodings, malformed
records, extension loops and overflow fail without partial or truncated numbers.
The Telit adapter owns CRSM and SIM-alpha decoding; apps see only typed entries.

Calls and queued SMS work take priority between reads. CRSM is not abortable:
cancellation retains ownership of its final response and its original deadline.
A read timeout gets a further 5-second response-drain limit; an unreleased
channel is marked failed, not reused. The complete operation is bounded to
120 seconds, with a 3-second deadline per command.

## Qualification And References

Host tests cover decoding and capacity boundaries, menu visibility/shortcuts,
SIM invalidation, cancellation, timeouts, call pre-emption and direct SMS
interleaving. [BP] No available test SIM provides these lists, so live SIM-file
access remains unqualified. Host-rendered screens use the original assets but
are not a substitute for a side-by-side handset comparison with a suitable SIM.
The adapter uses Telit's selected SIM application and readable-file metadata;
it does not infer SST versus UST layout from the card's response envelope.

Record formats follow [3GPP TS 51.011](https://www.etsi.org/deliver/etsi_ts/151000_151099/151011/04.07.00_60/ts_151011v040700p.pdf),
[TS 31.102](https://www.etsi.org/deliver/etsi_ts/131100_131199/131102/18.04.00_60/ts_131102v180400p.pdf)
and [CPHS 4.2](https://support.nowsms.com/discus/messages/1/Cphs4_2_generic-69969.pdf).
Info-number extensions use standard EF_EXT1 `6F4A`; CPHS 4.2 prints `6F3E` beside
the EXT1 name, but that identifier is EF_GID1 in TS 51.011.
UICC metadata follows [ETSI TS 102 221](https://www.etsi.org/deliver/etsi_ts/102200_102299/102221/16.02.00_60/ts_102221v160200p.pdf);
both `05` and `07` lifecycle values denote an activated file.

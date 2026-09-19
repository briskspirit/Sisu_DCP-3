# SMS direct delivery (unified receive route)

Current architecture: direct modem delivery, local littlefs messages.
The September 16 implementation plan describes the superseded ME re-store path.
See [Storage engine](storage_engine.md) for file layout, quotas and durability.

## Problem

With a Verizon SIM (US Mobile "Warp", IMSI 311480, Telit image
`#FWSWITCH: 1`) the LE910C1-WWX receives SMS over IMS in 3GPP2 (C.S0015)
form. In the shipped store mode (`AT+CNMI=1,1,0,0,0`) the Qualcomm stack files
those messages in its CDMA store and announces them with the non-standard
`$QCMTI: "ME",<n>` URC. Bench evidence 2026-09-16:

- The CDMA store is not readable on this image in any mode: in 3GPP mode
  (`#SMSFORMAT=0`) `+CMGR=<n>` returns `OK` with no data; in 3GPP2 mode
  (`#SMSFORMAT=1`) `+CMGR` returns `+CMS ERROR: none` (code 0) and `+CMGL`
  returns `+CMS ERROR: memory failure`. `+CPMS` is "operation not supported"
  in 3GPP2 mode. This is the "error 0 / error 500" the owner saw.
- With direct delivery (`AT+CNMI=2,2,0,0,0`) every Verizon message arrives as a
  `+CMT` URC, in whichever form matches the current `+CMGF`. With `+CSDH=1`
  the text form carries the fields needed to rebuild the message.
- AT&T (3GPP) messages already work in store mode and their `+CMTI` path is
  bench-proven, including binary/picture SMS.

That finding motivated direct delivery. The local message pipeline retains
normalized 3GPP TS 23.040 DELIVER segments without writing them back to the modem.

## Decision

One receive route for every carrier: direct delivery, classify complete
controls, route recognized pictures to their local record, and queue ordinary
SMS for littlefs. [Picture messages](picture_messages.md) describes the separate
pending-reception/gallery workflow.

```
modem +CMT header/body
    -> generic raw collector + vendor translation
    -> normalized SMS-DELIVER
    -> complete control filter
    -> picture port 0x158A: local picture record / View / Save
    -> other messages: message_service RAM queue
        -> atomic per-message staging file
        -> complete-message validation / VVM filter
        -> atomic inbox file / local unread attribute / arrival notification
```

The modem owns transport and network acceptance. `message_service` owns
durable inbox/outbox files, reassembly, IDs and the RAM index. UI operations
use asynchronous typed requests and never issue storage AT commands.
`phonebook_service` owns contacts independently of the installed SIM.

### Resting mode stays TEXT (`AT+CMGF=1`)

The Telit 3GPP2 PDU form strips the user data header: the long message
(WEMT teleservice 4101) arrived as bare packed septets, so concatenation
would be lost. The text form with `+CSDH=1` carries the UDH as hex (bench
sample `0500036202019473...`, `<length>` 159 septets). Per 3GPP 27.005 and
the Telit guide (`+CMGL`/`+CMGR` `<data>`), text mode renders the user data
as hex whenever the UDH bit is set or the DCS is 8-bit/UCS2; a plain GSM-7
body is delivered as the GSM-charset characters themselves (`+CSCS="GSM"`).

The text form is lossless only if the body is read exactly. The firmware's
line path cannot carry a plain body: the line framer drops CR, splits on LF,
ends the C string at 0x00 (the GSM code for `@`) and `process_line` trims
spaces, so a body with a line break, a leading/trailing space or an `@`
would be split into fragments (a fragment reading `OK` could even complete
an active command). The collector therefore reads the body **raw, by
`<length>`** (the RAW-mode rule):

- a `+CMT:` header with a positive final length field `L` enters RAW mode.
  Its units depend on the format and encoding. The first `L` bytes are
  always data, including CR, LF, NUL and spaces. The UART drain bypasses
  the line framer while the collector owns the body;
- each subsequent line break is a terminator candidate. The vendor hook
  runs first, followed by the generic parsers for unrecognized headers.
  ACCEPTED completes the body; REJECTED consumes that terminator and ends
  the invalid delivery immediately, leaving the next header or command
  line intact. Only an explicit INCOMPLETE keeps the CR/LF as body data;
- Telit ASCII/IA5 text lengths count packed octets. A valid prefix whose
  packed size is too small returns INCOMPLETE; invalid bytes, unsupported
  formats and oversized bodies return REJECTED. The character-count
  alternative is not accepted;
- two consecutive character counts can share one packed length. If an
  otherwise accepted body can also include a bare LF, that LF is preserved
  as data until the module's CRLF terminator. This preserves trailing
  newlines too. Bare-LF-only termination is accepted only when it cannot
  be mistaken for another body character; the ambiguous case needs CRLF;
- after a candidate line break becomes data, accumulation is bounded to
  `ceil(8*L/7) + 1` bytes for the supported plain-body formats. Hex bodies
  contain no CR/LF, so they finish at their first candidate without this
  bound. The overall cap remains 400 bytes. The first byte exceeding a
  bound is returned to the normal line/prompt parser, not discarded;
- a 5-second body-idle deadline resets an interrupted delivery and records
  an error. A new `+CMT:` header can only arrive through the line framer,
  so RAW mode cannot nest;
- the parsers receive an explicit byte range, never a C string. Each
  parser validates the length in its encoding's units before storage;
- a header whose last field is not a number (the `+CSDH=0` text form) keeps
  the two-line behaviour: the next framed line is the body.

Therefore:

- init keeps `AT+CMGF=1`, adds `AT+CSDH=1`, and replaces
  `AT+CNMI=1,1,0,0,0` with `AT+CNMI=2,2,0,0,0` (mode 2 buffers URCs while
  the link is reserved and flushes them afterwards);
- production picture sending stays in text mode with temporary 8-bit/UDHI
  CSMP settings, then restores normal text settings. Local file operations do
  not change modem modes;
- a `+CMT` that lands inside a diagnostic binary-send PDU window is parsed in its PDU form
  (3GPP: standard SMS-DELIVER, pass-through; 3GPP2: Telit PDU, UDH already
  stripped by the module). Lost headers cannot be reconstructed reliably;
  this remains a transport limitation, not a lossless fallback.

### Acknowledgement

The module acknowledges the network on receipt. Host acknowledgement through
`+CSMS=1`/`+CNMA` is not enabled. Local durability starts when a staged file
commits, not when the network is acknowledged. The RAM queue retries storage
failures but cannot preserve an arrival through power loss before its first
commit. Queue overflow is explicitly counted; exactly-once delivery is not
claimed.

### Controls before storage

The collector classifies only a completed delivery, never a speculative
CR/LF boundary. A `FILTERED` result consumes the body without entering the
local storage queue, advancing mailbox/user-arrival counters or
recording a command error. This also works when the queue or filesystem category is full.
The normal module-managed network acknowledgement is unchanged.

The initial filter is deliberately small:

- Type-0 SMS (`TP-PID=0x40`) is discarded as specified by 3GPP TS 23.040.
  This is not Class-0 flash SMS.
- Complete VVM controls use the existing strict port-and-payload recognizer.
  Multipart controls are recognized only after complete local reassembly;
  individual fragments remain durable until then.
- OMA-DM notifications require a complete single-datagram WAP Push on port
  2948, compact WSP headers `06 03 C4 AF 87` after the transaction ID,
  a 16-byte digest, notification version 11, server initiation, zero reserved
  bits, a nonempty printable server identifier and no vendor-specific body.
  Both 3GPP UDH ports and Telit octet-encoded WAP teleservice 4100 are accepted.
  The latter carries WDP metadata separately through translation; arbitrary
  binary payloads cannot acquire WDP status by resembling its header.
  The accepted octet codings are classless `0x04` and class-1 `0x15`/`0xF5`.
  Verizon reprovisioning notifications were observed with `0xF5`; they still
  require every port, WSP and Package-0 validation above. This does not widen
  the separate VVM filter's coding policy.

Unknown WSP encodings, extra headers, multipart packets, malformed/duplicate
UDH and trailing data stay on the normal path. Outside Type-0, filtering
requires PID 0 and DCS 0, 4 or 8, with the class-1 exception for OMA-DM above.
In particular, it does not swallow MWI,
Class-0/2 messages, SIM downloads, MMS or LwM2M application notifications.
There is no sender-number, server-name or carrier blacklist.

This is an unsupported-host-service policy, not an OMA-DM client: no digest
authentication, management session or successful-update response is implied.
Telit's internal management clients and provisioning are not changed.
OMA-DM is filtered only with the original complete transport metadata. Unknown
or multipart management payloads are kept, not inferred from a sender blacklist.

Three saturating RAM counters (`sms_filtered type0/vvm/oma_dm` in `status`)
provide diagnostics without retaining payloads or writing persistent logs.

References:
- [3GPP TS 23.040, Type-0 SMS](https://www.etsi.org/deliver/etsi_ts/123000_123099/123040/16.00.00_60/ts_123040v160000p.pdf)
- [3GPP TS 23.038, SMS data coding schemes](https://www.etsi.org/deliver/etsi_ts/123000_123099/123038/18.00.00_60/ts_123038v180000p.pdf)
- [OMA-DM Notification 1.2.1, sections 6 and 7](https://www.openmobilealliance.org/release/DM/V1_2_1-20080617-A/OMA-TS-DM_Notification-V1_2_1-20080617-A.pdf)
- [OMA Push application IDs](https://oma-knowledge-base.openmobilealliance.org/omna/wag/push_application_id.html)

### Sleep and power: unchanged

DTR/RI/CFUN provisioning and wake qualification are unchanged. Uncommitted
local writes block dormant entry and run through the shared flash/DMA safety
window. Completed staging files and unread messages do not themselves keep the
CPU awake. USB-connected bench tests do not qualify disconnected standby power;
the historical measurements in `docs/standby_power.md` are not a measurement
of this storage revision.

### Layering

- `sms_deliver_codec.[ch]` (services, pure): `sms_deliver_t` and the
  SMS-DELIVER TPDU builder. No modem knowledge.
- `modem_sms_direct.[ch]` (services, pure): the two-line `+CMT` collector and
  the generic 3GPP text/PDU parsers. Takes the vendor translate hook as a
  function argument, so the generic code never names a vendor.
- `modem_vendor.h`: one new optional member
  `modem_sms_direct_translate_fn translate_direct_sms`. `modem_vendor_none`
  sets it to NULL.
- `modem_vendor_telit_sms.c` (vendor): the 3GPP2 text and PDU forms of this
  module, mapped to `sms_deliver_t`. The only file that knows teleservice
  ids or CDMA encodings. `$QCMTI` lives in the Telit aux-URC table
  (`modem_vendor_telit.c` / `telit_parse_aux_urc`), mapped to the neutral
  `MODEM_AUX_EVENT_MESSAGE_STORED_UNREADABLE` event.
- `modem_service.c`: owns collection, normalization and control/picture
  routing, then submits ordinary arrivals to `message_service`. It never
  chooses filesystem paths or serializes persistent records.
- `message_file_codec`: file encoding, concatenation checks, lazy decoding
  and complete-message control recognition, without modem AT knowledge.
- `message_service`: local message lifecycle and asynchronous UI requests.

### Format mapping (3GPP2 -> SMS-DELIVER)

The module also emits the ten-field 3GPP text form. A live capture on
2026-09-19 reported 51 characters while delivering 53 GSM bytes: each brace
used an escape byte. The vendor adapter counts GSM extension pairs as one
character and normalizes the length before the generic parser packs the body.
The raw collector permits up to 160 septets, preserving embedded CR/LF and
GSM NUL (`@`); malformed escapes and non-GSM bytes are rejected. Hex/UDH
deliveries retain their separate length rules.

Header, text form (`+CSDH=1`):
`+CMT: "<orig>","<callback>","<YYYYMMDDHHMMSS>",<tooa>,<tele_id>,<priority>,<enc>,<length>`

Header, PDU form: `+CMT: "<orig>","<callback>",<len>` followed by
`<addr_len_bytes><toa><bcd digits><YYMMDDhhmmss><tele_id:2><priority><enc><data_len><data>`.

| enc | meaning | text-form body | mapping |
|---|---|---|---|
| 9 | GSM 7-bit | hex (UDH + packed septets when tele_id 4101; packed septets otherwise); `<length>` in septets | DCS 0x00, UD copied, UDL checked against packed data as below, UDHI = (tele_id == 4101) |
| 4 | Unicode | hex UTF-16BE, `<length>` in code units | DCS 0x08, UD copied, UDL = 2 x length |
| 0 | octet | hex | DCS 0x04, UD copied, UDL = octets |
| 8 | Latin-1 | plain text (module applied `+CSCS="GSM"`), `<length>` = characters | GSM-7 packed from the received codes, DCS 0x00; PDU form bytes mapped Latin-1 -> GSM, UCS2 fallback when a byte has no GSM mapping |
| 2, 3 | 7-bit ASCII / IA5 (IS-637) | plain text (module applied `+CSCS="GSM"`), `<length>` = **packed octets** ceil(7*chars/8), the only accepted relation | text form: GSM-7 packed from the received codes, DCS 0x00; PDU form: `<data_len>` = **characters**, data = exactly ceil(7*N/8) octets packed 7 bits per char MSB-first (bit 7 of octet 0 is bit 6 of char 0), unpacked then ASCII -> GSM with the UCS2 fallback; octet/character mismatch rejected |
| any | tele_id 4099 / 262144 (voice mail notification) | - | rejected with a log; MWI arrives via `#MWI` already |

A GSM-7 text-form body is treated as hex only when its character count
equals `2 * ceil(7 * length / 8)`; a plain-text body has exactly `length`
characters, and the two never coincide for length > 0. Bench 2026-09-16: the
module's text-form `<length>` is off by one on some WEMT parts (21 given for
20 octets = 22 septets), so a GSM-7 hex body's TP-UDL is selected from
{`<length>`, +1, -1} to match its packed size; otherwise the part is rejected.
UCS2/8-bit and plain bodies stay exact.

Packed size alone cannot distinguish 159 from 160 septets: both occupy
140 octets ([3GPP TS 23.038, section 6.1.2.1](https://www.etsi.org/deliver/etsi_ts/123000_123099/123038/18.00.00_60/ts_123038v180000p.pdf)).
The evening bench test exposed a WEMT first part reporting 159 while its
last seven bits contained the 153rd text character, `b`. Choosing 159 lost
that character at the multipart join. For native WEMT text only, a candidate
with seven spare bits is increased by one when those bits contain a nonzero,
non-CR septet. The adjustment stays within one septet of the reported length.
Zero padding and the CR padding observed in the final part keep the reported
count; a real extra `@` or CR is indistinguishable from padding in this
malformed-length case. Generic 3GPP and native PDU-form lengths are unchanged.

Timestamp: `YYYYMMDDHHMMSS` (local time as delivered) -> TP-SCTS semi-octets,
time zone 0. Address: digits with TOA from `<tooa>` (129 -> 0x81,
145 -> 0x91). Callback number is ignored.

### Format mapping (3GPP, generic)

- PDU form `+CMT: [<alpha>],<length>` + hex: validated with
  `sms_pdu_decode`, stored verbatim (`<length>` is the TPDU length).
- Text form (`+CSDH=1`):
  `+CMT: <oa>,<alpha>,<scts>,<tooa>,<fo>,<pid>,<dcs>,<sca>,<tosca>,<length>`;
  body is hex when `<fo>` has UDHI (0x40) or the DCS is 8-bit/UCS2, else
  GSM-charset text. `<scts>` `yy/MM/dd,hh:mm:ss+zz` keeps its zone.

### Failure handling

Malformed or unsupported direct deliveries increment receive-loss diagnostics;
a lost body is never replaced with a fabricated message. Empty bodies finish at
the header, and bounded raw collection cannot consume the next unrelated final
or URC indefinitely.

The local queue holds eight arrivals/sent copies. BUSY, I/O and FULL retain the
queued item for retry; a full category rotates behind other queued categories.
A ninth arrival is rejected and reported. No bounded queue can promise delivery
under indefinitely full media. Durable staging survives reboot, and a complete
message waits there if the inbox is full. Deletion remains possible.

Publication uses the same durable ID, commits the inbox body first, then removes
staging. Recovery verifies byte equality before removing a duplicate staging
copy. Delete removes staging first to prevent resurrection. Conflicting fragments
are quarantined, never silently spliced or overwritten. See the storage tests
for interruption at each program/erase boundary.

Unexpected stored-message indications (`+CMTI` or Telit's `$QCMTI`) are
reported as receive losses: the production path has no ME fallback. Network-send
results remain separate from local sent-copy persistence; a confirmed `+CMGS`
must not become a false transmission failure because the outbox is full.

## Evidence (bench, 2026-09-16, Verizon SIM, FW M0F.103008)

Text form, `+CSDH=1`, `+CSCS="GSM"`:

```
+CMT: "7866910488","","20260916105524",129,4098,0,8,9
Dhdjdjdjs
+CMT: "7866910488","","20260916105547",129,4101,0,9,159
0500036202019473B57AAE9ECFD573F75C3D57CF416434594D5693D5...   (UDH + packed GSM-7)
+CMT: "7866910488","","20260916105553",129,4101,0,9,23
050003620202D46435599D9EABE7EAB99AAC26ABC9
+CMT: "7866910488","","20260916105559",129,4098,0,4,2
D83EDD2A
```

PDU form (`+CMGF=0`):

```
+CMT: "7866910488","",25
068187661940882609161059281002000807446A73736A6A73        ("Djssjjs", enc 8)
+CMT: "7866910488","",22
068187661940882609161127491002000404D83DDE1C              (emoji, enc 4)
+CMT: "7866910488","",32
06818766194088260916112747100500090F6A721A4D4693D56435594D469301  (WEMT part, enc 9, no UDH)
```

Store mode (`+CNMI=1,1`): `$QCMTI: "ME",24` ... `$QCMTI: "ME",29`, rows
unreadable in every mode.

## Shared AT Channel

Startup and re-registration use SIM-stored `#CFF?` flags on every carrier;
absent flags leave forwarding status unknown. Explicit Call divert requests
still use `+CCFC`, with a 60-second budget for the observed slow replies.
Incoming indications and local SMS storage continue while waiting, but a call
answer or outgoing SMS must wait for that non-abortable query on the shared
AT channel.

## Historical Bench Scope

September 16 direct-delivery tests verified Verizon and AT&T short text,
line breaks, GSM NUL (`@`), multipart payloads and idle arrival. They used the
former ME re-store implementation. Its measured per-message AT activity and
320-byte multipart display limit no longer describe this local-storage revision.
The raw transport evidence above remains relevant; current local durability,
multipart capacity and quotas are covered in `storage_engine.md`.

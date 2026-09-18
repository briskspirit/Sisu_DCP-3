# SMS direct delivery (unified receive route)

Status: DESIGN LOCKED 2026-09-16, implementation plan in
`docs/superpowers/plans/2026-09-16-sms-direct-delivery.md`.

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

The firmware's inbox pipeline (mailbox scan, multipart grouping, VVM filter,
quarantine, UI) is built on 3GPP TS 23.040 PDUs read from the ME store.

## Decision

One receive route for every carrier: direct delivery, classify complete
controls, route recognized pictures to local flash, then re-store ordinary user
messages in ME. [Picture messages](picture_messages.md) describes the separate
pending-reception/gallery workflow and its transport limits.

```
modem  --+CMT header/payload-->  modem_sms_direct (generic collector)
                                     |  vendor translate hook (3GPP2 -> sms_deliver_t)
                                     |  generic 3GPP text/PDU parsers
                                     v
                              sms_deliver_codec (SMS-DELIVER TPDU builder)
                                     v
                       sms_control_filter -> known controls consumed in RAM
                                     v (keep)
                       picture port 0x158A -> host picture journal / View / Save
                                     v (other messages)
                       STORE_DELIVERED protocol op: AT+CMGF=0, AT+CMGW=<len>,0,
                       <pdu>^Z, AT+CMGF=1  ->  +CMGW: <idx>
                                     v
                     same bookkeeping as +CMTI (pending arrival, counters)
                                     v
                     existing mailbox scan / multipart / VVM / UI unchanged
```

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
  CSMP settings, then restores normal text settings. Existing ME save/read/scan
  operations still use transient PDU mode;
- a `+CMT` that lands inside a transient PDU window is parsed in its PDU form
  (3GPP: standard SMS-DELIVER, pass-through; 3GPP2: Telit PDU, UDH already
  stripped by the module). Lost headers cannot be reconstructed reliably;
  this remains a transport limitation, not a lossless fallback.

### Acknowledgement

`+CSMS=0` stays: the module acknowledges the network on receipt. 27.005
`+CSMS=1`/`+CNMA` was rejected for v1 because a missed acknowledgement makes
the module fall back to `<mt>=0` (indications off) until CNMI is re-executed,
a larger availability risk than the host-side window between `+CMT` and
`+CMGW`. The window is covered by a bounded retry ring (below).

### Controls before storage

The collector classifies only a completed delivery, never a speculative
CR/LF boundary. A `FILTERED` result consumes the body without entering the
storage ring, issuing `CMGW`, advancing mailbox/user-arrival counters or
recording a command error. This also works when the ring or ME is full.
The normal module-managed network acknowledgement is unchanged.

The initial filter is deliberately small:

- Type-0 SMS (`TP-PID=0x40`) is discarded as specified by 3GPP TS 23.040.
  This is not Class-0 flash SMS.
- Complete VVM controls use the existing strict port-and-payload recognizer.
  Multipart controls retain their existing store/reconciliation path.
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
Existing stored OMA-DM rows are not automatically deleted because their
translated TPDU no longer contains the original WAP teleservice metadata.

Three saturating RAM counters (`sms_filtered type0/vvm/oma_dm` in `status`)
provide diagnostics without retaining payloads or writing persistent logs.

References:
- [3GPP TS 23.040, Type-0 SMS](https://www.etsi.org/deliver/etsi_ts/123000_123099/123040/16.00.00_60/ts_123040v160000p.pdf)
- [3GPP TS 23.038, SMS data coding schemes](https://www.etsi.org/deliver/etsi_ts/123000_123099/123038/18.00.00_60/ts_123038v180000p.pdf)
- [OMA-DM Notification 1.2.1, sections 6 and 7](https://www.openmobilealliance.org/release/DM/V1_2_1-20080617-A/OMA-TS-DM_Notification-V1_2_1-20080617-A.pdf)
- [OMA Push application IDs](https://oma-knowledge-base.openmobilealliance.org/omna/wag/push_application_id.html)

### Sleep and power: unchanged

No DTR/RI/CFUN behaviour changes. CNMI mode 2 buffers the URC while the host
sleeps and flushes it on the next DTR wake, exactly like `+CMTI` today (the
bench already showed `+CMT` lines arriving after DTR sleep). Bench gate:
standby arrival latency and standby power with the Verizon SIM must match the
current figures (`docs/standby_power.md`).

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
- `modem_service.c`: routes `+CMT` to the collector, runs the
  `STORE_DELIVERED` operation, and applies the `+CMTI`-equivalent
  bookkeeping. The unreadable-store aux event is counted as a command
  error with a warning that names no URC (message not retrievable).

### Format mapping (3GPP2 -> SMS-DELIVER)

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

- A `+CMT` that no parser accepts is logged at warning level with the header
  and counted in `command_errors`; the module already acknowledged the
  network, so the message is lost. This is the same class of loss as a
  corrupt stored row today (quarantine) and is expected to be rare.
- A `+CMT` header whose `<length>` is 0 completes on the header line: the
  line framer never delivers an empty body line, so the collector runs the
  parse chain with an empty payload at once instead of pending on (and
  eating) the next unrelated line.
- The body of a header with `<length>` > 0 is read raw by the RAW-mode rule
  (see "Resting mode stays TEXT"): bytes bypass the framer until the first
  CRLF at or after `<length>` bytes; a body that reaches the 400-byte cap
  without a terminator is rejected and the following bytes flow through
  the framer again. A framer-DROPPED (overlong) line while a header is
  pending in line mode resets the collector, so the next line is not taken
  as the body.
- A store that ends UNCERTAIN (the `+CMGW` final timed out, or the session
  was cancelled after the body was submitted) is retried like a failed
  store, so a row that did land is stored twice. A duplicate is preferred
  over a lost message; the inbox scan shows it as two identical rows.
- A cancelled SMS operation (any kind) that had issued `AT+CMGF=0` without
  a completed `AT+CMGF=1` leaves an idle-scheduler "text-mode restore"
  pending: `AT+CMGF=1` is sent under the usual idle gate before the ring
  drain, cleared on OK and retried on ERROR/timeout up to 3 times.
- `$QCMTI` (the module filed a message in its unreadable CDMA store; only
  when direct delivery is not in effect) lives in the Telit aux-URC table
  and reaches the service as the neutral "message stored unreadable" event:
  counted in `command_errors` and `urc_count` with a warning, never read.
- The built PDU is held in an eight-entry ring in `modem_service` until
  `+CMGW` succeeds; entries are stored in arrival order (FIFO by sequence
  number, so a slot freed and refilled never overtakes older entries). A
  failed or cancelled store (call preemption, SIM not ready, CPMS error)
  re-queues the entry up to 3 times, then drops it with a warning, except
  when the `+CMGW` final says the store is full (`+CMS ERROR: 322`, "memory
  full" / "memory failure"): that outcome (`MODEM_SMS_OUTCOME_STORAGE_FULL`)
  burns no attempt; the ring is blocked (logged once) until a DELETE
  operation completes OK, a `+CPMS?` poll reports used < total for the
  receive store, or 60 s elapse, whichever first, then the store is
  retried. A ninth delivery while eight are held is dropped with a warning.
- A queue-full condition never drops a message: the ring is drained from the
  idle scheduler when the request queue has room.

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

## Out of scope / follow-ups

- `+CSMS=1` / `+CNMA` host acknowledgement (bench experiment first).
- The deferred-SMS-setup retry loop hammers the module when it is left in
  `#SMSFORMAT=1` (CSMP/CPMS unsupported there). The firmware never enters that
  mode; noted for the audit log.

Call-forwarding refresh no longer starts network queries in the background,
on any carrier. Startup and re-registration use the SIM-stored `#CFF?`
flags; absent flags leave the status unknown. Explicit Call divert requests
still use `+CCFC`, with a 60-second budget for the observed roughly 30-second
network replies. Incoming indications continue to be processed while waiting,
but an answer or SMS-store command must wait for the non-abortable query to
finish on the shared AT channel.

## Bench results (2026-09-16, branch head f5bbf7d, board #1)

Verizon (US Mobile "Warp", image 1), messages sent from another line:

| Row | Result |
|---|---|
| short plain text | stored, shown |
| text with a line break | stored exact (`Test\nLine`), shown on two lines |
| `Hi @you ` (`@` = GSM code 0x00, trailing space) | stored exact, shown |
| > 160 chars (2 parts, WEMT) | both parts stored, grouped (ref 0xFC), shown as one message; part 2 needed the `<length>` off-by-one tolerance (module said 21, octets said 22) |
| > 310 chars (3 parts) | all parts stored and grouped (ref 0xFD); inbox shows "Data message" because the pre-existing multipart assembler caps decoded text at 320 characters (`MODEM_SMS_DECODED_TEXT_MAX`) — not a delivery defect |
| emoji | stored as UCS2 (`D83D DE0D`); inbox renders `??` per the pre-existing surrogate policy |
| 3 messages sent while the phone had been idle ~3 min with USB unplugged | all three stored, alert "fast" (owner); transport sleep/wake cycles 13 -> 21 |

AT&T ("Dark Star", image 0, auto-switched): short, line-break and 2-part
messages arrive in the 10-field 3GPP text form
(`+CMT: "<oa>","","yy/MM/dd,hh:mm:ss-16",145,68,0,0,"<sca>",145,<len>`), are
stored as REC UNREAD DELIVER PDUs (zone byte 0x69), grouped and shown.

Transport awake time per received message (modem DTR-sleep residency counters,
phone untouched, status-only console polls, USB connected so only the modem
transport sleeps):

| Build | Window | Asleep | Awake | Baseline (idle) | Per message |
|---|---|---|---|---|---|
| master 9f495c2, store mode (`+CMTI`) | 94.6 s | 78.4 s | 16.2 s | ~3 s | ~13 s |
| branch f5bbf7d, direct delivery | 76.4 s | 57.9 s | 18.5 s | ~2.5 s | ~16 s |

The direct route adds about 3 s of transport activity per message (the
`STORE_DELIVERED` operation: `+CPMS`, wake qualification, `+CMGF=0`, `+CMGW`,
`+CMGF=1`); the rest is the pre-existing storage poll, inbox rescan and alert.
No DTR/RI/CFUN/`sms_wake` code changed. Standby power with the RP asleep was
not re-measured on the bench meter in this session (USB was connected for the
console); the modem-side mechanism is unchanged, so the standby floor in
`docs/standby_power.md` is expected to hold — re-measure when convenient.

Known pre-existing limits surfaced by the matrix: the 320-character decoded
text cap for multipart groups, and the `??` rendering of UTF-16 surrogates.

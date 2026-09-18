# Picture Messages

Picture messages use Nokia Smart Messaging on destination port `0x158A`, not
MMS. Ordinary text SMS still use the modem's ME store. Recognized picture parts
go directly to the phone's flash-backed picture journal and do not become Inbox
rows or consume ME slots. Existing ME messages are not automatically deleted.

## Original Behavior

Nokia 3210 v6.00 uses one shared collection for supplied pictures and saved
received pictures: four 400-byte EEPROM records under key `0x0757`. The loader
at `0x0026d0b6` and writer at `0x0026cd50` both limit the index to four slots.
The received-picture Save handler at `0x0026d372` writes a free slot or opens
replacement selection; replacement at `0x0026e328` uses the same writer.

Before Save, a decoded received picture is a type-9 object in a shared RAM
notification queue (`0x00298d72`), separate from permanent picture storage.
This establishes the UI/storage boundary, not the lifetime of every lower-layer
SMS fragment or whether original modem/SIM buffering is involved.

Sisu follows that interaction: **Picture message received -> View -> Save**.
C, either from the arrival notice or the received preview, opens **Save picture
message first?**: OK saves, C discards. A full gallery asks which picture to
replace. Arrival never replaces a saved picture. The Picture messages menu
also opens a pending reception. Captions can be read separately from the bitmap,
and saved pictures retain their sender number.

### UI Resources

The receive handler is `0x0026d2dc`. Layouts use the existing dialog framework
and translated SIDs, not English-only labels:

| Surface | Original resource |
| --- | --- |
| Arrival notice | SID `0x17c`, format `0x0e`, window `0x40`: FS2, x6/y7/w72/h30, vertically centred; View action `0x6b` |
| Received preview | Action `0x6d`: Save; bitmap window `0x22`, top/centre in 84x37; attached FS2 caption window `0x23`, MT1 |
| Caption page | Runtime object `0x001113f8`, initialised at `0x002b6c0c`: window `0x24`, FS2 x0/y0/w84/h37 |
| Save-first question | Object `0x002d7f68`, SID `0x17f`, window `0x2c`: FS2, EV, MT2, TLS2; OK/Exit |
| Full gallery, saved, replaced, erased | Information record `6`, SIDs `0x176`, `0x180`, `0x17d`, `0x172` |
| Replacement selection | Dynamic list `0x1d`, flags `0x60`, mode `5`; action `0x70` (Replace), built at `0x0026e264` |
| Sending | Progress record `0x24` (same layout as record `4`), SID `0x182` |
| Send result | Information record `6`, SID `0x183` or `0x174` |
| Sender details | Object `0x002d8154`, window `0x24`, SID `0x35c` with resolved name or number; OK |

The English arrival notice wraps as three lines: Picture / message / received.
Other languages are wrapped from the active string table. Host pixel tests use
the real fonts and every compiled language for this notice, the save-first
question and the sending layout. Preview, caption, sender details and the saved
information note also have pixel regressions. The seven-slot gallery and durable
pending queue are Sisu extensions, not claims about the original phone.

Information notes use window 12 with a window-3 graphic at x62/y0. Although the
text window is 84 pixels wide, the original text renderer at `0x0022e6cc`
narrows a row against neighboring graphic bounds (`0x0022ea62..0x0022eaee`).
The first two rows therefore stop at x62 for the 26-pixel-high information
graphic; the third row regains the full width. Treating the graphic as a simple
overlay incorrectly joined "message sent" on the second row, through the icon.

## Flash Layout And Durability

The existing picture journal contains:

- Seven shared saved-picture slots, including the four existing pictures.
- Two pending receptions, each with room for up to eight fragments and 384
  assembled bytes. Both complete and incomplete receptions count toward this
  limit; a third reception is rejected rather than evicting one.
- Sender metadata, concatenation identity, per-fragment lengths, and a local
  reception ID. Supported pictures fit the 72x28 display and 120-byte caption.

Version 2 occupies 3824 bytes of the existing 4064-byte journal payload. It
still uses the same pair of 4-KiB sectors; no other persistent unit moves.
Version-1 records migrate in memory with all four slots, including erased
slots, preserved. The three extra saved slots start empty. Migration is written
with the next normal picture mutation. Older firmware cannot read version 2;
downgrading after a commit requires restoring a compatible picture backup.

Fragments may arrive out of order. The receiver matches sender, source port,
concatenation reference and width, and same-date SMSC timestamps within 30
minutes, then checks total parts and DCS. The timestamp bound survives a reboot:
fresh uptime must not extend a saved reference's duplicate window to a whole
day. An identical duplicate does not cause another write. Conflicting
fragments quarantine an incomplete assembly, never damage a complete picture.
Incomplete/conflicted entries expire lazily on a later arrival after 30 minutes
without a matching part in the current boot. Completed pictures do not expire.
Saved/discarded entries retain duplicate-suppression records until reused.
Reference reuse inside that window is inherently ambiguous; this implementation
does not guess a new message from conflicting bytes. Multipart reception
across an SMSC date change is not supported yet.

This pending queue is intentionally more durable than Nokia's observed RAM UI
queue: committed fragments survive a reboot. Receipt initially changes RAM;
the normal paced journal writer makes it durable. A power loss before that
commit can still lose the newest fragment. The modem acknowledges the network
independently, so this is not an end-to-end exactly-once delivery guarantee.

The notification appears only after the complete picture is committed. Save
updates the selected gallery slot and consumes the pending reception in one
journal record. Save/Discard success is shown only after that commit succeeds.
A failed write preserves the prior journal, including the pending picture.
The existing preview or confirmation stays visible while the commit is pending;
there is no additional Saving/Discarding dialog. Calls and alarms can displace
that flow without being replaced by a late completion. Storage and receive
failures use the stock Not saved/Not done error dialog, with diagnostic detail
on CDC, rather than falling back to ME. Pictures are phone-owned, not scoped to
the installed SIM.

## Telit Transport

The WWX Verizon image delivers octet-encoded WEMT (teleservice 4101) with UDH
in text mode. Translation must retain UDHI for this format. Ordinary WMT
(4098) octets do not acquire a header merely because their bytes resemble one.

Production picture sends stay in text mode:

1. `AT+CMGF=1;+CSMP=81,167,0,4` selects 8-bit data with UDH.
2. Addressed `AT+CMGS` sends each segment's hex UDH and payload, not a full PDU.
3. `AT+CMGF=1;+CSMP=17,167,0,0` restores normal text parameters.

If cleanup fails, the accepted send result is not turned into a false failure
that invites duplicate sending. Parameter repair is retried in bounded bursts
with 30-second backoff. Other SMS operations fail while those parameters are
uncertain; call handling retains priority. Diagnostic binary modes retain their
existing PDU path.

**Known transport limitation:** the same modem image strips UDH from native
3GPP2 PDU-form delivery. Buffering with CNMI does not repair this: buffered
messages retain their arrival-time format. Keeping picture sends in text mode
removes the observed self-send failure window, but existing ME writes, scans,
and reads still use short PDU windows. A picture arriving during one can lose
its addressing/concatenation header before the host receives it. The host does
not guess missing headers. Eliminating those remaining windows is not claimed
by this implementation.

Text-mode octet/UDH sending and reception were verified on the modem. Repeated
three-part self-addressed pictures completed through the production DUT firmware:
reassembly, durable arrival, preview, caption, Save and the C/OK confirmation
flow. Discard returned to standby without adding a saved slot. Framebuffer
captures checked the corrected arrival, preview, caption, confirmation and saved
notice. Saved pictures survived reflashing, and a pending notice survived an
incoming call without displacing the call screen. Picture tests did not increase
ME usage or ordinary Inbox counters; a subsequent plain text SMS was received
and stored normally. These are Verizon self-send results, not cross-carrier or
original-handset interoperability results.
Host tests cover reordered parts, duplicate/conflicting parts, full queues,
reset during assembly/save, legacy migration, durable UI outcomes, cleanup
failure, and ordinary SMS/call regressions. AT&T binary-send failure reporting
remains separate work.

## Service UI Capture

Debug firmware retains two CDC commands for working with the real app state:

- `ui key navi|c|up|down|0..9|*|#` sends a key-down event through the app router.
- `ui dump` renders the current app state and prints its 84x48 framebuffer.

These are not fake screenshot fixtures: key events can save or discard data.
The dump captures `app_render`, not a readback from the physical LCD. Existing
`ui standby` and similar fixture commands are different and change app state.
Release firmware has no CDC service console.

To turn a saved CDC log into a nearest-neighbour contact sheet, install Pillow
in your Python environment and run:

```sh
python3 tools/render_ui_capture.py path/to/cdc.log path/to/screens.png
```

Incomplete or out-of-order frames are ignored. Captures can contain personal
messages or numbers; treat the log and rendered images accordingly.

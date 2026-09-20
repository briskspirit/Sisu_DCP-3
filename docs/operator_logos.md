# Operator Logos

Operator logos arrive as binary Nokia Smart Messaging SMS on destination port
`0x1582` (5506). They do not enter the text Inbox. The receive path accepts PID
zero and DCS `0x04` or `0xF5`, through the existing direct-delivery and modem
recovery adapters. It does not change Telit's receive mode.

## Payload And Network Identity

Both original forms are accepted:

- Compact: three PLMN bytes, followed by an OTA bitmap.
- Version zero: byte `0x30`, three PLMN bytes, header terminated by LF, then
  the OTA bitmap.

A normal bitmap is `00 48 0E 01` followed by 126 bytes: 72 by 14 monochrome
pixels, row-major, most-significant bit first. The compact body is 133 bytes,
so it fits exactly in one SMS with the seven-byte port-addressing UDH. The
135-byte versioned body needs concatenation. Smaller bitmaps are centered;
larger ones are cropped to the top-left 72 by 14 area. The decoder also accepts
the original one-octet info-field extension (`80 00`) and rejects unsupported
depths, malformed lengths and trailing data.

The network bytes preserve the complete PLMN, including MNC length:

| PLMN | Wire bytes |
| --- | --- |
| 244-05 | `42 F4 50` |
| 311-480 | `13 01 84` |
| 310-260 | `13 00 62` |
| 310-410 | `13 00 14` |
| 310-26 | `13 F0 62` |

For a two-digit MNC, the high nibble of byte 1 is `F`; for a three-digit MNC it
holds the third digit. Leading zeros and MNC length are significant. The old
single-digit representation `F5` in the last byte is treated as `50` (MNC 05),
matching Nokia's compatibility comparison.

Nokia's Smart Messaging document describes two-digit MNCs. The three-digit
handling here follows standard PLMN encoding and was checked by executing the
original v6.00 logo matcher: it accepts the complete three-byte values and
rejects truncated MNCs. This establishes matcher compatibility, not a claim
that an original 3210 can register on present-day US networks.

References: [Nokia Smart Messaging 3.0](https://customer.intele.no/files/Smart_Messaging_Specification_rev_3_0_0.pdf)
and [3GPP TS 29.060, section 7.7.51](https://www.etsi.org/deliver/etsi_TS/129000_129099/129060/08.07.00_60/ts_129060v080700p.pdf)
(PLMN digit layout, referencing TS 24.008).

## Phone Behaviour

After the complete receipt is durable, standby shows the translated
"Operator logo received" notice. Options are Show, Save and Discard. Show
uses the original 72-pixel-wide preview at `(6, 7)` with Back. Save replaces
the single saved logo; Discard and C/Exit leave the saved logo untouched.
The saved confirmation is shown only after the write completes. Failures use
the existing Not saved / Not done notes.

The saved image replaces the operator name only while registered on its
matching serving PLMN. A different network, missing SIM or lost registration
hides the logo without erasing it. Normal operator-name resolution remains the
fallback. The standby bitmap also starts at `(6, 7)`, leaving the existing
profile label below it. A clear-logo payload for a non-serving PLMN restores
the name by replacing the matching saved entry.

There is no logo gallery or forwarding command, consistent with the original
received-logo menu. Legacy textual `//SCKL` envelopes are not implemented.

## Persistence And Limits

The system partition owns record `0x3221`: one saved logo and two pending
receipts, with a fixed 1386-byte payload. The saved image itself is 126 bytes
plus its three-byte PLMN. Pending receipts are bounded to 512 payload bytes
each and use the existing SMS segment-count limit. No partition resize, erase
or migration is needed; the storage engine seeds a missing record normally.

Parts can arrive out of order and survive a reboot between arrivals. Sender,
ports, concatenation reference and each part's timestamp identify receipts.
Identical retransmissions are deduplicated. Conflicts and a full queue cannot
overwrite an unread completed logo. Save and receipt consumption share one
atomic commit; the old durable logo remains displayed while a replacement is
pending or its write has failed.

Partial, invalid and consumed receipts expire after 30 minutes of awake time
since the latest new part, rebased on boot. Completed unread receipts do not
expire. Consumed receipts may be reused sooner. Modem recovery requires an
exact committed receipt and an unchanged reread before deleting the ME copy.
Rejected logos never become ordinary Data messages.

The CDC `status` snapshot reports receive-part/error counts, pending receipt ID
and commit status. NetMonitor page 73 includes the `OP LOGO` storage unit.

Service builds also provide `logo <number> <mcc> <mnc> <format>` for sending a
test image through the normal SMS sender. Formats are `compact`, `versioned`,
`small` (64 by 14), `clear`, and `invalid` (unsupported bitmap depth). These
send real SMS; use a test destination. The existing sender uses conservative
128-byte chunks, so its full-size compact test is multipart even though the
wire format permits a 133-byte single-part body. The smaller test uses one SMS.
This console command is not a phone menu or a receive-side injection.

## Verification

Host tests cover decoding, malformed inputs, PLMN matching, multipart order,
duplicates, persistence/reboot, queue exhaustion, save failures, UI transitions,
translated controls, direct delivery and recovery. The partition test includes
logo-record torn writes and system writes while the user volume is full.

An optional original-ROM probe requires Unicorn and the hash-verified v6.00
image, neither of which is bundled with this test:

```sh
python3 tests/test_operator_logo_rom.py tools/assetgen/out/3210600c.fls
```

It executes the matcher at `0x2897DA` and the original window compositor for
standby item `0x0F` and preview window `0x4B`. EEPROM, allocation and physical
LCD operations are stubbed; matching and layout execute from the ROM.
Other reference points are parser `0x274C40`, menu `0x2DCFF8`, handler
`0x299140`, and save helper `0x28A224` (original EEPROM key `0x744`, 136 bytes).
The receive and saved strings are `0x21B` and `0x21D`; saved uses display
record 3. These are language-library strings, not fixed English UI text.

Live self-delivery on Verizon (311-480) was checked on 2026-09-20 with DCS F5:

- Compact and versioned bodies arrived and reassembled; a smaller single-part
  logo was centered correctly. Captured bitmap pixels matched the sent image.
- Show, Save, Discard and the saved confirmation were exercised on the DUT.
  The saved image and an unread completed receipt survived firmware reloads.
- Saving 311-48 did not match the serving 311-480 network. A non-serving
  clear-logo payload restored the carrier name, and a subsequent matching
  logo replaced it again.
- An unsupported bitmap depth produced a receive error, preserved the saved
  image and did not create a text Inbox entry. Existing contacts, messages,
  pictures and the saved ringtone remained intact.

The bench run exposed stale options-list pixels behind the preview. The logo
renderer now clears its framebuffer on every draw; the UI test reuses a dirty
framebuffer across both transitions instead of assuming a blank frame.
The corrected build passed all 141 host tests and both firmware build checks.
Other carriers, a physical SIM-swap visibility test and power cuts during an
active logo write were not exercised in this live run; network-state changes
and torn writes are covered by host tests.

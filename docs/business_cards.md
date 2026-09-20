# Business Cards

Phonebook **Send entry** sends a small vCard to Smart Messaging port `0x00E2`:

```text
BEGIN:VCARD<CR><LF>
N:Alice<CR><LF>
TEL:+123456789<CR><LF>
END:VCARD<CR><LF>
```

The name line is omitted for a number-only entry. The body matches the original
3210 encoder's bare/port-addressed form. Names use Latin-1 where representable;
other names use an explicit `CHARSET=UTF-8` property. Sending uses the same `0xF5`
binary transport as pictures and ringtones, without changing Telit's receive
mode. Sent business cards do not occupy Outbox.

The original Send entry caller selects the `//SCKE2 ` text envelope. A live
Verizon self-send of that envelope arrived port-addressed but missing the first
seven vCard characters, already absent in the modem's raw UART output. Whether
the modem or network performed that lossy conversion is not established. Our
sender therefore supplies the port and bare payload explicitly; the receiver
continues to reject truncated cards rather than attempting to repair them.

## Receiving And UI

Complete `//SCKE2 ` text messages and port-addressed vCards on `0x00E2`, `0x23F4`
or `0xC004` are recognized after durable SMS reassembly. Binary payloads default
to Latin-1; decoded text payloads are UTF-8. The parser understands `N`, `FN`,
the first `TEL`, common telephone separators, UTF-8/Latin-1 charset declarations
and quoted-printable names. Other vCard properties are not imported. `FN` takes
precedence over `N`; structured `N` text is retained, as in the original phone.

Cards use the local phonebook limits: 24 UTF-8 bytes for the name and 32 for the
number. An oversized or invalid value is rejected, never silently truncated.
CRLF lines, a complete BEGIN/END pair and a usable telephone number are required.
The parser rejects embedded controls, malformed UTF-8 and unsupported encodings.
It is a bounded Nokia-card decoder, not a general vCard library: line folding,
multiple imported numbers, the separate compact `Business Card` format on
`0x157D`, and `//SCKL` wrappers are not implemented. Sending rejects names
containing backslashes rather than emitting an ambiguous escape sequence.

Standby shows the translated **Business card received** notice after publication
is durable. Options are **Show**, **Save** and **Discard**. Show uses the original
name/number windows: FS2 name at the top-left of the 84x28 content area starting
at y=7, and an FS0 number at its bottom-right, with Back below. Long fields use
independent chunked scrolling. Leaving Show returns to Options, not Discard.

Save adds a local contact and shows Saved only after both the contact and receipt
state are durable. Discard, including C from Options, consumes the receipt without
adding a contact. Storage failures retain a retryable card and use existing
Not saved / Memory full notes. Recognized malformed cards show Not done and do
not become ordinary Inbox entries. A completion cannot replace an incoming call
or alarm that interrupted the menu.

## Storage And Recovery

The user-volume `cards` directory holds up to 16 complete receipts, including
bounded deduplication history. Each file retains the assembled message's original
segments, with an atomic attribute distinguishing unread, saved and discarded.
The directory shares the existing OTHER budget; partition sizes do not change.
Its metadata pair costs 8 KiB when the directory is first created.

Partial cards use the existing pending-SMS budget, identity checks and expiry.
Out-of-order parts, a split text prefix and reboot between parts are supported.
Publication writes the card before removing its staging file. Exact retransmitted
segments are recognized even after Save/Discard, while the retained receipt exists.
The oldest consumed receipts may be removed to admit new cards, including under
filesystem-budget pressure. Unread complete cards are never evicted or expired.

Contact import reuses the card's globally allocated object ID. If power fails
between writing the contact and consuming the card, retry verifies that exact
contact rather than allocating another one. It cannot overwrite a different or
corrupt contact. Corrupt card files are preserved, omitted from the healthy cache,
and counted in the persistent health record. NetMonitor page 76 and `storeinfo`
report that counter; `storeinfo` also reports readiness, pending ID and parser
errors. The older four-counter health record remains readable.

## Original References And Verification

The hash-verified Nokia 3210 v6.00 image supplies these references:

- Send entry caller `0x249B54` selects mode 1 of encoder `0x287BB0`, then invokes
  text submission at `0x228B08`. Formatter `0x27418C` omits VERSION and an empty N.
- Receive dispatcher `0x29E328` routes the three vCard ports to parser `0x27506E`.
  Port `0x157D` instead uses compact-card parser `0x275370`.
- Menu `0x2DD054`, handler `0x299324`: Show/Save/Discard string IDs
  `0x67/0x65/0x63`, receipt notice `0x64`, preview windows `0x37/0x38`.
- Normal add-contact confirmation uses record 26 and string `0x296` (Saved).

The optional `tests/test_business_card_rom.py` probe executes the original
encoder and port dispatcher with Unicorn; it needs the user's original image.
Host tests cover codec bounds, multipart order, restart recovery, deduplication,
quota exhaustion, damaged records, translated UI, scrolling, and interrupted
receipt publication/contact saves at each flash operation boundary. These tests
do not establish carrier delivery; that requires a live send/receive test.

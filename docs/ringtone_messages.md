# Ringtone Messages

Composer sends native Nokia Smart Messaging melodies as binary SMS to port
`0x1581` (5505), with source port zero, PID zero, and DCS `0xF5`. The payload
is the packed Ringing Tone Programming Language, not an RTTTL text string.
Long payloads use the modem service's existing concatenated binary transport.
Telit remains in text receive mode throughout the send, preserving incoming
Smart Messaging headers.

The send dialog follows the result of its own modem request. Modem acceptance
produces "Tone sent"; rejection produces the existing failure note. A timeout
with an uncertain outcome is not reported as success. Acceptance is not a
guarantee of delivery through the recipient's carrier.

## Receiving And Saving

Port `0x1581` messages go to the ringtone store, not the text Inbox or picture
store. DCS `0x04` and `0xF5` are accepted. The decoder checks the command framing,
Latin-1 or UCS-2 title, patterns/references, repeat counts, notes, scales,
durations, tempo, and playing style. Invalid or unsupported melodies produce
the existing "Not done" note rather than an unreadable Inbox record.

After a complete melody is committed, standby shows "Ringing tone received".
Options are Playback, Save, and Discard. Playback uses the original striped
"Playing tone" dialog with Quit. Save replaces the single Received tone slot;
the separate Own tone from Composer is untouched. Both slots can be selected
as ringtones and are played through the magnetic buzzer at the user's ringing
volume. Natural and staccato note spacing is handled by the melody player;
incoming volume instructions do not override the user's volume.

Two pending receipts, each holding at most 256 melody bytes, share the same
atomic littlefs record as Own/Received tone. Multipart arrivals may be out of
order and may survive a reboot between parts. Identical retransmissions are
deduplicated; conflicting parts do not replace a complete pending melody.
Saving the melody and consuming its receipt are one commit. The saved note is
shown only after that commit succeeds. A full pending queue preserves existing
unread tones and reports a receive failure.

Incomplete, invalid, and consumed receipts expire after 30 minutes of awake
runtime since their latest new part (rebased on boot). Complete unread melodies
do not expire. Consumed receipts may be reused sooner when another melody
arrives. Recovery from modem storage requires an exact durable part receipt
and an unchanged reread before deleting the modem copy.

The playback expansion is bounded to 512 notes. Infinite pattern repeats are
played once for preview and repeated while ringing a call. Legacy textual
`//SCKL` envelopes and sending built-in ROM ringtones are not implemented.

## Original-Firmware Reference

These paths were checked against Nokia 3210 NSE-8 v6.00, image SHA-256
`0ab99ed809232d2c14c6587c9fc904323358f223e978b6215d15475a62362f6c`:

- Composer send: `0x28bb1a` through `0x228aa0`; the submit builder writes
  destination `0x1581` and DCS `0xF5`.
- Incoming melody parser: `0x274928`; dispatch from `0x29e328`.
- Received-tone menu: `0x2dcf9c`; handler `0x298f68`.
- Playback uses display record 37 and string `0x221`; Save uses record 3
  and string `0x224`. The standby notice is string `0x222`.

All user-facing labels use the generated language strings. Host coverage is
in `test_ringtone_codec`, `test_store_ringtones`, `test_tone_composer_app`,
`test_audio_tonedecode`, and the Telit service harness.

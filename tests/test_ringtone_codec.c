#include "audio/composer_codec.h"
#include "audio/ringtone_codec.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint8_t data[512];
static unsigned pos;
static void bits(unsigned value, unsigned n) {
    while (n-- != 0u) {
        assert(pos < sizeof(data) * 8u);
        if (value & (1u << n)) data[pos / 8u] |= (uint8_t)(0x80u >> (pos % 8u));
        pos++;
    }
}
static void begin(void) {
    memset(data, 0, sizeof(data)); pos = 0u;
    bits(2u, 8u); bits(0x4au, 8u); bits(0x1du, 7u); bits(1u, 3u); bits(0u, 4u);
}
static size_t finish(void) { pos = (pos + 7u) & ~7u; bits(0u, 8u); return pos / 8u; }
static void pattern(unsigned id, unsigned loop, unsigned count) {
    bits(0u, 3u); bits(id, 2u); bits(loop, 4u); bits(count, 8u);
}
static void note(unsigned pitch, unsigned duration, unsigned modifier) {
    bits(1u, 3u); bits(pitch, 4u); bits(duration, 3u); bits(modifier, 2u);
}

int main(void) {
    ringtone_info_t info;
    ringtone_event_t events[RINGTONE_EVENT_MAX];
    composer_note_event_t own[96];
    for (unsigned i = 0u; i < 96u; i++) own[i] = (composer_note_event_t){1u, (uint8_t)(i % 13u), 2u, false};
    uint16_t len;
    assert(composer_codec_encode("Own", own, 96u, 12u, data, sizeof(data), &len));
    assert(ringtone_decode(data, len, &info, events, RINGTONE_EVENT_MAX));
    assert(info.event_count == 96u && info.duration_ms == 57600u && !strcmp(info.name, "Own"));
    for (unsigned n = 0u; n < len; n++) assert(!ringtone_decode(data, n, &info, NULL, 0u));
    assert(!ringtone_decode(data, len, &info, events, 95u));
    data[len] = 0u;
    assert(!ringtone_decode(data, len + 1u, &info, NULL, 0u));

    begin(); bits(2u, 8u); pattern(0u, 1u, 6u);
    bits(4u, 3u); bits(12u, 5u); /* tempo */
    bits(2u, 3u); bits(0u, 2u); /* low scale */
    bits(3u, 3u); bits(2u, 2u); /* staccato */
    bits(5u, 3u); bits(3u, 4u); /* volume */
    note(1u, 2u, 2u); note(0u, 3u, 3u);
    pattern(0u, 0u, 0u); /* previously defined pattern */
    size_t length = finish();
    assert(ringtone_decode(data, length, &info, events, RINGTONE_EVENT_MAX));
    assert(info.event_count == 6u && events[0].duration_ms == 1050u && events[1].duration_ms == 200u);
    assert(events[0].style == 2u && events[0].scale == 0u && events[0].volume == 3u);
    assert(info.duration_ms == 3750u);

    begin(); bits(1u, 8u); pattern(2u, 15u, 1u); note(12u, 2u, 0u); length = finish();
    assert(ringtone_decode(data, length, &info, events, RINGTONE_EVENT_MAX));
    assert(info.loop_start == 0u && info.loop_end == 1u);
    begin(); bits(1u, 8u); pattern(2u, 0u, 0u); length = finish();
    assert(!ringtone_decode(data, length, &info, NULL, 0u));
    begin(); bits(1u, 8u); pattern(0u, 0u, 1u); note(13u, 2u, 0u); length = finish();
    assert(!ringtone_decode(data, length, &info, NULL, 0u));
    begin(); bits(1u, 8u); pattern(0u, 0u, 1u); note(1u, 6u, 0u); length = finish();
    assert(!ringtone_decode(data, length, &info, NULL, 0u));
    begin(); bits(1u, 8u); pattern(0u, 14u, 255u);
    for (unsigned i = 0; i < 255; i++) note(1u, 2u, 0u);
    length = finish();
    assert(!ringtone_decode(data, length, &info, NULL, 0u));

    memset(data, 0, sizeof(data)); pos = 0;
    bits(3u, 8u); bits(0x4au, 8u); bits(0x44u, 8u);
    bits(0x1du, 7u); bits(1u, 3u); bits(2u, 4u); bits(0x0410u, 16u); bits(0x00e9u, 16u);
    bits(1u, 8u); pattern(0u, 0u, 1u); note(1u, 2u, 0u); length = finish();
    assert(ringtone_decode(data, length, &info, NULL, 0u));
    assert(!strcmp(info.name, "\xd0\x90\xc3\xa9"));
    assert(composer_codec_encode("\xd0\x90\xc3\xa9", own, 1u, 12u, data, sizeof(data), &len));
    assert(ringtone_decode(data, len, &info, NULL, 0u) && !strcmp(info.name, "\xd0\x90\xc3\xa9"));
    assert(composer_codec_encode("Caf\xc3\xa9", own, 1u, 12u, data, sizeof(data), &len));
    assert(data[0] == 2u && ringtone_decode(data, len, &info, NULL, 0u) && !strcmp(info.name, "Caf\xc3\xa9"));
    assert(!composer_codec_encode("\xe0\x80\x80", own, 1u, 12u, data, sizeof(data), &len));

    /* Deterministic malformed inputs under ASan/UBSan, bounded parsing. */
    uint32_t random = 1u;
    for (unsigned i = 0u; i < 10000u; i++) {
        for (unsigned j = 0u; j < sizeof(data); j++) {
            random = random * 1664525u + 1013904223u;
            data[j] = (uint8_t)(random >> 24u);
        }
        (void)ringtone_decode(data, (i % sizeof(data)) + 1u, &info, events, RINGTONE_EVENT_MAX);
    }
    puts("ringtone codec passed");
    return 0;
}

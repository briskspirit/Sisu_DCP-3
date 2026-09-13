#include <assert.h>
#include <stdio.h>

#include "audio/composer_codec.h"

/* The composer entry preview (tones_app) and the packed-tone decoder
 * (audio_service) both feed from these tables; a change here changes what a
 * saved Own Tone sounds like, so pin the traced values. */
int main(void) {
    /* Tempo bytecodes 0x04..0x13 span the original 16-step BPM ladder. */
    assert(composer_tempo_bpm(0x04u) == 40u);
    assert(composer_tempo_bpm(0x0cu) == 100u);
    assert(composer_tempo_bpm(0x13u) == 225u);
    assert(composer_tempo_bpm(0x00u) == 100u);   /* out of range -> default */
    assert(composer_tempo_bpm(0x14u) == 100u);

    /* Duration code <-> denominator round-trips through one table. */
    const unsigned short denoms[6] = {1u, 2u, 4u, 8u, 16u, 32u};
    for (unsigned char code = 0u; code < 6u; code++) {
        assert(composer_duration_denominator(code) == denoms[code]);
        assert(composer_duration_code_for_denominator(denoms[code]) == code);
    }
    assert(composer_duration_denominator(9u) == 4u);
    assert(composer_duration_code_for_denominator(3u) == 0u); /* unknown -> whole note */

    /* Whole note = 4 beats; dotted adds half; 40 ms floor. */
    assert(composer_note_ms(100u, 4u, false) == 600u);
    assert(composer_note_ms(100u, 4u, true) == 900u);
    assert(composer_note_ms(100u, 1u, false) == 2400u);
    assert(composer_note_ms(225u, 32u, false) == 40u);  /* floored */
    assert(composer_note_ms(0u, 0u, false) == 600u);    /* defaults: 100 BPM, 1/4 */

    /* Encode -> decode round trip: the encoder and decoder share one framing
     * implementation, and this locks the field widths and opcodes against any
     * future split. */
    {
        composer_note_event_t in[3] = {
            {1u, 1u, 2u, false},   /* c1 quarter */
            {2u, 5u, 3u, true},    /* e2 dotted eighth */
            {1u, 0u, 0u, false},   /* whole rest */
        };
        uint8_t packed[64];
        uint16_t packed_len = 0u;
        assert(composer_codec_encode("Ab", in, 3u, 0x0cu,
                                     packed, sizeof(packed), &packed_len));
        assert(packed_len > 0u);
        /* The stream opens byte-aligned with opcodes 0x02 0x4a. */
        assert(packed[0] == 0x02u && packed[1] == 0x4au);

        composer_note_event_t out[8];
        uint8_t tempo = 0u;
        uint8_t count = 0u;
        assert(composer_codec_decode(packed, packed_len, &tempo, out,
                                     8u, &count));
        assert(tempo == 0x0cu && count == 3u);
        for (unsigned i = 0u; i < 3u; i++) {
            assert(out[i].octave == in[i].octave);
            assert(out[i].pitch_code == in[i].pitch_code);
            assert(out[i].duration_code == in[i].duration_code);
            assert(out[i].dotted == in[i].dotted);
        }

        /* An empty name and the max 15-char name both round-trip. */
        assert(composer_codec_encode("", in, 1u, 0x04u,
                                     packed, sizeof(packed), &packed_len));
        assert(composer_codec_decode(packed, packed_len, &tempo, out, 8u, &count));
        assert(tempo == 0x04u && count == 1u);
        assert(composer_codec_encode("123456789012345678", in, 1u, 0x13u,
                                     packed, sizeof(packed), &packed_len));
        assert(composer_codec_decode(packed, packed_len, &tempo, out, 8u, &count));
        assert(tempo == 0x13u && count == 1u);

        /* Exact vector, derived from the traced field layout independently of
         * the encoder: name "" + one c1 quarter at 100 BPM, padded, then the
         * mandatory terminal 0x00 byte. */
        static const uint8_t GOLDEN[13] = {
            0x02u, 0x4au, 0x3au, 0x40u, 0x04u, 0x00u, 0x0bu, 0x18u,
            0xcau, 0xe9u, 0x22u, 0x80u, 0x00u,
        };
        composer_note_event_t one[1] = {{1u, 1u, 2u, false}};
        assert(composer_codec_encode("", one, 1u, 0x0cu,
                                     packed, sizeof(packed), &packed_len));
        assert(packed_len == sizeof(GOLDEN));
        for (unsigned i = 0u; i < sizeof(GOLDEN); i++) {
            assert(packed[i] == GOLDEN[i]);
        }
        assert(composer_codec_decode(GOLDEN, sizeof(GOLDEN), &tempo, out, 8u, &count));
        assert(tempo == 0x0cu && count == 1u && out[0].pitch_code == 1u);

        /* Stock v6.00 skips alignment padding without validating it. */
        uint8_t nonzero_padding[13];
        for (unsigned i = 0u; i < 13u; i++) { nonzero_padding[i] = GOLDEN[i]; }
        nonzero_padding[11] |= 0x01u;
        assert(composer_codec_decode(nonzero_padding, 13u, &tempo, out, 8u, &count));

        /* The terminal byte is mandatory: missing or nonzero -> reject. */
        assert(!composer_codec_decode(GOLDEN, sizeof(GOLDEN) - 1u, &tempo, out, 8u, &count));
        uint8_t bad_term[13];
        for (unsigned i = 0u; i < 13u; i++) { bad_term[i] = GOLDEN[i]; }
        bad_term[12] = 0x01u;
        assert(!composer_codec_decode(bad_term, 13u, &tempo, out, 8u, &count));

        /* Declared records beyond event_cap are still parsed and validated,
         * so the terminator check lands at the true end of the stream. */
        assert(composer_codec_encode("", in, 3u, 0x0cu,
                                     packed, sizeof(packed), &packed_len));
        assert(composer_codec_decode(packed, packed_len, &tempo, out, 1u, &count));
        assert(count == 1u);
        assert(!composer_codec_decode(packed, packed_len - 1u, &tempo, out, 1u, &count));

        /* Field-width contract: out-of-range tempo/pitch/duration are
         * rejected at the API instead of silently truncated by the writer. */
        composer_note_event_t bad_pitch[1] = {{1u, 16u, 2u, false}};
        composer_note_event_t bad_dur[1] = {{1u, 1u, 8u, false}};
        assert(!composer_codec_encode("", one, 1u, 0x20u,
                                      packed, sizeof(packed), &packed_len));
        assert(!composer_codec_encode("", bad_pitch, 1u, 0x0cu,
                                      packed, sizeof(packed), &packed_len));
        assert(!composer_codec_encode("", bad_dur, 1u, 0x0cu,
                                      packed, sizeof(packed), &packed_len));
        /* ...while an out-of-range octave is a defined clamp to 1. */
        composer_note_event_t oct0[1] = {{0u, 1u, 2u, false}};
        assert(composer_codec_encode("", oct0, 1u, 0x0cu,
                                     packed, sizeof(packed), &packed_len));
        assert(composer_codec_decode(packed, packed_len, &tempo, out, 8u, &count));
        assert(out[0].octave == 1u);

        /* Corrupted framing is rejected; overflow is reported, not truncated. */
        assert(composer_codec_encode("x", in, 1u, 0x0cu,
                                     packed, sizeof(packed), &packed_len));
        packed[0] ^= 0x01u;
        assert(!composer_codec_decode(packed, packed_len, &tempo, out, 8u, &count));
        uint8_t tiny[4];
        uint16_t tiny_len = 0u;
        assert(!composer_codec_encode("longer name", in, 3u, 0x0cu,
                                      tiny, sizeof(tiny), &tiny_len));
    }

    /* The 8-bit length field represents at most 126 notes. The boundary
     * round-trips; 127 must be rejected instead of wrapping the field. */
    {
        composer_note_event_t in[COMPOSER_CODEC_NOTE_MAX + 1u];
        composer_note_event_t out[COMPOSER_CODEC_NOTE_MAX];
        uint8_t packed[320];
        for (unsigned i = 0u; i < COMPOSER_CODEC_NOTE_MAX + 1u; i++) {
            in[i] = (composer_note_event_t){1u, 1u, 2u, false};
        }
        uint16_t packed_len = 0u;
        uint8_t tempo = 0u;
        uint8_t count = 0u;
        assert(composer_codec_encode("", in, COMPOSER_CODEC_NOTE_MAX, 0x0cu,
                                     packed, sizeof(packed), &packed_len));
        assert(composer_codec_decode(packed, packed_len, &tempo, out,
                                     COMPOSER_CODEC_NOTE_MAX, &count));
        assert(count == COMPOSER_CODEC_NOTE_MAX);
        assert(!composer_codec_encode("", in, COMPOSER_CODEC_NOTE_MAX + 1u, 0x0cu,
                                      packed, sizeof(packed), &packed_len));
    }

    /* Running-octave rule (ROM 0x0028b2d6..0x0028b41a): "c1 - e2 -" must
     * record the rests at octave 1 then 2; a leading rest takes 1; a note
     * without a digit keeps the running value; an out-of-range digit is
     * ignored. The tracker is the single owner of this rule. */
    {
        composer_octave_tracker_t t;
        composer_octave_tracker_init(&t);
        assert(composer_octave_tracker_next(&t, 0u) == 1u);   /* leading rest */
        assert(composer_octave_tracker_next(&t, 1u) == 1u);   /* c1 */
        assert(composer_octave_tracker_next(&t, 0u) == 1u);   /* -  -> 1 */
        assert(composer_octave_tracker_next(&t, 2u) == 2u);   /* e2 */
        assert(composer_octave_tracker_next(&t, 0u) == 2u);   /* -  -> 2 (NOT 1) */
        assert(composer_octave_tracker_next(&t, 7u) == 2u);   /* bad digit ignored */
        assert(composer_octave_tracker_next(&t, 3u) == 3u);
        assert(composer_octave_tracker_next(&t, 0u) == 3u);

        /* And the rule survives the wire: encode the tracked events, decode,
         * and the rest records come back at 1 then 2. */
        composer_octave_tracker_init(&t);
        const uint8_t digits[4] = {1u, 0u, 2u, 0u};        /* c1 - e2 - */
        const uint8_t pitches[4] = {1u, 0u, 5u, 0u};
        composer_note_event_t seq[4];
        composer_note_event_t out[8];
        uint8_t packed[64];
        uint16_t packed_len = 0u;
        uint8_t tempo = 0u;
        uint8_t count = 0u;
        for (unsigned i = 0u; i < 4u; i++) {
            seq[i].octave = composer_octave_tracker_next(&t, digits[i]);
            seq[i].pitch_code = pitches[i];
            seq[i].duration_code = 2u;
            seq[i].dotted = false;
        }
        assert(composer_codec_encode("", seq, 4u, 0x0cu,
                                     packed, sizeof(packed), &packed_len));
        assert(composer_codec_decode(packed, packed_len, &tempo, out, 8u, &count));
        assert(count == 4u);
        assert(out[1].octave == 1u && out[1].pitch_code == 0u);
        assert(out[3].octave == 2u && out[3].pitch_code == 0u);
    }

    puts("test_composer_codec: all assertions passed");
    return 0;
}

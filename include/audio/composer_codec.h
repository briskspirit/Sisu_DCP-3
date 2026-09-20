#ifndef AUDIO_COMPOSER_CODEC_H
#define AUDIO_COMPOSER_CODEC_H

#include <stdbool.h>
#include <stdint.h>

/* Shared constants and timing math of the Nokia composer packed-tone format.
 * The tones_app encoder/entry preview and the audio_service decoder must
 * agree byte-for-byte, so the tempo table, the duration-code table, and the
 * note-length formula live here exactly once. */

/* Tempo bytecode (0x04..0x13) -> BPM; anything else -> 100. */
uint16_t composer_tempo_bpm(uint8_t tempo_bytecode);
/* Duration code (0..5) -> note denominator (1,2,4,8,16,32); else -> 4. */
uint16_t composer_duration_denominator(uint8_t duration_code);
/* Note denominator -> duration code (inverse of the same table; 1 -> 0). */
uint8_t composer_duration_code_for_denominator(uint16_t denom);
/* Whole note = 4 beats at `bpm`; dotted adds half; 40 ms floor. */
uint16_t composer_note_ms(uint16_t bpm, uint16_t denom, bool dotted);

/* The packed stream's name field is 4 bits long. */
#define COMPOSER_CODEC_NAME_MAX 15u
/* The 8-bit note-length field stores (note_count * 2 + 3). */
#define COMPOSER_CODEC_NOTE_MAX 126u

typedef struct {
    uint8_t octave;        /* 1..3; anything else encodes as 1 */
    uint8_t pitch_code;    /* 0 = rest, 1..12 = C..B including sharps */
    uint8_t duration_code; /* 0..5 = whole..1/32 */
    bool dotted;
} composer_note_event_t;

/* Octave assignment rule of the v6.00 encoder (0x0028b2d6..0x0028b41a): ONE
 * running octave across the token stream, initialised to 1 and updated only
 * by a token that carries an explicit 1..3 digit. A rest (or any token without
 * a digit) is written with the octave of the note before it. Feed each token's
 * parsed digit (0 = none) in order; returns the octave to record. */
typedef struct {
    uint8_t running;
} composer_octave_tracker_t;

void composer_octave_tracker_init(composer_octave_tracker_t *t);
uint8_t composer_octave_tracker_next(composer_octave_tracker_t *t, uint8_t explicit_digit);

/* Bit-pack `note_count` events into the Nokia composer stream (header,
 * name, tempo, per-note records). False on an unrepresentable count,
 * malformed UTF-8/UCS-2 title (up to 15 characters),
 * destination overflow, or any value that does not fit its bit field
 * (tempo > 0x1f, pitch > 15, duration > 7); an octave outside 1..3 is
 * clamped to 1 as the format defines. */
bool composer_codec_encode(const char *name,
                           const composer_note_event_t *notes,
                           uint8_t note_count,
                           uint8_t tempo_bytecode,
                           uint8_t *dst, uint16_t cap, uint16_t *out_len);
/* Validate the stream framing and unpack tempo + note events (capped at
 * event_cap). False on any malformed field or an empty tone. */
bool composer_codec_decode(const uint8_t *data, uint16_t len,
                           uint8_t *out_tempo,
                           composer_note_event_t *events,
                           uint8_t event_cap,
                           uint8_t *out_count);

#endif

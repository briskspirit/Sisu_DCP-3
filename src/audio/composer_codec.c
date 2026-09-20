#include "audio/composer_codec.h"

#include <string.h>

static const uint16_t BPM_BY_TEMPO[16] = {
    40u, 45u, 50u, 56u, 63u, 70u, 80u, 90u,
    100u, 112u, 125u, 140u, 160u, 180u, 200u, 225u,
};

static const uint16_t DENOM_BY_CODE[6] = {1u, 2u, 4u, 8u, 16u, 32u};

uint16_t composer_tempo_bpm(uint8_t tempo_bytecode) {
    if (tempo_bytecode >= 0x04u && tempo_bytecode <= 0x13u) {
        return BPM_BY_TEMPO[tempo_bytecode - 0x04u];
    }
    return 100u;
}

uint16_t composer_duration_denominator(uint8_t duration_code) {
    return duration_code < 6u ? DENOM_BY_CODE[duration_code] : 4u;
}

uint8_t composer_duration_code_for_denominator(uint16_t denom) {
    for (uint8_t code = 0u; code < 6u; code++) {
        if (DENOM_BY_CODE[code] == denom) {
            return code;
        }
    }
    return 0u; /* unknown denominators encode as a whole note (traced encoder) */
}

uint16_t composer_note_ms(uint16_t bpm, uint16_t denom, bool dotted) {
    if (bpm == 0u) {
        bpm = 100u;
    }
    if (denom == 0u) {
        denom = 4u;
    }
    uint32_t duration = (60000u / bpm) * 4u / denom;
    if (dotted) {
        duration += duration / 2u;
    }
    return duration < 40u ? 40u : (uint16_t)duration;
}

void composer_octave_tracker_init(composer_octave_tracker_t *t) {
    t->running = 1u;
}

uint8_t composer_octave_tracker_next(composer_octave_tracker_t *t, uint8_t explicit_digit) {
    if (explicit_digit >= 1u && explicit_digit <= 3u) {
        t->running = explicit_digit;
    }
    return t->running;
}

/* --- bit-level packing (big-endian bit order within each byte) --- */

typedef struct {
    uint8_t *dst;
    uint16_t cap;
    uint16_t pos;
    uint8_t bit_offset;
    bool overflow;
} composer_bit_writer_t;

typedef struct {
    const uint8_t *data;
    uint16_t len;
    uint16_t bit_pos;
    bool error;
} composer_bit_reader_t;

static void composer_bit_writer_init(composer_bit_writer_t *writer, uint8_t *dst, uint16_t cap) {
    writer->dst = dst;
    writer->cap = cap;
    writer->pos = 0u;
    writer->bit_offset = 0u;
    writer->overflow = false;
    if (dst != 0 && cap != 0u) {
        memset(dst, 0, cap);
    }
}

static void composer_bit_write(composer_bit_writer_t *writer, uint16_t value, uint8_t bits) {
    if (writer == 0 || writer->dst == 0 || bits == 0u || bits > 16u || writer->overflow) {
        return;
    }
    for (int8_t bit = (int8_t)(bits - 1u); bit >= 0; bit--) {
        if (writer->pos >= writer->cap) {
            writer->overflow = true;
            return;
        }
        if (((value >> bit) & 1u) != 0u) {
            writer->dst[writer->pos] |= (uint8_t)(0x80u >> writer->bit_offset);
        }
        writer->bit_offset++;
        if (writer->bit_offset == 8u) {
            writer->bit_offset = 0u;
            writer->pos++;
        }
    }
}

/* Pad the final partial byte, then append the mandatory terminal 0x00 byte:
 * the v6.00 encoder writes eight zero bits after the last note record and
 * its decoder reads eight bits and rejects anything nonzero. */
static bool composer_bit_writer_finish(composer_bit_writer_t *writer, uint16_t *out_len) {
    if (writer == 0 || out_len == 0 || writer->overflow) {
        return false;
    }
    if (writer->bit_offset != 0u) {
        if (writer->pos >= writer->cap) {
            return false;
        }
        writer->pos++;
        writer->bit_offset = 0u;
    }
    if (writer->pos >= writer->cap) {
        return false;
    }
    writer->dst[writer->pos++] = 0u;
    *out_len = writer->pos;
    return true;
}

static void composer_reader_init(composer_bit_reader_t *reader, const uint8_t *data, uint16_t len) {
    reader->data = data;
    reader->len = len;
    reader->bit_pos = 0u;
    reader->error = false;
}

static uint16_t composer_read_bits(composer_bit_reader_t *reader, uint8_t bits) {
    if (reader == 0 || bits == 0u || bits > 16u || reader->error) {
        return 0u;
    }
    uint16_t value = 0u;
    for (uint8_t i = 0u; i < bits; i++) {
        if (reader->bit_pos >= (uint16_t)(reader->len * 8u)) {
            reader->error = true;
            return 0u;
        }
        uint8_t byte = reader->data[reader->bit_pos / 8u];
        uint8_t bit = (uint8_t)(7u - (reader->bit_pos % 8u));
        value = (uint16_t)((value << 1u) | ((byte >> bit) & 1u));
        reader->bit_pos++;
    }
    return value;
}

/* --- stream framing (byte-verified against the traced v6.00 format) --- */

static bool title_decode(const char *name, uint16_t *chars, uint8_t *count, bool *unicode) {
    const uint8_t *p = (const uint8_t *)(name != NULL ? name : "");
    *count = 0u;
    *unicode = false;
    while (*p != 0u && *count < COMPOSER_CODEC_NAME_MAX) {
        uint16_t cp = *p++;
        if (cp >= 0x80u) {
            unsigned extra;
            uint16_t minimum;
            if (cp >= 0xc2u && cp <= 0xdfu) { cp &= 0x1fu; extra = 1u; minimum = 0x80u; }
            else if (cp >= 0xe0u && cp <= 0xefu) { cp &= 0x0fu; extra = 2u; minimum = 0x800u; }
            else return false;
            while (extra-- != 0u) {
                if ((*p & 0xc0u) != 0x80u) return false;
                cp = (uint16_t)((cp << 6u) | (*p++ & 0x3fu));
            }
            if (cp < minimum || (cp >= 0xd800u && cp <= 0xdfffu)) return false;
        }
        if (cp > 0xffu) *unicode = true;
        chars[(*count)++] = cp;
    }
    return true;
}

bool composer_codec_encode(const char *name,
                           const composer_note_event_t *notes,
                           uint8_t note_count,
                           uint8_t tempo_bytecode,
                           uint8_t *dst, uint16_t cap, uint16_t *out_len) {
    if (dst == 0 || out_len == 0 || note_count > COMPOSER_CODEC_NOTE_MAX ||
        (notes == 0 && note_count != 0u)) {
        return false;
    }
    *out_len = 0u;
    /* Field-width contract: values that do not fit their bit fields would be
     * silently truncated by the writer (the ROM writer behaves the same, but
     * the ROM never feeds it out-of-range values); reject them instead. */
    if (tempo_bytecode > 0x1fu) {
        return false;
    }
    for (uint8_t i = 0u; i < note_count; i++) {
        if (notes[i].pitch_code > 0x0fu || notes[i].duration_code > 0x07u) {
            return false;
        }
    }
    composer_bit_writer_t writer;
    composer_bit_writer_init(&writer, dst, cap);
    uint16_t title[COMPOSER_CODEC_NAME_MAX];
    uint8_t name_len;
    bool unicode;
    if (!title_decode(name, title, &name_len, &unicode)) return false;
    uint8_t note_length_field = (uint8_t)(note_count * 2u + 3u);
    composer_bit_write(&writer, unicode ? 0x03u : 0x02u, 8u);
    composer_bit_write(&writer, 0x4au, 8u);
    if (unicode) composer_bit_write(&writer, 0x44u, 8u);
    composer_bit_write(&writer, 0x1du, 7u);
    composer_bit_write(&writer, 0x01u, 3u);
    composer_bit_write(&writer, name_len, 4u);
    for (uint8_t i = 0u; i < name_len; i++) {
        composer_bit_write(&writer, title[i], unicode ? 16u : 8u);
    }
    composer_bit_write(&writer, 0x01u, 8u);
    composer_bit_write(&writer, 0x00u, 3u);
    composer_bit_write(&writer, 0x00u, 2u);
    composer_bit_write(&writer, 0x00u, 4u);
    composer_bit_write(&writer, note_length_field, 8u);
    composer_bit_write(&writer, 0x04u, 3u);
    composer_bit_write(&writer, tempo_bytecode, 5u);
    composer_bit_write(&writer, 0x03u, 3u);
    composer_bit_write(&writer, 0x00u, 2u);
    composer_bit_write(&writer, 0x05u, 3u);
    composer_bit_write(&writer, 0x07u, 4u);
    for (uint8_t i = 0u; i < note_count; i++) {
        uint8_t octave = notes[i].octave;
        if (octave < 1u || octave > 3u) {
            octave = 1u;
        }
        composer_bit_write(&writer, 0x02u, 3u);
        composer_bit_write(&writer, octave, 2u);
        composer_bit_write(&writer, 0x01u, 3u);
        composer_bit_write(&writer, notes[i].pitch_code, 4u);
        composer_bit_write(&writer, notes[i].duration_code, 3u);
        composer_bit_write(&writer, notes[i].dotted ? 1u : 0u, 2u);
    }
    return composer_bit_writer_finish(&writer, out_len);
}

bool composer_codec_decode(const uint8_t *data, uint16_t len,
                           uint8_t *out_tempo,
                           composer_note_event_t *events,
                           uint8_t event_cap,
                           uint8_t *out_count) {
    if (data == 0 || len == 0u || out_tempo == 0 || events == 0 ||
        event_cap == 0u || out_count == 0) {
        return false;
    }
    *out_count = 0u;
    composer_bit_reader_t reader;
    composer_reader_init(&reader, data, len);

    uint16_t commands = composer_read_bits(&reader, 8u);
    bool unicode = commands == 3u;
    if ((commands != 2u && !unicode) || composer_read_bits(&reader, 8u) != 0x4au ||
        (unicode && composer_read_bits(&reader, 8u) != 0x44u) ||
        composer_read_bits(&reader, 7u) != 0x1du ||
        composer_read_bits(&reader, 3u) != 0x01u) {
        return false;
    }
    uint8_t name_len = (uint8_t)composer_read_bits(&reader, 4u);
    if (name_len > COMPOSER_CODEC_NAME_MAX) {
        return false;
    }
    for (uint8_t i = 0u; i < name_len; i++) {
        (void)composer_read_bits(&reader, unicode ? 16u : 8u);
    }
    if (composer_read_bits(&reader, 8u) != 0x01u) {
        return false;
    }
    (void)composer_read_bits(&reader, 3u);
    (void)composer_read_bits(&reader, 2u);
    (void)composer_read_bits(&reader, 4u);
    uint8_t note_length = (uint8_t)composer_read_bits(&reader, 8u);
    if (composer_read_bits(&reader, 3u) != 0x04u) {
        return false;
    }
    *out_tempo = (uint8_t)composer_read_bits(&reader, 5u);
    if (composer_read_bits(&reader, 3u) != 0x03u) {
        return false;
    }
    (void)composer_read_bits(&reader, 2u);
    if (composer_read_bits(&reader, 3u) != 0x05u ||
        composer_read_bits(&reader, 4u) != 0x07u ||
        reader.error ||
        note_length < 3u) {
        return false;
    }

    /* Every declared record is parsed and validated even past event_cap
     * (extra notes are dropped, not left unread), so the terminator check
     * below sits at the true end of the stream. */
    uint8_t note_count = (uint8_t)(((uint16_t)note_length - 3u + 1u) / 2u);
    for (uint8_t i = 0u; i < note_count; i++) {
        if (composer_read_bits(&reader, 3u) != 0x02u) {
            return false;
        }
        uint8_t octave = (uint8_t)composer_read_bits(&reader, 2u);
        if (composer_read_bits(&reader, 3u) != 0x01u) {
            return false;
        }
        uint8_t pitch_code = (uint8_t)composer_read_bits(&reader, 4u);
        uint8_t duration_code = (uint8_t)composer_read_bits(&reader, 3u);
        uint8_t dotted = (uint8_t)composer_read_bits(&reader, 2u);
        if (reader.error) {
            return false;
        }
        if (*out_count < event_cap) {
            events[*out_count].octave = octave;
            events[*out_count].pitch_code = pitch_code;
            events[*out_count].duration_code = duration_code;
            events[*out_count].dotted = dotted == 1u;
            (*out_count)++;
        }
    }
    /* The v6.00 decoder aligns by advancing the source byte and resetting its
     * bit offset (0x0028b7bc..0x0028b7ce); it does not inspect the skipped
     * padding. It then reads and validates the whole terminal byte. */
    uint8_t bit_offset = (uint8_t)(reader.bit_pos % 8u);
    if (bit_offset != 0u) {
        reader.bit_pos = (uint16_t)(reader.bit_pos + (8u - bit_offset));
    }
    if (composer_read_bits(&reader, 8u) != 0x00u || reader.error) {
        return false;
    }
    return *out_count != 0u;
}

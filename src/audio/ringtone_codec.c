#include "audio/ringtone_codec.h"

#include <string.h>

typedef struct {
    const uint8_t *data;
    size_t bits;
    size_t pos;
    bool error;
} reader_t;

typedef struct {
    size_t pos;
    uint8_t count;
} pattern_t;

typedef struct {
    uint8_t scale, style, tempo, volume;
} voice_t;

static unsigned read_bits(reader_t *r, unsigned n) {
    if (r->error || n > 16u || r->pos > r->bits || n > r->bits - r->pos) {
        r->error = true;
        return 0u;
    }
    unsigned value = 0u;
    while (n-- != 0u) {
        value = (value << 1u) | ((r->data[r->pos / 8u] >> (7u - r->pos % 8u)) & 1u);
        r->pos++;
    }
    return value;
}

static uint16_t note_ms(uint8_t tempo, unsigned duration, unsigned modifier) {
    static const uint16_t bpm[32] = {
        25, 28, 31, 35, 40, 45, 50, 56, 63, 70, 80, 90, 100, 112, 125, 140,
        160, 180, 200, 225, 250, 285, 320, 355, 400, 450, 500, 565, 635, 715, 800, 900
    };
    uint32_t ms = (60000u / bpm[tempo]) * 4u / (1u << duration);
    if (modifier == 1u) ms += ms / 2u;
    if (modifier == 2u) ms += ms * 3u / 4u;
    if (modifier == 3u) ms = ms * 2u / 3u;
    return (uint16_t)(ms != 0u ? ms : 1u);
}

static bool instructions(reader_t *r, uint8_t count, voice_t *v,
                         ringtone_info_t *info, ringtone_event_t *events,
                         size_t capacity) {
    for (unsigned i = 0; i < count; i++) {
        switch (read_bits(r, 3u)) {
        case 1u: {
            unsigned pitch = read_bits(r, 4u);
            unsigned duration = read_bits(r, 3u);
            unsigned modifier = read_bits(r, 2u);
            if (r->error || pitch > 12u || duration > 5u ||
                info->event_count == RINGTONE_EVENT_MAX ||
                (events != NULL && info->event_count >= capacity)) return false;
            ringtone_event_t event = {note_ms(v->tempo, duration, modifier),
                                     v->scale, (uint8_t)pitch, v->style, v->volume};
            if (events != NULL) events[info->event_count] = event;
            info->event_count++;
            info->duration_ms += event.duration_ms;
            break;
        }
        case 2u: v->scale = (uint8_t)read_bits(r, 2u); break;
        case 3u:
            v->style = (uint8_t)read_bits(r, 2u);
            if (v->style == 3u) return false;
            break;
        case 4u: v->tempo = (uint8_t)read_bits(r, 5u); break;
        case 5u: v->volume = (uint8_t)read_bits(r, 4u); break;
        default: return false;
        }
        if (r->error) return false;
    }
    return true;
}

static bool song(reader_t *r, bool unicode, ringtone_info_t *info,
                 ringtone_event_t *events, size_t capacity) {
    unsigned type = read_bits(r, 3u);
    if (type != 1u && type != 2u) return false; /* basic / temporary song */
    unsigned chars = type == 1u ? read_bits(r, 4u) : 0u;
    size_t title = 0u;
    for (unsigned i = 0u; i < chars; i++) {
        unsigned cp = read_bits(r, unicode ? 16u : 8u);
        if (r->error || (cp >= 0xd800u && cp <= 0xdfffu)) return false;
        /* Keep control bytes out of UI labels, without changing the melody. */
        if (cp < 32u || (cp >= 0x7fu && cp < 0xa0u)) cp = ' ';
        if (cp < 0x80u) info->name[title++] = (char)cp;
        else if (cp < 0x800u) {
            info->name[title++] = (char)(0xc0u | (cp >> 6u));
            info->name[title++] = (char)(0x80u | (cp & 63u));
        } else {
            info->name[title++] = (char)(0xe0u | (cp >> 12u));
            info->name[title++] = (char)(0x80u | ((cp >> 6u) & 63u));
            info->name[title++] = (char)(0x80u | (cp & 63u));
        }
    }
    info->name[title] = '\0';
    unsigned patterns = read_bits(r, 8u);
    pattern_t definitions[4] = {{0}};
    voice_t voice = {1u, 0u, 8u, 7u}; /* v6.00 reset tempo: table index 8 */
    unsigned work = 0u;
    uint32_t loop_duration = 0u;
    for (unsigned i = 0; i < patterns; i++) {
        if (read_bits(r, 3u) != 0u) return false;
        unsigned id = read_bits(r, 2u);
        unsigned repeats = read_bits(r, 4u);
        uint8_t count = (uint8_t)read_bits(r, 8u);
        if (r->error) return false;
        bool reference = count == 0u;
        size_t resume = r->pos;
        if (reference) {
            if (definitions[id].count == 0u) return false;
            r->pos = definitions[id].pos;
            count = definitions[id].count;
        } else definitions[id] = (pattern_t){r->pos, count};
        size_t start = r->pos;
        uint16_t first = info->event_count;
        unsigned traversals = repeats == 15u ? 1u : repeats + 1u;
        for (unsigned j = 0u; j < traversals; j++) {
            work += count;
            if (work > 8192u) return false;
            r->pos = start;
            if (!instructions(r, count, &voice, info, events, capacity)) return false;
        }
        if (repeats == 15u) {
            if (first == info->event_count || info->loop_end != 0u) return false;
            info->loop_start = first;
            info->loop_end = info->event_count;
            loop_duration = info->duration_ms;
        }
        if (reference) r->pos = resume;
    }
    if (info->loop_end != 0u) info->duration_ms = loop_duration;
    return !r->error;
}

bool ringtone_decode(const uint8_t *data, size_t len, ringtone_info_t *info,
                     ringtone_event_t *events, size_t capacity) {
    if (data == NULL || info == NULL || len == 0u || len > 4096u) return false;
    memset(info, 0, sizeof(*info));
    reader_t r = {data, len * 8u, 0u, false};
    bool programming = false, unicode = false, sound = false;
    for (;;) {
        unsigned parts = read_bits(&r, 8u);
        if (r.error) return false;
        if (parts == 0u) return sound && info->event_count != 0u && r.pos == r.bits;
        for (unsigned i = 0u; i < parts; i++) {
            unsigned command = read_bits(&r, 7u);
            if (r.error) return false;
            switch (command) {
            case 0x25u: programming = true; break;
            case 0x22u:
                if (!programming) return false;
                unicode = true;
                break;
            case 0x05u:
                if (read_bits(&r, 7u) != 0x22u) return false;
                unicode = false;
                break;
            case 0x1du:
                if (!programming || sound || !song(&r, unicode, info, events, capacity)) return false;
                sound = true;
                break;
            default: return false;
            }
            r.pos = (r.pos + 7u) & ~(size_t)7u;
        }
    }
}

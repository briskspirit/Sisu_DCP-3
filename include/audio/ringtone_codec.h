#ifndef AUDIO_RINGTONE_CODEC_H
#define AUDIO_RINGTONE_CODEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RINGTONE_SMS_PORT 0x1581u
#define RINGTONE_NAME_MAX 45u
#define RINGTONE_EVENT_MAX 512u

typedef struct {
    uint16_t duration_ms;
    uint8_t scale;
    uint8_t pitch;
    uint8_t style;
    uint8_t volume;
} ringtone_event_t;

typedef struct {
    char name[RINGTONE_NAME_MAX + 1u]; /* UTF-8, converted from Latin-1/UCS-2 */
    uint16_t event_count;
    uint16_t loop_start;
    uint16_t loop_end;
    uint32_t duration_ms; /* One traversal, including finite repeats. */
} ringtone_info_t;

/* Nokia Smart Messaging ringing-tone programming language. With events=NULL,
 * validates the entire stream without allocating the playback event array.
 * Infinite patterns are represented by loop_start/end, not expanded. */
bool ringtone_decode(const uint8_t *data, size_t len, ringtone_info_t *info,
                     ringtone_event_t *events, size_t capacity);

#endif

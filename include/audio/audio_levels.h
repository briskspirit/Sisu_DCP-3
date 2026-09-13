#ifndef AUDIO_LEVELS_H
#define AUDIO_LEVELS_H

#include <stdbool.h>
#include <stdint.h>

#define AUDIO_LEVEL_SILENT 0u
#define AUDIO_LEVEL_MAX 5u
/* The v6.00 call UI stores one 0..9 value, but its audio coefficient banks are
 * route-specific. The normal handset bank advances by 1 dB per step; both
 * external-accessory banks advance by 2 dB per step. These helpers return the
 * attenuation below each route's call-specific Level-10 anchor. */
#define AUDIO_CALL_VOLUME_MIN 1u
#define AUDIO_CALL_VOLUME_MAX 10u
#define AUDIO_CALL_HANDSET_STEP_DB 1u
#define AUDIO_CALL_HEADSET_STEP_DB 2u
#define AUDIO_CALL_HANDSET_MAX_ATTENUATION_DB \
    ((AUDIO_CALL_VOLUME_MAX - AUDIO_CALL_VOLUME_MIN) * AUDIO_CALL_HANDSET_STEP_DB)
#define AUDIO_CALL_HEADSET_MAX_ATTENUATION_DB \
    ((AUDIO_CALL_VOLUME_MAX - AUDIO_CALL_VOLUME_MIN) * AUDIO_CALL_HEADSET_STEP_DB)
/* Both codec outputs use a neutral 0 dB baseline. HDC-5 local sounds need a
 * separate -30 dB acoustic calibration, applied to the synthesized PCM before
 * it is mixed with modem downlink so voice remains at unity. 1036 / 32768 is
 * 0.031616, within 0.002 dB of the exact -30 dB voltage ratio. */
#define AUDIO_HEADSET_LOCAL_GAIN_Q15 1036u
/* Stock-Nokia acoustic calibration, measured 2026-08-30 with the same 3210
 * earpiece and enclosure on the DUT. Percent values are for diagnostics;
 * audio_service consumes the exact fixed-point values beside them. */
#define AUDIO_KEYPAD_CLICK_GAIN_PERCENT_DEFAULT 93u
#define AUDIO_KEYPAD_CLICK_GAIN_Q8_DEFAULT 238u
#define AUDIO_KEYPAD_CLICK_H2_PERCENT_DEFAULT 0
#define AUDIO_KEYPAD_CLICK_H2_Q12_DEFAULT 0
#define AUDIO_KEYPAD_CLICK_H2_PHASE_DEG_DEFAULT 0
#define AUDIO_KEYPAD_CLICK_H2_PHASE_Q32_DEFAULT 0u
#define AUDIO_KEYPAD_CLICK_H3_PERCENT_DEFAULT 1
#define AUDIO_KEYPAD_CLICK_H3_Q12_DEFAULT 41
#define AUDIO_KEYPAD_CLICK_H3_PHASE_DEG_DEFAULT 92
#define AUDIO_KEYPAD_CLICK_H3_PHASE_Q32_DEFAULT 0x416c16c1u
#define AUDIO_KEYPAD_CLICK_H4_PERCENT_DEFAULT 0
#define AUDIO_KEYPAD_CLICK_H4_Q12_DEFAULT 0
#define AUDIO_KEYPAD_CLICK_H4_PHASE_DEG_DEFAULT 0
#define AUDIO_KEYPAD_CLICK_H4_PHASE_Q32_DEFAULT 0u
#define AUDIO_KEYPAD_CLICK_H5_PERCENT_DEFAULT 0
#define AUDIO_KEYPAD_CLICK_H5_Q12_DEFAULT 0
#define AUDIO_KEYPAD_CLICK_H5_PHASE_DEG_DEFAULT 0
#define AUDIO_KEYPAD_CLICK_H5_PHASE_Q32_DEFAULT 0u
#define AUDIO_KEYPAD_CLICK_ATTACK_MS_DEFAULT 6u
#define AUDIO_KEYPAD_CLICK_RELEASE_MS_DEFAULT 4u
#define AUDIO_KEYPAD_DTMF_GAIN_PERCENT_DEFAULT 120u
#define AUDIO_KEYPAD_DTMF_GAIN_Q8_DEFAULT 307u
#define AUDIO_KEYPAD_DTMF_LOW_WEIGHT_DEFAULT 74u
#define AUDIO_KEYPAD_DTMF_HIGH_WEIGHT_DEFAULT 100u
/* HDC-5 bench against a stock 3210 at keypad-tone Level 3. The differential
 * headset outlet has a different acoustic transfer function than the handset
 * receiver, so preserve the handset mix and calibrate this route separately. */
#define AUDIO_KEYPAD_DTMF_HEADSET_LOW_WEIGHT_DEFAULT 54u
#define AUDIO_KEYPAD_DTMF_HEADSET_HIGH_WEIGHT_DEFAULT 100u
/* The v6.00 tone-03 stream is key-state driven rather than carrying a fixed
 * timeout. Two controlled stock-handset long holds stop at 3.861/3.883 s;
 * 480 ticks on its 8 ms scheduler is the matching production ceiling. */
#define AUDIO_KEYPAD_DTMF_MAX_HOLD_MS 3840u
/* The stock Level 1/2 clicks measured -16.75/-9.04 dB relative to Level 3.
 * These keypad-only trims reproduce that ladder without changing the shared
 * ringtone, alarm, or system-tone amplitude table. */
#define AUDIO_KEYPAD_LEVEL_LOW_GAIN_Q8 149u
#define AUDIO_KEYPAD_LEVEL_MID_GAIN_Q8 225u
#define AUDIO_KEYPAD_LEVEL_HIGH_GAIN_Q8 256u
/* Vibra command argument high byte: motor strength. The stock value is the
 * CBUS strength the original NSE-8 programs (0xa9); apps build their vibra
 * args from it and the HAL consumes it, so it lives in this shared contract
 * header rather than in either of them. */
#define AUDIO_VIBRA_STRENGTH_STOCK 0xa9u

uint8_t audio_level_from_ringing_volume(uint8_t value);
uint8_t audio_level_from_keypad_tones(uint8_t value);
uint16_t audio_keypad_level_gain_q8(uint8_t level);
uint8_t audio_call_handset_attenuation_db(uint8_t level);
uint8_t audio_call_headset_attenuation_db(uint8_t level);
uint8_t audio_key_id_from_key(uint16_t key);
char audio_key_char_from_id(uint8_t key_id);
uint16_t audio_arg(uint8_t code, uint8_t level);
/* Tone bytecode can contain vibra markers. Carry their profile gate in the
 * same command as the stream start so an older stream's completion cannot
 * clear a separately queued arm command for the new stream. */
uint16_t audio_arg_with_marker_vibra(uint8_t code, uint8_t level, bool enabled);
uint16_t audio_arg_for_key(uint16_t key, uint8_t level);
uint8_t audio_arg_code(uint16_t arg);
uint8_t audio_arg_level(uint16_t arg);
bool audio_arg_marker_vibra(uint16_t arg);

#endif

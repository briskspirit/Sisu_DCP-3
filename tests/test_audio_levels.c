#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "audio/audio_levels.h"
#include "hal/keypad.h"

static int s_failures;

static void assert_true(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        s_failures++;
    }
}

static void assert_eq_u(unsigned got, unsigned want, const char *message) {
    if (got != want) {
        fprintf(stderr, "FAIL: %s (got %u want %u)\n", message, got, want);
        s_failures++;
    }
}

/* Ringing volume -> 0..AUDIO_LEVEL_MAX. The setting is stored either on a 1..5
 * scale (legacy) or the original's 1..10 scale; both collapse to 0..5, with
 * 0 / 255 = silent and anything above 10 clamped to MAX. */
static void test_ringing_volume(void) {
    assert_eq_u(audio_level_from_ringing_volume(0u), AUDIO_LEVEL_SILENT, "ring vol 0 silent");
    assert_eq_u(audio_level_from_ringing_volume(255u), AUDIO_LEVEL_SILENT, "ring vol 255 silent");
    /* 1..5 pass through */
    for (uint8_t v = 1u; v <= AUDIO_LEVEL_MAX; v++) {
        assert_eq_u(audio_level_from_ringing_volume(v), v, "ring vol 1..5 passthrough");
    }
    /* 6..10 map to 1..5 (the original's scale) */
    assert_eq_u(audio_level_from_ringing_volume(6u), 1u, "ring vol 6 -> 1");
    assert_eq_u(audio_level_from_ringing_volume(10u), 5u, "ring vol 10 -> 5");
    /* >10 (but not 255) clamps to MAX */
    assert_eq_u(audio_level_from_ringing_volume(11u), AUDIO_LEVEL_MAX, "ring vol 11 -> MAX");
    assert_eq_u(audio_level_from_ringing_volume(254u), AUDIO_LEVEL_MAX, "ring vol 254 -> MAX");
    /* every output is within range */
    for (unsigned v = 0u; v <= 255u; v++) {
        assert_true(audio_level_from_ringing_volume((uint8_t)v) <= AUDIO_LEVEL_MAX,
                    "ring vol output in range");
    }
}

static void test_keypad_tones(void) {
    assert_eq_u(audio_level_from_keypad_tones(255u), AUDIO_LEVEL_SILENT, "keypad 255 silent");
    assert_eq_u(audio_level_from_keypad_tones(0u), 1u, "keypad 0 -> 1");
    assert_eq_u(audio_level_from_keypad_tones(1u), 2u, "keypad 1 -> 2");
    assert_eq_u(audio_level_from_keypad_tones(2u), 4u, "keypad 2 -> 4");
    assert_eq_u(audio_level_from_keypad_tones(254u), 4u, "keypad 254 -> 4");
    for (unsigned v = 0u; v <= 255u; v++) {
        assert_true(audio_level_from_keypad_tones((uint8_t)v) <= AUDIO_LEVEL_MAX,
                    "keypad output in range");
    }

    assert_eq_u(audio_keypad_level_gain_q8(AUDIO_LEVEL_SILENT), 0u,
                "silent keypad level has no gain");
    assert_eq_u(audio_keypad_level_gain_q8(1u), 149u,
                "stock keypad Level 1 trim is exact");
    assert_eq_u(audio_keypad_level_gain_q8(2u), 225u,
                "stock keypad Level 2 trim is exact");
    assert_eq_u(audio_keypad_level_gain_q8(4u), 256u,
                "stock keypad Level 3 is untrimmed");
    assert_eq_u(audio_keypad_level_gain_q8(AUDIO_LEVEL_MAX), 256u,
                "unexpected loud keypad levels stay bounded at unity");
}

static void test_call_volume_ladders(void) {
    static const uint8_t handset_db[AUDIO_CALL_VOLUME_MAX] = {
        9u, 8u, 7u, 6u, 5u, 4u, 3u, 2u, 1u, 0u,
    };
    static const uint8_t headset_db[AUDIO_CALL_VOLUME_MAX] = {
        18u, 16u, 14u, 12u, 10u, 8u, 6u, 4u, 2u, 0u,
    };

    assert_eq_u(AUDIO_CALL_VOLUME_MAX, 10u, "stock call ladder has ten levels");
    for (uint8_t level = AUDIO_CALL_VOLUME_MIN;
         level <= AUDIO_CALL_VOLUME_MAX; level++) {
        unsigned index = (unsigned)(level - AUDIO_CALL_VOLUME_MIN);
        assert_eq_u(audio_call_handset_attenuation_db(level), handset_db[index],
                    "handset call attenuation follows the 1 dB ROM bank");
        assert_eq_u(audio_call_headset_attenuation_db(level), headset_db[index],
                    "headset call attenuation follows the 2 dB ROM bank");
    }

    assert_eq_u(audio_call_handset_attenuation_db(0u), handset_db[0],
                "handset level below range clamps low");
    assert_eq_u(audio_call_headset_attenuation_db(0u), headset_db[0],
                "headset level below range clamps low");
    assert_eq_u(audio_call_handset_attenuation_db(UINT8_MAX), 0u,
                "handset level above range clamps high");
    assert_eq_u(audio_call_headset_attenuation_db(UINT8_MAX), 0u,
                "headset level above range clamps high");
}

/* audio_arg packs code (low byte) + level (high byte, clamped to MAX). */
static void test_arg_pack_unpack(void) {
    for (unsigned code = 0u; code <= 255u; code++) {
        for (unsigned level = 0u; level <= 12u; level++) {
            uint16_t arg = audio_arg((uint8_t)code, (uint8_t)level);
            uint8_t clamped = level > AUDIO_LEVEL_MAX ? AUDIO_LEVEL_MAX : (uint8_t)level;
            assert_eq_u(audio_arg_code(arg), code, "arg code round-trips");
            assert_eq_u(audio_arg_level(arg), clamped, "arg level round-trips (clamped)");
        }
    }
    /* level always clamped on the way out even if a raw arg encodes >MAX */
    assert_eq_u(audio_arg_level((uint16_t)(0xff << 8)), AUDIO_LEVEL_MAX, "raw arg level clamps");

    uint16_t marked = audio_arg_with_marker_vibra(0xa5u, 4u, true);
    assert_eq_u(audio_arg_code(marked), 0xa5u, "marker arg preserves code");
    assert_eq_u(audio_arg_level(marked), 4u, "marker arg preserves level");
    assert_true(audio_arg_marker_vibra(marked), "marker arg carries enabled gate");

    uint16_t unmarked = audio_arg_with_marker_vibra(0xa5u, 4u, false);
    assert_eq_u(unmarked, audio_arg(0xa5u, 4u), "disabled marker arg is wire-compatible");
    assert_true(!audio_arg_marker_vibra(unmarked), "disabled marker gate stays clear");
}

/* key id <-> char round-trip for the 12 dialable keys; non-keys map to NONE/0. */
static void test_key_id_char(void) {
    const uint16_t keys[] = {KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6,
                             KEY_7, KEY_8, KEY_9, KEY_STAR, KEY_0, KEY_HASH};
    const char chars[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9', '*', '0', '#'};
    for (unsigned i = 0u; i < sizeof(keys) / sizeof(keys[0]); i++) {
        uint8_t id = audio_key_id_from_key(keys[i]);
        assert_true(id != 0u, "dial key has a non-zero id");
        assert_eq_u((unsigned char)audio_key_char_from_id(id), (unsigned char)chars[i],
                    "key id -> expected char");
    }
    /* a non-dial key (navi/power-ish high value) yields NONE -> char 0 */
    assert_eq_u(audio_key_id_from_key(0xfffeu), 0u, "non-dial key -> id 0");
    assert_eq_u((unsigned char)audio_key_char_from_id(0u), 0u, "id 0 -> char 0");
    /* audio_arg_for_key composes key->id and level */
    uint16_t arg = audio_arg_for_key(KEY_5, 3u);
    assert_eq_u(audio_arg_code(arg), audio_key_id_from_key(KEY_5), "arg_for_key code");
    assert_eq_u(audio_arg_level(arg), 3u, "arg_for_key level");

    uint16_t power_arg = audio_arg_for_key(KEY_POWER, 2u);
    uint16_t clear_arg = audio_arg_for_key(KEY_C, 2u);
    assert_eq_u(power_arg, clear_arg,
                "power key uses the same ordinary click payload as C");
    assert_eq_u(audio_arg_code(power_arg), 0u,
                "power key cannot masquerade as an overlapping DTMF key");
}

int main(void) {
    test_ringing_volume();
    test_keypad_tones();
    test_call_volume_ladders();
    test_arg_pack_unpack();
    test_key_id_char();
    if (s_failures == 0) {
        printf("audio_levels tests passed\n");
        return 0;
    }
    fprintf(stderr, "%d audio_levels assertion(s) failed\n", s_failures);
    return 1;
}

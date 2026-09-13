#include "audio/audio_levels.h"

#include "services/input_keys.h"

#define AUDIO_ARG_MARKER_VIBRA (1u << 15)

enum {
    AUDIO_KEY_NONE = 0,
    AUDIO_KEY_1,
    AUDIO_KEY_2,
    AUDIO_KEY_3,
    AUDIO_KEY_4,
    AUDIO_KEY_5,
    AUDIO_KEY_6,
    AUDIO_KEY_7,
    AUDIO_KEY_8,
    AUDIO_KEY_9,
    AUDIO_KEY_STAR,
    AUDIO_KEY_0,
    AUDIO_KEY_HASH,
};

uint8_t audio_level_from_ringing_volume(uint8_t value) {
    if (value == 0u || value == 255u) {
        return AUDIO_LEVEL_SILENT;
    }
    if (value >= 6u && value <= 10u) {
        return (uint8_t)(value - 5u);
    }
    if (value <= AUDIO_LEVEL_MAX) {
        return value;
    }
    return AUDIO_LEVEL_MAX;
}

uint8_t audio_level_from_keypad_tones(uint8_t value) {
    if (value == 255u) {
        return AUDIO_LEVEL_SILENT;
    }
    if (value == 0u) {
        return 1u;
    }
    if (value == 1u) {
        return 2u;
    }
    return 4u;
}

uint16_t audio_keypad_level_gain_q8(uint8_t level) {
    if (level == AUDIO_LEVEL_SILENT) {
        return 0u;
    }
    if (level == 1u) {
        return AUDIO_KEYPAD_LEVEL_LOW_GAIN_Q8;
    }
    if (level == 2u) {
        return AUDIO_KEYPAD_LEVEL_MID_GAIN_Q8;
    }
    return AUDIO_KEYPAD_LEVEL_HIGH_GAIN_Q8;
}

static uint8_t call_volume_attenuation_db(uint8_t level, uint8_t step_db) {
    if (level < AUDIO_CALL_VOLUME_MIN) {
        level = AUDIO_CALL_VOLUME_MIN;
    } else if (level > AUDIO_CALL_VOLUME_MAX) {
        level = AUDIO_CALL_VOLUME_MAX;
    }
    return (uint8_t)((AUDIO_CALL_VOLUME_MAX - level) * step_db);
}

uint8_t audio_call_handset_attenuation_db(uint8_t level) {
    return call_volume_attenuation_db(level, AUDIO_CALL_HANDSET_STEP_DB);
}

uint8_t audio_call_headset_attenuation_db(uint8_t level) {
    return call_volume_attenuation_db(level, AUDIO_CALL_HEADSET_STEP_DB);
}

uint8_t audio_key_id_from_key(uint16_t key) {
    switch (key) {
    case KEY_1: return AUDIO_KEY_1;
    case KEY_2: return AUDIO_KEY_2;
    case KEY_3: return AUDIO_KEY_3;
    case KEY_4: return AUDIO_KEY_4;
    case KEY_5: return AUDIO_KEY_5;
    case KEY_6: return AUDIO_KEY_6;
    case KEY_7: return AUDIO_KEY_7;
    case KEY_8: return AUDIO_KEY_8;
    case KEY_9: return AUDIO_KEY_9;
    case KEY_STAR: return AUDIO_KEY_STAR;
    case KEY_0: return AUDIO_KEY_0;
    case KEY_HASH: return AUDIO_KEY_HASH;
    default: return AUDIO_KEY_NONE;
    }
}

char audio_key_char_from_id(uint8_t key_id) {
    switch (key_id) {
    case AUDIO_KEY_1: return '1';
    case AUDIO_KEY_2: return '2';
    case AUDIO_KEY_3: return '3';
    case AUDIO_KEY_4: return '4';
    case AUDIO_KEY_5: return '5';
    case AUDIO_KEY_6: return '6';
    case AUDIO_KEY_7: return '7';
    case AUDIO_KEY_8: return '8';
    case AUDIO_KEY_9: return '9';
    case AUDIO_KEY_STAR: return '*';
    case AUDIO_KEY_0: return '0';
    case AUDIO_KEY_HASH: return '#';
    default: return 0;
    }
}

uint16_t audio_arg(uint8_t code, uint8_t level) {
    if (level > AUDIO_LEVEL_MAX) {
        level = AUDIO_LEVEL_MAX;
    }
    return (uint16_t)(code | ((uint16_t)level << 8));
}

uint16_t audio_arg_with_marker_vibra(uint8_t code, uint8_t level, bool enabled) {
    uint16_t arg = audio_arg(code, level);
    return enabled ? (uint16_t)(arg | AUDIO_ARG_MARKER_VIBRA) : arg;
}

uint16_t audio_arg_for_key(uint16_t key, uint8_t level) {
    return audio_arg(audio_key_id_from_key(key), level);
}

uint8_t audio_arg_code(uint16_t arg) {
    return (uint8_t)(arg & 0xffu);
}

uint8_t audio_arg_level(uint16_t arg) {
    uint8_t level = (uint8_t)((arg >> 8) & 0x7fu);
    return level > AUDIO_LEVEL_MAX ? AUDIO_LEVEL_MAX : level;
}

bool audio_arg_marker_vibra(uint16_t arg) {
    return (arg & AUDIO_ARG_MARKER_VIBRA) != 0u;
}

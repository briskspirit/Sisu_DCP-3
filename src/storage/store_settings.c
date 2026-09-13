#include "storage/store_service.h"

#include "storage_bytes.h"
#include "store_service_internal.h"

#include <string.h>

#define SETTINGS_MAGIC 0x53455431u
#define PHONEBOOK_TONES_BLOCK 0x544fu
#define STORE_CONTACT_TONE_LIMIT 32u

typedef enum {
    SETTING_TYPE_U8 = 1,
    SETTING_TYPE_U16,
    SETTING_TYPE_U32,
    SETTING_TYPE_TEXT,
} setting_type_t;

typedef struct {
    store_setting_key_t key;
    store_domain_t domain;
    setting_type_t type;
    uint32_t default_num;
    const char *default_text;
} setting_meta_t;

typedef struct {
    setting_type_t type;
    uint32_t num;
    char text[STORE_TEXT_MAX + 1u];
} setting_value_t;

typedef struct {
    bool used;
    uint16_t contact_index;
    uint8_t value; /* v6.00 ringing-tone value byte; STORE_CONTACT_TONE_NO_TONE = silent */
} contact_tone_t;

static store_status_t mark_settings_domain_dirty(store_domain_t domain);
static const setting_meta_t *setting_meta(store_setting_key_t key);
static store_unit_t unit_for_domain(store_domain_t domain);
static bool serialize_settings_domain(store_domain_t domain,
                                      uint8_t *dst,
                                      size_t cap,
                                      size_t *out_len);
static bool apply_settings_payload(store_domain_t domain,
                                   const uint8_t *payload,
                                   size_t len);
static bool walk_settings_payload(store_domain_t domain,
                                  const uint8_t *payload,
                                  size_t len,
                                  bool publish);
static store_setting_key_t speed_dial_key(uint8_t key);
static int find_contact_tone(uint16_t contact_index);

static const setting_meta_t SETTINGS_META[] = {
    {STORE_SETTING_PHONEBOOK_VIEW_MODE, STORE_DOMAIN_PHONEBOOK, SETTING_TYPE_U8, 0u, 0},
    {STORE_SETTING_PHONEBOOK_MEMORY, STORE_DOMAIN_PHONEBOOK, SETTING_TYPE_U8, 0u, 0},
    {STORE_SETTING_SMS_DEFAULT_PROFILE, STORE_DOMAIN_SMS, SETTING_TYPE_U8, 0u, 0},
    {STORE_SETTING_SMS_INFO_SERVICE, STORE_DOMAIN_SMS, SETTING_TYPE_U8, 0u, 0},
    {STORE_SETTING_SMS_MESSAGE_CENTRE, STORE_DOMAIN_SMS, SETTING_TYPE_TEXT, 0u, ""},
    /* Sent-as / Validity / Delivery-reports are intentionally INERT: stored and
     * shown in the menu but never read into a modem command. This is 1:1 with
     * v6.00, where the ROM never wires these settings into the SMS path. */
    {STORE_SETTING_SMS_SENT_AS, STORE_DOMAIN_SMS, SETTING_TYPE_U8, 0u, 0},
    {STORE_SETTING_SMS_VALIDITY, STORE_DOMAIN_SMS, SETTING_TYPE_U8, 5u, 0},
    {STORE_SETTING_SMS_DELIVERY_REPORTS, STORE_DOMAIN_SMS, SETTING_TYPE_U8, 0u, 0},
    {STORE_SETTING_SMS_REPLY_SAME_CENTRE, STORE_DOMAIN_SMS, SETTING_TYPE_U8, 0u, 0},
    {STORE_SETTING_CALL_OWN_NUMBER_SENDING, STORE_DOMAIN_CALLS, SETTING_TYPE_U8, 0u, 0},
    {STORE_SETTING_CALL_SUMMARY_AFTER_CALL, STORE_DOMAIN_CALLS, SETTING_TYPE_U8, 0u, 0},
    {STORE_SETTING_CALL_TIMERS_CLEARED, STORE_DOMAIN_CALLS, SETTING_TYPE_U8, 0u, 0},
    {STORE_SETTING_CALL_COUNTERS_CLEARED, STORE_DOMAIN_CALLS, SETTING_TYPE_U8, 0u, 0},
    {STORE_SETTING_CALL_CREDIT_INFO_DISPLAY, STORE_DOMAIN_CALLS, SETTING_TYPE_U8, 0u, 0},
    {STORE_SETTING_PROFILE_ACTIVE, STORE_DOMAIN_PROFILES, SETTING_TYPE_U8, 0u, 0},
    {STORE_SETTING_CLOCK_SHOW_STANDBY, STORE_DOMAIN_CLOCK, SETTING_TYPE_U8, 1u, 0},
    {STORE_SETTING_CLOCK_FORMAT_24H, STORE_DOMAIN_CLOCK, SETTING_TYPE_U8, 1u, 0},
    /* Defaults to 0 so a cold boot with no stored clock runs the RTC-lost
     * Time:/Date: setup flow; set_clock_time() latches it to 1. */
    {STORE_SETTING_CLOCK_TIME_SET, STORE_DOMAIN_CLOCK, SETTING_TYPE_U8, 0u, 0},
    {STORE_SETTING_CLOCK_HOUR, STORE_DOMAIN_CLOCK, SETTING_TYPE_U8, 18u, 0},
    {STORE_SETTING_CLOCK_MINUTE, STORE_DOMAIN_CLOCK, SETTING_TYPE_U8, 21u, 0},
    {STORE_SETTING_CLOCK_DATE_DAY, STORE_DOMAIN_CLOCK, SETTING_TYPE_U8, 1u, 0},
    {STORE_SETTING_CLOCK_DATE_MONTH, STORE_DOMAIN_CLOCK, SETTING_TYPE_U8, 5u, 0},
    {STORE_SETTING_CLOCK_DATE_YEAR, STORE_DOMAIN_CLOCK, SETTING_TYPE_U16, 2026u, 0},
    {STORE_SETTING_CLOCK_ALARM_ENABLED, STORE_DOMAIN_CLOCK, SETTING_TYPE_U8, 0u, 0},
    {STORE_SETTING_CLOCK_ALARM_HOUR, STORE_DOMAIN_CLOCK, SETTING_TYPE_U8, 7u, 0},
    {STORE_SETTING_CLOCK_ALARM_MINUTE, STORE_DOMAIN_CLOCK, SETTING_TYPE_U8, 30u, 0},
    {STORE_SETTING_SYSTEM_LANGUAGE, STORE_DOMAIN_SYSTEM, SETTING_TYPE_U8, 0u, 0}, /* runtime language id: 0 = Automatic (resolves to English until auto-detect logic exists) */
    {STORE_SETTING_SYSTEM_WELCOME_NOTE, STORE_DOMAIN_SYSTEM, SETTING_TYPE_TEXT, 0u, ""},
    {STORE_SETTING_SYSTEM_VOICE_MAILBOX_NUMBER, STORE_DOMAIN_SYSTEM, SETTING_TYPE_TEXT, 0u, ""},
    {STORE_SETTING_SPEED_DIAL_1, STORE_DOMAIN_PHONEBOOK, SETTING_TYPE_U16, STORE_SPEED_DIAL_EMPTY, 0},
    {STORE_SETTING_SPEED_DIAL_2, STORE_DOMAIN_PHONEBOOK, SETTING_TYPE_U16, STORE_SPEED_DIAL_EMPTY, 0},
    {STORE_SETTING_SPEED_DIAL_3, STORE_DOMAIN_PHONEBOOK, SETTING_TYPE_U16, STORE_SPEED_DIAL_EMPTY, 0},
    {STORE_SETTING_SPEED_DIAL_4, STORE_DOMAIN_PHONEBOOK, SETTING_TYPE_U16, STORE_SPEED_DIAL_EMPTY, 0},
    {STORE_SETTING_SPEED_DIAL_5, STORE_DOMAIN_PHONEBOOK, SETTING_TYPE_U16, STORE_SPEED_DIAL_EMPTY, 0},
    {STORE_SETTING_SPEED_DIAL_6, STORE_DOMAIN_PHONEBOOK, SETTING_TYPE_U16, STORE_SPEED_DIAL_EMPTY, 0},
    {STORE_SETTING_SPEED_DIAL_7, STORE_DOMAIN_PHONEBOOK, SETTING_TYPE_U16, STORE_SPEED_DIAL_EMPTY, 0},
    {STORE_SETTING_SPEED_DIAL_8, STORE_DOMAIN_PHONEBOOK, SETTING_TYPE_U16, STORE_SPEED_DIAL_EMPTY, 0},
    {STORE_SETTING_SPEED_DIAL_9, STORE_DOMAIN_PHONEBOOK, SETTING_TYPE_U16, STORE_SPEED_DIAL_EMPTY, 0},
    {STORE_SETTING_SMS_DICTIONARY_ACTIVE, STORE_DOMAIN_SMS, SETTING_TYPE_U8, 0u, 0},
    {STORE_SETTING_SMS_DICTIONARY_LANGUAGE, STORE_DOMAIN_SMS, SETTING_TYPE_U8, 0u, 0},
    {STORE_SETTING_PROFILE_INCOMING_ALERT, STORE_DOMAIN_PROFILES, SETTING_TYPE_U8, 1u, 0},
    {STORE_SETTING_PROFILE_RINGING_TONE, STORE_DOMAIN_PROFILES, SETTING_TYPE_U8, 52u, 0},
    {STORE_SETTING_PROFILE_RINGING_VOLUME, STORE_DOMAIN_PROFILES, SETTING_TYPE_U8, 9u, 0},
    {STORE_SETTING_PROFILE_MESSAGE_ALERT, STORE_DOMAIN_PROFILES, SETTING_TYPE_U8, 0u, 0},
    {STORE_SETTING_PROFILE_KEYPAD_TONES, STORE_DOMAIN_PROFILES, SETTING_TYPE_U8, 1u, 0},
    {STORE_SETTING_PROFILE_WARNING_GAME_TONES, STORE_DOMAIN_PROFILES, SETTING_TYPE_U8, 4u, 0},
    {STORE_SETTING_PROFILE_VIBRATING_ALERT, STORE_DOMAIN_PROFILES, SETTING_TYPE_U8, 0u, 0},
    {STORE_SETTING_SETTINGS_AUTOMATIC_REDIAL, STORE_DOMAIN_CALLS, SETTING_TYPE_U8, 0u, 0},
    {STORE_SETTING_SETTINGS_SPEED_DIALLING, STORE_DOMAIN_CALLS, SETTING_TYPE_U8, 0u, 0},
    {STORE_SETTING_SETTINGS_PHONE_LINE, STORE_DOMAIN_CALLS, SETTING_TYPE_U8, 0u, 0},
    {STORE_SETTING_SETTINGS_AUTOMATIC_ANSWER, STORE_DOMAIN_CALLS, SETTING_TYPE_U8, 0u, 0},
    {STORE_SETTING_SETTINGS_CELL_INFO_DISPLAY, STORE_DOMAIN_SYSTEM, SETTING_TYPE_U8, 0u, 0},
    {STORE_SETTING_SETTINGS_NETWORK_SELECTION, STORE_DOMAIN_SYSTEM, SETTING_TYPE_U8, 0u, 0},
    {STORE_SETTING_SETTINGS_LIGHTS, STORE_DOMAIN_SYSTEM, SETTING_TYPE_U8, 0u, 0},
    {STORE_SETTING_SETTINGS_CONFIRM_SIM_ACTIONS, STORE_DOMAIN_SYSTEM, SETTING_TYPE_U8, 0u, 0},
    {STORE_SETTING_SETTINGS_PIN_CODE_REQUEST, STORE_DOMAIN_SYSTEM, SETTING_TYPE_U8, 0u, 0},
    {STORE_SETTING_SETTINGS_FIXED_DIALLING, STORE_DOMAIN_SYSTEM, SETTING_TYPE_U8, 0u, 0},
    {STORE_SETTING_SETTINGS_CLOSED_USER_GROUP, STORE_DOMAIN_SYSTEM, SETTING_TYPE_U8, 0u, 0},
    {STORE_SETTING_SETTINGS_PHONE_SECURITY, STORE_DOMAIN_SYSTEM, SETTING_TYPE_U8, 17u, 0},
    {STORE_SETTING_SETTINGS_PHONE_LINE_CHANGE_ALLOWED, STORE_DOMAIN_SYSTEM, SETTING_TYPE_U8, 1u, 0},
    {STORE_SETTING_CALCULATOR_EXCHANGE_RATE, STORE_DOMAIN_SYSTEM, SETTING_TYPE_TEXT, 0u, "1"},
    {STORE_SETTING_GAMES_SNAKE_LEVEL, STORE_DOMAIN_SYSTEM, SETTING_TYPE_U8, 1u, 0},
    {STORE_SETTING_GAMES_SNAKE_TOP_SCORE, STORE_DOMAIN_SYSTEM, SETTING_TYPE_U16, 0u, 0},
    {STORE_SETTING_GAMES_ROTATION_LEVEL, STORE_DOMAIN_SYSTEM, SETTING_TYPE_U8, 1u, 0},
    {STORE_SETTING_GAMES_ROTATION_TOP_SCORE, STORE_DOMAIN_SYSTEM, SETTING_TYPE_U16, 0u, 0},
    {STORE_SETTING_GAMES_MEMORY_LEVEL, STORE_DOMAIN_SYSTEM, SETTING_TYPE_U8, 0u, 0},
    {STORE_SETTING_GAMES_MEMORY_TOP_SCORE, STORE_DOMAIN_SYSTEM, SETTING_TYPE_U16, 0u, 0},
    {STORE_SETTING_GAMES_REACT_LEVEL, STORE_DOMAIN_SYSTEM, SETTING_TYPE_U8, 0u, 0},
    {STORE_SETTING_GAMES_REACT_TOP_SCORE, STORE_DOMAIN_SYSTEM, SETTING_TYPE_U16, 0u, 0},
    {STORE_SETTING_GAMES_LOGIC_LEVEL, STORE_DOMAIN_SYSTEM, SETTING_TYPE_U8, 0u, 0},
    {STORE_SETTING_GAMES_PACMAN_LEVEL, STORE_DOMAIN_SYSTEM, SETTING_TYPE_U8, 0u, 0},
    {STORE_SETTING_GAMES_PACMAN_TOP_SCORE, STORE_DOMAIN_SYSTEM, SETTING_TYPE_U16, 0u, 0},
    {STORE_SETTING_CALL_DURATION_LAST, STORE_DOMAIN_CALLS, SETTING_TYPE_U32, 0u, 0},
    {STORE_SETTING_CALL_DURATION_ALL, STORE_DOMAIN_CALLS, SETTING_TYPE_U32, 0u, 0},
    {STORE_SETTING_CALL_DURATION_RECEIVED, STORE_DOMAIN_CALLS, SETTING_TYPE_U32, 0u, 0},
    {STORE_SETTING_CALL_DURATION_DIALLED, STORE_DOMAIN_CALLS, SETTING_TYPE_U32, 0u, 0},
    {STORE_SETTING_SECURITY_CODE, STORE_DOMAIN_SYSTEM, SETTING_TYPE_TEXT, 0u, "12345"},
    {STORE_SETTING_PROFILE_PERSONAL_INCOMING_ALERT, STORE_DOMAIN_PROFILES, SETTING_TYPE_U8, 1u, 0},
    {STORE_SETTING_PROFILE_PERSONAL_RINGING_TONE, STORE_DOMAIN_PROFILES, SETTING_TYPE_U8, 52u, 0},
    {STORE_SETTING_PROFILE_PERSONAL_RINGING_VOLUME, STORE_DOMAIN_PROFILES, SETTING_TYPE_U8, 9u, 0},
    {STORE_SETTING_PROFILE_PERSONAL_MESSAGE_ALERT, STORE_DOMAIN_PROFILES, SETTING_TYPE_U8, 0u, 0},
    {STORE_SETTING_PROFILE_PERSONAL_KEYPAD_TONES, STORE_DOMAIN_PROFILES, SETTING_TYPE_U8, 1u, 0},
    {STORE_SETTING_PROFILE_PERSONAL_WARNING_GAME_TONES, STORE_DOMAIN_PROFILES, SETTING_TYPE_U8, 4u, 0},
    {STORE_SETTING_PROFILE_PERSONAL_VIBRATING_ALERT, STORE_DOMAIN_PROFILES, SETTING_TYPE_U8, 0u, 0},
    {STORE_SETTING_PROFILE_SILENT_INCOMING_ALERT, STORE_DOMAIN_PROFILES, SETTING_TYPE_U8, 4u, 0},
    {STORE_SETTING_PROFILE_SILENT_RINGING_TONE, STORE_DOMAIN_PROFILES, SETTING_TYPE_U8, 52u, 0},
    {STORE_SETTING_PROFILE_SILENT_RINGING_VOLUME, STORE_DOMAIN_PROFILES, SETTING_TYPE_U8, 8u, 0},
    {STORE_SETTING_PROFILE_SILENT_MESSAGE_ALERT, STORE_DOMAIN_PROFILES, SETTING_TYPE_U8, 0u, 0},
    {STORE_SETTING_PROFILE_SILENT_KEYPAD_TONES, STORE_DOMAIN_PROFILES, SETTING_TYPE_U8, 255u, 0},
    {STORE_SETTING_PROFILE_SILENT_WARNING_GAME_TONES, STORE_DOMAIN_PROFILES, SETTING_TYPE_U8, 255u, 0},
    {STORE_SETTING_PROFILE_SILENT_VIBRATING_ALERT, STORE_DOMAIN_PROFILES, SETTING_TYPE_U8, 0u, 0},
    {STORE_SETTING_PROFILE_DISCREET_INCOMING_ALERT, STORE_DOMAIN_PROFILES, SETTING_TYPE_U8, 1u, 0},
    {STORE_SETTING_PROFILE_DISCREET_RINGING_TONE, STORE_DOMAIN_PROFILES, SETTING_TYPE_U8, 52u, 0},
    {STORE_SETTING_PROFILE_DISCREET_RINGING_VOLUME, STORE_DOMAIN_PROFILES, SETTING_TYPE_U8, 8u, 0},
    {STORE_SETTING_PROFILE_DISCREET_MESSAGE_ALERT, STORE_DOMAIN_PROFILES, SETTING_TYPE_U8, 0u, 0},
    {STORE_SETTING_PROFILE_DISCREET_KEYPAD_TONES, STORE_DOMAIN_PROFILES, SETTING_TYPE_U8, 1u, 0},
    {STORE_SETTING_PROFILE_DISCREET_WARNING_GAME_TONES, STORE_DOMAIN_PROFILES, SETTING_TYPE_U8, 4u, 0},
    {STORE_SETTING_PROFILE_DISCREET_VIBRATING_ALERT, STORE_DOMAIN_PROFILES, SETTING_TYPE_U8, 0u, 0},
    {STORE_SETTING_PROFILE_LOUD_INCOMING_ALERT, STORE_DOMAIN_PROFILES, SETTING_TYPE_U8, 1u, 0},
    {STORE_SETTING_PROFILE_LOUD_RINGING_TONE, STORE_DOMAIN_PROFILES, SETTING_TYPE_U8, 52u, 0},
    {STORE_SETTING_PROFILE_LOUD_RINGING_VOLUME, STORE_DOMAIN_PROFILES, SETTING_TYPE_U8, 9u, 0},
    {STORE_SETTING_PROFILE_LOUD_MESSAGE_ALERT, STORE_DOMAIN_PROFILES, SETTING_TYPE_U8, 0u, 0},
    {STORE_SETTING_PROFILE_LOUD_KEYPAD_TONES, STORE_DOMAIN_PROFILES, SETTING_TYPE_U8, 1u, 0},
    {STORE_SETTING_PROFILE_LOUD_WARNING_GAME_TONES, STORE_DOMAIN_PROFILES, SETTING_TYPE_U8, 4u, 0},
    {STORE_SETTING_PROFILE_LOUD_VIBRATING_ALERT, STORE_DOMAIN_PROFILES, SETTING_TYPE_U8, 0u, 0},
    {STORE_SETTING_SERVICE_CODEC_BYTE, STORE_DOMAIN_SYSTEM, SETTING_TYPE_U8, 0x30u, 0},
    {STORE_SETTING_SERVICE_WARRANTY_PURCHASE_DATE, STORE_DOMAIN_SYSTEM, SETTING_TYPE_TEXT, 0u, ""},
    {STORE_SETTING_SYSTEM_NET_MONITOR_SELECTOR, STORE_DOMAIN_SYSTEM, SETTING_TYPE_U8, 0u, 0},
    {STORE_SETTING_PROFILE_HEADSET_INCOMING_ALERT, STORE_DOMAIN_PROFILES, SETTING_TYPE_U8, 1u, 0},
    {STORE_SETTING_PROFILE_HEADSET_RINGING_TONE, STORE_DOMAIN_PROFILES, SETTING_TYPE_U8, 52u, 0},
    {STORE_SETTING_PROFILE_HEADSET_RINGING_VOLUME, STORE_DOMAIN_PROFILES, SETTING_TYPE_U8, 8u, 0},
    {STORE_SETTING_PROFILE_HEADSET_MESSAGE_ALERT, STORE_DOMAIN_PROFILES, SETTING_TYPE_U8, 0u, 0},
    {STORE_SETTING_PROFILE_HEADSET_KEYPAD_TONES, STORE_DOMAIN_PROFILES, SETTING_TYPE_U8, 1u, 0},
    {STORE_SETTING_PROFILE_HEADSET_WARNING_GAME_TONES, STORE_DOMAIN_PROFILES, SETTING_TYPE_U8, 4u, 0},
    {STORE_SETTING_PROFILE_HEADSET_VIBRATING_ALERT, STORE_DOMAIN_PROFILES, SETTING_TYPE_U8, 0u, 0},
    {STORE_SETTING_PROFILE_SAVED, STORE_DOMAIN_PROFILES, SETTING_TYPE_U8, 0xffu, 0},
    {STORE_SETTING_CALL_VOLUME, STORE_DOMAIN_CALLS, SETTING_TYPE_U8, 5u, 0},
    /* Keep synchronized with LCD_CALIBRATION_STOCK_VOP. The storage layer
     * deliberately does not depend on the LCD service. */
    {STORE_SETTING_SYSTEM_LCD_VOP, STORE_DOMAIN_SYSTEM, SETTING_TYPE_U8, 63u, 0},
    {STORE_SETTING_SYSTEM_MODEM_PROVISION_VERSION, STORE_DOMAIN_SYSTEM,
     SETTING_TYPE_U16, 0u, 0},
    {STORE_SETTING_SYSTEM_NET_MONITOR_SCHEMA, STORE_DOMAIN_SYSTEM,
     SETTING_TYPE_U8, 0u, 0},
    /* 0x4c, version 1, bias 4, TC1, Vop 63. Keep synchronized with
     * LCD_CALIBRATION_STOCK_PACKED without adding an LCD dependency here. */
    {STORE_SETTING_SYSTEM_LCD_TUNING, STORE_DOMAIN_SYSTEM,
     SETTING_TYPE_U32, 0x4c0108bfu, 0},
    {STORE_SETTING_SYSTEM_NETMON_RADIO_RECOVERY, STORE_DOMAIN_SYSTEM,
     SETTING_TYPE_TEXT, 0u, ""},
    {STORE_SETTING_SYSTEM_BACKLIGHT_LEVEL, STORE_DOMAIN_SYSTEM,
     SETTING_TYPE_U8, 100u, 0},
    {STORE_SETTING_CALL_DURATION_LIFETIME, STORE_DOMAIN_CALLS,
     SETTING_TYPE_U32, 0u, 0},
};

_Static_assert(sizeof(SETTINGS_META) / sizeof(SETTINGS_META[0]) ==
                   STORE_SETTING_COUNT,
               "every store setting needs exactly one metadata entry");
_Static_assert(STORE_SETTING_COUNT <= UINT8_MAX,
               "settings serialization uses 8-bit keys and counts");

static setting_value_t s_settings[STORE_SETTING_COUNT];
static contact_tone_t s_contact_tones[STORE_CONTACT_TONE_LIMIT];

store_status_t store_setting_get_u8(store_setting_key_t key, uint8_t *out_value) {
    const setting_meta_t *meta = setting_meta(key);
    if (meta == 0 || out_value == 0) {
        return STORE_STATUS_INVALID_ARGUMENT;
    }
    if (meta->type != SETTING_TYPE_U8) {
        return STORE_STATUS_TYPE_MISMATCH;
    }
    *out_value = (uint8_t)s_settings[key].num;
    return STORE_STATUS_OK;
}

store_status_t store_setting_set_u8(store_setting_key_t key, uint8_t value) {
    const setting_meta_t *meta = setting_meta(key);
    if (meta == 0) {
        return STORE_STATUS_INVALID_ARGUMENT;
    }
    if (meta->type != SETTING_TYPE_U8) {
        return STORE_STATUS_TYPE_MISMATCH;
    }
    if (s_settings[key].num == value) {
        return STORE_STATUS_OK;
    }
    s_settings[key].num = value;
    return mark_settings_domain_dirty(meta->domain);
}

store_status_t store_setting_get_u16(store_setting_key_t key, uint16_t *out_value) {
    const setting_meta_t *meta = setting_meta(key);
    if (meta == 0 || out_value == 0) {
        return STORE_STATUS_INVALID_ARGUMENT;
    }
    if (meta->type != SETTING_TYPE_U16) {
        return STORE_STATUS_TYPE_MISMATCH;
    }
    *out_value = (uint16_t)s_settings[key].num;
    return STORE_STATUS_OK;
}

store_status_t store_setting_set_u16(store_setting_key_t key, uint16_t value) {
    const setting_meta_t *meta = setting_meta(key);
    if (meta == 0) {
        return STORE_STATUS_INVALID_ARGUMENT;
    }
    if (meta->type != SETTING_TYPE_U16) {
        return STORE_STATUS_TYPE_MISMATCH;
    }
    if (s_settings[key].num == value) {
        return STORE_STATUS_OK;
    }
    s_settings[key].num = value;
    return mark_settings_domain_dirty(meta->domain);
}

store_status_t store_setting_get_u32(store_setting_key_t key, uint32_t *out_value) {
    const setting_meta_t *meta = setting_meta(key);
    if (meta == 0 || out_value == 0) {
        return STORE_STATUS_INVALID_ARGUMENT;
    }
    if (meta->type != SETTING_TYPE_U32) {
        return STORE_STATUS_TYPE_MISMATCH;
    }
    *out_value = s_settings[key].num;
    return STORE_STATUS_OK;
}

store_status_t store_setting_set_u32(store_setting_key_t key, uint32_t value) {
    const setting_meta_t *meta = setting_meta(key);
    if (meta == 0) {
        return STORE_STATUS_INVALID_ARGUMENT;
    }
    if (meta->type != SETTING_TYPE_U32) {
        return STORE_STATUS_TYPE_MISMATCH;
    }
    if (s_settings[key].num == value) {
        return STORE_STATUS_OK;
    }
    s_settings[key].num = value;
    return mark_settings_domain_dirty(meta->domain);
}

store_status_t store_setting_get_text(store_setting_key_t key, char *out_text, uint8_t out_cap) {
    const setting_meta_t *meta = setting_meta(key);
    if (meta == 0 || out_text == 0 || out_cap == 0) {
        return STORE_STATUS_INVALID_ARGUMENT;
    }
    if (meta->type != SETTING_TYPE_TEXT) {
        return STORE_STATUS_TYPE_MISMATCH;
    }
    store_copy_text(out_text, out_cap, s_settings[key].text);
    return STORE_STATUS_OK;
}

store_status_t store_setting_set_text(store_setting_key_t key, const char *text) {
    const setting_meta_t *meta = setting_meta(key);
    if (meta == 0 || text == 0) {
        return STORE_STATUS_INVALID_ARGUMENT;
    }
    if (meta->type != SETTING_TYPE_TEXT) {
        return STORE_STATUS_TYPE_MISMATCH;
    }
    char normalized[STORE_TEXT_MAX + 1u];
    store_copy_text(normalized, sizeof(normalized), text);
    if (strncmp(s_settings[key].text, normalized, sizeof(normalized)) == 0) {
        return STORE_STATUS_OK;
    }
    store_copy_text(s_settings[key].text, sizeof(s_settings[key].text), normalized);
    return mark_settings_domain_dirty(meta->domain);
}

store_status_t store_phonebook_get_speed_dial(uint8_t key, uint16_t *out_contact_index) {
    if (out_contact_index == 0) {
        return STORE_STATUS_INVALID_ARGUMENT;
    }
    store_setting_key_t setting = speed_dial_key(key);
    if (setting >= STORE_SETTING_COUNT) {
        return STORE_STATUS_INVALID_ARGUMENT;
    }
    uint16_t value = STORE_SPEED_DIAL_EMPTY;
    store_status_t status = store_setting_get_u16(setting, &value);
    if (status != STORE_STATUS_OK) {
        return status;
    }
    if (value == STORE_SPEED_DIAL_EMPTY) {
        return STORE_STATUS_NOT_FOUND;
    }
    *out_contact_index = value;
    return STORE_STATUS_OK;
}

store_status_t store_phonebook_set_speed_dial(uint8_t key, uint16_t contact_index) {
    store_setting_key_t setting = speed_dial_key(key);
    if (setting >= STORE_SETTING_COUNT || contact_index == STORE_SPEED_DIAL_EMPTY) {
        return STORE_STATUS_INVALID_ARGUMENT;
    }
    return store_setting_set_u16(setting, contact_index);
}

store_status_t store_phonebook_clear_speed_dial(uint8_t key) {
    store_setting_key_t setting = speed_dial_key(key);
    if (setting >= STORE_SETTING_COUNT) {
        return STORE_STATUS_INVALID_ARGUMENT;
    }
    return store_setting_set_u16(setting, STORE_SPEED_DIAL_EMPTY);
}

uint8_t store_phonebook_get_contact_tone_value(uint16_t contact_index) {
    int index = find_contact_tone(contact_index);
    return index >= 0 ? s_contact_tones[index].value : STORE_CONTACT_TONE_PRESET;
}

store_status_t store_phonebook_set_contact_tone_value(uint16_t contact_index, uint8_t value) {
    if (contact_index == 0u) {
        return STORE_STATUS_INVALID_ARGUMENT;
    }
    int index = find_contact_tone(contact_index);
    if (value == STORE_CONTACT_TONE_PRESET) {
        if (index >= 0) {
            memset(&s_contact_tones[index], 0, sizeof(s_contact_tones[index]));
            return mark_settings_domain_dirty(STORE_DOMAIN_PHONEBOOK);
        }
        return STORE_STATUS_OK;
    }
    if (index < 0) {
        for (uint8_t i = 0; i < STORE_CONTACT_TONE_LIMIT; i++) {
            if (!s_contact_tones[i].used) {
                index = i;
                break;
            }
        }
    }
    if (index < 0) {
        return STORE_STATUS_STORAGE_ERROR;
    }
    s_contact_tones[index].used = true;
    s_contact_tones[index].contact_index = contact_index;
    s_contact_tones[index].value = value;
    return mark_settings_domain_dirty(STORE_DOMAIN_PHONEBOOK);
}

static store_status_t mark_settings_domain_dirty(store_domain_t domain) {
    if (domain >= STORE_DOMAIN_COUNT) {
        return STORE_STATUS_INVALID_ARGUMENT;
    }
    return store_engine_mark_dirty(unit_for_domain(domain));
}

static void reset_settings_unit(uint8_t instance) {
    store_domain_t domain = (store_domain_t)instance;
    if (domain >= STORE_DOMAIN_COUNT) {
        return;
    }
    for (uint8_t i = 0u; i < STORE_SETTING_COUNT; i++) {
        const setting_meta_t *meta = setting_meta((store_setting_key_t)i);
        if (meta == 0 || meta->domain != domain) {
            continue;
        }
        memset(&s_settings[i], 0, sizeof(s_settings[i]));
        s_settings[i].type = meta->type;
        s_settings[i].num = meta->default_num;
        if (meta->type == SETTING_TYPE_TEXT) {
            store_copy_text(s_settings[i].text, sizeof(s_settings[i].text),
                      meta->default_text);
        }
    }
    if (domain == STORE_DOMAIN_PHONEBOOK) {
        memset(s_contact_tones, 0, sizeof(s_contact_tones));
    }
}

static const setting_meta_t *setting_meta(store_setting_key_t key) {
    if (key >= STORE_SETTING_COUNT) {
        return 0;
    }
    for (uint8_t i = 0; i < (uint8_t)(sizeof(SETTINGS_META) / sizeof(SETTINGS_META[0])); i++) {
        if (SETTINGS_META[i].key == key) {
            return &SETTINGS_META[i];
        }
    }
    return 0;
}

static store_unit_t unit_for_domain(store_domain_t domain) {
    return (store_unit_t)((uint8_t)STORE_UNIT_SETTINGS_PHONEBOOK + (uint8_t)domain);
}

static bool serialize_settings_domain(store_domain_t domain, uint8_t *dst, size_t cap, size_t *out_len) {
    size_t pos = 0;
    uint8_t count = 0;
    if (!write_u32_field(dst, cap, &pos, SETTINGS_MAGIC) ||
        !write_u16_field(dst, cap, &pos, STORE_PAYLOAD_VERSION) ||
        !write_u8_field(dst, cap, &pos, (uint8_t)domain) ||
        !write_u8_field(dst, cap, &pos, 0u)) {
        return false;
    }
    size_t count_pos = 7u;
    for (uint8_t key = 0; key < STORE_SETTING_COUNT; key++) {
        const setting_meta_t *meta = setting_meta((store_setting_key_t)key);
        if (meta == 0 || meta->domain != domain) {
            continue;
        }
        uint8_t value_len = 0;
        uint8_t value[STORE_TEXT_MAX + 1u];
        memset(value, 0, sizeof(value));
        if (meta->type == SETTING_TYPE_U8) {
            value[0] = (uint8_t)s_settings[key].num;
            value_len = 1u;
        } else if (meta->type == SETTING_TYPE_U16) {
            write_u16(value, (uint16_t)s_settings[key].num);
            value_len = 2u;
        } else if (meta->type == SETTING_TYPE_U32) {
            write_u32(value, s_settings[key].num);
            value_len = 4u;
        } else {
            value_len = (uint8_t)strnlen(s_settings[key].text, STORE_TEXT_MAX);
            memcpy(value, s_settings[key].text, value_len);
        }
        if (!write_u16_field(dst, cap, &pos, key) ||
            !write_u8_field(dst, cap, &pos, (uint8_t)meta->type) ||
            !write_u8_field(dst, cap, &pos, value_len) ||
            !write_bytes(dst, cap, &pos, value, value_len)) {
            return false;
        }
        count++;
    }
    dst[count_pos] = count;
    if (domain == STORE_DOMAIN_PHONEBOOK) {
        uint8_t tone_count = 0;
        for (uint8_t i = 0; i < STORE_CONTACT_TONE_LIMIT; i++) {
            if (s_contact_tones[i].used) {
                tone_count++;
            }
        }
        if (!write_u16_field(dst, cap, &pos, PHONEBOOK_TONES_BLOCK) ||
            !write_u8_field(dst, cap, &pos, tone_count)) {
            return false;
        }
        for (uint8_t i = 0; i < STORE_CONTACT_TONE_LIMIT; i++) {
            if (!s_contact_tones[i].used) {
                continue;
            }
            /* Length-prefixed payload kept for framing; new format is one
             * value byte (old label-string entries are skipped on load). */
            if (!write_u16_field(dst, cap, &pos, s_contact_tones[i].contact_index) ||
                !write_u8_field(dst, cap, &pos, 1u) ||
                !write_u8_field(dst, cap, &pos, s_contact_tones[i].value)) {
                return false;
            }
        }
    }
    *out_len = pos;
    return true;
}

static bool apply_settings_payload(store_domain_t domain, const uint8_t *payload, size_t len) {
    /* Validate the complete frame before publishing any prefix. A truncated
     * record used to leave early settings/contact tones applied even though the
     * journal was logged as corrupt. Two passes keep the load atomic without a
     * 6 KiB settings-state copy on the firmware stack. */
    if (!walk_settings_payload(domain, payload, len, false)) {
        return false;
    }
    return walk_settings_payload(domain, payload, len, true);
}

static bool walk_settings_payload(store_domain_t domain,
                                  const uint8_t *payload,
                                  size_t len,
                                  bool publish) {
    if (len < 8u || read_u32(&payload[0]) != SETTINGS_MAGIC ||
        read_u16(&payload[4]) != STORE_PAYLOAD_VERSION || payload[6] != (uint8_t)domain) {
        return false;
    }
    uint8_t count = payload[7];
    size_t pos = 8u;
    for (uint8_t i = 0; i < count; i++) {
        if (pos + 4u > len) {
            return false;
        }
        store_setting_key_t key = (store_setting_key_t)read_u16(&payload[pos]);
        pos += 2u;
        setting_type_t type = (setting_type_t)payload[pos++];
        uint8_t value_len = payload[pos++];
        if (pos + value_len > len) {
            return false;
        }
        const setting_meta_t *meta = setting_meta(key);
        if (publish && meta != 0 && meta->domain == domain && meta->type == type) {
            if (type == SETTING_TYPE_U8 && value_len == 1u) {
                s_settings[key].num = payload[pos];
            } else if (type == SETTING_TYPE_U16 && value_len == 2u) {
                s_settings[key].num = read_u16(&payload[pos]);
            } else if (type == SETTING_TYPE_U32 && value_len == 4u) {
                s_settings[key].num = read_u32(&payload[pos]);
            } else if (type == SETTING_TYPE_TEXT && value_len <= STORE_TEXT_MAX) {
                memset(s_settings[key].text, 0, sizeof(s_settings[key].text));
                memcpy(s_settings[key].text, &payload[pos], value_len);
            }
        }
        pos += value_len;
    }
    if (domain == STORE_DOMAIN_PHONEBOOK) {
        if (publish) {
            memset(s_contact_tones, 0, sizeof(s_contact_tones));
        }
        while (pos + 3u <= len) {
            uint16_t block = read_u16(&payload[pos]);
            pos += 2u;
            uint8_t tone_count = payload[pos++];
            if (block != PHONEBOOK_TONES_BLOCK) {
                return true;
            }
            uint8_t slot = 0u;
            for (uint8_t i = 0; i < tone_count; i++) {
                if (pos + 3u > len) {
                    return false;
                }
                uint16_t contact_index = read_u16(&payload[pos]);
                pos += 2u;
                uint8_t value_len = payload[pos++];
                if (pos + value_len > len || value_len > STORE_TEXT_MAX) {
                    return false;
                }
                /* New format: exactly one value byte. Older label-string
                 * entries are dropped (they revert to Preset). */
                if (publish && value_len == 1u && slot < STORE_CONTACT_TONE_LIMIT) {
                    s_contact_tones[slot].used = true;
                    s_contact_tones[slot].contact_index = contact_index;
                    s_contact_tones[slot].value = payload[pos];
                    slot++;
                }
                pos += value_len;
            }
        }
    }
    return true;
}

static store_setting_key_t speed_dial_key(uint8_t key) {
    if (key < 1u || key > 9u) {
        return STORE_SETTING_COUNT;
    }
    return (store_setting_key_t)((uint8_t)STORE_SETTING_SPEED_DIAL_1 + key - 1u);
}

static int find_contact_tone(uint16_t contact_index) {
    for (uint8_t i = 0; i < STORE_CONTACT_TONE_LIMIT; i++) {
        if (s_contact_tones[i].used && s_contact_tones[i].contact_index == contact_index) {
            return (int)i;
        }
    }
    return -1;
}

static bool serialize_settings_unit(uint8_t instance,
                                    uint8_t *dst,
                                    size_t cap,
                                    size_t *out_len) {
    return serialize_settings_domain((store_domain_t)instance, dst, cap, out_len);
}

static bool apply_settings_unit(uint8_t instance, const uint8_t *payload, size_t len) {
    return apply_settings_payload((store_domain_t)instance, payload, len);
}

const store_unit_ops_t g_store_settings_unit_ops = {
    .reset_ram = reset_settings_unit,
    .serialize = serialize_settings_unit,
    .apply = apply_settings_unit,
    .fallback_missing_or_corrupt = 0,
    .name = "settings",
};


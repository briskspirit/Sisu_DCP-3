#ifndef STORE_SERVICE_H
#define STORE_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "services/battery_learning_logic.h"
#include "services/battery_charge_supervisor_logic.h"
#include "services/datetime_types.h"
#include "services/picture_message_types.h"

/* How long to hold off flash commits after audio/key activity, so a flash
 * erase (which parks core1) can't starve the audio DMA refill. Armed by
 * main.c on key activity / while audio is active, and by core1_services the
 * instant an audio command is posted (before core1 starts playing it). */
#define STORE_COMMIT_AUDIO_GUARD_MS 180u

#define STORE_TEXT_MAX 40u
#define STORE_CALL_LIST_LIMIT 20u
#define STORE_CALL_NUMBER_MAX 32u
#define STORE_CALL_NAME_MAX 24u
#define STORE_SPEED_DIAL_EMPTY 0xffffu
/* Per-contact ringing tone: stored as the v6.00 ringing-tone VALUE byte
 * (RINGING_TONE_OPTIONS catalogue), not a UI label string. */
#define STORE_CONTACT_TONE_PRESET 0x00u
#define STORE_CONTACT_TONE_NO_TONE 0xffu
#define STORE_T9_USER_WORD_LIMIT 16u
#define STORE_T9_WORD_MAX 32u
#define STORE_OWN_TONE_SLOT_COUNT 2u
#define STORE_OWN_TONE_NAME_MAX 15u
#define STORE_OWN_TONE_NOTES_MAX 276u
#define STORE_OWN_TONE_PACKED_MAX 256u
#define STORE_CALL_DIVERT_CONDITION_COUNT 5u
#define STORE_CALL_DIVERT_NUMBER_MAX 32u
#define STORE_WARRANTY_SERIAL_MAX 15u
#define STORE_WARRANTY_MMYY_MAX 4u

typedef enum {
    STORE_STATUS_OK = 0,
    STORE_STATUS_NOT_READY,
    STORE_STATUS_INVALID_ARGUMENT,
    STORE_STATUS_NOT_FOUND,
    STORE_STATUS_TYPE_MISMATCH,
    STORE_STATUS_STORAGE_ERROR,
    STORE_STATUS_CONFLICT,
} store_status_t;

/* Independently journaled persistence units. Exposed so diagnostics can name
 * the exact unit that is dirty or failing without knowing store internals. */
typedef enum {
    STORE_UNIT_SETTINGS_PHONEBOOK = 0,
    STORE_UNIT_SETTINGS_SMS,
    STORE_UNIT_SETTINGS_CALLS,
    STORE_UNIT_SETTINGS_PROFILES,
    STORE_UNIT_SETTINGS_CLOCK,
    STORE_UNIT_SETTINGS_SYSTEM,
    STORE_UNIT_CALLS_MISSED,
    STORE_UNIT_CALLS_RECEIVED,
    STORE_UNIT_CALLS_DIALLED,
    STORE_UNIT_T9_USER_DICT,
    STORE_UNIT_PICTURE_MESSAGES,
    STORE_UNIT_OWN_TONES,
    STORE_UNIT_CALL_DIVERT,
    STORE_UNIT_SERVICE_WARRANTY,
    STORE_UNIT_BATTERY_LEARNING,
    STORE_UNIT_BATTERY_CHARGE_SUPERVISOR,
    STORE_UNIT_COUNT
} store_unit_t;

#define STORE_DIAG_NO_UNIT 0xffu

typedef struct {
    bool ready;
    bool commit_active;
    bool defer_active;
    uint16_t dirty_mask;
    uint16_t degraded_mask;
    uint8_t current_unit;
    uint8_t last_unit;
    uint8_t next_scan_unit;
    uint8_t consecutive_failures[STORE_UNIT_COUNT];
    uint8_t last_status[STORE_UNIT_COUNT];
    uint32_t commit_attempts;
    uint32_t commit_successes;
    uint32_t commit_failures;
    uint32_t busy_deferrals;
    uint32_t flush_attempts;
    uint32_t flush_incomplete;
    uint32_t last_commit_ms;
} store_diag_snapshot_t;

typedef enum {
    STORE_SETTING_PHONEBOOK_VIEW_MODE = 0,
    STORE_SETTING_PHONEBOOK_MEMORY,
    STORE_SETTING_SMS_DEFAULT_PROFILE,
    STORE_SETTING_SMS_INFO_SERVICE,
    STORE_SETTING_SMS_MESSAGE_CENTRE,
    STORE_SETTING_SMS_SENT_AS,
    STORE_SETTING_SMS_VALIDITY,
    STORE_SETTING_SMS_DELIVERY_REPORTS,
    STORE_SETTING_SMS_REPLY_SAME_CENTRE,
    STORE_SETTING_CALL_OWN_NUMBER_SENDING,
    STORE_SETTING_CALL_SUMMARY_AFTER_CALL,
    STORE_SETTING_PROFILE_ACTIVE,
    STORE_SETTING_CLOCK_SHOW_STANDBY,
    STORE_SETTING_CLOCK_FORMAT_24H,
    STORE_SETTING_CLOCK_TIME_SET,
    STORE_SETTING_CLOCK_HOUR,
    STORE_SETTING_CLOCK_MINUTE,
    STORE_SETTING_CLOCK_DATE_DAY,
    STORE_SETTING_CLOCK_DATE_MONTH,
    STORE_SETTING_CLOCK_DATE_YEAR,
    STORE_SETTING_CLOCK_ALARM_ENABLED,
    STORE_SETTING_CLOCK_ALARM_HOUR,
    STORE_SETTING_CLOCK_ALARM_MINUTE,
    STORE_SETTING_SYSTEM_LANGUAGE,
    STORE_SETTING_SYSTEM_WELCOME_NOTE,
    STORE_SETTING_SYSTEM_VOICE_MAILBOX_NUMBER,
    STORE_SETTING_SPEED_DIAL_1,
    STORE_SETTING_SPEED_DIAL_2,
    STORE_SETTING_SPEED_DIAL_3,
    STORE_SETTING_SPEED_DIAL_4,
    STORE_SETTING_SPEED_DIAL_5,
    STORE_SETTING_SPEED_DIAL_6,
    STORE_SETTING_SPEED_DIAL_7,
    STORE_SETTING_SPEED_DIAL_8,
    STORE_SETTING_SPEED_DIAL_9,
    STORE_SETTING_CALL_TIMERS_CLEARED,
    STORE_SETTING_CALL_COUNTERS_CLEARED,
    STORE_SETTING_CALL_CREDIT_INFO_DISPLAY,
    STORE_SETTING_SMS_DICTIONARY_ACTIVE,
    STORE_SETTING_SMS_DICTIONARY_LANGUAGE,
    STORE_SETTING_PROFILE_INCOMING_ALERT,
    STORE_SETTING_PROFILE_RINGING_TONE,
    STORE_SETTING_PROFILE_RINGING_VOLUME,
    STORE_SETTING_PROFILE_MESSAGE_ALERT,
    STORE_SETTING_PROFILE_KEYPAD_TONES,
    STORE_SETTING_PROFILE_WARNING_GAME_TONES,
    STORE_SETTING_PROFILE_VIBRATING_ALERT,
    STORE_SETTING_SETTINGS_AUTOMATIC_REDIAL,
    STORE_SETTING_SETTINGS_SPEED_DIALLING,
    STORE_SETTING_SETTINGS_PHONE_LINE,
    STORE_SETTING_SETTINGS_AUTOMATIC_ANSWER,
    STORE_SETTING_SETTINGS_CELL_INFO_DISPLAY,
    STORE_SETTING_SETTINGS_NETWORK_SELECTION,
    STORE_SETTING_SETTINGS_LIGHTS,
    STORE_SETTING_SETTINGS_CONFIRM_SIM_ACTIONS,
    STORE_SETTING_SETTINGS_PIN_CODE_REQUEST,
    STORE_SETTING_SETTINGS_FIXED_DIALLING,
    STORE_SETTING_SETTINGS_CLOSED_USER_GROUP,
    STORE_SETTING_SETTINGS_PHONE_SECURITY,
    STORE_SETTING_SETTINGS_PHONE_LINE_CHANGE_ALLOWED,
    STORE_SETTING_CALCULATOR_EXCHANGE_RATE,
    STORE_SETTING_GAMES_SNAKE_LEVEL,
    STORE_SETTING_GAMES_SNAKE_TOP_SCORE,
    STORE_SETTING_PROFILE_PERSONAL_INCOMING_ALERT,
    STORE_SETTING_PROFILE_PERSONAL_RINGING_TONE,
    STORE_SETTING_PROFILE_PERSONAL_RINGING_VOLUME,
    STORE_SETTING_PROFILE_PERSONAL_MESSAGE_ALERT,
    STORE_SETTING_PROFILE_PERSONAL_KEYPAD_TONES,
    STORE_SETTING_PROFILE_PERSONAL_WARNING_GAME_TONES,
    STORE_SETTING_PROFILE_PERSONAL_VIBRATING_ALERT,
    STORE_SETTING_PROFILE_SILENT_INCOMING_ALERT,
    STORE_SETTING_PROFILE_SILENT_RINGING_TONE,
    STORE_SETTING_PROFILE_SILENT_RINGING_VOLUME,
    STORE_SETTING_PROFILE_SILENT_MESSAGE_ALERT,
    STORE_SETTING_PROFILE_SILENT_KEYPAD_TONES,
    STORE_SETTING_PROFILE_SILENT_WARNING_GAME_TONES,
    STORE_SETTING_PROFILE_SILENT_VIBRATING_ALERT,
    STORE_SETTING_PROFILE_DISCREET_INCOMING_ALERT,
    STORE_SETTING_PROFILE_DISCREET_RINGING_TONE,
    STORE_SETTING_PROFILE_DISCREET_RINGING_VOLUME,
    STORE_SETTING_PROFILE_DISCREET_MESSAGE_ALERT,
    STORE_SETTING_PROFILE_DISCREET_KEYPAD_TONES,
    STORE_SETTING_PROFILE_DISCREET_WARNING_GAME_TONES,
    STORE_SETTING_PROFILE_DISCREET_VIBRATING_ALERT,
    STORE_SETTING_PROFILE_LOUD_INCOMING_ALERT,
    STORE_SETTING_PROFILE_LOUD_RINGING_TONE,
    STORE_SETTING_PROFILE_LOUD_RINGING_VOLUME,
    STORE_SETTING_PROFILE_LOUD_MESSAGE_ALERT,
    STORE_SETTING_PROFILE_LOUD_KEYPAD_TONES,
    STORE_SETTING_PROFILE_LOUD_WARNING_GAME_TONES,
    STORE_SETTING_PROFILE_LOUD_VIBRATING_ALERT,
    STORE_SETTING_SERVICE_CODEC_BYTE,
    STORE_SETTING_SERVICE_WARRANTY_PURCHASE_DATE,
    STORE_SETTING_GAMES_ROTATION_LEVEL,
    STORE_SETTING_GAMES_ROTATION_TOP_SCORE,
    STORE_SETTING_GAMES_MEMORY_LEVEL,
    STORE_SETTING_GAMES_MEMORY_TOP_SCORE,
    STORE_SETTING_GAMES_REACT_LEVEL,
    STORE_SETTING_GAMES_REACT_TOP_SCORE,
    STORE_SETTING_GAMES_LOGIC_LEVEL,
    STORE_SETTING_GAMES_PACMAN_LEVEL,
    STORE_SETTING_GAMES_PACMAN_TOP_SCORE,
    /* CALL.3: persistent call-duration accounting (original EEPROM fields
     * 0x071b-0x071e); totals survive recent-list erase, Clear timers zeroes
     * them. Appended to keep persisted setting keys stable. */
    STORE_SETTING_CALL_DURATION_LAST,
    STORE_SETTING_CALL_DURATION_ALL,
    STORE_SETTING_CALL_DURATION_RECEIVED,
    STORE_SETTING_CALL_DURATION_DIALLED,
    /* Persisted security code (default 12345); "Change security code"
     * writes it, security-gated flows compare against it. */
    STORE_SETTING_SECURITY_CODE,
    /* Active Net Monitor overlay test; zero means disabled. Appended to keep
     * existing stored setting keys stable in the production firmware. */
    STORE_SETTING_SYSTEM_NET_MONITOR_SELECTOR,
    /* Headset accessory profile (auto-activates on headset insert; not in the
     * user-selectable Profiles list). Appended to keep existing keys stable. */
    STORE_SETTING_PROFILE_HEADSET_INCOMING_ALERT,
    STORE_SETTING_PROFILE_HEADSET_RINGING_TONE,
    STORE_SETTING_PROFILE_HEADSET_RINGING_VOLUME,
    STORE_SETTING_PROFILE_HEADSET_MESSAGE_ALERT,
    STORE_SETTING_PROFILE_HEADSET_KEYPAD_TONES,
    STORE_SETTING_PROFILE_HEADSET_WARNING_GAME_TONES,
    STORE_SETTING_PROFILE_HEADSET_VIBRATING_ALERT,
    /* User profile to restore when the headset is removed (mirrors the original's
     * saved-profile RAM at 0x0011fcaa). 0xff = none saved. */
    STORE_SETTING_PROFILE_SAVED,
    /* In-call earpiece volume level (1..10, default 5), persisted across power
     * cycles (user-confirmed original behavior). Appended at the enum tail: the
     * settings payload is key-tagged, so old flash records simply lack the key. */
    STORE_SETTING_CALL_VOLUME,
    /* Raw PCD8544 Vop calibration. Stock Nokia v6.00 maps EEPROM level 16 to
     * Vop 63; raw storage also supports replacement panels outside Nokia's
     * original 47..78 range. Appended to preserve every deployed key id. */
    STORE_SETTING_SYSTEM_LCD_VOP,
    /* Last fully verified modem provisioning schema. Diagnostic only: modem
     * boot still queries every persistent setting and never trusts this value
     * as permission to skip hardware readback. */
    STORE_SETTING_SYSTEM_MODEM_PROVISION_VERSION,
    /* Net Monitor page IDs were reassigned for the RevB2/Telit v2 registry.
     * Keep a schema byte beside the selector so old IDs are never reinterpreted
     * as a new editable page after an upgrade. Appended to preserve all keys. */
    STORE_SETTING_SYSTEM_NET_MONITOR_SCHEMA,
    /* Atomic LCD controller tuple: versioned Vop/TC/bias packed into one U32.
     * The older VOP-only key remains as a migration mirror for deployed boards. */
    STORE_SETTING_SYSTEM_LCD_TUNING,
    /* Nonempty only while Net Monitor's RAM-only band test owns modem policy.
     * Contains the original mode/config so an RP reset can restore it before
     * ordinary radio use. Appended to preserve every deployed key id. */
    STORE_SETTING_SYSTEM_NETMON_RADIO_RECOVERY,
    /* Backlight PWM duty while logically on. 1..100%, with 100% preserving
     * the behavior of firmware predating brightness calibration. */
    STORE_SETTING_SYSTEM_BACKLIGHT_LEVEL,
    /* Non-resettable service Life timer. Original v6.00 keeps this in the
     * call-accounting family, separate from the protected warranty record.
     * Appended to preserve every deployed setting key id. */
    STORE_SETTING_CALL_DURATION_LIFETIME,
    STORE_SETTING_COUNT
} store_setting_key_t;

typedef enum {
    STORE_CALL_LIST_MISSED = 0,
    STORE_CALL_LIST_RECEIVED,
    STORE_CALL_LIST_DIALLED,
    STORE_CALL_LIST_COUNT
} store_call_list_t;

typedef enum {
    STORE_CALL_REASON_NONE = 0,
    STORE_CALL_REASON_MISSED,
    STORE_CALL_REASON_RECEIVED,
    STORE_CALL_REASON_DIALLED,
    STORE_CALL_REASON_BUSY,
    STORE_CALL_REASON_NO_ANSWER,
    STORE_CALL_REASON_REJECTED,
    STORE_CALL_REASON_UNREACHABLE,
    STORE_CALL_REASON_NO_NETWORK,
} store_call_reason_t;

typedef struct {
    uint32_t id;
    char number[STORE_CALL_NUMBER_MAX + 1u];
    char name[STORE_CALL_NAME_MAX + 1u];
    uint32_t duration_seconds;
    rtc_datetime_t datetime;
    store_call_reason_t reason;
} store_call_record_t;

typedef struct {
    bool used;
    char name[STORE_OWN_TONE_NAME_MAX + 1u];
    char notes[STORE_OWN_TONE_NOTES_MAX + 1u];
    uint8_t tempo_index;
    uint16_t packed_len;
    uint8_t packed[STORE_OWN_TONE_PACKED_MAX];
} store_own_tone_t;

typedef struct {
    uint8_t active_mask;
    uint8_t delay_seconds;
    char numbers[STORE_CALL_DIVERT_CONDITION_COUNT][STORE_CALL_DIVERT_NUMBER_MAX + 1u];
} store_call_divert_state_t;

typedef struct {
    char serial[STORE_WARRANTY_SERIAL_MAX + 1u];
    char made[STORE_WARRANTY_MMYY_MAX + 1u];
    char repaired[STORE_WARRANTY_MMYY_MAX + 1u];
    char purchase_date[STORE_WARRANTY_MMYY_MAX + 1u];
    uint8_t flags;
} store_warranty_state_t;

store_status_t store_service_init(void);
bool store_service_ready(void);
/* Powered-on dormant gate: true when no healthy journal unit still needs a
 * flash commit. Dirty units already parked as degraded do not pin the phone
 * awake forever, matching the power-off flush policy. */
bool store_service_standby_ready(void);
void store_service_defer_commits_until(uint32_t deadline_ms);
void store_service_tick(uint32_t now_ms);
/* Power-off path: commit every dirty unit NOW, ignoring the activity defer
 * and the one-unit-per-32-ms pacing (the core is about to lose power; audio
 * is already silent, so the defer's reason is gone). True once nothing is
 * dirty. Must run BEFORE core1_services_shutdown() -- commits need the core1
 * flash-park handshake. */
bool store_service_flush_all(void);
void store_service_get_diag(store_diag_snapshot_t *out);

store_status_t store_setting_get_u8(store_setting_key_t key, uint8_t *out_value);
store_status_t store_setting_set_u8(store_setting_key_t key, uint8_t value);
store_status_t store_setting_get_u16(store_setting_key_t key, uint16_t *out_value);
store_status_t store_setting_set_u16(store_setting_key_t key, uint16_t value);
store_status_t store_setting_get_u32(store_setting_key_t key, uint32_t *out_value);
store_status_t store_setting_set_u32(store_setting_key_t key, uint32_t value);
store_status_t store_setting_get_text(store_setting_key_t key, char *out_text, uint8_t out_cap);
store_status_t store_setting_set_text(store_setting_key_t key, const char *text);

uint8_t store_call_count(store_call_list_t list);
store_status_t store_call_get(store_call_list_t list, uint8_t index, store_call_record_t *out_record);
store_status_t store_call_add(store_call_list_t list, const store_call_record_t *record, uint32_t *out_id);
store_status_t store_call_add_now(store_call_list_t list,
                                  const char *number,
                                  const char *name,
                                  uint32_t duration_seconds,
                                  store_call_reason_t reason,
                                  uint32_t *out_id);
store_status_t store_call_update_number(store_call_list_t list, uint8_t index, const char *number);
store_status_t store_call_update_result(store_call_list_t list,
                                        uint32_t id,
                                        uint32_t duration_seconds,
                                        store_call_reason_t reason);
store_status_t store_call_delete(store_call_list_t list, uint8_t index);
store_status_t store_call_clear(store_call_list_t list);
store_status_t store_phonebook_get_speed_dial(uint8_t key, uint16_t *out_contact_index);
store_status_t store_phonebook_set_speed_dial(uint8_t key, uint16_t contact_index);
store_status_t store_phonebook_clear_speed_dial(uint8_t key);
uint8_t store_phonebook_get_contact_tone_value(uint16_t contact_index);
store_status_t store_phonebook_set_contact_tone_value(uint16_t contact_index, uint8_t value);

store_status_t store_t9_user_words_load(char words[][STORE_T9_WORD_MAX + 1u],
                                        uint8_t word_cap,
                                        uint8_t *out_count);
store_status_t store_t9_user_words_save(char words[][STORE_T9_WORD_MAX + 1u], uint8_t count);

uint8_t store_picture_message_count(void);
store_status_t store_picture_message_get(uint8_t slot, store_picture_message_t *out_message);
store_status_t store_picture_message_set(uint8_t slot, const store_picture_message_t *message);
store_status_t store_picture_message_clear(uint8_t slot);

store_status_t store_own_tone_get(uint8_t slot, store_own_tone_t *out_tone);
store_status_t store_own_tone_set(uint8_t slot, const store_own_tone_t *tone);
store_status_t store_own_tone_clear(uint8_t slot);
bool store_own_tone_used(uint8_t slot);

store_status_t store_call_divert_get(store_call_divert_state_t *out_state);
store_status_t store_call_divert_set(const store_call_divert_state_t *state);

store_status_t store_warranty_get(store_warranty_state_t *out_state);
/* The board IMEI occupies the warranty record's serial field, but has a
 * stricter lifetime contract than the other editable service fields. It is
 * provisioned once from the modem and cannot be changed by warranty updates. */
bool store_board_imei_valid(const char *imei);
store_status_t store_board_imei_get(char *out_imei, size_t out_cap);
store_status_t store_board_imei_provision(const char *imei);
store_status_t store_warranty_set(const store_warranty_state_t *state);
store_status_t store_warranty_set_purchase_date(const char *mmyy);
store_status_t store_battery_learning_get(
    battery_learning_persisted_t *out_state);
store_status_t store_battery_learning_set(
    const battery_learning_persisted_t *state);
store_status_t store_battery_charge_supervisor_get(
    battery_charge_supervisor_persisted_t *out_state);
store_status_t store_battery_charge_supervisor_set(
    const battery_charge_supervisor_persisted_t *state);
/* Non-resettable connected-call odometer shown by *#92702689#. Kept outside
 * store_warranty_state_t so ordinary service-record updates cannot alter it. */
uint32_t store_life_timer_seconds(void);
store_status_t store_life_timer_add_seconds(uint32_t seconds);

#endif

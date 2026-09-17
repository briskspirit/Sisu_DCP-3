#ifndef APP_H
#define APP_H

#include <stdbool.h>
#include <stdint.h>

#include "apps/battery_status_logic.h"
#include "apps/sim_presence_logic.h"
#include "services/event_queue.h"
#include "ui/framebuffer.h"
#include "services/modem_service.h"
#include "storage/store_service.h"
#include "services/t9_service.h"
#include "ui/signal_bars.h"

#define APP_SMS_RECORD_LIMIT MODEM_SMS_RECORD_MAX
#define APP_SMS_T9_USER_WORD_LIMIT 16u

typedef struct {
    uint16_t modem_indices[MODEM_SMS_SEGMENT_MAX];
    uint32_t identity_hash;
    uint8_t modem_index_count;
    bool from_storage;
    bool picture;
    bool quarantined;
    char status[MODEM_SMS_STATUS_MAX + 1u];
    char address[MODEM_SMS_SENDER_MAX + 1u];
    char timestamp[MODEM_SMS_TIMESTAMP_MAX + 1u];
} app_sms_record_t;

typedef struct {
    bool valid;
    bool picture;
    union {
        char text[MODEM_SMS_DECODED_TEXT_MAX + 1u];
        store_picture_message_t picture_message;
    };
} app_sms_content_t;

typedef enum {
    APP_ROUTE_STANDBY = 0,
    APP_ROUTE_POWERUP,
    APP_ROUTE_MAIN_MENU,
    APP_ROUTE_POWER_OFF,
    APP_ROUTE_POWER_MENU,
    APP_ROUTE_DISPLAY_MESSAGE,
    APP_ROUTE_CONFIRM,
    APP_ROUTE_EDITOR,
    APP_ROUTE_PHONEBOOK_MENU,
    APP_ROUTE_PHONEBOOK_LIST,
    APP_ROUTE_PHONEBOOK_MEMORY,
    APP_ROUTE_PHONEBOOK_TONE_PICKER,
    APP_ROUTE_PHONEBOOK_SPEED_DIALS,
    APP_ROUTE_PHONEBOOK_SPEED_DIAL_OPTIONS,
    APP_ROUTE_PHONEBOOK_EDIT_CHOICE,
    APP_ROUTE_PHONEBOOK_SECURITY,
    APP_ROUTE_MESSAGES_MENU,
    APP_ROUTE_MESSAGES_LIST,
    APP_ROUTE_SMS_COMPOSER,
    APP_ROUTE_SMS_OPTIONS,
    APP_ROUTE_SMS_SYMBOLS,
    APP_ROUTE_CALL_REGISTER_MENU,
    APP_ROUTE_CALL_REGISTER_LIST,
    APP_ROUTE_CALL_REGISTER_OPTIONS,
    APP_ROUTE_CALL_REGISTER_DETAIL,
    APP_ROUTE_CALL_REGISTER_METRIC,
    APP_ROUTE_CALL,
    APP_ROUTE_CALL_OPTIONS,
    APP_ROUTE_INCOMING_CALL,
    APP_ROUTE_CLOCK_MENU,
    APP_ROUTE_CLOCK_EDITOR,
    APP_ROUTE_CLOCK_ALARM,
    APP_ROUTE_TONES_MENU,
    APP_ROUTE_TONES_SETTING,
    APP_ROUTE_TONE_COMPOSER,
    APP_ROUTE_TONE_COMPOSER_OPTIONS,
    APP_ROUTE_TONE_COMPOSER_TEMPO,
    APP_ROUTE_SETTINGS_MENU,
    APP_ROUTE_SETTINGS_VALUE,
    APP_ROUTE_SETTINGS_WELCOME_OPTIONS,
    APP_ROUTE_CALL_DIVERT_MENU,
    APP_ROUTE_SERVICE_CODES,
    APP_ROUTE_NET_MONITOR_TEST,
    APP_ROUTE_NET_MONITOR_PAGE,
    APP_ROUTE_PROFILES_MENU,
    APP_ROUTE_GAMES_MENU,
    APP_ROUTE_ROTATION_MENU,
    APP_ROUTE_ROTATION_PLAY,
    APP_ROUTE_ROTATION_LEVEL,
    APP_ROUTE_ROTATION_INSTRUCTIONS,
    APP_ROUTE_ROTATION_TOP_SCORE,
    APP_ROUTE_ROTATION_RESULT,
    APP_ROUTE_ROTATION_LAST_VIEW,
    APP_ROUTE_MEMORY_MENU,
    APP_ROUTE_MEMORY_PLAY,
    APP_ROUTE_MEMORY_LEVEL,
    APP_ROUTE_MEMORY_INSTRUCTIONS,
    APP_ROUTE_MEMORY_TOP_SCORE,
    APP_ROUTE_MEMORY_RESULT,
    APP_ROUTE_MEMORY_LAST_VIEW,
    APP_ROUTE_REACT_MENU,
    APP_ROUTE_REACT_PLAY,
    APP_ROUTE_REACT_LEVEL,
    APP_ROUTE_REACT_INSTRUCTIONS,
    APP_ROUTE_REACT_TOP_SCORE,
    APP_ROUTE_REACT_RESULT,
    APP_ROUTE_REACT_LAST_VIEW,
    APP_ROUTE_LOGIC_MENU,
    APP_ROUTE_LOGIC_PLAY,
    APP_ROUTE_LOGIC_LEVEL,
    APP_ROUTE_LOGIC_INSTRUCTIONS,
    APP_ROUTE_LOGIC_RESULT,
    APP_ROUTE_LOGIC_LAST_VIEW,
    APP_ROUTE_PACMAN_MENU,
    APP_ROUTE_PACMAN_PLAY,
    APP_ROUTE_PACMAN_LEVEL,
    APP_ROUTE_PACMAN_INSTRUCTIONS,
    APP_ROUTE_PACMAN_TOP_SCORE,
    APP_ROUTE_PACMAN_RESULT,
    APP_ROUTE_PACMAN_LAST_VIEW,
    APP_ROUTE_SNAKE_MENU,
    APP_ROUTE_SNAKE_PLAY,
    APP_ROUTE_SNAKE_LEVEL,
    APP_ROUTE_SNAKE_INSTRUCTIONS,
    APP_ROUTE_SNAKE_TOP_SCORE,
    APP_ROUTE_SNAKE_RESULT,
    APP_ROUTE_SNAKE_LAST_VIEW,
    APP_ROUTE_CALCULATOR,
} app_route_t;

typedef enum {
    APP_STANDBY_ACTION_CALL = 0,
    APP_STANDBY_ACTION_SAVE,
} app_standby_action_t;

typedef enum {
    APP_POWERUP_ALL_PIXELS = 0,
    APP_POWERUP_BLANK,
    APP_POWERUP_BATTERY_PRE,
    APP_POWERUP_BOOT_LOGO,
    APP_POWERUP_WELCOME_NOTE,
    APP_POWERUP_BATTERY_POST,
    APP_POWERUP_DONE,
} app_powerup_stage_t;

typedef struct {
    app_route_t route;
    bool dirty;
    bool backlight_force_active;
    bool backlight_force_on;
    bool backlight_activity_pending;
    uint32_t backlight_activity_ms;
    uint8_t power_menu_selected;
    bool power_off_failed;
    app_powerup_stage_t powerup_stage;
    bool powerup_skip_boot_logo;
    uint8_t powerup_frame_index;
    char welcome_note[41];
    uint32_t powerup_deadline_ms;
    uint32_t powerup_boot_started_ms;
    uint8_t menu_index;
    uint8_t menu_frame_index;
    bool menu_animation_playing;
    uint32_t menu_selected_ms;
    uint32_t menu_next_frame_ms;
    uint32_t menu_keyguard_armed_until_ms;
    bool menu_keyguard_armed; /* gate (not a 0-deadline sentinel: avoids the wrap-to-0 hole) */
    char input_text[31];
    uint8_t input_len;
    app_standby_action_t input_action;
    uint8_t star_cycle_index;
    uint32_t star_cycle_until_ms;
    bool keyguard_locked;
    bool unlock_armed;
    uint32_t unlock_armed_until_ms;
    char standby_message[33];
    uint32_t standby_message_until_ms;
    char operator_name[MODEM_OPERATOR_NAME_CAPACITY];
    bool sim_missing;
    sim_presence_ui_state_t sim_presence_ui;
    uint16_t modem_provision_version_recorded;
    uint32_t modem_provision_record_retry_ms;
    char clock_text[8];
    bool clock_alarm_enabled;
    uint8_t signal_bars;
    signal_bars_filter_t signal_bars_filter;
    bool call_divert_unconditional_active;
    modem_message_waiting_status_t message_waiting;
    modem_message_waiting_category_mask_t message_waiting_notice_mask;
    uint16_t message_waiting_notice_count[
        MODEM_MESSAGE_WAITING_CATEGORY_COUNT];
    uint32_t last_status_ms;
    app_route_t display_return_route;
    uint8_t display_record_id;
    /* Full resolved (localized) message text, wrapped to the record's window at
     * render time. A single localized dialog line reaches ~69 bytes, so the old
     * pre-split char[3][18] both truncated mid-UTF-8 and dropped anything past
     * one line -- store the whole string and let the renderer wrap it. */
    char display_text[128];
    uint32_t display_opened_ms;
    uint32_t display_last_frame_ms;
    uint8_t display_frame_index;
    uint8_t display_progress_phase;
    uint8_t confirm_context;
    /* 32 bytes/slot: localized single-line questions (e.g. Greek "Erase?" is 18
     * bytes) overflow a char[][18] slot and truncate mid-UTF-8. */
    char confirm_lines[3][32];
    uint8_t confirm_line_count;
    int8_t confirm_first_y;
    uint8_t phonebook_menu_kind;
    uint8_t phonebook_menu_selected;
    uint8_t phonebook_context;
    uint8_t phonebook_pending_kind;
    uint8_t phonebook_pending_label;
    uint32_t phonebook_request_id;
    /* Silent startup refresh which makes the shared SIM-phonebook resolver
     * available to messages, calls, and call-register writers before the
     * Phone book application itself has been opened. */
    uint32_t phonebook_sync_request_id;
    uint32_t phonebook_sync_retry_ms;
    /* The phonebook request whose progress note currently owns the display.
     * Its token remains through erase-all's short post-request grace; every
     * generic dialog open clears the claim before replacing that note. */
    uint32_t phonebook_wait_display_request_id;
    uint32_t phonebook_request_started_ms;
    uint16_t phonebook_pending_selected;
    char phonebook_pending_path[6];
    uint16_t phonebook_pending_index;
    uint16_t phonebook_visible_indices[MODEM_PHONEBOOK_MAX_RECORDS];
    uint16_t phonebook_visible_count;
    uint16_t phonebook_list_selected;
    char phonebook_search_query[17];
    char editor_title[25];
    char editor_value[STORE_PICTURE_TEXT_MAX + 1u];
    uint8_t editor_max_len;
    uint8_t editor_kind;
    uint8_t editor_mode;
    uint8_t editor_context;
    bool editor_show_cursor;
    bool editor_show_mode_badge;
    bool editor_cursor_visible;
    int8_t editor_cursor_x_offset;
    int8_t editor_cursor_y_offset;
    uint8_t editor_cursor_index;
    uint32_t editor_last_cursor_ms;
    uint16_t editor_last_key;
    uint32_t editor_last_key_ms;
    uint8_t editor_tap_index;
    char editor_draft_name[17];
    char editor_draft_number[33];
    char editor_original_name[17];
    uint8_t phonebook_tone_selected;
    uint8_t phonebook_speed_selected;
    uint8_t phonebook_speed_key;
    uint8_t phonebook_speed_options_selected;
    uint8_t phonebook_edit_choice_selected;
    char sms_recipient_prefill[33];
    uint32_t phonebook_erase_all_started_ms;
    bool phonebook_send_waiting;
    uint32_t phonebook_send_started_ms;
    uint32_t phonebook_send_request_id;
    uint8_t messages_menu_selected;
    uint8_t messages_kind;
    uint8_t messages_mode;
    uint8_t messages_selected;
    uint8_t messages_read_page;
    uint8_t messages_read_scroll;
    uint8_t messages_option_selected;
    uint8_t messages_detail_page;
    uint8_t messages_info_selected;
    uint8_t messages_settings_kind;
    uint8_t messages_settings_top_selected;
    uint8_t messages_settings_selected;
    uint8_t messages_settings_value_key;
    uint8_t messages_settings_value_selected;
    bool messages_picture_pending_valid;
    bool messages_picture_text_editing;
    bool messages_picture_save_pending;
    uint8_t messages_picture_pending_slot;
    uint8_t messages_picture_save_return_kind;
    uint8_t messages_picture_save_return_selected;
    char messages_picture_draft[STORE_PICTURE_TEXT_MAX + 1u];
    store_picture_message_t messages_picture_save_candidate;
    bool messages_picture_send_waiting;
    uint32_t messages_picture_send_started_ms;
    uint32_t messages_picture_send_request_id;
    uint16_t messages_picture_payload_len;
    uint8_t messages_picture_payload_chunks;
    uint32_t messages_open_started_ms;
    bool messages_open_pending;
    uint32_t messages_open_request_id;
    bool sms_delete_waiting;
    uint32_t sms_delete_started_ms;
    uint32_t sms_delete_request_id;
    uint32_t last_modem_sms_received_count;
    uint32_t last_modem_user_sms_received_count;
    uint32_t last_sms_storage_full_events; /* edge-tracks the modem's "receive store full" notice */
    bool sms_status_sync_silent;  /* the in-flight mailbox load only refreshes unread status (no UI) */
    bool sms_status_sync_pending; /* a boot/arrival sync still needs a complete, current mailbox view */
    bool sms_boot_status_sync_done;
    bool sms_open_deferred;       /* a user mailbox open is queued behind the in-flight silent reconcile */
    uint8_t sms_open_deferred_kind;
    uint32_t sms_mailbox_received_count_at_start; /* rejects a scan crossed by a newer +CMTI */
    /* Only one modem mailbox is open at a time and every open reloads it, so
     * Inbox and Outbox deliberately share the same full-domain view buffer. */
    union {
        app_sms_record_t sms_inbox[APP_SMS_RECORD_LIMIT];
        app_sms_record_t sms_outbox[APP_SMS_RECORD_LIMIT];
    };
    uint8_t sms_inbox_count;
    uint8_t sms_outbox_count;
    app_sms_content_t sms_selected_content;
    bool sms_read_waiting;
    uint32_t sms_read_started_ms;
    uint32_t sms_read_request_id;
    uint32_t sms_read_identity_hash;
    char sms_composer_text[MODEM_SMS_TEXT_MAX + 1u];
    uint16_t sms_composer_cursor; /* insertion point (DF3_CURSOR_MOVABLE) */
    bool sms_composer_cursor_visible;
    uint32_t sms_composer_cursor_blink_ms;
    uint8_t sms_composer_mode;
    uint16_t sms_composer_last_key;
    uint32_t sms_composer_last_key_ms;
    uint8_t sms_composer_tap_index;
    uint16_t sms_composer_hold_key;
    uint8_t sms_composer_hold_undo;
    char sms_composer_hold_previous_char;
    uint8_t sms_options_selected;
    bool sms_options_dictionary;
    uint8_t sms_symbols_selected;
    bool sms_dictionary_active;
    uint8_t sms_dictionary_index;
    bool sms_t9_manual_mode;
    char sms_t9_sequence[T9_SEQUENCE_MAX + 1u];
    uint16_t sms_t9_word_start;
    uint16_t sms_t9_word_end;
    uint8_t sms_t9_candidate_index;
    uint8_t sms_t9_candidate_count;
    char sms_t9_candidates[T9_CANDIDATE_LIMIT][T9_WORD_MAX + 1u];
    bool sms_t9_spell_offer;
    bool sms_t9_insert_replace_active;
    char sms_t9_user_words[APP_SMS_T9_USER_WORD_LIMIT][T9_WORD_MAX + 1u];
    uint8_t sms_t9_user_word_count;
    bool sms_send_waiting;
    uint32_t sms_send_started_ms;
    uint32_t sms_send_request_id;
    /* A send may finish after call/alarm UI preemption. Clear the draft on a
     * late success only while it is still byte-for-byte the submitted draft;
     * otherwise a newer edit must survive that old terminal. */
    char sms_send_submitted_text[MODEM_SMS_TEXT_MAX + 1u];
    char sms_send_submitted_recipient[MODEM_SMS_SENDER_MAX + 1u];
    app_route_t sms_send_return_route;
    bool sms_save_waiting;
    uint32_t sms_save_started_ms;
    uint32_t sms_save_request_id;
    bool sms_received_pending;
    uint8_t sms_received_pending_count;
    /* STBY.1: unread-records counter driving the persistent slot-0x03
     * envelope, independent of the "N messages received" prompt. */
    uint8_t sms_unread_count;
    /* Battery/charger runtime (board diagnostics service backed). */
    uint8_t battery_bars;
    bool battery_charger_connected;
    bool battery_charge_active;
    uint8_t battery_anim_level;
    uint32_t battery_anim_ms;
    bool battery_full_notified;
    bool battery_low_notified;      /* currently in the low zone (warning schedule armed) */
    bool battery_low_warn_fast;     /* the next low re-warn uses the fast (post-transition) interval */
    bool battery_capacity_voltage_disagreement;
    battery_endpoint_kind_t battery_endpoint_kind;
    battery_endpoint_state_t battery_endpoint_state;
    uint32_t battery_low_next_warn_ms; /* deadline for the next repeating "Battery low" notice */
    uint32_t battery_empty_off_ms;
    battery_raw_collapse_evidence_t battery_raw_collapse_evidence;
    uint8_t call_register_menu_kind;
    uint8_t call_register_menu_selected;
    uint8_t call_register_list_kind;
    uint8_t call_register_list_selected;
    app_route_t call_register_return_route;
    uint8_t call_register_navi_call;
    uint8_t call_register_options_selected;
    uint8_t call_register_metric_kind;
    uint8_t call_register_metric_selected;
    char call_register_detail_title[24];
    char call_register_detail_value[24];
    uint8_t call_register_detail_mode;
    app_route_t call_register_detail_return_route;
    modem_call_state_t last_modem_call_state;
    modem_call_result_t last_modem_call_result;
    char call_number[MODEM_PHONE_MAX + 1u];
    char call_name[MODEM_PHONEBOOK_NAME_MAX + 1u];
    char call_waiting_number[MODEM_PHONE_MAX + 1u];
    char call_waiting_name[MODEM_PHONEBOOK_NAME_MAX + 1u];
    char call_held_number[MODEM_PHONE_MAX + 1u];
    char call_held_name[MODEM_PHONEBOOK_NAME_MAX + 1u];
    uint8_t call_phase;
    bool call_incoming;
    bool call_answer_pending;
    bool call_incoming_recorded;
    bool call_incoming_silenced;
    bool call_incoming_withheld;  /* CLI withheld -> "Private number" */
    bool call_incoming_diverted;  /* network-diverted incoming -> "Diverted call" */
    bool call_waiting_pending;
    uint8_t call_waiting_id;       /* generation-qualified model leg behind the overlay */
    uint8_t call_waiting_generation;
    uint32_t call_waiting_episode; /* pre-id fallback until the leg is bound */
    bool call_waiting_withheld;   /* identity state belongs to the waiting leg */
    bool call_waiting_diverted;
    bool call_waiting_recorded;
    bool call_waiting_action_pending;
    bool call_waiting_release_pending;
    uint8_t call_waiting_release_active_id;
    uint32_t call_waiting_release_retry_ms;
    bool call_pending_local_hangup;
    bool call_result_armed;
    bool call_held;
    bool call_secondary_active;
    bool last_second_call_held; /* prev status.second_call_held, for the true->false reconcile edge */
    uint8_t last_active_call_id; /* prev status.active_call_id, to tell held-dropped from active-promoted */
    bool call_survivor_retrieve_pending;
    uint8_t call_survivor_retrieve_id;
    uint32_t call_survivor_retrieve_ms;
    uint8_t call_new_active_ref; /* active id when a NEW (2nd) call was dialled; the new call is
                                  * "connected" only once a DIFFERENT active id appears (not the
                                  * still-active/just-held original leg) */
    bool missed_call_pending;
    uint8_t missed_call_pending_count;
    store_call_list_t call_record_list;
    store_call_list_t call_held_record_list;
    uint32_t call_record_id;
    uint32_t call_held_record_id;
    uint32_t call_held_elapsed_seconds;
    uint32_t call_started_ms;
    uint32_t call_connected_ms;
    uint32_t call_active_seen_ms;
    uint32_t call_pending_failure_ms;
    modem_call_result_t call_pending_failure_result;
    uint8_t call_frame_index;
    uint8_t call_options_selected;
    uint8_t call_volume_level;
    bool call_volume_visible;
    uint32_t call_volume_until_ms;
    int32_t call_last_second;
    uint8_t clock_menu_kind;
    uint8_t clock_menu_selected;
    uint8_t clock_editor_kind;
    uint8_t clock_editor_cursor_digit;
    bool clock_editor_cursor_visible;
    uint32_t clock_editor_last_cursor_ms;
    char clock_editor_value[11];
    uint8_t clock_pending_kind;
    char clock_pending_time[6];
    /* The editor owns an accepted RTC write until the HAL's bounded retry loop
     * reports COMMITTED or FAILED; success UI/storage cannot run before that. */
    bool clock_datetime_commit_pending;
    uint8_t clock_alarm_mode;
    /* 72 bytes: localized alarm/snooze dialogs are multiline and long (BULG
     * "Snooze active" 0x044 is 59 bytes) -- a 24-byte buffer truncated them. */
    char clock_alarm_text[72];
    uint32_t clock_alarm_started_ms;
    uint32_t clock_alarm_last_frame_ms;
    uint32_t clock_alarm_last_backlight_ms;
    uint8_t clock_alarm_frame_index;
    bool clock_alarm_power_off_wake;
    /* Mandatory clock-setup chain in progress (Time: -> "Time is set" -> Date:).
     * Set by the boot RTC-lost flow AND by any menu action that needs an unset
     * clock (Alarm / Date setting / Show clock). Completing it enables the
     * standby display; dismissing it leaves the clock unset ("Time not set"). */
    bool clock_setup_pending;
    uint8_t tones_menu_selected;
    uint8_t tones_setting_kind;
    uint8_t tones_setting_selected;
    uint8_t tones_setting_view_start;
    bool tones_from_profiles;
    uint8_t tones_profile_index;
    /* v6.00 timer 0x30 defers ringing-tone/volume previews after list motion.
     * Keep a separate valid bit because the wrap-safe deadline may be zero. */
    bool tones_preview_pending;
    uint32_t tones_preview_due_ms;
    char tone_composer_name[STORE_OWN_TONE_NAME_MAX + 1u];
    char tone_composer_notes[STORE_OWN_TONE_NOTES_MAX + 1u];
    uint8_t tone_composer_cursor_index;
    uint8_t tone_composer_duration_index;
    uint8_t tone_composer_octave;
    uint8_t tone_composer_option_index;
    uint8_t tone_composer_tempo_index;
    bool tone_composer_sharp_armed;
    bool tone_composer_dotted_armed;
    bool tone_composer_cursor_visible;
    uint32_t tone_composer_last_cursor_ms;
    uint16_t tone_composer_hold_key;
    bool tone_composer_hold_consumed;
    uint8_t tone_composer_name_action;
    bool tone_composer_playing;
    uint8_t tone_composer_play_index;
    uint32_t tone_composer_next_note_ms;
    uint32_t tone_composer_preview_stop_ms;
    bool tone_composer_send_waiting;
    uint32_t tone_composer_send_started_ms;
    uint16_t tone_composer_packed_len;
    uint8_t tone_composer_packed[STORE_OWN_TONE_PACKED_MAX];
    uint8_t profiles_menu_kind;
    uint8_t profiles_selected_index;
    uint8_t profiles_option_index;
    uint8_t profiles_pending_index;
    uint32_t profiles_switch_started_ms;
    uint8_t settings_menu_kind;
    uint8_t settings_menu_selected;
    uint8_t settings_value_kind;
    uint8_t settings_value_selected;
    uint8_t settings_value_view_start;
    uint8_t settings_value_parent_kind;
    uint8_t settings_value_parent_selected;
    uint8_t settings_pending_value;
    uint8_t settings_pending_action;
    uint32_t settings_pending_started_ms;
    uint8_t settings_welcome_option_selected;
    uint8_t settings_access_kind;
    uint8_t settings_access_step;
    char settings_access_new_code[11];
    uint8_t call_divert_menu_kind;
    uint8_t call_divert_menu_selected;
    uint8_t call_divert_condition_index;
    uint8_t call_divert_parent_selected;
    char call_divert_numbers[5][33];
    uint8_t call_divert_delay_seconds;
    uint8_t call_divert_pending_action;
    uint8_t call_divert_pending_condition_index;
    uint32_t call_divert_request_id;
    app_route_t call_divert_result_return_route;
    uint8_t call_divert_status_detail_index;
    uint8_t call_divert_status_detail_count;
    bool call_divert_status_detail_active;
    char call_divert_status_number[33];
    uint8_t call_divert_status_delay_seconds;
    bool call_divert_storage_loaded;
    uint8_t service_code_selected;
    uint8_t service_code_stage;
    char service_code_purchase_draft[5];
    uint8_t service_code_purchase_cursor;
    bool service_code_cursor_visible;
    uint32_t service_code_last_cursor_ms;
    uint32_t service_code_transfer_started_ms;
    char net_monitor_input[4];
    uint8_t net_monitor_input_len;
    bool net_monitor_replace_on_digit;
    uint8_t net_monitor_page_index;
    uint32_t net_monitor_last_page_ms;
    uint16_t net_monitor_selector;
    uint32_t net_monitor_last_refresh_ms;
    uint32_t net_monitor_generation;
    uint32_t net_monitor_last_local_sequence;
    uint32_t net_monitor_last_modem_sequence;
    uint32_t net_monitor_last_control_sequence;
    uint8_t net_monitor_surface;
    bool net_monitor_modem_subscribed;
    uint32_t net_monitor_status_until_ms;
    char net_monitor_status[18];
    char calculator_value[16];
    char calculator_stored[16];
    char calculator_exchange_rate[16];
    char calculator_exchange_editor_value[16];
    uint8_t calculator_operator_code;
    char calculator_operator_char;
    uint8_t calculator_mode;
    bool calculator_options_open;
    uint8_t calculator_option_index;
    uint8_t calculator_exchange_mode;
    uint8_t calculator_exchange_index;
    uint8_t calculator_exchange_direction;
    bool calculator_exchange_dirty;
    uint8_t calculator_exchange_cursor_index;
    bool calculator_cursor_visible;
    uint32_t calculator_last_cursor_ms;
    uint8_t games_menu_selected;
    /* All six game option menus are mutually exclusive and share the stock
     * circular three-row list viewport. */
    uint8_t game_option_view_start;
    uint8_t rotation_menu_variant;
    uint8_t rotation_menu_selected;
    uint8_t rotation_level_byte;
    uint8_t rotation_level_draft_byte;
    uint16_t rotation_score;
    uint16_t rotation_top_score;
    uint8_t rotation_board[36];
    uint8_t rotation_cursor_x;
    uint8_t rotation_cursor_y;
    uint8_t rotation_ready_ticks_remaining;
    uint16_t rotation_elapsed_ticks;
    uint8_t rotation_animation_phase;
    uint8_t rotation_animation_steps_remaining;
    uint8_t rotation_pending_rotation;
    uint32_t rotation_next_animation_ms;
    uint32_t rotation_next_idle_ms;
    uint32_t rotation_solved_deadline_ms;
    uint32_t rotation_result_deadline_ms;
    bool rotation_ready;
    bool rotation_game_over;
    bool rotation_result_top_score;
    uint8_t rotation_instructions_scroll;
    uint8_t rotation_top_score_frame;
    uint32_t rotation_top_score_last_frame_ms;
    uint8_t memory_menu_variant;
    uint8_t memory_menu_selected;
    uint8_t memory_level_byte;
    uint8_t memory_level_draft_byte;
    uint16_t memory_score;
    uint16_t memory_top_score;
    uint8_t memory_cards[60];
    uint8_t memory_flags[60];
    uint8_t memory_cursor_col;
    uint8_t memory_cursor_row;
    int8_t memory_first_pick_index;
    int8_t memory_pending_a;
    int8_t memory_pending_b;
    uint32_t memory_rng_seed;
    uint32_t memory_result_deadline_ms;
    bool memory_pending_mismatch;
    bool memory_game_over;
    bool memory_result_top_score;
    uint8_t memory_instructions_scroll;
    uint8_t memory_top_score_frame;
    uint32_t memory_top_score_last_frame_ms;
    uint8_t react_menu_variant;
    uint8_t react_menu_selected;
    uint8_t react_level_byte;
    uint8_t react_level_draft_byte;
    uint16_t react_score;
    uint16_t react_top_score;
    uint32_t react_rng_seed;
    uint32_t react_next_tick_ms;
    uint32_t react_result_deadline_ms;
    uint16_t react_tick_counter;
    uint8_t react_phase_tick;
    uint8_t react_tries_remaining;
    uint8_t react_attempts_used;
    uint8_t react_lives_remaining;
    uint8_t react_slot_flags[6];
    uint8_t react_slot_delays[6];
    uint8_t react_slot_timers[6];
    uint8_t react_slot_hit_ticks[6];
    uint8_t react_active_mask;
    bool react_game_over;
    bool react_result_top_score;
    uint8_t react_instructions_scroll;
    uint8_t react_top_score_frame;
    uint32_t react_top_score_last_frame_ms;
    uint8_t logic_menu_variant;
    uint8_t logic_menu_selected;
    uint8_t logic_level_byte;
    uint8_t logic_level_draft_byte;
    uint8_t logic_rows[50];
    uint8_t logic_clues[10];
    uint8_t logic_checked_count;
    uint8_t logic_attempt_index;
    uint8_t logic_peg_index;
    uint8_t logic_solution[5];
    uint8_t logic_sprite_pool[10];
    uint32_t logic_rng_seed;
    uint32_t logic_result_deadline_ms;
    bool logic_game_over;
    uint8_t logic_instructions_scroll;
    uint8_t pacman_menu_variant;
    uint8_t pacman_menu_selected;
    uint8_t pacman_level_byte;
    uint8_t pacman_level_draft_byte;
    uint16_t pacman_score;
    uint16_t pacman_top_score;
    uint8_t pacman_lives;
    uint8_t pacman_player_x;
    uint8_t pacman_player_y;
    uint8_t pacman_direction;
    uint8_t pacman_pending_direction;
    uint8_t pacman_ghost_x[3];
    uint8_t pacman_ghost_y[3];
    uint8_t pacman_ghost_direction[3];
    uint8_t pacman_pellets[32];
    uint8_t pacman_pellets_remaining;
    uint8_t pacman_power_ticks;
    uint16_t pacman_tick_count;
    uint32_t pacman_rng_seed;
    uint32_t pacman_next_tick_ms;
    uint32_t pacman_result_deadline_ms;
    bool pacman_game_over;
    bool pacman_result_top_score;
    bool pacman_result_won;
    uint8_t pacman_instructions_scroll;
    uint8_t pacman_top_score_frame;
    uint32_t pacman_top_score_last_frame_ms;
    uint8_t snake_menu_variant;
    uint8_t snake_menu_selected;
    uint8_t snake_level_byte;
    uint8_t snake_level_draft_byte;
    uint16_t snake_score;
    uint16_t snake_top_score;
    uint8_t snake_body_x[220];
    uint8_t snake_body_y[220];
    uint8_t snake_body_len;
    uint8_t snake_food_x;
    uint8_t snake_food_y;
    uint8_t snake_direction;
    uint8_t snake_pending_direction;
    uint32_t snake_rng_seed;
    uint32_t snake_next_tick_ms;
    uint32_t snake_crash_deadline_ms;
    uint32_t snake_result_deadline_ms;
    bool snake_game_over;
    bool snake_result_top_score;
    uint8_t snake_instructions_scroll;
    uint8_t snake_top_score_frame;
    uint32_t snake_top_score_last_frame_ms;
} app_t;

void app_init(app_t *app);
bool app_handle_event(app_t *app, const input_event_t *event);
bool app_tick(app_t *app, uint32_t now_ms);
void app_render(const app_t *app, framebuffer_t *fb);

#endif

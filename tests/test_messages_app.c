/* App-level characterization for Messages. The test drives public routes and
 * keys against production code so Phase 5A translation-unit splits cannot
 * quietly change composition or picture-message behavior. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "audio/audio_levels.h"
#include "app_internal.h"
#include "apps/dialogs_app.h"
#include "apps/messages_app.h"
#include "apps/profiles_app.h"
#include "services/core1_services.h"
#include "services/feature_gates.h"
#include "services/input_keys.h"
#include "services/log.h"
#include "services/sms_picture_codec.h"
#include "services/strings.h"
#include "services/timebase.h"
#include "storage/store_service.h"
#include "ui/ui.h"
#include "../src/apps/messages/messages_internal.h"

static int s_failures;

static void check(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        s_failures++;
    }
}

static void check_str(const char *got, const char *want, const char *message) {
    if (strcmp(got, want) != 0) {
        fprintf(stderr, "FAIL: %s (got \"%s\", want \"%s\")\n", message, got, want);
        s_failures++;
    }
}

static uint8_t s_settings[STORE_SETTING_COUNT];
static char s_saved_words[APP_SMS_T9_USER_WORD_LIMIT][STORE_T9_WORD_MAX + 1u];
static uint8_t s_saved_word_count;
static unsigned s_saved_word_writes;
static bool s_picture_used[STORE_PICTURE_SLOT_COUNT];
static store_picture_message_t s_pictures[STORE_PICTURE_SLOT_COUNT];
static unsigned s_picture_writes;
static uint8_t s_picture_last_written_slot;
static bool s_modem_sim_ready;
static modem_call_state_t s_modem_call_state;
static uint32_t s_modem_sms_received_count;
static uint32_t s_modem_sms_user_received_count;
static uint32_t s_modem_sms_storage_full_events;
static uint8_t s_profile_message_alert;
static uint8_t s_profile_ringing_volume;
typedef struct {
    core1_cmd_t cmd;
    uint16_t arg;
} posted_command_t;
static posted_command_t s_posts[4];
static uint8_t s_post_count;
static bool s_save_sms_accept;
static unsigned s_save_sms_requests;
static char s_saved_sms_number[33];
static char s_saved_sms_text[MODEM_SMS_TEXT_MAX + 1u];
static unsigned s_send_sms_requests;
static char s_send_sms_number[MODEM_SMS_SENDER_MAX + 1u];
static char s_send_sms_text[MODEM_SMS_TEXT_MAX + 1u];
static bool s_binary_send_accept;
static unsigned s_binary_send_requests;
static char s_binary_number[33];
static uint8_t s_binary_payload[512];
static uint16_t s_binary_payload_len;
static uint16_t s_binary_dest_port;
static uint16_t s_binary_source_port;
static bool s_picture_encode_ok;
static store_picture_message_t s_encoded_picture;
static char s_encoded_text[STORE_PICTURE_TEXT_MAX + 1u];
static bool s_send_result_ready;
static modem_sms_send_result_t s_send_result;
static bool s_save_result_ready;
static modem_sms_save_result_t s_save_result;
static bool s_delete_result_ready;
static modem_sms_delete_result_t s_delete_result;
static uint32_t s_next_sms_request_id;
static modem_sms_record_t s_mailbox[6];
static uint8_t s_mailbox_count;
static bool s_mailbox_result_ready;
static modem_sms_mailbox_result_t s_mailbox_result;
static unsigned s_mailbox_requests;
static modem_sms_mailbox_t s_mailbox_requested_kind;
static unsigned s_read_requests;
static uint16_t s_read_indices[MODEM_SMS_SEGMENT_MAX];
static uint8_t s_read_index_count;
static bool s_read_quarantined;
static uint32_t s_read_identity_hash;
static bool s_read_result_ready;
static modem_sms_read_result_t s_read_result;
static unsigned s_display_calls;
static uint8_t s_display_record;
static uint16_t s_display_sid;
static app_route_t s_display_return_route;
static unsigned s_editor_calls;
static editor_context_t s_editor_context;
static uint8_t s_editor_max_len;
static char s_editor_title[32];
static char s_editor_value[40];
static uint16_t s_bitmap_ids[4];
static uint8_t s_bitmap_count;
static char s_right_text[16];
static char s_softkey[16];
static uint8_t s_list_count;
static uint8_t s_list_selected;
static char s_list_first[32];
static char s_list_last[32];
static unsigned s_pixel_count;
static char s_drawn_text[8][MODEM_SMS_TEXT_MAX + 1u];
static int s_drawn_y[8];
static uint8_t s_drawn_count;

static void reset_fixture(void) {
    memset(s_settings, 0, sizeof(s_settings));
    s_settings[STORE_SETTING_SMS_DICTIONARY_LANGUAGE] = 0x81u;
    memset(s_saved_words, 0, sizeof(s_saved_words));
    s_saved_word_count = 0u;
    s_saved_word_writes = 0u;
    memset(s_picture_used, 0, sizeof(s_picture_used));
    memset(s_pictures, 0, sizeof(s_pictures));
    s_picture_writes = 0u;
    s_picture_last_written_slot = 0xffu;
    s_modem_sim_ready = true;
    s_modem_call_state = MODEM_CALL_IDLE;
    s_modem_sms_received_count = 0u;
    s_modem_sms_user_received_count = 0u;
    s_modem_sms_storage_full_events = 0u;
    s_profile_message_alert = 0u;
    s_profile_ringing_volume = 0u;
    memset(s_posts, 0, sizeof(s_posts));
    s_post_count = 0u;
    s_save_sms_accept = true;
    s_save_sms_requests = 0u;
    s_saved_sms_number[0] = '\0';
    s_saved_sms_text[0] = '\0';
    s_send_sms_requests = 0u;
    s_send_sms_number[0] = '\0';
    s_send_sms_text[0] = '\0';
    s_binary_send_accept = true;
    s_binary_send_requests = 0u;
    s_binary_number[0] = '\0';
    memset(s_binary_payload, 0, sizeof(s_binary_payload));
    s_binary_payload_len = 0u;
    s_binary_dest_port = 0u;
    s_binary_source_port = 0u;
    s_picture_encode_ok = true;
    memset(&s_encoded_picture, 0, sizeof(s_encoded_picture));
    s_encoded_text[0] = '\0';
    s_send_result_ready = false;
    memset(&s_send_result, 0, sizeof(s_send_result));
    s_save_result_ready = false;
    memset(&s_save_result, 0, sizeof(s_save_result));
    s_delete_result_ready = false;
    memset(&s_delete_result, 0, sizeof(s_delete_result));
    s_next_sms_request_id = 0u;
    memset(s_mailbox, 0, sizeof(s_mailbox));
    s_mailbox_count = 0u;
    s_mailbox_result_ready = false;
    memset(&s_mailbox_result, 0, sizeof(s_mailbox_result));
    s_mailbox_requests = 0u;
    s_mailbox_requested_kind = MODEM_SMS_MAILBOX_INBOX;
    s_read_requests = 0u;
    memset(s_read_indices, 0, sizeof(s_read_indices));
    s_read_index_count = 0u;
    s_read_quarantined = false;
    s_read_identity_hash = 0u;
    s_read_result_ready = false;
    memset(&s_read_result, 0, sizeof(s_read_result));
    s_display_calls = 0u;
    s_display_record = 0u;
    s_display_sid = 0u;
    s_display_return_route = APP_ROUTE_STANDBY;
    s_editor_calls = 0u;
    s_editor_context = EDITOR_CONTEXT_NONE;
    s_editor_max_len = 0u;
    s_editor_title[0] = '\0';
    s_editor_value[0] = '\0';
    s_bitmap_count = 0u;
    memset(s_bitmap_ids, 0, sizeof(s_bitmap_ids));
    s_right_text[0] = '\0';
    s_softkey[0] = '\0';
    s_list_count = 0u;
    s_list_selected = 0u;
    s_list_first[0] = '\0';
    s_list_last[0] = '\0';
    s_pixel_count = 0u;
    memset(s_drawn_text, 0, sizeof(s_drawn_text));
    memset(s_drawn_y, 0, sizeof(s_drawn_y));
    s_drawn_count = 0u;
}

/* --------------------------------------------------------------- host seams */

void copy_text(char *dst, size_t cap, const char *src) {
    if (dst == NULL || cap == 0u) {
        return;
    }
    snprintf(dst, cap, "%s", src != NULL ? src : "");
}

int32_t time_diff_ms(uint32_t a, uint32_t b) {
    return (int32_t)(a - b);
}

const char *ts(uint16_t sid) {
    (void)sid;
    return NULL;
}

const char *ts_or(uint16_t sid, const char *fallback) {
    (void)sid;
    return fallback;
}

store_status_t store_setting_get_u8(store_setting_key_t key, uint8_t *out_value) {
    *out_value = s_settings[key];
    return STORE_STATUS_OK;
}

store_status_t store_setting_set_u8(store_setting_key_t key, uint8_t value) {
    s_settings[key] = value;
    return STORE_STATUS_OK;
}

store_status_t store_setting_get_text(store_setting_key_t key, char *out_text, uint8_t out_cap) {
    (void)key;
    copy_text(out_text, out_cap, "");
    return STORE_STATUS_OK;
}

store_status_t store_t9_user_words_load(char words[][STORE_T9_WORD_MAX + 1u],
                                        uint8_t word_cap,
                                        uint8_t *out_count) {
    uint8_t count = s_saved_word_count < word_cap ? s_saved_word_count : word_cap;
    for (uint8_t i = 0u; i < count; i++) {
        copy_text(words[i], STORE_T9_WORD_MAX + 1u, s_saved_words[i]);
    }
    *out_count = count;
    return STORE_STATUS_OK;
}

store_status_t store_t9_user_words_save(char words[][STORE_T9_WORD_MAX + 1u], uint8_t count) {
    s_saved_word_count = count < APP_SMS_T9_USER_WORD_LIMIT ? count : APP_SMS_T9_USER_WORD_LIMIT;
    for (uint8_t i = 0u; i < s_saved_word_count; i++) {
        copy_text(s_saved_words[i], sizeof(s_saved_words[i]), words[i]);
    }
    s_saved_word_writes++;
    return STORE_STATUS_OK;
}

uint8_t store_picture_message_count(void) {
    uint8_t count = 0u;
    for (uint8_t slot = 0u; slot < STORE_PICTURE_SLOT_COUNT; slot++) {
        if (s_picture_used[slot]) {
            count++;
        }
    }
    return count;
}

store_status_t store_picture_message_get(uint8_t slot, store_picture_message_t *out_message) {
    if (slot >= STORE_PICTURE_SLOT_COUNT || !s_picture_used[slot]) {
        return STORE_STATUS_NOT_FOUND;
    }
    *out_message = s_pictures[slot];
    return STORE_STATUS_OK;
}

store_status_t store_picture_message_set(uint8_t slot, const store_picture_message_t *message) {
    if (slot >= STORE_PICTURE_SLOT_COUNT || message == NULL) {
        return STORE_STATUS_INVALID_ARGUMENT;
    }
    s_picture_used[slot] = true;
    s_pictures[slot] = *message;
    s_picture_writes++;
    s_picture_last_written_slot = slot;
    return STORE_STATUS_OK;
}

uint8_t t9_dictionary_count(void) {
    return 2u;
}

const t9_dictionary_info_t *t9_dictionary_info(uint8_t index) {
    static const t9_dictionary_info_t INFO[] = {
        {"ENGL", "English"},
        {"GER", "Deutsch"},
    };
    return index < 2u ? &INFO[index] : NULL;
}

uint8_t t9_dictionary_lang_id(uint8_t index) {
    return (uint8_t)(index + 1u);
}

uint8_t t9_dictionary_index_for_lang_id(uint8_t lang_id) {
    return lang_id == 2u ? 1u : 0u;
}

static char keypad_digit(char ch) {
    if (ch >= 'A' && ch <= 'Z') {
        ch = (char)(ch + ('a' - 'A'));
    }
    if (ch >= 'a' && ch <= 'c') return '2';
    if (ch >= 'd' && ch <= 'f') return '3';
    if (ch >= 'g' && ch <= 'i') return '4';
    if (ch >= 'j' && ch <= 'l') return '5';
    if (ch >= 'm' && ch <= 'o') return '6';
    if (ch >= 'p' && ch <= 's') return '7';
    if (ch >= 't' && ch <= 'v') return '8';
    if (ch >= 'w' && ch <= 'z') return '9';
    return '\0';
}

bool t9_signature(const char *word, char *dst, uint8_t cap) {
    uint8_t out = 0u;
    for (size_t i = 0u; word != NULL && word[i] != '\0'; i++) {
        char digit = keypad_digit(word[i]);
        if (digit == '\0' || out + 1u >= cap) {
            return false;
        }
        dst[out++] = digit;
    }
    dst[out] = '\0';
    return out != 0u;
}

bool t9_candidates_for_sequence(const char *sequence,
                                uint8_t dictionary_index,
                                t9_candidate_list_t *out) {
    (void)dictionary_index;
    memset(out, 0, sizeof(*out));
    if (strcmp(sequence, "4663") == 0) {
        copy_text(out->words[0], sizeof(out->words[0]), "home");
        copy_text(out->words[1], sizeof(out->words[1]), "good");
        out->count = 2u;
    }
    return out->count != 0u;
}

void t9_fallback_word(const char *sequence, char *dst, uint8_t cap) {
    copy_text(dst, cap, sequence);
}

void t9_display_word(const char *word, uint8_t mode, bool sentence_start, char *dst, uint8_t cap) {
    copy_text(dst, cap, word);
    for (uint8_t i = 0u; dst[i] != '\0'; i++) {
        if (dst[i] >= 'A' && dst[i] <= 'Z') {
            dst[i] = (char)(dst[i] + ('a' - 'A'));
        }
    }
    if ((mode == SMS_MODE_UPPER || (mode == SMS_MODE_SENTENCE && sentence_start)) &&
        dst[0] >= 'a' && dst[0] <= 'z') {
        dst[0] = (char)(dst[0] - ('a' - 'A'));
    }
}

void modem_service_get_status(modem_status_t *out) {
    memset(out, 0, sizeof(*out));
    out->sim_ready = s_modem_sim_ready;
    out->call_state = s_modem_call_state;
    out->sms_received_count = s_modem_sms_received_count;
    out->sms_user_received_count = s_modem_sms_user_received_count;
    out->sms_storage_full_events = s_modem_sms_storage_full_events;
}

static bool admit_sms_request(bool accepted, uint32_t *request_id_out) {
    if (request_id_out == NULL) {
        return false;
    }
    *request_id_out = 0u;
    if (!accepted) {
        return false;
    }
    *request_id_out = ++s_next_sms_request_id;
    return true;
}

bool modem_service_request_save_sms(const char *number, const char *text,
                                    uint32_t *request_id_out) {
    s_save_sms_requests++;
    copy_text(s_saved_sms_number, sizeof(s_saved_sms_number), number);
    copy_text(s_saved_sms_text, sizeof(s_saved_sms_text), text);
    return admit_sms_request(s_save_sms_accept, request_id_out);
}

bool modem_service_get_voice_mailbox_number(char *out, size_t out_cap) {
    copy_text(out, out_cap, "");
    return false;
}

bool modem_service_request_sms_mailbox(modem_sms_mailbox_t mailbox,
                                       uint32_t *request_id_out) {
    s_mailbox_requests++;
    s_mailbox_requested_kind = mailbox;
    return admit_sms_request(true, request_id_out);
}

bool modem_service_request_sms_read(const uint16_t *indices,
                                    uint8_t index_count,
                                    bool quarantined,
                                    uint32_t expected_identity_hash,
                                    uint32_t *request_id_out) {
    s_read_requests++;
    s_read_index_count = index_count;
    if (index_count <= MODEM_SMS_SEGMENT_MAX) {
        memcpy(s_read_indices, indices, index_count * sizeof(indices[0]));
    }
    s_read_quarantined = quarantined;
    s_read_identity_hash = expected_identity_hash;
    return admit_sms_request(true, request_id_out);
}

bool modem_service_request_delete_sms_indices(const uint16_t *indices,
                                              uint8_t index_count,
                                              uint32_t *request_id_out) {
    (void)indices;
    (void)index_count;
    return admit_sms_request(true, request_id_out);
}

bool modem_service_request_send_sms(const char *number, const char *text,
                                    uint32_t *request_id_out) {
    s_send_sms_requests++;
    copy_text(s_send_sms_number, sizeof(s_send_sms_number), number);
    copy_text(s_send_sms_text, sizeof(s_send_sms_text), text);
    return admit_sms_request(true, request_id_out);
}

bool modem_service_request_send_binary_sms(const char *number,
                                           const uint8_t *payload,
                                           uint16_t payload_len,
                                           uint16_t dest_port,
                                           uint16_t source_port,
                                           uint32_t *request_id_out) {
    s_binary_send_requests++;
    copy_text(s_binary_number, sizeof(s_binary_number), number);
    s_binary_payload_len = payload_len;
    if (payload_len <= sizeof(s_binary_payload)) {
        memcpy(s_binary_payload, payload, payload_len);
    }
    s_binary_dest_port = dest_port;
    s_binary_source_port = source_port;
    return admit_sms_request(s_binary_send_accept, request_id_out);
}

bool sms_picture_payload_encode(const store_picture_message_t *message,
                                const char *text,
                                uint8_t *dst,
                                size_t cap,
                                uint16_t *out_len,
                                uint8_t *out_chunks) {
    s_encoded_picture = *message;
    copy_text(s_encoded_text, sizeof(s_encoded_text), text);
    if (!s_picture_encode_ok || cap < 3u) {
        return false;
    }
    dst[0] = 0x11u;
    dst[1] = 0x22u;
    dst[2] = 0x33u;
    *out_len = 3u;
    *out_chunks = 2u;
    return true;
}

bool sms_picture_payload_decode(const uint8_t *payload,
                                uint16_t payload_len,
                                store_picture_message_t *out_message) {
    (void)payload;
    (void)payload_len;
    (void)out_message;
    return false;
}

uint8_t audio_level_from_ringing_volume(uint8_t value) {
    return value;
}

uint16_t audio_arg(uint8_t code, uint8_t level) {
    return (uint16_t)(((uint16_t)level << 8u) | code);
}

bool modem_service_pop_sms_send_result(uint32_t request_id,
                                       modem_sms_send_result_t *out) {
    if (!s_send_result_ready || request_id == 0u || out == NULL ||
        s_send_result.request_id != request_id) {
        return false;
    }
    *out = s_send_result;
    s_send_result_ready = false;
    return true;
}

bool modem_service_pop_sms_mailbox_result(uint32_t request_id,
                                          modem_sms_mailbox_result_t *out) {
    if (!s_mailbox_result_ready || request_id == 0u || out == NULL ||
        s_mailbox_result.request_id != request_id) {
        return false;
    }
    *out = s_mailbox_result;
    s_mailbox_result_ready = false;
    return true;
}

bool modem_service_pop_sms_read_result(uint32_t request_id,
                                       modem_sms_read_result_t *out) {
    if (!s_read_result_ready || request_id == 0u || out == NULL ||
        s_read_result.request_id != request_id) {
        return false;
    }
    *out = s_read_result;
    s_read_result_ready = false;
    return true;
}

bool modem_service_pop_sms_delete_result(uint32_t request_id,
                                         modem_sms_delete_result_t *out) {
    if (!s_delete_result_ready || request_id == 0u || out == NULL ||
        s_delete_result.request_id != request_id) {
        return false;
    }
    *out = s_delete_result;
    s_delete_result_ready = false;
    return true;
}

bool modem_service_pop_sms_save_result(uint32_t request_id,
                                       modem_sms_save_result_t *out) {
    if (!s_save_result_ready || request_id == 0u || out == NULL ||
        s_save_result.request_id != request_id) {
        return false;
    }
    *out = s_save_result;
    s_save_result_ready = false;
    return true;
}

uint8_t modem_service_sms_mailbox_count(void) {
    return s_mailbox_count;
}

bool modem_service_sms_mailbox_entry(uint8_t position, modem_sms_record_t *out) {
    if (position >= s_mailbox_count) {
        return false;
    }
    *out = s_mailbox[position];
    return true;
}

void open_display(app_t *app,
                  uint8_t record_id,
                  const char *a,
                  const char *b,
                  const char *c,
                  app_route_t return_route,
                  uint32_t now) {
    (void)a;
    (void)b;
    (void)c;
    (void)now;
    s_display_calls++;
    s_display_record = record_id;
    s_display_return_route = return_route;
    app->route = APP_ROUTE_DISPLAY_MESSAGE;
    app->display_return_route = return_route;
    app->display_record_id = record_id;
}

void open_display_sid(app_t *app,
                      uint8_t record_id,
                      uint16_t sid,
                      const char *fallback,
                      app_route_t return_route,
                      uint32_t now) {
    (void)fallback;
    (void)now;
    s_display_calls++;
    s_display_record = record_id;
    s_display_sid = sid;
    s_display_return_route = return_route;
    app->route = APP_ROUTE_DISPLAY_MESSAGE;
    app->display_return_route = return_route;
    app->display_record_id = record_id;
}

uint8_t profile_active_index(void) {
    return 0u;
}

uint8_t profile_get_tone_setting(uint8_t profile_index,
                                 profile_setting_kind_t kind) {
    (void)profile_index;
    if (kind == PROFILE_SETTING_MESSAGE_ALERT) {
        return s_profile_message_alert;
    }
    if (kind == PROFILE_SETTING_RINGING_VOLUME) {
        return s_profile_ringing_volume;
    }
    return 0u;
}

void core1_post_command(core1_cmd_t cmd, uint16_t arg) {
    if (s_post_count < (uint8_t)(sizeof(s_posts) / sizeof(s_posts[0]))) {
        s_posts[s_post_count++] = (posted_command_t){.cmd = cmd, .arg = arg};
    }
}

void log_write(log_level_t level, const char *tag, const char *fmt, ...) {
    (void)level;
    (void)tag;
    (void)fmt;
}

void open_editor(app_t *app,
                 const char *title,
                 const char *value,
                 uint8_t max_len,
                 editor_kind_t kind,
                 editor_context_t context,
                 bool show_cursor,
                 uint32_t now) {
    (void)kind;
    (void)show_cursor;
    (void)now;
    s_editor_calls++;
    s_editor_context = context;
    s_editor_max_len = max_len;
    copy_text(s_editor_title, sizeof(s_editor_title), title);
    copy_text(s_editor_value, sizeof(s_editor_value), value);
    app->editor_context = (uint8_t)context;
    app->route = APP_ROUTE_EDITOR;
}

void open_confirm_sid(app_t *app,
                      confirm_context_t context,
                      uint16_t sid,
                      const char *fallback,
                      int8_t first_y) {
    (void)sid;
    (void)fallback;
    (void)first_y;
    app->confirm_context = context;
    app->route = APP_ROUTE_CONFIRM;
}

void resolve_contact_name(const char *number, char *dst, size_t cap) {
    (void)number;
    copy_text(dst, cap, "");
}

void close_editor(app_t *app) {
    app->route = APP_ROUTE_STANDBY;
}

const char *editor_chars_for_key(uint16_t key, editor_mode_t mode) {
    if (key == KEY_2) {
        return mode == EDITOR_MODE_LOWER ? "abc2" : "ABC2";
    }
    if (key == KEY_3) {
        return mode == EDITOR_MODE_LOWER ? "def3" : "DEF3";
    }
    return "";
}

bool feature_gate_visible(feature_gate_id_t gate) {
    (void)gate;
    return true;
}

static const font_t s_font = {
    .name = "test",
    .height = 7u,
    .spacing = 1u,
};

const font_t *asset_font(font_id_t font_id) {
    (void)font_id;
    return &s_font;
}

uint16_t asset_next_codepoint(const char **cursor) {
    const uint8_t *text = (const uint8_t *)*cursor;
    uint8_t first = text[0];
    if (first >= 0xc2u && first <= 0xdfu &&
        (text[1] & 0xc0u) == 0x80u) {
        *cursor += 2;
        return (uint16_t)(((uint16_t)(first & 0x1fu) << 6u) |
                          (text[1] & 0x3fu));
    }
    if (first >= 0xe0u && first <= 0xefu &&
        (text[1] & 0xc0u) == 0x80u &&
        (text[2] & 0xc0u) == 0x80u) {
        *cursor += 3;
        return (uint16_t)(((uint16_t)(first & 0x0fu) << 12u) |
                          ((uint16_t)(text[1] & 0x3fu) << 6u) |
                          (text[2] & 0x3fu));
    }
    (*cursor)++;
    return first;
}

const glyph_t *asset_glyph_codepoint(const font_t *font, uint16_t codepoint) {
    (void)font;
    static glyph_t glyph = {.height = 7u};
    if (codepoint == 'i' || codepoint == 'l' || codepoint == '.' ||
        codepoint == '\'') {
        glyph.width = 2u;
    } else if (codepoint == 'W') {
        glyph.width = 8u;
    } else {
        glyph.width = 5u;
    }
    return &glyph;
}

int asset_text_width(const font_t *font, const char *text) {
    int width = 0;
    bool first = true;
    while (text != NULL && *text != '\0') {
        const glyph_t *glyph = asset_glyph_codepoint(
            font, asset_next_codepoint(&text));
        width += (first ? 0 : font->spacing) + glyph->width;
        first = false;
    }
    return width;
}

void fb_clear(framebuffer_t *fb, bool color) {
    (void)fb;
    (void)color;
}

void fb_bitmap(framebuffer_t *fb, uint16_t bitmap_id, int x, int y, bool color, bool transparent) {
    (void)fb;
    (void)x;
    (void)y;
    (void)color;
    (void)transparent;
    if (s_bitmap_count < (uint8_t)(sizeof(s_bitmap_ids) / sizeof(s_bitmap_ids[0]))) {
        s_bitmap_ids[s_bitmap_count++] = bitmap_id;
    }
}

int fb_text(framebuffer_t *fb,
            const font_t *font,
            const char *text,
            int x,
            int y,
            bool color,
            int max_width) {
    (void)fb;
    (void)font;
    (void)x;
    (void)color;
    (void)max_width;
    if (s_drawn_count < (uint8_t)(sizeof(s_drawn_text) /
                                  sizeof(s_drawn_text[0]))) {
        copy_text(s_drawn_text[s_drawn_count],
                  sizeof(s_drawn_text[s_drawn_count]), text);
        s_drawn_y[s_drawn_count] = y;
        s_drawn_count++;
    }
    return 0;
}

void fb_hline(framebuffer_t *fb, int x, int y, int w, bool color) {
    (void)fb;
    (void)x;
    (void)y;
    (void)w;
    (void)color;
}

void fb_vline(framebuffer_t *fb, int x, int y, int h, bool color) {
    (void)fb;
    (void)x;
    (void)y;
    (void)h;
    (void)color;
}

void fb_fill_rect(framebuffer_t *fb, int x, int y, int w, int h, bool color) {
    (void)fb;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)color;
}

void fb_pixel(framebuffer_t *fb, int x, int y, bool color) {
    (void)fb;
    (void)x;
    (void)y;
    if (color) {
        s_pixel_count++;
    }
}

void draw_right_text_box(framebuffer_t *fb,
                         const font_t *font,
                         const char *text,
                         int x,
                         int y,
                         int width) {
    (void)fb;
    (void)font;
    (void)x;
    (void)y;
    (void)width;
    copy_text(s_right_text, sizeof(s_right_text), text);
}

void draw_softkey(framebuffer_t *fb, const char *label) {
    (void)fb;
    copy_text(s_softkey, sizeof(s_softkey), label);
}

void draw_flat_list(framebuffer_t *fb,
                    const char *const *labels,
                    uint8_t count,
                    uint8_t selected,
                    const char *breadcrumb,
                    const char *softkey) {
    (void)fb;
    (void)breadcrumb;
    (void)softkey;
    s_list_count = count;
    s_list_selected = selected;
    copy_text(s_list_first, sizeof(s_list_first), count != 0u ? labels[0] : "");
    copy_text(s_list_last, sizeof(s_list_last), count != 0u ? labels[count - 1u] : "");
}

void draw_flat_list_at_y(framebuffer_t *fb,
                         const char *const *labels,
                         uint8_t count,
                         uint8_t selected,
                         const char *breadcrumb,
                         const char *softkey,
                         int y) {
    (void)y;
    draw_flat_list(fb, labels, count, selected, breadcrumb, softkey);
    copy_text(s_softkey, sizeof(s_softkey), softkey);
}

void draw_static_page_list(framebuffer_t *fb,
                           const char *label,
                           const char *preview,
                           uint8_t selected,
                           uint8_t count,
                           const char *breadcrumb,
                           const char *softkey) {
    (void)preview;
    const char *labels[] = {label};
    draw_flat_list(fb, labels, count != 0u ? 1u : 0u, selected, breadcrumb, softkey);
}

void draw_text_block(framebuffer_t *fb,
                     const font_t *font,
                     const char *text,
                     int x,
                     int y,
                     int width,
                     int pitch,
                     uint8_t max_lines) {
    (void)fb;
    (void)font;
    (void)text;
    (void)x;
    (void)y;
    (void)width;
    (void)pitch;
    (void)max_lines;
}

uint16_t ui_wrap_line_count(const font_t *font,
                            const char *text,
                            int width) {
    (void)font;
    (void)width;
    return text != NULL && text[0] != '\0' ? 1u : 0u;
}

bool ui_wrap_line_at(const font_t *font,
                     const char *text,
                     int width,
                     uint16_t line_index,
                     char *line,
                     size_t line_cap) {
    (void)font;
    (void)width;
    if (line == NULL || line_cap == 0u || text == NULL ||
        text[0] == '\0' || line_index != 0u) {
        if (line != NULL && line_cap != 0u) {
            line[0] = '\0';
        }
        return false;
    }
    copy_text(line, line_cap, text);
    return true;
}

const char *ui_marquee_text(const font_t *font,
                            const char *text,
                            int max_width,
                            char *buf,
                            size_t cap) {
    (void)font;
    (void)max_width;
    copy_text(buf, cap, text);
    return buf;
}

static void seed_picture(uint8_t slot,
                         const char *text,
                         uint8_t width,
                         uint8_t height) {
    store_picture_message_t *picture = &s_pictures[slot];
    memset(picture, 0, sizeof(*picture));
    s_picture_used[slot] = true;
    picture->used = true;
    picture->width = width;
    picture->height = height;
    picture->bitmap_len = 1u;
    picture->bitmap[0] = 0x80u;
    copy_text(picture->text, sizeof(picture->text), text);
}

static void seed_mailbox_row(uint8_t position,
                             uint16_t index,
                             uint32_t identity_hash,
                             const char *status,
                             const char *sender,
                             const char *timestamp) {
    modem_sms_record_t *row = &s_mailbox[position];
    memset(row, 0, sizeof(*row));
    row->indices[0] = index;
    row->index_count = 1u;
    row->identity_hash = identity_hash;
    copy_text(row->status, sizeof(row->status), status);
    copy_text(row->sender, sizeof(row->sender), sender);
    copy_text(row->timestamp, sizeof(row->timestamp), timestamp);
}

/* --------------------------------------------------------------- scenarios */

static void test_init_loads_composer_state(void) {
    reset_fixture();
    s_settings[STORE_SETTING_SMS_DICTIONARY_ACTIVE] = 1u;
    copy_text(s_saved_words[0], sizeof(s_saved_words[0]), "sisu");
    s_saved_word_count = 1u;
    app_t app;
    memset(&app, 0xa5, sizeof(app));

    messages_app_init(&app);
    check(app.sms_composer_mode == SMS_MODE_SENTENCE &&
              app.sms_send_return_route == APP_ROUTE_SMS_COMPOSER,
          "messages init seeds the composer mode and send return route");
    check(app.sms_dictionary_active && app.sms_dictionary_index == 0u,
          "messages init restores the persisted dictionary selection");
    check(app.sms_t9_user_word_count == 1u,
          "messages init restores the learned-word count");
    check_str(app.sms_t9_user_words[0], "sisu",
              "messages init restores learned-word contents");
    check(app.sms_unread_count == 0u, "messages init clears the runtime unread counter");
}

static void test_open_multitap_delete_and_cursor(void) {
    reset_fixture();
    app_t app;
    memset(&app, 0, sizeof(app));

    open_sms_composer(&app, "", "5551234", 100u);
    check(app.route == APP_ROUTE_SMS_COMPOSER && app.sms_composer_cursor == 0u,
          "open starts an empty composer at cursor zero");
    check_str(app.sms_recipient_prefill, "5551234", "open preserves the recipient prefill");
    check(app.sms_composer_mode == SMS_MODE_SENTENCE && !app.sms_dictionary_active,
          "open starts sentence mode with the persisted dictionary state");

    (void)handle_sms_composer_key(&app, KEY_2, EVENT_KEY_DOWN, 100u);
    (void)handle_sms_composer_key(&app, KEY_2, EVENT_KEY_DOWN, 500u);
    (void)handle_sms_composer_key(&app, KEY_3, EVENT_KEY_DOWN, 1500u);
    check_str(app.sms_composer_text, "Bd", "multi-tap cycles in-window and commits after timeout");
    check(app.sms_composer_cursor == 2u, "multi-tap keeps the insertion point after the text");

    (void)handle_sms_composer_key(&app, KEY_C, EVENT_KEY_DOWN, 1600u);
    check_str(app.sms_composer_text, "B", "C deletes one character before the cursor");
    (void)handle_sms_composer_key(&app, KEY_C, EVENT_KEY_HOLD, 1700u);
    check_str(app.sms_composer_text, "", "held C clears the whole composer");
    (void)handle_sms_composer_key(&app, KEY_C, EVENT_KEY_DOWN, 1800u);
    check(app.route == APP_ROUTE_MESSAGES_MENU && app.messages_menu_selected == 2u,
          "C on an empty composer returns to Write messages");

    open_sms_composer(&app, "ABCDEFGHIJKLMNOPQRST", "", 2000u);
    check(app.sms_composer_cursor == 20u, "prefilled text starts with the cursor at the end");
    (void)handle_sms_composer_key(&app, KEY_UP, EVENT_KEY_DOWN, 2010u);
    check(app.sms_composer_cursor == 7u, "Up preserves the visual column on the previous wrapped line");
    (void)handle_sms_composer_key(&app, KEY_DOWN, EVENT_KEY_DOWN, 2020u);
    check(app.sms_composer_cursor == 20u, "Down returns to the matching end column");
}

static void test_composer_long_press_enters_digit(void) {
    reset_fixture();
    app_t app;
    memset(&app, 0, sizeof(app));

    open_sms_composer(&app, "", "", 2100u);
    (void)handle_sms_composer_key(&app, KEY_2, EVENT_KEY_DOWN, 2110u);
    (void)handle_sms_composer_key(&app, KEY_2, EVENT_KEY_HOLD, 2710u);
    check_str(app.sms_composer_text, "2",
              "holding a manual-mode key replaces its provisional letter with the digit");

    open_sms_composer(&app, "", "", 2800u);
    (void)handle_sms_composer_key(&app, KEY_2, EVENT_KEY_DOWN, 2810u);
    (void)handle_sms_composer_key(&app, KEY_2, EVENT_KEY_DOWN, 3000u);
    (void)handle_sms_composer_key(&app, KEY_2, EVENT_KEY_HOLD, 3600u);
    check_str(app.sms_composer_text, "A2",
              "a held repeat restores the preceding tap before appending its digit");

    open_sms_composer(&app, "", "", 3700u);
    app.sms_composer_mode = SMS_MODE_NUMERIC;
    (void)handle_sms_composer_key(&app, KEY_3, EVENT_KEY_DOWN, 3710u);
    (void)handle_sms_composer_key(&app, KEY_3, EVENT_KEY_HOLD, 4310u);
    check_str(app.sms_composer_text, "3",
              "holding in numeric mode does not duplicate the key-down digit");

    s_settings[STORE_SETTING_SMS_DICTIONARY_ACTIVE] = 1u;
    open_sms_composer(&app, "", "", 4400u);
    (void)handle_sms_composer_key(&app, KEY_4, EVENT_KEY_DOWN, 4410u);
    (void)handle_sms_composer_key(&app, KEY_4, EVENT_KEY_HOLD, 5010u);
    check_str(app.sms_composer_text, "4",
              "holding a T9 key replaces only that provisional sequence digit");
    check_str(app.sms_t9_sequence, "",
              "literal digit insertion commits and clears the T9 composition");

    open_sms_composer(&app, "", "", 5100u);
    (void)handle_sms_composer_key(&app, KEY_0, EVENT_KEY_DOWN, 5110u);
    (void)handle_sms_composer_key(&app, KEY_0, EVENT_KEY_HOLD, 5710u);
    check_str(app.sms_composer_text, "0",
              "holding zero replaces T9's provisional space with zero");

    char full[MODEM_SMS_TEXT_MAX + 1u];
    memset(full, 'x', MODEM_SMS_TEXT_MAX);
    full[MODEM_SMS_TEXT_MAX] = '\0';
    open_sms_composer(&app, full, "", 5800u);
    (void)handle_sms_composer_key(&app, KEY_5, EVENT_KEY_DOWN, 5810u);
    (void)handle_sms_composer_key(&app, KEY_5, EVENT_KEY_HOLD, 6410u);
    check(strlen(app.sms_composer_text) == MODEM_SMS_TEXT_MAX &&
              app.sms_composer_text[MODEM_SMS_TEXT_MAX - 1u] == 'x',
          "an ignored full-buffer key-down cannot make hold delete existing text");

    open_sms_composer(&app, "tail", "", 6500u);
    (void)handle_sms_composer_key(&app, KEY_6, EVENT_KEY_HOLD, 7100u);
    check_str(app.sms_composer_text, "tail",
              "a hold without its matching key-down cannot alter existing text");
}

static void test_limits_modes_and_symbols(void) {
    reset_fixture();
    app_t app;
    memset(&app, 0, sizeof(app));
    char full[MODEM_SMS_TEXT_MAX + 1u];
    memset(full, 'x', MODEM_SMS_TEXT_MAX);
    full[MODEM_SMS_TEXT_MAX] = '\0';
    open_sms_composer(&app, full, "", 3000u);

    (void)handle_sms_composer_key(&app, KEY_2, EVENT_KEY_DOWN, 3010u);
    check(strlen(app.sms_composer_text) == MODEM_SMS_TEXT_MAX,
          "normal key input cannot exceed the 160-character limit");
    check(!sms_t9_insert_text(&app, "overflow", false, false),
          "direct T9 insertion reports a full composer without overflowing");

    open_sms_composer(&app, "", "", 3020u);
    (void)handle_sms_composer_key(&app, KEY_HASH, EVENT_KEY_HOLD, 3030u);
    check(app.sms_composer_mode == SMS_MODE_NUMERIC, "held hash enters numeric mode");
    (void)handle_sms_composer_key(&app, KEY_STAR, EVENT_KEY_DOWN, 3040u);
    (void)handle_sms_composer_key(&app, KEY_STAR, EVENT_KEY_DOWN, 3050u);
    check_str(app.sms_composer_text, "+", "numeric star cycles through star and plus");

    (void)handle_sms_composer_key(&app, KEY_HASH, EVENT_KEY_HOLD, 3060u);
    (void)handle_sms_composer_key(&app, KEY_STAR, EVENT_KEY_DOWN, 3070u);
    check(app.route == APP_ROUTE_SMS_SYMBOLS && app.sms_symbols_selected == 0u,
          "star opens the symbol grid outside numeric mode");
    app.sms_symbols_selected = 3u;
    (void)handle_sms_symbols_key(&app, KEY_NAVI, 3080u);
    check(app.route == APP_ROUTE_SMS_COMPOSER, "using a symbol returns to the composer");
    check_str(app.sms_composer_text, "+!", "the selected symbol is inserted at the cursor");
    (void)handle_sms_composer_key(&app, KEY_STAR, EVENT_KEY_DOWN, 3090u);
    (void)handle_sms_symbols_key(&app, KEY_C, 3100u);
    check_str(app.sms_composer_text, "+!", "C leaves the symbol grid without inserting");
}

static void test_t9_candidates_spell_and_user_word(void) {
    reset_fixture();
    s_settings[STORE_SETTING_SMS_DICTIONARY_ACTIVE] = 1u;
    app_t app;
    memset(&app, 0, sizeof(app));
    open_sms_composer(&app, "", "", 4000u);

    (void)handle_sms_composer_key(&app, KEY_4, EVENT_KEY_DOWN, 4010u);
    (void)handle_sms_composer_key(&app, KEY_6, EVENT_KEY_DOWN, 4020u);
    (void)handle_sms_composer_key(&app, KEY_6, EVENT_KEY_DOWN, 4030u);
    (void)handle_sms_composer_key(&app, KEY_3, EVENT_KEY_DOWN, 4040u);
    check_str(app.sms_composer_text, "Home", "T9 replaces the active digit sequence with its first candidate");
    check_str(app.sms_t9_sequence, "4663", "T9 retains the active sequence for candidate cycling");
    (void)handle_sms_composer_key(&app, KEY_STAR, EVENT_KEY_DOWN, 4050u);
    check_str(app.sms_composer_text, "Good", "star cycles to the next T9 candidate");
    (void)handle_sms_composer_key(&app, KEY_STAR, EVENT_KEY_DOWN, 4060u);
    check(app.sms_t9_spell_offer, "cycling past the final candidate offers Spell");

    (void)handle_sms_composer_key(&app, KEY_NAVI, EVENT_KEY_DOWN, 4070u);
    check(s_editor_calls == 1u && s_editor_context == EDITOR_CONTEXT_SMS_T9_INSERT_WORD &&
              s_editor_max_len == T9_WORD_MAX && app.route == APP_ROUTE_EDITOR,
          "Spell opens the bounded Insert word editor");

    open_sms_composer(&app, "tail", "", 4080u);
    check(sms_t9_insert_text(&app, "Sisu", false, true), "inserted user word reports success");
    check_str(app.sms_composer_text, "tailSisu", "inserted user word appends to ordinary text");
    check(s_saved_word_writes == 1u && s_saved_word_count == 1u,
          "adding a user word persists the learned dictionary");
    check_str(s_saved_words[0], "sisu", "learned words are normalized before persistence");

    open_sms_composer(&app, "headtail", "", 4090u);
    app.sms_composer_cursor = 4u;
    check(sms_t9_insert_text(&app, "MID", false, false),
          "Insert word/number accepts text at a mid-message cursor");
    check_str(app.sms_composer_text, "headMIDtail",
              "Insert word/number inserts at the cursor instead of appending");
    check(app.sms_composer_cursor == 7u,
          "Insert word/number advances the cursor past inserted text");

    char nearly_full[MODEM_SMS_TEXT_MAX + 1u];
    memset(nearly_full, 'x', MODEM_SMS_TEXT_MAX - 2u);
    nearly_full[MODEM_SMS_TEXT_MAX - 2u] = '\0';
    open_sms_composer(&app, nearly_full, "", 4100u);
    app.sms_composer_cursor = 10u;
    char before[MODEM_SMS_TEXT_MAX + 1u];
    copy_text(before, sizeof(before), app.sms_composer_text);
    unsigned writes_before = s_saved_word_writes;
    check(!sms_t9_insert_text(&app, "word", false, true),
          "an Insert word that cannot fit is rejected");
    check_str(app.sms_composer_text, before,
              "a rejected Insert word leaves the draft byte-for-byte unchanged");
    check(app.sms_composer_cursor == 10u,
          "a rejected Insert word leaves the cursor unchanged");
    check(s_saved_word_writes == writes_before,
          "a rejected Insert word is not learned as if it had been inserted");
}

static void open_plain_options(app_t *app, const char *text, const char *recipient, uint32_t now) {
    open_sms_composer(app, text, recipient, now);
    (void)handle_sms_composer_key(app, KEY_NAVI, EVENT_KEY_DOWN, now + 1u);
    check(app->route == APP_ROUTE_SMS_OPTIONS && app->sms_options_selected == 0u,
          "Navi opens the normal composer options at Send");
}

static void test_options_and_return_routes(void) {
    reset_fixture();
    app_t app;
    memset(&app, 0, sizeof(app));

    open_plain_options(&app, "hello", "18005551234", 5000u);
    (void)handle_sms_options_key(&app, KEY_NAVI, 5002u);
    check(s_editor_context == EDITOR_CONTEXT_SMS_RECIPIENT && s_editor_max_len == 21u,
          "Send opens the SMS recipient editor");
    check_str(s_editor_value, "18005551234", "Send carries the recipient prefill into the editor");

    open_plain_options(&app, "draft", "555", 5010u);
    app.sms_options_selected = 3u; /* Save in the dictionary-off menu. */
    (void)handle_sms_options_key(&app, KEY_NAVI, 5012u);
    check(s_save_sms_requests == 1u && app.sms_save_waiting,
          "Save admits one modem save and arms its result wait");
    check_str(s_saved_sms_number, "555", "Save passes the draft recipient");
    check_str(s_saved_sms_text, "draft", "Save passes the exact composer body");
    check(s_display_record == 4u && s_display_return_route == APP_ROUTE_SMS_COMPOSER,
          "Save shows the original Saving record over the composer");

    open_plain_options(&app, "erase me", "", 5020u);
    app.sms_options_selected = 4u; /* Clear screen. */
    (void)handle_sms_options_key(&app, KEY_NAVI, 5022u);
    check(app.route == APP_ROUTE_SMS_COMPOSER && app.sms_composer_text[0] == '\0' &&
              app.sms_composer_cursor == 0u,
          "Clear screen empties the draft and returns to composition");

    open_plain_options(&app, "exit me", "123", 5030u);
    app.sms_options_selected = 5u; /* Exit. */
    (void)handle_sms_options_key(&app, KEY_NAVI, 5032u);
    check(app.route == APP_ROUTE_STANDBY && app.sms_composer_text[0] == '\0' &&
              app.sms_recipient_prefill[0] == '\0',
          "Exit clears transient text and recipient before returning to standby");

    open_plain_options(&app, "keep", "", 5040u);
    (void)handle_sms_options_key(&app, KEY_C, 5042u);
    check(app.route == APP_ROUTE_SMS_COMPOSER && strcmp(app.sms_composer_text, "keep") == 0,
          "C closes options without changing the draft");

    app.messages_picture_text_editing = true;
    app.sms_composer_mode = SMS_MODE_UPPER;
    copy_text(app.sms_composer_text, sizeof(app.sms_composer_text), "caption");
    copy_text(app.messages_picture_draft, sizeof(app.messages_picture_draft), "caption");
    app.sms_composer_cursor = 7u;
    app.sms_options_selected = 2u; /* Clear text. */
    app.route = APP_ROUTE_SMS_OPTIONS;
    (void)handle_sms_options_key(&app, KEY_NAVI, 5050u);
    check(app.route == APP_ROUTE_SMS_COMPOSER && app.sms_composer_text[0] == '\0' &&
              app.messages_picture_draft[0] == '\0',
          "picture Clear text clears both draft mirrors and returns to composition");
    check(app.sms_composer_mode == SMS_MODE_UPPER,
          "picture Clear text preserves the active input mode");
}

static void test_tick_and_render_contract(void) {
    reset_fixture();
    app_t app;
    memset(&app, 0, sizeof(app));
    open_sms_composer(&app, "hello", "", 6000u);
    app.sms_composer_cursor_blink_ms = 6000u;
    check(!tick_sms_composer(&app, 6511u) && app.sms_composer_cursor_visible,
          "cursor stays solid before the 512 ms blink deadline");
    check(tick_sms_composer(&app, 6512u) && !app.sms_composer_cursor_visible,
          "cursor toggles exactly at the blink deadline");

    framebuffer_t fb;
    memset(&fb, 0, sizeof(fb));
    s_bitmap_count = 0u;
    render_sms_composer(&app, &fb);
    check(s_bitmap_count >= 2u && s_bitmap_ids[0] == 14u && s_bitmap_ids[1] == 7u,
          "dictionary-off sentence composer renders pencil and Abc badges");
    check_str(s_right_text, "155", "composer counter renders the remaining character count");
    check_str(s_softkey, "Options", "ordinary composition renders Options");

    app.sms_dictionary_active = true;
    copy_text(app.sms_t9_sequence, sizeof(app.sms_t9_sequence), "4663");
    app.sms_t9_word_start = 0u;
    app.sms_t9_word_end = 4u;
    app.sms_t9_spell_offer = true;
    s_bitmap_count = 0u;
    render_sms_composer(&app, &fb);
    check(s_bitmap_ids[0] == 8u, "active dictionary renders the T9 badge");
    check_str(s_softkey, "Spell", "exhausted candidates render the Spell softkey");

    app.sms_dictionary_active = false;
    app.sms_options_dictionary = false;
    app.sms_options_selected = 5u;
    render_sms_options(&app, &fb);
    check(s_list_count == 6u && s_list_selected == 5u,
          "dictionary-off options expose the complete six-item menu");
    check_str(s_list_first, "Send", "composer options start with Send");
    check_str(s_list_last, "Exit", "composer options end with Exit");
}

static void test_composer_full_text_and_utf8_editing(void) {
    reset_fixture();
    app_t app;
    memset(&app, 0, sizeof(app));
    char narrow[MODEM_SMS_TEXT_MAX + 1u];
    memset(narrow, 'i', MODEM_SMS_TEXT_MAX);
    narrow[MODEM_SMS_TEXT_MAX] = '\0';
    open_sms_composer(&app, narrow, "", 6100u);

    framebuffer_t fb;
    memset(&fb, 0, sizeof(fb));
    render_sms_composer(&app, &fb);
    check(s_drawn_count == 4u,
          "the composer renders all four rows of its cursor-following viewport");
    check(strlen(s_drawn_text[0]) == 27u &&
              strlen(s_drawn_text[1]) == 27u &&
              strlen(s_drawn_text[2]) == 27u &&
              strlen(s_drawn_text[3]) == 25u,
          "160 narrow glyphs reach their final visual row without a byte cap");
    check(s_drawn_y[0] == 8 && s_drawn_y[3] == 38,
          "long composer rows retain the stock four-row geometry");

    reset_fixture();
    memset(&app, 0, sizeof(app));
    const char *utf8 = "A\xce\x94\xe2\x82\xac" "B";
    open_sms_composer(&app, utf8, "", 6200u);
    app.sms_composer_cursor = 5u; /* deliberately inside the euro sequence */
    (void)handle_sms_composer_key(&app, KEY_2, EVENT_KEY_DOWN, 6210u);
    check_str(app.sms_composer_text,
              "A\xce\x94" "a" "\xe2\x82\xac" "B",
              "mid-glyph insertion clamps to the preceding UTF-8 boundary");
    check(app.sms_composer_cursor == 4u,
          "mid-glyph insertion leaves the cursor on a valid boundary");
    (void)handle_sms_composer_key(&app, KEY_C, EVENT_KEY_DOWN, 6220u);
    check_str(app.sms_composer_text, utf8,
              "C removes the inserted ASCII glyph without damaging its UTF-8 neighbor");
    app.sms_composer_cursor = 6u; /* immediately after the three-byte euro */
    (void)handle_sms_composer_key(&app, KEY_C, EVENT_KEY_DOWN, 6230u);
    check_str(app.sms_composer_text, "A\xce\x94" "B",
              "C removes one complete multibyte glyph");
    check(app.sms_composer_cursor == 3u,
          "multibyte deletion returns to the previous codepoint boundary");

    reset_fixture();
    memset(&app, 0, sizeof(app));
    char caption[STORE_PICTURE_TEXT_MAX + 1u];
    memset(caption, 'i', STORE_PICTURE_TEXT_MAX);
    caption[STORE_PICTURE_TEXT_MAX] = '\0';
    messages_composer_open_picture(&app, caption, 6300u);
    render_sms_composer(&app, &fb);
    check(s_drawn_count == 4u &&
              strlen(s_drawn_text[0]) == 27u &&
              strlen(s_drawn_text[1]) == 27u &&
              strlen(s_drawn_text[2]) == 27u &&
              strlen(s_drawn_text[3]) == 12u,
          "picture captions use the same complete pixel-measured viewport");
}

static void test_picture_sparse_list_preview_and_options(void) {
    reset_fixture();
    seed_picture(1u, "First caption", STORE_PICTURE_WIDTH, STORE_PICTURE_HEIGHT);
    seed_picture(3u, "", STORE_PICTURE_WIDTH, STORE_PICTURE_HEIGHT);
    app_t app;
    memset(&app, 0, sizeof(app));
    app.messages_menu_selected = 3u;

    (void)handle_messages_menu_key(&app, KEY_NAVI, 7000u);
    check(app.route == APP_ROUTE_MESSAGES_LIST &&
              app.messages_kind == MESSAGES_KIND_PICTURES &&
              app.messages_mode == MESSAGES_MODE_LIST,
          "Picture messages opens the saved-picture list");

    framebuffer_t fb;
    memset(&fb, 0, sizeof(fb));
    render_messages_list(&app, &fb);
    check(s_list_count == 2u && s_list_selected == 0u,
          "sparse picture slots render as two compact list rows");
    check_str(s_list_first, "First caption", "picture list uses a stored caption");
    check_str(s_list_last, "Picture 4", "empty caption falls back to the physical slot number");
    check_str(s_softkey, "View", "saved-picture list renders View");

    (void)handle_messages_list_key(&app, KEY_DOWN, 7010u);
    (void)handle_messages_list_key(&app, KEY_NAVI, 7020u);
    check(app.messages_selected == 1u && app.messages_mode == MESSAGES_MODE_READ,
          "opening a sparse list row preserves its compact ordinal");
    s_pixel_count = 0u;
    render_messages_list(&app, &fb);
    check(s_pixel_count == 1u, "picture preview renders the stored bitmap bit");
    check_str(s_softkey, "Options", "picture preview renders Options");

    (void)handle_messages_list_key(&app, KEY_NAVI, 7030u);
    render_messages_list(&app, &fb);
    check(app.messages_mode == MESSAGES_MODE_OPTIONS && s_list_count == 5u,
          "picture preview opens the five-item saved-picture menu");
    check_str(s_list_first, "Edit text", "saved-picture options start with Edit text");
    check_str(s_list_last, "Details", "saved-picture options end with Details");

    app.messages_option_selected = 1u;
    (void)handle_messages_list_key(&app, KEY_NAVI, 7040u);
    check(app.route == APP_ROUTE_CONFIRM &&
              app.confirm_context == CONFIRM_CONTEXT_PICTURE_MESSAGE_ERASE,
          "Erase routes through the picture-message confirmation context");
    app.route = APP_ROUTE_MESSAGES_LIST;
    app.messages_mode = MESSAGES_MODE_OPTIONS;
    app.messages_option_selected = 2u;
    (void)handle_messages_list_key(&app, KEY_NAVI, 7050u);
    check(s_display_sid == 0x33au,
          "Use number shows the v6.00 no-number note for a saved picture");
    app.route = APP_ROUTE_MESSAGES_LIST;
    app.messages_mode = MESSAGES_MODE_OPTIONS;
    app.messages_option_selected = 3u;
    (void)handle_messages_list_key(&app, KEY_NAVI, 7060u);
    check(s_editor_context == EDITOR_CONTEXT_PICTURE_RECIPIENT &&
              s_editor_max_len == 21u,
          "Forward opens the bounded picture-recipient editor");
    app.route = APP_ROUTE_MESSAGES_LIST;
    app.messages_mode = MESSAGES_MODE_OPTIONS;
    app.messages_option_selected = 4u;
    (void)handle_messages_list_key(&app, KEY_NAVI, 7070u);
    check(s_display_sid == 0x177u,
          "Details shows the saved-picture no-more-details note");
}

static void test_picture_edit_save_preview_and_limit(void) {
    reset_fixture();
    seed_picture(2u, "old", STORE_PICTURE_WIDTH, STORE_PICTURE_HEIGHT);
    app_t app;
    memset(&app, 0, sizeof(app));
    app.messages_kind = MESSAGES_KIND_PICTURES;
    app.messages_mode = MESSAGES_MODE_OPTIONS;
    app.messages_option_selected = 0u;

    (void)handle_messages_list_key(&app, KEY_NAVI, 8000u);
    check(app.route == APP_ROUTE_SMS_COMPOSER && app.messages_picture_text_editing,
          "Edit text opens the picture composer");
    check_str(app.sms_composer_text, "old", "picture edit starts from the stored caption");
    check(!app.sms_composer_cursor_visible,
          "picture edit preserves the pre-split hidden cursor phase");
    check(app.messages_picture_pending_valid && app.messages_picture_pending_slot == 2u,
          "picture edit binds the physical storage slot");
    check(messages_picture_composer_max_len(&app) == STORE_PICTURE_TEXT_MAX,
          "stock 72x28 pictures retain the full caption limit");

    app_t phase_app;
    memset(&phase_app, 0, sizeof(phase_app));
    phase_app.sms_composer_cursor_visible = true;
    messages_composer_open_picture(&phase_app, "phase", 8001u);
    check(phase_app.sms_composer_cursor_visible,
          "picture edit leaves an already-visible cursor phase untouched");

    s_pictures[2].height = 42u;
    check(messages_picture_composer_max_len(&app) == 20u,
          "oversized picture payloads retain the protocol minimum text room");
    s_pictures[2].height = STORE_PICTURE_HEIGHT;

    copy_text(app.sms_composer_text, sizeof(app.sms_composer_text), "new caption");
    messages_picture_select_editor_option(&app, 3u, 8010u);
    check(app.route == APP_ROUTE_MESSAGES_LIST && app.messages_mode == MESSAGES_MODE_READ,
          "Preview returns to the saved-picture read view");
    check_str(app.messages_picture_draft, "new caption", "Preview preserves the unsaved draft");
    check(s_picture_writes == 0u, "Preview does not write picture storage");

    app.messages_picture_text_editing = true;
    copy_text(app.sms_composer_text, sizeof(app.sms_composer_text), "saved caption");
    messages_picture_select_editor_option(&app, 1u, 8020u);
    check(s_picture_writes == 1u && s_picture_last_written_slot == 2u,
          "Save writes the bound physical picture slot exactly once");
    check_str(s_pictures[2].text, "saved caption", "Save persists the edited caption");
    check(s_display_record == 3u && s_display_sid == 0x180u &&
              s_display_return_route == APP_ROUTE_MESSAGES_LIST,
          "Save shows the v6.00 Picture message saved record");
}

static void test_picture_send_contract(void) {
    reset_fixture();
    seed_picture(0u, "stored", STORE_PICTURE_WIDTH, STORE_PICTURE_HEIGHT);
    app_t app;
    memset(&app, 0, sizeof(app));
    app.messages_kind = MESSAGES_KIND_PICTURES;
    app.messages_mode = MESSAGES_MODE_READ;
    app.messages_picture_pending_valid = true;
    app.messages_picture_pending_slot = 0u;
    copy_text(app.messages_picture_draft, sizeof(app.messages_picture_draft), "draft");
    copy_text(app.editor_value, sizeof(app.editor_value), "18005550199");

    messages_submit_picture_recipient(&app, 9000u);
    check(s_binary_send_requests == 1u && app.messages_picture_send_waiting,
          "picture recipient submission admits one binary SMS request");
    check_str(s_binary_number, "18005550199", "binary send uses the edited recipient");
    check(s_binary_dest_port == SMS_CODEC_PICTURE_PORT &&
              s_binary_source_port == 0u,
          "picture send uses Nokia's destination and source ports");
    check(s_binary_payload_len == 3u && s_binary_payload[0] == 0x11u &&
              app.messages_picture_payload_chunks == 2u,
          "picture send publishes the codec payload length and segment count");
    check_str(s_encoded_text, "draft", "picture send encodes the pending draft caption");
    check_str(s_encoded_picture.text, "draft", "picture send applies the draft to the encoded record");
    check(s_display_record == 46u && s_display_sid == 0x182u,
          "picture send shows the v6.00 Sending picture message record");

    s_send_result_ready = true;
    s_send_result.request_id = app.messages_picture_send_request_id;
    s_send_result.kind = MODEM_SMS_REQUEST_SEND_BINARY;
    s_send_result.outcome = MODEM_SMS_OUTCOME_OK;
    check(messages_picture_poll_send(&app, 9010u),
          "picture send completion reports a UI change");
    check(!app.messages_picture_send_waiting &&
              app.sms_recipient_prefill[0] == '\0',
          "successful picture send retires the wait and recipient prefill");
    check(s_display_record == 3u && s_display_sid == 0x183u,
          "successful picture send shows Picture message sent");

    app.route = APP_ROUTE_EDITOR;
    messages_cancel_picture_recipient(&app, 9020u);
    check(app.route == APP_ROUTE_MESSAGES_LIST && app.messages_mode == MESSAGES_MODE_READ,
          "cancelling picture recipient returns to the preview");

    app.messages_picture_send_waiting = true;
    app.messages_picture_send_started_ms = 10000u;
    app.messages_picture_send_request_id = UINT32_C(0x7777);
    app.route = APP_ROUTE_DISPLAY_MESSAGE;
    app.display_record_id = 46u;
    app.display_return_route = APP_ROUTE_MESSAGES_LIST;
    check(!messages_picture_poll_send(&app, 189999u),
          "picture send remains pending before its 180-second deadline");
    check(messages_picture_poll_send(&app, 190000u) &&
              !app.messages_picture_send_waiting && s_display_sid == 0x359u,
          "picture send times out exactly at 180 seconds with the failure note");
    uint16_t timeout_sid = s_display_sid;
    s_send_result_ready = true;
    s_send_result = (modem_sms_send_result_t){
        .request_id = UINT32_C(0x7777),
        .kind = MODEM_SMS_REQUEST_SEND_BINARY,
        .outcome = MODEM_SMS_OUTCOME_OK,
    };
    check(messages_picture_poll_send(&app, 190010u) &&
              app.messages_picture_send_request_id == 0u &&
              s_display_sid == timeout_sid,
          "late picture terminal drains without replacing the timeout UI");

    app.messages_picture_send_waiting = true;
    app.messages_picture_send_request_id = UINT32_C(0x7778);
    app.route = APP_ROUTE_INCOMING_CALL;
    copy_text(app.sms_send_submitted_recipient,
              sizeof(app.sms_send_submitted_recipient), "5550101");
    copy_text(app.sms_recipient_prefill, sizeof(app.sms_recipient_prefill),
              "5550102");
    s_send_result_ready = true;
    s_send_result = (modem_sms_send_result_t){
        .request_id = UINT32_C(0x7778),
        .kind = MODEM_SMS_REQUEST_SEND_BINARY,
        .outcome = MODEM_SMS_OUTCOME_OK,
    };
    check(messages_picture_poll_send(&app, 190020u) &&
              app.messages_picture_send_request_id == 0u &&
              app.route == APP_ROUTE_INCOMING_CALL,
          "late picture success drains without reclaiming a call route");
    check_str(app.sms_recipient_prefill, "5550102",
              "late picture success preserves a newer recipient edit");

    app.messages_picture_send_waiting = true;
    app.messages_picture_send_request_id = UINT32_C(0x8888);
    app.messages_picture_send_started_ms = 200000u;
    app.route = APP_ROUTE_DISPLAY_MESSAGE;
    app.display_record_id = 46u;
    app.display_return_route = APP_ROUTE_MESSAGES_LIST;
    s_send_result_ready = true;
    s_send_result = (modem_sms_send_result_t){
        .request_id = UINT32_C(0x8888),
        .kind = MODEM_SMS_REQUEST_SEND_BINARY,
        .outcome = MODEM_SMS_OUTCOME_UNCERTAIN,
    };
    check(messages_picture_poll_send(&app, 200010u) &&
              s_display_sid == 0x229u,
          "uncertain picture send uses the neutral Result unknown note");
}

static void test_incoming_picture_save_and_replace(void) {
    reset_fixture();
    app_t app;
    memset(&app, 0, sizeof(app));
    app.messages_kind = MESSAGES_KIND_INBOX;
    app.messages_mode = MESSAGES_MODE_OPTIONS;
    app.messages_option_selected = 0u;
    app.sms_inbox_count = 1u;
    app.sms_inbox[0].picture = true;
    app.sms_selected_content.valid = true;
    app.sms_selected_content.picture = true;
    app.sms_selected_content.picture_message.used = true;
    app.sms_selected_content.picture_message.width = STORE_PICTURE_WIDTH;
    app.sms_selected_content.picture_message.height = STORE_PICTURE_HEIGHT;
    copy_text(app.sms_selected_content.picture_message.text,
              sizeof(app.sms_selected_content.picture_message.text),
              "received");

    (void)handle_messages_list_key(&app, KEY_NAVI, 10000u);
    check(s_picture_writes == 1u && s_picture_last_written_slot == 0u,
          "Save stores an incoming picture in the first free slot");
    check(app.messages_kind == MESSAGES_KIND_PICTURES &&
              app.messages_mode == MESSAGES_MODE_READ,
          "saved incoming picture opens its preview");

    reset_fixture();
    memset(&app, 0, sizeof(app));
    for (uint8_t slot = 0u; slot < STORE_PICTURE_SLOT_COUNT; slot++) {
        seed_picture(slot, "occupied", STORE_PICTURE_WIDTH, STORE_PICTURE_HEIGHT);
    }
    app.messages_kind = MESSAGES_KIND_INBOX;
    app.messages_mode = MESSAGES_MODE_OPTIONS;
    app.sms_inbox_count = 1u;
    app.sms_inbox[0].picture = true;
    app.sms_selected_content.valid = true;
    app.sms_selected_content.picture = true;
    copy_text(app.sms_selected_content.picture_message.text,
              sizeof(app.sms_selected_content.picture_message.text),
              "replacement");
    (void)handle_messages_list_key(&app, KEY_NAVI, 10010u);
    check(app.messages_picture_save_pending &&
              app.messages_kind == MESSAGES_KIND_PICTURES &&
              app.messages_mode == MESSAGES_MODE_LIST,
          "a full picture store opens the replacement selector");
    (void)handle_messages_list_key(&app, KEY_C, 10015u);
    check(!app.messages_picture_save_pending &&
              app.messages_kind == MESSAGES_KIND_INBOX &&
              app.messages_mode == MESSAGES_MODE_READ &&
              app.messages_selected == 0u,
          "C cancels replacement and returns to the originating inbox picture");
    app.messages_mode = MESSAGES_MODE_OPTIONS;
    app.messages_option_selected = 0u;
    (void)handle_messages_list_key(&app, KEY_NAVI, 10016u);
    (void)handle_messages_list_key(&app, KEY_DOWN, 10020u);
    (void)handle_messages_list_key(&app, KEY_NAVI, 10030u);
    check(!app.messages_picture_save_pending && s_picture_last_written_slot == 1u,
          "replacement writes the selected physical slot");
    check_str(s_pictures[1].text, "replacement", "replacement stores the received picture");
    check(s_display_sid == 0x17du, "replacement shows Old picture replaced");
}

static void test_mailbox_sort_and_lazy_read(void) {
    reset_fixture();
    app_t app;
    memset(&app, 0, sizeof(app));
    app.sms_boot_status_sync_done = true;
    seed_mailbox_row(0u, 10u, 0x10u, "REC READ", "+10", "26/08/19,12:00:00");
    seed_mailbox_row(1u, 11u, 0x11u, "REC UNREAD", "+11", "26/08/18,09:00:00");
    seed_mailbox_row(2u, 12u, 0x12u, "REC UNREAD", "+12", "26/08/19,11:00:00");
    seed_mailbox_row(3u, 13u, 0x13u, "REC READ", "+13", "26/08/17,08:00:00");
    s_mailbox_count = 4u;

    open_messages_mailbox(&app, MESSAGES_KIND_INBOX, 11000u);
    check(s_mailbox_requests == 1u &&
              s_mailbox_requested_kind == MODEM_SMS_MAILBOX_INBOX,
          "opening Inbox requests one protected modem mailbox snapshot");
    check(s_read_requests == 0u,
          "mailbox list admission does not eagerly read message bodies");

    s_mailbox_result_ready = true;
    s_mailbox_result.request_id = app.messages_open_request_id;
    s_mailbox_result.kind = MODEM_SMS_REQUEST_MAILBOX;
    s_mailbox_result.outcome = MODEM_SMS_OUTCOME_OK;
    s_mailbox_result.complete = true;
    s_mailbox_result.mailbox = MODEM_SMS_MAILBOX_INBOX;
    check(poll_sms(&app, 11010u), "mailbox completion reports a UI change");
    check(app.sms_inbox_count == 4u && app.sms_unread_count == 2u,
          "complete mailbox load publishes all rows and authoritative unread count");
    check_str(app.sms_inbox[0].address, "+12",
              "newest unread row sorts first");
    check_str(app.sms_inbox[1].address, "+11",
              "older unread row stays ahead of all read rows");
    check_str(app.sms_inbox[2].address, "+10",
              "newest read row follows the unread group");
    check_str(app.sms_inbox[3].address, "+13",
              "oldest read row sorts last");
    check(s_read_requests == 0u,
          "loading and sorting the mailbox still performs no body read");

    (void)handle_messages_list_key(&app, KEY_NAVI, 11020u);
    check(s_read_requests == 1u && s_read_index_count == 1u &&
              s_read_indices[0] == 12u && s_read_identity_hash == 0x12u,
          "opening the selected row lazily reads its exact modem identity");
    check(app.sms_read_waiting && app.messages_mode == MESSAGES_MODE_LIST,
          "the list remains intact while the selected body is pending");

    s_read_result_ready = true;
    s_read_result.request_id = app.sms_read_request_id;
    s_read_result.kind = MODEM_SMS_REQUEST_READ;
    s_read_result.outcome = MODEM_SMS_OUTCOME_OK;
    s_read_result.request_identity_hash = 0x12u;
    s_read_result.identity_hash = 0x12u;
    char expanded_body[MODEM_SMS_DECODED_TEXT_MAX + 1u];
    for (size_t i = 0u; i < MODEM_SMS_TEXT_MAX; i++) {
        expanded_body[i * 2u] = (char)0xceu;
        expanded_body[i * 2u + 1u] = (char)0x94u;
    }
    expanded_body[MODEM_SMS_DECODED_TEXT_MAX] = '\0';
    copy_text(s_read_result.message.text,
              sizeof(s_read_result.message.text),
              expanded_body);
    check(poll_sms(&app, 11030u), "selected body completion reports a UI change");
    check(app.messages_mode == MESSAGES_MODE_READ &&
              app.sms_selected_content.valid,
          "matching lazy read opens the selected message");
    check(strlen(app.sms_selected_content.text) ==
              MODEM_SMS_DECODED_TEXT_MAX &&
              memcmp(app.sms_selected_content.text, expanded_body,
                     sizeof(expanded_body)) == 0,
          "lazy read carries the full worst-case UTF-8 body into the UI");
    check_str(app.sms_inbox[0].status, "REC READ",
              "opening an unread row updates its loaded status immediately");
    check(app.sms_unread_count == 1u,
          "opening an unread row decrements the authoritative envelope count");

    reset_fixture();
    memset(&app, 0, sizeof(app));
    app.sms_boot_status_sync_done = true;
    seed_mailbox_row(0u, 21u, 0x2100u, "REC UNREAD", "+21",
                     "26/08/20,10:00:00");
    s_mailbox[0].indices[1] = 23u;
    s_mailbox[0].index_count = 2u;
    s_mailbox[0].quarantined = true;
    s_mailbox_count = 1u;
    open_messages_mailbox(&app, MESSAGES_KIND_INBOX, 11100u);
    s_mailbox_result_ready = true;
    s_mailbox_result.request_id = app.messages_open_request_id;
    s_mailbox_result.kind = MODEM_SMS_REQUEST_MAILBOX;
    s_mailbox_result.outcome = MODEM_SMS_OUTCOME_OK;
    s_mailbox_result.complete = true;
    s_mailbox_result.mailbox = MODEM_SMS_MAILBOX_INBOX;
    check(poll_sms(&app, 11110u) && app.sms_inbox_count == 1u &&
              app.sms_inbox[0].quarantined,
          "mailbox copy preserves explicit quarantine ownership");
    (void)handle_messages_list_key(&app, KEY_NAVI, 11120u);
    check(s_read_requests == 1u && s_read_quarantined &&
              s_read_index_count == 2u && s_read_indices[0] == 21u &&
              s_read_indices[1] == 23u,
          "opening a quarantine reads every observed modem row in data mode");
    s_read_result_ready = true;
    s_read_result.request_id = app.sms_read_request_id;
    s_read_result.kind = MODEM_SMS_REQUEST_READ;
    s_read_result.outcome = MODEM_SMS_OUTCOME_OK;
    s_read_result.request_identity_hash = 0x2100u;
    s_read_result.identity_hash = 0x2100u;
    s_read_result.message.binary = true;
    check(poll_sms(&app, 11130u) &&
              app.messages_mode == MESSAGES_MODE_READ &&
              app.sms_selected_content.valid &&
              !app.sms_selected_content.picture,
          "quarantined body opens as an ordinary Nokia data message");
    check_str(app.sms_selected_content.text, "Data message",
              "quarantine never exposes partial picture bytes");
}

static void test_outbox_send_uses_visible_draft(void) {
    reset_fixture();
    app_t app;
    memset(&app, 0, sizeof(app));
    app.messages_kind = MESSAGES_KIND_OUTBOX;
    app.messages_mode = MESSAGES_MODE_OPTIONS;
    app.messages_option_selected = 0u; /* Send */
    app.sms_outbox_count = 1u;
    copy_text(app.sms_outbox[0].address,
              sizeof(app.sms_outbox[0].address), "5550100");
    app.sms_selected_content.valid = true;
    for (size_t i = 0u; i < MODEM_SMS_TEXT_MAX + 24u; i++) {
        app.sms_selected_content.text[i] = (char)('A' + (i % 26u));
    }
    app.sms_selected_content.text[MODEM_SMS_TEXT_MAX + 24u] = '\0';

    (void)handle_messages_list_key(&app, KEY_NAVI, 12000u);

    check(s_send_sms_requests == 1u,
          "Outbox Send admits exactly one text request");
    check_str(s_send_sms_number, "5550100",
              "Outbox Send preserves the stored recipient");
    check(strlen(app.sms_composer_text) == MODEM_SMS_TEXT_MAX &&
              strcmp(s_send_sms_text, app.sms_composer_text) == 0,
          "Outbox Send submits exactly the bounded draft shown to the user");
}

static void test_async_sms_request_ownership(void) {
    reset_fixture();
    app_t app;
    memset(&app, 0, sizeof(app));
    app.sms_boot_status_sync_done = true;
    app.route = APP_ROUTE_DISPLAY_MESSAGE;
    app.display_record_id = 46u;
    app.display_return_route = APP_ROUTE_SMS_COMPOSER;
    app.sms_send_return_route = APP_ROUTE_SMS_COMPOSER;
    app.sms_send_waiting = true;
    app.sms_send_request_id = 11u;
    app.sms_send_started_ms = 1000u;

    s_send_result_ready = true;
    s_send_result = (modem_sms_send_result_t){
        .request_id = 12u,
        .kind = MODEM_SMS_REQUEST_SEND_TEXT,
        .outcome = MODEM_SMS_OUTCOME_OK,
    };
    check(!poll_sms(&app, 1010u) && app.sms_send_waiting &&
              app.sms_send_request_id == 11u && s_send_result_ready,
          "a foreign send owner cannot consume or complete the app request");

    s_send_result.request_id = 11u;
    s_send_result.outcome = MODEM_SMS_OUTCOME_UNCERTAIN;
    check(poll_sms(&app, 1020u) && !app.sms_send_waiting &&
              app.sms_send_request_id == 0u && s_display_sid == 0x229u,
          "matching uncertain send reports Result unknown exactly once");

    reset_fixture();
    memset(&app, 0, sizeof(app));
    app.sms_boot_status_sync_done = true;
    app.route = APP_ROUTE_INCOMING_CALL;
    app.sms_send_waiting = true;
    app.sms_send_request_id = 21u;
    app.sms_send_return_route = APP_ROUTE_SMS_COMPOSER;
    copy_text(app.sms_composer_text, sizeof(app.sms_composer_text), "sent body");
    copy_text(app.sms_recipient_prefill, sizeof(app.sms_recipient_prefill),
              "5550100");
    copy_text(app.sms_send_submitted_text,
              sizeof(app.sms_send_submitted_text), "sent body");
    copy_text(app.sms_send_submitted_recipient,
              sizeof(app.sms_send_submitted_recipient), "5550100");
    check(poll_sms(&app, 2000u) && !app.sms_send_waiting &&
              app.sms_send_request_id == 21u &&
              app.route == APP_ROUTE_INCOMING_CALL,
          "higher-priority route detaches send UI but retains terminal ownership");
    s_send_result_ready = true;
    s_send_result = (modem_sms_send_result_t){
        .request_id = 21u,
        .kind = MODEM_SMS_REQUEST_SEND_TEXT,
        .outcome = MODEM_SMS_OUTCOME_OK,
    };
    check(poll_sms(&app, 2010u) && app.sms_send_request_id == 0u &&
              app.route == APP_ROUTE_INCOMING_CALL && s_display_calls == 0u &&
              app.sms_composer_text[0] == '\0' &&
              app.sms_recipient_prefill[0] == '\0',
          "late send success drains silently and clears its unchanged submitted draft");

    reset_fixture();
    memset(&app, 0, sizeof(app));
    app.sms_boot_status_sync_done = true;
    app.route = APP_ROUTE_INCOMING_CALL;
    app.sms_send_waiting = true;
    app.sms_send_request_id = 22u;
    app.sms_send_return_route = APP_ROUTE_SMS_COMPOSER;
    copy_text(app.sms_composer_text, sizeof(app.sms_composer_text),
              "newer edited body");
    copy_text(app.sms_recipient_prefill, sizeof(app.sms_recipient_prefill),
              "5550102");
    copy_text(app.sms_send_submitted_text,
              sizeof(app.sms_send_submitted_text), "old submitted body");
    copy_text(app.sms_send_submitted_recipient,
              sizeof(app.sms_send_submitted_recipient), "5550101");
    s_send_result_ready = true;
    s_send_result = (modem_sms_send_result_t){
        .request_id = 22u,
        .kind = MODEM_SMS_REQUEST_SEND_TEXT,
        .outcome = MODEM_SMS_OUTCOME_OK,
    };
    check(poll_sms(&app, 2020u) && app.sms_send_request_id == 0u &&
              app.route == APP_ROUTE_INCOMING_CALL && s_display_calls == 0u,
          "late send success remains detached after the draft was edited");
    check_str(app.sms_composer_text, "newer edited body",
              "an old successful send cannot erase a newer draft");
    check_str(app.sms_recipient_prefill, "5550102",
              "an old successful send cannot erase a newer recipient");

    reset_fixture();
    memset(&app, 0, sizeof(app));
    app.sms_boot_status_sync_done = true;
    app.route = APP_ROUTE_INCOMING_CALL;
    app.messages_open_pending = true;
    app.messages_open_request_id = 23u;
    app.sms_status_sync_silent = true;
    app.sms_open_deferred = true;
    app.sms_open_deferred_kind = MESSAGES_KIND_INBOX;
    check(poll_sms(&app, 2030u) && !app.sms_open_deferred &&
              app.messages_open_pending &&
              app.messages_open_request_id == 23u &&
              app.route == APP_ROUTE_INCOMING_CALL,
          "call preemption cancels only the user open layered over a silent scan");
    s_mailbox_result_ready = true;
    s_mailbox_result = (modem_sms_mailbox_result_t){
        .request_id = 23u,
        .kind = MODEM_SMS_REQUEST_MAILBOX,
        .outcome = MODEM_SMS_OUTCOME_OK,
        .complete = true,
        .mailbox = MODEM_SMS_MAILBOX_INBOX,
    };
    check(poll_sms(&app, 2040u) &&
              app.messages_open_request_id == 0u &&
              app.route == APP_ROUTE_INCOMING_CALL && s_display_calls == 0u,
          "the displaced deferred open drains its background terminal without reclaiming UI");

    reset_fixture();
    memset(&app, 0, sizeof(app));
    app.sms_boot_status_sync_done = true;
    app.route = APP_ROUTE_DISPLAY_MESSAGE;
    app.display_record_id = 4u;
    app.display_return_route = APP_ROUTE_SMS_COMPOSER;
    app.sms_save_waiting = true;
    app.sms_save_request_id = 31u;
    s_save_result_ready = true;
    s_save_result = (modem_sms_save_result_t){
        .request_id = 31u,
        .kind = MODEM_SMS_REQUEST_SAVE,
        .outcome = MODEM_SMS_OUTCOME_UNCERTAIN,
    };
    check(poll_sms(&app, 3000u) && s_display_sid == 0x229u &&
              app.sms_save_request_id == 0u,
          "uncertain save uses the neutral Result unknown note");

    reset_fixture();
    memset(&app, 0, sizeof(app));
    app.sms_boot_status_sync_done = true;
    app.route = APP_ROUTE_DISPLAY_MESSAGE;
    app.display_record_id = 4u;
    app.display_return_route = APP_ROUTE_MESSAGES_LIST;
    app.sms_delete_waiting = true;
    app.sms_delete_request_id = 41u;
    s_delete_result_ready = true;
    s_delete_result = (modem_sms_delete_result_t){
        .request_id = 41u,
        .kind = MODEM_SMS_REQUEST_DELETE,
        .outcome = MODEM_SMS_OUTCOME_UNCERTAIN,
    };
    check(poll_sms(&app, 4000u) && s_display_sid == 0x229u &&
              app.sms_delete_request_id == 0u,
          "uncertain delete uses the neutral Result unknown note");

    reset_fixture();
    memset(&app, 0, sizeof(app));
    app.sms_read_waiting = true;
    app.sms_read_request_id = 51u;
    app.messages_open_pending = true;
    app.messages_open_request_id = 52u;
    app.sms_status_sync_silent = false;
    messages_picture_open_list(&app);
    check(!app.sms_read_waiting && app.sms_read_request_id == 51u &&
              !app.messages_open_pending &&
              app.messages_open_request_id == 52u,
          "local picture navigation detaches modem UI without forgetting owners");
}

static void test_modem_sms_counter_epoch_rebase(void) {
    reset_fixture();
    app_t app;
    memset(&app, 0, sizeof(app));
    app.route = APP_ROUTE_STANDBY;
    app.sms_boot_status_sync_done = true;
    app.last_modem_sms_received_count = 7u;
    app.last_modem_user_sms_received_count = 4u;
    app.last_sms_storage_full_events = 3u;
    app.sms_unread_count = 2u;

    check(poll_sms(&app, 5000u),
          "a lower modem counter is accepted as a new modem epoch");
    check(app.last_modem_sms_received_count == 0u &&
              app.last_modem_user_sms_received_count == 0u &&
              app.last_sms_storage_full_events == 0u,
          "all modem-owned SMS counters rebase without unsigned deltas");
    check(!app.sms_received_pending && app.sms_received_pending_count == 0u &&
              s_display_calls == 0u,
          "counter reset does not synthesize a user message or memory-full alert");
    check(s_mailbox_requests == 1u && app.messages_open_pending &&
              app.sms_status_sync_silent &&
              app.sms_mailbox_received_count_at_start == 0u,
          "raw counter reset starts one authoritative silent mailbox scan");

    s_mailbox_result_ready = true;
    s_mailbox_result = (modem_sms_mailbox_result_t){
        .request_id = app.messages_open_request_id,
        .kind = MODEM_SMS_REQUEST_MAILBOX,
        .outcome = MODEM_SMS_OUTCOME_OK,
        .complete = true,
        .mailbox = MODEM_SMS_MAILBOX_INBOX,
    };
    check(poll_sms(&app, 5010u) && !app.messages_open_pending &&
              !app.sms_status_sync_pending && app.sms_boot_status_sync_done &&
              app.sms_unread_count == 0u,
          "a clean post-reset mailbox snapshot completes against the rebased epoch");
    check(!poll_sms(&app, 5020u) && s_mailbox_requests == 1u,
          "completed post-reset sync does not enter a mailbox retry loop");

    s_modem_sms_storage_full_events = 1u;
    check(poll_sms(&app, 5030u) && app.last_sms_storage_full_events == 1u &&
              s_display_sid == 0x1c5u,
          "the first real storage-full edge after reset remains observable");
}

static void test_message_alert_follows_v600_call_state(void) {
    app_t app;

    reset_fixture();
    memset(&app, 0, sizeof(app));
    app.route = APP_ROUTE_STANDBY;
    app.sms_boot_status_sync_done = true;
    s_profile_message_alert = 4u;
    s_profile_ringing_volume = 3u;
    s_modem_sms_user_received_count = 1u;
    check(poll_sms(&app, 6000u) && app.backlight_activity_pending,
          "idle user SMS requests the v6.00 light event");
    check(s_post_count == 1u &&
              s_posts[0].cmd == CORE1_CMD_AUDIO_SYSTEM_TONE &&
              s_posts[0].arg == audio_arg(32u, 3u),
          "idle user SMS uses the selected profile alert");

    reset_fixture();
    memset(&app, 0, sizeof(app));
    app.route = APP_ROUTE_CALL;
    app.sms_boot_status_sync_done = true;
    s_modem_call_state = MODEM_CALL_ACTIVE;
    s_profile_message_alert = 0u;
    s_profile_ringing_volume = 0u;
    s_modem_sms_user_received_count = 1u;
    check(poll_sms(&app, 6100u) && app.backlight_activity_pending,
          "in-call user SMS still requests the v6.00 light event");
    check(s_post_count == 1u &&
              s_posts[0].cmd == CORE1_CMD_AUDIO_SYSTEM_TONE_QUIET &&
              s_posts[0].arg == audio_arg(14u, 1u),
          "in-call user SMS substitutes fixed earpiece tone 14");

    reset_fixture();
    memset(&app, 0, sizeof(app));
    app.route = APP_ROUTE_INCOMING_CALL;
    app.sms_boot_status_sync_done = true;
    s_modem_call_state = MODEM_CALL_RINGING;
    s_profile_message_alert = 4u;
    s_profile_ringing_volume = 3u;
    s_modem_sms_user_received_count = 1u;
    check(poll_sms(&app, 6200u) && app.sms_received_pending &&
              app.sms_received_pending_count == 1u,
          "SMS arriving during an incoming ring remains queued for the user");
    check(!app.backlight_activity_pending && s_post_count == 0u,
          "incoming ringing suppresses the competing SMS tone and light event");
}

int main(void) {
    test_init_loads_composer_state();
    test_open_multitap_delete_and_cursor();
    test_composer_long_press_enters_digit();
    test_limits_modes_and_symbols();
    test_t9_candidates_spell_and_user_word();
    test_options_and_return_routes();
    test_tick_and_render_contract();
    test_composer_full_text_and_utf8_editing();
    test_picture_sparse_list_preview_and_options();
    test_picture_edit_save_preview_and_limit();
    test_picture_send_contract();
    test_incoming_picture_save_and_replace();
    test_mailbox_sort_and_lazy_read();
    test_outbox_send_uses_visible_draft();
    test_async_sms_request_ownership();
    test_modem_sms_counter_epoch_rebase();
    test_message_alert_follows_v600_call_state();

    if (s_failures != 0) {
        fprintf(stderr, "%d messages app test(s) failed\n", s_failures);
        return 1;
    }
    printf("messages app tests passed\n");
    return 0;
}

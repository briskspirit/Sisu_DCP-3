#ifndef APPS_TONES_APP_H
#define APPS_TONES_APP_H

#include <stdbool.h>
#include <stdint.h>

#include "app_internal.h"

typedef enum {
    TONES_SETTING_INCOMING_ALERT = 0,
    TONES_SETTING_RINGING_TONE,
    TONES_SETTING_RINGING_VOLUME,
    TONES_SETTING_MESSAGE_ALERT,
    TONES_SETTING_KEYPAD_TONES,
    TONES_SETTING_WARNING_GAME_TONES,
    TONES_SETTING_VIBRATING_ALERT,
} tones_setting_kind_t;

void open_tones_menu(app_t *app, uint8_t selected);
void open_tones_personalise(app_t *app, uint8_t profile_index, uint8_t selected);

bool handle_tones_menu_key(app_t *app, uint16_t key, uint32_t now);
bool handle_tones_setting_key(app_t *app, uint16_t key, uint32_t now);
void tones_confirm_ringing_volume(app_t *app, bool accepted, uint32_t now);
bool tick_tones_setting(app_t *app, uint32_t now);
bool handle_tone_composer_key(app_t *app, uint16_t key, event_type_t event_type, uint32_t now);
bool handle_tone_composer_options_key(app_t *app, uint16_t key, uint32_t now);
bool handle_tone_composer_tempo_key(app_t *app, uint16_t key, uint32_t now);
bool tick_tone_composer(app_t *app, uint32_t now);

void render_tones_menu(const app_t *app, framebuffer_t *fb);
void render_tones_setting(const app_t *app, framebuffer_t *fb);
void render_tone_composer(const app_t *app, framebuffer_t *fb);
void render_tone_composer_options(const app_t *app, framebuffer_t *fb);
void render_tone_composer_tempo(const app_t *app, framebuffer_t *fb);

/* Ringing-tone catalogue (the traced ringing-tone builder list 0x00276d9c),
 * shared with the phonebook assign-tone picker. */
uint8_t ringing_tone_catalogue_count(void);
const char *ringing_tone_catalogue_label(uint8_t index);
uint8_t ringing_tone_catalogue_value(uint8_t index);
const char *ringing_tone_value_label(uint8_t value); /* 0 if value unknown */
void preview_ringing_tone_value_active_profile(uint8_t value);
void stop_ringing_tone_preview(void);

void open_tone_composer(app_t *app, uint32_t now);
void tone_composer_submit_name(app_t *app, uint32_t now);
void tone_composer_cancel_name(app_t *app, uint32_t now);
void open_tone_composer_recipient_editor(app_t *app, const char *value, uint32_t now);
void update_tone_composer_recipient_softkey(app_t *app);
void tone_composer_submit_recipient(app_t *app, uint32_t now);
void tone_composer_cancel_recipient(app_t *app, uint32_t now);

void open_received_tone(app_t *app);
bool handle_received_tone_key(app_t *app, uint16_t key, uint32_t now);
bool handle_received_tone_display_key(app_t *app, uint16_t key);
bool tick_received_tone(app_t *app, uint32_t now);
void render_received_tone(const app_t *app, framebuffer_t *fb);

#endif

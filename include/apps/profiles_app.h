#ifndef APPS_PROFILES_APP_H
#define APPS_PROFILES_APP_H

#include <stdbool.h>
#include <stdint.h>

#include "app_internal.h"
#include "storage/store_service.h"

typedef enum {
    PROFILE_SETTING_INCOMING_ALERT = 0,
    PROFILE_SETTING_RINGING_TONE,
    PROFILE_SETTING_RINGING_VOLUME,
    PROFILE_SETTING_MESSAGE_ALERT,
    PROFILE_SETTING_KEYPAD_TONES,
    PROFILE_SETTING_WARNING_GAME_TONES,
    PROFILE_SETTING_VIBRATING_ALERT,
    PROFILE_SETTING_COUNT
} profile_setting_kind_t;

void profiles_app_init(app_t *app);
void open_profiles_menu(app_t *app, uint8_t selected);
void open_profiles_options(app_t *app, uint8_t selected_profile);

bool handle_profiles_key(app_t *app, uint16_t key, uint32_t now);
bool tick_profiles(app_t *app, uint32_t now);
void render_profiles(const app_t *app, framebuffer_t *fb);

uint8_t profile_count(void);
uint8_t profile_active_index(void);
const char *profile_label(uint8_t profile_index);
const char *profile_active_standby_label(void);
bool profile_active_is_silent(void);
void profile_activate(uint8_t profile_index);
/* Accessory (Headset) profile: auto-activated on headset insert (saves the user
 * profile), restored on removal. Not user-selectable. */
void profile_activate_headset(void);
void profile_restore_from_headset(void);
/* Restore all editable profile tone fields to their v6.00 defaults while
 * preserving the currently active profile and any Headset return target. */
void profiles_restore_factory_defaults(void);

uint8_t profile_default_setting_value(uint8_t profile_index, profile_setting_kind_t kind);
uint8_t profile_get_tone_setting(uint8_t profile_index, profile_setting_kind_t kind);
void profile_set_tone_setting(uint8_t profile_index, profile_setting_kind_t kind, uint8_t value);

#endif

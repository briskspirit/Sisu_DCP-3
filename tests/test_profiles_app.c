/* The auto-only Headset profile may be active while the ordinary four-row
 * Profiles list is opened. The menu must select the saved user profile without
 * changing the live accessory profile, and every action must target that
 * displayed row. */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "apps/dialogs_app.h"
#include "apps/main_menu_app.h"
#include "apps/profiles_app.h"
#include "apps/tones_app.h"
#include "services/input_keys.h"
#include "services/strings.h"
#include "services/timebase.h"
#include "storage/store_service.h"

static int s_failures;
static uint8_t s_settings[STORE_SETTING_COUNT];
static unsigned s_personalise_opens;
static uint8_t s_personalise_profile;

static void check(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        s_failures++;
    }
}

store_status_t store_setting_get_u8(store_setting_key_t key,
                                    uint8_t *out_value) {
    if ((unsigned)key >= STORE_SETTING_COUNT || out_value == NULL) {
        return STORE_STATUS_INVALID_ARGUMENT;
    }
    *out_value = s_settings[key];
    return STORE_STATUS_OK;
}

store_status_t store_setting_set_u8(store_setting_key_t key, uint8_t value) {
    if ((unsigned)key >= STORE_SETTING_COUNT) {
        return STORE_STATUS_INVALID_ARGUMENT;
    }
    s_settings[key] = value;
    return STORE_STATUS_OK;
}

const char *ts(uint16_t id) {
    (void)id;
    return NULL;
}

int32_t time_diff_ms(uint32_t a, uint32_t b) {
    return (int32_t)(a - b);
}

void open_main_menu_at(app_t *app, uint8_t selected, uint32_t now_ms) {
    (void)app;
    (void)selected;
    (void)now_ms;
}

void open_tones_personalise(app_t *app,
                            uint8_t profile_index,
                            uint8_t selected) {
    (void)app;
    (void)selected;
    s_personalise_opens++;
    s_personalise_profile = profile_index;
}

void open_display_sid(app_t *app,
                      uint8_t record_id,
                      uint16_t sid,
                      const char *fallback,
                      app_route_t return_route,
                      uint32_t now) {
    (void)app;
    (void)record_id;
    (void)sid;
    (void)fallback;
    (void)return_route;
    (void)now;
}

void open_display(app_t *app,
                  uint8_t record_id,
                  const char *a,
                  const char *b,
                  const char *c,
                  app_route_t return_route,
                  uint32_t now) {
    (void)app;
    (void)record_id;
    (void)a;
    (void)b;
    (void)c;
    (void)return_route;
    (void)now;
}

static void reset_fixture(void) {
    memset(s_settings, 0, sizeof(s_settings));
    s_personalise_opens = 0u;
    s_personalise_profile = 0xffu;
}

static void test_headset_opens_on_saved_visible_profile(void) {
    reset_fixture();
    s_settings[STORE_SETTING_PROFILE_ACTIVE] = 4u; /* auto-only Headset */
    s_settings[STORE_SETTING_PROFILE_SAVED] = 2u;  /* Discreet */

    app_t app;
    memset(&app, 0, sizeof(app));
    open_profiles_menu(&app, profile_active_index());

    check(app.route == APP_ROUTE_PROFILES_MENU,
          "Headset opens the ordinary Profiles route");
    check(app.profiles_selected_index == 2u,
          "Headset menu selection maps to the saved visible profile");
    check(profile_active_index() == 4u,
          "opening Profiles does not deactivate the accessory profile");

    (void)handle_profiles_key(&app, KEY_NAVI, 100u);
    app.profiles_option_index = 1u;
    (void)handle_profiles_key(&app, KEY_NAVI, 101u);
    check(s_personalise_opens == 1u && s_personalise_profile == 2u,
          "Personalise targets exactly the row displayed to the user");
}

static void test_invalid_hidden_selection_falls_back_to_personal(void) {
    reset_fixture();
    s_settings[STORE_SETTING_PROFILE_ACTIVE] = 4u;
    s_settings[STORE_SETTING_PROFILE_SAVED] = 0xffu;

    app_t app;
    memset(&app, 0, sizeof(app));
    open_profiles_menu(&app, profile_active_index());
    check(app.profiles_selected_index == 0u,
          "missing saved user profile falls back to visible Personal");

    open_profiles_menu(&app, 3u);
    check(app.profiles_selected_index == 3u,
          "ordinary visible profile selection is preserved");
}

static void test_factory_restore_resets_all_tones_and_preserves_selection(void) {
    static const store_setting_key_t profile_keys[5][PROFILE_SETTING_COUNT] = {
        {
            STORE_SETTING_PROFILE_PERSONAL_INCOMING_ALERT,
            STORE_SETTING_PROFILE_PERSONAL_RINGING_TONE,
            STORE_SETTING_PROFILE_PERSONAL_RINGING_VOLUME,
            STORE_SETTING_PROFILE_PERSONAL_MESSAGE_ALERT,
            STORE_SETTING_PROFILE_PERSONAL_KEYPAD_TONES,
            STORE_SETTING_PROFILE_PERSONAL_WARNING_GAME_TONES,
            STORE_SETTING_PROFILE_PERSONAL_VIBRATING_ALERT,
        },
        {
            STORE_SETTING_PROFILE_SILENT_INCOMING_ALERT,
            STORE_SETTING_PROFILE_SILENT_RINGING_TONE,
            STORE_SETTING_PROFILE_SILENT_RINGING_VOLUME,
            STORE_SETTING_PROFILE_SILENT_MESSAGE_ALERT,
            STORE_SETTING_PROFILE_SILENT_KEYPAD_TONES,
            STORE_SETTING_PROFILE_SILENT_WARNING_GAME_TONES,
            STORE_SETTING_PROFILE_SILENT_VIBRATING_ALERT,
        },
        {
            STORE_SETTING_PROFILE_DISCREET_INCOMING_ALERT,
            STORE_SETTING_PROFILE_DISCREET_RINGING_TONE,
            STORE_SETTING_PROFILE_DISCREET_RINGING_VOLUME,
            STORE_SETTING_PROFILE_DISCREET_MESSAGE_ALERT,
            STORE_SETTING_PROFILE_DISCREET_KEYPAD_TONES,
            STORE_SETTING_PROFILE_DISCREET_WARNING_GAME_TONES,
            STORE_SETTING_PROFILE_DISCREET_VIBRATING_ALERT,
        },
        {
            STORE_SETTING_PROFILE_LOUD_INCOMING_ALERT,
            STORE_SETTING_PROFILE_LOUD_RINGING_TONE,
            STORE_SETTING_PROFILE_LOUD_RINGING_VOLUME,
            STORE_SETTING_PROFILE_LOUD_MESSAGE_ALERT,
            STORE_SETTING_PROFILE_LOUD_KEYPAD_TONES,
            STORE_SETTING_PROFILE_LOUD_WARNING_GAME_TONES,
            STORE_SETTING_PROFILE_LOUD_VIBRATING_ALERT,
        },
        {
            STORE_SETTING_PROFILE_HEADSET_INCOMING_ALERT,
            STORE_SETTING_PROFILE_HEADSET_RINGING_TONE,
            STORE_SETTING_PROFILE_HEADSET_RINGING_VOLUME,
            STORE_SETTING_PROFILE_HEADSET_MESSAGE_ALERT,
            STORE_SETTING_PROFILE_HEADSET_KEYPAD_TONES,
            STORE_SETTING_PROFILE_HEADSET_WARNING_GAME_TONES,
            STORE_SETTING_PROFILE_HEADSET_VIBRATING_ALERT,
        },
    };
    static const uint8_t defaults[5][PROFILE_SETTING_COUNT] = {
        {1u, 52u, 9u, 0u, 1u, 4u, 0u},
        {4u, 52u, 8u, 0u, 255u, 255u, 0u},
        {1u, 52u, 8u, 0u, 1u, 4u, 0u},
        {1u, 52u, 9u, 0u, 1u, 4u, 0u},
        {1u, 52u, 8u, 0u, 1u, 4u, 0u},
    };
    static const store_setting_key_t active_keys[PROFILE_SETTING_COUNT] = {
        STORE_SETTING_PROFILE_INCOMING_ALERT,
        STORE_SETTING_PROFILE_RINGING_TONE,
        STORE_SETTING_PROFILE_RINGING_VOLUME,
        STORE_SETTING_PROFILE_MESSAGE_ALERT,
        STORE_SETTING_PROFILE_KEYPAD_TONES,
        STORE_SETTING_PROFILE_WARNING_GAME_TONES,
        STORE_SETTING_PROFILE_VIBRATING_ALERT,
    };

    reset_fixture();
    memset(s_settings, 0xa5, sizeof(s_settings));
    s_settings[STORE_SETTING_PROFILE_ACTIVE] = 4u;
    s_settings[STORE_SETTING_PROFILE_SAVED] = 2u;

    profiles_restore_factory_defaults();

    check(s_settings[STORE_SETTING_PROFILE_ACTIVE] == 4u,
          "factory restore preserves active Headset profile");
    check(s_settings[STORE_SETTING_PROFILE_SAVED] == 2u,
          "factory restore preserves Headset return profile");
    for (uint8_t profile = 0u; profile < 5u; profile++) {
        for (uint8_t kind = 0u; kind < PROFILE_SETTING_COUNT; kind++) {
            check(s_settings[profile_keys[profile][kind]] == defaults[profile][kind],
                  "factory restore resets every per-profile tone field");
        }
    }
    for (uint8_t kind = 0u; kind < PROFILE_SETTING_COUNT; kind++) {
        check(s_settings[active_keys[kind]] == defaults[4][kind],
              "factory restore resynchronizes active-profile tone mirror");
    }
}

int main(void) {
    test_headset_opens_on_saved_visible_profile();
    test_invalid_hidden_selection_falls_back_to_personal();
    test_factory_restore_resets_all_tones_and_preserves_selection();
    if (s_failures != 0) {
        fprintf(stderr, "%d profile test(s) failed\n", s_failures);
        return 1;
    }
    puts("profiles app tests passed");
    return 0;
}

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "apps/settings_app.h"
#include "storage/store_service.h"

static int s_failures;
static uint8_t s_values[STORE_SETTING_COUNT];
static bool s_touched[STORE_SETTING_COUNT];
static unsigned s_u8_writes;
static unsigned s_profile_resets;
static unsigned s_language_applies;
static uint8_t s_applied_language;
static bool s_language_was_stored_before_apply;
static unsigned s_text_writes;
static store_setting_key_t s_text_key;
static char s_text_value[STORE_TEXT_MAX + 1u];

static void check(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        s_failures++;
    }
}

store_status_t store_setting_set_u8(store_setting_key_t key, uint8_t value) {
    if ((unsigned)key >= STORE_SETTING_COUNT) {
        return STORE_STATUS_INVALID_ARGUMENT;
    }
    s_values[key] = value;
    s_touched[key] = true;
    s_u8_writes++;
    return STORE_STATUS_OK;
}

store_status_t store_setting_set_text(store_setting_key_t key, const char *text) {
    s_text_writes++;
    s_text_key = key;
    snprintf(s_text_value, sizeof(s_text_value), "%s", text != NULL ? text : "");
    return STORE_STATUS_OK;
}

void profiles_restore_factory_defaults(void) {
    s_profile_resets++;
}

void strings_set_language(uint8_t lang_id) {
    s_language_applies++;
    s_applied_language = lang_id;
    s_language_was_stored_before_apply =
        s_touched[STORE_SETTING_SYSTEM_LANGUAGE] &&
        s_values[STORE_SETTING_SYSTEM_LANGUAGE] == lang_id;
}

static void test_factory_restore_contract(void) {
    typedef struct {
        store_setting_key_t key;
        uint8_t value;
    } expected_setting_t;
    static const expected_setting_t expected[] = {
        {STORE_SETTING_SETTINGS_AUTOMATIC_REDIAL, 0u},
        {STORE_SETTING_SETTINGS_SPEED_DIALLING, 0u},
        {STORE_SETTING_SETTINGS_PHONE_LINE, 0u},
        {STORE_SETTING_CALL_OWN_NUMBER_SENDING, 0u},
        {STORE_SETTING_SETTINGS_AUTOMATIC_ANSWER, 0u},
        {STORE_SETTING_SETTINGS_CELL_INFO_DISPLAY, 0u},
        {STORE_SETTING_SETTINGS_NETWORK_SELECTION, 0u},
        {STORE_SETTING_SETTINGS_LIGHTS, 0u},
        {STORE_SETTING_SETTINGS_CONFIRM_SIM_ACTIONS, 0u},
        {STORE_SETTING_SETTINGS_PIN_CODE_REQUEST, 0u},
        {STORE_SETTING_SETTINGS_FIXED_DIALLING, 0u},
        {STORE_SETTING_SETTINGS_CLOSED_USER_GROUP, 0u},
        {STORE_SETTING_SETTINGS_PHONE_SECURITY, 17u},
        {STORE_SETTING_SETTINGS_PHONE_LINE_CHANGE_ALLOWED, 1u},
        {STORE_SETTING_SYSTEM_LANGUAGE, 0u},
    };

    memset(s_values, 0xa5, sizeof(s_values));
    memset(s_touched, 0, sizeof(s_touched));
    s_u8_writes = 0u;
    s_profile_resets = 0u;
    s_language_applies = 0u;
    s_language_was_stored_before_apply = false;
    s_text_writes = 0u;
    s_text_key = STORE_SETTING_COUNT;
    s_text_value[0] = '\0';

    settings_restore_factory_defaults();

    check(s_u8_writes == sizeof(expected) / sizeof(expected[0]),
          "factory restore writes exactly the modeled scalar settings");
    for (unsigned i = 0u; i < sizeof(expected) / sizeof(expected[0]); i++) {
        check(s_touched[expected[i].key] &&
                  s_values[expected[i].key] == expected[i].value,
              "factory restore writes each setting's traced default");
    }
    check(!s_touched[STORE_SETTING_SYSTEM_BACKLIGHT_LEVEL],
          "factory restore preserves board calibration settings");
    check(!s_touched[STORE_SETTING_CALL_DURATION_LIFETIME],
          "factory restore preserves lifetime accounting");
    check(s_text_writes == 1u &&
              s_text_key == STORE_SETTING_SYSTEM_WELCOME_NOTE &&
              s_text_value[0] == '\0',
          "factory restore erases only the welcome note text");
    check(s_profile_resets == 1u,
          "factory restore resets profile customizations exactly once");
    check(s_language_applies == 1u && s_applied_language == 0u,
          "factory restore applies Automatic language immediately");
    check(s_language_was_stored_before_apply,
          "factory restore persists language before changing the live UI");
}

int main(void) {
    test_factory_restore_contract();
    if (s_failures != 0) {
        fprintf(stderr, "%d settings restore test(s) failed\n", s_failures);
        return 1;
    }
    puts("settings factory-restore tests passed");
    return 0;
}

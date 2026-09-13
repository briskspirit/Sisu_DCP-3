#include "app_router.h"

#include "apps/calculator_app.h"
#include "apps/call_register_app.h"
#include "apps/call_divert_app.h"
#include "apps/calls_app.h"
#include "apps/clock_app.h"
#include "apps/dialogs_app.h"
#include "apps/games_app.h"
#include "apps/logic_app.h"
#include "apps/main_menu_app.h"
#include "apps/memory_app.h"
#include "apps/messages_app.h"
#include "apps/net_monitor_app.h"
#include "apps/pacman_app.h"
#include "apps/phonebook_app.h"
#include "apps/power_app.h"
#include "apps/powerup_app.h"
#include "apps/profiles_app.h"
#include "apps/react_app.h"
#include "apps/rotation_app.h"
#include "apps/settings_app.h"
#include "apps/service_codes_app.h"
#include "apps/snake_app.h"
#include "apps/standby_app.h"
#include "apps/tones_app.h"
#include "services/input_keys.h"

bool app_router_handle_event(app_t *app, const input_event_t *event) {
    if (event->type != EVENT_KEY_DOWN && event->type != EVENT_KEY_HOLD) {
        return false;
    }

    uint16_t key = event->code;
    uint32_t now = event->when_ms;
    if (key == KEY_POWER) {
        return handle_power_key(app, event);
    }

    switch (app->route) {
    case APP_ROUTE_POWERUP:
        return true;
    case APP_ROUTE_POWER_MENU:
        return handle_power_menu_key(app, key, now);
    case APP_ROUTE_POWER_OFF:
        return true;
    case APP_ROUTE_DISPLAY_MESSAGE:
        return handle_display_message_key(app, key, now);
    case APP_ROUTE_CONFIRM:
        return handle_confirm_key(app, key, now);
    case APP_ROUTE_EDITOR:
        return handle_editor_key(app, key, event->type, now);
    case APP_ROUTE_PHONEBOOK_MENU:
        return handle_phonebook_menu_key(app, key, now);
    case APP_ROUTE_PHONEBOOK_LIST:
        return handle_phonebook_list_key(app, key, now);
    case APP_ROUTE_PHONEBOOK_MEMORY:
        if (key == KEY_C || key == KEY_NAVI) {
            open_phonebook_menu(app, PHONEBOOK_MENU_OPTIONS, 1u);
            return true;
        }
        return true;
    case APP_ROUTE_PHONEBOOK_TONE_PICKER:
        return handle_phonebook_tone_key(app, key, now);
    case APP_ROUTE_PHONEBOOK_SPEED_DIALS:
        return handle_phonebook_speed_key(app, key, now);
    case APP_ROUTE_PHONEBOOK_SPEED_DIAL_OPTIONS:
        return handle_phonebook_speed_options_key(app, key, now);
    case APP_ROUTE_PHONEBOOK_EDIT_CHOICE:
        return handle_phonebook_edit_choice_key(app, key, now);
    case APP_ROUTE_PHONEBOOK_SECURITY:
        return handle_phonebook_security_key(app, key, now);
    case APP_ROUTE_MESSAGES_MENU:
        return handle_messages_menu_key(app, key, now);
    case APP_ROUTE_MESSAGES_LIST:
        return handle_messages_list_key(app, key, now);
    case APP_ROUTE_SMS_COMPOSER:
        return handle_sms_composer_key(app, key, event->type, now);
    case APP_ROUTE_SMS_OPTIONS:
        return handle_sms_options_key(app, key, now);
    case APP_ROUTE_SMS_SYMBOLS:
        return handle_sms_symbols_key(app, key, now);
    case APP_ROUTE_CALL_REGISTER_MENU:
        return handle_call_register_menu_key(app, key, now);
    case APP_ROUTE_CALL_REGISTER_LIST:
        return handle_call_register_list_key(app, key, now);
    case APP_ROUTE_CALL_REGISTER_OPTIONS:
        return handle_call_register_options_key(app, key, now);
    case APP_ROUTE_CALL_REGISTER_DETAIL:
        return handle_call_register_detail_key(app, key, now);
    case APP_ROUTE_CALL_REGISTER_METRIC:
        return handle_call_register_metric_key(app, key, now);
    case APP_ROUTE_CALL_DIVERT_MENU:
        return handle_call_divert_menu_key(app, key, now);
    case APP_ROUTE_SERVICE_CODES:
        return handle_service_codes_key(app, key, now);
    case APP_ROUTE_NET_MONITOR_TEST:
        return handle_net_monitor_test_key(app, key, now);
    case APP_ROUTE_NET_MONITOR_PAGE:
        return handle_net_monitor_page_key(app, key, now);
    case APP_ROUTE_PROFILES_MENU:
        return handle_profiles_key(app, key, now);
    case APP_ROUTE_GAMES_MENU:
        return handle_games_menu_key(app, key, now);
    case APP_ROUTE_ROTATION_MENU:
        return handle_rotation_menu_key(app, key, now);
    case APP_ROUTE_ROTATION_PLAY:
    case APP_ROUTE_ROTATION_LAST_VIEW:
        return handle_rotation_play_key(app, key, now);
    case APP_ROUTE_ROTATION_LEVEL:
        return handle_rotation_level_key(app, key, now);
    case APP_ROUTE_ROTATION_INSTRUCTIONS:
        return handle_rotation_instructions_key(app, key, now);
    case APP_ROUTE_ROTATION_TOP_SCORE:
    case APP_ROUTE_ROTATION_RESULT:
        return handle_rotation_message_key(app, key, now);
    case APP_ROUTE_MEMORY_MENU:
        return handle_memory_menu_key(app, key, now);
    case APP_ROUTE_MEMORY_PLAY:
    case APP_ROUTE_MEMORY_LAST_VIEW:
        return handle_memory_play_key(app, key, now);
    case APP_ROUTE_MEMORY_LEVEL:
        return handle_memory_level_key(app, key, now);
    case APP_ROUTE_MEMORY_INSTRUCTIONS:
        return handle_memory_instructions_key(app, key, now);
    case APP_ROUTE_MEMORY_TOP_SCORE:
    case APP_ROUTE_MEMORY_RESULT:
        return handle_memory_message_key(app, key, now);
    case APP_ROUTE_REACT_MENU:
        return handle_react_menu_key(app, key, now);
    case APP_ROUTE_REACT_PLAY:
    case APP_ROUTE_REACT_LAST_VIEW:
        return handle_react_play_key(app, key, now);
    case APP_ROUTE_REACT_LEVEL:
        return handle_react_level_key(app, key, now);
    case APP_ROUTE_REACT_INSTRUCTIONS:
        return handle_react_instructions_key(app, key, now);
    case APP_ROUTE_REACT_TOP_SCORE:
    case APP_ROUTE_REACT_RESULT:
        return handle_react_message_key(app, key, now);
    case APP_ROUTE_LOGIC_MENU:
        return handle_logic_menu_key(app, key, now);
    case APP_ROUTE_LOGIC_PLAY:
    case APP_ROUTE_LOGIC_LAST_VIEW:
        return handle_logic_play_key(app, key, now);
    case APP_ROUTE_LOGIC_LEVEL:
        return handle_logic_level_key(app, key, now);
    case APP_ROUTE_LOGIC_INSTRUCTIONS:
        return handle_logic_instructions_key(app, key, now);
    case APP_ROUTE_LOGIC_RESULT:
        return handle_logic_message_key(app, key, now);
    case APP_ROUTE_PACMAN_MENU:
        return handle_pacman_menu_key(app, key, now);
    case APP_ROUTE_PACMAN_PLAY:
    case APP_ROUTE_PACMAN_LAST_VIEW:
        return handle_pacman_play_key(app, key, now);
    case APP_ROUTE_PACMAN_LEVEL:
        return handle_pacman_level_key(app, key, now);
    case APP_ROUTE_PACMAN_INSTRUCTIONS:
        return handle_pacman_instructions_key(app, key, now);
    case APP_ROUTE_PACMAN_TOP_SCORE:
    case APP_ROUTE_PACMAN_RESULT:
        return handle_pacman_message_key(app, key, now);
    case APP_ROUTE_SNAKE_MENU:
        return handle_snake_menu_key(app, key, now);
    case APP_ROUTE_SNAKE_PLAY:
    case APP_ROUTE_SNAKE_LAST_VIEW:
        return handle_snake_play_key(app, key, now);
    case APP_ROUTE_SNAKE_LEVEL:
        return handle_snake_level_key(app, key, now);
    case APP_ROUTE_SNAKE_INSTRUCTIONS:
        return handle_snake_instructions_key(app, key, now);
    case APP_ROUTE_SNAKE_TOP_SCORE:
    case APP_ROUTE_SNAKE_RESULT:
        return handle_snake_message_key(app, key, now);
    case APP_ROUTE_CALCULATOR:
        return handle_calculator_key(app, key, event->type, now);
    case APP_ROUTE_CALL:
        return handle_call_key(app, key, now);
    case APP_ROUTE_CALL_OPTIONS:
        return handle_call_options_key(app, key, now);
    case APP_ROUTE_INCOMING_CALL:
        return handle_incoming_call_key(app, key, now);
    case APP_ROUTE_CLOCK_MENU:
        return handle_clock_menu_key(app, key, now);
    case APP_ROUTE_CLOCK_EDITOR:
        return handle_clock_editor_key(app, key, now);
    case APP_ROUTE_CLOCK_ALARM:
        return handle_clock_alarm_key(app, key, now);
    case APP_ROUTE_TONES_MENU:
        return handle_tones_menu_key(app, key, now);
    case APP_ROUTE_TONES_SETTING:
        return handle_tones_setting_key(app, key, now);
    case APP_ROUTE_TONE_COMPOSER:
        return handle_tone_composer_key(app, key, event->type, now);
    case APP_ROUTE_TONE_COMPOSER_OPTIONS:
        return handle_tone_composer_options_key(app, key, now);
    case APP_ROUTE_TONE_COMPOSER_TEMPO:
        return handle_tone_composer_tempo_key(app, key, now);
    case APP_ROUTE_SETTINGS_MENU:
        return handle_settings_menu_key(app, key, now);
    case APP_ROUTE_SETTINGS_VALUE:
        return handle_settings_value_key(app, key, now);
    case APP_ROUTE_SETTINGS_WELCOME_OPTIONS:
        return handle_settings_welcome_options_key(app, key, now);
    default:
        break;
    }

    /* Snooze-active Stop outranks the keyguard: an auto-snooze drops to a LOCKED
     * standby, and the user must still be able to stop it. Let the Stop keys
     * (Navi/C) through to the snooze dismiss before keyguard swallows them; the
     * phone stays locked afterwards. Everything else stays keyguard-gated, and the
     * unlocked path is unchanged (standby handles the stop there). */
    if (app->keyguard_locked && app->clock_alarm_mode == 3u &&
        app->route == APP_ROUTE_STANDBY && app->input_len == 0u &&
        (key == KEY_NAVI || key == KEY_C)) {
        dismiss_clock_snooze(app, now);
        return true;
    }

    if (app->keyguard_locked) {
        return handle_keyguard_key(app, key, now);
    }

    if (app->route == APP_ROUTE_MAIN_MENU) {
        return handle_main_menu_key(app, key, now);
    }

    if (app->route == APP_ROUTE_STANDBY &&
        handle_net_monitor_overlay_key(
            app, key, event->type == EVENT_KEY_DOWN, now)) {
        return true;
    }

    return handle_standby_key(app, event);
}

void app_router_render(const app_t *app, framebuffer_t *fb) {
    switch (app->route) {
    case APP_ROUTE_POWERUP:
        render_powerup(app, fb);
        break;
    case APP_ROUTE_POWER_OFF:
        render_power_off(app, fb);
        break;
    case APP_ROUTE_POWER_MENU:
        render_power_menu(app, fb);
        break;
    case APP_ROUTE_MAIN_MENU:
        render_main_menu(app, fb);
        break;
    case APP_ROUTE_DISPLAY_MESSAGE:
        render_display_message(app, fb);
        break;
    case APP_ROUTE_CONFIRM:
        render_confirm(app, fb);
        break;
    case APP_ROUTE_EDITOR:
        render_editor(app, fb);
        break;
    case APP_ROUTE_PHONEBOOK_MENU:
        render_phonebook_menu(app, fb);
        break;
    case APP_ROUTE_PHONEBOOK_LIST:
        render_phonebook_list(app, fb);
        break;
    case APP_ROUTE_PHONEBOOK_MEMORY:
        render_phonebook_memory(app, fb);
        break;
    case APP_ROUTE_PHONEBOOK_TONE_PICKER:
        render_phonebook_tone_picker(app, fb);
        break;
    case APP_ROUTE_PHONEBOOK_SPEED_DIALS:
        render_phonebook_speed_dials(app, fb);
        break;
    case APP_ROUTE_PHONEBOOK_SPEED_DIAL_OPTIONS:
        render_phonebook_speed_options(app, fb);
        break;
    case APP_ROUTE_PHONEBOOK_EDIT_CHOICE:
        render_phonebook_edit_choice(app, fb);
        break;
    case APP_ROUTE_PHONEBOOK_SECURITY:
        render_phonebook_security(app, fb);
        break;
    case APP_ROUTE_MESSAGES_MENU:
        render_messages_menu(app, fb);
        break;
    case APP_ROUTE_MESSAGES_LIST:
        render_messages_list(app, fb);
        break;
    case APP_ROUTE_SMS_COMPOSER:
        render_sms_composer(app, fb);
        break;
    case APP_ROUTE_SMS_OPTIONS:
        render_sms_options(app, fb);
        break;
    case APP_ROUTE_SMS_SYMBOLS:
        render_sms_symbols(app, fb);
        break;
    case APP_ROUTE_CALL_REGISTER_MENU:
        render_call_register_menu(app, fb);
        break;
    case APP_ROUTE_CALL_REGISTER_LIST:
        render_call_register_list(app, fb);
        break;
    case APP_ROUTE_CALL_REGISTER_OPTIONS:
        render_call_register_options(app, fb);
        break;
    case APP_ROUTE_CALL_REGISTER_DETAIL:
        render_call_register_detail(app, fb);
        break;
    case APP_ROUTE_CALL_REGISTER_METRIC:
        render_call_register_metric(app, fb);
        break;
    case APP_ROUTE_CALL_DIVERT_MENU:
        render_call_divert_menu(app, fb);
        break;
    case APP_ROUTE_SERVICE_CODES:
        render_service_codes(app, fb);
        break;
    case APP_ROUTE_NET_MONITOR_TEST:
        render_net_monitor_test(app, fb);
        break;
    case APP_ROUTE_NET_MONITOR_PAGE:
        render_net_monitor_page(app, fb);
        break;
    case APP_ROUTE_PROFILES_MENU:
        render_profiles(app, fb);
        break;
    case APP_ROUTE_GAMES_MENU:
        render_games_menu(app, fb);
        break;
    case APP_ROUTE_ROTATION_MENU:
        render_rotation_menu(app, fb);
        break;
    case APP_ROUTE_ROTATION_PLAY:
    case APP_ROUTE_ROTATION_LAST_VIEW:
        render_rotation_play(app, fb);
        break;
    case APP_ROUTE_ROTATION_LEVEL:
        render_rotation_level(app, fb);
        break;
    case APP_ROUTE_ROTATION_INSTRUCTIONS:
        render_rotation_instructions(app, fb);
        break;
    case APP_ROUTE_ROTATION_TOP_SCORE:
        render_rotation_top_score(app, fb);
        break;
    case APP_ROUTE_ROTATION_RESULT:
        render_rotation_result(app, fb);
        break;
    case APP_ROUTE_MEMORY_MENU:
        render_memory_menu(app, fb);
        break;
    case APP_ROUTE_MEMORY_PLAY:
    case APP_ROUTE_MEMORY_LAST_VIEW:
        render_memory_play(app, fb);
        break;
    case APP_ROUTE_MEMORY_LEVEL:
        render_memory_level(app, fb);
        break;
    case APP_ROUTE_MEMORY_INSTRUCTIONS:
        render_memory_instructions(app, fb);
        break;
    case APP_ROUTE_MEMORY_TOP_SCORE:
        render_memory_top_score(app, fb);
        break;
    case APP_ROUTE_MEMORY_RESULT:
        render_memory_result(app, fb);
        break;
    case APP_ROUTE_REACT_MENU:
        render_react_menu(app, fb);
        break;
    case APP_ROUTE_REACT_PLAY:
    case APP_ROUTE_REACT_LAST_VIEW:
        render_react_play(app, fb);
        break;
    case APP_ROUTE_REACT_LEVEL:
        render_react_level(app, fb);
        break;
    case APP_ROUTE_REACT_INSTRUCTIONS:
        render_react_instructions(app, fb);
        break;
    case APP_ROUTE_REACT_TOP_SCORE:
        render_react_top_score(app, fb);
        break;
    case APP_ROUTE_REACT_RESULT:
        render_react_result(app, fb);
        break;
    case APP_ROUTE_LOGIC_MENU:
        render_logic_menu(app, fb);
        break;
    case APP_ROUTE_LOGIC_PLAY:
    case APP_ROUTE_LOGIC_LAST_VIEW:
        render_logic_play(app, fb);
        break;
    case APP_ROUTE_LOGIC_LEVEL:
        render_logic_level(app, fb);
        break;
    case APP_ROUTE_LOGIC_INSTRUCTIONS:
        render_logic_instructions(app, fb);
        break;
    case APP_ROUTE_LOGIC_RESULT:
        render_logic_result(app, fb);
        break;
    case APP_ROUTE_PACMAN_MENU:
        render_pacman_menu(app, fb);
        break;
    case APP_ROUTE_PACMAN_PLAY:
    case APP_ROUTE_PACMAN_LAST_VIEW:
        render_pacman_play(app, fb);
        break;
    case APP_ROUTE_PACMAN_LEVEL:
        render_pacman_level(app, fb);
        break;
    case APP_ROUTE_PACMAN_INSTRUCTIONS:
        render_pacman_instructions(app, fb);
        break;
    case APP_ROUTE_PACMAN_TOP_SCORE:
        render_pacman_top_score(app, fb);
        break;
    case APP_ROUTE_PACMAN_RESULT:
        render_pacman_result(app, fb);
        break;
    case APP_ROUTE_SNAKE_MENU:
        render_snake_menu(app, fb);
        break;
    case APP_ROUTE_SNAKE_PLAY:
    case APP_ROUTE_SNAKE_LAST_VIEW:
        render_snake_play(app, fb);
        break;
    case APP_ROUTE_SNAKE_LEVEL:
        render_snake_level(app, fb);
        break;
    case APP_ROUTE_SNAKE_INSTRUCTIONS:
        render_snake_instructions(app, fb);
        break;
    case APP_ROUTE_SNAKE_TOP_SCORE:
        render_snake_top_score(app, fb);
        break;
    case APP_ROUTE_SNAKE_RESULT:
        render_snake_result(app, fb);
        break;
    case APP_ROUTE_CALCULATOR:
        render_calculator(app, fb);
        break;
    case APP_ROUTE_CALL:
        render_call(app, fb);
        break;
    case APP_ROUTE_CALL_OPTIONS:
        render_call_options(app, fb);
        break;
    case APP_ROUTE_INCOMING_CALL:
        render_incoming_call(app, fb);
        break;
    case APP_ROUTE_CLOCK_MENU:
        render_clock_menu(app, fb);
        break;
    case APP_ROUTE_CLOCK_EDITOR:
        render_clock_editor(app, fb);
        break;
    case APP_ROUTE_CLOCK_ALARM:
        render_clock_alarm(app, fb);
        break;
    case APP_ROUTE_TONES_MENU:
        render_tones_menu(app, fb);
        break;
    case APP_ROUTE_TONES_SETTING:
        render_tones_setting(app, fb);
        break;
    case APP_ROUTE_TONE_COMPOSER:
        render_tone_composer(app, fb);
        break;
    case APP_ROUTE_TONE_COMPOSER_OPTIONS:
        render_tone_composer_options(app, fb);
        break;
    case APP_ROUTE_TONE_COMPOSER_TEMPO:
        render_tone_composer_tempo(app, fb);
        break;
    case APP_ROUTE_SETTINGS_MENU:
        render_settings_menu(app, fb);
        break;
    case APP_ROUTE_SETTINGS_VALUE:
        render_settings_value(app, fb);
        break;
    case APP_ROUTE_SETTINGS_WELCOME_OPTIONS:
        render_settings_welcome_options(app, fb);
        break;
    case APP_ROUTE_STANDBY:
    default:
        render_standby(app, fb);
        break;
    }
    render_net_monitor_overlay(app, fb);
}

#include "apps/phone_security_app.h"
#include "apps/settings_app.h"
#include "app_runtime.h"
#include "services/phone_security.h"
#include "services/input_keys.h"
#include "services/strings.h"
#include <assert.h>
#include <string.h>

static phone_security_state_t state;
static uint32_t generation = 1u;
static unsigned verifications, clock_setups, ordinary_polls, battery_polls, power_polls;
static uint16_t note_sid;
static security_code_result_t result;
static char submitted[11];
int32_t time_diff_ms(uint32_t a, uint32_t b) { return (int32_t)(a - b); }
phone_security_state_t phone_security_state(void) { return state; }
uint32_t phone_security_generation(void) { return generation; }
void security_code_forget_grant(void) {}
security_code_result_t security_code_verify(const char *code, uint32_t now) {
    (void)now; verifications++; strcpy(submitted, code); return result;
}
bool phone_security_unlock(uint32_t now) { (void)now; state = PHONE_SECURITY_OPEN; return true; }
bool start_clock_boot_setup_if_needed(app_t *app, uint32_t now) { (void)app; (void)now; clock_setups++; return false; }
void menu_shortcuts_cancel(app_t *app) { app->menu_shortcut_active = app->menu_shortcut_pending = false; }
void open_settings_menu(app_t *app, settings_menu_kind_t kind, uint8_t row) {
    assert(kind == SETTINGS_MENU_SECURITY && row == 3u); app->route = APP_ROUTE_SETTINGS_MENU;
}
void settings_open_phone_security_selector(app_t *app) { app->route = APP_ROUTE_SETTINGS_VALUE; }
void open_display_sid(app_t *app, uint8_t record, uint16_t sid, const char *text,
                      app_route_t destination, uint32_t now) {
    (void)record; (void)text; (void)now; note_sid = sid;
    app->route = APP_ROUTE_DISPLAY_MESSAGE; app->display_return_route = destination;
}
bool store_service_contact_service_required(void) { return false; }
void reset_settings_restore(app_t *app) { (void)app; }
void enter_contact_service(app_t *app, uint32_t now) { (void)now; app->route = APP_ROUTE_CONTACT_SERVICE; }
bool tick_power_off(app_t *app, uint32_t now) { (void)app; (void)now; power_polls++; return false; }
bool poll_battery(app_t *app, uint32_t now) { (void)app; (void)now; battery_polls++; return false; }
bool tick_display_message(app_t *app, uint32_t now) { (void)app; (void)now; return false; }
#define ORDINARY(name) bool name(app_t *app, uint32_t now) { (void)app; (void)now; ordinary_polls++; return false; }
ORDINARY(tick_network_selection)
ORDINARY(tick_contact_service)
ORDINARY(menu_shortcuts_tick)
ORDINARY(tick_powerup)
ORDINARY(tick_settings_restore)
ORDINARY(tick_standby)
ORDINARY(tick_main_menu)
ORDINARY(tick_editor)
ORDINARY(tick_clock_editor)
ORDINARY(poll_clock_alarm)
ORDINARY(poll_clock_datetime_commit)
ORDINARY(poll_call_runtime)
ORDINARY(poll_sms)
ORDINARY(tick_sms_composer)
ORDINARY(tick_clock_alarm)
ORDINARY(tick_call)
ORDINARY(poll_phonebook)
ORDINARY(tick_phonebook_erase_all)
ORDINARY(tick_phonebook_send)
ORDINARY(tick_call_divert)
ORDINARY(tick_call_waiting)
ORDINARY(tick_service_codes)
ORDINARY(tick_net_monitor)
ORDINARY(tick_profiles)
ORDINARY(tick_tones_setting)
ORDINARY(tick_tone_composer)
ORDINARY(tick_operator_logo)
ORDINARY(tick_business_card)
ORDINARY(tick_received_tone)
ORDINARY(tick_calculator)
ORDINARY(tick_rotation)
ORDINARY(tick_memory)
ORDINARY(tick_react)
ORDINARY(tick_logic)
ORDINARY(tick_pacman)
ORDINARY(tick_snake)
ORDINARY(poll_app_status)
#undef ORDINARY

static void key(app_t *app, uint16_t code) { handle_phone_security_key(app, code, EVENT_KEY_DOWN, 1000u); }
int main(void) {
    static app_t app;
    strings_set_language(0u);
    state = PHONE_SECURITY_LOCKED;
    app.route = APP_ROUTE_STANDBY;
    app.keyguard_locked = true;
    assert(tick_phone_security(&app, 10u) && app.route == APP_ROUTE_PHONE_SECURITY);
    assert(phone_security_blocks_ui(&app) && !app.keyguard_locked);
    key(&app, KEY_C); assert(app.route == APP_ROUTE_PHONE_SECURITY);
    key(&app, KEY_1); key(&app, KEY_2);
    framebuffer_t a, b;
    render_phone_security(&app, &a);
    strcpy(app.phone_security_code, "98"); render_phone_security(&app, &b);
    assert(memcmp(&a, &b, sizeof(a)) == 0);
    handle_phone_security_key(&app, KEY_C, EVENT_KEY_HOLD, 100u);
    assert(app.phone_security_code[0] == '\0');
    key(&app, KEY_1); result = SECURITY_CODE_WRONG; key(&app, KEY_NAVI);
    assert(verifications == 1u && strcmp(submitted, "1") == 0);
    assert(note_sid == 0x145u && app.phone_security_code[0] == '\0');
    assert(!tick_phone_security(&app, 101u) && app.route == APP_ROUTE_DISPLAY_MESSAGE);
    assert(app.display_return_route == APP_ROUTE_PHONE_SECURITY);
    assert(app_runtime_tick(&app, 102u) == false);
    assert(ordinary_polls == 0u && battery_polls == 1u && power_polls == 1u);
    static const app_route_t interruptions[] = {APP_ROUTE_CALL, APP_ROUTE_EDITOR, APP_ROUTE_MAIN_MENU,
        APP_ROUTE_CLOCK_ALARM, APP_ROUTE_SETTINGS_VALUE, APP_ROUTE_CONFIRM, APP_ROUTE_DISPLAY_MESSAGE};
    for (unsigned i = 0; i < sizeof(interruptions)/sizeof(interruptions[0]); i++) {
        app.route = interruptions[i]; app.display_return_route = APP_ROUTE_STANDBY;
        assert(tick_phone_security(&app, 103u) && app.route == APP_ROUTE_PHONE_SECURITY);
    }
    key(&app, KEY_1); generation++; state = PHONE_SECURITY_WAIT_IDENTITY;
    assert(tick_phone_security(&app, 104u) && app.phone_security_code[0] == '\0');
    key(&app, KEY_1); assert(app.phone_security_code[0] == '\0');
    state = PHONE_SECURITY_LOCKED;
    assert(tick_phone_security(&app, 105u));
    key(&app, KEY_1); result = SECURITY_CODE_OK; key(&app, KEY_NAVI);
    assert(app.route == APP_ROUTE_STANDBY && !phone_security_blocks_ui(&app) && clock_setups == 1u);
    open_phone_security(&app, false, 106u);
    key(&app, KEY_C); assert(app.route == APP_ROUTE_SETTINGS_MENU);
    open_phone_security(&app, false, 107u);
    key(&app, KEY_1); key(&app, KEY_NAVI);
    assert(app.route == APP_ROUTE_SETTINGS_VALUE && app.phone_security_setting_authorized);
    generation++; tick_phone_security(&app, 108u);
    assert(!app.phone_security_setting_authorized);
    app.route = APP_ROUTE_POWER_OFF;
    state = PHONE_SECURITY_LOCKED;
    tick_phone_security(&app, 109u); assert(!phone_security_blocks_ui(&app));
    app.route = APP_ROUTE_POWERUP; assert(phone_security_blocks_ui(&app));
    tick_phone_security(&app, 110u); assert(app.route == APP_ROUTE_POWERUP);
    ordinary_polls = 0u;
    app_runtime_tick(&app, 111u);
    assert(ordinary_polls == 1u && app.route == APP_ROUTE_POWERUP);
    app.route = APP_ROUTE_CONTACT_SERVICE; assert(!phone_security_blocks_ui(&app));
    return 0;
}

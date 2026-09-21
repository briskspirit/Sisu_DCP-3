#include <stdio.h>
#include <string.h>

#include "apps/menu_shortcuts.h"
#include "apps/dialogs_app.h"
#include "services/input_keys.h"

static int failures;
static unsigned activations, notices;
static unsigned previews;
static uint64_t hidden;
static uint8_t root_count = 11u;
static bool immediate, stay_put;
static char rendered[32];
int32_t time_diff_ms(uint32_t a, uint32_t b) { return (int32_t)(a - b); }

static void check(bool condition, const char *message) {
    if (!condition) { fprintf(stderr, "FAIL: %s\n", message); failures++; }
}

bool main_menu_shortcut_view(app_t *app, menu_shortcut_view_t *view) {
    view->selected = &app->menu_index;
    view->count = root_count;
    view->hidden_mask = hidden;
    view->immediate_mask = immediate ? UINT32_MAX : 0u;
    return true;
}

bool settings_shortcut_view(app_t *app, menu_shortcut_view_t *view) {
    view->selected = &app->settings_menu_selected;
    view->view_start = &app->settings_value_view_start;
    view->count = 4u;
    view->context = app->settings_menu_kind;
    return true;
}

#define NO_VIEW(name) \
    bool name(app_t *app, menu_shortcut_view_t *view) { \
        (void)app; (void)view; return false; \
    }
NO_VIEW(phonebook_shortcut_view)
NO_VIEW(messages_shortcut_view)
NO_VIEW(call_register_shortcut_view)
NO_VIEW(call_divert_shortcut_view)
NO_VIEW(clock_shortcut_view)
bool tones_shortcut_view(app_t *app, menu_shortcut_view_t *view) {
    view->selected = &app->tones_setting_selected;
    view->count = 6u;
    return true;
}
void tones_shortcut_selected(app_t *app, uint32_t now) {
    (void)now;
    check(app->route == APP_ROUTE_TONES_SETTING && app->tones_setting_selected == 2u,
          "preview observes the selected tone row");
    previews++;
}
NO_VIEW(profiles_shortcut_view)
NO_VIEW(games_shortcut_view)
bool snake_shortcut_view(app_t *app, menu_shortcut_view_t *view) {
    view->selected = &app->snake_menu_selected;
    view->count = 4u;
    view->immediate_mask = 0xfu;
    return true;
}
NO_VIEW(rotation_shortcut_view)
NO_VIEW(memory_shortcut_view)
NO_VIEW(react_shortcut_view)
NO_VIEW(logic_shortcut_view)
NO_VIEW(pacman_shortcut_view)

void app_router_select_menu(app_t *app, uint32_t now) {
    (void)now;
    activations++;
    if (stay_put) return;
    if (app->route == APP_ROUTE_SNAKE_MENU) {
        app->route = APP_ROUTE_SNAKE_LEVEL;
    } else if (app->route == APP_ROUTE_MAIN_MENU) {
        app->route = APP_ROUTE_SETTINGS_MENU;
        app->settings_menu_kind = 0u;
        app->settings_menu_selected = 0u;
    } else if (app->settings_menu_kind == 0u) {
        app->settings_menu_kind = 1u;
        app->settings_menu_selected = 0u;
    } else {
        app->route = APP_ROUTE_EDITOR;
    }
}

void open_display_sid(app_t *app, uint8_t record, uint16_t sid, const char *fallback,
                       app_route_t back, uint32_t now) {
    (void)fallback; (void)now;
    check(record == 52u && sid == 0x2aau && back == APP_ROUTE_STANDBY,
          "invalid shortcut uses the stock record, localized SID and menu exit");
    notices++;
    app->route = APP_ROUTE_DISPLAY_MESSAGE;
}
void fb_fill_rect(framebuffer_t *fb, int x, int y, int w, int h, bool on) {
    (void)fb;
    check(x == 0 && y == 0 && w == 84 && h == 6 && !on,
          "prefix redraw only clears the counter strip");
}
const font_t *asset_font(font_id_t id) { (void)id; return NULL; }
void draw_right_text_box(framebuffer_t *fb, const font_t *font, const char *text,
                          int x, int y, int w) {
    (void)fb; (void)font; (void)x; (void)y; (void)w;
    snprintf(rendered, sizeof(rendered), "%s", text);
}

static bool event(app_t *app, event_type_t type, uint16_t key, uint32_t now) {
    input_event_t input = {.type = type, .code = key, .when_ms = now};
    return menu_shortcuts_handle(app, &input);
}
static bool press(app_t *app, uint16_t key, uint32_t now) {
    bool result = event(app, EVENT_KEY_DOWN, key, now);
    (void)event(app, EVENT_KEY_UP, key, now + 1u);
    return result;
}
static void begin(app_t *app, uint32_t now) {
    memset(app, 0, sizeof(*app));
    activations = notices = previews = 0u;
    hidden = 0u; root_count = 11u; immediate = stay_put = false;
    rendered[0] = '\0';
    app->route = APP_ROUTE_MAIN_MENU;
    app->menu_keyguard_armed = true;
    menu_shortcuts_begin(app, now);
}

static void test_timers(void) {
    app_t app;
    begin(&app, 100u);
    check(!menu_shortcuts_tick(&app, 3179u), "input window lasts 3080 ms");
    check(menu_shortcuts_tick(&app, 3180u) && !app.menu_shortcut_active,
          "idle input window expires at its boundary");
    check(!press(&app, KEY_4, 3181u) && activations == 0u, "late digit cannot open a menu");

    begin(&app, 0u);
    check(press(&app, KEY_4, 100u), "digit selects a menu");
    check(app.menu_index == 3u && app.menu_shortcut_pending && activations == 0u,
          "unambiguous choice is highlighted before deferred activation");
    check(!app.menu_keyguard_armed, "numeric navigation ends Menu-star chord");
    check(!menu_shortcuts_tick(&app, 1635u), "selection waits for all 1536 ms");
    check(menu_shortcuts_tick(&app, 1636u) && activations == 1u &&
          app.route == APP_ROUTE_SETTINGS_MENU, "selection opens on the stock timer");
    check(!menu_shortcuts_tick(&app, 4715u), "submenu has its own input window");
    check(menu_shortcuts_tick(&app, 4716u) && !app.menu_shortcut_active,
          "submenu input window also expires");

    begin(&app, UINT32_MAX - 99u);
    check(!menu_shortcuts_tick(&app, 2979u), "deadline handles clock wrap before expiry");
    check(menu_shortcuts_tick(&app, 2980u), "deadline handles clock wrap at expiry");
    begin(&app, UINT32_MAX - 2000u);
    press(&app, KEY_4, UINT32_MAX - 1535u);
    check(!menu_shortcuts_tick(&app, UINT32_MAX), "zero deadline is not a disabled sentinel");
    check(menu_shortcuts_tick(&app, 0u) && activations == 1u, "zero deadline activates normally");
}

static void test_prefixes_and_visibility(void) {
    app_t app;
    begin(&app, 0u);
    press(&app, KEY_1, 10u);
    check(!app.menu_shortcut_pending && activations == 0u, "1 waits when 10/11 exist");
    menu_shortcuts_render(&app, NULL);
    check(strcmp(rendered, "1") == 0, "pending number appears in the stock counter");
    press(&app, KEY_0, 100u);
    check(app.menu_index == 9u && app.menu_shortcut_pending, "10 is one root choice");
    begin(&app, 0u);
    press(&app, KEY_1, 10u); press(&app, KEY_1, 100u);
    check(app.menu_index == 10u && app.menu_shortcut_pending, "11 is one root choice");
    begin(&app, 0u);
    press(&app, KEY_1, 10u);
    menu_shortcuts_tick(&app, 3090u);
    check(app.menu_index == 0u && app.menu_shortcut_pending && activations == 0u,
          "a lone ambiguous 1 resolves on input timeout");

    begin(&app, 0u);
    hidden = (UINT64_C(1) << 9) | (UINT64_C(1) << 10);
    press(&app, KEY_1, 10u);
    check(app.menu_shortcut_pending, "hidden trailing entries do not keep 1 ambiguous");
    begin(&app, 0u);
    hidden = UINT64_C(1) << 4;
    press(&app, KEY_6, 10u);
    check(app.menu_index == 5u, "hidden middle row does not renumber the next choice");
    begin(&app, 0u);
    hidden = UINT64_C(1) << 4;
    press(&app, KEY_5, 10u);
    check(notices == 1u && activations == 0u && !app.menu_shortcut_active,
          "hidden choice is rejected, not normalized to another action");
    begin(&app, 0u);
    press(&app, KEY_1, 10u); press(&app, KEY_2, 20u);
    check(notices == 1u && activations == 0u, "out-of-range two-digit number is invalid");
    begin(&app, 0u);
    press(&app, KEY_0, 10u); press(&app, KEY_0, 20u); press(&app, KEY_4, 30u);
    check(app.menu_index == 3u && app.menu_shortcut_pending, "two-character buffer tolerates leading zeros");
    begin(&app, 0u);
    press(&app, KEY_0, 10u);
    menu_shortcuts_tick(&app, 3090u);
    check(notices == 1u && activations == 0u, "zero-only shortcut fails after timeout");
    begin(&app, 0u);
    press(&app, KEY_4, 10u);
    hidden = UINT64_C(1) << 3;
    menu_shortcuts_tick(&app, 1546u);
    check(activations == 0u && !app.menu_shortcut_active, "visibility is rechecked before deferred action");
}

static void test_sequences_and_ownership(void) {
    app_t app;
    begin(&app, 0u);
    press(&app, KEY_4, 10u); press(&app, KEY_2, 20u); press(&app, KEY_3, 30u);
    check(activations == 2u && app.settings_menu_kind == 1u &&
          app.settings_menu_selected == 2u && app.settings_value_view_start == 2u,
          "rapid digits flush deferred parents and select the correct child");
    check(strcmp(app.menu_shortcut_path, "4-2") == 0, "numeric breadcrumb keeps the parent path");
    check(!press(&app, KEY_7, 40u) && app.route == APP_ROUTE_EDITOR && activations == 3u,
          "next fresh digit belongs to an editor reached by the previous shortcut");

    begin(&app, 0u);
    immediate = true;
    event(&app, EVENT_KEY_DOWN, KEY_4, 10u);
    check(activations == 1u, "stock immediate item does not wait");
    event(&app, EVENT_KEY_HOLD, KEY_4, 810u);
    check(activations == 1u && !app.menu_shortcut_pending,
          "held navigation digit is not reused in the submenu");
    app.route = APP_ROUTE_EDITOR;
    menu_shortcuts_tick(&app, 820u);
    check(event(&app, EVENT_KEY_HOLD, KEY_4, 830u), "held shortcut never leaks into an editor");
    event(&app, EVENT_KEY_UP, KEY_4, 840u);
    check(!event(&app, EVENT_KEY_HOLD, KEY_4, 850u), "release clears the consumed-key latch");

    begin(&app, 0u);
    app.route = APP_ROUTE_SNAKE_MENU;
    menu_shortcuts_begin(&app, 0u);
    check(press(&app, KEY_1, 10u) && app.route == APP_ROUTE_SNAKE_LEVEL &&
          activations == 1u && !app.menu_shortcut_active,
          "game option acts immediately and leaves the ladder's keys alone");
    check(!press(&app, KEY_2, 20u), "level ladder retains ordinary digit controls");

    begin(&app, 0u);
    app.route = APP_ROUTE_TONES_SETTING;
    menu_shortcuts_begin(&app, 0u);
    press(&app, KEY_3, 10u);
    check(previews == 1u && activations == 0u,
          "tone shortcut previews its highlighted row before delayed save");

    static const app_route_t interruptions[] = {
        APP_ROUTE_DISPLAY_MESSAGE, APP_ROUTE_INCOMING_CALL, APP_ROUTE_CALL,
        APP_ROUTE_CONFIRM, APP_ROUTE_POWER_OFF, APP_ROUTE_CONTACT_SERVICE,
        APP_ROUTE_SMS_COMPOSER, APP_ROUTE_SNAKE_PLAY, APP_ROUTE_SNAKE_MENU,
    };
    for (unsigned i = 0u; i < sizeof(interruptions) / sizeof(interruptions[0]); i++) {
        begin(&app, 0u);
        press(&app, KEY_4, 10u);
        app.route = interruptions[i];
        menu_shortcuts_tick(&app, 20u);
        app.route = APP_ROUTE_MAIN_MENU;
        menu_shortcuts_tick(&app, 2000u);
        check(activations == 0u && !app.menu_shortcut_active,
              "interrupted timeout cannot fire after returning to the same menu");
    }
    begin(&app, 0u);
    app.route = APP_ROUTE_SETTINGS_MENU;
    menu_shortcuts_begin(&app, 0u);
    press(&app, KEY_2, 10u);
    app.settings_menu_kind++;
    menu_shortcuts_tick(&app, 2000u);
    check(activations == 0u, "same-route context change invalidates the timeout");
    begin(&app, 0u);
    press(&app, KEY_4, 10u);
    app.menu_index = 0u;
    menu_shortcuts_tick(&app, 2000u);
    check(activations == 0u, "changed selection cannot execute a stale action");
    begin(&app, 0u);
    app.keyguard_locked = true;
    check(!press(&app, KEY_4, 10u) && activations == 0u, "keyguard owns its keys");
    begin(&app, 0u);
    press(&app, KEY_4, 10u);
    check(!press(&app, KEY_C, 20u), "C cancels numeric state and retains ordinary back action");
    menu_shortcuts_tick(&app, 2000u);
    check(activations == 0u, "C does not execute a pending choice");
    begin(&app, 0u);
    press(&app, KEY_4, 10u); press(&app, KEY_POWER, 20u);
    menu_shortcuts_tick(&app, 2000u);
    check(activations == 0u, "Power cancels without executing the menu action");
    begin(&app, 0u);
    press(&app, KEY_4, 10u); press(&app, KEY_DOWN, 20u);
    check(!press(&app, KEY_2, 30u), "manual scrolling leaves numeric shortcut mode");
    begin(&app, 0u);
    stay_put = true;
    press(&app, KEY_4, 10u); menu_shortcuts_tick(&app, 2000u);
    check(!app.menu_shortcut_active && activations == 1u, "unchanged action cannot repeat forever");
}

int main(void) {
    test_timers();
    test_prefixes_and_visibility();
    test_sequences_and_ownership();
    if (failures) fprintf(stderr, "%d failures\n", failures);
    return failures ? 1 : 0;
}

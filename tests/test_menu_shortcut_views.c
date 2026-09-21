#include <stdio.h>
#include <string.h>

#include "apps/menu_shortcuts.h"
#include "apps/settings_app.h"
#include "apps/messages_app.h"
#include "apps/phonebook_app.h"
#include "apps/call_divert_app.h"
#include "apps/call_register_app.h"
#include "apps/clock_app.h"
#include "apps/profiles_app.h"
#include "apps/tones_app.h"
#include "services/feature_gates.h"

static int failures;
static bool headset, own_tone[2];
bool board_diag_headset_inserted(void) { return headset; }
bool store_own_tone_used(uint8_t slot) { return slot < 2u && own_tone[slot]; }
uint8_t strings_language_count(void) { return 1u; }
uint8_t strings_language_id_at(uint8_t index) { (void)index; return 1u; }
const char *strings_language_self_name_at(uint8_t index) { (void)index; return "English"; }
const char *ts_or(uint16_t sid, const char *fallback) { (void)sid; return fallback; }

static void check(bool condition, const char *message) {
    if (!condition) { fprintf(stderr, "FAIL: %s\n", message); failures++; }
}
typedef bool (*view_fn)(app_t *, menu_shortcut_view_t *);
static menu_shortcut_view_t get(view_fn fn, app_t *app) {
    menu_shortcut_view_t view = {0};
    check(fn(app, &view) && view.selected != NULL && view.count > 0u && view.count <= 64u,
          "numbered menu exposes a bounded selection");
    return view;
}

int main(void) {
    app_t app = {0};
    feature_gates_reset_defaults();
    feature_gate_set_visible(FEATURE_GATE_PROFILES, false);
    feature_gate_set_visible(FEATURE_GATE_NET_MONITOR, false);
    menu_shortcut_view_t view = get(main_menu_shortcut_view, &app);
    check(view.count == 11u && view.selected == &app.menu_index &&
          (view.hidden_mask & (UINT64_C(3) << 9)) == (UINT64_C(3) << 9) &&
          view.immediate_mask == (1u << 6), "root preserves ordinals and stock calculator flag");

    feature_gate_set_visible(FEATURE_GATE_PHONEBOOK_SERVICE_NOS, false);
    feature_gate_set_visible(FEATURE_GATE_PHONEBOOK_INFO_NUMBERS, false);
    app.phonebook_menu_kind = PHONEBOOK_MENU_ROOT;
    view = get(phonebook_shortcut_view, &app);
    check(view.count == 10u && (view.hidden_mask & 0x202u) == 0x202u &&
          view.selected == &app.phonebook_menu_selected && (view.immediate_mask & 1u),
          "phonebook gates and immediate Search match the original table");
    app.phonebook_menu_kind = PHONEBOOK_MENU_OPTIONS;
    view = get(phonebook_shortcut_view, &app);
    check(view.count == 2u && view.context == PHONEBOOK_MENU_OPTIONS && !view.hidden_mask,
          "phonebook Options has independent numbering");

    app.route = APP_ROUTE_SETTINGS_MENU;
    app.settings_menu_kind = SETTINGS_MENU_CALL;
    feature_gate_set_visible(FEATURE_GATE_CALL_SETTINGS_AUTOMATIC_ANSWER, true);
    view = get(settings_shortcut_view, &app);
    check(view.count == 6u && (view.hidden_mask & (1u << 5)),
          "automatic answer cannot be reached without its accessory");
    headset = true;
    view = get(settings_shortcut_view, &app);
    check(!(view.hidden_mask & (1u << 5)), "accessory visibility is read live");

    app.route = APP_ROUTE_MESSAGES_MENU;
    feature_gate_set_visible(FEATURE_GATE_MESSAGES_INFO_SERVICE, false);
    view = get(messages_shortcut_view, &app);
    check(view.count == 7u && (view.hidden_mask & (1u << 5)) &&
          view.immediate_mask == 0x4cu, "messages use existing gates and editor handoffs");
    app.route = APP_ROUTE_MESSAGES_LIST;
    app.messages_mode = MESSAGES_MODE_SETTINGS_TOP;
    view = get(messages_shortcut_view, &app);
    check(view.count == 2u && view.selected == &app.messages_settings_top_selected,
          "message sets are numbered menus");
    app.messages_mode = MESSAGES_MODE_SETTINGS_MENU;
    app.messages_settings_kind = 0u;
    view = get(messages_shortcut_view, &app);
    check(view.count == 3u, "Set 1 has three choices");
    app.messages_settings_kind = 1u;
    view = get(messages_shortcut_view, &app);
    check(view.count == 2u, "Common has two choices");
    for (uint8_t mode = MESSAGES_MODE_LIST; mode <= MESSAGES_MODE_DETAIL; mode++) {
        app.messages_mode = mode;
        memset(&view, 0, sizeof(view));
        check(!messages_shortcut_view(&app, &view), "inbox contents/actions never inherit shortcut state");
    }

    app.route = APP_ROUTE_TONES_MENU;
    feature_gate_set_visible(FEATURE_GATE_TONES_VIBRATING_ALERT, false);
    view = get(tones_shortcut_view, &app);
    check(view.count == 8u && (view.hidden_mask & 0x80u) && view.immediate_mask == 4u,
          "tones preserve hidden Vibra and immediate Composer");
    app.route = APP_ROUTE_TONES_SETTING;
    app.tones_setting_kind = TONES_SETTING_RINGING_TONE;
    view = get(tones_shortcut_view, &app);
    uint8_t base_count = view.count;
    own_tone[1] = true;
    view = get(tones_shortcut_view, &app);
    check(view.count == base_count + 1u && view.view_start == &app.tones_setting_view_start,
          "ringtone numbering follows the live visible catalogue");
    own_tone[0] = true;
    view = get(tones_shortcut_view, &app);
    check(view.count == base_count + 2u, "both custom tones are addressable when stored");

    app.route = APP_ROUTE_CALL_REGISTER_MENU;
    app.call_register_menu_kind = CALL_REGISTER_MENU_ROOT;
    view = get(call_register_shortcut_view, &app);
    check(view.count == 8u && view.immediate_mask == 7u, "call register list handoffs match stock flags");
    app.route = APP_ROUTE_CALL_REGISTER_METRIC;
    app.call_register_metric_kind = CALL_REGISTER_METRIC_DURATION;
    view = get(call_register_shortcut_view, &app);
    check(view.count == 5u && view.selected == &app.call_register_metric_selected,
          "call duration counters share ordinary menu actions");

    app.call_divert_menu_kind = CALL_DIVERT_MENU_CONDITION;
    app.call_divert_condition_index = 0u;
    view = get(call_divert_shortcut_view, &app);
    uint32_t context = view.context;
    app.call_divert_condition_index = 1u;
    view = get(call_divert_shortcut_view, &app);
    check(view.context != context, "divert condition changes invalidate deferred requests");
    app.clock_menu_kind = CLOCK_MENU_ROOT;
    view = get(clock_shortcut_view, &app);
    check(view.count == 3u && view.immediate_mask == 4u, "clock date editor is an immediate handoff");
    view = get(profiles_shortcut_view, &app);
    check(view.selected == &app.profiles_selected_index, "profiles use their own selected row");
    feature_gate_set_visible(FEATURE_GATE_GAMES_EXTRA_ROWS, false);
    view = get(games_shortcut_view, &app);
    check(view.count == 6u && view.hidden_mask == 0x18u &&
          view.view_start == &app.game_option_view_start,
          "hidden extra games do not renumber the bonus game");
    const view_fn game_views[] = {snake_shortcut_view, rotation_shortcut_view,
        memory_shortcut_view, react_shortcut_view, logic_shortcut_view, pacman_shortcut_view};
    uint8_t *variants[] = {&app.snake_menu_variant, &app.rotation_menu_variant,
        &app.memory_menu_variant, &app.react_menu_variant, &app.logic_menu_variant,
        &app.pacman_menu_variant};
    for (unsigned i = 0; i < ARRAY_COUNT(game_views); i++) {
        view = get(game_views[i], &app);
        uint8_t count = view.count;
        check(count == (i == 4u ? 3u : 4u) &&
              view.immediate_mask == (1u << count) - 1u,
              "game action menus inherit shortcuts and activate immediately");
        *variants[i] = 2u; /* Continue */
        view = get(game_views[i], &app);
        check(view.count == count + 1u && view.context == 2u &&
              view.view_start == &app.game_option_view_start,
              "resumable game menu uses the live action list");
        *variants[i] = 3u; /* Last view */
        view = get(game_views[i], &app);
        check(view.count == count + 1u && view.context == 3u,
              "game menu variant invalidates a stale shortcut context");
    }
    if (failures) fprintf(stderr, "%d failures\n", failures);
    return failures ? 1 : 0;
}

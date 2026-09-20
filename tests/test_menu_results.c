#include <stdio.h>
#include <string.h>

#include "apps/settings_app.h"
#include "apps/call_register_app.h"
#include "apps/dialogs_app.h"
#include "services/input_keys.h"
#include "services/strings.h"
#include "storage/store_service.h"

static int s_failures;
static uint8_t s_values[STORE_SETTING_COUNT];
static uint32_t s_counters[STORE_SETTING_COUNT];
static unsigned s_writes, s_editors, s_language_applies;
static uint8_t s_record;
static uint16_t s_sid;
static app_route_t s_return;
static char s_preview[64];
static char s_code[STORE_TEXT_MAX + 1u] = "12345";
static store_status_t s_write_status = STORE_STATUS_OK;

static void check(bool ok, const char *what) {
    if (!ok) { fprintf(stderr, "FAIL: %s\n", what); s_failures++; }
}

void copy_text(char *dst, size_t cap, const char *src) {
    snprintf(dst, cap, "%s", src != NULL ? src : "");
}
const char *ts(uint16_t sid) { (void)sid; return NULL; }
const char *ts_or(uint16_t sid, const char *fallback) { (void)sid; return fallback; }
uint8_t strings_language_count(void) { return 1u; }
uint8_t strings_language_id_at(uint8_t index) { (void)index; return 1u; }
const char *strings_language_self_name_at(uint8_t index) { (void)index; return "English"; }
void strings_set_language(uint8_t id) { (void)id; s_language_applies++; }
void profiles_restore_factory_defaults(void) {}
bool board_diag_headset_inserted(void) { return true; }

store_status_t store_setting_get_u8(store_setting_key_t key, uint8_t *value) {
    *value = s_values[key]; return STORE_STATUS_OK;
}
store_status_t store_setting_set_u8(store_setting_key_t key, uint8_t value) {
    s_writes++;
    if (s_write_status == STORE_STATUS_OK) s_values[key] = value;
    return s_write_status;
}
store_status_t store_setting_get_u32(store_setting_key_t key, uint32_t *value) {
    *value = s_counters[key]; return STORE_STATUS_OK;
}
store_status_t store_setting_set_u32(store_setting_key_t key, uint32_t value) {
    s_writes++; s_counters[key] = value; return STORE_STATUS_OK;
}
store_status_t store_setting_get_text(store_setting_key_t key, char *out, uint8_t cap) {
    copy_text(out, cap, key == STORE_SETTING_SECURITY_CODE ? s_code : "");
    return STORE_STATUS_OK;
}
store_status_t store_setting_set_text(store_setting_key_t key, const char *text) {
    s_writes++;
    if (s_write_status == STORE_STATUS_OK && key == STORE_SETTING_SECURITY_CODE)
        copy_text(s_code, sizeof(s_code), text);
    return s_write_status;
}
uint8_t store_call_count(store_call_list_t list) { (void)list; return 0u; }
store_status_t store_call_clear(store_call_list_t list) {
    (void)list; s_writes++; return STORE_STATUS_OK;
}

void open_display_sid(app_t *app, uint8_t record, uint16_t sid,
                      const char *fallback, app_route_t back, uint32_t now) {
    (void)fallback; (void)now;
    s_record = record; s_sid = sid; s_return = back;
    app->route = APP_ROUTE_DISPLAY_MESSAGE;
}
void open_main_menu_at(app_t *app, uint8_t selected, uint32_t now) {
    (void)now; app->menu_index = selected; app->route = APP_ROUTE_MAIN_MENU;
}
void open_editor(app_t *app, const char *title, const char *value, uint8_t max_len,
                 editor_kind_t kind, editor_context_t context, bool search, uint32_t now) {
    (void)title; (void)max_len; (void)kind; (void)search; (void)now;
    s_editors++; app->route = APP_ROUTE_EDITOR; app->editor_context = context;
    copy_text(app->editor_value, sizeof(app->editor_value), value);
}
void close_editor(app_t *app) { app->editor_context = EDITOR_CONTEXT_NONE; }
void fb_clear(framebuffer_t *fb, bool on) { (void)fb; (void)on; }
void draw_static_page_list(framebuffer_t *fb, const char *label, const char *preview,
                           uint8_t selected, uint8_t count, const char *crumb,
                           const char *softkey) {
    (void)fb; (void)label; (void)selected; (void)count; (void)crumb; (void)softkey;
    copy_text(s_preview, sizeof(s_preview), preview);
}

static void expect_not_done(const app_t *app, app_route_t back, const char *what) {
    check(app->route == APP_ROUTE_DISPLAY_MESSAGE &&
              s_record == 32u && s_sid == 0x210u && s_return == back, what);
    s_record = 0xffu;
    s_sid = 0xffffu;
}

static void test_unavailable_settings(void) {
    static const struct { settings_menu_kind_t menu; uint8_t row, choices; } cases[] = {
        {SETTINGS_MENU_CALL, 2u, 3u}, {SETTINGS_MENU_CALL, 4u, 2u},
        {SETTINGS_MENU_PHONE, 3u, 2u},
        {SETTINGS_MENU_SECURITY, 0u, 2u}, {SETTINGS_MENU_SECURITY, 1u, 3u},
        {SETTINGS_MENU_SECURITY, 2u, 3u}, {SETTINGS_MENU_SECURITY, 3u, 2u},
    };
    memset(s_values, 1, sizeof(s_values));
    for (unsigned i = 0u; i < sizeof(cases) / sizeof(cases[0]); i++) {
        for (uint8_t choice = 0u; choice < cases[i].choices; choice++) {
            app_t app = {0};
            unsigned before = s_writes, editors = s_editors;
            open_settings_menu(&app, cases[i].menu, cases[i].row);
            render_settings_menu(&app, NULL);
            const char *preview = cases[i].menu == SETTINGS_MENU_CALL && cases[i].row == 2u ? "" : "--";
            check(strcmp(s_preview, preview) == 0,
                  "unsupported setting preserves its preview layout without inventing state");
            handle_settings_menu_key(&app, KEY_NAVI, 10u);
            check(app.route == APP_ROUTE_SETTINGS_VALUE, "original value picker remains available");
            app.settings_value_selected = choice;
            handle_settings_value_key(&app, KEY_NAVI, 20u);
            expect_not_done(&app, APP_ROUTE_SETTINGS_MENU, "unsupported setting fails immediately");
            check(s_writes == before && s_editors == editors,
                  "unsupported setting neither writes nor asks for credentials");
            check(app.settings_menu_kind == cases[i].menu && app.settings_menu_selected == cases[i].row,
                  "failure returns to the selected parent menu");
        }
    }
    for (uint8_t kind = 1u; kind <= 2u; kind++) {
        app_t app = {0};
        unsigned before = s_writes, editors = s_editors;
        open_settings_menu(&app, SETTINGS_MENU_ACCESS_CODES, kind);
        handle_settings_menu_key(&app, KEY_NAVI, 30u);
        expect_not_done(&app, APP_ROUTE_SETTINGS_MENU, "PIN/PIN2 menu never starts a simulated change");
        check(s_editors == editors, "unsupported PIN operations do not collect a code");
        for (uint8_t step = 0u; step < 3u; step++) {
            app.settings_access_kind = kind; app.settings_access_step = step;
            copy_text(app.editor_value, sizeof(app.editor_value), "1234");
            settings_submit_access_code_editor(&app, 40u);
            expect_not_done(&app, APP_ROUTE_SETTINGS_MENU, "direct PIN callback cannot report success");
        }
        settings_submit_pin_request_editor(&app, 50u);
        expect_not_done(&app, APP_ROUTE_SETTINGS_MENU, "direct PIN-request callback cannot report success");
        check(s_writes == before, "SIM security callbacks leave local preferences alone");
    }
    app_t app = {0};
    unsigned before = s_writes;
    open_settings_menu(&app, SETTINGS_MENU_SECURITY, 5u);
    handle_settings_menu_key(&app, KEY_NAVI, 60u);
    expect_not_done(&app, APP_ROUTE_SETTINGS_MENU, "phone line protection is not simulated");
    check(s_writes == before, "phone line protection does not change a local flag");
}

static void test_local_preferences(void) {
    static const struct { settings_menu_kind_t menu; uint8_t row; store_setting_key_t key; } cases[] = {
        {SETTINGS_MENU_CALL, 0u, STORE_SETTING_SETTINGS_AUTOMATIC_REDIAL},
        {SETTINGS_MENU_CALL, 1u, STORE_SETTING_SETTINGS_SPEED_DIALLING},
        {SETTINGS_MENU_CALL, 3u, STORE_SETTING_CALL_OWN_NUMBER_SENDING},
        {SETTINGS_MENU_CALL, 5u, STORE_SETTING_SETTINGS_AUTOMATIC_ANSWER},
        {SETTINGS_MENU_PHONE, 0u, STORE_SETTING_SYSTEM_LANGUAGE},
        {SETTINGS_MENU_PHONE, 1u, STORE_SETTING_SETTINGS_CELL_INFO_DISPLAY},
        {SETTINGS_MENU_PHONE, 4u, STORE_SETTING_SETTINGS_LIGHTS},
        {SETTINGS_MENU_PHONE, 5u, STORE_SETTING_SETTINGS_CONFIRM_SIM_ACTIONS},
    };
    for (unsigned i = 0u; i < sizeof(cases) / sizeof(cases[0]); i++) {
        app_t app = {0};
        open_settings_menu(&app, cases[i].menu, cases[i].row);
        render_settings_menu(&app, NULL);
        check(s_preview[0] != '\0', "local preference retains its saved-value preview");
        handle_settings_menu_key(&app, KEY_NAVI, 10u);
        app.settings_value_selected = 0u;
        unsigned before = s_writes;
        handle_settings_value_key(&app, KEY_NAVI, 20u);
        check(s_sid == 0x132u && s_writes == before + 1u, "local preference can be saved, including simulated settings");
        open_settings_menu(&app, cases[i].menu, cases[i].row);
        handle_settings_menu_key(&app, KEY_NAVI, 30u);
        uint8_t saved = s_values[cases[i].key];
        unsigned language_before = s_language_applies;
        s_write_status = STORE_STATUS_INVALID_ARGUMENT;
        handle_settings_value_key(&app, KEY_NAVI, 40u);
        expect_not_done(&app, APP_ROUTE_SETTINGS_VALUE, "rejected local write does not report Done");
        check(s_values[cases[i].key] == saved && s_language_applies == language_before,
              "failed local write preserves the live preference");
        s_write_status = STORE_STATUS_OK;
    }
    app_t app = {0};
    open_settings_menu(&app, SETTINGS_MENU_ACCESS_CODES, 0u);
    handle_settings_menu_key(&app, KEY_NAVI, 50u);
    copy_text(app.editor_value, sizeof(app.editor_value), "wrong");
    settings_submit_access_code_editor(&app, 60u);
    check(s_sid == 0x145u && app.settings_access_step == 0u, "local security code is still checked");
    copy_text(app.editor_value, sizeof(app.editor_value), "12345");
    settings_submit_access_code_editor(&app, 70u);
    copy_text(app.editor_value, sizeof(app.editor_value), "54321");
    settings_submit_access_code_editor(&app, 80u);
    copy_text(app.editor_value, sizeof(app.editor_value), "54321");
    s_write_status = STORE_STATUS_INVALID_ARGUMENT;
    settings_submit_access_code_editor(&app, 90u);
    expect_not_done(&app, APP_ROUTE_EDITOR, "failed security-code write is not acknowledged");
    check(strcmp(s_code, "12345") == 0, "old code survives a failed write");
    s_write_status = STORE_STATUS_OK;
    settings_submit_access_code_editor(&app, 100u);
    check(s_sid == 0x29bu && strcmp(s_code, "54321") == 0, "local security-code change still works");
}

static void test_call_register_results(void) {
    for (uint8_t row = 5u; row < 8u; row++) {
        app_t app = {0};
        unsigned before = s_writes;
        open_call_register_menu(&app, CALL_REGISTER_MENU_ROOT, row);
        handle_call_register_menu_key(&app, KEY_NAVI, 10u);
        if (row == 5u) {
            check(app.route == APP_ROUTE_CALL_REGISTER_METRIC &&
                      app.call_register_metric_kind == CALL_REGISTER_METRIC_COSTS,
                  "Show call costs preserves its original submenu");
        } else {
            check(app.route == APP_ROUTE_CALL_REGISTER_MENU &&
                      app.call_register_menu_kind == (row == 6u ? CALL_REGISTER_MENU_COST_SETTINGS : CALL_REGISTER_MENU_PREPAID),
                  "cost settings and prepaid preserve their original submenus");
        }
        check(s_writes == before, "opening billing submenus does not change preferences");
    }
    app_t app = {0};
    static const struct { call_register_menu_kind_t kind; uint8_t row; const char *preview; } queries[] = {
        {CALL_REGISTER_MENU_COST_SETTINGS, 0u, "--"},
        {CALL_REGISTER_MENU_COST_SETTINGS, 1u, "--"},
        {CALL_REGISTER_MENU_PREPAID, 1u, "--"},
        {CALL_REGISTER_MENU_PREPAID, 2u, "--"},
        {CALL_REGISTER_MENU_RECHARGE_STATUS, 0u, "--.--"},
        {CALL_REGISTER_MENU_RECHARGE_STATUS, 1u, "--.--"},
    };
    for (unsigned i = 0u; i < sizeof(queries) / sizeof(queries[0]); i++) {
        unsigned before = s_writes;
        open_call_register_menu(&app, queries[i].kind, queries[i].row);
        render_call_register_menu(&app, NULL);
        check(strcmp(s_preview, queries[i].preview) == 0, "billing readings remain explicitly unknown");
        handle_call_register_menu_key(&app, KEY_NAVI, 20u);
        expect_not_done(&app, APP_ROUTE_CALL_REGISTER_MENU, "unsupported billing action uses Not done");
        check(s_writes == before, "unsupported billing action does not write");
    }
    open_call_register_menu(&app, CALL_REGISTER_MENU_PREPAID, 3u);
    handle_call_register_menu_key(&app, KEY_NAVI, 21u);
    check(app.call_register_menu_kind == CALL_REGISTER_MENU_RECHARGE_STATUS,
          "recharge status opens its original submenu");
    handle_call_register_menu_key(&app, KEY_C, 22u);
    check(app.call_register_menu_kind == CALL_REGISTER_MENU_PREPAID &&
              app.call_register_menu_selected == 3u, "Back restores the prepaid selection");
    for (uint8_t choice = 0u; choice < 2u; choice++) {
        open_call_register_menu(&app, CALL_REGISTER_MENU_PREPAID, 0u);
        handle_call_register_menu_key(&app, KEY_NAVI, 23u);
        check(app.call_register_menu_kind == CALL_REGISTER_MENU_CREDIT_INFO,
              "credit display preference keeps its On/Off picker");
        app.call_register_menu_selected = choice;
        handle_call_register_menu_key(&app, KEY_NAVI, 24u);
        check(s_sid == 0x3b4u && app.call_register_menu_kind == CALL_REGISTER_MENU_PREPAID &&
                  s_values[STORE_SETTING_CALL_CREDIT_INFO_DISPLAY] == (choice == 0u ? 1u : 0u),
              "credit display preference saves locally");
        render_call_register_menu(&app, NULL);
        check(strcmp(s_preview, choice == 0u ? "On" : "Off") == 0,
              "credit display preview retains the stored choice");
        handle_call_register_menu_key(&app, KEY_NAVI, 25u);
        check(app.call_register_menu_selected == choice, "credit picker reopens at the stored value");
        app.call_register_menu_selected ^= 1u;
        s_write_status = STORE_STATUS_INVALID_ARGUMENT;
        handle_call_register_menu_key(&app, KEY_NAVI, 26u);
        expect_not_done(&app, APP_ROUTE_CALL_REGISTER_MENU, "failed credit preference write is not acknowledged");
        check(app.call_register_menu_kind == CALL_REGISTER_MENU_CREDIT_INFO &&
                  s_values[STORE_SETTING_CALL_CREDIT_INFO_DISPLAY] == (choice == 0u ? 1u : 0u),
              "failed credit preference save keeps the picker and previous value");
        s_write_status = STORE_STATUS_OK;
    }
    app.call_register_metric_kind = CALL_REGISTER_METRIC_COSTS;
    for (uint8_t row = 0u; row < 3u; row++) {
        app.call_register_metric_selected = row;
        unsigned before = s_writes;
        render_call_register_metric(&app, NULL);
        check(row == 2u || strcmp(s_preview, "--") == 0, "unknown cost is not a sample or zero");
        handle_call_register_metric_key(&app, KEY_NAVI, 30u);
        expect_not_done(&app, APP_ROUTE_CALL_REGISTER_METRIC, "no simulated cost query or counter reset");
        check(s_writes == before, "unavailable counters are not reset");
    }
    s_counters[STORE_SETTING_CALL_DURATION_LAST] = 123u;
    open_call_register_menu(&app, CALL_REGISTER_MENU_ROOT, 4u);
    handle_call_register_menu_key(&app, KEY_NAVI, 40u);
    render_call_register_metric(&app, NULL);
    check(strcmp(s_preview, "0:02:03") == 0, "real call-duration accounting is preserved");
    app.call_register_metric_selected = 4u;
    handle_call_register_metric_key(&app, KEY_NAVI, 50u);
    check(s_sid == 0x3cau && s_counters[STORE_SETTING_CALL_DURATION_LAST] == 0u,
          "real call timers can still be cleared");
}

int main(void) {
    test_unavailable_settings();
    test_local_preferences();
    test_call_register_results();
    if (s_failures) return 1;
    puts("menu result tests passed");
    return 0;
}

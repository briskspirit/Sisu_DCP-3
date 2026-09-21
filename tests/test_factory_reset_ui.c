#include "apps/settings_app.h"
#include "apps/dialogs_app.h"
#include "services/input_keys.h"
#include "services/strings.h"
#include "storage/store_service.h"
#include "storage/store_operator_logo.h"
#include "ui/text_layout.h"

#include <assert.h>
#include <string.h>

#define RESET_UNITS ((1u << STORE_UNIT_SETTINGS_SYSTEM) | (1u << STORE_UNIT_SETTINGS_CALLS) | \
                     (1u << STORE_UNIT_SETTINGS_PROFILES) | (1u << STORE_UNIT_OPERATOR_LOGO))

static store_diag_snapshot_t diag;
static store_status_t read_status, write_status;
static char stored_code[STORE_TEXT_MAX + 1u];
static unsigned writes, profile_resets, notes;
static uint16_t note_sid;
static uint8_t note_record;
static uint8_t language_at_note;
static bool profiles_ok;
static unsigned logo_clears;
static store_status_t logo_status;

store_status_t store_operator_logo_clear(void) {
    logo_clears++;
    return logo_status;
}

uint32_t time_ms(void) { return 0u; }
int32_t time_diff_ms(uint32_t a, uint32_t b) { return (int32_t)(a - b); }
bool board_diag_headset_inserted(void) { return false; }
void store_service_get_diag(store_diag_snapshot_t *out) { *out = diag; }
store_status_t store_setting_get_text(store_setting_key_t key, char *out, uint8_t cap) {
    assert(key == STORE_SETTING_SECURITY_CODE);
    if (read_status == STORE_STATUS_OK) copy_text(out, cap, stored_code);
    return read_status;
}
store_status_t store_setting_get_u8(store_setting_key_t key, uint8_t *out) {
    (void)key; *out = 0u; return STORE_STATUS_OK;
}
store_status_t store_setting_set_u8(store_setting_key_t key, uint8_t value) {
    (void)key; (void)value;
    writes++;
    if (write_status == STORE_STATUS_OK) diag.dirty_mask |= RESET_UNITS;
    return write_status;
}
store_status_t store_setting_set_text(store_setting_key_t key, const char *value) {
    assert(key == STORE_SETTING_SYSTEM_WELCOME_NOTE && value[0] == '\0');
    writes++;
    if (write_status == STORE_STATUS_OK) diag.dirty_mask |= RESET_UNITS;
    return write_status;
}
bool profiles_restore_factory_defaults(void) { profile_resets++; return profiles_ok; }
void open_main_menu_at(app_t *app, uint8_t row, uint32_t now) {
    (void)row; (void)now; app->route = APP_ROUTE_MAIN_MENU;
}
void open_editor(app_t *app, const char *title, const char *value, uint8_t max_len,
                 editor_kind_t kind, editor_context_t context, bool search, uint32_t now) {
    (void)app; (void)title; (void)value; (void)max_len; (void)kind;
    (void)context; (void)search; (void)now;
    assert(!"reset must not use the unmasked generic editor");
}
void open_display_sid(app_t *app, uint8_t record, uint16_t sid, const char *fallback,
                      app_route_t destination, uint32_t now) {
    (void)fallback; (void)now;
    note_sid = sid; note_record = record; notes++;
    language_at_note = strings_get_language();
    app->route = APP_ROUTE_DISPLAY_MESSAGE;
    app->display_return_route = destination;
}
static void begin(app_t *app) {
    memset(app, 0, sizeof(*app));
    diag = (store_diag_snapshot_t){.ready = true};
    read_status = write_status = STORE_STATUS_OK;
    profiles_ok = true;
    logo_status = STORE_STATUS_OK;
    logo_clears = 0u;
    strcpy(stored_code, "54321");
    writes = profile_resets = notes = 0u;
    note_sid = 0u;
    strings_set_language(0x81u);
    open_settings_menu(app, SETTINGS_MENU_ROOT, 3u);
    handle_settings_menu_key(app, KEY_NAVI, 10u);
    assert(app->route == APP_ROUTE_SETTINGS_RESTORE && app->settings_restore_active);
    assert(writes == 0u && profile_resets == 0u && notes == 0u && logo_clears == 0u);
}
static void type_code(app_t *app, const char *text) {
    static const uint16_t keys[] = {KEY_0, KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9};
    for (; *text; text++) handle_settings_restore_key(app, keys[*text - '0'], EVENT_KEY_DOWN, 50u);
}
static void submit(app_t *app, uint32_t now) {
    handle_settings_restore_key(app, KEY_NAVI, EVENT_KEY_DOWN, now);
}
static void erased(const app_t *app) {
    static const char zero[11];
    assert(memcmp(app->settings_restore_code, zero, sizeof(zero)) == 0);
}
static void not_done(const app_t *app) {
    assert(note_sid == 0x210u && note_record == 32u &&
           app->display_return_route == APP_ROUTE_SETTINGS_MENU && !app->settings_restore_pending);
    erased(app);
}

int main(void) {
    static app_t app;
    begin(&app);
    submit(&app, 100u);
    assert(notes == 0u && writes == 0u);
    type_code(&app, "12345");
    submit(&app, 100u);
    assert(note_sid == 0x145u && app.display_return_route == APP_ROUTE_SETTINGS_RESTORE && writes == 0u && logo_clears == 0u);
    erased(&app);
    app.route = app.display_return_route;
    type_code(&app, "54321");
    framebuffer_t got, expected;
    for (uint8_t i = 0u; i < strings_language_count(); i++) {
        strings_set_language(strings_language_id_at(i));
#ifndef SISU_SYNTHETIC_ASSETS
        assert(ui_wrap_line_count(asset_font(FONT_FS2), ts(0x29au), FB_WIDTH) <= 2u);
#endif
        render_settings_restore(&app, &got);
        fb_clear(&expected, false);
        fb_bitmap(&expected, 14u, 0, 0, true, true);
        draw_text_block(&expected, asset_font(FONT_FS2), ts_or(0x29au, "Security\ncode:"), 0, 7, FB_WIDTH, 9, 2u);
        fb_text(&expected, asset_font(FONT_FS2), "*****", 0, 28, true, FB_WIDTH);
        draw_softkey(&expected, ts_or(0x2e9u, "OK"));
        assert(memcmp(got.data, expected.data, FB_SIZE) == 0);
    }
    strings_set_language(0x81u);
    submit(&app, 100u);
    erased(&app);
    assert(writes == 16u && profile_resets == 1u && logo_clears == 1u && app.settings_restore_pending);
    unsigned before_notes = notes, before_writes = writes;
    submit(&app, 110u);
    handle_settings_restore_key(&app, KEY_NAVI, EVENT_KEY_HOLD, 120u);
    handle_settings_restore_key(&app, KEY_C, EVENT_KEY_DOWN, 120u);
    assert(writes == before_writes && !tick_settings_restore(&app, 200u) && notes == before_notes);
    diag.dirty_mask = 1u << STORE_UNIT_SETTINGS_PROFILES;
    assert(!tick_settings_restore(&app, 300u));
    diag.dirty_mask = 1u << STORE_UNIT_OPERATOR_LOGO;
    assert(!tick_settings_restore(&app, 350u) && notes == before_notes);
    diag.dirty_mask = 1u << STORE_UNIT_CALLS_MISSED;
    assert(tick_settings_restore(&app, 400u));
    assert(note_sid == 0x1c3u && note_record == 3u && language_at_note == 0u &&
           app.settings_menu_selected == 3u && !app.settings_restore_pending);
    assert(!tick_settings_restore(&app, 500u));

    begin(&app);
    type_code(&app, "123456789012");
    assert(strlen(app.settings_restore_code) == 10u);
    handle_settings_restore_key(&app, KEY_C, EVENT_KEY_DOWN, 80u);
    assert(strcmp(app.settings_restore_code, "123456789") == 0);
    handle_settings_restore_key(&app, KEY_0, EVENT_KEY_HOLD, 90u);
    assert(strcmp(app.settings_restore_code, "123456789") == 0);
    handle_settings_restore_key(&app, KEY_C, EVENT_KEY_HOLD, 100u);
    erased(&app);
    handle_settings_restore_key(&app, KEY_C, EVENT_KEY_DOWN, 110u);
    submit(&app, 120u);
    assert(app.route == APP_ROUTE_SETTINGS_MENU && writes == 0u && logo_clears == 0u && !app.settings_restore_active);

    for (unsigned fault = 0u; fault < 9u; fault++) {
        begin(&app);
        type_code(&app, "54321");
        switch (fault) {
        case 0u: read_status = STORE_STATUS_NOT_READY; break;
        case 1u: diag.ready = false; break;
        case 2u: diag.unavailable_mask = 1u << STORE_UNIT_SETTINGS_SYSTEM; break;
        case 3u: diag.degraded_mask = 1u << STORE_UNIT_SETTINGS_PROFILES; break;
        case 4u: diag.boot_faults = STORE_BOOT_FAULT_RECORD; break;
        case 5u: stored_code[0] = '\0'; break;
        case 6u: diag.unavailable_mask = 1u << STORE_UNIT_OPERATOR_LOGO; break;
        case 7u: diag.degraded_mask = 1u << STORE_UNIT_OPERATOR_LOGO; break;
        default: strcpy(stored_code, "123x5"); break;
        }
        submit(&app, 100u);
        not_done(&app);
        assert(writes == 0u && profile_resets == 0u && logo_clears == 0u);
    }
    begin(&app);
    write_status = STORE_STATUS_NOT_READY;
    type_code(&app, "54321"); submit(&app, 100u);
    not_done(&app); assert(profile_resets == 0u);
    begin(&app);
    profiles_ok = false;
    type_code(&app, "54321"); submit(&app, 100u);
    not_done(&app);
    begin(&app);
    logo_status = STORE_STATUS_STORAGE_ERROR;
    type_code(&app, "54321"); submit(&app, 100u);
    not_done(&app);
    for (unsigned fault = 0u; fault < 6u; fault++) {
        begin(&app);
        type_code(&app, "54321"); submit(&app, 100u);
        switch (fault) {
        case 0u: diag.degraded_mask = 1u << STORE_UNIT_SETTINGS_CALLS; break;
        case 1u: diag.unavailable_mask = 1u << STORE_UNIT_SETTINGS_SYSTEM; break;
        case 2u: diag.ready = false; break;
        case 3u: diag.degraded_mask = 1u << STORE_UNIT_OPERATOR_LOGO; break;
        case 4u: diag.unavailable_mask = 1u << STORE_UNIT_OPERATOR_LOGO; break;
        default: diag.boot_faults = STORE_BOOT_FAULT_RECORD; break;
        }
        assert(tick_settings_restore(&app, 200u)); not_done(&app);
    }
    begin(&app);
    type_code(&app, "54321"); submit(&app, UINT32_MAX - 100u);
    assert(!tick_settings_restore(&app, 29898u));
    assert(tick_settings_restore(&app, 29900u)); not_done(&app);
    diag.dirty_mask = 0u;
    before_notes = notes;
    assert(!tick_settings_restore(&app, 40000u) && notes == before_notes);

    for (uint8_t record = 14u; record <= 16u; record++) {
        begin(&app);
        type_code(&app, "54321"); submit(&app, 100u);
        app.route = APP_ROUTE_DISPLAY_MESSAGE;
        app.display_record_id = record;
        app.display_return_route = APP_ROUTE_SETTINGS_RESTORE;
        diag.dirty_mask = 0u;
        assert(!tick_settings_restore(&app, 200u));
        assert(notes == 0u && app.display_record_id == record && app.settings_restore_pending);
        app.route = app.display_return_route;
        assert(tick_settings_restore(&app, 300u));
        assert(note_sid == 0x1c3u && !app.settings_restore_pending);
    }

    const app_route_t preempt[] = {APP_ROUTE_POWER_OFF, APP_ROUTE_INCOMING_CALL, APP_ROUTE_CLOCK_ALARM};
    for (unsigned i = 0u; i < sizeof(preempt) / sizeof(preempt[0]); i++) {
        begin(&app);
        type_code(&app, "54321");
        app.route = preempt[i];
        tick_settings_restore(&app, 100u);
        erased(&app); submit(&app, 110u);
        assert(writes == 0u && notes == 0u);
        begin(&app);
        type_code(&app, "54321"); submit(&app, 100u);
        app.route = preempt[i];
        tick_settings_restore(&app, 200u);
        diag.dirty_mask = 0u;
        tick_settings_restore(&app, 300u);
        assert(notes == 0u && app.route == preempt[i] && !app.settings_restore_pending);
        erased(&app);
    }
    return 0;
}

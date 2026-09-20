#include "apps/operator_logo_app.h"
#include "apps/dialogs_app.h"
#include "services/input_keys.h"
#include "services/strings.h"
#include "storage/store_operator_logo.h"

#include <assert.h>
#include <string.h>

static operator_logo_t saved;
static uint32_t pending = 7u;
static store_status_t admission = STORE_STATUS_OK, commit_status = STORE_STATUS_OK;
static unsigned saves, discards, notes;
static uint16_t note_sid;
static uint8_t note_record;
static app_route_t note_return;

uint32_t time_ms(void) { return 0u; }

uint32_t store_operator_logo_pending_first(void) { return pending; }
store_status_t store_operator_logo_pending_get(uint32_t id, operator_logo_t *out) {
    assert(id == 7u);
    *out = saved;
    return STORE_STATUS_OK;
}
store_status_t store_operator_logo_get(operator_logo_t *out) {
    *out = saved;
    return STORE_STATUS_OK;
}
store_status_t store_operator_logo_commit_status(void) { return commit_status; }
store_status_t store_operator_logo_pending_save(uint32_t id) {
    assert(id == 7u); saves++; return admission;
}
store_status_t store_operator_logo_pending_discard(uint32_t id) {
    assert(id == 7u); discards++; return admission;
}
void open_display_sid(app_t *app, uint8_t record, uint16_t sid, const char *fallback,
                      app_route_t return_route, uint32_t now) {
    (void)fallback; (void)now;
    notes++; note_sid = sid; note_record = record; note_return = return_route;
    app->route = APP_ROUTE_DISPLAY_MESSAGE;
}

int main(void) {
    app_t app = {0};
    assert(operator_logo_network_encode("311", "480", saved.network));
    memset(saved.pixels, 0xff, sizeof(saved.pixels));
    open_received_operator_logo(&app);
    assert(app.route == APP_ROUTE_OPERATOR_LOGO && app.operator_logo_receive_id == 7u);
    handle_operator_logo_key(&app, KEY_UP, 0u);
    assert(app.operator_logo_option == 2u);
    handle_operator_logo_key(&app, KEY_DOWN, 0u);
    assert(app.operator_logo_option == 0u);
    handle_operator_logo_key(&app, KEY_NAVI, 0u);
    assert(app.operator_logo_preview);
    framebuffer_t got;
    memset(&got, 0xa5, sizeof(got));
    for (uint8_t i = 0u; i < strings_language_count(); i++) {
        strings_set_language(strings_language_id_at(i));
        framebuffer_t want = {0};
        /* Reuse the LCD framebuffer across preview/list transitions. */
        render_operator_logo(&app, &got);
        fb_fill_rect(&want, 6, 7, 72, 14, true);
        draw_softkey(&want, ts(0x2d8u));
        assert(memcmp(got.data, want.data, FB_SIZE) == 0);
        handle_operator_logo_key(&app, KEY_C, 0u);
        assert(!app.operator_logo_preview && discards == 0u);
        fb_clear(&want, false);
        render_operator_logo(&app, &got);
        const char *labels[] = {ts(0x21eu), ts(0x21cu), ts(0x21au)};
        draw_flat_list(&want, labels, 3u, 0u, 0, "Select");
        assert(memcmp(got.data, want.data, FB_SIZE) == 0);
        handle_operator_logo_key(&app, KEY_NAVI, 0u);
    }
    handle_operator_logo_key(&app, KEY_NAVI, 0u);
    handle_operator_logo_key(&app, KEY_DOWN, 0u);
    admission = STORE_STATUS_STORAGE_ERROR;
    handle_operator_logo_key(&app, KEY_NAVI, 0u);
    assert(note_sid == 0x3b3u && note_record == 0u &&
           note_return == APP_ROUTE_OPERATOR_LOGO && app.operator_logo_save_action == 0u);
    app.route = APP_ROUTE_OPERATOR_LOGO;
    admission = STORE_STATUS_OK;
    commit_status = STORE_STATUS_NOT_READY;
    handle_operator_logo_key(&app, KEY_NAVI, 0u);
    assert(app.operator_logo_save_action == 1u);
    unsigned old_notes = notes, old_saves = saves;
    handle_operator_logo_key(&app, KEY_NAVI, 0u);
    assert(saves == old_saves && !tick_operator_logo(&app, 0u) && notes == old_notes);
    commit_status = STORE_STATUS_STORAGE_ERROR;
    assert(tick_operator_logo(&app, 0u) && note_sid == 0x3b3u);
    app.route = APP_ROUTE_OPERATOR_LOGO;
    handle_operator_logo_key(&app, KEY_NAVI, 0u);
    commit_status = STORE_STATUS_OK;
    assert(tick_operator_logo(&app, 0u) && note_sid == 0x21du && note_record == 3u &&
           note_return == APP_ROUTE_STANDBY);
    open_received_operator_logo(&app);
    handle_operator_logo_key(&app, KEY_C, 0u);
    assert(discards == 1u && app.operator_logo_save_action == 2u);
    pending = 0u;
    assert(tick_operator_logo(&app, 0u) && app.route == APP_ROUTE_STANDBY);
    open_received_operator_logo(&app);
    assert(app.route == APP_ROUTE_STANDBY);

    modem_status_t status = {.at_ready = true, .network_registered = true};
    status.signal.valid_fields = MODEM_SIGNAL_VALID_PLMN;
    strcpy(status.signal.mcc, "311"); strcpy(status.signal.mnc, "480");
    assert(operator_logo_update_network(&app, &status) && app.operator_logo_visible);
    assert(!operator_logo_update_network(&app, &status));
    strcpy(status.signal.mnc, "48");
    assert(operator_logo_update_network(&app, &status) && !app.operator_logo_visible);
    strcpy(status.signal.mnc, "480");
    assert(operator_logo_update_network(&app, &status) && app.operator_logo_visible);
    status.network_registered = false;
    assert(operator_logo_update_network(&app, &status) && !app.operator_logo_visible);
    status.network_registered = true; app.sim_missing = true;
    assert(!operator_logo_update_network(&app, &status) && !app.operator_logo_visible);
    app.sim_missing = false;
    assert(operator_logo_update_network(&app, &status));
    status.signal.valid_fields = 0u;
    assert(operator_logo_update_network(&app, &status) && !app.operator_logo_visible);
    status.signal.valid_fields = MODEM_SIGNAL_VALID_PLMN;
    assert(operator_logo_update_network(&app, &status));
    status.at_ready = false;
    assert(operator_logo_update_network(&app, &status) && !app.operator_logo_visible);
    return 0;
}

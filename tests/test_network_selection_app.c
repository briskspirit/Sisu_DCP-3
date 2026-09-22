#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "apps/network_selection_app.h"
#include "apps/dialogs_app.h"
#include "services/modem_service.h"
#include "services/input_keys.h"
#include "services/strings.h"

static modem_network_snapshot_t status;
static uint32_t next_id, cancelled;
static uint16_t last_sid;
static bool admitted = true;
static char last_text[128];
uint32_t time_ms(void) { return 0u; }
void modem_service_get_network_snapshot(modem_network_snapshot_t *out) { *out = status; }
bool modem_service_get_network_entry(uint32_t gen, uint8_t index, modem_network_entry_t *out) {
    if (status.busy || gen != status.list_generation || index >= status.count) return false;
    *out = (modem_network_entry_t){.plmn = "310410", .name = "AT&T", .availability = 2};
    return true;
}
uint32_t modem_service_network_request(modem_network_action_t action, uint32_t gen, uint8_t index) {
    (void)gen; (void)index;
    if (!admitted) return 0;
    status.request_id = ++next_id; status.busy = true;
    status.action = action; status.result = MODEM_NETWORK_PENDING;
    return next_id;
}
void modem_service_network_cancel(uint32_t id) { if (id != 0u) cancelled = id; }
void open_display(app_t *app, uint8_t record, const char *a, const char *b, const char *c,
                  app_route_t back, uint32_t now) {
    (void)b; (void)c;
    app->route = APP_ROUTE_DISPLAY_MESSAGE; app->display_record_id = record;
    app->display_generation++; app->display_opened_ms = now; app->display_return_route = back;
    snprintf(last_text, sizeof(last_text), "%s", a);
}
void open_display_sid(app_t *app, uint8_t record, uint16_t sid, const char *fallback,
                      app_route_t back, uint32_t now) {
    last_sid = sid; open_display(app, record, ts_or(sid, fallback), "", "", back, now);
}
static void complete(modem_network_result_t result) { status.busy = false; status.result = result; }

int main(void) {
    app_t app = {0};
    status = (modem_network_snapshot_t){.ready = true, .list_generation = 2, .count = 4};
    open_network_selection(&app, true, 10);
    assert(status.action == MODEM_NETWORK_SCAN && last_sid == 0x299u && app.display_record_id == 0x25u);
    assert(!tick_network_selection(&app, 11));
    uint32_t first = app.network_request_id;
    assert(handle_network_selection_display_key(&app, KEY_NAVI, 12));
    assert(cancelled == first && app.route == APP_ROUTE_SETTINGS_VALUE);
    complete(MODEM_NETWORK_DONE);
    assert(!tick_network_selection(&app, 13) && app.route == APP_ROUTE_SETTINGS_VALUE);

    open_network_selection(&app, true, 20); complete(MODEM_NETWORK_DONE);
    assert(tick_network_selection(&app, 21) && app.route == APP_ROUTE_NETWORK_LIST);
    assert(app.network_list_generation == 2 && app.network_selected == 0u);
    handle_network_selection_key(&app, KEY_UP, 22);
    assert(app.network_selected == 3u && app.network_view_start == 3u);
    framebuffer_t fb;
    render_network_selection(&app, &fb);
    handle_network_selection_key(&app, KEY_DOWN, 23);
    handle_network_selection_key(&app, KEY_NAVI, 24);
    assert(status.action == MODEM_NETWORK_SELECT_MANUAL && last_sid == 0x23eu);
    complete(MODEM_NETWORK_DONE); assert(tick_network_selection(&app, 25));
    assert(app.display_record_id == 3u && strstr(last_text, "AT&T"));

    open_network_selection(&app, true, 26); complete(MODEM_NETWORK_DONE);
    assert(tick_network_selection(&app, 27));
    handle_network_selection_key(&app, KEY_NAVI, 28);
    status.home = true;
    complete(MODEM_NETWORK_DONE); assert(tick_network_selection(&app, 29));
    assert(app.display_record_id == 0x1au && last_sid == 0x18au);
    assert(app.display_return_route == APP_ROUTE_SETTINGS_VALUE);
    status.home = false;

    open_network_selection(&app, false, 30); complete(MODEM_NETWORK_DONE);
    assert(tick_network_selection(&app, 31) && app.route == APP_ROUTE_SETTINGS_MENU);
    open_network_selection(&app, true, 40); status.count = 0; complete(MODEM_NETWORK_DONE);
    assert(tick_network_selection(&app, 41) && last_sid == 0x207u);
    open_network_selection(&app, false, 50); complete(MODEM_NETWORK_NO_ACCESS);
    assert(tick_network_selection(&app, 51) && last_sid == 0x1ddu);
    open_network_selection(&app, false, 60); complete(MODEM_NETWORK_NOT_DONE);
    assert(tick_network_selection(&app, 61) && last_sid == 0x210u);

    app_route_t preempt[] = {APP_ROUTE_POWER_OFF, APP_ROUTE_CALL, APP_ROUTE_STANDBY,
                            APP_ROUTE_CONTACT_SERVICE};
    for (unsigned i = 0; i < sizeof(preempt) / sizeof(preempt[0]); i++) {
        open_network_selection(&app, true, 70); first = app.network_request_id;
        app.route = preempt[i];
        assert(!tick_network_selection(&app, 71) && cancelled == first && app.route == preempt[i]);
        complete(MODEM_NETWORK_DONE); assert(!tick_network_selection(&app, 72));
    }
    open_network_selection(&app, true, 80); first = app.network_request_id;
    open_display_sid(&app, 0x25, 0x23e, "Requesting", APP_ROUTE_SETTINGS_VALUE, 80);
    assert(!tick_network_selection(&app, 81) && cancelled == first);
    open_network_selection(&app, true, 90); app.keyguard_locked = true;
    assert(!tick_network_selection(&app, 91)); app.keyguard_locked = false;
    admitted = false; open_network_selection(&app, true, 100);
    assert(last_sid == 0x210u && app.network_request_id == 0u);
    admitted = true; status.ready = false;
    open_network_selection(&app, true, 110); assert(status.action == MODEM_NETWORK_REFRESH);
    complete(MODEM_NETWORK_DONE); status.ready = true;
    assert(tick_network_selection(&app, 111) && status.action == MODEM_NETWORK_SCAN);
    status.count = 2u; complete(MODEM_NETWORK_DONE);
    assert(tick_network_selection(&app, 112) && app.route == APP_ROUTE_NETWORK_LIST);
    status.list_generation++;
    assert(tick_network_selection(&app, 113) && last_sid == 0x210u);
    app.route = APP_ROUTE_NETWORK_LIST;
    handle_network_selection_key(&app, KEY_NAVI, 114);
    assert(last_sid == 0x210u && app.network_request_id == 0u);
    puts("network selection UI tests passed"); return 0;
}

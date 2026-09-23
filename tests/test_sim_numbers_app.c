#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "apps/sim_numbers_app.h"
#include "apps/dialogs_app.h"
#include "apps/calls_app.h"
#include "services/modem_service.h"
#include "services/input_keys.h"

static sim_numbers_snapshot_t status;
static sim_number_t entries[SIM_NUMBERS_MAX];
static bool admitted = true, home = true;
static uint32_t cancelled;
static uint16_t last_sid;
static unsigned calls;
static char dialled[33];
uint32_t time_ms(void) { return 0u; }
void modem_service_sim_numbers_snapshot(sim_numbers_snapshot_t *out) { *out = status; }
bool modem_service_sim_number(uint32_t id, uint8_t index, sim_number_t *out) {
    if (id != status.request_id || status.busy || status.result != SIM_NUMBERS_DONE || index >= status.count) return false;
    *out = entries[index]; return true;
}
bool modem_service_sim_numbers_home(void) { return home; }
bool modem_service_sim_numbers_available(sim_numbers_kind_t kind) { (void)kind; return false; }
uint32_t modem_service_sim_numbers_request(sim_numbers_kind_t kind) {
    (void)kind;
    if (!admitted) return 0u;
    status.request_id++; status.busy = true; status.result = SIM_NUMBERS_PENDING;
    return status.request_id;
}
void modem_service_sim_numbers_cancel(uint32_t id) { if (id) cancelled = id; }
void open_display_sid(app_t *app, uint8_t record, uint16_t sid, const char *fallback,
                      app_route_t back, uint32_t now) {
    (void)fallback; (void)now;
    last_sid = sid; app->route = APP_ROUTE_DISPLAY_MESSAGE; app->display_record_id = record;
    app->display_generation++; app->display_return_route = back;
}
bool start_outgoing_call(app_t *app, const char *number, const char *name, uint32_t now, app_route_t back) {
    (void)name; (void)now;
    assert(back == APP_ROUTE_SIM_NUMBERS);
    snprintf(dialled, sizeof(dialled), "%s", number);
    calls++; app->route = APP_ROUTE_CALL; return true;
}
static void complete(sim_numbers_result_t result) { status.busy = false; status.result = result; }
int main(void) {
    app_t app = {0}; framebuffer_t fb;
    entries[0] = (sim_number_t){.name = "Travel", .folder = true, .parent = SIM_NUMBERS_ROOT};
    entries[1] = (sim_number_t){.name = "Taxi", .number = "123", .parent = 0, .premium = true, .home_only = true};
    entries[2] = (sim_number_t){.name = "Care", .number = "611", .parent = SIM_NUMBERS_ROOT};
    status.count = 3;
    open_sim_numbers(&app, SIM_NUMBERS_INFO, 10);
    assert(last_sid == 0x33du && app.display_record_id == 4u);
    uint32_t id = app.sim_numbers_request;
    assert(!tick_sim_numbers(&app, 11));
    handle_sim_numbers_display_key(&app, KEY_C, 12);
    assert(cancelled == id && app.route == APP_ROUTE_PHONEBOOK_MENU);
    complete(SIM_NUMBERS_DONE); assert(!tick_sim_numbers(&app, 13));

    open_sim_numbers(&app, SIM_NUMBERS_INFO, 20); complete(SIM_NUMBERS_DONE);
    assert(tick_sim_numbers(&app, 21) && app.route == APP_ROUTE_SIM_NUMBERS && app.sim_numbers_selected == 0);
    render_sim_numbers(&app, &fb);
    handle_sim_numbers_key(&app, KEY_DOWN, 22); assert(app.sim_numbers_selected == 2);
    handle_sim_numbers_key(&app, KEY_UP, 23); assert(app.sim_numbers_selected == 0);
    handle_sim_numbers_key(&app, KEY_NAVI, 24); assert(app.sim_numbers_selected == 1 && app.sim_numbers_parent == 0);
    render_sim_numbers(&app, &fb);
    home = false; handle_sim_numbers_key(&app, KEY_NAVI, 25);
    assert(calls == 0 && last_sid == 0x2aeu); /* Check eligibility again at dispatch. */
    home = true; app.route = APP_ROUTE_SIM_NUMBERS;
    handle_sim_numbers_key(&app, KEY_NAVI, 26); assert(calls == 1 && !strcmp(dialled, "123"));
    app.route = APP_ROUTE_SIM_NUMBERS;
    handle_sim_numbers_key(&app, KEY_C, 27); assert(app.sim_numbers_parent == SIM_NUMBERS_ROOT && app.sim_numbers_selected == 0);
    home = false; handle_sim_numbers_key(&app, KEY_NAVI, 28); assert(app.sim_numbers_parent == SIM_NUMBERS_ROOT);
    handle_sim_numbers_key(&app, KEY_DOWN, 29); assert(app.sim_numbers_selected == 2);
    entries[2].number[0] = '\0'; handle_sim_numbers_key(&app, KEY_NAVI, 30);
    assert(last_sid == 0x208u && app.display_return_route == APP_ROUTE_SIM_NUMBERS);
    app.route = APP_ROUTE_SIM_NUMBERS;
    status.result = SIM_NUMBERS_UNAVAILABLE; assert(tick_sim_numbers(&app, 31) && last_sid == 0x2aeu);

    app_route_t interrupts[] = {APP_ROUTE_CALL, APP_ROUTE_POWER_OFF, APP_ROUTE_CONTACT_SERVICE, APP_ROUTE_STANDBY};
    for (unsigned i = 0; i < sizeof(interrupts) / sizeof(interrupts[0]); i++) {
        open_sim_numbers(&app, SIM_NUMBERS_INFO, 40); id = app.sim_numbers_request;
        app.route = interrupts[i]; assert(!tick_sim_numbers(&app, 41));
        assert(cancelled == id && app.route == interrupts[i]);
        complete(SIM_NUMBERS_DONE); assert(!tick_sim_numbers(&app, 42));
    }
    open_sim_numbers(&app, SIM_NUMBERS_INFO, 50); id = app.sim_numbers_request;
    app.display_generation++; assert(!tick_sim_numbers(&app, 51) && cancelled == id);
    open_sim_numbers(&app, SIM_NUMBERS_INFO, 60); app.keyguard_locked = true;
    assert(!tick_sim_numbers(&app, 61)); app.keyguard_locked = false;
    open_sim_numbers(&app, SIM_NUMBERS_INFO, 70); complete(SIM_NUMBERS_UNAVAILABLE);
    assert(tick_sim_numbers(&app, 71) && last_sid == 0x20du);
    open_sim_numbers(&app, SIM_NUMBERS_INFO, 80); status.count = 0; complete(SIM_NUMBERS_DONE);
    assert(tick_sim_numbers(&app, 81) && app.route == APP_ROUTE_PHONEBOOK_MENU);
    status.count = SIM_NUMBERS_MAX;
    for (unsigned i = 0; i < SIM_NUMBERS_MAX; i++)
        entries[i] = (sim_number_t){.name = "Care", .number = "611", .parent = SIM_NUMBERS_ROOT};
    open_sim_numbers(&app, SIM_NUMBERS_SERVICE, 82); complete(SIM_NUMBERS_DONE);
    assert(tick_sim_numbers(&app, 83) && app.sim_numbers_selected == 0);
    handle_sim_numbers_key(&app, KEY_UP, 84); assert(app.sim_numbers_selected == SIM_NUMBERS_MAX - 1u);
    render_sim_numbers(&app, &fb);
    handle_sim_numbers_key(&app, KEY_DOWN, 85); assert(app.sim_numbers_selected == 0);
    open_sim_numbers(&app, SIM_NUMBERS_SERVICE, 86);
    status.request_id++; complete(SIM_NUMBERS_DONE);
    assert(tick_sim_numbers(&app, 87) && last_sid == 0x2aeu && app.route == APP_ROUTE_DISPLAY_MESSAGE);
    admitted = false; open_sim_numbers(&app, SIM_NUMBERS_SERVICE, 90); assert(last_sid == 0x20du);
    puts("SIM numbers UI: OK");
    return 0;
}

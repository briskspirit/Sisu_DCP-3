#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "apps/call_waiting_app.h"
#include "apps/dialogs_app.h"
#include "services/input_keys.h"
#include "services/modem_service.h"

static uint32_t next_id, cancelled;
static bool accept = true, pending;
static call_waiting_result_t result;
static uint16_t sid;

bool modem_service_request_call_waiting(call_waiting_action_t action, uint32_t *id) {
    if (!accept) { *id = 0u; return false; }
    *id = ++next_id;
    result = (call_waiting_result_t){.request_id = *id, .action = action};
    return true;
}
bool modem_service_cancel_queued_call_waiting(uint32_t id) { cancelled = id; return false; }
bool modem_service_pop_call_waiting_result(call_waiting_result_t *out) {
    if (!pending) return false;
    *out = result; pending = false; return true;
}
void open_display_sid(app_t *app, uint8_t record, uint16_t text_sid,
                      const char *fallback, app_route_t target, uint32_t now) {
    (void)fallback; (void)now;
    sid = text_sid;
    app->call_waiting_display_request_id = 0u;
    app->route = APP_ROUTE_DISPLAY_MESSAGE;
    app->display_record_id = record;
    app->display_return_route = target;
}

int main(void) {
    app_t app = {0};
    static const uint16_t success_sids[] = {0xa2u, 0xa4u, 0xa6u, 0xa3u};
    for (unsigned i = 0u; i < 4u; i++) {
        open_call_waiting_request(&app, i < 2u ? (call_waiting_action_t)(i + 1u) : CALL_WAITING_QUERY, 10u);
        assert(sid == 0x2a4u && app.display_record_id == 0x25u);
        assert(!tick_call_waiting(&app, 11u));
        result.outcome = CALL_WAITING_SUCCESS;
        result.status_known = i >= 2u; result.active = i == 3u; pending = true;
        assert(tick_call_waiting(&app, 20u));
        assert(sid == success_sids[i] && app.display_record_id == 3u);
        assert(app.display_return_route == APP_ROUTE_SETTINGS_VALUE && app.call_waiting_request_id == 0u);
    }
    static const call_waiting_outcome_t errors[] = {
        CALL_WAITING_NO_NETWORK, CALL_WAITING_NOT_DONE, CALL_WAITING_UNKNOWN, CALL_WAITING_CANCELLED,
    };
    static const uint16_t error_sids[] = {0x209u, 0x210u, 0x263u, 0x3e2u};
    for (unsigned i = 0u; i < 4u; i++) {
        open_call_waiting_request(&app, CALL_WAITING_QUERY, 30u);
        result.outcome = errors[i]; pending = true;
        assert(tick_call_waiting(&app, 31u) && sid == error_sids[i]);
        assert(app.display_record_id == (i == 3u ? 0x0cu : 0x20u));
    }
    open_call_waiting_request(&app, CALL_WAITING_ACTIVATE, 40u);
    uint32_t id = app.call_waiting_request_id;
    assert(handle_call_waiting_display_key(&app, KEY_NAVI, 41u));
    assert(cancelled == id && sid == 0x3e2u && app.display_return_route == APP_ROUTE_STANDBY);
    pending = true;
    assert(!tick_call_waiting(&app, 42u) && !pending && sid == 0x3e2u);

    open_call_waiting_request(&app, CALL_WAITING_QUERY, 50u);
    result.status_known = true; result.active = true; pending = true;
    app.route = APP_ROUTE_CALL;
    assert(!tick_call_waiting(&app, 51u) && pending);
    app.route = APP_ROUTE_STANDBY; app.keyguard_locked = true;
    assert(!tick_call_waiting(&app, 52u) && pending);
    app.keyguard_locked = false;
    assert(tick_call_waiting(&app, 53u) && sid == 0xa3u && app.display_return_route == APP_ROUTE_STANDBY);

    open_call_waiting_request(&app, CALL_WAITING_QUERY, 60u);
    result.status_known = true; pending = true;
    open_display_sid(&app, 0x25u, 0u, "Other progress", APP_ROUTE_STANDBY, 61u);
    assert(!handle_call_waiting_display_key(&app, KEY_C, 62u));
    assert(!tick_call_waiting(&app, 63u) && pending);
    app.route = APP_ROUTE_POWER_OFF;
    assert(!tick_call_waiting(&app, 64u) && !pending && app.call_waiting_request_id == 0u);
    accept = false;
    open_call_waiting_request(&app, CALL_WAITING_QUERY, 70u);
    assert(sid == 0x210u && app.call_waiting_request_id == 0u);
    puts("test_call_waiting_app: OK");
    return 0;
}

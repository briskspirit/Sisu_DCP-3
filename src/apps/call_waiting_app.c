#include "apps/call_waiting_app.h"

#include "apps/dialogs_app.h"
#include "services/input_keys.h"
#include "services/modem_service.h"

static bool owns_progress(const app_t *app) {
    return app->route == APP_ROUTE_DISPLAY_MESSAGE && app->display_record_id == 0x25u &&
           app->call_waiting_request_id != 0u &&
           app->call_waiting_display_request_id == app->call_waiting_request_id;
}

static void show_result(app_t *app, const call_waiting_result_t *result, uint32_t now) {
    app_route_t target = app->route == APP_ROUTE_STANDBY
        ? APP_ROUTE_STANDBY : APP_ROUTE_SETTINGS_VALUE;
    app->call_waiting_request_id = 0u;
    app->call_waiting_display_request_id = 0u;
    uint8_t record = 0x20u;
    uint16_t sid = 0x210u;
    const char *fallback = "Not done";
    switch (result->outcome) {
    case CALL_WAITING_SUCCESS:
        /* v6.00 0x25da44/0x25d9f0/0x25defe all use success record 3. */
        record = 3u;
        if (result->action == CALL_WAITING_ACTIVATE) {
            sid = 0xa2u; fallback = "Call\nwaiting\nactivated";
        } else if (result->action == CALL_WAITING_CANCEL) {
            sid = 0xa4u; fallback = "Call\nwaiting\ncancelled";
        } else if (result->status_known) {
            sid = result->active ? 0xa3u : 0xa6u;
            fallback = result->active ? "Call\nwaiting\nactive" : "Call\nwaiting\nnot active";
        } else {
            record = 0x20u; sid = 0x263u; fallback = "Result\nunknown";
        }
        break;
    case CALL_WAITING_NO_NETWORK:
        sid = 0x209u; fallback = "No network\ncoverage"; break;
    case CALL_WAITING_UNKNOWN:
        sid = 0x263u; fallback = "Result\nunknown"; break;
    case CALL_WAITING_CANCELLED:
        record = 0x0cu; sid = 0x3e2u; fallback = "Request\nnot\nconfirmed";
        target = APP_ROUTE_STANDBY;
        break;
    case CALL_WAITING_NOT_DONE: break;
    }
    open_display_sid(app, record, sid, fallback, target, now);
}

void open_call_waiting_request(app_t *app, call_waiting_action_t action, uint32_t now) {
    if (app->call_waiting_request_id != 0u)
        (void)modem_service_cancel_queued_call_waiting(app->call_waiting_request_id);
    app->call_waiting_request_id = 0u;
    if (!modem_service_request_call_waiting(action, &app->call_waiting_request_id)) {
        call_waiting_result_t result = {.outcome = CALL_WAITING_NOT_DONE};
        show_result(app, &result, now);
        return;
    }
    open_display_sid(app, 0x25u, 0x2a4u, "Requesting", APP_ROUTE_SETTINGS_VALUE, now);
    app->call_waiting_display_request_id = app->call_waiting_request_id;
}

bool handle_call_waiting_display_key(app_t *app, uint16_t key, uint32_t now) {
    if (!owns_progress(app) || (key != KEY_C && key != KEY_NAVI)) return false;
    /* A sent CCWA cannot be aborted. Quit detaches its UI; queued work can be removed. */
    (void)modem_service_cancel_queued_call_waiting(app->call_waiting_request_id);
    call_waiting_result_t result = {.outcome = CALL_WAITING_CANCELLED};
    show_result(app, &result, now);
    return true;
}

bool tick_call_waiting(app_t *app, uint32_t now) {
    if (app->route == APP_ROUTE_POWER_OFF) {
        app->call_waiting_request_id = 0u;
        app->call_waiting_display_request_id = 0u;
    }
    if (app->call_waiting_request_id != 0u &&
        (app->keyguard_locked || (!owns_progress(app) && app->route != APP_ROUTE_STANDBY)))
        return false;
    call_waiting_result_t result;
    bool changed = false;
    while (modem_service_pop_call_waiting_result(&result)) {
        if (app->call_waiting_request_id == 0u || result.request_id != app->call_waiting_request_id)
            continue;
        show_result(app, &result, now);
        changed = true;
    }
    return changed;
}

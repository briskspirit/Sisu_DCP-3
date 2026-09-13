#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "apps/call_divert_app.h"
#include "apps/dialogs_app.h"
#include "apps/main_menu_app.h"
#include "hal/keypad.h"
#include "services/call_forward_mmi.h"
#include "services/modem_service.h"
#include "services/strings.h"
#include "storage/store_service.h"
#include "ui/ui.h"

static int s_failures;
static bool s_request_accept = true;
static uint32_t s_next_request_id = 40u;
static call_forward_request_t s_last_request;
static uint32_t s_request_count;
static uint32_t s_cancel_count;
static uint32_t s_last_cancel_id;
static bool s_cancel_queued = true;
static call_forward_result_t s_result;
static bool s_result_pending;
static uint16_t s_last_sid;
static unsigned s_last_num;
static char s_last_a[64];
static char s_last_b[64];
static char s_local_mailbox[MODEM_PHONE_MAX + 1u];
static char s_sim_mailbox[MODEM_PHONE_MAX + 1u];
static store_call_divert_state_t s_stored_divert;
static bool s_stored_divert_valid;
static store_call_divert_state_t s_saved_divert;

static void check(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        s_failures++;
    }
}

static void reset_fixture(app_t *app) {
    memset(app, 0, sizeof(*app));
    memset(&s_last_request, 0, sizeof(s_last_request));
    memset(&s_result, 0, sizeof(s_result));
    memset(&s_saved_divert, 0, sizeof(s_saved_divert));
    memset(&s_stored_divert, 0, sizeof(s_stored_divert));
    s_request_accept = true;
    s_request_count = 0u;
    s_cancel_count = 0u;
    s_last_cancel_id = 0u;
    s_cancel_queued = true;
    s_result_pending = false;
    s_last_sid = 0u;
    s_last_num = 0u;
    s_last_a[0] = '\0';
    s_last_b[0] = '\0';
    s_local_mailbox[0] = '\0';
    s_sim_mailbox[0] = '\0';
    s_stored_divert_valid = false;
    app->route = APP_ROUTE_STANDBY;
}

static void queue_result(uint32_t request_id,
                         const call_forward_request_t *request,
                         call_forward_outcome_t outcome,
                         bool status_known, bool active,
                         const char *number, uint8_t delay) {
    memset(&s_result, 0, sizeof(s_result));
    s_result.pending = true;
    s_result.request_id = request_id;
    s_result.request = *request;
    s_result.outcome = outcome;
    s_result.status_known = status_known;
    s_result.active = active;
    if (number != NULL) {
        copy_text(s_result.number, sizeof(s_result.number), number);
    }
    s_result.has_delay = delay != 0u;
    s_result.delay_seconds = delay;
    s_result_pending = true;
}

bool modem_service_request_call_forward(const call_forward_request_t *request,
                                        uint32_t *request_id_out) {
    s_request_count++;
    s_last_request = *request;
    if (!s_request_accept) {
        if (request_id_out != NULL) {
            *request_id_out = 0u;
        }
        return false;
    }
    s_next_request_id++;
    if (request_id_out != NULL) {
        *request_id_out = s_next_request_id;
    }
    return true;
}

bool modem_service_pop_call_forward_result(call_forward_result_t *out) {
    if (!s_result_pending || out == NULL) {
        return false;
    }
    *out = s_result;
    s_result_pending = false;
    return true;
}

bool modem_service_cancel_queued_call_forward(uint32_t request_id) {
    s_cancel_count++;
    s_last_cancel_id = request_id;
    return s_cancel_queued;
}

bool modem_service_get_voice_mailbox_number(char *out, size_t out_cap) {
    if (out == NULL || out_cap == 0u || s_sim_mailbox[0] == '\0' ||
        strlen(s_sim_mailbox) >= out_cap) {
        if (out != NULL && out_cap > 0u) {
            out[0] = '\0';
        }
        return false;
    }
    copy_text(out, out_cap, s_sim_mailbox);
    return true;
}

store_status_t store_setting_get_text(store_setting_key_t key, char *out_text,
                                      uint8_t out_cap) {
    if (out_text == NULL || out_cap == 0u) {
        return STORE_STATUS_INVALID_ARGUMENT;
    }
    if (key == STORE_SETTING_SYSTEM_VOICE_MAILBOX_NUMBER) {
        copy_text(out_text, out_cap, s_local_mailbox);
        return STORE_STATUS_OK;
    }
    out_text[0] = '\0';
    return STORE_STATUS_NOT_FOUND;
}

store_status_t store_call_divert_get(store_call_divert_state_t *out_state) {
    if (!s_stored_divert_valid || out_state == NULL) {
        return STORE_STATUS_NOT_FOUND;
    }
    *out_state = s_stored_divert;
    return STORE_STATUS_OK;
}

store_status_t store_call_divert_set(const store_call_divert_state_t *state) {
    if (state == NULL) {
        return STORE_STATUS_INVALID_ARGUMENT;
    }
    s_saved_divert = *state;
    return STORE_STATUS_OK;
}

const char *ts_or(uint16_t sid, const char *fallback) {
    (void)sid;
    return fallback;
}

void copy_text(char *dst, size_t cap, const char *src) {
    if (dst == NULL || cap == 0u) {
        return;
    }
    if (src == NULL) {
        src = "";
    }
    snprintf(dst, cap, "%s", src);
}

uint16_t asset_next_codepoint(const char **cursor) {
    if (cursor == NULL || *cursor == NULL || **cursor == '\0') {
        return 0u;
    }
    return (uint8_t)*(*cursor)++;
}

static void capture_display(app_t *app, uint8_t record_id,
                            app_route_t return_route, uint32_t now) {
    app->call_divert_status_detail_active = false;
    app->route = APP_ROUTE_DISPLAY_MESSAGE;
    app->display_record_id = record_id;
    app->display_return_route = return_route;
    app->display_opened_ms = now;
    app->dirty = true;
}

void open_display_sid(app_t *app, uint8_t record_id, uint16_t sid,
                      const char *fallback, app_route_t return_route,
                      uint32_t now) {
    s_last_sid = sid;
    copy_text(s_last_a, sizeof(s_last_a), fallback);
    s_last_b[0] = '\0';
    capture_display(app, record_id, return_route, now);
}

void open_display_sid_num(app_t *app, uint8_t record_id, uint16_t sid,
                          const char *fallback, unsigned num,
                          app_route_t return_route, uint32_t now) {
    s_last_sid = sid;
    s_last_num = num;
    copy_text(s_last_a, sizeof(s_last_a), fallback);
    s_last_b[0] = '\0';
    capture_display(app, record_id, return_route, now);
}

void open_display(app_t *app, uint8_t record_id, const char *a,
                  const char *b, const char *c,
                  app_route_t return_route, uint32_t now) {
    (void)c;
    s_last_sid = 0u;
    copy_text(s_last_a, sizeof(s_last_a), a);
    copy_text(s_last_b, sizeof(s_last_b), b);
    capture_display(app, record_id, return_route, now);
}

void open_editor(app_t *app, const char *title, const char *value,
                 uint8_t max_len, editor_kind_t kind,
                 editor_context_t context, bool show_cursor, uint32_t now) {
    (void)title;
    (void)max_len;
    (void)kind;
    (void)context;
    (void)show_cursor;
    (void)now;
    copy_text(app->editor_value, sizeof(app->editor_value), value);
    app->route = APP_ROUTE_EDITOR;
}

void close_editor(app_t *app) {
    app->editor_value[0] = '\0';
}

void open_main_menu_at(app_t *app, uint8_t selected, uint32_t now_ms) {
    (void)selected;
    (void)now_ms;
    app->route = APP_ROUTE_MAIN_MENU;
}

static void test_mmi_request_and_async_identity(void) {
    app_t app;
    reset_fixture(&app);
    s_stored_divert_valid = true;
    s_stored_divert.delay_seconds = 17u;
    copy_text(s_stored_divert.numbers[1],
              sizeof(s_stored_divert.numbers[1]), "+15550000067");
    copy_text(s_stored_divert.numbers[2],
              sizeof(s_stored_divert.numbers[2]), "+15550000061");
    call_forward_request_t request = {
        .reason = CALL_FORWARD_REASON_UNCONDITIONAL,
        .action = CALL_FORWARD_ACTION_REGISTER,
        .has_number = true,
    };
    copy_text(request.number, sizeof(request.number), "+15551234567");

    check(call_divert_submit_mmi(&app, &request, 100u),
          "MMI registration is consumed");
    uint32_t request_id = s_next_request_id;
    check(s_request_count == 1u &&
              memcmp(&s_last_request, &request, sizeof(request)) == 0,
          "MMI submits the exact neutral request");
    check(app.route == APP_ROUTE_DISPLAY_MESSAGE &&
              app.display_record_id == 0x25u && s_last_sid == 0x2a4u &&
              app.display_return_route == APP_ROUTE_STANDBY,
          "MMI opens Nokia Requesting with a standby return");

    queue_result(request_id - 1u, &request, CALL_FORWARD_OUTCOME_SUCCESS,
                 false, false, NULL, 0u);
    check(!tick_call_divert(&app, 110u) && s_last_sid == 0x2a4u,
          "a stale request result cannot complete the current UI");

    queue_result(request_id, &request, CALL_FORWARD_OUTCOME_SUCCESS,
                 false, false, NULL, 0u);
    check(tick_call_divert(&app, 120u) && s_last_sid == 0x122u &&
              app.display_record_id == 3u,
          "registration completion uses Nokia Divert activated");
    check(s_saved_divert.active_mask == 0u &&
              strcmp(s_saved_divert.numbers[0], "+15551234567") == 0 &&
              strcmp(s_saved_divert.numbers[1], "+15550000067") == 0 &&
              strcmp(s_saved_divert.numbers[2], "+15550000061") == 0 &&
              s_saved_divert.delay_seconds == 20u,
          "first-use MMI preserves histories and normalizes a stale delay");

    request.action = CALL_FORWARD_ACTION_ENABLE;
    request.has_number = false;
    request.number[0] = '\0';
    check(call_divert_submit_mmi(&app, &request, 130u),
          "registered-destination activation is admitted");
    request_id = s_next_request_id;
    queue_result(request_id, &request, CALL_FORWARD_OUTCOME_SUCCESS,
                 false, false, NULL, 0u);
    check(tick_call_divert(&app, 140u) && s_last_sid == 0x12cu,
          "activation uses the distinct v6.00 activation-result SID");

    request.has_number = true;
    copy_text(request.number, sizeof(request.number), "+15550000021");
    check(call_divert_submit_mmi(&app, &request, 150u),
          "activation with a new MMI destination is admitted");
    request_id = s_next_request_id;
    queue_result(request_id, &request, CALL_FORWARD_OUTCOME_SUCCESS,
                 false, false, NULL, 0u);
    check(tick_call_divert(&app, 160u) &&
              strcmp(s_saved_divert.numbers[0], "+15550000021") == 0,
          "successful MMI activation keeps the menu destination synchronized");

    memset(&request, 0, sizeof(request));
    request.reason = CALL_FORWARD_REASON_NO_REPLY;
    request.action = CALL_FORWARD_ACTION_ENABLE;
    request.has_delay = true;
    request.delay_seconds = 25u;
    check(call_divert_submit_mmi(&app, &request, 170u),
          "timer-only no-reply activation is admitted");
    request_id = s_next_request_id;
    queue_result(request_id, &request, CALL_FORWARD_OUTCOME_SUCCESS,
                 false, false, NULL, 0u);
    check(tick_call_divert(&app, 180u) &&
              s_saved_divert.delay_seconds == 25u &&
              strcmp(s_saved_divert.numbers[2], "+15550000061") == 0,
          "timer-only MMI activation synchronizes delay without losing history");

    memset(&request, 0, sizeof(request));
    request.reason = CALL_FORWARD_REASON_ALL;
    request.action = CALL_FORWARD_ACTION_REGISTER;
    request.has_number = true;
    copy_text(request.number, sizeof(request.number), "+15550000002");
    check(call_divert_submit_mmi(&app, &request, 190u),
          "aggregate all-diverts registration is admitted");
    request_id = s_next_request_id;
    queue_result(request_id, &request, CALL_FORWARD_OUTCOME_SUCCESS,
                 false, false, NULL, 0u);
    check(tick_call_divert(&app, 191u),
          "aggregate all-diverts registration completes");
    bool all_synced = true;
    for (uint8_t i = 0u; i < STORE_CALL_DIVERT_CONDITION_COUNT; i++) {
        all_synced = all_synced &&
            strcmp(s_saved_divert.numbers[i], "+15550000002") == 0;
    }
    check(all_synced,
          "SC-002 registration synchronizes every menu destination history");

    request.reason = CALL_FORWARD_REASON_ALL_CONDITIONAL;
    copy_text(request.number, sizeof(request.number), "+15550000004");
    check(call_divert_submit_mmi(&app, &request, 192u),
          "aggregate conditional registration is admitted");
    request_id = s_next_request_id;
    queue_result(request_id, &request, CALL_FORWARD_OUTCOME_SUCCESS,
                 false, false, NULL, 0u);
    check(tick_call_divert(&app, 193u) &&
              strcmp(s_saved_divert.numbers[0], "+15550000002") == 0,
          "SC-004 leaves the unconditional destination untouched");
    bool conditional_synced = true;
    for (uint8_t i = 1u; i < STORE_CALL_DIVERT_CONDITION_COUNT; i++) {
        conditional_synced = conditional_synced &&
            strcmp(s_saved_divert.numbers[i], "+15550000004") == 0;
    }
    check(conditional_synced,
          "SC-004 synchronizes every conditional menu destination history");
}

static void test_quit_detaches_and_status_details(void) {
    app_t app;
    reset_fixture(&app);
    call_forward_request_t request = {
        .reason = CALL_FORWARD_REASON_NO_REPLY,
        .action = CALL_FORWARD_ACTION_QUERY,
    };
    check(call_divert_submit_mmi(&app, &request, 200u),
          "status request admitted");
    uint32_t request_id = s_next_request_id;
    check(handle_call_divert_display_key(&app, KEY_C, 210u) &&
              s_last_sid == 0x3e2u && app.display_record_id == 12u &&
              s_cancel_count == 1u && s_last_cancel_id == request_id,
          "Quit cancels queued work and shows Nokia Request not confirmed");
    queue_result(request_id, &request, CALL_FORWARD_OUTCOME_SUCCESS,
                 true, true, "+15559876543", 20u);
    check(!tick_call_divert(&app, 220u) && s_last_sid == 0x3e2u,
          "late completion after Quit cannot replace the dialog");

    check(call_divert_submit_mmi(&app, &request, 230u),
          "second status request admitted");
    request_id = s_next_request_id;
    queue_result(request_id, &request, CALL_FORWARD_OUTCOME_SUCCESS,
                 true, true, "+15559876543", 20u);
    check(tick_call_divert(&app, 240u) && app.display_record_id == 32u &&
              strcmp(s_last_a, "To number\n+15559876543") == 0 &&
              s_last_b[0] == '\0' &&
              strcmp(s_saved_divert.numbers[2], "+15559876543") == 0 &&
              s_saved_divert.delay_seconds == 20u,
          "active status expands the localized %S number template");
    check(handle_call_divert_display_key(&app, KEY_NAVI, 250u) &&
              s_last_sid == 0x3a5u && s_last_num == 20u,
          "Navi advances to the network-provided delay detail");
    check(handle_call_divert_display_key(&app, KEY_NAVI, 260u) &&
              app.route == APP_ROUTE_STANDBY,
          "final detail returns to the MMI caller");
}

static void test_status_detail_ownership_retires_on_preemption(void) {
    app_t app;
    reset_fixture(&app);
    call_forward_request_t request = {
        .reason = CALL_FORWARD_REASON_NO_REPLY,
        .action = CALL_FORWARD_ACTION_QUERY,
    };
    check(call_divert_submit_mmi(&app, &request, 270u),
          "detail ownership fixture request admitted");
    queue_result(s_next_request_id, &request,
                 CALL_FORWARD_OUTCOME_SUCCESS, true, true,
                 "+15559876543", 20u);
    check(tick_call_divert(&app, 280u) &&
              app.call_divert_status_detail_count == 2u &&
              app.call_divert_status_detail_active,
          "multi-page status detail owns its Nokia record");

    open_display(&app, CALL_DIVERT_DETAIL_RECORD_ID, "Not done", NULL, NULL,
                 APP_ROUTE_STANDBY, 285u);
    check(!tick_call_divert(&app, 286u) &&
              app.call_divert_status_detail_count == 0u &&
              !handle_call_divert_display_key(&app, KEY_NAVI, 287u),
          "same-record replacement cannot inherit divert detail navigation");

    check(call_divert_submit_mmi(&app, &request, 288u),
          "route-preemption detail request admitted");
    queue_result(s_next_request_id, &request,
                 CALL_FORWARD_OUTCOME_SUCCESS, true, true,
                 "+15559876543", 20u);
    check(tick_call_divert(&app, 289u) &&
              app.call_divert_status_detail_count == 2u,
          "route-preemption detail sequence opens");
    app.route = APP_ROUTE_INCOMING_CALL;
    check(!tick_call_divert(&app, 290u) &&
              app.call_divert_status_detail_count == 0u,
          "incoming call retires preempted status-detail navigation");
    open_display(&app, CALL_DIVERT_DETAIL_RECORD_ID, "Not done", NULL, NULL,
                 APP_ROUTE_STANDBY, 300u);
    check(!handle_call_divert_display_key(&app, KEY_NAVI, 301u),
          "shared record 0x20 cannot revive stale divert detail pages");
}

static void test_menu_and_mmi_share_requests(void) {
    app_t menu_app;
    reset_fixture(&menu_app);
    copy_text(s_sim_mailbox, sizeof(s_sim_mailbox), "+18005551212");
    open_call_divert_menu(&menu_app, CALL_DIVERT_MENU_ROOT, 1u);
    check(handle_call_divert_menu_key(&menu_app, KEY_NAVI, 300u) &&
              menu_app.call_divert_menu_kind == CALL_DIVERT_MENU_CONDITION,
          "busy condition opens its action list");
    check(handle_call_divert_menu_key(&menu_app, KEY_NAVI, 310u) &&
              menu_app.call_divert_menu_kind == CALL_DIVERT_MENU_ACTIVATE,
          "Activate opens its destination list");
    check(handle_call_divert_menu_key(&menu_app, KEY_NAVI, 320u),
          "SIM voice mailbox selection submits");
    call_forward_request_t menu_request = s_last_request;
    check(menu_request.reason == CALL_FORWARD_REASON_BUSY &&
              menu_request.action == CALL_FORWARD_ACTION_REGISTER &&
              strcmp(menu_request.number, "+18005551212") == 0,
          "menu resolves SIM mailbox into a busy-divert registration");

    call_forward_request_t parsed;
    check(call_forward_mmi_parse("**67*+18005551212#", &parsed) ==
              CALL_FORWARD_MMI_VALID &&
              memcmp(&parsed, &menu_request, sizeof(parsed)) == 0,
          "standby MMI and menu produce one identical request contract");

    app_t missing_app;
    reset_fixture(&missing_app);
    open_call_divert_menu(&missing_app, CALL_DIVERT_MENU_ROOT, 0u);
    (void)handle_call_divert_menu_key(&missing_app, KEY_NAVI, 330u);
    (void)handle_call_divert_menu_key(&missing_app, KEY_NAVI, 340u);
    (void)handle_call_divert_menu_key(&missing_app, KEY_NAVI, 350u);
    check(s_request_count == 0u && s_last_sid == 0x20au &&
              missing_app.display_record_id == 0u,
          "missing local and SIM mailbox uses the exact v6.00 prompt");

    app_t cancel_app;
    reset_fixture(&cancel_app);
    open_call_divert_menu(&cancel_app, CALL_DIVERT_MENU_ROOT, 5u);
    (void)handle_call_divert_menu_key(&cancel_app, KEY_NAVI, 360u);
    check(s_request_count == 1u &&
              s_last_request.reason == CALL_FORWARD_REASON_ALL &&
              s_last_request.action == CALL_FORWARD_ACTION_ERASE,
          "Cancel all menu emits the same ##002# semantic request");

    app_t delay_app;
    reset_fixture(&delay_app);
    open_call_divert_menu(&delay_app, CALL_DIVERT_MENU_ROOT, 2u);
    (void)handle_call_divert_menu_key(&delay_app, KEY_NAVI, 370u);
    (void)handle_call_divert_menu_key(&delay_app, KEY_DOWN, 371u);
    (void)handle_call_divert_menu_key(&delay_app, KEY_DOWN, 372u);
    (void)handle_call_divert_menu_key(&delay_app, KEY_DOWN, 373u);
    (void)handle_call_divert_menu_key(&delay_app, KEY_NAVI, 374u);
    check(delay_app.call_divert_menu_kind == CALL_DIVERT_MENU_DELAY,
          "no-reply Set delay opens the six-value selector");
    (void)handle_call_divert_menu_key(&delay_app, KEY_NAVI, 375u);
    check(s_request_count == 0u && s_last_sid == 0x3a8u &&
              s_saved_divert.delay_seconds == 20u,
          "delay selection persists locally and shows v6.00 Done");
}

static void test_failure_mapping_and_icon_authority(void) {
    app_t app;
    reset_fixture(&app);
    call_forward_request_t request = {
        .reason = CALL_FORWARD_REASON_BUSY,
        .action = CALL_FORWARD_ACTION_QUERY,
    };
    check(call_divert_submit_mmi(&app, &request, 400u),
          "failure fixture request admitted");
    queue_result(s_next_request_id, &request,
                 CALL_FORWARD_OUTCOME_NO_NETWORK,
                 false, false, NULL, 0u);
    check(tick_call_divert(&app, 410u) && s_last_sid == 0x209u &&
              app.display_record_id == 32u,
          "no-service completion maps to Nokia No network coverage");

    app.call_divert_unconditional_active = true;
    check(call_divert_any_active(&app),
          "standby icon follows authoritative runtime CFU state");
    app.call_divert_unconditional_active = false;
    check(!call_divert_any_active(&app),
          "standby icon clears with authoritative runtime CFU state");
}

static void test_result_waits_for_ui_preemption(void) {
    app_t app;
    reset_fixture(&app);
    call_forward_request_t request = {
        .reason = CALL_FORWARD_REASON_BUSY,
        .action = CALL_FORWARD_ACTION_QUERY,
    };
    check(call_divert_submit_mmi(&app, &request, 500u),
          "preemption fixture request admitted");
    uint32_t request_id = s_next_request_id;
    queue_result(request_id, &request, CALL_FORWARD_OUTCOME_SUCCESS,
                 true, false, NULL, 0u);

    app.route = APP_ROUTE_INCOMING_CALL;
    check(!tick_call_divert(&app, 510u) && s_result_pending &&
              app.call_divert_pending_action != 0u,
          "incoming call keeps both its UI and the pending divert result");

    app.route = APP_ROUTE_DISPLAY_MESSAGE;
    app.display_record_id = 5u;
    check(!tick_call_divert(&app, 520u) && s_result_pending,
          "missed-call notice cannot be overwritten by the late result");

    app.route = APP_ROUTE_STANDBY;
    check(tick_call_divert(&app, 530u) && !s_result_pending &&
              s_last_sid == 0x128u && app.display_record_id == 3u,
          "deferred result renders after the preempting call UI returns");

    check(call_divert_submit_mmi(&app, &request, 540u),
          "power-off fixture request admitted");
    request_id = s_next_request_id;
    app.route = APP_ROUTE_POWER_OFF;
    check(!tick_call_divert(&app, 550u) &&
              app.call_divert_pending_action == 0u &&
              app.call_divert_request_id == 0u,
          "power-off detaches the pending divert UI request");
    queue_result(request_id, &request, CALL_FORWARD_OUTCOME_CANCELLED,
                 false, false, NULL, 0u);
    app.route = APP_ROUTE_STANDBY;
    check(!tick_call_divert(&app, 560u) && !s_result_pending &&
              s_last_sid == 0x2a4u,
          "late power-off cancellation is discarded after the next power-on");
}

static void test_menu_result_cannot_teleport_through_keyguard(void) {
    app_t app;
    reset_fixture(&app);
    copy_text(s_sim_mailbox, sizeof(s_sim_mailbox), "+18005551212");
    open_call_divert_menu(&app, CALL_DIVERT_MENU_ROOT, 1u);
    (void)handle_call_divert_menu_key(&app, KEY_NAVI, 600u);
    (void)handle_call_divert_menu_key(&app, KEY_DOWN, 601u);
    (void)handle_call_divert_menu_key(&app, KEY_DOWN, 602u);
    (void)handle_call_divert_menu_key(&app, KEY_NAVI, 603u);
    uint32_t request_id = s_next_request_id;
    check(app.route == APP_ROUTE_DISPLAY_MESSAGE &&
              app.call_divert_result_return_route ==
                  APP_ROUTE_CALL_DIVERT_MENU,
          "menu status request starts with a menu return route");
    queue_result(request_id, &s_last_request,
                 CALL_FORWARD_OUTCOME_SUCCESS, true, false, NULL, 0u);

    app.route = APP_ROUTE_INCOMING_CALL;
    check(!tick_call_divert(&app, 610u) && s_result_pending,
          "incoming call retains the delayed menu result");
    app.route = APP_ROUTE_STANDBY;
    app.keyguard_locked = true;
    check(!tick_call_divert(&app, 620u) && s_result_pending,
          "keyguard blocks delayed supplementary-service presentation");
    app.keyguard_locked = false;
    check(tick_call_divert(&app, 630u) && !s_result_pending &&
              app.route == APP_ROUTE_DISPLAY_MESSAGE &&
              app.display_return_route == APP_ROUTE_STANDBY &&
              app.call_divert_result_return_route == APP_ROUTE_STANDBY,
          "unlocked result returns to standby instead of reopening the menu");
}

int main(void) {
    test_mmi_request_and_async_identity();
    test_quit_detaches_and_status_details();
    test_status_detail_ownership_retires_on_preemption();
    test_menu_and_mmi_share_requests();
    test_failure_mapping_and_icon_authority();
    test_result_waits_for_ui_preemption();
    test_menu_result_cannot_teleport_through_keyguard();
    if (s_failures != 0) {
        fprintf(stderr, "%d failure(s)\n", s_failures);
        return 1;
    }
    puts("call-divert app tests passed");
    return 0;
}

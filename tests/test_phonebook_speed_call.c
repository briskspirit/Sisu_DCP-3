/* Speed-dial Options > Call must enter the same calls_app transaction path as
 * every other outgoing-call entry point. A direct modem request leaves the UI
 * on Standby and bypasses call identity, timers, cancellation, and errors. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "apps/calls_app.h"
#include "apps/dialogs_app.h"
#include "apps/phonebook_app.h"
#include "apps/standby_app.h"
#include "services/input_keys.h"
#include "services/strings.h"
#include "storage/store_service.h"
#include "ui/ui.h"

_Static_assert(sizeof(((app_t *)0)->phonebook_pending_selected) ==
                   sizeof(uint16_t),
               "phonebook selection must represent all 500 cached rows");

static int s_failures;
static unsigned s_start_call_count;
static unsigned s_direct_dial_count;
static char s_call_number[MODEM_PHONE_MAX + 1u];
static char s_call_name[MODEM_PHONEBOOK_NAME_MAX + 1u];
static app_route_t s_error_route;
static bool s_phonebook_admit = true;
static uint32_t s_next_phonebook_request_id = 100u;
static modem_phonebook_result_t s_phonebook_results[8];
static uint8_t s_phonebook_result_head;
static uint8_t s_phonebook_result_count;
static uint8_t s_phonebook_entry_reads_before_failure = UINT8_MAX;
static char s_display_text[32];
static uint16_t s_phonebook_count = 1u;

static void check(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        s_failures++;
    }
}

int32_t time_diff_ms(uint32_t a, uint32_t b) {
    return (int32_t)(a - b);
}

void copy_text(char *dst, size_t cap, const char *src) {
    if (dst == NULL || cap == 0u) {
        return;
    }
    snprintf(dst, cap, "%s", src != NULL ? src : "");
}

const char *ts(uint16_t sid) {
    (void)sid;
    return NULL;
}

const char *ts_or(uint16_t sid, const char *fallback) {
    (void)sid;
    return fallback;
}

store_status_t store_phonebook_get_speed_dial(uint8_t key, uint16_t *out_contact_index) {
    check(key == 2u, "fixture requests speed-dial key 2");
    *out_contact_index = 42u;
    return STORE_STATUS_OK;
}

store_status_t store_phonebook_clear_speed_dial(uint8_t key) {
    (void)key;
    return STORE_STATUS_OK;
}

static bool admit_phonebook(uint32_t *request_id_out) {
    if (request_id_out != NULL) {
        *request_id_out = 0u;
    }
    if (!s_phonebook_admit) {
        return false;
    }
    if (request_id_out != NULL) {
        *request_id_out = ++s_next_phonebook_request_id;
    }
    return true;
}

bool modem_service_request_phonebook_list(uint32_t *request_id_out) {
    return admit_phonebook(request_id_out);
}

bool modem_service_request_phonebook_add(const char *name, const char *number,
                                         uint32_t *request_id_out) {
    (void)name;
    (void)number;
    return admit_phonebook(request_id_out);
}

bool modem_service_request_phonebook_update(uint16_t index, const char *name,
                                            const char *number,
                                            uint32_t *request_id_out) {
    (void)index;
    (void)name;
    (void)number;
    return admit_phonebook(request_id_out);
}

bool modem_service_request_phonebook_delete(uint16_t index,
                                            uint32_t *request_id_out) {
    (void)index;
    return admit_phonebook(request_id_out);
}

bool modem_service_pop_phonebook_result(modem_phonebook_result_t *out) {
    if (out == NULL || s_phonebook_result_count == 0u) {
        return false;
    }
    *out = s_phonebook_results[s_phonebook_result_head];
    s_phonebook_result_head =
        (uint8_t)((s_phonebook_result_head + 1u) % 8u);
    s_phonebook_result_count--;
    return true;
}

uint16_t modem_service_phonebook_count(void) {
    return s_phonebook_count;
}

bool modem_service_phonebook_entry(uint16_t position, modem_phonebook_entry_t *out) {
    if (position != 0u || out == NULL ||
        s_phonebook_entry_reads_before_failure == 0u) {
        return false;
    }
    if (s_phonebook_entry_reads_before_failure != UINT8_MAX) {
        s_phonebook_entry_reads_before_failure--;
    }
    memset(out, 0, sizeof(*out));
    out->index = 42u;
    copy_text(out->number, sizeof(out->number), "+15550000042");
    copy_text(out->name, sizeof(out->name), "Speed contact");
    return true;
}

bool modem_service_request_dial(const char *number) {
    (void)number;
    s_direct_dial_count++;
    return true;
}

bool start_outgoing_call(app_t *app, const char *number, const char *name,
                         uint32_t now, app_route_t error_route) {
    (void)now;
    s_start_call_count++;
    copy_text(s_call_number, sizeof(s_call_number), number);
    copy_text(s_call_name, sizeof(s_call_name), name);
    s_error_route = error_route;
    app->route = APP_ROUTE_CALL;
    return true;
}

void open_display_sid(app_t *app, uint8_t record_id, uint16_t sid,
                      const char *fallback, app_route_t return_route, uint32_t now) {
    (void)sid;
    copy_text(s_display_text, sizeof(s_display_text), fallback);
    (void)now;
    app->phonebook_wait_display_request_id = 0u;
    app->route = APP_ROUTE_DISPLAY_MESSAGE;
    app->display_record_id = record_id;
    app->display_return_route = return_route;
}

void open_display(app_t *app, uint8_t record_id, const char *a,
                  const char *b, const char *c,
                  app_route_t return_route, uint32_t now) {
    (void)b;
    (void)c;
    (void)now;
    copy_text(s_display_text, sizeof(s_display_text), a);
    app->phonebook_wait_display_request_id = 0u;
    app->route = APP_ROUTE_DISPLAY_MESSAGE;
    app->display_record_id = record_id;
    app->display_return_route = return_route;
}

void draw_softkey(framebuffer_t *fb, const char *label) {
    (void)fb;
    (void)label;
}

void open_display_sid_num(app_t *app, uint8_t record_id, uint16_t sid,
                          const char *fallback, unsigned num,
                          app_route_t return_route, uint32_t now) {
    (void)app;
    (void)record_id;
    (void)sid;
    (void)fallback;
    (void)num;
    (void)return_route;
    (void)now;
}

void set_standby_message(app_t *app, const char *text, uint32_t now_ms) {
    (void)app;
    (void)text;
    (void)now_ms;
}

void close_editor(app_t *app) {
    (void)app;
}

bool editor_delete_one(app_t *app) {
    (void)app;
    return false;
}

char key_digit(uint16_t key) {
    return key >= KEY_0 && key <= KEY_9 ? (char)('0' + key - KEY_0) : '\0';
}

store_status_t store_setting_get_text(store_setting_key_t key, char *out_text,
                                      uint8_t out_cap) {
    check(key == STORE_SETTING_SECURITY_CODE,
          "erase-all reads only the security code");
    if (out_text != NULL && out_cap > 0u) {
        snprintf(out_text, out_cap, "12345");
    }
    return STORE_STATUS_OK;
}

static void push_phonebook_result(uint32_t request_id,
                                  modem_phonebook_op_t kind,
                                  modem_phonebook_outcome_t outcome) {
    check(s_phonebook_result_count < 8u,
          "phonebook UI result fixture has journal space");
    uint8_t tail = (uint8_t)((s_phonebook_result_head +
                              s_phonebook_result_count) % 8u);
    s_phonebook_results[tail] = (modem_phonebook_result_t){
        .request_id = request_id,
        .kind = kind,
        .outcome = outcome,
    };
    s_phonebook_result_count++;
}

static void test_options_call_uses_calls_app(void) {
    app_t app;
    memset(&app, 0, sizeof(app));
    app.route = APP_ROUTE_PHONEBOOK_SPEED_DIAL_OPTIONS;
    app.phonebook_speed_selected = 0u;
    app.phonebook_speed_key = 2u;
    app.phonebook_speed_options_selected = 0u;

    check(handle_phonebook_speed_options_key(&app, KEY_NAVI, 1234u),
          "speed-dial Call consumes Navi");
    check(s_start_call_count == 1u,
          "speed-dial Call enters start_outgoing_call exactly once");
    check(s_direct_dial_count == 0u,
          "speed-dial Call never bypasses calls_app with a direct modem dial");
    check(strcmp(s_call_number, "+15550000042") == 0,
          "speed-dial Call carries the selected contact number");
    check(strcmp(s_call_name, "Speed contact") == 0,
          "speed-dial Call carries the selected contact name");
    check(s_error_route == APP_ROUTE_PHONEBOOK_SPEED_DIAL_OPTIONS,
          "dial failure returns to the speed-dial options screen");
    check(app.route == APP_ROUTE_CALL,
          "the calls app owns the resulting call route");
}

static void test_high_list_selection_is_not_narrowed(void) {
    app_t app;
    memset(&app, 0, sizeof(app));
    start_phonebook_list(&app, PHONEBOOK_LABEL_CALL, "1-1",
                         PHONEBOOK_CONTEXT_STANDBY, 300u, 77u);
    check(app.phonebook_pending_selected == 300u,
          "phonebook request preserves a selected row above 255");
    check(app.phonebook_request_id != 0u,
          "phonebook UI owns the admitted request id");
}

static void test_late_result_never_hijacks_priority_routes(void) {
    const app_route_t routes[] = {
        APP_ROUTE_STANDBY,
        APP_ROUTE_INCOMING_CALL,
        APP_ROUTE_CALL,
        APP_ROUTE_POWER_OFF,
    };
    for (size_t i = 0u; i < sizeof(routes) / sizeof(routes[0]); i++) {
        app_t app;
        memset(&app, 0, sizeof(app));
        start_phonebook_list(&app, PHONEBOOK_LABEL_CALL, "1-1",
                             PHONEBOOK_CONTEXT_STANDBY, 0u, 100u);
        uint32_t request_id = app.phonebook_request_id;
        app.route = routes[i];
        push_phonebook_result(request_id, MODEM_PHONEBOOK_OP_LIST,
                              MODEM_PHONEBOOK_OUTCOME_OK);
        check(!poll_phonebook(&app, 101u) && app.route == routes[i] &&
                  app.phonebook_pending_kind == PHONEBOOK_PENDING_NONE &&
                  app.phonebook_request_id == 0u,
              "late list completion cannot replace a priority route");
    }
}

static void test_late_result_never_replaces_newer_dialog(void) {
    app_t app;
    memset(&app, 0, sizeof(app));
    start_phonebook_list(&app, PHONEBOOK_LABEL_CALL, "1-1",
                         PHONEBOOK_CONTEXT_STANDBY, 0u, 150u);
    uint32_t request_id = app.phonebook_request_id;

    /* Record 4 is intentionally shared by several progress notes. Replacing the
     * phonebook's Opening note must revoke its UI ownership even though the new
     * note has the same route and record id. */
    open_display(&app, 4u, "Switching profile", NULL, NULL,
                 APP_ROUTE_PROFILES_MENU, 151u);
    push_phonebook_result(request_id, MODEM_PHONEBOOK_OP_LIST,
                          MODEM_PHONEBOOK_OUTCOME_OK);

    check(!poll_phonebook(&app, 152u) &&
              app.route == APP_ROUTE_DISPLAY_MESSAGE &&
              app.display_record_id == 4u &&
              app.display_return_route == APP_ROUTE_PROFILES_MENU &&
              strcmp(s_display_text, "Switching profile") == 0 &&
              app.phonebook_pending_kind == PHONEBOOK_PENDING_NONE &&
              app.phonebook_request_id == 0u &&
              s_phonebook_result_count == 0u,
          "a matching late phonebook terminal cannot replace a newer dialog");
}

static void test_erase_all_grace_never_hijacks_priority_route(void) {
    app_t app;
    memset(&app, 0, sizeof(app));
    app.phonebook_erase_all_started_ms = 100u;
    app.route = APP_ROUTE_CALL;

    bool polled = poll_phonebook(&app, 11100u);
    bool ticked = tick_phonebook_erase_all(&app, 11100u);
    check(!polled && !ticked && app.route == APP_ROUTE_CALL &&
              app.phonebook_erase_all_started_ms == 0u,
          "erase-all grace completion cannot replace a priority route");

    memset(&app, 0, sizeof(app));
    app.phonebook_erase_all_started_ms = 100u;
    app.phonebook_wait_display_request_id = 88u;
    app.route = APP_ROUTE_DISPLAY_MESSAGE;
    app.display_record_id = PHONEBOOK_REQUEST_DISPLAY_RECORD_ID;
    check(tick_phonebook_erase_all(&app, 11100u) &&
              app.route == APP_ROUTE_DISPLAY_MESSAGE &&
              app.display_record_id == 26u &&
              app.display_return_route == APP_ROUTE_PHONEBOOK_MENU &&
              app.phonebook_erase_all_started_ms == 0u &&
              app.phonebook_wait_display_request_id == 0u,
          "an owned erase-all grace note still reaches Memory erased");
}

static void test_newer_request_owns_completion(void) {
    app_t app;
    memset(&app, 0, sizeof(app));
    start_phonebook_list(&app, PHONEBOOK_LABEL_CALL, "1-1",
                         PHONEBOOK_CONTEXT_STANDBY, 0u, 200u);
    uint32_t old_id = app.phonebook_request_id;
    start_phonebook_list(&app, PHONEBOOK_LABEL_CALL, "1-1",
                         PHONEBOOK_CONTEXT_STANDBY, 0u, 201u);
    uint32_t current_id = app.phonebook_request_id;
    push_phonebook_result(old_id, MODEM_PHONEBOOK_OP_LIST,
                          MODEM_PHONEBOOK_OUTCOME_OK);
    push_phonebook_result(current_id, MODEM_PHONEBOOK_OP_LIST,
                          MODEM_PHONEBOOK_OUTCOME_OK);
    check(poll_phonebook(&app, 202u) && old_id != current_id &&
              app.route == APP_ROUTE_PHONEBOOK_LIST &&
              app.phonebook_pending_kind == PHONEBOOK_PENDING_NONE &&
              app.phonebook_request_id == 0u,
          "an old terminal is discarded while the newer token completes");
}

static void test_wrong_kind_and_backstop_fail_closed(void) {
    app_t app;
    memset(&app, 0, sizeof(app));
    start_phonebook_list(&app, PHONEBOOK_LABEL_CALL, "1-1",
                         PHONEBOOK_CONTEXT_STANDBY, 0u, 300u);
    push_phonebook_result(app.phonebook_request_id,
                          MODEM_PHONEBOOK_OP_DELETE,
                          MODEM_PHONEBOOK_OUTCOME_OK);
    check(poll_phonebook(&app, 301u) &&
              app.phonebook_pending_kind == PHONEBOOK_PENDING_NONE &&
              strcmp(s_display_text, "Not\nallowed") == 0,
          "a matching token with the wrong operation cannot mutate list UI");

    memset(&app, 0, sizeof(app));
    start_phonebook_list(&app, PHONEBOOK_LABEL_CALL, "1-1",
                         PHONEBOOK_CONTEXT_STANDBY, 0u, 400u);
    check(poll_phonebook(&app, 45400u) &&
              app.phonebook_pending_kind == PHONEBOOK_PENDING_NONE &&
              app.phonebook_request_id == 0u &&
              strcmp(s_display_text, "SIM card\nnot ready") == 0,
          "a missing terminal still has a bounded UI backstop");
}

static void test_erase_all_checks_admission_and_is_bounded(void) {
    app_t app;
    memset(&app, 0, sizeof(app));
    app.route = APP_ROUTE_PHONEBOOK_SECURITY;
    copy_text(app.editor_value, sizeof(app.editor_value), "12345");
    s_phonebook_admit = false;
    check(handle_phonebook_security_key(&app, KEY_NAVI, 500u) &&
              app.phonebook_erase_all_started_ms == 0u &&
              app.phonebook_pending_kind == PHONEBOOK_PENDING_NONE &&
              app.phonebook_request_id == 0u &&
              strcmp(s_display_text, "SIM card\nbusy") == 0,
          "erase-all rejects a full queue instead of waiting forever");

    memset(&app, 0, sizeof(app));
    app.route = APP_ROUTE_PHONEBOOK_SECURITY;
    copy_text(app.editor_value, sizeof(app.editor_value), "12345");
    s_phonebook_admit = true;
    check(handle_phonebook_security_key(&app, KEY_NAVI, 600u) &&
              app.phonebook_request_id != 0u &&
              poll_phonebook(&app, 45600u) &&
              app.phonebook_erase_all_started_ms == 0u &&
              app.phonebook_pending_kind == PHONEBOOK_PENDING_NONE &&
              app.phonebook_request_id == 0u,
          "erase-all cannot outlive the bounded request backstop");

    memset(&app, 0, sizeof(app));
    app.route = APP_ROUTE_PHONEBOOK_SECURITY;
    copy_text(app.editor_value, sizeof(app.editor_value), "12345");
    check(handle_phonebook_security_key(&app, KEY_NAVI, 700u),
          "erase-all admits its initial list for cache-race coverage");
    uint32_t request_id = app.phonebook_request_id;
    push_phonebook_result(request_id, MODEM_PHONEBOOK_OP_LIST,
                          MODEM_PHONEBOOK_OUTCOME_OK);
    /* build_phonebook_visible reads the row once; make the subsequent delete
     * lookup fail as if the cache changed between the two bounded reads. */
    s_phonebook_entry_reads_before_failure = 1u;
    check(poll_phonebook(&app, 701u) &&
              app.phonebook_erase_all_started_ms == 0u &&
              app.phonebook_pending_kind == PHONEBOOK_PENDING_NONE &&
              app.phonebook_request_id == 0u &&
              strcmp(s_display_text, "Not\nallowed") == 0,
          "erase-all fails closed if its selected cache row disappears");
    s_phonebook_entry_reads_before_failure = UINT8_MAX;
}

static void test_memory_status_uses_full_telit_domain(void) {
    app_t app;
    memset(&app, 0, sizeof(app));
    const font_t *font = asset_font(FONT_FS2);

    s_phonebook_count = 0u;
    framebuffer_t actual;
    memset(&actual, 0, sizeof(actual));
    render_phonebook_memory(&app, &actual);

    framebuffer_t expected;
    fb_clear(&expected, false);
    fb_text(&expected, font, "SIM card:", 0, 7, true, FB_WIDTH);
    fb_text(&expected, font, "500 free", 0, 16, true, FB_WIDTH);
    fb_text(&expected, font, "0 in use", 0, 25, true, FB_WIDTH);
    check(memcmp(&actual, &expected, sizeof(actual)) == 0,
          "empty Telit ME store reports all 500 records free");

    s_phonebook_count = MODEM_PHONEBOOK_MAX_RECORDS;
    memset(&actual, 0, sizeof(actual));
    render_phonebook_memory(&app, &actual);
    fb_clear(&expected, false);
    fb_text(&expected, font, "SIM card:", 0, 7, true, FB_WIDTH);
    fb_text(&expected, font, "0 free", 0, 16, true, FB_WIDTH);
    fb_text(&expected, font, "500 in use", 0, 25, true, FB_WIDTH);
    check(memcmp(&actual, &expected, sizeof(actual)) == 0,
          "full Telit ME store reports 500 records in use and none free");
    s_phonebook_count = 1u;
}

int main(void) {
    test_options_call_uses_calls_app();
    test_high_list_selection_is_not_narrowed();
    test_late_result_never_hijacks_priority_routes();
    test_late_result_never_replaces_newer_dialog();
    test_erase_all_grace_never_hijacks_priority_route();
    test_newer_request_owns_completion();
    test_wrong_kind_and_backstop_fail_closed();
    test_erase_all_checks_admission_and_is_bounded();
    test_memory_status_uses_full_telit_domain();
    if (s_failures != 0) {
        fprintf(stderr, "%d phonebook speed-call test(s) failed\n", s_failures);
        return 1;
    }
    puts("phonebook speed-call tests passed");
    return 0;
}

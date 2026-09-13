/* Exchange-rate persistence is transactional: invalid values must not be
 * presented as saved, and RAM must not publish a value that flash rejected. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "apps/calculator_app.h"
#include "apps/dialogs_app.h"
#include "apps/main_menu_app.h"
#include "services/input_keys.h"
#include "services/strings.h"
#include "storage/store_service.h"
#include "ui/ui.h"

static int s_failures;
static store_status_t s_store_result;
static unsigned s_store_calls;
static char s_stored_text[16];
static uint16_t s_display_sid;

static void check(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        s_failures++;
    }
}

void copy_text(char *dst, size_t cap, const char *src) {
    if (dst == NULL || cap == 0u) {
        return;
    }
    snprintf(dst, cap, "%s", src != NULL ? src : "");
}

const char *ts_or(uint16_t sid, const char *fallback) {
    (void)sid;
    return fallback;
}

void open_main_menu_at(app_t *app, uint8_t selected, uint32_t now_ms) {
    (void)selected;
    (void)now_ms;
    app->route = APP_ROUTE_MAIN_MENU;
}

store_status_t store_setting_set_text(store_setting_key_t key, const char *text) {
    check(key == STORE_SETTING_CALCULATOR_EXCHANGE_RATE,
          "exchange save uses the calculator rate setting");
    s_store_calls++;
    copy_text(s_stored_text, sizeof(s_stored_text), text);
    return s_store_result;
}

void open_display_sid(app_t *app, uint8_t record_id, uint16_t sid,
                      const char *fallback, app_route_t return_route, uint32_t now) {
    (void)record_id;
    (void)fallback;
    (void)return_route;
    (void)now;
    s_display_sid = sid;
    app->route = APP_ROUTE_DISPLAY_MESSAGE;
}

static void reset_fixture(app_t *app, const char *editor_value) {
    memset(app, 0, sizeof(*app));
    app->route = APP_ROUTE_CALCULATOR;
    app->calculator_exchange_mode = 2u;
    app->calculator_exchange_direction = 0u;
    app->calculator_exchange_dirty = true;
    copy_text(app->calculator_exchange_rate, sizeof(app->calculator_exchange_rate), "1.25");
    copy_text(app->calculator_exchange_editor_value,
              sizeof(app->calculator_exchange_editor_value), editor_value);
    app->calculator_exchange_cursor_index =
        (uint8_t)strlen(app->calculator_exchange_editor_value);
    s_store_result = STORE_STATUS_OK;
    s_store_calls = 0u;
    s_stored_text[0] = '\0';
    s_display_sid = 0u;
}

static void save_with_navi(app_t *app, uint32_t now) {
    check(handle_calculator_key(app, KEY_NAVI, EVENT_KEY_DOWN, now),
          "exchange editor consumes Navi");
}

static void test_empty_and_zero_are_rejected(void) {
    app_t app;
    reset_fixture(&app, "");
    save_with_navi(&app, 100u);
    check(s_store_calls == 0u, "empty rate is not persisted");
    check(strcmp(app.calculator_exchange_rate, "1.25") == 0,
          "empty rate leaves the active rate unchanged");
    check(s_display_sid == 0x7du,
          "empty rate reports the existing divide-by-zero error");

    reset_fixture(&app, "0");
    save_with_navi(&app, 200u);
    check(s_store_calls == 0u, "zero direct rate is not persisted");
    check(strcmp(app.calculator_exchange_rate, "1.25") == 0,
          "zero direct rate leaves the active rate unchanged");
    check(s_display_sid == 0x7du,
          "zero direct rate reports divide-by-zero instead of Saved");
}

static void test_publish_follows_successful_storage(void) {
    app_t app;
    reset_fixture(&app, "2.5");
    save_with_navi(&app, 300u);
    check(s_store_calls == 1u && strcmp(s_stored_text, "2.5") == 0,
          "valid rate is persisted exactly once");
    check(strcmp(app.calculator_exchange_rate, "2.5") == 0,
          "successful persistence publishes the new active rate");
    check(s_display_sid == 0x84u, "successful persistence reports Rate saved");

    reset_fixture(&app, "3");
    s_store_result = STORE_STATUS_STORAGE_ERROR;
    save_with_navi(&app, 400u);
    check(s_store_calls == 1u, "failed persistence is attempted exactly once");
    check(strcmp(app.calculator_exchange_rate, "1.25") == 0,
          "failed persistence does not publish an unsaved rate");
    check(s_display_sid == 0x280u,
          "failed persistence reports Memory full instead of Rate saved");
}

int main(void) {
    test_empty_and_zero_are_rejected();
    test_publish_follows_successful_storage();
    if (s_failures != 0) {
        fprintf(stderr, "%d calculator test(s) failed\n", s_failures);
        return 1;
    }
    puts("calculator tests passed");
    return 0;
}

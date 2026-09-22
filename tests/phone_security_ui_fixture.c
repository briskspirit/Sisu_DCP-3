/* Service boundary for unrelated UI tests; the real policy and persistence
 * are exercised separately by test_phone_security and test_store_service. */
#include "apps/phone_security_app.h"
#include "services/phone_security.h"
#include "storage/store_service.h"
#include <string.h>

phone_security_state_t phone_security_state(void) { return PHONE_SECURITY_OPEN; }
bool phone_security_enabled(void) { return false; }
uint32_t phone_security_generation(void) { return 1u; }
void security_code_forget_grant(void) {}
bool phone_security_set_enabled(bool enabled, uint32_t now) { (void)enabled; (void)now; return false; }
bool phone_security_restore_defaults(void) { return true; }
security_code_result_t security_code_verify(const char *code, uint32_t now) {
    (void)now;
    char expected[STORE_TEXT_MAX + 1u] = {0};
    if (store_setting_get_text(STORE_SETTING_SECURITY_CODE, expected, sizeof(expected)) != STORE_STATUS_OK ||
        strlen(expected) == 0u || strlen(expected) > 10u ||
        strspn(expected, "0123456789") != strlen(expected)) return SECURITY_CODE_UNAVAILABLE;
    return strcmp(code, expected) == 0 ? SECURITY_CODE_OK : SECURITY_CODE_WRONG;
}
bool security_code_change(const char *code) {
    return store_setting_set_text(STORE_SETTING_SECURITY_CODE, code) == STORE_STATUS_OK;
}
void open_phone_security(app_t *app, bool startup, uint32_t now) {
    (void)now;
    app->route = APP_ROUTE_PHONE_SECURITY;
    app->phone_security_startup = startup;
    app->phone_security_setting_authorized = false;
}

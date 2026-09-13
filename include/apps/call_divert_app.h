#ifndef APPS_CALL_DIVERT_APP_H
#define APPS_CALL_DIVERT_APP_H

#include <stdbool.h>
#include <stdint.h>

#include "app_internal.h"
#include "services/call_forward_types.h"

/* v6.00 display-message records shared with the generic dialog renderer. */
#define CALL_DIVERT_REQUEST_RECORD_ID 0x25u
#define CALL_DIVERT_DETAIL_RECORD_ID 0x20u
#define CALL_DIVERT_QUIT_RECORD_ID 0x0cu

typedef enum {
    CALL_DIVERT_MENU_ROOT = 0,
    CALL_DIVERT_MENU_CONDITION,
    CALL_DIVERT_MENU_ACTIVATE,
    CALL_DIVERT_MENU_DELAY,
} call_divert_menu_kind_t;

void open_call_divert_menu(app_t *app, call_divert_menu_kind_t kind, uint8_t selected);
bool handle_call_divert_menu_key(app_t *app, uint16_t key, uint32_t now);
bool handle_call_divert_display_key(app_t *app, uint16_t key, uint32_t now);
bool tick_call_divert(app_t *app, uint32_t now);
void render_call_divert_menu(const app_t *app, framebuffer_t *fb);
void call_divert_submit_other_number(app_t *app, uint32_t now);
void call_divert_cancel_other_number(app_t *app, uint32_t now);
bool call_divert_submit_mmi(app_t *app,
                            const call_forward_request_t *request,
                            uint32_t now);
bool call_divert_any_active(const app_t *app);

#endif

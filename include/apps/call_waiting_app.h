#ifndef APPS_CALL_WAITING_APP_H
#define APPS_CALL_WAITING_APP_H

#include "app_internal.h"
#include "services/call_waiting_types.h"

void open_call_waiting_request(app_t *app, call_waiting_action_t action, uint32_t now);
bool handle_call_waiting_display_key(app_t *app, uint16_t key, uint32_t now);
bool tick_call_waiting(app_t *app, uint32_t now);

#endif

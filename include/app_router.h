#ifndef APP_ROUTER_H
#define APP_ROUTER_H

#include <stdbool.h>

#include "app.h"
#include "ui/framebuffer.h"

bool app_router_handle_event(app_t *app, const input_event_t *event);
/* Invoke the current menu's ordinary Navi action without another key event. */
void app_router_select_menu(app_t *app, uint32_t now);

void app_router_render(const app_t *app, framebuffer_t *fb);

#endif

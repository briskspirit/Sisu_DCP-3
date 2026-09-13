#ifndef APP_ROUTER_H
#define APP_ROUTER_H

#include <stdbool.h>

#include "app.h"
#include "ui/framebuffer.h"

bool app_router_handle_event(app_t *app, const input_event_t *event);

void app_router_render(const app_t *app, framebuffer_t *fb);

#endif

#ifndef APPS_SERVICE_CODES_APP_H
#define APPS_SERVICE_CODES_APP_H

#include <stdbool.h>
#include <stdint.h>

#include "app_internal.h"

bool handle_standby_service_code(app_t *app, uint32_t now);

void open_service_codes_warranty(app_t *app, uint32_t now);
bool handle_service_codes_key(app_t *app, uint16_t key, uint32_t now);
bool tick_service_codes(app_t *app, uint32_t now);
void render_service_codes(const app_t *app, framebuffer_t *fb);

#endif

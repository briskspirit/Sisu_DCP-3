#ifndef APPS_CALCULATOR_APP_H
#define APPS_CALCULATOR_APP_H

#include <stdbool.h>
#include <stdint.h>

#include "app_internal.h"

void open_calculator(app_t *app, uint32_t now);
bool handle_calculator_key(app_t *app, uint16_t key, event_type_t type, uint32_t now);
bool tick_calculator(app_t *app, uint32_t now);
void render_calculator(const app_t *app, framebuffer_t *fb);

#endif

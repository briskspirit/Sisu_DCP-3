#ifndef APPS_SIM_NUMBERS_APP_H
#define APPS_SIM_NUMBERS_APP_H

#include "app_internal.h"
#include "services/sim_numbers.h"

void open_sim_numbers(app_t *app, sim_numbers_kind_t kind, uint32_t now);
bool tick_sim_numbers(app_t *app, uint32_t now);
bool handle_sim_numbers_key(app_t *app, uint16_t key, uint32_t now);
bool handle_sim_numbers_display_key(app_t *app, uint16_t key, uint32_t now);
void render_sim_numbers(const app_t *app, framebuffer_t *fb);

#endif

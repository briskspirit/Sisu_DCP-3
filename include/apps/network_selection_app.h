#ifndef APPS_NETWORK_SELECTION_APP_H
#define APPS_NETWORK_SELECTION_APP_H

#include "app_internal.h"
void open_network_selection(app_t *app, bool manual, uint32_t now);
bool handle_network_selection_display_key(app_t *app, uint16_t key, uint32_t now);
bool handle_network_selection_key(app_t *app, uint16_t key, uint32_t now);
bool tick_network_selection(app_t *app, uint32_t now);
void render_network_selection(const app_t *app, framebuffer_t *fb);

#endif

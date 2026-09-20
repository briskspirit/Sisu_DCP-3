#ifndef OPERATOR_LOGO_APP_H
#define OPERATOR_LOGO_APP_H

#include "app.h"

void open_received_operator_logo(app_t *app);
bool handle_operator_logo_key(app_t *app, uint16_t key, uint32_t now);
bool tick_operator_logo(app_t *app, uint32_t now);
void render_operator_logo(const app_t *app, framebuffer_t *fb);
void draw_operator_logo(framebuffer_t *fb, const operator_logo_t *logo, int y);
bool operator_logo_update_network(app_t *app, const modem_status_t *status);

#endif

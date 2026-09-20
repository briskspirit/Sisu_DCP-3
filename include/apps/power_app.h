#ifndef APPS_POWER_APP_H
#define APPS_POWER_APP_H

#include <stdbool.h>

#include "app_internal.h"

bool handle_power_key(app_t *app, const input_event_t *event);
bool handle_power_menu_key(app_t *app, uint16_t key, uint32_t now);
bool tick_power_off(app_t *app, uint32_t now);

/* True means the explicit request was accepted, possibly pending a bounded
 * battery qualification. Modem/codec startup waits for an allowed decision. */
bool power_on(app_t *app, uint32_t now);
void power_off(app_t *app, uint32_t now);
bool power_off_display_should_sleep(const app_t *app);
void render_power_off(const app_t *app, framebuffer_t *fb);
void render_power_menu(const app_t *app, framebuffer_t *fb);

#endif

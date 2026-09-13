#ifndef APPS_STANDBY_APP_H
#define APPS_STANDBY_APP_H

#include <stdbool.h>
#include <stdint.h>

#include "app_internal.h"

void render_standby(const app_t *app, framebuffer_t *fb);

bool handle_keyguard_key(app_t *app, uint16_t key, uint32_t now);
void lock_standby_keyguard(app_t *app, uint32_t now);
bool standby_clear_all_hold_event(const app_t *app,
                                  const input_event_t *event);
bool handle_standby_key(app_t *app, const input_event_t *event);
bool tick_standby(app_t *app, uint32_t now_ms);

void set_standby_message(app_t *app, const char *text, uint32_t now_ms);

#endif

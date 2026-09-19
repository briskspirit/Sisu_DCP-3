#ifndef APPS_POWERUP_APP_H
#define APPS_POWERUP_APP_H

#include <stdbool.h>
#include <stdint.h>

#include "app.h"
#include "ui/framebuffer.h"

void start_powerup(app_t *app, uint32_t now_ms);
void start_powerup_no_logo(app_t *app, uint32_t now_ms);

bool tick_powerup(app_t *app, uint32_t now_ms);

void render_powerup(const app_t *app, framebuffer_t *fb);
void enter_contact_service(app_t *app, uint32_t now_ms);
bool tick_contact_service(app_t *app, uint32_t now_ms);
void render_contact_service(const app_t *app, framebuffer_t *fb);

#endif

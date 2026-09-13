#ifndef APPS_MAIN_MENU_APP_H
#define APPS_MAIN_MENU_APP_H

#include <stdbool.h>
#include <stdint.h>

#include "app.h"
#include "ui/framebuffer.h"

void open_main_menu(app_t *app, uint32_t now_ms);
void open_main_menu_at(app_t *app, uint8_t selected, uint32_t now_ms);

bool handle_main_menu_key(app_t *app, uint16_t key, uint32_t now_ms);
bool tick_main_menu(app_t *app, uint32_t now_ms);

void render_main_menu(const app_t *app, framebuffer_t *fb);

#endif

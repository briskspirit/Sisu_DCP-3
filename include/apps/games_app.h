#ifndef APPS_GAMES_APP_H
#define APPS_GAMES_APP_H

#include <stdbool.h>
#include <stdint.h>

#include "app_internal.h"

void games_app_init(app_t *app);
void open_games_menu(app_t *app, uint8_t selected);

bool handle_games_menu_key(app_t *app, uint16_t key, uint32_t now);
void render_games_menu(const app_t *app, framebuffer_t *fb);

#endif

#ifndef APPS_SNAKE_APP_H
#define APPS_SNAKE_APP_H

#include <stdbool.h>
#include <stdint.h>

#include "app_internal.h"

void snake_app_init(app_t *app);
void open_snake_menu(app_t *app);

bool handle_snake_menu_key(app_t *app, uint16_t key, uint32_t now);
bool handle_snake_play_key(app_t *app, uint16_t key, uint32_t now);
bool handle_snake_level_key(app_t *app, uint16_t key, uint32_t now);
bool handle_snake_instructions_key(app_t *app, uint16_t key, uint32_t now);
bool handle_snake_message_key(app_t *app, uint16_t key, uint32_t now);
bool tick_snake(app_t *app, uint32_t now);

void render_snake_menu(const app_t *app, framebuffer_t *fb);
void render_snake_play(const app_t *app, framebuffer_t *fb);
void render_snake_level(const app_t *app, framebuffer_t *fb);
void render_snake_instructions(const app_t *app, framebuffer_t *fb);
void render_snake_top_score(const app_t *app, framebuffer_t *fb);
void render_snake_result(const app_t *app, framebuffer_t *fb);

#endif

#ifndef APPS_LOGIC_APP_H
#define APPS_LOGIC_APP_H

#include <stdbool.h>
#include <stdint.h>

#include "app_internal.h"

void logic_app_init(app_t *app);
void open_logic_menu(app_t *app);

bool handle_logic_menu_key(app_t *app, uint16_t key, uint32_t now);
bool handle_logic_play_key(app_t *app, uint16_t key, uint32_t now);
bool handle_logic_level_key(app_t *app, uint16_t key, uint32_t now);
bool handle_logic_instructions_key(app_t *app, uint16_t key, uint32_t now);
bool handle_logic_message_key(app_t *app, uint16_t key, uint32_t now);
bool tick_logic(app_t *app, uint32_t now);

void render_logic_menu(const app_t *app, framebuffer_t *fb);
void render_logic_play(const app_t *app, framebuffer_t *fb);
void render_logic_level(const app_t *app, framebuffer_t *fb);
void render_logic_instructions(const app_t *app, framebuffer_t *fb);
void render_logic_result(const app_t *app, framebuffer_t *fb);

#endif

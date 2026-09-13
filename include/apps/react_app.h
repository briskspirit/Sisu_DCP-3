#ifndef APPS_REACT_APP_H
#define APPS_REACT_APP_H

#include <stdbool.h>
#include <stdint.h>

#include "app_internal.h"

void react_app_init(app_t *app);
void open_react_menu(app_t *app);

bool handle_react_menu_key(app_t *app, uint16_t key, uint32_t now);
bool handle_react_play_key(app_t *app, uint16_t key, uint32_t now);
bool handle_react_level_key(app_t *app, uint16_t key, uint32_t now);
bool handle_react_instructions_key(app_t *app, uint16_t key, uint32_t now);
bool handle_react_message_key(app_t *app, uint16_t key, uint32_t now);
bool tick_react(app_t *app, uint32_t now);

void render_react_menu(const app_t *app, framebuffer_t *fb);
void render_react_play(const app_t *app, framebuffer_t *fb);
void render_react_level(const app_t *app, framebuffer_t *fb);
void render_react_instructions(const app_t *app, framebuffer_t *fb);
void render_react_top_score(const app_t *app, framebuffer_t *fb);
void render_react_result(const app_t *app, framebuffer_t *fb);

#endif

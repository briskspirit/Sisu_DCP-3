#ifndef APPS_ROTATION_APP_H
#define APPS_ROTATION_APP_H

#include <stdbool.h>
#include <stdint.h>

#include "app_internal.h"

void rotation_app_init(app_t *app);
void open_rotation_menu(app_t *app);

bool handle_rotation_menu_key(app_t *app, uint16_t key, uint32_t now);
bool handle_rotation_play_key(app_t *app, uint16_t key, uint32_t now);
bool handle_rotation_level_key(app_t *app, uint16_t key, uint32_t now);
bool handle_rotation_instructions_key(app_t *app, uint16_t key, uint32_t now);
bool handle_rotation_message_key(app_t *app, uint16_t key, uint32_t now);
bool tick_rotation(app_t *app, uint32_t now);

void render_rotation_menu(const app_t *app, framebuffer_t *fb);
void render_rotation_play(const app_t *app, framebuffer_t *fb);
void render_rotation_level(const app_t *app, framebuffer_t *fb);
void render_rotation_instructions(const app_t *app, framebuffer_t *fb);
void render_rotation_top_score(const app_t *app, framebuffer_t *fb);
void render_rotation_result(const app_t *app, framebuffer_t *fb);

#endif

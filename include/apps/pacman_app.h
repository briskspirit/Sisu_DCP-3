#ifndef APPS_PACMAN_APP_H
#define APPS_PACMAN_APP_H

#include <stdbool.h>
#include <stdint.h>

#include "app_internal.h"

void pacman_app_init(app_t *app);
void open_pacman_menu(app_t *app);

bool handle_pacman_menu_key(app_t *app, uint16_t key, uint32_t now);
bool handle_pacman_play_key(app_t *app, uint16_t key, uint32_t now);
bool handle_pacman_level_key(app_t *app, uint16_t key, uint32_t now);
bool handle_pacman_instructions_key(app_t *app, uint16_t key, uint32_t now);
bool handle_pacman_message_key(app_t *app, uint16_t key, uint32_t now);
bool tick_pacman(app_t *app, uint32_t now);

void render_pacman_menu(const app_t *app, framebuffer_t *fb);
void render_pacman_play(const app_t *app, framebuffer_t *fb);
void render_pacman_level(const app_t *app, framebuffer_t *fb);
void render_pacman_instructions(const app_t *app, framebuffer_t *fb);
void render_pacman_top_score(const app_t *app, framebuffer_t *fb);
void render_pacman_result(const app_t *app, framebuffer_t *fb);

#endif

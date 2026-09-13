#ifndef APPS_MEMORY_APP_H
#define APPS_MEMORY_APP_H

#include <stdbool.h>
#include <stdint.h>

#include "app_internal.h"

void memory_app_init(app_t *app);
void open_memory_menu(app_t *app);
const uint8_t *memory_game_card_sprite(uint8_t code);

bool handle_memory_menu_key(app_t *app, uint16_t key, uint32_t now);
bool handle_memory_play_key(app_t *app, uint16_t key, uint32_t now);
bool handle_memory_level_key(app_t *app, uint16_t key, uint32_t now);
bool handle_memory_instructions_key(app_t *app, uint16_t key, uint32_t now);
bool handle_memory_message_key(app_t *app, uint16_t key, uint32_t now);
bool tick_memory(app_t *app, uint32_t now);

void render_memory_menu(const app_t *app, framebuffer_t *fb);
void render_memory_play(const app_t *app, framebuffer_t *fb);
void render_memory_level(const app_t *app, framebuffer_t *fb);
void render_memory_instructions(const app_t *app, framebuffer_t *fb);
void render_memory_top_score(const app_t *app, framebuffer_t *fb);
void render_memory_result(const app_t *app, framebuffer_t *fb);

#endif

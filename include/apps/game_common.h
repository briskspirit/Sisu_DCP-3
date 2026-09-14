#ifndef APPS_GAME_COMMON_H
#define APPS_GAME_COMMON_H

#include <stdint.h>

#include "ui/framebuffer.h"

/* Shared by every game app: the active profile's "Game tones" warning
 * setting maps to an audio level (255 = off), and game sound effects are
 * fire-and-forget system tones at that level. */
uint8_t game_audio_level(void);
void play_game_system_tone(uint8_t index);

/* Game levels are stored zero-based. The stock selector presents them as a
 * rising 1..N ladder and has distinct feedback at either end stop. */
void draw_game_level_selector(framebuffer_t *fb, uint8_t draft_level, uint8_t level_count);
void adjust_game_level(uint8_t *draft_level, uint8_t level_count, int8_t direction);

#endif

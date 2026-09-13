#ifndef APPS_GAME_COMMON_H
#define APPS_GAME_COMMON_H

#include <stdint.h>

/* Shared by every game app: the active profile's "Game tones" warning
 * setting maps to an audio level (255 = off), and game sound effects are
 * fire-and-forget system tones at that level. */
uint8_t game_audio_level(void);
void play_game_system_tone(uint8_t index);

#endif

#ifndef VIBRA_HAL_H
#define VIBRA_HAL_H

#include <stdbool.h>
#include <stdint.h>

#include "audio/audio_levels.h"

#define VIBRA_HAL_STRENGTH_STOCK AUDIO_VIBRA_STRENGTH_STOCK

/* Driver for the vibra motor output. On the original NSE-8 the vibra is a
 * CBUS-controlled strength register pulsed by ringtone-bytecode control
 * markers. The clone keeps the same on/off marker timing and maps the strength
 * byte to PWM duty. Driven from core1. */
void vibra_hal_init(void);
void vibra_hal_set_strength(uint8_t strength);
void vibra_hal_set(bool on);

#endif

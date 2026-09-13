#ifndef BACKLIGHT_SERVICE_H
#define BACKLIGHT_SERVICE_H

#include <stdbool.h>
#include <stdint.h>

#define BACKLIGHT_LEVEL_MIN_PERCENT 1u
#define BACKLIGHT_LEVEL_MAX_PERCENT 100u
#define BACKLIGHT_LEVEL_DEFAULT_PERCENT 100u

void backlight_service_init(uint32_t now_ms);
void backlight_service_set_always_on(bool always_on, uint32_t now_ms);
void backlight_service_notify_activity(uint32_t now_ms);
void backlight_service_force_level(bool on);
void backlight_service_release_force(uint32_t now_ms);
void backlight_service_tick(uint32_t now_ms);
bool backlight_service_is_on(void);
bool backlight_service_set_level_percent(uint8_t level_percent);
uint8_t backlight_service_level_percent(void);

#endif

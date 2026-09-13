#ifndef APPS_CLOCK_APP_H
#define APPS_CLOCK_APP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app_internal.h"

typedef enum {
    CLOCK_MENU_ROOT = 0,
    CLOCK_MENU_ALARM,
    CLOCK_MENU_VISIBLE,
    CLOCK_MENU_HIDDEN,
    CLOCK_MENU_TIME_FORMAT,
    CLOCK_MENU_AM_PM,
} clock_menu_kind_t;

typedef enum {
    CLOCK_EDITOR_TIME = 0,
    CLOCK_EDITOR_DATE,
    CLOCK_EDITOR_ALARM,
} clock_editor_kind_t;

void render_clock_menu(const app_t *app, framebuffer_t *fb);
void render_clock_editor(const app_t *app, framebuffer_t *fb);
void render_clock_alarm(const app_t *app, framebuffer_t *fb);

bool handle_clock_menu_key(app_t *app, uint16_t key, uint32_t now);
bool handle_clock_editor_key(app_t *app, uint16_t key, uint32_t now);
bool handle_clock_alarm_key(app_t *app, uint16_t key, uint32_t now);

bool tick_clock_editor(app_t *app, uint32_t now);
/* Finalize an editor-owned wall-clock write only after the external RTC's
 * bounded retry transaction reaches COMMITTED or FAILED. */
bool poll_clock_datetime_commit(app_t *app, uint32_t now);
bool poll_clock_alarm(app_t *app, uint32_t now);
bool tick_clock_alarm(app_t *app, uint32_t now);
/* Dismiss an active snooze (Stop from the standby overlay): stop the one-shot
 * alarm and show the "Snooze off" confirmation. Snooze-active = clock_alarm_mode
 * == 3 while on the standby route. */
void dismiss_clock_snooze(app_t *app, uint32_t now);

void open_clock_root(app_t *app);
bool start_clock_boot_setup_if_needed(app_t *app, uint32_t now);
void load_clock_into_rtc(void);
void update_standby_clock(app_t *app);
void format_clock_time(char *dst, size_t cap, uint8_t hour, uint8_t minute, bool pad_hour, bool suffix);

#endif

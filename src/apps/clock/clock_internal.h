#ifndef APPS_CLOCK_INTERNAL_H
#define APPS_CLOCK_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "apps/clock_app.h"
#include "storage/store_service.h"

void clock_open_menu(app_t *app, clock_menu_kind_t kind, uint8_t selected);
void clock_editor_open(app_t *app,
                       clock_editor_kind_t kind,
                       const char *initial,
                       uint8_t cursor_digit,
                       uint32_t now);
void clock_editor_require_setup(app_t *app, uint32_t now, bool show_note);
bool clock_editor_commit_am_pm(app_t *app, const char *marker, uint32_t now);

static inline bool clock_setting_u8(store_setting_key_t key, uint8_t *value) {
    return store_setting_get_u8(key, value) == STORE_STATUS_OK;
}

void clock_format_date_preview(char *dst, size_t cap);

void clock_set_alarm(app_t *app, uint8_t hour, uint8_t minute, bool enabled);
void clock_format_alarm_preview(char *dst, size_t cap);

#endif

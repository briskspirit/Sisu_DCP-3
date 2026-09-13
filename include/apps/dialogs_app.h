#ifndef APPS_DIALOGS_APP_H
#define APPS_DIALOGS_APP_H

#include <stdbool.h>
#include <stdint.h>

#include "app_internal.h"

void render_display_message(const app_t *app, framebuffer_t *fb);
void render_confirm(const app_t *app, framebuffer_t *fb);
void render_editor(const app_t *app, framebuffer_t *fb);

bool handle_display_message_key(app_t *app, uint16_t key, uint32_t now);
bool handle_confirm_key(app_t *app, uint16_t key, uint32_t now);
bool handle_editor_key(app_t *app, uint16_t key, event_type_t event_type, uint32_t now);
bool tick_display_message(app_t *app, uint32_t now);
bool tick_editor(app_t *app, uint32_t now);

void open_display(app_t *app,
                  uint8_t record_id,
                  const char *a,
                  const char *b,
                  const char *c,
                  app_route_t return_route,
                  uint32_t now);
/* Like open_display, but resolves a single v6.00 multiline string id (SID) and
 * splits it on '\n' into up to 3 lines. `fallback` is shown when the sid has no
 * v6.00 record (clone-only message). The standard form for localized dialogs. */
void open_display_sid(app_t *app,
                      uint8_t record_id,
                      uint16_t sid,
                      const char *fallback,
                      app_route_t return_route,
                      uint32_t now);
/* open_display_sid with the v6.00 "%N" token replaced by `num`. */
void open_display_sid_num(app_t *app,
                          uint8_t record_id,
                          uint16_t sid,
                          const char *fallback,
                          unsigned num,
                          app_route_t return_route,
                          uint32_t now);
void return_from_display(app_t *app);
void open_editor(app_t *app,
                 const char *title,
                 const char *value,
                 uint8_t max_len,
                 editor_kind_t kind,
                 editor_context_t context,
                 bool show_cursor,
                 uint32_t now);
void close_editor(app_t *app);
void open_confirm(app_t *app, confirm_context_t context, const char *a, const char *b, const char *c, int8_t first_y);
/* Localized confirm: resolve `sid` (English `fallback` otherwise) and wrap into the confirm lines. */
void open_confirm_sid(app_t *app, confirm_context_t context, uint16_t sid, const char *fallback, int8_t first_y);
bool editor_delete_one(app_t *app);
const char *editor_chars_for_key(uint16_t key, editor_mode_t mode);

#endif

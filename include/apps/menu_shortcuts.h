#ifndef APPS_MENU_SHORTCUTS_H
#define APPS_MENU_SHORTCUTS_H

#include "app.h"

/* An app owns the choices and normal Navi action. The shortcut controller only
 * selects a valid row; it never duplicates a save, request, or confirmation. */
typedef struct {
    uint8_t *selected;
    uint8_t *view_start;
    uint8_t count; /* Raw one-based ordinals, at most 64. */
    uint32_t context; /* Submenu identity, excluding the selected row. */
    uint64_t hidden_mask;
    uint32_t deferred_mask; /* Stock item flag 0x04: wait before activation. */
} menu_shortcut_view_t;

bool main_menu_shortcut_view(app_t *app, menu_shortcut_view_t *view);
bool phonebook_shortcut_view(app_t *app, menu_shortcut_view_t *view);
bool messages_shortcut_view(app_t *app, menu_shortcut_view_t *view);
bool settings_shortcut_view(app_t *app, menu_shortcut_view_t *view);
bool call_register_shortcut_view(app_t *app, menu_shortcut_view_t *view);
bool call_divert_shortcut_view(app_t *app, menu_shortcut_view_t *view);
bool clock_shortcut_view(app_t *app, menu_shortcut_view_t *view);
bool tones_shortcut_view(app_t *app, menu_shortcut_view_t *view);
void tones_shortcut_selected(app_t *app, uint32_t now);
bool profiles_shortcut_view(app_t *app, menu_shortcut_view_t *view);
bool games_shortcut_view(app_t *app, menu_shortcut_view_t *view);
bool snake_shortcut_view(app_t *app, menu_shortcut_view_t *view);
bool rotation_shortcut_view(app_t *app, menu_shortcut_view_t *view);
bool memory_shortcut_view(app_t *app, menu_shortcut_view_t *view);
bool react_shortcut_view(app_t *app, menu_shortcut_view_t *view);
bool logic_shortcut_view(app_t *app, menu_shortcut_view_t *view);
bool pacman_shortcut_view(app_t *app, menu_shortcut_view_t *view);

void menu_shortcuts_begin(app_t *app, uint32_t now);
void menu_shortcuts_cancel(app_t *app);
bool menu_shortcuts_handle(app_t *app, const input_event_t *event);
bool menu_shortcuts_tick(app_t *app, uint32_t now);
void menu_shortcuts_render(const app_t *app, framebuffer_t *fb);

#endif

#ifndef APPS_PHONEBOOK_APP_H
#define APPS_PHONEBOOK_APP_H

#include <stdbool.h>
#include <stdint.h>

#include "app_internal.h"
#include "services/phonebook_types.h"
#include "storage/store_service.h"

typedef enum {
    PHONEBOOK_MENU_ROOT = 0,
    PHONEBOOK_MENU_ERASE,
    PHONEBOOK_MENU_OPTIONS,
    PHONEBOOK_MENU_TYPE_VIEW,
} phonebook_menu_kind_t;

typedef enum {
    PHONEBOOK_PENDING_NONE = 0,
    PHONEBOOK_PENDING_LIST,
    PHONEBOOK_PENDING_SEARCH,
    PHONEBOOK_PENDING_ADD,
    PHONEBOOK_PENDING_UPDATE,
    PHONEBOOK_PENDING_DELETE,
} phonebook_pending_t;

enum {
    PHONEBOOK_REQUEST_DISPLAY_RECORD_ID = 4u,
};

static inline bool phonebook_request_display_owned(const app_t *app) {
    return app != NULL &&
           app->phonebook_pending_kind != PHONEBOOK_PENDING_NONE &&
           app->phonebook_request_id != 0u &&
           app->phonebook_wait_display_request_id ==
               app->phonebook_request_id &&
           app->route == APP_ROUTE_DISPLAY_MESSAGE &&
           app->display_record_id == PHONEBOOK_REQUEST_DISPLAY_RECORD_ID;
}

static inline bool phonebook_erase_all_display_owned(const app_t *app) {
    return app != NULL && app->phonebook_erase_all_started_ms != 0u &&
           app->phonebook_wait_display_request_id != 0u &&
           app->route == APP_ROUTE_DISPLAY_MESSAGE &&
           app->display_record_id == PHONEBOOK_REQUEST_DISPLAY_RECORD_ID;
}

void render_phonebook_menu(const app_t *app, framebuffer_t *fb);
void render_phonebook_list(const app_t *app, framebuffer_t *fb);
void render_phonebook_memory(const app_t *app, framebuffer_t *fb);
void render_phonebook_tone_picker(const app_t *app, framebuffer_t *fb);
void render_phonebook_speed_dials(const app_t *app, framebuffer_t *fb);
void render_phonebook_speed_options(const app_t *app, framebuffer_t *fb);
void render_phonebook_edit_choice(const app_t *app, framebuffer_t *fb);
void render_phonebook_security(const app_t *app, framebuffer_t *fb);

bool handle_phonebook_menu_key(app_t *app, uint16_t key, uint32_t now);
bool handle_phonebook_list_key(app_t *app, uint16_t key, uint32_t now);
bool handle_phonebook_tone_key(app_t *app, uint16_t key, uint32_t now);
bool handle_phonebook_speed_key(app_t *app, uint16_t key, uint32_t now);
bool handle_phonebook_speed_options_key(app_t *app, uint16_t key, uint32_t now);
bool handle_phonebook_edit_choice_key(app_t *app, uint16_t key, uint32_t now);
bool handle_phonebook_security_key(app_t *app, uint16_t key, uint32_t now);

bool poll_phonebook(app_t *app, uint32_t now);
bool tick_phonebook_erase_all(app_t *app, uint32_t now);
bool tick_phonebook_send(app_t *app, uint32_t now);

void open_phonebook_menu(app_t *app, phonebook_menu_kind_t kind, uint8_t selected);
void start_phonebook_list(app_t *app,
                          phonebook_label_t label,
                          const char *path,
                          phonebook_context_t context,
                          uint16_t selected,
                          uint32_t now);
void start_phonebook_search(app_t *app, const char *query, uint32_t now);
void start_phonebook_add(app_t *app, const char *name, const char *number, uint32_t now);
void start_phonebook_add_with_context(app_t *app,
                                      const char *name,
                                      const char *number,
                                      phonebook_label_t label,
                                      const char *path,
                                      phonebook_context_t context,
                                      uint32_t now);
void start_phonebook_update(app_t *app, uint32_t index, const char *name, const char *number, uint32_t now);
bool start_phonebook_delete(app_t *app, uint32_t index, uint32_t now);
void show_phonebook_list(app_t *app,
                         phonebook_label_t label,
                         const char *path,
                         phonebook_context_t context,
                         uint16_t selected);
void update_phonebook_send_recipient_softkey(app_t *app);
void resolve_contact_name(const char *number, char *dst, size_t cap);

#endif

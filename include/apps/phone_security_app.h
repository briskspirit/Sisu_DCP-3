#ifndef APPS_PHONE_SECURITY_APP_H
#define APPS_PHONE_SECURITY_APP_H

#include "app_internal.h"

void open_phone_security(app_t *app, bool startup, uint32_t now);
bool tick_phone_security(app_t *app, uint32_t now);
bool phone_security_blocks_ui(const app_t *app);
bool handle_phone_security_key(app_t *app, uint16_t key, event_type_t type, uint32_t now);
void render_phone_security(const app_t *app, framebuffer_t *fb);

#endif

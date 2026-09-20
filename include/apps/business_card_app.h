#ifndef BUSINESS_CARD_APP_H
#define BUSINESS_CARD_APP_H

#include "app.h"

void open_received_business_card(app_t *app);
bool handle_business_card_key(app_t *app, uint16_t key, uint32_t now);
bool tick_business_card(app_t *app, uint32_t now);
void render_business_card(const app_t *app, framebuffer_t *fb);

#endif

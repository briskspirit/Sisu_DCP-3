#ifndef APPS_NET_MONITOR_APP_H
#define APPS_NET_MONITOR_APP_H

#include "app.h"

void net_monitor_app_init(app_t *app);
void open_net_monitor(app_t *app, uint32_t now);
bool handle_net_monitor_test_key(app_t *app, uint16_t key, uint32_t now);
bool handle_net_monitor_page_key(app_t *app, uint16_t key, uint32_t now);
bool handle_net_monitor_overlay_key(app_t *app, uint16_t key,
                                    bool key_down, uint32_t now);
bool tick_net_monitor(app_t *app, uint32_t now);
void render_net_monitor_test(const app_t *app, framebuffer_t *fb);
void render_net_monitor_page(const app_t *app, framebuffer_t *fb);
void render_net_monitor_overlay(const app_t *app, framebuffer_t *fb);

#endif

#ifndef APPS_CALL_REGISTER_APP_H
#define APPS_CALL_REGISTER_APP_H

#include <stdbool.h>
#include <stdint.h>

#include "app_internal.h"
#include "storage/store_service.h"

typedef enum {
    CALL_REGISTER_MENU_ROOT = 0,
    CALL_REGISTER_MENU_ERASE,
    CALL_REGISTER_MENU_COST_SETTINGS,
    CALL_REGISTER_MENU_PREPAID,
    CALL_REGISTER_MENU_CREDIT_INFO,
    CALL_REGISTER_MENU_RECHARGE_STATUS,
} call_register_menu_kind_t;

typedef enum {
    CALL_REGISTER_METRIC_DURATION = 0,
    CALL_REGISTER_METRIC_COSTS,
} call_register_metric_kind_t;

typedef enum {
    CALL_REGISTER_DETAIL_TIME = 0,
    CALL_REGISTER_DETAIL_NUMBER,
    CALL_REGISTER_DETAIL_METRIC,
} call_register_detail_mode_t;

void render_call_register_menu(const app_t *app, framebuffer_t *fb);
void render_call_register_list(const app_t *app, framebuffer_t *fb);
void render_call_register_options(const app_t *app, framebuffer_t *fb);
void render_call_register_detail(const app_t *app, framebuffer_t *fb);
void render_call_register_metric(const app_t *app, framebuffer_t *fb);

bool handle_call_register_menu_key(app_t *app, uint16_t key, uint32_t now);
bool handle_call_register_list_key(app_t *app, uint16_t key, uint32_t now);
bool handle_call_register_options_key(app_t *app, uint16_t key, uint32_t now);
bool handle_call_register_detail_key(app_t *app, uint16_t key, uint32_t now);
bool handle_call_register_metric_key(app_t *app, uint16_t key, uint32_t now);

void open_call_register_menu(app_t *app, call_register_menu_kind_t kind, uint8_t selected);
void open_call_register_list(app_t *app,
                             store_call_list_t kind,
                             app_route_t return_route,
                             bool navi_call,
                             uint8_t selected,
                             uint32_t now);

#endif

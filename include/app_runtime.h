#ifndef APP_RUNTIME_H
#define APP_RUNTIME_H

#include <stdbool.h>
#include <stdint.h>

#include "app.h"

void app_runtime_init(app_t *app);
bool app_runtime_tick(app_t *app, uint32_t now_ms);

#endif

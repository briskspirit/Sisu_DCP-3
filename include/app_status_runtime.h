#ifndef APP_STATUS_RUNTIME_H
#define APP_STATUS_RUNTIME_H

#include <stdbool.h>
#include <stdint.h>

#include "app.h"

void app_status_runtime_init(app_t *app);

bool poll_app_status(app_t *app, uint32_t now_ms);
bool poll_battery(app_t *app, uint32_t now_ms);
/* Maximum time powered-on dormant may sleep before the app's next required
 * timed update. UINT32_MAX means no app deadline; hardware and maintenance
 * wake sources remain armed independently. */
uint32_t app_status_next_wake_ms(const app_t *app, uint32_t now_ms);

#endif

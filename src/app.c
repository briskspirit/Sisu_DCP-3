#include "app.h"

#include <string.h>

#include "app_router.h"
#include "app_runtime.h"

void app_init(app_t *app) {
    memset(app, 0, sizeof(*app));
    app_runtime_init(app);
}

bool app_handle_event(app_t *app, const input_event_t *event) {
    return app_router_handle_event(app, event);
}

bool app_tick(app_t *app, uint32_t now_ms) {
    return app_runtime_tick(app, now_ms);
}

void app_render(const app_t *app, framebuffer_t *fb) {
    app_router_render(app, fb);
}

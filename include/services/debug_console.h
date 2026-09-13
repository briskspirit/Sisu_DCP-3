#ifndef DEBUG_CONSOLE_H
#define DEBUG_CONSOLE_H

#include "app.h"
#include "sisu_build_config.h"

#if SISU_RELEASE_BUILD
static inline void debug_console_init(void) {
}

static inline void debug_console_tick(app_t *app) {
    (void)app;
}

static inline void debug_console_set_phone_power_on(
    bool (*fn)(app_t *app, uint32_t now_ms)) {
    (void)fn;
}
#else
void debug_console_init(void);
void debug_console_tick(app_t *app);
/* Register the app layer's phone power-on entry so the console does not call
 * upward into power_app. The dev-only console still receives app_t for its
 * diagnostic commands; that direct dependency is an explicit layering-gate
 * exception. */
void debug_console_set_phone_power_on(bool (*fn)(app_t *app, uint32_t now_ms));
#endif

#endif

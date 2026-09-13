#ifndef APPS_CALLS_APP_H
#define APPS_CALLS_APP_H

#include <stdbool.h>
#include <stdint.h>

#include "app_internal.h"

typedef enum {
    CALL_PHASE_SETUP = 0,
    CALL_PHASE_CONNECTED,
} call_phase_t;

void calls_app_init(app_t *app);

void render_call(const app_t *app, framebuffer_t *fb);
void render_call_options(const app_t *app, framebuffer_t *fb);
void render_incoming_call(const app_t *app, framebuffer_t *fb);

bool handle_call_key(app_t *app, uint16_t key, uint32_t now);
bool handle_call_options_key(app_t *app, uint16_t key, uint32_t now);
bool handle_incoming_call_key(app_t *app, uint16_t key, uint32_t now);
bool calls_app_handle_hook(app_t *app, uint32_t now); /* headset hook = answer/end */
bool poll_call_runtime(app_t *app, uint32_t now);
bool tick_call(app_t *app, uint32_t now);

bool start_outgoing_call(app_t *app, const char *number, const char *name, uint32_t now, app_route_t error_route);
bool start_standby_call(app_t *app, uint32_t now);

/* In-call New-call: start_outgoing_call resets the call session, so the live
 * call's identity (it becomes the HELD leg) and any WAITING 3rd call C must
 * be captured first and re-applied after -- if C gives up while B is dialled
 * it still needs to log missed (poll_call_runtime's `!waiting &&
 * call_waiting_pending` path fires regardless of call_phase). Snapshot BEFORE
 * start_outgoing_call; restore only once the route settled on APP_ROUTE_CALL. */
typedef struct {
    char held_number[MODEM_PHONE_MAX + 1u];
    char held_name[MODEM_PHONEBOOK_NAME_MAX + 1u];
    store_call_list_t held_list;
    uint32_t held_id;
    uint32_t held_elapsed_seconds;
    char wait_number[MODEM_PHONE_MAX + 1u];
    char wait_name[MODEM_PHONEBOOK_NAME_MAX + 1u];
    bool had_waiting;
    uint8_t waiting_id;
    uint8_t waiting_generation;
    uint32_t waiting_episode;
    bool waiting_recorded;
    bool waiting_withheld;  /* C's presentation state: call_session_reset */
    bool waiting_diverted;  /* clears both, so the restore must re-apply them */
} call_newcall_preserve_t;

void call_newcall_snapshot(const app_t *app, uint32_t now, call_newcall_preserve_t *out);
void call_newcall_restore_held(app_t *app, const call_newcall_preserve_t *snap);
/* The production call-session reset: stops the call tone, then clears every
 * per-call field. Public so the preservation test drives the REAL reset the
 * snapshot must survive -- both the tone-stop linkage and the field set --
 * not a mirror of it. */
void call_session_reset(app_t *app);

#endif

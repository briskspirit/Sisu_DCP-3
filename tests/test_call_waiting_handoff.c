/* Remote foreground release while a waiting caller survives must transfer the
 * UI to that exact caller. The waiting leg may remain INCOMING/WAITING or may
 * already be ACTIVE by the next application poll; neither outcome is a missed
 * call. This harness drives the real poll_call_runtime() and records call-log
 * writes so a field-delta implementation cannot false-green. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "apps/calls_app.h"
#include "apps/profiles_app.h"
#include "audio/audio_levels.h"
#include "generated/tones.h"
#include "services/core1_services.h"
#include "services/input_keys.h"
#include "services/log.h"
#include "services/modem_service.h"
#include "storage/store_service.h"
#include "ui/ui.h"

static int s_failures;
static modem_status_t s_status;
static modem_call_snapshot_t s_call_snapshot;
static uint32_t s_now_ms;
static unsigned s_missed_writes;
static unsigned s_received_writes;
static unsigned s_dialled_updates;
static unsigned s_hold_requests;
static unsigned s_hangup_requests;
static unsigned s_waiting_reject_requests;
static bool s_release_active_pending;

static void check(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        s_failures++;
    }
}

void copy_text(char *dst, size_t cap, const char *src) {
    if (dst == NULL || cap == 0u) return;
    snprintf(dst, cap, "%s", src != NULL ? src : "");
}

int32_t time_diff_ms(uint32_t a, uint32_t b) {
    return (int32_t)(a - b);
}

uint32_t time_ms(void) {
    return s_now_ms;
}

void modem_service_get_status(modem_status_t *out) {
    *out = s_status;
}

void modem_service_get_call_snapshot(modem_call_snapshot_t *out) {
    *out = s_call_snapshot;
}

void core1_post_command(core1_cmd_t cmd, uint16_t arg) {
    (void)cmd;
    (void)arg;
}

void core1_post_audio_composer_packed(const uint8_t *data, uint16_t len,
                                      uint8_t level) {
    (void)data; (void)len; (void)level;
}

void core1_post_audio_composer_packed_loop(const uint8_t *data, uint16_t len,
                                           uint8_t level) {
    (void)data; (void)len; (void)level;
}

void core1_services_codec_set_call_volume(uint8_t level) {
    (void)level;
}

uint8_t audio_level_from_ringing_volume(uint8_t value) {
    return value;
}

uint16_t audio_arg(uint8_t code, uint8_t level) {
    return (uint16_t)code | ((uint16_t)level << 8u);
}

uint16_t audio_arg_with_marker_vibra(uint8_t code, uint8_t level,
                                     bool enabled) {
    (void)enabled;
    return audio_arg(code, level);
}

uint8_t profile_active_index(void) { return 0u; }

uint8_t profile_get_tone_setting(uint8_t profile_index,
                                 profile_setting_kind_t kind) {
    (void)profile_index;
    return kind == PROFILE_SETTING_INCOMING_ALERT ? 4u : 0u;
}

const ringtone_t *ringtone_by_value(uint8_t value) {
    (void)value;
    return NULL;
}

store_status_t store_own_tone_get(uint8_t slot,
                                  store_own_tone_t *out_tone) {
    (void)slot; (void)out_tone;
    return STORE_STATUS_NOT_FOUND;
}

void resolve_contact_name(const char *number, char *dst, size_t cap) {
    (void)number;
    if (dst != NULL && cap > 0u) dst[0] = '\0';
}

void log_write(log_level_t level, const char *tag, const char *fmt, ...) {
    (void)level; (void)tag; (void)fmt;
}

static void populate_connected_a(app_t *app) {
    memset(app, 0, sizeof(*app));
    app->route = APP_ROUTE_CALL;
    app->call_phase = CALL_PHASE_CONNECTED;
    app->call_result_armed = true;
    app->call_record_list = STORE_CALL_LIST_DIALLED;
    app->call_record_id = 41u;
    app->call_connected_ms = 1000u;
    copy_text(app->call_number, sizeof(app->call_number), "+15550000001");
    copy_text(app->call_name, sizeof(app->call_name), "Alice");
}

static void set_call_snapshot(uint8_t waiting_state,
                              bool pending_incoming) {
    memset(&s_call_snapshot, 0, sizeof(s_call_snapshot));
    s_call_snapshot.semantic_revision++;
    s_call_snapshot.pending_incoming_active = pending_incoming;
    s_call_snapshot.pending_incoming_alert_observed = pending_incoming;
    s_call_snapshot.pending_incoming_episode = 17u;
    s_call_snapshot.leg_count = 2u;
    s_call_snapshot.legs[0] = (modem_call_leg_snapshot_t){
        .id = 1u,
        .generation = 1u,
        .state = CALL_LEG_ACTIVE,
        .direction = CALL_DIR_MO,
    };
    s_call_snapshot.legs[1] = (modem_call_leg_snapshot_t){
        .id = 2u,
        .generation = 1u,
        .state = (call_leg_state_t)waiting_state,
        .direction = CALL_DIR_MT,
        .incoming_episode = 17u,
    };
}

static void present_waiting_b(app_t *app) {
    set_call_snapshot(CALL_LEG_WAITING, true);
    memset(&s_status, 0, sizeof(s_status));
    s_status.call_state = MODEM_CALL_ACTIVE;
    s_status.active_call_id = 1u;
    s_status.waiting_call = true;
    s_status.ring_active = true;
    copy_text(s_status.incoming_number, sizeof(s_status.incoming_number),
              "+15550000002");
    set_call_snapshot(CALL_LEG_INCOMING, true);
    s_call_snapshot.leg_count = 1u;
    s_call_snapshot.legs[0] = s_call_snapshot.legs[1];
    check(poll_call_runtime(app, s_now_ms),
          "waiting B opens through the real runtime poll");
    check(app->call_waiting_pending,
          "fixture captures a pending waiting caller");
}

static void reset_observations(void) {
    s_missed_writes = 0u;
    s_received_writes = 0u;
    s_dialled_updates = 0u;
    s_hold_requests = 0u;
    s_hangup_requests = 0u;
    s_waiting_reject_requests = 0u;
    s_release_active_pending = false;
}

static void test_c_rejects_only_waiting_leg(void) {
    app_t app;
    s_now_ms = 65000u;
    reset_observations();
    populate_connected_a(&app);
    present_waiting_b(&app);

    check(handle_call_key(&app, KEY_C, s_now_ms),
          "hard C is consumed on the waiting-call surface");
    check(s_waiting_reject_requests == 1u && s_hangup_requests == 0u,
          "hard C rejects B without hanging up foreground A");
    check(app.call_waiting_pending && app.call_waiting_action_pending,
          "waiting rejection remains URC-driven until B actually departs");
    check(app.route == APP_ROUTE_CALL &&
              app.call_phase == CALL_PHASE_CONNECTED &&
              strcmp(app.call_number, "+15550000001") == 0,
          "foreground A remains connected and owns the call UI");
}

static void test_c_still_ends_ordinary_call(void) {
    app_t app;
    s_now_ms = 66000u;
    reset_observations();
    populate_connected_a(&app);

    check(handle_call_key(&app, KEY_C, s_now_ms),
          "hard C is consumed on an ordinary connected call");
    check(s_hangup_requests == 1u && s_waiting_reject_requests == 0u,
          "ordinary hard C retains the session hangup behavior");
    check(app.route == APP_ROUTE_STANDBY && app.call_pending_local_hangup,
          "ordinary hard C closes the local call surface");
}

static void populate_active_b_held_a(app_t *app) {
    memset(app, 0, sizeof(*app));
    app->route = APP_ROUTE_CALL;
    app->call_phase = CALL_PHASE_CONNECTED;
    app->call_result_armed = true;
    app->call_secondary_active = true;
    app->last_second_call_held = true;
    app->last_active_call_id = 2u;
    app->call_record_list = STORE_CALL_LIST_RECEIVED;
    app->call_record_id = 42u;
    app->call_connected_ms = s_now_ms - 1000u;
    copy_text(app->call_number, sizeof(app->call_number), "+15550000002");
    copy_text(app->call_name, sizeof(app->call_name), "Bob");
    app->call_held_record_list = STORE_CALL_LIST_DIALLED;
    app->call_held_record_id = 41u;
    app->call_held_elapsed_seconds = 10u;
    copy_text(app->call_held_number, sizeof(app->call_held_number),
              "+15550000001");
    copy_text(app->call_held_name, sizeof(app->call_held_name), "Alice");
}

static void publish_sole_a(bool held) {
    memset(&s_status, 0, sizeof(s_status));
    s_status.call_state = MODEM_CALL_ACTIVE;
    s_status.active_call_id = 1u;
    s_status.call_on_hold = held;
    memset(&s_call_snapshot, 0, sizeof(s_call_snapshot));
    s_call_snapshot.leg_count = 1u;
    s_call_snapshot.legs[0] = (modem_call_leg_snapshot_t){
        .id = 1u,
        .generation = 1u,
        .state = held ? CALL_LEG_HELD : CALL_LEG_ACTIVE,
        .direction = CALL_DIR_MO,
    };
}

static void test_local_release_waits_for_owned_promotion(void) {
    app_t app;
    s_now_ms = 70000u;
    reset_observations();
    populate_active_b_held_a(&app);
    publish_sole_a(true);
    s_release_active_pending = true;

    (void)poll_call_runtime(&app, s_now_ms);
    check(app.call_survivor_retrieve_pending && s_hold_requests == 0u,
          "held survivor arms a fallback without toggling immediately");
    check(strcmp(app.call_number, "+15550000001") == 0,
          "the surviving A identity owns the call UI");

    s_now_ms += 500u;
    (void)poll_call_runtime(&app, s_now_ms);
    check(app.call_survivor_retrieve_pending && s_hold_requests == 0u,
          "unresolved release-active ownership suppresses the B6 toggle");

    s_release_active_pending = false;
    publish_sole_a(false);
    s_now_ms += 300u;
    (void)poll_call_runtime(&app, s_now_ms);
    check(!app.call_survivor_retrieve_pending && s_hold_requests == 0u &&
              !app.call_held,
          "network-owned promotion completes without a competing toggle");
}

static void test_remote_release_retains_b6_fallback(void) {
    app_t app;
    s_now_ms = 80000u;
    reset_observations();
    populate_active_b_held_a(&app);
    publish_sole_a(true);

    (void)poll_call_runtime(&app, s_now_ms);
    s_now_ms += 500u;
    (void)poll_call_runtime(&app, s_now_ms);
    check(s_hold_requests == 1u && !app.call_survivor_retrieve_pending,
          "remote active release still emits exactly one B6 retrieve toggle");
}

static void test_remote_a_release_leaves_b_ringing(void) {
    app_t app;
    s_now_ms = 10000u;
    reset_observations();
    populate_connected_a(&app);
    present_waiting_b(&app);

    s_now_ms += 500u;
    memset(&s_status, 0, sizeof(s_status));
    s_status.call_state = MODEM_CALL_RINGING;
    s_status.ring_active = true;
    copy_text(s_status.incoming_number, sizeof(s_status.incoming_number),
              "+15550000002");
    check(poll_call_runtime(&app, s_now_ms),
          "remote A release is consumed by the runtime poll");
    check(app.route == APP_ROUTE_INCOMING_CALL,
          "the surviving waiting caller becomes the ordinary incoming call");
    check(strcmp(app.call_number, "+15550000002") == 0,
          "the ringing handoff preserves B's identity");
    check(s_missed_writes == 0u && app.missed_call_pending_count == 0u,
          "B is not logged missed while it is still ringing");
    check(s_dialled_updates == 1u,
          "the departed foreground A is finalized exactly once");
}

static void test_remote_a_release_promotes_b_active(void) {
    app_t app;
    s_now_ms = 20000u;
    reset_observations();
    populate_connected_a(&app);
    present_waiting_b(&app);

    s_now_ms += 500u;
    memset(&s_status, 0, sizeof(s_status));
    s_status.call_state = MODEM_CALL_ACTIVE;
    s_status.active_call_id = 2u;
    set_call_snapshot(CALL_LEG_ACTIVE, false);
    s_call_snapshot.leg_count = 1u;
    s_call_snapshot.legs[0] = s_call_snapshot.legs[1];
    check(poll_call_runtime(&app, s_now_ms),
          "direct waiting-to-active promotion is consumed");
    check(app.route == APP_ROUTE_CALL &&
              app.call_phase == CALL_PHASE_CONNECTED,
          "the promoted waiting caller owns the connected-call surface");
    check(strcmp(app.call_number, "+15550000002") == 0,
          "the connected handoff preserves B's identity");
    check(s_missed_writes == 0u && app.missed_call_pending_count == 0u,
          "an active B is never logged missed");
    check(s_received_writes == 1u,
          "the promoted incoming caller is recorded received exactly once");
    check(s_dialled_updates == 1u,
          "the departed foreground A is finalized exactly once");
}

static void test_waiting_b_disappears_while_a_survives(void) {
    app_t app;
    s_now_ms = 30000u;
    reset_observations();
    populate_connected_a(&app);
    present_waiting_b(&app);

    s_now_ms += 500u;
    memset(&s_status, 0, sizeof(s_status));
    s_status.call_state = MODEM_CALL_ACTIVE;
    s_status.active_call_id = 1u;
    memset(&s_call_snapshot, 0, sizeof(s_call_snapshot));
    s_call_snapshot.leg_count = 1u;
    s_call_snapshot.legs[0] = (modem_call_leg_snapshot_t){
        .id = 1u, .generation = 1u, .state = CALL_LEG_ACTIVE,
        .direction = CALL_DIR_MO,
    };

    check(poll_call_runtime(&app, s_now_ms),
          "waiting-caller departure is consumed");
    check(app.route == APP_ROUTE_CALL &&
              strcmp(app.call_number, "+15550000001") == 0,
          "A remains the connected foreground call");
    check(s_missed_writes == 1u && app.missed_call_pending_count == 1u,
          "a genuinely departed B is logged missed exactly once");
    check(s_dialled_updates == 0u,
          "the surviving foreground A is not finalized");
}

static void test_reused_id_does_not_steal_waiting_identity(void) {
    app_t app;
    s_now_ms = 40000u;
    reset_observations();
    populate_connected_a(&app);
    present_waiting_b(&app); /* B owns {id=2,generation=1}, episode 17 */

    s_now_ms += 500u;
    memset(&s_status, 0, sizeof(s_status));
    s_status.call_state = MODEM_CALL_RINGING;
    s_status.ring_active = true;
    copy_text(s_status.incoming_number, sizeof(s_status.incoming_number),
              "+15550000003");
    memset(&s_call_snapshot, 0, sizeof(s_call_snapshot));
    s_call_snapshot.pending_incoming_active = true;
    s_call_snapshot.pending_incoming_alert_observed = true;
    s_call_snapshot.pending_incoming_episode = 18u;
    s_call_snapshot.leg_count = 1u;
    s_call_snapshot.legs[0] = (modem_call_leg_snapshot_t){
        .id = 2u, .generation = 2u, .state = CALL_LEG_INCOMING,
        .direction = CALL_DIR_MT, .incoming_episode = 18u,
    };

    check(poll_call_runtime(&app, s_now_ms),
          "same-id replacement is consumed");
    check(s_missed_writes == 1u && app.missed_call_pending_count == 1u,
          "departed generation-1 B is logged missed");
    check(app.route == APP_ROUTE_INCOMING_CALL &&
              strcmp(app.call_number, "+15550000003") == 0,
          "generation-2 C opens as a distinct incoming call");
    check(s_received_writes == 0u,
          "the replacement caller is not falsely recorded as answered");
    check(s_dialled_updates == 1u,
          "the concurrently departed foreground A is finalized once");
}

static void test_overflow_defers_missed_decision(void) {
    app_t app;
    s_now_ms = 50000u;
    reset_observations();
    populate_connected_a(&app);
    present_waiting_b(&app);

    s_now_ms += 500u;
    memset(&s_status, 0, sizeof(s_status));
    s_status.call_state = MODEM_CALL_ACTIVE;
    s_status.active_call_id = 1u;
    memset(&s_call_snapshot, 0, sizeof(s_call_snapshot));
    s_call_snapshot.overflow = true;
    check(!poll_call_runtime(&app, s_now_ms),
          "an overflowed identity snapshot defers the transition");
    check(app.call_waiting_pending && s_missed_writes == 0u,
          "uncertain topology cannot invent a missed call");

    s_call_snapshot.overflow = false;
    s_call_snapshot.leg_count = 1u;
    s_call_snapshot.legs[0] = (modem_call_leg_snapshot_t){
        .id = 1u, .generation = 1u, .state = CALL_LEG_ACTIVE,
        .direction = CALL_DIR_MO,
    };
    check(poll_call_runtime(&app, s_now_ms + 500u),
          "a later clean snapshot resolves the waiting departure");
    check(!app.call_waiting_pending && s_missed_writes == 1u,
          "the clean absence logs B exactly once");
}

static void test_ambiguous_episode_defers_missed_decision(void) {
    app_t app;
    s_now_ms = 60000u;
    reset_observations();
    populate_connected_a(&app);
    present_waiting_b(&app);

    memset(&s_status, 0, sizeof(s_status));
    s_status.call_state = MODEM_CALL_ACTIVE;
    s_status.active_call_id = 1u;
    memset(&s_call_snapshot, 0, sizeof(s_call_snapshot));
    s_call_snapshot.leg_count = 2u;
    s_call_snapshot.legs[0] = (modem_call_leg_snapshot_t){
        .id = 3u, .generation = 1u, .state = CALL_LEG_ACTIVE,
        .direction = CALL_DIR_MT, .incoming_episode = 17u,
    };
    s_call_snapshot.legs[1] = (modem_call_leg_snapshot_t){
        .id = 4u, .generation = 1u, .state = CALL_LEG_HELD,
        .direction = CALL_DIR_MT, .incoming_episode = 17u,
    };
    check(!poll_call_runtime(&app, s_now_ms + 500u),
          "duplicate episode ownership defers the transition");
    check(app.call_waiting_pending && s_missed_writes == 0u,
          "ambiguous identity cannot invent a missed call");
}

int main(void) {
    test_remote_a_release_leaves_b_ringing();
    test_remote_a_release_promotes_b_active();
    test_waiting_b_disappears_while_a_survives();
    test_reused_id_does_not_steal_waiting_identity();
    test_overflow_defers_missed_decision();
    test_ambiguous_episode_defers_missed_decision();
    test_c_rejects_only_waiting_leg();
    test_c_still_ends_ordinary_call();
    test_local_release_waits_for_owned_promotion();
    test_remote_release_retains_b6_fallback();
    if (s_failures != 0) {
        fprintf(stderr, "%d failures\n", s_failures);
        return 1;
    }
    puts("call_waiting_handoff tests passed");
    return 0;
}

/* ---------------- Reachable calls_app dependencies ---------------- */

store_status_t store_call_add_now(store_call_list_t list, const char *number,
                                  const char *name, uint32_t duration_seconds,
                                  store_call_reason_t reason,
                                  uint32_t *out_record_id) {
    (void)number;
    (void)name;
    (void)duration_seconds;
    (void)reason;
    if (list == STORE_CALL_LIST_MISSED) s_missed_writes++;
    if (list == STORE_CALL_LIST_RECEIVED) s_received_writes++;
    if (out_record_id != NULL) *out_record_id = 99u;
    return STORE_STATUS_OK;
}

store_status_t store_call_update_result(store_call_list_t list,
                                        uint32_t record_id,
                                        uint32_t duration_seconds,
                                        store_call_reason_t reason) {
    (void)record_id;
    (void)duration_seconds;
    (void)reason;
    if (list == STORE_CALL_LIST_DIALLED) s_dialled_updates++;
    return STORE_STATUS_OK;
}

store_status_t store_life_timer_add_seconds(uint32_t seconds) {
    (void)seconds;
    return STORE_STATUS_OK;
}

store_status_t store_setting_get_u32(store_setting_key_t key,
                                     uint32_t *out_value) {
    (void)key;
    if (out_value != NULL) *out_value = 0u;
    return STORE_STATUS_OK;
}

store_status_t store_setting_set_u32(store_setting_key_t key,
                                     uint32_t value) {
    (void)key;
    (void)value;
    return STORE_STATUS_OK;
}

store_status_t store_setting_set_u8(store_setting_key_t key, uint8_t value) {
    (void)key;
    (void)value;
    return STORE_STATUS_OK;
}

bool modem_service_phonebook_find_name(const char *number, char *out,
                                       size_t cap) {
    (void)number;
    if (out != NULL && cap > 0u) out[0] = '\0';
    return false;
}

bool modem_service_request_call_hold(void) {
    s_hold_requests++;
    return true;
}
bool modem_service_call_hold_available(void) { return false; }
bool modem_service_request_hangup(void) {
    s_hangup_requests++;
    return true;
}
bool modem_service_request_call_waiting_reject(void) {
    s_waiting_reject_requests++;
    return true;
}
bool modem_service_request_call_release_leg(uint8_t call_id) {
    (void)call_id;
    return true;
}
bool modem_service_request_dtmf(char symbol) {
    (void)symbol;
    return true;
}
bool modem_service_call_release_active_pending(void) {
    return s_release_active_pending;
}
bool modem_service_new_call_cleanup_pending(void) { return false; }
void modem_service_new_call_abandoned(void) {}

void open_display_sid(app_t *app, uint8_t record_id, uint16_t sid,
                      const char *fallback, app_route_t return_route,
                      uint32_t now) {
    (void)app; (void)record_id; (void)sid; (void)fallback;
    (void)return_route; (void)now;
}

void open_display_sid_num(app_t *app, uint8_t record_id, uint16_t sid,
                          const char *fallback, unsigned num,
                          app_route_t return_route, uint32_t now) {
    (void)app; (void)record_id; (void)sid; (void)fallback; (void)num;
    (void)return_route; (void)now;
}

const char *ts_or(uint16_t sid, const char *fallback) {
    (void)sid;
    return fallback;
}

uint16_t call_options_mask_for_state(bool active, bool held, bool waiting,
                                     bool hold_toggle_available) {
    (void)active;
    (void)held;
    (void)waiting;
    (void)hold_toggle_available;
    return 0u;
}

#include "apps/net_monitor_app.h"

#include <stddef.h>
#include <string.h>

#include "app_internal.h"
#include "apps/calls_app.h"
#include "apps/main_menu_app.h"
#include "apps/net_monitor_logic.h"
#include "apps/net_monitor_render.h"
#include "apps/standby_app.h"
#include "services/input_keys.h"
#include "services/modem_service.h"
#include "services/netmon_control_service.h"
#include "services/netmon_diag_service.h"
#include "services/timebase.h"
#include "storage/store_service.h"

#define NETMON_TEST_FIELD_Y 16
#define NETMON_TEST_FIELD_H 21
#define NETMON_SELECTOR_X 6
#define NETMON_SELECTOR_Y 0
#define NETMON_UNREAD_ENVELOPE_BITMAP_ID 9u
#define NETMON_OVERLAY_X 6
#define NETMON_OVERLAY_Y 7
#define NETMON_OVERLAY_W 72
#define NETMON_OVERLAY_H 30
#define NETMON_FRAME_ROTATION_MS 2400u
#define NETMON_STATUS_TIMEOUT_MS 1300u

/* Net Monitor runs only from core 0's serialized app tick/render path. Keep
 * its large diagnostic snapshots in module-owned scratch storage: placing all
 * three on the 4 KiB core-0 stack made an ordinary overlay render consume
 * 3.8 KiB once the snapshot getters were included. */
typedef struct {
    modem_diag_snapshot_t modem;
    netmon_local_diag_snapshot_t local;
    netmon_control_snapshot_t control;
} netmon_app_scratch_t;

static netmon_app_scratch_t s_scratch;

static uint16_t parse_selector(const app_t *app) {
    uint16_t value = 0u;
    for (uint8_t i = 0u; i < app->net_monitor_input_len; i++) {
        value = (uint16_t)(value * 10u +
                           (uint16_t)(app->net_monitor_input[i] - '0'));
    }
    return value;
}

static void set_input_to_selector(app_t *app, uint16_t selector) {
    if (selector > 99u) {
        selector = 0u;
    }
    app->net_monitor_input[0] =
        (char)('0' + (char)((selector / 10u) % 10u));
    app->net_monitor_input[1] = (char)('0' + (char)(selector % 10u));
    app->net_monitor_input[2] = '\0';
    app->net_monitor_input_len = 2u;
}

static void persist_selector(uint16_t selector) {
    (void)store_setting_set_u8(STORE_SETTING_SYSTEM_NET_MONITOR_SELECTOR,
                               (uint8_t)selector);
}

static void set_status(app_t *app, const char *text, uint32_t now_ms) {
    size_t count = text != NULL ? strlen(text) : 0u;
    if (count >= sizeof(app->net_monitor_status)) {
        count = sizeof(app->net_monitor_status) - 1u;
    }
    if (count != 0u) {
        memcpy(app->net_monitor_status, text, count);
    }
    app->net_monitor_status[count] = '\0';
    app->net_monitor_status_until_ms = now_ms + NETMON_STATUS_TIMEOUT_MS;
}

static void clear_status(app_t *app) {
    app->net_monitor_status[0] = '\0';
    app->net_monitor_status_until_ms = 0u;
}

static void advance_generation(app_t *app) {
    app->net_monitor_generation++;
    if (app->net_monitor_generation == 0u) {
        app->net_monitor_generation = 1u;
    }
}

static void cancel_modem_subscription(app_t *app) {
    if (!app->net_monitor_modem_subscribed) {
        return;
    }
    modem_service_diag_cancel(app->net_monitor_generation);
    app->net_monitor_modem_subscribed = false;
}

static void reset_page_runtime(app_t *app, uint32_t now_ms) {
    cancel_modem_subscription(app);
    netmon_control_service_set_active(NETMON_ACTION_NONE, now_ms);
    advance_generation(app);
    app->net_monitor_page_index = 0u;
    app->net_monitor_last_page_ms = now_ms;
    app->net_monitor_last_refresh_ms = 0u;
    app->net_monitor_last_local_sequence = 0u;
    app->net_monitor_last_modem_sequence = 0u;
    app->net_monitor_last_control_sequence = 0u;
    clear_status(app);
}

static void activate_selector(app_t *app, uint16_t selector,
                              uint32_t now_ms) {
    reset_page_runtime(app, now_ms);
    app->net_monitor_selector = selector;
    set_input_to_selector(app, selector);
    app->net_monitor_replace_on_digit = true;
    persist_selector(selector);
    app->dirty = true;
}

static void disable_overlay(app_t *app, uint32_t now_ms) {
    reset_page_runtime(app, now_ms);
    app->net_monitor_selector = 0u;
    set_input_to_selector(app, 0u);
    app->net_monitor_replace_on_digit = true;
    persist_selector(0u);
}

static uint32_t capabilities_for(const modem_diag_snapshot_t *modem) {
    uint32_t capabilities = NETMON_CAP_REVB2 | NETMON_CAP_LOCAL_CONTROLS |
                            NETMON_CAP_RUNTIME_EXT;
    if (modem != NULL && modem->backend_available) {
        capabilities |= NETMON_CAP_TELIT | NETMON_CAP_MODEM |
                        NETMON_CAP_TUNER | NETMON_CAP_VOICE;
        if (modem_service_maintenance_supported()) {
            capabilities |= NETMON_CAP_RADIO_CONTROL |
                            NETMON_CAP_RF_MAINTENANCE;
        }
    }
    return capabilities;
}

static netmon_surface_t current_surface(const app_t *app) {
    if (app->route == APP_ROUTE_STANDBY ||
        app->route == APP_ROUTE_NET_MONITOR_PAGE) {
        if (app->input_len != 0u || app->clock_alarm_mode == 3u ||
            (app->missed_call_pending &&
             app->missed_call_pending_count != 0u) ||
            app->sms_received_pending ||
            app->standby_message[0] != '\0' || app->sim_missing ||
            app->keyguard_locked) {
            return NETMON_SURFACE_OTHER;
        }
        return NETMON_SURFACE_STANDBY;
    }
    if (app->route == APP_ROUTE_INCOMING_CALL) {
        return NETMON_SURFACE_CALL_INCOMING;
    }
    if (app->route != APP_ROUTE_CALL) {
        return NETMON_SURFACE_OTHER;
    }
    if (app->call_waiting_pending) {
        return NETMON_SURFACE_CALL_WAITING;
    }
    if (app->call_volume_visible) {
        return NETMON_SURFACE_CALL_POPUP;
    }
    return app->call_phase == CALL_PHASE_CONNECTED
        ? NETMON_SURFACE_CALL_CONNECTED
        : NETMON_SURFACE_CALL_OUTGOING;
}

static uint32_t mix_stamp(uint32_t hash, uint32_t value) {
    return hash ^ (value + 0x9e3779b9u + (hash << 6u) + (hash >> 2u));
}

static uint32_t mix_text_stamp(uint32_t hash, const char *text,
                               size_t capacity) {
    if (text == NULL) {
        return mix_stamp(hash, 0u);
    }
    for (size_t i = 0u; i < capacity && text[i] != '\0'; i++) {
        hash = mix_stamp(hash, (uint8_t)text[i]);
    }
    return hash;
}

static uint32_t modem_view_stamp(const netmon_page_descriptor_t *page,
                                 const modem_diag_snapshot_t *modem) {
    if (page == NULL || modem == NULL) {
        return 0u;
    }
    uint32_t stamp = 0x4e4d5632u; /* "NMV2" */
    modem_diag_group_t group = netmon_page_modem_group(page);
    if (group > MODEM_DIAG_GROUP_NONE && group < MODEM_DIAG_GROUP_COUNT) {
        const modem_diag_group_meta_t *meta = &modem->group[group];
        stamp = mix_stamp(stamp, meta->sequence);
        stamp = mix_stamp(stamp, meta->last_attempt_ms);
        stamp = mix_stamp(stamp, meta->last_success_ms);
        stamp = mix_stamp(stamp, (uint32_t)meta->state);
        stamp = mix_stamp(stamp, (uint32_t)meta->last_error);
        stamp = mix_stamp(stamp, meta->consecutive_failures);
        stamp = mix_stamp(stamp, (uint32_t)meta->present_fields);
        stamp = mix_stamp(stamp, (uint32_t)(meta->present_fields >> 32u));
    }

    stamp = mix_stamp(stamp, modem->updated_ms);
    stamp = mix_stamp(stamp, modem->backend_available ? 1u : 0u);
    stamp = mix_stamp(stamp, modem->at_ready ? 1u : 0u);
    stamp = mix_stamp(stamp, modem->sim_checked ? 1u : 0u);
    stamp = mix_stamp(stamp, modem->sim_present ? 1u : 0u);
    stamp = mix_stamp(stamp, modem->sim_ready ? 1u : 0u);
    stamp = mix_stamp(stamp, modem->network_registered ? 1u : 0u);
    stamp = mix_stamp(stamp, modem->urc_count);
    stamp = mix_stamp(stamp, modem->command_errors);
    stamp = mix_stamp(stamp, modem->sms_received_count);
    stamp = mix_stamp(stamp, modem->sms_sent_count);
    stamp = mix_stamp(stamp, modem->sms_storage_full_events);

    stamp = mix_stamp(stamp, modem->scheduler.generation);
    stamp = mix_stamp(stamp, (uint32_t)modem->scheduler.selected_group);
    stamp = mix_stamp(stamp, (uint32_t)modem->scheduler.active_group);
    stamp = mix_stamp(stamp, modem->scheduler.active_query);
    stamp = mix_stamp(stamp, modem->scheduler.active_query_count);
    stamp = mix_stamp(stamp, modem->scheduler.pending ? 1u : 0u);
    stamp = mix_stamp(stamp, modem->scheduler.admissions);
    stamp = mix_stamp(stamp, modem->scheduler.coalesced);
    stamp = mix_stamp(stamp, modem->scheduler.cancelled);
    stamp = mix_stamp(stamp, modem->scheduler.completed);
    stamp = mix_stamp(stamp, modem->scheduler.command_failures);
    stamp = mix_stamp(stamp, modem->scheduler.command_timeouts);
    stamp = mix_stamp(stamp, modem->scheduler.malformed_lines);
    stamp = mix_stamp(stamp, modem->scheduler.max_command_latency_ms);
    stamp = mix_stamp(stamp, modem->scheduler.normal_queue_depth);
    stamp = mix_stamp(stamp, modem->scheduler.normal_queue_high_water);
    stamp = mix_stamp(stamp,
                      modem->scheduler.normal_queue_admission_failures);
    stamp = mix_stamp(stamp, modem->scheduler.normal_queue_evictions);

    stamp = mix_stamp(stamp, modem->transport.rx_bytes);
    stamp = mix_stamp(stamp, modem->transport.rx_overruns);
    stamp = mix_stamp(stamp, modem->transport.rx_dropped);
    stamp = mix_stamp(stamp, modem->transport.rx_line_errors);
    stamp = mix_stamp(stamp, modem->transport.tx_stall_drops);
    stamp = mix_stamp(stamp,
                      modem->transport.dtr_sleep_permitted ? 1u : 0u);
    stamp = mix_stamp(stamp, modem->transport.dtr_wake_pending ? 1u : 0u);
    stamp = mix_stamp(stamp, modem->transport.ri_release_pending ? 1u : 0u);
    stamp = mix_stamp(stamp, modem->transport.cts_asserted ? 1u : 0u);
    stamp = mix_stamp(stamp, modem->transport.ri_asserted ? 1u : 0u);
    stamp = mix_stamp(stamp, modem->transport.sleep_entries);
    stamp = mix_stamp(stamp, modem->transport.wake_attempts);
    stamp = mix_stamp(stamp, modem->transport.wake_timeouts);
    stamp = mix_stamp(stamp, modem->transport.ri_release_timeouts);
    stamp = mix_stamp(stamp, modem->transport.last_wake_latency_ms);
    stamp = mix_stamp(stamp, modem->transport.max_wake_latency_ms);

    stamp = mix_stamp(stamp, modem->runtime.state);
    stamp = mix_stamp(stamp, modem->runtime.active_kind);
    stamp = mix_stamp(stamp, modem->runtime.operation);
    stamp = mix_stamp(stamp, modem->runtime.init_index);
    stamp = mix_stamp(stamp, modem->runtime.init_retries);
    stamp = mix_stamp(stamp, modem->runtime.provision_index);
    stamp = mix_stamp(stamp, modem->runtime.provision_phase);
    stamp = mix_stamp(stamp, modem->runtime.provision_retries);
    stamp = mix_stamp(stamp,
                      modem->runtime.provisioning_verified ? 1u : 0u);
    stamp = mix_stamp(stamp, modem->runtime.provisioning_schema);
    stamp = mix_stamp(stamp, modem->runtime.audio_init_ok ? 1u : 0u);
    stamp = mix_stamp(stamp, modem->runtime.sms_init_ok ? 1u : 0u);
    stamp = mix_stamp(stamp, modem->runtime.power_on_requests);
    stamp = mix_stamp(stamp, modem->runtime.power_on_starts);
    stamp = mix_stamp(stamp, modem->runtime.ready_entries);
    stamp = mix_stamp(stamp, modem->runtime.power_off_requests);
    stamp = mix_stamp(stamp, modem->runtime.shutdown_completions);
    stamp = mix_stamp(stamp, modem->runtime.automatic_recoveries);
    stamp = mix_stamp(stamp, modem->runtime.controlled_restarts);
    stamp = mix_stamp(stamp, modem->runtime.power_failures);
    stamp = mix_stamp(stamp, modem->runtime.last_transition_ms);
    stamp = mix_stamp(stamp, modem->runtime.last_recovery_reason);
    stamp = mix_text_stamp(stamp, modem->runtime.last_command,
                           sizeof(modem->runtime.last_command));
    stamp = mix_text_stamp(stamp, modem->runtime.last_line,
                           sizeof(modem->runtime.last_line));

    stamp = mix_stamp(stamp, modem->calls.terminal_sequence);
    stamp = mix_stamp(stamp, (uint32_t)modem->calls.projected_state);
    stamp = mix_stamp(stamp, (uint32_t)modem->calls.last_result);
    stamp = mix_stamp(stamp, (uint32_t)modem->calls.second_result);
    stamp = mix_stamp(stamp, (uint32_t)modem->calls.active_call_id);
    stamp = mix_stamp(stamp, modem->calls.ringing ? 1u : 0u);
    stamp = mix_stamp(stamp, modem->calls.waiting ? 1u : 0u);
    stamp = mix_stamp(stamp, modem->calls.on_hold ? 1u : 0u);
    stamp = mix_stamp(stamp, modem->calls.second_held ? 1u : 0u);
    stamp = mix_stamp(stamp, modem->calls.wants_clcc ? 1u : 0u);
    stamp = mix_stamp(stamp, (uint32_t)modem->calls.leg_count);
    for (uint8_t i = 0u; i < modem->calls.leg_count &&
         i < MODEM_DIAG_CALL_LEGS_MAX; i++) {
        const modem_diag_call_leg_t *leg = &modem->calls.legs[i];
        stamp = mix_stamp(stamp, leg->id);
        stamp = mix_stamp(stamp, leg->generation);
        stamp = mix_stamp(stamp, leg->direction);
        stamp = mix_stamp(stamp, leg->state);
        stamp = mix_stamp(stamp, leg->role);
    }
    stamp = mix_stamp(stamp, (uint32_t)modem->calls.txn_count);
    for (uint8_t i = 0u; i < modem->calls.txn_count &&
         i < MODEM_DIAG_CALL_TXNS_MAX; i++) {
        const modem_diag_call_txn_t *txn = &modem->calls.txns[i];
        stamp = mix_stamp(stamp, txn->token);
        stamp = mix_stamp(stamp, txn->kind);
        stamp = mix_stamp(stamp, txn->state);
        stamp = mix_stamp(stamp, txn->target_id);
        stamp = mix_stamp(stamp, txn->target_generation);
    }
    stamp = mix_stamp(stamp, modem->calls.terminal_count);
    return stamp;
}

static netmon_control_key_t control_key_for(netmon_key_t key) {
    switch (key) {
    case NETMON_KEY_0: return NETMON_CONTROL_KEY_0;
    case NETMON_KEY_1: return NETMON_CONTROL_KEY_1;
    case NETMON_KEY_2: return NETMON_CONTROL_KEY_2;
    case NETMON_KEY_3: return NETMON_CONTROL_KEY_3;
    case NETMON_KEY_4: return NETMON_CONTROL_KEY_4;
    case NETMON_KEY_5: return NETMON_CONTROL_KEY_5;
    case NETMON_KEY_6: return NETMON_CONTROL_KEY_6;
    case NETMON_KEY_7: return NETMON_CONTROL_KEY_7;
    case NETMON_KEY_8: return NETMON_CONTROL_KEY_8;
    case NETMON_KEY_9: return NETMON_CONTROL_KEY_9;
    case NETMON_KEY_STAR: return NETMON_CONTROL_KEY_STAR;
    case NETMON_KEY_HASH: return NETMON_CONTROL_KEY_HASH;
    case NETMON_KEY_OTHER:
    case NETMON_KEY_UP:
    case NETMON_KEY_DOWN:
    case NETMON_KEY_NAVI:
    case NETMON_KEY_C:
    case NETMON_KEY_POWER:
    default: return NETMON_CONTROL_KEY_INVALID;
    }
}

void net_monitor_app_init(app_t *app) {
    uint8_t schema = 0u;
    uint8_t selector = 0u;
    bool reset = !netmon_registry_validate() ||
        store_setting_get_u8(STORE_SETTING_SYSTEM_NET_MONITOR_SCHEMA,
                             &schema) != STORE_STATUS_OK ||
        schema != NETMON_SCHEMA_VERSION;
    if (!reset) {
        reset = store_setting_get_u8(
                    STORE_SETTING_SYSTEM_NET_MONITOR_SELECTOR, &selector) !=
                    STORE_STATUS_OK ||
                (selector != 0u && netmon_registry_find(selector) == NULL);
    }
    if (reset) {
        selector = 0u;
        (void)store_setting_set_u8(
            STORE_SETTING_SYSTEM_NET_MONITOR_SCHEMA,
            NETMON_SCHEMA_VERSION);
        persist_selector(0u);
    }

    app->net_monitor_selector = selector;
    set_input_to_selector(app, selector);
    app->net_monitor_replace_on_digit = true;
    app->net_monitor_page_index = 0u;
    app->net_monitor_last_page_ms = 0u;
    app->net_monitor_last_refresh_ms = 0u;
    app->net_monitor_generation = 1u;
    app->net_monitor_last_local_sequence = 0u;
    app->net_monitor_last_modem_sequence = 0u;
    app->net_monitor_last_control_sequence = 0u;
    app->net_monitor_surface = (uint8_t)NETMON_SURFACE_OTHER;
    app->net_monitor_modem_subscribed = false;
    clear_status(app);
    netmon_control_service_init(time_ms());
}

void open_net_monitor(app_t *app, uint32_t now_ms) {
    cancel_modem_subscription(app);
    netmon_control_service_set_active(NETMON_ACTION_NONE, now_ms);
    app->route = APP_ROUTE_NET_MONITOR_TEST;
    set_input_to_selector(app, app->net_monitor_selector);
    app->net_monitor_replace_on_digit = true;
    app->net_monitor_page_index = 0u;
    app->net_monitor_last_page_ms = now_ms;
    clear_status(app);
    app->dirty = true;
}

bool handle_net_monitor_test_key(app_t *app, uint16_t key, uint32_t now_ms) {
    if (key == INPUT_KEY_C) {
        if (app->net_monitor_input_len != 0u) {
            app->net_monitor_input_len--;
            app->net_monitor_input[app->net_monitor_input_len] = '\0';
            app->net_monitor_replace_on_digit = false;
            app->dirty = true;
            return true;
        }
        open_main_menu_at(app, 10u, now_ms);
        return true;
    }
    if (key == INPUT_KEY_NAVI) {
        uint16_t selector = parse_selector(app);
        if (selector == 0u) {
            disable_overlay(app, now_ms);
            app->route = APP_ROUTE_STANDBY;
            app->dirty = true;
            return true;
        }
        if (selector <= 99u && netmon_registry_find(selector) != NULL) {
            activate_selector(app, selector, now_ms);
        } else {
            set_input_to_selector(app, app->net_monitor_selector);
            app->net_monitor_replace_on_digit = true;
            set_status(app, "NO TEST", now_ms);
        }
        app->route = APP_ROUTE_STANDBY;
        app->dirty = true;
        return true;
    }

    netmon_key_t normalized = netmon_key_from_code(key);
    if (normalized >= NETMON_KEY_0 && normalized <= NETMON_KEY_9) {
        if (app->net_monitor_replace_on_digit) {
            app->net_monitor_input_len = 0u;
            app->net_monitor_input[0] = '\0';
            app->net_monitor_replace_on_digit = false;
        }
        if (app->net_monitor_input_len < 2u) {
            app->net_monitor_input[app->net_monitor_input_len++] =
                (char)('0' + (char)(normalized - NETMON_KEY_0));
            app->net_monitor_input[app->net_monitor_input_len] = '\0';
            app->dirty = true;
        }
    }
    return true;
}

bool handle_net_monitor_overlay_key(app_t *app, uint16_t key,
                                    bool key_down, uint32_t now_ms) {
    const netmon_page_descriptor_t *page =
        netmon_registry_find(app->net_monitor_selector);
    if (page == NULL) {
        return false;
    }

    modem_service_get_diag_snapshot(&s_scratch.modem);
    uint32_t capabilities = capabilities_for(&s_scratch.modem);
    netmon_key_t normalized = netmon_key_from_code(key);
    netmon_key_disposition_t disposition = netmon_key_disposition(
        page, current_surface(app), key_down, normalized);
    if (disposition == NETMON_KEY_BROWSE_PREVIOUS ||
        disposition == NETMON_KEY_BROWSE_NEXT) {
        uint16_t next = netmon_adjacent_selector(
            app->net_monitor_selector,
            disposition == NETMON_KEY_BROWSE_NEXT, capabilities);
        if (next != 0u) {
            activate_selector(app, next, now_ms);
        }
        return true;
    }
    if (disposition == NETMON_KEY_CONSUME) {
        return true;
    }
    if (disposition != NETMON_KEY_EDIT ||
        !netmon_page_supported(page, capabilities)) {
        return false;
    }

    netmon_control_service_set_active(page->action, now_ms);
    (void)netmon_control_service_execute(
        page->action, control_key_for(normalized), now_ms);
    app->dirty = true;
    return true;
}

bool handle_net_monitor_page_key(app_t *app, uint16_t key, uint32_t now_ms) {
    if (key == INPUT_KEY_NAVI) {
        open_main_menu(app, now_ms);
    }
    return true;
}

bool tick_net_monitor(app_t *app, uint32_t now_ms) {
    bool changed = false;
    netmon_control_service_poll(now_ms);

    const netmon_page_descriptor_t *page =
        netmon_registry_find(app->net_monitor_selector);
    if (page == NULL) {
        if (app->net_monitor_modem_subscribed) {
            cancel_modem_subscription(app);
        }
        netmon_control_service_set_active(NETMON_ACTION_NONE, now_ms);
        return false;
    }

    const modem_diag_snapshot_t *modem_view = NULL;
    if (page->provider != NETMON_PROVIDER_LOCAL) {
        modem_service_get_diag_snapshot(&s_scratch.modem);
        modem_view = &s_scratch.modem;
    }
    uint32_t capabilities = capabilities_for(modem_view);
    netmon_surface_t surface = current_surface(app);
    if (app->net_monitor_surface != (uint8_t)surface) {
        app->net_monitor_surface = (uint8_t)surface;
        changed = true;
    }

    bool supported = netmon_page_supported(page, capabilities);
    netmon_action_t action = NETMON_ACTION_NONE;
    if (supported && surface == NETMON_SURFACE_STANDBY &&
        (page->behavior_flags & NETMON_PAGE_EDITABLE) != 0u) {
        action = page->action;
    }
    netmon_control_service_set_active(action, now_ms);

    bool query_allowed = supported &&
        surface == NETMON_SURFACE_STANDBY &&
        page->provider == NETMON_PROVIDER_MODEM_QUERY;
    modem_diag_group_t group = netmon_page_modem_group(page);
    if (!query_allowed || group == MODEM_DIAG_GROUP_NONE) {
        if (app->net_monitor_modem_subscribed) {
            cancel_modem_subscription(app);
            changed = true;
        }
    } else {
        bool due = !app->net_monitor_modem_subscribed;
        if (!due && page->refresh_period_ms != 0u) {
            due = time_diff_ms(now_ms,
                app->net_monitor_last_refresh_ms +
                page->refresh_period_ms) >= 0;
        }
        if (due) {
            (void)modem_service_diag_select(
                group, app->net_monitor_generation, true);
            app->net_monitor_modem_subscribed = true;
            app->net_monitor_last_refresh_ms = now_ms;
            changed = true;
        }
    }

    uint32_t local_sequence = 0u;
    if (page->provider == NETMON_PROVIDER_LOCAL) {
        netmon_diag_service_get_snapshot(&s_scratch.local);
        local_sequence = s_scratch.local.sequence;
    }
    if (local_sequence != app->net_monitor_last_local_sequence) {
        app->net_monitor_last_local_sequence = local_sequence;
        changed = true;
    }

    uint32_t modem_sequence =
        page != NULL && page->provider != NETMON_PROVIDER_LOCAL
            ? modem_view_stamp(page, modem_view) : 0u;
    if (modem_sequence != app->net_monitor_last_modem_sequence) {
        app->net_monitor_last_modem_sequence = modem_sequence;
        changed = true;
    }

    uint32_t control_sequence = 0u;
    if ((page->behavior_flags & NETMON_PAGE_EDITABLE) != 0u) {
        netmon_control_service_get_snapshot(&s_scratch.control);
        control_sequence = s_scratch.control.sequence;
    }
    if (control_sequence != app->net_monitor_last_control_sequence) {
        app->net_monitor_last_control_sequence = control_sequence;
        changed = true;
    }

    bool visible = netmon_overlay_visible(page, surface);
    uint8_t frame_count = netmon_frame_count(page, modem_view);
    if (!visible || frame_count <= 1u) {
        if (app->net_monitor_page_index != 0u) {
            app->net_monitor_page_index = 0u;
            changed = true;
        }
        if (visible && app->net_monitor_last_page_ms == 0u) {
            app->net_monitor_last_page_ms = now_ms;
        }
    } else if (app->net_monitor_last_page_ms == 0u) {
        app->net_monitor_last_page_ms = now_ms;
    } else if (time_diff_ms(now_ms,
                   app->net_monitor_last_page_ms +
                   NETMON_FRAME_ROTATION_MS) >= 0) {
        app->net_monitor_last_page_ms = now_ms;
        app->net_monitor_page_index =
            (uint8_t)((app->net_monitor_page_index + 1u) % frame_count);
        changed = true;
    }

    if (app->net_monitor_status_until_ms != 0u &&
        time_diff_ms(now_ms, app->net_monitor_status_until_ms) >= 0) {
        clear_status(app);
        changed = true;
    }
    return changed;
}

void render_net_monitor_test(const app_t *app, framebuffer_t *fb) {
    fb_clear(fb, false);
    fb_bitmap(fb, 14u, 0, 0, true, true);
    fb_text(fb, asset_font(FONT_FS2), "Test:", 0, 7, true, 80);
    fb_rect(fb, 0, NETMON_TEST_FIELD_Y, 84, NETMON_TEST_FIELD_H, true);
    draw_right_text_box(fb, asset_font(FONT_FS0), app->net_monitor_input,
                        2, 23, 80);
    draw_softkey(fb, "OK");
}

void render_net_monitor_page(const app_t *app, framebuffer_t *fb) {
    render_standby(app, fb);
}

void render_net_monitor_overlay(const app_t *app, framebuffer_t *fb) {
    const netmon_page_descriptor_t *page =
        netmon_registry_find(app->net_monitor_selector);
    netmon_surface_t surface = current_surface(app);
    if (!netmon_overlay_visible(page, surface)) {
        return;
    }

    modem_service_get_diag_snapshot(&s_scratch.modem);
    netmon_diag_service_get_snapshot(&s_scratch.local);
    netmon_control_service_get_snapshot(&s_scratch.control);

    fb_fill_rect(fb, NETMON_OVERLAY_X, NETMON_OVERLAY_Y,
                 NETMON_OVERLAY_W, NETMON_OVERLAY_H, false);
    char selector[3];
    selector[0] = (char)('0' + (char)(page->id / 10u));
    selector[1] = (char)('0' + (char)(page->id % 10u));
    selector[2] = '\0';

    const font_t *selector_font = asset_font(FONT_FS3);
    int selector_backdrop_width = asset_text_width(selector_font, selector);
    int selector_backdrop_height =
        selector_font != NULL ? selector_font->height : 0;
    if (app->sms_unread_count > 0u) {
        /* The standby envelope shares the selector's origin. Erase its full
         * slot so the wider 15x7 icon cannot survive around the 12x6 text. */
        const bitmap_t *envelope =
            asset_bitmap(NETMON_UNREAD_ENVELOPE_BITMAP_ID);
        if (envelope != NULL) {
            if ((int)envelope->width > selector_backdrop_width) {
                selector_backdrop_width = envelope->width;
            }
            if ((int)envelope->height > selector_backdrop_height) {
                selector_backdrop_height = envelope->height;
            }
        }
    }
    fb_fill_rect(fb, NETMON_SELECTOR_X, NETMON_SELECTOR_Y,
                 selector_backdrop_width, selector_backdrop_height, false);
    int selector_width = fb_text(fb, selector_font, selector,
                                 NETMON_SELECTOR_X, NETMON_SELECTOR_Y,
                                 true, 24);

    uint32_t now_ms = time_ms();
    if (app->net_monitor_status[0] != '\0' &&
        app->net_monitor_status_until_ms != 0u &&
        time_diff_ms(now_ms, app->net_monitor_status_until_ms) < 0) {
        draw_center_text_box(fb, asset_font(FONT_FS4),
                             app->net_monitor_status,
                             NETMON_OVERLAY_X, NETMON_OVERLAY_Y + 7,
                             NETMON_OVERLAY_W);
        return;
    }

    netmon_frame_t frame;
    netmon_format_frame(page, app->net_monitor_page_index,
                        capabilities_for(&s_scratch.modem), now_ms,
                        &s_scratch.local, &s_scratch.modem,
                        &s_scratch.control, &frame);
    if (frame.freshness_marker != ' ') {
        const char marker[2] = {frame.freshness_marker, '\0'};
        fb_text(fb, asset_font(FONT_FS4), marker,
                NETMON_SELECTOR_X + selector_width + 2,
                NETMON_SELECTOR_Y, true, 6);
    }
    for (uint8_t row = 0u; row < NETMON_FRAME_LINE_COUNT; row++) {
        if (frame.lines[row][0] == '\0') {
            continue;
        }
        fb_text(fb, asset_font(FONT_FS4), frame.lines[row],
                NETMON_OVERLAY_X, NETMON_OVERLAY_Y + (int)row * 7,
                true, NETMON_OVERLAY_W);
    }
}

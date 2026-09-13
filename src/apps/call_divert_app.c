#include "apps/call_divert_app.h"

#include <stdio.h>
#include <string.h>

#include "apps/dialogs_app.h"
#include "apps/main_menu_app.h"
#include "services/input_keys.h"
#include "services/modem_service.h"
#include "storage/store_service.h"
#include "services/strings.h"

#define CALL_DIVERT_DEFAULT_DELAY_SECONDS 20u

#define CALL_DIVERT_PENDING_NONE 0u
#define CALL_DIVERT_PENDING_ACTIVATE 1u
#define CALL_DIVERT_PENDING_CANCEL 2u
#define CALL_DIVERT_PENDING_STATUS 3u
#define CALL_DIVERT_PENDING_CANCEL_ALL 4u

typedef struct {
    const char *label;   /* English fallback + documentation (also the page text) */
    uint16_t sid;        /* v6.00 string id; 0 = no exact 1:1 match -> English literal */
    call_forward_reason_t reason;
    bool has_options;
    bool has_delay;
} call_divert_condition_t;

static const call_divert_condition_t CONDITIONS[] = {
    {"Divert all\nvoice calls\nwithout ringing", 0x124u,
     CALL_FORWARD_REASON_UNCONDITIONAL, true, false},
    {"Divert\nwhen busy", 0x12bu, CALL_FORWARD_REASON_BUSY, true, false},
    {"Divert\nwhen not answered", 0x129u,
     CALL_FORWARD_REASON_NO_REPLY, true, true},
    {"Divert when\nphone off\nor no coverage", 0x12au,
     CALL_FORWARD_REASON_NOT_REACHABLE, true, false},
    /* ROM [293] is single-line "Divert when off, no answer, or no coverage" (the
     * clone hand-split it with '\n'). Binding the SID renders the ROM's exact
     * per-language single line, which the renderer auto-wraps -> 1:1. */
    {"Divert when off,\nno answer, or\nno coverage", 0x125u,
     CALL_FORWARD_REASON_ALL_CONDITIONAL, true, true},
    /* ROM [167] is single-line "Cancel all diverts"; the SID renders it 1:1. */
    {"Cancel all\ndiverts", 0xa7u, CALL_FORWARD_REASON_ALL, false, false},
};

/* Menu option label + its v6.00 SID. The English literal stays the fallback (and
 * documentation); render localizes via ts_or (sid 0 -> English literal, for text
 * that is ambiguous across contexts). Dispatch is always by ordinal
 * (apply_*_option / menu_selected), never by label text, so this is behavior-safe. */
typedef struct {
    const char *label;
    uint16_t sid;
} divert_label_t;

/* Call Divert's own action set = Activate 0x3b / Cancel 0xa8 / Status 0xca
 * (distinct from Call Waiting's 0x3c/0xa9/0x3aa -- see memory v600 loc). */
static const divert_label_t CONDITION_OPTIONS[] = {
    {"Activate", 0x3bu},
    {"Cancel", 0xa8u},
    {"Status", 0xcau},
};
static const divert_label_t CONDITION_DELAY_OPTIONS[] = {
    {"Activate", 0x3bu}, {"Cancel", 0xa8u}, {"Status", 0xcau}, {"Set delay", 0x150u},
};
/* SET.2: condition 7 (svc 004) has no Status row — descriptor 0x002cfea0 has
 * exactly Activate/Cancel/Set delay; only cond2 (svc 61) carries Status. */
static const divert_label_t CONDITION_DELAY_NO_STATUS_OPTIONS[] = {
    {"Activate", 0x3bu}, {"Cancel", 0xa8u}, {"Set delay", 0x150u},
};
static const divert_label_t ACTIVATE_OPTIONS[] = {
    {"Voice mailbox", 0x12eu},  /* divert target, adjacent to "Other number" 0x12d */
    {"Other number", 0x12du},
};
static const divert_label_t DELAY_OPTIONS[] = {
    {"5 seconds", 0x156u},
    {"10 seconds", 0x151u},
    {"15 seconds", 0x152u},
    {"20 seconds", 0x153u},
    {"25 seconds", 0x154u},
    {"30 seconds", 0x155u},
};
static const uint8_t DELAY_VALUES[] = {5u, 10u, 15u, 20u, 25u, 30u};

static uint8_t condition_count(void);
static const call_divert_condition_t *selected_condition(const app_t *app);
static const divert_label_t *menu_labels(call_divert_menu_kind_t kind, const app_t *app);
static uint8_t menu_count(call_divert_menu_kind_t kind, const app_t *app);
static const char *breadcrumb(const app_t *app, char *scratch, size_t cap);
static void select_condition(app_t *app, uint8_t index);
static bool open_request(app_t *app, uint8_t action, uint8_t condition_index,
                         const call_forward_request_t *request,
                         app_route_t return_route, uint32_t now);
static void finish_request(app_t *app, const call_forward_result_t *result,
                           uint32_t now);
static void show_request_failure(app_t *app, call_forward_outcome_t outcome,
                                 uint32_t now);
static void request_not_confirmed(app_t *app, uint32_t now);
static const char *active_number(const app_t *app, uint8_t condition_index);
static void show_status_detail(app_t *app, uint8_t index, uint32_t now);
static void clear_status_detail(app_t *app);
static void return_from_status_detail(app_t *app);
static void format_status_number(char *dst, size_t cap,
                                 const char *number);
static bool condition_uses_delay(uint8_t condition_index);
static bool delay_value_valid(uint8_t delay_seconds);
static uint8_t condition_index_for_reason(call_forward_reason_t reason);
static uint8_t pending_action_for_request(const call_forward_request_t *request);
static uint8_t delay_selected_index(const app_t *app);
static void apply_delay(app_t *app, uint32_t now);
static void apply_activate_option(app_t *app, uint32_t now);
static void apply_condition_option(app_t *app, uint32_t now);
static bool voice_mailbox_number(char *dst, size_t cap);
static void load_call_divert_state(app_t *app);
static void save_call_divert_state(const app_t *app);
static bool result_can_present(const app_t *app);

void open_call_divert_menu(app_t *app, call_divert_menu_kind_t kind, uint8_t selected) {
    load_call_divert_state(app);
    app->route = APP_ROUTE_CALL_DIVERT_MENU;
    app->call_divert_menu_kind = (uint8_t)kind;
    app->call_divert_menu_selected = selected;
    if (app->call_divert_delay_seconds == 0u) {
        app->call_divert_delay_seconds = CALL_DIVERT_DEFAULT_DELAY_SECONDS;
    }
    app->dirty = true;
}

bool handle_call_divert_menu_key(app_t *app, uint16_t key, uint32_t now) {
    call_divert_menu_kind_t kind = (call_divert_menu_kind_t)app->call_divert_menu_kind;
    uint8_t count = menu_count(kind, app);
    if (count == 0u) {
        return true;
    }
    if (app->call_divert_menu_selected >= count) {
        app->call_divert_menu_selected = 0u;
    }

    if (key == KEY_UP) {
        app->call_divert_menu_selected = app->call_divert_menu_selected == 0u
            ? (uint8_t)(count - 1u)
            : (uint8_t)(app->call_divert_menu_selected - 1u);
        app->dirty = true;
        return true;
    }
    if (key == KEY_DOWN) {
        app->call_divert_menu_selected = (uint8_t)((app->call_divert_menu_selected + 1u) % count);
        app->dirty = true;
        return true;
    }
    if (key == KEY_C) {
        if (kind == CALL_DIVERT_MENU_ROOT) {
            open_main_menu_at(app, 4u, now);
        } else if (kind == CALL_DIVERT_MENU_CONDITION) {
            open_call_divert_menu(app, CALL_DIVERT_MENU_ROOT, app->call_divert_condition_index);
        } else if (kind == CALL_DIVERT_MENU_ACTIVATE) {
            open_call_divert_menu(app, CALL_DIVERT_MENU_CONDITION, 0u);
        } else {
            open_call_divert_menu(app, CALL_DIVERT_MENU_CONDITION, app->call_divert_parent_selected);
        }
        return true;
    }
    if (key != KEY_NAVI) {
        return true;
    }

    if (kind == CALL_DIVERT_MENU_ROOT) {
        select_condition(app, app->call_divert_menu_selected);
        const call_divert_condition_t *condition = selected_condition(app);
        if (condition == 0) {
            return true;
        }
        if (!condition->has_options) {
            call_forward_request_t request;
            memset(&request, 0, sizeof(request));
            request.reason = CALL_FORWARD_REASON_ALL;
            request.action = CALL_FORWARD_ACTION_ERASE;
            (void)open_request(app, CALL_DIVERT_PENDING_CANCEL_ALL,
                               app->call_divert_condition_index, &request,
                               APP_ROUTE_CALL_DIVERT_MENU, now);
            return true;
        }
        open_call_divert_menu(app, CALL_DIVERT_MENU_CONDITION, 0u);
        return true;
    }
    if (kind == CALL_DIVERT_MENU_CONDITION) {
        apply_condition_option(app, now);
        return true;
    }
    if (kind == CALL_DIVERT_MENU_ACTIVATE) {
        apply_activate_option(app, now);
        return true;
    }
    if (kind == CALL_DIVERT_MENU_DELAY) {
        apply_delay(app, now);
        return true;
    }
    return true;
}

bool handle_call_divert_display_key(app_t *app, uint16_t key, uint32_t now) {
    if (app->route != APP_ROUTE_DISPLAY_MESSAGE) {
        return false;
    }
    if (app->call_divert_pending_action != CALL_DIVERT_PENDING_NONE &&
        app->display_record_id == CALL_DIVERT_REQUEST_RECORD_ID &&
        (key == KEY_NAVI || key == KEY_C)) {
        request_not_confirmed(app, now);
        return true;
    }
    if (app->call_divert_status_detail_active &&
        app->call_divert_status_detail_count > 0u &&
        app->display_record_id == CALL_DIVERT_DETAIL_RECORD_ID) {
        if (key == KEY_NAVI) {
            uint8_t next = (uint8_t)(app->call_divert_status_detail_index + 1u);
            if (next < app->call_divert_status_detail_count) {
                show_status_detail(app, next, now);
            } else {
                return_from_status_detail(app);
            }
            return true;
        }
        if (key == KEY_C) {
            return_from_status_detail(app);
            return true;
        }
    }
    return false;
}

bool tick_call_divert(app_t *app, uint32_t now) {
    if (app->call_divert_status_detail_count > 0u &&
        (!app->call_divert_status_detail_active ||
         app->route != APP_ROUTE_DISPLAY_MESSAGE ||
         app->display_record_id != CALL_DIVERT_DETAIL_RECORD_ID)) {
        /* A call, alarm, power transition, or another dialog may replace a
         * multi-page status result. Retire its navigation ownership as soon as
         * that display is no longer current; record 0x20 is shared by unrelated
         * Nokia notes and cannot identify the owner by itself. */
        clear_status_detail(app);
    }

    if (app->call_divert_pending_action != CALL_DIVERT_PENDING_NONE &&
        app->route == APP_ROUTE_POWER_OFF) {
        /* Power-off owns the screen and cancels modem work. Detach the UI now so
         * a late cancellation/result cannot appear after the next power-on. */
        app->call_divert_pending_action = CALL_DIVERT_PENDING_NONE;
        app->call_divert_request_id = 0u;
    }

    if (app->call_divert_pending_action != CALL_DIVERT_PENDING_NONE &&
        !result_can_present(app)) {
        /* Incoming calls, alarms, and unrelated notes may preempt Requesting.
         * Leave the single service result queued until their UI returns to a
         * surface on which the supplementary-service result can safely render. */
        return false;
    }

    call_forward_result_t result;
    bool changed = false;
    while (modem_service_pop_call_forward_result(&result)) {
        if (app->call_divert_pending_action == CALL_DIVERT_PENDING_NONE ||
            app->call_divert_request_id == 0u ||
            result.request_id != app->call_divert_request_id) {
            continue;
        }
        if (app->route == APP_ROUTE_STANDBY) {
            /* Requesting was preempted and the preempting flow has now returned
             * home. Present the delayed result there; reopening the old menu
             * would be a surprising route jump and could bypass keyguard. */
            app->call_divert_result_return_route = APP_ROUTE_STANDBY;
        }
        finish_request(app, &result, now);
        changed = true;
    }
    return changed;
}

static bool result_can_present(const app_t *app) {
    if (app->keyguard_locked) {
        return false;
    }
    if (app->route == APP_ROUTE_DISPLAY_MESSAGE) {
        return app->display_record_id == CALL_DIVERT_REQUEST_RECORD_ID;
    }
    return app->route == APP_ROUTE_STANDBY ||
           app->route == APP_ROUTE_CALL_DIVERT_MENU;
}

void render_call_divert_menu(const app_t *app, framebuffer_t *fb) {
    call_divert_menu_kind_t kind = (call_divert_menu_kind_t)app->call_divert_menu_kind;
    uint8_t count = menu_count(kind, app);
    uint8_t selected = app->call_divert_menu_selected;
    if (selected >= count) {
        selected = 0u;
    }
    char crumb[16];
    if (kind == CALL_DIVERT_MENU_ROOT) {
        draw_static_page_list(fb,
                              ts_or(CONDITIONS[selected].sid, CONDITIONS[selected].label),
                              "",
                              selected,
                              count,
                              breadcrumb(app, crumb, sizeof(crumb)),
                              "Select");
        return;
    }
    /* Localize for display; labels[].label stays the (English) dispatch-neutral
     * fallback. DELAY_OPTIONS is the widest list (6 rows). */
    const divert_label_t *labels = menu_labels(kind, app);
    const char *localized[6];
    if (count > (uint8_t)ARRAY_COUNT(localized)) {
        count = (uint8_t)ARRAY_COUNT(localized);
    }
    for (uint8_t i = 0u; i < count; i++) {
        localized[i] = ts_or(labels[i].sid, labels[i].label);
    }
    draw_flat_list(fb,
                   localized,
                   count,
                   selected,
                   breadcrumb(app, crumb, sizeof(crumb)),
                   kind == CALL_DIVERT_MENU_DELAY ? "OK" : "Select");
}

void call_divert_submit_other_number(app_t *app, uint32_t now) {
    if (app->editor_value[0] == '\0') {
        open_display_sid(app, 0u, 0x192u, "Invalid\nphone\nnumber", APP_ROUTE_EDITOR, now);
        return;
    }
    char number[33];
    copy_text(number, sizeof(number), app->editor_value);
    close_editor(app);
    const call_divert_condition_t *condition = selected_condition(app);
    call_forward_request_t request;
    memset(&request, 0, sizeof(request));
    request.reason = condition->reason;
    request.action = CALL_FORWARD_ACTION_REGISTER;
    request.has_number = true;
    copy_text(request.number, sizeof(request.number), number);
    if (condition->has_delay) {
        request.has_delay = true;
        request.delay_seconds = app->call_divert_delay_seconds;
    }
    (void)open_request(app, CALL_DIVERT_PENDING_ACTIVATE,
                       app->call_divert_condition_index, &request,
                       APP_ROUTE_CALL_DIVERT_MENU, now);
}

void call_divert_cancel_other_number(app_t *app, uint32_t now) {
    (void)now;
    close_editor(app);
    open_call_divert_menu(app, CALL_DIVERT_MENU_ACTIVATE, 1u);
}

bool call_divert_any_active(const app_t *app) {
    return app != NULL && app->call_divert_unconditional_active;
}

bool call_divert_submit_mmi(app_t *app,
                            const call_forward_request_t *request,
                            uint32_t now) {
    if (app == NULL || request == NULL) {
        return false;
    }
    /* MMI can be the first Call-divert entry point after boot. Populate the
     * editor-history cache before a successful request writes one condition,
     * otherwise saving that result would blank the other persisted numbers. */
    load_call_divert_state(app);
    uint8_t condition_index = condition_index_for_reason(request->reason);
    if (condition_index >= condition_count()) {
        app->call_divert_pending_action = pending_action_for_request(request);
        app->call_divert_pending_condition_index = 0u;
        app->call_divert_result_return_route = APP_ROUTE_STANDBY;
        show_request_failure(app, CALL_FORWARD_OUTCOME_NOT_DONE, now);
        return true;
    }
    select_condition(app, condition_index);
    return open_request(app, pending_action_for_request(request),
                        condition_index, request, APP_ROUTE_STANDBY, now);
}

static uint8_t condition_count(void) {
    return (uint8_t)ARRAY_COUNT(CONDITIONS);
}

static const call_divert_condition_t *selected_condition(const app_t *app) {
    if (app->call_divert_condition_index >= condition_count()) {
        return &CONDITIONS[0];
    }
    return &CONDITIONS[app->call_divert_condition_index];
}

static bool condition_has_status_row(const app_t *app) {
    return selected_condition(app)->reason !=
           CALL_FORWARD_REASON_ALL_CONDITIONAL;
}

static const divert_label_t *menu_labels(call_divert_menu_kind_t kind, const app_t *app) {
    if (kind == CALL_DIVERT_MENU_CONDITION) {
        if (!condition_uses_delay(app->call_divert_condition_index)) {
            return CONDITION_OPTIONS;
        }
        return condition_has_status_row(app) ? CONDITION_DELAY_OPTIONS : CONDITION_DELAY_NO_STATUS_OPTIONS;
    }
    if (kind == CALL_DIVERT_MENU_ACTIVATE) {
        return ACTIVATE_OPTIONS;
    }
    return DELAY_OPTIONS;
}

static uint8_t menu_count(call_divert_menu_kind_t kind, const app_t *app) {
    if (kind == CALL_DIVERT_MENU_ROOT) {
        return condition_count();
    }
    if (kind == CALL_DIVERT_MENU_CONDITION) {
        if (!condition_uses_delay(app->call_divert_condition_index)) {
            return (uint8_t)ARRAY_COUNT(CONDITION_OPTIONS);
        }
        return condition_has_status_row(app)
            ? (uint8_t)ARRAY_COUNT(CONDITION_DELAY_OPTIONS)
            : (uint8_t)ARRAY_COUNT(CONDITION_DELAY_NO_STATUS_OPTIONS);
    }
    if (kind == CALL_DIVERT_MENU_ACTIVATE) {
        return (uint8_t)ARRAY_COUNT(ACTIVATE_OPTIONS);
    }
    return (uint8_t)ARRAY_COUNT(DELAY_OPTIONS);
}

static const char *breadcrumb(const app_t *app, char *scratch, size_t cap) {
    call_divert_menu_kind_t kind = (call_divert_menu_kind_t)app->call_divert_menu_kind;
    if (kind == CALL_DIVERT_MENU_ROOT) {
        snprintf(scratch, cap, "5-%u", (unsigned)(app->call_divert_menu_selected + 1u));
    } else if (kind == CALL_DIVERT_MENU_CONDITION) {
        snprintf(scratch, cap, "5-%u-%u",
                 (unsigned)(app->call_divert_condition_index + 1u),
                 (unsigned)(app->call_divert_menu_selected + 1u));
    } else if (kind == CALL_DIVERT_MENU_ACTIVATE) {
        snprintf(scratch, cap, "5-%u-1-%u",
                 (unsigned)(app->call_divert_condition_index + 1u),
                 (unsigned)(app->call_divert_menu_selected + 1u));
    } else {
        snprintf(scratch, cap, "5-%u-%u-%u",
                 (unsigned)(app->call_divert_condition_index + 1u),
                 (unsigned)(app->call_divert_parent_selected + 1u),
                 (unsigned)(app->call_divert_menu_selected + 1u));
    }
    return scratch;
}

static void select_condition(app_t *app, uint8_t index) {
    app->call_divert_condition_index = index >= condition_count() ? 0u : index;
}

static void prepare_result_target(app_t *app, uint8_t action,
                                  uint8_t condition_index) {
    if (app->call_divert_result_return_route != APP_ROUTE_CALL_DIVERT_MENU) {
        app->route = app->call_divert_result_return_route;
        app->dirty = true;
        return;
    }
    if (action == CALL_DIVERT_PENDING_CANCEL_ALL) {
        open_call_divert_menu(app, CALL_DIVERT_MENU_ROOT, 5u);
        return;
    }
    uint8_t selected = action == CALL_DIVERT_PENDING_ACTIVATE ? 0u
                     : action == CALL_DIVERT_PENDING_CANCEL ? 1u : 2u;
    select_condition(app, condition_index);
    open_call_divert_menu(app, CALL_DIVERT_MENU_CONDITION, selected);
}

static bool open_request(app_t *app, uint8_t action, uint8_t condition_index,
                         const call_forward_request_t *request,
                         app_route_t return_route, uint32_t now) {
    app->call_divert_pending_action = action;
    app->call_divert_pending_condition_index = condition_index;
    app->call_divert_request_id = 0u;
    app->call_divert_result_return_route = return_route;
    clear_status_detail(app);
    if (!modem_service_request_call_forward(
            request, &app->call_divert_request_id)) {
        show_request_failure(app, CALL_FORWARD_OUTCOME_NOT_DONE, now);
        return true;
    }
    open_display_sid(app, CALL_DIVERT_REQUEST_RECORD_ID, 0x2a4u,
                     "Requesting", return_route, now);
    return true;
}

static void finish_request(app_t *app, const call_forward_result_t *result,
                           uint32_t now) {
    uint8_t action = app->call_divert_pending_action;
    uint8_t condition_index = app->call_divert_pending_condition_index;
    app->call_divert_request_id = 0u;
    clear_status_detail(app);

    if (result->outcome != CALL_FORWARD_OUTCOME_SUCCESS) {
        show_request_failure(app, result->outcome, now);
        return;
    }
    app->call_divert_pending_action = CALL_DIVERT_PENDING_NONE;

    if ((result->request.action == CALL_FORWARD_ACTION_REGISTER ||
         result->request.action == CALL_FORWARD_ACTION_ENABLE) &&
        (result->request.has_number || result->request.has_delay)) {
        bool changed = false;
        if (result->request.has_number) {
            uint8_t first = condition_index;
            uint8_t limit = condition_index < STORE_CALL_DIVERT_CONDITION_COUNT
                ? (uint8_t)(condition_index + 1u) : 0u;
            if (result->request.reason == CALL_FORWARD_REASON_ALL) {
                first = 0u;
                limit = STORE_CALL_DIVERT_CONDITION_COUNT;
            } else if (result->request.reason ==
                       CALL_FORWARD_REASON_ALL_CONDITIONAL) {
                first = 1u;
                limit = STORE_CALL_DIVERT_CONDITION_COUNT;
            }
            for (uint8_t i = first; i < limit; i++) {
                if (strcmp(app->call_divert_numbers[i],
                           result->request.number) == 0) {
                    continue;
                }
                copy_text(app->call_divert_numbers[i],
                          sizeof(app->call_divert_numbers[i]),
                          result->request.number);
                changed = true;
            }
        }
        if (result->request.has_delay &&
            delay_value_valid(result->request.delay_seconds) &&
            app->call_divert_delay_seconds != result->request.delay_seconds) {
            app->call_divert_delay_seconds = result->request.delay_seconds;
            changed = true;
        }
        if (changed) {
            save_call_divert_state(app);
        }
    }

    if (action == CALL_DIVERT_PENDING_STATUS && result->status_known &&
        result->active && result->number[0] != '\0' &&
        condition_index < STORE_CALL_DIVERT_CONDITION_COUNT) {
        bool changed = strcmp(app->call_divert_numbers[condition_index],
                              result->number) != 0;
        copy_text(app->call_divert_numbers[condition_index],
                  sizeof(app->call_divert_numbers[condition_index]),
                  result->number);
        if (result->has_delay && delay_value_valid(result->delay_seconds) &&
            app->call_divert_delay_seconds != result->delay_seconds) {
            app->call_divert_delay_seconds = result->delay_seconds;
            changed = true;
        }
        if (changed) {
            save_call_divert_state(app);
        }
    }

    prepare_result_target(app, action, condition_index);

    if (action == CALL_DIVERT_PENDING_ACTIVATE) {
        uint16_t sid = result->request.action == CALL_FORWARD_ACTION_ENABLE
                           ? 0x12cu
                           : 0x122u;
        open_display_sid(app, 3u, sid, "Divert activated",
                         app->call_divert_result_return_route, now);
        return;
    }
    if (action == CALL_DIVERT_PENDING_CANCEL) {
        open_display_sid(app, 3u, 0x127u, "Divert de-\nactivated",
                         app->call_divert_result_return_route, now);
        return;
    }
    if (action == CALL_DIVERT_PENDING_CANCEL_ALL) {
        open_display_sid(app, 3u, 0x126u, "Divert\ncancelled",
                         app->call_divert_result_return_route, now);
        return;
    }
    if (action == CALL_DIVERT_PENDING_STATUS && result->status_known &&
        result->active) {
        if (result->number[0] != '\0') {
            copy_text(app->call_divert_status_number,
                      sizeof(app->call_divert_status_number), result->number);
            app->call_divert_status_delay_seconds = result->delay_seconds;
            app->call_divert_status_detail_count =
                result->has_delay ? 2u : 1u;
            show_status_detail(app, 0u, now);
            return;
        }
    }

    if (result->status_known && result->active) {
        open_display_sid(app, 3u, 0x123u, "Divert\nactive",
                         app->call_divert_result_return_route, now);
    } else {
        open_display_sid(app, 3u, 0x128u, "Divert\nnot\nactive",
                         app->call_divert_result_return_route, now);
    }
}

static void show_request_failure(app_t *app, call_forward_outcome_t outcome,
                                 uint32_t now) {
    uint8_t action = app->call_divert_pending_action;
    uint8_t condition_index = app->call_divert_pending_condition_index;
    app->call_divert_pending_action = CALL_DIVERT_PENDING_NONE;
    app->call_divert_request_id = 0u;
    prepare_result_target(app, action, condition_index);
    if (outcome == CALL_FORWARD_OUTCOME_NO_NETWORK) {
        open_display_sid(app, CALL_DIVERT_DETAIL_RECORD_ID, 0x209u,
                         "No network\ncoverage",
                         app->call_divert_result_return_route, now);
    } else if (outcome == CALL_FORWARD_OUTCOME_RESULT_UNKNOWN) {
        open_display_sid(app, CALL_DIVERT_DETAIL_RECORD_ID, 0x263u,
                         "Result\nunknown",
                         app->call_divert_result_return_route, now);
    } else if (outcome == CALL_FORWARD_OUTCOME_CANCELLED) {
        open_display_sid(app, CALL_DIVERT_QUIT_RECORD_ID, 0x3e2u,
                         "Request\nnot\nconfirmed",
                         APP_ROUTE_STANDBY, now);
    } else {
        open_display_sid(app, CALL_DIVERT_DETAIL_RECORD_ID, 0x210u,
                         "Not done",
                         app->call_divert_result_return_route, now);
    }
}

static void request_not_confirmed(app_t *app, uint32_t now) {
    uint32_t request_id = app->call_divert_request_id;
    if (request_id != 0u) {
        (void)modem_service_cancel_queued_call_forward(request_id);
    }
    app->call_divert_pending_action = CALL_DIVERT_PENDING_NONE;
    app->call_divert_request_id = 0u;
    clear_status_detail(app);
    open_display_sid(app, CALL_DIVERT_QUIT_RECORD_ID, 0x3e2u,
                     "Request\nnot\nconfirmed", APP_ROUTE_STANDBY, now);
}

static const char *active_number(const app_t *app, uint8_t condition_index) {
    if (condition_index >= 5u) {
        return "";
    }
    return app->call_divert_numbers[condition_index];
}

static void show_status_detail(app_t *app, uint8_t index, uint32_t now) {
    app->call_divert_status_detail_index = index;
    if (index == 0u) {
        char text[sizeof(app->display_text)];
        format_status_number(text, sizeof(text),
                             app->call_divert_status_number);
        open_display(app, CALL_DIVERT_DETAIL_RECORD_ID, text, 0, 0,
                     app->call_divert_result_return_route, now);
    } else {
        open_display_sid_num(app, CALL_DIVERT_DETAIL_RECORD_ID, 0x3a5u,
                             "Delay time\n%N seconds",
                             (unsigned)app->call_divert_status_delay_seconds,
                             app->call_divert_result_return_route, now);
    }
    app->call_divert_status_detail_active = true;
}

static void clear_status_detail(app_t *app) {
    app->call_divert_status_detail_count = 0u;
    app->call_divert_status_detail_index = 0u;
    app->call_divert_status_detail_active = false;
    app->call_divert_status_number[0] = '\0';
    app->call_divert_status_delay_seconds = 0u;
}

static void format_status_number(char *dst, size_t cap,
                                 const char *number) {
    if (dst == NULL || cap == 0u) {
        return;
    }
    const char *tmpl = ts_or(0x3a6u, "To number\n%S");
    const char *value = number != NULL ? number : "";
    size_t used = 0u;
    while (*tmpl != '\0' && used + 1u < cap) {
        if (tmpl[0] == '%' && tmpl[1] == 'S') {
            while (*value != '\0' && used + 1u < cap) {
                dst[used++] = *value++;
            }
            tmpl += 2;
            continue;
        }
        const char *next = tmpl;
        asset_next_codepoint(&next);
        size_t bytes = (size_t)(next - tmpl);
        if (used + bytes + 1u > cap) {
            break;
        }
        memcpy(&dst[used], tmpl, bytes);
        used += bytes;
        tmpl = next;
    }
    dst[used] = '\0';
}

static void return_from_status_detail(app_t *app) {
    clear_status_detail(app);
    if (app->call_divert_result_return_route == APP_ROUTE_CALL_DIVERT_MENU) {
        open_call_divert_menu(app, CALL_DIVERT_MENU_CONDITION, 2u);
    } else {
        app->route = app->call_divert_result_return_route;
        app->dirty = true;
    }
}

static bool condition_uses_delay(uint8_t condition_index) {
    return condition_index < condition_count() && CONDITIONS[condition_index].has_delay;
}

static bool delay_value_valid(uint8_t delay_seconds) {
    return delay_seconds >= 5u && delay_seconds <= 30u &&
           (delay_seconds % 5u) == 0u;
}

static uint8_t condition_index_for_reason(call_forward_reason_t reason) {
    for (uint8_t i = 0u; i < condition_count(); i++) {
        if (CONDITIONS[i].reason == reason) {
            return i;
        }
    }
    return condition_count();
}

static uint8_t pending_action_for_request(
    const call_forward_request_t *request) {
    if (request->action == CALL_FORWARD_ACTION_QUERY) {
        return CALL_DIVERT_PENDING_STATUS;
    }
    if (request->reason == CALL_FORWARD_REASON_ALL &&
        request->action == CALL_FORWARD_ACTION_ERASE) {
        return CALL_DIVERT_PENDING_CANCEL_ALL;
    }
    if (request->action == CALL_FORWARD_ACTION_DISABLE ||
        request->action == CALL_FORWARD_ACTION_ERASE) {
        return CALL_DIVERT_PENDING_CANCEL;
    }
    return CALL_DIVERT_PENDING_ACTIVATE;
}

static uint8_t delay_selected_index(const app_t *app) {
    uint8_t delay = app->call_divert_delay_seconds == 0u ? CALL_DIVERT_DEFAULT_DELAY_SECONDS : app->call_divert_delay_seconds;
    for (uint8_t i = 0u; i < ARRAY_COUNT(DELAY_VALUES); i++) {
        if (DELAY_VALUES[i] == delay) {
            return i;
        }
    }
    return 3u;
}

static void apply_delay(app_t *app, uint32_t now) {
    if (app->call_divert_menu_selected >= ARRAY_COUNT(DELAY_VALUES)) {
        app->call_divert_menu_selected = delay_selected_index(app);
    }
    app->call_divert_delay_seconds = DELAY_VALUES[app->call_divert_menu_selected];
    save_call_divert_state(app);
    open_call_divert_menu(app, CALL_DIVERT_MENU_CONDITION, app->call_divert_parent_selected);
    open_display_sid(app, 3u, 0x3a8u, "Done",
                     APP_ROUTE_CALL_DIVERT_MENU, now);
}

static void apply_activate_option(app_t *app, uint32_t now) {
    if (app->call_divert_menu_selected == 0u) {
        char number[STORE_TEXT_MAX + 1u];
        if (!voice_mailbox_number(number, sizeof(number))) {
            open_call_divert_menu(app, CALL_DIVERT_MENU_ACTIVATE, 0u);
            open_display_sid(app, 0u, 0x20au,
                             "Save voice\nmailbox\nnumber first",
                             APP_ROUTE_CALL_DIVERT_MENU, now);
            return;
        }
        const call_divert_condition_t *condition = selected_condition(app);
        call_forward_request_t request;
        memset(&request, 0, sizeof(request));
        request.reason = condition->reason;
        request.action = CALL_FORWARD_ACTION_REGISTER;
        request.has_number = true;
        copy_text(request.number, sizeof(request.number), number);
        if (condition->has_delay) {
            request.has_delay = true;
            request.delay_seconds = app->call_divert_delay_seconds;
        }
        (void)open_request(app, CALL_DIVERT_PENDING_ACTIVATE,
                           app->call_divert_condition_index, &request,
                           APP_ROUTE_CALL_DIVERT_MENU, now);
        return;
    }
    open_editor(app,
                "Enter number:",
                active_number(app, app->call_divert_condition_index),
                30u,
                EDITOR_KIND_NUMBER,
                EDITOR_CONTEXT_CALL_DIVERT_OTHER_NUMBER,
                true,
                now);
}

static void apply_condition_option(app_t *app, uint32_t now) {
    uint8_t selected = app->call_divert_menu_selected;
    /* Condition 7's 3-row list has Set delay at ordinal 3 (no Status). */
    bool has_status = condition_has_status_row(app);
    if (selected == 0u) {
        app->call_divert_parent_selected = selected;
        open_call_divert_menu(app, CALL_DIVERT_MENU_ACTIVATE, 0u);
    } else if (selected == 1u) {
        call_forward_request_t request;
        memset(&request, 0, sizeof(request));
        request.reason = selected_condition(app)->reason;
        request.action = CALL_FORWARD_ACTION_ERASE;
        (void)open_request(app, CALL_DIVERT_PENDING_CANCEL,
                           app->call_divert_condition_index, &request,
                           APP_ROUTE_CALL_DIVERT_MENU, now);
    } else if (selected == 2u && has_status) {
        call_forward_request_t request;
        memset(&request, 0, sizeof(request));
        request.reason = selected_condition(app)->reason;
        request.action = CALL_FORWARD_ACTION_QUERY;
        (void)open_request(app, CALL_DIVERT_PENDING_STATUS,
                           app->call_divert_condition_index, &request,
                           APP_ROUTE_CALL_DIVERT_MENU, now);
    } else {
        app->call_divert_parent_selected = selected;
        app->call_divert_menu_selected = delay_selected_index(app);
        open_call_divert_menu(app, CALL_DIVERT_MENU_DELAY, app->call_divert_menu_selected);
    }
}

static bool voice_mailbox_number(char *dst, size_t cap) {
    if (dst == NULL || cap == 0u) {
        return false;
    }
    store_setting_get_text(STORE_SETTING_SYSTEM_VOICE_MAILBOX_NUMBER, dst, (uint8_t)cap);
    if (dst[0] == '\0') {
        (void)modem_service_get_voice_mailbox_number(dst, cap);
    }
    return dst[0] != '\0';
}

static void load_call_divert_state(app_t *app) {
    if (app->call_divert_storage_loaded) {
        return;
    }
    store_call_divert_state_t state;
    if (store_call_divert_get(&state) == STORE_STATUS_OK) {
        app->call_divert_delay_seconds = delay_value_valid(state.delay_seconds)
            ? state.delay_seconds
            : CALL_DIVERT_DEFAULT_DELAY_SECONDS;
        for (uint8_t i = 0u; i < STORE_CALL_DIVERT_CONDITION_COUNT; i++) {
            copy_text(app->call_divert_numbers[i],
                      sizeof(app->call_divert_numbers[i]),
                      state.numbers[i]);
        }
    }
    if (app->call_divert_delay_seconds == 0u) {
        app->call_divert_delay_seconds = CALL_DIVERT_DEFAULT_DELAY_SECONDS;
    }
    app->call_divert_storage_loaded = true;
}

static void save_call_divert_state(const app_t *app) {
    store_call_divert_state_t state;
    memset(&state, 0, sizeof(state));
    /* Network state owns the standby icon. Keep the legacy payload byte zero so
     * an older EEPROM image can never be mistaken for live divert authority. */
    state.active_mask = 0u;
    state.delay_seconds = delay_value_valid(app->call_divert_delay_seconds)
        ? app->call_divert_delay_seconds
        : CALL_DIVERT_DEFAULT_DELAY_SECONDS;
    for (uint8_t i = 0u; i < STORE_CALL_DIVERT_CONDITION_COUNT; i++) {
        copy_text(state.numbers[i], sizeof(state.numbers[i]), app->call_divert_numbers[i]);
    }
    (void)store_call_divert_set(&state);
}

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "app_status_runtime.h"
#include "apps/dialogs_app.h"
#include "apps/power_app.h"
#include "apps/profiles_app.h"
#include "services/battery_charge_supervisor_service.h"
#include "services/battery_learning_service.h"
#include "services/board_diag_service.h"
#include "services/core1_services.h"
#include "storage/store_service.h"

static board_diag_snapshot_t board;
static uint32_t sequence, learned_empty, displays, powered_off, floor_releases;
static uint8_t last_record;
static uint16_t last_sid;
static battery_learning_service_snapshot_t learner;

int32_t time_diff_ms(uint32_t a, uint32_t b) { return (int32_t)(a - b); }
void board_diag_service_poll(uint32_t now) { (void)now; }
bool board_diag_battery_valid(void) { return board.battery_valid; }
bool board_diag_charger_connected(void) { return board.charger_connected; }
bool board_diag_charge_status_valid(void) { return board.charge_status_valid; }
battery_charger_state_t board_diag_charger_state(void) {
    return board.charge_state == BOARD_DIAG_CHARGE_DISABLED
        ? BATTERY_CHARGER_DETACHED : (battery_charger_state_t)board.charge_state;
}
void board_diag_get_snapshot(board_diag_snapshot_t *out) { *out = board; }
uint8_t board_diag_battery_level_bars(void) { return 1u; }
uint16_t board_diag_battery_millivolts_fresh(void) { return board.battery_mv; }
uint32_t board_diag_battery_sample_sequence(void) { return sequence; }
bool board_diag_battery_supply_failure_indicates_empty(void) { return false; }
uint16_t board_diag_battery_shutdown_threshold_mv(bool call) {
    return board.chemistry == BOARD_DIAG_CHEM_LI_ION
        ? (call ? 3000u : 3300u) : (call ? 1800u : 2100u);
}
bool board_diag_battery_empty(void) { return board.battery_empty; }
bool board_diag_battery_low(void) { return board.battery_low; }
bool modem_service_take_supply_power_failure(void) { return false; }
uint32_t battery_charge_supervisor_service_poll(uint32_t now) { (void)now; return 0u; }
void battery_charge_supervisor_service_note_legacy(bool active, bool completed) {
    (void)active; (void)completed;
}
void battery_charge_supervisor_service_get_snapshot(battery_charge_supervisor_service_snapshot_t *out) {
    memset(out, 0, sizeof(*out));
    out->model.policy = BATTERY_CHARGE_POLICY_ENFORCE_LI_ION;
}
uint16_t battery_charge_supervisor_service_effective_charge_factor_permille(void) { return 1000u; }
bool battery_charge_supervisor_service_release_for_battery_floor(void) {
    floor_releases++; return true;
}
uint32_t battery_learning_service_poll(uint32_t now, uint32_t events, uint16_t factor) {
    (void)now; (void)factor;
    if (events & BATTERY_LEARNING_EVENT_EMPTY) learned_empty++;
    return 0u;
}
void battery_learning_service_get_snapshot(battery_learning_service_snapshot_t *out) {
    *out = learner;
}
bool store_service_flush_all(void) { return true; }
uint8_t profile_active_index(void) { return 0u; }
uint8_t profile_get_tone_setting(uint8_t index, profile_setting_kind_t kind) {
    (void)index; (void)kind; return 255u;
}
bool core1_services_codec_init(void) { return true; }
void core1_services_codec_standby(void) {}
void core1_post_command(core1_cmd_t cmd, uint16_t arg) { (void)cmd; (void)arg; }
void status_chrome_set_battery(uint8_t bars) { (void)bars; }
void return_from_display(app_t *app) { app->route = APP_ROUTE_STANDBY; }
void power_off(app_t *app, uint32_t now) {
    (void)now; powered_off++; app->route = APP_ROUTE_POWER_OFF;
}
void open_display_sid(app_t *app, uint8_t record, uint16_t sid,
                      const char *fallback, app_route_t route, uint32_t now) {
    (void)fallback; (void)route; (void)now;
    last_record = record; last_sid = sid; displays++;
    app->route = APP_ROUTE_DISPLAY_MESSAGE;
    app->display_record_id = record;
}

static void poll(app_t *app, uint32_t now) {
    sequence++;
    (void)poll_battery(app, now);
}

static void reset_fixture(void) {
    board = (board_diag_snapshot_t){
        .battery_valid = true,
        .battery_mv = 3800u,
        .chemistry = BOARD_DIAG_CHEM_LI_ION,
    };
    learner = (battery_learning_service_snapshot_t){0};
    learner.initialized = true;
    learner.model.remaining_capacity_valid = true;
    learner.model.remaining_capacity_nah = 100000000u;
    learner.model.soc_confidence = BATTERY_SOC_CONFIDENCE_ANCHORED;
    sequence = learned_empty = displays = powered_off = floor_releases = 0u;
    last_record = 0u;
    last_sid = 0u;
}

static void measured_capacity(uint16_t capacity_mah, uint64_t remaining_nah) {
    learner.model.learned_capacity_valid = true;
    learner.model.learned_capacity_mah = capacity_mah;
    learner.model.soc_capacity_mah = capacity_mah;
    learner.model.capacity_confidence = BATTERY_CAPACITY_CONFIDENCE_OBSERVED;
    learner.model.remaining_capacity_nah = remaining_nah;
}

static void test_voltage_notice_delivery_and_endpoint_isolation(void) {
    reset_fixture();
    app_t app = {0};
    app.route = APP_ROUTE_DISPLAY_MESSAGE;
    app.display_record_id = 20u;
    board.battery_low_notice = true;
    board.battery_mv = 3550u;
    /* Pending LOW must remain due while another dialog occupies the screen. */
    poll(&app, 1000u);
    assert(displays == 0u && app.battery_low_notified);
    assert(app.battery_low_eligible_ms == 1000u);
    assert(app.battery_low_deferred && app.battery_low_deferred_ms == 1000u);
    app.route = APP_ROUTE_STANDBY;
    poll(&app, 2000u);
    assert(displays == 1u && last_record == 16u && last_sid == 0x62u);
    assert(!app.battery_low_deferred && app.battery_low_shown_ms == 2000u);
    assert(app.battery_low_shown_count == 1u);
    app.route = APP_ROUTE_STANDBY;
    poll(&app, 62999u);
    assert(displays == 1u);
    poll(&app, 63000u);
    assert(displays == 2u && app.battery_low_shown_count == 2u);
    app.route = APP_ROUTE_STANDBY;
    poll(&app, 676999u);
    assert(displays == 2u);
    poll(&app, 677000u);
    assert(displays == 3u);

    /* Early warning cannot corroborate a counted-zero EMPTY, or release a
     * charger hold through the physical battery-floor path. */
    app.route = APP_ROUTE_STANDBY;
    learner.model.capacity_prediction_exhausted = true;
    learner.model.remaining_capacity_nah = 0u;
    board.charger_connected = true;
    poll(&app, 678000u);
    assert(!app.battery_empty_off_ms && learned_empty == 0u);
    /* Counted-zero has its own pre-existing force_low/release policy. */
    learner.model.capacity_prediction_exhausted = false;
    learner.model.remaining_capacity_nah = 100000000u;
    floor_releases = 0u;
    poll(&app, 679000u);
    assert(floor_releases == 0u);

    /* An unshown EMPTY still arms the original deadline, preserving LOW trace. */
    app.route = APP_ROUTE_DISPLAY_MESSAGE;
    board.battery_mv = 3290u;
    poll(&app, 680000u);
    assert(!app.battery_empty_off_ms);
    poll(&app, 681000u);
    assert(app.battery_empty_off_ms == 683056u && learned_empty == 1u);
    assert(app.battery_last_empty_ms == 681000u && app.battery_last_empty_mv == 3290u);
    poll(&app, 683056u);
    assert(powered_off == 1u && app.route == APP_ROUTE_POWER_OFF);
    poll(&app, 684000u);
    assert(app.battery_low_shown_count == 3u && app.battery_last_empty_ms == 681000u);
}

static void test_capacity_notice_delivery_and_hysteresis(void) {
    reset_fixture();
    app_t app = {.route = APP_ROUTE_STANDBY};
    measured_capacity(812u, 40600001u);
    learner.model.state_of_charge_percent = 5u;
    poll(&app, 1000u);
    assert(!app.battery_capacity_low_notice && !displays);
    learner.model.remaining_capacity_nah--;
    app.route = APP_ROUTE_DISPLAY_MESSAGE;
    poll(&app, 2000u);
    assert(app.battery_capacity_low_notice && app.battery_low_deferred);
    assert(!displays && !app.battery_empty_off_ms && !learned_empty);
    app.route = APP_ROUTE_STANDBY;
    poll(&app, 3000u);
    assert(displays == 1u && last_record == 16u && last_sid == 0x62u);
    assert(app.battery_low_eligible_mv == 3800u);
    assert(!app.battery_low_deferred && !app.battery_empty_off_ms);
    app.route = APP_ROUTE_STANDBY;
    poll(&app, 64000u);
    assert(displays == 2u);
    app.route = APP_ROUTE_STANDBY;
    poll(&app, 678000u);
    assert(displays == 3u);
    app.route = APP_ROUTE_STANDBY;
    learner.model.remaining_capacity_nah = 56840000u;
    poll(&app, 679000u);
    assert(app.battery_capacity_low_notice && app.battery_low_notified);
    learner.model.remaining_capacity_nah++;
    poll(&app, 680000u);
    assert(!app.battery_capacity_low_notice && !app.battery_low_notified);
    learner.model.remaining_capacity_nah = 48720000u;
    poll(&app, 681000u);
    assert(!app.battery_capacity_low_notice);
    learner.model.remaining_capacity_nah = 40600000u;
    board.charger_connected = true;
    board.charge_state = BOARD_DIAG_CHARGE_DISABLED;
    poll(&app, 682000u);
    assert(app.battery_capacity_low_notice && displays == 4u);
    assert(!floor_releases && !app.battery_empty_off_ms && !learned_empty);
    /* A 5% notice plus physical LOW is still not a counted-zero endpoint. */
    board.charger_connected = false;
    board.battery_low = true;
    app.route = APP_ROUTE_STANDBY;
    poll(&app, 683000u);
    assert(!app.battery_empty_off_ms && !learned_empty);
    /* Counted zero at healthy voltage remains only a warning. */
    board.battery_low = false;
    learner.model.remaining_capacity_nah = 0u;
    learner.model.capacity_prediction_exhausted = true;
    poll(&app, 684000u);
    assert(!app.battery_empty_off_ms && !learned_empty);
    assert(app.battery_endpoint_kind == BATTERY_ENDPOINT_CAPACITY_PENDING);
}

static void test_capacity_notice_requires_trusted_estimate(void) {
    for (unsigned invalid = 0u; invalid < 7u; invalid++) {
        reset_fixture();
        app_t app = {.route = APP_ROUTE_STANDBY};
        measured_capacity(812u, 40600000u);
        poll(&app, 1000u);
        assert(app.battery_capacity_low_notice);
        app.route = APP_ROUTE_STANDBY;
        switch (invalid) {
        case 0u: learner.model.learned_capacity_valid = false; break;
        case 1u: learner.model.remaining_capacity_valid = false; break;
        case 2u: learner.model.soc_confidence = BATTERY_SOC_CONFIDENCE_PROVISIONAL; break;
        case 3u: learner.model.capacity_confidence = BATTERY_CAPACITY_CONFIDENCE_PRIOR; break;
        case 4u: learner.model.capacity_confidence = BATTERY_CAPACITY_CONFIDENCE_CONFLICTED; break;
        case 5u: learner.model.soc_capacity_mah = 0u; break;
        case 6u: board.battery_valid = false; break;
        }
        poll(&app, 62000u);
        assert(!app.battery_capacity_low_notice && displays == 1u);
        assert(!app.battery_empty_off_ms && !learned_empty);
        /* Losing SOC authority never disables the independent voltage notice. */
        board.battery_valid = true;
        board.battery_low_notice = true;
        board.battery_mv = 3550u;
        poll(&app, 63000u);
        assert(displays == 2u && last_sid == 0x62u);
    }
}

static void test_capacity_notice_charging_and_power_off(void) {
    reset_fixture();
    app_t app = {.route = APP_ROUTE_STANDBY, .battery_charger_connected = true};
    measured_capacity(812u, 40000000u);
    board.charger_connected = true;
    board.charge_status_valid = true;
    board.charge_state = BOARD_DIAG_CHARGE_ACTIVE;
    board.charger_enable_valid = true;
    board.charger_enabled = true;
    board.ltc_sample_valid = true;
    board.ltc_continuity_valid = true;
    board.ltc_current_polarity_verified = true;
    board.ltc_current_ua = 100000;
    poll(&app, 1000u);
    assert(app.battery_capacity_low_notice && !app.battery_low_notified && !displays);
    /* The charging label is not sufficient when the battery is draining. */
    board.ltc_current_ua = -10000;
    poll(&app, 2000u);
    assert(displays == 1u && app.battery_low_notified);
    app.route = APP_ROUTE_STANDBY;
    board.ltc_current_ua = 100000;
    poll(&app, 3000u);
    assert(!app.battery_low_notified && displays == 1u);
    learner.model.remaining_capacity_nah = 56840001u;
    poll(&app, 4000u);
    assert(!app.battery_capacity_low_notice);
    learner.model.remaining_capacity_nah = 40000000u;
    app.route = APP_ROUTE_POWER_OFF;
    board.ltc_current_ua = -10000;
    poll(&app, 65000u);
    assert(displays == 1u && !app.battery_low_notified && !learned_empty);
}

static void test_capacity_notice_nimh_and_voltage_fallback(void) {
    reset_fixture();
    app_t app = {.route = APP_ROUTE_STANDBY};
    board.chemistry = BOARD_DIAG_CHEM_NI_MH;
    board.battery_mv = 2600u;
    measured_capacity(1242u, 62100000u);
    learner.model.capacity_confidence = BATTERY_CAPACITY_CONFIDENCE_LEARNED;
    poll(&app, 1000u);
    assert(displays == 1u && app.battery_capacity_low_notice && !learned_empty);
    reset_fixture();
    app = (app_t){.route = APP_ROUTE_STANDBY};
    measured_capacity(812u, 200000000u);
    board.battery_low_notice = true;
    board.battery_mv = 3550u;
    poll(&app, 1000u);
    assert(!app.battery_capacity_low_notice && displays == 1u);
    assert(!app.battery_empty_off_ms && !learned_empty);
}

int main(void) {
    test_voltage_notice_delivery_and_endpoint_isolation();
    test_capacity_notice_delivery_and_hysteresis();
    test_capacity_notice_requires_trusted_estimate();
    test_capacity_notice_charging_and_power_off();
    test_capacity_notice_nimh_and_voltage_fallback();
    puts("Battery warning runtime delivery and endpoint isolation tests passed");
    return 0;
}

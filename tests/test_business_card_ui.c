#include "apps/business_card_app.h"
#include "apps/dialogs_app.h"
#include "services/business_card_service.h"
#include "services/input_keys.h"
#include "services/strings.h"

#include <assert.h>
#include <string.h>

static uint32_t pending = 7u, clock_ms;
static phonebook_entry_t entry = {.index=7u, .name="Alice", .number="+123456"};
static bool accept = true, completed;
static business_card_action_result_t result;
static unsigned saves, discards, notes;
static uint16_t note_sid;
static uint8_t note_record;
static app_route_t note_return;

uint32_t time_ms(void) { return clock_ms; }
int32_t time_diff_ms(uint32_t a, uint32_t b) { return (int32_t)(a - b); }
uint32_t business_card_service_pending_first(void) { return pending; }
bool business_card_service_get(uint32_t id, phonebook_entry_t *out) {
    assert(id == 7u); *out = entry; return true;
}
bool business_card_service_request(uint32_t id, bool save) {
    assert(id == 7u);
    if (!accept) return false;
    if (save) saves++; else discards++;
    result = (business_card_action_result_t){.id=id, .save=save};
    return true;
}
bool business_card_service_pop_result(business_card_action_result_t *out) {
    if (!completed) return false;
    *out = result; completed = false; return true;
}
void open_display_sid(app_t *app, uint8_t record, uint16_t sid, const char *fallback,
                      app_route_t return_route, uint32_t now) {
    (void)fallback; (void)now;
    notes++; note_sid = sid; note_record = record; note_return = return_route;
    app->route = APP_ROUTE_DISPLAY_MESSAGE;
}

int main(void) {
    app_t app = {0};
    open_received_business_card(&app);
    assert(app.route == APP_ROUTE_BUSINESS_CARD && app.business_card_receive_id == 7u);
    handle_business_card_key(&app, KEY_UP, 0u);
    assert(app.business_card_option == 2u);
    handle_business_card_key(&app, KEY_DOWN, 0u);
    assert(app.business_card_option == 0u);
    framebuffer_t got, want;
    memset(&got, 0xa5, sizeof(got));
    for (uint8_t i = 0; i < strings_language_count(); i++) {
        strings_set_language(strings_language_id_at(i));
        handle_business_card_key(&app, KEY_NAVI, 0u);
        assert(app.business_card_preview);
        fb_clear(&want, false);
        fb_text(&want, asset_font(FONT_FS2), entry.name, 0, 7, true, FB_WIDTH);
        draw_text_right(&want, asset_font(FONT_FS0), entry.number,
                        35 - asset_font(FONT_FS0)->height, FB_WIDTH);
        draw_softkey(&want, ts(0x2d8u));
        render_business_card(&app, &got);
        assert(memcmp(got.data, want.data, FB_SIZE) == 0);
        handle_business_card_key(&app, KEY_C, 0u);
        assert(!app.business_card_preview && discards == 0u);
        fb_clear(&want, false);
        const char *labels[] = {ts(0x67u), ts(0x65u), ts(0x63u)};
        draw_flat_list(&want, labels, 3u, 0u, 0, "Select");
        render_business_card(&app, &got);
        assert(memcmp(got.data, want.data, FB_SIZE) == 0);
    }
    /* Both original text windows must advance when both values overflow. */
    strcpy(entry.name, "ABCDEFGHIJKLMNOPQRSTUVWX");
    strcpy(entry.number, "+1234567890123456789012345678901");
    handle_business_card_key(&app, KEY_NAVI, 0);
    render_business_card(&app, &got);
    clock_ms = 1023; assert(!ui_marquee_tick(clock_ms));
    clock_ms = 1024; assert(ui_marquee_tick(clock_ms));
    render_business_card(&app, &want);
    bool top_changed = false, bottom_changed = false;
    for (unsigned y = 7; y < 15; y++) for (unsigned x = 0; x < FB_WIDTH; x++)
        top_changed |= ((got.data[(y / 8u) * FB_WIDTH + x] ^ want.data[(y / 8u) * FB_WIDTH + x]) & (1u << (y % 8u))) != 0;
    for (unsigned y = 22; y < 35; y++) for (unsigned x = 0; x < FB_WIDTH; x++)
        bottom_changed |= ((got.data[(y / 8u) * FB_WIDTH + x] ^ want.data[(y / 8u) * FB_WIDTH + x]) & (1u << (y % 8u))) != 0;
    assert(top_changed && bottom_changed);
    handle_business_card_key(&app, KEY_C, 0);
    handle_business_card_key(&app, KEY_DOWN, 0);
    accept = false; handle_business_card_key(&app, KEY_NAVI, 0);
    assert(note_sid == 0x3b3u && note_return == APP_ROUTE_BUSINESS_CARD && !app.business_card_action_pending);
    app.route = APP_ROUTE_BUSINESS_CARD; accept = true;
    handle_business_card_key(&app, KEY_NAVI, 0);
    assert(saves == 1u && app.business_card_action_pending);
    handle_business_card_key(&app, KEY_NAVI, 0);
    assert(saves == 1u && !tick_business_card(&app, 0));
    completed = true; result.outcome = PHONEBOOK_OUTCOME_FULL;
    assert(tick_business_card(&app, 0) && note_sid == 0x280u && !app.business_card_action_pending);
    app.route = APP_ROUTE_BUSINESS_CARD;
    handle_business_card_key(&app, KEY_NAVI, 0);
    completed = true; result.outcome = PHONEBOOK_OUTCOME_OK;
    assert(tick_business_card(&app, 0) && note_sid == 0x296u && note_record == 26u && note_return == APP_ROUTE_STANDBY);
    open_received_business_card(&app); handle_business_card_key(&app, KEY_C, 0);
    assert(discards == 1u);
    pending = 0; completed = true; result.outcome = PHONEBOOK_OUTCOME_OK;
    assert(tick_business_card(&app, 0) && app.route == APP_ROUTE_STANDBY);
    pending = 7; open_received_business_card(&app);
    handle_business_card_key(&app, KEY_C, 0);
    app.route = APP_ROUTE_INCOMING_CALL;
    unsigned old_notes = notes;
    completed = true; result.outcome = PHONEBOOK_OUTCOME_ERROR;
    assert(tick_business_card(&app, 0) && notes == old_notes && app.route == APP_ROUTE_INCOMING_CALL);
    return 0;
}

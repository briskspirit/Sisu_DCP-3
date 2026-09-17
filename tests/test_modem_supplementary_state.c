#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "services/modem_supplementary_state.h"

static int s_failures;

static void check(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        s_failures++;
    }
}

static call_forward_request_t request(call_forward_reason_t reason,
                                      call_forward_action_t action) {
    call_forward_request_t value;
    memset(&value, 0, sizeof(value));
    value.reason = reason;
    value.action = action;
    return value;
}

static modem_aux_event_t mwi_event(modem_message_waiting_category_t category,
                                   bool active, uint16_t count) {
    modem_aux_event_t event;
    memset(&event, 0, sizeof(event));
    event.kind = MODEM_AUX_EVENT_MESSAGE_WAITING;
    event.message_waiting_category = category;
    event.active = active;
    event.count = count;
    return event;
}

static modem_supplementary_cache_t cache_snapshot(void) {
    modem_supplementary_cache_t cache;
    modem_supplementary_get_cache(&cache);
    return cache;
}

static void test_request_ids_and_result_ordering(void) {
    modem_supplementary_init();
    uint32_t first = modem_supplementary_call_forward_next_request_id();
    uint32_t second = modem_supplementary_call_forward_next_request_id();
    check(first == 1u && second == 2u,
          "request ids are nonzero and monotonic");

    call_forward_request_t newer = request(
        CALL_FORWARD_REASON_BUSY, CALL_FORWARD_ACTION_QUERY);
    call_forward_request_t older = request(
        CALL_FORWARD_REASON_UNCONDITIONAL, CALL_FORWARD_ACTION_QUERY);
    modem_supplementary_call_forward_publish_result(
        second, &newer, CALL_FORWARD_OUTCOME_SUCCESS,
        true, false, "", false, 0u);
    modem_supplementary_call_forward_publish_result(
        first, &older, CALL_FORWARD_OUTCOME_NOT_DONE,
        false, false, NULL, false, 0u);

    call_forward_result_t result;
    check(modem_supplementary_call_forward_pop_result(&result) &&
              result.request_id == second &&
              result.request.reason == CALL_FORWARD_REASON_BUSY &&
              result.outcome == CALL_FORWARD_OUTCOME_SUCCESS,
          "older completion cannot replace a pending newer result");
    check(!modem_supplementary_call_forward_pop_result(&result),
          "result latch is acknowledged exactly once");
}

static void test_call_forward_query_and_conflict(void) {
    modem_supplementary_init();
    call_forward_request_t query = request(
        CALL_FORWARD_REASON_UNCONDITIONAL, CALL_FORWARD_ACTION_QUERY);
    uint32_t id = modem_supplementary_call_forward_next_request_id();
    modem_supplementary_call_forward_begin(1u);

    call_forward_row_t row;
    memset(&row, 0, sizeof(row));
    row.active = true;
    row.class_mask = 1u;
    row.has_number = true;
    strcpy(row.number, "+15551234567");
    row.has_delay = true;
    row.delay_seconds = 25u;
    modem_supplementary_call_forward_collect_row(&row);
    uint8_t next_step = 0xffu;
    check(!modem_supplementary_call_forward_step_complete(
              true, false, true, &next_step) && next_step == 0u,
          "single-step query reaches completion");

    bool status_updated = false;
    check(modem_supplementary_call_forward_finish(
              id, &query, true, false, true, 10u, &status_updated) &&
              status_updated,
          "authoritative CFU query succeeds and updates status");
    modem_supplementary_cache_t cache = cache_snapshot();
    check(cache.call_forward_unconditional_known &&
              cache.call_forward_unconditional_active,
          "CFU cache records an active authoritative query");
    call_forward_result_t result;
    check(modem_supplementary_call_forward_pop_result(&result) &&
              result.status_known && result.active &&
              strcmp(result.number, "+15551234567") == 0 &&
              result.has_delay && result.delay_seconds == 25u,
          "query result carries normalized number and delay");

    modem_supplementary_call_forward_begin(1u);
    modem_supplementary_call_forward_collect_row(&row);
    row.delay_seconds = 20u;
    modem_supplementary_call_forward_collect_row(&row);
    check(!modem_supplementary_call_forward_finish(
              modem_supplementary_call_forward_next_request_id(), &query,
              true, false, true, 20u, &status_updated),
          "conflicting duplicate voice rows are result-unknown");
    check(modem_supplementary_call_forward_pop_result(&result) &&
              result.outcome == CALL_FORWARD_OUTCOME_RESULT_UNKNOWN &&
              !result.status_known,
          "conflict cannot publish guessed forwarding state");
}

static void test_call_forward_partial_mutation(void) {
    modem_supplementary_init();
    modem_aux_event_t cfu = {
        .kind = MODEM_AUX_EVENT_CFU_STATE,
        .active = true,
    };
    check(modem_supplementary_apply_aux_event(&cfu),
          "CFU URC seeds authoritative cache");

    call_forward_request_t mutation = request(
        CALL_FORWARD_REASON_ALL, CALL_FORWARD_ACTION_DISABLE);
    uint32_t id = modem_supplementary_call_forward_next_request_id();
    modem_supplementary_call_forward_begin(2u);
    uint8_t next_step = 0u;
    check(modem_supplementary_call_forward_step_complete(
              true, false, false, &next_step) && next_step == 1u,
          "first successful mutation advances composite request");
    check(modem_supplementary_call_forward_cancel_uncertain(false, false),
          "a completed first step makes later cancellation uncertain");
    check(!modem_supplementary_call_forward_step_complete(
              false, false, false, &next_step),
          "failed second step terminates composite request");
    bool status_updated = false;
    check(!modem_supplementary_call_forward_finish(
              id, &mutation, false, false, true, 100u, &status_updated) &&
              status_updated,
          "partial mutation returns failure and invalidates CFU authority");
    modem_supplementary_cache_t cache = cache_snapshot();
    check(!cache.call_forward_unconditional_known &&
              cache.call_forward_unconditional_active,
          "invalidation preserves the last value but marks it unknown");
    check(modem_supplementary_refresh_due(
              MODEM_SUPPLEMENTARY_REFRESH_CFU, 100u),
          "possible CFU mutation schedules immediate reconciliation");
    call_forward_result_t result;
    check(modem_supplementary_call_forward_pop_result(&result) &&
              result.outcome == CALL_FORWARD_OUTCOME_RESULT_UNKNOWN,
          "partial mutation reports Result unknown");
}

static void test_refresh_generation_and_retry(void) {
    modem_supplementary_init();
    modem_supplementary_refresh_arm(
        MODEM_SUPPLEMENTARY_REFRESH_VOICE_MAILBOX, 10u);
    check(modem_supplementary_refresh_due(
              MODEM_SUPPLEMENTARY_REFRESH_VOICE_MAILBOX, 10u),
          "armed refresh is immediately due");
    modem_supplementary_refresh_begin(
        MODEM_SUPPLEMENTARY_REFRESH_VOICE_MAILBOX);
    modem_supplementary_refresh_arm(
        MODEM_SUPPLEMENTARY_REFRESH_VOICE_MAILBOX, 20u);
    modem_supplementary_voice_mailbox_begin();
    check(!modem_supplementary_voice_mailbox_finish(false, 100u),
          "failed stale mailbox query remains failed");
    check(modem_supplementary_refresh_due(
              MODEM_SUPPLEMENTARY_REFRESH_VOICE_MAILBOX, 20u),
          "older final cannot consume a newer refresh generation");

    modem_supplementary_refresh_begin(
        MODEM_SUPPLEMENTARY_REFRESH_VOICE_MAILBOX);
    modem_supplementary_voice_mailbox_begin();
    (void)modem_supplementary_voice_mailbox_finish(false, 100u);
    check(!modem_supplementary_refresh_due(
              MODEM_SUPPLEMENTARY_REFRESH_VOICE_MAILBOX, 1099u) &&
              modem_supplementary_refresh_due(
                  MODEM_SUPPLEMENTARY_REFRESH_VOICE_MAILBOX, 1100u),
          "failed current generation retries once after the fixed backoff");
    modem_supplementary_refresh_begin(
        MODEM_SUPPLEMENTARY_REFRESH_VOICE_MAILBOX);
    modem_supplementary_voice_mailbox_begin();
    (void)modem_supplementary_voice_mailbox_finish(false, 1200u);
    check(!modem_supplementary_refresh_due(
              MODEM_SUPPLEMENTARY_REFRESH_VOICE_MAILBOX, UINT32_MAX),
          "retry budget drains after the bounded second failure");
}

static void test_local_forwarding_flags(void) {
    modem_supplementary_init();
    modem_aux_event_t cfu = {
        .kind = MODEM_AUX_EVENT_CFU_STATE,
        .active = true,
    };
    (void)modem_supplementary_apply_aux_event(&cfu);
    modem_supplementary_refresh_arm(MODEM_SUPPLEMENTARY_REFRESH_CFU, 10u);
    modem_supplementary_refresh_begin(MODEM_SUPPLEMENTARY_REFRESH_CFU);
    check(!modem_supplementary_call_forward_flags_finish(false, 20u) &&
              cache_snapshot().call_forward_unconditional_known &&
              cache_snapshot().call_forward_unconditional_active &&
              !modem_supplementary_refresh_due(MODEM_SUPPLEMENTARY_REFRESH_CFU, 1019u) &&
              modem_supplementary_refresh_due(MODEM_SUPPLEMENTARY_REFRESH_CFU, 1020u),
          "failed flags read preserves evidence and schedules a bounded retry");
    modem_supplementary_refresh_begin(MODEM_SUPPLEMENTARY_REFRESH_CFU);
    (void)modem_supplementary_call_forward_flags_finish(false, 1030u);
    check(!modem_supplementary_refresh_due(MODEM_SUPPLEMENTARY_REFRESH_CFU, 2030u),
          "flags read stops retrying after the second failure");

    modem_supplementary_refresh_arm(MODEM_SUPPLEMENTARY_REFRESH_CFU, 3000u);
    modem_supplementary_refresh_begin(MODEM_SUPPLEMENTARY_REFRESH_CFU);
    check(modem_supplementary_call_forward_flags_finish(true, 3010u) &&
              !cache_snapshot().call_forward_unconditional_known &&
              cache_snapshot().call_forward_unconditional_active &&
              !modem_supplementary_refresh_due(MODEM_SUPPLEMENTARY_REFRESH_CFU, 4010u),
          "valid flags-absent reply marks the old value unknown without retrying");

    (void)modem_supplementary_apply_aux_event(&cfu);
    modem_supplementary_refresh_arm(MODEM_SUPPLEMENTARY_REFRESH_CFU, 5000u);
    modem_supplementary_refresh_begin(MODEM_SUPPLEMENTARY_REFRESH_CFU);
    modem_supplementary_refresh_arm(MODEM_SUPPLEMENTARY_REFRESH_CFU, 5010u);
    check(!modem_supplementary_call_forward_flags_finish(true, 5020u) &&
              cache_snapshot().call_forward_unconditional_known &&
              modem_supplementary_refresh_due(MODEM_SUPPLEMENTARY_REFRESH_CFU, 5020u),
          "older flags-absent final cannot consume a newly armed refresh");

    modem_supplementary_refresh_begin(MODEM_SUPPLEMENTARY_REFRESH_CFU);
    cfu.active = false;
    (void)modem_supplementary_apply_aux_event(&cfu);
    check(!modem_supplementary_call_forward_flags_finish(true, 5030u) &&
              cache_snapshot().call_forward_unconditional_known &&
              !cache_snapshot().call_forward_unconditional_active &&
              !modem_supplementary_refresh_due(MODEM_SUPPLEMENTARY_REFRESH_CFU, 5030u),
          "newer explicit flags survive the older query final");
}

static void test_voice_mailbox_cache(void) {
    modem_supplementary_init();
    modem_supplementary_voice_mailbox_begin();
    modem_voice_mailbox_row_t data = {
        .index = 1u,
        .voice = false,
    };
    strcpy(data.number, "data");
    modem_supplementary_voice_mailbox_collect_row(&data);
    modem_voice_mailbox_row_t voice = {
        .index = 2u,
        .voice = true,
    };
    strcpy(voice.number, "+18005551212");
    modem_supplementary_voice_mailbox_collect_row(&voice);
    strcpy(voice.number, "+19999999999");
    modem_supplementary_voice_mailbox_collect_row(&voice);
    check(modem_supplementary_voice_mailbox_finish(true, 0u),
          "valid mailbox snapshot succeeds");
    char number[MODEM_PHONE_MAX + 1u];
    check(modem_supplementary_voice_mailbox_get(number, sizeof(number)) &&
              strcmp(number, "+18005551212") == 0,
          "first voice mailbox row wins over nonvoice and duplicate rows");

    modem_supplementary_voice_mailbox_begin();
    modem_supplementary_voice_mailbox_collect_invalid();
    check(!modem_supplementary_voice_mailbox_finish(true, 0u) &&
              modem_supplementary_voice_mailbox_get(number, sizeof(number)) &&
              strcmp(number, "+18005551212") == 0,
          "malformed refresh preserves the last authoritative mailbox");
}

static void test_message_waiting_authority(void) {
    modem_supplementary_init();
    modem_aux_event_t voice = mwi_event(
        MODEM_MESSAGE_WAITING_VOICE_LINE_1, true, 2u);
    check(modem_supplementary_apply_aux_event(&voice),
          "valid voice MWI URC applies");

    modem_supplementary_message_waiting_begin();
    modem_aux_event_t ambiguous;
    memset(&ambiguous, 0, sizeof(ambiguous));
    ambiguous.kind = MODEM_AUX_EVENT_MESSAGE_WAITING;
    ambiguous.message_waiting_uncertain_mask =
        modem_message_waiting_category_bit(
            MODEM_MESSAGE_WAITING_VOICE_LINE_1);
    modem_supplementary_message_waiting_collect_row(
        MODEM_MESSAGE_WAITING_ROW_AMBIGUOUS, &ambiguous);
    modem_aux_event_t fax = mwi_event(MODEM_MESSAGE_WAITING_FAX, true, 1u);
    modem_supplementary_message_waiting_collect_row(
        MODEM_MESSAGE_WAITING_ROW_VALID, &fax);
    check(modem_supplementary_message_waiting_finish(true, 0u),
          "mixed authoritative and ambiguous MWI snapshot is syntactically valid");
    modem_supplementary_cache_t cache = cache_snapshot();
    check(!cache.message_waiting_known &&
              cache.message_waiting.category[MODEM_MESSAGE_WAITING_VOICE_LINE_1]
                  .active &&
              cache.message_waiting.category[MODEM_MESSAGE_WAITING_VOICE_LINE_1]
                  .count == 2u &&
              cache.message_waiting.category[MODEM_MESSAGE_WAITING_FAX].active,
          "ambiguous category is preserved while authoritative category updates");

    modem_aux_event_t clear = mwi_event(
        MODEM_MESSAGE_WAITING_ALL, false, 0u);
    check(modem_supplementary_apply_aux_event(&clear),
          "authoritative all-category clear applies");
    cache = cache_snapshot();
    check(cache.message_waiting_known &&
              !cache.message_waiting.category[MODEM_MESSAGE_WAITING_VOICE_LINE_1]
                   .active &&
              !cache.message_waiting.category[MODEM_MESSAGE_WAITING_FAX].active,
          "all-category clear retires every indicator");

    clear.active = true;
    check(!modem_supplementary_apply_aux_event(&clear),
          "active ALL sentinel is rejected as malformed");
}

static void test_reset_ownership(void) {
    modem_supplementary_init();
    modem_aux_event_t cfu = {
        .kind = MODEM_AUX_EVENT_CFU_STATE,
        .active = true,
    };
    (void)modem_supplementary_apply_aux_event(&cfu);
    call_forward_request_t query = request(
        CALL_FORWARD_REASON_BUSY, CALL_FORWARD_ACTION_QUERY);
    modem_supplementary_call_forward_publish_result(
        7u, &query, CALL_FORWARD_OUTCOME_CANCELLED,
        false, false, NULL, false, 0u);
    modem_supplementary_reset_transient();
    modem_supplementary_cache_t cache = cache_snapshot();
    call_forward_result_t result;
    check(cache.call_forward_unconditional_known &&
              cache.call_forward_unconditional_active,
          "transport reset preserves authoritative cache");
    check(modem_supplementary_call_forward_pop_result(&result) &&
              result.request_id == 7u,
          "transport reset preserves completion for the detached UI");
    modem_supplementary_invalidate_cache();
    cache = cache_snapshot();
    check(!cache.call_forward_unconditional_known &&
              !cache.call_forward_unconditional_active,
          "SIM/power invalidation clears cached authority and value");
}

int main(void) {
    test_request_ids_and_result_ordering();
    test_call_forward_query_and_conflict();
    test_call_forward_partial_mutation();
    test_refresh_generation_and_retry();
    test_local_forwarding_flags();
    test_voice_mailbox_cache();
    test_message_waiting_authority();
    test_reset_ownership();

    if (s_failures == 0) {
        printf("test_modem_supplementary_state: OK\n");
    }
    return s_failures != 0;
}

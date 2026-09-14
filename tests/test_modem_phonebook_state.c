#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "services/modem_phonebook_state.h"

static int s_failures;

static void check(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        s_failures++;
    }
}

static modem_phonebook_entry_t make_entry(uint16_t index) {
    modem_phonebook_entry_t entry;
    memset(&entry, 0, sizeof(entry));
    entry.index = index;
    snprintf(entry.name, sizeof(entry.name), "Name%u", (unsigned)index);
    snprintf(entry.number, sizeof(entry.number), "+1555%04u",
             (unsigned)index);
    return entry;
}

static void test_cache_capacity_and_order(void) {
    modem_phonebook_state_init();
    check(!modem_phonebook_state_cache_valid(),
          "unread phonebook does not masquerade as an empty valid cache");
    for (uint16_t i = 0u; i < MODEM_PHONEBOOK_MAX_RECORDS; i++) {
        modem_phonebook_entry_t entry = make_entry((uint16_t)(i + 1u));
        check(modem_phonebook_state_append(&entry),
              "entry inside fixed cache capacity is accepted");
    }
    modem_phonebook_entry_t overflow = make_entry(999u);
    check(!modem_phonebook_state_append(&overflow) &&
              !modem_phonebook_state_append(NULL) &&
              modem_phonebook_state_count() == MODEM_PHONEBOOK_MAX_RECORDS,
          "full cache and null input are rejected without changing count");

    modem_phonebook_entry_t first;
    modem_phonebook_entry_t last;
    check(modem_phonebook_state_entry(0u, &first) &&
              modem_phonebook_state_entry(
                  MODEM_PHONEBOOK_MAX_RECORDS - 1u, &last) &&
              first.index == 1u &&
              last.index == MODEM_PHONEBOOK_MAX_RECORDS,
          "cache preserves modem row order through its final slot");
    check(!modem_phonebook_state_entry(MODEM_PHONEBOOK_MAX_RECORDS, &last) &&
              !modem_phonebook_state_entry(0u, NULL),
          "cache read enforces position and output boundaries");

    modem_phonebook_state_clear();
    check(modem_phonebook_state_count() == 0u &&
              !modem_phonebook_state_entry(0u, &first) &&
              !modem_phonebook_state_cache_valid(),
          "refresh clear atomically retires the visible row count");
}

static void test_result_journal_and_reservations(void) {
    modem_phonebook_state_init();
    modem_phonebook_result_t result;
    check(!modem_phonebook_state_pop_result(&result) &&
              !modem_phonebook_state_pop_result(NULL),
          "result journal starts empty and validates its output");

    uint32_t first = 0u;
    uint32_t second = 0u;
    check(modem_phonebook_state_reserve_request(&first) && first != 0u &&
              modem_phonebook_state_reserve_request(&second) &&
              second != 0u && second != first,
          "accepted operations receive distinct nonzero reservations");
    check(modem_phonebook_state_publish_result(
              first, MODEM_PHONEBOOK_OP_LIST,
              MODEM_PHONEBOOK_OUTCOME_OK, false) &&
              !modem_phonebook_state_publish_result(
                  first, MODEM_PHONEBOOK_OP_LIST,
                  MODEM_PHONEBOOK_OUTCOME_ERROR, false) &&
              modem_phonebook_state_publish_result(
                  second, MODEM_PHONEBOOK_OP_UPDATE,
                  MODEM_PHONEBOOK_OUTCOME_TIMEOUT, true),
          "a reservation accepts exactly one terminal result");
    modem_phonebook_state_clear();
    check(modem_phonebook_state_pop_result(&result) &&
              result.request_id == first &&
              result.kind == MODEM_PHONEBOOK_OP_LIST &&
              result.outcome == MODEM_PHONEBOOK_OUTCOME_OK &&
              !result.sim_not_ready &&
              modem_phonebook_state_pop_result(&result) &&
              result.request_id == second &&
              result.kind == MODEM_PHONEBOOK_OP_UPDATE &&
              result.outcome == MODEM_PHONEBOOK_OUTCOME_TIMEOUT &&
              result.sim_not_ready,
          "terminal results retain FIFO identity and survive cache clear");
    check(!modem_phonebook_state_pop_result(&result) &&
              !modem_phonebook_state_release_request(first),
          "acknowledged and completed reservations cannot complete twice");

    uint32_t reservations[MODEM_PHONEBOOK_RESULT_CAPACITY];
    memset(reservations, 0, sizeof(reservations));
    bool filled = true;
    for (uint8_t i = 0u; i < MODEM_PHONEBOOK_RESULT_CAPACITY; i++) {
        filled = filled &&
            modem_phonebook_state_reserve_request(&reservations[i]);
    }
    uint32_t overflow_id = 123u;
    check(filled &&
              !modem_phonebook_state_reserve_request(&overflow_id) &&
              overflow_id == 123u,
          "admission stops before terminal storage can be overcommitted");
    check(modem_phonebook_state_release_request(reservations[3]) &&
              !modem_phonebook_state_release_request(reservations[3]) &&
              modem_phonebook_state_reserve_request(&overflow_id) &&
              overflow_id != 0u,
          "a pre-admission release returns exactly one journal reservation");

    modem_phonebook_state_init();
    check(!modem_phonebook_state_pop_result(&result) &&
              modem_phonebook_state_count() == 0u,
          "service initialization resets cache, reservations, and terminals");
}

static void test_refresh_publication_is_atomic(void) {
    modem_phonebook_state_init();
    modem_phonebook_entry_t old = make_entry(7u);
    modem_phonebook_entry_t high = make_entry(500u);
    check(modem_phonebook_state_append(&old) &&
              modem_phonebook_state_count() == 1u,
          "fixture seeds one visible pre-refresh contact");

    modem_phonebook_state_refresh_begin();
    check(modem_phonebook_state_count() == 0u &&
              modem_phonebook_state_append(&high) &&
              modem_phonebook_state_count() == 0u,
          "staged rows remain invisible until a clean final");
    check(modem_phonebook_state_refresh_finish(true) &&
              modem_phonebook_state_count() == 1u &&
              modem_phonebook_state_cache_valid(),
          "clean final atomically publishes the staged count");
    modem_phonebook_entry_t visible;
    check(modem_phonebook_state_entry(0u, &visible) &&
              visible.index == MODEM_PHONEBOOK_LAST_INDEX,
          "published refresh preserves a high 16-bit modem index");

    modem_phonebook_state_refresh_begin();
    check(modem_phonebook_state_append(&old) &&
              modem_phonebook_state_refresh_finish(false) &&
              modem_phonebook_state_count() == 0u &&
              !modem_phonebook_state_entry(0u, &visible) &&
              !modem_phonebook_state_cache_valid(),
          "failed final discards every staged row");
    check(!modem_phonebook_state_refresh_finish(true),
          "a refresh cannot be finalized twice");

    modem_phonebook_state_refresh_begin();
    check(modem_phonebook_state_refresh_finish(true) &&
              modem_phonebook_state_count() == 0u &&
              modem_phonebook_state_cache_valid(),
          "a clean empty phonebook is valid and needs no repeated refresh");
}

int main(void) {
    test_cache_capacity_and_order();
    test_result_journal_and_reservations();
    test_refresh_publication_is_atomic();
    if (s_failures == 0) {
        printf("test_modem_phonebook_state: OK\n");
    }
    return s_failures != 0;
}

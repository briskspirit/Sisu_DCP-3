#include "services/business_card_service.h"

#include <string.h>
#include "services/phonebook_service.h"
#include "storage/storage_user_space.h"
#include "storage/store_health.h"
#include "storage/store_service.h"

#define CARD_DISCARDED 1u
#define CARD_SAVED 2u

typedef struct {
    phonebook_entry_t entry;
    uint8_t state;
} card_t;

static card_t s_cards[BUSINESS_CARD_LIMIT];
static uint8_t s_count;
static bool s_ready, s_done;
static bool s_maintenance_scheduled;
static uint32_t s_maintenance_at;
static business_card_action_result_t s_action;
static message_file_t s_file;
static uint8_t s_wire[MESSAGE_FILE_WIRE_MAX], s_compare[MESSAGE_FILE_WIRE_MAX];

static int find(uint32_t id) {
    for (unsigned i = 0u; i < s_count; i++)
        if (s_cards[i].entry.index == id) return (int)i;
    return -1;
}

static storage_record_result_t load(uint32_t id, card_t *card) {
    size_t len;
    storage_record_result_t rc = storage_object_read(STORAGE_OBJECT_BUSINESS_CARD, id,
                                                     s_wire, sizeof(s_wire), &len);
    if (rc != STORAGE_RECORD_OK) return rc;
    if (!message_file_decode(&s_file, s_wire, len) ||
        business_card_file_decode(&s_file, &card->entry) != BUSINESS_CARD_VALID)
        return STORAGE_RECORD_CORRUPT;
    uint32_t state;
    rc = storage_object_get_state(STORAGE_OBJECT_BUSINESS_CARD, id, &state);
    if (rc != STORAGE_RECORD_OK) return rc;
    if (state > CARD_SAVED) return STORAGE_RECORD_CORRUPT;
    card->entry.index = id;
    card->state = (uint8_t)state;
    return STORAGE_RECORD_OK;
}

static storage_record_result_t reload(void) {
    s_ready = false;
    s_count = 0u;
    storage_record_result_t rc = storage_object_scan_begin(STORAGE_OBJECT_BUSINESS_CARD);
    if (rc == STORAGE_RECORD_NOT_FOUND) {
        s_ready = true;
        return STORAGE_RECORD_OK;
    }
    if (rc != STORAGE_RECORD_OK) return rc;
    uint32_t id;
    while ((rc = storage_object_scan_next(&id)) == STORAGE_RECORD_OK) {
        card_t card;
        rc = load(id, &card);
        if (rc == STORAGE_RECORD_CORRUPT) {
            store_health_note_corrupt(STORAGE_OBJECT_BUSINESS_CARD, id);
            continue;
        }
        if (rc != STORAGE_RECORD_OK) break;
        if (s_count == BUSINESS_CARD_LIMIT) { rc = STORAGE_RECORD_FULL; break; }
        s_cards[s_count++] = card;
    }
    storage_object_scan_end();
    s_ready = rc == STORAGE_RECORD_NOT_FOUND;
    if (!s_ready) s_count = 0u;
    return s_ready ? STORAGE_RECORD_OK : rc;
}

void business_card_service_init(void) {
    memset(&s_action, 0, sizeof(s_action));
    s_done = false;
    s_maintenance_scheduled = false;
    if (reload() != STORAGE_RECORD_OK) store_service_require_service(STORE_BOOT_FAULT_COLLECTION);
}

bool business_card_service_ready(void) { return s_ready; }
bool business_card_service_idle(void) { return s_action.id == 0u || s_done; }

uint32_t business_card_service_pending_first(void) {
    uint32_t first = 0u;
    if (!s_ready) return 0u;
    for (unsigned i = 0u; i < s_count; i++) {
        const card_t *c = &s_cards[i];
        if (c->state == 0u && (first == 0u || c->entry.index < first)) first = c->entry.index;
    }
    return first;
}

bool business_card_service_get(uint32_t id, phonebook_entry_t *out) {
    int position = find(id);
    if (!s_ready || out == NULL || position < 0) return false;
    *out = s_cards[position].entry;
    return true;
}

bool business_card_service_request(uint32_t id, bool save) {
    if (id == 0u || s_action.id != 0u || (s_ready && find(id) < 0)) return false;
    s_action = (business_card_action_result_t){.id=id, .save=save};
    s_done = false;
    return true;
}

void business_card_service_tick(void) {
    if (s_action.id == 0u || s_done) return;
    storage_record_result_t rc = s_ready ? STORAGE_RECORD_OK : reload();
    if (rc == STORAGE_RECORD_BUSY) return;
    int position = find(s_action.id);
    phonebook_outcome_t result = PHONEBOOK_OUTCOME_OK;
    if (rc != STORAGE_RECORD_OK || position < 0) result = PHONEBOOK_OUTCOME_ERROR;
    else if (s_cards[position].state != 0u) {
        if (s_action.save && s_cards[position].state != CARD_SAVED) result = PHONEBOOK_OUTCOME_ERROR;
    } else {
        if (s_action.save) result = phonebook_service_import(s_action.id, &s_cards[position].entry);
        if (result == PHONEBOOK_OUTCOME_NONE) return;
        if (result == PHONEBOOK_OUTCOME_OK) {
            uint8_t state = s_action.save ? CARD_SAVED : CARD_DISCARDED;
            rc = storage_object_set_state(STORAGE_OBJECT_BUSINESS_CARD, s_action.id, state);
            if (rc != STORAGE_RECORD_OK) s_ready = false;
            if (rc == STORAGE_RECORD_BUSY) return;
            result = rc == STORAGE_RECORD_OK ? PHONEBOOK_OUTCOME_OK : PHONEBOOK_OUTCOME_ERROR;
            if (rc == STORAGE_RECORD_OK) s_cards[position].state = state;
        }
    }
    s_action.outcome = result;
    s_done = true;
}

bool business_card_service_pop_result(business_card_action_result_t *out) {
    if (!s_done || out == NULL) return false;
    *out = s_action;
    memset(&s_action, 0, sizeof(s_action));
    s_done = false;
    return true;
}

static storage_record_result_t evict_consumed(void) {
    int oldest = -1;
    for (unsigned i = 0u; i < s_count; i++) {
        if (s_cards[i].state != 0u && s_cards[i].entry.index != s_action.id &&
            (oldest < 0 || s_cards[i].entry.index < s_cards[oldest].entry.index)) {
            /* A cut may leave the published source staged. Removing its
             * receipt first would allow that source to become unread again. */
            uint32_t state;
            storage_record_result_t rc = storage_object_get_state(STORAGE_OBJECT_PENDING_SMS,
                s_cards[i].entry.index, &state);
            if (rc == STORAGE_RECORD_OK || rc == STORAGE_RECORD_CORRUPT) continue;
            if (rc != STORAGE_RECORD_NOT_FOUND) { s_ready = false; return rc; }
            oldest = (int)i;
        }
    }
    if (oldest < 0) return STORAGE_RECORD_FULL;
    storage_record_result_t rc = storage_object_remove(STORAGE_OBJECT_BUSINESS_CARD, s_cards[oldest].entry.index);
    if (rc != STORAGE_RECORD_OK && rc != STORAGE_RECORD_NOT_FOUND) { s_ready = false; return rc; }
    s_cards[oldest] = s_cards[--s_count];
    return STORAGE_RECORD_OK;
}

void business_card_service_maintain(uint32_t now_ms) {
    if (!business_card_service_idle() ||
        (s_maintenance_scheduled && (int32_t)(now_ms - s_maintenance_at) < 0)) return;
    s_maintenance_scheduled = true;
    s_maintenance_at = now_ms + 1000u;
    if (!s_ready && reload() != STORAGE_RECORD_OK) return;
    bool consumed = false;
    for (unsigned i = 0u; i < s_count; i++)
        consumed |= s_cards[i].state != 0u && s_cards[i].entry.index != s_action.id;
    if (!consumed) return;
    static storage_user_usage_t usage;
    if (storage_user_get_usage(&usage) != STORAGE_RECORD_OK) return;
    const storage_user_pool_usage_t *other = &usage.pools[STORAGE_USER_OTHER];
    if (other->allocated_bytes + STORAGE_USER_CARD_HEADROOM_BYTES > other->limit_bytes)
        (void)evict_consumed();
}

storage_record_result_t business_card_service_publish(uint32_t id, const message_file_t *file) {
    storage_record_result_t rc = s_ready ? STORAGE_RECORD_OK : reload();
    if (rc != STORAGE_RECORD_OK) return rc;
    card_t card = {0};
    if (business_card_file_decode(file, &card.entry) != BUSINESS_CARD_VALID) return STORAGE_RECORD_ERROR;
    size_t len, existing_len;
    if (!message_file_encode(file, s_compare, sizeof(s_compare), &len)) return STORAGE_RECORD_ERROR;
    rc = storage_object_read(STORAGE_OBJECT_BUSINESS_CARD, id, s_wire, sizeof(s_wire), &existing_len);
    if (rc == STORAGE_RECORD_OK) {
        if (existing_len != len || memcmp(s_wire, s_compare, len) != 0) return STORAGE_RECORD_CORRUPT;
        rc = load(id, &card);
    } else if (rc == STORAGE_RECORD_NOT_FOUND) {
        if (s_count == BUSINESS_CARD_LIMIT) {
            rc = evict_consumed();
            if (rc != STORAGE_RECORD_OK) return rc;
        }
        while ((rc = storage_object_write(STORAGE_OBJECT_BUSINESS_CARD, id, s_compare, len)) == STORAGE_RECORD_FULL) {
            rc = evict_consumed();
            if (rc != STORAGE_RECORD_OK) return rc;
        }
    }
    if (rc != STORAGE_RECORD_OK) {
        s_ready = false;
        if (rc == STORAGE_RECORD_CORRUPT) store_health_note_corrupt(STORAGE_OBJECT_BUSINESS_CARD, id);
        return rc;
    }
    card.entry.index = id;
    int position = find(id);
    if (position < 0) {
        if (s_count == BUSINESS_CARD_LIMIT) { s_ready = false; return STORAGE_RECORD_FULL; }
        position = s_count++;
    }
    s_cards[position] = card;
    return STORAGE_RECORD_OK;
}

storage_record_result_t business_card_service_duplicate(const char *pdu, bool *duplicate) {
    if (pdu == NULL || duplicate == NULL) return STORAGE_RECORD_ERROR;
    *duplicate = false;
    storage_record_result_t rc = s_ready ? STORAGE_RECORD_OK : reload();
    if (rc != STORAGE_RECORD_OK) return rc;
    for (unsigned i = 0u; i < s_count; i++) {
        card_t card;
        rc = load(s_cards[i].entry.index, &card);
        if (rc != STORAGE_RECORD_OK) { s_ready = false; return rc; }
        if (message_file_merge(&s_file, pdu) == MESSAGE_MERGE_DUPLICATE) {
            *duplicate = true;
            return STORAGE_RECORD_OK;
        }
    }
    return STORAGE_RECORD_OK;
}

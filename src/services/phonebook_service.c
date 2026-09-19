#include "services/phonebook_service.h"

#include <string.h>

#include "storage/storage_objects.h"
#include "storage/storage_user_space.h"
#include "storage/store_service.h"
#include "storage/store_health.h"

#define CONTACT_HEADER 4u
#define CONTACT_WIRE_MAX (CONTACT_HEADER + PHONEBOOK_NAME_MAX + MODEM_PHONE_MAX)

typedef struct {
    uint32_t request_id;
    phonebook_op_t kind;
    phonebook_entry_t entry;
    bool attempted;
} request_t;

static phonebook_entry_t s_contacts[PHONEBOOK_MAX_RECORDS];
static uint16_t s_count;
static bool s_ready;
static bool s_has_corrupt;
static request_t s_requests[PHONEBOOK_RESULT_CAPACITY];
static phonebook_result_t s_results[PHONEBOOK_RESULT_CAPACITY];
static uint8_t s_request_head, s_request_count, s_result_head, s_result_count;
static uint32_t s_next_request;
static uint32_t s_used_bytes, s_limit_bytes;
static bool s_space_valid;

static void refresh_space(void) {
    storage_user_usage_t usage;
    s_space_valid = storage_user_get_usage(&usage) == STORAGE_RECORD_OK;
    if (s_space_valid) {
        s_used_bytes = usage.pools[STORAGE_USER_CONTACTS].allocated_bytes;
        s_limit_bytes = usage.pools[STORAGE_USER_CONTACTS].limit_bytes;
    }
}

static bool contact_exists(uint32_t id) {
    for (uint16_t i = 0u; i < s_count; i++) {
        if (s_contacts[i].index == id) return true;
    }
    return false;
}

static bool decode(uint32_t id, const uint8_t *wire, size_t len,
                   phonebook_entry_t *entry) {
    if (len < CONTACT_HEADER || wire[0] != 'C' || wire[1] != 1u ||
        wire[2] > PHONEBOOK_NAME_MAX || wire[3] == 0u ||
        wire[3] > MODEM_PHONE_MAX ||
        len != CONTACT_HEADER + (size_t)wire[2] + wire[3] ||
        memchr(wire + CONTACT_HEADER, 0, len - CONTACT_HEADER) != NULL) return false;
    memset(entry, 0, sizeof(*entry));
    entry->index = id;
    memcpy(entry->name, wire + CONTACT_HEADER, wire[2]);
    memcpy(entry->number, wire + CONTACT_HEADER + wire[2], wire[3]);
    return true;
}

static size_t encode(const phonebook_entry_t *entry, uint8_t *wire) {
    size_t name_len = strlen(entry->name), number_len = strlen(entry->number);
    wire[0] = 'C'; wire[1] = 1u;
    wire[2] = (uint8_t)name_len; wire[3] = (uint8_t)number_len;
    memcpy(wire + CONTACT_HEADER, entry->name, name_len);
    memcpy(wire + CONTACT_HEADER + name_len, entry->number, number_len);
    return CONTACT_HEADER + name_len + number_len;
}

static storage_record_result_t reload(void) {
    s_ready = false;
    s_count = 0u;
    s_has_corrupt = false;
    storage_record_result_t rc = storage_object_scan_begin(STORAGE_OBJECT_CONTACT);
    if (rc != STORAGE_RECORD_OK) return rc;
    uint32_t id;
    while ((rc = storage_object_scan_next(&id)) == STORAGE_RECORD_OK) {
        uint8_t wire[CONTACT_WIRE_MAX];
        size_t len = 0u;
        rc = storage_object_read(STORAGE_OBJECT_CONTACT, id, wire, sizeof(wire), &len);
        phonebook_entry_t entry;
        if (rc == STORAGE_RECORD_OK && !decode(id, wire, len, &entry))
            rc = STORAGE_RECORD_CORRUPT;
        if (rc == STORAGE_RECORD_CORRUPT) {
            store_health_note_corrupt(STORAGE_OBJECT_CONTACT, id);
            s_has_corrupt = true;
            continue;
        }
        if (rc != STORAGE_RECORD_OK) break;
        if (s_count == PHONEBOOK_MAX_RECORDS) { rc = STORAGE_RECORD_FULL; break; }
        s_contacts[s_count++] = entry;
    }
    storage_object_scan_end();
    if (rc == STORAGE_RECORD_NOT_FOUND) {
        s_ready = true;
        /* A binding to a preserved damaged contact is not an orphan. */
        if (!s_has_corrupt) store_phonebook_prune_bindings(contact_exists);
        return STORAGE_RECORD_OK;
    }
    s_count = 0u;
    return rc;
}

void phonebook_service_init(void) {
    s_request_head = s_request_count = s_result_head = s_result_count = 0u;
    s_next_request = 0u;
    memset(s_requests, 0, sizeof(s_requests));
    memset(s_results, 0, sizeof(s_results));
    if (reload() != STORAGE_RECORD_OK)
        store_service_require_service(STORE_BOOT_FAULT_COLLECTION);
    refresh_space();
}

static int find(uint32_t id) {
    for (uint16_t i = 0u; i < s_count; i++) {
        if (s_contacts[i].index == id) return i;
    }
    return -1;
}

static bool queue(phonebook_op_t kind, uint32_t id, const char *name,
                  const char *number, uint32_t *request_id) {
    if (request_id == NULL) return false;
    *request_id = 0u;
    if (s_request_count + s_result_count >= PHONEBOOK_RESULT_CAPACITY ||
        ((kind == PHONEBOOK_OP_UPDATE || kind == PHONEBOOK_OP_DELETE) && id == 0u))
        return false;
    if ((kind == PHONEBOOK_OP_ADD || kind == PHONEBOOK_OP_UPDATE) &&
        (name == NULL || number == NULL || number[0] == '\0' ||
         strlen(name) > PHONEBOOK_NAME_MAX || strlen(number) > MODEM_PHONE_MAX))
        return false;
    uint8_t tail = (s_request_head + s_request_count) % PHONEBOOK_RESULT_CAPACITY;
    request_t *request = &s_requests[tail];
    memset(request, 0, sizeof(*request));
    /* At most eight outstanding IDs; skip all live IDs at wrap. */
    bool used;
    do {
        s_next_request++;
        used = s_next_request == 0u;
        for (uint8_t i = 0; i < PHONEBOOK_RESULT_CAPACITY; i++) {
            used |= s_requests[i].request_id == s_next_request ||
                    s_results[i].request_id == s_next_request;
        }
    } while (used);
    request->request_id = s_next_request;
    request->kind = kind;
    request->entry.index = id;
    if (name != NULL) strcpy(request->entry.name, name);
    if (number != NULL) strcpy(request->entry.number, number);
    *request_id = request->request_id;
    s_request_count++;
    return true;
}

bool phonebook_service_request_list(uint32_t *id) {
    return queue(PHONEBOOK_OP_LIST, 0u, NULL, NULL, id);
}
bool phonebook_service_request_add(const char *name, const char *number, uint32_t *id) {
    return queue(PHONEBOOK_OP_ADD, 0u, name, number, id);
}
bool phonebook_service_request_update(uint32_t index, const char *name,
                                      const char *number, uint32_t *id) {
    return queue(PHONEBOOK_OP_UPDATE, index, name, number, id);
}
bool phonebook_service_request_delete(uint32_t index, uint32_t *id) {
    return queue(PHONEBOOK_OP_DELETE, index, NULL, NULL, id);
}

void phonebook_service_tick(void) {
    if (s_request_count == 0u) return;
    request_t *request = &s_requests[s_request_head];
    storage_record_result_t rc = s_ready ? STORAGE_RECORD_OK : reload();
    int position = find(request->entry.index);
    if (rc == STORAGE_RECORD_OK && request->kind == PHONEBOOK_OP_ADD &&
        position < 0 && s_count == PHONEBOOK_MAX_RECORDS) rc = STORAGE_RECORD_FULL;
    if (rc == STORAGE_RECORD_OK &&
        (request->kind == PHONEBOOK_OP_UPDATE || request->kind == PHONEBOOK_OP_DELETE) &&
        position < 0 && !(request->kind == PHONEBOOK_OP_DELETE && request->attempted))
        rc = STORAGE_RECORD_NOT_FOUND;
    if (rc == STORAGE_RECORD_OK && request->kind == PHONEBOOK_OP_ADD &&
        request->entry.index == 0u) rc = storage_object_allocate(&request->entry.index);
    if (rc == STORAGE_RECORD_OK &&
        (request->kind == PHONEBOOK_OP_ADD || request->kind == PHONEBOOK_OP_UPDATE)) {
        uint8_t wire[CONTACT_WIRE_MAX];
        size_t len = encode(&request->entry, wire);
        request->attempted = true;
        rc = storage_object_write(STORAGE_OBJECT_CONTACT, request->entry.index, wire, len);
        if (rc == STORAGE_RECORD_OK) {
            if (position < 0) position = s_count++;
            s_contacts[position] = request->entry;
        }
    } else if (rc == STORAGE_RECORD_OK && request->kind == PHONEBOOK_OP_DELETE &&
               position >= 0) {
        request->attempted = true;
        rc = storage_object_remove(STORAGE_OBJECT_CONTACT, request->entry.index);
        if (rc == STORAGE_RECORD_OK) {
            s_count--;
            memmove(&s_contacts[position], &s_contacts[position + 1],
                    (s_count - (uint16_t)position) * sizeof(s_contacts[0]));
            if (!s_has_corrupt) store_phonebook_prune_bindings(contact_exists);
        }
    }
    /* Even BUSY may follow a committed rename. Invalidate before retrying so
     * the next request never trusts a stale RAM copy after a media failure. */
    if (rc != STORAGE_RECORD_OK) s_ready = false;
    if (rc == STORAGE_RECORD_BUSY) return;
    refresh_space();
    uint8_t tail = (s_result_head + s_result_count) % PHONEBOOK_RESULT_CAPACITY;
    s_results[tail] = (phonebook_result_t){
        .request_id = request->request_id, .kind = request->kind,
        .outcome = rc == STORAGE_RECORD_OK ? PHONEBOOK_OUTCOME_OK :
                   rc == STORAGE_RECORD_FULL ? PHONEBOOK_OUTCOME_FULL : PHONEBOOK_OUTCOME_ERROR,
    };
    s_result_count++;
    memset(request, 0, sizeof(*request));
    s_request_head = (s_request_head + 1u) % PHONEBOOK_RESULT_CAPACITY;
    s_request_count--;
}

bool phonebook_service_pop_result(phonebook_result_t *out) {
    if (out == NULL || s_result_count == 0u) return false;
    *out = s_results[s_result_head];
    memset(&s_results[s_result_head], 0, sizeof(s_results[s_result_head]));
    s_result_head = (s_result_head + 1u) % PHONEBOOK_RESULT_CAPACITY;
    s_result_count--;
    return true;
}
bool phonebook_service_cache_valid(void) { return s_ready; }
bool phonebook_service_idle(void) { return s_request_count == 0u; }
bool phonebook_service_space(uint32_t *used_bytes, uint32_t *limit_bytes) {
    if (!s_space_valid || used_bytes == NULL || limit_bytes == NULL) return false;
    *used_bytes = s_used_bytes;
    *limit_bytes = s_limit_bytes;
    return true;
}
uint16_t phonebook_service_count(void) { return s_ready ? s_count : 0u; }
bool phonebook_service_entry(uint16_t position, phonebook_entry_t *out) {
    if (!s_ready || out == NULL || position >= s_count) return false;
    *out = s_contacts[position];
    return true;
}

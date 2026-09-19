#include "services/message_service.h"

#include <string.h>
#include "services/message_file_codec.h"
#include "storage/storage_objects.h"
#include "storage/store_health.h"
#include "storage/store_service.h"

#define PENDING_LIMIT 64u
#define RECEIVE_LIMIT 8u
#define STATE_READ 1u
#define INDEX_COMPLETE 2u
#define INDEX_QUARANTINED 4u
#define INDEX_CONTROL 8u

typedef struct { uint32_t id, time; uint8_t flags; } index_t;
typedef struct {
    message_result_t result;
    message_mailbox_t mailbox;
    bool done, attempted;
    message_metadata_t *rows;
    uint16_t capacity;
} request_t;
typedef struct {
    union {
        char pdu[MESSAGE_FILE_PDU_MAX * 2u + 1u];
        struct { char address[MODEM_SMS_SENDER_MAX + 1u], text[MODEM_SMS_TEXT_MAX + 1u]; } sent;
    };
    uint32_t id;
    bool outgoing;
} receive_t;

static index_t s_index[2][MESSAGE_MAILBOX_LIMIT], s_pending[PENDING_LIMIT];
static uint16_t s_count[2], s_pending_count;
static request_t s_requests[MESSAGE_OP_COUNT];
static uint32_t s_token, s_retry_at;
static bool s_retry_pending;
static receive_t s_receive[RECEIVE_LIMIT];
static uint8_t s_head, s_queued;
static message_status_t s_status;
/* Scratch belongs to the single storage owner, not the MCU's small stack. */
static message_file_t s_file;
static uint8_t s_wire[MESSAGE_FILE_WIRE_MAX];
static uint8_t s_compare[MESSAGE_FILE_WIRE_MAX];
static message_content_t s_content;
static char s_draft_address[MODEM_SMS_SENDER_MAX + 1u];
static char s_draft_text[MODEM_SMS_TEXT_MAX + 1u];

static storage_object_collection_t collection(message_mailbox_t mailbox) {
    return mailbox == MESSAGE_INBOX ? STORAGE_OBJECT_INBOX : STORAGE_OBJECT_OUTBOX;
}

static int find(const index_t *index, uint16_t count, uint32_t id) {
    for (uint16_t i = 0; i < count; i++) if (index[i].id == id) return i;
    return -1;
}

static storage_record_result_t load(storage_object_collection_t c, uint32_t id) {
    size_t len;
    storage_record_result_t rc = storage_object_read(c, id, s_wire, sizeof(s_wire), &len);
    if (rc == STORAGE_RECORD_OK && !message_file_decode(&s_file, s_wire, len))
        rc = STORAGE_RECORD_CORRUPT;
    if (rc == STORAGE_RECORD_CORRUPT) store_health_note_corrupt(c, id);
    return rc;
}

static storage_record_result_t save(storage_object_collection_t c, uint32_t id) {
    size_t len;
    if (!message_file_encode(&s_file, s_wire, sizeof(s_wire), &len)) return STORAGE_RECORD_ERROR;
    return storage_object_write(c, id, s_wire, len);
}

static storage_record_result_t index_current(storage_object_collection_t c, uint32_t id, index_t *entry) {
    message_metadata_t meta;
    uint32_t state;
    if (!message_file_metadata(&s_file, &meta)) return STORAGE_RECORD_CORRUPT;
    storage_record_result_t rc = storage_object_get_state(c, id, &state);
    if (rc != STORAGE_RECORD_OK) return rc;
    bool pending = c == STORAGE_OBJECT_PENDING_SMS;
    if (!pending && (state & ~STATE_READ)) return STORAGE_RECORD_CORRUPT;
    /* Pending objects use their attribute for first local receipt time. Inbox
     * and outbox attributes retain the read flag; publication copies only body. */
    *entry = (index_t){.id=id, .time=pending ? state : message_timestamp_seconds(meta.timestamp),
        .flags=(uint8_t)((pending ? 0u : state) | (message_file_complete(&s_file) ? INDEX_COMPLETE : 0u) |
            (pending && message_file_control(&s_file) != SMS_CONTROL_KEEP ? INDEX_CONTROL : 0u) |
            (meta.quarantined ? INDEX_QUARANTINED : 0u))};
    return STORAGE_RECORD_OK;
}

static void insert(index_t *index, uint16_t *count, const index_t *entry, bool unread_first) {
    uint16_t at = 0;
    for (; at < *count; at++) {
        bool a_read = unread_first && (index[at].flags & STATE_READ);
        bool b_read = unread_first && (entry->flags & STATE_READ);
        if (a_read != b_read) { if (a_read) break; else continue; }
        if (entry->time > index[at].time ||
            (entry->time == index[at].time && entry->id > index[at].id)) break;
    }
    memmove(index + at + 1u, index + at, (*count - at) * sizeof(*index));
    index[at] = *entry;
    (*count)++;
}

static void erase_index(index_t *index, uint16_t *count, unsigned pos) {
    (*count)--;
    memmove(index + pos, index + pos + 1u, (*count - pos) * sizeof(*index));
}

static storage_record_result_t scan(storage_object_collection_t c, index_t *index,
                                    uint16_t *count, uint16_t limit) {
    *count = 0u;
    storage_record_result_t rc = storage_object_scan_begin(c);
    if (rc != STORAGE_RECORD_OK) return rc;
    uint32_t id;
    while ((rc = storage_object_scan_next(&id)) == STORAGE_RECORD_OK) {
        rc = load(c, id);
        if (rc == STORAGE_RECORD_OK) {
            bool draft = (s_file.flags & MESSAGE_FILE_DRAFT) != 0u;
            if ((c == STORAGE_OBJECT_OUTBOX) != draft ||
                (c != STORAGE_OBJECT_PENDING_SMS && !message_file_complete(&s_file)))
                rc = STORAGE_RECORD_CORRUPT;
        }
        index_t entry;
        if (rc == STORAGE_RECORD_OK) rc = index_current(c, id, &entry);
        if (rc == STORAGE_RECORD_CORRUPT) {
            store_health_note_corrupt(c, id);
            continue;
        }
        if (rc != STORAGE_RECORD_OK) break;
        if (*count == limit) { rc = STORAGE_RECORD_FULL; break; }
        insert(index, count, &entry, c == STORAGE_OBJECT_INBOX);
    }
    storage_object_scan_end();
    return rc == STORAGE_RECORD_NOT_FOUND ? STORAGE_RECORD_OK : rc;
}

static void recount(void) {
    s_status.inbox = s_count[MESSAGE_INBOX];
    s_status.outbox = s_count[MESSAGE_OUTBOX];
    s_status.pending = s_pending_count;
    s_status.queued = s_queued;
    s_status.unread = 0u;
    for (uint16_t i = 0; i < s_count[MESSAGE_INBOX]; i++)
        if (!(s_index[MESSAGE_INBOX][i].flags & STATE_READ)) s_status.unread++;
}

static storage_record_result_t reload(void) {
    s_status.ready = false;
    storage_record_result_t rc = scan(STORAGE_OBJECT_INBOX, s_index[0], &s_count[0], MESSAGE_MAILBOX_LIMIT);
    if (rc == STORAGE_RECORD_OK)
        rc = scan(STORAGE_OBJECT_OUTBOX, s_index[1], &s_count[1], MESSAGE_MAILBOX_LIMIT);
    if (rc == STORAGE_RECORD_OK)
        rc = scan(STORAGE_OBJECT_PENDING_SMS, s_pending, &s_pending_count, PENDING_LIMIT);
    if (rc == STORAGE_RECORD_OK) s_status.ready = true;
    else { s_count[0] = s_count[1] = s_pending_count = 0u; }
    recount();
    return rc;
}

static void failure(storage_record_result_t rc, uint32_t now) {
    if (rc == STORAGE_RECORD_FULL) {
        if (!s_status.full) s_status.full_events++;
        s_status.full = true;
    } else if (rc == STORAGE_RECORD_ERROR || rc == STORAGE_RECORD_NOT_FOUND ||
               rc == STORAGE_RECORD_CORRUPT) {
        s_status.storage_error = true;
    }
    /* A failed call may have committed before the I/O error was reported. */
    s_status.ready = false;
    s_retry_at = now + 1000u;
    s_retry_pending = true;
}

void message_service_init(void) {
    memset(&s_status, 0, sizeof(s_status));
    memset(s_requests, 0, sizeof(s_requests));
    memset(s_receive, 0, sizeof(s_receive));
    s_head = s_queued = 0u;
    s_token = s_retry_at = 0u;
    s_retry_pending = false;
    if (reload() != STORAGE_RECORD_OK) {
        s_status.storage_error = true;
        store_service_require_service(STORE_BOOT_FAULT_COLLECTION);
    }
}

static bool request(message_op_t kind, message_mailbox_t mailbox, uint32_t id, uint32_t *token) {
    if (token == NULL) return false;
    *token = 0u;
    if ((unsigned)mailbox > MESSAGE_OUTBOX || s_requests[kind].result.request_id != 0u ||
        ((kind == MESSAGE_OP_READ || kind == MESSAGE_OP_DELETE) && id == 0u)) return false;
    bool used;
    do {
        s_token++;
        used = s_token == 0u;
        for (unsigned i = 0; i < MESSAGE_OP_COUNT; i++) used |= s_requests[i].result.request_id == s_token;
    } while (used);
    s_requests[kind] = (request_t){.result={.request_id=s_token, .object_id=id, .kind=kind}, .mailbox=mailbox};
    *token = s_token;
    return true;
}

bool message_service_request_list(message_mailbox_t m, message_metadata_t *rows,
                                  uint16_t capacity, uint32_t *t) {
    if (rows == NULL || capacity == 0u || !request(MESSAGE_OP_LIST, m, 0u, t)) return false;
    s_requests[MESSAGE_OP_LIST].rows = rows;
    s_requests[MESSAGE_OP_LIST].capacity = capacity;
    return true;
}
bool message_service_request_read(message_mailbox_t m, uint32_t id, uint32_t *t) { return request(MESSAGE_OP_READ, m, id, t); }
bool message_service_request_delete(message_mailbox_t m, uint32_t id, uint32_t *t) { return request(MESSAGE_OP_DELETE, m, id, t); }
bool message_service_request_save(const char *address, const char *text, uint32_t *token) {
    if (address == NULL || text == NULL || strlen(address) > MODEM_SMS_SENDER_MAX ||
        strlen(text) > MODEM_SMS_TEXT_MAX || !request(MESSAGE_OP_SAVE, MESSAGE_OUTBOX, 0u, token)) return false;
    strcpy(s_draft_address, address); strcpy(s_draft_text, text);
    return true;
}

bool message_service_pop_result(uint32_t token, message_result_t *out, message_content_t *content) {
    if (token == 0u || out == NULL) return false;
    for (unsigned i = 0; i < MESSAGE_OP_COUNT; i++) {
        request_t *r = &s_requests[i];
        if (r->result.request_id != token || !r->done) continue;
        *out = r->result;
        if (content != NULL && i == MESSAGE_OP_READ && out->outcome == MESSAGE_RESULT_OK) *content = s_content;
        memset(r, 0, sizeof(*r));
        return true;
    }
    return false;
}

bool message_service_entry(message_mailbox_t mailbox, uint16_t position, message_metadata_t *out) {
    if (!s_status.ready || (unsigned)mailbox > MESSAGE_OUTBOX || position >= s_count[mailbox] || out == NULL)
        return false;
    const index_t *entry = &s_index[mailbox][position];
    if (load(collection(mailbox), entry->id) != STORAGE_RECORD_OK || !message_file_metadata(&s_file, out)) {
        s_status.ready = false; s_status.storage_error = true; return false;
    }
    out->id = entry->id;
    out->read = (entry->flags & STATE_READ) != 0u;
    return true;
}

static storage_record_result_t perform(request_t *r) {
    uint32_t id = r->result.object_id;
    message_mailbox_t m = r->mailbox;
    storage_object_collection_t c = collection(m);
    int pos = find(s_index[m], s_count[m], id);
    storage_record_result_t rc;
    if (r->result.kind == MESSAGE_OP_LIST) {
        if (s_count[m] > r->capacity) return STORAGE_RECORD_FULL;
        for (uint16_t i = 0u; i < s_count[m]; i++)
            if (!message_service_entry(m, i, &r->rows[i])) return STORAGE_RECORD_ERROR;
        r->result.count = s_count[m];
        r->result.revision = s_status.revision;
        return STORAGE_RECORD_OK;
    }
    if (r->result.kind == MESSAGE_OP_SAVE) {
        if (pos < 0 && s_count[m] == MESSAGE_MAILBOX_LIMIT) return STORAGE_RECORD_FULL;
        if (id == 0u) {
            rc = storage_object_allocate(&r->result.object_id);
            if (rc != STORAGE_RECORD_OK) return rc;
            id = r->result.object_id;
        }
        if (!message_file_draft(&s_file, s_draft_address, s_draft_text)) return STORAGE_RECORD_ERROR;
        r->attempted = true;
        rc = save(c, id);
        if (rc != STORAGE_RECORD_OK) return rc;
        index_t entry;
        rc = index_current(c, id, &entry);
        if (rc != STORAGE_RECORD_OK) return rc;
        if (pos >= 0) erase_index(s_index[m], &s_count[m], (unsigned)pos);
        insert(s_index[m], &s_count[m], &entry, false);
    } else if (r->result.kind == MESSAGE_OP_READ) {
        if (pos < 0) return STORAGE_RECORD_NOT_FOUND;
        rc = load(c, id);
        if (rc != STORAGE_RECORD_OK) return rc;
        if (!message_file_content(&s_file, &s_content)) return STORAGE_RECORD_ERROR;
        rc = storage_object_set_state(c, id, STATE_READ);
        if (rc != STORAGE_RECORD_OK) return rc;
        s_content.metadata.id = id; s_content.metadata.read = true;
        index_t entry = s_index[m][pos]; entry.flags |= STATE_READ;
        erase_index(s_index[m], &s_count[m], (unsigned)pos);
        insert(s_index[m], &s_count[m], &entry, m == MESSAGE_INBOX);
    } else {
        if (pos < 0 && !r->attempted) return STORAGE_RECORD_NOT_FOUND;
        /* Remove any duplicate staging copy first, or a cut during deletion
         * could resurrect a published message on the next boot. */
        int pending = find(s_pending, s_pending_count, id);
        if (pending >= 0) {
            rc = storage_object_remove(STORAGE_OBJECT_PENDING_SMS, id);
            if (rc != STORAGE_RECORD_OK && rc != STORAGE_RECORD_NOT_FOUND) return rc;
            erase_index(s_pending, &s_pending_count, (unsigned)pending);
        }
        r->attempted = true;
        rc = storage_object_remove(c, id);
        if (rc != STORAGE_RECORD_OK && rc != STORAGE_RECORD_NOT_FOUND) return rc;
        if (pos >= 0) erase_index(s_index[m], &s_count[m], (unsigned)pos);
        s_status.full = false;
    }
    s_status.revision++;
    return STORAGE_RECORD_OK;
}

bool message_service_receive(const char *pdu) {
    if (pdu == NULL || strlen(pdu) > MESSAGE_FILE_PDU_MAX * 2u) {
        s_status.receive_errors++; return false;
    }
    /* Parsing uses shared scratch, but admission never writes the filesystem. */
    if (!message_file_receive(&s_file, pdu)) { s_status.receive_errors++; return false; }
    if (message_file_control(&s_file) != SMS_CONTROL_KEEP) {
        s_status.filtered_controls++;
        return true;
    }
    if (s_queued == RECEIVE_LIMIT) { s_status.receive_errors++; return false; }
    receive_t *r = &s_receive[(s_head + s_queued) % RECEIVE_LIMIT];
    strcpy(r->pdu, pdu); r->id = 0u; r->outgoing = false; s_queued++;
    s_status.queued = s_queued;
    return true;
}

bool message_service_sent(const char *address, const char *text) {
    if (address == NULL || text == NULL || strlen(address) > MODEM_SMS_SENDER_MAX ||
        strlen(text) > MODEM_SMS_TEXT_MAX || s_queued == RECEIVE_LIMIT) {
        s_status.receive_errors++; return false;
    }
    receive_t *r = &s_receive[(s_head + s_queued) % RECEIVE_LIMIT];
    strcpy(r->sent.address, address); strcpy(r->sent.text, text);
    r->id = 0u; r->outgoing = true; s_queued++;
    s_status.queued = s_queued;
    return true;
}

static storage_record_result_t receive_one(void) {
    receive_t *r = &s_receive[s_head];
    storage_record_result_t rc;
    if (r->outgoing) {
        int pos = find(s_index[MESSAGE_OUTBOX], s_count[MESSAGE_OUTBOX], r->id);
        if (pos >= 0) return STORAGE_RECORD_OK;
        if (s_count[MESSAGE_OUTBOX] == MESSAGE_MAILBOX_LIMIT) return STORAGE_RECORD_FULL;
        if (r->id == 0u) {
            rc = storage_object_allocate(&r->id);
            if (rc != STORAGE_RECORD_OK) return rc;
        }
        if (!message_file_draft(&s_file, r->sent.address, r->sent.text)) return STORAGE_RECORD_ERROR;
        s_file.flags |= MESSAGE_FILE_SENT;
        rc = save(STORAGE_OBJECT_OUTBOX, r->id);
        if (rc != STORAGE_RECORD_OK) return rc;
        index_t entry;
        rc = index_current(STORAGE_OBJECT_OUTBOX, r->id, &entry);
        if (rc == STORAGE_RECORD_OK) {
            insert(s_index[MESSAGE_OUTBOX], &s_count[MESSAGE_OUTBOX], &entry, false);
            s_status.revision++;
        }
        return rc;
    }
    /* Completed inbox messages only suppress byte-identical retransmissions.
     * A reused concatenation reference must not splice a new message into one
     * already shown to the user. */
    for (unsigned group = 0; group < 2u; group++) {
        index_t *index = group == 0u ? s_pending : s_index[MESSAGE_INBOX];
        uint16_t count = group == 0u ? s_pending_count : s_count[MESSAGE_INBOX];
        storage_object_collection_t c = group == 0u ? STORAGE_OBJECT_PENDING_SMS : STORAGE_OBJECT_INBOX;
        for (uint16_t i = 0; i < count; i++) {
            rc = load(c, index[i].id);
            if (rc != STORAGE_RECORD_OK) return rc;
            message_merge_t merged = message_file_merge(&s_file, r->pdu);
            if (merged == MESSAGE_MERGE_DUPLICATE) return STORAGE_RECORD_OK;
            if (group != 0u || merged == MESSAGE_MERGE_UNRELATED) continue;
            if (merged == MESSAGE_MERGE_FULL) return STORAGE_RECORD_FULL;
            r->id = index[i].id;
            rc = save(c, r->id);
            if (rc != STORAGE_RECORD_OK) return rc;
            return index_current(c, r->id, &index[i]);
        }
    }
    if (s_pending_count == PENDING_LIMIT) return STORAGE_RECORD_FULL;
    if (r->id == 0u) {
        rc = storage_object_allocate(&r->id);
        if (rc != STORAGE_RECORD_OK) return rc;
    }
    if (!message_file_receive(&s_file, r->pdu)) return STORAGE_RECORD_ERROR;
    rc = save(STORAGE_OBJECT_PENDING_SMS, r->id);
    if (rc != STORAGE_RECORD_OK) return rc;
    index_t entry;
    rc = index_current(STORAGE_OBJECT_PENDING_SMS, r->id, &entry);
    if (rc == STORAGE_RECORD_OK) insert(s_pending, &s_pending_count, &entry, false);
    return rc;
}

static storage_record_result_t publish(unsigned pos) {
    uint32_t id = s_pending[pos].id;
    int existing = find(s_index[MESSAGE_INBOX], s_count[MESSAGE_INBOX], id);
    storage_record_result_t rc = load(STORAGE_OBJECT_PENDING_SMS, id);
    if (rc != STORAGE_RECORD_OK) return rc;
    if (existing < 0 && message_file_control(&s_file) != SMS_CONTROL_KEEP) {
        rc = storage_object_remove(STORAGE_OBJECT_PENDING_SMS, id);
        if (rc == STORAGE_RECORD_OK || rc == STORAGE_RECORD_NOT_FOUND) {
            erase_index(s_pending, &s_pending_count, pos);
            s_status.filtered_controls++;
            return STORAGE_RECORD_OK;
        }
        return rc;
    }
    if (existing >= 0) {
        /* Both copies survived a cut after publication. Never delete the
         * staging copy unless the published body is exactly the same. */
        size_t pending_len;
        if (!message_file_encode(&s_file, s_compare, sizeof(s_compare), &pending_len)) return STORAGE_RECORD_ERROR;
        size_t inbox_len;
        rc = storage_object_read(STORAGE_OBJECT_INBOX, id, s_wire, sizeof(s_wire), &inbox_len);
        if (rc != STORAGE_RECORD_OK) return rc;
        if (pending_len != inbox_len || memcmp(s_compare, s_wire, inbox_len) != 0) return STORAGE_RECORD_ERROR;
    } else {
        if (s_count[MESSAGE_INBOX] == MESSAGE_MAILBOX_LIMIT) return STORAGE_RECORD_FULL;
        rc = save(STORAGE_OBJECT_INBOX, id);
        if (rc != STORAGE_RECORD_OK) return rc;
        index_t entry;
        rc = index_current(STORAGE_OBJECT_INBOX, id, &entry);
        if (rc != STORAGE_RECORD_OK) return rc;
        insert(s_index[MESSAGE_INBOX], &s_count[MESSAGE_INBOX], &entry, true);
        s_status.received++; s_status.revision++;
    }
    rc = storage_object_remove(STORAGE_OBJECT_PENDING_SMS, id);
    if (rc == STORAGE_RECORD_OK || rc == STORAGE_RECORD_NOT_FOUND) {
        erase_index(s_pending, &s_pending_count, pos);
        return STORAGE_RECORD_OK;
    }
    return rc;
}

/* One durable mutation per tick. A cut before receipt-time initialization only
 * extends retention; it cannot cause premature expiry. Rewrites preserve the
 * attribute, so extra parts and duplicate deliveries never renew the timer. */
static bool cleanup_incomplete(uint32_t wall, storage_record_result_t *rc) {
    if (wall == 0u) return false;
    for (unsigned i = 0u; i < s_pending_count; i++) {
        index_t *entry = &s_pending[i];
        if (entry->flags & (INDEX_COMPLETE | INDEX_CONTROL)) continue;
        if (entry->time == 0u || wall < entry->time) {
            *rc = storage_object_set_state(STORAGE_OBJECT_PENDING_SMS, entry->id, wall);
            if (*rc == STORAGE_RECORD_OK) entry->time = wall;
            return true;
        }
        if (wall - entry->time < MESSAGE_INCOMPLETE_TTL_SECONDS) continue;
        *rc = storage_object_remove(STORAGE_OBJECT_PENDING_SMS, entry->id);
        if (*rc == STORAGE_RECORD_OK || *rc == STORAGE_RECORD_NOT_FOUND) {
            *rc = STORAGE_RECORD_OK;
            erase_index(s_pending, &s_pending_count, i);
            s_status.expired_incomplete++;
            s_status.full = false;
        }
        return true;
    }
    return false;
}

void message_service_tick(uint32_t now, const rtc_datetime_t *wall_time) {
    uint32_t wall = message_datetime_seconds(wall_time);
    s_status.retention_clock_valid = wall != 0u;
    if (s_retry_pending && (int32_t)(now - s_retry_at) < 0) return;
    s_retry_pending = false;
    storage_record_result_t rc = s_status.ready ? STORAGE_RECORD_OK : reload();
    /* User deletes must still run when an inbox/pending quota is full. */
    for (unsigned i = 0; i < MESSAGE_OP_COUNT; i++) {
        request_t *r = &s_requests[i];
        if (r->result.request_id == 0u || r->done) continue;
        if (rc == STORAGE_RECORD_OK) rc = perform(r);
        if (rc == STORAGE_RECORD_BUSY) { failure(rc, now); return; }
        r->result.outcome = rc == STORAGE_RECORD_OK ? MESSAGE_RESULT_OK :
            rc == STORAGE_RECORD_FULL ? MESSAGE_RESULT_FULL : MESSAGE_RESULT_ERROR;
        r->done = true;
        if (rc != STORAGE_RECORD_OK) failure(rc, now);
        recount();
        return;
    }
    if (rc != STORAGE_RECORD_OK) { failure(rc, now); return; }
    s_status.storage_error = false;
    if (cleanup_incomplete(wall, &rc)) {
        if (rc != STORAGE_RECORD_OK) failure(rc, now);
        recount();
        return;
    }
    bool publication_full = false;
    for (unsigned i = 0; i < s_pending_count; i++) {
        if (!(s_pending[i].flags & (INDEX_COMPLETE | INDEX_CONTROL))) continue;
        if (publication_full && !(s_pending[i].flags & INDEX_CONTROL) &&
            find(s_index[MESSAGE_INBOX], s_count[MESSAGE_INBOX], s_pending[i].id) < 0) continue;
        rc = publish(i);
        if (rc != STORAGE_RECORD_OK && rc != STORAGE_RECORD_FULL) { failure(rc, now); return; }
        if (rc == STORAGE_RECORD_OK) { recount(); return; }
        if (!s_status.full) s_status.full_events++;
        s_status.full = true;
        publication_full = true;
        /* A full inbox must not strand a filtered control or a post-cut
         * duplicate behind the message waiting for publication. */
    }
    if (s_queued != 0u) {
        rc = receive_one();
        if (rc != STORAGE_RECORD_OK) {
            /* A full category must not block another category behind it. */
            if (rc == STORAGE_RECORD_FULL && s_queued > 1u) {
                uint8_t tail = (s_head + s_queued) % RECEIVE_LIMIT;
                if (tail != s_head) {
                    s_receive[tail] = s_receive[s_head];
                    memset(&s_receive[s_head], 0, sizeof(s_receive[s_head]));
                }
                s_head = (s_head + 1u) % RECEIVE_LIMIT;
            }
            failure(rc, now);
            return;
        }
        memset(&s_receive[s_head], 0, sizeof(s_receive[s_head]));
        s_head = (s_head + 1u) % RECEIVE_LIMIT; s_queued--;
    }
    recount();
}

bool message_service_idle(void) {
    if (s_queued != 0u) return false;
    for (unsigned i = 0; i < MESSAGE_OP_COUNT; i++)
        if (s_requests[i].result.request_id != 0u && !s_requests[i].done) return false;
    return true;
}
void message_service_note_receive_loss(void) { s_status.receive_errors++; }
void message_service_get_status(message_status_t *out) { if (out != NULL) *out = s_status; }

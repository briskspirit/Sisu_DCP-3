#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include "services/message_service.h"
#include "services/message_file_codec.h"
#include "services/business_card_service.h"
#include "services/phonebook_service.h"
#include "services/sms_deliver_codec.h"
#include "sms_control_fixtures.h"
#include "storage/storage_lfs.h"
#include "storage/storage_layout.h"
#include "storage/storage_objects.h"
#include "storage/storage_user_space.h"
#include "storage/store_health.h"
#include "storage/store_service.h"

static unsigned corrupt_detections[STORAGE_OBJECT_COLLECTION_COUNT];
static uint8_t boot_faults;
void store_health_note_corrupt(storage_object_collection_t collection, uint32_t id) {
    assert((unsigned)collection < STORAGE_OBJECT_COLLECTION_COUNT && id != 0u);
    corrupt_detections[collection]++;
}
void store_service_require_service(uint8_t faults) { boot_faults |= faults; }
void store_phonebook_prune_bindings(bool (*exists)(uint32_t)) { (void)exists; }

static uint8_t media[STORAGE_USER_BYTES], baseline[STORAGE_USER_BYTES];
static unsigned operations, cut_at, tear;
static bool busy, io_error, write_error;
static jmp_buf cut;
static storage_backend_t backend;
static uint32_t now;
static rtc_datetime_t wall;
static bool wall_valid;
static message_file_t file, decoded;
static message_content_t content;
static uint8_t wire[MESSAGE_FILE_WIRE_MAX];
static char pdu[8][SMS_DELIVER_HEX_MAX];

static nvm_status_t read_media(nvm_hal_t *h, uint32_t off, void *dst, size_t n) {
    assert(off <= h->capacity && n <= h->capacity - off);
    if (io_error) return NVM_STATUS_IO_ERROR;
    memcpy(dst, media + off, n); return NVM_STATUS_OK;
}
static nvm_status_t change(nvm_hal_t *h, uint32_t off, const void *src, size_t n) {
    assert(off <= h->capacity && n <= h->capacity - off);
    if (busy || io_error || write_error) return busy ? NVM_STATUS_BUSY : NVM_STATUS_IO_ERROR;
    bool interrupted = ++operations == cut_at;
    size_t done = interrupted ? (tear == 0 ? 0 : tear == 1 ? n / 2 : n) : n;
    for (size_t i = 0; i < done; i++) {
        if (src == NULL) media[off+i] = 0xff;
        else {
            uint8_t b = ((const uint8_t *)src)[i];
            assert((media[off+i] & b) == b); media[off+i] &= b;
        }
    }
    if (interrupted) longjmp(cut, 1);
    return NVM_STATUS_OK;
}
static nvm_status_t write_media(nvm_hal_t *h, uint32_t off, const void *src, size_t n) {
    assert(off % 256u == 0u && n == 256u); return change(h, off, src, n);
}
static nvm_status_t erase_media(nvm_hal_t *h, uint32_t off, size_t n) {
    assert(off % 4096u == 0u && n == 4096u); return change(h, off, NULL, n);
}
static nvm_hal_t hal = {.capacity=sizeof(media), .erase_block=4096, .write_block=256,
    .erase_required=true, .read=read_media, .write=write_media, .erase=erase_media};
static void reopen(void) {
    storage_lfs_deinit();
    assert(storage_lfs_init(&backend, &hal) == STORAGE_RECORD_OK);
    assert(storage_objects_open() == STORAGE_RECORD_OK);
    phonebook_service_init();
    message_service_init();
}
static void fresh(void) {
    storage_lfs_deinit(); memset(media, 0xff, sizeof(media));
    boot_faults = 0u; memset(corrupt_detections, 0, sizeof(corrupt_detections));
    cut_at = 0; busy = io_error = write_error = false; now = 0; wall_valid = false; reopen();
}
static void tick(void) { now += 1001u; message_service_tick(now, wall_valid ? &wall : NULL); }
static void drain(void) { for (unsigned i = 0; i < 24u; i++) tick(); }
static message_status_t status(void) { message_status_t s; message_service_get_status(&s); return s; }
static void make_pdu(char *out, unsigned total, unsigned seq, unsigned ref, unsigned second,
                     const char *text) {
    sms_deliver_t d = {0};
    strcpy(d.address, "+18135550123");
    assert(sms_deliver_scts_encode(2026, 9, 19, 12, 0, (uint8_t)second, 0, d.scts));
    d.dcs = 8u;
    if (total != 1u) {
        d.udhi = true;
        uint8_t header[] = {5,0,3,(uint8_t)ref,(uint8_t)total,(uint8_t)seq};
        memcpy(d.ud, header, sizeof(header)); d.ud_len = sizeof(header);
    }
    for (size_t i = 0; text[i]; i++) {
        assert(d.ud_len <= 138u);
        d.ud[d.ud_len++] = 0; d.ud[d.ud_len++] = (uint8_t)text[i];
    }
    d.udl = d.ud_len;
    uint8_t len;
    assert(sms_deliver_build(&d, out, SMS_DELIVER_HEX_MAX, &len));
}
static message_result_t result(uint32_t token, bool read) {
    drain(); message_result_t r;
    assert(message_service_pop_result(token, &r, read ? &content : NULL));
    assert(!message_service_pop_result(token, &r, NULL)); return r;
}

static void make_control_pdu(char *out, uint8_t seq, const char *text) {
    sms_deliver_t d = {0};
    strcpy(d.address, "+18135550123");
    assert(sms_deliver_scts_encode(2026, 9, 19, 12, 0, seq, 0, d.scts));
    d.dcs = 4u; d.udhi = true;
    const uint8_t header[] = {11, 0, 3, 17, 2, seq, 5, 4, 0x15, 0x7c, 0, 0};
    memcpy(d.ud, header, sizeof(header));
    size_t n = strlen(text);
    assert(n <= sizeof(d.ud) - sizeof(header));
    memcpy(d.ud + sizeof(header), text, n);
    d.udl = d.ud_len = (uint8_t)(sizeof(header) + n);
    uint8_t len;
    assert(sms_deliver_build(&d, out, SMS_DELIVER_HEX_MAX, &len));
}
static uint32_t first_id(message_mailbox_t box) {
    static message_metadata_t rows[MESSAGE_MAILBOX_LIMIT];
    uint32_t token;
    assert(message_service_request_list(box, rows, MESSAGE_MAILBOX_LIMIT, &token));
    message_result_t r = result(token, false);
    assert(r.outcome == MESSAGE_RESULT_OK && r.count > 0u);
    return rows[0].id;
}

static void test_codec(void) {
    make_pdu(pdu[0], 2, 1, 17, 1, "First ");
    make_pdu(pdu[1], 2, 2, 17, 2, "second");
    assert(message_file_receive(&file, pdu[1]));
    assert(!message_file_complete(&file));
    assert(message_file_merge(&file, pdu[0]) == MESSAGE_MERGE_ADDED);
    assert(message_file_content(&file, &content));
    assert(strcmp(content.text, "First second") == 0);
    assert(message_file_merge(&file, pdu[0]) == MESSAGE_MERGE_DUPLICATE);
    size_t len;
    assert(message_file_encode(&file, wire, sizeof(wire), &len));
    assert(len < 492u && message_file_decode(&decoded, wire, len));
    assert(message_file_content(&decoded, &content));
    assert(strcmp(content.text, "First second") == 0);
    for (size_t n = 0; n < len; n++) assert(!message_file_decode(&decoded, wire, n));
    make_pdu(pdu[2], 2, 1, 17, 1, "Conflict");
    assert(message_file_merge(&file, pdu[2]) == MESSAGE_MERGE_ADDED);
    assert(file.flags & MESSAGE_FILE_QUARANTINED);
    assert(message_file_content(&file, &content) && content.metadata.binary && content.text[0] == 0);
    file.flags = 0;
    assert(!message_file_encode(&file, wire, sizeof(wire), &len));
    assert(message_file_receive(&file, pdu[0]));
    make_pdu(pdu[2], 2, 2, 18, 2, "Unrelated");
    assert(message_file_receive(&decoded, pdu[2]));
    file.parts[1] = decoded.parts[0]; file.count = 2;
    assert(!message_file_complete(&file));
    assert(!message_file_encode(&file, wire, sizeof(wire), &len));
    assert(message_file_draft(&file, "", "Saved text"));
    assert(message_file_encode(&file, wire, sizeof(wire), &len));
    assert(message_file_decode(&decoded, wire, len));
    assert(message_file_content(&decoded, &content) && strcmp(content.text, "Saved text") == 0);
    memset(file.draft.text, 'x', sizeof(file.draft.text));
    assert(!message_file_encode(&file, wire, sizeof(wire), &len));
    assert(message_timestamp_seconds("26/02/30,00:00:00") == 0u);
    assert(message_timestamp_seconds("24/03/01,00:00:00") - message_timestamp_seconds("24/02/28,00:00:00") == 172800u);
}

static void test_mailboxes(void) {
    fresh();
    uint32_t token, another;
    assert(message_service_request_save("123", "A saved draft", &token));
    assert(!message_service_request_save("456", "Another", &another));
    assert(result(token, false).outcome == MESSAGE_RESULT_OK);
    uint32_t draft = first_id(MESSAGE_OUTBOX);
    assert(status().outbox == 1u);
    reopen();
    assert(first_id(MESSAGE_OUTBOX) == draft);
    assert(message_service_request_read(MESSAGE_OUTBOX, draft, &token));
    assert(result(token, true).outcome == MESSAGE_RESULT_OK);
    assert(strcmp(content.text, "A saved draft") == 0 && content.metadata.id == draft);
    assert(message_service_request_delete(MESSAGE_OUTBOX, draft, &token));
    assert(result(token, false).outcome == MESSAGE_RESULT_OK && status().outbox == 0u);
    make_pdu(pdu[0], 1, 1, 0, 1, "Received");
    assert(message_service_receive(pdu[0])); drain();
    assert(status().inbox == 1u && status().unread == 1u && status().received == 1u && status().pending == 0u);
    uint32_t inbox = first_id(MESSAGE_INBOX);
    assert(inbox > draft);
    assert(message_service_request_read(MESSAGE_INBOX, inbox, &token));
    assert(result(token, true).outcome == MESSAGE_RESULT_OK && status().unread == 0u);
    assert(strcmp(content.text, "Received") == 0);
    reopen();
    assert(status().unread == 0u && status().received == 0u);
    assert(message_service_receive(pdu[0])); drain();
    assert(status().inbox == 1u && status().received == 0u);
    static message_metadata_t rows[MESSAGE_MAILBOX_LIMIT];
    assert(message_service_request_list(MESSAGE_INBOX, rows, MESSAGE_MAILBOX_LIMIT, &token));
    assert(result(token, false).outcome == MESSAGE_RESULT_OK);
    assert(message_service_request_delete(MESSAGE_INBOX, inbox, &token));
    assert(result(token, false).outcome == MESSAGE_RESULT_OK);
    reopen(); assert(status().inbox == 0u && status().pending == 0u);
}

static void test_multipart(void) {
    fresh();
    make_control_pdu(pdu[0], 1u, "//VVM:SYNC:");
    make_control_pdu(pdu[1], 2u, "ev=NM;id=123;");
    assert(message_service_receive(pdu[0])); drain();
    assert(status().pending == 1u && status().filtered_controls == 0u);
    reopen();
    assert(message_service_receive(pdu[1])); drain();
    assert(status().pending == 0u && status().inbox == 0u &&
           status().received == 0u && status().filtered_controls == 1u);
    make_control_pdu(pdu[1], 2u, "not a control");
    assert(message_service_receive(pdu[0]));
    assert(message_service_receive(pdu[1])); drain();
    assert(status().inbox == 1u && status().filtered_controls == 1u);

    fresh();
    char text[68]; memset(text, 'A', 67); text[67] = 0;
    for (unsigned i = 0; i < 8; i++) {
        memset(text, 'A' + (int)i, 67);
        make_pdu(pdu[i], 8, i+1u, 12, i, text);
    }
    assert(message_service_receive(pdu[7])); drain();
    assert(status().inbox == 0u && status().pending == 1u);
    reopen();
    for (unsigned i = 0; i < 7; i++) assert(message_service_receive(pdu[i]));
    drain();
    assert(status().inbox == 1u && status().pending == 0u && status().received == 1u);
    uint32_t token;
    assert(message_service_request_read(MESSAGE_INBOX, first_id(MESSAGE_INBOX), &token));
    assert(result(token, true).outcome == MESSAGE_RESULT_OK && strlen(content.text) == 536u);
    for (unsigned i = 0; i < 536; i++) assert(content.text[i] == 'A' + (int)(i / 67u));
    for (unsigned i = 0; i < 8; i++) assert(message_service_receive(pdu[i]));
    drain(); assert(status().inbox == 1u && status().received == 1u);
    /* Same reference after a completed message belongs to a fresh assembly. */
    make_pdu(pdu[0], 2, 1, 12, 20, "New ");
    make_pdu(pdu[1], 2, 2, 12, 21, "message");
    assert(message_service_receive(pdu[0])); assert(message_service_receive(pdu[1])); drain();
    assert(status().inbox == 2u && status().received == 2u);
}

static void test_power_cuts(void) {
    fresh();
    make_pdu(pdu[0], 2, 1, 41, 1, "One ");
    make_pdu(pdu[1], 2, 2, 41, 2, "two");
    assert(message_service_receive(pdu[0])); drain();
    memcpy(baseline, media, sizeof(media));
    operations = 0u;
    assert(message_service_receive(pdu[1])); drain();
    unsigned count = operations;
    for (unsigned point = 1; point <= count; point++) {
        for (tear = 0; tear < 3; tear++) {
            storage_lfs_deinit(); memcpy(media, baseline, sizeof(media)); reopen();
            operations = 0; cut_at = point;
            if (setjmp(cut) == 0) {
                assert(message_service_receive(pdu[1])); drain(); assert(false);
            }
            cut_at = 0; reopen(); drain();
            assert(status().ready && !status().storage_error);
            assert(message_service_receive(pdu[0]));
            assert(message_service_receive(pdu[1])); drain();
            assert(status().inbox == 1u && status().pending == 0u && status().queued == 0u);
            uint32_t token;
            assert(message_service_request_read(MESSAGE_INBOX, first_id(MESSAGE_INBOX), &token));
            assert(result(token, true).outcome == MESSAGE_RESULT_OK);
            assert(strcmp(content.text, "One two") == 0);
        }
    }
    printf("multipart stage/publish/cleanup: %u torn-write cases passed\n", count * 3u);
}

static void test_busy_and_full(void) {
    fresh();
    make_pdu(pdu[0], 1, 1, 0, 1, "Busy");
    busy = true;
    assert(message_service_receive(pdu[0])); tick();
    assert(!message_service_idle() && !message_service_sleep_ready() &&
           status().queued == 1u && status().inbox == 0u);
    drain();
    assert(!message_service_sleep_ready()); /* BUSY is not a terminal failure. */
    busy = false; drain();
    assert(message_service_idle() && status().inbox == 1u);
    for (unsigned i = 0; i < 8; i++) assert(message_service_receive(pdu[0]));
    assert(!message_service_receive(pdu[0]) && status().receive_errors == 1u);
    drain(); assert(status().inbox == 1u);
    /* Fill the inbox with valid large messages, without borrowing contacts. */
    fresh();
    char text[68]; memset(text, 'X', 67); text[67] = 0;
    for (unsigned n = 0; n < 45; n++) {
        for (unsigned i = 0; i < 4; i++) {
            make_pdu(pdu[i], 4, i+1u, n, i, text);
            assert(message_service_receive(pdu[i])); drain();
        }
        if (status().full) break;
    }
    assert(status().full && status().pending > 0u && status().inbox > 0u);
    unsigned held = status().pending, inbox_count = status().inbox;
    wall = (rtc_datetime_t){2026, 9, 19, 12, 0, 0}; wall_valid = true;
    drain(); wall.day = 30; drain();
    assert(status().pending == held && status().inbox == inbox_count &&
           status().expired_incomplete == 0u); /* complete, waiting for quota */
    make_control_pdu(pdu[0], 1u, "//VVM:SYNC:");
    make_control_pdu(pdu[1], 2u, "ev=NM;id=123;");
    assert(message_service_receive(pdu[0])); assert(message_service_receive(pdu[1])); drain();
    assert(status().pending == held && status().filtered_controls == 1u);
    uint32_t id;
    assert(storage_object_allocate(&id) == STORAGE_RECORD_OK);
    assert(storage_object_write(STORAGE_OBJECT_CONTACT, id, (const uint8_t *)"contact", 7) == STORAGE_RECORD_OK);
    uint32_t token;
    assert(message_service_request_read(MESSAGE_INBOX, first_id(MESSAGE_INBOX), &token));
    assert(result(token, true).outcome == MESSAGE_RESULT_OK);
    assert(message_service_request_delete(MESSAGE_INBOX, first_id(MESSAGE_INBOX), &token));
    assert(result(token, false).outcome == MESSAGE_RESULT_OK);
    drain();
    assert(status().pending == 0u);
}

static void test_sent_copy_and_category_isolation(void) {
    fresh();
    assert(message_service_sent("123", "Network accepted"));
    drain();
    uint32_t sent = first_id(MESSAGE_OUTBOX), token;
    reopen();
    assert(message_service_request_read(MESSAGE_OUTBOX, sent, &token));
    assert(result(token, true).outcome == MESSAGE_RESULT_OK && content.metadata.sent &&
           strcmp(content.text, "Network accepted") == 0);
    assert(message_file_draft(&file, "123", "Category fill"));
    size_t len;
    assert(message_file_encode(&file, wire, sizeof(wire), &len));
    unsigned added = 0;
    for (;;) {
        uint32_t id;
        assert(storage_object_allocate(&id) == STORAGE_RECORD_OK);
        storage_record_result_t rc = storage_object_write(STORAGE_OBJECT_OUTBOX, id, wire, len);
        if (rc == STORAGE_RECORD_FULL) break;
        assert(rc == STORAGE_RECORD_OK);
        assert(++added < MESSAGE_MAILBOX_LIMIT);
    }
    reopen();
    unsigned count = status().outbox;
    assert(message_service_sent("123", "Waiting for outbox room"));
    assert(!message_service_sleep_ready());
    tick();
    assert(message_service_sleep_ready() && !message_service_idle());
    make_pdu(pdu[0], 1, 1, 0, 40, "Inbox must remain available");
    assert(message_service_receive(pdu[0]));
    assert(!message_service_sleep_ready());
    drain();
    assert(status().inbox == 1u && status().outbox == count && status().queued == 1u);
    assert(message_service_sleep_ready());
    unsigned before = operations;
    for (unsigned i = 0u; i < 10u; i++) tick();
    assert(operations == before && status().queued == 1u && message_service_sleep_ready());
    /* Reclaim a whole quota block, then the exact held sent copy can commit. */
    for (unsigned n = 0; n < 16u && status().queued; n++) {
        assert(message_service_request_delete(MESSAGE_OUTBOX, first_id(MESSAGE_OUTBOX), &token));
        assert(result(token, false).outcome == MESSAGE_RESULT_OK);
        drain();
    }
    assert(status().queued == 0u && status().inbox == 1u);
    printf("outbox budget held %u compact records; full outbox did not block inbox\n", count);
}

static void test_failed_queue_sleep(void) {
    for (unsigned reads_fail = 0u; reads_fail < 2u; reads_fail++) {
        fresh();
        now = UINT32_MAX - 4000u;
        make_pdu(pdu[0], 1, 1, 0, 41, "Retain through standby");
        assert(message_service_receive(pdu[0]));
        io_error = reads_fail != 0u;
        write_error = !io_error;
        tick();
        assert(!message_service_sleep_ready());
        drain();
        assert(message_service_sleep_ready() && !message_service_idle() && status().queued == 1u);
        io_error = write_error = false;
        if (reads_fail) {
            make_pdu(pdu[1], 1, 1, 0, 42, "Fresh work interrupts reload backoff");
            assert(message_service_receive(pdu[1]));
            assert(!message_service_sleep_ready());
        } else {
            now += 60000u; /* Maintenance retries retained work, including across wrap. */
        }
        drain();
        assert(message_service_idle() && message_service_sleep_ready() && status().inbox == 1u + reads_fail);
    }
    fresh();
    uint32_t token;
    assert(message_service_request_save("123", "A user request must complete", &token));
    assert(!message_service_sleep_ready());
    write_error = true;
    tick();
    assert(message_service_sleep_ready());
    message_result_t r;
    assert(message_service_pop_result(token, &r, NULL) && r.outcome == MESSAGE_RESULT_ERROR);

    for (unsigned cleanup = 0u; cleanup < 2u; cleanup++) {
        fresh();
        make_pdu(pdu[0], cleanup ? 2u : 1u, 1, 93, 43, "Already staged");
        assert(message_service_receive(pdu[0])); tick();
        assert(status().pending == 1u && status().queued == 0u);
        if (cleanup) {
            wall = (rtc_datetime_t){2026, 9, 19, 12, 0, 0}; wall_valid = true;
        }
        write_error = true;
        make_pdu(pdu[1], 1, 1, 0, 44, "Queued behind failed maintenance");
        assert(message_service_receive(pdu[1])); tick();
        assert(!message_service_sleep_ready());
        drain();
        assert(message_service_sleep_ready() && status().queued == 1u && status().storage_error);
        write_error = false; now += 60000u; drain();
        assert(message_service_idle() && status().inbox == 2u - cleanup && status().pending == cleanup);
    }
}

static void test_io_retry_and_corrupt_record(void) {
    fresh();
    make_pdu(pdu[0], 1, 1, 0, 45, "Retained across an I/O error");
    assert(message_service_receive(pdu[0]));
    io_error = true; tick();
    assert(status().queued == 1u && status().storage_error && !status().ready);
    io_error = false; drain();
    assert(status().inbox == 1u && status().queued == 0u && !status().storage_error);
    uint32_t id = first_id(MESSAGE_INBOX);
    make_pdu(pdu[1], 1, 1, 0, 46, "Healthy neighbour");
    assert(message_service_receive(pdu[1])); drain();
    assert(storage_object_write(STORAGE_OBJECT_INBOX, id, (const uint8_t *)"bad", 3u) == STORAGE_RECORD_OK);
    reopen();
    assert(status().ready && !status().storage_error && status().inbox == 1u);
    assert(first_id(MESSAGE_INBOX) != id);
    assert(corrupt_detections[STORAGE_OBJECT_INBOX] != 0u && boot_faults == 0u);
    size_t len = 0u;
    assert(storage_object_read(STORAGE_OBJECT_INBOX, id, wire, sizeof(wire), &len) == STORAGE_RECORD_OK &&
           len == 3u && memcmp(wire, "bad", 3u) == 0);
}

static void test_publication_conflict_isolation(void) {
    uint8_t destination[MESSAGE_FILE_WIRE_MAX + 1u], pending[MESSAGE_FILE_WIRE_MAX];
    uint8_t actual[sizeof(destination)];
    for (unsigned kind = 0u; kind < 4u; kind++) {
        fresh();
        make_pdu(pdu[0], 1, 1, 0, 47, "Preserve the staging evidence");
        assert(message_file_receive(&file, pdu[0]));
        size_t pending_len, destination_len;
        assert(message_file_encode(&file, pending, sizeof(pending), &pending_len));
        uint32_t id;
        assert(storage_object_allocate(&id) == STORAGE_RECORD_OK);
        assert(storage_object_write(STORAGE_OBJECT_PENDING_SMS, id, pending, pending_len) == STORAGE_RECORD_OK);
        if (kind == 0u) {
            memcpy(destination, "bad", 3u); destination_len = 3u;
        } else if (kind == 1u) {
            memcpy(destination, pending, pending_len); destination_len = pending_len;
        } else if (kind == 2u) {
            make_pdu(pdu[1], 1, 1, 0, 48, "A different valid inbox body");
            assert(message_file_receive(&file, pdu[1]));
            assert(message_file_encode(&file, destination, sizeof(destination), &destination_len));
        } else {
            /* Valid object envelope, outside the SMS decoder's size bound. */
            memset(destination, 'X', sizeof(destination)); destination_len = sizeof(destination);
        }
        assert(storage_object_write(STORAGE_OBJECT_INBOX, id, destination, destination_len) == STORAGE_RECORD_OK);
        uint32_t expected_state = kind == 1u ? 2u : 0u;
        assert(storage_object_set_state(STORAGE_OBJECT_INBOX, id, expected_state) == STORAGE_RECORD_OK);
        reopen();
        for (unsigned boot = 0u; boot < 2u; boot++) {
            make_pdu(pdu[2], 1, 1, 0, 51u + boot, "Unrelated arrival must be saved");
            assert(message_service_receive(pdu[2])); drain();
            assert(status().ready && !status().storage_error && status().pending == 1u &&
                   status().queued == 0u && status().inbox == boot + 1u + (kind == 2u));
            assert(message_service_idle() && message_service_sleep_ready());
            storage_object_collection_t damaged = kind == 2u ? STORAGE_OBJECT_PENDING_SMS : STORAGE_OBJECT_INBOX;
            assert(corrupt_detections[damaged] != 0u && boot_faults == 0u);
            size_t len;
            uint32_t state;
            assert(storage_object_read(STORAGE_OBJECT_INBOX, id, actual, sizeof(actual), &len) == STORAGE_RECORD_OK &&
                   len == destination_len && memcmp(actual, destination, len) == 0);
            assert(storage_object_get_state(STORAGE_OBJECT_INBOX, id, &state) == STORAGE_RECORD_OK && state == expected_state);
            assert(storage_object_read(STORAGE_OBJECT_PENDING_SMS, id, actual, sizeof(actual), &len) == STORAGE_RECORD_OK &&
                   len == pending_len && memcmp(actual, pending, len) == 0);
            unsigned before = operations;
            drain();
            assert(operations == before); /* No repeated destructive repair attempts. */
            reopen();
        }
    }
}

static void test_delete_power_cuts(void) {
    fresh();
    make_pdu(pdu[0], 1, 1, 0, 50, "Atomic delete");
    assert(message_service_receive(pdu[0])); drain();
    uint32_t id = first_id(MESSAGE_INBOX), token;
    memcpy(baseline, media, sizeof(media));
    operations = 0;
    assert(message_service_request_delete(MESSAGE_INBOX, id, &token));
    assert(result(token, false).outcome == MESSAGE_RESULT_OK);
    unsigned count = operations;
    for (unsigned point = 1; point <= count; point++) {
        for (tear = 0; tear < 3; tear++) {
            storage_lfs_deinit(); memcpy(media, baseline, sizeof(media)); reopen();
            operations = 0; cut_at = point;
            if (setjmp(cut) == 0) {
                assert(message_service_request_delete(MESSAGE_INBOX, id, &token));
                drain(); assert(false);
            }
            cut_at = 0; reopen(); drain();
            assert(status().ready && !status().storage_error && status().inbox <= 1u && status().pending == 0u);
            if (status().inbox) {
                assert(first_id(MESSAGE_INBOX) == id);
                assert(message_service_request_delete(MESSAGE_INBOX, id, &token));
                assert(result(token, false).outcome == MESSAGE_RESULT_OK);
            }
            reopen(); drain(); assert(status().inbox == 0u && status().pending == 0u);
        }
    }
}

static void test_expiry(void) {
    fresh();
    make_pdu(pdu[0], 3, 1, 91, 1, "Incomplete ");
    make_pdu(pdu[1], 3, 2, 91, 2, "group");
    make_pdu(pdu[2], 1, 1, 0, 3, "Complete");
    assert(message_service_receive(pdu[0]));
    assert(message_service_receive(pdu[2])); drain();
    assert(status().pending == 1u && status().inbox == 1u && !status().retention_clock_valid);
    wall = (rtc_datetime_t){2026, 9, 19, 12, 0, 0}; wall_valid = true;
    drain();
    wall.day = 25;
    assert(message_service_receive(pdu[0])); /* duplicate cannot renew */
    assert(message_service_receive(pdu[1])); /* nor can a new part */
    drain(); reopen(); drain();
    assert(status().pending == 1u && status().expired_incomplete == 0u);
    wall.day = 26; wall_valid = false;
    now = UINT32_MAX - 4000u; /* expiry and retry must survive uptime wrap */
    drain(); assert(status().pending == 1u);
    wall_valid = true;
    busy = true; tick();
    assert(status().pending == 1u && status().expired_incomplete == 0u);
    busy = false; drain();
    assert(status().pending == 0u && status().inbox == 1u && status().expired_incomplete == 1u);
    reopen(); drain(); assert(status().pending == 0u && status().inbox == 1u);

    /* A backward RTC correction cannot wrap subtraction into immediate expiry. */
    assert(message_service_receive(pdu[0])); drain();
    wall.day = 18; drain(); reopen();
    wall.day = 24; drain(); assert(status().pending == 1u);
    wall.day = 25; drain(); assert(status().pending == 0u);
    wall.year = 2100; drain(); assert(!status().retention_clock_valid);
}

static void test_expiry_power_cuts(void) {
    fresh();
    wall = (rtc_datetime_t){2026, 9, 19, 12, 0, 0}; wall_valid = true;
    make_pdu(pdu[0], 2, 1, 97, 1, "Expire atomically");
    assert(message_service_receive(pdu[0])); drain();
    memcpy(baseline, media, sizeof(media));
    wall.day = 26; operations = 0; drain();
    unsigned count = operations;
    assert(count > 0u && status().pending == 0u);
    for (unsigned point = 1u; point <= count; point++) {
        for (tear = 0u; tear < 3u; tear++) {
            storage_lfs_deinit(); memcpy(media, baseline, sizeof(media)); reopen();
            operations = 0; cut_at = point;
            if (setjmp(cut) == 0) { drain(); assert(false); }
            cut_at = 0; reopen(); drain();
            assert(status().ready && !status().storage_error && status().pending == 0u);
        }
    }
}

static void make_dm(char *out, unsigned seq, bool bad) {
    static const char hex[] = CONTROL_DM_WSP_HEX;
    sms_deliver_t d = {0};
    strcpy(d.address, "15551230000");
    assert(sms_deliver_scts_encode(2026, 9, 19, 12, 0, (uint8_t)seq, 0, d.scts));
    d.udhi = true; d.dcs = 0xf5u;
    const uint8_t udh[] = {11, 0, 3, 44, 2, (uint8_t)seq, 5, 4, 0x0b, 0x84, 0xc0, 2};
    memcpy(d.ud, udh, sizeof(udh)); d.ud_len = sizeof(udh);
    size_t begin = seq == 1u ? 0u : 20u, end = seq == 1u ? 20u : strlen(hex) / 2u;
    for (size_t i = begin; i < end; i++) {
        unsigned byte;
        assert(sscanf(hex + 2u * i, "%2x", &byte) == 1);
        d.ud[d.ud_len++] = (uint8_t)byte;
    }
    if (bad && seq == 2u) d.ud[d.ud_len++] = 0u;
    d.udl = d.ud_len;
    uint8_t length;
    assert(sms_deliver_build(&d, out, SMS_DELIVER_HEX_MAX, &length));
}

static void test_control_admission(void) {
    fresh();
    sms_deliver_t d = {0};
    strcpy(d.address, "15551230000"); d.pid = 0x40u; d.dcs = 4u;
    assert(sms_deliver_scts_encode(2026, 9, 19, 12, 0, 0, 0, d.scts));
    uint8_t length;
    assert(sms_deliver_build(&d, pdu[0], sizeof(pdu[0]), &length));
    unsigned before = operations;
    assert(message_service_receive(pdu[0])); drain();
    assert(status().filtered_controls == 1u && status().queued == 0u &&
           status().pending == 0u && status().inbox == 0u && operations == before);
    make_pdu(pdu[1], 1, 1, 0, 1, "Queue fill");
    for (unsigned i = 0u; i < 8u; i++) assert(message_service_receive(pdu[1]));
    assert(message_service_receive(pdu[0])); /* controls need no RAM slot either */
    assert(status().filtered_controls == 2u && status().receive_errors == 0u);
    fresh();
    make_dm(pdu[0], 1u, false); make_dm(pdu[1], 2u, false);
    assert(message_service_receive(pdu[1])); drain(); reopen();
    assert(message_service_receive(pdu[0])); drain();
    assert(status().inbox == 0u && status().pending == 0u && status().filtered_controls == 1u);
    storage_user_usage_t usage;
    assert(storage_user_get_usage(&usage) == STORAGE_RECORD_OK);
    assert(usage.pools[STORAGE_USER_PENDING].contents.files == 0u);
    make_dm(pdu[1], 2u, true);
    assert(message_service_receive(pdu[0])); assert(message_service_receive(pdu[1])); drain();
    assert(status().inbox == 1u && status().pending == 0u && status().filtered_controls == 1u);
}

static void test_durable_receive_receipts(void) {
    fresh();
    make_pdu(pdu[0], 2u, 1u, 41u, 1u, "First ");
    make_pdu(pdu[1], 2u, 2u, 41u, 2u, "second");
    uint32_t receipt, next;
    assert(message_service_receive_tracked(pdu[0], &receipt));
    assert(!message_service_receive_committed(receipt));
    assert(!message_service_receive_tracked(pdu[1], &next));
    busy = true; tick();
    assert(!message_service_receive_committed(receipt));
    busy = false; write_error = true; drain();
    assert(!message_service_receive_committed(receipt));
    write_error = false; now += 60001u; drain();
    assert(message_service_receive_committed(receipt) && status().pending == 1u);
    /* The modem still has its copy after a cut. Reimport is idempotent. */
    reopen();
    assert(!message_service_receive_committed(receipt));
    assert(message_service_receive_tracked(pdu[0], &next));
    drain();
    assert(message_service_receive_committed(next) && status().pending == 1u);
    message_service_receive_forget(next);
    assert(message_service_receive_tracked(pdu[1], &receipt));
    message_service_receive_forget(receipt);
    assert(message_service_receive_tracked(pdu[0], &next) && next != receipt);
    tick();
    assert(!message_service_receive_committed(next));
    drain();
    assert(message_service_receive_committed(next) && status().inbox == 1u);
    message_service_receive_forget(next);
}

static const char card_text[] = "//SCKE2 BEGIN:VCARD\r\nN:Alice\r\nTEL:+123456789\r\nEND:VCARD\r\n";

static uint32_t receive_card(unsigned second) {
    make_pdu(pdu[0], 1, 1, 0, second, card_text);
    assert(message_service_receive(pdu[0])); drain();
    assert(status().inbox == 0u && status().pending == 0u);
    uint32_t id = business_card_service_pending_first();
    assert(id != 0u); return id;
}

static business_card_action_result_t card_action(uint32_t id, bool save) {
    assert(business_card_service_request(id, save));
    drain();
    business_card_action_result_t r;
    assert(business_card_service_pop_result(&r) && r.id == id && r.save == save);
    assert(!business_card_service_pop_result(&r)); return r;
}

static void test_business_cards(void) {
    fresh();
    uint32_t id = receive_card(1);
    phonebook_entry_t contact;
    assert(business_card_service_get(id, &contact) && strcmp(contact.name, "Alice") == 0);
    assert(phonebook_service_count() == 0u);
    assert(message_service_receive(pdu[0])); drain();
    assert(business_card_service_pending_first() == id && status().pending == 0u);
    reopen(); assert(business_card_service_pending_first() == id);
    assert(business_card_service_request(id, true));
    busy = true; tick();
    business_card_action_result_t r;
    assert(!business_card_service_pop_result(&r) && !message_service_sleep_ready());
    busy = false; drain();
    assert(business_card_service_pop_result(&r) && r.outcome == PHONEBOOK_OUTCOME_OK);
    assert(phonebook_service_count() == 1u && business_card_service_pending_first() == 0u);
    assert(phonebook_service_entry(0, &contact) && contact.index == id);
    reopen();
    assert(message_service_receive(pdu[0])); drain();
    assert(business_card_service_pending_first() == 0u && phonebook_service_count() == 1u);
    assert(card_action(id, true).outcome == PHONEBOOK_OUTCOME_OK && phonebook_service_count() == 1u);

    id = receive_card(2);
    assert(card_action(id, false).outcome == PHONEBOOK_OUTCOME_OK);
    assert(card_action(id, true).outcome == PHONEBOOK_OUTCOME_ERROR);
    reopen(); assert(message_service_receive(pdu[0])); drain();
    assert(business_card_service_pending_first() == 0u && phonebook_service_count() == 1u);

    make_pdu(pdu[0], 2, 1, 32, 3, "//SCK");
    make_pdu(pdu[1], 2, 2, 32, 4, card_text + 5);
    assert(message_service_receive(pdu[1])); drain(); reopen();
    assert(status().pending == 1u && business_card_service_pending_first() == 0u);
    assert(message_service_receive(pdu[0])); drain();
    id = business_card_service_pending_first();
    assert(id != 0u && status().pending == 0u && status().inbox == 0u);
    write_error = true;
    assert(card_action(id, true).outcome == PHONEBOOK_OUTCOME_ERROR);
    write_error = false; reopen();
    assert(business_card_service_pending_first() == id);
    assert(card_action(id, true).outcome == PHONEBOOK_OUTCOME_OK && phonebook_service_count() == 2u);

    make_pdu(pdu[0], 1, 1, 0, 5, "//SCKE2 BEGIN:VCARD\r\nTEL:123\r\n");
    assert(message_service_receive(pdu[0])); drain();
    assert(status().business_card_errors == 1u && status().inbox == 0u && status().pending == 0u);
    assert(message_service_sent("123", card_text)); drain();
    assert(status().outbox == 0u);
}

static void test_card_runtime_recovery(void) {
    for (unsigned save = 0u; save < 2u; save++) {
        fresh();
        uint32_t id = receive_card(1);
        if (save) {
            phonebook_entry_t entry;
            assert(business_card_service_get(id, &entry));
            assert(phonebook_service_import(id, &entry) == PHONEBOOK_OUTCOME_OK);
        }
        assert(business_card_service_request(id, save != 0u));
        write_error = true; tick();
        business_card_action_result_t result;
        assert(business_card_service_pop_result(&result) && result.outcome == PHONEBOOK_OUTCOME_ERROR);
        assert(!business_card_service_ready());
        write_error = false; io_error = true;
        drain();
        assert(!business_card_service_ready() && message_service_sleep_ready());
        io_error = false;
        drain(); /* No new SMS, user action, or reboot may be needed. */
        assert(business_card_service_ready() && business_card_service_pending_first() == id);
        assert(card_action(id, save != 0u).outcome == PHONEBOOK_OUTCOME_OK);
        assert(phonebook_service_count() == save);
    }
    /* A full pre-card OTHER budget must not latch Contact service at boot. */
    fresh();
    static uint8_t padding[4000];
    unsigned records = 0;
    storage_record_result_t rc;
    while ((rc = backend.write(&backend, (uint16_t)(0x7700u + records), padding, sizeof(padding))) == STORAGE_RECORD_OK)
        records++;
    assert(rc == STORAGE_RECORD_FULL);
    reopen();
    assert(boot_faults == 0u && business_card_service_ready() && status().ready);
}

static void test_card_save_power_cuts(void) {
    fresh(); uint32_t id = receive_card(1);
    memcpy(baseline, media, sizeof(media));
    operations = 0;
    assert(card_action(id, true).outcome == PHONEBOOK_OUTCOME_OK);
    unsigned count = operations;
    assert(count > 0);
    for (unsigned point = 1; point <= count; point++) {
        for (tear = 0; tear < 3; tear++) {
            storage_lfs_deinit(); memcpy(media, baseline, sizeof(media)); reopen();
            operations = 0; cut_at = point;
            if (setjmp(cut) == 0) { (void)card_action(id, true); assert(false); }
            cut_at = 0; reopen(); drain();
            assert(phonebook_service_cache_valid() && phonebook_service_count() <= 1u);
            assert(card_action(id, true).outcome == PHONEBOOK_OUTCOME_OK);
            assert(phonebook_service_count() == 1u && business_card_service_pending_first() == 0u);
            phonebook_entry_t saved;
            assert(phonebook_service_entry(0, &saved) && saved.index == id);
            assert(strcmp(saved.name, "Alice") == 0 && strcmp(saved.number, "+123456789") == 0);
        }
    }
    printf("business-card save: %u torn-write cases passed\n", count * 3u);
}

static void test_card_publication_power_cuts(void) {
    fresh();
    make_pdu(pdu[0], 1, 1, 0, 1, card_text);
    assert(message_service_receive(pdu[0])); tick(); /* durable pending receipt */
    assert(status().pending == 1u);
    memcpy(baseline, media, sizeof(media));
    operations = 0; drain();
    unsigned count = operations;
    uint32_t id = business_card_service_pending_first();
    assert(count > 0u && id != 0u);
    for (unsigned point = 1; point <= count; point++) {
        for (tear = 0; tear < 3; tear++) {
            storage_lfs_deinit(); memcpy(media, baseline, sizeof(media)); reopen();
            operations = 0; cut_at = point;
            if (setjmp(cut) == 0) { drain(); assert(false); }
            cut_at = 0; reopen(); drain();
            assert(status().ready && !status().storage_error && status().inbox == 0u && status().pending == 0u);
            assert(business_card_service_pending_first() == id);
            assert(message_service_receive(pdu[0])); drain();
            assert(business_card_service_pending_first() == id && status().pending == 0u);
        }
    }
    printf("business-card publication: %u torn-write cases passed\n", count * 3u);
}

static void test_card_limits_and_corruption(void) {
    fresh();
    uint32_t first = receive_card(1);
    for (unsigned i = 2; i <= BUSINESS_CARD_LIMIT; i++) receive_card(i);
    make_pdu(pdu[0], 1, 1, 0, 20, card_text);
    assert(message_service_receive(pdu[0])); drain();
    assert(status().pending == 1u && status().full && business_card_service_pending_first() == first);
    /* Card capacity cannot block ordinary SMS publication. */
    make_pdu(pdu[1], 1, 1, 0, 21, "Ordinary SMS");
    assert(message_service_receive(pdu[1])); drain();
    assert(status().inbox == 1u);
    assert(card_action(first, false).outcome == PHONEBOOK_OUTCOME_OK);
    drain(); assert(status().pending == 0u && business_card_service_pending_first() != first);
    assert(!business_card_service_get(first, &((phonebook_entry_t){0})));

    fresh(); first = receive_card(1);
    const uint8_t bad[] = {0};
    assert(storage_object_write(STORAGE_OBJECT_CONTACT, first, bad, sizeof(bad)) == STORAGE_RECORD_OK);
    assert(card_action(first, true).outcome == PHONEBOOK_OUTCOME_ERROR);
    size_t n;
    assert(storage_object_read(STORAGE_OBJECT_CONTACT, first, wire, sizeof(wire), &n) == STORAGE_RECORD_OK);
    assert(n == sizeof(bad) && wire[0] == 0u && business_card_service_pending_first() == first);
    assert(storage_object_write(STORAGE_OBJECT_BUSINESS_CARD, first, bad, sizeof(bad)) == STORAGE_RECORD_OK);
    reopen();
    assert(business_card_service_ready() && business_card_service_pending_first() == 0u);
    assert(corrupt_detections[STORAGE_OBJECT_BUSINESS_CARD] > 0u);
    assert(storage_object_read(STORAGE_OBJECT_BUSINESS_CARD, first, wire, sizeof(wire), &n) == STORAGE_RECORD_OK);
    assert(n == sizeof(bad) && wire[0] == 0u);
}

static void test_card_budget_reclaims_consumed(void) {
    fresh();
    static uint8_t padding[4000];
    for (unsigned i = 0u; i < 7u; i++)
        assert(backend.write(&backend, (uint16_t)(0x7700u + i), padding, sizeof(padding)) == STORAGE_RECORD_OK);
    static char body[900];
    const char *head = "BEGIN:VCARD\r\nN:Alice\r\nTEL:123\r\nNOTE:";
    strcpy(body, head);
    memset(body + strlen(head), 'A', 800u - strlen(head));
    strcpy(body + 800u, "\r\nEND:VCARD\r\n");
    uint32_t first = 0u, last = 0u;
    for (unsigned card = 0u; card < 12u; card++) {
        memset(&file, 0, sizeof(file));
        size_t len = strlen(body), count = (len + 119u) / 120u;
        for (unsigned seq = 0u; seq < count; seq++) {
            sms_deliver_t d = {.udhi=true, .dcs=0xf5};
            strcpy(d.address, "+18135550123");
            assert(sms_deliver_scts_encode(2026, 9, 19, 12, 0, (uint8_t)card, 0, d.scts));
            uint8_t header[] = {11,0,3,(uint8_t)card,(uint8_t)count,(uint8_t)(seq+1u),5,4,0,0xe2,0,0};
            memcpy(d.ud, header, sizeof(header));
            size_t n = len - seq * 120u; if (n > 120u) n = 120u;
            memcpy(d.ud + sizeof(header), body + seq * 120u, n);
            d.udl = d.ud_len = (uint8_t)(sizeof(header) + n);
            uint8_t tpdu;
            assert(sms_deliver_build(&d, pdu[0], sizeof(pdu[0]), &tpdu));
            if (seq == 0u) assert(message_file_receive(&file, pdu[0]));
            else assert(message_file_merge(&file, pdu[0]) == MESSAGE_MERGE_ADDED);
        }
        uint32_t id;
        assert(storage_object_allocate(&id) == STORAGE_RECORD_OK);
        assert(business_card_service_publish(id, &file) == STORAGE_RECORD_OK);
        if (card == 0u) first = id;
        last = id;
        assert(card_action(id, false).outcome == PHONEBOOK_OUTCOME_OK);
    }
    assert(!business_card_service_get(first, &((phonebook_entry_t){0})));
    storage_user_usage_t usage;
    assert(storage_user_get_usage(&usage) == STORAGE_RECORD_OK);
    assert(usage.pools[STORAGE_USER_OTHER].allocated_bytes + STORAGE_USER_CARD_HEADROOM_BYTES <=
           usage.pools[STORAGE_USER_OTHER].limit_bytes);
    assert(backend.write(&backend, 0x7700u, padding, 1200u) == STORAGE_RECORD_OK);
    uint32_t unread = receive_card(40);
    assert(business_card_service_get(last, &((phonebook_entry_t){0})));
    assert(backend.write(&backend, 0x7799u, padding, sizeof(padding)) == STORAGE_RECORD_OK);
    /* Recreate the state after publication committed but staging cleanup did
     * not. The receipt is already consumed and must not be resurrected. */
    size_t len;
    assert(storage_object_read(STORAGE_OBJECT_BUSINESS_CARD, last, wire, sizeof(wire), &len) == STORAGE_RECORD_OK);
    assert(storage_object_write(STORAGE_OBJECT_PENDING_SMS, last, wire, len) == STORAGE_RECORD_OK);
    reopen();
    business_card_service_maintain(now);
    assert(business_card_service_get(last, &((phonebook_entry_t){0})));
    memcpy(baseline, media, sizeof(media));
    operations = 0;
    drain(); /* Legacy-record pressure alone must reclaim consumed history. */
    unsigned count = operations;
    assert(count > 0u);
    assert(!business_card_service_get(last, &((phonebook_entry_t){0})));
    assert(business_card_service_pending_first() == unread);
    for (unsigned point = 1u; point <= count; point++) {
        for (tear = 0u; tear < 3u; tear++) {
            storage_lfs_deinit(); memcpy(media, baseline, sizeof(media)); reopen();
            operations = 0; cut_at = point;
            if (setjmp(cut) == 0) { drain(); assert(false); }
            cut_at = 0; reopen(); drain();
            assert(!business_card_service_get(last, &((phonebook_entry_t){0})));
            assert(business_card_service_pending_first() == unread && status().pending == 0u);
        }
    }
    printf("card pressure cleanup: %u torn-write cases passed\n", count * 3u);
    assert(backend.write(&backend, 0x7700u, padding, sizeof(padding)) == STORAGE_RECORD_OK);
    reopen();
    assert(business_card_service_pending_first() == unread && boot_faults == 0u);
}

static const sms_report_t report_fixture = {
    .first_octet=2u, .reference=17u, .address_type=145u, .status=0u,
    .recipient="+15551234567", .submitted="26/09/21,12:00:00-16", .discharged="26/09/21,12:01:00-16"
};

static void test_reports(void) {
    fresh();
    size_t len;
    assert(message_file_report(&file, &report_fixture));
    assert(message_file_complete(&file) && message_file_control(&file) == SMS_CONTROL_KEEP);
    assert(message_file_encode(&file, wire, sizeof(wire), &len) && len < 100u);
    assert(message_file_decode(&decoded, wire, len));
    assert(memcmp(&decoded.report, &report_fixture, sizeof(report_fixture)) == 0);
    assert(message_file_content(&decoded, &content) && content.metadata.report &&
           !content.metadata.binary && content.metadata.report_status == 0u && content.text[0] == 0);
    for (size_t i = 0; i < len; i++) assert(!message_file_decode(&decoded, wire, i));
    wire[3] |= MESSAGE_FILE_SENT;
    assert(!message_file_decode(&decoded, wire, len));
    wire[3] = MESSAGE_FILE_REPORT;
    wire[12] = 0; /* embedded NUL in recipient */
    assert(!message_file_decode(&decoded, wire, len));

    assert(message_service_receive_report(&report_fixture)); drain();
    assert(status().inbox == 1u && status().received == 1u && status().unread == 1u);
    uint32_t id = first_id(MESSAGE_INBOX), token;
    assert(message_service_request_read(MESSAGE_INBOX, id, &token));
    assert(result(token, true).outcome == MESSAGE_RESULT_OK && content.metadata.report);
    reopen();
    assert(status().unread == 0u && first_id(MESSAGE_INBOX) == id);
    assert(message_service_receive_report(&report_fixture)); drain();
    assert(status().inbox == 1u && status().received == 0u && status().unread == 0u);
    sms_report_t pending = report_fixture; pending.status = 0x20u;
    assert(message_service_receive_report(&pending)); drain();
    assert(status().inbox == 2u); /* Late pending evidence cannot revert a delivered report. */
    pending.reference++;
    assert(message_service_receive_report(&pending)); drain();
    assert(status().inbox == 3u);
    make_pdu(pdu[0], 1, 1, 0, 1, "Keep ordinary SMS");
    assert(message_service_receive(pdu[0])); drain();
    assert(message_service_request_save("123", "Keep draft", &token));
    assert(result(token, false).outcome == MESSAGE_RESULT_OK);
    assert(message_service_request_delete_reports(&token));
    message_result_t r = result(token, false);
    assert(r.outcome == MESSAGE_RESULT_OK && r.count == 3u);
    reopen(); assert(status().inbox == 1u && status().outbox == 1u);
    assert(message_service_request_read(MESSAGE_INBOX, first_id(MESSAGE_INBOX), &token));
    assert(result(token, true).outcome == MESSAGE_RESULT_OK && !content.metadata.report);

    busy = true;
    assert(message_service_receive_report(&report_fixture)); drain();
    assert(status().queued == 1u && status().inbox == 1u);
    busy = false; drain(); assert(status().queued == 0u && status().inbox == 2u);
    sms_report_t invalid = report_fixture; memset(invalid.recipient, '1', sizeof(invalid.recipient));
    assert(!message_service_receive_report(&invalid) && status().receive_errors == 1u);

    fresh();
    assert(message_service_receive_report(&report_fixture)); drain();
    id = first_id(MESSAGE_INBOX);
    assert(message_file_report(&file, &report_fixture));
    assert(message_file_encode(&file, wire, sizeof(wire), &len));
    wire[12] = 0u;
    assert(storage_object_write(STORAGE_OBJECT_INBOX, id, wire, len) == STORAGE_RECORD_OK);
    /* Do not reload: the bulk operation must revalidate its still-cached row. */
    assert(message_service_request_delete_reports(&token));
    assert(result(token, false).outcome == MESSAGE_RESULT_ERROR);
    size_t actual;
    assert(storage_object_read(STORAGE_OBJECT_INBOX, id, wire, sizeof(wire), &actual) == STORAGE_RECORD_OK &&
           actual == len && wire[12] == 0u && corrupt_detections[STORAGE_OBJECT_INBOX] != 0u);
    reopen(); assert(status().ready && status().inbox == 0u && boot_faults == 0u);
}

static void test_report_power_cuts(void) {
    fresh();
    make_pdu(pdu[0], 1, 1, 0, 1, "Survives report writes");
    assert(message_service_receive(pdu[0])); drain();
    memcpy(baseline, media, sizeof(media));
    operations = 0;
    assert(message_service_receive_report(&report_fixture)); drain();
    unsigned count = operations;
    assert(count > 0u);
    for (unsigned point = 1u; point <= count; point++) {
        for (tear = 0u; tear < 3u; tear++) {
            storage_lfs_deinit(); memcpy(media, baseline, sizeof(media)); reopen();
            operations = 0; cut_at = point;
            if (setjmp(cut) == 0) { assert(message_service_receive_report(&report_fixture)); drain(); assert(false); }
            cut_at = 0; reopen(); drain();
            assert(status().ready && status().inbox >= 1u && status().inbox <= 2u && !status().storage_error);
            assert(message_service_receive_report(&report_fixture)); drain();
            assert(status().inbox == 2u && boot_faults == 0u);
        }
    }
    printf("report writes: %u torn-write cases passed\n", count * 3u);
}

static void test_report_delete_power_cuts(void) {
    fresh();
    uint32_t token, damaged;
    make_pdu(pdu[0], 1, 1, 0, 1, "Keep beside reports");
    assert(message_service_receive(pdu[0])); drain();
    uint32_t normal = first_id(MESSAGE_INBOX);
    assert(message_service_request_save("123", "Keep outbox", &token));
    assert(result(token, false).outcome == MESSAGE_RESULT_OK);
    sms_report_t report = report_fixture;
    for (unsigned i = 0u; i < 3u; i++) {
        report.reference++;
        assert(message_service_receive_report(&report)); drain();
    }
    size_t len;
    assert(message_file_report(&file, &report));
    assert(message_file_encode(&file, wire, sizeof(wire), &len));
    wire[12] = 0u;
    assert(storage_object_allocate(&damaged) == STORAGE_RECORD_OK);
    assert(storage_object_write(STORAGE_OBJECT_INBOX, damaged, wire, len) == STORAGE_RECORD_OK);
    reopen();
    assert(status().inbox == 4u && corrupt_detections[STORAGE_OBJECT_INBOX] != 0u);
    memcpy(baseline, media, sizeof(media));
    operations = 0u;
    assert(message_service_request_delete_reports(&token));
    assert(result(token, false).outcome == MESSAGE_RESULT_OK);
    unsigned count = operations;
    assert(count > 0u);
    for (unsigned point = 1u; point <= count; point++) {
        for (tear = 0u; tear < 3u; tear++) {
            storage_lfs_deinit(); memcpy(media, baseline, sizeof(media)); reopen();
            operations = 0u; cut_at = point;
            if (setjmp(cut) == 0) {
                assert(message_service_request_delete_reports(&token));
                drain(); assert(false);
            }
            cut_at = 0u; reopen(); drain();
            assert(status().ready && !status().storage_error && status().inbox >= 1u &&
                   status().inbox <= 4u && status().outbox == 1u);
            assert(message_service_request_delete_reports(&token));
            assert(result(token, false).outcome == MESSAGE_RESULT_OK);
            reopen();
            assert(status().inbox == 1u && first_id(MESSAGE_INBOX) == normal && status().outbox == 1u);
            assert(message_service_request_read(MESSAGE_INBOX, normal, &token));
            assert(result(token, true).outcome == MESSAGE_RESULT_OK &&
                   strcmp(content.text, "Keep beside reports") == 0);
            size_t actual;
            assert(storage_object_read(STORAGE_OBJECT_INBOX, damaged, wire, sizeof(wire), &actual) == STORAGE_RECORD_OK &&
                   actual == len && wire[3] == MESSAGE_FILE_REPORT && wire[12] == 0u && boot_faults == 0u);
        }
    }
    printf("report bulk deletion: %u torn-write cases passed\n", count * 3u);
}

static void test_report_full(void) {
    fresh();
    size_t len;
    assert(message_file_report(&file, &report_fixture));
    assert(message_file_encode(&file, wire, sizeof(wire), &len));
    unsigned added = 0u;
    for (; added < MESSAGE_MAILBOX_LIMIT; added++) {
        uint32_t id;
        assert(storage_object_allocate(&id) == STORAGE_RECORD_OK);
        storage_record_result_t rc = storage_object_write(STORAGE_OBJECT_INBOX, id, wire, len);
        if (rc == STORAGE_RECORD_FULL) break;
        assert(rc == STORAGE_RECORD_OK);
    }
    reopen();
    assert(added != 0u && status().inbox == added);
    sms_report_t report = report_fixture; report.reference++;
    assert(message_service_receive_report(&report));
    tick();
    assert(status().full && status().full_events == 1u && status().queued == 1u &&
           status().received == 0u && message_service_sleep_ready());
    uint32_t token;
    assert(message_service_request_save("123", "Full inbox cannot block outbox", &token));
    assert(result(token, false).outcome == MESSAGE_RESULT_OK && status().outbox == 1u);
    assert(message_service_request_delete_reports(&token));
    message_result_t r;
    bool done = false;
    for (unsigned i = 0u; i <= added + 1u; i++) {
        message_service_tick(++now, NULL);
        if (message_service_pop_result(token, &r, NULL)) { done = true; break; }
    }
    assert(done && r.outcome == MESSAGE_RESULT_OK && r.count == added);
    message_service_tick(++now, NULL);
    assert(status().queued == 0u && status().inbox == 1u && status().received == 1u && !status().full);
    /* The held arrival is published after the erase, not erased with old reports. */
    assert(message_service_request_read(MESSAGE_INBOX, first_id(MESSAGE_INBOX), &token));
    assert(result(token, true).outcome == MESSAGE_RESULT_OK && content.metadata.report);
    reopen(); assert(status().inbox == 1u && status().outbox == 1u);

    fresh();
    for (unsigned i = 0u; i < 8u; i++) {
        report.reference++;
        assert(message_service_receive_report(&report));
    }
    assert(!message_service_receive_report(&report) && status().receive_errors == 1u);
    drain();
    assert(status().inbox == 8u && status().queued == 0u);
}

int main(void) {
    test_reports(); test_report_power_cuts(); test_report_delete_power_cuts(); test_report_full();
    test_codec(); test_mailboxes(); test_multipart(); test_power_cuts(); test_busy_and_full();
    test_sent_copy_and_category_isolation(); test_failed_queue_sleep();
    test_io_retry_and_corrupt_record(); test_publication_conflict_isolation(); test_delete_power_cuts();
    test_expiry(); test_expiry_power_cuts(); test_control_admission();
    test_durable_receive_receipts();
    test_business_cards(); test_card_save_power_cuts(); test_card_publication_power_cuts();
    test_card_runtime_recovery();
    test_card_limits_and_corruption();
    test_card_budget_reclaims_consumed();
    storage_lfs_deinit(); puts("PASS: local messages");
}

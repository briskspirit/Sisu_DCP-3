#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include "services/message_service.h"
#include "services/message_file_codec.h"
#include "services/sms_deliver_codec.h"
#include "storage/storage_lfs.h"
#include "storage/storage_layout.h"
#include "storage/storage_objects.h"
#include "storage/storage_user_space.h"

static uint8_t media[STORAGE_USER_BYTES], baseline[STORAGE_USER_BYTES];
static unsigned operations, cut_at, tear;
static bool busy, io_error;
static jmp_buf cut;
static storage_backend_t backend;
static uint32_t now;
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
    if (busy || io_error) return busy ? NVM_STATUS_BUSY : NVM_STATUS_IO_ERROR;
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
    message_service_init();
}
static void fresh(void) {
    storage_lfs_deinit(); memset(media, 0xff, sizeof(media));
    cut_at = 0; busy = io_error = false; now = 0; reopen();
}
static void tick(void) { now += 1001u; message_service_tick(now); }
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
    assert(!message_service_idle() && status().queued == 1u && status().inbox == 0u);
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
    make_pdu(pdu[0], 1, 1, 0, 40, "Inbox must remain available");
    assert(message_service_receive(pdu[0]));
    drain();
    assert(status().inbox == 1u && status().outbox == count && status().queued == 1u);
    /* Reclaim a whole quota block, then the exact held sent copy can commit. */
    for (unsigned n = 0; n < 16u && status().queued; n++) {
        assert(message_service_request_delete(MESSAGE_OUTBOX, first_id(MESSAGE_OUTBOX), &token));
        assert(result(token, false).outcome == MESSAGE_RESULT_OK);
        drain();
    }
    assert(status().queued == 0u && status().inbox == 1u);
    printf("outbox budget held %u compact records; full outbox did not block inbox\n", count);
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
    assert(storage_object_write(STORAGE_OBJECT_INBOX, id, (const uint8_t *)"bad", 3u) == STORAGE_RECORD_OK);
    reopen();
    assert(!status().ready && status().storage_error);
    size_t len = 0u;
    assert(storage_object_read(STORAGE_OBJECT_INBOX, id, wire, sizeof(wire), &len) == STORAGE_RECORD_OK &&
           len == 3u && memcmp(wire, "bad", 3u) == 0);
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

int main(void) {
    test_codec(); test_mailboxes(); test_multipart(); test_power_cuts(); test_busy_and_full();
    test_sent_copy_and_category_isolation(); test_io_retry_and_corrupt_record(); test_delete_power_cuts();
    storage_lfs_deinit(); puts("PASS: local messages");
}

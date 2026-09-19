#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

#include "storage/storage_lfs.h"
#include "storage/storage_layout.h"
#include "storage/storage_objects.h"
#include "storage/storage_user_space.h"
#include "services/phonebook_service.h"
#include "storage/store_service.h"

static uint8_t media[STORAGE_USER_BYTES], baseline[sizeof(media)];
static unsigned operations, cut_at, tear;
static bool busy, io_error, return_error;
static jmp_buf cut;
static storage_backend_t backend;

static nvm_status_t read_media(nvm_hal_t *h, uint32_t off, void *dst, size_t n) {
    assert(off <= h->capacity && n <= h->capacity - off);
    if (io_error) return NVM_STATUS_IO_ERROR;
    memcpy(dst, media + off, n);
    return NVM_STATUS_OK;
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
            assert((media[off+i] & b) == b);
            media[off+i] &= b;
        }
    }
    if (interrupted) {
        if (return_error) return NVM_STATUS_IO_ERROR;
        longjmp(cut, 1);
    }
    return NVM_STATUS_OK;
}
static nvm_status_t write_media(nvm_hal_t *h, uint32_t off, const void *src, size_t n) {
    assert(off % 256u == 0u && n == 256u);
    return change(h, off, src, n);
}
static nvm_status_t erase_media(nvm_hal_t *h, uint32_t off, size_t n) {
    assert(off % 4096u == 0u && n == 4096u);
    return change(h, off, NULL, n);
}
static nvm_hal_t hal = {.capacity=sizeof(media), .erase_block=4096, .write_block=256,
    .erase_required=true, .read=read_media, .write=write_media, .erase=erase_media};

static void reopen(void) {
    storage_lfs_deinit();
    assert(storage_lfs_init(&backend, &hal) == STORAGE_RECORD_OK);
    assert(storage_objects_open() == STORAGE_RECORD_OK);
    phonebook_service_init();
}
static void fresh(void) {
    storage_lfs_deinit();
    memset(media, 0xff, sizeof(media));
    cut_at = 0;
    busy = io_error = return_error = false;
    reopen();
}

static unsigned prunes;
void store_phonebook_prune_bindings(bool (*exists)(uint32_t)) {
    assert(exists != NULL);
    prunes++;
}
static phonebook_result_t finish(uint32_t id) {
    phonebook_result_t result;
    phonebook_service_tick();
    assert(phonebook_service_pop_result(&result) && result.request_id == id);
    assert(!phonebook_service_pop_result(&result));
    return result;
}
static uint32_t add(const char *name, const char *number) {
    uint32_t request;
    assert(phonebook_service_request_add(name, number, &request));
    assert(finish(request).outcome == PHONEBOOK_OUTCOME_OK);
    phonebook_entry_t entry;
    assert(phonebook_service_entry(phonebook_service_count()-1u, &entry));
    return entry.index;
}
static void test_crud(void) {
    fresh();
    assert(phonebook_service_cache_valid() && phonebook_service_count() == 0u);
    uint8_t counter[] = {1, 0, 1, 0};
    assert(backend.write(&backend, 0xfff1u, counter, sizeof(counter)) == STORAGE_RECORD_OK);
    reopen();
    uint32_t id = add("Ada", "+15550000001");
    assert(id == 65537u);
    reopen();
    phonebook_entry_t entry;
    assert(phonebook_service_entry(0, &entry) && entry.index == id);
    assert(strcmp(entry.name, "Ada") == 0 && strcmp(entry.number, "+15550000001") == 0);
    uint32_t request;
    assert(phonebook_service_request_update(id, "New name", "5551234", &request));
    assert(finish(request).outcome == PHONEBOOK_OUTCOME_OK);
    reopen();
    assert(phonebook_service_entry(0, &entry) && strcmp(entry.name, "New name") == 0);
    unsigned before = prunes;
    assert(phonebook_service_request_delete(id, &request));
    assert(finish(request).outcome == PHONEBOOK_OUTCOME_OK && prunes > before);
    reopen();
    assert(phonebook_service_count() == 0);
    assert(add("Replacement", "123") > id);
    assert(phonebook_service_request_update(id, "Stale", "456", &request));
    assert(finish(request).outcome == PHONEBOOK_OUTCOME_ERROR);
    assert(phonebook_service_request_list(&request));
    assert(finish(request).outcome == PHONEBOOK_OUTCOME_OK && phonebook_service_count() == 1);
    assert(!phonebook_service_request_add("Name that cannot fit in twenty four bytes", "123", &request));
    assert(!phonebook_service_request_add("A", "", &request));
    assert(!phonebook_service_request_list(NULL));
}
static void test_queue_and_busy(void) {
    fresh();
    uint32_t request[PHONEBOOK_RESULT_CAPACITY], rejected = 9;
    for (unsigned i = 0; i < PHONEBOOK_RESULT_CAPACITY; i++)
        assert(phonebook_service_request_list(&request[i]));
    assert(!phonebook_service_request_list(&rejected) && rejected == 0);
    for (unsigned i = 0; i < PHONEBOOK_RESULT_CAPACITY; i++) phonebook_service_tick();
    assert(phonebook_service_idle());
    assert(!phonebook_service_request_list(&rejected) && rejected == 0);
    phonebook_result_t result;
    for (unsigned i = 0; i < PHONEBOOK_RESULT_CAPACITY; i++) {
        assert(phonebook_service_pop_result(&result) && result.request_id == request[i] &&
               result.outcome == PHONEBOOK_OUTCOME_OK);
    }
    assert(phonebook_service_request_add("Busy", "123", &request[0]));
    busy = true;
    phonebook_service_tick();
    assert(!phonebook_service_idle() && !phonebook_service_pop_result(&result));
    busy = false;
    assert(finish(request[0]).outcome == PHONEBOOK_OUTCOME_OK);
    reopen();
    assert(phonebook_service_count() == 1);
}
static void test_cuts(unsigned operation, bool fail_io) {
    fresh();
    uint32_t existing = add("Before", "123");
    memcpy(baseline, media, sizeof(media));
    unsigned count = 0;
    for (unsigned pass = 0; pass == 0 || pass <= count * 3u; pass++) {
        storage_lfs_deinit();
        memcpy(media, baseline, sizeof(media));
        reopen();
        operations = 0;
        cut_at = pass == 0 ? 0 : (pass - 1) / 3 + 1;
        tear = pass == 0 ? 0 : (pass - 1) % 3;
        return_error = fail_io;
        uint32_t request;
        if (operation == 0) assert(phonebook_service_request_add("Added", "456", &request));
        else if (operation == 1) assert(phonebook_service_request_update(existing, "After", "789", &request));
        else assert(phonebook_service_request_delete(existing, &request));
        if (setjmp(cut) == 0) {
            phonebook_service_tick();
            if (pass == 0) count = operations;
        }
        return_error = false;
        cut_at = 0;
        reopen();
        assert(phonebook_service_cache_valid());
        phonebook_entry_t entry;
        if (operation == 0) {
            assert(phonebook_service_count() == 1 || phonebook_service_count() == 2);
            assert(phonebook_service_entry(0, &entry) && strcmp(entry.name, "Before") == 0);
        } else if (operation == 1) {
            assert(phonebook_service_count() == 1 && phonebook_service_entry(0, &entry));
            assert(entry.index == existing);
            assert((strcmp(entry.name, "Before") == 0 && strcmp(entry.number, "123") == 0) ||
                   (strcmp(entry.name, "After") == 0 && strcmp(entry.number, "789") == 0));
        } else assert(phonebook_service_count() <= 1);
    }
    printf("contact operation %u: %u boundaries x 3 %s cases passed\n",
           operation, count, fail_io ? "I/O error" : "power-cut");
}
static void test_full_and_corrupt(void) {
    fresh();
    phonebook_outcome_t outcome = PHONEBOOK_OUTCOME_NONE;
    unsigned added = 0;
    while (added < PHONEBOOK_MAX_RECORDS) {
        uint32_t request;
        assert(phonebook_service_request_add("123456789012345678901234", "+1234567890123456789012345678901", &request));
        outcome = finish(request).outcome;
        if (outcome != PHONEBOOK_OUTCOME_OK) break;
        added++;
    }
    assert(added > 10);
    uint32_t request;
    if (added == PHONEBOOK_MAX_RECORDS) {
        assert(phonebook_service_request_add("Overflow", "123", &request));
        outcome = finish(request).outcome;
    }
    assert(outcome == PHONEBOOK_OUTCOME_FULL);
    reopen();
    assert(phonebook_service_count() == added);
    phonebook_entry_t entry;
    assert(phonebook_service_entry(0, &entry));
    assert(phonebook_service_request_delete(entry.index, &request));
    assert(finish(request).outcome == PHONEBOOK_OUTCOME_OK);
    reopen();
    assert(phonebook_service_count() == added - 1u);
    fresh();
    uint32_t id = add("Corrupt", "123");
    uint8_t bad[] = {'C', 1, 250, 1};
    assert(storage_object_write(STORAGE_OBJECT_CONTACT, id, bad, sizeof(bad)) == STORAGE_RECORD_OK);
    unsigned before = prunes;
    reopen();
    assert(!phonebook_service_cache_valid() && phonebook_service_count() == 0 && prunes == before);
}
int main(void) {
    test_crud();
    test_queue_and_busy();
    for (unsigned i = 0; i < 3; i++) { test_cuts(i, false); test_cuts(i, true); }
    test_full_and_corrupt();
    storage_lfs_deinit();
    puts("local phonebook tests passed");
    return 0;
}

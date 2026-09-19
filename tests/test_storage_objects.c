#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

#include "storage/storage_lfs.h"
#include "storage/storage_layout.h"
#include "storage/storage_objects.h"
#include "storage/storage_user_space.h"

static uint8_t media[STORAGE_USER_BYTES], baseline[sizeof(media)];
static uint8_t data[STORAGE_OBJECT_MAX_PAYLOAD], readback[sizeof(data)];
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
}
static void fresh(void) {
    storage_lfs_deinit();
    memset(media, 0xff, sizeof(media));
    cut_at = 0;
    busy = io_error = return_error = false;
    reopen();
}
static void verify(uint32_t id, size_t n, uint8_t byte) {
    size_t got;
    assert(storage_object_read(STORAGE_OBJECT_INBOX, id, readback, sizeof(readback), &got) == STORAGE_RECORD_OK);
    assert(got == n);
    for (size_t i = 0; i < n; i++) assert(readback[i] == byte);
}
static void test_basics(void) {
    fresh();
    uint32_t id, next;
    assert(storage_object_allocate(&id) == STORAGE_RECORD_OK && id == 1u);
    memset(data, 0x6a, sizeof(data));
    assert(storage_object_write(STORAGE_OBJECT_INBOX, id, data, sizeof(data)) == STORAGE_RECORD_OK);
    reopen();
    verify(id, sizeof(data), 0x6a);
    assert(storage_object_scan_begin(STORAGE_OBJECT_INBOX) == STORAGE_RECORD_OK);
    assert(storage_object_scan_next(&next) == STORAGE_RECORD_OK && next == id);
    assert(storage_object_allocate(&next) == STORAGE_RECORD_ERROR);
    verify(id, sizeof(data), 0x6a);
    assert(storage_object_scan_next(&next) == STORAGE_RECORD_NOT_FOUND);
    storage_object_scan_end();
    assert(storage_object_remove(STORAGE_OBJECT_INBOX, id) == STORAGE_RECORD_OK);
    reopen();
    assert(storage_object_allocate(&next) == STORAGE_RECORD_OK && next > id);
    assert(storage_object_write(STORAGE_OBJECT_CONTACT, next, data, 80u) == STORAGE_RECORD_OK);
    size_t n;
    assert(storage_object_read(STORAGE_OBJECT_INBOX, next, readback, sizeof(readback), &n) == STORAGE_RECORD_NOT_FOUND);
    assert(storage_object_write(STORAGE_OBJECT_INBOX, next+1u, data, 1u) == STORAGE_RECORD_ERROR);
    assert(storage_object_write((storage_object_collection_t)-1, id, data, 1u) == STORAGE_RECORD_ERROR);
    assert(storage_lfs_remove(&backend, 0xfff1u) == STORAGE_RECORD_OK);
    storage_lfs_deinit();
    assert(storage_lfs_init(&backend, &hal) == STORAGE_RECORD_OK);
    assert(storage_objects_open() == STORAGE_RECORD_ERROR);
}
static void test_write_cuts(size_t old_len, size_t new_len, bool fail_io) {
    fresh();
    uint32_t id;
    assert(storage_object_allocate(&id) == STORAGE_RECORD_OK);
    memset(data, 0x11, sizeof(data));
    assert(storage_object_write(STORAGE_OBJECT_INBOX, id, data, old_len) == STORAGE_RECORD_OK);
    uint32_t neighbor;
    assert(storage_object_allocate(&neighbor) == STORAGE_RECORD_OK);
    assert(storage_object_write(STORAGE_OBJECT_INBOX, neighbor, data, 80u) == STORAGE_RECORD_OK);
    memcpy(baseline, media, sizeof(media));
    memset(data, 0x22, sizeof(data));
    operations = 0;
    assert(storage_object_write(STORAGE_OBJECT_INBOX, id, data, new_len) == STORAGE_RECORD_OK);
    unsigned count = operations;
    for (unsigned point = 1; point <= count; point++) {
        for (tear = 0; tear < 3u; tear++) {
            storage_lfs_deinit();
            memcpy(media, baseline, sizeof(media));
            reopen();
            operations = 0;
            cut_at = point;
            return_error = fail_io;
            if (setjmp(cut) == 0) {
                storage_record_result_t rc = storage_object_write(STORAGE_OBJECT_INBOX, id, data, new_len);
                assert(fail_io && rc == STORAGE_RECORD_ERROR && operations >= point);
            }
            return_error = false;
            cut_at = 0;
            if (!fail_io) reopen();
            size_t n;
            assert(storage_object_read(STORAGE_OBJECT_INBOX, id, readback, sizeof(readback), &n) == STORAGE_RECORD_OK);
            bool old = n == old_len && (n == 0u || readback[0] == 0x11);
            bool replacement = n == new_len && (n == 0u || readback[0] == 0x22);
            assert(old || replacement);
            verify(id, n, old ? 0x11 : 0x22);
            verify(neighbor, 80u, 0x11);
            assert(storage_object_write(STORAGE_OBJECT_INBOX, id, data, new_len) == STORAGE_RECORD_OK);
            reopen();
            verify(id, new_len, 0x22);
        }
    }
    printf("object replacement %zu -> %zu (%s): %u torn-write cases passed\n",
           old_len, new_len, fail_io ? "I/O error" : "power cut", count*3u);
}
static void test_allocator_cuts(bool fail_io) {
    fresh();
    uint32_t first;
    assert(storage_object_allocate(&first) == STORAGE_RECORD_OK);
    assert(storage_object_write(STORAGE_OBJECT_INBOX, first, data, 80u) == STORAGE_RECORD_OK);
    memcpy(baseline, media, sizeof(media));
    uint32_t allocated;
    operations = 0;
    assert(storage_object_allocate(&allocated) == STORAGE_RECORD_OK);
    unsigned count = operations;
    for (unsigned point = 1; point <= count; point++) {
        for (tear = 0; tear < 3; tear++) {
            storage_lfs_deinit();
            memcpy(media, baseline, sizeof(media));
            reopen();
            operations = 0;
            cut_at = point;
            return_error = fail_io;
            if (setjmp(cut) == 0) {
                storage_record_result_t rc = storage_object_allocate(&allocated);
                assert(fail_io && rc == STORAGE_RECORD_ERROR && allocated == 0u);
            }
            return_error = false;
            cut_at = 0;
            if (!fail_io) reopen();
            assert(storage_object_allocate(&allocated) == STORAGE_RECORD_OK && allocated > first);
            uint32_t next;
            assert(storage_object_allocate(&next) == STORAGE_RECORD_OK && next > allocated);
        }
    }
}
static void test_full(size_t payload) {
    fresh();
    uint32_t first = 0;
    unsigned records = 0;
    memset(data, 0x44, sizeof(data));
    for (;;) {
        uint32_t id;
        storage_record_result_t rc = storage_object_allocate(&id);
        if (rc == STORAGE_RECORD_OK) rc = storage_object_write(STORAGE_OBJECT_INBOX, id, data, payload);
        if (rc != STORAGE_RECORD_OK) {
            assert(rc == STORAGE_RECORD_FULL);
            break;
        }
        if (first == 0u) first = id;
        records++;
        assert(records < 3000u);
    }
    reopen();
    for (unsigned i = 0; i < records; i++) verify(first + i, payload, 0x44);
    printf("FULL at %u objects, %ld allocated blocks\n", records, (long)storage_lfs_used_blocks());
    /* A physically full metadata chain can need more than one deletion before
     * it releases a block. Verify eventual reuse, without assuming a quota
     * implementation that guarantees one-for-one replacement at physical FULL. */
    uint32_t id = 0u;
    unsigned removed = 0u;
    storage_record_result_t rc;
    do {
        assert(removed < records);
        assert(storage_object_remove(STORAGE_OBJECT_INBOX, first + removed) == STORAGE_RECORD_OK);
        removed++;
        rc = id == 0u ? storage_object_allocate(&id) : STORAGE_RECORD_OK;
        if (rc == STORAGE_RECORD_OK) rc = storage_object_write(STORAGE_OBJECT_INBOX, id, data, 80u);
        assert(rc == STORAGE_RECORD_FULL || rc == STORAGE_RECORD_OK);
    } while (rc != STORAGE_RECORD_OK);
    reopen();
    verify(id, 80u, 0x44);
    for (unsigned i = removed; i < records; i++) verify(first + i, payload, 0x44);
    printf("144 KiB inbox budget: %u objects with %zu-byte payload before FULL; %u deletions before reuse\n",
           records, payload, removed);
}

static storage_user_usage_t check_usage(void) {
    storage_user_usage_t usage, cached;
    static const unsigned limits[] = {64, 144, 48, 32, 64};
    unsigned before = operations;
    assert(storage_user_get_usage(&usage) == STORAGE_RECORD_OK);
    assert(storage_user_get_usage(&cached) == STORAGE_RECORD_OK);
    assert(operations == before && memcmp(&usage, &cached, sizeof(usage)) == 0);
    assert(usage.capacity_bytes == sizeof(media));
    assert(usage.recovery_reserve_bytes == 32u * 1024u);
    unsigned allocated = 0;
    for (unsigned i = 0; i < STORAGE_USER_POOL_COUNT; i++) {
        assert(usage.pools[i].limit_bytes == limits[i] * 1024u);
        assert(usage.pools[i].allocated_bytes <= usage.pools[i].limit_bytes);
        allocated += usage.pools[i].allocated_bytes;
    }
    assert(allocated == usage.allocated_bytes);
    assert(allocated == (uint32_t)storage_lfs_used_blocks() * 4096u);
    assert(usage.capacity_bytes - allocated >= usage.recovery_reserve_bytes);
    return usage;
}

static void test_accounting(void) {
    fresh();
    storage_user_usage_t usage = check_usage();
    assert(usage.allocated_bytes == 10u * 4096u);
    for (unsigned i = 0; i < STORAGE_OBJECT_COLLECTION_COUNT; i++) {
        assert(usage.pools[i].allocated_bytes == 8192u);
        assert(usage.pools[i].contents.files == 0u);
    }
    uint32_t id;
    assert(storage_object_allocate(&id) == STORAGE_RECORD_OK);
    assert(storage_object_write(STORAGE_OBJECT_INBOX, id, data, 492u) == STORAGE_RECORD_OK);
    usage = check_usage();
    assert(usage.pools[STORAGE_USER_INBOX].contents.file_bytes == 512u);
    assert(usage.pools[STORAGE_USER_INBOX].allocated_bytes == 8192u);
    assert(storage_object_write(STORAGE_OBJECT_INBOX, id, data, 493u) == STORAGE_RECORD_OK);
    usage = check_usage();
    assert(usage.pools[STORAGE_USER_INBOX].contents.file_bytes == 513u);
    assert(usage.pools[STORAGE_USER_INBOX].allocated_bytes == 12288u);
    const uint16_t records[] = {0x3216, 0x3217, 0x3218, 0x3219, 0x321a, 0x321b, 0x321c};
    for (unsigned i = 0; i < 7u; i++) {
        assert(backend.write(&backend, records[i], data, 300u) == STORAGE_RECORD_OK);
    }
    usage = check_usage();
    assert(usage.legacy[STORAGE_USER_CALLS].files == 3u);
    assert(usage.legacy[STORAGE_USER_CALLS].file_bytes == 3u * 316u);
    assert(usage.legacy[STORAGE_USER_PICTURES].file_bytes == 316u);
    assert(usage.legacy[STORAGE_USER_TONES].file_bytes == 316u);
    assert(usage.legacy[STORAGE_USER_DICTIONARY].file_bytes == 316u);
    assert(usage.legacy[STORAGE_USER_DIVERT].file_bytes == 316u);
    reopen();
    storage_user_usage_t remounted = check_usage();
    assert(memcmp(&usage, &remounted, sizeof(usage)) == 0);
    assert(storage_object_remove(STORAGE_OBJECT_INBOX, id) == STORAGE_RECORD_OK);
    usage = check_usage();
    assert(usage.pools[STORAGE_USER_INBOX].contents.files == 0u);
    assert(usage.pools[STORAGE_USER_INBOX].allocated_bytes == 8192u);
}

static void test_category_isolation(size_t payload) {
    fresh();
    memset(data, 0x5a, sizeof(data));
    const storage_object_collection_t order[] = {STORAGE_OBJECT_INBOX,
        STORAGE_OBJECT_CONTACT, STORAGE_OBJECT_OUTBOX, STORAGE_OBJECT_PENDING_SMS};
    uint32_t first[4], last[4];
    unsigned counts[4] = {0};
    for (unsigned p = 0; p < 4u; p++) {
        storage_object_collection_t collection = order[p];
        for (;;) {
            uint32_t id;
            assert(storage_object_allocate(&id) == STORAGE_RECORD_OK);
            storage_record_result_t rc = storage_object_write(collection, id, data, payload);
            if (rc == STORAGE_RECORD_FULL) break;
            assert(rc == STORAGE_RECORD_OK);
            if (counts[p]++ == 0u) first[p] = id;
            last[p] = id;
            assert(counts[p] < 1000u);
            (void)check_usage();
        }
        assert(counts[p] >= (payload > 492u ? 6u : 12u));
        reopen();
        storage_user_usage_t usage = check_usage();
        assert(usage.pools[collection].contents.files == counts[p]);
        assert(usage.pools[collection].contents.file_bytes == counts[p] * (payload + 20u));
    }
    /* All four collections are FULL, but existing semantic user data still
     * has its own space. System isolation is tested by test_storage_partitions. */
    for (uint16_t id = 0x3216; id <= 0x321c; id++) {
        assert(backend.write(&backend, id, data, 3000u) == STORAGE_RECORD_OK);
        (void)check_usage();
    }
    reopen();
    for (unsigned p = 0; p < 4u; p++) {
        unsigned count = 0;
        assert(storage_object_scan_begin(order[p]) == STORAGE_RECORD_OK);
        uint32_t id;
        storage_record_result_t rc;
        while ((rc = storage_object_scan_next(&id)) == STORAGE_RECORD_OK) {
            size_t n;
            assert(storage_object_read(order[p], id, readback, sizeof(readback), &n) == STORAGE_RECORD_OK);
            assert(n == payload && memcmp(data, readback, n) == 0);
            count++;
        }
        assert(rc == STORAGE_RECORD_NOT_FOUND && count == counts[p]);
        storage_object_scan_end();
    }
    /* A quota-denied larger replacement must not lose the old inline message. */
    if (payload <= 492u) {
        assert(storage_object_write(STORAGE_OBJECT_INBOX, first[0], data, sizeof(data)) == STORAGE_RECORD_FULL);
        reopen();
        verify(first[0], payload, 0x5a);
    }
    for (unsigned p = 0; p < 4u; p++) {
        for (uint32_t id = first[p]; id <= last[p]; id++) {
            assert(storage_object_remove(order[p], id) == STORAGE_RECORD_OK);
            (void)check_usage();
        }
    }
    reopen();
    storage_user_usage_t usage = check_usage();
    for (unsigned p = 0; p < 4u; p++) {
        assert(usage.pools[p].contents.files == 0u);
        uint32_t id;
        assert(storage_object_allocate(&id) == STORAGE_RECORD_OK);
        assert(storage_object_write((storage_object_collection_t)p, id, data, sizeof(data)) == STORAGE_RECORD_OK);
    }
    printf("category isolation (%zu-byte payload): inbox=%u contacts=%u outbox=%u pending=%u; shared writes and reuse passed\n",
           payload, counts[0], counts[1], counts[2], counts[3]);
}

static void test_quota_boundary(void) {
    fresh();
    memset(data, 0x31, sizeof(data));
    uint32_t first = 0u, last = 0u, id;
    unsigned records = 0u;
    for (;;) {
        assert(storage_object_allocate(&id) == STORAGE_RECORD_OK);
        storage_record_result_t rc = storage_object_write(STORAGE_OBJECT_INBOX, id, data, 514u);
        if (rc == STORAGE_RECORD_FULL) break;
        assert(rc == STORAGE_RECORD_OK);
        if (records++ == 0u) first = id;
        last = id;
    }
    reopen();
    assert(storage_object_write(STORAGE_OBJECT_INBOX, first, data, 80u) == STORAGE_RECORD_OK);
    assert(storage_object_allocate(&id) == STORAGE_RECORD_OK);
    assert(storage_object_write(STORAGE_OBJECT_INBOX, id, data, 514u) == STORAGE_RECORD_OK);
    storage_user_usage_t usage = check_usage();
    assert(usage.pools[STORAGE_USER_INBOX].allocated_bytes == 144u * 1024u);
    /* No new block can be admitted. Wear-level relocation must fall back to
     * the original pair, never erase an unadmitted candidate after NOSPC. */
    for (unsigned i = 0; i < 1500u; i++) {
        memset(data, (i & 1u) ? 0x31 : 0x32, 80u);
        assert(storage_object_write(STORAGE_OBJECT_INBOX, first, data, 80u) == STORAGE_RECORD_OK);
        if (i % 25u == 0u) {
            reopen();
            (void)check_usage();
            verify(first, 80u, data[0]);
            verify(last, 514u, 0x31);
        }
    }
    reopen();
    memcpy(baseline, media, sizeof(media));
    memset(data, 0x62, sizeof(data));
    operations = 0;
    assert(storage_object_write(STORAGE_OBJECT_CONTACT, first, data, sizeof(data)) == STORAGE_RECORD_OK);
    unsigned writes = operations;
    for (unsigned point = 1; point <= writes; point++) {
        for (tear = 0; tear < 3u; tear++) {
            for (unsigned mode = 0; mode < 2u; mode++) {
                storage_lfs_deinit();
                memcpy(media, baseline, sizeof(media));
                reopen();
                operations = 0;
                cut_at = point;
                return_error = mode != 0u;
                if (setjmp(cut) == 0) {
                    storage_record_result_t rc = storage_object_write(STORAGE_OBJECT_CONTACT,
                        first, data, sizeof(data));
                    assert(return_error && rc == STORAGE_RECORD_ERROR);
                }
                cut_at = 0;
                return_error = false;
                reopen();
                (void)check_usage();
                verify(first, 80u, 0x31);
                verify(last, 514u, 0x31);
                size_t len;
                storage_record_result_t rc = storage_object_read(STORAGE_OBJECT_CONTACT,
                    first, readback, sizeof(readback), &len);
                assert(rc == STORAGE_RECORD_NOT_FOUND ||
                       (rc == STORAGE_RECORD_OK && len == sizeof(data) &&
                        memcmp(data, readback, len) == 0));
                assert(storage_object_write(STORAGE_OBJECT_CONTACT, first,
                                            data, sizeof(data)) == STORAGE_RECORD_OK);
            }
        }
    }
    printf("quota boundary: 1500 inline updates, %u contact write failures with full inbox passed\n",
           writes * 6u);
}

static void test_initialization_cuts(void) {
    fresh();
    /* A mounted volume without an object allocator or directories. */
    storage_lfs_deinit();
    memset(media, 0xff, sizeof(media));
    assert(storage_lfs_init(&backend, &hal) == STORAGE_RECORD_OK);
    memcpy(baseline, media, sizeof(media));
    operations = 0;
    assert(storage_objects_open() == STORAGE_RECORD_OK);
    unsigned count = operations;
    assert(count > 0u);
    for (unsigned point = 1; point <= count; point++) {
        for (tear = 0; tear < 3; tear++) {
            storage_lfs_deinit();
            memcpy(media, baseline, sizeof(media));
            assert(storage_lfs_init(&backend, &hal) == STORAGE_RECORD_OK);
            operations = 0;
            cut_at = point;
            if (setjmp(cut) == 0) {
                (void)storage_objects_open();
                assert(!"expected interrupted initialization");
            }
            cut_at = 0;
            reopen();
            uint32_t id;
            assert(storage_object_allocate(&id) == STORAGE_RECORD_OK && id == 1u);
            assert(storage_object_write(STORAGE_OBJECT_CONTACT, id, data, 80u) == STORAGE_RECORD_OK);
        }
    }
    printf("object initialization: %u torn-write cases passed\n", count * 3u);
}

static void test_delete_cuts(void) {
    fresh();
    uint32_t id, neighbor;
    memset(data, 0x66, sizeof(data));
    assert(storage_object_allocate(&id) == STORAGE_RECORD_OK);
    assert(storage_object_allocate(&neighbor) == STORAGE_RECORD_OK);
    assert(storage_object_write(STORAGE_OBJECT_INBOX, id, data, sizeof(data)) == STORAGE_RECORD_OK);
    assert(storage_object_write(STORAGE_OBJECT_INBOX, neighbor, data, 80u) == STORAGE_RECORD_OK);
    memcpy(baseline, media, sizeof(media));
    operations = 0;
    assert(storage_object_remove(STORAGE_OBJECT_INBOX, id) == STORAGE_RECORD_OK);
    unsigned count = operations;
    for (unsigned point = 1; point <= count; point++) {
        for (tear = 0; tear < 3; tear++) {
            storage_lfs_deinit();
            memcpy(media, baseline, sizeof(media));
            reopen();
            operations = 0;
            cut_at = point;
            if (setjmp(cut) == 0) {
                (void)storage_object_remove(STORAGE_OBJECT_INBOX, id);
                assert(!"expected interrupted deletion");
            }
            cut_at = 0;
            reopen();
            size_t len;
            storage_record_result_t rc = storage_object_read(STORAGE_OBJECT_INBOX,
                id, readback, sizeof(readback), &len);
            assert(rc == STORAGE_RECORD_NOT_FOUND || rc == STORAGE_RECORD_OK);
            if (rc == STORAGE_RECORD_OK) verify(id, sizeof(data), 0x66);
            verify(neighbor, 80u, 0x66);
            uint32_t next;
            assert(storage_object_allocate(&next) == STORAGE_RECORD_OK && next > neighbor);
        }
    }
    printf("object deletion: %u torn-write cases passed\n", count * 3u);
}

static void test_payload_crc_does_not_invalidate_scan(void) {
    fresh();
    uint32_t bad, good, id;
    assert(storage_object_allocate(&bad) == STORAGE_RECORD_OK);
    /* Out-of-line bytes have the application envelope CRC, independently of
     * littlefs directory CRCs. Flip one data byte without harming metadata. */
    memset(data, 0x6a, sizeof(data));
    assert(storage_object_write(STORAGE_OBJECT_INBOX, bad, data, sizeof(data)) == STORAGE_RECORD_OK);
    assert(storage_object_allocate(&good) == STORAGE_RECORD_OK);
    assert(storage_object_write(STORAGE_OBJECT_INBOX, good, (const uint8_t *)"healthy", 7u) == STORAGE_RECORD_OK);
    storage_lfs_deinit();
    unsigned matches = 0;
    for (size_t off = 0; off + 1024u <= sizeof(media); off += 4096u) {
        if (memcmp(media + off + 20u, data, 1000u) == 0) {
            media[off + 100u] ^= 1u;
            matches++;
        }
    }
    assert(matches == 1u);
    reopen();
    memcpy(baseline, media, sizeof(media));
    operations = 0;
    assert(storage_object_scan_begin(STORAGE_OBJECT_INBOX) == STORAGE_RECORD_OK);
    assert(storage_object_scan_next(&id) == STORAGE_RECORD_OK && id == bad);
    size_t len;
    assert(storage_object_read(STORAGE_OBJECT_INBOX, bad, readback, sizeof(readback), &len) == STORAGE_RECORD_CORRUPT);
    assert(storage_object_scan_next(&id) == STORAGE_RECORD_OK && id == good);
    assert(storage_object_read(STORAGE_OBJECT_INBOX, good, readback, sizeof(readback), &len) == STORAGE_RECORD_OK);
    assert(len == 7u && memcmp(readback, "healthy", 7u) == 0);
    assert(storage_object_scan_next(&id) == STORAGE_RECORD_NOT_FOUND);
    storage_object_scan_end();
    assert(operations == 0 && memcmp(media, baseline, sizeof(media)) == 0);
}

static void test_busy_and_scan(void) {
    fresh();
    uint32_t id, next;
    assert(storage_object_allocate(&id) == STORAGE_RECORD_OK);
    memset(data, 0x77, sizeof(data));
    assert(storage_object_write(STORAGE_OBJECT_INBOX, id, data, sizeof(data)) == STORAGE_RECORD_OK);
    busy = true;
    assert(storage_object_allocate(&next) == STORAGE_RECORD_BUSY && next == 0u);
    busy = false;
    assert(storage_object_allocate(&next) == STORAGE_RECORD_OK && next > id);
    busy = true;
    assert(storage_object_write(STORAGE_OBJECT_INBOX, id, data, 80u) == STORAGE_RECORD_BUSY);
    busy = false;
    verify(id, sizeof(data), 0x77);
    assert(storage_object_scan_begin(STORAGE_OBJECT_INBOX) == STORAGE_RECORD_OK);
    assert(storage_object_scan_next(&next) == STORAGE_RECORD_OK && next == id);
    assert(storage_object_write(STORAGE_OBJECT_INBOX, id, data, 80u) == STORAGE_RECORD_ERROR);
    assert(storage_object_remove(STORAGE_OBJECT_INBOX, id) == STORAGE_RECORD_ERROR);
    io_error = true;
    size_t len;
    assert(storage_object_read(STORAGE_OBJECT_INBOX, id, readback, sizeof(readback), &len) == STORAGE_RECORD_ERROR);
    io_error = false;
    assert(storage_object_scan_next(&next) == STORAGE_RECORD_ERROR);
    /* A read remounts and invalidates the old iterator, never turns a failed
     * scan into an apparently complete empty directory. */
    verify(id, sizeof(data), 0x77);
    assert(storage_object_scan_next(&next) == STORAGE_RECORD_ERROR);
    assert(storage_object_scan_begin(STORAGE_OBJECT_INBOX) == STORAGE_RECORD_OK);
    assert(storage_object_scan_next(&next) == STORAGE_RECORD_OK && next == id);
    assert(storage_object_scan_next(&next) == STORAGE_RECORD_NOT_FOUND);
    storage_object_scan_end();
}
static void test_state(void) {
    fresh();
    uint32_t id, state;
    assert(storage_object_allocate(&id) == STORAGE_RECORD_OK);
    assert(storage_object_get_state(STORAGE_OBJECT_INBOX, id, &state) == STORAGE_RECORD_NOT_FOUND);
    assert(storage_object_set_state(STORAGE_OBJECT_INBOX, id, 1u) == STORAGE_RECORD_NOT_FOUND);
    memset(data, 0x55, sizeof(data));
    assert(storage_object_write(STORAGE_OBJECT_INBOX, id, data, sizeof(data)) == STORAGE_RECORD_OK);
    assert(storage_object_get_state(STORAGE_OBJECT_INBOX, id, &state) == STORAGE_RECORD_OK && state == 0u);
    assert(storage_object_set_state(STORAGE_OBJECT_INBOX, id, 7u) == STORAGE_RECORD_OK);
    unsigned before = operations;
    assert(storage_object_set_state(STORAGE_OBJECT_INBOX, id, 7u) == STORAGE_RECORD_OK && operations == before);
    assert(storage_object_scan_begin(STORAGE_OBJECT_INBOX) == STORAGE_RECORD_OK);
    assert(storage_object_set_state(STORAGE_OBJECT_INBOX, id, 8u) == STORAGE_RECORD_ERROR);
    storage_object_scan_end();
    busy = true;
    assert(storage_object_set_state(STORAGE_OBJECT_INBOX, id, 8u) == STORAGE_RECORD_BUSY);
    busy = false;
    assert(storage_object_write(STORAGE_OBJECT_INBOX, id, data, sizeof(data)) == STORAGE_RECORD_OK);
    reopen();
    assert(storage_object_get_state(STORAGE_OBJECT_INBOX, id, &state) == STORAGE_RECORD_OK && state == 7u);
    memcpy(baseline, media, sizeof(media));
    operations = 0u;
    assert(storage_object_set_state(STORAGE_OBJECT_INBOX, id, 0x12345678u) == STORAGE_RECORD_OK);
    unsigned count = operations;
    for (unsigned point = 1u; point <= count; point++) {
        for (tear = 0; tear < 3u; tear++) {
            storage_lfs_deinit();
            memcpy(media, baseline, sizeof(media));
            reopen();
            operations = 0u; cut_at = point;
            if (setjmp(cut) == 0) {
                (void)storage_object_set_state(STORAGE_OBJECT_INBOX, id, 0x12345678u);
                assert(false);
            }
            cut_at = 0u;
            reopen();
            assert(storage_object_get_state(STORAGE_OBJECT_INBOX, id, &state) == STORAGE_RECORD_OK);
            assert(state == 7u || state == 0x12345678u);
            verify(id, sizeof(data), 0x55);
        }
    }
}

int main(void) {
    test_state();
    test_basics();
    test_initialization_cuts();
    test_write_cuts(80u, sizeof(data), false);
    test_write_cuts(sizeof(data), 80u, false);
    test_write_cuts(80u, 0u, false);
    test_write_cuts(80u, 300u, false);
    test_write_cuts(80u, sizeof(data), true);
    test_write_cuts(sizeof(data), 80u, true);
    test_allocator_cuts(false);
    test_allocator_cuts(true);
    test_delete_cuts();
    test_busy_and_scan();
    test_payload_crc_does_not_invalidate_scan();
    test_accounting();
    test_category_isolation(180u);
    test_category_isolation(514u);
    test_quota_boundary();
    test_full(64u);
    test_full(200u);
    test_full(300u);
    test_full(2560u);
    storage_lfs_deinit();
    puts("PASS: storage objects");
}

#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

#include "storage/storage_lfs.h"

#define CAPACITY (128u * 1024u)
static uint8_t media[CAPACITY];
static uint8_t saved[CAPACITY];
static uint8_t interrupted[CAPACITY];
static uint8_t old_data[STORAGE_RECORD_MAX_PAYLOAD];
static uint8_t new_data[STORAGE_RECORD_MAX_PAYLOAD];
static uint8_t readback[STORAGE_RECORD_MAX_PAYLOAD];
static storage_backend_t backend;
static jmp_buf power_cut;
static unsigned ops, cut_at, tear_mode;
static unsigned recovery_cuts;
static unsigned erases[CAPACITY / 4096u];
static bool busy, io_error, return_error;

static nvm_status_t read_media(nvm_hal_t *hal, uint32_t off, void *dst, size_t len) {
    assert(off <= hal->capacity && len <= hal->capacity - off);
    if (io_error) {
        return NVM_STATUS_IO_ERROR;
    }
    memcpy(dst, media + off, len);
    return NVM_STATUS_OK;
}

static nvm_status_t change_media(nvm_hal_t *hal, uint32_t off, const void *src,
                                 size_t len, bool erase) {
    assert(off <= hal->capacity && len <= hal->capacity - off);
    assert(off % (erase ? 4096u : 256u) == 0u);
    assert(len == (erase ? 4096u : 256u));
    if (busy || io_error) {
        return busy ? NVM_STATUS_BUSY : NVM_STATUS_IO_ERROR;
    }
    ops++;
    bool cut = ops == cut_at;
    size_t written = cut ? (tear_mode == 0 ? 0 : tear_mode == 1 ? len / 2u : len) : len;
    if (erase) {
        erases[off / 4096u]++;
        memset(media + off, 0xff, written);
    } else {
        for (size_t i = 0; i < written; i++) {
            assert((media[off + i] & ((const uint8_t *)src)[i]) == ((const uint8_t *)src)[i]);
            media[off + i] &= ((const uint8_t *)src)[i];
        }
    }
    if (cut) {
        if (return_error) {
            return NVM_STATUS_IO_ERROR;
        }
        longjmp(power_cut, 1);
    }
    return NVM_STATUS_OK;
}

static nvm_status_t erase_media(nvm_hal_t *hal, uint32_t off, size_t len) {
    return change_media(hal, off, NULL, len, true);
}

static nvm_status_t prog_media(nvm_hal_t *hal, uint32_t off, const void *src, size_t len) {
    return change_media(hal, off, src, len, false);
}

static nvm_hal_t hal = {
    .name = "emulated-nor", .capacity = CAPACITY, .erase_block = 4096,
    .write_block = 256, .erase_required = true,
    .read = read_media, .erase = erase_media, .write = prog_media,
};

static void mount_ok(void) {
    assert(storage_lfs_init(&backend, &hal) == STORAGE_RECORD_OK);
}

static void expect_record(uint16_t id, const uint8_t *data, size_t length) {
    size_t len;
    storage_record_result_t rc = backend.read(&backend, id, readback, sizeof(readback), &len);
    if (rc != STORAGE_RECORD_OK) {
        fprintf(stderr, "read id=%u rc=%u expected_len=%zu ops=%u tear=%u\n", id, rc, length, ops, tear_mode);
    }
    assert(rc == STORAGE_RECORD_OK);
    assert(len == length && memcmp(readback, data, len) == 0);
}

static void expect_atomic_records(size_t old_len, size_t new_len) {
    size_t len;
    assert(backend.read(&backend, 7, readback, sizeof(readback), &len) == STORAGE_RECORD_OK);
    assert((len == old_len && memcmp(readback, old_data, len) == 0) ||
           (len == new_len && memcmp(readback, new_data, len) == 0));
    for (unsigned id = 0; id < 16; id++) {
        if (id != 7) {
            expect_record(id, old_data, old_len);
        }
    }
}

static void test_interrupted_recovery(size_t old_len, size_t new_len) {
    storage_lfs_deinit();
    memcpy(interrupted, media, sizeof(media));
    ops = 0;
    mount_ok();
    unsigned count = ops;
    expect_atomic_records(old_len, new_len);
    if (count != 0) {
        storage_lfs_deinit();
        memcpy(media, interrupted, sizeof(media));
        busy = true;
        assert(storage_lfs_init(&backend, &hal) == STORAGE_RECORD_BUSY);
        busy = false;
        mount_ok();
        expect_atomic_records(old_len, new_len);
    }
    for (unsigned point = 1; point <= count; point++) {
        for (unsigned mode = 0; mode < 3; mode++) {
            storage_lfs_deinit();
            memcpy(media, interrupted, sizeof(media));
            ops = 0;
            cut_at = point;
            tear_mode = mode;
            if (setjmp(power_cut) == 0) {
                mount_ok();
                assert(!"recovery fault point must be reached");
            }
            cut_at = 0;
            mount_ok();
            expect_atomic_records(old_len, new_len);
            recovery_cuts++;
        }
    }
}

static void test_atomic_update(size_t old_len, size_t new_len, bool fail_io) {
    memset(media, 0xff, sizeof(media));
    mount_ok();
    for (unsigned id = 0; id < 16; id++) {
        assert(backend.write(&backend, id, old_data, old_len) == STORAGE_RECORD_OK);
    }
    memcpy(saved, media, sizeof(media));
    mount_ok();
    ops = 0;
    assert(backend.write(&backend, 7, new_data, new_len) == STORAGE_RECORD_OK);
    unsigned count = ops;
    assert(count > 0);
    for (unsigned point = 1; point <= count; point++) {
        for (unsigned mode = 0; mode < 3; mode++) {
            storage_lfs_deinit();
            memcpy(media, saved, sizeof(media));
            cut_at = 0;
            mount_ok();
            cut_at = point;
            tear_mode = mode;
            return_error = fail_io;
            ops = 0;
            if (setjmp(power_cut) == 0) {
                storage_record_result_t rc = backend.write(&backend, 7, new_data, new_len);
                assert(fail_io && ops >= point && rc == STORAGE_RECORD_ERROR);
            }
            return_error = false;
            cut_at = 0;
            test_interrupted_recovery(old_len, new_len);
            assert(backend.write(&backend, 7, new_data, new_len) == STORAGE_RECORD_OK);
            mount_ok();
            expect_record(7, new_data, new_len);
        }
    }
    printf("atomic replacement %zu -> %zu bytes (%s): %u boundaries x 3 torn-write modes passed\n",
           old_len, new_len, fail_io ? "I/O error" : "power cut", count);
}

static void test_busy_and_failures(void) {
    busy = true;
    assert(backend.write(&backend, 7, old_data, sizeof(old_data)) == STORAGE_RECORD_BUSY);
    busy = false;
    expect_record(7, new_data, sizeof(new_data));
    assert(backend.write(&backend, 7, old_data, sizeof(old_data)) == STORAGE_RECORD_OK);
    io_error = true;
    assert(backend.write(&backend, 7, new_data, sizeof(new_data)) == STORAGE_RECORD_ERROR);
    io_error = false;
    assert(backend.write(&backend, 7, new_data, sizeof(new_data)) == STORAGE_RECORD_OK);
    mount_ok();
    expect_record(7, new_data, sizeof(new_data));
    assert(backend.write(&backend, 7, new_data, sizeof(new_data) + 1u) == STORAGE_RECORD_ERROR);
}

static void test_churn_and_full(void) {
    memset(erases, 0, sizeof(erases));
    for (unsigned i = 0; i < 1000; i++) {
        uint8_t small[128];
        memset(small, i & 255u, sizeof(small));
        assert(backend.write(&backend, 40, small, sizeof(small)) == STORAGE_RECORD_OK);
        if (i % 31u == 0) {
            mount_ok();
        }
        expect_record(40, small, sizeof(small));
    }
    unsigned touched = 0;
    for (unsigned i = 0; i < CAPACITY / 4096; i++) {
        touched += erases[i] != 0;
    }
    assert(touched > 2);
    expect_atomic_records(sizeof(old_data), sizeof(new_data));
    unsigned id;
    for (id = 50; id < 200; id++) {
        if (backend.write(&backend, id, new_data, sizeof(new_data)) != STORAGE_RECORD_OK) {
            break;
        }
    }
    assert(id < 200);
    mount_ok();
    expect_atomic_records(sizeof(old_data), sizeof(new_data));
    printf("1000 updates/remounts passed; %u blocks used at full\n", (unsigned)storage_lfs_used_blocks());
}

static void test_no_destructive_format(void) {
    storage_lfs_deinit();
    memset(media, 0xff, sizeof(media));
    media[70000] = 0;
    ops = 0;
    assert(storage_lfs_init(&backend, &hal) == STORAGE_RECORD_ERROR);
    assert(ops == 0 && media[70000] == 0);
    memset(media, 0xff, sizeof(media));
    mount_ok();
    assert(backend.write(&backend, 1, old_data, 96) == STORAGE_RECORD_OK);
    storage_lfs_deinit();
    memset(media, 0, 8192); /* both superblock copies destroyed */
    ops = 0;
    assert(storage_lfs_init(&backend, &hal) == STORAGE_RECORD_ERROR);
    assert(ops == 0);
}

static void test_interrupted_churn(void) {
    static uint8_t expected[16][STORAGE_RECORD_MAX_PAYLOAD];
    static size_t lengths[16];
    static uint8_t candidate[STORAGE_RECORD_MAX_PAYLOAD];
    static uint32_t random = 0x32103210u;
    static const size_t sizes[] = {0, 96, 128, 512, STORAGE_RECORD_MAX_PAYLOAD};
    storage_lfs_deinit();
    memset(media, 0xff, sizeof(media));
    mount_ok();
    for (unsigned id = 0; id < 16; id++) {
        lengths[id] = 96;
        memset(expected[id], id, lengths[id]);
        assert(backend.write(&backend, id, expected[id], lengths[id]) == STORAGE_RECORD_OK);
    }
    for (unsigned iteration = 0; iteration < 1000; iteration++) {
        random = random * 1664525u + 1013904223u;
        unsigned id = (random >> 16) % 16;
        size_t length = sizes[(random >> 24) % 5];
        for (size_t i = 0; i < length; i++) {
            candidate[i] = (uint8_t)(iteration + i * 7u);
        }
        mount_ok();
        memcpy(saved, media, sizeof(media));
        ops = 0;
        assert(backend.write(&backend, id, candidate, length) == STORAGE_RECORD_OK);
        unsigned count = ops;
        assert(count != 0);
        storage_lfs_deinit();
        memcpy(media, saved, sizeof(media));
        mount_ok();
        ops = 0;
        cut_at = random % count + 1;
        tear_mode = iteration % 3;
        if (setjmp(power_cut) == 0) {
            backend.write(&backend, id, candidate, length);
            assert(!"churn cut must execute");
        }
        cut_at = 0;
        mount_ok();
        size_t actual;
        assert(backend.read(&backend, id, readback, sizeof(readback), &actual) == STORAGE_RECORD_OK);
        if (actual == length && memcmp(readback, candidate, actual) == 0) {
            memcpy(expected[id], candidate, length);
            lengths[id] = length;
        } else {
            assert(actual == lengths[id] && memcmp(readback, expected[id], actual) == 0);
        }
        for (unsigned check = 0; check < 16; check++) {
            expect_record(check, expected[check], lengths[check]);
        }
    }
    puts("1000 mixed-size updates with power cuts and all-record readback passed");
}

int main(void) {
    for (size_t i = 0; i < sizeof(old_data); i++) {
        old_data[i] = (uint8_t)(i * 17u);
        new_data[i] = (uint8_t)(i * 31u + 7u);
    }
    for (unsigned fail_io = 0; fail_io < 2; fail_io++) {
        test_atomic_update(96, 128, fail_io);
        test_atomic_update(128, 96, fail_io);
        test_atomic_update(0, sizeof(new_data), fail_io);
        test_atomic_update(sizeof(old_data), 0, fail_io);
        test_atomic_update(sizeof(old_data), sizeof(new_data), fail_io);
    }
    assert(recovery_cuts > 0);
    printf("interrupted recovery: %u additional cuts passed\n", recovery_cuts);
    test_busy_and_failures();
    test_churn_and_full();
    test_interrupted_churn();
    test_no_destructive_format();
    storage_lfs_deinit();
    puts("storage_lfs tests passed");
    return 0;
}

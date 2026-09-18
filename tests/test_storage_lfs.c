#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

#include "storage/storage_lfs.h"

#define CAPACITY (128u * 1024u)
static uint8_t media[CAPACITY];
static uint8_t saved[CAPACITY];
static uint8_t old_data[STORAGE_RECORD_MAX_PAYLOAD];
static uint8_t new_data[STORAGE_RECORD_MAX_PAYLOAD];
static uint8_t readback[STORAGE_RECORD_MAX_PAYLOAD];
static storage_backend_t backend;
static jmp_buf power_cut;
static unsigned ops, cut_at, tear_mode;
static unsigned erases[CAPACITY / 4096u];
static bool busy, io_error;

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
    assert(backend.read(&backend, id, readback, sizeof(readback), &len) == STORAGE_RECORD_OK);
    assert(len == length && memcmp(readback, data, len) == 0);
}

static void test_atomic_update(void) {
    memset(media, 0xff, sizeof(media));
    mount_ok();
    for (unsigned id = 0; id < 16; id++) {
        assert(backend.write(&backend, id, old_data, sizeof(old_data)) == STORAGE_RECORD_OK);
    }
    memcpy(saved, media, sizeof(media));
    ops = 0;
    assert(backend.write(&backend, 7, new_data, sizeof(new_data)) == STORAGE_RECORD_OK);
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
            ops = 0;
            if (setjmp(power_cut) == 0) {
                backend.write(&backend, 7, new_data, sizeof(new_data));
                assert(!"fault point must be reached");
            }
            cut_at = 0;
            mount_ok();
            size_t len;
            assert(backend.read(&backend, 7, readback, sizeof(readback), &len) == STORAGE_RECORD_OK);
            assert(len == sizeof(old_data));
            assert(memcmp(readback, old_data, len) == 0 || memcmp(readback, new_data, len) == 0);
            for (unsigned id = 0; id < 16; id++) {
                if (id != 7) {
                    expect_record(id, old_data, sizeof(old_data));
                }
            }
            assert(backend.write(&backend, 7, new_data, sizeof(new_data)) == STORAGE_RECORD_OK);
            mount_ok();
            expect_record(7, new_data, sizeof(new_data));
        }
    }
    printf("atomic replacement: %u program/erase boundaries x 3 torn-write modes passed\n", count);
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
    unsigned id;
    for (id = 50; id < 200; id++) {
        if (backend.write(&backend, id, new_data, sizeof(new_data)) != STORAGE_RECORD_OK) {
            break;
        }
    }
    assert(id < 200);
    mount_ok();
    expect_record(0, old_data, sizeof(old_data));
    expect_record(7, new_data, sizeof(new_data));
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

int main(void) {
    for (size_t i = 0; i < sizeof(old_data); i++) {
        old_data[i] = (uint8_t)(i * 17u);
        new_data[i] = (uint8_t)(i * 31u + 7u);
    }
    test_atomic_update();
    test_busy_and_failures();
    test_churn_and_full();
    test_no_destructive_format();
    storage_lfs_deinit();
    puts("storage_lfs tests passed");
    return 0;
}

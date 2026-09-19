#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

#include "storage/storage_layout.h"
#include "storage/storage_lfs.h"
#include "storage/storage_partitions.h"

static uint8_t media[STORAGE_RESERVED_BYTES], saved[STORAGE_RESERVED_BYTES];
static uint8_t intermediate[STORAGE_RESERVED_BYTES];
static uint8_t data[STORAGE_RECORD_MAX_PAYLOAD], readback[STORAGE_RECORD_MAX_PAYLOAD];
static storage_backend_t router, raw;
static unsigned operations, cut_at, mode, cut_cases;
static jmp_buf power_cut;
static bool busy;

static nvm_status_t read_media(nvm_hal_t *hal, uint32_t off, void *dst, size_t len) {
    assert(off <= hal->capacity && len <= hal->capacity - off);
    memcpy(dst, media + (uintptr_t)hal->ctx + off, len);
    return NVM_STATUS_OK;
}

static nvm_status_t change_media(nvm_hal_t *hal, uint32_t off, const void *src,
                                size_t len, bool erase) {
    assert(off <= hal->capacity && len <= hal->capacity - off);
    assert(off % (erase ? 4096u : 256u) == 0u);
    assert(len == (erase ? 4096u : 256u));
    if (busy) {
        return NVM_STATUS_BUSY;
    }
    bool cut = ++operations == cut_at;
    size_t amount = cut ? (mode == 0u ? 0u : mode == 1u ? len / 2u : len) : len;
    uint8_t *dst = media + (uintptr_t)hal->ctx + off;
    if (erase) {
        memset(dst, 0xff, amount);
    } else {
        for (size_t i = 0; i < amount; i++) {
            assert((dst[i] & ((const uint8_t *)src)[i]) == ((const uint8_t *)src)[i]);
            dst[i] &= ((const uint8_t *)src)[i];
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
static nvm_hal_t system_hal = {
    .capacity = STORAGE_SYSTEM_BYTES, .erase_block = 4096u, .write_block = 256u,
    .erase_required = true, .read = read_media, .erase = erase_media, .write = prog_media,
};
static nvm_hal_t user_hal = {
    .ctx = (void *)(uintptr_t)STORAGE_SYSTEM_BYTES, .capacity = STORAGE_USER_BYTES,
    .erase_block = 4096u, .write_block = 256u, .erase_required = true,
    .read = read_media, .erase = erase_media, .write = prog_media,
};
static nvm_hal_t stage1_hal;
static const uint16_t sizes[16] = {
    75, 52, 98, 228, 64, 187, 678, 1492, 1492, 8, 3824, 1120, 178, 47, 96, 128,
};

static void fill(unsigned id, size_t len) {
    for (size_t i = 0; i < len; i++) {
        data[i] = (uint8_t)(id * 19u + i * 37u);
    }
}

static void open_ok(void) {
    assert(storage_partitions_open(&router, &system_hal, &user_hal) == STORAGE_RECORD_OK);
}

static void verify_record(storage_backend_t *backend, uint16_t id,
                          const uint8_t *expected, size_t size) {
    size_t len;
    assert(backend->read(backend, id, readback, sizeof(readback), &len) == STORAGE_RECORD_OK);
    assert(len == size && memcmp(expected, readback, len) == 0);
}

static void verify_all(void) {
    for (unsigned i = 0; i < 16; i++) {
        fill(i, sizes[i]);
        verify_record(&router, (uint16_t)(0x3210u + i), data, sizes[i]);
    }
    verify_record(&router, 0xffffu, (const uint8_t *)"SIS\1", 4u);
    storage_partition_diag_t diag;
    storage_partitions_get_diag(&diag);
    assert(diag.ready && diag.split && diag.system_blocks == 16 && diag.user_blocks == 96);
}

static void seed(bool format_system) {
    storage_lfs_deinit();
    memset(media, 0xff, sizeof(media));
    /* Growing must not interpret the old journal's non-erased bytes as files. */
    memset(media + sizeof(media) - STORAGE_LEGACY_BYTES, 0xa5, STORAGE_LEGACY_BYTES);
    stage1_hal = user_hal;
    stage1_hal.capacity = STORAGE_RECORD_BYTES;
    assert(storage_lfs_init(&raw, &stage1_hal) == STORAGE_RECORD_OK);
    for (unsigned i = 0; i < 16u; i++) {
        fill(i, sizes[i]);
        assert(raw.write(&raw, (uint16_t)(0x3210u + i), data, sizes[i]) == STORAGE_RECORD_OK);
    }
    assert(raw.write(&raw, 0xffffu, (const uint8_t *)"SIS\1", 4u) == STORAGE_RECORD_OK);
    for (uint16_t id = 0xfff7u; id <= 0xfff9u; id++) {
        assert(raw.write(&raw, id, data, sizeof(data)) == STORAGE_RECORD_OK);
    }
    assert(raw.write(&raw, 0xfffeu, data, sizeof(data)) == STORAGE_RECORD_OK);
    if (format_system) {
        assert(storage_lfs_init_volume(&raw, &system_hal, STORAGE_LFS_SYSTEM,
                                      STORAGE_SYSTEM_BYTES) == STORAGE_RECORD_OK);
    }
    storage_lfs_deinit();
}

static void test_migration_cuts(void) {
    seed(true);
    memcpy(saved, media, sizeof(media));
    operations = 0u;
    open_ok();
    unsigned count = operations;
    verify_all();
    for (unsigned point = 1u; point <= count; point++) {
        for (unsigned tear = 0; tear < 3; tear++) {
            storage_lfs_deinit();
            memcpy(media, saved, sizeof(media));
            operations = 0;
            cut_at = point;
            mode = tear;
            if (setjmp(power_cut) == 0) {
                open_ok();
                assert(!"migration fault must be reached");
            }
            cut_at = 0;
            /* Interrupt the recovery itself at another program/erase boundary. */
            if (point % 11u == 0u && tear == 1u) {
                storage_lfs_deinit();
                memcpy(intermediate, media, sizeof(media));
                operations = 0;
                open_ok();
                unsigned recovery_ops = operations;
                storage_lfs_deinit();
                memcpy(media, intermediate, sizeof(media));
                operations = 0;
                cut_at = recovery_ops == 0u ? 0u : recovery_ops / 2u + 1u;
                if (setjmp(power_cut) == 0) {
                    open_ok();
                    assert(cut_at == 0u);
                }
                cut_at = 0;
            }
            open_ok();
            verify_all();
            cut_cases++;
        }
    }
    printf("partition migration: %u boundaries x 3 torn-write modes (%u cases) passed\n",
           count, cut_cases);
}

static void test_first_format_cuts(void) {
    seed(false);
    memcpy(saved, media, sizeof(media));
    operations = 0;
    assert(storage_lfs_init_volume(&raw, &system_hal, STORAGE_LFS_SYSTEM,
                                  STORAGE_SYSTEM_BYTES) == STORAGE_RECORD_OK);
    unsigned count = operations;
    unsigned refused = 0;
    for (unsigned point = 1u; point <= count; point++) {
        for (unsigned tear = 0; tear < 3; tear++) {
            storage_lfs_deinit();
            memcpy(media, saved, sizeof(media));
            operations = 0;
            cut_at = point;
            mode = tear;
            if (setjmp(power_cut) == 0) {
                open_ok();
                assert(!"first format cut must be reached");
            }
            cut_at = 0;
            storage_record_result_t rc = storage_partitions_open(&router, &system_hal, &user_hal);
            if (rc != STORAGE_RECORD_OK) {
                assert(rc == STORAGE_RECORD_ERROR);
                assert(memcmp(media + STORAGE_SYSTEM_BYTES, saved + STORAGE_SYSTEM_BYTES,
                              STORAGE_USER_BYTES) == 0);
                /* Explicit test-operator recovery, never an automatic policy:
                 * the source has been proven unchanged and new volume is unused. */
                storage_lfs_deinit();
                memset(media, 0xff, STORAGE_SYSTEM_BYTES);
                open_ok();
                refused++;
            }
            verify_all();
        }
    }
    printf("first format: %u cuts checked; %u failed closed with original source intact\n",
           count * 3u, refused);
}

static void test_authority(void) {
    seed(true);
    busy = true;
    assert(storage_partitions_open(&router, &system_hal, &user_hal) == STORAGE_RECORD_BUSY);
    busy = false;
    open_ok();
    verify_all();
    /* A subsequent scratch replacement survives every ordinary remount. */
    fill(9u, sizeof(data));
    assert(router.write(&router, 0xfffeu, data, sizeof(data)) == STORAGE_RECORD_OK);
    open_ok();
    verify_record(&router, 0xfffeu, data, sizeof(data));
    assert(storage_lfs_init_volume(&raw, &system_hal, STORAGE_LFS_SYSTEM,
                                  STORAGE_SYSTEM_BYTES) == STORAGE_RECORD_OK);
    assert(storage_lfs_remove(&raw, 0xffffu) == STORAGE_RECORD_OK);
    assert(storage_partitions_open(&router, &system_hal, &user_hal) == STORAGE_RECORD_ERROR);

    seed(true);
    open_ok();
    assert(storage_lfs_init_volume(&raw, &system_hal, STORAGE_LFS_SYSTEM,
                                  STORAGE_SYSTEM_BYTES) == STORAGE_RECORD_OK);
    assert(storage_lfs_remove(&raw, 0xfff0u) == STORAGE_RECORD_OK);
    assert(storage_partitions_open(&router, &system_hal, &user_hal) == STORAGE_RECORD_ERROR);

    seed(true);
    open_ok();
    storage_lfs_deinit();
    memset(media + STORAGE_SYSTEM_BYTES, 0xff, STORAGE_USER_BYTES);
    assert(storage_partitions_open(&router, &system_hal, &user_hal) == STORAGE_RECORD_ERROR);
}

static unsigned fill_volume(storage_backend_t *backend) {
    memset(data, 0x6a, sizeof(data));
    unsigned count = 0;
    for (; count < 256; count++) {
        storage_record_result_t rc = backend->write(backend, (uint16_t)(0x4000u + count),
                                                    data, sizeof(data));
        if (rc != STORAGE_RECORD_OK) {
            assert(rc == STORAGE_RECORD_FULL);
            break;
        }
    }
    assert(count > 0 && count < 256);
    for (unsigned i = 0; i < count; i++) {
        verify_record(backend, (uint16_t)(0x4000u + i), data, sizeof(data));
    }
    return count;
}

static void test_full_and_isolation(void) {
    seed(true);
    open_ok();
    unsigned user_count = fill_volume(&router);
    verify_all();
    /* User FULL must not block system commits or poison their cache state. */
    for (unsigned i = 0; i < 32; i++) {
        fill(i, 128u);
        assert(router.write(&router, 0x3210u, data, 128u) == STORAGE_RECORD_OK);
        verify_record(&router, 0x3210u, data, 128u);
    }
    open_ok();
    verify_record(&router, 0x3210u, data, 128u);
    assert(storage_lfs_init_volume(&raw, &system_hal, STORAGE_LFS_SYSTEM,
                                  STORAGE_SYSTEM_BYTES) == STORAGE_RECORD_OK);
    unsigned system_count = fill_volume(&raw);
    size_t previous_len;
    assert(router.read(&router, 0x3210u, readback, sizeof(readback), &previous_len) == STORAGE_RECORD_OK);
    uint8_t previous[128];
    assert(previous_len == sizeof(previous));
    memcpy(previous, readback, sizeof(previous));
    memset(data, 0xab, sizeof(data));
    storage_record_result_t rc = router.write(&router, 0x3210u, data, sizeof(data));
    assert(rc == STORAGE_RECORD_FULL);
    verify_record(&router, 0x3210u, previous, sizeof(previous));
    storage_partition_diag_t diag;
    storage_partitions_get_diag(&diag);
    printf("FULL preserved records: system %ld/%ld blocks (%u fill records), user %ld/%ld (%u)\n",
           (long)diag.system_used, (long)diag.system_blocks, system_count,
           (long)diag.user_used, (long)diag.user_blocks, user_count);
    open_ok();
    verify_record(&router, 0x3210u, previous, sizeof(previous));
    assert(storage_lfs_init_volume(&raw, &user_hal, STORAGE_LFS_USER,
                                  STORAGE_RECORD_BYTES) == STORAGE_RECORD_OK);
    for (unsigned i = 0; i < user_count; i++) {
        assert(storage_lfs_remove(&raw, (uint16_t)(0x4000u + i)) == STORAGE_RECORD_OK);
    }
    fill(99u, sizeof(data));
    assert(router.write(&router, 0x3216u, data, sizeof(data)) == STORAGE_RECORD_OK);
    verify_record(&router, 0x3216u, data, sizeof(data));
    assert(storage_lfs_init_volume(&raw, &system_hal, STORAGE_LFS_SYSTEM,
                                  STORAGE_SYSTEM_BYTES) == STORAGE_RECORD_OK);
    for (unsigned i = 0; i < system_count; i++) {
        assert(storage_lfs_remove(&raw, (uint16_t)(0x4000u + i)) == STORAGE_RECORD_OK);
    }
    assert(router.write(&router, 0x3210u, data, sizeof(data)) == STORAGE_RECORD_OK);
    open_ok();
    verify_record(&router, 0x3210u, data, sizeof(data));
    verify_record(&router, 0x3216u, data, sizeof(data));
}

static void test_split_update_cuts(void) {
    seed(true);
    open_ok();
    storage_lfs_deinit();
    memcpy(saved, media, sizeof(media));
    static const uint16_t ids[] = {0x3210u, 0x3216u};
    unsigned cases = 0u;
    for (unsigned volume = 0; volume < 2u; volume++) {
        uint16_t id = ids[volume];
        open_ok();
        fill(99u, sizeof(data));
        operations = 0u;
        assert(router.write(&router, id, data, sizeof(data)) == STORAGE_RECORD_OK);
        unsigned count = operations;
        for (unsigned point = 1u; point <= count; point++) {
            for (unsigned tear = 0; tear < 3u; tear++) {
                storage_lfs_deinit();
                memcpy(media, saved, sizeof(media));
                open_ok();
                fill(99u, sizeof(data));
                operations = 0u;
                cut_at = point;
                mode = tear;
                if (setjmp(power_cut) == 0) {
                    (void)router.write(&router, id, data, sizeof(data));
                    assert(!"update fault must be reached");
                }
                cut_at = 0u;
                storage_lfs_deinit();
                open_ok();
                for (unsigned i = 0; i < 16u; i++) {
                    size_t len;
                    uint16_t record = (uint16_t)(0x3210u + i);
                    assert(router.read(&router, record, readback, sizeof(readback), &len)
                           == STORAGE_RECORD_OK);
                    bool new_record = record == id && len == sizeof(data);
                    fill(new_record ? 99u : i, new_record ? sizeof(data) : sizes[i]);
                    assert(len == (new_record ? sizeof(data) : sizes[i]));
                    assert(memcmp(data, readback, len) == 0);
                }
                if (volume == 0u) {
                    assert(memcmp(media + STORAGE_SYSTEM_BYTES, saved + STORAGE_SYSTEM_BYTES,
                                  STORAGE_USER_BYTES) == 0);
                } else {
                    assert(memcmp(media, saved, STORAGE_SYSTEM_BYTES) == 0);
                }
                cases++;
            }
        }
        storage_lfs_deinit();
        memcpy(media, saved, sizeof(media));
    }
    printf("split updates: %u torn writes recovered; other volume byte-identical\n", cases);
}

static void test_fresh_staging(void) {
    storage_lfs_deinit();
    memset(media, 0xff, sizeof(media));
    open_ok();
    storage_partition_diag_t diag;
    storage_partitions_get_diag(&diag);
    assert(diag.ready && !diag.split && diag.user_blocks == 32);
    for (unsigned i = 0; i < 16u; i++) {
        fill(i, sizes[i]);
        assert(router.write(&router, (uint16_t)(0x3210u + i), data, sizes[i]) == STORAGE_RECORD_OK);
    }
    open_ok();
    storage_partitions_get_diag(&diag);
    assert(!diag.split);
    assert(router.write(&router, 0xffffu, (const uint8_t *)"SIS\1", 4u) == STORAGE_RECORD_OK);
    open_ok();
    verify_all();
}

int main(void) {
    test_migration_cuts();
    test_first_format_cuts();
    test_authority();
    test_full_and_isolation();
    test_split_update_cuts();
    test_fresh_staging();
    storage_lfs_deinit();
    puts("PASS: split storage partitions");
    return 0;
}

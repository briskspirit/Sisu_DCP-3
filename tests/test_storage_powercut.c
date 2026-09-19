#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

#include "diag/storage_powercut_test.h"
#include "storage/storage_lfs.h"

#define CAPACITY (128u * 1024u)
#define LEGACY_CRC 0x71329abdu
static uint8_t media[CAPACITY], saved[CAPACITY], recovering[CAPACITY];
static uint8_t data[STORAGE_RECORD_MAX_PAYLOAD];
static storage_backend_t backend;
static jmp_buf cut;
static unsigned operations, cut_at, tear_mode, cases, recovery_cases;
static bool fail_io;

static nvm_status_t read_media(nvm_hal_t *hal, uint32_t off, void *dst, size_t len) {
    assert(off <= hal->capacity && len <= hal->capacity - off);
    memcpy(dst, media + off, len);
    return NVM_STATUS_OK;
}

static nvm_status_t change_media(nvm_hal_t *hal, uint32_t off, const void *src,
                                size_t len, bool erase) {
    assert(off <= hal->capacity && len <= hal->capacity - off);
    assert(off % (erase ? 4096u : 256u) == 0u);
    assert(len == (erase ? 4096u : 256u));
    if (fail_io) {
        return NVM_STATUS_IO_ERROR;
    }
    bool interrupted = ++operations == cut_at;
    size_t changed = interrupted ? (tear_mode == 0 ? 0 : tear_mode == 1 ? len / 2 : len) : len;
    if (erase) {
        memset(media + off, 0xff, changed);
    } else {
        const uint8_t *bytes = src;
        for (size_t i = 0; i < changed; i++) {
            assert((media[off + i] & bytes[i]) == bytes[i]);
            media[off + i] &= bytes[i];
        }
    }
    if (interrupted) {
        longjmp(cut, 1);
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
    .capacity = CAPACITY, .erase_block = 4096u, .write_block = 256u,
    .erase_required = true, .read = read_media, .erase = erase_media, .write = prog_media,
};

static void mount(void) {
    assert(storage_lfs_init(&backend, &hal) == STORAGE_RECORD_OK);
}

static void open_ok(void) {
    mount();
    assert(powercut_open(&backend, LEGACY_CRC) == POWERCUT_OK);
}

static void cycle(void) {
    for (unsigned i = 0; i < 3u; i++) {
        assert(powercut_step() == POWERCUT_OK);
    }
}

static void seed(void) {
    storage_lfs_deinit();
    memset(media, 0xff, sizeof(media));
    mount();
    assert(powercut_open(&backend, LEGACY_CRC) == POWERCUT_UNARMED);
    for (unsigned id = 0; id < 16u; id++) {
        memset(data, id + 1u, sizeof(data));
        assert(backend.write(&backend, (uint16_t)(0x3210u + id), data,
                             id == 7u ? sizeof(data) : 128u) == STORAGE_RECORD_OK);
    }
    assert(backend.write(&backend, 0xffffu, (const uint8_t *)"SIS\x01", 4u) == STORAGE_RECORD_OK);
    assert(powercut_arm() == POWERCUT_OK);
}

static void test_recovery_cuts(void) {
    storage_lfs_deinit();
    memcpy(recovering, media, sizeof(media));
    operations = 0u;
    open_ok();
    unsigned count = operations;
    for (unsigned point = 1u; point <= count; point++) {
        for (unsigned mode = 0u; mode < 3u; mode++) {
            storage_lfs_deinit();
            memcpy(media, recovering, sizeof(media));
            operations = 0u;
            cut_at = point;
            tear_mode = mode;
            if (setjmp(cut) == 0) {
                open_ok();
                assert(!"recovery fault must be reached");
            }
            cut_at = 0u;
            open_ok();
            cycle();
            recovery_cases++;
        }
    }
}

static void test_power_cuts(void) {
    seed();
    for (unsigned pass = 0u; pass < 14u; pass++) {
        storage_lfs_deinit();
        memcpy(saved, media, sizeof(media));
        open_ok();
        powercut_status_t before;
        powercut_get_status(&before);
        operations = 0u;
        cycle();
        unsigned count = operations;
        for (unsigned point = 1u; point <= count; point++) {
            for (unsigned mode = 0u; mode < 3u; mode++) {
                storage_lfs_deinit();
                memcpy(media, saved, sizeof(media));
                open_ok();
                operations = 0u;
                cut_at = point;
                tear_mode = mode;
                if (setjmp(cut) == 0) {
                    cycle();
                    assert(!"transaction fault must be reached");
                }
                cut_at = 0u;
                if (point == count / 2u && mode == 1u) {
                    test_recovery_cuts();
                } else {
                    open_ok();
                    powercut_status_t after;
                    powercut_get_status(&after);
                    assert(after.completed == before.completed ||
                           after.completed == before.completed + 1u);
                    cycle();
                }
                cases++;
            }
        }
        storage_lfs_deinit();
        memcpy(media, saved, sizeof(media));
        open_ok();
        cycle();
    }
    printf("power-cut bench oracle: %u operation cuts, %u recovery cuts passed\n",
           cases, recovery_cases);
}

static void test_faults_and_pause(void) {
    seed();
    cycle();
    assert(powercut_step() == POWERCUT_OK); /* durable intent, old data */
    open_ok();
    powercut_status_t status;
    powercut_get_status(&status);
    assert(status.recovered_old == 1u);
    assert(powercut_step() == POWERCUT_OK);
    assert(powercut_step() == POWERCUT_OK); /* new data, no acknowledgment */
    open_ok();
    powercut_get_status(&status);
    assert(status.recovered_new == 1u);
    assert(powercut_pause() == POWERCUT_OK);
    unsigned previous = status.completed;
    open_ok();
    cycle();
    powercut_get_status(&status);
    assert(!status.armed && status.completed == previous);
    assert(powercut_arm() == POWERCUT_OK);
    cycle();
    fail_io = true;
    assert(powercut_step() == POWERCUT_IO);
    fail_io = false;
    unsigned ops_before = operations;
    assert(powercut_step() == POWERCUT_IO && powercut_arm() == POWERCUT_IO);
    assert(operations == ops_before); /* latched failure never retries writes */
    open_ok();

    assert(powercut_open(&backend, LEGACY_CRC + 1u) == POWERCUT_CONTROL);
    open_ok();
    memset(data, 0x5a, sizeof(data));
    assert(backend.write(&backend, POWERCUT_DATA_ID, data, 96u) == STORAGE_RECORD_OK);
    assert(powercut_open(&backend, LEGACY_CRC) == POWERCUT_PATTERN);

    seed();
    memset(data, 0x77, sizeof(data));
    assert(backend.write(&backend, 0x3210u, data, 128u) == STORAGE_RECORD_OK);
    assert(powercut_open(&backend, LEGACY_CRC) == POWERCUT_BASELINE);

    seed();
    assert(backend.write(&backend, POWERCUT_CONTROL_ID, data, 8u) == STORAGE_RECORD_OK);
    assert(powercut_open(&backend, LEGACY_CRC) == POWERCUT_CONTROL);

    storage_lfs_deinit();
    memset(media, 0xff, sizeof(media));
    mount();
    assert(powercut_open(&backend, LEGACY_CRC) == POWERCUT_UNARMED);
    operations = 0;
    assert(powercut_arm() == POWERCUT_BASELINE && operations == 0u);
}

int main(void) {
    test_power_cuts();
    test_faults_and_pause();
    storage_lfs_deinit();
    puts("PASS: storage power-cut diagnostic");
    return 0;
}

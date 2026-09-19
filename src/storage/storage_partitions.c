#include "storage/storage_partitions.h"

#include <string.h>

#include "storage/storage_layout.h"
#include "storage/storage_lfs.h"

#define AUTHORITY_ID 0xfff0u
#define IMPORT_ID 0xffffu
static const uint8_t IMPORT_DONE[] = {'S', 'I', 'S', 1};
static const uint8_t SYSTEM_READY[] = {'S', 'Y', 'S', 2};
static const uint8_t USER_READY[] = {'U', 'S', 'R', 2};
static const uint16_t SYSTEM_IDS[] = {
    0x3210, 0x3211, 0x3212, 0x3213, 0x3214, 0x3215, 0x321d, 0x321e, 0x321f,
};

typedef struct {
    storage_backend_t system;
    storage_backend_t user;
    bool ready;
    bool split;
} partitions_t;

static partitions_t s_partitions;
static uint8_t s_copy[STORAGE_RECORD_MAX_PAYLOAD];
static uint8_t s_verify[STORAGE_RECORD_MAX_PAYLOAD];

static bool system_id(uint16_t id) {
    for (unsigned i = 0; i < sizeof(SYSTEM_IDS) / sizeof(SYSTEM_IDS[0]); i++) {
        if (id == SYSTEM_IDS[i]) {
            return true;
        }
    }
    return id == IMPORT_ID;
}

static storage_backend_t *target(partitions_t *partitions, uint16_t id) {
    return partitions->split && system_id(id) ? &partitions->system : &partitions->user;
}

static storage_record_result_t read_record(storage_backend_t *backend, uint16_t id,
                                           uint8_t *dst, size_t cap, size_t *len) {
    partitions_t *partitions = backend->ctx;
    if (!partitions->ready) {
        return STORAGE_RECORD_ERROR;
    }
    storage_backend_t *volume = target(partitions, id);
    return volume->read(volume, id, dst, cap, len);
}

static storage_record_result_t write_record(storage_backend_t *backend, uint16_t id,
                                            const uint8_t *src, size_t len) {
    partitions_t *partitions = backend->ctx;
    if (!partitions->ready) {
        return STORAGE_RECORD_ERROR;
    }
    storage_backend_t *volume = target(partitions, id);
    return volume->write(volume, id, src, len);
}

static storage_record_result_t marker(storage_backend_t *backend, uint16_t id,
                                     const uint8_t expected[4], bool *present) {
    size_t len;
    storage_record_result_t rc = backend->read(backend, id, s_copy, sizeof(s_copy), &len);
    *present = rc == STORAGE_RECORD_OK;
    if (rc == STORAGE_RECORD_NOT_FOUND) {
        return STORAGE_RECORD_OK;
    }
    if (rc != STORAGE_RECORD_OK) {
        return rc;
    }
    return len == 4u && memcmp(s_copy, expected, 4u) == 0
        ? STORAGE_RECORD_OK : STORAGE_RECORD_ERROR;
}

static storage_record_result_t copy_record(uint16_t id) {
    partitions_t *p = &s_partitions;
    size_t len, verified;
    storage_record_result_t rc = p->user.read(&p->user, id, s_copy, sizeof(s_copy), &len);
    if (rc != STORAGE_RECORD_OK) {
        return rc == STORAGE_RECORD_NOT_FOUND ? STORAGE_RECORD_ERROR : rc;
    }
    rc = p->system.write(&p->system, id, s_copy, len);
    if (rc == STORAGE_RECORD_OK) {
        rc = p->system.read(&p->system, id, s_verify, sizeof(s_verify), &verified);
        if (rc == STORAGE_RECORD_OK &&
            (len != verified || memcmp(s_copy, s_verify, len) != 0)) {
            rc = STORAGE_RECORD_ERROR;
        }
    }
    return rc;
}

static storage_record_result_t remove_old(uint16_t id) {
    storage_record_result_t rc = storage_lfs_remove(&s_partitions.user, id);
    return rc == STORAGE_RECORD_NOT_FOUND ? STORAGE_RECORD_OK : rc;
}

static storage_record_result_t migrate(void) {
    partitions_t *p = &s_partitions;
    bool system_ready, user_ready;
    storage_record_result_t rc = marker(&p->system, AUTHORITY_ID, SYSTEM_READY, &system_ready);
    if (rc != STORAGE_RECORD_OK) {
        return rc;
    }
    rc = marker(&p->user, AUTHORITY_ID, USER_READY, &user_ready);
    if (rc != STORAGE_RECORD_OK) {
        return rc;
    }
    int32_t blocks = storage_lfs_volume_blocks(&p->user);
    if (blocks < 0) {
        return STORAGE_RECORD_ERROR;
    }
    if (system_ready) {
        bool imported;
        rc = marker(&p->system, IMPORT_ID, IMPORT_DONE, &imported);
        if (rc != STORAGE_RECORD_OK || !imported) {
            return rc == STORAGE_RECORD_OK ? STORAGE_RECORD_ERROR : rc;
        }
        if (!user_ready) {
            rc = marker(&p->user, IMPORT_ID, IMPORT_DONE, &imported);
            if (rc != STORAGE_RECORD_OK || !imported) {
                return rc == STORAGE_RECORD_OK ? STORAGE_RECORD_ERROR : rc;
            }
        }
    }
    if (!system_ready) {
        /* Once growth or the user marker exists, the legacy area is reclaimed.
         * Never import old system snapshots after losing the new authority. */
        if (user_ready || (uint32_t)blocks != STORAGE_RECORD_BYTES / 4096u) {
            return STORAGE_RECORD_ERROR;
        }
        bool imported;
        rc = marker(&p->user, IMPORT_ID, IMPORT_DONE, &imported);
        if (rc != STORAGE_RECORD_OK || !imported) {
            /* Stage-0/fresh device: store_service imports the A/B records (or
             * defaults), publishes SIS1, then reopens to complete the split. */
            return rc;
        }
        for (unsigned i = 0; i < sizeof(SYSTEM_IDS) / sizeof(SYSTEM_IDS[0]); i++) {
            rc = copy_record(SYSTEM_IDS[i]);
            if (rc != STORAGE_RECORD_OK) {
                return rc;
            }
        }
        rc = copy_record(IMPORT_ID);
        if (rc != STORAGE_RECORD_OK) {
            return rc;
        }
        rc = p->system.write(&p->system, AUTHORITY_ID, SYSTEM_READY, sizeof(SYSTEM_READY));
        if (rc != STORAGE_RECORD_OK) {
            return rc;
        }
    }

    /* SYS2 commits before growth can allocate over the old journal. A reset
     * resumes with either durable size, but never recopies stale settings. */
    rc = storage_lfs_grow(&p->user);
    if (rc != STORAGE_RECORD_OK) {
        return rc;
    }
    if (!user_ready) {
        /* Remove bench-only evidence once, before publishing USER_READY.
         * Later diagnostic use of fffe must survive ordinary remounts. */
        static const uint16_t bench_ids[] = {0xfff7, 0xfff8, 0xfff9, 0xfffe};
        for (unsigned i = 0; i < sizeof(bench_ids) / sizeof(bench_ids[0]); i++) {
            rc = remove_old(bench_ids[i]);
            if (rc != STORAGE_RECORD_OK) {
                return rc;
            }
        }
        rc = p->user.write(&p->user, AUTHORITY_ID, USER_READY, sizeof(USER_READY));
        if (rc != STORAGE_RECORD_OK) {
            return rc;
        }
    }
    p->split = true;
    for (unsigned i = 0; i < sizeof(SYSTEM_IDS) / sizeof(SYSTEM_IDS[0]); i++) {
        rc = remove_old(SYSTEM_IDS[i]);
        if (rc != STORAGE_RECORD_OK) {
            return rc;
        }
    }
    return remove_old(IMPORT_ID);
}

storage_record_result_t storage_partitions_open(storage_backend_t *backend,
                                                nvm_hal_t *system, nvm_hal_t *user) {
    partitions_t *p = &s_partitions;
    p->ready = false;
    p->split = false;
    if (backend == NULL || system == NULL || user == NULL ||
        system->capacity != STORAGE_SYSTEM_BYTES || user->capacity != STORAGE_USER_BYTES) {
        return STORAGE_RECORD_ERROR;
    }
    storage_record_result_t rc = storage_lfs_init_volume(
        &p->system, system, STORAGE_LFS_SYSTEM, STORAGE_SYSTEM_BYTES);
    if (rc == STORAGE_RECORD_OK) {
        rc = storage_lfs_init_volume(&p->user, user, STORAGE_LFS_USER, STORAGE_RECORD_BYTES);
    }
    if (rc == STORAGE_RECORD_OK) {
        rc = migrate();
    }
    if (rc == STORAGE_RECORD_OK) {
        p->ready = true;
        *backend = (storage_backend_t){.ctx = p, .read = read_record, .write = write_record};
    }
    return rc;
}

void storage_partitions_get_diag(storage_partition_diag_t *out) {
    partitions_t *p = &s_partitions;
    *out = (storage_partition_diag_t){
        .ready = p->ready, .split = p->split,
        .system_blocks = p->ready ? storage_lfs_volume_blocks(&p->system) : -1,
        .system_used = p->ready ? storage_lfs_volume_used(&p->system) : -1,
        .user_blocks = p->ready ? storage_lfs_volume_blocks(&p->user) : -1,
        .user_used = p->ready ? storage_lfs_volume_used(&p->user) : -1,
    };
}

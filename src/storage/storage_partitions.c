#include "storage/storage_partitions.h"

#include "storage/storage_layout.h"
#include "storage/storage_lfs.h"
#include "storage/storage_objects.h"

typedef struct {
    storage_backend_t system;
    storage_backend_t user;
    bool ready;
} partitions_t;

static partitions_t s_partitions;

static bool system_id(uint16_t id) {
    return (id >= 0x3210u && id <= 0x3215u) ||
           (id >= 0x321du && id <= 0x321fu);
}

static storage_backend_t *target(partitions_t *partitions, uint16_t id) {
    return system_id(id) ? &partitions->system : &partitions->user;
}

static storage_record_result_t read_record(storage_backend_t *backend, uint16_t id,
                                           uint8_t *dst, size_t cap, size_t *len) {
    partitions_t *p = backend->ctx;
    if (!p->ready) return STORAGE_RECORD_ERROR;
    storage_backend_t *volume = target(p, id);
    return volume->read(volume, id, dst, cap, len);
}

static storage_record_result_t write_record(storage_backend_t *backend, uint16_t id,
                                            const uint8_t *src, size_t len) {
    partitions_t *p = backend->ctx;
    if (!p->ready) return STORAGE_RECORD_ERROR;
    storage_backend_t *volume = target(p, id);
    return volume->write(volume, id, src, len);
}

storage_record_result_t storage_partitions_open(storage_backend_t *backend,
                                                nvm_hal_t *system, nvm_hal_t *user) {
    partitions_t *p = &s_partitions;
    p->ready = false;
    if (backend == NULL || system == NULL || user == NULL ||
        system->capacity != STORAGE_SYSTEM_BYTES || user->capacity != STORAGE_USER_BYTES) {
        return STORAGE_RECORD_ERROR;
    }
    storage_record_result_t rc = storage_lfs_init_volume(
        &p->system, system, STORAGE_LFS_SYSTEM, STORAGE_SYSTEM_BYTES);
    if (rc == STORAGE_RECORD_OK) {
        rc = storage_lfs_init_volume(&p->user, user, STORAGE_LFS_USER, STORAGE_USER_BYTES);
    }
    if (rc == STORAGE_RECORD_OK &&
        (storage_lfs_volume_blocks(&p->system) != STORAGE_SYSTEM_BYTES / 4096u ||
         storage_lfs_volume_blocks(&p->user) != STORAGE_USER_BYTES / 4096u)) {
        rc = STORAGE_RECORD_ERROR;
    }
    if (rc == STORAGE_RECORD_OK) rc = storage_objects_open();
    if (rc == STORAGE_RECORD_OK) {
        p->ready = true;
        *backend = (storage_backend_t){.ctx = p, .read = read_record, .write = write_record};
    }
    return rc;
}

void storage_partitions_get_diag(storage_partition_diag_t *out) {
    partitions_t *p = &s_partitions;
    *out = (storage_partition_diag_t){
        .ready = p->ready,
        .system_blocks = p->ready ? storage_lfs_volume_blocks(&p->system) : -1,
        .system_used = p->ready ? storage_lfs_volume_used(&p->system) : -1,
        .user_blocks = p->ready ? storage_lfs_volume_blocks(&p->user) : -1,
        .user_used = p->ready ? storage_lfs_volume_used(&p->user) : -1,
    };
}

#include "storage/storage_partitions.h"

#include "storage/storage_layout.h"
#include "storage/storage_lfs.h"
#include "storage/storage_objects.h"

typedef struct {
    storage_backend_t system;
    storage_backend_t user;
    bool ready;
    storage_record_result_t system_status, user_status, objects_status;
} partitions_t;

static partitions_t s_partitions;

static bool system_id(uint16_t id) {
    return (id >= 0x3210u && id <= 0x3215u) ||
           (id >= 0x321du && id <= 0x3220u);
}

static storage_backend_t *target(partitions_t *partitions, uint16_t id) {
    return system_id(id) ? &partitions->system : &partitions->user;
}

static storage_record_result_t read_record(storage_backend_t *backend, uint16_t id,
                                           uint8_t *dst, size_t cap, size_t *len) {
    partitions_t *p = backend->ctx;
    storage_record_result_t rc = system_id(id) ? p->system_status : p->user_status;
    if (rc != STORAGE_RECORD_OK) return rc;
    storage_backend_t *volume = target(p, id);
    return volume->read(volume, id, dst, cap, len);
}

static storage_record_result_t write_record(storage_backend_t *backend, uint16_t id,
                                            const uint8_t *src, size_t len) {
    partitions_t *p = backend->ctx;
    storage_record_result_t rc = system_id(id) ? p->system_status : p->user_status;
    if (rc != STORAGE_RECORD_OK) return rc;
    storage_backend_t *volume = target(p, id);
    return volume->write(volume, id, src, len);
}

storage_record_result_t storage_partitions_open(storage_backend_t *backend,
                                                nvm_hal_t *system, nvm_hal_t *user) {
    partitions_t *p = &s_partitions;
    *p = (partitions_t){.system_status=STORAGE_RECORD_ERROR,
        .user_status=STORAGE_RECORD_ERROR, .objects_status=STORAGE_RECORD_ERROR};
    if (backend == NULL) return STORAGE_RECORD_ERROR;
    storage_lfs_deinit();
    if (system != NULL && system->capacity == STORAGE_SYSTEM_BYTES)
        p->system_status = storage_lfs_init_volume(
            &p->system, system, STORAGE_LFS_SYSTEM, STORAGE_SYSTEM_BYTES);
    if (user != NULL && user->capacity == STORAGE_USER_BYTES)
        p->user_status = storage_lfs_init_volume(&p->user, user, STORAGE_LFS_USER, STORAGE_USER_BYTES);
    if (p->system_status == STORAGE_RECORD_OK &&
        storage_lfs_volume_blocks(&p->system) != STORAGE_SYSTEM_BYTES / 4096u)
        p->system_status = STORAGE_RECORD_ERROR;
    if (p->user_status == STORAGE_RECORD_OK &&
        storage_lfs_volume_blocks(&p->user) != STORAGE_USER_BYTES / 4096u)
        p->user_status = STORAGE_RECORD_ERROR;
    if (p->user_status == STORAGE_RECORD_OK) p->objects_status = storage_objects_open();
    p->ready = p->system_status == STORAGE_RECORD_OK &&
        p->user_status == STORAGE_RECORD_OK && p->objects_status == STORAGE_RECORD_OK;
    /* Publish the router even after a partial failure: healthy power-management
     * records must remain readable independently of the other volume. */
    *backend = (storage_backend_t){.ctx = p, .read = read_record, .write = write_record};
    return p->system_status != STORAGE_RECORD_OK ? p->system_status :
        p->user_status != STORAGE_RECORD_OK ? p->user_status : p->objects_status;
}

void storage_partitions_get_diag(storage_partition_diag_t *out) {
    partitions_t *p = &s_partitions;
    *out = (storage_partition_diag_t){
        .ready = p->ready,
        .system_ready = p->system_status == STORAGE_RECORD_OK,
        .user_ready = p->user_status == STORAGE_RECORD_OK,
        .system_status = p->system_status, .user_status = p->user_status,
        .objects_status = p->objects_status,
        .system_blocks = p->system_status == STORAGE_RECORD_OK ? storage_lfs_volume_blocks(&p->system) : -1,
        .system_used = p->system_status == STORAGE_RECORD_OK ? storage_lfs_volume_used(&p->system) : -1,
        .user_blocks = p->user_status == STORAGE_RECORD_OK ? storage_lfs_volume_blocks(&p->user) : -1,
        .user_used = p->user_status == STORAGE_RECORD_OK ? storage_lfs_volume_used(&p->user) : -1,
    };
}

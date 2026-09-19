#ifndef STORAGE_LFS_H
#define STORAGE_LFS_H

#include "storage/nvm_hal.h"
#include "storage/storage_backend.h"

typedef enum {
    STORAGE_LFS_SYSTEM = 0,
    STORAGE_LFS_USER,
    STORAGE_LFS_VOLUME_COUNT,
} storage_lfs_volume_t;

/* Single core0 owner, separate static caches for each volume. No IRQ calls.
 * initial_bytes is the first-use format extent, not an automatic growth request.
 * Only a completely erased extent is formatted; corrupt media is never erased. */
storage_record_result_t storage_lfs_init_volume(storage_backend_t *backend,
    nvm_hal_t *hal, storage_lfs_volume_t volume, uint32_t initial_bytes);
storage_record_result_t storage_lfs_grow(storage_backend_t *backend);
storage_record_result_t storage_lfs_remove(storage_backend_t *backend, uint16_t id);
int32_t storage_lfs_volume_blocks(storage_backend_t *backend);
int32_t storage_lfs_volume_used(storage_backend_t *backend);

/* Single-volume compatibility entry for host tests and the stage-1 bench. */
storage_record_result_t storage_lfs_init(storage_backend_t *backend, nvm_hal_t *hal);
void storage_lfs_deinit(void);
int32_t storage_lfs_used_blocks(void);

#endif

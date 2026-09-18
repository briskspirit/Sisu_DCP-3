#ifndef STORAGE_LFS_H
#define STORAGE_LFS_H

#include "storage/nvm_hal.h"
#include "storage/storage_backend.h"

/* Single core0 owner, no calls from IRQs. Only completely erased media is
 * formatted automatically; corrupt or incompatible media is never erased. */
storage_record_result_t storage_lfs_init(storage_backend_t *backend, nvm_hal_t *hal);
void storage_lfs_deinit(void);
int32_t storage_lfs_used_blocks(void);

#endif

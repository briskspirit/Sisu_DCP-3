#ifndef STORAGE_PARTITIONS_H
#define STORAGE_PARTITIONS_H

#include "storage/nvm_hal.h"
#include "storage/storage_backend.h"

typedef struct {
    bool ready;
    bool system_ready;
    bool user_ready;
    storage_record_result_t system_status;
    storage_record_result_t user_status;
    storage_record_result_t objects_status;
    int32_t system_blocks;
    int32_t system_used;
    int32_t user_blocks;
    int32_t user_used;
} storage_partition_diag_t;

/* Record affinity belongs to storage composition, not
 * domain codecs or the media HAL. Both HAL objects must outlive this backend. */
storage_record_result_t storage_partitions_open(storage_backend_t *backend,
                                                nvm_hal_t *system, nvm_hal_t *user);
void storage_partitions_get_diag(storage_partition_diag_t *out);

#endif

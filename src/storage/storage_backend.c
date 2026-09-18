#include "storage/storage_backend.h"

#include "storage/storage_lfs.h"

storage_record_result_t storage_backend_open(storage_backend_t *backend) {
    static nvm_hal_t hal;
    if (nvm_record_flash_hal_init(&hal) != NVM_STATUS_OK) {
        return STORAGE_RECORD_ERROR;
    }
    return storage_lfs_init(backend, &hal);
}

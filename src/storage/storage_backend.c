#include "storage/storage_backend.h"

#include "storage/storage_partitions.h"

storage_record_result_t storage_backend_open(storage_backend_t *backend) {
    static nvm_hal_t system, user;
    if (nvm_system_flash_hal_init(&system) != NVM_STATUS_OK ||
        nvm_user_flash_hal_init(&user) != NVM_STATUS_OK) {
        return STORAGE_RECORD_ERROR;
    }
    return storage_partitions_open(backend, &system, &user);
}

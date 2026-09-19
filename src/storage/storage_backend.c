#include "storage/storage_backend.h"

#include "storage/storage_partitions.h"

storage_record_result_t storage_backend_open(storage_backend_t *backend) {
    static nvm_hal_t system, user;
    bool system_ok = nvm_system_flash_hal_init(&system) == NVM_STATUS_OK;
    bool user_ok = nvm_user_flash_hal_init(&user) == NVM_STATUS_OK;
    return storage_partitions_open(backend, system_ok ? &system : NULL,
                                    user_ok ? &user : NULL);
}

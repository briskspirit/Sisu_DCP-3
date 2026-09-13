#ifndef NVM_HAL_H
#define NVM_HAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    NVM_STATUS_OK = 0,
    NVM_STATUS_INVALID_ARGUMENT,
    NVM_STATUS_OUT_OF_RANGE,
    NVM_STATUS_ALIGNMENT,
    NVM_STATUS_NOT_PRESENT,
    NVM_STATUS_IO_ERROR,
    NVM_STATUS_BUSY,
} nvm_status_t;

typedef struct nvm_hal nvm_hal_t;

typedef nvm_status_t (*nvm_read_fn)(nvm_hal_t *hal, uint32_t offset, void *dst, size_t len);
typedef nvm_status_t (*nvm_erase_fn)(nvm_hal_t *hal, uint32_t offset, size_t len);
typedef nvm_status_t (*nvm_write_fn)(nvm_hal_t *hal, uint32_t offset, const void *src, size_t len);

struct nvm_hal {
    const char *name;
    void *ctx;
    uint32_t capacity;
    uint32_t erase_block;
    uint32_t write_block;
    bool erase_required;
    nvm_read_fn read;
    nvm_erase_fn erase;
    nvm_write_fn write;
};

nvm_status_t nvm_flash_hal_init(nvm_hal_t *hal);

#endif

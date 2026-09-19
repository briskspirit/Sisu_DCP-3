#include "storage/nvm_hal.h"
#include "storage/storage_layout.h"

#include <string.h>

#include "hardware/flash.h"
#include "hardware/regs/addressmap.h"
#include "hardware/sync.h"
#include "hardware/watchdog.h"
#include "pico/error.h"
#include "pico/stdlib.h"
#include "services/runtime_watchdog.h"

/* Consecutive core1-park failures before we reboot rather than retry forever. Each
 * failure costs core1_flash_pause_begin()'s ~100 ms bound, so ~20 => ~2 s of pinned
 * retrying before recovery -- long enough to ride out a transient, short of a hang. */
#ifndef NVM_FLASH_PARK_FAIL_REBOOT
#define NVM_FLASH_PARK_FAIL_REBOOT 20u
#endif

#if PICO_FLASH_SIZE_BYTES != (2u * 1024u * 1024u)
#error "Sisu DCP-3 storage expects the RP2354B 2 MB flash map; rebuild with PICO_BOARD=sisu_sse_revb2"
#endif

#if (STORAGE_RESERVED_BYTES % FLASH_SECTOR_SIZE) != 0
#error "Storage must be flash-sector aligned"
#endif

#include "services/core1_services.h"

/* Run a flash erase/program with BOTH cores kept out of XIP for the duration:
 * core1 is parked in a RAM spin (IRQs off) by core1_flash_pause_begin(), and we
 * disable core0's IRQs here, then call the op (flash_range_* are RAM-resident).
 *
 * We deliberately do NOT use the SDK's flash_safe_execute(): its multicore
 * lockout relies on core1 servicing its inter-core FIFO IRQ, which the audio-
 * resident core1 does not reliably acknowledge -- commits timed out and retried
 * forever, hanging the phone. Parking core1 ourselves + disabling core0 IRQs is
 * exactly what flash_safe_execute reduces to once the other core is handled,
 * with no dependency on an SDK config macro reaching the pico_flash library. */
static int nvm_safe_exec(void (*fn)(void *), void *op) {
    static uint32_t s_park_fail_streak;
    int rc;
    if (core1_flash_pause_begin()) {
        s_park_fail_streak = 0u;
        /* Short lease is the ultimate backstop if erase/program or the core1
         * unpark path stalls. The runtime owner restores its main-loop lease;
         * flash must never leave the watchdog disabled. */
        runtime_watchdog_flash_begin();
        uint32_t irq = save_and_disable_interrupts();
        fn(op);                  /* flash_range_erase / flash_range_program (RAM-resident) */
        restore_interrupts(irq);
        runtime_watchdog_flash_checkpoint();
        core1_flash_pause_end();
        runtime_watchdog_flash_end();
        rc = PICO_OK;
    } else {
        /* core1 could not confirm parked within core1_flash_pause_begin()'s bound
         * (e.g. wedged in an I2S teardown). The store layer retries the dirty unit
         * every tick, and retrying FOREVER pins core0 at ~100 ms/tick -- this was the
         * back half of the remote-hangup freeze (the bounded DMA-abort in the I2S
         * HALs is the front half). Bounded here too: after a streak of failures,
         * reboot instead of live-locking. Transaction recovery belongs to the
         * storage backend; this HAL must not proceed without the park handshake. */
        if (++s_park_fail_streak >= NVM_FLASH_PARK_FAIL_REBOOT) {
            watchdog_reboot(0, 0, 0);
        }
        rc = PICO_ERROR_TIMEOUT; /* core1 not parked -> skip; retry */
    }
    return rc;
}

typedef enum {
    FLASH_OP_ERASE = 0,
    FLASH_OP_PROGRAM,
} flash_op_kind_t;

typedef struct {
    flash_op_kind_t kind;
    uint32_t flash_offset;
    const uint8_t *data;
    size_t len;
} flash_op_t;

typedef struct {
    uint32_t flash_offset;
} flash_hal_ctx_t;

static nvm_status_t flash_read(nvm_hal_t *hal, uint32_t offset, void *dst, size_t len);
static nvm_status_t flash_erase(nvm_hal_t *hal, uint32_t offset, size_t len);
static nvm_status_t flash_write(nvm_hal_t *hal, uint32_t offset, const void *src, size_t len);
static void flash_op_execute(void *param);
static bool valid_range(const nvm_hal_t *hal, uint32_t offset, size_t len);

static flash_hal_ctx_t s_flash_ctx[4];

static nvm_status_t init_region(nvm_hal_t *hal, unsigned region,
                                 uint32_t offset, uint32_t capacity) {
    if (hal == 0 || offset > PICO_FLASH_SIZE_BYTES ||
        capacity > PICO_FLASH_SIZE_BYTES - offset) {
        return NVM_STATUS_INVALID_ARGUMENT;
    }
    memset(hal, 0, sizeof(*hal));
    s_flash_ctx[region].flash_offset = offset;

    hal->name = "internal-flash";
    hal->ctx = &s_flash_ctx[region];
    hal->capacity = capacity;
    hal->erase_block = FLASH_SECTOR_SIZE;
    hal->write_block = FLASH_PAGE_SIZE;
    hal->erase_required = true;
    hal->read = flash_read;
    hal->erase = flash_erase;
    hal->write = flash_write;
    return NVM_STATUS_OK;
}

nvm_status_t nvm_flash_hal_init(nvm_hal_t *hal) {
    return init_region(hal, 0u, PICO_FLASH_SIZE_BYTES - STORAGE_LEGACY_BYTES,
                       STORAGE_LEGACY_BYTES);
}

nvm_status_t nvm_record_flash_hal_init(nvm_hal_t *hal) {
    /* Historical stage-1 view, retained for the pre-split power-cut bench. */
    return init_region(hal, 1u, PICO_FLASH_SIZE_BYTES - STORAGE_LEGACY_BYTES - STORAGE_RECORD_BYTES,
                       STORAGE_RECORD_BYTES);
}

nvm_status_t nvm_system_flash_hal_init(nvm_hal_t *hal) {
    return init_region(hal, 2u, PICO_FLASH_SIZE_BYTES - STORAGE_RESERVED_BYTES,
                       STORAGE_SYSTEM_BYTES);
}

nvm_status_t nvm_user_flash_hal_init(nvm_hal_t *hal) {
    return init_region(hal, 3u, PICO_FLASH_SIZE_BYTES - STORAGE_USER_BYTES,
                       STORAGE_USER_BYTES);
}

static nvm_status_t flash_read(nvm_hal_t *hal, uint32_t offset, void *dst, size_t len) {
    if (hal == 0 || dst == 0) {
        return NVM_STATUS_INVALID_ARGUMENT;
    }
    if (!valid_range(hal, offset, len)) {
        return NVM_STATUS_OUT_OF_RANGE;
    }
    const flash_hal_ctx_t *ctx = (const flash_hal_ctx_t *)hal->ctx;
    const uint8_t *src = (const uint8_t *)(XIP_BASE + ctx->flash_offset + offset);
    memcpy(dst, src, len);
    return NVM_STATUS_OK;
}

static nvm_status_t flash_erase(nvm_hal_t *hal, uint32_t offset, size_t len) {
    if (hal == 0) {
        return NVM_STATUS_INVALID_ARGUMENT;
    }
    if (!valid_range(hal, offset, len)) {
        return NVM_STATUS_OUT_OF_RANGE;
    }
    if ((offset % FLASH_SECTOR_SIZE) != 0 || (len % FLASH_SECTOR_SIZE) != 0) {
        return NVM_STATUS_ALIGNMENT;
    }
    const flash_hal_ctx_t *ctx = (const flash_hal_ctx_t *)hal->ctx;
    flash_op_t op = {
        .kind = FLASH_OP_ERASE,
        .flash_offset = ctx->flash_offset + offset,
        .data = 0,
        .len = FLASH_SECTOR_SIZE,
    };
    for (size_t done = 0; done < len; done += FLASH_SECTOR_SIZE) {
        op.flash_offset = ctx->flash_offset + offset + (uint32_t)done;
        int rc = nvm_safe_exec(flash_op_execute, &op);
        if (rc != PICO_OK) {
            return rc == PICO_ERROR_TIMEOUT ? NVM_STATUS_BUSY : NVM_STATUS_IO_ERROR;
        }
    }
    return NVM_STATUS_OK;
}

static nvm_status_t flash_write(nvm_hal_t *hal, uint32_t offset, const void *src, size_t len) {
    if (hal == 0 || src == 0) {
        return NVM_STATUS_INVALID_ARGUMENT;
    }
    if (!valid_range(hal, offset, len)) {
        return NVM_STATUS_OUT_OF_RANGE;
    }
    if ((offset % FLASH_PAGE_SIZE) != 0 || (len % FLASH_PAGE_SIZE) != 0) {
        return NVM_STATUS_ALIGNMENT;
    }
    const flash_hal_ctx_t *ctx = (const flash_hal_ctx_t *)hal->ctx;
    /* Never hand an XIP-resident caller buffer to the ROM while XIP is down.
     * One page per critical section also bounds IRQ/watchdog blackout. */
    uint8_t page[FLASH_PAGE_SIZE];
    flash_op_t op = {
        .kind = FLASH_OP_PROGRAM,
        .flash_offset = ctx->flash_offset + offset,
        .data = page,
        .len = sizeof(page),
    };
    for (size_t done = 0; done < len; done += sizeof(page)) {
        memcpy(page, (const uint8_t *)src + done, sizeof(page));
        op.flash_offset = ctx->flash_offset + offset + (uint32_t)done;
        int rc = nvm_safe_exec(flash_op_execute, &op);
        if (rc != PICO_OK) {
            return rc == PICO_ERROR_TIMEOUT ? NVM_STATUS_BUSY : NVM_STATUS_IO_ERROR;
        }
    }
    return NVM_STATUS_OK;
}

static void flash_op_execute(void *param) {
    flash_op_t *op = (flash_op_t *)param;
    if (op->kind == FLASH_OP_ERASE) {
        flash_range_erase(op->flash_offset, op->len);
    } else {
        flash_range_program(op->flash_offset, op->data, op->len);
    }
}

static bool valid_range(const nvm_hal_t *hal, uint32_t offset, size_t len) {
    if (offset > hal->capacity) {
        return false;
    }
    return len <= ((size_t)hal->capacity - offset);
}

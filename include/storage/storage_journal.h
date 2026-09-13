#ifndef STORAGE_JOURNAL_H
#define STORAGE_JOURNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "storage/nvm_hal.h"

#define STORAGE_JOURNAL_SLOT_SIZE 4096u
#define STORAGE_JOURNAL_HEADER_SIZE 32u
#define STORAGE_JOURNAL_MAX_PAYLOAD (STORAGE_JOURNAL_SLOT_SIZE - STORAGE_JOURNAL_HEADER_SIZE)

typedef struct {
    nvm_hal_t *hal;
    uint32_t base_offset;
    uint32_t slot_size;
    uint16_t unit_id;
} storage_journal_t;

typedef struct {
    uint32_t sequence;
    uint32_t payload_len;
} storage_journal_info_t;

typedef enum {
    STORAGE_JOURNAL_WRITE_OK = 0,
    STORAGE_JOURNAL_WRITE_ERROR,
    STORAGE_JOURNAL_WRITE_BUSY,
} storage_journal_write_result_t;

bool storage_journal_init(storage_journal_t *journal,
                          nvm_hal_t *hal,
                          uint16_t unit_id,
                          uint32_t base_offset,
                          uint32_t slot_size);
bool storage_journal_read_latest(const storage_journal_t *journal,
                                 uint8_t *dst,
                                 size_t dst_cap,
                                 size_t *out_len,
                                 storage_journal_info_t *out_info);
/* Detailed form preserves a transient backend-busy origin across the journal
 * layer. The bool wrapper remains for callers that only need success/failure. */
storage_journal_write_result_t storage_journal_write_detailed(
    storage_journal_t *journal,
    const uint8_t *payload,
    size_t payload_len);
bool storage_journal_write(storage_journal_t *journal, const uint8_t *payload, size_t payload_len);

#endif

#include "storage/storage_journal.h"

#include "storage_bytes.h"

#include <string.h>

#define JOURNAL_MAGIC 0x4e333231u
#define JOURNAL_VERSION 1u
#define JOURNAL_COMMITTED 0x43544d21u

typedef struct {
    bool valid;
    uint32_t sequence;
    uint32_t payload_len;
} slot_state_t;

static uint8_t s_slot_buffer[STORAGE_JOURNAL_SLOT_SIZE];

static uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t len);
static bool read_slot(const storage_journal_t *journal, uint8_t slot_index, slot_state_t *out_state);
static bool read_slot_payload(const storage_journal_t *journal,
                              uint8_t slot_index,
                              uint8_t *dst,
                              size_t dst_cap,
                              size_t *out_len,
                              storage_journal_info_t *out_info);

bool storage_journal_init(storage_journal_t *journal,
                          nvm_hal_t *hal,
                          uint16_t unit_id,
                          uint32_t base_offset,
                          uint32_t slot_size) {
    if (journal == 0 || hal == 0 || slot_size == 0 || slot_size > STORAGE_JOURNAL_SLOT_SIZE) {
        return false;
    }
    if (slot_size < STORAGE_JOURNAL_HEADER_SIZE || (base_offset + (slot_size * 2u)) > hal->capacity) {
        return false;
    }
    if (hal->erase_required && ((slot_size % hal->erase_block) != 0 || (base_offset % hal->erase_block) != 0)) {
        return false;
    }
    /* A full slot is written in one nvm_write() call, so the slot size must also
     * be a whole number of write blocks (flash program pages), or every write
     * would fail alignment at runtime. */
    if (hal->write_block != 0u && ((slot_size % hal->write_block) != 0u || (base_offset % hal->write_block) != 0u)) {
        return false;
    }
    journal->hal = hal;
    journal->base_offset = base_offset;
    journal->slot_size = slot_size;
    journal->unit_id = unit_id;
    return true;
}

/* True if sequence x is strictly newer than y, using modular (sequence-distance)
 * comparison so it stays correct across the 32-bit wraparound (where the counter
 * skips 0xffffffff -> 1). A plain x > y would, post-wrap, treat the stale
 * 0xffffffff slot as newer than a fresh low-numbered slot and strand the journal. */
static bool seq_after(uint32_t x, uint32_t y) {
    return (int32_t)(x - y) > 0;
}

bool storage_journal_read_latest(const storage_journal_t *journal,
                                 uint8_t *dst,
                                 size_t dst_cap,
                                 size_t *out_len,
                                 storage_journal_info_t *out_info) {
    if (journal == 0 || dst == 0) {
        return false;
    }
    slot_state_t a;
    slot_state_t b;
    if (!read_slot(journal, 0u, &a) || !read_slot(journal, 1u, &b)) {
        return false;
    }
    if (!a.valid && !b.valid) {
        return false;
    }
    uint8_t slot = 0u;
    if (!a.valid || (b.valid && seq_after(b.sequence, a.sequence))) {
        slot = 1u;
    }
    return read_slot_payload(journal, slot, dst, dst_cap, out_len, out_info);
}

storage_journal_write_result_t storage_journal_write_detailed(
    storage_journal_t *journal,
    const uint8_t *payload,
    size_t payload_len) {
    if (journal == 0 || journal->hal == 0 || payload == 0) {
        return STORAGE_JOURNAL_WRITE_ERROR;
    }
    if (payload_len > (journal->slot_size - STORAGE_JOURNAL_HEADER_SIZE)) {
        return STORAGE_JOURNAL_WRITE_ERROR;
    }

    slot_state_t a;
    slot_state_t b;
    if (!read_slot(journal, 0u, &a) || !read_slot(journal, 1u, &b)) {
        return STORAGE_JOURNAL_WRITE_ERROR;
    }

    uint8_t target_slot = 0u;
    uint32_t sequence = 1u;
    /* Overwrite the OLDER slot (modular compare, wraparound-safe); the new record
     * gets newest_sequence + 1. "a is newest" == b is not strictly after a. */
    if (a.valid && (!b.valid || !seq_after(b.sequence, a.sequence))) {
        target_slot = 1u;
        sequence = a.sequence + 1u;
    } else if (b.valid) {
        target_slot = 0u;
        sequence = b.sequence + 1u;
    }
    if (sequence == 0u) {
        sequence = 1u;
    }

    memset(s_slot_buffer, 0xff, journal->slot_size);
    write_u32(&s_slot_buffer[0], JOURNAL_MAGIC);
    write_u16(&s_slot_buffer[4], JOURNAL_VERSION);
    write_u16(&s_slot_buffer[6], STORAGE_JOURNAL_HEADER_SIZE);
    write_u16(&s_slot_buffer[8], journal->unit_id);
    write_u16(&s_slot_buffer[10], 0u);
    write_u32(&s_slot_buffer[12], sequence);
    write_u32(&s_slot_buffer[16], (uint32_t)payload_len);
    write_u32(&s_slot_buffer[20], crc32_update(0xffffffffu, payload, payload_len) ^ 0xffffffffu);
    write_u32(&s_slot_buffer[24], JOURNAL_COMMITTED);
    write_u32(&s_slot_buffer[28], 0xffffffffu);
    memcpy(&s_slot_buffer[STORAGE_JOURNAL_HEADER_SIZE], payload, payload_len);

    uint32_t slot_offset = journal->base_offset + ((uint32_t)target_slot * journal->slot_size);
    if (journal->hal->erase_required) {
        nvm_status_t status = journal->hal->erase(
            journal->hal, slot_offset, journal->slot_size);
        if (status != NVM_STATUS_OK) {
            return status == NVM_STATUS_BUSY
                       ? STORAGE_JOURNAL_WRITE_BUSY
                       : STORAGE_JOURNAL_WRITE_ERROR;
        }
    }
    nvm_status_t status = journal->hal->write(
        journal->hal, slot_offset, s_slot_buffer, journal->slot_size);
    if (status != NVM_STATUS_OK) {
        return status == NVM_STATUS_BUSY
                   ? STORAGE_JOURNAL_WRITE_BUSY
                   : STORAGE_JOURNAL_WRITE_ERROR;
    }

    slot_state_t verify;
    return read_slot(journal, target_slot, &verify) && verify.valid &&
                   verify.sequence == sequence
               ? STORAGE_JOURNAL_WRITE_OK
               : STORAGE_JOURNAL_WRITE_ERROR;
}

bool storage_journal_write(storage_journal_t *journal,
                           const uint8_t *payload,
                           size_t payload_len) {
    return storage_journal_write_detailed(journal, payload, payload_len) ==
           STORAGE_JOURNAL_WRITE_OK;
}

static bool read_slot(const storage_journal_t *journal, uint8_t slot_index, slot_state_t *out_state) {
    if (journal == 0 || out_state == 0 || slot_index > 1u) {
        return false;
    }
    memset(out_state, 0, sizeof(*out_state));

    uint8_t header[STORAGE_JOURNAL_HEADER_SIZE];
    uint32_t offset = journal->base_offset + ((uint32_t)slot_index * journal->slot_size);
    if (journal->hal->read(journal->hal, offset, header, sizeof(header)) != NVM_STATUS_OK) {
        return false;
    }

    if (read_u32(&header[0]) != JOURNAL_MAGIC ||
        read_u16(&header[4]) != JOURNAL_VERSION ||
        read_u16(&header[6]) != STORAGE_JOURNAL_HEADER_SIZE ||
        read_u16(&header[8]) != journal->unit_id ||
        read_u32(&header[24]) != JOURNAL_COMMITTED) {
        return true;
    }

    uint32_t payload_len = read_u32(&header[16]);
    if (payload_len > (journal->slot_size - STORAGE_JOURNAL_HEADER_SIZE)) {
        return true;
    }

    uint32_t expected_crc = read_u32(&header[20]);
    uint32_t crc = 0xffffffffu;
    uint8_t chunk[64];
    uint32_t payload_offset = offset + STORAGE_JOURNAL_HEADER_SIZE;
    uint32_t remaining = payload_len;
    while (remaining != 0) {
        size_t chunk_len = remaining > sizeof(chunk) ? sizeof(chunk) : remaining;
        if (journal->hal->read(journal->hal, payload_offset, chunk, chunk_len) != NVM_STATUS_OK) {
            return false;
        }
        crc = crc32_update(crc, chunk, chunk_len);
        payload_offset += (uint32_t)chunk_len;
        remaining -= (uint32_t)chunk_len;
    }
    crc ^= 0xffffffffu;
    if (crc != expected_crc) {
        return true;
    }

    out_state->valid = true;
    out_state->sequence = read_u32(&header[12]);
    out_state->payload_len = payload_len;
    return true;
}

static bool read_slot_payload(const storage_journal_t *journal,
                              uint8_t slot_index,
                              uint8_t *dst,
                              size_t dst_cap,
                              size_t *out_len,
                              storage_journal_info_t *out_info) {
    slot_state_t state;
    if (!read_slot(journal, slot_index, &state) || !state.valid || state.payload_len > dst_cap) {
        return false;
    }
    uint32_t offset = journal->base_offset + ((uint32_t)slot_index * journal->slot_size) +
                      STORAGE_JOURNAL_HEADER_SIZE;
    if (journal->hal->read(journal->hal, offset, dst, state.payload_len) != NVM_STATUS_OK) {
        return false;
    }
    if (out_len != 0) {
        *out_len = state.payload_len;
    }
    if (out_info != 0) {
        out_info->sequence = state.sequence;
        out_info->payload_len = state.payload_len;
    }
    return true;
}

static uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t bit = 0; bit < 8u; bit++) {
            uint32_t mask = 0u - (crc & 1u);
            crc = (crc >> 1) ^ (0xedb88320u & mask);
        }
    }
    return crc;
}

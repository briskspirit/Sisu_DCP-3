/*
 * Host unit test for the dual-slot CRC journal in src/storage/storage_journal.c.
 *
 * The journal persists through an nvm_hal_t (read/erase/write callbacks). We back
 * those callbacks with a plain RAM buffer here so the logic runs on the host and
 * under ASan/UBSan. Oracle values (CRC, header byte layout, slot selection) are
 * derived independently from the spec/first principles, not copied from the code.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "storage/storage_journal.h"

static int s_failures;

static void assert_true(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        s_failures++;
    }
}

static void assert_eq_u32(uint32_t got, uint32_t want, const char *message) {
    if (got != want) {
        fprintf(stderr, "FAIL: %s (got 0x%08x want 0x%08x)\n", message, got, want);
        s_failures++;
    }
}

/* ---- RAM-backed nvm_hal stub --------------------------------------------- */

#define RAM_SIZE (64u * 1024u)

typedef struct {
    uint8_t data[RAM_SIZE];
    /* Fault injection knobs. */
    bool fail_write;
    bool fail_erase;
    bool fail_read;
    bool busy_write;
    bool busy_erase;
    uint32_t read_calls;
    uint32_t write_calls;
    uint32_t erase_calls;
} ram_store_t;

static ram_store_t s_ram;

static nvm_status_t ram_read(nvm_hal_t *hal, uint32_t offset, void *dst, size_t len) {
    ram_store_t *r = (ram_store_t *)hal->ctx;
    r->read_calls++;
    if (r->fail_read) {
        return NVM_STATUS_IO_ERROR;
    }
    if ((uint64_t)offset + len > hal->capacity) {
        return NVM_STATUS_OUT_OF_RANGE;
    }
    memcpy(dst, &r->data[offset], len);
    return NVM_STATUS_OK;
}

static nvm_status_t ram_erase(nvm_hal_t *hal, uint32_t offset, size_t len) {
    ram_store_t *r = (ram_store_t *)hal->ctx;
    r->erase_calls++;
    if (r->fail_erase) {
        return NVM_STATUS_IO_ERROR;
    }
    if (r->busy_erase) {
        return NVM_STATUS_BUSY;
    }
    if ((uint64_t)offset + len > hal->capacity) {
        return NVM_STATUS_OUT_OF_RANGE;
    }
    if (hal->erase_block != 0u && (offset % hal->erase_block != 0u || len % hal->erase_block != 0u)) {
        return NVM_STATUS_ALIGNMENT;
    }
    memset(&r->data[offset], 0xff, len);
    return NVM_STATUS_OK;
}

static nvm_status_t ram_write(nvm_hal_t *hal, uint32_t offset, const void *src, size_t len) {
    ram_store_t *r = (ram_store_t *)hal->ctx;
    r->write_calls++;
    if (r->fail_write) {
        return NVM_STATUS_IO_ERROR;
    }
    if (r->busy_write) {
        return NVM_STATUS_BUSY;
    }
    if ((uint64_t)offset + len > hal->capacity) {
        return NVM_STATUS_OUT_OF_RANGE;
    }
    if (hal->write_block != 0u && (offset % hal->write_block != 0u || len % hal->write_block != 0u)) {
        return NVM_STATUS_ALIGNMENT;
    }
    memcpy(&r->data[offset], src, len);
    return NVM_STATUS_OK;
}

static void hal_init(nvm_hal_t *hal, ram_store_t *r, bool erase_required, uint32_t erase_block,
                     uint32_t write_block) {
    memset(r, 0, sizeof(*r));
    memset(r->data, 0xff, sizeof(r->data)); /* erased flash state */
    memset(hal, 0, sizeof(*hal));
    hal->name = "ram";
    hal->ctx = r;
    hal->capacity = RAM_SIZE;
    hal->erase_block = erase_block;
    hal->write_block = write_block;
    hal->erase_required = erase_required;
    hal->read = ram_read;
    hal->erase = ram_erase;
    hal->write = ram_write;
}

/* ---- Independent CRC32 oracle (bit-reflected, poly 0x04C11DB7) ------------ */
/* Standard zlib/Ethernet CRC32: reflected poly 0xEDB88320, init 0xFFFFFFFF,
 * final XOR 0xFFFFFFFF. Implemented independently of the code under test. */
static uint32_t ref_crc32(const uint8_t *data, size_t len) {
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            if (crc & 1u) {
                crc = (crc >> 1) ^ 0xedb88320u;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc ^ 0xffffffffu;
}

/* ---- Header layout constants (from spec / storage_journal.c) -------------- */
#define J_MAGIC 0x4e333231u
#define J_VERSION 1u
#define J_COMMITTED 0x43544d21u
#define HDR STORAGE_JOURNAL_HEADER_SIZE

static uint32_t rd_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t rd_u16(const uint8_t *p) { return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8)); }
static void wr_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}
static void wr_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* Hand-build a valid committed slot image directly into the RAM store. */
static void forge_slot(ram_store_t *r, uint32_t slot_offset, uint16_t unit_id, uint32_t sequence,
                       const uint8_t *payload, uint32_t payload_len, uint32_t slot_size) {
    uint8_t *s = &r->data[slot_offset];
    memset(s, 0xff, slot_size);
    wr_u32(&s[0], J_MAGIC);
    wr_u16(&s[4], J_VERSION);
    wr_u16(&s[6], HDR);
    wr_u16(&s[8], unit_id);
    wr_u16(&s[10], 0u);
    wr_u32(&s[12], sequence);
    wr_u32(&s[16], payload_len);
    wr_u32(&s[20], ref_crc32(payload, payload_len));
    wr_u32(&s[24], J_COMMITTED);
    wr_u32(&s[28], 0xffffffffu);
    if (payload_len) {
        memcpy(&s[HDR], payload, payload_len);
    }
}

/* ======================================================================== */

static void test_crc_oracle_matches_layout(void) {
    /* Write a payload, then independently verify the on-flash CRC field. */
    nvm_hal_t hal;
    hal_init(&hal, &s_ram, false, 0u, 0u);
    storage_journal_t j;
    assert_true(storage_journal_init(&j, &hal, 0x1234u, 0u, 256u), "init no-erase");

    uint8_t payload[100];
    for (size_t i = 0; i < sizeof(payload); i++) {
        payload[i] = (uint8_t)(i * 7u + 3u);
    }
    assert_true(storage_journal_write(&j, payload, sizeof(payload)), "first write");

    /* Fresh journal: first write lands in slot 0 (target stays 0 when nothing valid). */
    uint8_t *s = &s_ram.data[0];
    assert_eq_u32(rd_u32(&s[0]), J_MAGIC, "magic field");
    assert_eq_u32(rd_u16(&s[4]), J_VERSION, "version field");
    assert_eq_u32(rd_u16(&s[6]), HDR, "header-size field");
    assert_eq_u32(rd_u16(&s[8]), 0x1234u, "unit-id field");
    assert_eq_u32(rd_u32(&s[12]), 1u, "first sequence == 1");
    assert_eq_u32(rd_u32(&s[16]), sizeof(payload), "payload len field");
    assert_eq_u32(rd_u32(&s[20]), ref_crc32(payload, sizeof(payload)), "CRC matches independent oracle");
    assert_eq_u32(rd_u32(&s[24]), J_COMMITTED, "committed marker");
    assert_true(memcmp(&s[HDR], payload, sizeof(payload)) == 0, "payload bytes stored");
}

static void test_round_trip_and_readback(void) {
    nvm_hal_t hal;
    hal_init(&hal, &s_ram, false, 0u, 0u);
    storage_journal_t j;
    assert_true(storage_journal_init(&j, &hal, 0xabcdu, 0u, 512u), "init");

    uint8_t payload[300];
    for (size_t i = 0; i < sizeof(payload); i++) {
        payload[i] = (uint8_t)(0xa5u ^ (i & 0xffu));
    }
    assert_true(storage_journal_write(&j, payload, sizeof(payload)), "write");

    uint8_t dst[512];
    size_t got_len = 0;
    storage_journal_info_t info;
    memset(&info, 0, sizeof(info));
    assert_true(storage_journal_read_latest(&j, dst, sizeof(dst), &got_len, &info), "read latest");
    assert_true(got_len == sizeof(payload), "read len matches");
    assert_true(memcmp(dst, payload, sizeof(payload)) == 0, "read payload matches");
    assert_eq_u32(info.sequence, 1u, "info sequence");
    assert_eq_u32(info.payload_len, (uint32_t)sizeof(payload), "info payload_len");
}

static void test_alternating_writes_pick_higher_sequence(void) {
    nvm_hal_t hal;
    hal_init(&hal, &s_ram, false, 0u, 0u);
    storage_journal_t j;
    assert_true(storage_journal_init(&j, &hal, 1u, 0u, 256u), "init");

    /* Perform a sequence of writes; each should bump the sequence number and
     * ping-pong between the two slots. Read-latest must always return the
     * newest payload. */
    for (uint32_t n = 1; n <= 10u; n++) {
        uint8_t payload[16];
        memset(payload, (uint8_t)n, sizeof(payload));
        assert_true(storage_journal_write(&j, payload, sizeof(payload)), "iter write");

        uint8_t dst[16];
        size_t len = 0;
        storage_journal_info_t info;
        memset(&info, 0, sizeof(info));
        assert_true(storage_journal_read_latest(&j, dst, sizeof(dst), &len, &info), "iter read");
        assert_eq_u32(info.sequence, n, "iter sequence increments");
        assert_true(len == sizeof(payload) && dst[0] == (uint8_t)n, "iter latest payload");
    }
}

static void test_torn_second_slot_falls_back(void) {
    /* Slot 0 valid seq=5, slot 1 half-written (committed marker missing -> torn).
     * read_latest must return slot 0. */
    nvm_hal_t hal;
    hal_init(&hal, &s_ram, false, 0u, 0u);
    storage_journal_t j;
    assert_true(storage_journal_init(&j, &hal, 7u, 0u, 256u), "init");

    uint8_t good[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    forge_slot(&s_ram, 0u, 7u, 5u, good, sizeof(good), 256u);
    /* Forge slot 1 as a torn write: looks like a newer seq but committed marker
     * is left erased (0xffffffff), simulating power loss before commit. */
    uint8_t newer[8] = {9, 9, 9, 9, 9, 9, 9, 9};
    forge_slot(&s_ram, 256u, 7u, 6u, newer, sizeof(newer), 256u);
    wr_u32(&s_ram.data[256u + 24u], 0xffffffffu); /* erase committed marker -> torn */

    uint8_t dst[8];
    size_t len = 0;
    storage_journal_info_t info;
    memset(&info, 0, sizeof(info));
    assert_true(storage_journal_read_latest(&j, dst, sizeof(dst), &len, &info), "read with torn slot1");
    assert_eq_u32(info.sequence, 5u, "torn slot ignored -> slot0 seq5");
    assert_true(memcmp(dst, good, sizeof(good)) == 0, "returned the committed payload");

    /* The next write must go to the torn slot 1 (slot0 valid, slot1 invalid ->
     * target 1) with sequence 6. */
    uint8_t fresh[8] = {0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17};
    assert_true(storage_journal_write(&j, fresh, sizeof(fresh)), "write over torn slot");
    memset(&info, 0, sizeof(info));
    assert_true(storage_journal_read_latest(&j, dst, sizeof(dst), &len, &info), "read after recovery");
    assert_eq_u32(info.sequence, 6u, "recovered to seq6");
    assert_true(memcmp(dst, fresh, sizeof(fresh)) == 0, "fresh payload now latest");
}

static void test_both_valid_pick_higher_sequence(void) {
    nvm_hal_t hal;
    hal_init(&hal, &s_ram, false, 0u, 0u);
    storage_journal_t j;
    assert_true(storage_journal_init(&j, &hal, 3u, 0u, 256u), "init");

    uint8_t a[4] = {0xaa, 0xaa, 0xaa, 0xaa};
    uint8_t b[4] = {0xbb, 0xbb, 0xbb, 0xbb};
    /* slot0 seq 100, slot1 seq 101 -> latest is slot1 */
    forge_slot(&s_ram, 0u, 3u, 100u, a, sizeof(a), 256u);
    forge_slot(&s_ram, 256u, 3u, 101u, b, sizeof(b), 256u);

    uint8_t dst[4];
    size_t len = 0;
    storage_journal_info_t info;
    memset(&info, 0, sizeof(info));
    assert_true(storage_journal_read_latest(&j, dst, sizeof(dst), &len, &info), "both valid read");
    assert_eq_u32(info.sequence, 101u, "pick higher seq (slot1)");
    assert_true(memcmp(dst, b, sizeof(b)) == 0, "slot1 payload returned");

    /* Now make slot0 the higher one. */
    forge_slot(&s_ram, 0u, 3u, 200u, a, sizeof(a), 256u);
    forge_slot(&s_ram, 256u, 3u, 101u, b, sizeof(b), 256u);
    memset(&info, 0, sizeof(info));
    assert_true(storage_journal_read_latest(&j, dst, sizeof(dst), &len, &info), "both valid read2");
    assert_eq_u32(info.sequence, 200u, "pick higher seq (slot0)");
    assert_true(memcmp(dst, a, sizeof(a)) == 0, "slot0 payload returned");

    /* Next write should overwrite the OLDER slot (slot1, seq101) with seq201. */
    uint8_t c[4] = {0xcc, 0xcc, 0xcc, 0xcc};
    assert_true(storage_journal_write(&j, c, sizeof(c)), "write picks older slot");
    assert_eq_u32(rd_u32(&s_ram.data[256u + 12u]), 201u, "older slot1 bumped to 201");
    assert_eq_u32(rd_u32(&s_ram.data[0u + 12u]), 200u, "slot0 left intact at 200");
}

static void test_both_invalid_is_fresh(void) {
    nvm_hal_t hal;
    hal_init(&hal, &s_ram, false, 0u, 0u);
    storage_journal_t j;
    assert_true(storage_journal_init(&j, &hal, 9u, 0u, 256u), "init");
    /* RAM is all 0xff (erased). No valid slot. */
    uint8_t dst[16];
    size_t len = 0;
    storage_journal_info_t info;
    assert_true(!storage_journal_read_latest(&j, dst, sizeof(dst), &len, &info), "fresh -> read fails");

    /* First write of a fresh journal yields sequence 1, into slot 0. */
    uint8_t p[4] = {1, 2, 3, 4};
    assert_true(storage_journal_write(&j, p, sizeof(p)), "fresh first write");
    assert_eq_u32(rd_u32(&s_ram.data[12]), 1u, "fresh seq == 1");
}

static void test_corrupted_crc_rejected(void) {
    nvm_hal_t hal;
    hal_init(&hal, &s_ram, false, 0u, 0u);
    storage_journal_t j;
    assert_true(storage_journal_init(&j, &hal, 5u, 0u, 256u), "init");

    uint8_t p[32];
    for (size_t i = 0; i < sizeof(p); i++) {
        p[i] = (uint8_t)i;
    }
    forge_slot(&s_ram, 0u, 5u, 1u, p, sizeof(p), 256u);
    /* Flip one payload byte AFTER computing CRC -> CRC mismatch. */
    s_ram.data[HDR + 5u] ^= 0x01u;

    uint8_t dst[32];
    size_t len = 0;
    storage_journal_info_t info;
    assert_true(!storage_journal_read_latest(&j, dst, sizeof(dst), &len, &info), "corrupt payload rejected");

    /* Corrupt the stored CRC field instead -> also rejected. */
    forge_slot(&s_ram, 0u, 5u, 1u, p, sizeof(p), 256u);
    s_ram.data[20] ^= 0xffu;
    assert_true(!storage_journal_read_latest(&j, dst, sizeof(dst), &len, &info), "corrupt CRC field rejected");
}

static void test_header_field_rejections(void) {
    nvm_hal_t hal;
    hal_init(&hal, &s_ram, false, 0u, 0u);
    storage_journal_t j;
    assert_true(storage_journal_init(&j, &hal, 0x55u, 0u, 256u), "init");

    uint8_t p[8] = {0, 1, 2, 3, 4, 5, 6, 7};
    uint8_t dst[8];
    size_t len = 0;
    storage_journal_info_t info;

    /* Bad magic. */
    forge_slot(&s_ram, 0u, 0x55u, 1u, p, sizeof(p), 256u);
    wr_u32(&s_ram.data[0], J_MAGIC ^ 1u);
    assert_true(!storage_journal_read_latest(&j, dst, sizeof(dst), &len, &info), "bad magic rejected");

    /* Bad version. */
    forge_slot(&s_ram, 0u, 0x55u, 1u, p, sizeof(p), 256u);
    wr_u16(&s_ram.data[4], 99u);
    assert_true(!storage_journal_read_latest(&j, dst, sizeof(dst), &len, &info), "bad version rejected");

    /* Wrong unit_id (cross-unit slot must be ignored). */
    forge_slot(&s_ram, 0u, 0x99u, 1u, p, sizeof(p), 256u);
    assert_true(!storage_journal_read_latest(&j, dst, sizeof(dst), &len, &info), "wrong unit_id rejected");

    /* payload_len larger than slot capacity -> rejected (no OOB read). */
    forge_slot(&s_ram, 0u, 0x55u, 1u, p, sizeof(p), 256u);
    wr_u32(&s_ram.data[16], 256u - HDR + 1u); /* one past max payload */
    assert_true(!storage_journal_read_latest(&j, dst, sizeof(dst), &len, &info), "oversized payload_len rejected");

    /* Missing committed marker -> rejected. */
    forge_slot(&s_ram, 0u, 0x55u, 1u, p, sizeof(p), 256u);
    wr_u32(&s_ram.data[24], 0u);
    assert_true(!storage_journal_read_latest(&j, dst, sizeof(dst), &len, &info), "uncommitted rejected");
}

static void test_empty_payload(void) {
    nvm_hal_t hal;
    hal_init(&hal, &s_ram, false, 0u, 0u);
    storage_journal_t j;
    assert_true(storage_journal_init(&j, &hal, 1u, 0u, 256u), "init");

    uint8_t dummy = 0;
    assert_true(storage_journal_write(&j, &dummy, 0u), "zero-length write");
    /* CRC of empty payload is 0x00000000 by the standard algorithm. */
    assert_eq_u32(rd_u32(&s_ram.data[20]), ref_crc32(NULL, 0u), "empty payload CRC == 0");
    assert_eq_u32(rd_u32(&s_ram.data[16]), 0u, "empty payload len == 0");

    uint8_t dst[8];
    size_t len = 123u;
    storage_journal_info_t info;
    memset(&info, 0, sizeof(info));
    assert_true(storage_journal_read_latest(&j, dst, sizeof(dst), &len, &info), "read empty payload");
    assert_true(len == 0u, "empty read len 0");
    assert_eq_u32(info.payload_len, 0u, "empty info len 0");
}

static void test_max_payload(void) {
    nvm_hal_t hal;
    hal_init(&hal, &s_ram, false, 0u, 0u);
    storage_journal_t j;
    uint32_t slot = 4096u;
    assert_true(storage_journal_init(&j, &hal, 1u, 0u, slot), "init max slot");

    uint32_t max_payload = slot - HDR; /* 4064 */
    static uint8_t big[4096];
    for (uint32_t i = 0; i < max_payload; i++) {
        big[i] = (uint8_t)(i * 31u + 7u);
    }
    assert_true(storage_journal_write(&j, big, max_payload), "write max payload");

    /* One byte over the max must be rejected without writing. */
    assert_true(!storage_journal_write(&j, big, max_payload + 1u), "over-max payload rejected");

    static uint8_t dst[4096];
    size_t len = 0;
    storage_journal_info_t info;
    memset(&info, 0, sizeof(info));
    assert_true(storage_journal_read_latest(&j, dst, sizeof(dst), &len, &info), "read max payload");
    assert_true(len == max_payload, "max read len");
    assert_true(memcmp(dst, big, max_payload) == 0, "max payload round-trips");
    assert_eq_u32(rd_u32(&s_ram.data[20]), ref_crc32(big, max_payload), "max payload CRC oracle");
}

static void test_dst_cap_too_small(void) {
    nvm_hal_t hal;
    hal_init(&hal, &s_ram, false, 0u, 0u);
    storage_journal_t j;
    assert_true(storage_journal_init(&j, &hal, 1u, 0u, 256u), "init");

    uint8_t p[64];
    memset(p, 0x5a, sizeof(p));
    assert_true(storage_journal_write(&j, p, sizeof(p)), "write 64");

    uint8_t small[32];
    size_t len = 999u;
    storage_journal_info_t info;
    /* dst_cap (32) < payload_len (64): must fail, must NOT overrun small[]. */
    assert_true(!storage_journal_read_latest(&j, small, sizeof(small), &len, &info), "too-small dst rejected");

    /* Exactly-fit dst_cap works. */
    uint8_t exact[64];
    len = 0;
    assert_true(storage_journal_read_latest(&j, exact, sizeof(exact), &len, &info), "exact dst fits");
    assert_true(len == sizeof(p) && memcmp(exact, p, sizeof(p)) == 0, "exact payload matches");
}

static void test_init_validation(void) {
    nvm_hal_t hal;
    hal_init(&hal, &s_ram, false, 0u, 0u);
    storage_journal_t j;

    assert_true(!storage_journal_init(NULL, &hal, 1u, 0u, 256u), "null journal rejected");
    assert_true(!storage_journal_init(&j, NULL, 1u, 0u, 256u), "null hal rejected");
    assert_true(!storage_journal_init(&j, &hal, 1u, 0u, 0u), "zero slot rejected");
    assert_true(!storage_journal_init(&j, &hal, 1u, 0u, STORAGE_JOURNAL_SLOT_SIZE + 1u), "oversize slot rejected");
    assert_true(!storage_journal_init(&j, &hal, 1u, 0u, STORAGE_JOURNAL_HEADER_SIZE - 1u), "below-header slot rejected");
    /* base + 2*slot must fit capacity. */
    assert_true(!storage_journal_init(&j, &hal, 1u, RAM_SIZE - 256u, 256u), "two slots overflow capacity rejected");

    /* Erase alignment requirements. */
    nvm_hal_t eh;
    ram_store_t er;
    hal_init(&eh, &er, true, 4096u, 256u);
    storage_journal_t je;
    assert_true(!storage_journal_init(&je, &eh, 1u, 0u, 256u), "slot not multiple of erase_block rejected");
    assert_true(storage_journal_init(&je, &eh, 1u, 0u, 4096u), "erase-aligned slot accepted");
    assert_true(!storage_journal_init(&je, &eh, 1u, 100u, 4096u), "unaligned base rejected");
}

static void test_write_with_erase_required(void) {
    nvm_hal_t hal;
    ram_store_t r;
    hal_init(&hal, &r, true, 4096u, 256u);
    storage_journal_t j;
    assert_true(storage_journal_init(&j, &hal, 1u, 0u, 4096u), "init erase journal");

    uint8_t p[100];
    memset(p, 0x33, sizeof(p));
    assert_true(storage_journal_write(&j, p, sizeof(p)), "erase-required write");
    assert_true(r.erase_calls >= 1u, "erase was called");

    uint8_t dst[100];
    size_t len = 0;
    storage_journal_info_t info;
    memset(&info, 0, sizeof(info));
    assert_true(storage_journal_read_latest(&j, dst, sizeof(dst), &len, &info), "erase journal read");
    assert_true(len == sizeof(p) && memcmp(dst, p, sizeof(p)) == 0, "erase journal payload");
}

static void test_write_io_failure(void) {
    nvm_hal_t hal;
    ram_store_t r;
    hal_init(&hal, &r, false, 0u, 0u);
    storage_journal_t j;
    assert_true(storage_journal_init(&j, &hal, 1u, 0u, 256u), "init");
    r.fail_write = true;
    uint8_t p[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    assert_true(!storage_journal_write(&j, p, sizeof(p)), "write reports IO failure");
}

static void test_write_failure_origin(void) {
    nvm_hal_t hal;
    ram_store_t r;
    storage_journal_t j;
    uint8_t p[8] = {1, 2, 3, 4, 5, 6, 7, 8};

    hal_init(&hal, &r, true, 4096u, 256u);
    assert_true(storage_journal_init(&j, &hal, 1u, 0u, 4096u),
                "init busy erase journal");
    r.busy_erase = true;
    assert_true(storage_journal_write_detailed(&j, p, sizeof(p)) ==
                    STORAGE_JOURNAL_WRITE_BUSY,
                "busy erase origin preserved");
    assert_eq_u32(r.write_calls, 0u, "busy erase stops before write");

    hal_init(&hal, &r, false, 0u, 0u);
    assert_true(storage_journal_init(&j, &hal, 1u, 0u, 256u),
                "init busy write journal");
    r.busy_write = true;
    assert_true(storage_journal_write_detailed(&j, p, sizeof(p)) ==
                    STORAGE_JOURNAL_WRITE_BUSY,
                "busy write origin preserved");

    r.busy_write = false;
    r.fail_write = true;
    assert_true(storage_journal_write_detailed(&j, p, sizeof(p)) ==
                    STORAGE_JOURNAL_WRITE_ERROR,
                "ordinary write failure is not busy");
    assert_true(storage_journal_write_detailed(&j, p, 225u) ==
                    STORAGE_JOURNAL_WRITE_ERROR,
                "pre-HAL validation failure is not busy");
}

static void test_null_arg_guards(void) {
    nvm_hal_t hal;
    hal_init(&hal, &s_ram, false, 0u, 0u);
    storage_journal_t j;
    assert_true(storage_journal_init(&j, &hal, 1u, 0u, 256u), "init");

    uint8_t p[4] = {1, 2, 3, 4};
    assert_true(!storage_journal_write(NULL, p, sizeof(p)), "write null journal");
    assert_true(!storage_journal_write(&j, NULL, sizeof(p)), "write null payload");

    uint8_t dst[8];
    size_t len = 0;
    storage_journal_info_t info;
    assert_true(!storage_journal_read_latest(NULL, dst, sizeof(dst), &len, &info), "read null journal");
    assert_true(!storage_journal_read_latest(&j, NULL, sizeof(dst), &len, &info), "read null dst");
}

static void test_read_tolerates_null_out_params(void) {
    nvm_hal_t hal;
    hal_init(&hal, &s_ram, false, 0u, 0u);
    storage_journal_t j;
    assert_true(storage_journal_init(&j, &hal, 1u, 0u, 256u), "init");
    uint8_t p[4] = {9, 8, 7, 6};
    assert_true(storage_journal_write(&j, p, sizeof(p)), "write");
    uint8_t dst[4];
    /* out_len and out_info may be NULL. */
    assert_true(storage_journal_read_latest(&j, dst, sizeof(dst), NULL, NULL), "read with null out params");
    assert_true(memcmp(dst, p, sizeof(p)) == 0, "payload still returned");
}

/* Sequence wraparound: when sequence would reach 0 it is reset to 1. We can't
 * realistically do 4 billion writes, so forge a slot at UINT32_MAX and check the
 * next write's stored sequence value. */
static void test_sequence_wraparound(void) {
    nvm_hal_t hal;
    hal_init(&hal, &s_ram, false, 0u, 0u);
    storage_journal_t j;
    assert_true(storage_journal_init(&j, &hal, 2u, 0u, 256u), "init");

    uint8_t p[4] = {1, 1, 1, 1};
    /* slot0 valid at UINT32_MAX, slot1 invalid. */
    forge_slot(&s_ram, 0u, 2u, 0xffffffffu, p, sizeof(p), 256u);
    memset(&s_ram.data[256u], 0xff, 256u); /* slot1 erased/invalid */

    uint8_t q[4] = {2, 2, 2, 2};
    assert_true(storage_journal_write(&j, q, sizeof(q)), "wrap write");
    /* a.sequence+1 == 0 -> code resets to 1. New record lands in slot1 (was invalid). */
    assert_eq_u32(rd_u32(&s_ram.data[256u + 12u]), 1u, "wrapped sequence reset to 1");

    /* FIXED (modular seq compare): read_latest must pick the JUST-WRITTEN slot1
     * (seq 1), not the stale slot0 (seq 0xffffffff) -- post-wrap, 1 is "newer". */
    uint8_t dst[4];
    size_t len = 0;
    storage_journal_info_t info;
    memset(&info, 0, sizeof(info));
    assert_true(storage_journal_read_latest(&j, dst, sizeof(dst), &len, &info), "wrap read");
    assert_eq_u32(info.sequence, 1u, "after wrap, read picks the new slot1 (seq 1)");
    assert_true(len == sizeof(q) && memcmp(dst, q, sizeof(q)) == 0, "wrap read returns new payload");

    /* And a further write must overwrite the STALE slot0 (the older slot), not get
     * stuck re-writing slot1 -- the write-side modular compare sees slot0 as older. */
    uint8_t r[4] = {3, 3, 3, 3};
    assert_true(storage_journal_write(&j, r, sizeof(r)), "post-wrap write");
    assert_eq_u32(rd_u32(&s_ram.data[12]), 2u, "post-wrap write lands in slot0 with seq 2");
    memset(&info, 0, sizeof(info));
    assert_true(storage_journal_read_latest(&j, dst, sizeof(dst), &len, &info), "post-wrap read");
    assert_eq_u32(info.sequence, 2u, "read picks slot0 seq 2");
    assert_true(memcmp(dst, r, sizeof(r)) == 0, "post-wrap read returns newest payload");
}

int main(void) {
    test_crc_oracle_matches_layout();
    test_round_trip_and_readback();
    test_alternating_writes_pick_higher_sequence();
    test_torn_second_slot_falls_back();
    test_both_valid_pick_higher_sequence();
    test_both_invalid_is_fresh();
    test_corrupted_crc_rejected();
    test_header_field_rejections();
    test_empty_payload();
    test_max_payload();
    test_dst_cap_too_small();
    test_init_validation();
    test_write_with_erase_required();
    test_write_io_failure();
    test_write_failure_origin();
    test_null_arg_guards();
    test_read_tolerates_null_out_params();
    test_sequence_wraparound();

    if (s_failures != 0) {
        fprintf(stderr, "%d failures\n", s_failures);
        return 1;
    }
    printf("storage_journal tests passed\n");
    return 0;
}

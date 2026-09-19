#include "diag/storage_powercut_test.h"

#include <limits.h>
#include <string.h>

#include "lfs_util.h"
#include "storage_bytes.h"

#define CONTROL_MAGIC 0x31544350u
#define BASELINE_COUNT 18u
#define CONTROL_SIZE (48u + BASELINE_COUNT * 8u)
#define NO_PENDING UINT32_MAX

static storage_backend_t *s_backend;
static powercut_status_t s_status;
static uint32_t s_legacy_crc;
static uint32_t s_lengths[BASELINE_COUNT];
static uint32_t s_crcs[BASELINE_COUNT];
static uint8_t s_data[STORAGE_RECORD_MAX_PAYLOAD];
static unsigned s_phase;

static uint16_t baseline_id(unsigned index) {
    return index < 16u ? (uint16_t)(0x3210u + index)
                       : (uint16_t)(0xfffeu + index - 16u);
}

static uint32_t checksum(const uint8_t *data, size_t len) {
    return lfs_crc(0xffffffffu, data, len) ^ 0xffffffffu;
}

static powercut_result_t fail(powercut_result_t result, uint16_t id) {
    s_status.result = result;
    s_status.failed_id = id;
    return result;
}

static powercut_result_t write_record(uint16_t id, size_t len) {
    if (s_backend->write(s_backend, id, s_data, len) != STORAGE_RECORD_OK) {
        return fail(POWERCUT_IO, id);
    }
    return POWERCUT_OK;
}

static powercut_result_t save_control(void) {
    memset(s_data, 0, CONTROL_SIZE);
    write_u32(s_data, CONTROL_MAGIC);
    write_u32(s_data + 4, 1u);
    write_u32(s_data + 8, s_status.boots);
    write_u32(s_data + 12, s_status.completed);
    write_u32(s_data + 16, s_status.armed);
    write_u32(s_data + 20, s_status.pending);
    write_u32(s_data + 24, s_status.generation[0]);
    write_u32(s_data + 28, s_status.generation[1]);
    write_u32(s_data + 32, s_status.recovered_old);
    write_u32(s_data + 36, s_status.recovered_new);
    write_u32(s_data + 40, s_legacy_crc);
    for (unsigned i = 0; i < BASELINE_COUNT; i++) {
        write_u32(s_data + 48u + 8u * i, s_lengths[i]);
        write_u32(s_data + 52u + 8u * i, s_crcs[i]);
    }
    return write_record(POWERCUT_CONTROL_ID, CONTROL_SIZE);
}

static size_t pattern_size(unsigned slot, uint32_t generation) {
    static const uint16_t sizes[] = {96, 128, 4064, 240, 241, 1024, 0};
    return sizes[(generation % 7u + slot * 2u) % 7u];
}

static uint8_t pattern_byte(unsigned slot, uint32_t generation, size_t offset) {
    uint32_t v = 0x91e10da5u ^ generation ^ (slot * 0x9e3779b9u) ^
                 ((uint32_t)offset * 0x85ebca6bu);
    v ^= v >> 16;
    v *= 0x7feb352du;
    v ^= v >> 15;
    return (uint8_t)(v ^ (v >> 8));
}

static bool matches(unsigned slot, uint32_t generation, size_t len) {
    if (len != pattern_size(slot, generation)) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (s_data[i] != pattern_byte(slot, generation, i)) {
            return false;
        }
    }
    return true;
}

static powercut_result_t write_pattern(unsigned slot, uint32_t generation) {
    size_t len = pattern_size(slot, generation);
    for (size_t i = 0; i < len; i++) {
        s_data[i] = pattern_byte(slot, generation, i);
    }
    return write_record((uint16_t)(POWERCUT_DATA_ID + slot), len);
}

static powercut_result_t baseline(bool capture) {
    for (unsigned i = 0; i < BASELINE_COUNT; i++) {
        uint16_t id = baseline_id(i);
        size_t len = 0;
        storage_record_result_t rc = s_backend->read(
            s_backend, id, s_data, sizeof(s_data), &len);
        if (rc != STORAGE_RECORD_OK && rc != STORAGE_RECORD_NOT_FOUND) {
            return fail(POWERCUT_IO, id);
        }
        uint32_t length = rc == STORAGE_RECORD_OK ? (uint32_t)len : UINT32_MAX;
        uint32_t crc = rc == STORAGE_RECORD_OK ? checksum(s_data, len) : 0u;
        if (capture) {
            /* The migration marker must exist: never initialize a test on an
             * empty volume and mistake that for preserving migrated records. */
            if (id == 0xffffu &&
                (rc != STORAGE_RECORD_OK || len != 4u ||
                 memcmp(s_data, "SIS\x01", 4u) != 0)) {
                return fail(POWERCUT_BASELINE, id);
            }
            s_lengths[i] = length;
            s_crcs[i] = crc;
        } else if (s_lengths[i] != length || s_crcs[i] != crc) {
            return fail(POWERCUT_BASELINE, id);
        }
    }
    return POWERCUT_OK;
}

static powercut_result_t verify_data(bool require_new, bool recovering) {
    unsigned pending = s_status.pending;
    bool new_present = false;
    for (unsigned slot = 0; slot < 2u; slot++) {
        uint16_t id = (uint16_t)(POWERCUT_DATA_ID + slot);
        size_t len;
        if (s_backend->read(s_backend, id, s_data, sizeof(s_data), &len) !=
            STORAGE_RECORD_OK) {
            return fail(POWERCUT_IO, id);
        }
        uint32_t generation = s_status.generation[slot];
        if (slot == pending && generation != UINT32_MAX &&
            matches(slot, generation + 1u, len)) {
            new_present = true;
        } else if ((slot == pending && require_new) ||
                   !matches(slot, generation, len)) {
            return fail(POWERCUT_PATTERN, id);
        }
    }
    if (baseline(false) != POWERCUT_OK) {
        return s_status.result;
    }
    if (pending != NO_PENDING) {
        if (new_present) {
            s_status.generation[pending]++;
            s_status.completed++;
            s_status.recovered_new += recovering;
        } else {
            s_status.recovered_old += recovering;
        }
        s_status.pending = NO_PENDING;
    }
    return POWERCUT_OK;
}

powercut_result_t powercut_open(storage_backend_t *backend, uint32_t legacy_crc) {
    s_backend = backend;
    memset(&s_status, 0, sizeof(s_status));
    s_status.pending = NO_PENDING;
    s_legacy_crc = legacy_crc;
    s_phase = 0;
    size_t len;
    storage_record_result_t rc = backend->read(
        backend, POWERCUT_CONTROL_ID, s_data, sizeof(s_data), &len);
    if (rc == STORAGE_RECORD_NOT_FOUND) {
        return fail(POWERCUT_UNARMED, POWERCUT_CONTROL_ID);
    }
    if (rc != STORAGE_RECORD_OK) {
        return fail(POWERCUT_IO, POWERCUT_CONTROL_ID);
    }
    if (len != CONTROL_SIZE || read_u32(s_data) != CONTROL_MAGIC ||
        read_u32(s_data + 4) != 1u || read_u32(s_data + 16) > 1u ||
        (read_u32(s_data + 20) != NO_PENDING && read_u32(s_data + 20) > 1u) ||
        read_u32(s_data + 40) != legacy_crc) {
        return fail(POWERCUT_CONTROL, POWERCUT_CONTROL_ID);
    }
    s_status.boots = read_u32(s_data + 8);
    s_status.completed = read_u32(s_data + 12);
    s_status.armed = read_u32(s_data + 16) != 0u;
    s_status.pending = read_u32(s_data + 20);
    s_status.generation[0] = read_u32(s_data + 24);
    s_status.generation[1] = read_u32(s_data + 28);
    s_status.recovered_old = read_u32(s_data + 32);
    s_status.recovered_new = read_u32(s_data + 36);
    if (s_status.boots == UINT32_MAX || s_status.completed == UINT32_MAX ||
        s_status.recovered_old == UINT32_MAX || s_status.recovered_new == UINT32_MAX ||
        (uint64_t)s_status.generation[0] + s_status.generation[1] != s_status.completed ||
        (s_status.pending != NO_PENDING && !s_status.armed)) {
        return fail(POWERCUT_CONTROL, POWERCUT_CONTROL_ID);
    }
    for (unsigned i = 0; i < BASELINE_COUNT; i++) {
        s_lengths[i] = read_u32(s_data + 48u + i * 8u);
        s_crcs[i] = read_u32(s_data + 52u + i * 8u);
    }
    if (verify_data(false, true) != POWERCUT_OK) {
        return s_status.result;
    }
    s_status.boots++;
    return save_control();
}

powercut_result_t powercut_arm(void) {
    if (s_status.result == POWERCUT_UNARMED) {
        if (baseline(true) != POWERCUT_OK) {
            return s_status.result;
        }
        if (write_pattern(0u, 0u) != POWERCUT_OK ||
            write_pattern(1u, 0u) != POWERCUT_OK) {
            return s_status.result;
        }
        s_status.boots = 1u;
        s_status.result = POWERCUT_OK;
        s_status.failed_id = 0u;
    } else if (s_status.result != POWERCUT_OK) {
        return s_status.result;
    }
    if (verify_data(false, false) != POWERCUT_OK) {
        return s_status.result;
    }
    s_status.armed = true;
    s_phase = 0u;
    return save_control();
}

powercut_result_t powercut_step(void) {
    if (s_status.result != POWERCUT_OK || !s_status.armed) {
        return s_status.result;
    }
    if (s_phase == 0u) {
        if (s_status.completed == UINT32_MAX) {
            return fail(POWERCUT_LIMIT, POWERCUT_CONTROL_ID);
        }
        s_status.pending = s_status.completed % 2u;
        if (save_control() != POWERCUT_OK) {
            return s_status.result;
        }
        s_phase = 1u;
    } else if (s_phase == 1u) {
        unsigned slot = s_status.pending;
        if (write_pattern(slot, s_status.generation[slot] + 1u) != POWERCUT_OK) {
            return s_status.result;
        }
        s_phase = 2u;
    } else {
        if (verify_data(true, false) != POWERCUT_OK || save_control() != POWERCUT_OK) {
            return s_status.result;
        }
        s_phase = 0u;
    }
    return POWERCUT_OK;
}

powercut_result_t powercut_pause(void) {
    if (s_status.result != POWERCUT_OK) {
        return s_status.result;
    }
    if (verify_data(false, false) != POWERCUT_OK) {
        return s_status.result;
    }
    s_status.armed = false;
    s_phase = 0u;
    return save_control();
}

void powercut_get_status(powercut_status_t *status) {
    *status = s_status;
}

#ifndef STORAGE_POWERCUT_TEST_H
#define STORAGE_POWERCUT_TEST_H

#include <stdbool.h>

#include "storage/storage_backend.h"

#define POWERCUT_CONTROL_ID 0xfff7u
#define POWERCUT_DATA_ID 0xfff8u

typedef enum {
    POWERCUT_OK = 0,
    POWERCUT_UNARMED,
    POWERCUT_IO,
    POWERCUT_CONTROL,
    POWERCUT_BASELINE,
    POWERCUT_PATTERN,
    POWERCUT_LIMIT,
} powercut_result_t;

typedef struct {
    bool armed;
    uint32_t boots;
    uint32_t completed;
    uint32_t recovered_old;
    uint32_t recovered_new;
    uint32_t generation[2];
    uint32_t pending;
    powercut_result_t result;
    uint16_t failed_id;
} powercut_status_t;

/* Bench-only, single owner. No production records are written. Opening an
 * existing run verifies it before committing the next verified-boot count. */
powercut_result_t powercut_open(storage_backend_t *backend, uint32_t legacy_crc);
powercut_result_t powercut_arm(void);
powercut_result_t powercut_step(void);
powercut_result_t powercut_pause(void);
void powercut_get_status(powercut_status_t *status);

#endif

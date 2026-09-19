#ifndef STORAGE_USER_SPACE_H
#define STORAGE_USER_SPACE_H

#include "storage/storage_backend.h"

typedef enum {
    STORAGE_USER_CONTACTS,
    STORAGE_USER_INBOX,
    STORAGE_USER_OUTBOX,
    STORAGE_USER_PENDING,
    STORAGE_USER_OTHER,
    STORAGE_USER_POOL_COUNT,
} storage_user_pool_t;

typedef enum {
    STORAGE_USER_CALLS,
    STORAGE_USER_PICTURES,
    STORAGE_USER_TONES,
    STORAGE_USER_DICTIONARY,
    STORAGE_USER_DIVERT,
    STORAGE_USER_LEGACY_CATEGORY_COUNT,
} storage_user_legacy_category_t;

typedef struct {
    uint32_t files;
    uint32_t file_bytes; /* Includes the storage envelope; excludes temporary files. */
} storage_user_files_t;

typedef struct {
    storage_user_files_t contents;
    uint32_t allocated_bytes; /* Live data and both blocks of each metadata pair. */
    uint32_t limit_bytes;
} storage_user_pool_usage_t;

typedef struct {
    uint32_t capacity_bytes;
    uint32_t allocated_bytes;
    uint32_t recovery_reserve_bytes;
    storage_user_pool_usage_t pools[STORAGE_USER_POOL_COUNT];
    /* These existing record groups share OTHER's metadata and budget. */
    storage_user_files_t legacy[STORAGE_USER_LEGACY_CATEGORY_COUNT];
} storage_user_usage_t;

/* Read-only snapshot, rebuilt from durable filesystem contents after remount.
 * Single core0 storage owner, never an ISR API. No persistent usage database. */
storage_record_result_t storage_user_get_usage(storage_user_usage_t *out);

#endif

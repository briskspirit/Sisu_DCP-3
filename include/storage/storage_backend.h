#ifndef STORAGE_BACKEND_H
#define STORAGE_BACKEND_H

#include <stddef.h>
#include <stdint.h>

/* Semantic units store opaque, versioned payloads. Media and transactions stay
 * below this interface, allowing a future FRAM backend for selected units. */
#define STORAGE_RECORD_MAX_PAYLOAD 4064u

typedef enum {
    STORAGE_RECORD_OK = 0,
    STORAGE_RECORD_NOT_FOUND,
    STORAGE_RECORD_ERROR,
    STORAGE_RECORD_BUSY,
    STORAGE_RECORD_FULL,
    /* The filesystem read succeeded, but this record's contents are invalid.
     * Unlike ERROR/BUSY this is safe to isolate while continuing a scan. */
    STORAGE_RECORD_CORRUPT,
} storage_record_result_t;

typedef struct storage_backend storage_backend_t;
struct storage_backend {
    void *ctx;
    storage_record_result_t (*read)(storage_backend_t *backend, uint16_t id,
                                    uint8_t *dst, size_t cap, size_t *len);
    storage_record_result_t (*write)(storage_backend_t *backend, uint16_t id,
                                     const uint8_t *src, size_t len);
};

/* Board composition point. Domain owners never select media themselves. */
storage_record_result_t storage_backend_open(storage_backend_t *backend);

#endif

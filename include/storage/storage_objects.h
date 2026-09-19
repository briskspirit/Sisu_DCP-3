#ifndef STORAGE_OBJECTS_H
#define STORAGE_OBJECTS_H

#include "storage/storage_backend.h"

/* Named collections on the user volume. No filesystem types cross this API.
 * All operations run synchronously on the sole core0 storage owner, never an
 * ISR. Each logical multipart message is one object, not one per segment. */
typedef enum {
    STORAGE_OBJECT_CONTACT,
    STORAGE_OBJECT_INBOX,
    STORAGE_OBJECT_OUTBOX,
    STORAGE_OBJECT_PENDING_SMS,
    STORAGE_OBJECT_COLLECTION_COUNT,
} storage_object_collection_t;

#define STORAGE_OBJECT_MAX_PAYLOAD 4060u

storage_record_result_t storage_objects_open(void);
/* Durably reserve an ID before creating an object. IDs are never reused;
 * interrupted creation may leave a gap, but cannot alias an old selection. */
storage_record_result_t storage_object_allocate(uint32_t *id);
storage_record_result_t storage_object_read(storage_object_collection_t collection,
    uint32_t id, uint8_t *dst, size_t cap, size_t *len);
/* FULL includes category/temporary-space limits, not just physical exhaustion.
 * Interrupted replacement exposes the old or complete new object, never a
 * missing/partial object. An I/O error can occur after the commit reached media. */
storage_record_result_t storage_object_write(storage_object_collection_t collection,
    uint32_t id, const uint8_t *src, size_t len);
storage_record_result_t storage_object_remove(storage_object_collection_t collection,
    uint32_t id);
/* Small per-file state, atomically committed in directory metadata. New files
 * start at zero; body replacements preserve it. Fixed-size state updates may
 * use the bounded recovery allowance without rewriting the message body. */
storage_record_result_t storage_object_get_state(storage_object_collection_t collection,
    uint32_t id, uint32_t *state);
storage_record_result_t storage_object_set_state(storage_object_collection_t collection,
    uint32_t id, uint32_t state);
/* A single startup scan owns the iterator. Close it before mutating objects.
 * NOT_FOUND means end of collection; all other failures are real errors. */
storage_record_result_t storage_object_scan_begin(storage_object_collection_t collection);
storage_record_result_t storage_object_scan_next(uint32_t *id);
void storage_object_scan_end(void);

#endif

#ifndef PICO_SYNC_STUB_H
#define PICO_SYNC_STUB_H

#include <stdlib.h>
#include <stdint.h>

/* The host is single-threaded, but lock ordering and re-entry still matter.
 * State lives in the lock object so separately linked translation units see
 * the same ownership. Tests may override the failure hook before including
 * this header; normal host tests fail hard even when NDEBUG is defined. */
#ifndef PICO_SYNC_STUB_FAILURE
static inline void pico_sync_stub_abort(const char *reason)
{
    (void)reason;
    abort();
}
#define PICO_SYNC_STUB_FAILURE(reason) pico_sync_stub_abort(reason)
#endif

#define PICO_SYNC_STUB_MAGIC UINT32_C(0x43534953)

typedef struct {
    uint32_t magic;
    uint32_t depth;
} critical_section_t;

static inline void critical_section_init(critical_section_t *c)
{
    if (c == NULL) {
        PICO_SYNC_STUB_FAILURE("critical_section_init: null lock");
        return;
    }
    c->magic = PICO_SYNC_STUB_MAGIC;
    c->depth = 0u;
}

static inline void critical_section_enter_blocking(critical_section_t *c)
{
    if (c == NULL || c->magic != PICO_SYNC_STUB_MAGIC) {
        PICO_SYNC_STUB_FAILURE("critical_section_enter: lock not initialized");
        return;
    }
    if (c->depth != 0u) {
        PICO_SYNC_STUB_FAILURE("critical_section_enter: recursive entry");
        return;
    }
    c->depth = 1u;
}

static inline void critical_section_exit(critical_section_t *c)
{
    if (c == NULL || c->magic != PICO_SYNC_STUB_MAGIC) {
        PICO_SYNC_STUB_FAILURE("critical_section_exit: lock not initialized");
        return;
    }
    if (c->depth != 1u) {
        PICO_SYNC_STUB_FAILURE("critical_section_exit: lock not entered");
        return;
    }
    c->depth = 0u;
}

static inline void critical_section_deinit(critical_section_t *c)
{
    if (c == NULL || c->magic != PICO_SYNC_STUB_MAGIC) {
        PICO_SYNC_STUB_FAILURE("critical_section_deinit: lock not initialized");
        return;
    }
    if (c->depth != 0u) {
        PICO_SYNC_STUB_FAILURE("critical_section_deinit: lock still entered");
        return;
    }
    c->magic = 0u;
}

#endif

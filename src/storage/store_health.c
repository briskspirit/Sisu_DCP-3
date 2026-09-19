#include "storage/store_health.h"

#include "storage_bytes.h"
#include "store_service_internal.h"

#define HEALTH_MAGIC 0x31484c53u
#define HEALTH_WIRE_SIZE (8u + 4u * STORAGE_OBJECT_COLLECTION_COUNT)
/* Above the combined normal contact/mailbox/staging index limits. A damaged
 * collection exceeding this bound requires service instead of unbounded RAM. */
#define HEALTH_SEEN_LIMIT 2048u
static uint32_t s_counts[STORAGE_OBJECT_COLLECTION_COUNT];
static uint32_t s_seen_ids[HEALTH_SEEN_LIMIT];
static uint8_t s_seen_collections[HEALTH_SEEN_LIMIT];
static uint16_t s_seen_count;

void store_health_note_corrupt(storage_object_collection_t collection, uint32_t id) {
    if ((unsigned)collection >= STORAGE_OBJECT_COLLECTION_COUNT || id == 0u) return;
    for (uint16_t i = 0; i < s_seen_count; i++) {
        if (s_seen_ids[i] == id && s_seen_collections[i] == (unsigned)collection) return;
    }
    if (s_seen_count == HEALTH_SEEN_LIMIT) {
        store_service_require_service(STORE_BOOT_FAULT_TRACKING);
        return;
    }
    s_seen_ids[s_seen_count] = id;
    s_seen_collections[s_seen_count++] = (uint8_t)collection;
    if (s_counts[collection] != UINT32_MAX) {
        s_counts[collection]++;
        (void)store_engine_mark_dirty(STORE_UNIT_STORAGE_HEALTH);
    }
}

bool store_health_get_counts(uint32_t counts[STORAGE_OBJECT_COLLECTION_COUNT]) {
    if (counts == NULL || !store_service_unit_ready(STORE_UNIT_STORAGE_HEALTH)) return false;
    memcpy(counts, s_counts, sizeof(s_counts));
    return true;
}

static void reset(uint8_t instance) {
    (void)instance;
    memset(s_counts, 0, sizeof(s_counts));
    s_seen_count = 0u;
}

static bool serialize(uint8_t instance, uint8_t *dst, size_t cap, size_t *len) {
    (void)instance;
    if (cap < HEALTH_WIRE_SIZE) return false;
    write_u32(dst, HEALTH_MAGIC);
    write_u32(dst + 4u, STORE_PAYLOAD_VERSION);
    for (unsigned i = 0; i < STORAGE_OBJECT_COLLECTION_COUNT; i++)
        write_u32(dst + 8u + i * 4u, s_counts[i]);
    *len = HEALTH_WIRE_SIZE;
    return true;
}

static bool apply(uint8_t instance, const uint8_t *src, size_t len) {
    (void)instance;
    if (len != HEALTH_WIRE_SIZE || read_u32(src) != HEALTH_MAGIC ||
        read_u32(src + 4u) != STORE_PAYLOAD_VERSION) return false;
    for (unsigned i = 0; i < STORAGE_OBJECT_COLLECTION_COUNT; i++)
        s_counts[i] = read_u32(src + 8u + i * 4u);
    return true;
}

const store_unit_ops_t g_store_health_unit_ops = {
    .reset_ram=reset, .serialize=serialize, .apply=apply, .name="storage health",
};

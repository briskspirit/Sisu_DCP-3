#ifndef STORE_HEALTH_H
#define STORE_HEALTH_H

#include <stdbool.h>
#include "storage/storage_objects.h"

/* Persistent validation-failure observations, once per object per boot.
 * A subsequent boot detecting the same damaged record is another observation.
 * Counters saturate; known damaged files are never modified by this API. */
void store_health_note_corrupt(storage_object_collection_t collection, uint32_t id);
bool store_health_get_counts(uint32_t counts[STORAGE_OBJECT_COLLECTION_COUNT]);

#endif

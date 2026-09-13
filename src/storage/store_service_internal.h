#ifndef STORE_SERVICE_INTERNAL_H
#define STORE_SERVICE_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "storage/store_service.h"
#include "storage/storage_journal.h"

#define STORE_PAYLOAD_VERSION 1u

static inline void store_copy_text(char *dst, size_t cap, const char *src) {
    if (cap == 0u) {
        return;
    }
    if (src == 0) {
        src = "";
    }
    size_t i = 0u;
    while (i + 1u < cap && src[i] != '\0') {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

typedef struct {
    void (*reset_ram)(uint8_t instance);
    bool (*serialize)(uint8_t instance,
                      uint8_t *dst,
                      size_t cap,
                      size_t *out_len);
    bool (*apply)(uint8_t instance, const uint8_t *payload, size_t len);
    void (*fallback_missing_or_corrupt)(uint8_t instance);
    const char *name;
} store_unit_ops_t;

typedef struct {
    const store_unit_ops_t *ops;
    uint8_t instance;
} store_unit_binding_t;

typedef struct {
    store_status_t status;
    bool flash_busy;
} store_commit_result_t;

typedef enum {
    STORE_DOMAIN_PHONEBOOK = 0,
    STORE_DOMAIN_SMS,
    STORE_DOMAIN_CALLS,
    STORE_DOMAIN_PROFILES,
    STORE_DOMAIN_CLOCK,
    STORE_DOMAIN_SYSTEM,
    STORE_DOMAIN_COUNT
} store_domain_t;

extern const store_unit_ops_t g_store_settings_unit_ops;
extern const store_unit_ops_t g_store_calls_unit_ops;
extern const store_unit_ops_t g_store_t9_unit_ops;
extern const store_unit_ops_t g_store_pictures_unit_ops;
extern const store_unit_ops_t g_store_tones_unit_ops;
extern const store_unit_ops_t g_store_divert_unit_ops;
extern const store_unit_ops_t g_store_warranty_unit_ops;
extern const store_unit_ops_t g_store_battery_learning_unit_ops;
extern const store_unit_ops_t g_store_battery_charge_supervisor_unit_ops;

const store_unit_binding_t *store_engine_unit_binding(store_unit_t unit);
store_commit_result_t store_engine_commit_binding(
    const store_unit_binding_t *binding,
    storage_journal_t *journal,
    uint8_t *payload,
    size_t payload_cap);
store_status_t store_engine_mark_dirty(store_unit_t unit);
void store_calls_migrate_life_timer(uint32_t warranty_donor);
void store_warranty_post_load(void);
uint32_t store_warranty_legacy_life_timer(void);

#endif

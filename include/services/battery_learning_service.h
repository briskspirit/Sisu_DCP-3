#ifndef BATTERY_LEARNING_SERVICE_H
#define BATTERY_LEARNING_SERVICE_H

#include <stdbool.h>
#include <stdint.h>

#include "services/battery_learning_logic.h"

typedef struct {
    battery_learning_snapshot_t model;
    bool initialized;
    bool persistence_pending;
    uint32_t update_count;
    uint32_t persistence_requests;
    uint32_t persistence_failures;
    uint32_t last_model_result;
} battery_learning_service_snapshot_t;

typedef enum {
    BATTERY_LEARNING_PROVISION_OK = 0,
    BATTERY_LEARNING_PROVISION_INVALID_ARGUMENT,
    BATTERY_LEARNING_PROVISION_ACTIVE_EVIDENCE,
    BATTERY_LEARNING_PROVISION_OBSERVATION_UNAVAILABLE,
    BATTERY_LEARNING_PROVISION_STORE_NOT_READY,
    BATTERY_LEARNING_PROVISION_STORE_ERROR,
} battery_learning_provision_status_t;

typedef enum {
    BATTERY_LEARNING_RECOVER_OK = 0,
    BATTERY_LEARNING_RECOVER_INVALID_ARGUMENT,
    BATTERY_LEARNING_RECOVER_STALE_COUNT,
    BATTERY_LEARNING_RECOVER_ACTIVE_EVIDENCE,
    BATTERY_LEARNING_RECOVER_OBSERVATION_UNAVAILABLE,
    BATTERY_LEARNING_RECOVER_STORE_NOT_READY,
    BATTERY_LEARNING_RECOVER_STORE_ERROR,
} battery_learning_recover_status_t;

void battery_learning_service_init(void);

/* Existing-awake-path supervisor update. Events are BATTERY_LEARNING_EVENT_*
 * bits.
 * charge_factor_permille is the supervisor's effective physical calibration;
 * invalid values fall back to the product profile prior. This function owns
 * no timer and must only be called from an existing awake path. Capacity
 * exhaustion is diagnostic prediction only; the app must supply a separately
 * qualified BATTERY_LEARNING_EVENT_EMPTY before an endpoint is learned. */
uint32_t battery_learning_service_poll(uint32_t now_ms, uint32_t events,
                                       uint16_t charge_factor_permille);

/* Development/provisioning seam. This replaces health evidence only and is
 * refused while a charge/full-to-empty cycle is live. A successful return
 * means the store accepted the new record for its normally paced journal
 * commit; callers can inspect the store dirty bit for flash durability. */
battery_learning_provision_status_t battery_learning_service_provision(
    const battery_learning_provision_t *provision);

/* CDC recovery seam for a measured cycle whose EMPTY event was missed. Unlike
 * provisioning, this appends to the existing history and preserves anchors,
 * resistance evidence, counters, and pack identity. It is accepted only while
 * detached with coherent current gauge evidence. */
battery_learning_recover_status_t
battery_learning_service_recover_capacity_cycle(
    uint16_t capacity_mah,
    uint16_t expected_accepted_cycles);

/* Guarded postmortem import of an exact FULL ACR endpoint captured in the
 * current gauge session. The charger must be detached and the accepted-cycle
 * count must match, making retries and stale operator state fail atomically. */
battery_learning_recover_status_t
battery_learning_service_recover_full_endpoint(
    uint32_t full_acr_raw,
    int64_t full_session_delta_nah,
    uint16_t expected_accepted_cycles);

void battery_learning_service_get_snapshot(
    battery_learning_service_snapshot_t *out);

#endif

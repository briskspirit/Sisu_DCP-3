#include "services/battery_learning_service.h"

#include <limits.h>
#include <string.h>

#include "services/board_diag_service.h"
#include "storage/store_service.h"

static battery_learning_state_t s_state;
static battery_learning_persisted_t s_pending_persisted;
static battery_learning_service_snapshot_t s_snapshot;

static uint32_t saturating_increment_u32(uint32_t value) {
    return value == UINT32_MAX ? value : value + 1u;
}

static void publish_snapshot(void) {
    battery_learning_get_snapshot(&s_state, &s_snapshot.model);
}

static void try_persist(void) {
    if (!s_snapshot.persistence_pending || !store_service_ready()) {
        return;
    }
    store_status_t status =
        store_battery_learning_set(&s_pending_persisted);
    if (status == STORE_STATUS_OK) {
        s_snapshot.persistence_pending = false;
    } else {
        s_snapshot.persistence_failures = saturating_increment_u32(
            s_snapshot.persistence_failures);
    }
}

void battery_learning_service_init(void) {
    memset(&s_snapshot, 0, sizeof(s_snapshot));
    battery_learning_persisted_t persisted;
    store_status_t status = store_battery_learning_get(&persisted);
    bool valid = status == STORE_STATUS_OK &&
                 battery_learning_persisted_valid(&persisted, NULL);
    battery_learning_init(&s_state, NULL, valid ? &persisted : NULL);
    s_snapshot.initialized = true;
    bool migrated_full_anchor = valid && !persisted.soc_valid &&
                                persisted.full_anchor_valid &&
                                s_state.persisted.soc_valid;
    if (!valid || migrated_full_anchor) {
        battery_learning_export_persisted(
            &s_state, &s_pending_persisted);
        s_snapshot.persistence_pending = true;
        s_snapshot.persistence_requests = 1u;
        try_persist();
    }
    publish_snapshot();
}

uint32_t battery_learning_service_poll(uint32_t now_ms, uint32_t events,
                                       uint16_t charge_factor_permille) {
    if (!s_snapshot.initialized) {
        battery_learning_service_init();
    }
    battery_learning_profile_t profile = *battery_learning_nimh_profile();
    if (charge_factor_permille >= BATTERY_CHARGE_FACTOR_MIN_PERMILLE &&
        charge_factor_permille <= BATTERY_CHARGE_FACTOR_MAX_PERMILLE) {
        profile.charge_factor_permille = charge_factor_permille;
    }
    battery_learning_observation_t observation;
    board_diag_get_battery_learning_observation(now_ms, &observation);
    uint32_t result = battery_learning_update(
        &s_state, &profile, &observation, events);
    s_snapshot.update_count =
        saturating_increment_u32(s_snapshot.update_count);
    s_snapshot.last_model_result = result;
    if ((result & BATTERY_LEARNING_RESULT_PERSIST) != 0u) {
        battery_learning_export_persisted(
            &s_state, &s_pending_persisted);
        s_snapshot.persistence_pending = true;
        s_snapshot.persistence_requests = saturating_increment_u32(
            s_snapshot.persistence_requests);
    }
    try_persist();
    publish_snapshot();
    return result;
}

battery_learning_provision_status_t battery_learning_service_provision(
    const battery_learning_provision_t *provision) {
    if (provision == NULL) {
        return BATTERY_LEARNING_PROVISION_INVALID_ARGUMENT;
    }
    if (!s_snapshot.initialized) {
        battery_learning_service_init();
    }
    if (!store_service_ready()) {
        return BATTERY_LEARNING_PROVISION_STORE_NOT_READY;
    }

    battery_learning_observation_t observation;
    board_diag_get_battery_learning_observation(0u, &observation);
    if (s_snapshot.model.full_anchor_valid || observation.charger_connected ||
        observation.charge_active) {
        return BATTERY_LEARNING_PROVISION_ACTIVE_EVIDENCE;
    }
    if (!observation.authoritative || !observation.sample_valid ||
        !observation.current_valid ||
        !observation.continuity_valid) {
        return BATTERY_LEARNING_PROVISION_OBSERVATION_UNAVAILABLE;
    }

    /* Complete an older handoff before replacing it. This cannot block on a
     * flash write: try_persist only updates the store's journaled RAM owner. */
    try_persist();
    if (s_snapshot.persistence_pending) {
        return BATTERY_LEARNING_PROVISION_STORE_ERROR;
    }

    battery_learning_state_t candidate = s_state;
    /* Bind the candidate to the gauge session visible at command time. This
     * makes provisioning safe even if CDC wins the race against the first
     * ordinary sidecar poll after boot or battery replacement. */
    (void)battery_learning_update(&candidate, NULL, &observation,
                                  BATTERY_LEARNING_EVENT_NONE);
    if (!battery_learning_provision(&candidate, NULL, provision)) {
        return BATTERY_LEARNING_PROVISION_INVALID_ARGUMENT;
    }
    battery_learning_persisted_t persisted;
    battery_learning_export_persisted(&candidate, &persisted);
    if (store_battery_learning_set(&persisted) != STORE_STATUS_OK) {
        return BATTERY_LEARNING_PROVISION_STORE_ERROR;
    }

    s_state = candidate;
    s_pending_persisted = persisted;
    s_snapshot.persistence_pending = false;
    s_snapshot.persistence_requests = saturating_increment_u32(
        s_snapshot.persistence_requests);
    publish_snapshot();
    return BATTERY_LEARNING_PROVISION_OK;
}

battery_learning_recover_status_t
battery_learning_service_recover_capacity_cycle(
    uint16_t capacity_mah,
    uint16_t expected_accepted_cycles) {
    const battery_learning_profile_t *profile =
        battery_learning_nimh_profile();
    if (capacity_mah < profile->capacity_min_mah ||
        capacity_mah > profile->capacity_max_mah) {
        return BATTERY_LEARNING_RECOVER_INVALID_ARGUMENT;
    }
    if (!s_snapshot.initialized) {
        battery_learning_service_init();
    }
    if (!store_service_ready()) {
        return BATTERY_LEARNING_RECOVER_STORE_NOT_READY;
    }

    battery_learning_observation_t observation;
    board_diag_get_battery_learning_observation(0u, &observation);
    if (observation.charger_connected || observation.charge_active) {
        return BATTERY_LEARNING_RECOVER_ACTIVE_EVIDENCE;
    }
    if (!observation.authoritative || !observation.sample_valid ||
        !observation.current_valid || !observation.continuity_valid) {
        return BATTERY_LEARNING_RECOVER_OBSERVATION_UNAVAILABLE;
    }

    try_persist();
    if (s_snapshot.persistence_pending) {
        return BATTERY_LEARNING_RECOVER_STORE_ERROR;
    }

    battery_learning_state_t candidate = s_state;
    (void)battery_learning_update(
        &candidate, profile, &observation, BATTERY_LEARNING_EVENT_NONE);
    if (candidate.persisted.accepted_capacity_cycles !=
        expected_accepted_cycles) {
        return BATTERY_LEARNING_RECOVER_STALE_COUNT;
    }
    if (!battery_learning_recover_capacity_cycle(
            &candidate, profile, capacity_mah,
            expected_accepted_cycles)) {
        return BATTERY_LEARNING_RECOVER_INVALID_ARGUMENT;
    }

    battery_learning_persisted_t persisted;
    battery_learning_export_persisted(&candidate, &persisted);
    if (store_battery_learning_set(&persisted) != STORE_STATUS_OK) {
        return BATTERY_LEARNING_RECOVER_STORE_ERROR;
    }

    s_state = candidate;
    s_pending_persisted = persisted;
    s_snapshot.persistence_pending = false;
    s_snapshot.persistence_requests = saturating_increment_u32(
        s_snapshot.persistence_requests);
    publish_snapshot();
    return BATTERY_LEARNING_RECOVER_OK;
}

battery_learning_recover_status_t
battery_learning_service_recover_full_endpoint(
    uint32_t full_acr_raw,
    int64_t full_session_delta_nah,
    uint16_t expected_accepted_cycles) {
    if (!s_snapshot.initialized) {
        battery_learning_service_init();
    }
    if (!store_service_ready()) {
        return BATTERY_LEARNING_RECOVER_STORE_NOT_READY;
    }

    battery_learning_observation_t observation;
    board_diag_get_battery_learning_observation(0u, &observation);
    if (observation.charger_connected || observation.charge_active) {
        return BATTERY_LEARNING_RECOVER_ACTIVE_EVIDENCE;
    }
    if (!observation.authoritative || !observation.sample_valid ||
        !observation.current_valid || !observation.continuity_valid ||
        observation.current_ua > 0) {
        return BATTERY_LEARNING_RECOVER_OBSERVATION_UNAVAILABLE;
    }

    try_persist();
    if (s_snapshot.persistence_pending) {
        return BATTERY_LEARNING_RECOVER_STORE_ERROR;
    }

    battery_learning_state_t candidate = s_state;
    const battery_learning_profile_t *profile =
        battery_learning_nimh_profile();
    (void)battery_learning_update(
        &candidate, profile, &observation, BATTERY_LEARNING_EVENT_NONE);
    if (candidate.persisted.accepted_capacity_cycles !=
        expected_accepted_cycles) {
        return BATTERY_LEARNING_RECOVER_STALE_COUNT;
    }
    if (!battery_learning_recover_full_endpoint(
            &candidate, profile, &observation, full_acr_raw,
            full_session_delta_nah, expected_accepted_cycles)) {
        return BATTERY_LEARNING_RECOVER_INVALID_ARGUMENT;
    }

    battery_learning_persisted_t persisted;
    battery_learning_export_persisted(&candidate, &persisted);
    if (store_battery_learning_set(&persisted) != STORE_STATUS_OK) {
        return BATTERY_LEARNING_RECOVER_STORE_ERROR;
    }

    s_state = candidate;
    s_pending_persisted = persisted;
    s_snapshot.persistence_pending = false;
    s_snapshot.persistence_requests = saturating_increment_u32(
        s_snapshot.persistence_requests);
    publish_snapshot();
    return BATTERY_LEARNING_RECOVER_OK;
}

void battery_learning_service_get_snapshot(
    battery_learning_service_snapshot_t *out) {
    if (out != NULL) {
        *out = s_snapshot;
    }
}

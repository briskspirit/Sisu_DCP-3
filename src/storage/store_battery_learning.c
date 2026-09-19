#include "storage/store_service.h"

#include "storage_bytes.h"
#include "store_service_internal.h"

#include <string.h>

#define BATTERY_LEARNING_MAGIC_V2 UINT32_C(0x324c5442) /* "BTL2" */
#define BATTERY_LEARNING_PAYLOAD_VERSION_V2 2u
#define BATTERY_LEARNING_PAYLOAD_LEN_V2 96u
#define BATTERY_LEARNING_FLAG_FULL_ANCHOR (1u << 0)
#define BATTERY_LEARNING_FLAG_CYCLE_QUALIFIED (1u << 1)
#define BATTERY_LEARNING_FLAG_NATURAL_EMPTY (1u << 2)
#define BATTERY_LEARNING_FLAG_SOC_VALID (1u << 3)
#define BATTERY_LEARNING_FLAG_SOC_CHARGE_SEGMENT (1u << 4)
#define BATTERY_LEARNING_FLAGS_VALID \
    (BATTERY_LEARNING_FLAG_FULL_ANCHOR | \
     BATTERY_LEARNING_FLAG_CYCLE_QUALIFIED | \
     BATTERY_LEARNING_FLAG_NATURAL_EMPTY | \
     BATTERY_LEARNING_FLAG_SOC_VALID | \
     BATTERY_LEARNING_FLAG_SOC_CHARGE_SEGMENT)

static battery_learning_persisted_t s_battery_learning;

static bool battery_learning_equal(
    const battery_learning_persisted_t *a,
    const battery_learning_persisted_t *b) {
    return a->profile_id == b->profile_id &&
        a->nominal_capacity_mah == b->nominal_capacity_mah &&
        memcmp(a->capacity_history_mah, b->capacity_history_mah,
               sizeof(a->capacity_history_mah)) == 0 &&
        a->capacity_history_count == b->capacity_history_count &&
        a->capacity_history_next == b->capacity_history_next &&
        a->accepted_capacity_cycles == b->accepted_capacity_cycles &&
        a->rejected_capacity_cycles == b->rejected_capacity_cycles &&
        a->last_capacity_mah == b->last_capacity_mah &&
        memcmp(a->resistance_mohm, b->resistance_mohm,
               sizeof(a->resistance_mohm)) == 0 &&
        memcmp(a->resistance_sample_count, b->resistance_sample_count,
               sizeof(a->resistance_sample_count)) == 0 &&
        a->pack_generation == b->pack_generation &&
        a->full_anchor_acr_raw == b->full_anchor_acr_raw &&
        a->full_anchor_nah == b->full_anchor_nah &&
        a->cycle_min_temperature_mdegc ==
            b->cycle_min_temperature_mdegc &&
        a->cycle_max_temperature_mdegc ==
            b->cycle_max_temperature_mdegc &&
        a->full_anchor_valid == b->full_anchor_valid &&
        a->capacity_cycle_qualified == b->capacity_cycle_qualified &&
        a->natural_empty_valid == b->natural_empty_valid &&
        a->soc_valid == b->soc_valid &&
        a->soc_charge_segment == b->soc_charge_segment &&
        a->soc_capacity_mah == b->soc_capacity_mah &&
        a->soc_charge_factor_permille == b->soc_charge_factor_permille &&
        a->soc_bootstrap_reference_mv == b->soc_bootstrap_reference_mv &&
        a->soc_anchor_provenance == b->soc_anchor_provenance &&
        a->soc_confidence == b->soc_confidence &&
        a->soc_anchor_session_delta_nah ==
            b->soc_anchor_session_delta_nah &&
        a->soc_anchor_remaining_nah == b->soc_anchor_remaining_nah;
}

store_status_t store_battery_learning_get(
    battery_learning_persisted_t *out_state) {
    if (out_state == NULL) {
        return STORE_STATUS_INVALID_ARGUMENT;
    }
    *out_state = s_battery_learning;
    return STORE_STATUS_OK;
}

store_status_t store_battery_learning_set(
    const battery_learning_persisted_t *state) {
    if (!battery_learning_persisted_valid(state, NULL)) {
        return STORE_STATUS_INVALID_ARGUMENT;
    }
    /* Compare semantic fields, not C-struct padding. Otherwise an equivalent
     * caller-built record can spuriously dirty flash. */
    if (battery_learning_equal(&s_battery_learning, state)) {
        return STORE_STATUS_OK;
    }
    s_battery_learning = *state;
    return store_engine_mark_dirty(STORE_UNIT_BATTERY_LEARNING);
}

static void load_defaults(void) {
    battery_learning_persisted_defaults(&s_battery_learning, NULL);
}

static bool serialize_battery_learning(
    uint8_t *dst, size_t cap, size_t *out_len) {
    if (dst == NULL || out_len == NULL ||
        !battery_learning_persisted_valid(&s_battery_learning, NULL)) {
        return false;
    }
    uint16_t flags = 0u;
    if (s_battery_learning.full_anchor_valid) {
        flags |= BATTERY_LEARNING_FLAG_FULL_ANCHOR;
    }
    if (s_battery_learning.capacity_cycle_qualified) {
        flags |= BATTERY_LEARNING_FLAG_CYCLE_QUALIFIED;
    }
    if (s_battery_learning.natural_empty_valid) {
        flags |= BATTERY_LEARNING_FLAG_NATURAL_EMPTY;
    }
    if (s_battery_learning.soc_valid) {
        flags |= BATTERY_LEARNING_FLAG_SOC_VALID;
    }
    if (s_battery_learning.soc_charge_segment) {
        flags |= BATTERY_LEARNING_FLAG_SOC_CHARGE_SEGMENT;
    }

    size_t pos = 0u;
    if (!write_u32_field(dst, cap, &pos, BATTERY_LEARNING_MAGIC_V2) ||
        !write_u16_field(dst, cap, &pos,
                         BATTERY_LEARNING_PAYLOAD_VERSION_V2) ||
        !write_u16_field(dst, cap, &pos, flags) ||
        !write_u8_field(dst, cap, &pos,
                        s_battery_learning.profile_id) ||
        !write_u8_field(dst, cap, &pos,
                        (uint8_t)s_battery_learning.soc_anchor_provenance) ||
        !write_u8_field(dst, cap, &pos,
                        (uint8_t)s_battery_learning.soc_confidence) ||
        !write_u8_field(dst, cap, &pos, 0u) ||
        !write_u16_field(dst, cap, &pos,
                         s_battery_learning.nominal_capacity_mah)) {
        return false;
    }
    for (uint8_t i = 0u;
         i < BATTERY_LEARNING_CAPACITY_HISTORY_COUNT; i++) {
        if (!write_u16_field(dst, cap, &pos,
                             s_battery_learning.capacity_history_mah[i])) {
            return false;
        }
    }
    if (!write_u8_field(dst, cap, &pos,
                        s_battery_learning.capacity_history_count) ||
        !write_u8_field(dst, cap, &pos,
                        s_battery_learning.capacity_history_next) ||
        !write_u16_field(dst, cap, &pos,
                         s_battery_learning.accepted_capacity_cycles) ||
        !write_u16_field(dst, cap, &pos,
                         s_battery_learning.rejected_capacity_cycles) ||
        !write_u16_field(dst, cap, &pos,
                         s_battery_learning.last_capacity_mah)) {
        return false;
    }
    for (uint8_t i = 0u;
         i < BATTERY_LEARNING_RESISTANCE_BIN_COUNT; i++) {
        if (!write_u16_field(dst, cap, &pos,
                             s_battery_learning.resistance_mohm[i])) {
            return false;
        }
    }
    for (uint8_t i = 0u;
         i < BATTERY_LEARNING_RESISTANCE_BIN_COUNT; i++) {
        if (!write_u16_field(
                dst, cap, &pos,
                s_battery_learning.resistance_sample_count[i])) {
            return false;
        }
    }
    if (!write_u32_field(dst, cap, &pos,
                         s_battery_learning.pack_generation) ||
        !write_u32_field(dst, cap, &pos,
                         s_battery_learning.full_anchor_acr_raw) ||
        !write_i64_field(dst, cap, &pos,
                         s_battery_learning.full_anchor_nah) ||
        !write_i32_field(dst, cap, &pos,
                         s_battery_learning.cycle_min_temperature_mdegc) ||
        !write_i32_field(dst, cap, &pos,
                         s_battery_learning.cycle_max_temperature_mdegc) ||
        !write_u16_field(dst, cap, &pos,
                         s_battery_learning.soc_capacity_mah) ||
        !write_u16_field(dst, cap, &pos,
                         s_battery_learning.soc_charge_factor_permille) ||
        !write_u16_field(dst, cap, &pos,
                         s_battery_learning.soc_bootstrap_reference_mv) ||
        !write_u16_field(dst, cap, &pos, 0u) ||
        !write_i64_field(dst, cap, &pos,
                         s_battery_learning.soc_anchor_session_delta_nah) ||
        !write_i64_field(dst, cap, &pos,
                         s_battery_learning.soc_anchor_remaining_nah) ||
        !write_u32_field(dst, cap, &pos, 0u) ||
        !write_u32_field(dst, cap, &pos, 0u) ||
        pos != BATTERY_LEARNING_PAYLOAD_LEN_V2) {
        return false;
    }
    *out_len = pos;
    return true;
}

static bool decode_battery_learning_v2(
    const uint8_t *payload, size_t len,
    battery_learning_persisted_t *loaded) {
    if (len != BATTERY_LEARNING_PAYLOAD_LEN_V2 ||
        read_u32(&payload[0]) != BATTERY_LEARNING_MAGIC_V2 ||
        read_u16(&payload[4]) != BATTERY_LEARNING_PAYLOAD_VERSION_V2 ||
        (read_u16(&payload[6]) &
         (uint16_t)~BATTERY_LEARNING_FLAGS_VALID) != 0u ||
        payload[11] != 0u || read_u16(&payload[70]) != 0u ||
        read_u32(&payload[88]) != 0u ||
        read_u32(&payload[92]) != 0u) {
        return false;
    }
    uint16_t flags = read_u16(&payload[6]);
    battery_learning_persisted_defaults(loaded, NULL);
    loaded->full_anchor_valid =
        (flags & BATTERY_LEARNING_FLAG_FULL_ANCHOR) != 0u;
    loaded->capacity_cycle_qualified =
        (flags & BATTERY_LEARNING_FLAG_CYCLE_QUALIFIED) != 0u;
    loaded->natural_empty_valid =
        (flags & BATTERY_LEARNING_FLAG_NATURAL_EMPTY) != 0u;
    loaded->soc_valid =
        (flags & BATTERY_LEARNING_FLAG_SOC_VALID) != 0u;
    loaded->soc_charge_segment =
        (flags & BATTERY_LEARNING_FLAG_SOC_CHARGE_SEGMENT) != 0u;
    loaded->profile_id = payload[8];
    loaded->soc_anchor_provenance =
        (battery_soc_provenance_t)payload[9];
    loaded->soc_confidence = (battery_soc_confidence_t)payload[10];
    loaded->nominal_capacity_mah = read_u16(&payload[12]);
    for (uint8_t i = 0u;
         i < BATTERY_LEARNING_CAPACITY_HISTORY_COUNT; i++) {
        loaded->capacity_history_mah[i] =
            read_u16(&payload[14u + (size_t)i * 2u]);
    }
    loaded->capacity_history_count = payload[20];
    loaded->capacity_history_next = payload[21];
    loaded->accepted_capacity_cycles = read_u16(&payload[22]);
    loaded->rejected_capacity_cycles = read_u16(&payload[24]);
    loaded->last_capacity_mah = read_u16(&payload[26]);
    for (uint8_t i = 0u;
         i < BATTERY_LEARNING_RESISTANCE_BIN_COUNT; i++) {
        loaded->resistance_mohm[i] =
            read_u16(&payload[28u + (size_t)i * 2u]);
        loaded->resistance_sample_count[i] =
            read_u16(&payload[34u + (size_t)i * 2u]);
    }
    loaded->pack_generation = read_u32(&payload[40]);
    loaded->full_anchor_acr_raw = read_u32(&payload[44]);
    loaded->full_anchor_nah = read_i64(&payload[48]);
    loaded->cycle_min_temperature_mdegc = read_i32(&payload[56]);
    loaded->cycle_max_temperature_mdegc = read_i32(&payload[60]);
    loaded->soc_capacity_mah = read_u16(&payload[64]);
    loaded->soc_charge_factor_permille = read_u16(&payload[66]);
    loaded->soc_bootstrap_reference_mv = read_u16(&payload[68]);
    loaded->soc_anchor_session_delta_nah = read_i64(&payload[72]);
    loaded->soc_anchor_remaining_nah = read_i64(&payload[80]);
    return true;
}

static bool apply_battery_learning(
    const uint8_t *payload, size_t len) {
    if (payload == NULL) {
        return false;
    }
    battery_learning_persisted_t loaded;
    bool decoded = decode_battery_learning_v2(payload, len, &loaded);
    if (!decoded || !battery_learning_persisted_valid(&loaded, NULL)) {
        return false;
    }
    s_battery_learning = loaded;
    return true;
}

static void reset_battery_learning_unit(uint8_t instance) {
    (void)instance;
    load_defaults();
}

static bool serialize_battery_learning_unit(
    uint8_t instance, uint8_t *dst, size_t cap, size_t *out_len) {
    (void)instance;
    return serialize_battery_learning(dst, cap, out_len);
}

static bool apply_battery_learning_unit(
    uint8_t instance, const uint8_t *payload, size_t len) {
    (void)instance;
    return apply_battery_learning(payload, len);
}

const store_unit_ops_t g_store_battery_learning_unit_ops = {
    .reset_ram = reset_battery_learning_unit,
    .serialize = serialize_battery_learning_unit,
    .apply = apply_battery_learning_unit,
    .fallback_missing_or_corrupt = NULL,
    .name = "battery learning",
};

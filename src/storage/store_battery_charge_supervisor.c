#include "storage/store_service.h"

#include "storage_bytes.h"
#include "store_service_internal.h"

#include <string.h>

#define CHARGE_SUPERVISOR_MAGIC_V4 UINT32_C(0x34534743) /* "CGS4" */
#define CHARGE_SUPERVISOR_PAYLOAD_LEN_V4 128u
#define CHARGE_SUPERVISOR_FLAG_ACTIVE (1u << 0)
#define CHARGE_SUPERVISOR_FLAG_CAPACITY (1u << 1)
#define CHARGE_SUPERVISOR_FLAG_REMAINING (1u << 2)
#define CHARGE_SUPERVISOR_FLAG_RESISTANCE (1u << 3)
#define CHARGE_SUPERVISOR_FLAG_DEFICIT (1u << 4)
#define CHARGE_SUPERVISOR_FLAG_FACTOR_CONFIDENT (1u << 5)
#define CHARGE_SUPERVISOR_FLAG_TARGET (1u << 6)
#define CHARGE_SUPERVISOR_FLAG_LAST_TERMINAL (1u << 7)
#define CHARGE_SUPERVISOR_FLAG_LAST_NET_VALID (1u << 8)
#define CHARGE_SUPERVISOR_FLAG_LAST_VOLTAGE_VALID (1u << 9)
#define CHARGE_SUPERVISOR_FLAG_LAST_TRACE_COMPLETE (1u << 10)
#define CHARGE_SUPERVISOR_FLAG_CONFIG_FACTOR_CONFIDENT (1u << 11)
#define CHARGE_SUPERVISOR_FLAG_INHIBIT_LATCHED (1u << 12)
#define CHARGE_SUPERVISOR_FLAG_MAINTENANCE_REARM (1u << 13)
#define CHARGE_SUPERVISOR_FLAG_LAST_FULL_QUALIFIED (1u << 14)
#define CHARGE_SUPERVISOR_FLAGS_VALID_V4 UINT16_C(0x7fff)

static battery_charge_supervisor_persisted_t s_charge_supervisor;

static bool persisted_equal(
    const battery_charge_supervisor_persisted_t *a,
    const battery_charge_supervisor_persisted_t *b) {
    return a->hardware_profile_id == b->hardware_profile_id &&
        a->chemistry == b->chemistry &&
        a->charge_generation == b->charge_generation &&
        a->active_session_valid == b->active_session_valid &&
        a->pack_generation == b->pack_generation &&
        a->gauge_session == b->gauge_session &&
        a->start_acr_raw == b->start_acr_raw &&
        a->start_session_delta_nah == b->start_session_delta_nah &&
        a->frozen_capacity_valid == b->frozen_capacity_valid &&
        a->frozen_capacity_mah == b->frozen_capacity_mah &&
        a->frozen_capacity_confidence == b->frozen_capacity_confidence &&
        a->frozen_remaining_valid == b->frozen_remaining_valid &&
        a->frozen_remaining_mah == b->frozen_remaining_mah &&
        a->frozen_remaining_nah == b->frozen_remaining_nah &&
        a->frozen_soc_provenance == b->frozen_soc_provenance &&
        a->frozen_soc_confidence == b->frozen_soc_confidence &&
        a->frozen_resistance_valid == b->frozen_resistance_valid &&
        a->frozen_resistance_mohm == b->frozen_resistance_mohm &&
        a->deficit_valid == b->deficit_valid &&
        a->deficit_mah == b->deficit_mah &&
        a->deficit_nah == b->deficit_nah &&
        a->charge_factor_permille == b->charge_factor_permille &&
        a->charge_factor_confident == b->charge_factor_confident &&
        a->target_valid == b->target_valid &&
        a->target_input_nah == b->target_input_nah &&
        a->safety_input_nah == b->safety_input_nah &&
        a->configured_policy == b->configured_policy &&
        a->configured_charge_factor_permille ==
            b->configured_charge_factor_permille &&
        a->configured_charge_factor_confident ==
            b->configured_charge_factor_confident &&
        a->supervisor_inhibit_latched ==
            b->supervisor_inhibit_latched &&
        a->latched_stop_reason == b->latched_stop_reason &&
        a->maintenance_rearm_pending == b->maintenance_rearm_pending &&
        a->completion_rearm_used == b->completion_rearm_used &&
        a->last_terminal_valid == b->last_terminal_valid &&
        a->last_terminal_full_qualified ==
            b->last_terminal_full_qualified &&
        a->last_terminal_net_input_valid ==
            b->last_terminal_net_input_valid &&
        a->last_terminal_voltage_valid ==
            b->last_terminal_voltage_valid &&
        a->last_terminal_trace_complete ==
            b->last_terminal_trace_complete &&
        a->last_terminal_reason == b->last_terminal_reason &&
        a->last_terminal_generation == b->last_terminal_generation &&
        a->last_terminal_elapsed_ms == b->last_terminal_elapsed_ms &&
        a->last_terminal_net_input_nah == b->last_terminal_net_input_nah &&
        a->last_terminal_mv == b->last_terminal_mv &&
        a->last_curve_peak_mv == b->last_curve_peak_mv &&
        a->last_curve_drop_mv == b->last_curve_drop_mv &&
        a->last_curve_slope_mv_per_min == b->last_curve_slope_mv_per_min;
}

store_status_t store_battery_charge_supervisor_get(
    battery_charge_supervisor_persisted_t *out_state) {
    if (out_state == NULL) {
        return STORE_STATUS_INVALID_ARGUMENT;
    }
    *out_state = s_charge_supervisor;
    return STORE_STATUS_OK;
}

store_status_t store_battery_charge_supervisor_set(
    const battery_charge_supervisor_persisted_t *state) {
    if (!battery_charge_supervisor_persisted_valid(state)) {
        return STORE_STATUS_INVALID_ARGUMENT;
    }
    if (persisted_equal(&s_charge_supervisor, state)) {
        return STORE_STATUS_OK;
    }
    s_charge_supervisor = *state;
    return store_engine_mark_dirty(STORE_UNIT_BATTERY_CHARGE_SUPERVISOR);
}

static uint16_t flags_from_state(
    const battery_charge_supervisor_persisted_t *state) {
    uint16_t flags = 0u;
    if (state->active_session_valid) {
        flags |= CHARGE_SUPERVISOR_FLAG_ACTIVE;
    }
    if (state->frozen_capacity_valid) {
        flags |= CHARGE_SUPERVISOR_FLAG_CAPACITY;
    }
    if (state->frozen_remaining_valid) {
        flags |= CHARGE_SUPERVISOR_FLAG_REMAINING;
    }
    if (state->frozen_resistance_valid) {
        flags |= CHARGE_SUPERVISOR_FLAG_RESISTANCE;
    }
    if (state->deficit_valid) {
        flags |= CHARGE_SUPERVISOR_FLAG_DEFICIT;
    }
    if (state->charge_factor_confident) {
        flags |= CHARGE_SUPERVISOR_FLAG_FACTOR_CONFIDENT;
    }
    if (state->target_valid) {
        flags |= CHARGE_SUPERVISOR_FLAG_TARGET;
    }
    if (state->last_terminal_valid) {
        flags |= CHARGE_SUPERVISOR_FLAG_LAST_TERMINAL;
    }
    if (state->last_terminal_net_input_valid) {
        flags |= CHARGE_SUPERVISOR_FLAG_LAST_NET_VALID;
    }
    if (state->last_terminal_voltage_valid) {
        flags |= CHARGE_SUPERVISOR_FLAG_LAST_VOLTAGE_VALID;
    }
    if (state->last_terminal_trace_complete) {
        flags |= CHARGE_SUPERVISOR_FLAG_LAST_TRACE_COMPLETE;
    }
    if (state->configured_charge_factor_confident) {
        flags |= CHARGE_SUPERVISOR_FLAG_CONFIG_FACTOR_CONFIDENT;
    }
    if (state->supervisor_inhibit_latched) {
        flags |= CHARGE_SUPERVISOR_FLAG_INHIBIT_LATCHED;
    }
    if (state->maintenance_rearm_pending) {
        flags |= CHARGE_SUPERVISOR_FLAG_MAINTENANCE_REARM;
    }
    if (state->last_terminal_full_qualified) {
        flags |= CHARGE_SUPERVISOR_FLAG_LAST_FULL_QUALIFIED;
    }
    return flags;
}

static bool serialize_charge_supervisor(
    uint8_t *dst, size_t cap, size_t *out_len) {
    if (dst == NULL || out_len == NULL ||
        !battery_charge_supervisor_persisted_valid(&s_charge_supervisor)) {
        return false;
    }
    uint16_t slope_raw;
    memcpy(&slope_raw, &s_charge_supervisor.last_curve_slope_mv_per_min,
           sizeof(slope_raw));
    size_t pos = 0u;
    if (!write_u32_field(dst, cap, &pos, CHARGE_SUPERVISOR_MAGIC_V4) ||
        !write_u16_field(dst, cap, &pos, STORE_PAYLOAD_VERSION) ||
        !write_u16_field(dst, cap, &pos,
                         flags_from_state(&s_charge_supervisor)) ||
        !write_u8_field(dst, cap, &pos,
                        s_charge_supervisor.hardware_profile_id) ||
        !write_u8_field(dst, cap, &pos,
                        (uint8_t)s_charge_supervisor.chemistry) ||
        !write_u8_field(dst, cap, &pos,
                        (uint8_t)s_charge_supervisor.frozen_capacity_confidence) ||
        !write_u8_field(dst, cap, &pos,
                        (uint8_t)s_charge_supervisor.last_terminal_reason) ||
        !write_u32_field(dst, cap, &pos,
                         s_charge_supervisor.charge_generation) ||
        !write_u32_field(dst, cap, &pos,
                         s_charge_supervisor.pack_generation) ||
        !write_u32_field(dst, cap, &pos,
                         s_charge_supervisor.gauge_session) ||
        !write_u32_field(dst, cap, &pos,
                         s_charge_supervisor.start_acr_raw) ||
        !write_i64_field(dst, cap, &pos,
                         s_charge_supervisor.start_session_delta_nah) ||
        !write_u16_field(dst, cap, &pos,
                         s_charge_supervisor.frozen_capacity_mah) ||
        !write_u16_field(dst, cap, &pos,
                         s_charge_supervisor.frozen_remaining_mah) ||
        !write_u16_field(dst, cap, &pos,
                         s_charge_supervisor.frozen_resistance_mohm) ||
        !write_u16_field(dst, cap, &pos,
                         s_charge_supervisor.deficit_mah) ||
        !write_u16_field(dst, cap, &pos,
                         s_charge_supervisor.charge_factor_permille) ||
        !write_u16_field(
            dst, cap, &pos,
            s_charge_supervisor.configured_charge_factor_permille)) {
        return false;
    }
    uint8_t wide[8];
    write_u64(wide, s_charge_supervisor.target_input_nah);
    if (!write_bytes(dst, cap, &pos, wide, sizeof(wide)) ||
        !write_u32_field(dst, cap, &pos,
                         s_charge_supervisor.last_terminal_generation) ||
        !write_u32_field(dst, cap, &pos,
                         s_charge_supervisor.last_terminal_elapsed_ms) ||
        !write_i64_field(dst, cap, &pos,
                         s_charge_supervisor.last_terminal_net_input_nah) ||
        !write_u16_field(dst, cap, &pos,
                         s_charge_supervisor.last_terminal_mv) ||
        !write_u16_field(dst, cap, &pos,
                         s_charge_supervisor.last_curve_peak_mv) ||
        !write_u16_field(dst, cap, &pos,
                         s_charge_supervisor.last_curve_drop_mv) ||
        !write_u16_field(dst, cap, &pos, slope_raw) ||
        !write_u8_field(dst, cap, &pos,
                        (uint8_t)s_charge_supervisor.configured_policy) ||
        !write_u8_field(dst, cap, &pos,
                        (uint8_t)s_charge_supervisor.frozen_soc_provenance) ||
        !write_u8_field(dst, cap, &pos,
                        (uint8_t)s_charge_supervisor.frozen_soc_confidence) ||
        !write_u8_field(dst, cap, &pos,
                        (uint8_t)s_charge_supervisor.latched_stop_reason)) {
        return false;
    }
    write_u64(wide, s_charge_supervisor.frozen_remaining_nah);
    if (!write_bytes(dst, cap, &pos, wide, sizeof(wide))) {
        return false;
    }
    write_u64(wide, s_charge_supervisor.deficit_nah);
    if (!write_bytes(dst, cap, &pos, wide, sizeof(wide))) {
        return false;
    }
    write_u64(wide, s_charge_supervisor.safety_input_nah);
    if (!write_bytes(dst, cap, &pos, wide, sizeof(wide)) ||
        !write_u32_field(dst, cap, &pos,
                         s_charge_supervisor.completion_rearm_used ? 1u : 0u) ||
        !write_u32_field(dst, cap, &pos, 0u) ||
        !write_u32_field(dst, cap, &pos, 0u) ||
        !write_u32_field(dst, cap, &pos, 0u) ||
        !write_u32_field(dst, cap, &pos, 0u) ||
        pos != CHARGE_SUPERVISOR_PAYLOAD_LEN_V4) {
        return false;
    }
    *out_len = pos;
    return true;
}

static void apply_common_flags(
    battery_charge_supervisor_persisted_t *loaded, uint16_t flags) {
    loaded->active_session_valid =
        (flags & CHARGE_SUPERVISOR_FLAG_ACTIVE) != 0u;
    loaded->frozen_capacity_valid =
        (flags & CHARGE_SUPERVISOR_FLAG_CAPACITY) != 0u;
    loaded->frozen_remaining_valid =
        (flags & CHARGE_SUPERVISOR_FLAG_REMAINING) != 0u;
    loaded->frozen_resistance_valid =
        (flags & CHARGE_SUPERVISOR_FLAG_RESISTANCE) != 0u;
    loaded->deficit_valid =
        (flags & CHARGE_SUPERVISOR_FLAG_DEFICIT) != 0u;
    loaded->charge_factor_confident =
        (flags & CHARGE_SUPERVISOR_FLAG_FACTOR_CONFIDENT) != 0u;
    loaded->target_valid =
        (flags & CHARGE_SUPERVISOR_FLAG_TARGET) != 0u;
    loaded->last_terminal_valid =
        (flags & CHARGE_SUPERVISOR_FLAG_LAST_TERMINAL) != 0u;
    loaded->last_terminal_net_input_valid =
        (flags & CHARGE_SUPERVISOR_FLAG_LAST_NET_VALID) != 0u;
    loaded->last_terminal_voltage_valid =
        (flags & CHARGE_SUPERVISOR_FLAG_LAST_VOLTAGE_VALID) != 0u;
    loaded->last_terminal_trace_complete =
        (flags & CHARGE_SUPERVISOR_FLAG_LAST_TRACE_COMPLETE) != 0u;
}

static void decode_common_fields(
    const uint8_t *payload,
    battery_charge_supervisor_persisted_t *loaded) {
    loaded->hardware_profile_id = payload[8];
    loaded->chemistry = (battery_charge_supervisor_chemistry_t)payload[9];
    loaded->frozen_capacity_confidence =
        (battery_capacity_confidence_t)payload[10];
    loaded->last_terminal_reason =
        (battery_charge_supervisor_terminal_t)payload[11];
    loaded->charge_generation = read_u32(&payload[12]);
    loaded->pack_generation = read_u32(&payload[16]);
    loaded->gauge_session = read_u32(&payload[20]);
    loaded->start_acr_raw = read_u32(&payload[24]);
    loaded->start_session_delta_nah = read_i64(&payload[28]);
    loaded->frozen_capacity_mah = read_u16(&payload[36]);
    loaded->frozen_remaining_mah = read_u16(&payload[38]);
    loaded->frozen_resistance_mohm = read_u16(&payload[40]);
    loaded->deficit_mah = read_u16(&payload[42]);
    loaded->charge_factor_permille = read_u16(&payload[44]);
    loaded->target_input_nah = read_u64(&payload[48]);
    loaded->last_terminal_generation = read_u32(&payload[56]);
    loaded->last_terminal_elapsed_ms = read_u32(&payload[60]);
    loaded->last_terminal_net_input_nah = read_i64(&payload[64]);
    loaded->last_terminal_mv = read_u16(&payload[72]);
    loaded->last_curve_peak_mv = read_u16(&payload[74]);
    loaded->last_curve_drop_mv = read_u16(&payload[76]);
    uint16_t slope_raw = read_u16(&payload[78]);
    memcpy(&loaded->last_curve_slope_mv_per_min, &slope_raw,
           sizeof(slope_raw));
}

static bool apply_charge_supervisor_v4(
    const uint8_t *payload, size_t len,
    battery_charge_supervisor_persisted_t *loaded) {
    if (payload == NULL || loaded == NULL ||
        len != CHARGE_SUPERVISOR_PAYLOAD_LEN_V4 ||
        read_u32(&payload[0]) != CHARGE_SUPERVISOR_MAGIC_V4 ||
        read_u16(&payload[4]) != STORE_PAYLOAD_VERSION ||
        (read_u16(&payload[6]) &
         (uint16_t)~CHARGE_SUPERVISOR_FLAGS_VALID_V4) != 0u ||
        read_u32(&payload[108]) > 1u) {
        return false;
    }
    for (size_t i = 112u; i < CHARGE_SUPERVISOR_PAYLOAD_LEN_V4; i++) {
        if (payload[i] != 0u) {
            return false;
        }
    }

    uint16_t flags = read_u16(&payload[6]);
    battery_charge_supervisor_persisted_defaults(loaded);
    decode_common_fields(payload, loaded);
    apply_common_flags(loaded, flags);
    loaded->configured_charge_factor_permille = read_u16(&payload[46]);
    loaded->configured_policy =
        (battery_charge_supervisor_policy_t)payload[80];
    loaded->frozen_soc_provenance =
        (battery_soc_provenance_t)payload[81];
    loaded->frozen_soc_confidence =
        (battery_soc_confidence_t)payload[82];
    loaded->latched_stop_reason =
        (battery_charge_supervisor_terminal_t)payload[83];
    loaded->frozen_remaining_nah = read_u64(&payload[84]);
    loaded->deficit_nah = read_u64(&payload[92]);
    loaded->safety_input_nah = read_u64(&payload[100]);
    loaded->configured_charge_factor_confident =
        (flags & CHARGE_SUPERVISOR_FLAG_CONFIG_FACTOR_CONFIDENT) != 0u;
    loaded->supervisor_inhibit_latched =
        (flags & CHARGE_SUPERVISOR_FLAG_INHIBIT_LATCHED) != 0u;
    loaded->maintenance_rearm_pending =
        (flags & CHARGE_SUPERVISOR_FLAG_MAINTENANCE_REARM) != 0u;
    loaded->last_terminal_full_qualified =
        (flags & CHARGE_SUPERVISOR_FLAG_LAST_FULL_QUALIFIED) != 0u;
    loaded->completion_rearm_used = read_u32(&payload[108]) != 0u;
    return battery_charge_supervisor_persisted_valid(loaded);
}

static bool apply_charge_supervisor(const uint8_t *payload, size_t len) {
    battery_charge_supervisor_persisted_t loaded;
    bool valid = apply_charge_supervisor_v4(payload, len, &loaded);
    if (!valid) {
        return false;
    }
    s_charge_supervisor = loaded;
    return true;
}

static void reset_charge_supervisor_unit(uint8_t instance) {
    (void)instance;
    battery_charge_supervisor_persisted_defaults(&s_charge_supervisor);
}

static bool serialize_charge_supervisor_unit(
    uint8_t instance, uint8_t *dst, size_t cap, size_t *out_len) {
    (void)instance;
    return serialize_charge_supervisor(dst, cap, out_len);
}

static bool apply_charge_supervisor_unit(
    uint8_t instance, const uint8_t *payload, size_t len) {
    (void)instance;
    return apply_charge_supervisor(payload, len);
}

const store_unit_ops_t g_store_battery_charge_supervisor_unit_ops = {
    .reset_ram = reset_charge_supervisor_unit,
    .serialize = serialize_charge_supervisor_unit,
    .apply = apply_charge_supervisor_unit,
    .fallback_missing_or_corrupt = NULL,
    .name = "charge supervisor",
};

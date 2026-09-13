#include "storage/store_service.h"

#include "storage_bytes.h"
#include "store_service_internal.h"

#include "hal/rtc_alarm_hal.h"

#include <string.h>

#include "services/log.h"

#define CALLS_MAGIC 0x43414c31u
#define CALLS_PAYLOAD_VERSION 2u
#define CALLS_LEGACY_PAYLOAD_VERSION 1u
#define CALLS_FIRST_YEAR 1999u
#define CALLS_LAST_YEAR 2090u
#define CALLS_YEAR_CODE_MAX (CALLS_LAST_YEAR - CALLS_FIRST_YEAR)

_Static_assert(CALLS_YEAR_CODE_MAX <= UINT8_MAX,
               "call-log year code must fit its version-2 byte");
_Static_assert(CALLS_LEGACY_PAYLOAD_VERSION == STORE_PAYLOAD_VERSION,
               "version-1 call migration must match the original store schema");

typedef struct {
    store_call_record_t records[STORE_CALL_LIST_LIMIT];
    uint8_t count;
    uint32_t next_id;
} call_list_state_t;

static call_list_state_t s_calls[STORE_CALL_LIST_COUNT];

static store_status_t mark_call_list_dirty(store_call_list_t list);
static uint32_t pack_datetime(const rtc_datetime_t *dt);
static void unpack_datetime(uint32_t packed, rtc_datetime_t *dt);
static uint8_t datetime_year_code(const rtc_datetime_t *dt);

uint8_t store_call_count(store_call_list_t list) {
    if (list >= STORE_CALL_LIST_COUNT) {
        return 0;
    }
    return s_calls[list].count;
}

store_status_t store_call_get(store_call_list_t list, uint8_t index, store_call_record_t *out_record) {
    if (list >= STORE_CALL_LIST_COUNT || out_record == 0) {
        return STORE_STATUS_INVALID_ARGUMENT;
    }
    if (index >= s_calls[list].count) {
        return STORE_STATUS_NOT_FOUND;
    }
    *out_record = s_calls[list].records[index];
    return STORE_STATUS_OK;
}

store_status_t store_call_add(store_call_list_t list, const store_call_record_t *record, uint32_t *out_id) {
    if (list >= STORE_CALL_LIST_COUNT || record == 0) {
        return STORE_STATUS_INVALID_ARGUMENT;
    }
    call_list_state_t *state = &s_calls[list];
    uint8_t max_index = state->count < STORE_CALL_LIST_LIMIT ? state->count : (STORE_CALL_LIST_LIMIT - 1u);
    for (uint8_t i = max_index; i > 0; i--) {
        state->records[i] = state->records[i - 1u];
    }

    store_call_record_t stored = *record;
    stored.id = state->next_id++;
    if (state->next_id == 0u) {
        state->next_id = 1u;
    }
    stored.number[STORE_CALL_NUMBER_MAX] = '\0';
    stored.name[STORE_CALL_NAME_MAX] = '\0';
    state->records[0] = stored;
    if (state->count < STORE_CALL_LIST_LIMIT) {
        state->count++;
    }
    if (out_id != 0) {
        *out_id = stored.id;
    }
    return mark_call_list_dirty(list);
}

store_status_t store_call_add_now(store_call_list_t list,
                                  const char *number,
                                  const char *name,
                                  uint32_t duration_seconds,
                                  store_call_reason_t reason,
                                  uint32_t *out_id) {
    store_call_record_t record;
    memset(&record, 0, sizeof(record));
    store_copy_text(record.number, sizeof(record.number), number == 0 ? "" : number);
    store_copy_text(record.name, sizeof(record.name), name == 0 ? "" : name);
    record.duration_seconds = duration_seconds;
    record.reason = reason;
    rtc_alarm_hal_get_datetime(&record.datetime);
    return store_call_add(list, &record, out_id);
}

store_status_t store_call_update_number(store_call_list_t list, uint8_t index, const char *number) {
    if (list >= STORE_CALL_LIST_COUNT || number == 0) {
        return STORE_STATUS_INVALID_ARGUMENT;
    }
    call_list_state_t *state = &s_calls[list];
    if (index >= state->count) {
        return STORE_STATUS_NOT_FOUND;
    }
    store_copy_text(state->records[index].number, sizeof(state->records[index].number), number);
    state->records[index].number[STORE_CALL_NUMBER_MAX] = '\0';
    return mark_call_list_dirty(list);
}

store_status_t store_call_update_result(store_call_list_t list,
                                        uint32_t id,
                                        uint32_t duration_seconds,
                                        store_call_reason_t reason) {
    if (list >= STORE_CALL_LIST_COUNT || id == 0u) {
        return STORE_STATUS_INVALID_ARGUMENT;
    }
    call_list_state_t *state = &s_calls[list];
    for (uint8_t i = 0; i < state->count; i++) {
        if (state->records[i].id != id) {
            continue;
        }
        state->records[i].duration_seconds = duration_seconds;
        state->records[i].reason = reason;
        return mark_call_list_dirty(list);
    }
    return STORE_STATUS_NOT_FOUND;
}

store_status_t store_call_delete(store_call_list_t list, uint8_t index) {
    if (list >= STORE_CALL_LIST_COUNT) {
        return STORE_STATUS_INVALID_ARGUMENT;
    }
    call_list_state_t *state = &s_calls[list];
    if (index >= state->count) {
        return STORE_STATUS_NOT_FOUND;
    }
    for (uint8_t i = index; i + 1u < state->count; i++) {
        state->records[i] = state->records[i + 1u];
    }
    memset(&state->records[state->count - 1u], 0, sizeof(state->records[0]));
    state->count--;
    return mark_call_list_dirty(list);
}

store_status_t store_call_clear(store_call_list_t list) {
    if (list >= STORE_CALL_LIST_COUNT) {
        return STORE_STATUS_INVALID_ARGUMENT;
    }
    memset(&s_calls[list], 0, sizeof(s_calls[list]));
    s_calls[list].next_id = 1u;
    return mark_call_list_dirty(list);
}

uint32_t store_life_timer_seconds(void) {
    uint32_t lifetime = 0u;
    (void)store_setting_get_u32(STORE_SETTING_CALL_DURATION_LIFETIME, &lifetime);
    return lifetime;
}

store_status_t store_life_timer_add_seconds(uint32_t seconds) {
    if (seconds == 0u) {
        return STORE_STATUS_OK;
    }
    uint32_t before = store_life_timer_seconds();
    uint32_t after = before + seconds;
    if (after < before) {
        after = UINT32_MAX;
    }
    return store_setting_set_u32(STORE_SETTING_CALL_DURATION_LIFETIME, after);
}

void store_calls_migrate_life_timer(uint32_t warranty_donor) {
    uint32_t lifetime = store_life_timer_seconds();
    uint32_t all_calls = 0u;
    (void)store_setting_get_u32(STORE_SETTING_CALL_DURATION_ALL, &all_calls);
    uint32_t migrated = lifetime;
    if (all_calls > migrated) {
        migrated = all_calls;
    }
    if (warranty_donor > migrated) {
        migrated = warranty_donor;
    }
    if (migrated != lifetime) {
        (void)store_setting_set_u32(STORE_SETTING_CALL_DURATION_LIFETIME, migrated);
        LOGI("store", "migrated Life timer: %lu seconds",
             (unsigned long)migrated);
    }
}

static store_unit_t unit_for_call_list(store_call_list_t list) {
    return (store_unit_t)((uint8_t)STORE_UNIT_CALLS_MISSED + (uint8_t)list);
}

static store_status_t mark_call_list_dirty(store_call_list_t list) {
    if (list >= STORE_CALL_LIST_COUNT) {
        return STORE_STATUS_INVALID_ARGUMENT;
    }
    return store_engine_mark_dirty(unit_for_call_list(list));
}

static void reset_calls_unit(uint8_t instance) {
    if (instance >= STORE_CALL_LIST_COUNT) {
        return;
    }
    memset(&s_calls[instance], 0, sizeof(s_calls[instance]));
    s_calls[instance].next_id = 1u;
}

static bool serialize_call_list(store_call_list_t list, uint8_t *dst, size_t cap, size_t *out_len) {
    const call_list_state_t *state = &s_calls[list];
    size_t pos = 0;
    if (!write_u32_field(dst, cap, &pos, CALLS_MAGIC) ||
        !write_u16_field(dst, cap, &pos, CALLS_PAYLOAD_VERSION) ||
        !write_u8_field(dst, cap, &pos, (uint8_t)list) ||
        !write_u8_field(dst, cap, &pos, state->count) ||
        !write_u32_field(dst, cap, &pos, state->next_id)) {
        return false;
    }
    for (uint8_t i = 0; i < state->count; i++) {
        const store_call_record_t *record = &state->records[i];
        uint8_t number_len = (uint8_t)strnlen(record->number, STORE_CALL_NUMBER_MAX);
        uint8_t name_len = (uint8_t)strnlen(record->name, STORE_CALL_NAME_MAX);
        uint8_t fixed_number[STORE_CALL_NUMBER_MAX + 1u];
        uint8_t fixed_name[STORE_CALL_NAME_MAX + 1u];
        memset(fixed_number, 0, sizeof(fixed_number));
        memset(fixed_name, 0, sizeof(fixed_name));
        memcpy(fixed_number, record->number, number_len);
        memcpy(fixed_name, record->name, name_len);
        if (!write_u32_field(dst, cap, &pos, record->id) ||
            !write_u32_field(dst, cap, &pos, record->duration_seconds) ||
            !write_u32_field(dst, cap, &pos, pack_datetime(&record->datetime)) ||
            !write_u8_field(dst, cap, &pos, (uint8_t)record->reason) ||
            !write_u8_field(dst, cap, &pos, number_len) ||
            !write_u8_field(dst, cap, &pos, name_len) ||
            !write_u8_field(dst, cap, &pos, datetime_year_code(&record->datetime)) ||
            !write_bytes(dst, cap, &pos, fixed_number, sizeof(fixed_number)) ||
            !write_bytes(dst, cap, &pos, fixed_name, sizeof(fixed_name))) {
            return false;
        }
    }
    *out_len = pos;
    return true;
}

static bool apply_call_payload(store_call_list_t list, const uint8_t *payload, size_t len) {
    if (len < 12u || read_u32(&payload[0]) != CALLS_MAGIC ||
        payload[6] != (uint8_t)list) {
        return false;
    }
    uint16_t version = read_u16(&payload[4]);
    if (version != CALLS_LEGACY_PAYLOAD_VERSION &&
        version != CALLS_PAYLOAD_VERSION) {
        return false;
    }
    uint8_t count = payload[7];
    if (count > STORE_CALL_LIST_LIMIT) {
        return false;
    }
    call_list_state_t loaded;
    memset(&loaded, 0, sizeof(loaded));
    loaded.count = count;
    loaded.next_id = read_u32(&payload[8]);
    if (loaded.next_id == 0u) {
        loaded.next_id = 1u;
    }
    size_t pos = 12u;
    for (uint8_t i = 0; i < count; i++) {
        if (pos + 16u + STORE_CALL_NUMBER_MAX + 1u + STORE_CALL_NAME_MAX + 1u > len) {
            return false;
        }
        store_call_record_t *record = &loaded.records[i];
        record->id = read_u32(&payload[pos]);
        pos += 4u;
        record->duration_seconds = read_u32(&payload[pos]);
        pos += 4u;
        unpack_datetime(read_u32(&payload[pos]), &record->datetime);
        pos += 4u;
        record->reason = (store_call_reason_t)payload[pos++];
        uint8_t number_len = payload[pos++];
        uint8_t name_len = payload[pos++];
        uint8_t year_code = payload[pos++];
        if (number_len > STORE_CALL_NUMBER_MAX || name_len > STORE_CALL_NAME_MAX) {
            return false;
        }
        if (version == CALLS_PAYLOAD_VERSION) {
            if (year_code > CALLS_YEAR_CODE_MAX) {
                return false;
            }
            record->datetime.year = (uint16_t)(CALLS_FIRST_YEAR + year_code);
        }
        memcpy(record->number, &payload[pos], number_len);
        record->number[number_len] = '\0';
        pos += STORE_CALL_NUMBER_MAX + 1u;
        memcpy(record->name, &payload[pos], name_len);
        record->name[name_len] = '\0';
        pos += STORE_CALL_NAME_MAX + 1u;
    }
    s_calls[list] = loaded;
    return true;
}

static uint32_t pack_datetime(const rtc_datetime_t *dt) {
    uint16_t year = dt->year < 2000u ? 2000u : dt->year;
    uint32_t year_offset = (uint32_t)(year - 2000u);
    if (year_offset > 63u) {
        year_offset = 63u;
    }
    return (year_offset << 26) |
           ((uint32_t)(dt->month & 0x0fu) << 22) |
           ((uint32_t)(dt->day & 0x1fu) << 17) |
           ((uint32_t)(dt->hour & 0x1fu) << 12) |
           ((uint32_t)(dt->minute & 0x3fu) << 6) |
           (uint32_t)(dt->second & 0x3fu);
}

static void unpack_datetime(uint32_t packed, rtc_datetime_t *dt) {
    dt->year = (uint16_t)(2000u + ((packed >> 26) & 0x3fu));
    dt->month = (uint8_t)((packed >> 22) & 0x0fu);
    dt->day = (uint8_t)((packed >> 17) & 0x1fu);
    dt->hour = (uint8_t)((packed >> 12) & 0x1fu);
    dt->minute = (uint8_t)((packed >> 6) & 0x3fu);
    dt->second = (uint8_t)(packed & 0x3fu);
}

static uint8_t datetime_year_code(const rtc_datetime_t *dt) {
    uint16_t year = dt->year;
    if (year < CALLS_FIRST_YEAR) {
        year = CALLS_FIRST_YEAR;
    } else if (year > CALLS_LAST_YEAR) {
        year = CALLS_LAST_YEAR;
    }
    return (uint8_t)(year - CALLS_FIRST_YEAR);
}

static bool serialize_calls_unit(uint8_t instance,
                                 uint8_t *dst,
                                 size_t cap,
                                 size_t *out_len) {
    return serialize_call_list((store_call_list_t)instance, dst, cap, out_len);
}

static bool apply_calls_unit(uint8_t instance, const uint8_t *payload, size_t len) {
    return apply_call_payload((store_call_list_t)instance, payload, len);
}

const store_unit_ops_t g_store_calls_unit_ops = {
    .reset_ram = reset_calls_unit,
    .serialize = serialize_calls_unit,
    .apply = apply_calls_unit,
    .fallback_missing_or_corrupt = 0,
    .name = "calls",
};

#include "storage/store_service.h"

#include "storage_bytes.h"
#include "store_service_internal.h"

#include <string.h>

#define OWN_TONES_MAGIC 0x4f544e31u

typedef struct {
    store_own_tone_t slots[STORE_OWN_TONE_SLOT_COUNT];
} own_tone_state_t;

static own_tone_state_t s_own_tones;

static void reset_tones_unit(uint8_t instance) {
    (void)instance;
    memset(&s_own_tones, 0, sizeof(s_own_tones));
}

static bool serialize_tones_unit(uint8_t instance,
                                 uint8_t *dst,
                                 size_t cap,
                                 size_t *out_len) {
    (void)instance;
    size_t pos = 0u;
    uint8_t count = 0u;
    for (uint8_t i = 0u; i < STORE_OWN_TONE_SLOT_COUNT; i++) {
        if (s_own_tones.slots[i].used) {
            count++;
        }
    }
    if (!write_u32_field(dst, cap, &pos, OWN_TONES_MAGIC) ||
        !write_u16_field(dst, cap, &pos, STORE_PAYLOAD_VERSION) ||
        !write_u8_field(dst, cap, &pos, count) ||
        !write_u8_field(dst, cap, &pos, 0u)) {
        return false;
    }
    for (uint8_t i = 0u; i < STORE_OWN_TONE_SLOT_COUNT; i++) {
        const store_own_tone_t *slot = &s_own_tones.slots[i];
        uint8_t fixed_name[STORE_OWN_TONE_NAME_MAX + 1u];
        uint8_t fixed_notes[STORE_OWN_TONE_NOTES_MAX + 1u];
        uint8_t fixed_packed[STORE_OWN_TONE_PACKED_MAX];
        uint8_t name_len =
            (uint8_t)strnlen(slot->name, STORE_OWN_TONE_NAME_MAX);
        uint16_t notes_len =
            (uint16_t)strnlen(slot->notes, STORE_OWN_TONE_NOTES_MAX);
        uint16_t packed_len = slot->packed_len > STORE_OWN_TONE_PACKED_MAX
                                  ? STORE_OWN_TONE_PACKED_MAX
                                  : slot->packed_len;
        memset(fixed_name, 0, sizeof(fixed_name));
        memset(fixed_notes, 0, sizeof(fixed_notes));
        memset(fixed_packed, 0, sizeof(fixed_packed));
        memcpy(fixed_name, slot->name, name_len);
        memcpy(fixed_notes, slot->notes, notes_len);
        memcpy(fixed_packed, slot->packed, packed_len);
        if (!write_u8_field(dst, cap, &pos, slot->used ? 1u : 0u) ||
            !write_u8_field(dst, cap, &pos, slot->tempo_index) ||
            !write_u8_field(dst, cap, &pos, name_len) ||
            !write_u16_field(dst, cap, &pos, notes_len) ||
            !write_u16_field(dst, cap, &pos, packed_len) ||
            !write_bytes(dst, cap, &pos, fixed_name, sizeof(fixed_name)) ||
            !write_bytes(dst, cap, &pos, fixed_notes, sizeof(fixed_notes)) ||
            !write_bytes(dst, cap, &pos, fixed_packed, sizeof(fixed_packed))) {
            return false;
        }
    }
    *out_len = pos;
    return true;
}

static bool apply_tones_unit(uint8_t instance,
                             const uint8_t *payload,
                             size_t len) {
    (void)instance;
    if (len < 8u || read_u32(&payload[0]) != OWN_TONES_MAGIC ||
        read_u16(&payload[4]) != STORE_PAYLOAD_VERSION) {
        return false;
    }
    const size_t old_slot_bytes =
        5u + STORE_OWN_TONE_NAME_MAX + 1u + STORE_OWN_TONE_NOTES_MAX + 1u;
    const size_t new_slot_bytes =
        old_slot_bytes + 2u + STORE_OWN_TONE_PACKED_MAX;
    const bool has_packed_bytes =
        len >= 8u + STORE_OWN_TONE_SLOT_COUNT * new_slot_bytes;
    const size_t slot_bytes =
        has_packed_bytes ? new_slot_bytes : old_slot_bytes;
    if (len < 8u + STORE_OWN_TONE_SLOT_COUNT * slot_bytes) {
        return false;
    }
    own_tone_state_t loaded;
    memset(&loaded, 0, sizeof(loaded));
    size_t pos = 8u;
    for (uint8_t i = 0u; i < STORE_OWN_TONE_SLOT_COUNT; i++) {
        store_own_tone_t *slot = &loaded.slots[i];
        size_t slot_start = pos;
        slot->used = payload[pos++] != 0u;
        slot->tempo_index = payload[pos++];
        uint8_t name_len = payload[pos++];
        uint16_t notes_len = read_u16(&payload[pos]);
        pos += 2u;
        uint16_t packed_len = 0u;
        if (has_packed_bytes) {
            packed_len = read_u16(&payload[pos]);
            pos += 2u;
        }
        if (name_len > STORE_OWN_TONE_NAME_MAX ||
            notes_len > STORE_OWN_TONE_NOTES_MAX ||
            packed_len > STORE_OWN_TONE_PACKED_MAX) {
            return false;
        }
        memcpy(slot->name, &payload[pos], name_len);
        slot->name[name_len] = '\0';
        pos += STORE_OWN_TONE_NAME_MAX + 1u;
        memcpy(slot->notes, &payload[pos], notes_len);
        slot->notes[notes_len] = '\0';
        pos += STORE_OWN_TONE_NOTES_MAX + 1u;
        if (has_packed_bytes) {
            memcpy(slot->packed, &payload[pos], packed_len);
            slot->packed_len = packed_len;
        }
        pos = slot_start + slot_bytes;
    }
    s_own_tones = loaded;
    return true;
}

const store_unit_ops_t g_store_tones_unit_ops = {
    .reset_ram = reset_tones_unit,
    .serialize = serialize_tones_unit,
    .apply = apply_tones_unit,
    .fallback_missing_or_corrupt = 0,
    .name = "own tones",
};

store_status_t store_own_tone_get(uint8_t slot, store_own_tone_t *out_tone) {
    if (slot >= STORE_OWN_TONE_SLOT_COUNT || out_tone == 0) {
        return STORE_STATUS_INVALID_ARGUMENT;
    }
    if (!s_own_tones.slots[slot].used) {
        return STORE_STATUS_NOT_FOUND;
    }
    *out_tone = s_own_tones.slots[slot];
    return STORE_STATUS_OK;
}

store_status_t store_own_tone_set(uint8_t slot, const store_own_tone_t *tone) {
    if (slot >= STORE_OWN_TONE_SLOT_COUNT || tone == 0) {
        return STORE_STATUS_INVALID_ARGUMENT;
    }
    s_own_tones.slots[slot] = *tone;
    s_own_tones.slots[slot].used = true;
    s_own_tones.slots[slot].name[STORE_OWN_TONE_NAME_MAX] = '\0';
    s_own_tones.slots[slot].notes[STORE_OWN_TONE_NOTES_MAX] = '\0';
    if (s_own_tones.slots[slot].packed_len > STORE_OWN_TONE_PACKED_MAX) {
        s_own_tones.slots[slot].packed_len = STORE_OWN_TONE_PACKED_MAX;
    }
    memset(&s_own_tones.slots[slot]
                .packed[s_own_tones.slots[slot].packed_len],
           0,
           STORE_OWN_TONE_PACKED_MAX - s_own_tones.slots[slot].packed_len);
    return store_engine_mark_dirty(STORE_UNIT_OWN_TONES);
}

store_status_t store_own_tone_clear(uint8_t slot) {
    if (slot >= STORE_OWN_TONE_SLOT_COUNT) {
        return STORE_STATUS_INVALID_ARGUMENT;
    }
    memset(&s_own_tones.slots[slot], 0, sizeof(s_own_tones.slots[slot]));
    return store_engine_mark_dirty(STORE_UNIT_OWN_TONES);
}

bool store_own_tone_used(uint8_t slot) {
    return slot < STORE_OWN_TONE_SLOT_COUNT && s_own_tones.slots[slot].used;
}

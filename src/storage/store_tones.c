#include "storage/store_service.h"
#include "audio/ringtone_codec.h"
#include "services/timebase.h"
#include "services/message_file_codec.h"

#include "storage_bytes.h"
#include "store_service_internal.h"

#include <string.h>

#define OWN_TONES_MAGIC 0x4f544e31u
#define RECEIVED_TONES_MAGIC_V1 0x31544e52u
#define RECEIVED_TONES_MAGIC 0x32544e52u
#define PENDING_COUNT 2u
#define RECEIVE_TTL_MS (30u * 60u * 1000u)

enum { TONE_FREE, TONE_PARTIAL, TONE_READY, TONE_CONSUMED, TONE_INVALID };
typedef struct {
    uint32_t id;
    uint16_t reference, source_port;
    uint8_t state, total, seen, dcs, flags;
    uint8_t lengths[MODEM_SMS_SEGMENT_MAX];
    uint32_t timestamps[MODEM_SMS_SEGMENT_MAX];
    char sender[MODEM_SMS_SENDER_MAX + 1u];
    char timestamp[MODEM_SMS_TIMESTAMP_MAX + 1u];
    uint8_t data[STORE_OWN_TONE_PACKED_MAX];
} pending_tone_t;

typedef struct {
    store_own_tone_t slots[STORE_OWN_TONE_SLOT_COUNT];
    uint32_t next_id;
    pending_tone_t pending[PENDING_COUNT];
} own_tone_state_t;

static own_tone_state_t s_own_tones;
static uint32_t s_touched[PENDING_COUNT];
static sms_codec_message_t s_part;
static bool serialize_pending(uint8_t *dst, size_t cap, size_t *pos);
static bool apply_pending(own_tone_state_t *loaded, const uint8_t *data, size_t len);

static void reset_tones_unit(uint8_t instance) {
    (void)instance;
    memset(&s_own_tones, 0, sizeof(s_own_tones));
    memset(s_touched, 0, sizeof(s_touched));
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
    if (s_own_tones.next_id != 0u && !serialize_pending(dst, cap, &pos)) return false;
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
    /* Store callbacks run serially on core 0, not on the IRQ stack. */
    static own_tone_state_t loaded;
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
    if (pos != len && !apply_pending(&loaded, payload + pos, len - pos)) return false;
    s_own_tones = loaded;
    for (unsigned i = 0u; i < PENDING_COUNT; i++) s_touched[i] = time_ms();
    return true;
}

const store_unit_ops_t g_store_tones_unit_ops = {
    .reset_ram = reset_tones_unit,
    .serialize = serialize_tones_unit,
    .apply = apply_tones_unit,
    .fallback_missing = 0,
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
    store_status_t status = store_engine_mark_dirty(STORE_UNIT_OWN_TONES);
    if (status != STORE_STATUS_OK) return status;
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
    return STORE_STATUS_OK;
}

store_status_t store_own_tone_clear(uint8_t slot) {
    if (slot >= STORE_OWN_TONE_SLOT_COUNT) {
        return STORE_STATUS_INVALID_ARGUMENT;
    }
    store_status_t status = store_engine_mark_dirty(STORE_UNIT_OWN_TONES);
    if (status != STORE_STATUS_OK) return status;
    memset(&s_own_tones.slots[slot], 0, sizeof(s_own_tones.slots[slot]));
    return STORE_STATUS_OK;
}

bool store_own_tone_used(uint8_t slot) {
    return slot < STORE_OWN_TONE_SLOT_COUNT && s_own_tones.slots[slot].used;
}

static uint16_t pending_size(const pending_tone_t *p) {
    uint16_t size = 0u;
    for (unsigned i = 0u; i < MODEM_SMS_SEGMENT_MAX; i++) size += p->lengths[i];
    return size;
}

store_status_t store_ringtone_commit_status(void) {
    store_diag_snapshot_t diag;
    store_service_get_diag(&diag);
    if (!diag.ready) return STORE_STATUS_NOT_READY;
    uint32_t mask = 1u << STORE_UNIT_OWN_TONES;
    if ((diag.degraded_mask | diag.unavailable_mask) & mask) return STORE_STATUS_STORAGE_ERROR;
    return diag.dirty_mask & mask ? STORE_STATUS_NOT_READY : STORE_STATUS_OK;
}

void store_ringtone_expire(uint32_t now_ms) {
    for (unsigned i = 0u; i < PENDING_COUNT; i++) {
        pending_tone_t *p = &s_own_tones.pending[i];
        if (p->state != TONE_FREE && p->state != TONE_READY &&
            (uint32_t)(now_ms - s_touched[i]) >= RECEIVE_TTL_MS &&
            store_ringtone_commit_status() == STORE_STATUS_OK &&
            store_engine_mark_dirty(STORE_UNIT_OWN_TONES) == STORE_STATUS_OK)
            memset(p, 0, sizeof(*p));
    }
}

static bool valid_part(const sms_codec_message_t *p) {
    uint8_t total = p->has_concat ? p->concat_total : 1u;
    uint8_t seq = p->has_concat ? p->concat_seq : 1u;
    return !p->submit && p->binary && p->has_ports && p->dest_port == RINGTONE_SMS_PORT &&
        !p->udh_unhandled && !p->trailing_data && p->pid == 0u &&
        (p->dcs == 4u || p->dcs == 0xf5u) && p->binary_len != 0u &&
        p->binary_len <= 140u && p->address[0] != '\0' &&
        total != 0u && total <= MODEM_SMS_SEGMENT_MAX && seq != 0u && seq <= total;
}

static bool timestamp_matches(const char *a, const char *b, bool concat) {
    if (strcmp(a, b) == 0) return true;
    if (!concat) return false;
    uint32_t ta = message_timestamp_seconds(a), tb = message_timestamp_seconds(b);
    uint32_t gap = ta > tb ? ta - tb : tb - ta;
    return ta != 0u && tb != 0u && gap < RECEIVE_TTL_MS / 1000u;
}

static unsigned identity_match_rank(const pending_tone_t *p, const sms_codec_message_t *part) {
    uint8_t flags = (part->has_concat ? 1u : 0u) | (part->concat_ref_16bit ? 2u : 0u);
    if (p->state == TONE_FREE || p->reference != part->concat_ref ||
        p->source_port != part->source_port || p->flags != flags ||
        strcmp(p->sender, part->address) != 0) return 0u;
    unsigned index = part->has_concat ? part->concat_seq - 1u : 0u;
    bool complete = p->state == TONE_READY || p->state == TONE_CONSUMED;
    if (complete && (p->seen & (1u << index)) == 0u) return 0u;
    uint32_t timestamp = message_timestamp_seconds(part->timestamp);
    if ((p->seen & (1u << index)) != 0u && p->timestamps[index] != 0u) {
        if (p->timestamps[index] == timestamp) return 2u;
        /* Completed receipts deduplicate individual transmissions, not every
         * later use of the same concatenation reference within the TTL. */
        if (complete) return 0u;
    }
    return timestamp_matches(p->timestamp, part->timestamp, part->has_concat) ? 1u : 0u;
}

store_status_t store_ringtone_receive(const sms_codec_message_t *part, uint32_t now_ms) {
    if (part == NULL || !valid_part(part)) return STORE_STATUS_INVALID_ARGUMENT;
    if (!store_service_ready()) return STORE_STATUS_NOT_READY;
    store_ringtone_expire(now_ms);
    pending_tone_t *p = NULL, *available = NULL;
    unsigned rank = 0u;
    for (unsigned i = 0u; i < PENDING_COUNT; i++) {
        pending_tone_t *candidate = &s_own_tones.pending[i];
        unsigned candidate_rank = identity_match_rank(candidate, part);
        if (candidate_rank > rank) { p = candidate; rank = candidate_rank; }
        if (rank == 2u) break;
        if (candidate->state == TONE_FREE ||
            (available == NULL && candidate->state == TONE_CONSUMED &&
                store_ringtone_commit_status() == STORE_STATUS_OK))
            available = candidate;
    }
    uint8_t total = part->has_concat ? part->concat_total : 1u;
    uint8_t seq = part->has_concat ? part->concat_seq : 1u;
    if (p != NULL && p->state == TONE_INVALID) return STORE_STATUS_CONFLICT;
    uint16_t offset = 0u, size = p != NULL ? pending_size(p) : 0u;
    if (p != NULL) {
        for (unsigned i = 0u; i + 1u < seq; i++) offset += p->lengths[i];
        bool duplicate = (p->seen & (1u << (seq - 1u))) != 0u;
        if (p->total != total || p->dcs != part->dcs ||
            (duplicate && (p->lengths[seq - 1u] != part->binary_len ||
                memcmp(p->data + offset, part->binary_data, part->binary_len) != 0))) {
            if (p->state == TONE_PARTIAL && store_engine_mark_dirty(STORE_UNIT_OWN_TONES) == STORE_STATUS_OK)
                p->state = TONE_INVALID;
            return STORE_STATUS_CONFLICT;
        }
        if (duplicate) return STORE_STATUS_OK;
    }
    if (size + part->binary_len > STORE_OWN_TONE_PACKED_MAX) return STORE_STATUS_INVALID_ARGUMENT;
    if (p == NULL && available == NULL) return STORE_STATUS_STORAGE_ERROR;
    store_status_t status = store_engine_mark_dirty(STORE_UNIT_OWN_TONES);
    if (status != STORE_STATUS_OK) return status;
    if (p == NULL) {
        p = available;
        memset(p, 0, sizeof(*p));
        p->state = TONE_PARTIAL;
        p->id = ++s_own_tones.next_id;
        if (p->id == 0u) p->id = ++s_own_tones.next_id;
        p->reference = part->concat_ref;
        p->source_port = part->source_port;
        p->total = total;
        p->dcs = part->dcs;
        p->flags = (part->has_concat ? 1u : 0u) | (part->concat_ref_16bit ? 2u : 0u);
        store_copy_text(p->sender, sizeof(p->sender), part->address);
        store_copy_text(p->timestamp, sizeof(p->timestamp), part->timestamp);
    }
    s_touched[p - s_own_tones.pending] = now_ms;
    memmove(p->data + offset + part->binary_len, p->data + offset, size - offset);
    memcpy(p->data + offset, part->binary_data, part->binary_len);
    p->lengths[seq - 1u] = (uint8_t)part->binary_len;
    p->timestamps[seq - 1u] = message_timestamp_seconds(part->timestamp);
    p->seen |= (uint8_t)(1u << (seq - 1u));
    if (p->seen == (uint8_t)((1u << total) - 1u)) {
        ringtone_info_t info;
        if (!ringtone_decode(p->data, pending_size(p), &info, NULL, 0u)) {
            p->state = TONE_INVALID;
            return STORE_STATUS_INVALID_ARGUMENT;
        }
        p->state = TONE_READY;
    }
    return STORE_STATUS_OK;
}

store_status_t store_ringtone_receive_pdu(const char *pdu, uint32_t now_ms) {
    if (!sms_pdu_decode(pdu, &s_part) || !s_part.has_ports || s_part.dest_port != RINGTONE_SMS_PORT)
        return STORE_STATUS_NOT_FOUND;
    return store_ringtone_receive(&s_part, now_ms);
}

store_status_t store_ringtone_received_pdu_status(const char *pdu) {
    if (!sms_pdu_decode(pdu, &s_part) || !s_part.has_ports || s_part.dest_port != RINGTONE_SMS_PORT)
        return STORE_STATUS_NOT_FOUND;
    if (!valid_part(&s_part)) return STORE_STATUS_INVALID_ARGUMENT;
    store_status_t status = store_ringtone_commit_status();
    if (status != STORE_STATUS_OK) return status;
    uint8_t total = s_part.has_concat ? s_part.concat_total : 1u;
    uint8_t seq = s_part.has_concat ? s_part.concat_seq : 1u;
    for (unsigned i = 0u; i < PENDING_COUNT; i++) {
        const pending_tone_t *p = &s_own_tones.pending[i];
        if (identity_match_rank(p, &s_part) == 0u || p->state == TONE_INVALID || p->total != total ||
            p->dcs != s_part.dcs || !(p->seen & (1u << (seq - 1u)))) continue;
        uint16_t offset = 0u;
        for (unsigned j = 0u; j + 1u < seq; j++) offset += p->lengths[j];
        if (p->lengths[seq - 1u] == s_part.binary_len &&
            memcmp(p->data + offset, s_part.binary_data, s_part.binary_len) == 0) return STORE_STATUS_OK;
    }
    return STORE_STATUS_NOT_FOUND;
}

static pending_tone_t *pending_by_id(uint32_t id) {
    for (unsigned i = 0u; id != 0u && i < PENDING_COUNT; i++) {
        pending_tone_t *p = &s_own_tones.pending[i];
        if (p->id == id && (p->state == TONE_READY || p->state == TONE_CONSUMED)) return p;
    }
    return NULL;
}

uint32_t store_ringtone_pending_first(void) {
    if (store_ringtone_commit_status() != STORE_STATUS_OK) return 0u;
    uint32_t first = 0u;
    for (unsigned i = 0u; i < PENDING_COUNT; i++) {
        const pending_tone_t *p = &s_own_tones.pending[i];
        if (p->state == TONE_READY && (first == 0u || (int32_t)(p->id - first) < 0)) first = p->id;
    }
    return first;
}

store_status_t store_ringtone_pending_get(uint32_t id, store_own_tone_t *out) {
    pending_tone_t *p = pending_by_id(id);
    if (p == NULL) return STORE_STATUS_NOT_FOUND;
    if (out == NULL) return STORE_STATUS_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    out->used = true;
    out->packed_len = pending_size(p);
    memcpy(out->packed, p->data, out->packed_len);
    return STORE_STATUS_OK;
}

store_status_t store_ringtone_pending_save(uint32_t id) {
    pending_tone_t *p = pending_by_id(id);
    if (p == NULL) return STORE_STATUS_NOT_FOUND;
    store_status_t status = store_engine_mark_dirty(STORE_UNIT_OWN_TONES);
    if (status != STORE_STATUS_OK) return status;
    (void)store_ringtone_pending_get(id, &s_own_tones.slots[1]);
    p->state = TONE_CONSUMED;
    return STORE_STATUS_OK;
}

store_status_t store_ringtone_pending_discard(uint32_t id) {
    pending_tone_t *p = pending_by_id(id);
    if (p == NULL) return STORE_STATUS_NOT_FOUND;
    store_status_t status = store_engine_mark_dirty(STORE_UNIT_OWN_TONES);
    if (status == STORE_STATUS_OK) p->state = TONE_CONSUMED;
    return status;
}

static bool serialize_pending(uint8_t *dst, size_t cap, size_t *pos) {
    if (!write_u32_field(dst, cap, pos, RECEIVED_TONES_MAGIC) ||
        !write_u32_field(dst, cap, pos, s_own_tones.next_id)) return false;
    for (unsigned i = 0u; i < PENDING_COUNT; i++) {
        const pending_tone_t *p = &s_own_tones.pending[i];
        if (!write_u32_field(dst, cap, pos, p->id) ||
            !write_u16_field(dst, cap, pos, p->reference) ||
            !write_u16_field(dst, cap, pos, p->source_port) ||
            !write_u8_field(dst, cap, pos, p->state) ||
            !write_u8_field(dst, cap, pos, p->total) ||
            !write_u8_field(dst, cap, pos, p->seen) ||
            !write_u8_field(dst, cap, pos, p->dcs) ||
            !write_u8_field(dst, cap, pos, p->flags) ||
            !write_bytes(dst, cap, pos, p->lengths, sizeof(p->lengths))) return false;
        for (unsigned j = 0u; j < MODEM_SMS_SEGMENT_MAX; j++) {
            if (!write_u32_field(dst, cap, pos, p->timestamps[j])) return false;
        }
        if (!write_bytes(dst, cap, pos, p->sender, sizeof(p->sender)) ||
            !write_bytes(dst, cap, pos, p->timestamp, sizeof(p->timestamp)) ||
            !write_bytes(dst, cap, pos, p->data, sizeof(p->data))) return false;
    }
    return true;
}

static bool apply_pending(own_tone_state_t *loaded, const uint8_t *data, size_t len) {
    if (len < 8u) return false;
    uint32_t magic = read_u32(data);
    bool has_timestamps = magic == RECEIVED_TONES_MAGIC;
    if (!has_timestamps && magic != RECEIVED_TONES_MAGIC_V1) return false;
    const size_t record_size = 13u + MODEM_SMS_SEGMENT_MAX +
        (has_timestamps ? 4u * MODEM_SMS_SEGMENT_MAX : 0u) + MODEM_SMS_SENDER_MAX + 1u +
        MODEM_SMS_TIMESTAMP_MAX + 1u + STORE_OWN_TONE_PACKED_MAX;
    if (len != 8u + PENDING_COUNT * record_size) return false;
    loaded->next_id = read_u32(data + 4u);
    size_t pos = 8u;
    for (unsigned i = 0u; i < PENDING_COUNT; i++) {
        pending_tone_t *p = &loaded->pending[i];
        p->id = read_u32(data + pos); pos += 4u;
        p->reference = read_u16(data + pos); pos += 2u;
        p->source_port = read_u16(data + pos); pos += 2u;
        p->state = data[pos++]; p->total = data[pos++]; p->seen = data[pos++];
        p->dcs = data[pos++]; p->flags = data[pos++];
        memcpy(p->lengths, data + pos, sizeof(p->lengths)); pos += sizeof(p->lengths);
        /* V1 receipts have only an assembly timestamp. Preserve them with
         * conservative deduplication until expired or replaced. */
        if (has_timestamps) {
            for (unsigned j = 0u; j < MODEM_SMS_SEGMENT_MAX; j++) {
                p->timestamps[j] = read_u32(data + pos); pos += 4u;
            }
        }
        memcpy(p->sender, data + pos, sizeof(p->sender)); pos += sizeof(p->sender);
        memcpy(p->timestamp, data + pos, sizeof(p->timestamp)); pos += sizeof(p->timestamp);
        memcpy(p->data, data + pos, sizeof(p->data)); pos += sizeof(p->data);
        if (p->state > TONE_INVALID || p->flags > 3u ||
            p->sender[MODEM_SMS_SENDER_MAX] != '\0' || p->timestamp[MODEM_SMS_TIMESTAMP_MAX] != '\0' ||
            pending_size(p) > sizeof(p->data)) return false;
        if (p->state == TONE_FREE) continue;
        if (p->id == 0u || p->total == 0u || p->total > MODEM_SMS_SEGMENT_MAX ||
            (p->dcs != 4u && p->dcs != 0xf5u) || (p->seen >> p->total) != 0u) return false;
        for (unsigned j = 0u; j < MODEM_SMS_SEGMENT_MAX; j++) {
            if (p->lengths[j] > 140u || ((p->seen & (1u << j)) != 0u) != (p->lengths[j] != 0u) ||
                (p->lengths[j] == 0u && p->timestamps[j] != 0u))
                return false;
        }
        if (p->state == TONE_READY || p->state == TONE_CONSUMED) {
            ringtone_info_t info;
            if (p->seen != (uint8_t)((1u << p->total) - 1u) ||
                !ringtone_decode(p->data, pending_size(p), &info, NULL, 0u)) return false;
        }
    }
    return true;
}

#include "storage/store_operator_logo.h"
#include "services/timebase.h"
#include "services/message_file_codec.h"
#include "storage_bytes.h"
#include "store_service_internal.h"

#include <string.h>

#define LOGOS_MAGIC 0x31474f4cu
#define PENDING_COUNT 2u
#define RECEIVE_TTL_MS (30u * 60u * 1000u)

enum { LOGO_FREE, LOGO_PARTIAL, LOGO_READY, LOGO_CONSUMED, LOGO_INVALID };
typedef struct {
    uint32_t id;
    uint16_t reference, source_port;
    uint8_t state, total, seen, dcs, flags;
    uint8_t lengths[MODEM_SMS_SEGMENT_MAX];
    uint32_t timestamps[MODEM_SMS_SEGMENT_MAX];
    char sender[MODEM_SMS_SENDER_MAX + 1u];
    char timestamp[MODEM_SMS_TIMESTAMP_MAX + 1u];
    uint8_t data[OPERATOR_LOGO_PAYLOAD_MAX];
} pending_logo_t;

typedef struct {
    bool saved;
    operator_logo_t logo;
    uint32_t next_id;
    pending_logo_t pending[PENDING_COUNT];
} logo_state_t;

static logo_state_t s_logos;
static operator_logo_t s_committed_logo;
static bool s_committed_saved;
static uint32_t s_touched[PENDING_COUNT];
static sms_codec_message_t s_part;

static void reset_unit(uint8_t instance) {
    (void)instance;
    memset(&s_logos, 0, sizeof(s_logos));
    memset(&s_committed_logo, 0, sizeof(s_committed_logo));
    memset(s_touched, 0, sizeof(s_touched));
    s_committed_saved = false;
}

static uint16_t pending_size(const pending_logo_t *p) {
    uint16_t size = 0u;
    for (unsigned i = 0u; i < MODEM_SMS_SEGMENT_MAX; i++) size += p->lengths[i];
    return size;
}

store_status_t store_operator_logo_commit_status(void) {
    store_diag_snapshot_t diag;
    store_service_get_diag(&diag);
    if (!diag.ready) return STORE_STATUS_NOT_READY;
    uint32_t mask = 1u << STORE_UNIT_OPERATOR_LOGO;
    if ((diag.degraded_mask | diag.unavailable_mask) & mask) return STORE_STATUS_STORAGE_ERROR;
    return diag.dirty_mask & mask ? STORE_STATUS_NOT_READY : STORE_STATUS_OK;
}

static void refresh_committed_logo(void) {
    if (store_operator_logo_commit_status() == STORE_STATUS_OK) {
        s_committed_logo = s_logos.logo;
        s_committed_saved = s_logos.saved;
    }
}

static store_status_t mark_dirty(void) {
    /* A successful save may not have been read before the next receipt arrives. */
    refresh_committed_logo();
    return store_engine_mark_dirty(STORE_UNIT_OPERATOR_LOGO);
}

void store_operator_logo_expire(uint32_t now_ms) {
    for (unsigned i = 0u; i < PENDING_COUNT; i++) {
        pending_logo_t *p = &s_logos.pending[i];
        if (p->state != LOGO_FREE && p->state != LOGO_READY &&
            (uint32_t)(now_ms - s_touched[i]) >= RECEIVE_TTL_MS &&
            store_operator_logo_commit_status() == STORE_STATUS_OK &&
            mark_dirty() == STORE_STATUS_OK)
            memset(p, 0, sizeof(*p));
    }
}

static bool valid_part(const sms_codec_message_t *p) {
    uint8_t total = p->has_concat ? p->concat_total : 1u;
    uint8_t seq = p->has_concat ? p->concat_seq : 1u;
    return !p->submit && p->binary && p->has_ports && p->dest_port == OPERATOR_LOGO_PORT &&
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

static unsigned identity_match_rank(const pending_logo_t *p, const sms_codec_message_t *part) {
    uint8_t flags = (part->has_concat ? 1u : 0u) | (part->concat_ref_16bit ? 2u : 0u);
    if (p->state == LOGO_FREE || p->reference != part->concat_ref ||
        p->source_port != part->source_port || p->flags != flags ||
        strcmp(p->sender, part->address) != 0) return 0u;
    unsigned index = part->has_concat ? part->concat_seq - 1u : 0u;
    bool complete = p->state == LOGO_READY || p->state == LOGO_CONSUMED;
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

store_status_t store_operator_logo_receive(const sms_codec_message_t *part, uint32_t now_ms) {
    if (part == NULL || !valid_part(part)) return STORE_STATUS_INVALID_ARGUMENT;
    if (!store_service_ready()) return STORE_STATUS_NOT_READY;
    store_operator_logo_expire(now_ms);
    pending_logo_t *p = NULL, *available = NULL;
    unsigned rank = 0u;
    for (unsigned i = 0u; i < PENDING_COUNT; i++) {
        pending_logo_t *candidate = &s_logos.pending[i];
        unsigned candidate_rank = identity_match_rank(candidate, part);
        if (candidate_rank > rank) { p = candidate; rank = candidate_rank; }
        if (rank == 2u) break;
        if (candidate->state == LOGO_FREE ||
            (candidate->state == LOGO_INVALID &&
                (available == NULL || available->state == LOGO_CONSUMED)) ||
            (available == NULL && candidate->state == LOGO_CONSUMED &&
                store_operator_logo_commit_status() == STORE_STATUS_OK))
            available = candidate;
    }
    uint8_t total = part->has_concat ? part->concat_total : 1u;
    uint8_t seq = part->has_concat ? part->concat_seq : 1u;
    if (p != NULL && p->state == LOGO_INVALID) return STORE_STATUS_CONFLICT;
    uint16_t offset = 0u, size = p != NULL ? pending_size(p) : 0u;
    if (p != NULL) {
        for (unsigned i = 0u; i + 1u < seq; i++) offset += p->lengths[i];
        bool duplicate = (p->seen & (1u << (seq - 1u))) != 0u;
        if (p->total != total || p->dcs != part->dcs ||
            (duplicate && (p->lengths[seq - 1u] != part->binary_len ||
                memcmp(p->data + offset, part->binary_data, part->binary_len) != 0))) {
            if (p->state == LOGO_PARTIAL && mark_dirty() == STORE_STATUS_OK)
                p->state = LOGO_INVALID;
            return STORE_STATUS_CONFLICT;
        }
        if (duplicate) return STORE_STATUS_OK;
    }
    if (size + part->binary_len > OPERATOR_LOGO_PAYLOAD_MAX) return STORE_STATUS_INVALID_ARGUMENT;
    if (p == NULL && available == NULL) return STORE_STATUS_STORAGE_ERROR;
    store_status_t status = mark_dirty();
    if (status != STORE_STATUS_OK) return status;
    if (p == NULL) {
        p = available;
        memset(p, 0, sizeof(*p));
        p->state = LOGO_PARTIAL;
        p->id = ++s_logos.next_id;
        if (p->id == 0u) p->id = ++s_logos.next_id;
        p->reference = part->concat_ref;
        p->source_port = part->source_port;
        p->total = total;
        p->dcs = part->dcs;
        p->flags = (part->has_concat ? 1u : 0u) | (part->concat_ref_16bit ? 2u : 0u);
        store_copy_text(p->sender, sizeof(p->sender), part->address);
        store_copy_text(p->timestamp, sizeof(p->timestamp), part->timestamp);
    }
    s_touched[p - s_logos.pending] = now_ms;
    memmove(p->data + offset + part->binary_len, p->data + offset, size - offset);
    memcpy(p->data + offset, part->binary_data, part->binary_len);
    p->lengths[seq - 1u] = (uint8_t)part->binary_len;
    p->timestamps[seq - 1u] = message_timestamp_seconds(part->timestamp);
    p->seen |= (uint8_t)(1u << (seq - 1u));
    if (p->seen == (uint8_t)((1u << total) - 1u)) {
        operator_logo_t info;
        if (!operator_logo_decode(p->data, pending_size(p), &info)) {
            p->state = LOGO_INVALID;
            return STORE_STATUS_INVALID_ARGUMENT;
        }
        p->state = LOGO_READY;
    }
    return STORE_STATUS_OK;
}

store_status_t store_operator_logo_receive_pdu(const char *pdu, uint32_t now_ms) {
    if (!sms_pdu_decode(pdu, &s_part) || !s_part.has_ports || s_part.dest_port != OPERATOR_LOGO_PORT)
        return STORE_STATUS_NOT_FOUND;
    return store_operator_logo_receive(&s_part, now_ms);
}

store_status_t store_operator_logo_received_pdu_status(const char *pdu) {
    if (!sms_pdu_decode(pdu, &s_part) || !s_part.has_ports || s_part.dest_port != OPERATOR_LOGO_PORT)
        return STORE_STATUS_NOT_FOUND;
    if (!valid_part(&s_part)) return STORE_STATUS_INVALID_ARGUMENT;
    store_status_t status = store_operator_logo_commit_status();
    if (status != STORE_STATUS_OK) return status;
    uint8_t total = s_part.has_concat ? s_part.concat_total : 1u;
    uint8_t seq = s_part.has_concat ? s_part.concat_seq : 1u;
    const pending_logo_t *p = NULL;
    unsigned rank = 0u;
    for (unsigned i = 0u; i < PENDING_COUNT; i++) {
        const pending_logo_t *candidate = &s_logos.pending[i];
        unsigned candidate_rank = identity_match_rank(candidate, &s_part);
        if (candidate_rank > rank) { p = candidate; rank = candidate_rank; }
        if (rank == 2u) break;
    }
    if (p == NULL || p->total != total || p->dcs != s_part.dcs ||
        !(p->seen & (1u << (seq - 1u)))) return STORE_STATUS_NOT_FOUND;
    if (p->state == LOGO_INVALID) return STORE_STATUS_CONFLICT;
    uint16_t offset = 0u;
    for (unsigned j = 0u; j + 1u < seq; j++) offset += p->lengths[j];
    if (p->lengths[seq - 1u] == s_part.binary_len &&
        memcmp(p->data + offset, s_part.binary_data, s_part.binary_len) == 0) return STORE_STATUS_OK;
    return STORE_STATUS_NOT_FOUND;
}

static pending_logo_t *pending_by_id(uint32_t id) {
    for (unsigned i = 0u; id != 0u && i < PENDING_COUNT; i++) {
        pending_logo_t *p = &s_logos.pending[i];
        if (p->id == id && (p->state == LOGO_READY || p->state == LOGO_CONSUMED)) return p;
    }
    return NULL;
}

uint32_t store_operator_logo_pending_first(void) {
    if (store_operator_logo_commit_status() != STORE_STATUS_OK) return 0u;
    uint32_t first = 0u;
    for (unsigned i = 0u; i < PENDING_COUNT; i++) {
        const pending_logo_t *p = &s_logos.pending[i];
        if (p->state == LOGO_READY && (first == 0u || (int32_t)(p->id - first) < 0)) first = p->id;
    }
    return first;
}

store_status_t store_operator_logo_pending_get(uint32_t id, operator_logo_t *out) {
    if (out == NULL) return STORE_STATUS_INVALID_ARGUMENT;
    pending_logo_t *p = pending_by_id(id);
    if (p == NULL) return STORE_STATUS_NOT_FOUND;
    return operator_logo_decode(p->data, pending_size(p), out)
        ? STORE_STATUS_OK : STORE_STATUS_STORAGE_ERROR;
}

store_status_t store_operator_logo_get(operator_logo_t *out) {
    if (out == NULL) return STORE_STATUS_INVALID_ARGUMENT;
    if (!store_service_unit_ready(STORE_UNIT_OPERATOR_LOGO)) return STORE_STATUS_NOT_READY;
    refresh_committed_logo();
    if (!s_committed_saved) return STORE_STATUS_NOT_FOUND;
    *out = s_committed_logo;
    return STORE_STATUS_OK;
}

store_status_t store_operator_logo_clear(void) {
    if (!store_service_ready() || !store_service_unit_ready(STORE_UNIT_OPERATOR_LOGO))
        return STORE_STATUS_NOT_READY;
    if (!s_logos.saved) return STORE_STATUS_OK;
    store_status_t status = mark_dirty();
    if (status == STORE_STATUS_OK) {
        s_logos.saved = false;
        memset(&s_logos.logo, 0, sizeof(s_logos.logo));
    }
    return status;
}

store_status_t store_operator_logo_pending_save(uint32_t id) {
    operator_logo_t logo;
    store_status_t status = store_operator_logo_pending_get(id, &logo);
    if (status != STORE_STATUS_OK) return status;
    status = mark_dirty();
    if (status != STORE_STATUS_OK) return status;
    s_logos.logo = logo;
    s_logos.saved = true;
    pending_by_id(id)->state = LOGO_CONSUMED;
    return STORE_STATUS_OK;
}

store_status_t store_operator_logo_pending_discard(uint32_t id) {
    pending_logo_t *p = pending_by_id(id);
    if (p == NULL) return STORE_STATUS_NOT_FOUND;
    store_status_t status = mark_dirty();
    if (status == STORE_STATUS_OK) p->state = LOGO_CONSUMED;
    return status;
}

static bool serialize_unit(uint8_t instance, uint8_t *dst, size_t cap, size_t *out_len) {
    (void)instance;
    size_t pos = 0u;
    if (!write_u32_field(dst, cap, &pos, LOGOS_MAGIC) ||
        !write_u16_field(dst, cap, &pos, STORE_PAYLOAD_VERSION) ||
        !write_u8_field(dst, cap, &pos, s_logos.saved ? 1u : 0u) ||
        !write_bytes(dst, cap, &pos, s_logos.logo.network, 3u) ||
        !write_bytes(dst, cap, &pos, s_logos.logo.pixels, OPERATOR_LOGO_BYTES) ||
        !write_u32_field(dst, cap, &pos, s_logos.next_id)) return false;
    for (unsigned i = 0u; i < PENDING_COUNT; i++) {
        const pending_logo_t *p = &s_logos.pending[i];
        if (!write_u32_field(dst, cap, &pos, p->id) ||
            !write_u16_field(dst, cap, &pos, p->reference) ||
            !write_u16_field(dst, cap, &pos, p->source_port) ||
            !write_u8_field(dst, cap, &pos, p->state) ||
            !write_u8_field(dst, cap, &pos, p->total) ||
            !write_u8_field(dst, cap, &pos, p->seen) ||
            !write_u8_field(dst, cap, &pos, p->dcs) ||
            !write_u8_field(dst, cap, &pos, p->flags) ||
            !write_bytes(dst, cap, &pos, p->lengths, sizeof(p->lengths))) return false;
        for (unsigned j = 0u; j < MODEM_SMS_SEGMENT_MAX; j++)
            if (!write_u32_field(dst, cap, &pos, p->timestamps[j])) return false;
        if (!write_bytes(dst, cap, &pos, p->sender, sizeof(p->sender)) ||
            !write_bytes(dst, cap, &pos, p->timestamp, sizeof(p->timestamp)) ||
            !write_bytes(dst, cap, &pos, p->data, sizeof(p->data))) return false;
    }
    *out_len = pos;
    return true;
}

static bool apply_unit(uint8_t instance, const uint8_t *data, size_t len) {
    (void)instance;
    const size_t record_size = 13u + 5u * MODEM_SMS_SEGMENT_MAX +
        MODEM_SMS_SENDER_MAX + 1u + MODEM_SMS_TIMESTAMP_MAX + 1u + OPERATOR_LOGO_PAYLOAD_MAX;
    if (len != 140u + PENDING_COUNT * record_size || read_u32(data) != LOGOS_MAGIC ||
        read_u16(data + 4u) != STORE_PAYLOAD_VERSION || data[6] > 1u) return false;
    static logo_state_t loaded;
    memset(&loaded, 0, sizeof(loaded));
    loaded.saved = data[6] != 0u;
    memcpy(loaded.logo.network, data + 7u, 3u);
    memcpy(loaded.logo.pixels, data + 10u, OPERATOR_LOGO_BYTES);
    if (loaded.saved && !operator_logo_network_valid(loaded.logo.network)) return false;
    loaded.next_id = read_u32(data + 136u);
    size_t pos = 140u;
    for (unsigned i = 0u; i < PENDING_COUNT; i++) {
        pending_logo_t *p = &loaded.pending[i];
        p->id = read_u32(data + pos); pos += 4u;
        p->reference = read_u16(data + pos); pos += 2u;
        p->source_port = read_u16(data + pos); pos += 2u;
        p->state = data[pos++]; p->total = data[pos++]; p->seen = data[pos++];
        p->dcs = data[pos++]; p->flags = data[pos++];
        memcpy(p->lengths, data + pos, sizeof(p->lengths)); pos += sizeof(p->lengths);
        for (unsigned j = 0u; j < MODEM_SMS_SEGMENT_MAX; j++) {
            p->timestamps[j] = read_u32(data + pos); pos += 4u;
        }
        memcpy(p->sender, data + pos, sizeof(p->sender)); pos += sizeof(p->sender);
        memcpy(p->timestamp, data + pos, sizeof(p->timestamp)); pos += sizeof(p->timestamp);
        memcpy(p->data, data + pos, sizeof(p->data)); pos += sizeof(p->data);
        if (p->state > LOGO_INVALID || p->flags > 3u ||
            p->sender[MODEM_SMS_SENDER_MAX] != '\0' || p->timestamp[MODEM_SMS_TIMESTAMP_MAX] != '\0' ||
            pending_size(p) > sizeof(p->data)) return false;
        if (p->state == LOGO_FREE) continue;
        if (p->id == 0u || p->total == 0u || p->total > MODEM_SMS_SEGMENT_MAX ||
            (p->dcs != 4u && p->dcs != 0xf5u) || (p->seen >> p->total) != 0u ||
            (!(p->flags & 1u) && (p->total != 1u || (p->flags & 2u))) ||
            p->sender[0] == '\0') return false;
        for (unsigned j = 0u; j < MODEM_SMS_SEGMENT_MAX; j++) {
            if (p->lengths[j] > 140u || ((p->seen & (1u << j)) != 0u) != (p->lengths[j] != 0u) ||
                (p->lengths[j] == 0u && p->timestamps[j] != 0u)) return false;
        }
        if (p->state == LOGO_READY || p->state == LOGO_CONSUMED) {
            operator_logo_t logo;
            if (p->seen != (uint8_t)((1u << p->total) - 1u) ||
                !operator_logo_decode(p->data, pending_size(p), &logo)) return false;
        }
        for (unsigned j = 0u; j < i; j++)
            if (loaded.pending[j].state != LOGO_FREE && loaded.pending[j].id == p->id) return false;
    }
    s_logos = loaded;
    s_committed_logo = loaded.logo;
    s_committed_saved = loaded.saved;
    for (unsigned i = 0u; i < PENDING_COUNT; i++) s_touched[i] = time_ms();
    return true;
}

const store_unit_ops_t g_store_operator_logo_unit_ops = {
    .reset_ram = reset_unit, .serialize = serialize_unit, .apply = apply_unit,
    .name = "operator logo",
};

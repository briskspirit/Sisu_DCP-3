#include "modem_sms_state_internal.h"

#include <stdio.h>
#include <string.h>

#include "services/sms_identity.h"
#include "services/sms_submit_codec.h"

#define MODEM_SMS_MULTIPART_GROUPS ((MODEM_SMS_RECORD_MAX + 1u) / 2u)
/* Cross-scan claims are alert-only state. Sixteen simultaneous incomplete
 * logical messages cover the practical case in 128 bytes; saturation is
 * deliberately fail-open and can produce a duplicate alert, never silence. */
#define MODEM_SMS_ARRIVAL_CLAIM_MAX 16u
_Static_assert(MODEM_SMS_MULTIPART_GROUPS <= UINT8_MAX,
               "multipart group scans use uint8_t");

typedef struct {
    bool used;
    uint8_t flags;
    uint16_t ref;
    modem_sms_record_t record;
    uint8_t total;
    uint8_t received_mask;
    uint8_t metadata_sequence;
    uint16_t payload_total;
} sms_multipart_group_t;

enum {
    SMS_MULTIPART_GROUP_UNSUPPORTED = 1u << 0,
    SMS_MULTIPART_GROUP_REF_16BIT = 1u << 1,
    SMS_MULTIPART_GROUP_ARRIVAL_CLAIMED = 1u << 2,
    SMS_MULTIPART_GROUP_TEXT = 1u << 3,
    SMS_MULTIPART_GROUP_UNREAD = 1u << 4,
};

_Static_assert(sizeof(sms_multipart_group_t) == 108u,
               "multipart group exceeded its bounded BSS budget");

typedef struct {
    uint32_t fingerprint;
    uint16_t anchor_index;
    uint8_t flags;
} sms_multipart_arrival_claim_t;

enum {
    SMS_ARRIVAL_CLAIM_USED = 1u << 0,
    SMS_ARRIVAL_CLAIM_ANCHOR_SEEN = 1u << 1,
    SMS_ARRIVAL_CLAIM_COMPLETE = 1u << 2,
};

enum {
    SMS_ARRIVAL_DEFERRED_BASE = MODEM_SMS_ARRIVAL_FILTERED + 1u,
};

_Static_assert(sizeof(sms_multipart_arrival_claim_t) == 8u,
               "multipart arrival claim must remain compact");

static void copy_bounded(char *dst, size_t cap, const char *src) {
    if (dst == NULL || cap == 0u) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    size_t len = strlen(src);
    if (len >= cap) {
        len = cap - 1u;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static void multipart_marker(char *dst, size_t cap, bool text,
                             bool reference_16bit, uint16_t reference,
                             uint8_t total) {
    if (dst == NULL || cap == 0u) {
        return;
    }
    snprintf(dst, cap, "%s:%u:%u:%u",
             text ? "Multipart text" : "Multipart picture",
             reference_16bit ? 1u : 0u, (unsigned)reference,
             (unsigned)total);
}

static uint32_t multipart_fingerprint(const char *sender, bool text,
                                      bool reference_16bit,
                                      uint16_t reference, uint8_t total) {
    char marker[sizeof("Multipart picture:1:65535:255")];
    multipart_marker(marker, sizeof(marker), text, reference_16bit,
                     reference, total);
    /* Segment timestamps can differ by several seconds. They are intentionally
     * excluded from this short-lived alert key; the physical anchor index keeps
     * a stale claim from surviving an authoritative scan after deletion. */
    return sms_identity_hash(sender, "", marker);
}

static uint32_t multipart_text_identity_hash(
    const char *sender, const char *timestamp, bool reference_16bit,
    uint16_t reference, uint8_t total) {
    char marker[sizeof("Multipart text:1:65535:255")];
    multipart_marker(marker, sizeof(marker), true, reference_16bit,
                     reference, total);
    return sms_identity_hash(sender, timestamp, marker);
}

static modem_sms_record_t s_mailbox[MODEM_SMS_RECORD_MAX];
static uint8_t s_mailbox_count;
static sms_multipart_group_t s_multipart_groups[MODEM_SMS_MULTIPART_GROUPS];

/* CMGL's status is the first-sighting value. Stream each following PDU directly
 * into the compact mailbox cache so a full store does not require one CMGR
 * round trip per row. */
static uint8_t s_scan_raw_count;
static bool s_scan_incomplete;
static uint8_t s_collection_list_status;
static modem_sms_message_t s_collection;
static bool s_collection_body;
static bool s_collection_decoded;

static uint16_t s_selected_first_index;
static uint8_t s_selected_index_count;
static uint8_t s_selected_index_pos;
static uint8_t s_selected_accepted_count;
static modem_sms_message_t s_selected_message;
static uint8_t s_selected_received_mask;
static uint8_t s_selected_text_next_sequence;
static bool s_selected_failed;
static bool s_selected_quarantined;

static char s_binary_pdu_hex[SMS_SUBMIT_PDU_HEX_MAX + 1u];
static uint16_t s_binary_position;
static uint8_t s_binary_tpdu_len;
static uint8_t s_binary_segment;
static uint8_t s_binary_segment_total;
/* Process-lifetime concatenation reference. modem_service_init() historically
 * did not reset it, so modem_sms_state_init() deliberately leaves it intact. */
static uint8_t s_binary_reference;
static bool s_binary_send_ok;

/* +CMTI identifies storage and index but not payload type. Keep arrivals
 * pending until a complete, uncrossed Inbox snapshot classifies each index. */
static uint16_t s_pending_arrival_indices[MODEM_SMS_RECORD_MAX];
static uint8_t s_pending_arrival_results[MODEM_SMS_RECORD_MAX];
static uint8_t s_pending_arrival_count;
static uint32_t s_arrival_scan_revision;
static sms_multipart_arrival_claim_t
    s_multipart_arrival_claims[MODEM_SMS_ARRIVAL_CLAIM_MAX];

static int16_t pending_arrival_position(uint16_t index) {
    for (uint8_t i = 0u; i < s_pending_arrival_count; i++) {
        if (s_pending_arrival_indices[i] == index) {
            return (int16_t)i;
        }
    }
    return -1;
}

static uint32_t multipart_group_fingerprint(
    const sms_multipart_group_t *group) {
    return multipart_fingerprint(
        group->record.sender,
        (group->flags & SMS_MULTIPART_GROUP_TEXT) != 0u,
        (group->flags & SMS_MULTIPART_GROUP_REF_16BIT) != 0u,
        group->ref, group->total);
}

static int8_t multipart_claim_find(uint32_t fingerprint) {
    for (uint8_t i = 0u; i < MODEM_SMS_ARRIVAL_CLAIM_MAX; i++) {
        if ((s_multipart_arrival_claims[i].flags &
             SMS_ARRIVAL_CLAIM_USED) != 0u &&
            s_multipart_arrival_claims[i].fingerprint == fingerprint) {
            return (int8_t)i;
        }
    }
    return -1;
}

static int8_t multipart_claim_add(uint32_t fingerprint,
                                  uint16_t anchor_index) {
    int8_t existing = multipart_claim_find(fingerprint);
    if (existing >= 0) {
        return existing;
    }
    for (uint8_t i = 0u; i < MODEM_SMS_ARRIVAL_CLAIM_MAX; i++) {
        if ((s_multipart_arrival_claims[i].flags &
             SMS_ARRIVAL_CLAIM_USED) == 0u) {
            s_multipart_arrival_claims[i].fingerprint = fingerprint;
            s_multipart_arrival_claims[i].anchor_index = anchor_index;
            s_multipart_arrival_claims[i].flags =
                SMS_ARRIVAL_CLAIM_USED | SMS_ARRIVAL_CLAIM_ANCHOR_SEEN;
            return (int8_t)i;
        }
    }
    /* Claims affect alert deduplication only. On saturation, fail open and let
     * the user receive a possible duplicate rather than suppressing an SMS. */
    return -1;
}

static void multipart_claim_note_segment(
    const sms_multipart_group_t *group, uint16_t index) {
    int8_t slot = multipart_claim_find(multipart_group_fingerprint(group));
    if (slot >= 0 &&
        s_multipart_arrival_claims[(uint8_t)slot].anchor_index == index) {
        s_multipart_arrival_claims[(uint8_t)slot].flags |=
            SMS_ARRIVAL_CLAIM_ANCHOR_SEEN;
    }
}

static void multipart_claim_complete(
    const sms_multipart_group_t *group) {
    int8_t slot = multipart_claim_find(multipart_group_fingerprint(group));
    if (slot >= 0) {
        s_multipart_arrival_claims[(uint8_t)slot].flags |=
            SMS_ARRIVAL_CLAIM_COMPLETE;
    }
}

static void classify_pending_arrival(uint16_t index,
                                     sms_multipart_group_t *group) {
    if (group != NULL) {
        multipart_claim_note_segment(group, index);
    }
    int16_t position = pending_arrival_position(index);
    if (position < 0 ||
        s_pending_arrival_results[position] != MODEM_SMS_ARRIVAL_UNSEEN) {
        return;
    }
    if (group != NULL) {
        uint32_t fingerprint = multipart_group_fingerprint(group);
        int8_t claim_slot = multipart_claim_find(fingerprint);
        if ((group->flags & SMS_MULTIPART_GROUP_ARRIVAL_CLAIMED) != 0u ||
            claim_slot >= 0) {
            s_pending_arrival_results[position] = claim_slot >= 0
                ? (uint8_t)(SMS_ARRIVAL_DEFERRED_BASE + claim_slot)
                : MODEM_SMS_ARRIVAL_FILTERED;
            return;
        }
        group->flags |= SMS_MULTIPART_GROUP_ARRIVAL_CLAIMED;
        (void)multipart_claim_add(fingerprint, index);
    }
    s_pending_arrival_results[position] = MODEM_SMS_ARRIVAL_USER;
}

/* Recognized application controls are omitted from the public cache, then
 * removed serially after CMGL has restored text mode. */
static uint16_t s_filtered_indices[MODEM_SMS_RECORD_MAX];
static uint8_t s_filtered_count;
static uint8_t s_filtered_pos;
static bool s_filtered_delete_failed;
static bool s_filtered_active;

static modem_sms_mailbox_result_t s_mailbox_result;
static bool s_mailbox_result_pending;
static modem_sms_read_result_t s_read_result;
static bool s_read_result_pending;
static modem_sms_send_result_t s_send_result;
static bool s_send_result_pending;
static modem_sms_save_result_t s_save_result;
static bool s_save_result_pending;
static modem_sms_delete_result_t s_delete_result;
static bool s_delete_result_pending;

typedef enum {
    SMS_RESULT_CHANNEL_SEND = 0,
    SMS_RESULT_CHANNEL_SAVE,
    SMS_RESULT_CHANNEL_MAILBOX,
    SMS_RESULT_CHANNEL_READ,
    SMS_RESULT_CHANNEL_DELETE,
    SMS_RESULT_CHANNEL_COUNT,
} sms_result_channel_t;

static uint32_t s_next_request_id;
static uint32_t s_reserved_request_ids[SMS_RESULT_CHANNEL_COUNT];
static modem_sms_request_kind_t s_reserved_request_kinds[SMS_RESULT_CHANNEL_COUNT];

static bool result_channel_for_kind(modem_sms_request_kind_t kind,
                                    sms_result_channel_t *out) {
    if (out == NULL) {
        return false;
    }
    switch (kind) {
    case MODEM_SMS_REQUEST_SEND_TEXT:
    case MODEM_SMS_REQUEST_SEND_BINARY:
        *out = SMS_RESULT_CHANNEL_SEND;
        return true;
    case MODEM_SMS_REQUEST_SAVE:
        *out = SMS_RESULT_CHANNEL_SAVE;
        return true;
    case MODEM_SMS_REQUEST_MAILBOX:
        *out = SMS_RESULT_CHANNEL_MAILBOX;
        return true;
    case MODEM_SMS_REQUEST_READ:
        *out = SMS_RESULT_CHANNEL_READ;
        return true;
    case MODEM_SMS_REQUEST_DELETE:
        *out = SMS_RESULT_CHANNEL_DELETE;
        return true;
    case MODEM_SMS_REQUEST_NONE:
    default:
        return false;
    }
}

static uint32_t pending_result_id(sms_result_channel_t channel) {
    switch (channel) {
    case SMS_RESULT_CHANNEL_SEND:
        return s_send_result_pending ? s_send_result.request_id : 0u;
    case SMS_RESULT_CHANNEL_SAVE:
        return s_save_result_pending ? s_save_result.request_id : 0u;
    case SMS_RESULT_CHANNEL_MAILBOX:
        return s_mailbox_result_pending ? s_mailbox_result.request_id : 0u;
    case SMS_RESULT_CHANNEL_READ:
        return s_read_result_pending ? s_read_result.request_id : 0u;
    case SMS_RESULT_CHANNEL_DELETE:
        return s_delete_result_pending ? s_delete_result.request_id : 0u;
    case SMS_RESULT_CHANNEL_COUNT:
    default:
        return 0u;
    }
}

static bool request_id_in_use(uint32_t request_id) {
    for (uint8_t i = 0u; i < SMS_RESULT_CHANNEL_COUNT; i++) {
        if (s_reserved_request_ids[i] == request_id ||
            pending_result_id((sms_result_channel_t)i) == request_id) {
            return true;
        }
    }
    return false;
}

static bool claim_terminal(uint32_t request_id,
                           modem_sms_request_kind_t kind) {
    sms_result_channel_t channel;
    if (request_id == 0u || !result_channel_for_kind(kind, &channel) ||
        s_reserved_request_ids[channel] != request_id ||
        s_reserved_request_kinds[channel] != kind) {
        return false;
    }
    s_reserved_request_ids[channel] = 0u;
    s_reserved_request_kinds[channel] = MODEM_SMS_REQUEST_NONE;
    return true;
}

void modem_sms_state_init(void) {
    s_mailbox_count = 0u;
    modem_sms_state_multipart_groups_reset();
    modem_sms_state_scan_reset();
    modem_sms_state_selected_begin(0u, 0u, false);
    modem_sms_state_pending_arrivals_reset();
    modem_sms_state_filtered_reset();
    memset(&s_mailbox_result, 0, sizeof(s_mailbox_result));
    s_mailbox_result_pending = false;
    memset(&s_read_result, 0, sizeof(s_read_result));
    s_read_result_pending = false;
    memset(&s_send_result, 0, sizeof(s_send_result));
    s_send_result_pending = false;
    memset(&s_save_result, 0, sizeof(s_save_result));
    s_save_result_pending = false;
    memset(&s_delete_result, 0, sizeof(s_delete_result));
    s_delete_result_pending = false;
    s_next_request_id = 0u;
    memset(s_reserved_request_ids, 0, sizeof(s_reserved_request_ids));
    memset(s_reserved_request_kinds, 0, sizeof(s_reserved_request_kinds));
}

bool modem_sms_state_reserve_request(modem_sms_request_kind_t kind,
                                     uint32_t *request_id_out) {
    sms_result_channel_t channel;
    if (request_id_out == NULL || !result_channel_for_kind(kind, &channel) ||
        s_reserved_request_ids[channel] != 0u ||
        pending_result_id(channel) != 0u) {
        return false;
    }

    uint32_t request_id;
    do {
        request_id = ++s_next_request_id;
    } while (request_id == 0u || request_id_in_use(request_id));

    s_reserved_request_ids[channel] = request_id;
    s_reserved_request_kinds[channel] = kind;
    *request_id_out = request_id;
    return true;
}

bool modem_sms_state_release_request(uint32_t request_id,
                                     modem_sms_request_kind_t kind) {
    return claim_terminal(request_id, kind);
}

void modem_sms_state_mailbox_clear(void) {
    s_mailbox_count = 0u;
}

bool modem_sms_state_mailbox_append(const modem_sms_record_t *record) {
    if (record == NULL || s_mailbox_count >= MODEM_SMS_RECORD_MAX) {
        return false;
    }
    s_mailbox[s_mailbox_count++] = *record;
    return true;
}

uint8_t modem_sms_state_mailbox_count(void) {
    return s_mailbox_count;
}

bool modem_sms_state_mailbox_record(uint8_t position,
                                    modem_sms_record_t *out) {
    if (out == NULL || position >= s_mailbox_count) {
        return false;
    }
    *out = s_mailbox[position];
    return true;
}

void modem_sms_state_multipart_groups_reset(void) {
    memset(s_multipart_groups, 0, sizeof(s_multipart_groups));
}

bool modem_sms_state_multipart_groups_pending(void) {
    for (uint8_t i = 0u; i < MODEM_SMS_MULTIPART_GROUPS; i++) {
        if (s_multipart_groups[i].used) {
            return true;
        }
    }
    return false;
}

static void prepare_single_record(const modem_sms_message_t *message,
                                  bool picture,
                                  modem_sms_record_t *out) {
    memset(out, 0, sizeof(*out));
    out->indices[0] = message->index;
    out->index_count = 1u;
    out->picture = picture;
    copy_bounded(out->status, sizeof(out->status), message->status);
    copy_bounded(out->sender, sizeof(out->sender), message->sender);
    copy_bounded(out->timestamp, sizeof(out->timestamp), message->timestamp);
    const char *identity_body = picture
        ? "Picture message"
        : ((message->binary || message->has_ports)
               ? "Data message" : message->text);
    out->identity_hash = sms_identity_hash(
        out->sender, out->timestamp, identity_body);
}

static uint32_t quarantine_identity_hash(const char *sender,
                                         const char *timestamp,
                                         uint16_t first_index) {
    if ((sender != NULL && sender[0] != '\0') ||
        (timestamp != NULL && timestamp[0] != '\0')) {
        return sms_identity_hash(sender, timestamp, "Data message");
    }
    /* A corrupt PDU has no decoded sender/timestamp/body. Salt that otherwise
     * constant identity with its stable ME slot so adjacent corrupt rows cannot
     * satisfy each other's stale-read guard. This text is never user-visible. */
    char body[sizeof("Data message 65535")];
    snprintf(body, sizeof(body), "Data message %u", (unsigned)first_index);
    return sms_identity_hash("", "", body);
}

bool modem_sms_state_mailbox_prepare_quarantine(
    const modem_sms_message_t *message, modem_sms_record_t *out) {
    if (message == NULL || out == NULL) {
        s_scan_incomplete = true;
        return false;
    }
    prepare_single_record(message, false, out);
    out->quarantined = true;
    out->identity_hash = quarantine_identity_hash(
        out->sender, out->timestamp, out->indices[0]);
    return true;
}

static bool prepare_group_quarantine(const sms_multipart_group_t *group,
                                     modem_sms_record_t *out) {
    if (group == NULL || out == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    for (uint8_t sequence = 0u;
         sequence < group->total && sequence < MODEM_SMS_SEGMENT_MAX;
         sequence++) {
        uint8_t bit = (uint8_t)(1u << sequence);
        if ((group->received_mask & bit) != 0u) {
            out->indices[out->index_count++] =
                group->record.indices[sequence];
        }
    }
    if (out->index_count == 0u) {
        return false;
    }
    out->quarantined = true;
    copy_bounded(out->status, sizeof(out->status), group->record.status);
    copy_bounded(out->sender, sizeof(out->sender), group->record.sender);
    copy_bounded(out->timestamp, sizeof(out->timestamp),
                 group->record.timestamp);
    out->identity_hash = quarantine_identity_hash(
        out->sender, out->timestamp, out->indices[0]);
    return true;
}

bool modem_sms_state_multipart_group_take_quarantine(
    modem_sms_record_t *out) {
    if (out == NULL) {
        return false;
    }
    for (uint8_t i = 0u; i < MODEM_SMS_MULTIPART_GROUPS; i++) {
        if (!s_multipart_groups[i].used) {
            continue;
        }
        bool ok = prepare_group_quarantine(&s_multipart_groups[i], out);
        memset(&s_multipart_groups[i], 0, sizeof(s_multipart_groups[i]));
        if (ok) {
            return true;
        }
        s_scan_incomplete = true;
    }
    return false;
}

bool modem_sms_state_mailbox_prepare_record(
    const modem_sms_message_t *message, modem_sms_record_t *out) {
    if (message == NULL || out == NULL) {
        s_scan_incomplete = true;
        return false;
    }

    bool picture = message->binary && message->has_ports &&
                   message->dest_port == SMS_CODEC_PICTURE_PORT;
    bool multipart_picture = picture && message->has_concat &&
                             message->concat_total > 1u;
    bool multipart_text = !message->binary && !message->has_ports &&
                          message->has_concat &&
                          message->concat_total > 1u;
    bool multipart = multipart_picture || multipart_text;
    if (!multipart) {
        classify_pending_arrival(message->index, NULL);
        prepare_single_record(message, picture, out);
        return true;
    }

    uint8_t total = message->concat_total;
    uint8_t sequence = message->concat_seq;
    size_t text_len = multipart_text ? strlen(message->text) : 0u;
    if ((multipart_picture &&
         (message->binary_len == 0u ||
          message->binary_len > MODEM_SMS_BINARY_CHUNK_MAX)) ||
        total > MODEM_SMS_SEGMENT_MAX || sequence == 0u ||
        sequence > total) {
        classify_pending_arrival(message->index, NULL);
        return modem_sms_state_mailbox_prepare_quarantine(message, out);
    }

    sms_multipart_group_t *group = NULL;
    for (uint8_t i = 0u; i < MODEM_SMS_MULTIPART_GROUPS; i++) {
        if (s_multipart_groups[i].used &&
            s_multipart_groups[i].ref == message->concat_ref &&
            ((s_multipart_groups[i].flags &
              SMS_MULTIPART_GROUP_REF_16BIT) != 0u) ==
                message->concat_ref_16bit &&
            ((s_multipart_groups[i].flags &
              SMS_MULTIPART_GROUP_TEXT) != 0u) == multipart_text &&
            s_multipart_groups[i].total == total &&
            strcmp(s_multipart_groups[i].record.sender,
                   message->sender) == 0) {
            group = &s_multipart_groups[i];
            break;
        }
    }
    if (group == NULL) {
        for (uint8_t i = 0u; i < MODEM_SMS_MULTIPART_GROUPS; i++) {
            if (!s_multipart_groups[i].used) {
                group = &s_multipart_groups[i];
                memset(group, 0, sizeof(*group));
                group->used = true;
                group->ref = message->concat_ref;
                if (message->concat_ref_16bit) {
                    group->flags |= SMS_MULTIPART_GROUP_REF_16BIT;
                }
                if (multipart_text) {
                    group->flags |= SMS_MULTIPART_GROUP_TEXT;
                }
                group->total = total;
                group->metadata_sequence = sequence;
                group->record.index_count = total;
                group->record.picture = multipart_picture;
                copy_bounded(group->record.status,
                             sizeof(group->record.status), message->status);
                copy_bounded(group->record.sender,
                             sizeof(group->record.sender), message->sender);
                copy_bounded(group->record.timestamp,
                             sizeof(group->record.timestamp),
                             message->timestamp);
                break;
            }
        }
    }
    if (group == NULL) {
        classify_pending_arrival(message->index, NULL);
        return modem_sms_state_mailbox_prepare_quarantine(message, out);
    }

    classify_pending_arrival(message->index, group);

    uint8_t sequence_bit = (uint8_t)(1u << (sequence - 1u));
    if ((group->received_mask & sequence_bit) != 0u) {
        if (group->record.indices[sequence - 1u] == message->index) {
            return false;
        }
        /* Preserve a second physical row claiming the same wire sequence. The
         * original assembly remains viable; the conflicting row is visible as
         * a standalone Data message instead of disappearing. */
        return modem_sms_state_mailbox_prepare_quarantine(message, out);
    }

    if (multipart_picture) {
        uint16_t offset = (uint16_t)((sequence - 1u) *
                                     MODEM_SMS_BINARY_CHUNK_MAX);
        if (offset + message->binary_len > MODEM_SMS_BINARY_MAX) {
            group->flags |= SMS_MULTIPART_GROUP_UNSUPPORTED;
        }
    } else if (text_len > MODEM_SMS_DECODED_TEXT_MAX ||
               group->payload_total >
                   MODEM_SMS_DECODED_TEXT_MAX - text_len) {
        group->flags |= SMS_MULTIPART_GROUP_UNSUPPORTED;
    } else {
        group->payload_total = (uint16_t)(group->payload_total + text_len);
    }
    bool unread = strstr(message->status, "UNREAD") != NULL;
    if (sequence < group->metadata_sequence) {
        group->metadata_sequence = sequence;
        copy_bounded(group->record.sender, sizeof(group->record.sender),
                     message->sender);
        copy_bounded(group->record.timestamp,
                     sizeof(group->record.timestamp), message->timestamp);
        if ((group->flags & SMS_MULTIPART_GROUP_UNREAD) == 0u || unread) {
            copy_bounded(group->record.status, sizeof(group->record.status),
                         message->status);
        }
    }
    if (unread) {
        group->flags |= SMS_MULTIPART_GROUP_UNREAD;
        copy_bounded(group->record.status, sizeof(group->record.status),
                     message->status);
    }
    group->record.indices[sequence - 1u] = message->index;
    group->received_mask |= sequence_bit;
    uint8_t complete_mask = total >= MODEM_SMS_SEGMENT_MAX
        ? UINT8_MAX : (uint8_t)((1u << total) - 1u);
    if ((group->received_mask & complete_mask) != complete_mask) {
        return false;
    }

    multipart_claim_complete(group);
    if ((group->flags & SMS_MULTIPART_GROUP_UNSUPPORTED) != 0u) {
        (void)prepare_group_quarantine(group, out);
    } else {
        group->record.identity_hash = multipart_picture
            ? sms_identity_hash(group->record.sender,
                                group->record.timestamp,
                                "Picture message")
            : multipart_text_identity_hash(
                  group->record.sender, group->record.timestamp,
                  (group->flags & SMS_MULTIPART_GROUP_REF_16BIT) != 0u,
                  group->ref, group->total);
        *out = group->record;
    }
    memset(group, 0, sizeof(*group));
    return true;
}

void modem_sms_state_scan_reset(void) {
    s_scan_raw_count = 0u;
    s_scan_incomplete = false;
    s_collection_list_status = UINT8_MAX;
    memset(&s_collection, 0, sizeof(s_collection));
    s_collection_body = false;
    s_collection_decoded = false;
}

void modem_sms_state_scan_mark_incomplete(void) {
    s_scan_incomplete = true;
}

bool modem_sms_state_scan_incomplete(void) {
    return s_scan_incomplete;
}

uint8_t modem_sms_state_scan_raw_count(void) {
    return s_scan_raw_count;
}

void modem_sms_state_collection_abort_body(void) {
    s_collection_body = false;
}

void modem_sms_state_mailbox_row_begin(bool index_valid, uint16_t index,
                                       bool status_valid,
                                       uint8_t list_status) {
    if (s_collection_body) {
        /* A second header before the prior row's PDU means the prior row was
         * malformed or lost. Keep scanning, but never publish the view as
         * authoritative for unread reconciliation. */
        s_scan_incomplete = true;
    }
    memset(&s_collection, 0, sizeof(s_collection));
    s_collection_decoded = false;
    s_collection_body = true;
    s_collection_list_status = UINT8_MAX;
    if (!index_valid || s_scan_raw_count >= MODEM_SMS_RECORD_MAX) {
        s_scan_incomplete = true;
        return;
    }
    s_scan_raw_count++;
    s_collection.index = index;
    if (status_valid) {
        s_collection_list_status = list_status;
    } else {
        s_scan_incomplete = true;
    }
}

void modem_sms_state_detail_row_begin(uint16_t index) {
    memset(&s_collection, 0, sizeof(s_collection));
    s_collection.index = index;
    s_collection_list_status = UINT8_MAX;
    s_collection_body = false;
    s_collection_decoded = false;
}

void modem_sms_state_detail_header(const char *status, const char *sender,
                                   const char *timestamp) {
    if (status != NULL) {
        copy_bounded(s_collection.status, sizeof(s_collection.status), status);
    }
    if (sender != NULL) {
        copy_bounded(s_collection.sender, sizeof(s_collection.sender), sender);
    }
    if (timestamp != NULL) {
        copy_bounded(s_collection.timestamp,
                     sizeof(s_collection.timestamp), timestamp);
    }
    s_collection_body = true;
}

void modem_sms_state_collection_feed_decoded(
    const sms_codec_message_t *decoded) {
    if (decoded == NULL) {
        return;
    }
    copy_bounded(s_collection.sender, sizeof(s_collection.sender),
                 decoded->address);
    copy_bounded(s_collection.timestamp, sizeof(s_collection.timestamp),
                 decoded->timestamp);
    s_collection.binary = decoded->binary;
    s_collection.has_ports = decoded->has_ports;
    s_collection.dest_port = decoded->dest_port;
    s_collection.source_port = decoded->source_port;
    s_collection.has_concat = decoded->has_concat;
    s_collection.concat_ref_16bit = decoded->concat_ref_16bit;
    s_collection.concat_ref = decoded->concat_ref;
    s_collection.concat_total = decoded->concat_total;
    s_collection.concat_seq = decoded->concat_seq;
    s_collection.binary_len = decoded->binary_len <= MODEM_SMS_BINARY_MAX
        ? decoded->binary_len : MODEM_SMS_BINARY_MAX;
    if (decoded->binary) {
        memset(s_collection.binary_data, 0,
               sizeof(s_collection.binary_data));
        memcpy(s_collection.binary_data, decoded->binary_data,
               s_collection.binary_len);
    } else {
        copy_bounded(s_collection.text, sizeof(s_collection.text),
                     decoded->text);
    }
    s_collection_body = false;
    s_collection_decoded = true;
}

void modem_sms_state_collection_feed_text(const char *line) {
    if (line == NULL) {
        return;
    }
    size_t used = strlen(s_collection.text);
    if (used > 0u && used + 1u < sizeof(s_collection.text)) {
        s_collection.text[used++] = '\n';
        s_collection.text[used] = '\0';
    }
    copy_bounded(&s_collection.text[used],
                 sizeof(s_collection.text) - used, line);
}

void modem_sms_state_collection_set_status(const char *status) {
    copy_bounded(s_collection.status, sizeof(s_collection.status), status);
}

void modem_sms_state_collection_view(modem_sms_collected_view_t *out) {
    if (out == NULL) {
        return;
    }
    out->message = &s_collection;
    out->list_status = s_collection_list_status;
    out->collecting_body = s_collection_body;
    out->decoded = s_collection_decoded;
}

void modem_sms_state_selected_begin(uint16_t first_index,
                                    uint8_t index_count,
                                    bool quarantined) {
    s_selected_first_index = first_index;
    s_selected_index_count = index_count <= MODEM_SMS_SEGMENT_MAX
        ? index_count : 0u;
    s_selected_index_pos = 0u;
    s_selected_accepted_count = 0u;
    memset(&s_selected_message, 0, sizeof(s_selected_message));
    s_selected_received_mask = 0u;
    s_selected_text_next_sequence = 1u;
    s_selected_failed = false;
    s_selected_quarantined = quarantined;
}

bool modem_sms_state_selected_next_index(const uint16_t *indices,
                                         uint8_t index_count,
                                         uint16_t *index_out) {
    if (index_out == NULL) {
        return false;
    }
    if (indices == NULL || index_count != s_selected_index_count) {
        s_selected_failed = true;
        return false;
    }
    if (s_selected_index_pos >= s_selected_index_count) {
        return false;
    }
    *index_out = indices[s_selected_index_pos++];
    return true;
}

void modem_sms_state_selected_fail(void) {
    s_selected_failed = true;
}

bool modem_sms_state_selected_accept_segment(
    const modem_sms_message_t *segment) {
    if (segment == NULL || s_selected_failed ||
        s_selected_index_count == 0u) {
        s_selected_failed = true;
        return false;
    }
    if (s_selected_quarantined) {
        if (s_selected_accepted_count == 0u) {
            s_selected_message = *segment;
            s_selected_message.index = s_selected_first_index;
            s_selected_message.binary = true;
            s_selected_message.has_ports = false;
            s_selected_message.has_concat = false;
            s_selected_message.binary_len = 0u;
            memset(s_selected_message.binary_data, 0,
                   sizeof(s_selected_message.binary_data));
        } else if (strcmp(s_selected_message.sender, segment->sender) != 0) {
            s_selected_failed = true;
            return false;
        }
        if (s_selected_accepted_count >= s_selected_index_count) {
            s_selected_failed = true;
            return false;
        }
        s_selected_accepted_count++;
        return true;
    }
    if (s_selected_index_count == 1u) {
        s_selected_message = *segment;
        s_selected_received_mask = 1u;
        return true;
    }
    bool picture_segment = segment->binary && segment->has_ports &&
                           segment->dest_port == SMS_CODEC_PICTURE_PORT;
    bool text_segment = !segment->binary && !segment->has_ports;
    if ((!picture_segment && !text_segment) || !segment->has_concat ||
        segment->concat_total != s_selected_index_count ||
        segment->concat_seq == 0u ||
        segment->concat_seq > segment->concat_total ||
        (picture_segment &&
         (segment->binary_len == 0u ||
          segment->binary_len > MODEM_SMS_BINARY_CHUNK_MAX))) {
        s_selected_failed = true;
        return false;
    }

    if (s_selected_received_mask == 0u) {
        if (text_segment && segment->concat_seq != 1u) {
            s_selected_failed = true;
            return false;
        }
        s_selected_message = *segment;
        s_selected_message.index = s_selected_first_index;
        if (picture_segment) {
            memset(s_selected_message.binary_data, 0,
                   sizeof(s_selected_message.binary_data));
            s_selected_message.binary_len = 0u;
        } else {
            s_selected_message.text[0] = '\0';
        }
    } else if (strcmp(s_selected_message.sender, segment->sender) != 0 ||
               s_selected_message.concat_ref != segment->concat_ref ||
               s_selected_message.concat_ref_16bit !=
                   segment->concat_ref_16bit ||
               s_selected_message.concat_total != segment->concat_total ||
               s_selected_message.binary != picture_segment) {
        s_selected_failed = true;
        return false;
    }

    uint8_t sequence_bit =
        (uint8_t)(1u << (segment->concat_seq - 1u));
    if (text_segment) {
        size_t used = strlen(s_selected_message.text);
        size_t segment_len = strlen(segment->text);
        if (segment->concat_seq != s_selected_text_next_sequence ||
            (s_selected_received_mask & sequence_bit) != 0u ||
            segment_len > MODEM_SMS_DECODED_TEXT_MAX ||
            used > MODEM_SMS_DECODED_TEXT_MAX - segment_len) {
            s_selected_failed = true;
            return false;
        }
        memcpy(&s_selected_message.text[used], segment->text,
               segment_len + 1u);
        s_selected_received_mask |= sequence_bit;
        s_selected_text_next_sequence++;
        return true;
    }

    uint16_t offset = (uint16_t)((segment->concat_seq - 1u) *
                                 MODEM_SMS_BINARY_CHUNK_MAX);
    if (offset + segment->binary_len > MODEM_SMS_BINARY_MAX) {
        s_selected_failed = true;
        return false;
    }
    memcpy(&s_selected_message.binary_data[offset], segment->binary_data,
           segment->binary_len);
    uint16_t end = (uint16_t)(offset + segment->binary_len);
    if (end > s_selected_message.binary_len) {
        s_selected_message.binary_len = end;
    }
    s_selected_received_mask |= sequence_bit;
    return true;
}

bool modem_sms_state_selected_accept_undecoded(
    const modem_sms_message_t *segment) {
    if (!s_selected_quarantined) {
        s_selected_failed = true;
        return false;
    }
    return modem_sms_state_selected_accept_segment(segment);
}

bool modem_sms_state_selected_complete(void) {
    if (s_selected_failed || s_selected_index_count == 0u) {
        return false;
    }
    if (s_selected_quarantined) {
        return s_selected_accepted_count == s_selected_index_count;
    }
    uint8_t complete_mask = s_selected_index_count >= MODEM_SMS_SEGMENT_MAX
        ? UINT8_MAX
        : (uint8_t)((1u << s_selected_index_count) - 1u);
    return s_selected_received_mask == complete_mask;
}

bool modem_sms_state_selected_publish_result(uint32_t request_id,
                                             modem_sms_request_kind_t kind,
                                             modem_sms_outcome_t outcome,
                                             bool sim_not_ready,
                                             uint32_t expected_identity_hash,
                                             bool *identity_mismatch_out) {
    if (identity_mismatch_out != NULL) {
        *identity_mismatch_out = false;
    }
    bool ok = outcome == MODEM_SMS_OUTCOME_OK &&
              modem_sms_state_selected_complete();
    bool picture = s_selected_message.binary &&
                   s_selected_message.has_ports &&
                   s_selected_message.dest_port == SMS_CODEC_PICTURE_PORT;
    bool multipart_text = !s_selected_message.binary &&
                          !s_selected_message.has_ports &&
                          s_selected_message.has_concat &&
                          s_selected_index_count > 1u;
    const char *identity_body = picture
        ? "Picture message"
        : ((s_selected_message.binary || s_selected_message.has_ports)
               ? "Data message" : s_selected_message.text);
    uint32_t actual_identity_hash = 0u;
    if (ok) {
        actual_identity_hash = s_selected_quarantined
            ? quarantine_identity_hash(
                  s_selected_message.sender, s_selected_message.timestamp,
                  s_selected_first_index)
            : (multipart_text
                   ? multipart_text_identity_hash(
                         s_selected_message.sender,
                         s_selected_message.timestamp,
                         s_selected_message.concat_ref_16bit,
                         s_selected_message.concat_ref,
                         s_selected_message.concat_total)
                   : sms_identity_hash(
                         s_selected_message.sender,
                         s_selected_message.timestamp, identity_body));
        if (actual_identity_hash != expected_identity_hash) {
            ok = false;
            outcome = MODEM_SMS_OUTCOME_ERROR;
            if (identity_mismatch_out != NULL) {
                *identity_mismatch_out = true;
            }
        }
    }
    if (outcome == MODEM_SMS_OUTCOME_OK && !ok) {
        outcome = MODEM_SMS_OUTCOME_ERROR;
    }
    return modem_sms_state_publish_read_result(
        request_id, kind, outcome, sim_not_ready, expected_identity_hash,
        actual_identity_hash, ok ? &s_selected_message : NULL);
}

uint8_t modem_sms_state_binary_begin(uint16_t payload_len) {
    s_binary_position = 0u;
    s_binary_segment = 1u;
    s_binary_segment_total = (uint8_t)(
        (payload_len + (MODEM_SMS_BINARY_CHUNK_MAX - 1u)) /
        MODEM_SMS_BINARY_CHUNK_MAX);
    if (s_binary_segment_total == 0u) {
        s_binary_segment_total = 1u;
    }
    s_binary_reference++;
    if (s_binary_reference == 0u) {
        s_binary_reference = 1u;
    }
    s_binary_send_ok = false;
    return s_binary_segment_total;
}

bool modem_sms_state_binary_build_segment(
    const char *number, const uint8_t *payload, uint16_t payload_len,
    uint16_t dest_port, uint16_t source_port,
    modem_binary_sms_mode_t mode) {
    sms_submit_pdu_t submit = {
        .number = number,
        .payload = payload,
        .payload_len = payload_len,
        .position = s_binary_position,
        .dest_port = dest_port,
        .source_port = source_port,
        .mode = mode,
        .segment = s_binary_segment,
        .segment_total = s_binary_segment_total,
        .reference = s_binary_reference,
    };
    if (!sms_submit_pdu_build(&submit, s_binary_pdu_hex,
                              sizeof(s_binary_pdu_hex),
                              &s_binary_tpdu_len)) {
        return false;
    }
    s_binary_position = submit.position;
    s_binary_segment = submit.segment;
    return true;
}

void modem_sms_state_binary_segment_view(
    modem_sms_binary_segment_view_t *out) {
    if (out == NULL) {
        return;
    }
    out->pdu_hex = s_binary_pdu_hex;
    out->tpdu_len = s_binary_tpdu_len;
    /* sms_submit_pdu_build() already post-incremented s_binary_segment before
     * the '>' prompt fires. The -1u recovers the one-based segment just written.
     * Do NOT drop it: that is a real off-by-one, not display decoration. */
    out->segment = s_binary_segment > 0u
        ? (uint8_t)(s_binary_segment - 1u) : 0u;
    out->segment_total = s_binary_segment_total;
}

bool modem_sms_state_picture_build_text_segment(
    const char *number, const uint8_t *payload, uint16_t payload_len,
    uint16_t dest_port, uint16_t source_port) {
    sms_submit_pdu_t submit = {
        .number = number, .payload = payload, .payload_len = payload_len,
        .position = s_binary_position, .dest_port = dest_port,
        .source_port = source_port, .mode = MODEM_BINARY_SMS_MODE_DCS04_PORT_FIRST,
        .segment = s_binary_segment, .segment_total = s_binary_segment_total,
        .reference = s_binary_reference,
    };
    if (!sms_submit_picture_text_build(&submit, s_binary_pdu_hex,
                                       sizeof(s_binary_pdu_hex))) {
        return false;
    }
    s_binary_tpdu_len = 0u; /* The body is user data, not a TPDU. */
    s_binary_position = submit.position;
    s_binary_segment = submit.segment;
    return true;
}

bool modem_sms_state_binary_has_more(uint16_t payload_len) {
    return s_binary_position < payload_len;
}

void modem_sms_state_binary_set_send_ok(bool sent_ok) {
    s_binary_send_ok = sent_ok;
}

bool modem_sms_state_binary_send_ok(void) {
    return s_binary_send_ok;
}

void modem_sms_state_pending_arrivals_reset(void) {
    s_pending_arrival_count = 0u;
    s_arrival_scan_revision = 0u;
    memset(s_multipart_arrival_claims, 0,
           sizeof(s_multipart_arrival_claims));
}

bool modem_sms_state_pending_arrival_track(uint16_t index) {
    if (pending_arrival_position(index) >= 0) {
        return true;
    }
    if (s_pending_arrival_count >= MODEM_SMS_RECORD_MAX) {
        return false;
    }
    uint8_t pos = s_pending_arrival_count++;
    s_pending_arrival_indices[pos] = index;
    s_pending_arrival_results[pos] = MODEM_SMS_ARRIVAL_UNSEEN;
    return true;
}

void modem_sms_state_arrival_scan_begin(uint32_t revision) {
    memset(s_pending_arrival_results, MODEM_SMS_ARRIVAL_UNSEEN,
           s_pending_arrival_count);
    for (uint8_t i = 0u; i < MODEM_SMS_ARRIVAL_CLAIM_MAX; i++) {
        s_multipart_arrival_claims[i].flags &=
            (uint8_t)~SMS_ARRIVAL_CLAIM_ANCHOR_SEEN;
    }
    s_arrival_scan_revision = revision;
}

void modem_sms_state_arrival_scan_note(uint16_t index,
                                      modem_sms_arrival_result_t result) {
    int16_t position = pending_arrival_position(index);
    if (position >= 0) {
        s_pending_arrival_results[position] = (uint8_t)result;
    }
}

bool modem_sms_state_arrival_scan_commit(bool complete, bool inbox,
                                         uint32_t current_revision,
                                         uint32_t *user_arrivals_out) {
    if (user_arrivals_out != NULL) {
        *user_arrivals_out = 0u;
    }
    if (!complete || !inbox ||
        current_revision != s_arrival_scan_revision) {
        return false;
    }

    uint32_t user_arrivals = 0u;
    for (uint16_t i = 0u; i < s_pending_arrival_count; i++) {
        uint8_t result = s_pending_arrival_results[i];
        if (result == MODEM_SMS_ARRIVAL_USER) {
            user_arrivals++;
        } else if (result >= SMS_ARRIVAL_DEFERRED_BASE) {
            uint8_t slot = (uint8_t)(result - SMS_ARRIVAL_DEFERRED_BASE);
            bool anchor_present = slot < MODEM_SMS_ARRIVAL_CLAIM_MAX &&
                (s_multipart_arrival_claims[slot].flags &
                 (SMS_ARRIVAL_CLAIM_USED |
                  SMS_ARRIVAL_CLAIM_ANCHOR_SEEN)) ==
                    (SMS_ARRIVAL_CLAIM_USED |
                     SMS_ARRIVAL_CLAIM_ANCHOR_SEEN);
            if (!anchor_present) {
                /* The claimed orphan disappeared before this snapshot. Treat
                 * the new segment as a fresh user message, never silently. */
                user_arrivals++;
            }
        }
    }
    for (uint8_t i = 0u; i < MODEM_SMS_ARRIVAL_CLAIM_MAX; i++) {
        uint8_t flags = s_multipart_arrival_claims[i].flags;
        if ((flags & SMS_ARRIVAL_CLAIM_USED) != 0u &&
            ((flags & SMS_ARRIVAL_CLAIM_COMPLETE) != 0u ||
             (flags & SMS_ARRIVAL_CLAIM_ANCHOR_SEEN) == 0u)) {
            memset(&s_multipart_arrival_claims[i], 0,
                   sizeof(s_multipart_arrival_claims[i]));
        }
    }
    s_pending_arrival_count = 0u;
    if (user_arrivals_out != NULL) {
        *user_arrivals_out = user_arrivals;
    }
    return true;
}

void modem_sms_state_filtered_reset(void) {
    s_filtered_count = 0u;
    s_filtered_pos = 0u;
    s_filtered_delete_failed = false;
    s_filtered_active = false;
}

bool modem_sms_state_filtered_queue(uint16_t index) {
    for (uint16_t i = 0u; i < s_filtered_count; i++) {
        if (s_filtered_indices[i] == index) {
            return true;
        }
    }
    if (s_filtered_count >= MODEM_SMS_RECORD_MAX) {
        s_filtered_delete_failed = true;
        return false;
    }
    s_filtered_indices[s_filtered_count++] = index;
    return true;
}

uint8_t modem_sms_state_filtered_count(void) {
    return s_filtered_count;
}

bool modem_sms_state_filtered_begin(void) {
    s_filtered_pos = 0u;
    s_filtered_active = s_filtered_count != 0u;
    return s_filtered_active;
}

bool modem_sms_state_filtered_active(void) {
    return s_filtered_active;
}

bool modem_sms_state_filtered_current(uint16_t *index_out) {
    if (index_out == NULL || !s_filtered_active ||
        s_filtered_pos >= s_filtered_count) {
        return false;
    }
    *index_out = s_filtered_indices[s_filtered_pos];
    return true;
}

bool modem_sms_state_filtered_advance(bool delete_ok) {
    if (!s_filtered_active || s_filtered_pos >= s_filtered_count) {
        return false;
    }
    if (!delete_ok) {
        s_filtered_delete_failed = true;
    }
    s_filtered_pos++;
    return s_filtered_pos < s_filtered_count;
}

void modem_sms_state_filtered_note_failure(void) {
    s_filtered_delete_failed = true;
}

void modem_sms_state_filtered_finish(modem_sms_filtered_summary_t *summary) {
    if (summary != NULL) {
        summary->count = s_filtered_count;
        summary->delete_ok = !s_filtered_delete_failed;
    }
    s_filtered_active = false;
}

bool modem_sms_state_publish_mailbox_result(
    uint32_t request_id, modem_sms_request_kind_t kind,
    modem_sms_outcome_t outcome, bool sim_not_ready, bool complete,
    modem_sms_mailbox_t mailbox) {
    if (kind != MODEM_SMS_REQUEST_MAILBOX ||
        outcome == MODEM_SMS_OUTCOME_NONE ||
        !claim_terminal(request_id, kind)) {
        return false;
    }
    s_mailbox_result = (modem_sms_mailbox_result_t){
        .request_id = request_id,
        .kind = kind,
        .outcome = outcome,
        .sim_not_ready = sim_not_ready,
        .complete = outcome == MODEM_SMS_OUTCOME_OK && complete,
        .mailbox = mailbox,
    };
    s_mailbox_result_pending = true;
    return true;
}

bool modem_sms_state_pop_mailbox_result(uint32_t request_id,
                                        modem_sms_mailbox_result_t *out) {
    if (request_id == 0u || out == NULL || !s_mailbox_result_pending ||
        s_mailbox_result.request_id != request_id) {
        return false;
    }
    *out = s_mailbox_result;
    memset(&s_mailbox_result, 0, sizeof(s_mailbox_result));
    s_mailbox_result_pending = false;
    return true;
}

bool modem_sms_state_publish_read_result(
    uint32_t request_id, modem_sms_request_kind_t kind,
    modem_sms_outcome_t outcome, bool sim_not_ready,
    uint32_t request_identity_hash, uint32_t identity_hash,
    const modem_sms_message_t *message) {
    if (kind != MODEM_SMS_REQUEST_READ ||
        outcome == MODEM_SMS_OUTCOME_NONE ||
        (outcome == MODEM_SMS_OUTCOME_OK && message == NULL) ||
        !claim_terminal(request_id, kind)) {
        return false;
    }
    memset(&s_read_result, 0, sizeof(s_read_result));
    s_read_result.request_id = request_id;
    s_read_result.kind = kind;
    s_read_result.outcome = outcome;
    s_read_result.sim_not_ready = sim_not_ready;
    s_read_result.request_identity_hash = request_identity_hash;
    if (outcome == MODEM_SMS_OUTCOME_OK) {
        s_read_result.message = *message;
        s_read_result.identity_hash = identity_hash;
    }
    s_read_result_pending = true;
    return true;
}

bool modem_sms_state_pop_read_result(uint32_t request_id,
                                     modem_sms_read_result_t *out) {
    if (request_id == 0u || out == NULL || !s_read_result_pending ||
        s_read_result.request_id != request_id) {
        return false;
    }
    *out = s_read_result;
    memset(&s_read_result, 0, sizeof(s_read_result));
    s_read_result_pending = false;
    return true;
}

bool modem_sms_state_publish_send_result(uint32_t request_id,
                                         modem_sms_request_kind_t kind,
                                         modem_sms_outcome_t outcome) {
    if ((kind != MODEM_SMS_REQUEST_SEND_TEXT &&
         kind != MODEM_SMS_REQUEST_SEND_BINARY) ||
        outcome == MODEM_SMS_OUTCOME_NONE ||
        !claim_terminal(request_id, kind)) {
        return false;
    }
    s_send_result = (modem_sms_send_result_t){
        .request_id = request_id,
        .kind = kind,
        .outcome = outcome,
    };
    s_send_result_pending = true;
    return true;
}

bool modem_sms_state_pop_send_result(uint32_t request_id,
                                     modem_sms_send_result_t *out) {
    if (request_id == 0u || out == NULL || !s_send_result_pending ||
        s_send_result.request_id != request_id) {
        return false;
    }
    *out = s_send_result;
    memset(&s_send_result, 0, sizeof(s_send_result));
    s_send_result_pending = false;
    return true;
}

bool modem_sms_state_publish_save_result(uint32_t request_id,
                                         modem_sms_request_kind_t kind,
                                         modem_sms_outcome_t outcome,
                                         bool sim_not_ready) {
    if (kind != MODEM_SMS_REQUEST_SAVE ||
        outcome == MODEM_SMS_OUTCOME_NONE ||
        !claim_terminal(request_id, kind)) {
        return false;
    }
    s_save_result = (modem_sms_save_result_t){
        .request_id = request_id,
        .kind = kind,
        .outcome = outcome,
        .sim_not_ready = sim_not_ready,
    };
    s_save_result_pending = true;
    return true;
}

bool modem_sms_state_pop_save_result(uint32_t request_id,
                                     modem_sms_save_result_t *out) {
    if (request_id == 0u || out == NULL || !s_save_result_pending ||
        s_save_result.request_id != request_id) {
        return false;
    }
    *out = s_save_result;
    memset(&s_save_result, 0, sizeof(s_save_result));
    s_save_result_pending = false;
    return true;
}

bool modem_sms_state_publish_delete_result(uint32_t request_id,
                                           modem_sms_request_kind_t kind,
                                           modem_sms_outcome_t outcome,
                                           bool sim_not_ready) {
    if (kind != MODEM_SMS_REQUEST_DELETE ||
        outcome == MODEM_SMS_OUTCOME_NONE ||
        !claim_terminal(request_id, kind)) {
        return false;
    }
    s_delete_result = (modem_sms_delete_result_t){
        .request_id = request_id,
        .kind = kind,
        .outcome = outcome,
        .sim_not_ready = sim_not_ready,
    };
    s_delete_result_pending = true;
    return true;
}

bool modem_sms_state_pop_delete_result(uint32_t request_id,
                                       modem_sms_delete_result_t *out) {
    if (request_id == 0u || out == NULL || !s_delete_result_pending ||
        s_delete_result.request_id != request_id) {
        return false;
    }
    *out = s_delete_result;
    memset(&s_delete_result, 0, sizeof(s_delete_result));
    s_delete_result_pending = false;
    return true;
}

#include "services/message_file_codec.h"

#include <string.h>
#include "services/sms_picture_codec.h"
#include "services/sms_vvm_filter.h"

#define KNOWN_FLAGS (MESSAGE_FILE_DRAFT | MESSAGE_FILE_QUARANTINED | MESSAGE_FILE_SENT)
#define CONCAT_WINDOW_SECONDS 86400u

/* Called only by the sole core0 message owner. Reuse decode scratch rather
 * than nesting several ~0.5 KiB decoded PDUs on the MCU's 4 KiB stack. No
 * decoded view escapes a public call, and these calls are not reentrant. */
static sms_codec_message_t s_first, s_part;
static message_file_part_t s_incoming;
static char s_hex[MESSAGE_FILE_PDU_MAX * 2u + 1u];
static uint8_t s_control[MODEM_SMS_BINARY_MAX];

static int nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool part_parse(const char *hex, message_file_part_t *part, sms_codec_message_t *decoded) {
    if (hex == NULL) return false;
    size_t len = strlen(hex);
    if (len == 0u || (len & 1u) || len > MESSAGE_FILE_PDU_MAX * 2u ||
        !sms_pdu_decode(hex, decoded)) return false;
    part->size = (uint8_t)(len / 2u);
    for (size_t i = 0; i < part->size; i++) {
        int hi = nibble(hex[2u * i]), lo = nibble(hex[2u * i + 1u]);
        if (hi < 0 || lo < 0) return false;
        part->bytes[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

static bool part_decode(const message_file_part_t *part, sms_codec_message_t *out) {
    if (part->size == 0u || part->size > MESSAGE_FILE_PDU_MAX) return false;
    static const char digits[] = "0123456789ABCDEF";
    char *hex = s_hex;
    for (unsigned i = 0u; i < part->size; i++) {
        hex[2u * i] = digits[part->bytes[i] >> 4];
        hex[2u * i + 1u] = digits[part->bytes[i] & 15u];
    }
    hex[part->size * 2u] = '\0';
    return sms_pdu_decode(hex, out);
}

static bool quarantined(const sms_codec_message_t *part) {
    return part->submit || part->udh_unhandled || part->trailing_data ||
        (part->has_concat && (part->concat_total == 0u ||
            part->concat_total > MODEM_SMS_SEGMENT_MAX || part->concat_seq == 0u ||
            part->concat_seq > part->concat_total));
}

static unsigned pair(const char *p) {
    return (unsigned)(p[0] - '0') * 10u + (unsigned)(p[1] - '0');
}

uint32_t message_timestamp_seconds(const char *timestamp) {
    if (timestamp == NULL || strlen(timestamp) < 17u || timestamp[2] != '/' ||
        timestamp[5] != '/' || timestamp[8] != ',' || timestamp[11] != ':' ||
        timestamp[14] != ':') return 0u;
    const unsigned fields[] = {0, 3, 6, 9, 12, 15};
    for (unsigned i = 0; i < 6; i++) {
        const char *p = timestamp + fields[i];
        if (p[0] < '0' || p[0] > '9' || p[1] < '0' || p[1] > '9') return 0u;
    }
    unsigned year = pair(timestamp), month = pair(timestamp + 3), day = pair(timestamp + 6);
    unsigned hour = pair(timestamp + 9), minute = pair(timestamp + 12), second = pair(timestamp + 15);
    static const uint8_t days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 0u || month > 12u || day == 0u || hour > 23u || minute > 59u || second > 59u)
        return 0u;
    bool leap = year % 4u == 0u; /* 2000..2099 */
    if (day > days[month - 1u] + (month == 2u && leap ? 1u : 0u)) return 0u;
    uint32_t total = year * 365u + (year + 3u) / 4u + day - 1u;
    for (unsigned m = 1u; m < month; m++) total += days[m - 1u] + (m == 2u && leap ? 1u : 0u);
    return total * 86400u + hour * 3600u + minute * 60u + second;
}

static bool same_group(const sms_codec_message_t *a, const sms_codec_message_t *b) {
    uint32_t ta = message_timestamp_seconds(a->timestamp), tb = message_timestamp_seconds(b->timestamp);
    uint32_t distance = ta > tb ? ta - tb : tb - ta;
    return a->has_concat && b->has_concat && a->concat_ref == b->concat_ref &&
        a->concat_ref_16bit == b->concat_ref_16bit && a->concat_total == b->concat_total &&
        a->pid == b->pid && a->dcs == b->dcs && a->has_ports == b->has_ports &&
        (!a->has_ports || (a->dest_port == b->dest_port && a->source_port == b->source_port)) &&
        strcmp(a->address, b->address) == 0 && ta != 0u && tb != 0u &&
        distance <= CONCAT_WINDOW_SECONDS;
}

static bool valid_file(const message_file_t *file) {
    if (file == NULL || (file->flags & ~KNOWN_FLAGS)) return false;
    if (file->flags & MESSAGE_FILE_DRAFT) {
        return !(file->flags & MESSAGE_FILE_QUARANTINED) && file->count == 0u &&
            memchr(file->draft.address, 0, sizeof(file->draft.address)) != NULL &&
            memchr(file->draft.text, 0, sizeof(file->draft.text)) != NULL;
    }
    if (file->count == 0u || file->count > MESSAGE_FILE_PART_LIMIT) return false;
    sms_codec_message_t *first = &s_first, *part = &s_part;
    if (!part_decode(&file->parts[0], first)) return false;
    bool quarantine = (file->flags & MESSAGE_FILE_QUARANTINED) != 0u;
    if (quarantined(first) && !quarantine) return false;
    uint8_t mask = 0u;
    for (unsigned i = 0u; i < file->count; i++) {
        if (!part_decode(&file->parts[i], part) ||
            (i != 0u && !same_group(first, part))) return false;
        if (quarantine) continue;
        if (quarantined(part)) return false;
        if (!part->has_concat) return file->count == 1u;
        uint8_t bit = (uint8_t)(1u << (part->concat_seq - 1u));
        if (mask & bit) return false;
        mask |= bit;
    }
    return true;
}

bool message_file_draft(message_file_t *file, const char *address, const char *text) {
    if (file == NULL || address == NULL || text == NULL ||
        strlen(address) > MODEM_SMS_SENDER_MAX || strlen(text) > MODEM_SMS_TEXT_MAX) return false;
    memset(file, 0, sizeof(*file));
    file->flags = MESSAGE_FILE_DRAFT;
    strcpy(file->draft.address, address);
    strcpy(file->draft.text, text);
    return true;
}

bool message_file_receive(message_file_t *file, const char *pdu) {
    if (file == NULL) return false;
    memset(file, 0, sizeof(*file));
    sms_codec_message_t *decoded = &s_part;
    if (!part_parse(pdu, &file->parts[0], decoded)) return false;
    file->count = 1u;
    if (quarantined(decoded)) file->flags |= MESSAGE_FILE_QUARANTINED;
    return true;
}

message_merge_t message_file_merge(message_file_t *file, const char *pdu) {
    if (file == NULL || (file->flags & MESSAGE_FILE_DRAFT) || file->count == 0u ||
        file->count > MESSAGE_FILE_PART_LIMIT) return MESSAGE_MERGE_UNRELATED;
    message_file_part_t *incoming = &s_incoming;
    sms_codec_message_t *a = &s_first, *b = &s_part;
    if (!part_parse(pdu, incoming, b) || !part_decode(&file->parts[0], a))
        return MESSAGE_MERGE_UNRELATED;
    for (uint8_t i = 0; i < file->count; i++) {
        if (file->parts[i].size == incoming->size &&
            memcmp(file->parts[i].bytes, incoming->bytes, incoming->size) == 0)
            return MESSAGE_MERGE_DUPLICATE;
    }
    if (!same_group(a, b)) return MESSAGE_MERGE_UNRELATED;
    if (file->count == MESSAGE_FILE_PART_LIMIT) return MESSAGE_MERGE_FULL;
    bool conflict = quarantined(b);
    for (uint8_t i = 0; i < file->count; i++) {
        if (!part_decode(&file->parts[i], a)) return MESSAGE_MERGE_UNRELATED;
        if (a->concat_seq == b->concat_seq) conflict = true;
    }
    file->parts[file->count++] = *incoming;
    if (conflict) file->flags |= MESSAGE_FILE_QUARANTINED;
    return MESSAGE_MERGE_ADDED;
}

bool message_file_complete(const message_file_t *file) {
    if (!valid_file(file)) return false;
    if (file->flags & MESSAGE_FILE_DRAFT) return true;
    if (file->count == 0u || file->count > MESSAGE_FILE_PART_LIMIT) return false;
    if (file->flags & MESSAGE_FILE_QUARANTINED) return true;
    sms_codec_message_t *part = &s_part;
    if (!part_decode(&file->parts[0], part)) return false;
    if (!part->has_concat) return file->count == 1u;
    uint8_t total = part->concat_total, mask = 0u;
    if (total == 0u || total > MODEM_SMS_SEGMENT_MAX || total != file->count) return false;
    for (uint8_t i = 0u; i < file->count; i++) {
        if (!part_decode(&file->parts[i], part) || part->concat_seq == 0u || part->concat_seq > total)
            return false;
        mask |= (uint8_t)(1u << (part->concat_seq - 1u));
    }
    return mask == (uint8_t)((1u << total) - 1u);
}

bool message_file_is_vvm_control(const message_file_t *file) {
    if (!message_file_complete(file) ||
        (file->flags & (MESSAGE_FILE_DRAFT | MESSAGE_FILE_QUARANTINED)) ||
        !part_decode(&file->parts[0], &s_first) || s_first.pid != 0u ||
        (s_first.dcs != 0u && s_first.dcs != 4u && s_first.dcs != 8u)) return false;
    size_t used = 0u;
    for (uint8_t seq = 1u; seq <= file->count; seq++) {
        bool found = false;
        for (uint8_t i = 0u; i < file->count; i++) {
            if (!part_decode(&file->parts[i], &s_part)) return false;
            if (!s_part.has_concat || s_part.concat_seq == seq) { found = true; break; }
        }
        if (!found || s_part.binary != s_first.binary) return false;
        size_t n = s_part.binary ? s_part.binary_len : strlen(s_part.text);
        if (n > sizeof(s_control) - used) return false;
        memcpy(s_control + used, s_part.binary ? s_part.binary_data :
               (const uint8_t *)s_part.text, n);
        used += n;
    }
    return sms_vvm_control_payload_is_recognized(s_first.has_ports,
                s_first.dest_port, s_control, used);
}

bool message_file_encode(const message_file_t *file, uint8_t *dst, size_t cap, size_t *len) {
    if (!valid_file(file) || dst == NULL || len == NULL || cap < 8u) return false;
    memset(dst, 0, 8u);
    dst[0] = 'S'; dst[1] = 'M'; dst[2] = 1u; dst[3] = file->flags;
    size_t used = 8u;
    if (file->flags & MESSAGE_FILE_DRAFT) {
        size_t address = strlen(file->draft.address), text = strlen(file->draft.text);
        if (address > MODEM_SMS_SENDER_MAX || text > MODEM_SMS_TEXT_MAX || cap - used < address + text)
            return false;
        dst[4] = (uint8_t)address; dst[6] = (uint8_t)text;
        memcpy(dst + used, file->draft.address, address); used += address;
        memcpy(dst + used, file->draft.text, text); used += text;
    } else {
        if (file->count == 0u || file->count > MESSAGE_FILE_PART_LIMIT) return false;
        dst[4] = file->count;
        for (uint8_t i = 0u; i < file->count; i++) {
            size_t n = file->parts[i].size;
            if (n == 0u || n > MESSAGE_FILE_PDU_MAX || cap - used < n + 1u) return false;
            dst[used++] = (uint8_t)n;
            memcpy(dst + used, file->parts[i].bytes, n); used += n;
        }
    }
    *len = used;
    return true;
}

bool message_file_decode(message_file_t *file, const uint8_t *src, size_t len) {
    if (file == NULL || src == NULL || len < 8u || src[0] != 'S' || src[1] != 'M' ||
        src[2] != 1u || (src[3] & ~KNOWN_FLAGS) || src[5] != 0u || src[7] != 0u) return false;
    memset(file, 0, sizeof(*file));
    file->flags = src[3];
    size_t pos = 8u;
    if (file->flags & MESSAGE_FILE_DRAFT) {
        if (src[4] > MODEM_SMS_SENDER_MAX || src[6] > MODEM_SMS_TEXT_MAX ||
            len != pos + src[4] + src[6] || memchr(src + pos, 0, len - pos) != NULL) return false;
        memcpy(file->draft.address, src + pos, src[4]); pos += src[4];
        memcpy(file->draft.text, src + pos, src[6]); pos += src[6];
    } else {
        if (src[4] == 0u || src[4] > MESSAGE_FILE_PART_LIMIT || src[6] != 0u) return false;
        file->count = src[4];
        for (uint8_t i = 0; i < file->count; i++) {
            if (pos == len) return false;
            uint8_t n = src[pos++];
            if (n == 0u || n > MESSAGE_FILE_PDU_MAX || n > len - pos) return false;
            file->parts[i].size = n;
            memcpy(file->parts[i].bytes, src + pos, n); pos += n;
        }
    }
    return pos == len && valid_file(file);
}

bool message_file_metadata(const message_file_t *file, message_metadata_t *out) {
    if (!valid_file(file) || out == NULL) return false;
    memset(out, 0, sizeof(*out));
    out->sent = (file->flags & MESSAGE_FILE_SENT) != 0u;
    out->quarantined = (file->flags & MESSAGE_FILE_QUARANTINED) != 0u;
    if (file->flags & MESSAGE_FILE_DRAFT) {
        strcpy(out->address, file->draft.address);
        return true;
    }
    sms_codec_message_t *part = &s_part;
    if (file->count == 0u || !part_decode(&file->parts[0], part)) return false;
    strcpy(out->address, part->address);
    strcpy(out->timestamp, part->timestamp);
    out->binary = part->binary || part->has_ports || out->quarantined;
    return true;
}

bool message_file_content(const message_file_t *file, message_content_t *out) {
    if (out == NULL || !message_file_complete(file) || !message_file_metadata(file, &out->metadata))
        return false;
    out->text[0] = '\0';
    if (file->flags & MESSAGE_FILE_DRAFT) {
        strcpy(out->text, file->draft.text);
        return true;
    }
    if (out->metadata.binary) return true;
    size_t used = 0u;
    for (uint8_t seq = 1u; seq <= file->count; seq++) {
        sms_codec_message_t *part = &s_part;
        bool found = false;
        for (uint8_t i = 0u; i < file->count; i++) {
            if (!part_decode(&file->parts[i], part)) return false;
            if (!part->has_concat || part->concat_seq == seq) { found = true; break; }
        }
        if (!found) return false;
        size_t n = strlen(part->text);
        if (n > MESSAGE_TEXT_MAX - used) return false;
        memcpy(out->text + used, part->text, n + 1u); used += n;
    }
    return true;
}

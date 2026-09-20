#include "services/business_card_codec.h"

#include <stdio.h>
#include <string.h>

static uint8_t s_payload[BUSINESS_CARD_PAYLOAD_MAX + 1u];
static sms_codec_message_t s_part;

static bool equal(const uint8_t *s, size_t n, const char *word) {
    if (strlen(word) != n) return false;
    for (size_t i = 0u; i < n; i++) {
        unsigned c = s[i];
        if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
        if (c != (unsigned char)word[i]) return false;
    }
    return true;
}

bool business_card_text_envelope(const char *text) {
    return text != NULL && strncmp(text, "//SCK", 5u) == 0 &&
        (text[5] == 'E' || text[5] == 'e') && text[6] == '2';
}

static int hex(unsigned c) {
    if (c >= '0' && c <= '9') return (int)c - '0';
    if (c >= 'A' && c <= 'F') return (int)c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return (int)c - 'a' + 10;
    return -1;
}

static bool text_value(const uint8_t *s, size_t n, bool utf8, bool quoted,
                       char *out, size_t cap) {
    /* Name conversion is bounded by the local phonebook, not a modem field. */
    uint8_t bytes[PHONEBOOK_NAME_MAX + 1u];
    size_t count = 0u, used = 0u;
    for (size_t i = 0u; i < n; i++) {
        unsigned c = s[i];
        if (quoted && c == '=') {
            if (i + 2u >= n || hex(s[i + 1u]) < 0 || hex(s[i + 2u]) < 0) return false;
            c = (unsigned)((hex(s[i + 1u]) << 4) | hex(s[i + 2u]));
            i += 2u;
        } else if (c == '\\') {
            if (++i == n || (s[i] != '\\' && s[i] != ';' && s[i] != ',')) return false;
            c = s[i];
        }
        if (c < 32u || c == 127u || count == sizeof(bytes) - 1u) return false;
        bytes[count++] = (uint8_t)c;
    }
    if (utf8) {
        /* Reuse the strict UTF-8 validator/converter used by SMS sending. */
        uint8_t ucs2[2u * PHONEBOOK_NAME_MAX];
        size_t size;
        bytes[count] = 0u;
        if (!sms_ucs2_from_utf8((const char *)bytes, ucs2, sizeof(ucs2), &size) || count >= cap)
            return false;
        memcpy(out, bytes, count);
        used = count;
    } else {
        for (size_t i = 0; i < count; i++) {
            unsigned c = bytes[i];
            size_t width = c < 128u ? 1u : 2u;
            if (used + width >= cap) return false;
            if (width == 2u) out[used++] = (char)(0xc0u | (c >> 6));
            out[used++] = (char)(width == 2u ? 0x80u | (c & 63u) : c);
        }
    }
    out[used] = '\0';
    return true;
}

static bool number_value(const uint8_t *s, size_t n, char *out) {
    size_t used = 0u;
    for (size_t i = 0u; i < n; i++) {
        unsigned c = s[i];
        if (c == ' ' || c == '-' || c == '(' || c == ')' || c == '.') continue;
        if (!((c >= '0' && c <= '9') || c == '*' || c == '#' ||
              c == 'p' || c == 'w' || (c == '+' && used == 0u)) || used == MODEM_PHONE_MAX)
            return false;
        out[used++] = (char)c;
    }
    out[used] = '\0';
    return used != 0u && !(used == 1u && out[0] == '+');
}

bool business_card_encode(const phonebook_entry_t *entry, char *out, size_t cap) {
    if (entry == NULL || out == NULL || cap == 0u ||
        memchr(entry->name, 0, sizeof(entry->name)) == NULL ||
        memchr(entry->number, 0, sizeof(entry->number)) == NULL) return false;
    phonebook_entry_t checked = {0};
    /* The original emits the name literally. Reject line/property injection. */
    uint8_t ucs2[PHONEBOOK_NAME_MAX * 2u];
    size_t size;
    if (!sms_ucs2_from_utf8(entry->name, ucs2, sizeof(ucs2), &size) ||
        !number_value((const uint8_t *)entry->number, strlen(entry->number), checked.number)) return false;
    for (const unsigned char *p = (const unsigned char *)entry->name; *p; p++)
        if (*p < 32u || *p == 127u || *p == '\\') return false;
    int n = entry->name[0]
        ? snprintf(out, cap, "//SCKE2 BEGIN:VCARD\r\nN:%s\r\nTEL:%s\r\nEND:VCARD\r\n",
                   entry->name, checked.number)
        : snprintf(out, cap, "//SCKE2 BEGIN:VCARD\r\nTEL:%s\r\nEND:VCARD\r\n", checked.number);
    if (n < 0 || (size_t)n >= cap || n > (int)MODEM_SMS_TEXT_MAX) { out[0] = '\0'; return false; }
    return true;
}

bool business_card_decode(const uint8_t *body, size_t len, bool utf8,
                          phonebook_entry_t *out) {
    if (body == NULL || out == NULL || len == 0u || len > BUSINESS_CARD_PAYLOAD_MAX ||
        memchr(body, 0, len) != NULL) return false;
    phonebook_entry_t card = {0};
    bool began = false, ended = false, named = false, fn = false, tel = false;
    size_t pos = 0u;
    while (pos < len) {
        size_t end = pos;
        while (end < len && body[end] != '\r' && body[end] != '\n') end++;
        if (end + 1u >= len || body[end] != '\r' || body[end + 1u] != '\n') return false;
        const uint8_t *line = body + pos;
        size_t size = end - pos;
        pos = end + 2u;
        if (!began) {
            if (!equal(line, size, "BEGIN:VCARD")) return false;
            began = true;
            continue;
        }
        if (equal(line, size, "END:VCARD")) { ended = true; break; }
        size_t colon = 0u, key = 0u;
        while (colon < size && line[colon] != ':') colon++;
        if (colon == size) return false;
        while (key < colon && line[key] != ';') key++;
        bool is_fn = equal(line, key, "FN"), is_n = equal(line, key, "N");
        bool is_tel = equal(line, key, "TEL");
        if (equal(line, key, "BEGIN") || equal(line, key, "END")) return false;
        if (!is_fn && !is_n && !is_tel) continue;
        bool value_utf8 = utf8, quoted = false, latin1 = false;
        for (size_t p = key; p < colon;) {
            size_t start = ++p;
            while (p < colon && line[p] != ';') p++;
            size_t n = p - start;
            if (n >= 8u && equal(line + start, 8u, "CHARSET=")) {
                if (equal(line + start + 8u, n - 8u, "UTF-8")) value_utf8 = true;
                else if (equal(line + start + 8u, n - 8u, "ISO-8859-1") ||
                         equal(line + start + 8u, n - 8u, "US-ASCII")) latin1 = true;
                else return false;
            } else if (n >= 9u && equal(line + start, 9u, "ENCODING=")) {
                if (!equal(line + start + 9u, n - 9u, "QUOTED-PRINTABLE")) return false;
                quoted = true;
            }
        }
        const uint8_t *value = line + colon + 1u;
        size_t n = size - colon - 1u;
        if (quoted && latin1) value_utf8 = false;
        if (is_tel) {
            if (quoted) return false;
            if (!tel && !number_value(value, n, card.number)) return false;
            tel = true;
        } else if (is_fn || !fn) {
            if ((is_fn && fn) || (is_n && named)) return false;
            if (!text_value(value, n, value_utf8, quoted, card.name, sizeof(card.name))) return false;
            if (is_fn) fn = true; else named = true;
        }
    }
    if (!ended || pos != len || !tel) return false;
    *out = card;
    return true;
}

static bool card_port(uint16_t port) {
    return port == 0xe2u || port == 0x23f4u || port == 0xc004u;
}

business_card_result_t business_card_file_decode(const message_file_t *file,
                                                phonebook_entry_t *out) {
    if (file == NULL || out == NULL || file->flags & MESSAGE_FILE_DRAFT ||
        file->count == 0u || file->count > MESSAGE_FILE_PART_LIMIT) return BUSINESS_CARD_NONE;
    bool recognized = false, binary = false, ported = false, invalid = false;
    bool eligible = true;
    for (unsigned i = 0u; i < file->count; i++) {
        if (!message_file_part_decode(&file->parts[i], &s_part)) return BUSINESS_CARD_INVALID;
        if (s_part.has_ports && card_port(s_part.dest_port)) {
            recognized = true; ported = true; binary = s_part.binary;
        } else if (!s_part.has_ports && !s_part.binary &&
                   (!s_part.has_concat || s_part.concat_seq == 1u) &&
                   business_card_text_envelope(s_part.text)) recognized = true;
        else if (s_part.has_ports || s_part.binary) eligible = false;
        invalid |= s_part.submit || s_part.pid != 0u || s_part.udh_unhandled || s_part.trailing_data;
    }
    if (!recognized && !eligible) return BUSINESS_CARD_NONE;
    if ((file->flags & MESSAGE_FILE_QUARANTINED) || !message_file_complete(file))
        return recognized ? BUSINESS_CARD_INVALID : BUSINESS_CARD_NONE;
    size_t used = 0u;
    for (unsigned seq = 1u; seq <= file->count; seq++) {
        bool found = false;
        for (unsigned i = 0u; i < file->count; i++) {
            if (!message_file_part_decode(&file->parts[i], &s_part)) return BUSINESS_CARD_INVALID;
            if (!s_part.has_concat || s_part.concat_seq == seq) { found = true; break; }
        }
        if (!found || s_part.binary != binary)
            return recognized ? BUSINESS_CARD_INVALID : BUSINESS_CARD_NONE;
        size_t size = binary ? s_part.binary_len : strlen(s_part.text);
        if (size > BUSINESS_CARD_PAYLOAD_MAX - used)
            return recognized || (used >= 7u && business_card_text_envelope((const char *)s_payload))
                ? BUSINESS_CARD_INVALID : BUSINESS_CARD_NONE;
        memcpy(s_payload + used, binary ? s_part.binary_data : (const uint8_t *)s_part.text, size);
        used += size;
    }
    s_payload[used] = 0u;
    size_t start = 0u;
    if (!ported) {
        if (!business_card_text_envelope((const char *)s_payload)) return BUSINESS_CARD_NONE;
        if (used < 8u || s_payload[7] != ' ') return BUSINESS_CARD_INVALID;
        start = 8u;
    }
    if (invalid || !eligible) return BUSINESS_CARD_INVALID;
    return business_card_decode(s_payload + start, used - start, !binary, out)
        ? BUSINESS_CARD_VALID : BUSINESS_CARD_INVALID;
}

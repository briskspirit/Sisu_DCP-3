#include "services/sms_picture_codec.h"

#include <string.h>

#define SMS_PICTURE_CHUNK_TEXT 0x01u
#define SMS_PICTURE_CHUNK_BITMAP 0x02u
#define SMS_PICTURE_CHUNK_MARKER 0x00u
#define SMS_PICTURE_FORMAT 0x01u
#define SMS_TP_UD_MAX_OCTETS 140u

typedef enum {
    SMS_DCS_GSM7 = 0,
    SMS_DCS_UCS2,
    SMS_DCS_BINARY,
} sms_dcs_kind_t;

static uint16_t picture_bitmap_bytes(uint8_t width, uint8_t height);
static bool read_hex_byte(const char *hex, size_t hex_len, size_t *pos, uint8_t *out);
static bool decode_address(const uint8_t *src, size_t available,
                           uint8_t address_length, uint8_t toa,
                           char *dst, size_t cap, size_t *consumed);
static uint8_t semi_decimal(uint8_t value);
static void decode_timestamp(const uint8_t *src, char *dst, size_t cap);
static void parse_udh(const uint8_t *data, uint8_t data_len, sms_codec_message_t *out, uint8_t *payload_offset);
static sms_dcs_kind_t dcs_kind(uint8_t dcs);
static bool decode_user_data(const uint8_t *data, size_t available,
                             uint8_t udl, bool udhi, uint8_t dcs,
                             sms_codec_message_t *out);
static bool decode_gsm7_user_data(const uint8_t *data, uint8_t data_len,
                                  uint8_t udl, bool udhi,
                                  sms_codec_message_t *out);
static bool decode_gsm7_utf8(const uint8_t *data, uint8_t data_len,
                             uint8_t septets, uint16_t bit_offset,
                             char *dst, size_t cap);
static bool decode_ucs2(const uint8_t *data, uint8_t data_len,
                        char *dst, size_t cap);
static bool gsm7_septet_at(const uint8_t *data, uint8_t data_len,
                           uint16_t bit_offset, uint8_t index,
                           uint8_t *out);
static uint32_t gsm7_default_codepoint(uint8_t value);
static uint32_t gsm7_extension_codepoint(uint8_t value);
static bool append_utf8(char *dst, size_t cap, size_t *pos,
                        uint32_t codepoint);
static size_t bounded_strlen(const char *text, size_t cap);

bool sms_picture_payload_encode(const store_picture_message_t *message,
                                const char *text,
                                uint8_t *dst,
                                size_t cap,
                                uint16_t *out_len,
                                uint8_t *out_chunks) {
    if (message == 0 || dst == 0 || out_len == 0 || out_chunks == 0) {
        return false;
    }
    size_t pos = 0u;
    if (pos + 1u > cap) {
        return false;
    }
    dst[pos++] = 0x30u;
    size_t text_len = text != 0 ? bounded_strlen(text, STORE_PICTURE_TEXT_MAX) : 0u;
    if (text_len != 0u) {
        if (pos + 3u + text_len > cap || text_len > 0xffu) {
            return false;
        }
        dst[pos++] = SMS_PICTURE_CHUNK_TEXT;
        dst[pos++] = 0x00u;
        dst[pos++] = (uint8_t)text_len;
        memcpy(&dst[pos], text, text_len);
        pos += text_len;
    }

    uint8_t width = message->width == 0u ? STORE_PICTURE_WIDTH : message->width;
    uint8_t height = message->height == 0u ? STORE_PICTURE_HEIGHT : message->height;
    uint16_t bitmap_len = message->bitmap_len <= STORE_PICTURE_BITMAP_BYTES
        ? message->bitmap_len
        : STORE_PICTURE_BITMAP_BYTES;
    uint16_t chunk_len = (uint16_t)(bitmap_len + 4u);
    if (pos + 3u + chunk_len > cap) {
        return false;
    }
    dst[pos++] = SMS_PICTURE_CHUNK_BITMAP;
    dst[pos++] = (uint8_t)(chunk_len >> 8);
    dst[pos++] = (uint8_t)(chunk_len & 0xffu);
    dst[pos++] = SMS_PICTURE_CHUNK_MARKER;
    dst[pos++] = width;
    dst[pos++] = height;
    dst[pos++] = SMS_PICTURE_FORMAT;
    memcpy(&dst[pos], message->bitmap, bitmap_len);
    pos += bitmap_len;
    *out_len = (uint16_t)pos;
    *out_chunks = (uint8_t)((pos + 0x7fu) / 0x80u);
    if (*out_chunks == 0u) {
        *out_chunks = 1u;
    }
    return true;
}

bool sms_picture_payload_decode(const uint8_t *payload,
                                uint16_t payload_len,
                                store_picture_message_t *out_message) {
    if (payload == 0 || payload_len < 1u || out_message == 0 || payload[0] != 0x30u) {
        return false;
    }
    store_picture_message_t decoded;
    memset(&decoded, 0, sizeof(decoded));
    size_t pos = 1u;
    bool have_picture = false;
    bool have_text = false;
    while (pos + 3u <= payload_len) {
        uint8_t kind = payload[pos++];
        uint16_t len = (uint16_t)(((uint16_t)payload[pos] << 8) | payload[pos + 1u]);
        pos += 2u;
        if (pos + len > payload_len) {
            return false;
        }
        if (kind == SMS_PICTURE_CHUNK_TEXT) {
            if (have_text || len > STORE_PICTURE_TEXT_MAX || memchr(&payload[pos], '\0', len) != NULL) {
                return false;
            }
            memcpy(decoded.text, &payload[pos], len);
            decoded.text[len] = '\0';
            have_text = true;
        } else if (kind == SMS_PICTURE_CHUNK_BITMAP) {
            if (have_picture || len < 4u || payload[pos] != SMS_PICTURE_CHUNK_MARKER ||
                payload[pos + 3u] != SMS_PICTURE_FORMAT) {
                return false;
            }
            decoded.width = payload[pos + 1u];
            decoded.height = payload[pos + 2u];
            uint16_t expected = picture_bitmap_bytes(decoded.width, decoded.height);
            if (expected == 0u || decoded.width > STORE_PICTURE_WIDTH ||
                decoded.height > STORE_PICTURE_HEIGHT ||
                expected > STORE_PICTURE_BITMAP_BYTES || len - 4u != expected) {
                return false;
            }
            decoded.bitmap_len = expected;
            memcpy(decoded.bitmap, &payload[pos + 4u], expected);
            have_picture = true;
        }
        pos += len;
    }
    if (!have_picture || pos != payload_len) {
        return false;
    }
    decoded.used = true;
    *out_message = decoded;
    return true;
}

bool sms_pdu_decode(const char *hex, sms_codec_message_t *out_message) {
    if (hex == 0 || out_message == 0) {
        return false;
    }
    uint8_t pdu[192];
    /* Cap the hex scan at exactly what pdu[] can hold (2 hex chars per byte) so
     * a PDU between 193 and 210 bytes is not accepted by the length check and
     * then silently dropped mid-decode. */
    const size_t max_hex_len = sizeof(pdu) * 2u;
    size_t hex_len = bounded_strlen(hex, max_hex_len + 1u);
    if (hex_len > max_hex_len) {
        return false;
    }
    size_t hex_pos = 0u;
    size_t pdu_len = 0u;
    while (hex_pos < hex_len) {
        if (pdu_len >= sizeof(pdu) || !read_hex_byte(hex, hex_len, &hex_pos, &pdu[pdu_len])) {
            return false;
        }
        pdu_len++;
    }
    if (pdu_len < 2u) {
        return false;
    }

    sms_codec_message_t decoded;
    memset(&decoded, 0, sizeof(decoded));
    size_t pos = 0u;
    uint8_t smsc_len = pdu[pos++];
    if (pos + smsc_len >= pdu_len) {
        return false;
    }
    pos += smsc_len;
    uint8_t first = pdu[pos++];
    uint8_t mti = first & 0x03u;
    bool udhi = (first & 0x40u) != 0u;

    if (mti == 0u) {
        if (pos + 2u > pdu_len) {
            return false;
        }
        uint8_t address_length = pdu[pos++];
        uint8_t toa = pdu[pos++];
        size_t address_bytes = 0u;
        if (!decode_address(&pdu[pos], pdu_len - pos, address_length, toa,
                            decoded.address, sizeof(decoded.address),
                            &address_bytes) ||
            pos + address_bytes + 10u > pdu_len) {
            return false;
        }
        pos += address_bytes;
        decoded.pid = pdu[pos++];
        uint8_t dcs = pdu[pos++];
        decoded.dcs = dcs;
        decode_timestamp(&pdu[pos], decoded.timestamp, sizeof(decoded.timestamp));
        pos += 7u;
        if (pos >= pdu_len) {
            return false;
        }
        uint8_t udl = pdu[pos++];
        if (!decode_user_data(&pdu[pos], pdu_len - pos, udl, udhi,
                              dcs, &decoded)) {
            return false;
        }
    } else if (mti == 1u) {
        decoded.submit = true;
        if (pos + 3u > pdu_len) {
            return false;
        }
        pos++; /* TP-MR */
        uint8_t address_length = pdu[pos++];
        uint8_t toa = pdu[pos++];
        size_t address_bytes = 0u;
        if (!decode_address(&pdu[pos], pdu_len - pos, address_length, toa,
                            decoded.address, sizeof(decoded.address),
                            &address_bytes) ||
            pos + address_bytes + 3u > pdu_len) {
            return false;
        }
        pos += address_bytes;
        decoded.pid = pdu[pos++];
        uint8_t dcs = pdu[pos++];
        decoded.dcs = dcs;
        uint8_t vpf = (uint8_t)((first >> 3) & 0x03u);
        if (vpf == 2u) {
            if (pos + 1u > pdu_len) {
                return false;
            }
            pos += 1u;
        } else if (vpf == 1u || vpf == 3u) {
            if (pos + 7u > pdu_len) {
                return false;
            }
            pos += 7u;
        }
        if (pos >= pdu_len) {
            return false;
        }
        uint8_t udl = pdu[pos++];
        if (!decode_user_data(&pdu[pos], pdu_len - pos, udl, udhi,
                              dcs, &decoded)) {
            return false;
        }
    } else {
        return false;
    }

    *out_message = decoded;
    return true;
}

static uint16_t picture_bitmap_bytes(uint8_t width, uint8_t height) {
    if (width == 0u || height == 0u || width > STORE_PICTURE_WIDTH || height > STORE_PICTURE_HEIGHT) {
        return 0u;
    }
    return (uint16_t)(((uint16_t)width * (uint16_t)height + 7u) / 8u);
}

static bool read_hex_byte(const char *hex, size_t hex_len, size_t *pos, uint8_t *out) {
    uint8_t value = 0u;
    for (uint8_t i = 0u; i < 2u; i++) {
        while (*pos < hex_len && (hex[*pos] == ' ' || hex[*pos] == '\r' || hex[*pos] == '\n')) {
            (*pos)++;
        }
        if (*pos >= hex_len) {
            return false;
        }
        char ch = hex[(*pos)++];
        uint8_t nibble;
        if (ch >= '0' && ch <= '9') {
            nibble = (uint8_t)(ch - '0');
        } else if (ch >= 'A' && ch <= 'F') {
            nibble = (uint8_t)(ch - 'A' + 10);
        } else if (ch >= 'a' && ch <= 'f') {
            nibble = (uint8_t)(ch - 'a' + 10);
        } else {
            return false;
        }
        value = (uint8_t)((value << 4) | nibble);
    }
    *out = value;
    return true;
}

static bool decode_address(const uint8_t *src, size_t available,
                           uint8_t address_length, uint8_t toa,
                           char *dst, size_t cap, size_t *consumed) {
    if (src == 0 || dst == 0 || cap == 0u || consumed == 0) {
        return false;
    }
    /* TS 23.040 9.1.2.5 caps the complete address field at 12 octets:
     * length + TOA + at most ten value octets. Address-Length always counts
     * useful semi-octets, including when TON=5 changes the value encoding to
     * packed GSM 7-bit. */
    size_t value_bytes = (size_t)(address_length + 1u) / 2u;
    if (value_bytes > 10u || available < value_bytes) {
        return false;
    }
    uint8_t ton = (uint8_t)((toa >> 4u) & 0x07u);
    if (ton == 5u) {
        uint8_t septets = (uint8_t)(((uint16_t)address_length * 4u) / 7u);
        uint8_t canonical_length = (uint8_t)(
            ((uint16_t)septets * 7u + 3u) / 4u);
        if (canonical_length != address_length ||
            !decode_gsm7_utf8(src, (uint8_t)value_bytes, septets, 0u,
                              dst, cap)) {
            return false;
        }
        *consumed = value_bytes;
        return true;
    }

    size_t out = 0u;
    if (ton == 1u) {
        if (out + 1u >= cap) {
            return false;
        }
        dst[out++] = '+';
    }
    for (uint8_t i = 0u; i < address_length; i++) {
        uint8_t byte = src[i / 2u];
        uint8_t nibble = (i & 1u) == 0u ? (byte & 0x0fu) : ((byte >> 4) & 0x0fu);
        char digit = '\0';
        if (nibble <= 9u) {
            digit = (char)('0' + nibble);
        } else if (nibble == 0x0au) {
            digit = '*'; /* TS 23.040 9.1.2.3 BCD digit 'A' */
        } else if (nibble == 0x0bu) {
            digit = '#'; /* BCD digit 'B' */
        } else if (nibble >= 0x0cu && nibble <= 0x0eu) {
            digit = (char)('a' + (nibble - 0x0cu));
        }
        if (digit != '\0') {
            if (out + 1u >= cap) {
                return false;
            }
            dst[out++] = digit;
        }
        /* 0xF is the odd-length filler and is not displayable. */
    }
    dst[out] = '\0';
    /* A draft saved before its recipient is entered has a valid zero-length
     * TP-DA. Telit emits national TOA 0x81 for that case, so there is no '+' to
     * make out nonzero. Nonempty addresses must still yield a decoded digit. */
    if (address_length != 0u && out == 0u) {
        return false;
    }
    *consumed = value_bytes;
    return true;
}

static uint8_t semi_decimal(uint8_t value) {
    return (uint8_t)((value & 0x0fu) * 10u + ((value >> 4) & 0x0fu));
}

static void decode_timestamp(const uint8_t *src, char *dst, size_t cap) {
    if (src == 0 || dst == 0 || cap == 0u) {
        return;
    }
    uint8_t yy = semi_decimal(src[0]);
    uint8_t mm = semi_decimal(src[1]);
    uint8_t dd = semi_decimal(src[2]);
    uint8_t hh = semi_decimal(src[3]);
    uint8_t mi = semi_decimal(src[4]);
    uint8_t ss = semi_decimal(src[5]);
    (void)src[6];
    if (cap > 0u) {
        dst[0] = '\0';
    }
    if (cap >= 18u) {
        dst[0] = (char)('0' + (yy / 10u));
        dst[1] = (char)('0' + (yy % 10u));
        dst[2] = '/';
        dst[3] = (char)('0' + (mm / 10u));
        dst[4] = (char)('0' + (mm % 10u));
        dst[5] = '/';
        dst[6] = (char)('0' + (dd / 10u));
        dst[7] = (char)('0' + (dd % 10u));
        dst[8] = ',';
        dst[9] = (char)('0' + (hh / 10u));
        dst[10] = (char)('0' + (hh % 10u));
        dst[11] = ':';
        dst[12] = (char)('0' + (mi / 10u));
        dst[13] = (char)('0' + (mi % 10u));
        dst[14] = ':';
        dst[15] = (char)('0' + (ss / 10u));
        dst[16] = (char)('0' + (ss % 10u));
        dst[17] = '\0';
    }
}

static void parse_udh(const uint8_t *data, uint8_t data_len, sms_codec_message_t *out, uint8_t *payload_offset) {
    *payload_offset = 0u;
    if (out != NULL) {
        out->udh_unhandled = true;
    }
    if (data == 0 || data_len == 0u || out == 0) {
        return;
    }
    uint8_t udhl = data[0];
    if ((uint16_t)udhl + 1u > data_len) {
        return;
    }
    out->udh_unhandled = false;
    size_t pos = 1u;
    while (pos + 2u <= (size_t)udhl + 1u) {
        uint8_t iei = data[pos++];
        uint8_t len = data[pos++];
        if (pos + len > (size_t)udhl + 1u) {
            out->udh_unhandled = true;
            break;
        }
        if (iei == 0x05u && len == 4u) {
            out->udh_unhandled |= out->has_ports;
            out->has_ports = true;
            out->dest_port = (uint16_t)(((uint16_t)data[pos] << 8) | data[pos + 1u]);
            out->source_port = (uint16_t)(((uint16_t)data[pos + 2u] << 8) | data[pos + 3u]);
        } else if (iei == 0x00u && len == 3u) {
            out->udh_unhandled |= out->has_concat;
            out->has_concat = true;
            out->concat_ref_16bit = false;
            out->concat_ref = data[pos];
            out->concat_total = data[pos + 1u];
            out->concat_seq = data[pos + 2u];
        } else if (iei == 0x08u && len == 4u) {
            out->udh_unhandled |= out->has_concat;
            out->has_concat = true;
            out->concat_ref_16bit = true;
            out->concat_ref = (uint16_t)(((uint16_t)data[pos] << 8u) |
                                         data[pos + 1u]);
            out->concat_total = data[pos + 2u];
            out->concat_seq = data[pos + 3u];
        } else {
            out->udh_unhandled = true;
        }
        pos += len;
    }
    out->udh_unhandled |= pos != (size_t)udhl + 1u;
    *payload_offset = (uint8_t)(udhl + 1u);
}

static sms_dcs_kind_t dcs_kind(uint8_t dcs) {
    uint8_t group = (uint8_t)(dcs >> 4u);
    if (group <= 7u) {
        /* General and automatic-deletion groups share bits 5..0. Compressed
         * text is unsupported and must remain opaque application data. */
        if ((dcs & 0x20u) != 0u) {
            return SMS_DCS_BINARY;
        }
        switch (dcs & 0x0cu) {
        case 0x00u: return SMS_DCS_GSM7;
        case 0x08u: return SMS_DCS_UCS2;
        default:    return SMS_DCS_BINARY; /* 8-bit or reserved alphabet */
        }
    }
    if (group == 0x0cu || group == 0x0du) {
        return SMS_DCS_GSM7; /* discard/store MWI */
    }
    if (group == 0x0eu) {
        return SMS_DCS_UCS2; /* store MWI, UCS2 */
    }
    if (group == 0x0fu) {
        /* Bit 3 is reserved and must be zero. Preserve reserved F8..FF as
         * opaque data rather than interpreting carrier bytes as user text. */
        if ((dcs & 0x08u) != 0u) {
            return SMS_DCS_BINARY;
        }
        return (dcs & 0x04u) != 0u ? SMS_DCS_BINARY : SMS_DCS_GSM7;
    }
    /* 1000..1011 are reserved coding groups. */
    return SMS_DCS_BINARY;
}

static bool decode_user_data(const uint8_t *data, size_t available,
                             uint8_t udl, bool udhi, uint8_t dcs,
                             sms_codec_message_t *out) {
    if (data == 0 || out == 0) {
        return false;
    }
    sms_dcs_kind_t kind = dcs_kind(dcs);
    if (kind == SMS_DCS_GSM7) {
        if (udl > MODEM_SMS_TEXT_MAX) {
            return false;
        }
        uint8_t bytes = (uint8_t)(((uint16_t)udl * 7u + 7u) / 8u);
        out->trailing_data = available > bytes;
        return available >= bytes &&
               decode_gsm7_user_data(data, bytes, udl, udhi, out);
    }

    if (udl > SMS_TP_UD_MAX_OCTETS || available < udl) {
        return false;
    }
    out->trailing_data = available > udl;
    uint8_t payload_offset = 0u;
    if (udhi) {
        parse_udh(data, udl, out, &payload_offset);
        /* UDHI set but no complete header was consumed: never leak header
         * bytes into either an opaque binary body or UCS2 text. */
        if (payload_offset == 0u) {
            return false;
        }
    }
    size_t payload_len = (size_t)udl - payload_offset;
    if (kind == SMS_DCS_UCS2) {
        return decode_ucs2(&data[payload_offset], (uint8_t)payload_len,
                           out->text, sizeof(out->text));
    }
    if (payload_len > SMS_CODEC_BINARY_MAX) {
        return false;
    }
    out->binary = true;
    out->binary_len = (uint16_t)payload_len;
    memcpy(out->binary_data, &data[payload_offset], payload_len);
    return true;
}

static bool decode_gsm7_user_data(const uint8_t *data, uint8_t data_len,
                                  uint8_t udl, bool udhi,
                                  sms_codec_message_t *out) {
    uint8_t text_septets = udl;
    uint16_t bit_offset = 0u;
    if (udhi) {
        uint8_t payload_offset = 0u;
        parse_udh(data, data_len, out, &payload_offset);
        if (payload_offset == 0u) {
            return false;
        }
        uint8_t header_septets = (uint8_t)(
            ((uint16_t)payload_offset * 8u + 6u) / 7u);
        if (header_septets > udl) {
            return false;
        }
        text_septets = (uint8_t)(udl - header_septets);
        /* TP-UD starts the text after the UDH plus the fill bits needed to
         * align the first payload septet. */
        bit_offset = (uint16_t)header_septets * 7u;
    }
    return decode_gsm7_utf8(data, data_len, text_septets, bit_offset,
                            out->text, sizeof(out->text));
}

static bool decode_gsm7_utf8(const uint8_t *data, uint8_t data_len,
                             uint8_t septets, uint16_t bit_offset,
                             char *dst, size_t cap) {
    if (data == 0 || dst == 0 || cap == 0u) {
        return false;
    }
    size_t out = 0u;
    bool escaped = false;
    for (uint8_t i = 0u; i < septets; i++) {
        uint8_t value = 0u;
        if (!gsm7_septet_at(data, data_len, bit_offset, i, &value)) {
            dst[out] = '\0';
            return false;
        }
        if (!escaped && value == 0x1bu) {
            escaped = true;
            continue;
        }
        uint32_t codepoint = escaped
            ? gsm7_extension_codepoint(value)
            : gsm7_default_codepoint(value);
        escaped = false;
        if (!append_utf8(dst, cap, &out, codepoint)) {
            dst[out] = '\0';
            return false;
        }
    }
    if (escaped && !append_utf8(dst, cap, &out, '?')) {
        dst[out] = '\0';
        return false;
    }
    dst[out] = '\0';
    return true;
}

static bool decode_ucs2(const uint8_t *data, uint8_t data_len,
                        char *dst, size_t cap) {
    if (data == 0 || dst == 0 || cap == 0u || (data_len & 1u) != 0u) {
        return false;
    }
    size_t out = 0u;
    for (uint8_t i = 0u; i < data_len; i = (uint8_t)(i + 2u)) {
        uint16_t codepoint = (uint16_t)(
            ((uint16_t)data[i] << 8u) | data[i + 1u]);
        if (i == 0u && codepoint == 0xfeffu) {
            continue; /* tolerate a leading big-endian BOM */
        }
        /* TP-DCS specifies UCS2, not UTF-16. Surrogates, embedded NUL, and
         * noncharacters cannot form one renderable glyph; preserve position
         * with the same '?' fallback the font layer uses for unknown glyphs. */
        if (codepoint == 0u ||
            (codepoint >= 0xd800u && codepoint <= 0xdfffu) ||
            codepoint == 0xfffeu || codepoint == 0xffffu) {
            codepoint = '?';
        }
        if (!append_utf8(dst, cap, &out, codepoint)) {
            dst[out] = '\0';
            return false;
        }
    }
    dst[out] = '\0';
    return true;
}

static bool gsm7_septet_at(const uint8_t *data, uint8_t data_len,
                           uint16_t bit_offset, uint8_t index,
                           uint8_t *out) {
    if (data == 0 || out == 0) {
        return false;
    }
    uint16_t bit = (uint16_t)(bit_offset + (uint16_t)index * 7u);
    uint16_t byte = (uint16_t)(bit / 8u);
    if (byte >= data_len) {
        return false;
    }
    uint8_t shift = (uint8_t)(bit & 7u);
    uint16_t value = (uint16_t)(data[byte] >> shift);
    if (shift != 0u && byte + 1u < data_len) {
        value |= (uint16_t)data[byte + 1u] << (8u - shift);
    }
    *out = (uint8_t)(value & 0x7fu);
    return true;
}

static uint32_t gsm7_default_codepoint(uint8_t value) {
    static const uint16_t table[128] = {
        '@', 0x00a3u, '$', 0x00a5u, 0x00e8u, 0x00e9u, 0x00f9u, 0x00ecu,
        0x00f2u, 0x00c7u, '\n', 0x00d8u, 0x00f8u, '\r', 0x00c5u, 0x00e5u,
        0x0394u, '_', 0x03a6u, 0x0393u, 0x039bu, 0x03a9u, 0x03a0u, 0x03a8u,
        0x03a3u, 0x0398u, 0x039eu, 0u, 0x00c6u, 0x00e6u, 0x00dfu, 0x00c9u,
        ' ', '!', '"', '#', 0x00a4u, '%', '&', '\'', '(', ')', '*', '+', ',', '-', '.', '/',
        '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', ':', ';', '<', '=', '>', '?',
        0x00a1u, 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O',
        'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z', 0x00c4u, 0x00d6u, 0x00d1u,
        0x00dcu, 0x00a7u, 0x00bfu, 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k',
        'l', 'm', 'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z', 0x00e4u,
        0x00f6u, 0x00f1u, 0x00fcu, 0x00e0u,
    };
    return table[value & 0x7fu];
}

static uint32_t gsm7_extension_codepoint(uint8_t value) {
    switch (value) {
    case 0x0au: return '\f';
    case 0x14u: return '^';
    case 0x28u: return '{';
    case 0x29u: return '}';
    case 0x2fu: return '\\';
    case 0x3cu: return '[';
    case 0x3du: return '~';
    case 0x3eu: return ']';
    case 0x40u: return '|';
    case 0x65u: return 0x20acu;
    default:    return '?';
    }
}

static bool append_utf8(char *dst, size_t cap, size_t *pos,
                        uint32_t codepoint) {
    if (dst == 0 || pos == 0 || *pos >= cap) {
        return false;
    }
    size_t bytes = codepoint <= 0x7fu ? 1u
                 : codepoint <= 0x7ffu ? 2u : 3u;
    if (*pos + bytes >= cap) {
        return false;
    }
    if (bytes == 1u) {
        dst[(*pos)++] = (char)codepoint;
    } else if (bytes == 2u) {
        dst[(*pos)++] = (char)(0xc0u | (codepoint >> 6u));
        dst[(*pos)++] = (char)(0x80u | (codepoint & 0x3fu));
    } else {
        dst[(*pos)++] = (char)(0xe0u | (codepoint >> 12u));
        dst[(*pos)++] = (char)(0x80u | ((codepoint >> 6u) & 0x3fu));
        dst[(*pos)++] = (char)(0x80u | (codepoint & 0x3fu));
    }
    return true;
}

static size_t bounded_strlen(const char *text, size_t cap) {
    size_t len = 0u;
    if (text == 0) {
        return 0u;
    }
    while (len < cap && text[len] != '\0') {
        len++;
    }
    return len;
}

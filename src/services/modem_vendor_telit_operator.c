#include "modem_vendor_telit_internal.h"

#include <string.h>

#define TELIT_SIM_PROVIDER_NAME_LINE_MAX 96u
#define TELIT_SIM_PROVIDER_NAME_FILE_BYTES (1u + TELIT_SIM_PROVIDER_NAME_ALPHA_BYTES)

/* Telit 80586ST11193A Rev.4 pp780-784: CRSM transports SIM bytes as hex.
 * TS 31.102 4.2.12 / TS 51.011 10.3.11: EFSPN is policy + 16 alpha bytes.
 * TS 23.038 6.2.1/6.2.1.1 and TS 51.011 Annex B define the alphabets.
 * Keep this strict decoder separate from SMS's replacement-glyph policy. */
static const uint16_t GSM_DEFAULT[128] = {
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

static uint16_t gsm_extension(uint8_t value) {
    switch (value) {
    case 0x14u: return '^';
    case 0x28u: return '{';
    case 0x29u: return '}';
    case 0x2fu: return '\\';
    case 0x3cu: return '[';
    case 0x3du: return '~';
    case 0x3eu: return ']';
    case 0x40u: return '|';
    case 0x65u: return 0x20acu;
    default: return 0u; /* Includes the form-feed extension. */
    }
}

static bool append_utf8(char *out, size_t cap, size_t *used,
                        uint32_t codepoint) {
    if (codepoint < 0x20u || (codepoint >= 0x7fu && codepoint <= 0x9fu) ||
        (codepoint >= 0xd800u && codepoint <= 0xdfffu) ||
        (codepoint >= 0xfdd0u && codepoint <= 0xfdefu) || codepoint >= 0xfffdu ||
        codepoint == 0xfeffu || codepoint == 0x200eu || codepoint == 0x200fu ||
        (codepoint >= 0x2028u && codepoint <= 0x202eu) ||
        (codepoint >= 0x2066u && codepoint <= 0x2069u)) {
        return false;
    }
    size_t bytes = codepoint < 0x80u ? 1u
                 : codepoint < 0x800u ? 2u : 3u;
    if (*used + bytes >= cap) {
        return false;
    }
    if (bytes == 1u) {
        out[(*used)++] = (char)codepoint;
    } else if (bytes == 2u) {
        out[(*used)++] = (char)(0xc0u | (codepoint >> 6u));
        out[(*used)++] = (char)(0x80u | (codepoint & 0x3fu));
    } else {
        out[(*used)++] = (char)(0xe0u | (codepoint >> 12u));
        out[(*used)++] = (char)(0x80u | ((codepoint >> 6u) & 0x3fu));
        out[(*used)++] = (char)(0x80u | (codepoint & 0x3fu));
    }
    return true;
}

static bool is_padding(const uint8_t *data, size_t length) {
    for (size_t i = 0u; i < length; i++) {
        if (data[i] != 0xffu) {
            return false;
        }
    }
    return true;
}

static bool decode_gsm(const uint8_t *data, size_t length, bool compressed,
                        uint16_t base, char *out, size_t cap, size_t *used) {
    for (size_t i = 0u; i < length; i++) {
        uint8_t value = data[i];
        uint32_t codepoint;
        if (value >= 0x80u) {
            if (!compressed) {
                return value == 0xffu && is_padding(data + i, length - i);
            }
            /* FF inside the declared compressed length is a real offset. */
            codepoint = (uint32_t)base + (value & 0x7fu);
        } else if (value == 0x1bu) {
            if (++i == length) {
                return false;
            }
            codepoint = gsm_extension(data[i]);
        } else {
            codepoint = GSM_DEFAULT[value];
        }
        if (!append_utf8(out, cap, used, codepoint)) {
            return false;
        }
    }
    return true;
}

static bool decode_alpha(const uint8_t *alpha, char *out, size_t cap,
                          size_t *used) {
    const size_t length = TELIT_SIM_PROVIDER_NAME_ALPHA_BYTES;
    if (alpha[0] == 0x80u) {
        size_t i = 1u;
        for (; i + 1u < length; i += 2u) {
            uint16_t codepoint = (uint16_t)(((uint16_t)alpha[i] << 8u) | alpha[i + 1u]);
            if (codepoint == 0xffffu) {
                break;
            }
            if (!append_utf8(out, cap, used, codepoint)) {
                return false;
            }
        }
        return is_padding(alpha + i, length - i);
    }
    if (alpha[0] == 0x81u || alpha[0] == 0x82u) {
        size_t header = alpha[0] == 0x81u ? 3u : 4u;
        size_t count = alpha[1];
        uint16_t base = header == 3u ? (uint16_t)((uint16_t)alpha[2] << 7u)
                                    : (uint16_t)(((uint16_t)alpha[2] << 8u) | alpha[3]);
        if (count > length - header ||
            !is_padding(alpha + header + count, length - header - count)) {
            return false;
        }
        return decode_gsm(alpha + header, count, true, base, out, cap, used);
    }
    return decode_gsm(alpha, length, false, 0u, out, cap, used);
}

static void skip_spaces(const char **cursor, const char *end) {
    while (*cursor < end && (**cursor == ' ' || **cursor == '\t')) {
        (*cursor)++;
    }
}

static bool parse_status(const char **cursor, const char *end, uint8_t *out) {
    skip_spaces(cursor, end);
    unsigned value = 0u;
    size_t digits = 0u;
    while (*cursor < end && **cursor >= '0' && **cursor <= '9') {
        if (++digits > 3u) {
            return false;
        }
        value = value * 10u + (unsigned)(**cursor - '0');
        (*cursor)++;
    }
    skip_spaces(cursor, end);
    if (digits == 0u || value > 255u || *cursor == end || **cursor != ',') {
        return false;
    }
    (*cursor)++;
    *out = (uint8_t)value;
    return true;
}

static int hex_digit(char value) {
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}

bool telit_parse_sim_provider_name(const char *line, char *out, size_t cap) {
    if (out == NULL || cap == 0u) {
        return false;
    }
    if (line == NULL || strncmp(line, "+CRSM:", 6u) != 0) {
        goto invalid;
    }

    size_t length = 6u;
    while (length <= TELIT_SIM_PROVIDER_NAME_LINE_MAX && line[length] != '\0') {
        length++;
    }
    if (length > TELIT_SIM_PROVIDER_NAME_LINE_MAX) {
        goto invalid;
    }
    const char *begin = line + 6u;
    const char *end = line + length;
    uint8_t sw1, sw2;
    if (!parse_status(&begin, end, &sw1) || !parse_status(&begin, end, &sw2) ||
        !((sw1 == 0x90u && sw2 == 0u) || sw1 == 0x9fu)) {
        goto invalid;
    }
    skip_spaces(&begin, end);
    while (end > begin && (end[-1] == ' ' || end[-1] == '\t')) {
        end--;
    }
    if (begin < end && *begin == '"') {
        begin++;
        if (end <= begin || end[-1] != '"') {
            goto invalid;
        }
        end--;
    }
    if ((size_t)(end - begin) != 2u * TELIT_SIM_PROVIDER_NAME_FILE_BYTES) {
        goto invalid;
    }
    uint8_t data[TELIT_SIM_PROVIDER_NAME_FILE_BYTES];
    for (size_t i = 0u; i < sizeof(data); i++) {
        int high = hex_digit(begin[2u * i]);
        int low = hex_digit(begin[2u * i + 1u]);
        if (high < 0 || low < 0) {
            goto invalid;
        }
        data[i] = (uint8_t)((unsigned)high * 16u + (unsigned)low);
    }

    char decoded[TELIT_SIM_PROVIDER_NAME_UTF8_CAP];
    size_t used = 0u;
    if (!decode_alpha(data + 1u, decoded, sizeof(decoded), &used)) {
        goto invalid;
    }
    size_t first = 0u;
    while (first < used && decoded[first] == ' ') {
        first++;
    }
    while (used > first && decoded[used - 1u] == ' ') {
        used--;
    }
    size_t name_length = used - first;
    if (name_length >= cap) {
        goto invalid;
    }
    decoded[used] = '\0';
    memcpy(out, decoded + first, name_length + 1u);
    return true;

invalid:
    out[0] = '\0';
    return false;
}

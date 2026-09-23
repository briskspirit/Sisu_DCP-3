#include "modem_vendor_telit_internal.h"
#include "services/sim_numbers.h"

#include <stdio.h>
#include <string.h>

static bool build(const sim_number_query_t *q, char *out, size_t cap) {
    if (q == NULL || out == NULL || cap == 0u) return false;
    /* Deliberate read-only allowlist; the modem selects the SIM application. */
    switch (q->file) {
    case 0x6f49: case 0x6f16: case 0x6f15: case 0x6f19: case 0xea01:
    case 0x6f4a: case 0x6f4c: break;
    default: return false;
    }
    int n;
    if (q->kind == SIM_NUMBER_FILE_INFO) n = snprintf(out, cap, "AT+CRSM=192,%u", q->file);
    else if (q->length == 0u || q->length > SIM_NUMBER_RECORD_MAX) return false;
    else if (q->kind == SIM_NUMBER_BINARY)
        n = snprintf(out, cap, "AT+CRSM=176,%u,0,0,%u", q->file, q->length);
    else if (q->kind == SIM_NUMBER_RECORD && q->record != 0u && q->record != 255u)
        n = snprintf(out, cap, "AT+CRSM=178,%u,%u,4,%u", q->file, q->record, q->length);
    else return false;
    return n >= 0 && (size_t)n < cap;
}

static uint16_t be16(const uint8_t *d) { return (uint16_t)((uint16_t)d[0] << 8u | d[1]); }

static bool metadata(uint16_t file, sim_number_reply_t *out) {
    const uint8_t *d = out->data;
    size_t length = out->length;
    bool records = false;
    uint16_t record_length = 0u;
    if (length == 0u) return false;
    if (d[0] != 0x62u) {
        /* GSM 11.11: EF, matching identifier, not invalidated, linear/transparent. */
        if (length < 15u || d[6] != 4u || be16(d + 4u) != file ||
            (d[11] & 1u) == 0u || (d[13] != 0u && d[13] != 1u)) return false;
        out->size = be16(d + 2u);
        records = d[13] == 1u;
        record_length = records ? d[14] : 0u;
    } else {
        /* ETSI TS 102 221 FCP: ignore unknown TLVs but never search their payload. */
        size_t pos = 2u, body = length > 1u ? d[1] : 0u;
        if (body == 0x81u && length > 2u) { body = d[2]; pos++; }
        if (length < pos || body != length - pos) return false;
        bool descriptor = false, identity = false, size = false, lifecycle = false;
        uint8_t declared_records = 0u;
        while (pos < length) {
            if (length - pos < 2u) return false;
            uint8_t tag = d[pos++]; size_t n = d[pos++];
            if (n == 0x81u && pos < length) n = d[pos++];
            else if (n >= 0x80u) return false;
            if (n > length - pos) return false;
            if (tag == 0x82u) {
                if (descriptor || (n != 2u && n != 5u)) return false;
                uint8_t type = d[pos] & 0xbfu;
                if ((type != 1u || n != 2u) && (type != 2u || n != 5u)) return false;
                records = type == 2u;
                if (records) { record_length = be16(d + pos + 2u); declared_records = d[pos + 4u]; }
                descriptor = true;
            } else if (tag == 0x83u) {
                if (identity || n != 2u || be16(d + pos) != file) return false;
                identity = true;
            } else if (tag == 0x80u) {
                if (size || (n != 1u && n != 2u)) return false;
                out->size = n == 1u ? d[pos] : be16(d + pos);
                size = true;
            } else if (tag == 0x8au) {
                if (lifecycle || n != 1u || (d[pos] & 0xfdu) != 5u) return false;
                lifecycle = true;
            }
            pos += n;
        }
        if (!descriptor || !identity || !size || !lifecycle ||
            (records && (uint32_t)record_length * declared_records != out->size)) return false;
    }
    if (records) {
        if (record_length == 0u || record_length > SIM_NUMBER_RECORD_MAX ||
            out->size % record_length != 0u || out->size / record_length > 254u) return false;
        out->record_length = (uint8_t)record_length;
        out->records = (uint8_t)(out->size / record_length);
    }
    return true;
}

static sim_number_line_t read_reply(const sim_number_query_t *q, const char *line,
                                     sim_number_reply_t *out) {
    if (!telit_starts_with(line, "+CRSM:")) return SIM_NUMBER_IGNORE;
    memset(out, 0, sizeof(*out));
    telit_csv_view_t fields[3]; size_t count; uint32_t sw1, sw2;
    if (!telit_csv_view_split_limit(line + 6u, fields, 3u, &count, 2u * SIM_NUMBER_RECORD_MAX) ||
        count < 2u || !telit_view_parse_u32(fields[0], 255u, &sw1) ||
        !telit_view_parse_u32(fields[1], 255u, &sw2)) return SIM_NUMBER_INVALID;
    if ((sw1 == 106u && sw2 == 130u) || (sw1 == 148u && sw2 == 4u)) {
        out->missing = true;
        return count == 2u ? SIM_NUMBER_VALID : SIM_NUMBER_INVALID;
    }
    if (!((sw1 == 144u && sw2 == 0u) || sw1 == 145u || sw1 == 159u) || count != 3u ||
        fields[2].length == 0u || fields[2].length % 2u != 0u ||
        fields[2].length / 2u > sizeof(out->data)) return SIM_NUMBER_INVALID;
    out->length = (uint8_t)(fields[2].length / 2u);
    for (uint8_t i = 0u; i < out->length; i++) {
        uint64_t value;
        telit_csv_view_t byte = {.text = fields[2].text + 2u * i, .length = 2u};
        if (!telit_view_parse_hex_u64(byte, 255u, &value)) return SIM_NUMBER_INVALID;
        out->data[i] = (uint8_t)value;
    }
    return (q->kind == SIM_NUMBER_FILE_INFO ? metadata(q->file, out) : out->length == q->length)
        ? SIM_NUMBER_VALID : SIM_NUMBER_INVALID;
}

const sim_numbers_backend_t TELIT_SIM_NUMBERS_BACKEND = {
    .build = build, .read = read_reply, .alpha = telit_sim_alpha,
};

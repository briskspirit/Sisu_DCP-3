#include "services/sms_submit_codec.h"

#include <stdio.h>
#include <string.h>

#include "services/call_types.h"

static bool mode_port_first(modem_binary_sms_mode_t mode) {
    return mode == MODEM_BINARY_SMS_MODE_DCS04_PORT_FIRST ||
           mode == MODEM_BINARY_SMS_MODE_F5_PORT_FIRST;
}

static bool mode_uses_f5(modem_binary_sms_mode_t mode) {
    return mode == MODEM_BINARY_SMS_MODE_F5_PORT_FIRST ||
           mode == MODEM_BINARY_SMS_MODE_F5_CONCAT_FIRST_VP;
}

static bool mode_uses_vp(modem_binary_sms_mode_t mode) {
    return mode == MODEM_BINARY_SMS_MODE_F5_CONCAT_FIRST_VP;
}

static bool mode_gsm7_text(modem_binary_sms_mode_t mode) {
    return mode == MODEM_BINARY_SMS_MODE_GSM7_TEXT;
}

bool sms_submit_pdu_build(sms_submit_pdu_t *submit,
                          char *hex,
                          size_t hex_cap,
                          uint8_t *out_tpdu_len) {
    if (submit == NULL || hex == NULL || hex_cap == 0u ||
        out_tpdu_len == NULL || submit->number == NULL ||
        submit->payload == NULL || submit->payload_len == 0u ||
        submit->position >= submit->payload_len) {
        return false;
    }

    uint16_t remain = (uint16_t)(submit->payload_len - submit->position);
    uint8_t chunk_len = remain > MODEM_SMS_BINARY_CHUNK_MAX
        ? MODEM_SMS_BINARY_CHUNK_MAX
        : (uint8_t)remain;
    bool gsm7_text = mode_gsm7_text(submit->mode);

    uint8_t pdu[180];
    size_t pos = 0u;
    pdu[pos++] = 0x00u; /* SMSC: use modem/SIM default. */
    bool use_port = !gsm7_text &&
        (submit->dest_port != 0u || submit->source_port != 0u);
    bool multipart = !gsm7_text && submit->segment_total > 1u;
    bool use_udh = use_port || multipart;
    uint8_t udh_len = 0u;
    if (multipart && use_port) {
        udh_len = 0x0bu;
    } else if (multipart) {
        udh_len = 0x05u;
    } else if (use_port) {
        udh_len = 0x06u;
    }
    uint8_t udh_total = use_udh ? (uint8_t)(udh_len + 1u) : 0u;

    bool use_vp = mode_uses_vp(submit->mode);
    pdu[pos++] = use_udh
        ? (use_vp ? 0x51u : 0x41u)
        : (use_vp ? 0x11u : 0x01u); /* SMS-SUBMIT, optional TP-UDHI. */
    pdu[pos++] = 0x00u; /* TP-MR: let modem allocate. */
    if (!sms_submit_append_address(pdu, sizeof(pdu), &pos,
                                   submit->number)) {
        return false;
    }

    uint8_t user_data[MODEM_SMS_BINARY_CHUNK_MAX];
    uint8_t user_data_len = chunk_len;
    uint8_t udl = (uint8_t)(udh_total + chunk_len);
    if (gsm7_text) {
        if (!sms_submit_pack_gsm7(&submit->payload[submit->position],
                                  chunk_len, user_data, &user_data_len)) {
            return false;
        }
        udl = chunk_len;
    }
    if (pos + 3u + (use_vp ? 1u : 0u) + udh_total + user_data_len >
        sizeof(pdu)) {
        return false;
    }
    pdu[pos++] = 0x00u; /* PID: normal SMS. */
    pdu[pos++] = gsm7_text
        ? 0x00u  /* GSM 7-bit default alphabet. */
        : (mode_uses_f5(submit->mode)
               ? 0xf5u  /* 8-bit data, class 1: Nokia smart message. */
               : 0x04u); /* 8-bit data; class bits clear for strict carriers. */
    if (use_vp) {
        pdu[pos++] = 0xa7u; /* Relative VP matching text-mode setup. */
    }
    pdu[pos++] = udl;

    if (pos + udh_total + user_data_len > sizeof(pdu)) {
        return false;
    }
    if (use_udh) {
        pdu[pos++] = udh_len;
    }
    if (use_port && mode_port_first(submit->mode)) {
        pdu[pos++] = 0x05u;
        pdu[pos++] = 0x04u;
        pdu[pos++] = (uint8_t)(submit->dest_port >> 8);
        pdu[pos++] = (uint8_t)(submit->dest_port & 0xffu);
        pdu[pos++] = (uint8_t)(submit->source_port >> 8);
        pdu[pos++] = (uint8_t)(submit->source_port & 0xffu);
    }
    if (multipart) {
        pdu[pos++] = 0x00u;
        pdu[pos++] = 0x03u;
        pdu[pos++] = submit->reference;
        pdu[pos++] = submit->segment_total;
        pdu[pos++] = submit->segment;
    }
    if (use_port && !mode_port_first(submit->mode)) {
        pdu[pos++] = 0x05u;
        pdu[pos++] = 0x04u;
        pdu[pos++] = (uint8_t)(submit->dest_port >> 8);
        pdu[pos++] = (uint8_t)(submit->dest_port & 0xffu);
        pdu[pos++] = (uint8_t)(submit->source_port >> 8);
        pdu[pos++] = (uint8_t)(submit->source_port & 0xffu);
    }
    if (gsm7_text) {
        memcpy(&pdu[pos], user_data, user_data_len);
        pos += user_data_len;
    } else {
        memcpy(&pdu[pos], &submit->payload[submit->position], chunk_len);
        pos += chunk_len;
    }

    size_t hex_pos = 0u;
    for (size_t i = 0u; i < pos; i++) {
        if (!sms_submit_append_hex_byte(hex, hex_cap, &hex_pos, pdu[i])) {
            return false;
        }
    }
    hex[hex_pos] = '\0';
    *out_tpdu_len = (uint8_t)(pos - 1u);
    submit->position = (uint16_t)(submit->position + chunk_len);
    submit->segment++;
    return true;
}

bool sms_submit_pack_gsm7(const uint8_t *src,
                          uint8_t septets,
                          uint8_t *dst,
                          uint8_t *out_len) {
    if (src == NULL || dst == NULL || out_len == NULL || septets == 0u ||
        septets > MODEM_SMS_TEXT_MAX) {
        return false;
    }
    uint8_t bytes = (uint8_t)(((uint16_t)septets * 7u + 7u) / 8u);
    memset(dst, 0, bytes);
    for (uint8_t i = 0u; i < septets; i++) {
        uint8_t value = sms_submit_gsm7_code(src[i]);
        uint16_t bit = (uint16_t)i * 7u;
        uint8_t byte = (uint8_t)(bit / 8u);
        uint8_t shift = (uint8_t)(bit & 7u);
        dst[byte] |= (uint8_t)(value << shift);
        if (shift > 1u && byte + 1u < bytes) {
            dst[byte + 1u] |= (uint8_t)(value >> (8u - shift));
        }
    }
    *out_len = bytes;
    return true;
}

uint8_t sms_submit_gsm7_code(uint8_t ch) {
    if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
        (ch >= '0' && ch <= '9')) {
        return ch;
    }
    if (ch == ' ') {
        return 0x20u;
    }
    if (ch == '+') {
        return 0x2bu;
    }
    if (ch == '-') {
        return 0x2du;
    }
    if (ch == '.') {
        return 0x2eu;
    }
    return 0x20u;
}

bool sms_submit_append_hex_byte(char *dst,
                                size_t cap,
                                size_t *pos,
                                uint8_t value) {
    static const char HEX[] = "0123456789ABCDEF";
    if (dst == NULL || pos == NULL || *pos + 2u >= cap) {
        return false;
    }
    dst[(*pos)++] = HEX[(value >> 4) & 0x0fu];
    dst[(*pos)++] = HEX[value & 0x0fu];
    return true;
}

bool sms_submit_append_address(uint8_t *dst,
                               size_t cap,
                               size_t *pos,
                               const char *number) {
    if (dst == NULL || pos == NULL || number == NULL || *pos + 2u > cap) {
        return false;
    }
    uint8_t digits[MODEM_PHONE_MAX];
    uint8_t count = 0u;
    bool international = false;
    for (size_t i = 0u; number[i] != '\0'; i++) {
        char ch = number[i];
        if (ch == '+' && count == 0u) {
            international = true;
            continue;
        }
        if (ch >= '0' && ch <= '9') {
            if (count >= MODEM_PHONE_MAX) {
                return false;
            }
            digits[count++] = (uint8_t)(ch - '0');
        }
    }
    if (count == 0u) {
        return false;
    }
    uint8_t packed = (uint8_t)((count + 1u) / 2u);
    if (*pos + 2u + packed > cap) {
        return false;
    }
    dst[(*pos)++] = count;
    dst[(*pos)++] = international ? 0x91u : 0x81u;
    for (uint8_t i = 0u; i < count; i = (uint8_t)(i + 2u)) {
        uint8_t low = digits[i];
        uint8_t high = i + 1u < count ? digits[i + 1u] : 0x0fu;
        dst[(*pos)++] = (uint8_t)(low | (high << 4));
    }
    return true;
}

void sms_submit_format_text_command(char *dst,
                                    size_t dst_len,
                                    const char *command,
                                    const char *address,
                                    bool sent_status) {
    if (dst == NULL || dst_len == 0u) {
        return;
    }
    if (address == NULL || address[0] == '\0') {
        snprintf(dst, dst_len, "%s", command);
        return;
    }
    int toa = address[0] == '+' ? 145 : 129;
    if (sent_status) {
        snprintf(dst, dst_len, "%s=\"%s\",%d,\"STO SENT\"",
                 command, address, toa);
    } else if (toa == 145) {
        snprintf(dst, dst_len, "%s=\"%s\",145", command, address);
    } else {
        snprintf(dst, dst_len, "%s=\"%s\"", command, address);
    }
}

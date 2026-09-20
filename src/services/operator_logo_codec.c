#include "services/operator_logo_codec.h"

#include <string.h>

static bool digits(const char *s, size_t min, size_t max) {
    if (s == NULL) return false;
    size_t n = 0u;
    while (n <= max && s[n] >= '0' && s[n] <= '9') n++;
    return n >= min && n <= max && s[n] == '\0';
}

bool operator_logo_network_encode(const char *mcc, const char *mnc, uint8_t out[3]) {
    if (out == NULL || !digits(mcc, 3u, 3u) || !digits(mnc, 2u, 3u)) return false;
    /* TS 24.008 PLMN: the third MNC digit occupies the two-digit F filler.
     * Keep that nibble: the v6.00 logo matcher compares it, not just the MCC. */
    out[0] = (uint8_t)(((mcc[1] - '0') << 4) | (mcc[0] - '0'));
    out[1] = (uint8_t)(((mnc[2] ? mnc[2] - '0' : 15) << 4) | (mcc[2] - '0'));
    out[2] = (uint8_t)(((mnc[1] - '0') << 4) | (mnc[0] - '0'));
    return true;
}

bool operator_logo_network_valid(const uint8_t network[3]) {
    if (network == NULL) return false;
    uint8_t third = network[1] >> 4;
    return (network[0] & 15u) <= 9u && (network[0] >> 4) <= 9u &&
        (network[1] & 15u) <= 9u && (third <= 9u || third == 15u) &&
        (network[2] & 15u) <= 9u &&
        ((network[2] >> 4) <= 9u || (third == 15u && (network[2] >> 4) == 15u));
}

static uint8_t normalized_mnc(uint8_t value) {
    /* Original 0x28970c also accepts F5 in place of 50 (MNC 05). */
    return (value & 0xf0u) == 0xf0u ? (uint8_t)(value << 4) : value;
}

bool operator_logo_matches(const operator_logo_t *logo, const char *mcc, const char *mnc) {
    uint8_t network[3];
    return logo != NULL && operator_logo_network_valid(logo->network) &&
        operator_logo_network_encode(mcc, mnc, network) &&
        logo->network[0] == network[0] && logo->network[1] == network[1] &&
        normalized_mnc(logo->network[2]) == network[2];
}

bool operator_logo_decode(const uint8_t *payload, size_t len, operator_logo_t *out) {
    if (payload == NULL || out == NULL || len < 7u || len > OPERATOR_LOGO_PAYLOAD_MAX)
        return false;
    size_t network = payload[0] == 0x30u ? 1u : 0u;
    if (!operator_logo_network_valid(payload + network)) return false;
    size_t pos = network + 3u;
    if (network != 0u) {
        /* Version-zero header extensions are skipped through LF by the ROM. */
        while (pos < len && payload[pos] != 0x0au) pos++;
        if (pos == len) return false;
        pos++;
    }
    if (len - pos < 4u) return false;
    if (payload[pos] == 0x80u) {
        pos++; /* The ROM accepts one info-field extension octet. */
        if (len - pos < 4u || payload[pos] != 0u) return false;
    } else if (payload[pos] != 0u) return false;
    unsigned width = payload[pos + 1u], height = payload[pos + 2u];
    if (payload[pos + 3u] != 1u) return false;
    pos += 4u;
    if (len - pos != (width * height + 7u) / 8u) return false;

    operator_logo_t logo = {0};
    memcpy(logo.network, payload + network, sizeof(logo.network));
    unsigned dx = width < OPERATOR_LOGO_WIDTH ? (OPERATOR_LOGO_WIDTH - width) / 2u : 0u;
    unsigned dy = height < OPERATOR_LOGO_HEIGHT ? (OPERATOR_LOGO_HEIGHT - height) / 2u : 0u;
    for (unsigned y = 0u; y < height && y < OPERATOR_LOGO_HEIGHT; y++) {
        for (unsigned x = 0u; x < width && x < OPERATOR_LOGO_WIDTH; x++) {
            unsigned source = y * width + x;
            unsigned dest = (y + dy) * OPERATOR_LOGO_WIDTH + x + dx;
            if (payload[pos + source / 8u] & (0x80u >> (source % 8u)))
                logo.pixels[dest / 8u] |= (uint8_t)(0x80u >> (dest % 8u));
        }
    }
    *out = logo;
    return true;
}

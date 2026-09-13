#include "services/sms_identity.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static uint32_t sms_identity_crc32(uint32_t crc, const uint8_t *data,
                                   size_t len) {
    /* Keep the established polynomial so metadata/read identities remain
     * consistent across all modem-service paths. */
    for (size_t i = 0u; i < len; i++) {
        crc ^= data[i];
        for (uint8_t bit = 0u; bit < 8u; bit++) {
            uint32_t mask = (uint32_t)0 - (crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88020u & mask);
        }
    }
    return crc;
}

uint32_t sms_identity_hash(const char *sender, const char *timestamp,
                           const char *body) {
    uint32_t crc = 0xffffffffu;
    const uint8_t separator = 0u;
    if (sender != NULL) {
        crc = sms_identity_crc32(crc, (const uint8_t *)sender, strlen(sender));
    }
    crc = sms_identity_crc32(crc, &separator, 1u);
    if (timestamp != NULL) {
        crc = sms_identity_crc32(crc, (const uint8_t *)timestamp,
                                 strlen(timestamp));
    }
    crc = sms_identity_crc32(crc, &separator, 1u);
    if (body != NULL) {
        crc = sms_identity_crc32(crc, (const uint8_t *)body, strlen(body));
    }
    return crc ^ 0xffffffffu;
}

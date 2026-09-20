#ifndef OPERATOR_LOGO_CODEC_H
#define OPERATOR_LOGO_CODEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define OPERATOR_LOGO_PORT 0x1582u
#define OPERATOR_LOGO_WIDTH 72u
#define OPERATOR_LOGO_HEIGHT 14u
#define OPERATOR_LOGO_BYTES 126u
#define OPERATOR_LOGO_PAYLOAD_MAX 512u

typedef struct {
    uint8_t network[3];
    uint8_t pixels[OPERATOR_LOGO_BYTES]; /* Row-major, MSB first. */
} operator_logo_t;

bool operator_logo_network_encode(const char *mcc, const char *mnc, uint8_t out[3]);
bool operator_logo_network_valid(const uint8_t network[3]);
bool operator_logo_matches(const operator_logo_t *logo, const char *mcc, const char *mnc);
bool operator_logo_decode(const uint8_t *payload, size_t len, operator_logo_t *out);

#endif

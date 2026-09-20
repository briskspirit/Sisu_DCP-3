#include "services/operator_logo_codec.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    const struct { const char *mcc, *mnc; uint8_t wire[3]; } networks[] = {
        {"244", "05", {0x42,0xf4,0x50}},
        {"311", "480", {0x13,0x01,0x84}},
        {"310", "260", {0x13,0x00,0x62}},
        {"310", "410", {0x13,0x00,0x14}},
        {"001", "01", {0x00,0xf1,0x10}},
        {"001", "001", {0x00,0x11,0x00}},
    };
    for (unsigned i = 0u; i < sizeof(networks) / sizeof(networks[0]); i++) {
        uint8_t wire[3];
        assert(operator_logo_network_encode(networks[i].mcc, networks[i].mnc, wire));
        assert(memcmp(wire, networks[i].wire, 3u) == 0);
        operator_logo_t logo = {0};
        memcpy(logo.network, wire, 3u);
        assert(operator_logo_matches(&logo, networks[i].mcc, networks[i].mnc));
        for (unsigned j = 0u; j < sizeof(networks) / sizeof(networks[0]); j++)
            assert(operator_logo_matches(&logo, networks[j].mcc, networks[j].mnc) == (i == j));
    }
    operator_logo_t legacy = {.network={0x42,0xf4,0xf5}};
    assert(operator_logo_matches(&legacy, "244", "05"));
    assert(!operator_logo_matches(&legacy, "244", "050"));
    uint8_t wire[3];
    assert(!operator_logo_network_encode("31", "260", wire));
    assert(!operator_logo_network_encode("310", "2", wire));
    assert(!operator_logo_network_encode("310", "2600", wire));
    assert(!operator_logo_network_encode("31A", "260", wire));
    assert(!operator_logo_network_encode(NULL, "260", wire));
    for (unsigned mnc = 0u; mnc < 1000u; mnc++) {
        char digits[4];
        snprintf(digits, sizeof(digits), "%03u", mnc);
        assert(operator_logo_network_encode("310", digits, wire));
        assert(operator_logo_network_valid(wire));
    }

    uint8_t compact[133] = {0x13,0x01,0x84,0x00,72,14,1};
    for (unsigned i = 7u; i < sizeof(compact); i++) compact[i] = (uint8_t)i;
    operator_logo_t logo, versioned;
    assert(operator_logo_decode(compact, sizeof(compact), &logo));
    assert(memcmp(logo.pixels, compact + 7u, OPERATOR_LOGO_BYTES) == 0);
    uint8_t payload[OPERATOR_LOGO_PAYLOAD_MAX] = {0x30,0x13,0x01,0x84,0x0a};
    memcpy(payload + 5u, compact + 3u, sizeof(compact) - 3u);
    assert(operator_logo_decode(payload, 135u, &versioned));
    assert(memcmp(&logo, &versioned, sizeof(logo)) == 0);
    for (size_t n = 0u; n < 135u; n++) {
        operator_logo_t guard;
        memset(&guard, 0xa5, sizeof(guard));
        assert(!operator_logo_decode(payload, n, &guard));
        for (size_t j = 0u; j < sizeof(guard); j++) assert(((uint8_t *)&guard)[j] == 0xa5u);
    }
    assert(!operator_logo_decode(payload, 136u, &logo));
    payload[4] = 0u;
    memset(payload + 5u, 0xff, sizeof(payload) - 5u);
    assert(!operator_logo_decode(payload, sizeof(payload), &logo));
    uint8_t tiny[] = {0x42,0xf4,0x50,0,2,2,1,0x90};
    assert(operator_logo_decode(tiny, sizeof(tiny), &logo));
    unsigned set = 0u;
    for (unsigned bit = 0u; bit < 72u * 14u; bit++) {
        bool pixel = (logo.pixels[bit / 8u] & (0x80u >> (bit % 8u))) != 0u;
        bool expected = bit == 6u * 72u + 35u || bit == 7u * 72u + 36u;
        assert(pixel == expected);
        set += pixel;
    }
    assert(set == 2u);
    tiny[6] = 2u; assert(!operator_logo_decode(tiny, sizeof(tiny), &logo));
    tiny[6] = 1u; tiny[1] = 0xa4u; assert(!operator_logo_decode(tiny, sizeof(tiny), &logo));
    uint8_t empty[] = {0x30,0x00,0xf0,0x00,0x0a,0,0,0,1};
    assert(operator_logo_decode(empty, sizeof(empty), &logo));
    assert(!operator_logo_matches(&logo, "311", "480"));

    uint8_t extended[134] = {0x13,0x01,0x84,0x80,0,72,14,1};
    memcpy(extended + 8u, compact + 7u, 126u);
    assert(operator_logo_decode(extended, sizeof(extended), &logo));
    assert(memcmp(logo.pixels, compact + 7u, 126u) == 0);
    uint8_t large[151] = {0x13,0x01,0x84,0,80,14,1};
    memset(large + 7u, 0xff, 140u);
    assert(operator_logo_decode(large, 147u, &logo));
    for (unsigned i = 0u; i < OPERATOR_LOGO_BYTES; i++) assert(logo.pixels[i] == 0xffu);
    /* Deterministic malformed-input sweep runs under ASan/UBSan. */
    uint32_t random = 1u;
    for (unsigned n = 0u; n < 10000u; n++) {
        for (unsigned i = 0u; i < sizeof(payload); i++) {
            random = random * 1664525u + 1013904223u;
            payload[i] = (uint8_t)(random >> 24);
        }
        (void)operator_logo_decode(payload, n % (sizeof(payload) + 1u), &logo);
    }
    puts("operator logo codec passed");
    return 0;
}

#include "tuner_logic.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static const telit_tuner_branch_t BRANCHES[] = {
    /* Telit ant1=GPIO2=BGSA CTRL1; ant2=GPIO3=BGSA CTRL2. */
    {1u, 1u, false, false, "B2"},
    {2u, 2u, false, true,  "B12/B14"},
    {3u, 3u, true,  false, "B5"},
    {4u, 4u, true,  true,  "B4"},
};

static bool normalized_line(const uint8_t *line, size_t len,
                            char *out, size_t out_cap) {
    if (line == NULL || out == NULL || out_cap == 0u) {
        return false;
    }
    size_t begin = 0u;
    while (begin < len && (line[begin] == 0u || isspace(line[begin]))) {
        begin++;
    }
    while (len > begin && (line[len - 1u] == 0u || isspace(line[len - 1u]))) {
        len--;
    }
    size_t text_len = len - begin;
    if (text_len == 0u || text_len + 1u > out_cap) {
        return false;
    }
    for (size_t i = 0u; i < text_len; i++) {
        uint8_t ch = line[begin + i];
        if (ch < 0x20u || ch > 0x7eu) {
            return false;
        }
        out[i] = (char)ch;
    }
    out[text_len] = '\0';
    return true;
}

size_t telit_tuner_branch_count(void) {
    return sizeof BRANCHES / sizeof BRANCHES[0];
}

const telit_tuner_branch_t *telit_tuner_branch_for_digit(uint8_t digit) {
    for (size_t i = 0u; i < telit_tuner_branch_count(); i++) {
        if (BRANCHES[i].key_digit == digit) {
            return &BRANCHES[i];
        }
    }
    return NULL;
}

telit_tuner_final_t telit_tuner_parse_final(const uint8_t *line, size_t len) {
    char text[64];
    if (!normalized_line(line, len, text, sizeof text)) {
        return TELIT_TUNER_FINAL_NONE;
    }
    if (strcmp(text, "OK") == 0) {
        return TELIT_TUNER_FINAL_OK;
    }
    if (strcmp(text, "ERROR") == 0 ||
        strncmp(text, "+CME ERROR:", 11u) == 0 ||
        strncmp(text, "+CMS ERROR:", 11u) == 0) {
        return TELIT_TUNER_FINAL_ERROR;
    }
    return TELIT_TUNER_FINAL_NONE;
}

bool telit_tuner_parse_cfun(const uint8_t *line, size_t len,
                            uint8_t *value) {
    char text[64];
    unsigned parsed = 0u;
    int consumed = 0;
    if (value == NULL ||
        !normalized_line(line, len, text, sizeof text) ||
        sscanf(text, "+CFUN: %u %n", &parsed, &consumed) != 1 ||
        consumed <= 0 || text[consumed] != '\0' || parsed > UINT8_MAX) {
        return false;
    }
    *value = (uint8_t)parsed;
    return true;
}

bool telit_tuner_parse_stuneant(const uint8_t *line, size_t len,
                                bool *enabled) {
    char text[64];
    unsigned parsed = 0u;
    int consumed = 0;
    if (enabled == NULL ||
        !normalized_line(line, len, text, sizeof text) ||
        sscanf(text, "#STUNEANT: %u %n", &parsed, &consumed) != 1 ||
        consumed <= 0 || text[consumed] != '\0' || parsed > 1u) {
        return false;
    }
    *enabled = parsed != 0u;
    return true;
}

bool telit_tuner_parse_gpio(const uint8_t *line, size_t len,
                            uint8_t *direction, bool *level) {
    char text[64];
    unsigned parsed_direction = 0u;
    unsigned parsed_level = 0u;
    int consumed = 0;
    if (direction == NULL || level == NULL ||
        !normalized_line(line, len, text, sizeof text) ||
        sscanf(text, "#GPIO: %u , %u %n", &parsed_direction,
               &parsed_level, &consumed) != 2 ||
        consumed <= 0 || text[consumed] != '\0' ||
        parsed_direction > UINT8_MAX || parsed_level > 1u) {
        return false;
    }
    *direction = (uint8_t)parsed_direction;
    *level = parsed_level != 0u;
    return true;
}

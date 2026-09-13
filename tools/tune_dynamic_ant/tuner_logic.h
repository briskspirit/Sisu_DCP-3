#ifndef TELIT_TUNER_LOGIC_H
#define TELIT_TUNER_LOGIC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t key_digit;
    uint8_t rf_path;
    bool gpio2;
    bool gpio3;
    const char *bands;
} telit_tuner_branch_t;

typedef enum {
    TELIT_TUNER_FINAL_NONE = 0,
    TELIT_TUNER_FINAL_OK,
    TELIT_TUNER_FINAL_ERROR,
} telit_tuner_final_t;

size_t telit_tuner_branch_count(void);
const telit_tuner_branch_t *telit_tuner_branch_for_digit(uint8_t digit);

/* Parsers accept raw modem bytes so boot-time leading NULs cannot disguise a
 * response. Whitespace around values is accepted; trailing junk is rejected. */
telit_tuner_final_t telit_tuner_parse_final(const uint8_t *line, size_t len);
bool telit_tuner_parse_cfun(const uint8_t *line, size_t len,
                            uint8_t *value);
bool telit_tuner_parse_stuneant(const uint8_t *line, size_t len,
                                bool *enabled);
bool telit_tuner_parse_gpio(const uint8_t *line, size_t len,
                            uint8_t *direction, bool *level);

#endif

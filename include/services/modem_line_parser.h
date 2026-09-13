#ifndef MODEM_LINE_PARSER_H
#define MODEM_LINE_PARSER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "services/call_types.h"

typedef struct {
    uint8_t rssi;
    uint8_t ber;
} modem_csq_line_t;

typedef struct {
    char number[MODEM_PHONE_MAX + 1u];
    uint8_t validity;
} modem_cli_line_t;

bool modem_line_parse_csq(const char *line, modem_csq_line_t *out);
bool modem_line_parse_cereg(const char *line, unsigned *status_out);
void modem_line_parse_cops(const char *line, char *operator_out,
                           size_t operator_cap);
bool modem_line_parse_cpms(const char *line, uint16_t *used_out,
                           uint16_t *total_out);
bool modem_line_parse_cmti(const char *line, const char *expected_storage,
                           uint16_t *index_out);
const char *modem_line_sms_status_from_numeric(unsigned status);
/* CMGR may omit fields. Only fields actually present in the line are updated;
 * caller-owned output buffers otherwise retain their existing value. */
void modem_line_parse_cmgr_header(const char *line,
                                  char *status_out, size_t status_cap,
                                  char *sender_out, size_t sender_cap,
                                  char *timestamp_out, size_t timestamp_cap);
bool modem_line_parse_clip(const char *line, modem_cli_line_t *out);
bool modem_line_parse_ccwa(const char *line, modem_cli_line_t *out);
bool modem_line_is_ccwa_urc(const char *line);
bool modem_line_is_sim_absent_error(const char *line);

#endif

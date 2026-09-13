#ifndef CLOCK_DATE_LOGIC_H
#define CLOCK_DATE_LOGIC_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    CLOCK_DATE_INVALID = 0,
    CLOCK_DATE_ENTERED,
    CLOCK_DATE_DEFAULTED,
} clock_date_resolution_t;

bool clock_date_valid(uint8_t day, uint8_t month, uint16_t year);
bool clock_date_parse(const char *value, uint8_t *out_day, uint8_t *out_month, uint16_t *out_year);

/* Resolve the Date: editor at commit. During the mandatory unset-clock setup,
 * an entirely untouched template selects the v6.00 epoch (01.01.1999). Any
 * partial entry remains invalid; normal date edits never use the default. */
clock_date_resolution_t clock_date_resolve(const char *value,
                                           bool allow_untouched_default,
                                           uint8_t *out_day,
                                           uint8_t *out_month,
                                           uint16_t *out_year);

#endif

#include "apps/clock_date_logic.h"

#include <stddef.h>

static uint16_t parse_fixed_digits(const char *digits, uint8_t start, uint8_t len);
static uint8_t days_in_month(uint8_t month, uint16_t year);
static bool is_leap_year(uint16_t year);
static bool has_digit(const char *value);

bool clock_date_valid(uint8_t day, uint8_t month, uint16_t year) {
    if (year < 1999u || year > 2090u || month < 1u || month > 12u || day < 1u) {
        return false;
    }
    return day <= days_in_month(month, year);
}

bool clock_date_parse(const char *value, uint8_t *out_day, uint8_t *out_month, uint16_t *out_year) {
    if (out_day == NULL || out_month == NULL || out_year == NULL) {
        return false;
    }
    char digits[8];
    uint8_t len = 0u;
    for (size_t i = 0u; value != NULL && value[i] != '\0'; i++) {
        if (value[i] >= '0' && value[i] <= '9') {
            if (len >= sizeof(digits)) {
                return false;
            }
            digits[len++] = value[i];
        }
    }
    if (len != sizeof(digits)) {
        return false;
    }
    uint8_t day = (uint8_t)parse_fixed_digits(digits, 0u, 2u);
    uint8_t month = (uint8_t)parse_fixed_digits(digits, 2u, 2u);
    uint16_t year = parse_fixed_digits(digits, 4u, 4u);
    if (!clock_date_valid(day, month, year)) {
        return false;
    }
    *out_day = day;
    *out_month = month;
    *out_year = year;
    return true;
}

clock_date_resolution_t clock_date_resolve(const char *value,
                                           bool allow_untouched_default,
                                           uint8_t *out_day,
                                           uint8_t *out_month,
                                           uint16_t *out_year) {
    if (clock_date_parse(value, out_day, out_month, out_year)) {
        return CLOCK_DATE_ENTERED;
    }
    if (!allow_untouched_default || value == NULL || has_digit(value) ||
        out_day == NULL || out_month == NULL || out_year == NULL) {
        return CLOCK_DATE_INVALID;
    }
    *out_day = 1u;
    *out_month = 1u;
    *out_year = 1999u;
    return CLOCK_DATE_DEFAULTED;
}

static uint16_t parse_fixed_digits(const char *digits, uint8_t start, uint8_t len) {
    uint16_t value = 0u;
    for (uint8_t i = 0u; i < len; i++) {
        value = (uint16_t)(value * 10u + (uint16_t)(digits[start + i] - '0'));
    }
    return value;
}

static uint8_t days_in_month(uint8_t month, uint16_t year) {
    static const uint8_t DAYS[] = {31u, 28u, 31u, 30u, 31u, 30u, 31u, 31u, 30u, 31u, 30u, 31u};
    if (month == 2u && is_leap_year(year)) {
        return 29u;
    }
    return DAYS[month - 1u];
}

static bool is_leap_year(uint16_t year) {
    return ((year % 4u) == 0u && (year % 100u) != 0u) || ((year % 400u) == 0u);
}

static bool has_digit(const char *value) {
    for (size_t i = 0u; value[i] != '\0'; i++) {
        if (value[i] >= '0' && value[i] <= '9') {
            return true;
        }
    }
    return false;
}

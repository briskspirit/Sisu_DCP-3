#ifndef SERVICES_DATETIME_TYPES_H
#define SERVICES_DATETIME_TYPES_H

#include <stdint.h>

/* Calendar value type shared by the RTC HAL, the persistent store (call-log
 * timestamps) and the clock feature. A plain value, not a device contract:
 * lives here so the store and its consumers need no hal/ header to carry a
 * timestamp. */
typedef struct {
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
} rtc_datetime_t;

#endif

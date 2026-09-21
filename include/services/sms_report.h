#ifndef SMS_REPORT_H
#define SMS_REPORT_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "services/sms_types.h"

/* Vendor-neutral SMS-STATUS-REPORT. Keep the zone and raw TP-ST; a successful
 * submission, or a 'forwarded, unconfirmed' report, is not delivered proof. */
typedef struct {
    uint8_t first_octet, reference, address_type, status;
    char recipient[MODEM_SMS_SENDER_MAX + 1u];
    char submitted[21];
    char discharged[21];
} sms_report_t;

typedef enum { SMS_REPORT_UNKNOWN, SMS_REPORT_DELIVERED,
               SMS_REPORT_PENDING, SMS_REPORT_FAILED } sms_report_state_t;

static inline sms_report_state_t sms_report_state(uint8_t status) {
    if (status == 0u) return SMS_REPORT_DELIVERED;
    if (status >= 0x20u && status <= 0x3fu) return SMS_REPORT_PENDING;
    if (status >= 0x40u && status <= 0x7fu) return SMS_REPORT_FAILED;
    return SMS_REPORT_UNKNOWN;
}

static inline bool sms_report_time_valid(const char value[21]) {
    if (memchr(value, '\0', 21u) != value + 20u) return false;
    unsigned fields[7];
    static const uint8_t positions[] = {0, 3, 6, 9, 12, 15, 18};
    for (unsigned i = 0u; i < 7u; i++) {
        unsigned at = positions[i];
        if (value[at] < '0' || value[at] > '9' ||
            value[at + 1u] < '0' || value[at + 1u] > '9') return false;
        fields[i] = (unsigned)(value[at] - '0') * 10u + (unsigned)(value[at + 1u] - '0');
    }
    if (value[2] != '/' || value[5] != '/' || value[8] != ',' ||
        value[11] != ':' || value[14] != ':' || (value[17] != '+' && value[17] != '-') ||
        fields[1] < 1u || fields[1] > 12u || fields[3] > 23u ||
        fields[4] > 59u || fields[5] > 59u || fields[6] > 79u) return false;
    static const uint8_t days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    unsigned limit = days[fields[1] - 1u] + (fields[1] == 2u && fields[0] % 4u == 0u);
    return fields[2] >= 1u && fields[2] <= limit;
}

static inline bool sms_report_valid(const sms_report_t *report) {
    if (report == NULL || (report->first_octet & 0x23u) != 2u ||
        !(report->address_type & 0x80u) || (report->address_type & 0x70u) == 0x50u ||
        report->recipient[0] == '\0' ||
        !sms_service_centre_valid(report->recipient, sizeof(report->recipient))) return false;
    return sms_report_time_valid(report->submitted) && sms_report_time_valid(report->discharged);
}

#endif

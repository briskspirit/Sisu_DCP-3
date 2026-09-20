#ifndef MODEM_SMS_RECOVERY_H
#define MODEM_SMS_RECOVERY_H

#include "services/modem_sms_direct.h"

typedef enum {
    MODEM_SMS_RECOVERY_NONE, MODEM_SMS_RECOVERY_SELECT,
    MODEM_SMS_RECOVERY_READ, MODEM_SMS_RECOVERY_VERIFY, MODEM_SMS_RECOVERY_DELETE,
    MODEM_SMS_RECOVERY_STORE
} modem_sms_recovery_step_t;

/* Pure, single-record 27.005 recovery. The host owns transport, SIM/session
 * invalidation, and the durable receipt. No storage or HAL dependency. */
typedef struct {
    modem_sms_recovery_step_t step;
    uint16_t capacity, remaining, index;
    uint32_t retry_at, recovered, failures;
    bool pending, again, selected, header_seen, readable, payload_seen, filtered;
    bool retry, verify_match, issued;
    char pdu[SMS_DELIVER_HEX_MAX];
} modem_sms_recovery_t;

void modem_sms_recovery_reset(modem_sms_recovery_t *r);
void modem_sms_recovery_request(modem_sms_recovery_t *r);
bool modem_sms_recovery_command(modem_sms_recovery_t *r, uint32_t now,
                                char *command, size_t cap);
bool modem_sms_recovery_line(modem_sms_recovery_t *r, const char *line,
                             char *delivery_header, size_t cap);
void modem_sms_recovery_payload(modem_sms_recovery_t *r,
                                modem_sms_direct_step_t result, const char *pdu);
void modem_sms_recovery_final(modem_sms_recovery_t *r, bool ok,
                              bool empty_slot, uint32_t now);
void modem_sms_recovery_committed(modem_sms_recovery_t *r);
/* Retain a permanently rejected record and continue this scan without deletion. */
void modem_sms_recovery_rejected(modem_sms_recovery_t *r, uint32_t now);
void modem_sms_recovery_defer(modem_sms_recovery_t *r, uint32_t now);

#endif

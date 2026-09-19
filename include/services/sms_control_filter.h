#ifndef SMS_CONTROL_FILTER_H
#define SMS_CONTROL_FILTER_H

#include "services/sms_picture_codec.h"

typedef enum {
    SMS_CONTROL_KEEP = 0,
    SMS_CONTROL_TYPE0,
    SMS_CONTROL_VVM,
    SMS_CONTROL_OMA_DM,
} sms_control_filter_t;

/* A storage/UI policy, not a management client or an authentication check.
 * wdp must come from the receiving transport, never from payload sniffing. */
sms_control_filter_t sms_control_classify(const sms_codec_message_t *message,
                                          bool wdp);
/* Same policy for a validated, fully assembled payload. The caller must clear
 * has_concat only after proving completeness and unambiguous part identity. */
sms_control_filter_t sms_control_classify_payload(const sms_codec_message_t *message,
    bool wdp, const uint8_t *payload, size_t length);

#endif

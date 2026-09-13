#ifndef SMS_IDENTITY_H
#define SMS_IDENTITY_H

#include <stdint.h>

/* Stable identity binding compact mailbox metadata to a later lazy body read.
 * Message-store indices are reusable, while sender + timestamp + logical body
 * identify whether the selected slot still contains the listed message. */
uint32_t sms_identity_hash(const char *sender, const char *timestamp,
                           const char *body);

#endif

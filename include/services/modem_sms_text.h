#ifndef MODEM_SMS_TEXT_H
#define MODEM_SMS_TEXT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "services/sms_types.h"

/* Modem-owned wire representation, never exposed to applications/storage. */
typedef struct {
    uint8_t body[MODEM_SMS_TEXT_MAX * 4u + 1u];
    size_t length;
    uint8_t dcs;
    bool multipart;
} modem_sms_text_t;

#endif

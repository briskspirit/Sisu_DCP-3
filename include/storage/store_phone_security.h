#ifndef STORE_PHONE_SECURITY_H
#define STORE_PHONE_SECURITY_H

#include "storage/store_service.h"

#define PHONE_SECURITY_SIM_COUNT 5u
#define PHONE_SECURITY_SIM_CAP 21u

typedef struct {
    bool enabled;
    bool clear_calls;
    uint8_t failures;
    char sims[PHONE_SECURITY_SIM_COUNT][PHONE_SECURITY_SIM_CAP];
} phone_security_record_t;

bool store_phone_security_read(phone_security_record_t *out);
/* Success means the entire record is durable, not merely dirty in RAM. */
bool store_phone_security_write(const phone_security_record_t *record);
bool phone_security_identity_valid(const char *identity);

#endif

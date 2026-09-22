#ifndef PHONE_SECURITY_H
#define PHONE_SECURITY_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    PHONE_SECURITY_OPEN,
    PHONE_SECURITY_WAIT_IDENTITY,
    PHONE_SECURITY_LOCKED,
    PHONE_SECURITY_FAULT,
} phone_security_state_t;
typedef enum {
    SECURITY_CODE_OK,
    SECURITY_CODE_WRONG,
    SECURITY_CODE_UNAVAILABLE,
} security_code_result_t;

void phone_security_init(uint32_t now);
/* NULL retires authorization immediately; only a final-qualified identity is valid. */
void phone_security_tick(const char *identity, uint32_t now);
phone_security_state_t phone_security_state(void);
bool phone_security_enabled(void);
uint32_t phone_security_generation(void);
void security_code_forget_grant(void);
security_code_result_t security_code_verify(const char *code, uint32_t now);
bool security_code_change(const char *code);
bool phone_security_unlock(uint32_t now);
/* The caller must own a successful code challenge for the current UI session. */
bool phone_security_set_enabled(bool enabled, uint32_t now);
bool phone_security_restore_defaults(void);

#endif

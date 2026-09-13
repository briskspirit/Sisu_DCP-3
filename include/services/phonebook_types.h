#ifndef PHONEBOOK_TYPES_H
#define PHONEBOOK_TYPES_H

#include <stdbool.h>
#include <stdint.h>

#include "services/call_types.h"

#define MODEM_PHONEBOOK_NAME_MAX 24u
#define MODEM_PHONEBOOK_FIRST_INDEX 1u
#define MODEM_PHONEBOOK_LAST_INDEX 500u
#define MODEM_PHONEBOOK_MAX_RECORDS \
    (MODEM_PHONEBOOK_LAST_INDEX - MODEM_PHONEBOOK_FIRST_INDEX + 1u)
#define MODEM_PHONEBOOK_RESULT_CAPACITY 8u
/* Keep contacts in module memory; this policy is shared without importing the
 * root modem-service contract. The 1..500 ME domain was bench-confirmed on the
 * fitted LE910C1-WWX with AT+CPBS? and AT+CPBR=?. */
#define MODEM_PHONEBOOK_STORAGE "ME"

typedef enum {
    MODEM_PHONEBOOK_OP_NONE = 0,
    MODEM_PHONEBOOK_OP_LIST,
    MODEM_PHONEBOOK_OP_ADD,
    MODEM_PHONEBOOK_OP_UPDATE,
    MODEM_PHONEBOOK_OP_DELETE,
} modem_phonebook_op_t;

typedef enum {
    MODEM_PHONEBOOK_OUTCOME_NONE = 0,
    MODEM_PHONEBOOK_OUTCOME_OK,
    MODEM_PHONEBOOK_OUTCOME_ERROR,
    MODEM_PHONEBOOK_OUTCOME_TIMEOUT,
    MODEM_PHONEBOOK_OUTCOME_CANCELLED,
    MODEM_PHONEBOOK_OUTCOME_EVICTED,
} modem_phonebook_outcome_t;

typedef struct {
    uint16_t index;
    char name[MODEM_PHONEBOOK_NAME_MAX + 1u];
    char number[MODEM_PHONE_MAX + 1u];
} modem_phonebook_entry_t;

typedef struct {
    uint32_t request_id;
    modem_phonebook_op_t kind;
    modem_phonebook_outcome_t outcome;
    bool sim_not_ready;
} modem_phonebook_result_t;

#endif /* PHONEBOOK_TYPES_H */

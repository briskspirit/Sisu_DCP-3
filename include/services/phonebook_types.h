#ifndef PHONEBOOK_TYPES_H
#define PHONEBOOK_TYPES_H

#include <stdbool.h>
#include <stdint.h>

#include "services/call_types.h"

#define PHONEBOOK_NAME_MAX 24u
#define PHONEBOOK_MAX_RECORDS 500u
#define PHONEBOOK_RESULT_CAPACITY 8u

typedef enum {
    PHONEBOOK_OP_NONE = 0,
    PHONEBOOK_OP_LIST,
    PHONEBOOK_OP_ADD,
    PHONEBOOK_OP_UPDATE,
    PHONEBOOK_OP_DELETE,
} phonebook_op_t;

typedef enum {
    PHONEBOOK_OUTCOME_NONE = 0,
    PHONEBOOK_OUTCOME_OK,
    PHONEBOOK_OUTCOME_ERROR,
    PHONEBOOK_OUTCOME_FULL,
} phonebook_outcome_t;

typedef struct {
    uint32_t index;
    char name[PHONEBOOK_NAME_MAX + 1u];
    char number[MODEM_PHONE_MAX + 1u];
} phonebook_entry_t;

typedef struct {
    uint32_t request_id;
    phonebook_op_t kind;
    phonebook_outcome_t outcome;
} phonebook_result_t;

#endif /* PHONEBOOK_TYPES_H */

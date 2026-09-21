#ifndef CALL_WAITING_TYPES_H
#define CALL_WAITING_TYPES_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    CALL_WAITING_NONE = 0,
    CALL_WAITING_ACTIVATE,
    CALL_WAITING_CANCEL,
    CALL_WAITING_QUERY,
} call_waiting_action_t;

typedef enum {
    CALL_WAITING_SUCCESS = 0,
    CALL_WAITING_NO_NETWORK,
    CALL_WAITING_NOT_DONE,
    CALL_WAITING_UNKNOWN,
    CALL_WAITING_CANCELLED,
} call_waiting_outcome_t;

typedef struct {
    bool active;
    uint8_t class_mask;
} call_waiting_row_t;

typedef struct {
    uint32_t request_id;
    call_waiting_action_t action;
    call_waiting_outcome_t outcome;
    bool status_known;
    bool active;
} call_waiting_result_t;

#endif

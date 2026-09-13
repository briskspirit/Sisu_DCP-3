#ifndef CALL_FORWARD_TYPES_H
#define CALL_FORWARD_TYPES_H

/* Vendor-neutral 3GPP call-forwarding contract. Raw command values stop at the
 * selected modem adapter; the service and UI consume these semantic types. */

#include <stdbool.h>
#include <stdint.h>

#include "services/message_waiting_types.h"

#include "services/call_types.h"

typedef enum {
    CALL_FORWARD_REASON_UNCONDITIONAL = 0,
    CALL_FORWARD_REASON_BUSY = 1,
    CALL_FORWARD_REASON_NO_REPLY = 2,
    CALL_FORWARD_REASON_NOT_REACHABLE = 3,
    CALL_FORWARD_REASON_ALL = 4,
    CALL_FORWARD_REASON_ALL_CONDITIONAL = 5,
} call_forward_reason_t;

typedef enum {
    CALL_FORWARD_ACTION_DISABLE = 0,
    CALL_FORWARD_ACTION_ENABLE = 1,
    CALL_FORWARD_ACTION_QUERY = 2,
    CALL_FORWARD_ACTION_REGISTER = 3,
    CALL_FORWARD_ACTION_ERASE = 4,
} call_forward_action_t;

typedef struct {
    call_forward_reason_t reason;
    call_forward_action_t action;
    bool has_number;
    char number[MODEM_PHONE_MAX + 1u];
    bool has_delay;
    uint8_t delay_seconds;
} call_forward_request_t;

/* One normalized call-forwarding query row. class_mask remains neutral 3GPP basic-
 * service bits; callers select voice with bit 0 and never assume a vendor's
 * default class. */
typedef struct {
    bool active;
    uint8_t class_mask;
    bool has_number;
    char number[MODEM_PHONE_MAX + 1u];
    uint8_t number_type;
    bool has_delay;
    uint8_t delay_seconds;
} call_forward_row_t;

typedef enum {
    CALL_FORWARD_OUTCOME_NONE = 0,
    CALL_FORWARD_OUTCOME_SUCCESS,
    CALL_FORWARD_OUTCOME_NO_NETWORK,
    CALL_FORWARD_OUTCOME_NOT_DONE,
    CALL_FORWARD_OUTCOME_RESULT_UNKNOWN,
    CALL_FORWARD_OUTCOME_CANCELLED,
} call_forward_outcome_t;

typedef struct {
    bool pending;
    uint32_t request_id;
    call_forward_request_t request;
    call_forward_outcome_t outcome;
    bool status_known;
    bool active;
    char number[MODEM_PHONE_MAX + 1u];
    bool has_delay;
    uint8_t delay_seconds;
} call_forward_result_t;

typedef struct {
    uint16_t index;
    char number[MODEM_PHONE_MAX + 1u];
    uint8_t number_type;
    bool voice;
} modem_voice_mailbox_row_t;

typedef enum {
    MODEM_AUX_EVENT_NONE = 0,
    MODEM_AUX_EVENT_MESSAGE_WAITING,
    MODEM_AUX_EVENT_CFU_STATE,
    MODEM_AUX_EVENT_INCOMING_DIVERTED,
} modem_aux_event_kind_t;

typedef struct {
    modem_aux_event_kind_t kind;
    modem_message_waiting_category_t message_waiting_category;
    modem_message_waiting_category_mask_t message_waiting_uncertain_mask;
    bool active;
    uint16_t count;
    char number[MODEM_PHONE_MAX + 1u];
} modem_aux_event_t;

/* A solicited message-waiting row can share its prefix and field shape with
 * an unsolicited update. AMBIGUOUS means the backend recognized a real
 * readback but cannot derive authoritative states for the categories named by
 * message_waiting_uncertain_mask. Callers preserve those categories rather
 * than reinterpret the row as a URC. */
typedef enum {
    MODEM_MESSAGE_WAITING_ROW_INVALID = 0,
    MODEM_MESSAGE_WAITING_ROW_VALID,
    MODEM_MESSAGE_WAITING_ROW_AMBIGUOUS,
} modem_message_waiting_row_result_t;

#endif /* CALL_FORWARD_TYPES_H */

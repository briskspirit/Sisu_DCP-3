#ifndef MODEM_PHONEBOOK_PROTOCOL_INTERNAL_H
#define MODEM_PHONEBOOK_PROTOCOL_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>

#include "services/phonebook_types.h"

typedef enum {
    MODEM_PHONEBOOK_COMMAND_CPBS = 0,
    MODEM_PHONEBOOK_COMMAND_CPBR,
    MODEM_PHONEBOOK_COMMAND_CPBW,
} modem_phonebook_command_kind_t;

/* The root request owns the pointed-to strings for the whole operation. The
 * phonebook module borrows this view only during synchronous entry calls. */
typedef struct {
    uint32_t request_id;
    modem_phonebook_op_t operation;
    uint16_t index;
    const char *name;
    const char *number;
} modem_phonebook_protocol_request_t;

typedef enum {
    MODEM_PHONEBOOK_ACTION_COMMAND = 0,
    MODEM_PHONEBOOK_ACTION_BEGIN_REFRESH,
    MODEM_PHONEBOOK_ACTION_APPEND_ENTRY,
    MODEM_PHONEBOOK_ACTION_FINISH_REFRESH,
    MODEM_PHONEBOOK_ACTION_COMPLETE,
} modem_phonebook_action_type_t;

typedef struct {
    modem_phonebook_action_type_t type;
    union {
        struct {
            modem_phonebook_command_kind_t kind;
            const char *command;
            uint32_t timeout_ms;
            uint32_t now_ms;
        } command;
        modem_phonebook_entry_t entry;
        struct {
            bool publish;
        } refresh;
        struct {
            uint32_t request_id;
            modem_phonebook_op_t operation;
            modem_phonebook_outcome_t outcome;
        } complete;
    } data;
} modem_phonebook_protocol_action_t;

/* Hooks are synchronous and are never retained. The monotonic integrity
 * counter lets a read reject UART/framing loss without importing transport or
 * root-service state. */
typedef struct {
    bool (*emit)(const modem_phonebook_protocol_action_t *action);
    uint32_t (*transport_counter)(void);
} modem_phonebook_protocol_hooks_t;

bool modem_phonebook_protocol_begin(
    const modem_phonebook_protocol_request_t *request,
    const modem_phonebook_protocol_hooks_t *hooks, uint32_t now_ms);
bool modem_phonebook_protocol_parse_line(
    modem_phonebook_command_kind_t kind, const char *line,
    const modem_phonebook_protocol_hooks_t *hooks);
void modem_phonebook_protocol_line_dropped(void);
void modem_phonebook_protocol_on_final(
    modem_phonebook_command_kind_t kind, bool ok,
    const modem_phonebook_protocol_request_t *request,
    const modem_phonebook_protocol_hooks_t *hooks, uint32_t now_ms);
void modem_phonebook_protocol_on_timeout(
    modem_phonebook_command_kind_t kind,
    const modem_phonebook_protocol_request_t *request,
    const modem_phonebook_protocol_hooks_t *hooks);
void modem_phonebook_protocol_cancel(
    const modem_phonebook_protocol_hooks_t *hooks);

#endif /* MODEM_PHONEBOOK_PROTOCOL_INTERNAL_H */

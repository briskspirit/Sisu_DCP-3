#include "services/modem_phonebook_state.h"

#include <stdio.h>
#include <string.h>

#include "modem_phonebook_protocol_internal.h"
#include "services/modem_at_util.h"

static modem_phonebook_entry_t s_cache[MODEM_PHONEBOOK_MAX_RECORDS];
static uint16_t s_count;
static uint16_t s_staging_count;
static bool s_refresh_active;
static bool s_cache_valid;
static uint32_t s_next_request_id;
static uint32_t s_reserved_request_ids[MODEM_PHONEBOOK_RESULT_CAPACITY];
static uint8_t s_reserved_count;
static modem_phonebook_result_t
    s_results[MODEM_PHONEBOOK_RESULT_CAPACITY];
static uint8_t s_result_head;
static uint8_t s_result_count;

typedef struct {
    bool active;
    bool clean;
    uint16_t last_index;
    uint32_t transport_counter;
} phonebook_read_integrity_t;

static phonebook_read_integrity_t s_read;

static bool emit(const modem_phonebook_protocol_hooks_t *hooks,
                 const modem_phonebook_protocol_action_t *action) {
    return hooks != NULL && hooks->emit != NULL && action != NULL &&
           hooks->emit(action);
}

static bool hooks_valid(const modem_phonebook_protocol_hooks_t *hooks) {
    return hooks != NULL && hooks->emit != NULL &&
           hooks->transport_counter != NULL;
}

static bool request_valid(const modem_phonebook_protocol_request_t *request) {
    if (request == NULL || request->request_id == 0u) {
        return false;
    }
    switch (request->operation) {
    case MODEM_PHONEBOOK_OP_LIST:
        return true;
    case MODEM_PHONEBOOK_OP_ADD:
        return request->number != NULL && request->number[0] != '\0';
    case MODEM_PHONEBOOK_OP_UPDATE:
        return request->index >= MODEM_PHONEBOOK_FIRST_INDEX &&
               request->index <= MODEM_PHONEBOOK_LAST_INDEX &&
               request->number != NULL &&
               request->number[0] != '\0';
    case MODEM_PHONEBOOK_OP_DELETE:
        return request->index >= MODEM_PHONEBOOK_FIRST_INDEX &&
               request->index <= MODEM_PHONEBOOK_LAST_INDEX;
    case MODEM_PHONEBOOK_OP_NONE:
    default:
        return false;
    }
}

static bool emit_command(const modem_phonebook_protocol_hooks_t *hooks,
                         modem_phonebook_command_kind_t kind,
                         const char *command, uint32_t timeout_ms,
                         uint32_t now_ms) {
    modem_phonebook_protocol_action_t action = {
        .type = MODEM_PHONEBOOK_ACTION_COMMAND,
        .data.command = {
            .kind = kind,
            .command = command,
            .timeout_ms = timeout_ms,
            .now_ms = now_ms,
        },
    };
    return emit(hooks, &action);
}

static bool emit_refresh(const modem_phonebook_protocol_hooks_t *hooks,
                         modem_phonebook_action_type_t type,
                         bool publish) {
    const modem_phonebook_protocol_action_t action = {
        .type = type,
        .data.refresh = { .publish = publish },
    };
    return emit(hooks, &action);
}

static void emit_complete(const modem_phonebook_protocol_request_t *request,
                          const modem_phonebook_protocol_hooks_t *hooks,
                          modem_phonebook_outcome_t outcome) {
    modem_phonebook_protocol_action_t action = {
        .type = MODEM_PHONEBOOK_ACTION_COMPLETE,
        .data.complete = {
            .request_id = request != NULL ? request->request_id : 0u,
            .operation = request != NULL ? request->operation
                                         : MODEM_PHONEBOOK_OP_NONE,
            .outcome = outcome,
        },
    };
    (void)emit(hooks, &action);
}

static bool start_read(const modem_phonebook_protocol_hooks_t *hooks,
                       uint32_t now_ms) {
    memset(&s_read, 0, sizeof(s_read));
    s_read.active = true;
    s_read.clean = true;
    s_read.transport_counter = hooks->transport_counter();
    if (!emit_refresh(hooks, MODEM_PHONEBOOK_ACTION_BEGIN_REFRESH, false) ||
        !emit_command(hooks, MODEM_PHONEBOOK_COMMAND_CPBR,
                      "AT+CPBR=1,500", 15000u, now_ms)) {
        s_read.active = false;
        (void)emit_refresh(hooks, MODEM_PHONEBOOK_ACTION_FINISH_REFRESH,
                           false);
        return false;
    }
    return true;
}

static bool start_write(const modem_phonebook_protocol_request_t *request,
                        const modem_phonebook_protocol_hooks_t *hooks,
                        uint32_t now_ms) {
    char command[128];
    if (request->operation == MODEM_PHONEBOOK_OP_DELETE) {
        snprintf(command, sizeof(command), "AT+CPBW=%u",
                 (unsigned)request->index);
    } else {
        char number[MODEM_PHONE_MAX + 4u];
        char name[MODEM_PHONEBOOK_NAME_MAX + 4u];
        modem_at_append_quoted(number, sizeof(number), request->number);
        modem_at_append_quoted(name, sizeof(name), request->name);
        /* Type-of-address: 145 (international) for a leading '+', else 129
         * (national/unknown) -- mirrors the SMS address path and 3GPP 27.007
         * 8.14. Hardcoding 129 made strict modems reject or mis-store '+'-
         * prefixed contacts. The '+' remains quoted, so CPBR round-trips it. */
        unsigned toa = request->number[0] == '+' ? 145u : 129u;
        if (request->operation == MODEM_PHONEBOOK_OP_UPDATE) {
            snprintf(command, sizeof(command), "AT+CPBW=%u,%s,%u,%s",
                     (unsigned)request->index, number, toa, name);
        } else {
            snprintf(command, sizeof(command), "AT+CPBW=,%s,%u,%s", number,
                     toa, name);
        }
    }
    return emit_command(hooks, MODEM_PHONEBOOK_COMMAND_CPBW, command, 10000u,
                        now_ms);
}

static void skip_spaces(const char **cursor) {
    while (**cursor == ' ') {
        (*cursor)++;
    }
}

static bool parse_uint(const char **cursor, uint32_t maximum,
                       uint32_t *value_out) {
    const char *p = *cursor;
    if (*p < '0' || *p > '9') {
        return false;
    }
    uint32_t value = 0u;
    do {
        uint32_t digit = (uint32_t)(*p - '0');
        if (digit > maximum || value > (maximum - digit) / 10u) {
            return false;
        }
        value = value * 10u + digit;
        p++;
    } while (*p >= '0' && *p <= '9');
    *cursor = p;
    *value_out = value;
    return true;
}

static bool parse_quoted(const char **cursor, char *out, size_t cap) {
    const char *p = *cursor;
    if (*p++ != '"' || cap == 0u) {
        return false;
    }
    size_t length = 0u;
    while (*p != '\0' && *p != '"') {
        if (length + 1u < cap) {
            out[length] = *p;
        }
        length++;
        p++;
    }
    if (*p != '"') {
        return false;
    }
    out[length < cap ? length : cap - 1u] = '\0';
    *cursor = p + 1u;
    return true;
}

static bool parse_cpbr_entry(const char *line,
                             modem_phonebook_entry_t *entry) {
    static const char prefix[] = "+CPBR:";
    if (line == NULL || entry == NULL ||
        strncmp(line, prefix, sizeof(prefix) - 1u) != 0) {
        return false;
    }

    const char *cursor = line + sizeof(prefix) - 1u;
    uint32_t index = 0u;
    uint32_t type = 0u;
    skip_spaces(&cursor);
    if (!parse_uint(&cursor, MODEM_PHONEBOOK_LAST_INDEX, &index) ||
        index < MODEM_PHONEBOOK_FIRST_INDEX) {
        return false;
    }
    skip_spaces(&cursor);
    if (*cursor++ != ',') {
        return false;
    }

    memset(entry, 0, sizeof(*entry));
    entry->index = (uint16_t)index;
    skip_spaces(&cursor);
    if (!parse_quoted(&cursor, entry->number, sizeof(entry->number))) {
        return false;
    }
    skip_spaces(&cursor);
    if (*cursor++ != ',') {
        return false;
    }
    skip_spaces(&cursor);
    if (!parse_uint(&cursor, UINT8_MAX, &type)) {
        return false;
    }
    (void)type;
    skip_spaces(&cursor);
    if (*cursor == '\0') {
        return true;
    }
    if (*cursor++ != ',') {
        return false;
    }
    skip_spaces(&cursor);
    if (*cursor == '\0') {
        return true;
    }
    if (!parse_quoted(&cursor, entry->name, sizeof(entry->name))) {
        return false;
    }
    skip_spaces(&cursor);
    if (*cursor != '\0' && *cursor != ',') {
        return false;
    }
    return true;
}

void modem_phonebook_state_init(void) {
    s_count = 0u;
    s_staging_count = 0u;
    s_refresh_active = false;
    s_cache_valid = false;
    memset(&s_read, 0, sizeof(s_read));
    s_next_request_id = 0u;
    memset(s_reserved_request_ids, 0, sizeof(s_reserved_request_ids));
    s_reserved_count = 0u;
    memset(s_results, 0, sizeof(s_results));
    s_result_head = 0u;
    s_result_count = 0u;
}

void modem_phonebook_state_clear(void) {
    s_count = 0u;
    s_staging_count = 0u;
    s_refresh_active = false;
    s_cache_valid = false;
    memset(&s_read, 0, sizeof(s_read));
}

void modem_phonebook_state_refresh_begin(void) {
    s_count = 0u;
    s_staging_count = 0u;
    s_refresh_active = true;
    s_cache_valid = false;
}

bool modem_phonebook_state_refresh_finish(bool publish) {
    if (!s_refresh_active) {
        return false;
    }
    s_count = publish ? s_staging_count : 0u;
    s_staging_count = 0u;
    s_refresh_active = false;
    s_cache_valid = publish;
    return true;
}

bool modem_phonebook_state_append(const modem_phonebook_entry_t *entry) {
    uint16_t *count = s_refresh_active ? &s_staging_count : &s_count;
    if (entry == NULL || *count >= MODEM_PHONEBOOK_MAX_RECORDS) {
        return false;
    }
    s_cache[(*count)++] = *entry;
    return true;
}

bool modem_phonebook_state_cache_valid(void) {
    return s_cache_valid;
}

uint16_t modem_phonebook_state_count(void) {
    return s_count;
}

bool modem_phonebook_state_entry(uint16_t position,
                                 modem_phonebook_entry_t *out) {
    if (out == NULL || position >= s_count) {
        return false;
    }
    *out = s_cache[position];
    return true;
}

static bool request_id_in_use(uint32_t request_id) {
    for (uint8_t i = 0u; i < MODEM_PHONEBOOK_RESULT_CAPACITY; i++) {
        if (s_reserved_request_ids[i] == request_id) {
            return true;
        }
    }
    for (uint8_t i = 0u; i < s_result_count; i++) {
        uint8_t position = (uint8_t)(
            (s_result_head + i) % MODEM_PHONEBOOK_RESULT_CAPACITY);
        if (s_results[position].request_id == request_id) {
            return true;
        }
    }
    return false;
}

bool modem_phonebook_state_reserve_request(uint32_t *request_id_out) {
    if (request_id_out == NULL ||
        (uint8_t)(s_reserved_count + s_result_count) >=
            MODEM_PHONEBOOK_RESULT_CAPACITY) {
        return false;
    }

    uint32_t request_id;
    do {
        request_id = ++s_next_request_id;
    } while (request_id == 0u || request_id_in_use(request_id));

    for (uint8_t i = 0u; i < MODEM_PHONEBOOK_RESULT_CAPACITY; i++) {
        if (s_reserved_request_ids[i] == 0u) {
            s_reserved_request_ids[i] = request_id;
            s_reserved_count++;
            *request_id_out = request_id;
            return true;
        }
    }
    return false;
}

bool modem_phonebook_state_release_request(uint32_t request_id) {
    if (request_id == 0u) {
        return false;
    }
    for (uint8_t i = 0u; i < MODEM_PHONEBOOK_RESULT_CAPACITY; i++) {
        if (s_reserved_request_ids[i] == request_id) {
            s_reserved_request_ids[i] = 0u;
            s_reserved_count--;
            return true;
        }
    }
    return false;
}

bool modem_phonebook_state_publish_result(
    uint32_t request_id, modem_phonebook_op_t kind,
    modem_phonebook_outcome_t outcome, bool sim_not_ready) {
    if (request_id == 0u || kind == MODEM_PHONEBOOK_OP_NONE ||
        outcome == MODEM_PHONEBOOK_OUTCOME_NONE) {
        return false;
    }

    uint8_t reservation = MODEM_PHONEBOOK_RESULT_CAPACITY;
    for (uint8_t i = 0u; i < MODEM_PHONEBOOK_RESULT_CAPACITY; i++) {
        if (s_reserved_request_ids[i] == request_id) {
            reservation = i;
            break;
        }
    }
    if (reservation == MODEM_PHONEBOOK_RESULT_CAPACITY ||
        s_result_count >= MODEM_PHONEBOOK_RESULT_CAPACITY) {
        return false;
    }

    uint8_t tail = (uint8_t)(
        (s_result_head + s_result_count) % MODEM_PHONEBOOK_RESULT_CAPACITY);
    s_results[tail] = (modem_phonebook_result_t){
        .request_id = request_id,
        .kind = kind,
        .outcome = outcome,
        .sim_not_ready = sim_not_ready,
    };
    s_result_count++;
    s_reserved_request_ids[reservation] = 0u;
    s_reserved_count--;
    return true;
}

bool modem_phonebook_state_pop_result(modem_phonebook_result_t *out) {
    if (out == NULL || s_result_count == 0u) {
        return false;
    }
    *out = s_results[s_result_head];
    memset(&s_results[s_result_head], 0, sizeof(s_results[s_result_head]));
    s_result_head = (uint8_t)(
        (s_result_head + 1u) % MODEM_PHONEBOOK_RESULT_CAPACITY);
    s_result_count--;
    return true;
}

bool modem_phonebook_protocol_begin(
    const modem_phonebook_protocol_request_t *request,
    const modem_phonebook_protocol_hooks_t *hooks, uint32_t now_ms) {
    if (!request_valid(request) || !hooks_valid(hooks)) {
        return false;
    }
    return emit_command(hooks, MODEM_PHONEBOOK_COMMAND_CPBS,
                        "AT+CPBS=\"" MODEM_PHONEBOOK_STORAGE "\"", 5000u,
                        now_ms);
}

bool modem_phonebook_protocol_parse_line(
    modem_phonebook_command_kind_t kind, const char *line,
    const modem_phonebook_protocol_hooks_t *hooks) {
    if (line == NULL) {
        return false;
    }
    if (kind == MODEM_PHONEBOOK_COMMAND_CPBS) {
        return strncmp(line, "+CPBS:", sizeof("+CPBS:") - 1u) == 0;
    }
    if (kind != MODEM_PHONEBOOK_COMMAND_CPBR ||
        strncmp(line, "+CPBR:", sizeof("+CPBR:") - 1u) != 0) {
        return false;
    }

    modem_phonebook_entry_t entry;
    bool valid = s_read.active && hooks_valid(hooks) &&
                 parse_cpbr_entry(line, &entry) &&
                 entry.index > s_read.last_index;
    if (valid) {
        modem_phonebook_protocol_action_t action = {
            .type = MODEM_PHONEBOOK_ACTION_APPEND_ENTRY,
            .data.entry = entry,
        };
        valid = emit(hooks, &action);
    }
    if (valid) {
        s_read.last_index = entry.index;
    } else if (s_read.active) {
        s_read.clean = false;
    }
    /* A malformed solicited CPBR row is still owned by this command. Letting it
     * escape into the URC router would only misclassify modem response bytes. */
    return true;
}

void modem_phonebook_protocol_line_dropped(void) {
    if (s_read.active) {
        s_read.clean = false;
    }
}

void modem_phonebook_protocol_on_final(
    modem_phonebook_command_kind_t kind, bool ok,
    const modem_phonebook_protocol_request_t *request,
    const modem_phonebook_protocol_hooks_t *hooks, uint32_t now_ms) {
    if (!request_valid(request) || !hooks_valid(hooks)) {
        emit_complete(request, hooks, MODEM_PHONEBOOK_OUTCOME_ERROR);
        return;
    }

    switch (kind) {
    case MODEM_PHONEBOOK_COMMAND_CPBS:
        if (!ok) {
            emit_complete(request, hooks, MODEM_PHONEBOOK_OUTCOME_ERROR);
        } else if (request->operation == MODEM_PHONEBOOK_OP_LIST) {
            if (!start_read(hooks, now_ms)) {
                emit_complete(request, hooks, MODEM_PHONEBOOK_OUTCOME_ERROR);
            }
        } else if (!start_write(request, hooks, now_ms)) {
            emit_complete(request, hooks, MODEM_PHONEBOOK_OUTCOME_ERROR);
        }
        break;
    case MODEM_PHONEBOOK_COMMAND_CPBW:
        if (!ok) {
            emit_complete(request, hooks, MODEM_PHONEBOOK_OUTCOME_ERROR);
        } else if (!start_read(hooks, now_ms)) {
            emit_complete(request, hooks, MODEM_PHONEBOOK_OUTCOME_ERROR);
        }
        break;
    case MODEM_PHONEBOOK_COMMAND_CPBR: {
        bool publish = ok && s_read.active && s_read.clean &&
            hooks->transport_counter() == s_read.transport_counter;
        s_read.active = false;
        if (!emit_refresh(hooks, MODEM_PHONEBOOK_ACTION_FINISH_REFRESH,
                          publish)) {
            publish = false;
        }
        emit_complete(request, hooks,
                      publish ? MODEM_PHONEBOOK_OUTCOME_OK
                              : MODEM_PHONEBOOK_OUTCOME_ERROR);
        break;
    }
    default:
        break;
    }
}

void modem_phonebook_protocol_on_timeout(
    modem_phonebook_command_kind_t kind,
    const modem_phonebook_protocol_request_t *request,
    const modem_phonebook_protocol_hooks_t *hooks) {
    if (kind == MODEM_PHONEBOOK_COMMAND_CPBR && s_read.active) {
        s_read.active = false;
        (void)emit_refresh(hooks, MODEM_PHONEBOOK_ACTION_FINISH_REFRESH,
                           false);
    }
    emit_complete(request, hooks, MODEM_PHONEBOOK_OUTCOME_TIMEOUT);
}

void modem_phonebook_protocol_cancel(
    const modem_phonebook_protocol_hooks_t *hooks) {
    if (s_read.active) {
        s_read.active = false;
        (void)emit_refresh(hooks, MODEM_PHONEBOOK_ACTION_FINISH_REFRESH,
                           false);
    }
    memset(&s_read, 0, sizeof(s_read));
}

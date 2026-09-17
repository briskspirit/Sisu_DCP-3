#include "modem_vendor_telit_internal.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------------ */
/* Call events, call-list rows, and call-control builders.                  */
/* ------------------------------------------------------------------------ */

bool telit_parse_ecam(const char *line, modem_call_event_t *out) {
    if (out == NULL) {
        return false;
    }
    telit_csv_view_t fields[7];
    size_t count = 0u;
    if (!telit_view_split_prefixed(line, "#ECAM:", fields, 7u, &count) ||
        count < 6u || count > 7u) {
        return false;
    }

    uint32_t call_id = 0u;
    uint32_t status = 0u;
    uint32_t call_type = 0u;
    if (!telit_view_parse_u32(fields[0], 255u, &call_id) ||
        !telit_view_parse_u32(fields[1], 9u, &status) ||
        !telit_view_parse_u32(fields[2], 2u, &call_type) ||
        (call_type != 1u && call_type != 2u) ||
        fields[3].length != 0u || fields[4].length != 0u) {
        return false;
    }

    if (fields[5].length != 0u) {
        uint32_t number_type = 0u;
        if (status != 1u || fields[5].length > MODEM_PHONE_MAX ||
            count != 7u ||
            !telit_view_parse_u32(fields[6], 255u, &number_type) ||
            (number_type != 129u && number_type != 145u)) {
            return false;
        }
    } else if (count == 7u && fields[6].length != 0u) {
        return false;
    }

    static const modem_call_event_kind_t status_map[10] = {
        MODEM_CALL_EV_RELEASED,
        MODEM_CALL_EV_DIALING,
        MODEM_CALL_EV_ALERTING_MO,
        MODEM_CALL_EV_ACTIVE,
        MODEM_CALL_EV_HELD,
        MODEM_CALL_EV_WAITING_MT,
        MODEM_CALL_EV_RINGING_MT,
        MODEM_CALL_EV_BUSY,
        MODEM_CALL_EV_ACTIVE,
        MODEM_CALL_EV_SETUP_DONE,
    };

    /* The Rev. 4 manual's own examples always use ccid=0. Keep every ECAM ID
     * coarse until board traces prove a stable ECAM-to-CLCC identity mapping. */
    out->call_id = call_id <= MODEM_CALL_ID_MAX ? (uint8_t)call_id : 0u;
    out->id_valid = false;
    out->event = status_map[status];
    return true;
}

static bool telit_dial_number_valid(const char *number) {
    if (number == NULL || number[0] == '\0') {
        return false;
    }
    size_t len = strlen(number);
    if (len > MODEM_PHONE_MAX) {
        return false;
    }
    for (size_t i = 0u; i < len; i++) {
        char ch = number[i];
        bool allowed = (ch >= '0' && ch <= '9') || ch == '*' || ch == '#' ||
                       ch == 'p' || ch == 'P' || ch == 'w' || ch == 'W' ||
                       ch == ',';
        if (ch == '+' && i == 0u) {
            allowed = true;
        }
        if (!allowed) {
            return false;
        }
    }
    return true;
}

bool telit_build_call_command(call_txn_kind_t kind, const char *number,
                              uint8_t target_id, char *out,
                              size_t out_cap, uint32_t *timeout_ms) {
    if (out == NULL || out_cap == 0u || timeout_ms == NULL) {
        return false;
    }

    const char *fixed = NULL;
    uint32_t timeout = 25000u;
    int written = -1;
    switch (kind) {
    case CALL_TXN_DIAL:
        if (!telit_dial_number_valid(number)) {
            return false;
        }
        timeout = 30000u;
        written = snprintf(out, out_cap, "ATD%s;", number);
        break;
    case CALL_TXN_ANSWER:
        fixed = "ATA";
        timeout = 15000u;
        break;
    case CALL_TXN_HANGUP:
        fixed = "AT+CHUP";
        timeout = 10000u;
        break;
    case CALL_TXN_WAIT_ANSWER:
    case CALL_TXN_SWAP:
    case CALL_TXN_HOLD:
        fixed = "AT+CHLD=2";
        break;
    case CALL_TXN_WAIT_REJECT:
        fixed = "AT+CHLD=0";
        break;
    case CALL_TXN_RELEASE_ACTIVE:
        fixed = "AT+CHLD=1";
        break;
    case CALL_TXN_RELEASE_LEG:
        if (target_id == 0u || target_id > MODEM_CALL_ID_MAX) {
            return false;
        }
        written = snprintf(out, out_cap, "AT+CHLD=1%u",
                           (unsigned)target_id);
        break;
    case CALL_TXN_NONE:
        return false;
    }

    if (fixed != NULL) {
        written = snprintf(out, out_cap, "%s", fixed);
    }
    if (written < 0 || (size_t)written >= out_cap) {
        return false;
    }
    *timeout_ms = timeout;
    return true;
}

bool telit_build_dtmf_command(char symbol, char *out, size_t out_cap,
                              uint32_t *timeout_ms) {
    if (out == NULL || out_cap == 0u || timeout_ms == NULL ||
        !((symbol >= '0' && symbol <= '9') || symbol == '*' ||
          symbol == '#')) {
        return false;
    }
    int written = snprintf(out, out_cap, "AT+VTS=%c", symbol);
    if (written < 0 || (size_t)written >= out_cap) {
        return false;
    }
    *timeout_ms = 1500u;
    return true;
}

static bool telit_forward_number_valid(const char *number) {
    if (number == NULL) {
        return false;
    }
    size_t length = 0u;
    while (length <= MODEM_PHONE_MAX && number[length] != '\0') {
        length++;
    }
    if (length == 0u || length > MODEM_PHONE_MAX ||
        (length == 1u && number[0] == '+')) {
        return false;
    }
    for (size_t i = 0u; i < length; i++) {
        if (number[i] == '+' && i == 0u) {
            continue;
        }
        if (number[i] < '0' || number[i] > '9') {
            return false;
        }
    }
    return true;
}

uint8_t telit_call_forward_step_count(
    const call_forward_request_t *request) {
    if (request == NULL ||
        (unsigned)request->reason >
            (unsigned)CALL_FORWARD_REASON_ALL_CONDITIONAL ||
        (unsigned)request->action >
            (unsigned)CALL_FORWARD_ACTION_ERASE) {
        return 0u;
    }
    if ((request->reason == CALL_FORWARD_REASON_ALL ||
         request->reason == CALL_FORWARD_REASON_ALL_CONDITIONAL) &&
        request->action == CALL_FORWARD_ACTION_QUERY) {
        return 0u;
    }

    if (request->action == CALL_FORWARD_ACTION_REGISTER) {
        if (!request->has_number ||
            !telit_forward_number_valid(request->number) ||
            (request->has_delay &&
             (request->delay_seconds < 5u ||
              request->delay_seconds > 30u ||
              (request->delay_seconds % 5u) != 0u ||
              (request->reason != CALL_FORWARD_REASON_NO_REPLY &&
               request->reason != CALL_FORWARD_REASON_ALL_CONDITIONAL)))) {
            return 0u;
        }
        if (request->has_delay) {
            /* Telit documents <time> only for reason 2 with cmd 1/2.
             * Register first, then set the no-reply timer with a separate
             * enable command. Reason 5 uses the timer of its reason-2
             * component, so it follows the same two-step sequence. */
            return 2u;
        }
    } else if (request->action == CALL_FORWARD_ACTION_ENABLE) {
        if ((request->has_number &&
             !telit_forward_number_valid(request->number)) ||
            (request->has_delay &&
             (request->delay_seconds < 5u ||
              request->delay_seconds > 30u ||
              (request->delay_seconds % 5u) != 0u ||
              request->reason != CALL_FORWARD_REASON_NO_REPLY))) {
            return 0u;
        }
    } else {
        if (request->has_number || request->has_delay) {
            return 0u;
        }
    }
    return 1u;
}

bool telit_build_call_forward_step(
    const call_forward_request_t *request, uint8_t step_index,
    char *out, size_t out_cap) {
    uint8_t step_count = telit_call_forward_step_count(request);
    if (out == NULL || out_cap == 0u || step_count == 0u ||
        step_index >= step_count) {
        return false;
    }

    if (request->reason == CALL_FORWARD_REASON_ALL &&
        request->action == CALL_FORWARD_ACTION_ERASE) {
        int length = snprintf(out, out_cap, "AT+CCFC=4,4");
        return length > 0 && (size_t)length < out_cap;
    }

    int length;
    if (step_count == 2u && step_index == 1u) {
        length = snprintf(out, out_cap, "AT+CCFC=2,1,,,1,%u",
                          (unsigned)request->delay_seconds);
    } else if (request->action == CALL_FORWARD_ACTION_ENABLE &&
               !request->has_number && request->has_delay) {
        length = snprintf(out, out_cap, "AT+CCFC=%u,1,,,1,%u",
                          (unsigned)request->reason,
                          (unsigned)request->delay_seconds);
    } else if (request->action == CALL_FORWARD_ACTION_REGISTER ||
               (request->action == CALL_FORWARD_ACTION_ENABLE &&
                request->has_number)) {
        unsigned number_type = request->number[0] == '+' ? 145u : 129u;
        if (request->has_delay &&
            request->action == CALL_FORWARD_ACTION_ENABLE) {
            length = snprintf(out, out_cap,
                              "AT+CCFC=%u,%u,\"%s\",%u,1,%u",
                              (unsigned)request->reason,
                              (unsigned)request->action, request->number,
                              number_type,
                              (unsigned)request->delay_seconds);
        } else {
            length = snprintf(out, out_cap,
                              "AT+CCFC=%u,%u,\"%s\",%u,1",
                              (unsigned)request->reason,
                              (unsigned)request->action, request->number,
                              number_type);
        }
    } else {
        length = snprintf(out, out_cap, "AT+CCFC=%u,%u,,,1",
                          (unsigned)request->reason,
                          (unsigned)request->action);
    }
    return length > 0 && (size_t)length < out_cap;
}

bool telit_parse_call_forward_row(const char *line,
                                         call_forward_row_t *out) {
    if (out == NULL) {
        return false;
    }
    telit_csv_view_t fields[7];
    size_t count = 0u;
    uint32_t status = 0u;
    uint32_t class_mask = 0u;
    if (!telit_view_split_prefixed(line, "+CCFC:", fields, 7u, &count) ||
        count < 2u ||
        !telit_view_parse_u32(fields[0], 1u, &status) ||
        !telit_view_parse_u32(fields[1], UINT8_MAX, &class_mask)) {
        return false;
    }

    call_forward_row_t parsed;
    memset(&parsed, 0, sizeof(parsed));
    parsed.active = status != 0u;
    parsed.class_mask = (uint8_t)class_mask;
    if (count > 2u) {
        uint32_t number_type = 0u;
        if (count < 4u || fields[2].length == 0u ||
            !telit_view_copy_exact(parsed.number, sizeof(parsed.number),
                                   fields[2]) ||
            !telit_forward_number_valid(parsed.number) ||
            !telit_view_parse_u32(fields[3], 255u, &number_type) ||
            (number_type != 129u && number_type != 145u) ||
            ((number_type == 145u) != (parsed.number[0] == '+'))) {
            return false;
        }
        parsed.has_number = true;
        parsed.number_type = (uint8_t)number_type;
    }
    if (count > 4u) {
        if (count != 7u || fields[4].length != 0u ||
            fields[5].length != 0u) {
            return false;
        }
        uint32_t delay = 0u;
        if (!telit_view_parse_u32(fields[6], 30u, &delay) || delay == 0u) {
            return false;
        }
        parsed.has_delay = true;
        parsed.delay_seconds = (uint8_t)delay;
    }
    *out = parsed;
    return true;
}

bool telit_parse_voice_mailbox_row(
    const char *line, modem_voice_mailbox_row_t *out) {
    if (out == NULL) {
        return false;
    }
    telit_csv_view_t fields[5];
    size_t count = 0u;
    uint32_t index = 0u;
    uint32_t number_type = 0u;
    if (!telit_view_split_prefixed(line, "#MBN:", fields, 5u, &count) ||
        count < 3u ||
        !telit_view_parse_u32(fields[0], UINT16_MAX, &index) ||
        !telit_view_parse_u32(fields[2], 255u, &number_type) ||
        (number_type != 129u && number_type != 145u)) {
        return false;
    }
    modem_voice_mailbox_row_t parsed;
    memset(&parsed, 0, sizeof(parsed));
    if (!telit_view_copy_exact(parsed.number, sizeof(parsed.number), fields[1]) ||
        !telit_forward_number_valid(parsed.number) ||
        ((number_type == 145u) != (parsed.number[0] == '+'))) {
        return false;
    }
    parsed.index = (uint16_t)index;
    parsed.number_type = (uint8_t)number_type;
    telit_csv_view_t mailbox_type = {0};
    if (count == 4u &&
        (telit_view_equals(fields[3], "VOICE") ||
         telit_view_equals(fields[3], "FAX") ||
         telit_view_equals(fields[3], "EMAIL") ||
         telit_view_equals(fields[3], "OTHER"))) {
        mailbox_type = fields[3];
    } else if (count == 5u &&
               (telit_view_equals(fields[4], "VOICE") ||
                telit_view_equals(fields[4], "FAX") ||
                telit_view_equals(fields[4], "EMAIL") ||
                telit_view_equals(fields[4], "OTHER"))) {
        mailbox_type = fields[4];
    } else if (count == 5u) {
        return false;
    }
    parsed.voice = mailbox_type.text == NULL ||
                   telit_view_equals(mailbox_type, "VOICE");
    *out = parsed;
    return true;
}

bool telit_parse_qss_query(const char *line,
                                  telit_qss_state_t *state) {
    if (state == NULL) {
        return false;
    }
    telit_csv_view_t fields[2];
    size_t count = 0u;
    uint32_t mode = 0u;
    uint32_t status = 0u;
    if (!telit_view_split_prefixed(line, "#QSS:", fields, 2u, &count) ||
        count != 2u ||
        !telit_view_parse_u32(fields[0], 2u, &mode) ||
        !telit_view_parse_u32(fields[1], 3u, &status)) {
        return false;
    }
    *state = (telit_qss_state_t){
        .mode = (uint8_t)mode,
        .status = (uint8_t)status,
    };
    return true;
}

bool telit_parse_qss_urc(const char *line,
                                uint8_t *status_out) {
    if (status_out == NULL) {
        return false;
    }
    telit_csv_view_t fields[1];
    size_t count = 0u;
    uint32_t status = 0u;
    if (!telit_view_split_prefixed(line, "#QSS:", fields, 1u, &count) ||
        count != 1u ||
        !telit_view_parse_u32(fields[0], 3u, &status)) {
        return false;
    }
    *status_out = (uint8_t)status;
    return true;
}

modem_sim_observation_t telit_parse_sim_observation(const char *line) {
    telit_csv_view_t fields[2];
    size_t count = 0u;
    if (!telit_view_split_prefixed(line, "#QSS:", fields, 2u, &count) ||
        (count != 1u && count != 2u)) {
        return MODEM_SIM_OBSERVATION_NONE;
    }

    uint32_t status = 0u;
    uint32_t mode = 0u;
    if ((count == 2u && !telit_view_parse_u32(fields[0], 2u, &mode)) ||
        !telit_view_parse_u32(fields[count - 1u], 3u, &status)) {
        return MODEM_SIM_OBSERVATION_NONE;
    }
    if (status == 0u) {
        return MODEM_SIM_OBSERVATION_ABSENT;
    }
    if (status == 3u) {
        return MODEM_SIM_OBSERVATION_READY;
    }
    return MODEM_SIM_OBSERVATION_PRESENT;
}

static bool telit_mwi_category(uint32_t indicator,
                               modem_message_waiting_category_t *out) {
    if (out == NULL) {
        return false;
    }
    switch (indicator) {
    case 1u:
        *out = MODEM_MESSAGE_WAITING_VOICE_LINE_1;
        return true;
    case 2u:
        *out = MODEM_MESSAGE_WAITING_VOICE_LINE_2;
        return true;
    case 3u:
        *out = MODEM_MESSAGE_WAITING_FAX;
        return true;
    case 4u:
        *out = MODEM_MESSAGE_WAITING_EMAIL;
        return true;
    case 5u:
        *out = MODEM_MESSAGE_WAITING_OTHER;
        return true;
    default:
        return false;
    }
}

static bool telit_apply_mwi(telit_mwi_state_t *state, uint32_t status,
                            bool indicator_present, uint32_t indicator,
                            bool count_present, uint32_t message_count) {
    if (state == NULL || status > 1u ||
        (indicator_present && (indicator < 1u || indicator > 5u)) ||
        (!indicator_present && status != 0u) ||
        (count_present && (!indicator_present || message_count > UINT16_MAX))) {
        return false;
    }

    if (indicator_present) {
        if (!telit_mwi_category(indicator, &state->category)) {
            return false;
        }
    } else {
        state->category = MODEM_MESSAGE_WAITING_ALL;
    }
    state->active = status == 1u;
    state->count = state->active && count_present
        ? (uint16_t)message_count
        : 0u;
    return true;
}

bool telit_parse_mwi_urc(const char *line,
                                telit_mwi_state_t *state) {
    telit_csv_view_t fields[3];
    size_t count = 0u;
    uint32_t status = 0u;
    uint32_t indicator = 0u;
    uint32_t message_count = 0u;
    telit_mwi_state_t parsed = {0};
    if (state == NULL ||
        !telit_view_split_prefixed(line, "#MWI:", fields, 3u, &count) ||
        count < 1u || count > 3u ||
        !telit_view_parse_u32(fields[0], 1u, &status) ||
        (count >= 2u &&
         !telit_view_parse_u32(fields[1], 5u, &indicator)) ||
        (count >= 3u &&
         !telit_view_parse_u32(fields[2], UINT16_MAX, &message_count))) {
        return false;
    }
    if (!telit_apply_mwi(&parsed, status, count >= 2u, indicator,
                         count >= 3u, message_count)) {
        return false;
    }
    *state = parsed;
    return true;
}

bool telit_parse_mwi_query(const char *line,
                                  telit_mwi_state_t *state) {
    if (state == NULL) {
        return false;
    }
    telit_csv_view_t fields[4];
    size_t count = 0u;
    uint32_t enable = 0u;
    uint32_t status = 0u;
    uint32_t indicator = 0u;
    uint32_t message_count = 0u;
    telit_mwi_state_t parsed = {0};
    if (!telit_view_split_prefixed(line, "#MWI:", fields, 4u, &count) ||
        count < 2u || count > 4u ||
        !telit_view_parse_u32(fields[0], 1u, &enable) ||
        !telit_view_parse_u32(fields[1], 1u, &status) ||
        (count >= 3u &&
         !telit_view_parse_u32(fields[2], 5u, &indicator)) ||
        (count >= 4u &&
         !telit_view_parse_u32(fields[3], UINT16_MAX, &message_count)) ||
        !telit_apply_mwi(&parsed, status, count >= 3u, indicator,
                         count >= 4u, message_count)) {
        return false;
    }
    parsed.enabled = (uint8_t)enable;
    *state = parsed;
    return true;
}

modem_message_waiting_row_result_t telit_parse_message_waiting_row(
    const char *line, modem_aux_event_t *out) {
    if (out == NULL) {
        return MODEM_MESSAGE_WAITING_ROW_INVALID;
    }
    memset(out, 0, sizeof(*out));
    telit_csv_view_t fields[3];
    size_t field_count = 0u;
    uint32_t first = 0u;
    uint32_t second = 0u;
    uint32_t third = 0u;
    bool split = telit_view_split_prefixed(line, "#MWI:", fields, 3u,
                                           &field_count);
    /* Read rows and URCs share a prefix and can have identical bytes. Two
     * fields "1,1" are either enabled+set-with-missing-indicator or
     * set+voice. Three fields "<enable>,1,<1..5>" are either an active read
     * row whose count is omitted or a voice URC whose count happens to be a
     * category number. Neither interpretation is safe inside AT#MWI?. Preserve
     * the prior category until an unambiguous four-field row/readback arrives.
     * A count outside 1..5 (for example "1,1,7") cannot be a read indicator
     * and remains an unambiguous URC. */
    bool ambiguous = split && field_count >= 2u &&
        telit_view_parse_u32(fields[0], 1u, &first) &&
        telit_view_parse_u32(fields[1], 1u, &second) && second == 1u &&
        ((field_count == 2u) ||
         (field_count == 3u &&
          telit_view_parse_u32(fields[2], 5u, &third) && third >= 1u));
    if (ambiguous) {
        out->kind = MODEM_AUX_EVENT_MESSAGE_WAITING;
        out->message_waiting_uncertain_mask =
            modem_message_waiting_category_bit(
                MODEM_MESSAGE_WAITING_VOICE_LINE_1);
        if (field_count == 3u) {
            modem_message_waiting_category_t query_category;
            if (!telit_mwi_category(third, &query_category)) {
                return MODEM_MESSAGE_WAITING_ROW_INVALID;
            }
            out->message_waiting_uncertain_mask |=
                modem_message_waiting_category_bit(query_category);
        }
        return MODEM_MESSAGE_WAITING_ROW_AMBIGUOUS;
    }

    telit_mwi_state_t state;
    if (!telit_parse_mwi_query(line, &state)) {
        return MODEM_MESSAGE_WAITING_ROW_INVALID;
    }
    out->kind = MODEM_AUX_EVENT_MESSAGE_WAITING;
    out->message_waiting_category = state.category;
    out->active = state.active;
    out->count = state.count;
    return MODEM_MESSAGE_WAITING_ROW_VALID;
}

bool telit_parse_temperature(const char *line,
                                    telit_temperature_t *temperature_out) {
    if (temperature_out == NULL) {
        return false;
    }
    telit_csv_view_t fields[2];
    size_t count = 0u;
    int32_t level = 0;
    int32_t temperature = 0;
    if (!telit_view_split_prefixed(line, "#TEMPMEAS:", fields, 2u, &count) ||
        count != 2u ||
        !telit_view_parse_i32(fields[0], -2, 2, &level) ||
        !telit_view_parse_i32(fields[1], -100, 200, &temperature)) {
        return false;
    }
    *temperature_out = (telit_temperature_t){
        .level = (int8_t)level,
        .celsius = (int16_t)temperature,
    };
    return true;
}

bool telit_parse_ismscfg(const char *line,
                                uint8_t *mode_out) {
    if (mode_out == NULL) {
        return false;
    }
    telit_csv_view_t fields[1];
    size_t count = 0u;
    uint32_t mode = 0u;
    if (!telit_view_split_prefixed(line, "#ISMSCFG:", fields, 1u, &count) ||
        count != 1u || !telit_view_parse_u32(fields[0], 1u, &mode)) {
        return false;
    }
    *mode_out = (uint8_t)mode;
    return true;
}

bool telit_parse_fwswitch(const char *line,
                                 telit_fwswitch_t *state) {
    if (state == NULL) {
        return false;
    }
    telit_csv_view_t fields[3];
    size_t count = 0u;
    uint32_t image = 0u;
    uint32_t storage = 0u;
    uint32_t restore = 0u;
    if (!telit_view_split_prefixed(line, "#FWSWITCH:", fields, 3u, &count) ||
        count != 3u ||
        !telit_view_parse_u32(fields[0], UINT16_MAX, &image) ||
        !telit_view_parse_u32(fields[1], 1u, &storage) ||
        !telit_view_parse_u32(fields[2], 1u, &restore)) {
        return false;
    }
    *state = (telit_fwswitch_t){
        .image = (uint16_t)image,
        .storage = (uint8_t)storage,
        .restore = (uint8_t)restore,
    };
    return true;
}

bool telit_parse_fwautosim(const char *line,
                                  uint8_t *mode_out) {
    if (mode_out == NULL) {
        return false;
    }
    telit_csv_view_t fields[1];
    size_t count = 0u;
    uint32_t mode = 0u;
    if (!telit_view_split_prefixed(line, "#FWAUTOSIM:", fields, 1u, &count) ||
        count != 1u || !telit_view_parse_u32(fields[0], 3u, &mode)) {
        return false;
    }
    *mode_out = (uint8_t)mode;
    return true;
}

static bool telit_parse_cff(const char *line, modem_aux_event_t *out) {
    telit_csv_view_t fields[3];
    size_t count = 0u;
    uint32_t enable = 0u;
    uint32_t status = 0u;
    if (!telit_view_split_prefixed(line, "#CFF:", fields, 3u, &count) ||
        count < 1u || !telit_view_parse_u32(fields[0], 1u, &enable)) {
        return false;
    }
    if (count == 1u) {
        return true;
    }
    if (count != 3u || !telit_view_parse_u32(fields[1], 1u, &status) ||
        (fields[2].length != 0u &&
         !telit_view_copy_exact(out->number, sizeof(out->number), fields[2]))) {
        return false;
    }
    out->kind = MODEM_AUX_EVENT_CFU_STATE;
    out->active = status != 0u;
    return true;
}

static bool telit_parse_css_notification(const char *line,
                                         modem_aux_event_t *out) {
    telit_csv_view_t fields[1];
    size_t count = 0u;
    uint32_t code = 0u;
    if (telit_view_split_prefixed(line, "+CSSI:", fields, 1u, &count)) {
        if (count != 1u || !telit_view_parse_u32(fields[0], 6u, &code) ||
            code == 4u) {
            return false;
        }
        if (code == 0u) {
            out->kind = MODEM_AUX_EVENT_CFU_STATE;
            out->active = true;
        }
        return true;
    }
    if (!telit_view_split_prefixed(line, "+CSSU:", fields, 1u, &count) ||
        count != 1u || !telit_view_parse_u32(fields[0], 10u, &code) ||
        (code != 0u && code != 2u && code != 3u && code != 5u &&
         code != 10u)) {
        return false;
    }
    if (code == 0u || code == 10u) {
        /* 0 is the first forwarded MT setup; 10 is an additional forwarded
         * incoming call (the call-waiting variant). Both describe the current
         * incoming leg and select Nokia's diverted presentation. */
        out->kind = MODEM_AUX_EVENT_INCOMING_DIVERTED;
        out->active = true;
    }
    return true;
}

bool telit_parse_aux_urc(const char *line, modem_aux_event_t *out) {
    if (out == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    if (telit_starts_with(line, "#QSS:")) {
        uint8_t status = 0u;
        return telit_parse_qss_urc(line, &status);
    }
    if (telit_starts_with(line, "#MWI:")) {
        telit_mwi_state_t state = {0};
        if (!telit_parse_mwi_urc(line, &state)) {
            return false;
        }
        out->kind = MODEM_AUX_EVENT_MESSAGE_WAITING;
        out->message_waiting_category = state.category;
        out->active = state.active;
        out->count = state.count;
        return true;
    }
    if (telit_starts_with(line, "#CFF:")) {
        return telit_parse_cff(line, out);
    }
    if (telit_starts_with(line, "+CSSI:") ||
        telit_starts_with(line, "+CSSU:")) {
        return telit_parse_css_notification(line, out);
    }
    if (telit_starts_with(line, "#TEMPMEAS:")) {
        telit_temperature_t temperature;
        return telit_parse_temperature(line, &temperature);
    }
    if (telit_starts_with(line, "$QCMTI:")) {
        /* $QCMTI: "<mem>",<index>: the Qualcomm stack filed a message in its
         * CDMA store, which this image cannot read in any mode (design doc,
         * "Problem"). Only emitted when direct delivery is not in effect. */
        telit_csv_view_t fields[2];
        size_t count = 0u;
        if (!telit_view_split_prefixed(line, "$QCMTI:", fields, 2u, &count) ||
            count != 2u || !telit_view_digits_only(fields[1], 1u, 5u)) {
            return false;
        }
        out->kind = MODEM_AUX_EVENT_MESSAGE_STORED_UNREADABLE;
        return true;
    }
    return false;
}

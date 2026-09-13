#include "services/call_forward_mmi.h"

#include <stddef.h>
#include <string.h>

static bool number_valid(const char *text, size_t length) {
    if (text == NULL || length == 0u || length > MODEM_PHONE_MAX) {
        return false;
    }
    for (size_t i = 0u; i < length; i++) {
        if (text[i] == '+' && i == 0u) {
            continue;
        }
        if (text[i] < '0' || text[i] > '9') {
            return false;
        }
    }
    return !(length == 1u && text[0] == '+');
}

static bool parse_decimal(const char *text, size_t length,
                          unsigned *value_out) {
    if (text == NULL || length == 0u || value_out == NULL) {
        return false;
    }
    unsigned value = 0u;
    for (size_t i = 0u; i < length; i++) {
        if (text[i] < '0' || text[i] > '9') {
            return false;
        }
        unsigned digit = (unsigned)(text[i] - '0');
        if (value > (255u - digit) / 10u) {
            return false;
        }
        value = value * 10u + digit;
    }
    *value_out = value;
    return true;
}

static bool service_reason(const char *text, size_t length,
                           call_forward_reason_t *reason_out) {
    if (length == 2u && memcmp(text, "21", 2u) == 0) {
        *reason_out = CALL_FORWARD_REASON_UNCONDITIONAL;
    } else if (length == 2u && memcmp(text, "67", 2u) == 0) {
        *reason_out = CALL_FORWARD_REASON_BUSY;
    } else if (length == 2u && memcmp(text, "61", 2u) == 0) {
        *reason_out = CALL_FORWARD_REASON_NO_REPLY;
    } else if (length == 2u && memcmp(text, "62", 2u) == 0) {
        *reason_out = CALL_FORWARD_REASON_NOT_REACHABLE;
    } else if (length == 3u && memcmp(text, "004", 3u) == 0) {
        *reason_out = CALL_FORWARD_REASON_ALL_CONDITIONAL;
    } else if (length == 3u && memcmp(text, "002", 3u) == 0) {
        *reason_out = CALL_FORWARD_REASON_ALL;
    } else {
        return false;
    }
    return true;
}

static bool service_prefix_recognized(const char *text, size_t length) {
    static const char *const codes[] = {"21", "67", "61", "62", "004", "002"};
    for (size_t i = 0u; i < sizeof(codes) / sizeof(codes[0]); i++) {
        size_t code_length = strlen(codes[i]);
        if (length < code_length ||
            memcmp(text, codes[i], code_length) != 0) {
            continue;
        }
        if (length == code_length) {
            return true;
        }
        /* A longer decimal service code (for example 210) is unrelated. Once
         * a known code is followed by MMI punctuation or any non-digit, however,
         * it is unmistakably a malformed call-forwarding request and must not
         * fall through to ordinary dialing. */
        char next = text[code_length];
        return next < '0' || next > '9';
    }
    return false;
}

call_forward_mmi_result_t call_forward_mmi_parse(
    const char *text, call_forward_request_t *out) {
    if (text == NULL || out == NULL) {
        return CALL_FORWARD_MMI_NOT_MATCHED;
    }
    size_t length = strlen(text);
    if (length < 4u || text[length - 1u] != '#') {
        return CALL_FORWARD_MMI_NOT_MATCHED;
    }

    call_forward_request_t parsed;
    memset(&parsed, 0, sizeof(parsed));
    size_t prefix_length;
    if (text[0] == '*' && text[1] == '*') {
        parsed.action = CALL_FORWARD_ACTION_REGISTER;
        prefix_length = 2u;
    } else if (text[0] == '#' && text[1] == '#') {
        parsed.action = CALL_FORWARD_ACTION_ERASE;
        prefix_length = 2u;
    } else if (text[0] == '*' && text[1] == '#') {
        parsed.action = CALL_FORWARD_ACTION_QUERY;
        prefix_length = 2u;
    } else if (text[0] == '*') {
        parsed.action = CALL_FORWARD_ACTION_ENABLE;
        prefix_length = 1u;
    } else if (text[0] == '#') {
        parsed.action = CALL_FORWARD_ACTION_DISABLE;
        prefix_length = 1u;
    } else {
        return CALL_FORWARD_MMI_NOT_MATCHED;
    }

    size_t service_end = prefix_length;
    while (service_end + 1u < length && text[service_end] != '*') {
        service_end++;
    }
    if (!service_reason(text + prefix_length, service_end - prefix_length,
                        &parsed.reason)) {
        return service_prefix_recognized(
                   text + prefix_length, service_end - prefix_length)
                   ? CALL_FORWARD_MMI_INVALID
                   : CALL_FORWARD_MMI_NOT_MATCHED;
    }

    size_t body_end = length - 1u;
    const char *args[3] = {0};
    size_t arg_lengths[3] = {0u};
    size_t arg_count = 0u;
    size_t cursor = service_end;
    while (cursor < body_end) {
        if (text[cursor] != '*' || arg_count >= 3u) {
            return CALL_FORWARD_MMI_INVALID;
        }
        cursor++;
        size_t start = cursor;
        while (cursor < body_end && text[cursor] != '*') {
            cursor++;
        }
        args[arg_count] = text + start;
        arg_lengths[arg_count] = cursor - start;
        arg_count++;
    }

    if ((parsed.reason == CALL_FORWARD_REASON_ALL ||
         parsed.reason == CALL_FORWARD_REASON_ALL_CONDITIONAL) &&
        parsed.action == CALL_FORWARD_ACTION_QUERY) {
        /* Telit implements reasons 4/5 for mutation only, matching 27.007. */
        return CALL_FORWARD_MMI_INVALID;
    }

    bool number_allowed = parsed.action == CALL_FORWARD_ACTION_REGISTER ||
                          parsed.action == CALL_FORWARD_ACTION_ENABLE;
    if (!number_allowed) {
        if (arg_count == 0u) {
            *out = parsed;
            return CALL_FORWARD_MMI_VALID;
        }
        /* SIA is not meaningful for these actions. A voice basic-service
         * qualifier is accepted only in its real SIB position: **11. */
        if (arg_count != 2u || arg_lengths[0] != 0u) {
            return CALL_FORWARD_MMI_INVALID;
        }
        unsigned basic_service = 0u;
        if (!parse_decimal(args[1], arg_lengths[1], &basic_service) ||
            basic_service != 11u) {
            return CALL_FORWARD_MMI_INVALID;
        }
        *out = parsed;
        return CALL_FORWARD_MMI_VALID;
    }

    if (arg_count == 0u) {
        if (parsed.action != CALL_FORWARD_ACTION_ENABLE) {
            return CALL_FORWARD_MMI_INVALID;
        }
        *out = parsed;
        return CALL_FORWARD_MMI_VALID;
    }
    if (arg_lengths[0] == 0u) {
        /* Activation can reuse the network's registered destination. The
         * standard voice-qualified form is *SC**11#; no-reply may also carry
         * SIC to update the timer without resending the number. */
        if (parsed.action != CALL_FORWARD_ACTION_ENABLE ||
            (arg_count != 2u && arg_count != 3u)) {
            return CALL_FORWARD_MMI_INVALID;
        }
        if (arg_lengths[1] != 0u) {
            unsigned basic_service = 0u;
            if (!parse_decimal(args[1], arg_lengths[1], &basic_service) ||
                basic_service != 11u) {
                return CALL_FORWARD_MMI_INVALID;
            }
        } else if (arg_count != 3u) {
            return CALL_FORWARD_MMI_INVALID;
        }
        if (arg_count == 3u) {
            unsigned delay = 0u;
            if (parsed.reason != CALL_FORWARD_REASON_NO_REPLY ||
                !parse_decimal(args[2], arg_lengths[2], &delay) ||
                delay < 5u || delay > 30u || (delay % 5u) != 0u) {
                return CALL_FORWARD_MMI_INVALID;
            }
            parsed.has_delay = true;
            parsed.delay_seconds = (uint8_t)delay;
        }
        *out = parsed;
        return CALL_FORWARD_MMI_VALID;
    }
    if (!number_valid(args[0], arg_lengths[0])) {
        return CALL_FORWARD_MMI_INVALID;
    }
    memcpy(parsed.number, args[0], arg_lengths[0]);
    parsed.number[arg_lengths[0]] = '\0';
    parsed.has_number = true;

    if (arg_count >= 2u && arg_lengths[1] != 0u) {
        unsigned basic_service = 0u;
        if (!parse_decimal(args[1], arg_lengths[1], &basic_service) ||
            basic_service != 11u) {
            return CALL_FORWARD_MMI_INVALID;
        }
    } else if (arg_count == 2u) {
        /* A second separator is meaningful only when it introduces the
         * Nokia no-reply delay field (for example **61*number**20#). */
        return CALL_FORWARD_MMI_INVALID;
    }
    if (arg_count == 3u) {
        unsigned delay = 0u;
        if (!parse_decimal(args[2], arg_lengths[2], &delay) || delay < 5u ||
            delay > 30u || (delay % 5u) != 0u ||
            (parsed.reason != CALL_FORWARD_REASON_NO_REPLY &&
             parsed.reason != CALL_FORWARD_REASON_ALL_CONDITIONAL) ||
            (parsed.reason == CALL_FORWARD_REASON_ALL_CONDITIONAL &&
             parsed.action != CALL_FORWARD_ACTION_REGISTER)) {
            return CALL_FORWARD_MMI_INVALID;
        }
        parsed.has_delay = true;
        parsed.delay_seconds = (uint8_t)delay;
    }

    *out = parsed;
    return CALL_FORWARD_MMI_VALID;
}

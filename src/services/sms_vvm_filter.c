#include "services/sms_vvm_filter.h"

#include <string.h>

typedef struct {
    const uint8_t *data;
    size_t len;
} byte_view_t;

static bool view_starts_with(byte_view_t view, const char *literal) {
    size_t len = strlen(literal);
    return view.len >= len && memcmp(view.data, literal, len) == 0;
}

static bool view_equals(byte_view_t view, const char *literal) {
    size_t len = strlen(literal);
    return view.len == len && memcmp(view.data, literal, len) == 0;
}

static size_t view_find(byte_view_t view, uint8_t needle, size_t start) {
    for (size_t i = start; i < view.len; i++) {
        if (view.data[i] == needle) {
            return i;
        }
    }
    return SIZE_MAX;
}

static byte_view_t view_slice(byte_view_t view, size_t start, size_t end) {
    byte_view_t result = {0};
    if (start <= end && end <= view.len) {
        result.data = view.data + start;
        result.len = end - start;
    }
    return result;
}

static bool byte_is_field_text(uint8_t value) {
    return value >= 0x20u && value <= 0x7eu;
}

static bool fields_have_key_policy(byte_view_t fields, uint8_t separator,
                                   const char *required_key,
                                   bool require_value) {
    bool have_field = false;
    bool have_required = false;
    size_t pos = 0u;

    while (pos < fields.len) {
        size_t end = view_find(fields, separator, pos);
        if (end == SIZE_MAX) {
            end = fields.len;
        }
        if (end == pos) {
            pos = end + (end < fields.len ? 1u : 0u);
            continue;
        }

        byte_view_t field = view_slice(fields, pos, end);
        size_t equals = view_find(field, '=', 0u);
        if (equals == SIZE_MAX || equals == 0u) {
            return false;
        }
        for (size_t i = 0u; i < field.len; i++) {
            if (!byte_is_field_text(field.data[i])) {
                return false;
            }
        }

        byte_view_t key = view_slice(field, 0u, equals);
        have_field = true;
        if (view_equals(key, required_key) &&
            (!require_value || equals + 1u < field.len)) {
            have_required = true;
        }
        pos = end + (end < fields.len ? 1u : 0u);
    }
    return have_field && have_required;
}

static bool fields_have_key(byte_view_t fields, uint8_t separator,
                            const char *required_key) {
    return fields_have_key_policy(fields, separator, required_key, true);
}

static bool fields_have_key_allow_empty(byte_view_t fields, uint8_t separator,
                                        const char *required_key) {
    return fields_have_key_policy(fields, separator, required_key, false);
}

static bool omtp_client_prefix_is_vvm(byte_view_t prefix) {
    if (prefix.len < 5u || prefix.len > 32u || prefix.data[0] != '/' ||
        prefix.data[1] != '/' ||
        memcmp(prefix.data + prefix.len - 3u, "VVM", 3u) != 0) {
        return false;
    }
    for (size_t i = 2u; i < prefix.len; i++) {
        uint8_t ch = prefix.data[i];
        bool allowed = (ch >= 'A' && ch <= 'Z') ||
                       (ch >= '0' && ch <= '9') || ch == '_' || ch == '-';
        if (!allowed) {
            return false;
        }
    }
    return true;
}

static bool omtp_payload_is_recognized(byte_view_t payload) {
    if (!view_starts_with(payload, "//")) {
        return false;
    }
    size_t prefix_end = view_find(payload, ':', 2u);
    if (prefix_end == SIZE_MAX ||
        !omtp_client_prefix_is_vvm(view_slice(payload, 0u, prefix_end))) {
        return false;
    }
    size_t event_end = view_find(payload, ':', prefix_end + 1u);
    if (event_end == SIZE_MAX || event_end + 1u > payload.len) {
        return false;
    }

    byte_view_t event = view_slice(payload, prefix_end + 1u, event_end);
    byte_view_t fields = view_slice(payload, event_end + 1u, payload.len);
    if (view_equals(event, "STATUS")) {
        return fields_have_key(fields, ';', "st") ||
               fields_have_key(fields, ';', "rc");
    }
    if (view_equals(event, "SYNC")) {
        return fields_have_key(fields, ';', "ev");
    }
    return false;
}

static bool legacy_port(uint16_t port) {
    return port == SMS_VVM_LEGACY_STATUS_PORT ||
           port == SMS_VVM_LEGACY_SYNC_PORT;
}

static bool legacy_payload_is_recognized(uint16_t port, byte_view_t payload) {
    if (!legacy_port(port)) {
        return false;
    }
    static const char state_prefix[] = "STATE?";
    static const char update_prefix[] = "MBOXUPDATE?";
    static const char unknown_prefix[] = "UNRECOGNIZED?";
    static const char unknown_uk_prefix[] = "UNRECOGNISED?";

    if (view_starts_with(payload, state_prefix)) {
        return fields_have_key(
            view_slice(payload, sizeof(state_prefix) - 1u, payload.len),
            ';', "state");
    }
    if (view_starts_with(payload, update_prefix)) {
        return fields_have_key(
            view_slice(payload, sizeof(update_prefix) - 1u, payload.len),
            ';', "m");
    }
    if (view_starts_with(payload, unknown_prefix)) {
        return fields_have_key(
            view_slice(payload, sizeof(unknown_prefix) - 1u, payload.len),
            ';', "cmd");
    }
    if (view_starts_with(payload, unknown_uk_prefix)) {
        return fields_have_key(
            view_slice(payload, sizeof(unknown_uk_prefix) - 1u, payload.len),
            ';', "cmd");
    }
    return false;
}

static bool legacy_url_authority_is_valid(byte_view_t authority) {
    size_t colon = view_find(authority, ':', 0u);
    if (colon == SIZE_MAX || colon == 0u || colon + 1u >= authority.len ||
        view_find(authority, ':', colon + 1u) != SIZE_MAX) {
        return false;
    }

    byte_view_t host = view_slice(authority, 0u, colon);
    bool label_has_alnum = false;
    bool last_was_hyphen = false;
    for (size_t i = 0u; i < host.len; i++) {
        uint8_t ch = host.data[i];
        bool alnum = (ch >= 'a' && ch <= 'z') ||
                     (ch >= 'A' && ch <= 'Z') ||
                     (ch >= '0' && ch <= '9');
        if (alnum) {
            label_has_alnum = true;
            last_was_hyphen = false;
        } else if (ch == '-') {
            if (!label_has_alnum) {
                return false;
            }
            last_was_hyphen = true;
        } else if (ch == '.') {
            if (!label_has_alnum || last_was_hyphen) {
                return false;
            }
            label_has_alnum = false;
            last_was_hyphen = false;
        } else {
            return false;
        }
    }
    if (!label_has_alnum || last_was_hyphen) {
        return false;
    }

    byte_view_t server_port = view_slice(authority, colon + 1u, authority.len);
    uint32_t value = 0u;
    for (size_t i = 0u; i < server_port.len; i++) {
        uint8_t ch = server_port.data[i];
        if (ch < '0' || ch > '9') {
            return false;
        }
        uint32_t digit = (uint32_t)(ch - '0');
        if (value > (UINT16_MAX - digit) / 10u) {
            return false;
        }
        value = value * 10u + digit;
    }
    return value != 0u;
}

static bool legacy_url_payload_is_recognized(uint16_t port,
                                             byte_view_t payload) {
    if (port != SMS_VVM_LEGACY_SYNC_PORT) {
        return false;
    }
    size_t query = view_find(payload, '?', 0u);
    if (query == SIZE_MAX || query == 0u || query + 1u >= payload.len ||
        view_find(payload, '?', query + 1u) != SIZE_MAX ||
        !legacy_url_authority_is_valid(view_slice(payload, 0u, query))) {
        return false;
    }

    /* Legacy URL notifications are not OMTP, but their stable envelope is
     * operator-independent: format/version, mailbox, password slot, service
     * port, and opaque notification token. Password may legitimately be empty. */
    byte_view_t fields = view_slice(payload, query + 1u, payload.len);
    return fields_have_key(fields, '&', "f") &&
           fields_have_key(fields, '&', "v") &&
           fields_have_key(fields, '&', "m") &&
           fields_have_key_allow_empty(fields, '&', "p") &&
           fields_have_key(fields, '&', "s") &&
           fields_have_key(fields, '&', "t");
}

bool sms_vvm_control_payload_is_recognized(bool has_ports,
                                           uint16_t destination_port,
                                           const uint8_t *payload,
                                           size_t payload_len) {
    if (!has_ports || payload == NULL || payload_len == 0u) {
        return false;
    }
    while (payload_len > 0u &&
           (payload[payload_len - 1u] == '\0' ||
            payload[payload_len - 1u] == '\r' ||
            payload[payload_len - 1u] == '\n')) {
        payload_len--;
    }
    if (payload_len == 0u) {
        return false;
    }

    byte_view_t view = {payload, payload_len};
    return omtp_payload_is_recognized(view) ||
           legacy_payload_is_recognized(destination_port, view) ||
           legacy_url_payload_is_recognized(destination_port, view);
}

#include "services/feature_gates.h"

static bool s_feature_visible[FEATURE_GATE_COUNT];
static bool s_feature_initialized;

static void ensure_initialized(void) {
    if (!s_feature_initialized) {
        feature_gates_reset_defaults();
    }
}

void feature_gates_reset_defaults(void) {
    for (uint8_t i = 0; i < FEATURE_GATE_COUNT; i++) {
        s_feature_visible[i] = true;
    }
    s_feature_initialized = true;
}

bool feature_gate_visible(feature_gate_id_t gate) {
    ensure_initialized();
    if (gate >= FEATURE_GATE_COUNT) {
        return true;
    }
    return s_feature_visible[gate];
}

void feature_gate_set_visible(feature_gate_id_t gate, bool visible) {
    ensure_initialized();
    if (gate < FEATURE_GATE_COUNT) {
        s_feature_visible[gate] = visible;
    }
}

const char *feature_gate_name(feature_gate_id_t gate) {
    static const char *const NAMES[] = {
        "net_monitor",
        "call_divert",
        "profiles",
        "phonebook_service_nos",
        "phonebook_info_numbers",
        "tones_vibrating_alert",
        "phone_settings_lights",
        "call_settings_automatic_answer",
        "call_settings_phone_line_in_use",
        "security_phone_line_change",
        "messages_info_service",
        "messages_voice_mailbox_number",
        "games_extra_rows",
    };
    if (gate >= FEATURE_GATE_COUNT) {
        return "unknown";
    }
    return NAMES[gate];
}

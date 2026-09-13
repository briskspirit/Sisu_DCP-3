#ifndef FEATURE_GATES_H
#define FEATURE_GATES_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    FEATURE_GATE_NET_MONITOR = 0,
    FEATURE_GATE_CALL_DIVERT,
    FEATURE_GATE_PROFILES,
    FEATURE_GATE_PHONEBOOK_SERVICE_NOS,
    FEATURE_GATE_PHONEBOOK_INFO_NUMBERS,
    FEATURE_GATE_TONES_VIBRATING_ALERT,
    FEATURE_GATE_PHONE_SETTINGS_LIGHTS,
    FEATURE_GATE_CALL_SETTINGS_AUTOMATIC_ANSWER,
    FEATURE_GATE_CALL_SETTINGS_PHONE_LINE_IN_USE,
    FEATURE_GATE_SECURITY_PHONE_LINE_CHANGE,
    FEATURE_GATE_MESSAGES_INFO_SERVICE,
    FEATURE_GATE_MESSAGES_VOICE_MAILBOX_NUMBER,
    /* SET.8: EEPROM config 0x21 (0x0029a24e -> 0x002b47a0(0x21)) — true
     * masks 5 games (0x001f), false masks 3 (0x0007: Rotation/Snake/Memory). */
    FEATURE_GATE_GAMES_EXTRA_ROWS,
    FEATURE_GATE_COUNT,
} feature_gate_id_t;

void feature_gates_reset_defaults(void);
bool feature_gate_visible(feature_gate_id_t gate);
void feature_gate_set_visible(feature_gate_id_t gate, bool visible);
const char *feature_gate_name(feature_gate_id_t gate);

#endif

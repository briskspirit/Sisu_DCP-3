#include "apps/net_monitor_logic.h"

#include "services/input_keys.h"

#define LOCAL_CAP NETMON_CAP_REVB2
#define MODEM_CAP (NETMON_CAP_REVB2 | NETMON_CAP_TELIT | NETMON_CAP_MODEM)
#define TUNER_CAP (MODEM_CAP | NETMON_CAP_TUNER)
#define VOICE_CAP (MODEM_CAP | NETMON_CAP_VOICE)
#define CONTROL_CAP (LOCAL_CAP | NETMON_CAP_LOCAL_CONTROLS)
#define RUNTIME_EXT_CAP (LOCAL_CAP | NETMON_CAP_RUNTIME_EXT)
#define MODEM_RUNTIME_EXT_CAP (MODEM_CAP | NETMON_CAP_RUNTIME_EXT)
#define RADIO_CONTROL_CAP (MODEM_CAP | NETMON_CAP_RADIO_CONTROL)
#define RF_MAINTENANCE_CAP \
    (TUNER_CAP | NETMON_CAP_RADIO_CONTROL | NETMON_CAP_RF_MAINTENANCE)

#define RO_LOCAL(id_, category_) \
    {id_, category_, LOCAL_CAP, NETMON_PROVIDER_LOCAL, NETMON_REFRESH_NONE, \
     0u, 0u, NETMON_PAGE_READ_ONLY | NETMON_PAGE_CALL_VISIBLE, 0u, \
     NETMON_ACTION_NONE}
#define RO_LOCAL_CALL(id_) \
    {id_, NETMON_CATEGORY_AUDIO, LOCAL_CAP, NETMON_PROVIDER_LOCAL, \
     NETMON_REFRESH_NONE, 0u, 0u, \
     NETMON_PAGE_READ_ONLY | NETMON_PAGE_CALL_VISIBLE, 0u, NETMON_ACTION_NONE}
#define RO_CACHE(id_, category_) \
    {id_, category_, MODEM_CAP, NETMON_PROVIDER_MODEM_CACHE, \
     NETMON_REFRESH_NONE, 0u, 0u, \
     NETMON_PAGE_READ_ONLY | NETMON_PAGE_CALL_VISIBLE, 0u, \
     NETMON_ACTION_NONE}
#define RO_QUERY(id_, category_, group_, period_, stale_) \
    {id_, category_, MODEM_CAP, NETMON_PROVIDER_MODEM_QUERY, group_, \
     period_, stale_, NETMON_PAGE_READ_ONLY | NETMON_PAGE_CALL_VISIBLE, 0u, \
     NETMON_ACTION_NONE}
#define EDIT_LOCAL(id_, keys_, action_) \
    {id_, NETMON_CATEGORY_CONTROL, CONTROL_CAP, NETMON_PROVIDER_LOCAL, \
     NETMON_REFRESH_NONE, 0u, 0u, \
     NETMON_PAGE_EDITABLE | NETMON_PAGE_STANDBY_ONLY, keys_, action_}
#define EDIT_MODEM_CACHE(id_, caps_, keys_, action_) \
    {id_, NETMON_CATEGORY_CONTROL, caps_, NETMON_PROVIDER_MODEM_CACHE, \
     NETMON_REFRESH_NONE, \
     0u, 0u, NETMON_PAGE_EDITABLE | NETMON_PAGE_STANDBY_ONLY, keys_, action_}

static const netmon_page_descriptor_t NETMON_PAGES[] = {
    RO_QUERY(1u, NETMON_CATEGORY_RADIO, NETMON_REFRESH_SERVING, 2000u, 6000u),
    RO_QUERY(2u, NETMON_CATEGORY_RADIO, NETMON_REFRESH_SERVING, 2000u, 6000u),
    RO_QUERY(3u, NETMON_CATEGORY_RADIO, NETMON_REFRESH_SERVING, 5000u, 15000u),
    RO_QUERY(4u, NETMON_CATEGORY_RADIO, NETMON_REFRESH_REGISTRATION, 5000u, 15000u),
    RO_QUERY(5u, NETMON_CATEGORY_RADIO, NETMON_REFRESH_SERVING, 5000u, 15000u),
    RO_QUERY(6u, NETMON_CATEGORY_RADIO, NETMON_REFRESH_RADIO_POLICY, 30000u, 60000u),
    RO_QUERY(7u, NETMON_CATEGORY_RADIO, NETMON_REFRESH_RADIO_POLICY, 30000u, 60000u),
    RO_QUERY(8u, NETMON_CATEGORY_RADIO, NETMON_REFRESH_RADIO_POLICY, 30000u, 60000u),
    {9u, NETMON_CATEGORY_RADIO, TUNER_CAP, NETMON_PROVIDER_MODEM_QUERY,
     NETMON_REFRESH_TUNER, 30000u, 60000u,
     NETMON_PAGE_READ_ONLY | NETMON_PAGE_CALL_VISIBLE, 0u,
     NETMON_ACTION_NONE},
    {10u, NETMON_CATEGORY_RADIO, TUNER_CAP, NETMON_PROVIDER_MODEM_QUERY,
     NETMON_REFRESH_TUNER, 30000u, 60000u,
     NETMON_PAGE_READ_ONLY | NETMON_PAGE_CALL_VISIBLE, 0u,
     NETMON_ACTION_NONE},
    {11u, NETMON_CATEGORY_RADIO, TUNER_CAP, NETMON_PROVIDER_MODEM_QUERY,
     NETMON_REFRESH_TUNER, 30000u, 60000u,
     NETMON_PAGE_READ_ONLY | NETMON_PAGE_CALL_VISIBLE, 0u,
     NETMON_ACTION_NONE},
    {12u, NETMON_CATEGORY_RADIO, TUNER_CAP, NETMON_PROVIDER_MODEM_QUERY,
     NETMON_REFRESH_TUNER, 30000u, 60000u,
     NETMON_PAGE_READ_ONLY | NETMON_PAGE_CALL_VISIBLE, 0u,
     NETMON_ACTION_NONE},
    {13u, NETMON_CATEGORY_RADIO, TUNER_CAP, NETMON_PROVIDER_MODEM_QUERY,
     NETMON_REFRESH_TUNER, 30000u, 60000u,
     NETMON_PAGE_READ_ONLY | NETMON_PAGE_CALL_VISIBLE, 0u,
     NETMON_ACTION_NONE},
    RO_QUERY(14u, NETMON_CATEGORY_RADIO, NETMON_REFRESH_PACKET, 10000u, 30000u),
    RO_QUERY(15u, NETMON_CATEGORY_RADIO, NETMON_REFRESH_SIM, 10000u, 30000u),
    {16u, NETMON_CATEGORY_RADIO, VOICE_CAP, NETMON_PROVIDER_MODEM_QUERY,
     NETMON_REFRESH_VOICE, 10000u, 30000u,
     NETMON_PAGE_READ_ONLY | NETMON_PAGE_CALL_VISIBLE, 0u,
     NETMON_ACTION_NONE},
    RO_QUERY(17u, NETMON_CATEGORY_RADIO, NETMON_REFRESH_TEMPERATURE, 5000u, 15000u),

    RO_CACHE(20u, NETMON_CATEGORY_MODEM),
    RO_CACHE(21u, NETMON_CATEGORY_MODEM),
    RO_CACHE(22u, NETMON_CATEGORY_MODEM),
    RO_CACHE(23u, NETMON_CATEGORY_MODEM),
    RO_CACHE(24u, NETMON_CATEGORY_MODEM),
    RO_LOCAL(25u, NETMON_CATEGORY_MODEM),
    RO_QUERY(26u, NETMON_CATEGORY_MODEM, NETMON_REFRESH_IDENTITY, 0u, 60000u),
    {27u, NETMON_CATEGORY_MODEM, VOICE_CAP, NETMON_PROVIDER_MODEM_QUERY,
     NETMON_REFRESH_DVI, 0u, 60000u,
     NETMON_PAGE_READ_ONLY | NETMON_PAGE_CALL_VISIBLE, 0u,
     NETMON_ACTION_NONE},
    {28u, NETMON_CATEGORY_MODEM, MODEM_RUNTIME_EXT_CAP,
     NETMON_PROVIDER_MODEM_CACHE,
     NETMON_REFRESH_NONE, 0u, 0u,
     NETMON_PAGE_READ_ONLY | NETMON_PAGE_CALL_VISIBLE, 0u,
     NETMON_ACTION_NONE},
    RO_CACHE(29u, NETMON_CATEGORY_MODEM),

    RO_CACHE(30u, NETMON_CATEGORY_TELEPHONY),
    RO_CACHE(31u, NETMON_CATEGORY_TELEPHONY),
    RO_CACHE(32u, NETMON_CATEGORY_TELEPHONY),
    RO_QUERY(33u, NETMON_CATEGORY_TELEPHONY, NETMON_REFRESH_CALL_CAUSE, 0u, 30000u),
    RO_CACHE(34u, NETMON_CATEGORY_TELEPHONY),
    RO_QUERY(35u, NETMON_CATEGORY_TELEPHONY, NETMON_REFRESH_SMS_CONFIG, 0u, 60000u),
    RO_QUERY(36u, NETMON_CATEGORY_TELEPHONY, NETMON_REFRESH_SMS_CONFIG, 0u, 60000u),
    RO_QUERY(37u, NETMON_CATEGORY_TELEPHONY, NETMON_REFRESH_STORAGE_CAPS, 0u, 60000u),

    RO_LOCAL(40u, NETMON_CATEGORY_POWER),
    RO_LOCAL(41u, NETMON_CATEGORY_POWER),
    RO_LOCAL(42u, NETMON_CATEGORY_POWER),
    RO_LOCAL(43u, NETMON_CATEGORY_POWER),
    RO_LOCAL(44u, NETMON_CATEGORY_POWER),
    RO_LOCAL(45u, NETMON_CATEGORY_POWER),
    RO_LOCAL(46u, NETMON_CATEGORY_POWER),
    RO_LOCAL(47u, NETMON_CATEGORY_POWER),
    RO_LOCAL(48u, NETMON_CATEGORY_POWER),

    RO_LOCAL(50u, NETMON_CATEGORY_BOARD),
    RO_LOCAL(51u, NETMON_CATEGORY_BOARD),
    RO_LOCAL(52u, NETMON_CATEGORY_BOARD),
    RO_LOCAL(53u, NETMON_CATEGORY_BOARD),
    RO_LOCAL(54u, NETMON_CATEGORY_BOARD),
    RO_LOCAL(55u, NETMON_CATEGORY_BOARD),
    RO_LOCAL(56u, NETMON_CATEGORY_BOARD),
    RO_LOCAL(57u, NETMON_CATEGORY_BOARD),
    RO_LOCAL(58u, NETMON_CATEGORY_BOARD),

    RO_LOCAL_CALL(60u),
    RO_LOCAL_CALL(61u),
    RO_LOCAL_CALL(62u),
    RO_LOCAL_CALL(63u),
    RO_LOCAL_CALL(64u),
    RO_LOCAL_CALL(65u),
    {66u, NETMON_CATEGORY_AUDIO, RUNTIME_EXT_CAP, NETMON_PROVIDER_LOCAL,
     NETMON_REFRESH_NONE, 0u, 0u,
     NETMON_PAGE_READ_ONLY | NETMON_PAGE_CALL_VISIBLE, 0u,
     NETMON_ACTION_NONE},
    RO_LOCAL_CALL(67u),
    RO_LOCAL_CALL(68u),

    RO_LOCAL(70u, NETMON_CATEGORY_RUNTIME),
    RO_LOCAL(71u, NETMON_CATEGORY_RUNTIME),
    {72u, NETMON_CATEGORY_RUNTIME, RUNTIME_EXT_CAP, NETMON_PROVIDER_LOCAL,
     NETMON_REFRESH_NONE, 0u, 0u,
     NETMON_PAGE_READ_ONLY | NETMON_PAGE_CALL_VISIBLE, 0u,
     NETMON_ACTION_NONE},
    {73u, NETMON_CATEGORY_RUNTIME, RUNTIME_EXT_CAP, NETMON_PROVIDER_LOCAL,
     NETMON_REFRESH_NONE, 0u, 0u,
     NETMON_PAGE_READ_ONLY | NETMON_PAGE_CALL_VISIBLE, 0u,
     NETMON_ACTION_NONE},
    RO_LOCAL(74u, NETMON_CATEGORY_RUNTIME),
    {75u, NETMON_CATEGORY_RUNTIME, RUNTIME_EXT_CAP, NETMON_PROVIDER_LOCAL,
     NETMON_REFRESH_NONE, 0u, 0u,
     NETMON_PAGE_READ_ONLY | NETMON_PAGE_CALL_VISIBLE, 0u,
     NETMON_ACTION_NONE},

    RO_LOCAL(80u, NETMON_CATEGORY_SLEEP),
    RO_LOCAL(81u, NETMON_CATEGORY_SLEEP),
    {82u, NETMON_CATEGORY_SLEEP, RUNTIME_EXT_CAP, NETMON_PROVIDER_LOCAL,
     NETMON_REFRESH_NONE, 0u, 0u,
     NETMON_PAGE_READ_ONLY | NETMON_PAGE_CALL_VISIBLE, 0u,
     NETMON_ACTION_NONE},
    {83u, NETMON_CATEGORY_SLEEP, RUNTIME_EXT_CAP, NETMON_PROVIDER_LOCAL,
     NETMON_REFRESH_NONE, 0u, 0u,
     NETMON_PAGE_READ_ONLY | NETMON_PAGE_CALL_VISIBLE, 0u,
     NETMON_ACTION_NONE},
    RO_LOCAL(84u, NETMON_CATEGORY_SLEEP),
    RO_LOCAL(85u, NETMON_CATEGORY_SLEEP),
    RO_LOCAL(86u, NETMON_CATEGORY_SLEEP),
    RO_LOCAL(87u, NETMON_CATEGORY_SLEEP),
    RO_LOCAL(88u, NETMON_CATEGORY_SLEEP),
    EDIT_LOCAL(89u, NETMON_EDIT_KEY(NETMON_KEY_0) |
                        NETMON_EDIT_KEY(NETMON_KEY_1) |
                        NETMON_EDIT_KEY(NETMON_KEY_3) |
                        NETMON_EDIT_KEY(NETMON_KEY_4) |
                        NETMON_EDIT_KEY(NETMON_KEY_6) |
                        NETMON_EDIT_KEY(NETMON_KEY_STAR) |
                        NETMON_EDIT_KEY(NETMON_KEY_HASH),
               NETMON_ACTION_BACKLIGHT_LEVEL),

    EDIT_LOCAL(90u, NETMON_EDIT_KEY(NETMON_KEY_0) |
                        NETMON_EDIT_KEY(NETMON_KEY_1) |
                        NETMON_EDIT_KEY(NETMON_KEY_2) |
                        NETMON_EDIT_KEY(NETMON_KEY_3),
               NETMON_ACTION_OUTPUT_TEST),
    EDIT_LOCAL(91u, NETMON_EDIT_KEY(NETMON_KEY_1) |
                        NETMON_EDIT_KEY(NETMON_KEY_2) |
                        NETMON_EDIT_KEY(NETMON_KEY_3) |
                        NETMON_EDIT_KEY(NETMON_KEY_4) |
                        NETMON_EDIT_KEY(NETMON_KEY_5) |
                        NETMON_EDIT_KEY(NETMON_KEY_HASH),
               NETMON_ACTION_BATTERY_SIM),
    EDIT_LOCAL(92u, NETMON_EDIT_KEY(NETMON_KEY_1) |
                        NETMON_EDIT_KEY(NETMON_KEY_2) |
                        NETMON_EDIT_KEY(NETMON_KEY_3) |
                        NETMON_EDIT_KEY(NETMON_KEY_4) |
                        NETMON_EDIT_KEY(NETMON_KEY_HASH),
               NETMON_ACTION_CHARGER),
    EDIT_LOCAL(93u, NETMON_EDIT_KEY(NETMON_KEY_0) |
                        NETMON_EDIT_KEY(NETMON_KEY_1) |
                        NETMON_EDIT_KEY(NETMON_KEY_2),
               NETMON_ACTION_HEADSET_FORCE),
    EDIT_LOCAL(94u, NETMON_EDIT_KEY(NETMON_KEY_0) |
                        NETMON_EDIT_KEY(NETMON_KEY_1),
               NETMON_ACTION_CONVERTER_MODE),
    EDIT_LOCAL(95u, NETMON_EDIT_KEY(NETMON_KEY_0) |
                        NETMON_EDIT_KEY(NETMON_KEY_1) |
                        NETMON_EDIT_KEY(NETMON_KEY_3) |
                        NETMON_EDIT_KEY(NETMON_KEY_4) |
                        NETMON_EDIT_KEY(NETMON_KEY_6) |
                        NETMON_EDIT_KEY(NETMON_KEY_7) |
                        NETMON_EDIT_KEY(NETMON_KEY_9) |
                        NETMON_EDIT_KEY(NETMON_KEY_STAR) |
                        NETMON_EDIT_KEY(NETMON_KEY_HASH),
               NETMON_ACTION_LCD_CALIBRATION),
    EDIT_LOCAL(96u, NETMON_EDIT_KEY(NETMON_KEY_0),
               NETMON_ACTION_MEASUREMENT_RESET),
    EDIT_MODEM_CACHE(97u, RADIO_CONTROL_CAP,
               NETMON_EDIT_KEY(NETMON_KEY_1) |
                   NETMON_EDIT_KEY(NETMON_KEY_2) |
                   NETMON_EDIT_KEY(NETMON_KEY_3) |
                   NETMON_EDIT_KEY(NETMON_KEY_4) |
                   NETMON_EDIT_KEY(NETMON_KEY_5) |
                   NETMON_EDIT_KEY(NETMON_KEY_6) |
                   NETMON_EDIT_KEY(NETMON_KEY_7) |
                   NETMON_EDIT_KEY(NETMON_KEY_STAR) |
                   NETMON_EDIT_KEY(NETMON_KEY_HASH),
               NETMON_ACTION_NO_SERVICE_TIMER),
    EDIT_MODEM_CACHE(98u, RADIO_CONTROL_CAP,
               NETMON_EDIT_KEY(NETMON_KEY_0) |
                   NETMON_EDIT_KEY(NETMON_KEY_1) |
                   NETMON_EDIT_KEY(NETMON_KEY_2) |
                   NETMON_EDIT_KEY(NETMON_KEY_3) |
                   NETMON_EDIT_KEY(NETMON_KEY_4) |
                   NETMON_EDIT_KEY(NETMON_KEY_5) |
                   NETMON_EDIT_KEY(NETMON_KEY_STAR) |
                   NETMON_EDIT_KEY(NETMON_KEY_HASH),
               NETMON_ACTION_BAND_TEST),
    EDIT_MODEM_CACHE(99u, RF_MAINTENANCE_CAP,
               NETMON_EDIT_KEY(NETMON_KEY_0) |
                   NETMON_EDIT_KEY(NETMON_KEY_1) |
                   NETMON_EDIT_KEY(NETMON_KEY_2) |
                   NETMON_EDIT_KEY(NETMON_KEY_3) |
                   NETMON_EDIT_KEY(NETMON_KEY_4) |
                   NETMON_EDIT_KEY(NETMON_KEY_STAR) |
                   NETMON_EDIT_KEY(NETMON_KEY_HASH),
               NETMON_ACTION_ANTENNA_MAINTENANCE),
};

size_t netmon_registry_count(void) {
    return sizeof(NETMON_PAGES) / sizeof(NETMON_PAGES[0]);
}

const netmon_page_descriptor_t *netmon_registry_at(size_t index) {
    if (index >= netmon_registry_count()) {
        return NULL;
    }
    return &NETMON_PAGES[index];
}

const netmon_page_descriptor_t *netmon_registry_find(uint16_t selector) {
    for (size_t i = 0u; i < netmon_registry_count(); i++) {
        if (NETMON_PAGES[i].id == selector) {
            return &NETMON_PAGES[i];
        }
    }
    return NULL;
}

bool netmon_registry_validate(void) {
    uint8_t previous = 0u;
    for (size_t i = 0u; i < netmon_registry_count(); i++) {
        const netmon_page_descriptor_t *page = &NETMON_PAGES[i];
        if (page->id == 0u || page->id <= previous) {
            return false;
        }
        if (((page->behavior_flags & NETMON_PAGE_READ_ONLY) != 0u) ==
            ((page->behavior_flags & NETMON_PAGE_EDITABLE) != 0u)) {
            return false;
        }
        if ((page->behavior_flags & NETMON_PAGE_EDITABLE) == 0u &&
            (page->edit_keys != 0u || page->action != NETMON_ACTION_NONE)) {
            return false;
        }
        if ((page->edit_keys &
             (NETMON_EDIT_KEY(NETMON_KEY_NAVI) |
              NETMON_EDIT_KEY(NETMON_KEY_C) |
              NETMON_EDIT_KEY(NETMON_KEY_POWER) |
              NETMON_EDIT_KEY(NETMON_KEY_UP) |
              NETMON_EDIT_KEY(NETMON_KEY_DOWN))) != 0u) {
            return false;
        }
        previous = page->id;
    }
    return true;
}

bool netmon_page_supported(const netmon_page_descriptor_t *page,
                           uint32_t capabilities) {
    return page != NULL &&
           (capabilities & page->required_caps) == page->required_caps;
}

modem_diag_group_t netmon_page_modem_group(
    const netmon_page_descriptor_t *page) {
    if (page == NULL) {
        return MODEM_DIAG_GROUP_NONE;
    }
    switch (page->refresh_group) {
    case NETMON_REFRESH_SERVING: return MODEM_DIAG_GROUP_SERVING;
    case NETMON_REFRESH_REGISTRATION: return MODEM_DIAG_GROUP_REGISTRATION;
    case NETMON_REFRESH_RADIO_POLICY: return MODEM_DIAG_GROUP_RADIO_POLICY;
    case NETMON_REFRESH_TUNER: return MODEM_DIAG_GROUP_TUNER;
    case NETMON_REFRESH_PACKET: return MODEM_DIAG_GROUP_PACKET;
    case NETMON_REFRESH_SIM: return MODEM_DIAG_GROUP_SIM;
    case NETMON_REFRESH_VOICE: return MODEM_DIAG_GROUP_VOICE;
    case NETMON_REFRESH_TEMPERATURE: return MODEM_DIAG_GROUP_TEMPERATURE;
    case NETMON_REFRESH_IDENTITY: return MODEM_DIAG_GROUP_IDENTITY;
    case NETMON_REFRESH_DVI: return MODEM_DIAG_GROUP_DVI;
    case NETMON_REFRESH_CALL_CAUSE: return MODEM_DIAG_GROUP_CALL_CAUSE;
    case NETMON_REFRESH_SMS_CONFIG: return MODEM_DIAG_GROUP_SMS_CONFIG;
    case NETMON_REFRESH_STORAGE_CAPS: return MODEM_DIAG_GROUP_STORAGE_CAPS;
    case NETMON_REFRESH_MODEM_RUNTIME:
    case NETMON_REFRESH_NONE:
    default: return MODEM_DIAG_GROUP_NONE;
    }
}

uint16_t netmon_adjacent_selector(uint16_t selector, bool forward,
                                  uint32_t capabilities) {
    const size_t count = netmon_registry_count();

    size_t current = count;
    for (size_t i = 0u; i < count; i++) {
        if (NETMON_PAGES[i].id == selector) {
            current = i;
            break;
        }
    }
    for (size_t step = 1u; step <= count; step++) {
        size_t index;
        if (current == count) {
            index = forward ? step - 1u : count - step;
        } else if (forward) {
            index = (current + step) % count;
        } else {
            index = (current + count - (step % count)) % count;
        }
        if (netmon_page_supported(&NETMON_PAGES[index], capabilities)) {
            return NETMON_PAGES[index].id;
        }
    }
    return 0u;
}

netmon_key_t netmon_key_from_code(uint16_t code) {
    switch (code) {
    case INPUT_KEY_0: return NETMON_KEY_0;
    case INPUT_KEY_1: return NETMON_KEY_1;
    case INPUT_KEY_2: return NETMON_KEY_2;
    case INPUT_KEY_3: return NETMON_KEY_3;
    case INPUT_KEY_4: return NETMON_KEY_4;
    case INPUT_KEY_5: return NETMON_KEY_5;
    case INPUT_KEY_6: return NETMON_KEY_6;
    case INPUT_KEY_7: return NETMON_KEY_7;
    case INPUT_KEY_8: return NETMON_KEY_8;
    case INPUT_KEY_9: return NETMON_KEY_9;
    case INPUT_KEY_STAR: return NETMON_KEY_STAR;
    case INPUT_KEY_HASH: return NETMON_KEY_HASH;
    case INPUT_KEY_UP: return NETMON_KEY_UP;
    case INPUT_KEY_DOWN: return NETMON_KEY_DOWN;
    case INPUT_KEY_NAVI: return NETMON_KEY_NAVI;
    case INPUT_KEY_C: return NETMON_KEY_C;
    case INPUT_KEY_POWER: return NETMON_KEY_POWER;
    default: return NETMON_KEY_OTHER;
    }
}

netmon_key_disposition_t netmon_key_disposition(
    const netmon_page_descriptor_t *page, netmon_surface_t surface,
    bool key_down, netmon_key_t key) {
    if (page == NULL || surface != NETMON_SURFACE_STANDBY) {
        return NETMON_KEY_PASS;
    }
    if (key == NETMON_KEY_NAVI || key == NETMON_KEY_C ||
        key == NETMON_KEY_POWER) {
        return NETMON_KEY_PASS;
    }
    if (!key_down) {
        /* The router's non-down event is KEY_HOLD. Swallow it only when this
         * editable page owns the key: the action remains one-shot, and the
         * hold cannot leak into normal standby speed-dial/number handling. */
        return (page->behavior_flags & NETMON_PAGE_EDITABLE) != 0u &&
                       (unsigned)key < 32u &&
                       (page->edit_keys & NETMON_EDIT_KEY(key)) != 0u
                   ? NETMON_KEY_CONSUME
                   : NETMON_KEY_PASS;
    }
    if (key == NETMON_KEY_UP) {
        return NETMON_KEY_BROWSE_PREVIOUS;
    }
    if (key == NETMON_KEY_DOWN) {
        return NETMON_KEY_BROWSE_NEXT;
    }
    if ((page->behavior_flags & NETMON_PAGE_EDITABLE) == 0u ||
        (unsigned)key >= 32u) {
        return NETMON_KEY_PASS;
    }
    return (page->edit_keys & NETMON_EDIT_KEY(key)) != 0u
               ? NETMON_KEY_EDIT
               : NETMON_KEY_PASS;
}

bool netmon_overlay_visible(const netmon_page_descriptor_t *page,
                            netmon_surface_t surface) {
    if (page == NULL) {
        return false;
    }
    if (surface == NETMON_SURFACE_STANDBY) {
        return true;
    }
    if ((page->behavior_flags & NETMON_PAGE_CALL_VISIBLE) == 0u) {
        return false;
    }
    return surface == NETMON_SURFACE_CALL_OUTGOING ||
           surface == NETMON_SURFACE_CALL_CONNECTED;
}

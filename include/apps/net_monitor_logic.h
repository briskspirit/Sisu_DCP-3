#ifndef APPS_NET_MONITOR_LOGIC_H
#define APPS_NET_MONITOR_LOGIC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "services/netmon_control_types.h"
#include "services/modem_diag.h"

#define NETMON_SCHEMA_VERSION 3u

typedef enum {
    NETMON_CATEGORY_RADIO = 0,
    NETMON_CATEGORY_MODEM,
    NETMON_CATEGORY_TELEPHONY,
    NETMON_CATEGORY_POWER,
    NETMON_CATEGORY_BOARD,
    NETMON_CATEGORY_AUDIO,
    NETMON_CATEGORY_RUNTIME,
    NETMON_CATEGORY_SLEEP,
    NETMON_CATEGORY_CONTROL,
} netmon_category_t;

typedef enum {
    NETMON_PROVIDER_LOCAL = 0,
    NETMON_PROVIDER_MODEM_CACHE,
    NETMON_PROVIDER_MODEM_QUERY,
} netmon_provider_t;

typedef enum {
    NETMON_REFRESH_NONE = 0,
    NETMON_REFRESH_SERVING,
    NETMON_REFRESH_REGISTRATION,
    NETMON_REFRESH_RADIO_POLICY,
    NETMON_REFRESH_TUNER,
    NETMON_REFRESH_PACKET,
    NETMON_REFRESH_SIM,
    NETMON_REFRESH_VOICE,
    NETMON_REFRESH_TEMPERATURE,
    NETMON_REFRESH_MODEM_RUNTIME,
    NETMON_REFRESH_IDENTITY,
    NETMON_REFRESH_DVI,
    NETMON_REFRESH_CALL_CAUSE,
    NETMON_REFRESH_SMS_CONFIG,
    NETMON_REFRESH_STORAGE_CAPS,
} netmon_refresh_group_t;

typedef enum {
    NETMON_KEY_OTHER = 0,
    NETMON_KEY_0,
    NETMON_KEY_1,
    NETMON_KEY_2,
    NETMON_KEY_3,
    NETMON_KEY_4,
    NETMON_KEY_5,
    NETMON_KEY_6,
    NETMON_KEY_7,
    NETMON_KEY_8,
    NETMON_KEY_9,
    NETMON_KEY_STAR,
    NETMON_KEY_HASH,
    NETMON_KEY_UP,
    NETMON_KEY_DOWN,
    NETMON_KEY_NAVI,
    NETMON_KEY_C,
    NETMON_KEY_POWER,
} netmon_key_t;

#define NETMON_EDIT_KEY(key) (1u << (unsigned)(key))
#define NETMON_EDIT_DIGITS \
    (NETMON_EDIT_KEY(NETMON_KEY_0) | NETMON_EDIT_KEY(NETMON_KEY_1) | \
     NETMON_EDIT_KEY(NETMON_KEY_2) | NETMON_EDIT_KEY(NETMON_KEY_3) | \
     NETMON_EDIT_KEY(NETMON_KEY_4) | NETMON_EDIT_KEY(NETMON_KEY_5) | \
     NETMON_EDIT_KEY(NETMON_KEY_6) | NETMON_EDIT_KEY(NETMON_KEY_7) | \
     NETMON_EDIT_KEY(NETMON_KEY_8) | NETMON_EDIT_KEY(NETMON_KEY_9))

typedef enum {
    NETMON_SURFACE_OTHER = 0,
    NETMON_SURFACE_STANDBY,
    NETMON_SURFACE_CALL_OUTGOING,
    NETMON_SURFACE_CALL_CONNECTED,
    NETMON_SURFACE_CALL_INCOMING,
    NETMON_SURFACE_CALL_WAITING,
    NETMON_SURFACE_CALL_POPUP,
} netmon_surface_t;

typedef enum {
    NETMON_KEY_PASS = 0,
    NETMON_KEY_CONSUME,
    NETMON_KEY_BROWSE_PREVIOUS,
    NETMON_KEY_BROWSE_NEXT,
    NETMON_KEY_EDIT,
} netmon_key_disposition_t;

enum {
    NETMON_CAP_REVB2 = 1u << 0,
    NETMON_CAP_TELIT = 1u << 1,
    NETMON_CAP_MODEM = 1u << 2,
    NETMON_CAP_TUNER = 1u << 3,
    NETMON_CAP_VOICE = 1u << 4,
    NETMON_CAP_LOCAL_CONTROLS = 1u << 5,
    NETMON_CAP_RUNTIME_EXT = 1u << 6,
    NETMON_CAP_RADIO_CONTROL = 1u << 7,
    NETMON_CAP_RF_MAINTENANCE = 1u << 8,
};

enum {
    NETMON_PAGE_READ_ONLY = 1u << 0,
    NETMON_PAGE_EDITABLE = 1u << 1,
    NETMON_PAGE_CALL_VISIBLE = 1u << 2,
    NETMON_PAGE_STANDBY_ONLY = 1u << 3,
};

typedef struct {
    uint8_t id;
    netmon_category_t category;
    uint32_t required_caps;
    netmon_provider_t provider;
    netmon_refresh_group_t refresh_group;
    uint32_t refresh_period_ms;
    uint32_t stale_after_ms;
    uint8_t behavior_flags;
    uint32_t edit_keys;
    netmon_action_t action;
} netmon_page_descriptor_t;

size_t netmon_registry_count(void);
const netmon_page_descriptor_t *netmon_registry_at(size_t index);
const netmon_page_descriptor_t *netmon_registry_find(uint16_t selector);
bool netmon_registry_validate(void);
bool netmon_page_supported(const netmon_page_descriptor_t *page,
                           uint32_t capabilities);
modem_diag_group_t netmon_page_modem_group(
    const netmon_page_descriptor_t *page);
uint16_t netmon_adjacent_selector(uint16_t selector, bool forward,
                                  uint32_t capabilities);

netmon_key_t netmon_key_from_code(uint16_t code);
netmon_key_disposition_t netmon_key_disposition(
    const netmon_page_descriptor_t *page, netmon_surface_t surface,
    bool key_down, netmon_key_t key);
bool netmon_overlay_visible(const netmon_page_descriptor_t *page,
                            netmon_surface_t surface);

#endif

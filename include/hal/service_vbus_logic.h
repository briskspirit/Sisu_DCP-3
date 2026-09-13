#ifndef SERVICE_VBUS_LOGIC_H
#define SERVICE_VBUS_LOGIC_H

#include <stdbool.h>
#include <stdint.h>

#define SERVICE_VBUS_ATTACH_DEBOUNCE_MS 16u
#define SERVICE_VBUS_DETACH_DEBOUNCE_MS 32u

typedef struct {
    bool stable_present;
    bool candidate_present;
    bool candidate_valid;
    uint32_t candidate_since_ms;
} service_vbus_filter_t;

void service_vbus_filter_init(service_vbus_filter_t *filter,
                              bool raw_present);
bool service_vbus_filter_update(service_vbus_filter_t *filter,
                                bool raw_present,
                                uint32_t now_ms,
                                bool *changed);

#endif

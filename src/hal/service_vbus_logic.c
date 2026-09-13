#include "hal/service_vbus_logic.h"

static bool deadline_reached(uint32_t now_ms, uint32_t deadline_ms) {
    return (int32_t)(now_ms - deadline_ms) >= 0;
}

void service_vbus_filter_init(service_vbus_filter_t *filter,
                              bool raw_present) {
    if (filter == 0) {
        return;
    }
    filter->stable_present = raw_present;
    filter->candidate_present = raw_present;
    filter->candidate_valid = false;
    filter->candidate_since_ms = 0u;
}

bool service_vbus_filter_update(service_vbus_filter_t *filter,
                                bool raw_present,
                                uint32_t now_ms,
                                bool *changed) {
    if (changed != 0) {
        *changed = false;
    }
    if (filter == 0) {
        return false;
    }
    if (raw_present == filter->stable_present) {
        filter->candidate_valid = false;
        return filter->stable_present;
    }
    if (!filter->candidate_valid ||
        filter->candidate_present != raw_present) {
        filter->candidate_present = raw_present;
        filter->candidate_since_ms = now_ms;
        filter->candidate_valid = true;
        return filter->stable_present;
    }

    uint32_t delay_ms = raw_present ? SERVICE_VBUS_ATTACH_DEBOUNCE_MS
                                    : SERVICE_VBUS_DETACH_DEBOUNCE_MS;
    if (deadline_reached(now_ms, filter->candidate_since_ms + delay_ms)) {
        filter->stable_present = raw_present;
        filter->candidate_valid = false;
        if (changed != 0) {
            *changed = true;
        }
    }
    return filter->stable_present;
}

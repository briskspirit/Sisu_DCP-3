#include "hal/battery_runtime_logic.h"

battery_sample_mode_t battery_sample_mode_select(bool high_load,
                                                 bool app_awake) {
    if (high_load) {
        return BATTERY_SAMPLE_MODE_HIGH_LOAD;
    }
    return app_awake ? BATTERY_SAMPLE_MODE_AWAKE
                     : BATTERY_SAMPLE_MODE_QUIET;
}

uint32_t battery_sample_period_ms(battery_sample_mode_t mode,
                                  bool charging) {
    uint32_t period_ms;
    switch (mode) {
    case BATTERY_SAMPLE_MODE_HIGH_LOAD:
        period_ms = BATTERY_SAMPLE_HIGH_LOAD_MS;
        break;
    case BATTERY_SAMPLE_MODE_AWAKE:
        period_ms = BATTERY_SAMPLE_AWAKE_MS;
        break;
    case BATTERY_SAMPLE_MODE_QUIET:
    default:
        period_ms = BATTERY_SAMPLE_QUIET_MS;
        break;
    }
    if (charging && period_ms > BATTERY_SAMPLE_AWAKE_MS) {
        period_ms = BATTERY_SAMPLE_AWAKE_MS;
    }
    return period_ms;
}

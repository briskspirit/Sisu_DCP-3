#ifndef BATTERY_RUNTIME_LOGIC_H
#define BATTERY_RUNTIME_LOGIC_H

#include <stdbool.h>
#include <stdint.h>

/* These periods are opportunities while core 0 is already executing. They are
 * deliberately absent from the powered-on dormant wake calculation. */
#define BATTERY_SAMPLE_HIGH_LOAD_MS 250u
#define BATTERY_SAMPLE_AWAKE_MS 1000u
#define BATTERY_SAMPLE_QUIET_MS 5000u

typedef enum {
    BATTERY_SAMPLE_MODE_QUIET = 0,
    BATTERY_SAMPLE_MODE_AWAKE,
    BATTERY_SAMPLE_MODE_HIGH_LOAD,
} battery_sample_mode_t;

battery_sample_mode_t battery_sample_mode_select(bool high_load,
                                                 bool app_awake);
uint32_t battery_sample_period_ms(battery_sample_mode_t mode,
                                  bool charging);

#endif

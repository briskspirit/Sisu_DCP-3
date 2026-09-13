#ifndef BACKLIGHT_CALIBRATION_H
#define BACKLIGHT_CALIBRATION_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint8_t active_percent;
    bool stored_valid;
    uint8_t stored_percent;
} backlight_calibration_status_t;

/* Call after backlight_service_init() and store_service_init(). */
bool backlight_calibration_service_init(void);
bool backlight_calibration_preview(uint8_t level_percent);
bool backlight_calibration_preview_default(void);
bool backlight_calibration_revert_stored(void);
bool backlight_calibration_save_active(void);
void backlight_calibration_get_status(
    backlight_calibration_status_t *out_status);

#endif

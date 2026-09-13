#include "services/backlight_calibration.h"

#include <string.h>

#include "services/backlight_service.h"
#include "services/log.h"
#include "storage/store_service.h"

static bool level_valid(uint8_t level_percent) {
    return level_percent >= BACKLIGHT_LEVEL_MIN_PERCENT &&
           level_percent <= BACKLIGHT_LEVEL_MAX_PERCENT;
}

bool backlight_calibration_service_init(void) {
    uint8_t level_percent = BACKLIGHT_LEVEL_DEFAULT_PERCENT;
    store_status_t status = store_setting_get_u8(
        STORE_SETTING_SYSTEM_BACKLIGHT_LEVEL, &level_percent);
    if (status != STORE_STATUS_OK || !level_valid(level_percent)) {
        if (status == STORE_STATUS_OK) {
            LOGW("backlight", "invalid stored level %u; using %u%%",
                 (unsigned)level_percent,
                 (unsigned)BACKLIGHT_LEVEL_DEFAULT_PERCENT);
            (void)store_setting_set_u8(
                STORE_SETTING_SYSTEM_BACKLIGHT_LEVEL,
                BACKLIGHT_LEVEL_DEFAULT_PERCENT);
        }
        level_percent = BACKLIGHT_LEVEL_DEFAULT_PERCENT;
    }
    return backlight_service_set_level_percent(level_percent);
}

bool backlight_calibration_preview(uint8_t level_percent) {
    return backlight_service_set_level_percent(level_percent);
}

bool backlight_calibration_preview_default(void) {
    return backlight_calibration_preview(BACKLIGHT_LEVEL_DEFAULT_PERCENT);
}

bool backlight_calibration_revert_stored(void) {
    uint8_t level_percent = BACKLIGHT_LEVEL_DEFAULT_PERCENT;
    if (store_setting_get_u8(STORE_SETTING_SYSTEM_BACKLIGHT_LEVEL,
                             &level_percent) != STORE_STATUS_OK ||
        !level_valid(level_percent)) {
        level_percent = BACKLIGHT_LEVEL_DEFAULT_PERCENT;
    }
    return backlight_calibration_preview(level_percent);
}

bool backlight_calibration_save_active(void) {
    return store_setting_set_u8(
               STORE_SETTING_SYSTEM_BACKLIGHT_LEVEL,
               backlight_service_level_percent()) == STORE_STATUS_OK;
}

void backlight_calibration_get_status(
    backlight_calibration_status_t *out_status) {
    if (out_status == NULL) {
        return;
    }
    memset(out_status, 0, sizeof(*out_status));
    out_status->active_percent = backlight_service_level_percent();
    uint8_t stored = 0u;
    if (store_setting_get_u8(STORE_SETTING_SYSTEM_BACKLIGHT_LEVEL,
                             &stored) == STORE_STATUS_OK &&
        level_valid(stored)) {
        out_status->stored_valid = true;
        out_status->stored_percent = stored;
    }
}

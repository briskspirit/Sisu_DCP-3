#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "services/backlight_calibration.h"
#include "services/backlight_service.h"
#include "storage/store_service.h"

static int s_failures;
static uint8_t s_active;
static uint8_t s_stored;
static store_status_t s_get_status;
static store_status_t s_set_status;
static unsigned s_apply_count;
static unsigned s_store_count;

static void check(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        s_failures++;
    }
}

bool backlight_service_set_level_percent(uint8_t level_percent) {
    if (level_percent < BACKLIGHT_LEVEL_MIN_PERCENT ||
        level_percent > BACKLIGHT_LEVEL_MAX_PERCENT) {
        return false;
    }
    s_active = level_percent;
    s_apply_count++;
    return true;
}

uint8_t backlight_service_level_percent(void) {
    return s_active;
}

store_status_t store_setting_get_u8(store_setting_key_t key,
                                    uint8_t *out_value) {
    check(key == STORE_SETTING_SYSTEM_BACKLIGHT_LEVEL,
          "calibration reads only its backlight key");
    if (s_get_status == STORE_STATUS_OK && out_value != NULL) {
        *out_value = s_stored;
    }
    return s_get_status;
}

store_status_t store_setting_set_u8(store_setting_key_t key, uint8_t value) {
    check(key == STORE_SETTING_SYSTEM_BACKLIGHT_LEVEL,
          "calibration writes only its backlight key");
    s_store_count++;
    if (s_set_status == STORE_STATUS_OK) {
        s_stored = value;
    }
    return s_set_status;
}

void log_write(int level, const char *tag, const char *fmt, ...) {
    (void)level;
    (void)tag;
    (void)fmt;
}

static void reset_fixture(void) {
    s_active = BACKLIGHT_LEVEL_DEFAULT_PERCENT;
    s_stored = BACKLIGHT_LEVEL_DEFAULT_PERCENT;
    s_get_status = STORE_STATUS_OK;
    s_set_status = STORE_STATUS_OK;
    s_apply_count = 0u;
    s_store_count = 0u;
}

static void test_initialization_and_healing(void) {
    reset_fixture();
    s_stored = 37u;
    check(backlight_calibration_service_init(), "stored level applies");
    check(s_active == 37u && s_apply_count == 1u,
          "stored duty applies exactly once");

    reset_fixture();
    s_stored = 0u;
    check(backlight_calibration_service_init(), "invalid level falls back");
    check(s_active == BACKLIGHT_LEVEL_DEFAULT_PERCENT &&
              s_stored == BACKLIGHT_LEVEL_DEFAULT_PERCENT &&
              s_store_count == 1u,
          "invalid persisted duty is healed to 100%");

    reset_fixture();
    s_get_status = STORE_STATUS_NOT_READY;
    check(backlight_calibration_service_init(),
          "unavailable storage still leaves a usable light");
    check(s_active == BACKLIGHT_LEVEL_DEFAULT_PERCENT && s_store_count == 0u,
          "unavailable storage uses default without a write");
}

static void test_preview_revert_status_and_save(void) {
    reset_fixture();
    s_stored = 65u;
    check(backlight_calibration_preview(42u) && s_active == 42u,
          "preview applies live level");
    unsigned before = s_apply_count;
    check(!backlight_calibration_preview(0u) &&
              !backlight_calibration_preview(101u) &&
              s_apply_count == before,
          "preview rejects values outside 1..100");
    check(backlight_calibration_revert_stored() && s_active == 65u,
          "revert restores persisted level");
    check(backlight_calibration_preview_default() &&
              s_active == BACKLIGHT_LEVEL_DEFAULT_PERCENT,
          "factory preview is exactly 100%");

    backlight_calibration_status_t status;
    backlight_calibration_get_status(&status);
    check(status.active_percent == BACKLIGHT_LEVEL_DEFAULT_PERCENT &&
              status.stored_valid && status.stored_percent == 65u,
          "status separates active preview from persisted value");

    check(backlight_calibration_preview(73u) &&
              backlight_calibration_save_active() &&
              s_stored == 73u,
          "active preview saves through the journaled setting");
    s_set_status = STORE_STATUS_STORAGE_ERROR;
    check(!backlight_calibration_save_active(),
          "storage failure is reported to NetMonitor");

    s_get_status = STORE_STATUS_NOT_FOUND;
    backlight_calibration_get_status(&status);
    check(!status.stored_valid && status.stored_percent == 0u,
          "missing storage is shown as unavailable, not zero percent");
}

int main(void) {
    test_initialization_and_healing();
    test_preview_revert_status_and_save();
    if (s_failures != 0) {
        fprintf(stderr, "%d backlight calibration failure(s)\n", s_failures);
        return 1;
    }
    puts("backlight calibration tests passed");
    return 0;
}

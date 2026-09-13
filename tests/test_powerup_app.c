#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "apps/powerup_app.h"
#include "audio/audio_levels.h"
#include "services/core1_services.h"
#include "storage/store_service.h"

static int s_failures;
static uint8_t s_keypad_setting = 1u;
static store_status_t s_keypad_status = STORE_STATUS_OK;
static unsigned s_audio_posts;
static core1_cmd_t s_audio_cmd;
static uint16_t s_audio_arg;

static void check(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        s_failures++;
    }
}

bool store_service_ready(void) {
    return false;
}

store_status_t store_setting_get_u8(store_setting_key_t key, uint8_t *out_value) {
    check(key == STORE_SETTING_PROFILE_KEYPAD_TONES,
          "power-up reads only the active keypad-tone setting");
    if (s_keypad_status == STORE_STATUS_OK && out_value != NULL) {
        *out_value = s_keypad_setting;
    }
    return s_keypad_status;
}

store_status_t store_setting_get_text(store_setting_key_t key, char *out_text, uint8_t out_cap) {
    (void)key;
    (void)out_text;
    (void)out_cap;
    return STORE_STATUS_NOT_READY;
}

void core1_post_command(core1_cmd_t cmd, uint16_t arg) {
    s_audio_posts++;
    s_audio_cmd = cmd;
    s_audio_arg = arg;
}

void copy_text(char *dst, size_t cap, const char *src) {
    if (dst == NULL || cap == 0u) {
        return;
    }
    snprintf(dst, cap, "%s", src != NULL ? src : "");
}

bool start_clock_boot_setup_if_needed(app_t *app, uint32_t now) {
    (void)app;
    (void)now;
    return false;
}

uint32_t time_ms(void) {
    return 0u;
}

int32_t time_diff_ms(uint32_t a, uint32_t b) {
    return (int32_t)(a - b);
}

static void reset_audio(void) {
    s_audio_posts = 0u;
    s_audio_cmd = CORE1_CMD_NONE;
    s_audio_arg = 0u;
}

static void advance_to_blank(app_t *app) {
    check(tick_powerup(app, 1000u), "all-pixels stage advances to blank");
    check(app->powerup_stage == APP_POWERUP_BLANK,
          "first transition enters the plain blank stage");
    check(s_audio_posts == 0u, "blanking the LCD does not play early audio");
}

static void test_normal_powerup_click(void) {
    app_t app = {0};
    reset_audio();
    s_keypad_status = STORE_STATUS_OK;
    s_keypad_setting = 1u;

    start_powerup(&app, 0u);
    advance_to_blank(&app);
    check(tick_powerup(&app, 1550u), "blank stage advances to battery pre-roll");
    check(app.powerup_stage == APP_POWERUP_BATTERY_PRE,
          "click boundary enters battery pre-roll");
    check(s_audio_posts == 1u && s_audio_cmd == CORE1_CMD_AUDIO_CLICK,
          "normal power-up posts exactly one ordinary keypad click");
    check(s_audio_arg == audio_arg(0u, audio_level_from_keypad_tones(1u)),
          "power-up click uses the active keypad-tone level");

    check(!tick_powerup(&app, 1600u), "battery pre-roll remains pending");
    check(s_audio_posts == 1u, "later ticks do not replay the power-up click");
}

static void test_silent_and_no_logo_paths(void) {
    app_t app = {0};
    reset_audio();
    s_keypad_status = STORE_STATUS_OK;
    s_keypad_setting = 0xffu;

    start_powerup(&app, 0u);
    advance_to_blank(&app);
    check(tick_powerup(&app, 1550u), "silent-profile boot reaches battery pre-roll");
    check(s_audio_posts == 0u, "keypad-tones Off suppresses the power-up click");

    app = (app_t){0};
    reset_audio();
    s_keypad_setting = 1u;
    start_powerup_no_logo(&app, 0u);
    advance_to_blank(&app);
    check(tick_powerup(&app, 1550u), "alarm wake reaches battery pre-roll");
    check(s_audio_posts == 0u, "alarm/no-logo wake does not synthesize a power-key click");
}

static void test_unreadable_setting_uses_default(void) {
    app_t app = {0};
    reset_audio();
    s_keypad_status = STORE_STATUS_NOT_READY;

    start_powerup(&app, 0u);
    advance_to_blank(&app);
    check(tick_powerup(&app, 1550u), "boot with unavailable settings reaches battery pre-roll");
    check(s_audio_posts == 1u &&
              s_audio_arg == audio_arg(0u, audio_level_from_keypad_tones(1u)),
          "unavailable storage uses the normal keypad-level default");
}

int main(void) {
    test_normal_powerup_click();
    test_silent_and_no_logo_paths();
    test_unreadable_setting_uses_default();

    if (s_failures != 0) {
        fprintf(stderr, "%d power-up app test(s) failed\n", s_failures);
        return 1;
    }
    puts("power-up app tests passed");
    return 0;
}

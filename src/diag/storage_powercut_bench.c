#include "diag/storage_powercut_bench.h"

#include <stdio.h>
#include <string.h>

#include "diag/storage_powercut_test.h"
#include "hal/board.h"
#include "lfs_util.h"
#include "pico/stdlib.h"
#include "services/core1_services.h"
#include "services/runtime_watchdog.h"
#include "services/stack_monitor.h"
#include "services/timebase.h"
#include "services/usb_service.h"
#include "storage/storage_lfs.h"

#define WRITES_PER_BOOT 512u

static storage_backend_t s_backend;
static nvm_hal_t s_hal, s_legacy;
static uint8_t s_page[256];
static core1_services_diag_t s_diag;

static uint32_t legacy_checksum(bool *ok) {
    uint32_t crc = 0xffffffffu;
    *ok = nvm_flash_hal_init(&s_legacy) == NVM_STATUS_OK;
    for (uint32_t off = 0; *ok && off < s_legacy.capacity; off += sizeof(s_page)) {
        *ok = s_legacy.read(&s_legacy, off, s_page, sizeof(s_page)) == NVM_STATUS_OK;
        if (*ok) {
            crc = lfs_crc(crc, s_page, sizeof(s_page));
        }
    }
    return crc ^ 0xffffffffu;
}

static bool transport_ok(void) {
    core1_services_get_diag(&s_diag);
    return s_diag.flash_pause_timeouts == 0u && s_diag.flash_resume_timeouts == 0u;
}

static void report(bool ready, bool watchdog_failed, uint32_t crc) {
    powercut_status_t status;
    powercut_get_status(&status);
    core1_services_get_diag(&s_diag);
    printf("[powercut] ready=%u watchdog_failure=%u result=%u failed_id=%04x armed=%u "
           "boots=%lu completed=%lu pending=%lu gen=%lu,%lu recovered_old=%lu recovered_new=%lu "
           "legacy_crc=%08lx parks=%lu pause_timeout=%lu resume_timeout=%lu flags=%02lx\n",
           ready, watchdog_failed, status.result, status.failed_id, status.armed,
           (unsigned long)status.boots, (unsigned long)status.completed,
           (unsigned long)status.pending, (unsigned long)status.generation[0],
           (unsigned long)status.generation[1], (unsigned long)status.recovered_old,
           (unsigned long)status.recovered_new, (unsigned long)crc,
           (unsigned long)s_diag.flash_pause_successes,
           (unsigned long)s_diag.flash_pause_timeouts,
           (unsigned long)s_diag.flash_resume_timeouts,
           (unsigned long)s_diag.flash_park_last_flags);
}

static void display(lcd_pcd8544_t *lcd, framebuffer_t *fb,
                    const char *state, const powercut_status_t *status) {
    char text[24];
    fb_clear(fb, false);
    fb_text5(fb, "FLASH TEST", 0, 0, true, FB_WIDTH);
    fb_text5(fb, state, 0, 9, true, FB_WIDTH);
    snprintf(text, sizeof(text), "Boots %lu", (unsigned long)status->boots);
    fb_text5(fb, text, 0, 18, true, FB_WIDTH);
    snprintf(text, sizeof(text), "Writes %lu", (unsigned long)status->completed);
    fb_text5(fb, text, 0, 27, true, FB_WIDTH);
    snprintf(text, sizeof(text), "Err %u:%04X", status->result, status->failed_id);
    fb_text5(fb, text, 0, 36, true, FB_WIDTH);
    lcd_show(lcd, fb);
}

_Noreturn void storage_powercut_bench_run(lcd_pcd8544_t *lcd, framebuffer_t *fb) {
    runtime_watchdog_boot_evidence_t evidence;
    runtime_watchdog_get_boot_evidence(&evidence);
    bool read_ok;
    uint32_t crc = legacy_checksum(&read_ok);
    /* Initialization must not autoformat a missing/corrupt volume. A fully
     * erased volume is rejected before the ordinary adapter can format it. */
    bool nonblank = false;
    bool ready = read_ok && !evidence.timeout_reset &&
                 nvm_record_flash_hal_init(&s_hal) == NVM_STATUS_OK;
    for (uint32_t off = 0; ready && off < s_hal.capacity; off += sizeof(s_page)) {
        if (s_hal.read(&s_hal, off, s_page, sizeof(s_page)) != NVM_STATUS_OK) {
            ready = false;
            break;
        }
        for (size_t i = 0; i < sizeof(s_page); i++) {
            nonblank |= s_page[i] != 0xffu;
        }
    }
    ready = ready && nonblank && storage_lfs_init(&s_backend, &s_hal) == STORAGE_RECORD_OK;
    if (ready) {
        powercut_result_t result = powercut_open(&s_backend, crc);
        ready = result == POWERCUT_OK || result == POWERCUT_UNARMED;
    }
    /* Exercise production core1 park/unpark with codec DMA kept alive. The
     * modem stays off, and normal app/store services never start in this image. */
    core1_services_audio_gate_set_enabled(false);
    board_set_backlight(true);
    runtime_watchdog_start();
    powercut_status_t status;
    powercut_get_status(&status);
    uint32_t starting_writes = status.completed;
    uint32_t last_display = time_ms() - 500u;
    char line[32];
    size_t used = 0u;
    bool overflow = false;
    bool usb_test = false;
    bool previous_usb = board_service_vbus_present();
    for (;;) {
        uint32_t now = time_ms();
        bool usb = board_service_vbus_present();
        usb_service_poll(usb, now);
        if (usb && !previous_usb) {
            usb_test = false;
        }
        previous_usb = usb;
        ready = ready && transport_ok();
        int ch;
        while ((ch = getchar_timeout_us(0)) != PICO_ERROR_TIMEOUT) {
            if (ch == '\r' || ch == '\n') {
                line[used] = '\0';
                if (!overflow && used != 0u) {
                    if (strcmp(line, "bootsel confirm") == 0) {
                        board_reset_to_bootsel();
                    } else if (ready && strcmp(line, "arm confirm") == 0) {
                        ready = powercut_arm() == POWERCUT_OK;
                        usb_test = false;
                    } else if (ready && strcmp(line, "pause") == 0) {
                        powercut_result_t result = powercut_pause();
                        ready = result == POWERCUT_OK || result == POWERCUT_UNARMED;
                        usb_test = false;
                    } else if (ready && strcmp(line, "runusb confirm") == 0) {
                        powercut_get_status(&status);
                        usb_test = status.armed && status.result == POWERCUT_OK;
                    } else if (strcmp(line, "status") != 0) {
                        printf("[powercut] commands: status | arm confirm | pause | runusb confirm | bootsel confirm\n");
                    }
                    report(ready, evidence.timeout_reset, crc);
                }
                used = 0u;
                overflow = false;
            } else if (ch >= 32 && ch < 127) {
                if (used + 1u < sizeof(line)) {
                    line[used++] = (char)ch;
                } else {
                    overflow = true;
                }
            }
        }
        powercut_get_status(&status);
        bool limit = status.completed - starting_writes >= WRITES_PER_BOOT;
        if (ready && status.armed && (!usb || usb_test) && !limit) {
            ready = powercut_step() == POWERCUT_OK && transport_ok();
        }
        if (now - last_display >= 500u) {
            powercut_get_status(&status);
            const char *state = !ready ? "FAIL - STOP" :
                !status.armed ? "NOT ARMED" : limit ? "LIMIT - PAUSED" :
                usb && !usb_test ? "USB - PAUSED" : "CUT POWER";
            display(lcd, fb, state, &status);
            last_display = now;
        }
        stack_monitor_core0_sample(now);
        runtime_watchdog_feed(RUNTIME_WATCHDOG_PHASE_MAIN_LOOP);
        sleep_ms(5u);
    }
}

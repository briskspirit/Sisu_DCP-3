#include "services/usb_service.h"

#include "sisu_build_config.h"
#include "hardware/clocks.h"
#include "hardware/regs/clocks.h"
#include "hardware/regs/usb.h"
#include "hardware/resets.h"
#include "hardware/structs/usb.h"
#include "pico/stdlib.h"

#if !SISU_RELEASE_BUILD
#include "hardware/irq.h"
#include "hardware/sync.h"
#include "pico/stdio_usb.h"
#include "services/log.h"
#include "services/timebase.h"
#include "tusb.h"
#endif

#if !SISU_RELEASE_BUILD
#define USB_ATTACH_RETRY_MS 1000u
#define USB_PHY_SETTLE_US 100u
#define USB_BOOT_DETACH_MS 50u
#endif

static bool s_initialized;
static bool s_phy_parked;
#if !SISU_RELEASE_BUILD
static bool s_active;
static bool s_retry_pending;
static uint32_t s_retry_at_ms;
static uint32_t s_attach_failures;
static uint32_t s_worker_event_pending_streak;
static volatile usb_service_diag_t s_diag;

#ifndef PICO_STDIO_USB_LOW_PRIORITY_IRQ
#error "USB service requires a fixed pico_stdio_usb worker IRQ"
#endif
#if !PICO_STDIO_USB_ENABLE_IRQ_BACKGROUND_TASK
#error "USB service worker kicks require pico_stdio_usb IRQ background mode"
#endif
_Static_assert(PICO_STDIO_USB_LOW_PRIORITY_IRQ >= FIRST_USER_IRQ &&
                   PICO_STDIO_USB_LOW_PRIORITY_IRQ < NUM_IRQS,
               "pico_stdio_usb worker must use an RP2350 user IRQ");

static uint32_t increment_saturating(uint32_t value) {
    return value == UINT32_MAX ? UINT32_MAX : value + 1u;
}

/* pico_stdio_usb normally schedules its low-priority TinyUSB worker from the
 * USBCTRL IRQ. If the worker finds the SDK mutex busy, it relies on a one-shot
 * alarm whose allocation/result is not recoverable by the caller. A lost retry
 * can leave TinyUSB initialized but with CDC OUT unserviced indefinitely: the
 * host node still opens, while commands receive no response until USB is
 * reinitialized. Re-pend the SDK-owned worker from the product's 8 ms core-0
 * poll. The worker keeps its own mutex and callback ordering; no second caller
 * runs the TinyUSB task directly. This is service-image-only, so it has no release
 * power cost. */
static void kick_usb_worker(void) {
    bool events_pending = tud_task_event_ready();
    if (events_pending) {
        s_diag.event_pending_observations =
            increment_saturating(s_diag.event_pending_observations);
        s_worker_event_pending_streak =
            increment_saturating(s_worker_event_pending_streak);
        if (s_worker_event_pending_streak >
            s_diag.event_pending_streak_max) {
            s_diag.event_pending_streak_max =
                s_worker_event_pending_streak;
        }
    } else {
        s_worker_event_pending_streak = 0u;
    }

    if (!irq_is_enabled(PICO_STDIO_USB_LOW_PRIORITY_IRQ)) {
        s_diag.worker_disabled_rearms =
            increment_saturating(s_diag.worker_disabled_rearms);
        irq_set_enabled(PICO_STDIO_USB_LOW_PRIORITY_IRQ, true);
    }
    s_diag.worker_kicks = increment_saturating(s_diag.worker_kicks);
    irq_set_pending(PICO_STDIO_USB_LOW_PRIORITY_IRQ);
}

static void configure_usb_clock(void) {
    clock_configure_undivided(
        clk_usb,
        0u,
        CLOCKS_CLK_USB_CTRL_AUXSRC_VALUE_CLKSRC_PLL_USB,
        USB_CLK_HZ);
}
#endif

static void set_phy_detached(void) {
    usb_hw->phy_direct = USB_USBPHY_DIRECT_TX_PD_BITS |
                         USB_USBPHY_DIRECT_RX_PD_BITS |
                         USB_USBPHY_DIRECT_DP_PULLDN_EN_BITS |
                         USB_USBPHY_DIRECT_DM_PULLDN_EN_BITS;
    usb_hw->phy_direct_override =
        USB_USBPHY_DIRECT_OVERRIDE_TX_PD_OVERRIDE_EN_BITS |
        USB_USBPHY_DIRECT_OVERRIDE_RX_PD_OVERRIDE_EN_BITS |
        USB_USBPHY_DIRECT_OVERRIDE_DP_PULLDN_EN_OVERRIDE_EN_BITS |
        USB_USBPHY_DIRECT_OVERRIDE_DM_PULLDN_EN_OVERRIDE_EN_BITS;
}

static void park_phy(void) {
    reset_block_mask(RESETS_RESET_USBCTRL_BITS);
    unreset_block_mask_wait_blocking(RESETS_RESET_USBCTRL_BITS);
    hw_clear_bits(&usb_hw->main_ctrl, USB_MAIN_CTRL_PHY_ISO_BITS);
    set_phy_detached();
    busy_wait_us(10u);
    hw_set_bits(&usb_hw->main_ctrl, USB_MAIN_CTRL_PHY_ISO_BITS);
    clock_stop(clk_usb);
    s_phy_parked = true;
}

#if !SISU_RELEASE_BUILD
static void stop_usb(void) {
    /* stdio's driver chain is process-wide and unguarded by the SDK when a
     * driver is added/removed. Serialize against core-1 LOG calls; core-0
     * console output cannot overlap this main-loop operation. */
    log_output_lock();
    if (s_active) {
        (void)stdio_usb_deinit();
    }
    if (tud_inited()) {
        (void)tud_deinit(0u);
    }
    s_active = false;
    s_diag.stops = increment_saturating(s_diag.stops);
    log_output_unlock();
    park_phy();
}

static bool start_usb(bool force_boot_detach) {
    configure_usb_clock();
    reset_block_mask(RESETS_RESET_USBCTRL_BITS);
    unreset_block_mask_wait_blocking(RESETS_RESET_USBCTRL_BITS);
    hw_clear_bits(&usb_hw->main_ctrl, USB_MAIN_CTRL_PHY_ISO_BITS);
    busy_wait_us(USB_PHY_SETTLE_US);

    if (force_boot_detach) {
        /* A REBOOT2-class reset can be too quick for the host to observe. Hold
         * the same powered-down/pulldown state used while parked long enough to
         * guarantee a real detach before TinyUSB raises the device pull-up. */
        set_phy_detached();
        busy_wait_ms(USB_BOOT_DETACH_MS);
    }
    usb_hw->phy_direct_override = 0u;
    usb_hw->phy_direct = 0u;
    s_phy_parked = false;

    log_output_lock();
    bool initialized = stdio_usb_init();
    if (!initialized) {
        if (tud_inited()) {
            (void)tud_deinit(0u);
        }
        log_output_unlock();
        park_phy();
        return false;
    }
    log_output_unlock();
    s_active = true;
    s_diag.starts = increment_saturating(s_diag.starts);
    kick_usb_worker();
    return true;
}
#endif

void usb_service_init(bool vbus_present, uint32_t now_ms) {
    s_initialized = true;
    s_phy_parked = false;
#if SISU_RELEASE_BUILD
    (void)vbus_present;
    (void)now_ms;
    /* The release image never attaches the RP USB device. Keep the controller
     * reset and the PHY powered down even when the service cable is present;
     * direct GP28 remains available only to the boot-time USB+Power BOOTSEL
     * gate. */
    park_phy();
#else
    s_active = false;
    s_retry_pending = false;
    s_retry_at_ms = now_ms;
    s_attach_failures = 0u;
    s_worker_event_pending_streak = 0u;
    s_diag = (usb_service_diag_t){0};

    if (vbus_present) {
        if (!start_usb(true)) {
            s_attach_failures++;
            s_retry_pending = true;
            s_retry_at_ms = now_ms + USB_ATTACH_RETRY_MS;
        }
    } else {
        park_phy();
    }
#endif
}

void usb_service_poll(bool vbus_present, uint32_t now_ms) {
#if SISU_RELEASE_BUILD
    (void)vbus_present;
    (void)now_ms;
    if (!s_initialized) {
        usb_service_init(false, 0u);
    } else if (!s_phy_parked) {
        park_phy();
    }
#else
    if (!s_initialized) {
        usb_service_init(vbus_present, now_ms);
        return;
    }
    if (!vbus_present) {
        s_retry_pending = false;
        if (s_active || !s_phy_parked) {
            stop_usb();
        }
        return;
    }
    if (s_active) {
        kick_usb_worker();
        return;
    }
    if (s_retry_pending && time_diff_ms(now_ms, s_retry_at_ms) < 0) {
        return;
    }
    s_retry_pending = false;
    if (!start_usb(false)) {
        s_attach_failures++;
        s_retry_pending = true;
        s_retry_at_ms = now_ms + USB_ATTACH_RETRY_MS;
    }
#endif
}

void usb_service_shutdown(void) {
    if (!s_initialized) {
        s_initialized = true;
        s_phy_parked = false;
#if !SISU_RELEASE_BUILD
        s_active = false;
#endif
    }
#if SISU_RELEASE_BUILD
    if (!s_phy_parked) {
        park_phy();
    }
#else
    s_retry_pending = false;
    if (s_active || !s_phy_parked) {
        stop_usb();
    }
#endif
}

bool usb_service_active(void) {
#if SISU_RELEASE_BUILD
    return false;
#else
    return s_active;
#endif
}

bool usb_service_connected(void) {
#if SISU_RELEASE_BUILD
    return false;
#else
    return s_active && tud_connected();
#endif
}

bool usb_service_mounted(void) {
#if SISU_RELEASE_BUILD
    return false;
#else
    return s_active && tud_mounted();
#endif
}

bool usb_service_host_live(void) {
#if SISU_RELEASE_BUILD
    return false;
#else
    return s_active && tud_connected() && !tud_suspended();
#endif
}

uint32_t usb_service_sie_status(void) {
#if SISU_RELEASE_BUILD
    return 0u;
#else
    return s_active ? usb_hw->sie_status : 0u;
#endif
}

uint32_t usb_service_attach_failures(void) {
#if SISU_RELEASE_BUILD
    return 0u;
#else
    return s_attach_failures;
#endif
}

void usb_service_get_diag(usb_service_diag_t *out) {
    if (out == NULL) {
        return;
    }
#if SISU_RELEASE_BUILD
    *out = (usb_service_diag_t){0};
#else
    uint32_t irq_state = save_and_disable_interrupts();
    out->starts = s_diag.starts;
    out->stops = s_diag.stops;
    out->worker_kicks = s_diag.worker_kicks;
    out->worker_disabled_rearms = s_diag.worker_disabled_rearms;
    out->event_pending_observations = s_diag.event_pending_observations;
    out->event_pending_streak_max = s_diag.event_pending_streak_max;
    out->mounts = s_diag.mounts;
    out->unmounts = s_diag.unmounts;
    out->suspends = s_diag.suspends;
    out->resumes = s_diag.resumes;
    out->line_state_changes = s_diag.line_state_changes;
    out->rx_callbacks = s_diag.rx_callbacks;
    out->line_state = s_diag.line_state;
    restore_interrupts(irq_state);
#endif
}

#if !SISU_RELEASE_BUILD
void tud_mount_cb(void) {
    s_diag.mounts = increment_saturating(s_diag.mounts);
}

void tud_umount_cb(void) {
    s_diag.unmounts = increment_saturating(s_diag.unmounts);
}

void tud_suspend_cb(bool remote_wakeup_en) {
    (void)remote_wakeup_en;
    s_diag.suspends = increment_saturating(s_diag.suspends);
}

void tud_resume_cb(void) {
    s_diag.resumes = increment_saturating(s_diag.resumes);
}

void tud_cdc_line_state_cb(uint8_t itf, bool dtr, bool rts) {
    if (itf != 0u) {
        return;
    }
    s_diag.line_state = (uint8_t)((dtr ? 1u : 0u) |
                                  (rts ? 2u : 0u));
    s_diag.line_state_changes =
        increment_saturating(s_diag.line_state_changes);
}

void tud_cdc_rx_cb(uint8_t itf) {
    if (itf == 0u) {
        s_diag.rx_callbacks = increment_saturating(s_diag.rx_callbacks);
    }
}
#endif

#include "services/power_sleep.h"

#include "sisu_build_config.h"
#include "audio/nau88c22_codec.h"
#include "hal/board.h"
#include "hal/battery_hal.h"
#include "hal/lcd_pcd8544.h"
#include "hal/ltc2959_hal.h"
#include "hal/power_sleep_hal.h"
#include "hal/rtc_alarm_hal.h"
#include "hal/rv8803_hal.h"
#include "hal/tca8418_hal.h"
#include "services/core1_services.h"
#include "services/log.h"
#include "services/modem_service.h"
#include "services/shared_irq_service.h"
#include "services/shared_3v8_service.h"
#include "services/timebase.h"
#include "services/usb_service.h"
#include "storage/store_service.h"
#include "services/phonebook_service.h"

#include "hardware/powman.h"
#include "hardware/regs/powman.h"
#include "hardware/sync.h"
#include "hardware/watchdog.h"
#include "pico/stdlib.h"

/* Scratch register carrying "we entered the dormant off state" across the
 * power cycle. POWMAN scratch survives P1.7 and, unlike most POWMAN registers,
 * takes full-32-bit unpassworded writes; BOOT0..3 belong to the bootrom. */
#define POWER_SLEEP_SCRATCH_IDX 7u
#define POWER_SLEEP_MAGIC 0x0ff51eedu

/* Dormant telemetry (layout documented in power_sleep.h): scratch[7] is the
 * marker, [4]/[5] telemetry, [3] the `wake` command's rw probe. The 2026-07-06
 * floor-hunt bench knobs (VREG-LP VSEL / LPOSC / pad-park A/B, AON-persisted
 * in scratch[6]) were retired after the measurement matrix came back flat --
 * see power_off_design.md 4 and git history (b2229c5) if they are ever
 * needed again. */
#define POWER_SLEEP_SCRATCH_TELEM_INFO 4u
#define POWER_SLEEP_SCRATCH_TELEM_STAMP 5u
#define POWER_SLEEP_TELEM_REFUSED_BITS (1u << 24)
#define POWER_SLEEP_TELEM_ABANDONED_BITS (1u << 25)
#define POWER_SLEEP_TELEM_STAT2_DEAD_BITS (1u << 26)
#define POWER_SLEEP_TELEM_LAST_ABORT_SHIFT 27u
#define POWER_SLEEP_TELEM_LAST_ABORT_MASK (0x1fu << POWER_SLEEP_TELEM_LAST_ABORT_SHIFT)

/* The off conditions must hold uninterrupted this long before committing.
 * It covers UI blanking, the modem rail-cycle tail, and charger-debounce
 * transients. */
#define POWER_SLEEP_SETTLE_MS 2000u
#define POWER_SLEEP_RETRY_BACKOFF_MS 1000u
#define POWER_SLEEP_CORE1_TIMEOUT_MS 100u
/* LTC2959 single-shot conversion time is 3 ms. Dormant entry can reach the
 * boundary during that tiny window; finish it once instead of paying the
 * generic 1 s retry backoff. */
#define POWER_SLEEP_LTC_SETTLE_RETRY_MS 4u
/* The SDK polls WAITING for only 100 CPU iterations even though the request is
 * synchronized through clk_pow (~32 kHz). Give a valid request a real bounded
 * slow-clock window before classifying it as refused. */
#define POWER_SLEEP_POWMAN_WAIT_US 1000u

/* The post-arm STAT2 level check catches a charger ALREADY charging at arm
 * time (edge-triggered GPI would never latch it). Rev B2 routes the BQ25171
 * open-drain STAT pins directly to the TCA; charger-powered 10 k pull-ups
 * disappear on removal and the TCA internal pulls define detached as (1,1).
 * Board-1 bench confirmed detached (1,1) and active (1,0). Keep the bounded
 * stuck-low escape as a fail-safe for a damaged status net: a real charger
 * also closes the debounced charger-present gate before dormant entry. */
#define POWER_SLEEP_STAT2_ABORT_MAX 3u

static power_wake_cause_t s_wake_cause;
static bool s_boot_was_warm;
static power_sleep_evidence_t s_evidence; /* raw pre-consumption reads, for diagnostics */
static bool s_wake_alarm_pending; /* AF latched at boot, captured pre-app_init */
static bool s_vbus_present;
static bool s_ready_since_valid;
static uint32_t s_ready_since_ms;
static uint32_t s_retry_not_before_ms;
static power_sleep_abort_stats_t s_abort_stats;
static power_sleep_runtime_diag_t s_runtime_diag;
static uint8_t s_stat2_abort_streak;
static bool s_legacy_timer_stopped;

static void power_sleep_enter(void);

static void note_abort(power_sleep_abort_cause_t cause) {
    if (s_abort_stats.counts[cause] != UINT16_MAX) {
        s_abort_stats.counts[cause]++;
    }
    s_abort_stats.last_cause = (uint8_t)cause;
    s_abort_stats.last_ms = time_ms();
}

void power_sleep_boot_capture(void) {
    s_runtime_diag.boot_capture_ms = time_ms();
    s_evidence.chip_reset = powman_hw->chip_reset;
    s_evidence.scratch_marker = powman_hw->scratch[POWER_SLEEP_SCRATCH_IDX];
    power_sleep_hal_wake_evidence_t wake_evidence;
    power_sleep_hal_read_wake_evidence(&wake_evidence);
    s_evidence.pwrup0 = wake_evidence.button;
    s_evidence.pwrup1 = wake_evidence.shared_irq;
    s_evidence.pwrup2 = wake_evidence.service_vbus;
    s_evidence.pwrup3 = wake_evidence.unused_slot;
    s_evidence.last_swcore_pwrup = powman_hw->last_swcore_pwrup;
    s_evidence.current_pwrup_req = powman_hw->current_pwrup_req;
    /* Telemetry from the boot that last entered dormant (NOT consumed: the
     * next entry overwrites it, so `wake` can always show the last story). */
    s_evidence.prev_entry_stamp = powman_hw->scratch[POWER_SLEEP_SCRATCH_TELEM_STAMP];
    s_evidence.prev_entry_info = powman_hw->scratch[POWER_SLEEP_SCRATCH_TELEM_INFO];
    s_runtime_diag.previous_entry_count =
        (uint16_t)(s_evidence.prev_entry_info & 0xffffu);
    s_runtime_diag.previous_awake_ms =
        s_evidence.prev_entry_stamp & 0xfffffu;
    s_runtime_diag.previous_abort_count =
        (uint8_t)((s_evidence.prev_entry_info >> 16u) & 0xffu);
    s_runtime_diag.previous_refused =
        (s_evidence.prev_entry_info & POWER_SLEEP_TELEM_REFUSED_BITS) != 0u;
    s_runtime_diag.previous_abandoned =
        (s_evidence.prev_entry_info & POWER_SLEEP_TELEM_ABANDONED_BITS) != 0u;

    bool had_pd = (s_evidence.chip_reset & POWMAN_CHIP_RESET_HAD_SWCORE_PD_BITS) != 0u;
    bool marker = s_evidence.scratch_marker == POWER_SLEEP_MAGIC;
    /* Warm reboot = no power-loss cause and not the dormant path: reflash
     * (BOOTSEL MSC auto-reboot / picotool REBOOT2), watchdog, and debugger
     * boots. The modem 3V8 rail never dropped across such boots. */
    s_boot_was_warm = !had_pd &&
                      (s_evidence.chip_reset & (POWMAN_CHIP_RESET_HAD_POR_BITS |
                                                POWMAN_CHIP_RESET_HAD_BOR_BITS |
                                                POWMAN_CHIP_RESET_HAD_RUN_LOW_BITS)) == 0u;
    /* Two source signals per slot, OR'd: edge STATUS is the per-slot latch,
     * while LAST_SWCORE_PWRUP is the AON record of which source actually
     * performed the power-up. Keep both because simultaneous sources occur. */
    power_sleep_hal_wake_sources_t wake_sources;
    power_sleep_hal_decode_wake_sources(
        &wake_evidence,
        s_evidence.last_swcore_pwrup,
        &wake_sources
    );
    s_wake_cause = power_sleep_decode_wake(
        had_pd,
        marker,
        wake_sources.button,
        wake_sources.shared_irq,
        wake_sources.service_vbus
    );
    s_evidence.cause = s_wake_cause;

    /* Dead-sensor latch: the STAT2 override streak lives in RAM and a wake is
     * a full reboot. If the boot that entered dormant used the override, seed
     * the streak so its first re-entry attempt can override again. COLD boots
     * re-earn it from scratch (the sensor might have been fixed or the board
     * changed). */
    if (s_wake_cause != POWER_WAKE_COLD &&
        (s_evidence.prev_entry_info & POWER_SLEEP_TELEM_STAT2_DEAD_BITS) != 0u) {
        s_stat2_abort_streak = POWER_SLEEP_STAT2_ABORT_MAX;
    }

    /* Consume the evidence so a later unrelated reset cannot misread it, and
     * disarm the wakeup slots (they are re-armed at the next dormant entry). */
    powman_hw->scratch[POWER_SLEEP_SCRATCH_IDX] = 0u;
    power_sleep_hal_disarm_wake_sources();
    /* A P1.x wake inherits the AON timer on LPOSC. Match the pico-sdk 2.3
     * P-state resume path and restore XOSC while it is available, so every
     * later entry performs the same XOSC -> LPOSC transition as a cold boot. */
    if (had_pd) {
        powman_timer_set_1khz_tick_source_xosc();
    }
}

bool power_sleep_boot_was_warm(void) {
    return s_boot_was_warm;
}

power_wake_cause_t power_sleep_wake_cause(void) {
    return s_wake_cause;
}

void power_sleep_boot_probe(void) {
    s_runtime_diag.boot_probe_ms = time_ms();
    s_runtime_diag.boot_capture_to_probe_ms =
        s_runtime_diag.boot_probe_ms - s_runtime_diag.boot_capture_ms;
    if (s_wake_cause != POWER_WAKE_COLD) {
        /* Capture AF BEFORE app_init: the clock app's boot re-arm
         * (rv8803_hal_set_alarm) clears AF, which would destroy the only
         * evidence that the alarm is what woke us. Re-injected in
         * power_sleep_boot_finish() AFTER app_init for the same reason
         * (set_daily_alarm also resets the software alarm event). */
        bool af = false;
        if (rv8803_hal_poll_alarm_flag(&af) && af) {
            s_wake_alarm_pending = true;
        }
    }
    /* Retirement cleanup: older firmware armed the RV-8803 countdown as a
     * charger-discovery heartbeat. Stop and clear it on every boot so an
     * upgrade cannot inherit periodic TF/SYS_INT pulses. Production no longer
     * arms the timer: 60 s failed the expected charger-feedback latency, while
     * a useful <=5 s cadence would erase the P1.7 power saving. */
    s_legacy_timer_stopped = rv8803_hal_stop_periodic_timer();
}

void power_sleep_boot_finish(void) {
    s_runtime_diag.boot_finish_ms = time_ms();
    s_runtime_diag.boot_probe_to_finish_ms =
        s_runtime_diag.boot_finish_ms - s_runtime_diag.boot_probe_ms;
    if (s_wake_alarm_pending) {
        rtc_alarm_hal_force_alarm_event();
    }
    LOGI("power",
         "wake: cause=%u alarm=%u | chip_reset=%08lx scratch=%08lx "
         "pwrup0=%03lx pwrup1=%03lx pwrup2=%03lx last=%02lx req=%02lx",
         (unsigned)s_wake_cause, (unsigned)s_wake_alarm_pending,
         (unsigned long)s_evidence.chip_reset, (unsigned long)s_evidence.scratch_marker,
         (unsigned long)s_evidence.pwrup0, (unsigned long)s_evidence.pwrup1,
         (unsigned long)s_evidence.pwrup2,
         (unsigned long)s_evidence.last_swcore_pwrup,
         (unsigned long)s_evidence.current_pwrup_req);
    LOGI("power",
         "prev dormant entry: #%u cause=%u lastpwrup=%02x awake=%lu ms aborts=%u "
         "refused=%u abandoned=%u",
         (unsigned)(s_evidence.prev_entry_info & 0xFFFFu),
         (unsigned)(s_evidence.prev_entry_stamp >> 28),
         (unsigned)((s_evidence.prev_entry_stamp >> 20) & 0xFFu),
         (unsigned long)(s_evidence.prev_entry_stamp & 0xFFFFFu),
         (unsigned)((s_evidence.prev_entry_info >> 16) & 0xFFu),
         (unsigned)((s_evidence.prev_entry_info >> 24) & 1u),
         (unsigned)((s_evidence.prev_entry_info >> 25) & 1u));
}

void power_sleep_get_evidence(power_sleep_evidence_t *out) {
    if (out != 0) {
        *out = s_evidence;
    }
}

void power_sleep_get_abort_stats(power_sleep_abort_stats_t *out) {
    if (out != 0) {
        *out = s_abort_stats;
    }
}

void power_sleep_get_runtime_diag(power_sleep_runtime_diag_t *out) {
    if (out != 0) {
        *out = s_runtime_diag;
    }
}

void power_sleep_tick(bool phone_off, bool charger_connected, uint32_t now_ms) {
    s_runtime_diag.off_route = phone_off;
    if (!phone_off) {
        /* Any real activity (power-on, alarm ringing, powerup) cancels the
         * settle run. */
        s_ready_since_valid = false;
        /* Re-base the soft-off retry deadline to "now" while powered on so the
         * NEXT power-off entry starts fresh. It is an absolute deadline whose
         * signed compares below invert once a stale value sits >2^31 ms in the
         * past -- after ~24.86 days of uptime (or a stale value from an OFF
         * episode that long ago) that silently blocked dormant entry, pinning
         * the phone at the ~20-30 mA soft-off floor until the 32-bit clock
         * caught up (~another 24.9 days), draining the pack while it looked
         * off. Refreshed here every tick, it cannot go stale across ON time. */
        s_retry_not_before_ms = now_ms;
        s_runtime_diag.eligible = false;
        s_runtime_diag.settling = false;
        s_runtime_diag.retry_backoff = false;
        s_runtime_diag.blocker_mask = 0u;
        s_runtime_diag.settle_elapsed_ms = 0u;
        s_runtime_diag.settle_remaining_ms = 0u;
        return;
    }

    power_sleep_hal_poll(now_ms);
#if SISU_RELEASE_BUILD
    /* The release image keeps GP28 solely as a boot-time USB+Power BOOTSEL
     * observation. A service cable neither blocks P1.7 nor becomes a wake
     * input, and no USB stack can be live. Charger policy remains independent. */
    s_vbus_present = false;
    bool service_vbus_raw = false;
    bool usb_live = false;
#else
    s_vbus_present = power_sleep_hal_service_vbus_present();

    /* A live USB host is a second service-cable truth source. Direct GP28 is
     * the authoritative physical-cable gate, so USB enumeration timing is not
     * correctness-critical. A real unplug idles the
     * bus and TinyUSB flags suspend within ~3 ms, so this cannot re-create
     * the unplug-never-sleeps state; a host-suspended port (laptop asleep)
     * is allowed to dormant. */
    bool usb_live = usb_service_host_live();
    bool service_vbus_raw = power_sleep_hal_service_vbus_raw();
#endif
    /* Edge-mode PWRUP detectors need an inactive baseline. Keep the complete
     * settle window behind released/deasserted wake inputs; this also prevents
     * a power-button wake from being re-armed while that same press is held.
     * The post-arm recheck below still closes the final sample-to-arm race. */
    bool wake_inputs_clear =
        !power_sleep_hal_power_button_asserted() &&
        !power_sleep_hal_shared_irq_asserted() &&
        !service_vbus_raw;
    bool ready = modem_service_is_powered_off() &&
                 shared_3v8_service_owner_mask() == 0u &&
                 !shared_3v8_service_enabled() &&
                 !charger_connected && !s_vbus_present &&
                 !usb_live && wake_inputs_clear;
    uint8_t blockers = 0u;
    if (!modem_service_is_powered_off()) {
        blockers |= POWER_SLEEP_BLOCKER_MODEM;
    }
    if (shared_3v8_service_owner_mask() != 0u ||
        shared_3v8_service_enabled()) {
        blockers |= POWER_SLEEP_BLOCKER_RAIL;
    }
    if (charger_connected) {
        blockers |= POWER_SLEEP_BLOCKER_CHARGER;
    }
    if (s_vbus_present || service_vbus_raw) {
        blockers |= POWER_SLEEP_BLOCKER_VBUS;
    }
    if (usb_live) {
        blockers |= POWER_SLEEP_BLOCKER_USB;
    }
    if (power_sleep_hal_power_button_asserted()) {
        blockers |= POWER_SLEEP_BLOCKER_BUTTON;
    }
    if (power_sleep_hal_shared_irq_asserted()) {
        blockers |= POWER_SLEEP_BLOCKER_SHARED_IRQ;
    }
    s_runtime_diag.blocker_mask = blockers;
    s_runtime_diag.eligible = ready;
    if (!ready) {
        s_ready_since_valid = false;
        /* Also keep the retry-backoff deadline fresh whenever we are not eligible
         * to sleep. Otherwise a continuous OFF episode that never reaches `ready`
         * (e.g. left off on a charger -- charging blocks readiness, so
         * power_sleep_enter never runs to re-arm it) freezes s_retry_not_before_ms
         * at the episode-start value; after >2^31 ms it would block dormant entry
         * once the charger is removed -- the same soft-off-floor drain this fix
         * closes. A genuine post-abort backoff is
         * preserved: that window has ready==true, so this branch does not run. */
        s_retry_not_before_ms = now_ms;
        s_runtime_diag.settling = false;
        s_runtime_diag.retry_backoff = false;
        s_runtime_diag.settle_elapsed_ms = 0u;
        s_runtime_diag.settle_remaining_ms = 0u;
        return;
    }
    if (!s_ready_since_valid) {
        s_ready_since_valid = true;
        s_ready_since_ms = now_ms;
    }
    uint32_t settle_elapsed = now_ms - s_ready_since_ms;
    s_runtime_diag.settle_elapsed_ms = settle_elapsed;
    s_runtime_diag.settle_remaining_ms =
        settle_elapsed >= POWER_SLEEP_SETTLE_MS
            ? 0u : POWER_SLEEP_SETTLE_MS - settle_elapsed;
    s_runtime_diag.settling = s_runtime_diag.settle_remaining_ms != 0u;
    s_runtime_diag.retry_backoff =
        time_diff_ms(now_ms, s_retry_not_before_ms) < 0;
    if (time_diff_ms(now_ms, s_ready_since_ms + POWER_SLEEP_SETTLE_MS) < 0 ||
        time_diff_ms(now_ms, s_retry_not_before_ms) < 0) {
        return;
    }

    s_runtime_diag.entry_attempts++;
    s_runtime_diag.last_attempt_ms = now_ms;
    s_runtime_diag.last_entry_latency_ms = now_ms - s_ready_since_ms;
    power_sleep_enter(); /* returns only on abort */
    s_runtime_diag.returned_aborts++;
    s_retry_not_before_ms = now_ms + POWER_SLEEP_RETRY_BACKOFF_MS;
    s_ready_since_valid = false;
    s_runtime_diag.settling = false;
    s_runtime_diag.retry_backoff = true;
}

static void power_sleep_enter(void) {
    /* ---- Fallible, rollbackable steps first: an abort here leaves the
     * soft-off state fully functional. ---- */
    if (!rtc_alarm_hal_alarm_config_committed() ||
        rtc_alarm_hal_datetime_write_pending()) {
        /* Most transient failures recover in rtc_alarm_hal's 250 ms service
         * loop during the 2 s settle window. Never cross the full-RAM-loss
         * boundary while a wall-clock write, daily alarm, disable, or temporary
         * snooze compare is still only an intention in RP RAM. */
        note_abort(POWER_SLEEP_ABORT_ALARM_CONFIG);
        LOGW("power", "dormant entry aborted: RTC config pending");
        return;
    }
    if (!phonebook_service_idle() || !store_service_flush_all()) {
        note_abort(POWER_SLEEP_ABORT_FLUSH);
        LOGW("power", "dormant entry aborted: store flush failed");
        return;
    }

    shared_irq_drain_result_t irq_drain;
    if (!shared_irq_service_drain_now(&irq_drain)) {
        note_abort(POWER_SLEEP_ABORT_SHARED_DRAIN);
        LOGW("power",
             "dormant entry aborted: shared IRQ drain stuck=%u err=%02x",
             (unsigned)irq_drain.stuck,
             (unsigned)irq_drain.error_mask);
        return;
    }

    bool af = false;
    if (!rv8803_hal_poll_alarm_flag(&af)) {
        note_abort(POWER_SLEEP_ABORT_ALARM_READ);
        LOGW("power", "dormant entry aborted: alarm flag read failed");
        return;
    }
    if (rtc_alarm_hal_alarm_event_pending() || af) {
        /* Alarm just latched: let the awake poll ring it instead. */
        note_abort(POWER_SLEEP_ABORT_ALARM_PENDING);
        LOGW("power", "dormant entry aborted: alarm pending");
        return;
    }
    if (!s_legacy_timer_stopped) {
        s_legacy_timer_stopped = rv8803_hal_stop_periodic_timer();
        if (!s_legacy_timer_stopped) {
            note_abort(POWER_SLEEP_ABORT_LEGACY_TIMER_STOP);
            LOGW("power", "dormant entry aborted: legacy RTC timer still armed");
            return;
        }
    }
    bool ltc_ready = ltc2959_hal_prepare_dormant();
    if (!ltc_ready) {
        busy_wait_ms(POWER_SLEEP_LTC_SETTLE_RETRY_MS);
        ltc2959_hal_poll(time_ms(), LTC2959_SAMPLE_DEFAULT_PERIOD_MS);
        ltc_ready = ltc2959_hal_prepare_dormant();
    }
    if (!ltc_ready) {
        note_abort(POWER_SLEEP_ABORT_LTC_PREP);
        LOGW("power", "dormant entry aborted: LTC smart-sleep failed");
        return;
    }
    if (!tca8418_hal_arm_charger_wake_interrupt()) {
        /* The arm routine is a multi-register transaction and may have changed
         * interrupt/configuration registers before the failing transfer. */
        (void)tca8418_hal_init();
        (void)ltc2959_hal_resume_active(time_ms());
        note_abort(POWER_SLEEP_ABORT_TCA_ARM);
        LOGW("power", "dormant entry aborted: TCA wake arm failed");
        return;
    }

    /* Arm every edge detector after the inactive settle window, then sample
     * the levels again. An assertion racing this window is therefore either
     * visible in the raw recheck or latched by POWMAN. */
    power_sleep_hal_arm_wake_sources();

    bool button_low = power_sleep_hal_power_button_asserted();
    bool sys_int_low = power_sleep_hal_shared_irq_asserted();
#if SISU_RELEASE_BUILD
    bool vbus = false;
    bool usb_live = false;
#else
    bool vbus = power_sleep_hal_service_vbus_raw();
#endif
    /* GP43 has no P1.7 wake capability. Sample it as late as possible before
     * commit so a full-pack charger whose STAT pins remain idle cannot be
     * hidden between the one-minute maintenance samples. This is deliberately raw:
     * a false positive merely delays sleep. Once P1.7 is committed this ADC
     * cannot wake the RP; an already-full pack plugged in later deliberately
     * receives no immediate UI response on Rev B2. */
    bool charger_input = battery_hal_charger_input_present_now(time_ms());
    bool stat2_high = true;
    bool reads_ok =
        tca8418_hal_input_level(TCA8418_PIN_COL4_CHR_STAT2, &stat2_high);
#if !SISU_RELEASE_BUILD
    bool usb_live = usb_service_host_live();
#endif
    bool stat2_overridden = false;
    if (reads_ok && !button_low && !sys_int_low &&
        !vbus && !charger_input && !usb_live && !stat2_high &&
        s_stat2_abort_streak >= POWER_SLEEP_STAT2_ABORT_MAX) {
        /* STAT2 has read low across repeated arm attempts while the debounced
         * charger decode stayed not-connected (gate precondition): dead
         * sensor, not a charger -- proceed (see POWER_SLEEP_STAT2_ABORT_MAX). */
        LOGW("power", "STAT2 stuck low with no charger decode: sleeping anyway");
        stat2_overridden = true;
    }
    power_sleep_arm_result_t arm_result =
        power_sleep_check_armed_levels(
            button_low,
            sys_int_low,
            vbus || charger_input,
            usb_live,
            reads_ok,
            stat2_high,
            stat2_overridden
        );
    if (arm_result != POWER_SLEEP_ARM_CLEAR) {
        power_sleep_hal_disarm_wake_sources();
        (void)tca8418_hal_init(); /* restore the keypad config */
        (void)ltc2959_hal_resume_active(time_ms());
        if (reads_ok && !button_low && !sys_int_low &&
            !vbus && !charger_input && !usb_live && !stat2_high) {
            s_stat2_abort_streak++;
        } else {
            s_stat2_abort_streak = 0u;
        }
        note_abort(
            arm_result == POWER_SLEEP_ARM_BUTTON_LOW
                ? POWER_SLEEP_ABORT_LEVEL_BUTTON
            : arm_result == POWER_SLEEP_ARM_SYS_INT_LOW
                ? POWER_SLEEP_ABORT_LEVEL_SYS_INT
            : arm_result == POWER_SLEEP_ARM_LEVEL_READ_FAILED
                ? POWER_SLEEP_ABORT_LEVEL_READ_FAIL
            : arm_result == POWER_SLEEP_ARM_EXTERNAL_POWER
                ? (charger_input && !vbus && !usb_live
                       ? POWER_SLEEP_ABORT_LEVEL_CHARGER_INPUT
                       : POWER_SLEEP_ABORT_LEVEL_VBUS)
                : POWER_SLEEP_ABORT_LEVEL_STAT2
        );
        LOGW("power", "dormant entry aborted: post-arm level check (%s)",
             button_low ? "power button low"
             : sys_int_low ? "SYS_INT low"
             : !reads_ok  ? "read fail"
             : vbus     ? "VBUS present"
             : charger_input ? "charger input present"
             : usb_live ? "USB mounted"
                        : "STAT2 low");
        return;
    }
    s_stat2_abort_streak = 0u;

    LOGI("power", "entering dormant off state");

    /* ---- Point of no return: recovery from here is only via reboot. ---- */
    /* Phase-3 SLEEP_EN gating is for ordinary core WFI gaps. Carrying its
     * XIP/DMA/I2C0/SPI0/PIO0 mask into a POWMAN P1.7 request leaves the board
     * at 2.40 mA despite a successful switched-core power-down. Restoring the
     * dynamic mask here preserves the on/standby savings while giving POWMAN
     * a clean transition (Rev B2 bench: 2.40 -> 0.84 mA, 2026-08-10). */
    board_set_sleep_gating(false);
    /* Re-assert shared +3V8 OFF. The gate already requires zero owners, but a
     * diagnostic or future direct HAL caller could have changed GP6. This is
     * beyond the point of no return, so clearing ownership is intentional. */
    shared_3v8_service_force_off();
    (void)core1_services_shutdown(POWER_SLEEP_CORE1_TIMEOUT_MS);
    /* Idempotent; also covers the cold-boot-to-off path where power_off()
     * never ran and the codec is still fully powered from init. */
    nau88c22_codec_power_standby();
    board_stop_mclk_output();
    board_set_backlight(false);
    lcd_power_down();
    /* Idempotent in the normal path: runtime GP28 handling already parked USB
     * before the 2 s off-entry settle. Keep the point-of-no-return assertion so
     * no future caller can carry an initialized TinyUSB stack into P1.7. */
    usb_service_shutdown();

    /* Last peripheral use is behind us: latch every pad into its cheapest
     * P1.7 state (see power_sleep_hal_park_pads). */
    power_sleep_hal_park_pads();

    /* A debugger (or its leftover CSYSPWRUPREQ) would otherwise veto the
     * power-down; the AON timer keeps counting on LPOSC; unlocking VREG lets
     * the sequencer drop the regulator to its low-power mode in P1.x. */
    powman_set_debug_power_request_ignored(true);
    powman_timer_set_1khz_tick_source_lposc();
    hw_set_bits(&powman_hw->vreg_ctrl, POWMAN_PASSWORD_BITS | POWMAN_VREG_CTRL_UNLOCK_BITS);

    /* Dormant telemetry: record THIS boot's story (decoded cause, the raw
     * LAST_SWCORE_PWRUP it saw, how long it stayed awake, how many entry
     * aborts) in AON scratch so a later `wake` (after any number of further
     * dormant cycles' overwrites, the LAST one) can reconstruct the previous
     * wake's behavior without a console. */
    uint32_t awake_ms = time_ms();
    if (awake_ms > 0xFFFFFu) {
        awake_ms = 0xFFFFFu;
    }
    uint32_t abort_total = 0u;
    for (uint8_t i = 0u; i < (uint8_t)POWER_SLEEP_ABORT_CAUSE_COUNT; i++) {
        abort_total += s_abort_stats.counts[i];
    }
    if (abort_total > 255u) {
        abort_total = 255u;
    }
    uint32_t last_abort = abort_total == 0u
        ? 0u
        : ((uint32_t)s_abort_stats.last_cause + 1u)
              << POWER_SLEEP_TELEM_LAST_ABORT_SHIFT;
    uint16_t entry_count =
        (uint16_t)(powman_hw->scratch[POWER_SLEEP_SCRATCH_TELEM_INFO] & 0xFFFFu) + 1u;
    powman_hw->scratch[POWER_SLEEP_SCRATCH_TELEM_STAMP] =
        ((uint32_t)s_wake_cause << 28) |
        ((s_evidence.last_swcore_pwrup & 0xFFu) << 20) | awake_ms;
    powman_hw->scratch[POWER_SLEEP_SCRATCH_TELEM_INFO] =
        (last_abort & POWER_SLEEP_TELEM_LAST_ABORT_MASK) |
        (abort_total << 16) | entry_count |
        (stat2_overridden ? POWER_SLEEP_TELEM_STAT2_DEAD_BITS : 0u);

    powman_hw->scratch[POWER_SLEEP_SCRATCH_IDX] = POWER_SLEEP_MAGIC;
    /* All three wake slots have remained armed since the rollbackable raw
     * level check. Do not clear or re-arm them here: their accumulated status
     * is part of the race closure for this point-of-no-return window. */

    powman_power_state off = POWMAN_POWER_STATE_NONE; /* P1.7: everything off */
    powman_power_state on = POWMAN_POWER_STATE_NONE;
    on = powman_power_state_with_domain_on(on, POWMAN_POWER_DOMAIN_SWITCHED_CORE);
    on = powman_power_state_with_domain_on(on, POWMAN_POWER_DOMAIN_XIP_CACHE);
    on = powman_power_state_with_domain_on(on, POWMAN_POWER_DOMAIN_SRAM_BANK0);
    on = powman_power_state_with_domain_on(on, POWMAN_POWER_DOMAIN_SRAM_BANK1);
    bool config_ok = powman_configure_wakeup_state(off, on);
    /* Reboot into a normal flash boot on wake (the bootrom consumes BOOT0..3). */
    powman_hw->boot[0] = 0u;
    powman_hw->boot[1] = 0u;
    powman_hw->boot[2] = 0u;
    powman_hw->boot[3] = 0u;

    /* Clear a stale abandoned-power-down flag (AON: it survives every reboot
     * short of a battery pull) so the post-WFI check below sees only THIS
     * attempt's outcome. */
    powman_clear_bits(&powman_hw->state, POWMAN_STATE_PWRUP_WHILE_WAITING_BITS);

    int rc = config_ok ? powman_set_power_state(off) : PICO_ERROR_INVALID_ARG;
    if (rc == PICO_ERROR_TIMEOUT) {
        for (uint32_t waited_us = 0u;
             waited_us < POWER_SLEEP_POWMAN_WAIT_US;
             waited_us++) {
            uint32_t state = powman_hw->state;
            if ((state & POWMAN_STATE_WAITING_BITS) != 0u) {
                rc = PICO_OK;
                break;
            }
            if ((state & (POWMAN_STATE_REQ_IGNORED_BITS |
                          POWMAN_STATE_BAD_SW_REQ_BITS)) != 0u) {
                break;
            }
            busy_wait_us(1u);
        }
    }
    if (rc != PICO_OK) {
        /* Refused (typically a wake source already pending) or misconfigured.
         * We are half shut down (core1 parked, USB dead): a clean reboot is
         * the recovery -- the marker is cleared so it decodes as COLD, and
         * whatever asserted SYS_INT gets discovered by the normal pollers. */
        /* Cancel a request that may still be crossing clk_pow before the
         * watchdog reboot. Leaving it live can power down a later boot with
         * the marker already cleared. */
        (void)powman_set_power_state(on);
        powman_hw->scratch[POWER_SLEEP_SCRATCH_TELEM_INFO] |= POWER_SLEEP_TELEM_REFUSED_BITS;
        powman_hw->scratch[POWER_SLEEP_SCRATCH_IDX] = 0u;
        watchdog_reboot(0, 0, 0);
        while (true) {
            tight_loop_contents();
        }
    }
    while (true) {
        __wfi(); /* the sequencer removes power here; wake = full reboot */
        if ((powman_hw->state & POWMAN_STATE_PWRUP_WHILE_WAITING_BITS) != 0u) {
            /* Datasheet 6.2.3.1: a wake source asserted while POWMAN was
             * WAITING for the cores -- the power-down is silently ABANDONED
             * and the core returns from WFI with everything still on.
             * powman_set_power_state already returned OK, so without this
             * check we would spin here forever: half shut down, console
             * dead, ~20-30 mA -- the suspected USB-unplug no-resleep state.
             * Recover exactly like the refuse path: reboot clean; whatever
             * asserted gets discovered by the normal boot/pollers. */
            powman_hw->scratch[POWER_SLEEP_SCRATCH_TELEM_INFO] |=
                POWER_SLEEP_TELEM_ABANDONED_BITS;
            powman_hw->scratch[POWER_SLEEP_SCRATCH_IDX] = 0u;
            watchdog_reboot(0, 0, 0);
            while (true) {
                tight_loop_contents();
            }
        }
    }
}

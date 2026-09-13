#ifndef POWER_SLEEP_H
#define POWER_SLEEP_H

#include <stdbool.h>
#include <stdint.h>

#include "services/power_sleep_logic.h"

/* POWMAN power-off: the RP2350 POWMAN full-off state (P1.7, everything
 * including SRAM powered down; ~tens of uA) behind the soft-off state.
 * Wake = a full bootrom->flash reboot; wake sources are GP7 (power button,
 * falling edge), GP42 (SYS_INT falling edge: RTC alarm, TCA charger status, or
 * LTC2959). The CDC-enabled service image additionally wakes on direct GP28
 * service VBUS; the release image ignores GP28 during normal operation and
 * samples it only after a Power wake for the BOOTSEL gesture. Enabled inputs
 * must remain inactive through the pre-entry settle window before they are
 * armed. Alarm/TCA/LTC sources hold the line until serviced. Pad state is held
 * through the off window by the RP2350 isolation latches (datasheet 9.7) --
 * notably GP6 stays low, keeping the shared +3V8 rail off.
 *
 * Boot integration order (see main.c):
 *   power_sleep_boot_capture()  -- FIRST: consume the POWMAN wake evidence
 *   board_init()                -- I2C up
 *   [BOOTSEL gate at COLD, plus release-image Power wake with GP28 high]
 *   ...
 *   rtc_alarm_hal_init();
 *   power_sleep_boot_probe()    -- AF snapshot BEFORE app_init's alarm re-arm
 *                                  clears AF; also retires any legacy timer
 *   ...
 *   app_init(app);
 *   power_sleep_boot_finish()    -- re-inject the alarm fire AFTER app_init
 */

void power_sleep_boot_capture(void);
power_wake_cause_t power_sleep_wake_cause(void);

/* True when this boot was a WARM reboot: the RP restarted without losing power
 * and not via the dormant path (no POR/BOR/RUN-pin cause, no switched-core
 * power-down). Reflash boots (BOOTSEL MSC auto-reboot, picotool REBOOT2),
 * watchdog reboots, and debugger restarts all read true -- external state that
 * only resets on a real power cycle (for example, persisted modem UART state)
 * survives such boots and may need explicit recovery. */
bool power_sleep_boot_was_warm(void);
void power_sleep_boot_probe(void);
void power_sleep_boot_finish(void);

/* Raw wake evidence as read at boot_capture, BEFORE consumption -- bench
 * diagnostics for the wake-decode chain (the `wake` console command).
 *
 * prev_entry_stamp / prev_entry_info are the DORMANT TELEMETRY: POWMAN
 * scratch[5]/[4] (AON -- survive P1.7 and watchdog reboots), written at every
 * dormant-entry commit and read back here, so the story of the PREVIOUS boot
 * (the one that entered dormant) survives into this one:
 *   stamp (scratch[5]): [31:28] that boot's decoded wake cause,
 *                       [27:20] its raw LAST_SWCORE_PWRUP (low 8 bits),
 *                       [19:0]  ms awake at commit (saturated).
 *   info  (scratch[4]): [31:27] last abort cause + 1 (zero means no abort),
 *                       [26]    the STAT2 dead-sensor override was used at
 *                               this entry (a wake boot seeds the override
 *                               streak from it, so the next wake does not
 *                               re-pay the 3-abort dance),
 *                       [25]    the power-down was ABANDONED by POWMAN (a
 *                               wake source asserted while the state machine
 *                               was WAITING, datasheet 6.2.3.1; caught after
 *                               WFI returned, then watchdog reboot),
 *                       [24]    the commit was REFUSED by POWMAN (set in the
 *                               refuse path just before the watchdog reboot),
 *                       [23:16] entry aborts that boot (saturated),
 *                       [15:0]  monotonic dormant-entry counter. */
typedef struct {
    uint32_t chip_reset;
    uint32_t scratch_marker;
    uint32_t pwrup0;
    uint32_t pwrup1;
    uint32_t pwrup2;
    uint32_t pwrup3;
    uint32_t last_swcore_pwrup;
    uint32_t current_pwrup_req;
    uint32_t prev_entry_stamp;
    uint32_t prev_entry_info;
    power_wake_cause_t cause;
} power_sleep_evidence_t;

void power_sleep_get_evidence(power_sleep_evidence_t *out);

/* Dormant-entry abort diagnostics. Two entry aborts are silent by design
 * (the AF recheck and the post-arm level recheck) and the console is usually
 * dead by the time entry runs (no VBUS) -- so every abort is counted per
 * cause and read back later via the `wake` console command after a USB
 * replug. A phone that "never sleeps" becomes diagnosable post-mortem. */
typedef enum {
    POWER_SLEEP_ABORT_FLUSH = 0,      /* store_service_flush_all failed */
    POWER_SLEEP_ABORT_ALARM_PENDING,  /* RTC AF latched at entry */
    POWER_SLEEP_ABORT_LEGACY_TIMER_STOP, /* inherited timer could not be stopped */
    POWER_SLEEP_ABORT_TCA_ARM,        /* TCA8418 wake-interrupt arm failed */
    POWER_SLEEP_ABORT_LEVEL_READ_FAIL,/* post-arm recheck: TCA read failed */
    POWER_SLEEP_ABORT_LEVEL_VBUS,     /* post-arm recheck: VBUS present */
    POWER_SLEEP_ABORT_LEVEL_STAT2,    /* post-arm recheck: CHR_STAT2 low */
    POWER_SLEEP_ABORT_SHARED_DRAIN,   /* GP42 source drain failed/stuck */
    POWER_SLEEP_ABORT_ALARM_READ,     /* post-drain RV-8803 AF read failed */
    POWER_SLEEP_ABORT_LTC_PREP,       /* smart-sleep/alert mode failed */
    POWER_SLEEP_ABORT_LEVEL_BUTTON,   /* post-arm recheck: button held low */
    POWER_SLEEP_ABORT_LEVEL_SYS_INT,  /* post-arm recheck: GP42 held low */
    POWER_SLEEP_ABORT_LEVEL_CHARGER_INPUT, /* post-arm GP43 VIN present */
    POWER_SLEEP_ABORT_ALARM_CONFIG,   /* requested RTC alarm state not committed */
    POWER_SLEEP_ABORT_CAUSE_COUNT
} power_sleep_abort_cause_t;

typedef struct {
    uint16_t counts[POWER_SLEEP_ABORT_CAUSE_COUNT]; /* saturating */
    uint8_t last_cause;       /* power_sleep_abort_cause_t; valid iff any count */
    uint32_t last_ms;         /* time_ms() of the most recent abort */
} power_sleep_abort_stats_t;

void power_sleep_get_abort_stats(power_sleep_abort_stats_t *out);

enum {
    POWER_SLEEP_BLOCKER_MODEM = 1u << 0,
    POWER_SLEEP_BLOCKER_RAIL = 1u << 1,
    POWER_SLEEP_BLOCKER_CHARGER = 1u << 2,
    POWER_SLEEP_BLOCKER_VBUS = 1u << 3,
    POWER_SLEEP_BLOCKER_USB = 1u << 4,
    POWER_SLEEP_BLOCKER_BUTTON = 1u << 5,
    POWER_SLEEP_BLOCKER_SHARED_IRQ = 1u << 6,
};

typedef struct {
    bool off_route;
    bool eligible;
    bool settling;
    bool retry_backoff;
    uint8_t blocker_mask;
    uint32_t settle_elapsed_ms;
    uint32_t settle_remaining_ms;
    uint32_t entry_attempts;
    uint32_t returned_aborts;
    uint32_t last_attempt_ms;
    uint32_t last_entry_latency_ms;
    uint32_t boot_capture_ms;
    uint32_t boot_probe_ms;
    uint32_t boot_finish_ms;
    uint32_t boot_capture_to_probe_ms;
    uint32_t boot_probe_to_finish_ms;
    uint16_t previous_entry_count;
    uint32_t previous_awake_ms;
    uint8_t previous_abort_count;
    bool previous_refused;
    bool previous_abandoned;
} power_sleep_runtime_diag_t;

void power_sleep_get_runtime_diag(power_sleep_runtime_diag_t *out);

/* Main-loop gate + entry. While `phone_off` is true with the modem fully off,
 * no charger active, and no VBUS (USB debug cable => never sleep), a settle
 * window runs out and the phone drops into the POWMAN off state -- this call
 * then never returns. The app layer projects its route and charger state into
 * the two neutral booleans; the sleep service does not own app_t. All fallible
 * steps (store flush, RTC alarm commit, TCA wake arm, LTC preparation,
 * post-arm level checks) happen before anything destructive, so an abort
 * leaves the soft-off state fully functional. The retired periodic timer is
 * also verified stopped before every commit. */
void power_sleep_tick(bool phone_off, bool charger_connected, uint32_t now_ms);

#endif

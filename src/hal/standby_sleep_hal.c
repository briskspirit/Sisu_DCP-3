#include "hal/standby_sleep_hal.h"

#include <limits.h>
#include <stddef.h>

#include "sisu_build_config.h"
#include "hal/board.h"
#include "hal/board_irq_hal.h"
#include "hal/modem_uart_hal.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/pll.h"
#include "hardware/powman.h"
#include "hardware/regs/addressmap.h"
#include "hardware/regs/otp_data.h"
#include "hardware/regs/clocks.h"
#include "hardware/regs/intctrl.h"
#include "hardware/regs/powman.h"
#include "hardware/regs/rosc.h"
#include "hardware/structs/clocks.h"
#include "hardware/structs/io_bank0.h"
#include "hardware/structs/rosc.h"
#include "hardware/structs/scb.h"
#include "hardware/sync.h"
#include "hardware/xosc.h"
#include "pico/runtime_init.h"

#define STANDBY_SLEEP_LPOSC_NOMINAL_HZ 32768u
#define STANDBY_SLEEP_LPOSC_MIN_HZ 26000u
#define STANDBY_SLEEP_LPOSC_MAX_HZ 40000u
#define STANDBY_SLEEP_ROSC_FALLBACK_HZ 6500000u
#define STANDBY_SLEEP_IRQ_GROUP_COUNT ((NUM_BANK0_GPIOS + 7u) / 8u)

typedef struct {
    uint8_t pin;
    uint8_t event;
    uint32_t wake_bit;
} dormant_source_t;

static const dormant_source_t DORMANT_SOURCES[] = {
    {MODEM_PIN_RI, GPIO_IRQ_EDGE_FALL, STANDBY_SLEEP_HAL_WAKE_MODEM_RI},
    {SYS_INT_PIN, GPIO_IRQ_EDGE_FALL, STANDBY_SLEEP_HAL_WAKE_SHARED_IRQ},
    {POWER_BUTTON_PIN, GPIO_IRQ_EDGE_FALL,
     STANDBY_SLEEP_HAL_WAKE_POWER_BUTTON},
#if !SISU_RELEASE_BUILD
    {SERVICE_VBUS_PIN, GPIO_IRQ_EDGE_RISE,
     STANDBY_SLEEP_HAL_WAKE_SERVICE_VBUS},
#endif
};

static uint32_t s_rosc_hz;
static uint32_t s_lposc_hz;
static bool s_initialized;

static uint32_t calibrated_lposc_hz(void) {
    const volatile uint16_t *calibration =
        (const volatile uint16_t *)OTP_DATA_BASE + OTP_DATA_LPOSC_CALIB_ROW;
    uint32_t frequency_hz = *calibration;
    return frequency_hz >= STANDBY_SLEEP_LPOSC_MIN_HZ &&
                   frequency_hz <= STANDBY_SLEEP_LPOSC_MAX_HZ
               ? frequency_hz
               : STANDBY_SLEEP_LPOSC_NOMINAL_HZ;
}

static void powman_timer_checkpoint(void) {
    /* Preserve the explicit checkpoint used with SDK 2.2.0. SDK 2.3.1 also
     * preserves the counter internally; retaining this keeps the existing
     * standby handoff unchanged while the SDK upgrade is qualified. */
    uint64_t current_ms = powman_timer_get_ms();
    powman_timer_set_ms(current_ms);
    if (!powman_timer_is_running()) {
        powman_timer_start();
    }
}

static uint32_t source_register_bits(const dormant_source_t *source) {
    return (uint32_t)source->event << (4u * (source->pin % 8u));
}

static void dormant_sources_disable(void) {
    for (uint32_t group = 0u; group < STANDBY_SLEEP_IRQ_GROUP_COUNT;
         group++) {
        io_bank0_hw->dormant_wake_irq_ctrl.inte[group] = 0u;
    }
}

static void dormant_sources_ack_mask(uint32_t wake_mask) {
    for (size_t i = 0u; i < sizeof(DORMANT_SOURCES) /
                                    sizeof(DORMANT_SOURCES[0]);
         i++) {
        if ((wake_mask & DORMANT_SOURCES[i].wake_bit) != 0u) {
            gpio_acknowledge_irq(DORMANT_SOURCES[i].pin,
                                 DORMANT_SOURCES[i].event);
        }
    }
}

static void dormant_sources_enable(void) {
    dormant_sources_disable();
    for (size_t i = 0u; i < sizeof(DORMANT_SOURCES) /
                                    sizeof(DORMANT_SOURCES[0]);
         i++) {
        const dormant_source_t *source = &DORMANT_SOURCES[i];
        hw_set_bits(
            &io_bank0_hw->dormant_wake_irq_ctrl.inte[source->pin / 8u],
            source_register_bits(source));
    }
}

static uint32_t gpio_wake_mask(bool dormant_status) {
    uint32_t mask = 0u;
    for (size_t i = 0u; i < sizeof(DORMANT_SOURCES) /
                                    sizeof(DORMANT_SOURCES[0]);
         i++) {
        const dormant_source_t *source = &DORMANT_SOURCES[i];
        uint32_t group = source->pin / 8u;
        uint32_t bits = source_register_bits(source);
        bool pending = (io_bank0_hw->intr[group] & bits) != 0u;
        if (dormant_status) {
            pending = pending ||
                (io_bank0_hw->dormant_wake_irq_ctrl.ints[group] & bits) != 0u;
        }
        if (pending) {
            mask |= source->wake_bit;
        }
    }

    board_irq_snapshot_t snapshot;
    board_irq_hal_get_snapshot(&snapshot);
    if (snapshot.modem_ri_pending) {
        mask |= STANDBY_SLEEP_HAL_WAKE_MODEM_RI;
    }
    if (snapshot.shared_pending) {
        mask |= STANDBY_SLEEP_HAL_WAKE_SHARED_IRQ;
    }
#if !SISU_RELEASE_BUILD
    if (snapshot.service_vbus_pending) {
        mask |= STANDBY_SLEEP_HAL_WAKE_SERVICE_VBUS;
    }
#endif

    if (gpio_get(MODEM_PIN_RI) == 0) {
        mask |= STANDBY_SLEEP_HAL_WAKE_MODEM_RI;
    }
    if (gpio_get(SYS_INT_PIN) == 0) {
        mask |= STANDBY_SLEEP_HAL_WAKE_SHARED_IRQ;
    }
    if (gpio_get(POWER_BUTTON_PIN) == 0) {
        mask |= STANDBY_SLEEP_HAL_WAKE_POWER_BUTTON;
    }
#if !SISU_RELEASE_BUILD
    if (board_service_vbus_present()) {
        mask |= STANDBY_SLEEP_HAL_WAKE_SERVICE_VBUS;
    }
#endif
    return mask;
}

static uint32_t service_and_level_wake_mask(void) {
    uint32_t mask = 0u;
    board_irq_snapshot_t snapshot;
    board_irq_hal_get_snapshot(&snapshot);
    if (snapshot.modem_ri_pending) {
        mask |= STANDBY_SLEEP_HAL_WAKE_MODEM_RI;
    }
    if (snapshot.shared_pending) {
        mask |= STANDBY_SLEEP_HAL_WAKE_SHARED_IRQ;
    }
#if !SISU_RELEASE_BUILD
    if (snapshot.service_vbus_pending) {
        mask |= STANDBY_SLEEP_HAL_WAKE_SERVICE_VBUS;
    }
#endif
    if (gpio_get(MODEM_PIN_RI) == 0) {
        mask |= STANDBY_SLEEP_HAL_WAKE_MODEM_RI;
    }
    if (gpio_get(SYS_INT_PIN) == 0) {
        mask |= STANDBY_SLEEP_HAL_WAKE_SHARED_IRQ;
    }
    if (gpio_get(POWER_BUTTON_PIN) == 0) {
        mask |= STANDBY_SLEEP_HAL_WAKE_POWER_BUTTON;
    }
#if !SISU_RELEASE_BUILD
    if (board_service_vbus_present()) {
        mask |= STANDBY_SLEEP_HAL_WAKE_SERVICE_VBUS;
    }
#endif
    return mask;
}

static void transfer_wake_latches(uint32_t wake_mask) {
    board_irq_hal_latch_dormant_wake(
        (wake_mask & STANDBY_SLEEP_HAL_WAKE_SHARED_IRQ) != 0u,
        (wake_mask & STANDBY_SLEEP_HAL_WAKE_SERVICE_VBUS) != 0u,
        (wake_mask & STANDBY_SLEEP_HAL_WAKE_MODEM_RI) != 0u);
}

static void rosc_set_dormant_local(void) {
    rosc_hw->dormant = ROSC_DORMANT_VALUE_DORMANT;
    while ((rosc_hw->status & ROSC_STATUS_STABLE_BITS) == 0u) {
        tight_loop_contents();
    }
}

static void rosc_restart_local(void) {
    rosc_hw->ctrl = ROSC_CTRL_ENABLE_VALUE_ENABLE << ROSC_CTRL_ENABLE_LSB;
    while ((rosc_hw->status & ROSC_STATUS_STABLE_BITS) == 0u) {
        tight_loop_contents();
    }
}

static void clocks_prepare_dormant(uint32_t *sleep_en0,
                                   uint32_t *sleep_en1,
                                   uint32_t *scr) {
    *sleep_en0 = clocks_hw->sleep_en0;
    *sleep_en1 = clocks_hw->sleep_en1;
    *scr = scb_hw->scr;

    powman_timer_checkpoint();
    powman_timer_set_1khz_tick_source_lposc_with_hz(s_lposc_hz);
    clock_configure_undivided(
        clk_ref, CLOCKS_CLK_REF_CTRL_SRC_VALUE_LPOSC_CLKSRC, 0u,
        s_lposc_hz);
    clock_configure_undivided(
        clk_sys, CLOCKS_CLK_SYS_CTRL_SRC_VALUE_CLKSRC_CLK_SYS_AUX,
        CLOCKS_CLK_SYS_CTRL_AUXSRC_VALUE_ROSC_CLKSRC, s_rosc_hz);
    clock_stop(clk_adc);
    clock_stop(clk_usb);
#if HAS_HSTX
    clock_stop(clk_hstx);
#endif
    clock_configure_undivided(
        clk_peri, 0u, CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLK_SYS,
        s_rosc_hz);
    pll_deinit(pll_sys);
    pll_deinit(pll_usb);
    xosc_disable();

    clocks_hw->sleep_en0 = CLOCKS_SLEEP_EN0_CLK_REF_POWMAN_BITS;
    clocks_hw->sleep_en1 = 0u;
    scb_hw->scr = *scr | ARM_CPU_PREFIXED(SCR_SLEEPDEEP_BITS);
}

static void clocks_restore_after_dormant(uint32_t sleep_en0,
                                         uint32_t sleep_en1,
                                         uint32_t scr) {
    rosc_restart_local();
    clocks_hw->sleep_en0 = sleep_en0;
    clocks_hw->sleep_en1 = sleep_en1;
    scb_hw->scr = scr;

    runtime_init_clocks();
    /* enter() is called after the service re-established the 6 MHz state, so
     * this restores the project's 153.6 MHz tree plus UART/I2C divisors. */
    board_exit_xosc_lowpower();
    /* runtime_init_clocks() briefly restored the unused USB and HSTX clocks;
     * return them to the parked/unused state before scheduling resumes. */
    clock_stop(clk_usb);
#if HAS_HSTX
    clock_stop(clk_hstx);
#endif
    powman_timer_checkpoint();
    powman_timer_set_1khz_tick_source_xosc();
}

void standby_sleep_hal_init(uint64_t now_ms) {
    dormant_sources_disable();
    dormant_sources_ack_mask(UINT32_MAX);
    powman_disable_alarm_wakeup();
    irq_clear(POWMAN_IRQ_TIMER);

    powman_timer_set_1khz_tick_source_xosc();
    powman_timer_set_ms(now_ms);
    if (!powman_timer_is_running()) {
        powman_timer_start();
    }

    uint32_t measured_khz =
        frequency_count_khz(CLOCKS_FC0_SRC_VALUE_ROSC_CLKSRC);
    s_rosc_hz = measured_khz == 0u
                    ? STANDBY_SLEEP_ROSC_FALLBACK_HZ
                    : measured_khz * 1000u;
    s_lposc_hz = calibrated_lposc_hz();
    s_initialized = true;
}

uint32_t standby_sleep_hal_prepare_wake_sources(void) {
    uint32_t irq_state = save_and_disable_interrupts();
    dormant_sources_disable();
    /* Discard raw edge history already consumed by the normal awake path (most
     * notably the polled power button), then sample again. A real source at this
     * boundary is also level-persistent: RI has a bounded pulse, SYS_INT latches
     * low, and a human power press remains low. The service image additionally
     * treats VBUS as persistent. Normal latches cover enabled edges that
     * completed before IRQ masking. */
    uint32_t wake_mask = service_and_level_wake_mask();
    dormant_sources_ack_mask(UINT32_MAX);
    wake_mask |= gpio_wake_mask(false);
    if (wake_mask != 0u) {
        transfer_wake_latches(wake_mask);
    }
    restore_interrupts(irq_state);
    return wake_mask;
}

bool standby_sleep_hal_wake_levels_clear(void) {
    bool clear = gpio_get(MODEM_PIN_RI) != 0 &&
                 gpio_get(SYS_INT_PIN) != 0 &&
                 gpio_get(POWER_BUTTON_PIN) != 0;
#if !SISU_RELEASE_BUILD
    clear = clear && !board_service_vbus_present();
#endif
    return clear;
}

standby_sleep_hal_status_t standby_sleep_hal_enter(
    uint32_t sleep_ms,
    standby_sleep_hal_result_t *result,
    standby_sleep_hal_guard_t *guard) {
    if (result != NULL) {
        *result = (standby_sleep_hal_result_t){0};
    }
    if (guard != NULL) {
        *guard = (standby_sleep_hal_guard_t){0};
    }
    if (result == NULL || guard == NULL || !s_initialized) {
        return STANDBY_SLEEP_HAL_ABORTED;
    }
    guard->saved_irq_state = save_and_disable_interrupts();
    guard->active = true;

    /* Close the service-snapshot-to-arm window at the hardware boundary. The
     * modem is contractually asleep here, but a late FIFO byte or CTS assertion
     * must abort before clk_peri/PL011 are stopped. */
    if (!modem_uart_hal_tx_idle() || !modem_uart_hal_rx_idle() ||
        modem_uart_hal_cts_asserted()) {
        result->wake_mask = STANDBY_SLEEP_HAL_WAKE_UNKNOWN;
        return STANDBY_SLEEP_HAL_ABORTED;
    }

    uint32_t wake_mask = gpio_wake_mask(false);
    if (wake_mask != 0u) {
        transfer_wake_latches(wake_mask);
        result->wake_mask = wake_mask;
        dormant_sources_disable();
        dormant_sources_ack_mask(wake_mask);
        return STANDBY_SLEEP_HAL_ABORTED;
    }

    dormant_sources_enable();
    uint64_t aon_before_ms = powman_timer_get_ms();
    uint64_t alarm_ms = aon_before_ms > UINT64_MAX - sleep_ms
                            ? UINT64_MAX
                            : aon_before_ms + sleep_ms;
    irq_clear(POWMAN_IRQ_TIMER);
    powman_enable_alarm_wakeup_at_ms(alarm_ms);

    /* No ACK occurs between the precheck and this read. An edge in the arm
     * window therefore appears in the dormant controller and aborts safely. */
    wake_mask = gpio_wake_mask(true);
    if ((powman_hw->ints & POWMAN_INTS_TIMER_BITS) != 0u) {
        wake_mask |= STANDBY_SLEEP_HAL_WAKE_MAINTENANCE;
    }
    if (wake_mask != 0u) {
        powman_disable_alarm_wakeup();
        irq_clear(POWMAN_IRQ_TIMER);
        transfer_wake_latches(wake_mask);
        result->wake_mask = wake_mask;
        dormant_sources_disable();
        dormant_sources_ack_mask(wake_mask);
        return STANDBY_SLEEP_HAL_ABORTED;
    }

    /* Keep RTS deasserted across removal/restoration of clk_peri. RI remains an
     * independent wake source and Telit buffers the URC until flow resumes. */
    if (!modem_uart_hal_clock_change_begin()) {
        modem_uart_hal_clock_change_end();
        powman_disable_alarm_wakeup();
        irq_clear(POWMAN_IRQ_TIMER);
        result->wake_mask = STANDBY_SLEEP_HAL_WAKE_UNKNOWN;
        dormant_sources_disable();
        dormant_sources_ack_mask(UINT32_MAX);
        return STANDBY_SLEEP_HAL_ABORTED;
    }

    uint32_t sleep_en0;
    uint32_t sleep_en1;
    uint32_t scr;
    clocks_prepare_dormant(&sleep_en0, &sleep_en1, &scr);
    result->entered_dormant = true;
    rosc_set_dormant_local();

    wake_mask = gpio_wake_mask(true);
    if ((powman_hw->ints & POWMAN_INTS_TIMER_BITS) != 0u) {
        wake_mask |= STANDBY_SLEEP_HAL_WAKE_MAINTENANCE;
    }
    if (wake_mask == 0u) {
        wake_mask = STANDBY_SLEEP_HAL_WAKE_UNKNOWN;
    }
    uint64_t aon_after_ms = powman_timer_get_ms();

    powman_disable_alarm_wakeup();
    irq_clear(POWMAN_IRQ_TIMER);
    dormant_sources_disable();
    dormant_sources_ack_mask(wake_mask);
    transfer_wake_latches(wake_mask);
    clocks_restore_after_dormant(sleep_en0, sleep_en1, scr);
    modem_uart_hal_clock_change_end();

    result->wake_mask = wake_mask;
    result->elapsed_ms = aon_after_ms >= aon_before_ms
                             ? aon_after_ms - aon_before_ms
                             : 0u;
    /* The AON timer was aligned to the firmware wall clock at init and keeps
     * running through both dormant and clock restoration. Returning its final
     * absolute value avoids losing restore/arm time or all of a very short
     * sleep when TIMER0 happened to advance farther than the sleep interval. */
    result->wall_time_ms = powman_timer_get_ms();
    return STANDBY_SLEEP_HAL_WOKE;
}

void standby_sleep_hal_finish(standby_sleep_hal_guard_t *guard) {
    if (guard == NULL || !guard->active) {
        return;
    }
    guard->active = false;
    restore_interrupts(guard->saved_irq_state);
}

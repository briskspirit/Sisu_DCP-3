#include "hal/board.h"
#include "hal/board_safe_gpio.h"
#if SISU_MODEM_BACKEND_ENABLED
#include "hal/modem_uart_hal.h" /* quiesce modem TX/RX before clock reparent */
#endif

#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/i2c.h"
#include "hardware/pll.h"
#include "hardware/pwm.h"
#include "hardware/regs/clocks.h"
#include "hardware/spi.h"
#include "hardware/sync.h"
#include "hardware/uart.h"
#include "pico/bootrom.h"
#include "pico/stdlib.h"

static uint s_backlight_slice;
static uint s_backlight_channel;
static uint8_t s_backlight_duty_percent = 100u;
static bool s_backlight_enabled;

#define BACKLIGHT_PWM_PERIOD_TICKS (BOARD_UI_PWM_WRAP + 1u)

_Static_assert(BACKLIGHT_PWM_PERIOD_TICKS <= UINT16_MAX,
               "backlight PWM period exceeds compare width");

static void apply_backlight_pwm(void) {
    uint32_t active_ticks =
        ((uint32_t)s_backlight_duty_percent * BACKLIGHT_PWM_PERIOD_TICKS +
         50u) /
        100u;
    uint16_t compare;
    if (!s_backlight_enabled) {
        compare = LCD_BACKLIGHT_ACTIVE_LOW
            ? (uint16_t)BACKLIGHT_PWM_PERIOD_TICKS : 0u;
    } else if (LCD_BACKLIGHT_ACTIVE_LOW) {
        compare = (uint16_t)(BACKLIGHT_PWM_PERIOD_TICKS - active_ticks);
    } else {
        compare = (uint16_t)active_ticks;
    }
    pwm_set_chan_level(s_backlight_slice, s_backlight_channel, compare);
}

static void safe_gpio_init(uint8_t pin) {
    gpio_init(pin);
}

static void safe_gpio_put(uint8_t pin, bool level) {
    gpio_put(pin, level);
}

static void safe_gpio_set_dir(uint8_t pin, bool output) {
    gpio_set_dir(pin, output ? GPIO_OUT : GPIO_IN);
}

static void safe_gpio_disable_pulls(uint8_t pin) {
    gpio_disable_pulls(pin);
}

static void safe_gpio_set_input_enabled(uint8_t pin, bool enabled) {
    gpio_set_input_enabled(pin, enabled);
}

static const board_safe_gpio_ops_t SAFE_GPIO_OPS = {
    .init = safe_gpio_init,
    .put = safe_gpio_put,
    .set_dir = safe_gpio_set_dir,
    .disable_pulls = safe_gpio_disable_pulls,
    .set_input_enabled = safe_gpio_set_input_enabled,
};

void board_set_system_clock(void) {
    /* Audio-friendly RP2354 clock tree:
     * XOSC 12 MHz * 128 = 1536 MHz VCO, /5 /2 = 153.6 MHz clk_sys.
     * Codec MCLK is then clk_sys / 25 = 6.144 MHz exactly, and the 16 kHz
     * stereo 16-bit PIO BCLK target is clk_sys / 150 = 1.024 MHz exactly. */
    set_sys_clock_pll(1536000000u, 5u, 2u);
}

/* Quiet backlight-off standby: divide the 12 MHz XOSC to 6 MHz for clk_sys and
 * clk_peri, then stop PLL_SYS. clk_ref remains at 12 MHz, so TIMER0 and all
 * software deadlines keep their normal timebase. PLL_USB remains available for
 * clk_adc; usb_service independently stops clk_usb whenever VBUS is absent.
 *
 * The PL011 divisor at 6 MHz still gives 115200 baud with ~0.16% error. UART and
 * I2C divisors are recalculated on every transition. Enter only when modem TX is
 * idle so no clock reparent lands mid-byte. Idempotent and reversible. */
#define BOARD_QUIET_CLOCK_HZ 6000000u
#define BOARD_XOSC_CLOCK_HZ 12000000u
#define BOARD_FULL_CLOCK_HZ 153600000u

static bool s_xosc_lowpower;
static bool s_clkdown_enabled = true; /* `clkdown` console toggle; default on */

bool board_is_xosc_lowpower(void) {
    return s_xosc_lowpower;
}

bool board_clkdown_enabled(void) {
    return s_clkdown_enabled;
}

void board_set_clkdown_enabled(bool enabled) {
    s_clkdown_enabled = enabled;
}

void board_enter_xosc_lowpower(void) {
    if (s_xosc_lowpower) {
        return;
    }
#if SISU_MODEM_BACKEND_ENABLED
    if (!modem_uart_hal_clock_change_begin()) {
        modem_uart_hal_clock_change_end();
        return;
    }
#endif
    /* Reparent clk_peri and re-set the modem UART baud atomically vs the RX ISR
     * so a URC edge cannot be sampled at the wrong baud during the switch. */
    uint32_t irq = save_and_disable_interrupts();
    clock_configure(clk_peri, 0u, CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_XOSC_CLKSRC,
                    BOARD_XOSC_CLOCK_HZ, BOARD_QUIET_CLOCK_HZ);
#if SISU_MODEM_BACKEND_ENABLED
    uart_set_baudrate(MODEM_UART_ID, MODEM_UART_BAUD);
#endif
    restore_interrupts(irq);
    /* clk_ref remains XOSC 12 MHz; divide only clk_sys, then stop PLL_SYS. */
    clock_configure(clk_sys, CLOCKS_CLK_SYS_CTRL_SRC_VALUE_CLK_REF, 0u,
                    BOARD_XOSC_CLOCK_HZ, BOARD_QUIET_CLOCK_HZ);
    /* The 153.6 MHz divider would slow I2C to ~31 kHz here. A 24-byte LTC2959
     * snapshot then exceeds the bounded 3 ms transaction timeout, invalidating
     * the gauge and blanking the battery bars until full-clock recovery. */
    i2c_set_baudrate(BOARD_I2C_PORT, BOARD_I2C_BAUD_HZ);
    pll_deinit(pll_sys);
    s_xosc_lowpower = true;
#if SISU_MODEM_BACKEND_ENABLED
    modem_uart_hal_clock_change_end();
#endif
}

void board_exit_xosc_lowpower(void) {
    if (!s_xosc_lowpower) {
        return;
    }
#if SISU_MODEM_BACKEND_ENABLED
    /* Full clock is mandatory for USB/audio/flash callers. A timeout means the
     * peer violated RTS; retain the hold and continue rather than strand those
     * callers at 6 MHz. UART health counters expose any resulting wire fault. */
    (void)modem_uart_hal_clock_change_begin();
#endif
    /* Finish any in-flight modem TX at the current 6 MHz-derived baud BEFORE we
     * reparent clk_peri: board_set_system_clock() below moves clk_peri (48 MHz via
     * PLL_USB, then 153.6 MHz) so a byte still shifting out would change bit rate
     * mid-frame and reach the modem corrupted. The entry side is TX-gated at its
     * call site; this makes the exit symmetric and covers ALL exit callers
     * (main-loop, flash-park begin, audio-start prologue) in one place. Bounded so
     * a CTS-stalled/wedged UART can't hang the latency-critical audio/flash paths;
     * ~2 ms >> the typical sub-ms drain, and it only waits when TX is truly busy. */
#if SISU_MODEM_BACKEND_ENABLED
    absolute_time_t tx_drain = make_timeout_time_us(2000);
    while (!modem_uart_hal_tx_idle() && !time_reached(tx_drain)) {
        tight_loop_contents();
    }
#endif
    /* PLL relock + clk_sys -> 153.6 MHz. IRQs stay ON: the ~1 ms PLL lock inside an
     * interrupts-off window would starve the modem RX ISR and drop URC bytes. */
    board_set_system_clock();
    i2c_set_baudrate(BOARD_I2C_PORT, BOARD_I2C_BAUD_HZ);
    /* Mirror the entry side: switch clk_peri to clk_sys (153.6 MHz) and re-baud the
     * modem UART atomically vs the RX ISR. board_set_system_clock/set_sys_clock_pll
     * has ALREADY reparented clk_peri to PLL_USB (48 MHz -- the project doesn't set
     * PICO_CLOCK_ADJUST_PERI_CLOCK_WITH_SYS_CLOCK), leaving the UART baud stale; this
     * reparents it to clk_sys and re-bauds under an IRQ lock so the ISR can't observe
     * a half-reconfigured UART. The only residual is the handful of instructions
     * between set_sys_clock_pll returning and this lock (tens of ns, far shorter than
     * one UART bit even at the wrong rate); closing it fully would mean holding IRQs
     * across the ~1 ms PLL relock, which would starve RX -- a worse trade. */
    uint32_t irq = save_and_disable_interrupts();
    clock_configure(clk_peri, 0u, CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLK_SYS,
                    BOARD_FULL_CLOCK_HZ, BOARD_FULL_CLOCK_HZ);
#if SISU_MODEM_BACKEND_ENABLED
    uart_set_baudrate(MODEM_UART_ID, MODEM_UART_BAUD);
#endif
    restore_interrupts(irq);
    s_xosc_lowpower = false;
#if SISU_MODEM_BACKEND_ENABLED
    modem_uart_hal_clock_change_end();
#endif
}

void board_enter_bootsel_if_power_held(bool (*vbus_probe)(bool *vbus_high)) {
    gpio_init(POWER_BUTTON_PIN);
    gpio_set_dir(POWER_BUTTON_PIN, GPIO_IN);
    gpio_pull_up(POWER_BUTTON_PIN);

    if (gpio_get(POWER_BUTTON_PIN)) {
        return;
    }

    /* Held: BOOTSEL needs BOTH the 5 s hold AND VBUS (see board.h). VBUS is
     * latched once seen because the cold-ramp TCA can read "absent" for
     * seconds; a probe I2C failure counts as present so a wedged bus --
     * exactly when recovery matters most -- cannot lock BOOTSEL out. A NULL
     * probe (standalone diag tools: bench use, USB always attached) assumes
     * present. With a probe, the caller must have run board_init() (I2C). */
    bool vbus_seen = (vbus_probe == 0);
    absolute_time_t bootsel_at = make_timeout_time_ms(POWER_BUTTON_BOOTSEL_HOLD_MS);
    absolute_time_t give_up_at = make_timeout_time_ms(POWER_BUTTON_BOOTSEL_GATE_MAX_MS);
    while (!time_reached(give_up_at)) {
        if (gpio_get(POWER_BUTTON_PIN)) {
            return; /* released before qualifying: continue the normal boot */
        }
        if (!vbus_seen) {
            bool vbus = false;
            vbus_seen = !vbus_probe(&vbus) || vbus;
        }
        if (vbus_seen && time_reached(bootsel_at)) {
            board_reset_to_bootsel();
        }
        sleep_ms(SYSTEM_TICK_MS);
    }
}

void board_reset_to_bootsel(void) {
    reset_usb_boot(0, 0);
}

static bool s_i2c_bus_was_stuck;

/* Shared-bus release: a warm reboot (reflash, watchdog) can interrupt the old
 * firmware mid-I2C-transaction; the RP resets but the TCA/RTC/codec do not,
 * and a slave abandoned mid-READ holds SDA low until it sees enough clocks --
 * classic stuck-bus. Bit-bang up to 9 SCL cycles + a STOP before handing the
 * pins to the I2C block so the first real transaction starts on a released
 * bus. No-op (SDA already high) on clean boots. */
static void board_i2c_bus_clear(void) {
    gpio_init(BOARD_PIN_I2C_SDA);
    gpio_init(BOARD_PIN_I2C_SCL);
    gpio_set_dir(BOARD_PIN_I2C_SDA, GPIO_IN);
    gpio_set_dir(BOARD_PIN_I2C_SCL, GPIO_IN);
    gpio_pull_up(BOARD_PIN_I2C_SDA);
    gpio_pull_up(BOARD_PIN_I2C_SCL);
    busy_wait_us(10);
    if (gpio_get(BOARD_PIN_I2C_SDA)) {
        return; /* bus already released */
    }
    s_i2c_bus_was_stuck = true;
    for (int i = 0; i < 9 && !gpio_get(BOARD_PIN_I2C_SDA); i++) {
        /* Open-drain clocking: drive low, release high (pull-up rises it). */
        gpio_set_dir(BOARD_PIN_I2C_SCL, GPIO_OUT);
        gpio_put(BOARD_PIN_I2C_SCL, false);
        busy_wait_us(5); /* ~100 kHz half-period */
        gpio_set_dir(BOARD_PIN_I2C_SCL, GPIO_IN);
        busy_wait_us(5);
    }
    /* Manufacture a STOP (SDA low->high while SCL high) to reset slave FSMs. */
    gpio_set_dir(BOARD_PIN_I2C_SDA, GPIO_OUT);
    gpio_put(BOARD_PIN_I2C_SDA, false);
    busy_wait_us(5);
    gpio_set_dir(BOARD_PIN_I2C_SCL, GPIO_IN); /* SCL high */
    busy_wait_us(5);
    gpio_set_dir(BOARD_PIN_I2C_SDA, GPIO_IN); /* SDA rises: STOP */
    busy_wait_us(5);
}

bool board_i2c_bus_was_stuck(void) {
    return s_i2c_bus_was_stuck;
}

void board_init(void) {
    /* This must be the first board-side operation. In particular, GP6/33/38/39
     * must have a low output latch before output-enable is asserted. */
    board_safe_gpio_apply(&SAFE_GPIO_OPS);

    board_i2c_bus_clear();
    i2c_init(BOARD_I2C_PORT, BOARD_I2C_BAUD_HZ);
    gpio_set_function(BOARD_PIN_I2C_SDA, GPIO_FUNC_I2C);
    gpio_set_function(BOARD_PIN_I2C_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(BOARD_PIN_I2C_SDA);
    gpio_pull_up(BOARD_PIN_I2C_SCL);

    spi_init(LCD_SPI_PORT, LCD_SPI_BAUD_HZ);
    /* PCD8544 is write-only SPI mode 0 (CPOL=0/CPHA=0), 8-bit, MSB-first. Set it
     * explicitly rather than relying on the SDK power-up default. */
    spi_set_format(LCD_SPI_PORT, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    gpio_set_function(LCD_PIN_SCK, GPIO_FUNC_SPI);
    gpio_set_function(LCD_PIN_MOSI, GPIO_FUNC_SPI);

    /* Boot leaves the modem rail disabled and forced-PWM mode off. The
     * safe policy above has already established both levels without a glitch. */
    board_3v8_rail_set_enabled(false);
    board_3v8_rail_set_force_pwm(false);

    gpio_set_function(LCD_PIN_BACKLIGHT, GPIO_FUNC_PWM);
    s_backlight_slice = pwm_gpio_to_slice_num(LCD_PIN_BACKLIGHT);
    s_backlight_channel = pwm_gpio_to_channel(LCD_PIN_BACKLIGHT);
    pwm_config backlight_cfg = pwm_get_default_config();
    /* GP22 vibra and GP23 backlight are the two channels of one PWM slice.
     * This is the sole configuration owner: pwm_init() clears BOTH channel
     * compares, so core1's later vibra init must only claim its GPIO/channel.
     * 153.6 MHz / 16 / 256 = 37.5 kHz. */
    pwm_config_set_wrap(&backlight_cfg, BOARD_UI_PWM_WRAP);
    pwm_config_set_clkdiv_int(&backlight_cfg, BOARD_UI_PWM_CLKDIV_INT);
    bool vibra_invert_a =
        pwm_gpio_to_channel(VIBRA_PIN) == PWM_CHAN_A &&
        VIBRA_ACTIVE_LOW != 0u;
    bool vibra_invert_b =
        pwm_gpio_to_channel(VIBRA_PIN) == PWM_CHAN_B &&
        VIBRA_ACTIVE_LOW != 0u;
    pwm_config_set_output_polarity(
        &backlight_cfg, vibra_invert_a, vibra_invert_b);
    pwm_init(s_backlight_slice, &backlight_cfg, true);
    s_backlight_duty_percent = 100u;
    s_backlight_enabled = false;
    board_set_backlight(false);

    clock_gpio_init_int_frac8(
        AUDIO_CODEC_PIN_MCLK,
        CLOCKS_CLK_GPOUT0_CTRL_AUXSRC_VALUE_CLK_SYS,
        25u,
        0u
    );

    /* GP42 is the shared active-low interrupt. Its board pull-up is external;
     * the safe policy leaves the RP input enabled with no internal pull. */

    board_prune_unused_clocks();
    /* R3b sleep gating is NOT armed here anymore. Root cause of the dead-
     * post-flash boot (beacon bisect v8, 2026-07-10): a REBOOT2-class reboot
     * fires while the old core parks in WFI, and the clock controller's sleep
     * qualification can come out of that reset STALE ("asleep"). SLEEP_EN
     * resets to all-enabled so the stale state is harmless -- until this
     * function cleared SLEEP_EN_XIP, which then gated the FLASH CLOCK
     * immediately and killed core0 mid-fetch (instant positional death,
     * POR-immune, battery-pull recovery). The main loop arms it after the
     * first genuine WFI cycle has resynced the qualification. */
}

void board_set_backlight(bool enabled) {
    s_backlight_enabled = enabled;
    apply_backlight_pwm();
}

void board_set_backlight_duty_percent(uint8_t duty_percent) {
    s_backlight_duty_percent = duty_percent > 100u ? 100u : duty_percent;
    apply_backlight_pwm();
}

void board_3v8_rail_set_enabled(bool enabled) {
    /* +3V8 is shared with the magnetic buzzer, so a NONE modem backend must not
     * clamp it off. Raising this rail alone cannot start the cellular module:
     * its separate ON/OFF input remains deasserted and modem-service-owned. */
    /* GP38/GP39 high asserts their external low-side FETs. Establish inactive
     * control latches before a rail can rise, and return every PMIC control to
     * its lowest-risk state when the rail is cut. */
    gpio_put(MODEM_PIN_ON_OFF, false);
    gpio_put(MODEM_PIN_HW_SHUTDOWN, false);
    if (!enabled) {
        gpio_put(PMIC_3V8_PIN_FORCE_PWM, false);
    }
    gpio_put(PMIC_3V8_PIN_ENABLE, enabled);
}

bool board_3v8_rail_power_good(void) {
    return gpio_get(PMIC_3V8_PIN_POWER_GOOD);
}

void board_3v8_rail_set_force_pwm(bool enabled) {
    gpio_put(PMIC_3V8_PIN_FORCE_PWM, enabled);
}

bool board_3v8_rail_enabled(void) {
    return gpio_get_out_level(PMIC_3V8_PIN_ENABLE);
}

bool board_3v8_rail_force_pwm_enabled(void) {
    return gpio_get_out_level(PMIC_3V8_PIN_FORCE_PWM);
}

bool board_modem_dtr_level(void) {
    return gpio_get_out_level(MODEM_PIN_DTR);
}

bool board_modem_ri_level(void) {
    return gpio_get(MODEM_PIN_RI);
}

bool board_modem_on_off_asserted(void) {
    return gpio_get_out_level(MODEM_PIN_ON_OFF);
}

bool board_modem_hw_shutdown_asserted(void) {
    return gpio_get_out_level(MODEM_PIN_HW_SHUTDOWN);
}

bool board_service_vbus_present(void) {
    bool level = gpio_get(SERVICE_VBUS_PIN);
    return SERVICE_VBUS_ACTIVE_HIGH ? level : !level;
}

void board_stop_mclk_output(void) {
    /* GP21 is clk_gpout0 (see the clock_gpio_init in board_init); disabling the
     * gpout leaves the pad function selected but statically low. */
    clock_stop(clk_gpout0);
}

void board_start_mclk_output(void) {
    /* Mirror of the board_init() MCLK setup: clk_sys / 25 = 6.144 MHz. Used by
     * the audio idle gate to restart MCLK before a sound plays. */
    clock_gpio_init_int_frac8(
        AUDIO_CODEC_PIN_MCLK,
        CLOCKS_CLK_GPOUT0_CTRL_AUXSRC_VALUE_CLK_SYS,
        25u,
        0u
    );
}

void board_prune_unused_clocks(void) {
    /* Unused-peripheral clock gating: WAKE_EN/SLEEP_EN reset to
     * all-enabled, so every peripheral
     * block this board never uses burns clock-tree power for nothing
     * (research estimate ~1.5-2.5 mA total). Gate the never-used set in BOTH
     * masks; the datasheet guarantees gate/ungate is glitch-free with state
     * retained (6.5.1). Deliberately KEPT: ADC (sense inputs), PIO0 (codec
     * I2S), PIO1 (modem I2S), PWM (backlight/buzzer/vibra), DMA, TIMER0,
     * WATCHDOG (core1 wedge reboot), USB pair (console; dynamic VBUS gating
     * needs a stdio lifecycle and is deferred to R3), SRAM banks, and all
     * fabric/infra clocks. JTAG is gated too -- this bench debugs over USB;
     * re-enable here first if an SWD probe ever misbehaves. Full SLEEP_EN
     * programming (gating in-use blocks during WFI) is the separate R3 step. */
    const uint32_t unused0 = CLOCKS_WAKE_EN0_CLK_HSTX_BITS |
                             CLOCKS_WAKE_EN0_CLK_SYS_HSTX_BITS |
                             CLOCKS_WAKE_EN0_CLK_SYS_I2C1_BITS |
                             CLOCKS_WAKE_EN0_CLK_SYS_JTAG_BITS |
                             CLOCKS_WAKE_EN0_CLK_SYS_PIO2_BITS |
                             CLOCKS_WAKE_EN0_CLK_SYS_SHA256_BITS;
    const uint32_t unused1 = CLOCKS_WAKE_EN1_CLK_PERI_SPI1_BITS |
                             CLOCKS_WAKE_EN1_CLK_SYS_SPI1_BITS |
                             CLOCKS_WAKE_EN1_CLK_PERI_UART1_BITS |
                             CLOCKS_WAKE_EN1_CLK_SYS_UART1_BITS |
                             CLOCKS_WAKE_EN1_CLK_SYS_TBMAN_BITS |
                             CLOCKS_WAKE_EN1_CLK_SYS_TIMER1_BITS |
                             CLOCKS_WAKE_EN1_CLK_SYS_TRNG_BITS;
    hw_clear_bits(&clocks_hw->wake_en0, unused0);
    hw_clear_bits(&clocks_hw->wake_en1, unused1);
    hw_clear_bits(&clocks_hw->sleep_en0, unused0);
    hw_clear_bits(&clocks_hw->sleep_en1, unused1);
}

void board_set_sleep_gating(bool enabled) {
    /* Sleep-state clock gating: extra clock gating that applies ONLY while the chip is in
     * the SLEEP state (both cores in WFE/WFI + no outstanding DMA -- reached in
     * the gaps between core0's 8 ms ticks now that core1 parks in WFE and the
     * audio gate stops the free-running I2S DMA). SLEEP_EN masks have no effect
     * during normal (WAKE) operation, so toggling this never disturbs a running
     * peripheral -- the only question is whether waking from a gated window
     * restores cleanly, which is the bench URC-soak's job. Gated set (all
     * provably idle during a genuine sleep window):
     *   DMA  -- the SLEEP condition itself requires no outstanding transfer
     *   I2C0/SPI0 -- core0's blocking transactions complete before it sleeps
     *   PIO0 -- codec I2S, parked by the audio gate when idle
     *   XIP  -- the big win (~5.8 mA); the wake path restores WAKE_EN before
     *           the core fetches from flash (datasheet SLEEP sequencing)
     * NOT gated: PIO1 (modem voice I2S -- SM-alignment risk, our worst bug
     * class), UART0 (modem RX), IO/PADS, PWM (backlight flicker), TIMER0,
     * SRAM, bus fabric, PLLs, USB. Default off; `sleepen` console toggle. */
    const uint32_t gate0 = CLOCKS_SLEEP_EN0_CLK_SYS_DMA_BITS |
                           CLOCKS_SLEEP_EN0_CLK_SYS_I2C0_BITS |
                           CLOCKS_SLEEP_EN0_CLK_SYS_PIO0_BITS;
    const uint32_t gate1 = CLOCKS_SLEEP_EN1_CLK_SYS_SPI0_BITS |
                           CLOCKS_SLEEP_EN1_CLK_SYS_XIP_BITS;
    if (enabled) {
        hw_clear_bits(&clocks_hw->sleep_en0, gate0);
        hw_clear_bits(&clocks_hw->sleep_en1, gate1);
    } else {
        hw_set_bits(&clocks_hw->sleep_en0, gate0);
        hw_set_bits(&clocks_hw->sleep_en1, gate1);
    }
}

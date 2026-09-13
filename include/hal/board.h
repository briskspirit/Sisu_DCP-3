#ifndef BOARD_H
#define BOARD_H

#include <stdbool.h>
#include <stdint.h>

/* Rev B2 is the sole supported production hardware. */
#if defined(SISU_HW_REV_B2)
#define MODEM_STATUS_MONITOR_AVAILABLE 1u
#else
#error "Sisu firmware supports Rev B2 only"
#endif

/* Hardware code only needs the neutral transport capability. Vendor identity
 * stays in the composition root and services layer. */
#if !defined(SISU_MODEM_BACKEND_ENABLED)
#error "select whether the modem backend is enabled"
#elif SISU_MODEM_BACKEND_ENABLED != 0 && SISU_MODEM_BACKEND_ENABLED != 1
#error "SISU_MODEM_BACKEND_ENABLED must be 0 or 1"
#endif

#define SYSTEM_TICK_MS 8u

#define LCD_SPI_PORT spi0
#define LCD_SPI_BAUD_HZ 4000000u
#define LCD_PIN_SCK 18u
#define LCD_PIN_MOSI 19u
#define LCD_PIN_CS 17u
#define LCD_PIN_DC 20u
#define LCD_PIN_RST 16u
#define LCD_PIN_BACKLIGHT 23u
#define LCD_BACKLIGHT_ACTIVE_LOW 0u
#define BOARD_UI_PWM_WRAP 255u
#define BOARD_UI_PWM_CLKDIV_INT 16u
#define LCD_ROTATE_180 0u
/* Nokia NSE-8 v6.00 stock LCD fallback: EEPROM calibration level 16 maps to
 * Vop 47+16=63; TC1 and bias 4 are fixed in both original init callers. The
 * calibration service reapplies the stored raw Vop after NVM init. */
#define LCD_CONTRAST 0x3fu
#define LCD_TEMPERATURE_COEFFICIENT 1u
#define LCD_BIAS_SYSTEM 4u

#define BOARD_I2C_PORT i2c0
#define BOARD_I2C_BAUD_HZ 400000u
#define BOARD_PIN_I2C_SDA 24u
#define BOARD_PIN_I2C_SCL 25u
/* Shared I2C bus devices: TCA8418 0x34 (keypad), NAU88C22 0x1A (codec),
 * RV-8803 0x32 (RTC), and LTC2959 0x63 (battery monitor). Every transfer MUST
 * be time-bounded with BOARD_I2C_TIMEOUT_US so a wedged bus (SDA stuck low)
 * cannot hang the main loop. All transfers run on core 0 only. */
#define BOARD_I2C_TIMEOUT_US 3000u

#define TCA8418_I2C_ADDR 0x34u

#define MODEM_UART_ID uart0
#define MODEM_UART_BAUD 115200u
#define MODEM_PIN_TX 0u
#define MODEM_PIN_RX 1u
#define MODEM_PIN_CTS 2u
#define MODEM_PIN_RTS 3u
#define PMIC_3V8_PIN_POWER_GOOD 4u
#define PMIC_3V8_PIN_ENABLE 6u
#define POWER_BUTTON_PIN 7u
/* BOOTSEL entry = power button held 5 s WITH service VBUS present on direct
 * GP28 or a live USB host. The CDC-enabled image applies the gate at cold boot.
 * The release image also applies it after a GP7 dormant wake, enabling the
 * cable-first, then Power-hold recovery gesture without making GP28 a normal
 * release wake source. */
#define POWER_BUTTON_BOOTSEL_HOLD_MS 5000u
#define POWER_BUTTON_BOOTSEL_GATE_MAX_MS 12000u

#define MODEM_I2S_PIN_CLK 8u
#define MODEM_I2S_PIN_WA 9u
#define MODEM_I2S_PIN_TXD 10u
#define MODEM_I2S_PIN_RXD 11u

#define AUDIO_CODEC_PIN_ADCOUT 12u
#define AUDIO_CODEC_PIN_LRC 13u
#define AUDIO_CODEC_PIN_BCLK 14u
#define AUDIO_CODEC_PIN_DIN 15u
#define AUDIO_CODEC_PIN_MCLK 21u

#define AUDIO_PIN_BCLK AUDIO_CODEC_PIN_BCLK
#define AUDIO_PIN_LRC AUDIO_CODEC_PIN_LRC
#define AUDIO_PIN_DIN AUDIO_CODEC_PIN_DIN
#define AUDIO_PIN_ADCOUT AUDIO_CODEC_PIN_ADCOUT
#define AUDIO_SAMPLE_RATE 16000u
#define AUDIO_MCLK_HZ 6144000u

/* Vibra motor PWM (Nokia 8210 motor through a MOSFET/driver). */
#define VIBRA_PIN 22u
#define VIBRA_PIN_CONFIGURED 1u
#define VIBRA_ACTIVE_LOW 0u

/* Magnetic ring buzzer (CMT-1085-85-SMT, external-drive transducer). */
#define BUZZER_PIN 5u
#define BUZZER_PIN_CONFIGURED 1u
#define BUZZER_ACTIVE_LOW 0u

#define SERVICE_VBUS_PIN 28u
#define SERVICE_VBUS_ACTIVE_HIGH 1u

#define PMIC_3V8_PIN_FORCE_PWM 33u
#define MODEM_PIN_DTR 36u
#define MODEM_PIN_RI 37u
/* GP38/GP39 drive 2N7002 gates. RP high pulls the corresponding active-low
 * module input low; RP low is the electrically inactive state. GP39 drives the
 * Telit HW_SHUTDOWN_N input. */
#define MODEM_PIN_ON_OFF 38u
#define MODEM_PIN_HW_SHUTDOWN 39u
#define MODEM_PIN_STATUS_ADC 40u
#define MODEM_STATUS_ADC_INPUT 0u

#define HEADSET_HOOK_ADC_PIN 41u
#define HEADSET_HOOK_ADC_INPUT 1u

#define SYS_INT_PIN 42u

#define CHARGER_INPUT_ADC_PIN 43u
#define CHARGER_INPUT_ADC_INPUT 3u

void board_init(void);
/* Diagnostic: true if the boot-time I2C bus-clear found SDA held low (a slave
 * left mid-transaction by a warm reboot) -- printed by the `hw` console cmd. */
bool board_i2c_bus_was_stuck(void);
/* vbus_probe reads service VBUS (false = read failed); firmware passes the
 * direct GP28 probe. See the BOOTSEL policy above. */
void board_enter_bootsel_if_power_held(bool (*vbus_probe)(bool *vbus_high));
/* Immediate composition-root handoff after a release-only physical gesture
 * has already qualified both service VBUS and the hold duration. */
void board_reset_to_bootsel(void);
void board_set_backlight(bool enabled);
/* Hardware PWM duty used whenever the backlight is enabled. The service layer
 * owns validation/persistence and reserves 0% for the existing off state. */
void board_set_backlight_duty_percent(uint8_t duty_percent);
void board_set_system_clock(void);
/* Rev B2 positive-logic +3V8 PMIC controls and observations. The rail is shared
 * by the cellular-module supply and the magnetic buzzer; policy/ownership lives
 * in shared_3v8_service, while these calls only operate the board hardware. */
void board_3v8_rail_set_enabled(bool enabled);
bool board_3v8_rail_power_good(void);
void board_3v8_rail_set_force_pwm(bool enabled);
bool board_3v8_rail_enabled(void);
bool board_3v8_rail_force_pwm_enabled(void);
bool board_modem_dtr_level(void);
bool board_modem_ri_level(void);
bool board_modem_on_off_asserted(void);
bool board_modem_hw_shutdown_asserted(void);
bool board_service_vbus_present(void);
/* Quiet standby clock-down: run clk_sys/clk_peri at XOSC/2 (6 MHz) with PLL_SYS
 * off. clk_ref stays at 12 MHz, and UART/I2C divisors are restored on each
 * transition. Driven from the main loop while backlight-off and idle; the
 * `clkdown` console toggle gates it (default on). */
void board_enter_xosc_lowpower(void);
void board_exit_xosc_lowpower(void);
bool board_is_xosc_lowpower(void);
bool board_clkdown_enabled(void);
void board_set_clkdown_enabled(bool enabled);
/* POWMAN power-off: stop the 6.144 MHz codec MCLK gpout so GP21 is a static
 * pad through the isolation-latched off state. Re-enabled only by the wake
 * reboot's board_init(). */
void board_stop_mclk_output(void);
/* Audio idle gate: restart the MCLK gpout (mirror of board_init's setup). */
void board_start_mclk_output(void);
/* Unused-peripheral clock gating: gate the clock-tree branches of peripheral
 * blocks this board
 * never uses (UART1/I2C1/SPI1/PIO2/HSTX/SHA256/TRNG/TIMER1/TBMAN/JTAG) in
 * both WAKE_EN and SLEEP_EN. Called once at the end of board_init(). */
void board_prune_unused_clocks(void);
/* Sleep-state clock gating: gate extra clocks during the SLEEP state only (XIP/DMA/I2C0/
 * SPI0/PIO0). Inert during active operation. Enabled at board_init (bench-
 * validated: incoming call + SMS survive the sleep windows); `sleepen`
 * console toggle remains for A/B measurement. */
void board_set_sleep_gating(bool enabled);

#endif

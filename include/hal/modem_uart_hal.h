#ifndef MODEM_UART_HAL_H
#define MODEM_UART_HAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void modem_uart_hal_init(void);
/* Arm the interrupt-driven RX ring after an initial polled AT exchange proves
 * the module UART is stable. Idempotent; init leaves RX in bounded FIFO-poll
 * mode so an electrically unsettled line cannot monopolize core 0. */
void modem_uart_hal_enable_rx_irq(void);
uint32_t modem_uart_hal_read_available(uint8_t *dst, uint32_t max_len);
/* Count of RX bytes dropped because the receive ring was full. The ring's
 * software RTS high-water gate should keep this at zero. */
uint32_t modem_uart_hal_rx_dropped(void);
/* Count of hardware RX FIFO overruns (PL011 OE flag): bytes lost in hardware
 * before the ISR ran, e.g. if flow control failed during an IRQ-off window. */
uint32_t modem_uart_hal_rx_overruns(void);
/* Count of framing/parity/break-marked UART words discarded by the transport.
 * A sustained break temporarily falls back to bounded polling so it cannot
 * monopolize core 0 through a level-triggered RX interrupt. */
uint32_t modem_uart_hal_rx_line_errors(void);
/* Number of times the software RX ring deasserted RTS at its high-water mark.
 * This is healthy backpressure, not data loss; it is exposed for bench proof. */
uint32_t modem_uart_hal_rx_flow_pauses(void);
/* Bounded transmit: with CTS hardware flow control a sleeping or wedged
 * module can gate TX indefinitely, and no watchdog runs on the core0 main
 * loop -- a genuinely blocking write would hard-hang the UI. Legitimate
 * stalls at 115200 are ms-scale, so hitting the bound means "the module is
 * not accepting": the remainder is dropped and counted, and the AT layer's
 * command timeout does the recovery. */
void modem_uart_hal_write(const uint8_t *data, size_t len);
void modem_uart_hal_write_cstr(const char *text);
/* Bootstrap transmit: suspend the CTS TX gate, drain the FIFO, then restore
 * hardware flow control. Reserved for commands that establish flow control. */
void modem_uart_hal_write_cstr_no_cts(const char *text);
/* Count of TX bytes dropped by the bounded-write stall guard (diagnostics). */
uint32_t modem_uart_hal_tx_stall_drops(void);
/* True when TX is fully drained (FIFO empty + shift register idle). The Phase-4
 * standby clock-down checks this before reparenting clk_peri / re-bauding the UART:
 * doing so mid-transmit (modem_uart_hal_write only queues into the FIFO, it does not
 * wait for drain) corrupts the byte in flight and fails the AT command. */
bool modem_uart_hal_tx_idle(void);
/* True only when both the software RX ring and PL011 FIFO are empty. Used at
 * the final dormant boundary after CTS has confirmed modem sleep. */
bool modem_uart_hal_rx_idle(void);
/* Hold RTS deasserted and wait for the modem RX wire to remain idle for two
 * complete characters before clk_peri/UART divisors change. Calls may nest so
 * powered-on dormant can retain the hold across clock removal and restoration.
 * Every begin call must be paired with end, including a failed begin. */
bool modem_uart_hal_clock_change_begin(void);
void modem_uart_hal_clock_change_end(void);
/* Safe raw readiness probe while the UART is parked. Enables only the RX pad's
 * input buffer and returns true once the modem drives the UART idle-high level;
 * it does not arm PL011 or its interrupts. */
bool modem_uart_hal_rx_idle_high(void);
/* Assert/deassert the module ON_OFF control (true asserts its active-low pin
 * through the Rev B2 gate transistor). */
void modem_uart_hal_set_power_pin(bool high);
typedef enum {
    MODEM_SHUTDOWN_PULSE_GRACEFUL = 0,
    MODEM_SHUTDOWN_PULSE_UNCONDITIONAL,
} modem_shutdown_pulse_t;
/* Start one bounded shutdown-control pulse. The HAL owns deassertion through a
 * hardware alarm, so a stalled service loop cannot leave ON_OFF or the
 * unconditional shutdown input asserted. Returns false without asserting a
 * pin if no alarm can be armed or another shutdown pulse is active. */
bool modem_uart_hal_start_shutdown_pulse(modem_shutdown_pulse_t pulse,
                                         uint32_t width_ms);
/* Cancel any armed shutdown pulse and synchronously deassert both controls.
 * Idempotent; call it at every power-state boundary. */
void modem_uart_hal_cancel_shutdown_pulse(void);
/* Neutral DTR semantics: true allows a DTR-sleep backend to sleep; false asks
 * it to wake. The HAL owns the board polarity. */
void modem_uart_hal_set_dtr_sleep_permitted(bool permitted);
/* True when the modem is currently allowing DTE transmission. The HAL owns
 * the active-low CTS polarity. */
bool modem_uart_hal_cts_asserted(void);
/* Quiesce UART IRQ/FIFOs and park modem-facing pads after module-off evidence.
 * modem_uart_hal_init() re-arms the transport for the next power-on. */
void modem_uart_hal_park(void);
/* Consume one latched active-low RI wake event. Unlike ri_asserted(), this
 * remains true after a pulse has completed between service ticks. */
bool modem_uart_hal_take_ri_wake(void);
bool modem_uart_hal_ri_asserted(void);
/* PMIC supply power-good observation, not a module status signal. */
bool modem_uart_hal_status_pin(void);

#endif

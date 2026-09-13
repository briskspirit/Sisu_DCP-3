#include "hal/modem_uart_hal.h"

#include "hal/modem_uart_flow_logic.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/sync.h"
#include "hardware/uart.h"
#include "hal/board.h"
#include "hal/board_irq_hal.h"
#include "pico/time.h"

/* IRQ-filled receive ring. The PL011 RX FIFO is only 32 bytes; at 115200 baud
 * ~92 bytes can arrive within one 8 ms UI tick, so the FIFO is drained in the RX
 * interrupt into this ring and modem_uart_hal_read_available() pops from it,
 * decoupling byte reception from the tick cadence so URCs/SMS aren't lost.
 *
 * Single-producer (RX ISR) / single-consumer (modem service tick), both on
 * core 0: the ISR writes head and reads tail, the consumer writes tail and reads
 * head; head/tail are 32-bit aligned so loads/stores are atomic on Cortex-M and
 * no lock is needed, only compiler barriers ordering the data vs index updates.
 *
 * Hardware auto-RTS protects the 32-byte FIFO while core-0 interrupts are
 * disabled. Because the ISR normally empties that FIFO, a separate software
 * high/low-water gate deasserts RTS before this ring fills during a long main-
 * loop stall. The ISR and its flow helper are RAM-resident (and read DR directly
 * instead of calling flash-resident uart_getc), so an IRQ remains safe with XIP
 * off. */
#define MODEM_UARTDR_LINE_ERROR_BITS \
    (UART_UARTDR_BE_BITS | UART_UARTDR_PE_BITS | UART_UARTDR_FE_BITS)
#define MODEM_RX_IRQ_REARM_POLLS 4u
#define MODEM_RX_INIT_DRAIN_BUDGET 64u
#define MODEM_RX_ISR_BUDGET 64u
#define MODEM_RX_CLOCK_QUIET_US 200u
#define MODEM_RX_CLOCK_QUIET_TIMEOUT_US 5000u

static uint8_t s_rx_ring[MODEM_UART_RX_RING_SIZE];
static volatile uint32_t s_rx_head; /* producer: RX ISR */
static volatile uint32_t s_rx_tail; /* consumer: modem service */
static volatile uint32_t s_rx_dropped;
static volatile uint32_t s_rx_overruns;
static volatile uint32_t s_rx_line_errors;
static volatile uint32_t s_rx_flow_pauses;
static bool s_uart_initialized;
static bool s_rx_irq_handler_installed;
static volatile bool s_rx_irq_enabled;
static bool s_rx_irq_authorized;
static volatile uint8_t s_rx_irq_rearm_polls;
static volatile bool s_rx_ring_backpressured;
static uint32_t s_rx_clock_pause_depth;
static volatile alarm_id_t s_shutdown_pulse_alarm_id;
static volatile bool s_shutdown_pulse_alarm_valid;

static void __not_in_flash_func(modem_uart_hal_apply_rx_flow)(void) {
#if SISU_MODEM_BACKEND_ENABLED
    if (!s_uart_initialized) {
        return;
    }
    if (s_rx_clock_pause_depth != 0u || s_rx_ring_backpressured) {
        /* With RTSEN clear, CR.RTS directly controls active-low nUARTRTS.
         * Clearing both bits drives the pin high and stops Telit transmission. */
        hw_clear_bits(&uart_get_hw(MODEM_UART_ID)->cr,
                      UART_UARTCR_RTSEN_BITS | UART_UARTCR_RTS_BITS);
    } else {
        hw_set_bits(&uart_get_hw(MODEM_UART_ID)->cr,
                    UART_UARTCR_RTSEN_BITS);
    }
#endif
}

static int64_t shutdown_pulse_alarm_callback(alarm_id_t id, void *user_data) {
    uint pin = (uint)(uintptr_t)user_data;
    gpio_put(pin, false);
    if (s_shutdown_pulse_alarm_valid && s_shutdown_pulse_alarm_id == id) {
        s_shutdown_pulse_alarm_valid = false;
    }
    return 0;
}

static void __not_in_flash_func(modem_uart_rx_isr)(void) {
    uint32_t serviced = 0u;
    bool fall_back_to_polling = false;
    while (serviced < MODEM_RX_ISR_BUDGET &&
           uart_is_readable(MODEM_UART_ID)) {
        uint32_t occupancy = modem_uart_rx_ring_occupancy(
            s_rx_head, s_rx_tail);
        if (modem_uart_rx_flow_action(occupancy,
                                      s_rx_ring_backpressured) ==
            MODEM_UART_RX_FLOW_PAUSE) {
            /* Do not consume another FIFO byte. Deassert RTS first, then mask
             * RX so the FIFO absorbs the bounded bytes already in flight. The
             * main-loop consumer resumes at the low-water mark. */
            s_rx_ring_backpressured = true;
            s_rx_flow_pauses++;
            modem_uart_hal_apply_rx_flow();
            fall_back_to_polling = true;
            break;
        }
        /* Read the data register directly: uart_is_readable and uart_get_hw are
         * inline, so the whole ISR stays in RAM (unlike uart_getc). The high
         * bits carry per-byte error flags; a hardware overrun (OE) means the
         * FIFO already lost bytes before we ran (e.g. flow control didn't pause
         * the modem during an IRQ-off window), so count it for diagnostics. */
        uint32_t dr = uart_get_hw(MODEM_UART_ID)->dr;
        serviced++;
        if ((dr & UART_UARTDR_OE_BITS) != 0u) {
            s_rx_overruns++;
        }
        if ((dr & MODEM_UARTDR_LINE_ERROR_BITS) != 0u) {
            s_rx_line_errors++;
            fall_back_to_polling = true;
            break;
        }
        uint8_t byte = (uint8_t)dr;
        uint32_t head = s_rx_head;
        uint32_t next = (head + 1u) & MODEM_UART_RX_RING_MASK;
        if (next == s_rx_tail) {
            /* Defensive last resort: the high-water gate above leaves 1/8 of
             * the ring free, so reaching this means its ownership invariant was
             * violated. Keep the diagnostic truthful. */
            s_rx_dropped++;
            continue;
        }
        s_rx_ring[head] = byte;
        __compiler_memory_barrier(); /* commit the byte before publishing head */
        s_rx_head = next;
    }

    /* RX and receive-timeout are level/pending sources. The startup probe is
     * consumed by polling and may leave RT pending when its final empties the
     * FIFO; acknowledging the serviced sources prevents a stale IRQ loop.
     * Electrical errors and a full per-entry budget fall back to the bounded
     * main-loop poller, which later requalifies and re-arms a clean idle line. */
    if (uart_is_readable(MODEM_UART_ID)) {
        fall_back_to_polling = true;
    }
    if (fall_back_to_polling) {
        uart_get_hw(MODEM_UART_ID)->imsc = 0u;
        s_rx_irq_enabled = false;
        s_rx_irq_rearm_polls = 0u;
    }
    uart_get_hw(MODEM_UART_ID)->rsr = 0u;
    uart_get_hw(MODEM_UART_ID)->icr = UART_UARTICR_BITS;
}

void modem_uart_hal_init(void) {
    /* The UART HAL owns only pins/FIFOs/IRQs. The modem service owns the 3V8
     * rail; initialization here must not energize it or assert ON_OFF. */
    s_rx_head = 0u;
    s_rx_tail = 0u;
    if (!s_rx_irq_handler_installed) {
        /* These are lifetime health counters. init() now runs on every modem
         * power cycle, so resetting them here unconditionally would erase the
         * intermittent fault evidence Net Monitor is meant to expose. */
        s_rx_dropped = 0u;
        s_rx_overruns = 0u;
        s_rx_line_errors = 0u;
        s_rx_flow_pauses = 0u;
    }
    s_rx_irq_enabled = false;
    s_rx_irq_authorized = false;
    s_rx_irq_rearm_polls = 0u;
    s_rx_ring_backpressured = false;
    s_rx_clock_pause_depth = 0u;

    uart_init(MODEM_UART_ID, MODEM_UART_BAUD);
    uart_set_format(MODEM_UART_ID, 8, 1, UART_PARITY_NONE);
    uart_set_hw_flow(MODEM_UART_ID, true, true);
    uart_set_fifo_enabled(MODEM_UART_ID, true);

    gpio_set_function(MODEM_PIN_TX, GPIO_FUNC_UART);
    gpio_set_function(MODEM_PIN_RX, GPIO_FUNC_UART);
    gpio_set_function(MODEM_PIN_RTS, GPIO_FUNC_UART);
    gpio_set_function(MODEM_PIN_CTS, GPIO_FUNC_UART);
    gpio_set_input_enabled(MODEM_PIN_TX, false);
    gpio_set_input_enabled(MODEM_PIN_RTS, false);
    gpio_set_input_enabled(MODEM_PIN_RX, true);
    gpio_set_input_enabled(MODEM_PIN_CTS, true);
    /* Active UART inputs use their protocol-idle levels. In particular RX must
     * not retain the off-state pull-down: Telit briefly tri-states TX during
     * boot, and a pull-down would manufacture a sustained UART break. The
     * explicit break-sense/off park paths below restore pull-downs when needed. */
    gpio_pull_up(MODEM_PIN_RX);
    gpio_pull_up(MODEM_PIN_CTS); /* active-low CTS: high = safely deasserted */

    gpio_init(MODEM_PIN_DTR);
    gpio_put(MODEM_PIN_DTR, false);
    gpio_set_dir(MODEM_PIN_DTR, GPIO_OUT);
    gpio_disable_pulls(MODEM_PIN_DTR);
    gpio_set_input_enabled(MODEM_PIN_DTR, false);

    gpio_init(MODEM_PIN_ON_OFF);
    gpio_put(MODEM_PIN_ON_OFF, false);
    gpio_set_dir(MODEM_PIN_ON_OFF, GPIO_OUT);
    gpio_set_input_enabled(MODEM_PIN_ON_OFF, false);

    gpio_init(MODEM_PIN_RI);
    gpio_set_dir(MODEM_PIN_RI, GPIO_IN);
    gpio_disable_pulls(MODEM_PIN_RI);
    gpio_set_input_enabled(MODEM_PIN_RI, true);
    board_irq_hal_set_modem_ri_enabled(true);

    /* Discard any bytes/error state the FIFO accumulated during bring-up so the
     * ring starts clean, then clear the sticky RX error flags. */
    for (uint32_t i = 0u;
         i < MODEM_RX_INIT_DRAIN_BUDGET && uart_is_readable(MODEM_UART_ID);
         i++) {
        (void)uart_get_hw(MODEM_UART_ID)->dr;
    }
    uart_get_hw(MODEM_UART_ID)->rsr = 0u;
    uart_get_hw(MODEM_UART_ID)->icr = UART_UARTICR_BITS;

    /* Telit can emit more than one FIFO of startup traffic before its saved
     * command-mode flow-control policy is active. Arm the bounded ISR as soon
     * as RX has qualified idle-high; a later line error disables it and falls
     * back to the main-loop poller. The successful AT probe calls enable again
     * idempotently, so protocol readiness remains owned by the service. */
    const uint uart_irq = (MODEM_UART_ID == uart0) ? UART0_IRQ : UART1_IRQ;
    uart_set_irq_enables(MODEM_UART_ID, false, false);
    if (!s_rx_irq_handler_installed) {
        /* modem_service_init() performs the first HAL init before core 1 is
         * launched. Claim the vector once in that single-core window; later
         * modem starts only reconfigure PL011 and unmask this owned handler. */
        irq_set_exclusive_handler(uart_irq, modem_uart_rx_isr);
        s_rx_irq_handler_installed = true;
    }
    irq_set_enabled(uart_irq, false);
    s_uart_initialized = true;
    modem_uart_hal_enable_rx_irq();
}

static void modem_uart_hal_arm_rx_irq(void) {
#if SISU_MODEM_BACKEND_ENABLED
    const uint uart_irq = (MODEM_UART_ID == uart0) ? UART0_IRQ : UART1_IRQ;
    /* Remove stale peripheral and NVIC pending state before unmasking receive
     * sources. The FIFO itself is left intact for the ISR to drain. */
    uart_get_hw(MODEM_UART_ID)->imsc = 0u;
    uart_get_hw(MODEM_UART_ID)->icr = UART_UARTICR_BITS;
    irq_clear(uart_irq);
    irq_set_enabled(uart_irq, true);
    /* Publish before unmasking the peripheral: an immediately pending break
     * may enter the ISR as soon as IMSC is written and suppress it again. */
    s_rx_irq_enabled = true;
    uart_set_irq_enables(MODEM_UART_ID, true, false);
#endif
}

void modem_uart_hal_enable_rx_irq(void) {
#if SISU_MODEM_BACKEND_ENABLED
    if (!s_uart_initialized || s_rx_irq_enabled) {
        return;
    }
    s_rx_irq_authorized = true;
    s_rx_irq_rearm_polls = 0u;
    if (gpio_get(MODEM_PIN_RX)) {
        modem_uart_hal_arm_rx_irq();
    }
#endif
}

uint32_t modem_uart_hal_read_available(uint8_t *dst, uint32_t max_len) {
    uint32_t head = s_rx_head; /* snapshot the producer index once */
    uint32_t tail = s_rx_tail;
    uint32_t count = 0u;
    while (count < max_len && tail != head) {
        dst[count++] = s_rx_ring[tail];
        tail = (tail + 1u) & MODEM_UART_RX_RING_MASK;
    }
    __compiler_memory_barrier(); /* finish reads before freeing the slots */
    s_rx_tail = tail;
    if (s_rx_ring_backpressured &&
        modem_uart_rx_flow_action(
            modem_uart_rx_ring_occupancy(s_rx_head, tail), true) ==
            MODEM_UART_RX_FLOW_RESUME) {
        s_rx_ring_backpressured = false;
        modem_uart_hal_apply_rx_flow();
        if (s_rx_irq_authorized && s_rx_clock_pause_depth == 0u) {
            modem_uart_hal_arm_rx_irq();
        }
    }
    /* During an electrically unstable startup the ISR can deliberately fall
     * back here after a line error or a bounded-drain limit. Once IRQ mode is
     * rearmed, the ISR-owned ring remains the sole hardware consumer. */
    if (s_uart_initialized && !s_rx_irq_enabled &&
        !s_rx_ring_backpressured) {
        uint32_t reads = 0u;
        bool line_error = false;
        while (reads < max_len && count < max_len &&
               uart_is_readable(MODEM_UART_ID)) {
            uint32_t dr = uart_get_hw(MODEM_UART_ID)->dr;
            reads++;
            if ((dr & UART_UARTDR_OE_BITS) != 0u) {
                s_rx_overruns++;
            }
            if ((dr & MODEM_UARTDR_LINE_ERROR_BITS) != 0u) {
                s_rx_line_errors++;
                line_error = true;
                uart_get_hw(MODEM_UART_ID)->rsr = 0u;
                continue;
            }
            dst[count++] = (uint8_t)dr;
        }
        if (s_rx_irq_authorized && !line_error && gpio_get(MODEM_PIN_RX)) {
            if (s_rx_irq_rearm_polls < MODEM_RX_IRQ_REARM_POLLS) {
                s_rx_irq_rearm_polls++;
            }
            if (s_rx_irq_rearm_polls >= MODEM_RX_IRQ_REARM_POLLS) {
                modem_uart_hal_arm_rx_irq();
            }
        } else {
            s_rx_irq_rearm_polls = 0u;
        }
    }
    return count;
}

uint32_t modem_uart_hal_rx_dropped(void) {
    return s_rx_dropped;
}

uint32_t modem_uart_hal_rx_overruns(void) {
    return s_rx_overruns;
}

uint32_t modem_uart_hal_rx_line_errors(void) {
    return s_rx_line_errors;
}

uint32_t modem_uart_hal_rx_flow_pauses(void) {
    return s_rx_flow_pauses;
}

bool modem_uart_hal_clock_change_begin(void) {
#if !SISU_MODEM_BACKEND_ENABLED
    return true;
#else
    if (!s_uart_initialized) {
        return true;
    }
    uint32_t irq_state = save_and_disable_interrupts();
    bool outermost = s_rx_clock_pause_depth++ == 0u;
    if (outermost) {
        modem_uart_hal_apply_rx_flow();
    }
    restore_interrupts(irq_state);
    if (!outermost) {
        return true;
    }

    /* RTS can only stop the next character; wait until every in-flight byte has
     * crossed and the wire has remained idle-high for two full characters. */
    uint64_t deadline = time_us_64() + MODEM_RX_CLOCK_QUIET_TIMEOUT_US;
    uint64_t high_since = 0u;
    while (time_us_64() < deadline) {
        uint64_t now = time_us_64();
        if (gpio_get(MODEM_PIN_RX)) {
            if (high_since == 0u) {
                high_since = now;
            } else if (now - high_since >= MODEM_RX_CLOCK_QUIET_US) {
                return true;
            }
        } else {
            high_since = 0u;
        }
        tight_loop_contents();
    }
    return false;
#endif
}

void modem_uart_hal_clock_change_end(void) {
#if SISU_MODEM_BACKEND_ENABLED
    if (!s_uart_initialized) {
        return;
    }
    uint32_t irq_state = save_and_disable_interrupts();
    if (s_rx_clock_pause_depth != 0u) {
        s_rx_clock_pause_depth--;
        if (s_rx_clock_pause_depth == 0u) {
            modem_uart_hal_apply_rx_flow();
        }
    }
    restore_interrupts(irq_state);
#endif
}

/* See the header: bounded so a CTS-gated sleeping or wedged module cannot
 * hard-hang core0. Sized for the worst legitimate transfer: a maximal SMS
 * PDU (~384 chars ~= 33 ms at 115200) plus mid-transfer CTS pauses -- 100 ms
 * is ~3x that and still invisible against the AT-level timeouts. */
#define MODEM_TX_STALL_TIMEOUT_US 100000u

static uint32_t s_tx_stall_drops;

void modem_uart_hal_write(const uint8_t *data, size_t len) {
#if !SISU_MODEM_BACKEND_ENABLED
    (void)data;
    (void)len;
    return;
#endif
    if (!s_uart_initialized) {
        /* A state-machine error must fail closed. Touching PL011 after park()
         * can enqueue bytes into a deinitialized peripheral or stall core 0. */
        s_tx_stall_drops += (uint32_t)len;
        return;
    }
    absolute_time_t deadline = make_timeout_time_us(MODEM_TX_STALL_TIMEOUT_US);
    for (size_t i = 0; i < len; i++) {
        while (!uart_is_writable(MODEM_UART_ID)) {
            if (time_reached(deadline)) {
                s_tx_stall_drops += (uint32_t)(len - i);
                return;
            }
            tight_loop_contents();
        }
        uart_get_hw(MODEM_UART_ID)->dr = data[i];
    }
}

void modem_uart_hal_write_cstr_no_cts(const char *text) {
#if !SISU_MODEM_BACKEND_ENABLED
    (void)text;
    return;
#endif
    if (!s_uart_initialized) {
        /* Route through the common guard so the attempted bytes are counted. */
        modem_uart_hal_write_cstr(text);
        return;
    }
    uart_set_hw_flow(MODEM_UART_ID, false, true);
    modem_uart_hal_write_cstr(text);
    uart_tx_wait_blocking(MODEM_UART_ID); /* drain before re-arming the CTS gate */
    uart_set_hw_flow(MODEM_UART_ID, true, true);
}

uint32_t modem_uart_hal_tx_stall_drops(void) {
    return s_tx_stall_drops;
}

bool modem_uart_hal_tx_idle(void) {
#if !SISU_MODEM_BACKEND_ENABLED
    /* No UART is initialized for the modem-less Phase-1 backend. The main-loop
     * clock-down predicate still asks this neutral question, so answer it
     * without touching an unowned PL011 register block. */
    return true; /* NONE has no modem UART work in flight. */
#else
    if (!s_uart_initialized) {
        return true;
    }
    /* PL011 FR.BUSY is set while the UART is transmitting (FIFO non-empty or the
     * shift register still shifting); clear = TX fully drained, safe to reclock. */
    return (uart_get_hw(MODEM_UART_ID)->fr & UART_UARTFR_BUSY_BITS) == 0u;
#endif
}

bool modem_uart_hal_rx_idle_high(void) {
#if !SISU_MODEM_BACKEND_ENABLED
    return false;
#else
    /* park() leaves the line as SIO with the input buffer disabled for minimum
     * off current. During a requested startup the rail is already on, so it is
     * safe to observe the modem-side level without enabling PL011 RX IRQs. */
    gpio_set_input_enabled(MODEM_PIN_RX, true);
    return gpio_get(MODEM_PIN_RX);
#endif
}

void modem_uart_hal_write_cstr(const char *text) {
    /* Through the bounded writer: this is the path every AT command and its
     * CRLF take, and it must share the stall guard: a bare uart_putc_raw
     * loop lets a CTS-gated sleeping module hang core0 on any command longer
     * than the 32-byte TX FIFO. */
    size_t len = 0u;
    while (text[len] != '\0') {
        len++;
    }
    modem_uart_hal_write((const uint8_t *)text, len);
}

void modem_uart_hal_set_power_pin(bool high) {
    /* GP38 drives a 2N7002 gate: high asserts the module's active-low
     * Telit ON_OFF input on Rev B2. */
#if !SISU_MODEM_BACKEND_ENABLED
    (void)high;
    gpio_put(MODEM_PIN_ON_OFF, false);
#else
    gpio_put(MODEM_PIN_ON_OFF, high);
#endif
}

bool modem_uart_hal_start_shutdown_pulse(modem_shutdown_pulse_t pulse,
                                         uint32_t width_ms) {
#if !SISU_MODEM_BACKEND_ENABLED
    (void)pulse;
    (void)width_ms;
    gpio_put(MODEM_PIN_ON_OFF, false);
    gpio_put(MODEM_PIN_HW_SHUTDOWN, false);
    return false;
#else
    if (width_ms == 0u || s_shutdown_pulse_alarm_valid ||
        (pulse != MODEM_SHUTDOWN_PULSE_GRACEFUL &&
         pulse != MODEM_SHUTDOWN_PULSE_UNCONDITIONAL)) {
        return false;
    }

    uint pin = pulse == MODEM_SHUTDOWN_PULSE_UNCONDITIONAL
        ? MODEM_PIN_HW_SHUTDOWN
        : MODEM_PIN_ON_OFF;
    /* Both controls are active low at the module through independent 2N7002
     * gates. Keep the unused gate inactive before asserting the selected one. */
    gpio_put(MODEM_PIN_ON_OFF, false);
    gpio_put(MODEM_PIN_HW_SHUTDOWN, false);
    gpio_put(pin, true);
    alarm_id_t alarm = add_alarm_in_ms(
        width_ms, shutdown_pulse_alarm_callback,
        (void *)(uintptr_t)pin, true);
    if (alarm < 0) {
        gpio_put(pin, false);
        return false;
    }
    s_shutdown_pulse_alarm_id = alarm;
    s_shutdown_pulse_alarm_valid = true;
    return true;
#endif
}

void modem_uart_hal_cancel_shutdown_pulse(void) {
    uint32_t irq_state = save_and_disable_interrupts();
    if (s_shutdown_pulse_alarm_valid) {
        (void)cancel_alarm(s_shutdown_pulse_alarm_id);
        s_shutdown_pulse_alarm_valid = false;
    }
    restore_interrupts(irq_state);
    gpio_put(MODEM_PIN_ON_OFF, false);
    gpio_put(MODEM_PIN_HW_SHUTDOWN, false);
}

void modem_uart_hal_set_dtr_sleep_permitted(bool permitted) {
#if !SISU_MODEM_BACKEND_ENABLED
    (void)permitted;
    gpio_put(MODEM_PIN_DTR, false);
#else
    /* Rev B2 routes GP36 through a non-inverting TXU0202 channel. Telit DTR
     * high is deasserted (sleep permitted); low is asserted (wake). */
    gpio_put(MODEM_PIN_DTR, permitted);
#endif
}

bool modem_uart_hal_cts_asserted(void) {
#if !SISU_MODEM_BACKEND_ENABLED
    return false;
#else
    /* park() deliberately pulls this active-low input down. Without the
     * initialized guard that harmless off-state level is indistinguishable
     * from a live module granting transmission. */
    return s_uart_initialized && !gpio_get(MODEM_PIN_CTS);
#endif
}

bool modem_uart_hal_rx_idle(void) {
#if !SISU_MODEM_BACKEND_ENABLED
    return true;
#else
    if (!s_uart_initialized) {
        return true;
    }
    uint32_t irq_state = save_and_disable_interrupts();
    bool idle = s_rx_head == s_rx_tail &&
                !uart_is_readable(MODEM_UART_ID);
    restore_interrupts(irq_state);
    return idle;
#endif
}

void modem_uart_hal_park(void) {
#if SISU_MODEM_BACKEND_ENABLED
    board_irq_hal_set_modem_ri_enabled(false);
    const uint uart_irq = (MODEM_UART_ID == uart0) ? UART0_IRQ : UART1_IRQ;
    uart_set_irq_enables(MODEM_UART_ID, false, false);
    irq_set_enabled(uart_irq, false);
    uart_get_hw(MODEM_UART_ID)->icr = UART_UARTICR_BITS;
    irq_clear(uart_irq);
    if (s_uart_initialized) {
        uart_deinit(MODEM_UART_ID);
    }
    s_uart_initialized = false;
    s_rx_irq_enabled = false;
    s_rx_irq_authorized = false;
    s_rx_irq_rearm_polls = 0u;
    s_rx_ring_backpressured = false;
    s_rx_clock_pause_depth = 0u;
#endif

    /* Stage harmless SIO values before changing functions. The level shifter
     * has partial-power-down isolation, but static low RP outputs also avoid
     * presenting an asserted control while the module side collapses. */
    gpio_put(MODEM_PIN_TX, false);
    gpio_set_dir(MODEM_PIN_TX, GPIO_OUT);
    gpio_set_function(MODEM_PIN_TX, GPIO_FUNC_SIO);
    gpio_disable_pulls(MODEM_PIN_TX);
    gpio_set_input_enabled(MODEM_PIN_TX, false);

    gpio_put(MODEM_PIN_RTS, false);
    gpio_set_dir(MODEM_PIN_RTS, GPIO_OUT);
    gpio_set_function(MODEM_PIN_RTS, GPIO_FUNC_SIO);
    gpio_disable_pulls(MODEM_PIN_RTS);
    gpio_set_input_enabled(MODEM_PIN_RTS, false);

    gpio_set_dir(MODEM_PIN_RX, GPIO_IN);
    gpio_set_function(MODEM_PIN_RX, GPIO_FUNC_SIO);
    gpio_pull_down(MODEM_PIN_RX);
    gpio_set_input_enabled(MODEM_PIN_RX, false);

    gpio_set_dir(MODEM_PIN_CTS, GPIO_IN);
    gpio_set_function(MODEM_PIN_CTS, GPIO_FUNC_SIO);
    gpio_pull_down(MODEM_PIN_CTS);
    gpio_set_input_enabled(MODEM_PIN_CTS, false);

    gpio_put(MODEM_PIN_DTR, false);
    gpio_set_dir(MODEM_PIN_DTR, GPIO_OUT);
    gpio_set_function(MODEM_PIN_DTR, GPIO_FUNC_SIO);
    gpio_disable_pulls(MODEM_PIN_DTR);
    gpio_set_input_enabled(MODEM_PIN_DTR, false);

    gpio_set_dir(MODEM_PIN_RI, GPIO_IN);
    gpio_set_function(MODEM_PIN_RI, GPIO_FUNC_SIO);
    gpio_disable_pulls(MODEM_PIN_RI);
    gpio_set_input_enabled(MODEM_PIN_RI, false);
}

bool modem_uart_hal_ri_asserted(void) {
#if !SISU_MODEM_BACKEND_ENABLED
    return false;
#else
    return s_uart_initialized && !gpio_get(MODEM_PIN_RI);
#endif
}

bool modem_uart_hal_take_ri_wake(void) {
#if !SISU_MODEM_BACKEND_ENABLED
    return false;
#else
    return s_uart_initialized && board_irq_hal_take_modem_ri_pending();
#endif
}

bool modem_uart_hal_status_pin(void) {
    return board_3v8_rail_power_good();
}

/*
 * Rev B2 Telit electrical-characterization diagnostic.
 *
 * This is intentionally not a modem backend. It exposes a bounded CDC command
 * surface for measuring power, control, UART, DTR/RI, and PWRMON behavior while
 * preserving raw timestamped modem output. Production remains on NONE until
 * the resulting bench evidence closes the Phase 2 electrical gate.
 */

#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "diag/telit_diag_logic.h"
#if defined(SISU_TELIT_DVI_DIAGNOSTIC)
#include "diag/telit_dvi_diag.h"
#endif
#if defined(SISU_TELIT_TUNER_DIAGNOSTIC)
#include "tuner_logic.h"
#include "hal/keypad.h"
#include "hal/lcd_pcd8544.h"
#include "hal/tca8418_hal.h"
#include "ui/framebuffer.h"
#endif
#include "hal/board.h"
#include "hal/ltc2959_hal.h"
#include "hal/modem_power_monitor_hal.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/regs/uart.h"
#include "hardware/sync.h"
#include "hardware/uart.h"
#include "pico/stdlib.h"
#include "pico/time.h"

#define DIAG_CONSOLE_LINE_CAP 256u
#define DIAG_MODEM_LINE_CAP 256u
#define DIAG_MODEM_ESCAPED_CAP (DIAG_MODEM_LINE_CAP * 4u + 1u)
#define DIAG_UART_RX_RING_SIZE 2048u
#define DIAG_UART_RX_RING_MASK (DIAG_UART_RX_RING_SIZE - 1u)
#define DIAG_UART_TX_TIMEOUT_MS 250u
#define DIAG_SAMPLE_PERIOD_MS 10u
#define DIAG_MONITOR_PERIOD_MS 500u

#if defined(SISU_TELIT_TUNER_DIAGNOSTIC)
#define TUNER_STATUS_OFF_MAX_RAW 64u
#define TUNER_STATUS_ON_MIN_RAW 1024u
#define TUNER_RAIL_SETTLE_MS 20u
#define TUNER_BOOT_PULSE_MS 1200u
#define TUNER_STARTUP_TIMEOUT_MS 30000u
#define TUNER_COMMAND_TIMEOUT_MS 5000u
#define TUNER_CFUN_TIMEOUT_MS 15000u
#define TUNER_COMMAND_RETRY_MS 250u
#define TUNER_PROBE_RETRY_MS 500u
#define TUNER_COMMAND_MAX_ATTEMPTS 3u
#define TUNER_POWER_OFF_HOLD_MS 1000u
#define TUNER_SHUTDOWN_WAIT_MS 8000u
#define TUNER_HW_OFF_PULSE_MS 3000u
#define TUNER_HW_OFF_WAIT_MS 10000u
#define TUNER_LCD_VOP 54u
#endif

_Static_assert(
    (DIAG_UART_RX_RING_SIZE & (DIAG_UART_RX_RING_SIZE - 1u)) == 0u,
    "diagnostic UART ring must be a power of two");

static telit_diag_logic_t s_logic;
static telit_diag_observation_t s_observation;
static uint16_t s_status_mv;
static bool s_ltc_init_ok;

static volatile uint8_t s_uart_rx_ring[DIAG_UART_RX_RING_SIZE];
static volatile uint32_t s_uart_rx_head;
static volatile uint32_t s_uart_rx_tail;
static volatile uint32_t s_uart_rx_dropped;
static volatile uint32_t s_uart_rx_overruns;
static volatile uint32_t s_uart_rx_framing_errors;
static volatile uint32_t s_uart_rx_parity_errors;
static volatile uint32_t s_uart_rx_breaks;
static bool s_uart_initialized;
static bool s_uart_hw_flow;

static char s_console_line[DIAG_CONSOLE_LINE_CAP];
static size_t s_console_len;
static bool s_console_discarding;
static uint8_t s_modem_line[DIAG_MODEM_LINE_CAP];
static size_t s_modem_line_len;
static char s_modem_escaped[DIAG_MODEM_ESCAPED_CAP];

static bool s_applied_rail;
static telit_diag_pulse_t s_applied_pulse;
static uint32_t s_applied_pulse_sequence;
static volatile alarm_id_t s_pulse_alarm_id;
static volatile bool s_pulse_alarm_valid;
static bool s_edges_initialized;
static bool s_last_pg;
static bool s_last_ri_n;
static bool s_last_cts_n;
static bool s_status_edge_initialized;
static bool s_last_status_above_threshold;

static uint32_t now_ms(void) {
    return to_ms_since_boot(get_absolute_time());
}

static void __not_in_flash_func(diag_uart_rx_isr)(void) {
    while (uart_is_readable(MODEM_UART_ID)) {
        uint32_t dr = uart_get_hw(MODEM_UART_ID)->dr;
        if ((dr & UART_UARTDR_OE_BITS) != 0u) {
            s_uart_rx_overruns++;
        }
        if ((dr & UART_UARTDR_FE_BITS) != 0u) {
            s_uart_rx_framing_errors++;
        }
        if ((dr & UART_UARTDR_PE_BITS) != 0u) {
            s_uart_rx_parity_errors++;
        }
        if ((dr & UART_UARTDR_BE_BITS) != 0u) {
            s_uart_rx_breaks++;
        }
        uint32_t head = s_uart_rx_head;
        uint32_t next = (head + 1u) & DIAG_UART_RX_RING_MASK;
        if (next == s_uart_rx_tail) {
            s_uart_rx_dropped++;
            continue;
        }
        s_uart_rx_ring[head] = (uint8_t)dr;
        __compiler_memory_barrier();
        s_uart_rx_head = next;
    }
}

static void diag_uart_park(void) {
    if (s_uart_initialized) {
        const uint uart_irq =
            MODEM_UART_ID == uart0 ? UART0_IRQ : UART1_IRQ;
        uart_set_irq_enables(MODEM_UART_ID, false, false);
        irq_set_enabled(uart_irq, false);
        uart_deinit(MODEM_UART_ID);
        s_uart_initialized = false;
    }

    gpio_set_function(MODEM_PIN_TX, GPIO_FUNC_SIO);
    gpio_put(MODEM_PIN_TX, false);
    gpio_set_dir(MODEM_PIN_TX, GPIO_OUT);
    gpio_set_input_enabled(MODEM_PIN_TX, false);

    gpio_set_function(MODEM_PIN_RTS, GPIO_FUNC_SIO);
    gpio_put(MODEM_PIN_RTS, false);
    gpio_set_dir(MODEM_PIN_RTS, GPIO_OUT);
    gpio_set_input_enabled(MODEM_PIN_RTS, false);

    gpio_set_function(MODEM_PIN_RX, GPIO_FUNC_SIO);
    gpio_set_dir(MODEM_PIN_RX, GPIO_IN);
    gpio_pull_down(MODEM_PIN_RX);
    gpio_set_input_enabled(MODEM_PIN_RX, true);

    gpio_set_function(MODEM_PIN_CTS, GPIO_FUNC_SIO);
    gpio_set_dir(MODEM_PIN_CTS, GPIO_IN);
    gpio_pull_down(MODEM_PIN_CTS);
    gpio_set_input_enabled(MODEM_PIN_CTS, true);

    s_uart_rx_head = 0u;
    s_uart_rx_tail = 0u;
    s_uart_hw_flow = false;
}

static bool diag_uart_busy(void) {
    return s_uart_initialized &&
           (uart_get_hw(MODEM_UART_ID)->fr & UART_UARTFR_BUSY_BITS) != 0u;
}

static bool diag_uart_set_flow(bool hardware_flow) {
    if (diag_uart_busy()) {
        return false;
    }

    if (!s_uart_initialized) {
        s_uart_rx_head = 0u;
        s_uart_rx_tail = 0u;
        s_uart_rx_dropped = 0u;
        s_uart_rx_overruns = 0u;
        s_uart_rx_framing_errors = 0u;
        s_uart_rx_parity_errors = 0u;
        s_uart_rx_breaks = 0u;
        uart_init(MODEM_UART_ID, MODEM_UART_BAUD);
        uart_set_format(MODEM_UART_ID, 8, 1, UART_PARITY_NONE);
        uart_set_fifo_enabled(MODEM_UART_ID, true);
        gpio_set_function(MODEM_PIN_TX, GPIO_FUNC_UART);
        gpio_set_function(MODEM_PIN_RX, GPIO_FUNC_UART);
        gpio_set_input_enabled(MODEM_PIN_TX, false);
        gpio_set_input_enabled(MODEM_PIN_RX, true);
        gpio_pull_down(MODEM_PIN_RX);
        s_uart_initialized = true;
    }

    uart_set_hw_flow(MODEM_UART_ID, hardware_flow, hardware_flow);
    if (hardware_flow) {
        gpio_set_function(MODEM_PIN_CTS, GPIO_FUNC_UART);
        gpio_set_function(MODEM_PIN_RTS, GPIO_FUNC_UART);
        gpio_set_input_enabled(MODEM_PIN_CTS, true);
        gpio_set_input_enabled(MODEM_PIN_RTS, false);
    } else {
        gpio_set_function(MODEM_PIN_RTS, GPIO_FUNC_SIO);
        gpio_put(MODEM_PIN_RTS, false);
        gpio_set_dir(MODEM_PIN_RTS, GPIO_OUT);
        gpio_set_input_enabled(MODEM_PIN_RTS, false);
        gpio_set_function(MODEM_PIN_CTS, GPIO_FUNC_SIO);
        gpio_set_dir(MODEM_PIN_CTS, GPIO_IN);
        gpio_pull_down(MODEM_PIN_CTS);
        gpio_set_input_enabled(MODEM_PIN_CTS, true);
    }

    while (uart_is_readable(MODEM_UART_ID)) {
        (void)uart_get_hw(MODEM_UART_ID)->dr;
    }
    uart_get_hw(MODEM_UART_ID)->rsr = 0u;
    const uint uart_irq =
        MODEM_UART_ID == uart0 ? UART0_IRQ : UART1_IRQ;
    irq_set_exclusive_handler(uart_irq, diag_uart_rx_isr);
    irq_set_enabled(uart_irq, true);
    uart_set_irq_enables(MODEM_UART_ID, true, false);
    s_uart_hw_flow = hardware_flow;
    return true;
}

static bool diag_uart_write_command(const char *command,
                                    bool hardware_flow) {
    size_t len = strlen(command);
    if (len < 2u || len >= DIAG_CONSOLE_LINE_CAP ||
        command[0] != 'A' || command[1] != 'T') {
        return false;
    }
    for (size_t i = 0u; i < len; i++) {
        unsigned char ch = (unsigned char)command[i];
        if (ch < 0x20u || ch > 0x7eu) {
            return false;
        }
    }
    if (!diag_uart_set_flow(hardware_flow)) {
        return false;
    }

    absolute_time_t deadline =
        make_timeout_time_ms(DIAG_UART_TX_TIMEOUT_MS);
    for (size_t i = 0u; i <= len; i++) {
        uint8_t byte = i == len ? '\r' : (uint8_t)command[i];
        while (!uart_is_writable(MODEM_UART_ID)) {
            if (time_reached(deadline)) {
                return false;
            }
            tight_loop_contents();
        }
        uart_get_hw(MODEM_UART_ID)->dr = byte;
    }
    return true;
}

static bool diag_uart_pop(uint8_t *out) {
    uint32_t tail = s_uart_rx_tail;
    if (tail == s_uart_rx_head) {
        return false;
    }
    *out = s_uart_rx_ring[tail];
    __compiler_memory_barrier();
    s_uart_rx_tail = (tail + 1u) & DIAG_UART_RX_RING_MASK;
    return true;
}

#if defined(SISU_TELIT_TUNER_DIAGNOSTIC)
typedef enum {
    TUNER_STAGE_OFF = 0,
    TUNER_STAGE_RAIL_WAIT,
    TUNER_STAGE_MODULE_WAIT,
    TUNER_STAGE_PROBE,
    TUNER_STAGE_ECHO_OFF,
    TUNER_STAGE_CMEE,
    TUNER_STAGE_CFUN_OFF,
    TUNER_STAGE_CFUN_VERIFY,
    TUNER_STAGE_STUNE_QUERY,
    TUNER_STAGE_GPIO2_RELEASE,
    TUNER_STAGE_GPIO3_RELEASE,
    TUNER_STAGE_APPLY_GPIO2,
    TUNER_STAGE_APPLY_GPIO3,
    TUNER_STAGE_VERIFY_GPIO2,
    TUNER_STAGE_VERIFY_GPIO3,
    TUNER_STAGE_READY,
    TUNER_STAGE_SHUTDOWN_GPIO2_RELEASE,
    TUNER_STAGE_SHUTDOWN_GPIO3_RELEASE,
    TUNER_STAGE_SHUTDOWN_COMMAND,
    TUNER_STAGE_SHUTDOWN_WAIT,
    TUNER_STAGE_SHUTDOWN_HW_WAIT,
    TUNER_STAGE_CUTTING_RAIL,
    TUNER_STAGE_FAULT,
} tuner_stage_t;

static lcd_pcd8544_t s_tuner_lcd;
static framebuffer_t s_tuner_fb;
static tuner_stage_t s_tuner_stage;
static const telit_tuner_branch_t *s_tuner_desired_branch;
static const telit_tuner_branch_t *s_tuner_apply_branch;
static const telit_tuner_branch_t *s_tuner_applied_branch;
static uint32_t s_tuner_startup_deadline_ms;
static uint32_t s_tuner_next_action_ms;
static uint32_t s_tuner_command_deadline_ms;
static uint32_t s_tuner_shutdown_deadline_ms;
static uint32_t s_tuner_power_pressed_ms;
static uint8_t s_tuner_stage_attempts;
static uint16_t s_tuner_previous_keys;
static bool s_tuner_command_active;
static bool s_tuner_power_was_pressed;
static bool s_tuner_ignore_power_until_release;
static bool s_tuner_power_hold_handled;
static bool s_tuner_shutdown_requested;
static bool s_tuner_at_ready;
static bool s_tuner_radio_off;
static bool s_tuner_original_stune_known;
static bool s_tuner_original_stune_enabled;
static bool s_tuner_gpio_touched;
static bool s_tuner_cfun_seen;
static uint8_t s_tuner_cfun;
static bool s_tuner_stune_seen;
static bool s_tuner_stune_enabled;
static bool s_tuner_gpio_seen;
static uint8_t s_tuner_gpio_direction;
static bool s_tuner_gpio_level;
static bool s_tuner_ui_dirty;
static char s_tuner_fault[24];

static void tuner_begin_shutdown(uint32_t timestamp_ms);

static bool tuner_deadline_reached(uint32_t timestamp_ms,
                                   uint32_t deadline_ms) {
    return (int32_t)(timestamp_ms - deadline_ms) >= 0;
}

static const char *tuner_stage_text(tuner_stage_t stage) {
    switch (stage) {
    case TUNER_STAGE_OFF: return "OFF";
    case TUNER_STAGE_RAIL_WAIT: return "3V8 START";
    case TUNER_STAGE_MODULE_WAIT: return "MODEM START";
    case TUNER_STAGE_PROBE: return "WAITING FOR AT";
    case TUNER_STAGE_ECHO_OFF:
    case TUNER_STAGE_CMEE: return "AT SETUP";
    case TUNER_STAGE_CFUN_OFF:
    case TUNER_STAGE_CFUN_VERIFY: return "RF SHUTDOWN";
    case TUNER_STAGE_STUNE_QUERY:
    case TUNER_STAGE_GPIO2_RELEASE:
    case TUNER_STAGE_GPIO3_RELEASE: return "GPIO SETUP";
    case TUNER_STAGE_APPLY_GPIO2:
    case TUNER_STAGE_APPLY_GPIO3:
    case TUNER_STAGE_VERIFY_GPIO2:
    case TUNER_STAGE_VERIFY_GPIO3: return "APPLYING";
    case TUNER_STAGE_READY: return "READY";
    case TUNER_STAGE_SHUTDOWN_GPIO2_RELEASE:
    case TUNER_STAGE_SHUTDOWN_GPIO3_RELEASE:
    case TUNER_STAGE_SHUTDOWN_COMMAND: return "SHUTTING DOWN";
    case TUNER_STAGE_SHUTDOWN_WAIT: return "WAIT PWRMON";
    case TUNER_STAGE_SHUTDOWN_HW_WAIT: return "HW OFF";
    case TUNER_STAGE_CUTTING_RAIL: return "3V8 OFF";
    case TUNER_STAGE_FAULT: return "FAULT";
    default: return "UNKNOWN";
    }
}

static bool tuner_stage_is_apply(tuner_stage_t stage) {
    return stage >= TUNER_STAGE_APPLY_GPIO2 &&
           stage <= TUNER_STAGE_VERIFY_GPIO3;
}

static bool tuner_stage_is_command(tuner_stage_t stage) {
    return (stage >= TUNER_STAGE_PROBE &&
            stage <= TUNER_STAGE_VERIFY_GPIO3) ||
           (stage >= TUNER_STAGE_SHUTDOWN_GPIO2_RELEASE &&
            stage <= TUNER_STAGE_SHUTDOWN_COMMAND);
}

static bool tuner_stage_is_shutdown_prep(tuner_stage_t stage) {
    return stage >= TUNER_STAGE_SHUTDOWN_GPIO2_RELEASE &&
           stage <= TUNER_STAGE_SHUTDOWN_GPIO3_RELEASE;
}

static void tuner_set_stage(tuner_stage_t stage, uint32_t timestamp_ms) {
    s_tuner_stage = stage;
    s_tuner_stage_attempts = 0u;
    s_tuner_next_action_ms = timestamp_ms;
    s_tuner_ui_dirty = true;
    printf("[tuner t=%lu] state=%s\n", (unsigned long)timestamp_ms,
           tuner_stage_text(stage));
}

static void tuner_set_fault(const char *reason, uint32_t timestamp_ms) {
    snprintf(s_tuner_fault, sizeof s_tuner_fault, "%s",
             reason != NULL ? reason : "UNKNOWN");
    s_tuner_command_active = false;
    s_tuner_shutdown_requested = false;
    tuner_set_stage(TUNER_STAGE_FAULT, timestamp_ms);
    printf("[tuner t=%lu] FAULT: %s; rail retained while PWRMON is high\n",
           (unsigned long)timestamp_ms, s_tuner_fault);
}

static void tuner_draw_ui(void) {
    if (!s_tuner_ui_dirty || s_tuner_stage == TUNER_STAGE_OFF) {
        return;
    }
    s_tuner_ui_dirty = false;
    char line[24];
    fb_clear(&s_tuner_fb, false);
    fb_text5(&s_tuner_fb, "ANTENNA TUNE", 0, 0, true, FB_WIDTH);

    if (s_tuner_stage == TUNER_STAGE_FAULT) {
        fb_text5(&s_tuner_fb, "FAULT", 0, 8, true, FB_WIDTH);
        fb_text5(&s_tuner_fb, s_tuner_fault, 0, 16, true, FB_WIDTH);
        snprintf(line, sizeof line, "PWR %umV", (unsigned)s_status_mv);
        fb_text5(&s_tuner_fb, line, 0, 24, true, FB_WIDTH);
        fb_text5(&s_tuner_fb, "HOLD P: OFF", 0, 40, true, FB_WIDTH);
        lcd_show(&s_tuner_lcd, &s_tuner_fb);
        return;
    }

    const telit_tuner_branch_t *branch =
        tuner_stage_is_apply(s_tuner_stage) ? s_tuner_apply_branch
                                           : s_tuner_applied_branch;
    if (s_tuner_stage == TUNER_STAGE_READY ||
        tuner_stage_is_apply(s_tuner_stage)) {
        if (branch == NULL) {
            branch = s_tuner_desired_branch;
        }
        snprintf(line, sizeof line, "RF%u %s",
                 (unsigned)branch->rf_path, branch->bands);
        fb_text5(&s_tuner_fb, line, 0, 8, true, FB_WIDTH);
        snprintf(line, sizeof line, "G3:%u G2:%u",
                 branch->gpio3 ? 1u : 0u, branch->gpio2 ? 1u : 0u);
        fb_text5(&s_tuner_fb, line, 0, 16, true, FB_WIDTH);
        fb_text5(&s_tuner_fb,
                 s_tuner_radio_off ? "RADIO: OFF" : "RF: NOT SAFE",
                 0, 24, true, FB_WIDTH);
        fb_text5(&s_tuner_fb, tuner_stage_text(s_tuner_stage),
                 0, 32, true, FB_WIDTH);
        fb_text5(&s_tuner_fb, "1-4  HOLD P", 0, 40, true, FB_WIDTH);
    } else {
        fb_text5(&s_tuner_fb, tuner_stage_text(s_tuner_stage),
                 0, 8, true, FB_WIDTH);
        snprintf(line, sizeof line, "PWR %umV", (unsigned)s_status_mv);
        fb_text5(&s_tuner_fb, line, 0, 16, true, FB_WIDTH);
        fb_text5(&s_tuner_fb,
                 s_tuner_radio_off ? "RADIO: OFF" : "RF: LOCKING",
                 0, 24, true, FB_WIDTH);
        fb_text5(&s_tuner_fb,
                 s_tuner_at_ready ? "MODEM: READY" : "PLEASE WAIT",
                 0, 32, true, FB_WIDTH);
        fb_text5(&s_tuner_fb, "HOLD P: OFF", 0, 40, true, FB_WIDTH);
    }
    lcd_show(&s_tuner_lcd, &s_tuner_fb);
}

static void tuner_reset_parsed_response(void) {
    s_tuner_cfun_seen = false;
    s_tuner_stune_seen = false;
    s_tuner_gpio_seen = false;
}

static bool tuner_send_stage_command(uint32_t timestamp_ms) {
    char command[64];
    uint32_t timeout_ms = TUNER_COMMAND_TIMEOUT_MS;
    const telit_tuner_branch_t *branch = s_tuner_apply_branch;

    switch (s_tuner_stage) {
    case TUNER_STAGE_PROBE:
        snprintf(command, sizeof command, "AT");
        break;
    case TUNER_STAGE_ECHO_OFF:
        snprintf(command, sizeof command, "ATE0");
        break;
    case TUNER_STAGE_CMEE:
        snprintf(command, sizeof command, "AT+CMEE=2");
        break;
    case TUNER_STAGE_CFUN_OFF:
        snprintf(command, sizeof command, "AT+CFUN=4");
        timeout_ms = TUNER_CFUN_TIMEOUT_MS;
        break;
    case TUNER_STAGE_CFUN_VERIFY:
        snprintf(command, sizeof command, "AT+CFUN?");
        break;
    case TUNER_STAGE_STUNE_QUERY:
        snprintf(command, sizeof command, "AT#STUNEANT?");
        break;
    case TUNER_STAGE_GPIO2_RELEASE:
    case TUNER_STAGE_SHUTDOWN_GPIO2_RELEASE:
        snprintf(command, sizeof command, "AT#GPIO=2,0,0,0");
        break;
    case TUNER_STAGE_GPIO3_RELEASE:
    case TUNER_STAGE_SHUTDOWN_GPIO3_RELEASE:
        snprintf(command, sizeof command, "AT#GPIO=3,0,0,0");
        break;
    case TUNER_STAGE_APPLY_GPIO2:
        if (branch == NULL) {
            return false;
        }
        snprintf(command, sizeof command, "AT#GPIO=2,%u,1,0",
                 branch->gpio2 ? 1u : 0u);
        break;
    case TUNER_STAGE_APPLY_GPIO3:
        if (branch == NULL) {
            return false;
        }
        snprintf(command, sizeof command, "AT#GPIO=3,%u,1,0",
                 branch->gpio3 ? 1u : 0u);
        break;
    case TUNER_STAGE_VERIFY_GPIO2:
        snprintf(command, sizeof command, "AT#GPIO=2,2");
        break;
    case TUNER_STAGE_VERIFY_GPIO3:
        snprintf(command, sizeof command, "AT#GPIO=3,2");
        break;
    case TUNER_STAGE_SHUTDOWN_COMMAND:
        snprintf(command, sizeof command, "AT#SHDN");
        break;
    default:
        return false;
    }

    tuner_reset_parsed_response();
    s_tuner_stage_attempts++;
    if (!diag_uart_write_command(command, true)) {
        return false;
    }
    s_tuner_command_active = true;
    s_tuner_command_deadline_ms = timestamp_ms + timeout_ms;
    printf("[tuner-tx t=%lu attempt=%u] %s\n",
           (unsigned long)timestamp_ms,
           (unsigned)s_tuner_stage_attempts, command);
    return true;
}

static void tuner_start_apply(uint32_t timestamp_ms) {
    s_tuner_apply_branch = s_tuner_desired_branch;
    if (s_tuner_apply_branch == NULL) {
        tuner_set_fault("NO RF BRANCH", timestamp_ms);
        return;
    }
    tuner_set_stage(TUNER_STAGE_APPLY_GPIO2, timestamp_ms);
}

static void tuner_retry_or_fail(const char *reason,
                                uint32_t timestamp_ms) {
    s_tuner_command_active = false;
    if (s_tuner_stage == TUNER_STAGE_PROBE &&
        !tuner_deadline_reached(timestamp_ms,
                               s_tuner_startup_deadline_ms)) {
        s_tuner_next_action_ms = timestamp_ms + TUNER_PROBE_RETRY_MS;
        return;
    }
    if (s_tuner_stage == TUNER_STAGE_SHUTDOWN_COMMAND) {
        tuner_set_stage(TUNER_STAGE_SHUTDOWN_WAIT, timestamp_ms);
        s_tuner_shutdown_deadline_ms = timestamp_ms + 1000u;
        return;
    }
    if (s_tuner_stage_attempts < TUNER_COMMAND_MAX_ATTEMPTS) {
        s_tuner_next_action_ms = timestamp_ms + TUNER_COMMAND_RETRY_MS;
        return;
    }
    if (tuner_stage_is_shutdown_prep(s_tuner_stage)) {
        printf("[tuner t=%lu] restore step failed (%s); continuing to SHDN\n",
               (unsigned long)timestamp_ms, reason);
        tuner_set_stage(TUNER_STAGE_SHUTDOWN_COMMAND, timestamp_ms);
        return;
    }
    tuner_set_fault(reason, timestamp_ms);
}

static void tuner_command_complete(bool success, uint32_t timestamp_ms) {
    tuner_stage_t completed = s_tuner_stage;
    s_tuner_command_active = false;
    if (!success) {
        tuner_retry_or_fail("AT ERROR", timestamp_ms);
        return;
    }

    if (s_tuner_shutdown_requested &&
        completed != TUNER_STAGE_SHUTDOWN_COMMAND &&
        !tuner_stage_is_shutdown_prep(completed)) {
        tuner_begin_shutdown(timestamp_ms);
        return;
    }

    switch (completed) {
    case TUNER_STAGE_PROBE:
        s_tuner_at_ready = true;
        tuner_set_stage(TUNER_STAGE_ECHO_OFF, timestamp_ms);
        break;
    case TUNER_STAGE_ECHO_OFF:
        tuner_set_stage(TUNER_STAGE_CMEE, timestamp_ms);
        break;
    case TUNER_STAGE_CMEE:
        tuner_set_stage(TUNER_STAGE_CFUN_OFF, timestamp_ms);
        break;
    case TUNER_STAGE_CFUN_OFF:
        tuner_set_stage(TUNER_STAGE_CFUN_VERIFY, timestamp_ms);
        break;
    case TUNER_STAGE_CFUN_VERIFY:
        if (!s_tuner_cfun_seen || s_tuner_cfun != 4u) {
            tuner_retry_or_fail("CFUN NOT 4", timestamp_ms);
            break;
        }
        s_tuner_radio_off = true;
        tuner_set_stage(TUNER_STAGE_STUNE_QUERY, timestamp_ms);
        break;
    case TUNER_STAGE_STUNE_QUERY:
        if (!s_tuner_stune_seen) {
            tuner_retry_or_fail("NO STUNE READ", timestamp_ms);
            break;
        }
        s_tuner_original_stune_known = true;
        s_tuner_original_stune_enabled = s_tuner_stune_enabled;
        if (s_tuner_original_stune_enabled) {
            tuner_set_fault("STUNE ENABLED", timestamp_ms);
            break;
        }
        tuner_set_stage(TUNER_STAGE_GPIO2_RELEASE, timestamp_ms);
        break;
    case TUNER_STAGE_GPIO2_RELEASE:
        s_tuner_gpio_touched = true;
        tuner_set_stage(TUNER_STAGE_GPIO3_RELEASE, timestamp_ms);
        break;
    case TUNER_STAGE_GPIO3_RELEASE:
        s_tuner_gpio_touched = true;
        tuner_start_apply(timestamp_ms);
        break;
    case TUNER_STAGE_APPLY_GPIO2:
        tuner_set_stage(TUNER_STAGE_APPLY_GPIO3, timestamp_ms);
        break;
    case TUNER_STAGE_APPLY_GPIO3:
        tuner_set_stage(TUNER_STAGE_VERIFY_GPIO2, timestamp_ms);
        break;
    case TUNER_STAGE_VERIFY_GPIO2:
        if (!s_tuner_gpio_seen || s_tuner_gpio_direction != 1u ||
            s_tuner_apply_branch == NULL ||
            s_tuner_gpio_level != s_tuner_apply_branch->gpio2) {
            tuner_retry_or_fail("GPIO2 VERIFY", timestamp_ms);
            break;
        }
        tuner_set_stage(TUNER_STAGE_VERIFY_GPIO3, timestamp_ms);
        break;
    case TUNER_STAGE_VERIFY_GPIO3:
        if (!s_tuner_gpio_seen || s_tuner_gpio_direction != 1u ||
            s_tuner_apply_branch == NULL ||
            s_tuner_gpio_level != s_tuner_apply_branch->gpio3) {
            tuner_retry_or_fail("GPIO3 VERIFY", timestamp_ms);
            break;
        }
        s_tuner_applied_branch = s_tuner_apply_branch;
        printf("[tuner t=%lu] SELECTED RF%u %s G3:G2=%u%u\n",
               (unsigned long)timestamp_ms,
               (unsigned)s_tuner_applied_branch->rf_path,
               s_tuner_applied_branch->bands,
               s_tuner_applied_branch->gpio3 ? 1u : 0u,
               s_tuner_applied_branch->gpio2 ? 1u : 0u);
        if (s_tuner_desired_branch != s_tuner_applied_branch) {
            tuner_start_apply(timestamp_ms);
        } else {
            tuner_set_stage(TUNER_STAGE_READY, timestamp_ms);
        }
        break;
    case TUNER_STAGE_SHUTDOWN_GPIO2_RELEASE:
        tuner_set_stage(TUNER_STAGE_SHUTDOWN_GPIO3_RELEASE, timestamp_ms);
        break;
    case TUNER_STAGE_SHUTDOWN_GPIO3_RELEASE:
        tuner_set_stage(TUNER_STAGE_SHUTDOWN_COMMAND, timestamp_ms);
        break;
    case TUNER_STAGE_SHUTDOWN_COMMAND:
        diag_uart_park();
        tuner_set_stage(TUNER_STAGE_SHUTDOWN_WAIT, timestamp_ms);
        s_tuner_shutdown_deadline_ms =
            timestamp_ms + TUNER_SHUTDOWN_WAIT_MS;
        break;
    default:
        tuner_set_fault("BAD AT STATE", timestamp_ms);
        break;
    }
}

static void tuner_on_modem_line(const uint8_t *line, size_t len,
                                uint32_t timestamp_ms) {
    if (!s_tuner_command_active) {
        return;
    }
    uint8_t value = 0u;
    bool flag = false;
    uint8_t direction = 0u;
    bool level = false;
    if (telit_tuner_parse_cfun(line, len, &value)) {
        s_tuner_cfun_seen = true;
        s_tuner_cfun = value;
    }
    if (telit_tuner_parse_stuneant(line, len, &flag)) {
        s_tuner_stune_seen = true;
        s_tuner_stune_enabled = flag;
    }
    if (telit_tuner_parse_gpio(line, len, &direction, &level)) {
        s_tuner_gpio_seen = true;
        s_tuner_gpio_direction = direction;
        s_tuner_gpio_level = level;
    }
    telit_tuner_final_t final = telit_tuner_parse_final(line, len);
    if (final != TELIT_TUNER_FINAL_NONE) {
        tuner_command_complete(final == TELIT_TUNER_FINAL_OK,
                               timestamp_ms);
    }
}

static void tuner_finish_power_off(uint32_t timestamp_ms) {
    s_tuner_command_active = false;
    s_tuner_at_ready = false;
    s_tuner_radio_off = false;
    s_tuner_gpio_touched = false;
    s_tuner_applied_branch = NULL;
    s_tuner_apply_branch = NULL;
    s_tuner_shutdown_requested = false;
    fb_clear(&s_tuner_fb, false);
    lcd_show(&s_tuner_lcd, &s_tuner_fb);
    board_set_backlight(false);
    lcd_power_down();
    tuner_set_stage(TUNER_STAGE_OFF, timestamp_ms);
    printf("[tuner t=%lu] OFF confirmed: modem down, 3V8 down\n",
           (unsigned long)timestamp_ms);
}

static void tuner_request_rail_cut(uint32_t timestamp_ms) {
    telit_diag_result_t result =
        telit_diag_request_rail_off(&s_logic, s_observation);
    if (result != TELIT_DIAG_OK) {
        tuner_set_fault(telit_diag_result_text(result), timestamp_ms);
        return;
    }
    tuner_set_stage(TUNER_STAGE_CUTTING_RAIL, timestamp_ms);
}

static void tuner_begin_shutdown(uint32_t timestamp_ms) {
    s_tuner_shutdown_requested = false;
    s_tuner_ui_dirty = true;
    if (s_tuner_command_active) {
        s_tuner_shutdown_requested = true;
        return;
    }
    if (!s_logic.rail_enabled) {
        tuner_finish_power_off(timestamp_ms);
        return;
    }
    if (s_observation.status_valid &&
        s_observation.status_raw <= TUNER_STATUS_OFF_MAX_RAW) {
        tuner_set_stage(TUNER_STAGE_SHUTDOWN_WAIT, timestamp_ms);
        s_tuner_shutdown_deadline_ms =
            timestamp_ms + TUNER_SHUTDOWN_WAIT_MS;
        return;
    }
    if (s_tuner_at_ready) {
        tuner_set_stage(s_tuner_gpio_touched
                            ? TUNER_STAGE_SHUTDOWN_GPIO2_RELEASE
                            : TUNER_STAGE_SHUTDOWN_COMMAND,
                        timestamp_ms);
        return;
    }
    telit_diag_result_t result = telit_diag_request_hw_off_pulse(
        &s_logic, timestamp_ms, s_observation, TUNER_HW_OFF_PULSE_MS);
    if (result != TELIT_DIAG_OK) {
        tuner_set_fault(telit_diag_result_text(result), timestamp_ms);
        return;
    }
    tuner_set_stage(TUNER_STAGE_SHUTDOWN_HW_WAIT, timestamp_ms);
    s_tuner_shutdown_deadline_ms = timestamp_ms + TUNER_HW_OFF_WAIT_MS;
}

static void tuner_start_power_on(uint32_t timestamp_ms) {
    if (s_tuner_stage != TUNER_STAGE_OFF) {
        return;
    }
    lcd_power_up();
    (void)lcd_set_tuning(TUNER_LCD_VOP, LCD_TEMPERATURE_COEFFICIENT,
                         LCD_BIAS_SYSTEM);
    board_set_backlight(true);
    s_tuner_fault[0] = '\0';
    s_tuner_at_ready = false;
    s_tuner_radio_off = false;
    s_tuner_original_stune_known = false;
    s_tuner_original_stune_enabled = false;
    s_tuner_gpio_touched = false;
    s_tuner_command_active = false;
    s_tuner_shutdown_requested = false;
    s_tuner_applied_branch = NULL;
    s_tuner_apply_branch = NULL;
    s_tuner_desired_branch = telit_tuner_branch_for_digit(1u);
    telit_diag_result_t result =
        telit_diag_request_rail_on(&s_logic, false);
    if (result != TELIT_DIAG_OK) {
        tuner_set_fault(telit_diag_result_text(result), timestamp_ms);
        return;
    }
    s_tuner_startup_deadline_ms =
        timestamp_ms + TUNER_STARTUP_TIMEOUT_MS;
    tuner_set_stage(TUNER_STAGE_RAIL_WAIT, timestamp_ms);
    s_tuner_next_action_ms = timestamp_ms + TUNER_RAIL_SETTLE_MS;
    printf("[tuner] RF remains unconfirmed until AT+CFUN? reports 4\n");
}

static void tuner_select_digit(uint8_t digit, uint32_t timestamp_ms) {
    const telit_tuner_branch_t *branch =
        telit_tuner_branch_for_digit(digit);
    if (branch == NULL) {
        printf("[tuner] select expects RF path 1..4\n");
        return;
    }
    if (s_tuner_stage == TUNER_STAGE_OFF ||
        s_tuner_stage == TUNER_STAGE_FAULT ||
        s_tuner_stage >= TUNER_STAGE_SHUTDOWN_GPIO2_RELEASE) {
        printf("[tuner] selection rejected while state=%s\n",
               tuner_stage_text(s_tuner_stage));
        return;
    }
    s_tuner_desired_branch = branch;
    s_tuner_ui_dirty = true;
    printf("[tuner t=%lu] requested RF%u %s G3:G2=%u%u\n",
           (unsigned long)timestamp_ms, (unsigned)branch->rf_path,
           branch->bands, branch->gpio3 ? 1u : 0u,
           branch->gpio2 ? 1u : 0u);
    if (s_tuner_stage == TUNER_STAGE_READY) {
        if (s_tuner_applied_branch != branch) {
            tuner_start_apply(timestamp_ms);
        }
    }
}

static void tuner_poll_controls(uint32_t timestamp_ms) {
    bool power_pressed = !gpio_get(POWER_BUTTON_PIN);
    if (power_pressed && !s_tuner_power_was_pressed) {
        s_tuner_power_pressed_ms = timestamp_ms;
        s_tuner_power_hold_handled = false;
        if (s_tuner_stage == TUNER_STAGE_OFF &&
            !s_tuner_ignore_power_until_release) {
            s_tuner_ignore_power_until_release = true;
            tuner_start_power_on(timestamp_ms);
        }
    }
    if (!power_pressed) {
        s_tuner_ignore_power_until_release = false;
        s_tuner_power_hold_handled = false;
    } else if (!s_tuner_ignore_power_until_release &&
               !s_tuner_power_hold_handled &&
               s_tuner_stage != TUNER_STAGE_OFF &&
               tuner_deadline_reached(
                   timestamp_ms,
                   s_tuner_power_pressed_ms + TUNER_POWER_OFF_HOLD_MS)) {
        s_tuner_power_hold_handled = true;
        if (s_tuner_command_active) {
            s_tuner_shutdown_requested = true;
            s_tuner_ui_dirty = true;
            printf("[tuner t=%lu] power-off queued after active AT command\n",
                   (unsigned long)timestamp_ms);
        } else {
            tuner_begin_shutdown(timestamp_ms);
        }
    }
    s_tuner_power_was_pressed = power_pressed;

    uint16_t keys = tca8418_hal_key_state();
    uint16_t pressed = (uint16_t)(keys & (uint16_t)~s_tuner_previous_keys);
    s_tuner_previous_keys = keys;
    if ((pressed & KEY_1) != 0u) tuner_select_digit(1u, timestamp_ms);
    if ((pressed & KEY_2) != 0u) tuner_select_digit(2u, timestamp_ms);
    if ((pressed & KEY_3) != 0u) tuner_select_digit(3u, timestamp_ms);
    if ((pressed & KEY_4) != 0u) tuner_select_digit(4u, timestamp_ms);
}

static void tuner_tick(uint32_t timestamp_ms) {
    tuner_poll_controls(timestamp_ms);

    if (s_tuner_shutdown_requested && !s_tuner_command_active) {
        tuner_begin_shutdown(timestamp_ms);
    }
    if (s_tuner_command_active &&
        tuner_deadline_reached(timestamp_ms,
                               s_tuner_command_deadline_ms)) {
        printf("[tuner t=%lu] command timeout in %s\n",
               (unsigned long)timestamp_ms,
               tuner_stage_text(s_tuner_stage));
        tuner_retry_or_fail("AT TIMEOUT", timestamp_ms);
    }

    switch (s_tuner_stage) {
    case TUNER_STAGE_OFF:
    case TUNER_STAGE_READY:
    case TUNER_STAGE_FAULT:
        break;
    case TUNER_STAGE_RAIL_WAIT:
        if (tuner_deadline_reached(timestamp_ms,
                                   s_tuner_startup_deadline_ms)) {
            tuner_set_fault("PMIC TIMEOUT", timestamp_ms);
        } else if (s_observation.power_good &&
                   tuner_deadline_reached(timestamp_ms,
                                          s_tuner_next_action_ms)) {
            telit_diag_result_t result = telit_diag_request_boot_pulse(
                &s_logic, timestamp_ms, s_observation,
                TUNER_BOOT_PULSE_MS);
            if (result == TELIT_DIAG_OK) {
                tuner_set_stage(TUNER_STAGE_MODULE_WAIT, timestamp_ms);
            } else {
                tuner_set_fault(telit_diag_result_text(result), timestamp_ms);
            }
        }
        break;
    case TUNER_STAGE_MODULE_WAIT:
        if (tuner_deadline_reached(timestamp_ms,
                                   s_tuner_startup_deadline_ms)) {
            tuner_set_fault("MODEM TIMEOUT", timestamp_ms);
        } else if (s_logic.pulse == TELIT_DIAG_PULSE_NONE &&
                   s_observation.status_valid &&
                   s_observation.status_raw >= TUNER_STATUS_ON_MIN_RAW &&
                   !gpio_get(MODEM_PIN_CTS)) {
            tuner_set_stage(TUNER_STAGE_PROBE, timestamp_ms);
        }
        break;
    case TUNER_STAGE_SHUTDOWN_WAIT:
        if (s_logic.status_low_stable) {
            tuner_request_rail_cut(timestamp_ms);
        } else if (tuner_deadline_reached(timestamp_ms,
                                          s_tuner_shutdown_deadline_ms)) {
            telit_diag_result_t result = telit_diag_request_hw_off_pulse(
                &s_logic, timestamp_ms, s_observation,
                TUNER_HW_OFF_PULSE_MS);
            if (result == TELIT_DIAG_OK) {
                tuner_set_stage(TUNER_STAGE_SHUTDOWN_HW_WAIT,
                                timestamp_ms);
                s_tuner_shutdown_deadline_ms =
                    timestamp_ms + TUNER_HW_OFF_WAIT_MS;
            } else {
                tuner_set_fault(telit_diag_result_text(result), timestamp_ms);
            }
        }
        break;
    case TUNER_STAGE_SHUTDOWN_HW_WAIT:
        if (s_logic.status_low_stable) {
            tuner_request_rail_cut(timestamp_ms);
        } else if (tuner_deadline_reached(timestamp_ms,
                                          s_tuner_shutdown_deadline_ms)) {
            tuner_set_fault("OFF NOT PROVEN", timestamp_ms);
        }
        break;
    case TUNER_STAGE_CUTTING_RAIL:
        if (!s_logic.rail_enabled && !s_applied_rail) {
            tuner_finish_power_off(timestamp_ms);
        }
        break;
    default:
        if (tuner_stage_is_command(s_tuner_stage) &&
            !s_tuner_command_active &&
            tuner_deadline_reached(timestamp_ms,
                                   s_tuner_next_action_ms)) {
            if (!tuner_send_stage_command(timestamp_ms)) {
                tuner_retry_or_fail("UART STALL", timestamp_ms);
            }
        }
        break;
    }
    tuner_draw_ui();
}

static void tuner_print_status(uint32_t timestamp_ms) {
    const telit_tuner_branch_t *branch = s_tuner_applied_branch;
    printf("[tuner-status t=%lu] state=%s at=%u rf_off=%u "
           "cmd=%u stune=%s%u gpio_touched=%u branch=",
           (unsigned long)timestamp_ms, tuner_stage_text(s_tuner_stage),
           s_tuner_at_ready ? 1u : 0u, s_tuner_radio_off ? 1u : 0u,
           s_tuner_command_active ? 1u : 0u,
           s_tuner_original_stune_known ? "" : "unknown:",
           s_tuner_original_stune_enabled ? 1u : 0u,
           s_tuner_gpio_touched ? 1u : 0u);
    if (branch != NULL) {
        printf("RF%u/%s/G3G2=%u%u\n", (unsigned)branch->rf_path,
               branch->bands, branch->gpio3 ? 1u : 0u,
               branch->gpio2 ? 1u : 0u);
    } else {
        printf("none\n");
    }
}

static void tuner_init(void) {
    gpio_init(POWER_BUTTON_PIN);
    gpio_set_dir(POWER_BUTTON_PIN, GPIO_IN);
    gpio_pull_up(POWER_BUTTON_PIN);
    (void)tca8418_hal_init();
    lcd_init(&s_tuner_lcd);
    (void)lcd_set_tuning(TUNER_LCD_VOP, LCD_TEMPERATURE_COEFFICIENT,
                         LCD_BIAS_SYSTEM);
    s_tuner_desired_branch = telit_tuner_branch_for_digit(1u);
    (void)telit_diag_set_off_threshold(&s_logic,
                                       TUNER_STATUS_OFF_MAX_RAW);
    fb_clear(&s_tuner_fb, false);
    lcd_show(&s_tuner_lcd, &s_tuner_fb);
    board_set_backlight(false);
    lcd_power_down();
    s_tuner_stage = TUNER_STAGE_OFF;
    printf("[tuner] Key mapping (physical BGSA throw):\n");
    for (uint8_t digit = 1u; digit <= 4u; digit++) {
        const telit_tuner_branch_t *branch =
            telit_tuner_branch_for_digit(digit);
        printf("[tuner]   %u -> RF%u %-5s GPIO3:GPIO2=%u%u\n",
               (unsigned)digit, (unsigned)branch->rf_path, branch->bands,
               branch->gpio3 ? 1u : 0u, branch->gpio2 ? 1u : 0u);
    }
    printf("[tuner] Press Power to start; hold Power 1s for qualified off.\n");
}
#endif

static size_t escape_modem_line(const uint8_t *line, size_t len) {
    static const char hex[] = "0123456789ABCDEF";
    size_t out = 0u;
    for (size_t i = 0u; i < len && out + 4u < sizeof s_modem_escaped; i++) {
        uint8_t ch = line[i];
        if (ch >= 0x20u && ch <= 0x7eu && ch != '\\') {
            s_modem_escaped[out++] = (char)ch;
        } else if (ch == '\\') {
            s_modem_escaped[out++] = '\\';
            s_modem_escaped[out++] = '\\';
        } else {
            s_modem_escaped[out++] = '\\';
            s_modem_escaped[out++] = 'x';
            s_modem_escaped[out++] = hex[ch >> 4u];
            s_modem_escaped[out++] = hex[ch & 0x0fu];
        }
    }
    s_modem_escaped[out] = '\0';
    return out;
}

static void flush_modem_line(uint32_t timestamp_ms, bool continued) {
    if (s_modem_line_len == 0u) {
        return;
    }
#if defined(SISU_TELIT_TUNER_DIAGNOSTIC)
    tuner_on_modem_line(s_modem_line, s_modem_line_len, timestamp_ms);
#endif
    (void)escape_modem_line(s_modem_line, s_modem_line_len);
    printf("[telit-rx t=%lu%s] %s\n",
           (unsigned long)timestamp_ms,
           continued ? " cont" : "",
           s_modem_escaped);
    s_modem_line_len = 0u;
}

static void drain_modem_uart(uint32_t timestamp_ms) {
    uint8_t byte;
    while (diag_uart_pop(&byte)) {
        if (byte == '\r' || byte == '\n') {
            flush_modem_line(timestamp_ms, false);
            continue;
        }
        s_modem_line[s_modem_line_len++] = byte;
        if (s_modem_line_len == sizeof s_modem_line) {
            flush_modem_line(timestamp_ms, true);
        }
    }
}

static int64_t pulse_alarm_callback(alarm_id_t id, void *user_data) {
    uint pin = (uint)(uintptr_t)user_data;
    gpio_put(pin, false);
    if (s_pulse_alarm_valid && s_pulse_alarm_id == id) {
        s_pulse_alarm_valid = false;
    }
    return 0;
}

static void cancel_pulse_alarm(void) {
    uint32_t irq_state = save_and_disable_interrupts();
    if (s_pulse_alarm_valid) {
        (void)cancel_alarm(s_pulse_alarm_id);
        s_pulse_alarm_valid = false;
    }
    restore_interrupts(irq_state);
    gpio_put(MODEM_PIN_ON_OFF, false);
    gpio_put(MODEM_PIN_HW_SHUTDOWN, false);
}

static bool sync_pulse_guard(uint32_t timestamp_ms) {
    if (s_logic.pulse == s_applied_pulse &&
        (s_logic.pulse == TELIT_DIAG_PULSE_NONE ||
         s_logic.pulse_sequence == s_applied_pulse_sequence)) {
        return true;
    }
    cancel_pulse_alarm();
    s_applied_pulse = TELIT_DIAG_PULSE_NONE;
    s_applied_pulse_sequence = 0u;
    if (s_logic.pulse == TELIT_DIAG_PULSE_NONE) {
        return true;
    }

    int32_t remaining =
        (int32_t)(s_logic.pulse_deadline_ms - timestamp_ms);
    if (remaining <= 0 || s_logic.pulse_width_ms == 0u) {
        telit_diag_abort_pulse(&s_logic);
        return false;
    }
    uint pin = telit_diag_emergency_asserted(&s_logic)
                   ? MODEM_PIN_HW_SHUTDOWN
                   : MODEM_PIN_ON_OFF;
    gpio_put(pin, true);
    uint32_t actual_start_ms = now_ms();
    alarm_id_t alarm = add_alarm_in_ms(
        s_logic.pulse_width_ms, pulse_alarm_callback,
        (void *)(uintptr_t)pin, true);
    if (alarm < 0) {
        gpio_put(pin, false);
        telit_diag_abort_pulse(&s_logic);
        return false;
    }
    s_pulse_alarm_id = alarm;
    s_pulse_alarm_valid = true;
    /* The hardware alarm owns the exact deassertion. Keep the model deadline
     * one scheduler millisecond later so its coarse ms clock cannot shorten
     * the physical pulse through truncation. */
    s_logic.pulse_deadline_ms =
        actual_start_ms + s_logic.pulse_width_ms + 1u;
    s_applied_pulse = s_logic.pulse;
    s_applied_pulse_sequence = s_logic.pulse_sequence;
    return true;
}

static bool apply_logic_outputs(uint32_t timestamp_ms) {
    if (!s_logic.rail_enabled) {
#if defined(SISU_TELIT_DVI_DIAGNOSTIC)
        telit_dvi_diag_stop();
#endif
        cancel_pulse_alarm();
        s_applied_pulse = TELIT_DIAG_PULSE_NONE;
        s_applied_pulse_sequence = 0u;
        gpio_put(MODEM_PIN_DTR, false);
        diag_uart_park();
        board_3v8_rail_set_force_pwm(false);
        board_3v8_rail_set_enabled(false);
        s_applied_rail = false;
        return true;
    }

    if (!s_applied_rail) {
        cancel_pulse_alarm();
        gpio_put(MODEM_PIN_DTR, false);
        board_3v8_rail_set_force_pwm(s_logic.force_pwm);
        board_3v8_rail_set_enabled(true);
        s_applied_rail = true;
    }
    gpio_put(MODEM_PIN_DTR, s_logic.dtr_sleep_permitted);
    return sync_pulse_guard(timestamp_ms);
}

static void refresh_observation(void) {
    uint16_t raw = 0u;
    uint16_t mv = 0u;
    bool valid = modem_power_monitor_hal_read(&raw, &mv);
    s_observation.power_good = board_3v8_rail_power_good();
    s_observation.status_valid = valid;
    s_observation.status_fresh = true;
    s_observation.status_raw = raw;
    s_status_mv = mv;
}

static void log_observation_edges(uint32_t timestamp_ms) {
    bool pg = s_observation.power_good;
    bool ri_n = gpio_get(MODEM_PIN_RI);
    bool cts_n = gpio_get(MODEM_PIN_CTS);
    bool status_known =
        s_logic.off_threshold_set && s_observation.status_valid;
    bool status_above_threshold =
        status_known &&
        s_observation.status_raw > s_logic.off_threshold_raw;
    if (!s_edges_initialized || pg != s_last_pg ||
        ri_n != s_last_ri_n || cts_n != s_last_cts_n ||
        (status_known &&
         (!s_status_edge_initialized ||
          status_above_threshold != s_last_status_above_threshold))) {
        printf("[telit-edge t=%lu] pg=%u ri_n=%u cts_n=%u "
               "stat=%s%u/%umV stat_on=%s\n",
               (unsigned long)timestamp_ms,
               pg ? 1u : 0u,
               ri_n ? 1u : 0u,
               cts_n ? 1u : 0u,
               s_observation.status_valid ? "" : "invalid:",
               (unsigned)s_observation.status_raw,
               (unsigned)s_status_mv,
               status_known
                   ? (status_above_threshold ? "1" : "0")
                   : "?");
        s_edges_initialized = true;
        s_last_pg = pg;
        s_last_ri_n = ri_n;
        s_last_cts_n = cts_n;
        if (status_known) {
            s_status_edge_initialized = true;
            s_last_status_above_threshold = status_above_threshold;
        }
    }
}

static void print_monitor(uint32_t timestamp_ms) {
    ltc2959_snapshot_t ltc;
    ltc2959_hal_get_snapshot(&ltc);
    printf("[telit-mon t=%lu] rail=%u pg=%u pwm=%u "
           "stat=%s%u/%umV off_th=%s%u ready=%u "
           "on=%u shdn=%u dtr=%u "
           "ri_n=%u cts_n=%u rx=%u uart=%u flow=%s "
           "pulse=%s drop=%lu oe=%lu fe=%lu pe=%lu be=%lu "
           "ltc=%u/%u/%u v=%u i=%ld seq=%lu i2c=%lu\n",
           (unsigned long)timestamp_ms,
           s_logic.rail_enabled ? 1u : 0u,
           s_observation.power_good ? 1u : 0u,
           s_logic.force_pwm ? 1u : 0u,
           s_observation.status_valid ? "" : "invalid:",
           (unsigned)s_observation.status_raw,
           (unsigned)s_status_mv,
           s_logic.off_threshold_set ? "" : "unset:",
           (unsigned)s_logic.off_threshold_raw,
           s_logic.status_low_stable ? 1u : 0u,
           gpio_get_out_level(MODEM_PIN_ON_OFF) ? 1u : 0u,
           gpio_get_out_level(MODEM_PIN_HW_SHUTDOWN) ? 1u : 0u,
           gpio_get_out_level(MODEM_PIN_DTR) ? 1u : 0u,
           gpio_get(MODEM_PIN_RI) ? 1u : 0u,
           gpio_get(MODEM_PIN_CTS) ? 1u : 0u,
           gpio_get(MODEM_PIN_RX) ? 1u : 0u,
           s_uart_initialized ? 1u : 0u,
           s_uart_hw_flow ? "hw" : "none",
           telit_diag_pulse_text(s_logic.pulse),
           (unsigned long)s_uart_rx_dropped,
           (unsigned long)s_uart_rx_overruns,
           (unsigned long)s_uart_rx_framing_errors,
           (unsigned long)s_uart_rx_parity_errors,
           (unsigned long)s_uart_rx_breaks,
           s_ltc_init_ok ? 1u : 0u,
           ltc.present ? 1u : 0u,
           ltc.sample_valid ? 1u : 0u,
           (unsigned)ltc.voltage_mv,
           (long)ltc.current_ua,
           (unsigned long)ltc.sample_sequence,
           (unsigned long)ltc.i2c_error_count);
}

static bool parse_u32(const char *text, uint32_t *out) {
    if (text == NULL || text[0] == '\0' || out == NULL ||
        text[0] == '-') {
        return false;
    }
    char *end = NULL;
    errno = 0;
    unsigned long value = strtoul(text, &end, 10);
    if (errno == ERANGE || end == text || *end != '\0' ||
        value > UINT32_MAX) {
        return false;
    }
    *out = (uint32_t)value;
    return true;
}

static bool parse_flow(const char *text, bool *hardware_flow) {
    if (strcmp(text, "hw") == 0) {
        *hardware_flow = true;
        return true;
    }
    if (strcmp(text, "none") == 0) {
        *hardware_flow = false;
        return true;
    }
    return false;
}

static char *next_token(char **cursor) {
    char *p = *cursor;
    while (*p == ' ') {
        p++;
    }
    if (*p == '\0') {
        *cursor = p;
        return NULL;
    }
    char *start = p;
    while (*p != '\0' && *p != ' ') {
        p++;
    }
    if (*p != '\0') {
        *p++ = '\0';
    }
    *cursor = p;
    return start;
}

static char *remaining_text(char *cursor) {
    while (*cursor == ' ') {
        cursor++;
    }
    return *cursor == '\0' ? NULL : cursor;
}

static bool no_more_arguments(char *cursor) {
    return next_token(&cursor) == NULL;
}

static void print_help(void) {
#if defined(SISU_TELIT_TUNER_DIAGNOSTIC)
    printf("[tuner] Commands:\n");
    printf("  status\n");
    printf("  power <on|off>\n");
    printf("  select <1..4>\n");
    printf("[tuner] Mutating raw AT/rail commands are locked in this image.\n");
#else
    printf("[telit] Commands:\n");
    printf("  status\n");
    printf("  threshold <raw 0..%u>\n",
           (unsigned)TELIT_DIAG_OFF_THRESHOLD_MAX_RAW);
    printf("  rail on <save|pwm>\n");
    printf("  rail off\n");
    printf("  boot <1000..2000 ms> <none|hw>\n");
    printf("  send <none|hw> <AT command>\n");
    printf("  shutdown <none|hw>\n");
    printf("  dtr <wake|sleep>\n");
    printf("  hw-off <2500..5000 ms>\n");
    printf("  emergency arm\n");
    printf("  emergency fire\n");
    printf("  abort\n");
#if defined(SISU_TELIT_DVI_DIAGNOSTIC)
    printf("  dvi init <handset|headset>\n");
    printf("  dvi route <handset|headset>\n");
    printf("  dvi start\n");
    printf("  dvi stop\n");
    printf("  dvi status\n");
    printf("  dvi measure\n");
    printf("[dvi] OAP remains explicit: send hw AT#OAP=1, then dvi start.\n");
    printf("[dvi] Stop in reverse order: dvi stop, then send hw AT#OAP=0.\n");
#endif
    printf("[telit] 'sleep' means DTR pad high; 'wake' means pad low.\n");
    printf("[telit] Rail-off requires a valid GP40 sample at/below the "
           "operator-set threshold.\n");
#endif
}

static void print_result(const char *operation,
                         telit_diag_result_t result,
                         uint32_t timestamp_ms) {
    printf("[telit t=%lu] %s: %s\n",
           (unsigned long)timestamp_ms,
           operation,
           telit_diag_result_text(result));
}

#if defined(SISU_TELIT_DVI_DIAGNOSTIC)
static bool parse_dvi_route(const char *text, telit_dvi_route_t *route) {
    if (text != NULL && strcmp(text, "handset") == 0) {
        *route = TELIT_DVI_ROUTE_HANDSET;
        return true;
    }
    if (text != NULL && strcmp(text, "headset") == 0) {
        *route = TELIT_DVI_ROUTE_HEADSET;
        return true;
    }
    return false;
}

static bool dvi_power_ready(uint32_t timestamp_ms,
                            const char *operation) {
    refresh_observation();
    if (telit_diag_uart_tx_allowed(&s_logic, s_observation)) {
        return true;
    }
    printf("[dvi t=%lu] %s: modem rail/PG/pulse state not ready\n",
           (unsigned long)timestamp_ms, operation);
    return false;
}

static void print_dvi_status(uint32_t timestamp_ms) {
    telit_dvi_diag_status_t status;
    telit_dvi_diag_get_status(&status);
    printf("[dvi t=%lu] init=%u codec=%u ci2s=%u mi2s=%u probe=%u "
           "route=%s pending=%u qualify=%u active=%u bclk=%u fresh=%u seen=%u\n",
           (unsigned long)timestamp_ms,
           status.initialized ? 1u : 0u,
           status.codec_ready ? 1u : 0u,
           status.codec_i2s_ready ? 1u : 0u,
           status.modem_i2s_ready ? 1u : 0u,
           status.clock_probe_ready ? 1u : 0u,
           telit_dvi_diag_route_text(status.route),
           status.start_pending ? 1u : 0u,
           status.bclk_qualifying ? 1u : 0u,
           status.bridge.active ? 1u : 0u,
           status.bclk_live ? 1u : 0u,
           status.bclk_fresh ? 1u : 0u,
           status.saw_bclk ? 1u : 0u);
    printf("[dvi-codec] tx=%lu rx=%lu last=%d/%d peak=%d/%d abort=%lu\n",
           (unsigned long)status.codec_i2s.tx_irq_count,
           (unsigned long)status.codec_i2s.rx_irq_count,
           (int)status.codec_i2s.last_left,
           (int)status.codec_i2s.last_right,
           (int)status.codec_i2s.peak_left,
           (int)status.codec_i2s.peak_right,
           (unsigned long)status.codec_i2s.abort_timeouts);
    printf("[dvi-modem] rx=%lu tx=%lu low/high=%d/%d mismatch=%lu "
           "relock=%lu/%u abort=%lu fifo=%lu sm=%u busy=%u\n",
           (unsigned long)status.modem_i2s.rx_irq_count,
           (unsigned long)status.modem_tx.tx_irq_count,
           (int)status.modem_i2s.last_wa_low,
           (int)status.modem_i2s.last_wa_high,
           (unsigned long)status.modem_i2s.last_slot_mismatch,
           (unsigned long)status.modem_i2s.relock_total,
           (unsigned)status.modem_i2s.relock_attempts,
           (unsigned long)status.modem_i2s.abort_timeouts,
           (unsigned long)status.modem_tx.tx_fifo_level,
           status.modem_tx.tx_sm_enabled ? 1u : 0u,
           status.modem_tx.dma0_busy ? 1u : 0u);
    printf("[dvi-bridge] dl=%u uf=%lu of=%lu ul=%u uf=%lu of=%lu\n",
           (unsigned)status.bridge.downlink_depth,
           (unsigned long)status.bridge.downlink_underflow,
           (unsigned long)status.bridge.downlink_overflow,
           (unsigned)status.bridge.uplink_depth,
           (unsigned long)status.bridge.uplink_underflow,
           (unsigned long)status.bridge.uplink_overflow);
}

static void handle_dvi(char *cursor, uint32_t timestamp_ms) {
    char *action = next_token(&cursor);
    if (action == NULL) {
        printf("[dvi] usage: dvi <init|route|start|stop|status|measure>\n");
        return;
    }
    if (strcmp(action, "init") == 0 || strcmp(action, "route") == 0) {
        telit_dvi_route_t route;
        char *route_text = next_token(&cursor);
        if (!parse_dvi_route(route_text, &route) ||
            !no_more_arguments(cursor)) {
            printf("[dvi] usage: dvi %s <handset|headset>\n", action);
            return;
        }
        if (strcmp(action, "init") == 0 &&
            !dvi_power_ready(timestamp_ms, "init")) {
            return;
        }
        bool ok = strcmp(action, "init") == 0
                      ? telit_dvi_diag_init(route)
                      : telit_dvi_diag_set_route(route);
        printf("[dvi t=%lu] %s %s: %s\n",
               (unsigned long)timestamp_ms, action, route_text,
               ok ? "OK" : "FAILED/BUSY");
        print_dvi_status(timestamp_ms);
        return;
    }
    if (!no_more_arguments(cursor)) {
        printf("[dvi] usage: dvi %s\n", action);
        return;
    }
    if (strcmp(action, "start") == 0) {
        if (!dvi_power_ready(timestamp_ms, "start")) {
            return;
        }
        bool ok = telit_dvi_diag_start();
        printf("[dvi t=%lu] start: %s%s\n",
               (unsigned long)timestamp_ms,
               ok ? "PENDING" : "FAILED",
               ok ? " (waiting for 20ms stable BCLK)" : "");
        return;
    }
    if (strcmp(action, "stop") == 0) {
        telit_dvi_diag_stop();
        printf("[dvi t=%lu] stop: OK\n", (unsigned long)timestamp_ms);
        return;
    }
    if (strcmp(action, "status") == 0) {
        print_dvi_status(timestamp_ms);
        return;
    }
    if (strcmp(action, "measure") == 0) {
        telit_dvi_clock_measurement_t clocks;
        bool ok = telit_dvi_diag_measure_clocks(&clocks);
        printf("[dvi-clock t=%lu] bclk=%s%luHz wa=%s%luHz "
               "ratio=%lu.%03lu clocks/frame result=%s\n",
               (unsigned long)timestamp_ms,
               clocks.bclk_valid ? "" : "invalid:",
               (unsigned long)clocks.bclk_hz,
               clocks.wa_valid ? "" : "invalid:",
               (unsigned long)clocks.wa_hz,
               (unsigned long)(clocks.bclk_per_frame_x1000 / 1000u),
               (unsigned long)(clocks.bclk_per_frame_x1000 % 1000u),
               ok ? "OK" : "NO CLOCK/PROBE");
        return;
    }
    printf("[dvi] unknown action: %s\n", action);
}
#endif

static void handle_send(char *cursor, uint32_t timestamp_ms,
                        const char *operation, const char *fixed_command) {
    char *flow_text = next_token(&cursor);
    bool hardware_flow;
    if (flow_text == NULL || !parse_flow(flow_text, &hardware_flow)) {
        printf("[telit] usage: %s <none|hw>%s\n",
               operation,
               fixed_command == NULL ? " <AT command>" : "");
        return;
    }
    const char *command = fixed_command == NULL ? remaining_text(cursor)
                                                : fixed_command;
    if (command == NULL) {
        printf("[telit] usage: send <none|hw> <AT command>\n");
        return;
    }
    if (fixed_command != NULL && !no_more_arguments(cursor)) {
        printf("[telit] usage: %s <none|hw>\n", operation);
        return;
    }
    refresh_observation();
    if (!telit_diag_uart_tx_allowed(&s_logic, s_observation)) {
        print_result(operation, TELIT_DIAG_ERR_STATE, timestamp_ms);
        return;
    }
    bool ok = diag_uart_write_command(command, hardware_flow);
    printf("[telit-tx t=%lu flow=%s] %s%s\n",
           (unsigned long)timestamp_ms,
           hardware_flow ? "hw" : "none",
           command,
           ok ? "" : " [REJECTED/STALL]");
}

static void handle_command(char *line, uint32_t timestamp_ms) {
    char *cursor = line;
    char *command = next_token(&cursor);
    if (command == NULL) {
        return;
    }
    if (strcmp(command, "help") == 0) {
        if (!no_more_arguments(cursor)) {
            printf("[telit] usage: help\n");
            return;
        }
        print_help();
        return;
    }
    if (strcmp(command, "status") == 0) {
        if (!no_more_arguments(cursor)) {
            printf("[telit] usage: status\n");
            return;
        }
        refresh_observation();
        print_monitor(timestamp_ms);
#if defined(SISU_TELIT_TUNER_DIAGNOSTIC)
        tuner_print_status(timestamp_ms);
#endif
        return;
    }
#if defined(SISU_TELIT_TUNER_DIAGNOSTIC)
    if (strcmp(command, "power") == 0) {
        char *action = next_token(&cursor);
        if (action == NULL || !no_more_arguments(cursor) ||
            (strcmp(action, "on") != 0 && strcmp(action, "off") != 0)) {
            printf("[tuner] usage: power <on|off>\n");
            return;
        }
        if (strcmp(action, "on") == 0) {
            tuner_start_power_on(timestamp_ms);
        } else if (s_tuner_stage == TUNER_STAGE_OFF) {
            printf("[tuner] already off\n");
        } else if (s_tuner_command_active) {
            s_tuner_shutdown_requested = true;
            s_tuner_ui_dirty = true;
            printf("[tuner t=%lu] CDC power-off queued after active AT command\n",
                   (unsigned long)timestamp_ms);
        } else {
            tuner_begin_shutdown(timestamp_ms);
        }
        return;
    }
    if (strcmp(command, "select") == 0) {
        uint32_t digit = 0u;
        char *digit_text = next_token(&cursor);
        if (digit_text == NULL || !parse_u32(digit_text, &digit) ||
            digit < 1u || digit > 4u || !no_more_arguments(cursor)) {
            printf("[tuner] usage: select <1..4>\n");
            return;
        }
        tuner_select_digit((uint8_t)digit, timestamp_ms);
        return;
    }
    printf("[tuner] command locked in antenna-tuning image: %s\n",
           command);
    return;
#endif
    if (strcmp(command, "threshold") == 0) {
        uint32_t raw;
        char *raw_text = next_token(&cursor);
        telit_diag_result_t result =
            raw_text != NULL && parse_u32(raw_text, &raw) &&
                    raw <= UINT16_MAX && no_more_arguments(cursor)
                ? telit_diag_set_off_threshold(&s_logic, (uint16_t)raw)
                : TELIT_DIAG_ERR_RANGE;
        if (result == TELIT_DIAG_OK) {
            s_status_edge_initialized = false;
        }
        print_result("threshold", result, timestamp_ms);
        return;
    }
    if (strcmp(command, "rail") == 0) {
        char *action = next_token(&cursor);
        if (action != NULL && strcmp(action, "on") == 0) {
            char *mode = next_token(&cursor);
            if (mode == NULL ||
                (strcmp(mode, "save") != 0 &&
                 strcmp(mode, "pwm") != 0) ||
                !no_more_arguments(cursor)) {
                printf("[telit] usage: rail on <save|pwm>\n");
                return;
            }
            telit_diag_result_t result = telit_diag_request_rail_on(
                &s_logic, strcmp(mode, "pwm") == 0);
            print_result("rail on", result, timestamp_ms);
            return;
        }
        if (action != NULL && strcmp(action, "off") == 0) {
            if (!no_more_arguments(cursor)) {
                printf("[telit] usage: rail off\n");
                return;
            }
            refresh_observation();
            telit_diag_result_t result =
                telit_diag_request_rail_off(&s_logic, s_observation);
            print_result("rail off", result, timestamp_ms);
            return;
        }
        printf("[telit] usage: rail <on save|on pwm|off>\n");
        return;
    }
    if (strcmp(command, "boot") == 0) {
        uint32_t width;
        bool hardware_flow;
        char *width_text = next_token(&cursor);
        char *flow_text = next_token(&cursor);
        if (width_text == NULL || flow_text == NULL ||
            !parse_u32(width_text, &width) ||
            !parse_flow(flow_text, &hardware_flow) ||
            !no_more_arguments(cursor)) {
            printf("[telit] usage: boot <1000..2000 ms> <none|hw>\n");
            return;
        }
        refresh_observation();
        telit_diag_result_t result = telit_diag_request_boot_pulse(
            &s_logic, timestamp_ms, s_observation, width);
        if (result == TELIT_DIAG_OK &&
            !diag_uart_set_flow(hardware_flow)) {
            telit_diag_abort_pulse(&s_logic);
            result = TELIT_DIAG_ERR_STATE;
        }
        print_result("boot", result, timestamp_ms);
        return;
    }
    if (strcmp(command, "send") == 0) {
        handle_send(cursor, timestamp_ms, "send", NULL);
        return;
    }
    if (strcmp(command, "shutdown") == 0) {
        handle_send(cursor, timestamp_ms, "shutdown", "AT#SHDN");
        return;
    }
    if (strcmp(command, "dtr") == 0) {
        char *mode = next_token(&cursor);
        if (mode == NULL ||
            (strcmp(mode, "wake") != 0 &&
             strcmp(mode, "sleep") != 0) ||
            !no_more_arguments(cursor)) {
            printf("[telit] usage: dtr <wake|sleep>\n");
            return;
        }
        refresh_observation();
        telit_diag_result_t result = telit_diag_request_dtr(
            &s_logic, strcmp(mode, "sleep") == 0, s_observation);
        print_result("dtr", result, timestamp_ms);
        return;
    }
    if (strcmp(command, "hw-off") == 0) {
        uint32_t width;
        char *width_text = next_token(&cursor);
        if (width_text == NULL || !parse_u32(width_text, &width) ||
            !no_more_arguments(cursor)) {
            printf("[telit] usage: hw-off <2500..5000 ms>\n");
            return;
        }
        refresh_observation();
        telit_diag_result_t result = telit_diag_request_hw_off_pulse(
            &s_logic, timestamp_ms, s_observation, width);
        print_result("hw-off", result, timestamp_ms);
        return;
    }
    if (strcmp(command, "emergency") == 0) {
        char *action = next_token(&cursor);
        if (!no_more_arguments(cursor)) {
            printf("[telit] usage: emergency <arm|fire>\n");
            return;
        }
        refresh_observation();
        telit_diag_result_t result;
        if (action != NULL && strcmp(action, "arm") == 0) {
            result = telit_diag_request_emergency_arm(
                &s_logic, timestamp_ms, s_observation);
        } else if (action != NULL && strcmp(action, "fire") == 0) {
            result = telit_diag_request_emergency_fire(
                &s_logic, timestamp_ms, s_observation);
        } else {
            printf("[telit] usage: emergency <arm|fire>\n");
            return;
        }
        print_result("emergency", result, timestamp_ms);
        return;
    }
    if (strcmp(command, "abort") == 0) {
        if (!no_more_arguments(cursor)) {
            printf("[telit] usage: abort\n");
            return;
        }
        telit_diag_abort_pulse(&s_logic);
        print_result("abort", TELIT_DIAG_OK, timestamp_ms);
        return;
    }
#if defined(SISU_TELIT_DVI_DIAGNOSTIC)
    if (strcmp(command, "dvi") == 0) {
        handle_dvi(cursor, timestamp_ms);
        return;
    }
#endif
    printf("[telit] unknown command: %s\n", command);
}

static bool poll_console(void) {
    bool command_processed = false;
    int ch;
    while ((ch = getchar_timeout_us(0)) != PICO_ERROR_TIMEOUT) {
        if (ch == '\r' || ch == '\n') {
            if (s_console_discarding) {
                printf("[telit] console line discarded: too long\n");
            } else {
                s_console_line[s_console_len] = '\0';
                handle_command(s_console_line, now_ms());
                command_processed = true;
            }
            s_console_len = 0u;
            s_console_discarding = false;
            continue;
        }
        if (ch == 0x08 || ch == 0x7f) {
            if (!s_console_discarding && s_console_len > 0u) {
                s_console_len--;
            }
            continue;
        }
        if (!isprint((unsigned char)ch)) {
            continue;
        }
        if (s_console_discarding) {
            continue;
        }
        if (s_console_len + 1u >= sizeof s_console_line) {
            s_console_discarding = true;
            continue;
        }
        s_console_line[s_console_len++] = (char)ch;
    }
    return command_processed;
}

static void apply_outputs_or_report(uint32_t timestamp_ms) {
    if (!apply_logic_outputs(timestamp_ms)) {
        printf("[telit t=%lu] pulse guard unavailable; control deasserted\n",
               (unsigned long)timestamp_ms);
    }
}

int main(void) {
    board_set_system_clock();
    board_init();
    modem_power_monitor_hal_init();
    telit_diag_logic_init(&s_logic);
    s_ltc_init_ok = ltc2959_hal_init(now_ms());

    gpio_init(MODEM_PIN_DTR);
    gpio_put(MODEM_PIN_DTR, false);
    gpio_set_dir(MODEM_PIN_DTR, GPIO_OUT);
    gpio_set_input_enabled(MODEM_PIN_DTR, false);

    gpio_init(MODEM_PIN_RI);
    gpio_set_dir(MODEM_PIN_RI, GPIO_IN);
    gpio_disable_pulls(MODEM_PIN_RI);
    gpio_set_input_enabled(MODEM_PIN_RI, true);

    diag_uart_park();
    board_3v8_rail_set_force_pwm(false);
    board_3v8_rail_set_enabled(false);
    stdio_init_all();

    refresh_observation();
#if defined(SISU_TELIT_TUNER_DIAGNOSTIC)
    printf("[telit] Rev B2 passive antenna-tuning diagnostic\n");
    printf("[telit] RF branch control is accepted only after CFUN=4 readback.\n");
    tuner_init();
#elif defined(SISU_TELIT_DVI_DIAGNOSTIC)
    printf("[telit] Rev B2 LE910C1 DVI/OAP characterization diagnostic\n");
#else
    printf("[telit] Rev B2 LE910C1 characterization diagnostic\n");
#endif
    printf("[telit] Production backend remains NONE. No automatic power-on.\n");
    printf("[telit] Modem USB is available separately on service pads 5/6.\n");
    print_help();

    uint32_t next_sample_ms = 0u;
    uint32_t next_monitor_ms = 0u;
    while (true) {
        uint32_t timestamp_ms = now_ms();
        if ((int32_t)(timestamp_ms - next_sample_ms) >= 0) {
            next_sample_ms = timestamp_ms + DIAG_SAMPLE_PERIOD_MS;
            refresh_observation();
            log_observation_edges(timestamp_ms);
        }
        ltc2959_hal_poll(timestamp_ms, false);
#if defined(SISU_TELIT_TUNER_DIAGNOSTIC)
        drain_modem_uart(timestamp_ms);
        tuner_tick(timestamp_ms);
#endif
        telit_diag_logic_tick(&s_logic, timestamp_ms, s_observation);
        s_observation.status_fresh = false;
        apply_outputs_or_report(timestamp_ms);
#if defined(SISU_TELIT_DVI_DIAGNOSTIC)
        telit_dvi_diag_tick(timestamp_ms);
#endif
#if !defined(SISU_TELIT_TUNER_DIAGNOSTIC)
        drain_modem_uart(timestamp_ms);
#endif
        if (poll_console()) {
            timestamp_ms = now_ms();
            telit_diag_logic_tick(&s_logic, timestamp_ms, s_observation);
            s_observation.status_fresh = false;
            apply_outputs_or_report(timestamp_ms);
        }
        timestamp_ms = now_ms();
#if !defined(SISU_TELIT_TUNER_DIAGNOSTIC)
        if ((int32_t)(timestamp_ms - next_monitor_ms) >= 0) {
            next_monitor_ms = timestamp_ms + DIAG_MONITOR_PERIOD_MS;
            print_monitor(timestamp_ms);
        }
#else
        (void)next_monitor_ms;
#endif
        sleep_ms(1u);
    }
}

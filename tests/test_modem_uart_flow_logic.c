#include "hal/modem_uart_flow_logic.h"

#include <stdio.h>

static int failures;

static void check(bool condition, const char *message) {
    if (!condition) {
        printf("FAIL: %s\n", message);
        failures++;
    }
}

int main(void) {
    check(modem_uart_rx_ring_occupancy(100u, 20u) == 80u,
          "ordinary occupancy is head minus tail");
    check(modem_uart_rx_ring_occupancy(10u, 1000u) == 34u,
          "occupancy is correct across ring wrap");

    check(modem_uart_rx_flow_action(MODEM_UART_RX_PAUSE_LEVEL - 1u, false) ==
              MODEM_UART_RX_FLOW_KEEP,
          "flow remains enabled below the high-water mark");
    check(modem_uart_rx_flow_action(MODEM_UART_RX_PAUSE_LEVEL, false) ==
              MODEM_UART_RX_FLOW_PAUSE,
          "high-water mark deasserts RTS before the ring fills");
    check(modem_uart_rx_flow_action(MODEM_UART_RX_RESUME_LEVEL + 1u, true) ==
              MODEM_UART_RX_FLOW_KEEP,
          "paused flow retains hysteresis above the low-water mark");
    check(modem_uart_rx_flow_action(MODEM_UART_RX_RESUME_LEVEL, true) ==
              MODEM_UART_RX_FLOW_RESUME,
          "low-water mark resumes the modem");

    if (failures == 0) {
        puts("PASS: modem UART flow logic");
    }
    return failures == 0 ? 0 : 1;
}

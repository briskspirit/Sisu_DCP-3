#ifndef MODEM_UART_FLOW_LOGIC_H
#define MODEM_UART_FLOW_LOGIC_H

#include <stdbool.h>
#include <stdint.h>

/* PL011 auto-RTS reflects only its 32-byte hardware FIFO. The RX ISR empties
 * that FIFO into this larger software ring, so software must stop and resume
 * the modem before the ring itself runs out of space. */
#define MODEM_UART_RX_RING_SIZE 1024u
#define MODEM_UART_RX_RING_MASK (MODEM_UART_RX_RING_SIZE - 1u)
#define MODEM_UART_RX_PAUSE_LEVEL ((MODEM_UART_RX_RING_SIZE * 7u) / 8u)
#define MODEM_UART_RX_RESUME_LEVEL (MODEM_UART_RX_RING_SIZE / 2u)

typedef enum {
    MODEM_UART_RX_FLOW_KEEP = 0,
    MODEM_UART_RX_FLOW_PAUSE,
    MODEM_UART_RX_FLOW_RESUME,
} modem_uart_rx_flow_action_t;

_Static_assert((MODEM_UART_RX_RING_SIZE & MODEM_UART_RX_RING_MASK) == 0u,
               "modem UART RX ring size must be a power of two");
_Static_assert(MODEM_UART_RX_RESUME_LEVEL < MODEM_UART_RX_PAUSE_LEVEL,
               "modem UART RX flow thresholds need hysteresis");

#if defined(__GNUC__)
#define MODEM_UART_FLOW_INLINE static inline __attribute__((always_inline))
#else
#define MODEM_UART_FLOW_INLINE static inline
#endif

MODEM_UART_FLOW_INLINE uint32_t modem_uart_rx_ring_occupancy(
    uint32_t head, uint32_t tail) {
    return (head - tail) & MODEM_UART_RX_RING_MASK;
}

MODEM_UART_FLOW_INLINE modem_uart_rx_flow_action_t modem_uart_rx_flow_action(
    uint32_t occupancy, bool paused) {
    if (!paused && occupancy >= MODEM_UART_RX_PAUSE_LEVEL) {
        return MODEM_UART_RX_FLOW_PAUSE;
    }
    if (paused && occupancy <= MODEM_UART_RX_RESUME_LEVEL) {
        return MODEM_UART_RX_FLOW_RESUME;
    }
    return MODEM_UART_RX_FLOW_KEEP;
}

#undef MODEM_UART_FLOW_INLINE

#endif

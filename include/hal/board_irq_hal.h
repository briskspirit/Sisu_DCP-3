#ifndef BOARD_IRQ_HAL_H
#define BOARD_IRQ_HAL_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t shared_falling_edges;
    uint32_t service_vbus_edges;
    uint32_t modem_ri_falling_edges;
    bool shared_pending;
    bool service_vbus_pending;
    bool modem_ri_pending;
} board_irq_snapshot_t;

/* Pico GPIO callbacks are registered once per core, not once per pin. This
 * module is therefore the sole active-mode callback owner for the Rev B2
 * interrupt inputs. The ISR only latches evidence; all device I/O stays on
 * core 0 in the main loop. Modem RI is enabled dynamically with its UART. */
void board_irq_hal_init(void);
bool board_irq_hal_take_shared_pending(void);
bool board_irq_hal_take_service_vbus_pending(void);
void board_irq_hal_set_modem_ri_enabled(bool enabled);
bool board_irq_hal_take_modem_ri_pending(void);
bool board_irq_hal_shared_asserted(void);
bool board_irq_hal_wake_latches_clear(void);
/* Preserve asynchronous events captured by the dedicated dormant controller
 * before its raw edge status is acknowledged. */
void board_irq_hal_latch_dormant_wake(bool shared_irq,
                                      bool service_vbus,
                                      bool modem_ri);
void board_irq_hal_get_snapshot(board_irq_snapshot_t *out);

#endif

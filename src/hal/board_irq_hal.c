#include "hal/board_irq_hal.h"

#include <limits.h>
#include <stddef.h>

#include "sisu_build_config.h"
#include "hal/board.h"
#include "hardware/gpio.h"
#include "hardware/sync.h"

static volatile bool s_shared_pending;
static volatile bool s_service_vbus_pending;
static volatile bool s_modem_ri_pending;
static volatile uint32_t s_shared_falling_edges;
static volatile uint32_t s_service_vbus_edges;
static volatile uint32_t s_modem_ri_falling_edges;
static volatile bool s_modem_ri_enabled;
static bool s_initialized;

static void board_gpio_irq(uint gpio, uint32_t events) {
    if (gpio == SYS_INT_PIN &&
        (events & GPIO_IRQ_EDGE_FALL) != 0u) {
        s_shared_pending = true;
        if (s_shared_falling_edges != UINT32_MAX) {
            s_shared_falling_edges++;
        }
    }
#if !SISU_RELEASE_BUILD
    if (gpio == SERVICE_VBUS_PIN &&
        (events & (GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL)) != 0u) {
        s_service_vbus_pending = true;
        if (s_service_vbus_edges != UINT32_MAX) {
            s_service_vbus_edges++;
        }
    }
#endif
    if (gpio == MODEM_PIN_RI && s_modem_ri_enabled &&
        (events & GPIO_IRQ_EDGE_FALL) != 0u) {
        s_modem_ri_pending = true;
        if (s_modem_ri_falling_edges != UINT32_MAX) {
            s_modem_ri_falling_edges++;
        }
    }
}

void board_irq_hal_init(void) {
    if (s_initialized) {
        return;
    }

    gpio_init(SYS_INT_PIN);
    gpio_set_dir(SYS_INT_PIN, GPIO_IN);
    gpio_disable_pulls(SYS_INT_PIN);
    gpio_set_input_enabled(SYS_INT_PIN, true);

    gpio_init(SERVICE_VBUS_PIN);
    gpio_set_dir(SERVICE_VBUS_PIN, GPIO_IN);
    gpio_disable_pulls(SERVICE_VBUS_PIN);
    gpio_set_input_enabled(SERVICE_VBUS_PIN, true);

    s_shared_pending = gpio_get(SYS_INT_PIN) == 0;
    s_service_vbus_pending = false;
    s_modem_ri_pending = false;
    s_shared_falling_edges = 0u;
    s_service_vbus_edges = 0u;
    s_modem_ri_falling_edges = 0u;
    s_modem_ri_enabled = false;

    gpio_set_irq_enabled_with_callback(
        SYS_INT_PIN,
        GPIO_IRQ_EDGE_FALL,
        true,
        board_gpio_irq
    );
#if !SISU_RELEASE_BUILD
    gpio_set_irq_enabled(
        SERVICE_VBUS_PIN,
        GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL,
        true
    );
#endif
    s_initialized = true;
}

static bool take_pending(volatile bool *pending) {
    uint32_t irq_state = save_and_disable_interrupts();
    bool value = *pending;
    *pending = false;
    restore_interrupts(irq_state);
    return value;
}

bool board_irq_hal_take_shared_pending(void) {
    return take_pending(&s_shared_pending);
}

bool board_irq_hal_take_service_vbus_pending(void) {
    return take_pending(&s_service_vbus_pending);
}

void board_irq_hal_set_modem_ri_enabled(bool enabled) {
    if (!s_initialized) {
        if (!enabled) {
            return;
        }
        board_irq_hal_init();
    }

    uint32_t irq_state = save_and_disable_interrupts();
    gpio_set_irq_enabled(MODEM_PIN_RI, GPIO_IRQ_EDGE_FALL, false);
    gpio_acknowledge_irq(MODEM_PIN_RI, GPIO_IRQ_EDGE_FALL);
    s_modem_ri_enabled = false;
    s_modem_ri_pending = false;

    if (enabled) {
        s_modem_ri_enabled = true;
        gpio_set_irq_enabled(MODEM_PIN_RI, GPIO_IRQ_EDGE_FALL, true);
        /* Enabling an edge IRQ cannot recover a line that was already low.
         * Latch that level while interrupts remain masked to close the arm
         * race; a later falling edge is captured by board_gpio_irq(). */
        if (gpio_get(MODEM_PIN_RI) == 0) {
            s_modem_ri_pending = true;
        }
    }
    restore_interrupts(irq_state);
}

bool board_irq_hal_take_modem_ri_pending(void) {
    return take_pending(&s_modem_ri_pending);
}

bool board_irq_hal_shared_asserted(void) {
    return gpio_get(SYS_INT_PIN) == 0;
}

bool board_irq_hal_wake_latches_clear(void) {
    uint32_t irq_state = save_and_disable_interrupts();
    bool clear = !s_shared_pending && !s_modem_ri_pending;
#if !SISU_RELEASE_BUILD
    clear = clear && !s_service_vbus_pending;
#endif
    restore_interrupts(irq_state);
    return clear;
}

static void increment_saturating(volatile uint32_t *counter) {
    if (*counter != UINT32_MAX) {
        (*counter)++;
    }
}

void board_irq_hal_latch_dormant_wake(bool shared_irq,
                                      bool service_vbus,
                                      bool modem_ri) {
    uint32_t irq_state = save_and_disable_interrupts();
    if (shared_irq && !s_shared_pending) {
        s_shared_pending = true;
        increment_saturating(&s_shared_falling_edges);
    }
#if !SISU_RELEASE_BUILD
    if (service_vbus && !s_service_vbus_pending) {
        s_service_vbus_pending = true;
        increment_saturating(&s_service_vbus_edges);
    }
#else
    (void)service_vbus;
#endif
    if (modem_ri && s_modem_ri_enabled && !s_modem_ri_pending) {
        s_modem_ri_pending = true;
        increment_saturating(&s_modem_ri_falling_edges);
    }
    restore_interrupts(irq_state);
}

void board_irq_hal_get_snapshot(board_irq_snapshot_t *out) {
    if (out == NULL) {
        return;
    }
    uint32_t irq_state = save_and_disable_interrupts();
    out->shared_falling_edges = s_shared_falling_edges;
    out->service_vbus_edges = s_service_vbus_edges;
    out->modem_ri_falling_edges = s_modem_ri_falling_edges;
    out->shared_pending = s_shared_pending;
    out->service_vbus_pending = s_service_vbus_pending;
    out->modem_ri_pending = s_modem_ri_pending;
    restore_interrupts(irq_state);
}

#ifndef BOARD_SAFE_GPIO_H
#define BOARD_SAFE_GPIO_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    void (*init)(uint8_t pin);
    void (*put)(uint8_t pin, bool level);
    void (*set_dir)(uint8_t pin, bool output);
    void (*disable_pulls)(uint8_t pin);
    void (*set_input_enabled)(uint8_t pin, bool enabled);
} board_safe_gpio_ops_t;

/* Apply the Rev B2 reset policy. For every output, the inactive SIO latch is
 * written before output-enable changes. External digital observations retain
 * their input receivers; analog, unconnected, and unpowered-modem pads are
 * pull-free with their input receivers disabled. */
void board_safe_gpio_apply(const board_safe_gpio_ops_t *ops);

#endif

#ifndef HOST_TEST_HARDWARE_GPIO_H
#define HOST_TEST_HARDWARE_GPIO_H

#include <stdbool.h>

#define GPIO_IN false
#define GPIO_OUT true
void gpio_init(unsigned int gpio);
void gpio_set_dir(unsigned int gpio, bool output);
void gpio_pull_up(unsigned int gpio);
bool gpio_get(unsigned int gpio);
void gpio_put(unsigned int gpio, bool value);

#endif

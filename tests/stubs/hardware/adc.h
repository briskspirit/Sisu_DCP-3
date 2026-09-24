#ifndef HOST_TEST_HARDWARE_ADC_H
#define HOST_TEST_HARDWARE_ADC_H

#include <stdint.h>
void adc_init(void);
void adc_gpio_init(unsigned int gpio);
void adc_select_input(unsigned int input);
uint16_t adc_read(void);

#endif

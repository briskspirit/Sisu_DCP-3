#ifndef HOST_TEST_HARDWARE_I2C_H
#define HOST_TEST_HARDWARE_I2C_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t unused;
} i2c_inst_t;

extern i2c_inst_t *const i2c0;

int i2c_write_timeout_us(i2c_inst_t *i2c,
                         uint8_t addr,
                         const uint8_t *src,
                         size_t len,
                         bool nostop,
                         unsigned int timeout_us);
int i2c_read_timeout_us(i2c_inst_t *i2c,
                        uint8_t addr,
                        uint8_t *dst,
                        size_t len,
                        bool nostop,
                        unsigned int timeout_us);

#endif

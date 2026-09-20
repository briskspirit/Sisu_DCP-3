#ifndef HOST_TEST_HARDWARE_SPI_H
#define HOST_TEST_HARDWARE_SPI_H

#include <stddef.h>
#include <stdint.h>

typedef struct spi_inst spi_inst_t;
extern spi_inst_t *const spi0;
int spi_write_blocking(spi_inst_t *spi, const uint8_t *src, size_t len);

#endif

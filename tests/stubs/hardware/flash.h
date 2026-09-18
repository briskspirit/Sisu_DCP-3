#ifndef TEST_HARDWARE_FLASH_H
#define TEST_HARDWARE_FLASH_H
#include <stddef.h>
#include <stdint.h>
#define FLASH_SECTOR_SIZE 4096u
#define FLASH_PAGE_SIZE 256u
#define PICO_FLASH_SIZE_BYTES (2u * 1024u * 1024u)
void flash_range_erase(uint32_t off, size_t len);
void flash_range_program(uint32_t off, const uint8_t *src, size_t len);
#endif

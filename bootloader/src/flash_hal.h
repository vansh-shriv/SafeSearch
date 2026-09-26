#ifndef FLASH_HAL_H
#define FLASH_HAL_H

#include <stdint.h>
#include <stddef.h>

/*
 * Flash access seam. The target build implements this over the STM32F4 flash
 * controller; host unit tests implement it over a RAM buffer that can also
 * simulate a power cut at a chosen operation.  All addresses are absolute.
 * Return 0 on success, nonzero on failure.
 */
int flash_read(uint32_t addr, void *buf, size_t len);
int flash_erase_sector(uint32_t sector);
int flash_write(uint32_t addr, const void *buf, size_t len);

#endif

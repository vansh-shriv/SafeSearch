#ifndef FLASH_MAP_H
#define FLASH_MAP_H

#include <stdint.h>

/*
 * Derived from Renode platforms/cpus/stm32f4.repl (flash: 0x08000000, 2 MB).
 * Sector layout follows STM32F4 bank 1: 4x16K, 1x64K, then 128K sectors.
 */
#define FLASH_BASE          0x08000000u

#define BOOT_ADDR           0x08000000u   /* sectors 0-1 (32 KB) */
#define BOOT_SIZE           0x00008000u

#define META_SECTOR_A       2u            /* 16 KB each */
#define META_SECTOR_B       3u
#define META_ADDR_A         0x08008000u
#define META_ADDR_B         0x0800C000u
#define META_SECTOR_SIZE    0x00004000u

/* Sector 4 (64 KB at 0x08010000) is deliberately left unused. */

#define SLOT_A_ADDR         0x08020000u   /* sectors 5-7  (384 KB) */
#define SLOT_B_ADDR         0x08080000u   /* sectors 8-10 (384 KB) */
#define SLOT_SIZE           0x00060000u

#define SLOT_A_FIRST_SECTOR 5u
#define SLOT_B_FIRST_SECTOR 8u
#define SLOT_SECTOR_COUNT   3u

/* Header occupies the start of each slot; vector table follows, 0x400-aligned. */
#define IMAGE_HEADER_SIZE   0x00000400u
#define IMAGE_MAX_SIZE      (SLOT_SIZE - IMAGE_HEADER_SIZE)

#define SLOT_COUNT          2u

static inline uint32_t slot_addr(uint8_t slot)
{
    return slot == 0 ? SLOT_A_ADDR : SLOT_B_ADDR;
}

#endif

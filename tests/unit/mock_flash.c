#include "mock_flash.h"
#include "flash_hal.h"
#include "flash_map.h"

#include <string.h>
#include <stdlib.h>

static uint8_t mem[MOCK_FLASH_SIZE];
static int op_count;
static int cut_at = -1;
static size_t cut_partial;
static int dead;

void mock_flash_reset(void)
{
    memset(mem, 0xFF, sizeof mem);
    op_count = 0;
    cut_at = -1;
    dead = 0;
}

uint8_t *mock_flash_raw(uint32_t addr) { return &mem[addr - FLASH_BASE]; }
void mock_flash_arm_cut(int op_index, size_t partial) { cut_at = op_index; cut_partial = partial; }
void mock_flash_power_on(void) { dead = 0; cut_at = -1; }
int  mock_flash_mutation_count(void) { return op_count; }

/* STM32F4 sector geometry: 4x16K, 1x64K, then 128K. */
static void sector_bounds(uint32_t s, uint32_t *off, uint32_t *size)
{
    if (s < 4)       { *off = s * 0x4000; *size = 0x4000; }
    else if (s == 4) { *off = 0x10000;    *size = 0x10000; }
    else             { *off = 0x20000 + (s - 5) * 0x20000; *size = 0x20000; }
}

int flash_read(uint32_t addr, void *buf, size_t len)
{
    if (addr < FLASH_BASE || addr - FLASH_BASE + len > MOCK_FLASH_SIZE)
        return -1;
    memcpy(buf, &mem[addr - FLASH_BASE], len);
    return 0;
}

int flash_erase_sector(uint32_t sector)
{
    uint32_t off, size;
    if (dead) return -1;
    sector_bounds(sector, &off, &size);

    if (op_count++ == cut_at) {
        /* Interrupted erase: contents indeterminate. */
        for (uint32_t i = 0; i < size; i++)
            mem[off + i] = (uint8_t)rand();
        dead = 1;
        return -1;
    }
    memset(&mem[off], 0xFF, size);
    return 0;
}

int flash_write(uint32_t addr, const void *buf, size_t len)
{
    const uint8_t *src = buf;
    if (dead) return -1;
    if (addr < FLASH_BASE || addr - FLASH_BASE + len > MOCK_FLASH_SIZE)
        return -1;

    size_t n = len;
    int cut = (op_count++ == cut_at);
    if (cut) {
        n = cut_partial < len ? cut_partial : len;
        dead = 1;
    }
    for (size_t i = 0; i < n; i++) {
        /* Flash can only clear bits; writing over non-erased data ANDs. */
        mem[addr - FLASH_BASE + i] &= src[i];
    }
    return cut ? -1 : 0;
}

#ifndef MOCK_FLASH_H
#define MOCK_FLASH_H

#include <stdint.h>
#include <stddef.h>

#define MOCK_FLASH_SIZE 0x200000u

void mock_flash_reset(void);                 /* all 0xFF, no cut armed */
uint8_t *mock_flash_raw(uint32_t addr);

/*
 * Arm a power cut: the (op_index)th flash mutation (erase/write, 0-based) is
 * interrupted after `partial` bytes take effect (write) or leaves the sector
 * with garbage contents (erase).  After the cut all mutations fail until
 * mock_flash_power_on().
 */
void mock_flash_arm_cut(int op_index, size_t partial);
void mock_flash_power_on(void);
int  mock_flash_mutation_count(void);

#endif

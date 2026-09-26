#ifndef MOCK_FLASH_H
#define MOCK_FLASH_H

#include <stdint.h>
#include <stddef.h>

#define MOCK_FLASH_SIZE 0x200000u
#define MOCK_MAX_OPS    4096

typedef enum { MOCK_OP_ERASE = 0, MOCK_OP_WRITE = 1 } mock_op_kind_t;

void mock_flash_reset(void);                 /* all 0xFF, no cut armed, trace cleared */
uint8_t *mock_flash_raw(uint32_t addr);

/*
 * Arm a power cut on the (op_index)th flash mutation (erase/write, 0-based counting from the last
 * reset/snapshot load). After the cut every mutation fails until mock_flash_power_on().
 *   write: the first `partial` bytes take effect, the rest is untouched.
 *   erase: partial % 3 selects the torn state: 0 = erase never took effect, 1 = sector left as
 *          random garbage, 2 = first half erased, second half unchanged.
 */
void mock_flash_arm_cut(int op_index, size_t partial);
void mock_flash_power_on(void);
int  mock_flash_mutation_count(void);

/* Trace of mutations since reset / snapshot load (for enumerating cut points). */
mock_op_kind_t mock_flash_op_kind(int i);
uint32_t       mock_flash_op_addr(int i);      /* erase: sector number; write: address */
size_t         mock_flash_op_len(int i);       /* write: byte count; erase: 0 */

/* Snapshot the array contents (not the counters) and restore them, clearing counters/cut/trace. */
void mock_flash_save(uint8_t *dst);
void mock_flash_restore(const uint8_t *src);

#endif

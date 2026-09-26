#include "flash_hal.h"
#include "flash_map.h"

/* STM32F4 FLASH interface registers (RM0090 §3.7) */
#define FLASH_REG(off) (*(volatile uint32_t *)(0x40023C00u + (off)))
#define FLASH_KEYR FLASH_REG(0x04)
#define FLASH_SR   FLASH_REG(0x0C)
#define FLASH_CR   FLASH_REG(0x10)

#define SR_BSY     (1u << 16)
#define SR_ERRORS  ((1u << 1) | (1u << 4) | (1u << 5) | (1u << 6) | (1u << 7)) /* OPERR WRPERR PGAERR PGPERR PGSERR */
#define CR_PG      (1u << 0)
#define CR_SER     (1u << 1)
#define CR_SNB(n)  ((uint32_t)(n) << 3)
#define CR_PSIZE32 (2u << 8)
#define CR_STRT    (1u << 16)
#define CR_LOCK    (1u << 31)

static void wait_idle(void)
{
    while (FLASH_SR & SR_BSY)
        ;
}

static void unlock(void)
{
    if (FLASH_CR & CR_LOCK) {
        FLASH_KEYR = 0x45670123u;
        FLASH_KEYR = 0xCDEF89ABu;
    }
}

static void lock(void)
{
    FLASH_CR |= CR_LOCK;
}

int flash_read(uint32_t addr, void *buf, size_t len)
{
    const volatile uint8_t *p = (const volatile uint8_t *)addr;
    uint8_t *d = buf;
    while (len--)
        *d++ = *p++;
    return 0;
}

int flash_erase_sector(uint32_t sector)
{
    int rc = 0;

    unlock();
    wait_idle();
    FLASH_SR = SR_ERRORS;
    FLASH_CR = CR_SER | CR_SNB(sector) | CR_PSIZE32;
    FLASH_CR |= CR_STRT;
    wait_idle();
    if (FLASH_SR & SR_ERRORS)
        rc = -1;
    FLASH_CR = 0;
    lock();
    return rc;
}

/* Program one aligned 32-bit word. Kept out-of-line: Renode power-cut tests hook this symbol. */
__attribute__((noinline)) static int program_word(uint32_t addr, uint32_t val)
{
    FLASH_SR = SR_ERRORS;
    FLASH_CR = CR_PG | CR_PSIZE32;
    *(volatile uint32_t *)addr = val;
    wait_idle();
    FLASH_CR = 0;
    return (FLASH_SR & SR_ERRORS) ? -1 : 0;
}

/*
 * Arbitrary-length write. Flash is programmed a word at a time, so unaligned head/tail bytes are
 * merged with 0xFF (programming 0xFF into erased flash leaves it erased).
 */
int flash_write(uint32_t addr, const void *buf, size_t len)
{
    const uint8_t *src = buf;
    int rc = 0;

    unlock();
    wait_idle();
    while (len && rc == 0) {
        uint32_t base = addr & ~3u;
        uint32_t off = addr - base;
        uint8_t word[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
        size_t n = 4 - off;
        if (n > len)
            n = len;
        for (size_t i = 0; i < n; i++)
            word[off + i] = src[i];
        uint32_t v = (uint32_t)word[0] | ((uint32_t)word[1] << 8) |
                     ((uint32_t)word[2] << 16) | ((uint32_t)word[3] << 24);
        rc = program_word(base, v);
        addr += n;
        src += n;
        len -= n;
    }
    lock();
    return rc;
}

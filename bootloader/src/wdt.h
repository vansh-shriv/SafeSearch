#ifndef WDT_H
#define WDT_H

#include <stdint.h>

/* STM32 IWDG (LSI ~32 kHz). Once started it cannot be stopped until the next reset. */
#define IWDG_BASE 0x40003000u
#define IWDG_KR   (*(volatile uint32_t *)(IWDG_BASE + 0x00))
#define IWDG_PR   (*(volatile uint32_t *)(IWDG_BASE + 0x04))
#define IWDG_RLR  (*(volatile uint32_t *)(IWDG_BASE + 0x08))
#define IWDG_SR   (*(volatile uint32_t *)(IWDG_BASE + 0x0C))

static inline void wdt_kick(void)
{
    IWDG_KR = 0xAAAAu;
}

/* Prescaler /64 -> 500 Hz tick, so reload = ms / 2 (max 4095 -> ~8.2 s). */
static inline void wdt_start(uint32_t timeout_ms)
{
    IWDG_KR = 0x5555u;
    IWDG_PR = 4u;
    IWDG_RLR = timeout_ms / 2u;
    IWDG_KR = 0xAAAAu;
    IWDG_KR = 0xCCCCu;
}

#endif

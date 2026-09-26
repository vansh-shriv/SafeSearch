#ifndef UART_H
#define UART_H

#include <stdint.h>

/* Polled USART1 output (Renode STM32_UART: SR @ +0x00, DR @ +0x04). Shared by bootloader and app. */
#define USART1_BASE 0x40011000u

static inline void uart_init(void)
{
    /* CR1 @ +0x0C: UE (bit 13) | TE (bit 3). Baud is irrelevant in the emulator. */
    *(volatile uint32_t *)(USART1_BASE + 0x0C) = (1u << 13) | (1u << 3) | (1u << 2);   /* UE | TE | RE */
}

static inline void uart_putc(char c)
{
    const volatile uint32_t *sr = (const volatile uint32_t *)(USART1_BASE + 0x00);
    volatile uint32_t *dr = (volatile uint32_t *)(USART1_BASE + 0x04);
    while (!(*sr & (1u << 7)))   /* TXE */
        ;
    *dr = (uint32_t)c;
}

/* Blocking receive: RXNE is SR bit 5; reading DR clears it. */
static inline uint8_t uart_getc(void)
{
    const volatile uint32_t *sr = (const volatile uint32_t *)(USART1_BASE + 0x00);
    const volatile uint32_t *dr = (const volatile uint32_t *)(USART1_BASE + 0x04);
    /* Brief pause between polls: at most ~5 us at 168 MHz, far below a byte time even at 921600 baud, and it
     * keeps an emulator from servicing a register read on every loop iteration. */
    while (!(*sr & (1u << 5))) {
        for (volatile int i = 0; i < 256; i++)
            ;
    }
    return (uint8_t)*dr;
}

static inline void uart_puts(const char *s)
{
    while (*s) {
        if (*s == '\n')
            uart_putc('\r');
        uart_putc(*s++);
    }
}

static inline void uart_puthex(uint32_t v)
{
    static const char d[] = "0123456789ABCDEF";
    uart_puts("0x");
    for (int i = 28; i >= 0; i -= 4)
        uart_putc(d[(v >> i) & 0xF]);
}

#endif

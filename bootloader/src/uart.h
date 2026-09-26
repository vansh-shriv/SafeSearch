#ifndef UART_H
#define UART_H

#include <stdint.h>

/* Polled USART1 output (Renode STM32_UART: SR @ +0x00, DR @ +0x04). Shared by bootloader and app. */
#define USART1_BASE 0x40011000u

static inline void uart_init(void)
{
    /* CR1 @ +0x0C: UE (bit 13) | TE (bit 3). Baud is irrelevant in the emulator. */
    *(volatile uint32_t *)(USART1_BASE + 0x0C) = (1u << 13) | (1u << 3);
}

static inline void uart_putc(char c)
{
    volatile uint32_t *sr = (volatile uint32_t *)(USART1_BASE + 0x00);
    volatile uint32_t *dr = (volatile uint32_t *)(USART1_BASE + 0x04);
    while (!(*sr & (1u << 7)))   /* TXE */
        ;
    *dr = (uint32_t)c;
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

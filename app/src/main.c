#include "uart.h"

#ifndef APP_VERSION
#define APP_VERSION 1
#endif

int main(void)
{
    uart_init();
    uart_puts("APP: running, version ");
    uart_putc((char)('0' + APP_VERSION));
    uart_puts("\n");
    for (;;)
        ;
}

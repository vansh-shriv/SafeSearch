#include "uart.h"
#include "safeflash_app.h"

#ifndef APP_VERSION
#define APP_VERSION 1
#endif

/* Build with -DAPP_CONFIRM=0 to make a "bad" image that runs but never confirms itself. */
#ifndef APP_CONFIRM
#define APP_CONFIRM 1
#endif

int main(void)
{
    uart_init();
    uart_puts("APP: running, version ");
    uart_putc((char)('0' + APP_VERSION));
    uart_puts("\n");

#if APP_CONFIRM
    if (sf_confirm_healthy() == 0)
        uart_puts("APP: confirmed healthy\n");
    else
        uart_puts("APP: confirm failed\n");
#endif

    /* Idle work + periodic kick. Kicking every iteration would be needlessly slow to emulate. */
    for (;;) {
        for (volatile uint32_t i = 0; i < 100000; i++)
            ;
        sf_wdt_kick();
    }
}

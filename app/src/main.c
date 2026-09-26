#include "uart.h"
#include "safeflash_app.h"

#ifndef APP_VERSION
#define APP_VERSION 1
#endif

/* Build with -DAPP_CONFIRM=0 to make a "bad" image that runs but never confirms itself. */
#ifndef APP_CONFIRM
#define APP_CONFIRM 1
#endif

/*
 * Simulated OTA transport: the host drops a complete signed image into RAM at STAGE_ADDR
 * ({ 'STGE', length, image bytes }) and the app installs it. Stands in for "the transport layer has
 * received the bytes"; the update mechanism (verification, atomicity, rollback) is what is under test.
 */
#ifndef APP_INSTALLER
#define APP_INSTALLER 1
#endif
#define STAGE_ADDR  0x20020000u
#define STAGE_MAGIC 0x45475453u   /* "STGE" little-endian */
#define SCB_AIRCR   (*(volatile uint32_t *)0xE000ED0Cu)

static void install_staged(void)
{
    volatile uint32_t *stage = (volatile uint32_t *)STAGE_ADDR;

    if (stage[0] != STAGE_MAGIC)
        return;
    uint32_t len = stage[1];
    stage[0] = 0;   /* consume the request so a reset cannot re-install */

    uart_puts("APP: installing update\n");
    int rc = sf_install_update((const void *)&stage[2], len);
    if (rc != SF_OK) {
        uart_puts("APP: install failed rc ");
        uart_puthex((uint32_t)rc);
        uart_puts("\n");
        return;
    }
    uart_puts("APP: install ok, resetting\n");
    SCB_AIRCR = 0x05FA0004u;   /* SYSRESETREQ */
    for (;;)
        ;
}

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

#if APP_INSTALLER
    install_staged();
#endif

    /* Idle work + periodic kick. Kicking every iteration would be needlessly slow to emulate. */
    for (;;) {
        for (volatile uint32_t i = 0; i < 100000; i++)
            ;
        sf_wdt_kick();
    }
}

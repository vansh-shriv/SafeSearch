#include "flash_map.h"
#include "boot_config.h"
#include "boot_logic.h"
#include "recovery.h"
#include "uart.h"
#include "wdt.h"

#define SCB_VTOR (*(volatile uint32_t *)0xE000ED08u)
#define SCB_AIRCR (*(volatile uint32_t *)0xE000ED0Cu)

void boot_puts(const char *s)
{
    uart_puts(s);
}

void boot_puthex(uint32_t v)
{
    uart_puthex(v);
}

/* Recovery byte stream: the UART. Blocks forever; there is no timeout, the device simply waits for an image. */
int recovery_getc(void)
{
    return uart_getc();
}

void recovery_putc(uint8_t b)
{
    uart_putc((char)b);
}

static void system_reset(void)
{
    SCB_AIRCR = 0x05FA0004u;   /* SYSRESETREQ */
    for (;;)
        ;
}

static void jump_to_app(uint32_t slot)
{
    uint32_t vt = slot + IMAGE_HEADER_SIZE;
    uint32_t sp = *(volatile uint32_t *)vt;
    uint32_t pc = *(volatile uint32_t *)(vt + 4);

    SCB_VTOR = vt;
    __asm volatile("msr msp, %0\n bx %1" : : "r"(sp), "r"(pc) :);
    for (;;)
        ;
}

static void run_recovery(void)
{
    uart_puts("BL: recovery mode, waiting for image\n");
    (void)recovery_run();   /* returns only after a verified image was committed */
    uart_puts("BL: recovery image installed, resetting\n");
    system_reset();
}

int main(void)
{
    boot_decision_t d;
    volatile uint32_t *req = (volatile uint32_t *)RECOVERY_REQUEST_ADDR;

    uart_init();
    uart_puts("SafeFlash BL\n");

    if (*req == RECOVERY_REQUEST_MAGIC) {
        *req = 0;   /* consume: one request, one recovery session */
        uart_puts("BL: recovery requested\n");
        run_recovery();
    }

    if (boot_decide(&d) != 0)
        run_recovery();   /* nothing bootable: wait for a signed image instead of halting */

    uart_puts("BL: jumping to slot ");
    uart_putc((char)('A' + d.slot));
    uart_puts(" version ");
    uart_puthex(d.version);
    uart_puts("\n");
    if (d.trial) {
        uart_puts("BL: trial ");
        uart_putc((char)('0' + d.trial_no));
        uart_puts("/");
        uart_putc((char)('0' + MAX_TRIALS));
        uart_puts(", watchdog armed\n");
        wdt_start(TRIAL_WDT_MS);
    }
    jump_to_app(slot_addr(d.slot));
    return 0;
}

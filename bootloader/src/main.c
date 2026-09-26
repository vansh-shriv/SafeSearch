#include "flash_map.h"
#include "boot_config.h"
#include "boot_logic.h"
#include "uart.h"
#include "wdt.h"

#define SCB_VTOR (*(volatile uint32_t *)0xE000ED08u)

void boot_puts(const char *s)
{
    uart_puts(s);
}

void boot_puthex(uint32_t v)
{
    uart_puthex(v);
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

int main(void)
{
    boot_decision_t d;

    uart_init();
    uart_puts("SafeFlash BL\n");

    if (boot_decide(&d) != 0)
        for (;;)
            ;

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

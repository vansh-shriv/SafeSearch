#include "flash_map.h"
#include "flash_hal.h"
#include "boot_config.h"
#include "image_verify.h"
#include "image_crypto.h"
#include "metadata.h"
#include "uart.h"
#include "wdt.h"

#define SCB_VTOR (*(volatile uint32_t *)0xE000ED08u)

static const char *status_str(img_status_t s)
{
    switch (s) {
    case IMG_OK:                return "ok";
    case IMG_ERR_READ:          return "read error";
    case IMG_ERR_MAGIC:         return "bad magic";
    case IMG_ERR_HEADER_CRC:    return "bad header crc";
    case IMG_ERR_SIZE:          return "bad size";
    case IMG_ERR_VERSION_FLOOR: return "below version floor";
    case IMG_ERR_IMAGE_CRC:     return "bad image crc";
    case IMG_ERR_HASH:          return "bad payload hash";
    case IMG_ERR_SIGNATURE:     return "bad signature";
    }
    return "?";
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

static img_status_t check_slot(uint8_t slot, uint32_t floor, image_header_t *h)
{
    /* Cheap structural checks first, then hash + signature. Nothing runs unless both pass. */
    img_status_t s = image_check_basic(slot_addr(slot), floor, h);
    if (s == IMG_OK)
        s = image_check_signature(slot_addr(slot), h);
    uart_puts("BL: slot ");
    uart_putc((char)('A' + slot));
    uart_puts(": ");
    uart_puts(status_str(s));
    uart_puts("\n");
    return s;
}

static void halt(const char *msg)
{
    uart_puts(msg);
    for (;;)
        ;
}

static void put_slot(uint8_t slot)
{
    uart_putc((char)('A' + slot));
}

int main(void)
{
    boot_metadata_t md;
    image_header_t h;

    uart_init();
    uart_puts("SafeFlash BL\n");

    if (metadata_load(&md) != 0) {
        uart_puts("BL: no valid metadata, initialising\n");
        md.active_slot = 0;
        md.boot_state = BOOT_STATE_NORMAL;
        md.trial_count = 0;
        md.reserved = 0;
        md.min_allowed_version = 0;
        if (metadata_store(&md) != 0)
            halt("BL: metadata write failed\n");
    }
    uart_puts("BL: metadata seq ");
    uart_puthex(md.seq);
    uart_puts(" active ");
    put_slot(md.active_slot);
    uart_puts(" state ");
    uart_putc((char)('0' + md.boot_state));
    uart_puts(" trials ");
    uart_putc((char)('0' + md.trial_count));
    uart_puts("\n");

    uint8_t slot = md.active_slot;
    int revert = md.boot_state == BOOT_STATE_TRIAL && md.trial_count >= MAX_TRIALS;
    if (revert)
        uart_puts("BL: trial limit reached\n");

    if (revert || check_slot(slot, md.min_allowed_version, &h) != IMG_OK) {
        /* Trial exhausted or active slot unusable: go back to the other slot and make that permanent. */
        slot = 1 - slot;
        if (check_slot(slot, md.min_allowed_version, &h) != IMG_OK)
            halt("BL: no bootable image, halting\n");
        md.active_slot = slot;
        md.boot_state = BOOT_STATE_CONFIRMED;
        md.trial_count = 0;
        if (metadata_store(&md) != 0)
            halt("BL: metadata write failed\n");
        uart_puts("BL: reverted to slot ");
        put_slot(slot);
        uart_puts("\n");
    }

    int trial = md.boot_state == BOOT_STATE_TRIAL;
    if (trial) {
        /* Count the attempt before running the image, so a hang/crash is charged against it. */
        md.trial_count++;
        if (metadata_store(&md) != 0)
            halt("BL: metadata write failed\n");
    }

    uart_puts("BL: jumping to slot ");
    put_slot(slot);
    uart_puts(" version ");
    uart_puthex(h.version);
    uart_puts("\n");
    if (trial) {
        uart_puts("BL: trial ");
        uart_putc((char)('0' + md.trial_count));
        uart_puts("/");
        uart_putc((char)('0' + MAX_TRIALS));
        uart_puts(", watchdog armed\n");
        wdt_start(TRIAL_WDT_MS);
    }
    jump_to_app(slot_addr(slot));
    return 0;
}

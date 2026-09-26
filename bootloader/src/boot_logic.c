#include "boot_logic.h"
#include "boot_config.h"
#include "flash_hal.h"
#include "flash_map.h"
#include "image_crypto.h"
#include "image_verify.h"
#include "metadata.h"

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

static void put_slot(uint8_t slot)
{
    char c[2] = { (char)('A' + slot), 0 };
    boot_puts(c);
}

static void put_digit(unsigned v)
{
    char c[2] = { (char)('0' + v), 0 };
    boot_puts(c);
}

static img_status_t check_slot(uint8_t slot, uint32_t floor, image_header_t *h)
{
    /* Cheap structural checks first, then hash + signature. Nothing runs unless both pass. */
    img_status_t s = image_check_basic(slot_addr(slot), floor, h);
#ifndef SF_MEASURE_NO_SIGNATURE   /* only ever defined for the boot-time comparison build; never ship */
    if (s == IMG_OK)
        s = image_check_signature(slot_addr(slot), h);
#endif
    boot_puts("BL: slot ");
    put_slot(slot);
    boot_puts(": ");
    boot_puts(status_str(s));
    boot_puts("\n");
    return s;
}

int boot_decide(boot_decision_t *d)
{
    boot_metadata_t md;
    image_header_t h;

    if (metadata_load(&md) != 0) {
        boot_puts("BL: no valid metadata, initialising\n");
        md.active_slot = 0;
        md.boot_state = BOOT_STATE_NORMAL;
        md.trial_count = 0;
        md.reserved = 0;
        md.min_allowed_version = 0;
        if (metadata_store(&md) != 0) {
            boot_puts("BL: metadata write failed\n");
            return -1;
        }
    }
    boot_puts("BL: metadata seq ");
    boot_puthex(md.seq);
    boot_puts(" active ");
    put_slot(md.active_slot);
    boot_puts(" state ");
    put_digit(md.boot_state);
    boot_puts(" trials ");
    put_digit(md.trial_count);
    boot_puts(" floor ");
    boot_puthex(md.min_allowed_version);
    boot_puts("\n");

    uint8_t slot = md.active_slot;
    int revert = md.boot_state == BOOT_STATE_TRIAL && md.trial_count >= MAX_TRIALS;
    if (revert)
        boot_puts("BL: trial limit reached\n");

    if (revert || check_slot(slot, md.min_allowed_version, &h) != IMG_OK) {
        /* Trial exhausted or active slot unusable: go back to the other slot and make that permanent. */
        slot = 1 - slot;
        if (check_slot(slot, md.min_allowed_version, &h) != IMG_OK) {
            boot_puts("BL: no bootable image, halting\n");
            return -1;
        }
        md.active_slot = slot;
        md.boot_state = BOOT_STATE_CONFIRMED;
        md.trial_count = 0;
        if (metadata_store(&md) != 0) {
            boot_puts("BL: metadata write failed\n");
            return -1;
        }
        boot_puts("BL: reverted to slot ");
        put_slot(slot);
        boot_puts("\n");
    }

    d->slot = slot;
    d->version = h.version;
    d->trial = md.boot_state == BOOT_STATE_TRIAL;
    d->trial_no = 0;
    if (d->trial) {
        /* Count the attempt before running the image, so a hang/crash is charged against it. */
        md.trial_count++;
        d->trial_no = md.trial_count;
        if (metadata_store(&md) != 0) {
            boot_puts("BL: metadata write failed\n");
            return -1;
        }
    }
    return 0;
}

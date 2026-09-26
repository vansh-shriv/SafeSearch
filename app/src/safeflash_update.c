#include "safeflash_app.h"
#include "flash_hal.h"
#include "flash_map.h"
#include "image_verify.h"
#include "metadata.h"

#define SECTOR_BYTES 0x20000u   /* slot sectors are all 128 KB on the STM32F4 */

/*
 * Ordering is what makes this power-safe:
 *   1. erase + program the INACTIVE slot (payload first, header last, so a torn write leaves no valid header)
 *   2. only then commit metadata (ping-pong: old or new copy always valid) pointing at the new slot in TRIAL
 * A cut before step 2 completes leaves the active slot and its metadata untouched.
 */
int sf_install_update(const void *img, size_t len)
{
    const image_header_t *h = img;
    boot_metadata_t md;

    if (metadata_load(&md) != 0)
        return SF_ERR_STATE;
    if (len <= IMAGE_HEADER_SIZE || len > SLOT_SIZE || h->magic != IMAGE_MAGIC)
        return SF_ERR_IMAGE;
    if (h->version < md.min_allowed_version)
        return SF_ERR_ROLLBACK;

    uint8_t target = 1 - md.active_slot;
    uint32_t base = slot_addr(target);
    uint32_t first = target == 0 ? SLOT_A_FIRST_SECTOR : SLOT_B_FIRST_SECTOR;
    uint32_t nsect = (uint32_t)((len + SECTOR_BYTES - 1) / SECTOR_BYTES);

    for (uint32_t i = 0; i < nsect; i++)
        if (flash_erase_sector(first + i) != 0)
            return SF_ERR_FLASH;

    const uint8_t *p = img;
    if (flash_write(base + IMAGE_HEADER_SIZE, p + IMAGE_HEADER_SIZE, len - IMAGE_HEADER_SIZE) != 0)
        return SF_ERR_FLASH;
    if (flash_write(base, p, IMAGE_HEADER_SIZE) != 0)
        return SF_ERR_FLASH;

    md.active_slot = target;
    md.boot_state = BOOT_STATE_TRIAL;
    md.trial_count = 0;
    if (metadata_store(&md) != 0)
        return SF_ERR_FLASH;
    return SF_OK;
}

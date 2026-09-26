#include "safeflash_app.h"
#include "boot_config.h"
#include "flash_hal.h"
#include "flash_map.h"
#include "image_verify.h"
#include "metadata.h"
#include "wdt.h"

static int kick_inited;
static int kick_allowed;

int sf_confirm_healthy(void)
{
    boot_metadata_t md;
    image_header_t h;

    if (metadata_load(&md) != 0)
        return -1;

    int dirty = 0;
    if (md.boot_state == BOOT_STATE_TRIAL) {
        md.boot_state = BOOT_STATE_CONFIRMED;
        md.trial_count = 0;
        dirty = 1;
    }
    /* Anti-rollback ratchet: once this version is confirmed, nothing older may ever boot again. */
    if (flash_read(slot_addr(md.active_slot), &h, sizeof h) == 0 && h.magic == IMAGE_MAGIC &&
        h.version > md.min_allowed_version) {
        md.min_allowed_version = h.version;
        dirty = 1;
    }
    if (dirty && metadata_store(&md) != 0)
        return -1;   /* stays TRIAL: worst case the trial expires and we revert, never unsafe */

    kick_inited = 1;
    kick_allowed = 1;
    return 0;
}

void sf_request_recovery(void)
{
    *(volatile uint32_t *)RECOVERY_REQUEST_ADDR = RECOVERY_REQUEST_MAGIC;
    *(volatile uint32_t *)0xE000ED0Cu = 0x05FA0004u;   /* SCB->AIRCR: SYSRESETREQ */
    for (;;)
        ;
}

void sf_wdt_kick(void)
{
    if (!kick_inited) {
        boot_metadata_t md;
        kick_allowed = metadata_load(&md) != 0 || md.boot_state != BOOT_STATE_TRIAL;
        kick_inited = 1;
    }
    if (kick_allowed)
        wdt_kick();
}

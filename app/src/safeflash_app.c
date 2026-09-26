#include "safeflash_app.h"
#include "metadata.h"
#include "wdt.h"

static int kick_inited;
static int kick_allowed;

int sf_confirm_healthy(void)
{
    boot_metadata_t md;

    if (metadata_load(&md) != 0)
        return -1;
    if (md.boot_state == BOOT_STATE_TRIAL) {
        md.boot_state = BOOT_STATE_CONFIRMED;
        md.trial_count = 0;
        if (metadata_store(&md) != 0)
            return -1;   /* stays TRIAL: worst case the trial expires and we revert, never unsafe */
    }
    kick_inited = 1;
    kick_allowed = 1;
    return 0;
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

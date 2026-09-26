#ifndef BOOT_CONFIG_H
#define BOOT_CONFIG_H

/* Boot attempts allowed for an unconfirmed (TRIAL) image before reverting to the other slot. */
#define MAX_TRIALS      3u

/*
 * Trial window: an unconfirmed image must call sf_confirm_healthy() within this time of boot or the
 * IWDG resets the device. Kept short so simulated runs stay quick; a real product would use seconds
 * to tens of seconds (IWDG max is ~32 s at /256).
 */
#define TRIAL_WDT_MS    2000u

#endif
